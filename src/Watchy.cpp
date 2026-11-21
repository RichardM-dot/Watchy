/*
 * ============================================================================
 * 【中文导读】Watchy.cpp —— 手表主类：一切流程的编排者（本项目核心）
 * ----------------------------------------------------------------------------
 * Watchy 是一个 Arduino 库：用户继承 Watchy 类、重写 drawWatchFace()
 * 即可做出自己的表盘。本文件是基类实现，主线：
 *   init() → RTC 对时/闹钟 → 按钮中断 → deepSleep()
 *   showWatchFace() → 用户 drawWatchFace() 画一帧 → 等 RTC/加速度计中断唤醒
 * 低功耗闭环：墨水屏断电保持画面 + ESP32 deep sleep（μA 级）+
 * PCF8563 RTC 每分钟/加速度计翻腕中断唤醒 → 重画 → 再睡。
 * 其它子系统：BLE 配对（BLE.cpp）、BMA423 计步（bma.cpp）、Wi-Fi NTP 对时。
 * ============================================================================
 */

/*
 * 【函数总览】Watchy.cpp —— 行号为本注释添加前原文件行号（注释后顺延）
 * ----------------------------------------------------------------------------
 * 生命周期     init() L41          deepSleep() L112
 * 按键/菜单    handleButtonPress() L149   showMenu() L303   showFastMenu() L336
 * 内置页面     showAbout() L368    showBuzz() L417   setTime() L439   showAccelerometer() L607
 * 表盘         showWatchFace() L679   drawWatchFace() L688（用户重写点）
 * 网络         getWeatherData() L702   setupWifi() L925   connectWiFi() L972
 *              showSyncNTP() L1099    syncNTP()×3 L1145
 * 硬件         getBatteryVoltage() L770   getBoardRevision() L783   _bmaConfig() L829
 * BMA I2C回调  _readRegister() L808   _writeRegister() L821
 */

#include "Watchy.h"

#ifdef ARDUINO_ESP32S3_DEV
  Watchy32KRTC Watchy::RTC;
  #define ACTIVE_LOW 0
#else
  WatchyRTC Watchy::RTC;
  #define ACTIVE_LOW 1
#endif
GxEPD2_BW<WatchyDisplay, WatchyDisplay::HEIGHT> Watchy::display(
    WatchyDisplay{});

/* ---------------------------------------------------------------------------
 * RTC 慢速内存全局区：RTC_DATA_ATTR 把变量放进 ESP32 内部 8KB RTC 慢存，
 * 深睡期间保持、醒来不清零（仅断电冷启动才随机化）。Watchy 的整个
 * "跨睡眠状态机"都靠它们：界面状态、菜单序号、传感器对象、天气缓存……
 * 每次深睡醒来 init() 都会重跑，但下面这些变量不会被重新初始化。
 * ------------------------------------------------------------------------- */
RTC_DATA_ATTR int guiState;
RTC_DATA_ATTR int menuIndex;
RTC_DATA_ATTR BMA423 sensor;
RTC_DATA_ATTR bool WIFI_CONFIGURED;
RTC_DATA_ATTR bool BLE_CONFIGURED;
RTC_DATA_ATTR weatherData currentWeather;
RTC_DATA_ATTR int weatherIntervalCounter = -1;
RTC_DATA_ATTR long gmtOffset = 0;
RTC_DATA_ATTR bool alreadyInMenu         = true;
RTC_DATA_ATTR bool USB_PLUGGED_IN = false;
RTC_DATA_ATTR tmElements_t bootTime;
RTC_DATA_ATTR uint32_t lastIPAddress;
RTC_DATA_ATTR char lastSSID[30];

/**
 * @brief  总入口：每次"上电或深睡唤醒"都从这里开始，按唤醒原因分派，末尾必然 deepSleep()
 * @param  datetime  冷启动时的初始时间串"YYYY:MM:DD:HH:MM:SS"，空串则不设时（仅 examples 的 .ino 调用）
 * @note   唤醒原因由 esp_sleep_get_wakeup_cause() 读出：
 *         - ESP_SLEEP_WAKEUP_EXT0：V1/V2 为 PCF8563 每分钟闹钟（V3 改用内部 timer）→ 刷表盘/菜单超时回表盘
 *         - ESP_SLEEP_WAKEUP_EXT1：按键按下 → handleButtonPress() 分派
 *         - default（上电/看门狗复位）：真正的冷启动 → 全外设初始化 + 全刷表盘
 *         注意本函数没有循环：执行完 switch 就调 deepSleep() 把控制权交回硬件，
 *         "主循环"其实是 深睡→唤醒→init()→深睡 的一圈圈循环。
 */
void Watchy::init(String datetime) {
  esp_sleep_wakeup_cause_t wakeup_reason;
  wakeup_reason = esp_sleep_get_wakeup_cause(); // get wake up reason
  #ifdef ARDUINO_ESP32S3_DEV
    Wire.begin(WATCHY_V3_SDA, WATCHY_V3_SCL);     // init i2c
  #else
    Wire.begin(SDA, SCL);                         // init i2c
  #endif
  RTC.init();
  // Init the display since is almost sure we will use it
  display.epd2.initWatchy();

  /* 唤醒原因在深睡期间由硬件锁存，醒来后读一次即可判断"这次为什么活过来" */
  switch (wakeup_reason) {
  #ifdef ARDUINO_ESP32S3_DEV
  case ESP_SLEEP_WAKEUP_TIMER: // RTC Alarm
  #else
  case ESP_SLEEP_WAKEUP_EXT0: // RTC Alarm
  #endif
    RTC.read(currentTime);
    /* 每分钟 tick 后画什么，由"睡前一刻"的 guiState 决定（RTC 慢存里 preserved） */
    switch (guiState) {
    case WATCHFACE_STATE:
      showWatchFace(true); // partial updates on tick
      if (settings.vibrateOClock) {
        if (currentTime.Minute == 0) {
          // The RTC wakes us up once per minute
          vibMotor(75, 4);
        }
      }
      break;
    case MAIN_MENU_STATE:
      // Return to watchface if in menu for more than one tick
      /* alreadyInMenu 是"菜单停留计时器"：第一个 tick 置 true，第二个 tick
         仍无人操作就回表盘——用 RTC 闹钟当 60 秒无操作超时器，不占任何定时器资源 */
      if (alreadyInMenu) {
        guiState = WATCHFACE_STATE;
        showWatchFace(false);
      } else {
        alreadyInMenu = true;
      }
      break;
    }
    break;
  case ESP_SLEEP_WAKEUP_EXT1: // button Press
    handleButtonPress();
    break;
  #ifdef ARDUINO_ESP32S3_DEV
  case ESP_SLEEP_WAKEUP_EXT0: // USB plug in
    pinMode(USB_DET_PIN, INPUT);
    USB_PLUGGED_IN = (digitalRead(USB_DET_PIN) == 1);
    if(guiState == WATCHFACE_STATE){
      RTC.read(currentTime);
      showWatchFace(true);
    }
    break;
  #endif
  default: // reset
    /* 冷启动（上电/按复位/烧录后）：唯一做全量外设初始化的路径——
       RTC 校时、BMA423 配置都只在这里跑一次，之后深睡唤醒不再重复 */
    RTC.config(datetime);
    _bmaConfig();
    #ifdef ARDUINO_ESP32S3_DEV
    pinMode(USB_DET_PIN, INPUT);
    USB_PLUGGED_IN = (digitalRead(USB_DET_PIN) == 1);
    #endif    
    gmtOffset = settings.gmtOffset;
    RTC.read(currentTime);
    RTC.read(bootTime);
    showWatchFace(false); // full update on reset
    vibMotor(75, 4);
    // For some reason, seems to be enabled on first boot
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    break;
  }
  deepSleep();
}
/**
 * @brief  把整块手表"关掉"：屏幕休眠 → 清闹钟标志 → 配置唤醒源 → esp_deep_sleep_start()
 * @note   所有执行路径（init 的三个分支、菜单、App 页面）最终都汇到这里。
 *         V1/V2 唤醒源：ext0 = RTC INT 引脚（每分钟，低电平），ext1 = 4 个按键（任一高电平）；
 *         V3(S3)  唤醒源：ext0 = USB 插拔检测脚，ext1 = 按键（任一低电平，S3 按键 active-low），
 *                 外加内部 timer 定到下一个整分（32K 时钟域）。
 *         墨水屏断电后保持显示，所以深睡期间表盘内容不丢——这是墨水屏手表省电的根基。
 */
void Watchy::deepSleep() {
  display.hibernate();
  RTC.clearAlarm();        // resets the alarm flag in the RTC
  #ifdef ARDUINO_ESP32S3_DEV
  esp_sleep_enable_ext0_wakeup((gpio_num_t)USB_DET_PIN, USB_PLUGGED_IN ? LOW : HIGH); //// enable deep sleep wake on USB plug in/out
  rtc_gpio_set_direction((gpio_num_t)USB_DET_PIN, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)USB_DET_PIN);

  esp_sleep_enable_ext1_wakeup(
      BTN_PIN_MASK,
      ESP_EXT1_WAKEUP_ANY_LOW); // enable deep sleep wake on button press
  rtc_gpio_set_direction((gpio_num_t)UP_BTN_PIN, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)UP_BTN_PIN);

  rtc_clk_32k_enable(true);
  //rtc_clk_slow_freq_set(RTC_SLOW_FREQ_32K_XTAL);
  struct tm timeinfo;
  getLocalTime(&timeinfo);
  int secToNextMin = 60 - timeinfo.tm_sec;
  esp_sleep_enable_timer_wakeup(secToNextMin * uS_TO_S_FACTOR);
  #else
  // Set GPIOs 0-39 to input to avoid power leaking out
  /* 悬空引脚全部切输入，切断经外部上下拉/外设的漏电通路；
     ignore 位掩码 = 0xF10309C2，跳过不能动的系统引脚：
     GPIO6/7/8/11 = flash SPI，GPIO1 = UART0 TX，另含 16/17/24/28-31 */
  const uint64_t ignore = 0b11110001000000110000100111000010; // Ignore some GPIOs due to resets
  for (int i = 0; i < GPIO_NUM_MAX; i++) {
    if ((ignore >> i) & 0b1)
      continue;
    pinMode(i, INPUT);
  }
  esp_sleep_enable_ext0_wakeup((gpio_num_t)RTC_INT_PIN,
                               0); // enable deep sleep wake on RTC interrupt
  esp_sleep_enable_ext1_wakeup(
      BTN_PIN_MASK,
      ESP_EXT1_WAKEUP_ANY_HIGH); // enable deep sleep wake on button press
  #endif
  esp_deep_sleep_start();
}

/**
 * @brief  按键唤醒分派器 + 菜单内"快菜单"轮询循环（Watchy 交互的另一半主循环）
 * @note   从 deepSleep 醒来（EXT1 按键唤醒）后由 init() 调用。
 *         第一段：读 esp_sleep_get_ext1_wakeup_status() 位图判断"刚才按的是哪颗键"，
 *                 按 guiState 状态机分派（见 Docs/04-菜单与按键状态机.md）。
 *         第二段：快菜单 while 循环——不再回深睡，直接轮询 GPIO 电平 5 秒，
 *                 让用户在菜单里连续上下滚动；5 秒无键超时退出，函数返回后
 *                 init() 末尾的 deepSleep() 照常睡觉。两段按键处理逻辑是复制粘贴关系。
 */
void Watchy::handleButtonPress() {
  uint64_t wakeupBit = esp_sleep_get_ext1_wakeup_status();
  /* 位图哪一位为 1 = 哪颗键把系统唤醒（deepSleep() 里 ext1 注册的 BTN_PIN_MASK） */
  // Menu Button
  if (wakeupBit & MENU_BTN_MASK) {
    if (guiState ==
        WATCHFACE_STATE) { // enter menu state if coming from watch face
      showMenu(menuIndex, false);
    } else if (guiState ==
               MAIN_MENU_STATE) { // if already in menu, then select menu item
      switch (menuIndex) {
      case 0:
        showAbout();
        break;
      case 1:
        showBuzz();
        break;
      case 2:
        showAccelerometer();
        break;
      case 3:
        setTime();
        break;
      case 4:
        setupWifi();
        break;
      /*case 5:
        showUpdateFW();
        break;*/
      case 5:
        showSyncNTP();
        break;
      default:
        break;
      }
    } /*else if (guiState == FW_UPDATE_STATE) {
      updateFWBegin();
    }*/
  }
  // Back Button
  else if (wakeupBit & BACK_BTN_MASK) {
    if (guiState == MAIN_MENU_STATE) { // exit to watch face if already in menu
      RTC.read(currentTime);
      showWatchFace(false);
    } else if (guiState == APP_STATE) {
      showMenu(menuIndex, false); // exit to menu if already in app
    } else if (guiState == FW_UPDATE_STATE) {
      showMenu(menuIndex, false); // exit to menu if already in app
    } else if (guiState == WATCHFACE_STATE) {
      return;
    }
  }
  // Up Button
  else if (wakeupBit & UP_BTN_MASK) {
    if (guiState == MAIN_MENU_STATE) { // increment menu index
      menuIndex--;
      if (menuIndex < 0) {
        menuIndex = MENU_LENGTH - 1;
      }
      showMenu(menuIndex, true);
    } else if (guiState == WATCHFACE_STATE) {
      return;
    }
  }
  // Down Button
  else if (wakeupBit & DOWN_BTN_MASK) {
    if (guiState == MAIN_MENU_STATE) { // decrement menu index
      menuIndex++;
      if (menuIndex > MENU_LENGTH - 1) {
        menuIndex = 0;
      }
      showMenu(menuIndex, true);
    } else if (guiState == WATCHFACE_STATE) {
      return;
    }
  }

  /***************** fast menu *****************/
  /* 快菜单：保持清醒、忙等轮询电平（不进深睡），实现连续滚动；
     5 秒（5000ms）无按键即超时跳出，返回后 init() 末尾统一深睡。
     注意 UP/DOWN 在这里走 showFastMenu()（部分刷新，滚得快）。 */
  bool timeout     = false;
  long lastTimeout = millis();
  pinMode(MENU_BTN_PIN, INPUT);
  pinMode(BACK_BTN_PIN, INPUT);
  pinMode(UP_BTN_PIN, INPUT);
  pinMode(DOWN_BTN_PIN, INPUT);
  while (!timeout) {
    if (millis() - lastTimeout > 5000) {
      timeout = true;
    } else {
      if (digitalRead(MENU_BTN_PIN) == ACTIVE_LOW) {
        lastTimeout = millis();
        if (guiState ==
            MAIN_MENU_STATE) { // if already in menu, then select menu item
          switch (menuIndex) {
          case 0:
            showAbout();
            break;
          case 1:
            showBuzz();
            break;
          case 2:
            showAccelerometer();
            break;
          case 3:
            setTime();
            break;
          case 4:
            setupWifi();
            break;
          /*case 5:
            showUpdateFW();
            break;*/
          case 5:
            showSyncNTP();
            break;
          default:
            break;
          }
        }/* else if (guiState == FW_UPDATE_STATE) {
          updateFWBegin();
        }*/
      } else if (digitalRead(BACK_BTN_PIN) == ACTIVE_LOW) {
        lastTimeout = millis();
        if (guiState ==
            MAIN_MENU_STATE) { // exit to watch face if already in menu
          RTC.read(currentTime);
          showWatchFace(false);
          break; // leave loop
        } else if (guiState == APP_STATE) {
          showMenu(menuIndex, false); // exit to menu if already in app
        } else if (guiState == FW_UPDATE_STATE) {
          showMenu(menuIndex, false); // exit to menu if already in app
        }
      } else if (digitalRead(UP_BTN_PIN) == ACTIVE_LOW) {
        lastTimeout = millis();
        if (guiState == MAIN_MENU_STATE) { // increment menu index
          menuIndex--;
          if (menuIndex < 0) {
            menuIndex = MENU_LENGTH - 1;
          }
          showFastMenu(menuIndex);
        }
      } else if (digitalRead(DOWN_BTN_PIN) == ACTIVE_LOW) {
        lastTimeout = millis();
        if (guiState == MAIN_MENU_STATE) { // decrement menu index
          menuIndex++;
          if (menuIndex > MENU_LENGTH - 1) {
            menuIndex = 0;
          }
          showFastMenu(menuIndex);
        }
      }
    }
  }
}

/**
 * @brief  绘制主菜单：黑底白字 6 项，选中行反白（白底黑字）+ 白色高亮条
 * @param  menuIndex       当前选中项 0~5（About/Buzz/加速计/SetTime/WiFi/SyncNTP）
 * @param  partialRefresh  true=部分快速刷新(~0.5s)，false=全刷(~2.6s，清残影)
 * @note   副作用：guiState ← MAIN_MENU_STATE，alreadyInMenu ← false（重启 60s 超时计数）。
 *         菜单项列表与 MENU_LENGTH=6 必须同步维护（config.h）。
 */
void Watchy::showMenu(byte menuIndex, bool partialRefresh) {
  display.setFullWindow();
  display.fillScreen(GxEPD_BLACK);
  display.setFont(&FreeMonoBold9pt7b);

  int16_t x1, y1;
  uint16_t w, h;
  int16_t yPos;

  const char *menuItems[] = {
      "About Watchy", "Vibrate Motor", "Show Accelerometer",
      "Set Time",     "Setup WiFi",    /*"Update Firmware",*/
      "Sync NTP"};
  for (int i = 0; i < MENU_LENGTH; i++) {
    yPos = MENU_HEIGHT + (MENU_HEIGHT * i);
    display.setCursor(0, yPos);
    if (i == menuIndex) {
      display.getTextBounds(menuItems[i], 0, yPos, &x1, &y1, &w, &h);
      display.fillRect(x1 - 1, y1 - 10, 200, h + 15, GxEPD_WHITE);
      display.setTextColor(GxEPD_BLACK);
      display.println(menuItems[i]);
    } else {
      display.setTextColor(GxEPD_WHITE);
      display.println(menuItems[i]);
    }
  }

  /* 选中行：getTextBounds 量出文字宽高 → 先涂一块白底再黑字画文字（反白高亮）；
     其余行直接白字画在黑底上。GxEPD 的颜色语义是"墨水颜色"：WHITE=有墨(可见)。 */
  display.display(partialRefresh);

  guiState = MAIN_MENU_STATE;
  alreadyInMenu = false;
}

/**
 * @brief  快菜单（菜单滚动专用）：与 showMenu() 绘制逻辑相同，但恒定部分快速刷新
 * @note   差异仅在最后 display.display(true)：上下键连按时每帧只花 ~0.5s，
 *         交互手感优先于清残影。不更新 alreadyInMenu（快菜单期间不算超时重新计时）。
 */
void Watchy::showFastMenu(byte menuIndex) {
  display.setFullWindow();
  display.fillScreen(GxEPD_BLACK);
  display.setFont(&FreeMonoBold9pt7b);

  int16_t x1, y1;
  uint16_t w, h;
  int16_t yPos;

  const char *menuItems[] = {
      "About Watchy", "Vibrate Motor", "Show Accelerometer",
      "Set Time",     "Setup WiFi",    /*"Update Firmware",*/
      "Sync NTP"};
  for (int i = 0; i < MENU_LENGTH; i++) {
    yPos = MENU_HEIGHT + (MENU_HEIGHT * i);
    display.setCursor(0, yPos);
    if (i == menuIndex) {
      display.getTextBounds(menuItems[i], 0, yPos, &x1, &y1, &w, &h);
      display.fillRect(x1 - 1, y1 - 10, 200, h + 15, GxEPD_WHITE);
      display.setTextColor(GxEPD_BLACK);
      display.println(menuItems[i]);
    } else {
      display.setTextColor(GxEPD_WHITE);
      display.println(menuItems[i]);
    }
  }

  /* 与 showMenu 唯一区别：恒为快速部分刷新，滚动跟手 */
  display.display(true);

  guiState = MAIN_MENU_STATE;
}

/**
 * @brief  "About Watchy" 页：固件版本 / 硬件版本 / 电池电压 / 开机时长 / WiFi 信息
 * @note   全刷显示后 guiState ← APP_STATE。开机时长 = 当前时间 - bootTime
 *         （bootTime 在冷启动分支里读 RTC 保存，存于 RTC 慢存跨深睡累计）。
 *         V3(S3) 没有 bootTime 逻辑，不显示 Uptime 行。
 */
void Watchy::showAbout() {
  display.setFullWindow();
  display.fillScreen(GxEPD_BLACK);
  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_WHITE);
  display.setCursor(0, 20);

  display.print("LibVer: ");
  display.println(WATCHY_LIB_VER);

  display.print("Rev: v");
  display.println(getBoardRevision());

  display.print("Batt: ");
  float voltage = getBatteryVoltage();
  display.print(voltage);
  display.println("V");

  #ifndef ARDUINO_ESP32S3_DEV
  display.print("Uptime: ");
  RTC.read(currentTime);
  /* uptime 用 RTC 真实时间差计算（不受深睡影响）：秒→时分天手动取模换算 */
  time_t b = makeTime(bootTime);
  time_t c = makeTime(currentTime);
  int totalSeconds = c-b;
  //int seconds = (totalSeconds % 60);
  int minutes = (totalSeconds % 3600) / 60;
  int hours = (totalSeconds % 86400) / 3600;
  int days = (totalSeconds % (86400 * 30)) / 86400; 
  display.print(days);
  display.print("d");
  display.print(hours);
  display.print("h");
  display.print(minutes);
  display.println("m");  
  #endif
  
  if(WIFI_CONFIGURED){
    display.print("SSID: ");
    display.println(lastSSID);
    display.print("IP: ");
    display.println(IPAddress(lastIPAddress).toString());
  }else{
    display.println("WiFi Not Connected");
  }
  display.display(false); // full refresh

  guiState = APP_STATE;
}

/**
 * @brief  "Vibrate Motor" 菜单项：显示 Buzz! 文案并驱动马达一段，随后回主菜单
 * @note   仅作菜单功能演示——真实用途（整点提醒/闹钟）见 init() 的 vibrateOClock 分支。
 */
void Watchy::showBuzz() {
  display.setFullWindow();
  display.fillScreen(GxEPD_BLACK);
  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_WHITE);
  display.setCursor(70, 80);
  display.println("Buzz!");
  display.display(false); // full refresh
  vibMotor();
  showMenu(menuIndex, false);
}

/**
 * @brief  方波驱动振动马达：电平按 intervalMs 交替翻转 length 次
 * @param  intervalMs  每个半周期时长 ms（默认 100）
 * @param  length      翻转次数（默认 20）→ 总时长 = intervalMs × length = 2s
 * @note   阻塞式 delay 实现；整点提醒调用 vibMotor(75, 4)（150ms×4 短震两下）。
 *         马达是感性负载，由 GPIO 经三极管驱动，这里只管翻转引脚。
 */
void Watchy::vibMotor(uint8_t intervalMs, uint8_t length) {
  pinMode(VIB_MOTOR_PIN, OUTPUT);
  bool motorOn = false;
  for (int i = 0; i < length; i++) {
    motorOn = !motorOn;
    digitalWrite(VIB_MOTOR_PIN, motorOn);
    delay(intervalMs);
  }
}

/**
 * @brief  手动对时页面：DSEG7 大字号显示 HH:MM 与日期，当前可编辑字段闪烁
 * @note   按键语义（轮询电平，非唤醒）：MENU=下一字段，BACK=上一字段，
 *         UP/DOWN=加减当前字段。字段顺序 SET_HOUR→SET_MINUTE→SET_YEAR→
 *         SET_MONTH→SET_DAY，在 SET_DAY 上按 MENU 退出循环并写入 RTC。
 *         "闪烁"用每轮重绘时切换该字段黑/白实现（墨水屏快速部分刷新 ~0.5s/帧）。
 *         V1/V2 与 V3 的 year 基准不同：前者 tmYearToY2k（2000 基），后者直接用。
 */
void Watchy::setTime() {

  guiState = APP_STATE;

  RTC.read(currentTime);

  #ifdef ARDUINO_ESP32S3_DEV
  uint8_t minute = currentTime.Minute;
  uint8_t hour   = currentTime.Hour;
  uint8_t day    = currentTime.Day;
  uint8_t month  = currentTime.Month;
  uint8_t year   = currentTime.Year;  
  #else
  int8_t minute = currentTime.Minute;
  int8_t hour   = currentTime.Hour;
  int8_t day    = currentTime.Day;
  int8_t month  = currentTime.Month;
  int8_t year   = tmYearToY2k(currentTime.Year);
  #endif

  int8_t setIndex = SET_HOUR;

  /* blink 每 while 轮取反一次，选中字段在黑白间翻转形成闪烁感 */
  int8_t blink = 0;

  pinMode(DOWN_BTN_PIN, INPUT);
  pinMode(UP_BTN_PIN, INPUT);
  pinMode(MENU_BTN_PIN, INPUT);
  pinMode(BACK_BTN_PIN, INPUT);

  display.setFullWindow();

  while (1) {

    /* MENU：字段游标右移；最后一个字段再按即结束（break 出去写 RTC） */
    if (digitalRead(MENU_BTN_PIN) == ACTIVE_LOW) {
      setIndex++;
      if (setIndex > SET_DAY) {
        break;
      }
    }
    if (digitalRead(BACK_BTN_PIN) == ACTIVE_LOW) {
      if (setIndex != SET_HOUR) {
        setIndex--;
      }
    }

    blink = 1 - blink;

    if (digitalRead(DOWN_BTN_PIN) == ACTIVE_LOW) {
      blink = 1;
      switch (setIndex) {
      case SET_HOUR:
        hour == 23 ? (hour = 0) : hour++;
        break;
      case SET_MINUTE:
        minute == 59 ? (minute = 0) : minute++;
        break;
      case SET_YEAR:
        year == 99 ? (year = 0) : year++;
        break;
      case SET_MONTH:
        month == 12 ? (month = 1) : month++;
        break;
      case SET_DAY:
        day == 31 ? (day = 1) : day++;
        break;
      default:
        break;
      }
    }

    if (digitalRead(UP_BTN_PIN) == ACTIVE_LOW) {
      blink = 1;
      switch (setIndex) {
      case SET_HOUR:
        hour == 0 ? (hour = 23) : hour--;
        break;
      case SET_MINUTE:
        minute == 0 ? (minute = 59) : minute--;
        break;
      case SET_YEAR:
        year == 0 ? (year = 99) : year--;
        break;
      case SET_MONTH:
        month == 1 ? (month = 12) : month--;
        break;
      case SET_DAY:
        day == 1 ? (day = 31) : day--;
        break;
      default:
        break;
      }
    }

    display.fillScreen(GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    display.setFont(&DSEG7_Classic_Bold_53);

    display.setCursor(5, 80);
    if (setIndex == SET_HOUR) { // blink hour digits
      display.setTextColor(blink ? GxEPD_WHITE : GxEPD_BLACK);
    }
    if (hour < 10) {
      display.print("0");
    }
    display.print(hour);

    display.setTextColor(GxEPD_WHITE);
    display.print(":");

    display.setCursor(108, 80);
    if (setIndex == SET_MINUTE) { // blink minute digits
      display.setTextColor(blink ? GxEPD_WHITE : GxEPD_BLACK);
    }
    if (minute < 10) {
      display.print("0");
    }
    display.print(minute);

    display.setTextColor(GxEPD_WHITE);

    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(45, 150);
    if (setIndex == SET_YEAR) { // blink minute digits
      display.setTextColor(blink ? GxEPD_WHITE : GxEPD_BLACK);
    }
    display.print(2000 + year);

    display.setTextColor(GxEPD_WHITE);
    display.print("/");

    if (setIndex == SET_MONTH) { // blink minute digits
      display.setTextColor(blink ? GxEPD_WHITE : GxEPD_BLACK);
    }
    if (month < 10) {
      display.print("0");
    }
    display.print(month);

    display.setTextColor(GxEPD_WHITE);
    display.print("/");

    if (setIndex == SET_DAY) { // blink minute digits
      display.setTextColor(blink ? GxEPD_WHITE : GxEPD_BLACK);
    }
    if (day < 10) {
      display.print("0");
    }
    display.print(day);
    display.display(true); // partial refresh
  }

  /* 打包调整完的字段写回 RTC；秒清零（对时精度到分） */
  tmElements_t tm;
  tm.Month  = month;
  tm.Day    = day;
  #ifdef ARDUINO_ESP32S3_DEV
  tm.Year   = year;
  #else
  tm.Year   = y2kYearToTm(year);
  #endif
  tm.Hour   = hour;
  tm.Minute = minute;
  tm.Second = 0;

  RTC.set(tm);

  showMenu(menuIndex, false);
}

/**
 * @brief  "Show Accelerometer" 页面：每 200ms 读一次三轴加速度与朝向并刷新
 * @note   这是全工程唯一"清醒状态下持续工作"的循环：millis() 定时器节流，
 *         BACK 键退出。同时演示了 sensor.getAccel()/getDirection() 的封装用法。
 *         部分刷新 5Hz 对墨水屏已接近极限（每帧 ~0.5s），再快会撕裂。
 */
void Watchy::showAccelerometer() {
  display.setFullWindow();
  display.fillScreen(GxEPD_BLACK);
  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_WHITE);

  Accel acc;

  long previousMillis = 0;
  long interval       = 200;

  guiState = APP_STATE;

  pinMode(BACK_BTN_PIN, INPUT);

  while (1) {

    unsigned long currentMillis = millis();

    if (digitalRead(BACK_BTN_PIN) == ACTIVE_LOW) {
      break;
    }

    if (currentMillis - previousMillis > interval) {
      previousMillis = currentMillis;
      // Get acceleration data
      /* getDirection() 内部再读一次加速度，按"哪根轴分量最大"判 6 个朝向 */
      bool res          = sensor.getAccel(acc);
      uint8_t direction = sensor.getDirection();
      display.fillScreen(GxEPD_BLACK);
      display.setCursor(0, 30);
      if (res == false) {
        display.println("getAccel FAIL");
      } else {
        display.print("  X:");
        display.println(acc.x);
        display.print("  Y:");
        display.println(acc.y);
        display.print("  Z:");
        display.println(acc.z);

        display.setCursor(30, 130);
        switch (direction) {
        case DIRECTION_DISP_DOWN:
          display.println("FACE DOWN");
          break;
        case DIRECTION_DISP_UP:
          display.println("FACE UP");
          break;
        case DIRECTION_BOTTOM_EDGE:
          display.println("BOTTOM EDGE");
          break;
        case DIRECTION_TOP_EDGE:
          display.println("TOP EDGE");
          break;
        case DIRECTION_RIGHT_EDGE:
          display.println("RIGHT EDGE");
          break;
        case DIRECTION_LEFT_EDGE:
          display.println("LEFT EDGE");
          break;
        default:
          display.println("ERROR!!!");
          break;
        }
      }
      display.display(true); // full refresh
    }
  }

  showMenu(menuIndex, false);
}

/**
 * @brief  表盘渲染外壳：设全屏窗口 → 异步升压 → 调用户 drawWatchFace() → 送显
 * @param  partialRefresh  true=每分钟 tick 的部分刷新（省时省电），false=全刷
 * @note   用户侧唯一必须理解的两个调用点之一（另一个是 drawWatchFace 本身）。
 *         asyncPowerOn() 把屏的 ~100ms 升压预热与用户的绘制代码重叠执行。
 *         末尾 guiState ← WATCHFACE_STATE，让下一次 RTC tick 走表盘分支。
 */
void Watchy::showWatchFace(bool partialRefresh) {
  display.setFullWindow();
  // At this point it is sure we are going to update
  display.epd2.asyncPowerOn();
  drawWatchFace();
  display.display(partialRefresh); // partial refresh
  guiState = WATCHFACE_STATE;
}

/**
 * @brief  基类默认表盘：纯 DSEG7 大字号 HH:MM（占屏中央，无日期无电量）
 * @note   用户表盘 = 继承 Watchy 并 override 本函数；画什么都行，最后无需自己
 *         调 display.display()（外壳 showWatchFace 统一送显）。
 *         currentTime 已由 init()/调用方读好，这里直接用。
 */
void Watchy::drawWatchFace() {
  display.setFont(&DSEG7_Classic_Bold_53);
  display.setCursor(5, 53 + 60);
  if (currentTime.Hour < 10) {
    display.print("0");
  }
  display.print(currentTime.Hour);
  display.print(":");
  if (currentTime.Minute < 10) {
    display.print("0");
  }
  display.println(currentTime.Minute);
}

/**
 * @brief  天气获取入口：用 settings 里用户配置好的参数调 _getWeatherData()
 * @return weatherData（温度/天气码/日出日落等，见 Watchy.h 结构体注释）
 * @note   表盘里直接调即可；内部带"每 updateInterval 分钟才真正联网一次"的节流。
 */
weatherData Watchy::getWeatherData() {
  return _getWeatherData(settings.cityID, settings.lat, settings.lon,
    settings.weatherUnit, settings.weatherLang, settings.weatherURL,
    settings.weatherAPIKey, settings.weatherUpdateInterval);
}

/**
 * @brief  天气实现：节流计数 → 联网拉 OpenWeatherMap / 无网退化为芯片温度 → 顺带 NTP 对时
 * @param  cityID/lat/lon  城市或经纬度定位（二选一，url 模板里 {cityID} 或 {lat}{lon} 占位）
 * @param  units/lang      metric|imperial 与语言，直接替换进 URL
 * @param  apiKey          OpenWeatherMap 的 APPID
 * @param  updateInterval  天气更新间隔（分钟），examples/settings.h 里默认 30
 * @return currentWeather（RTC 慢存全局，跨深睡缓存）
 * @note   weatherIntervalCounter 是 RTC 慢存计数器：每分钟 tick 调一次本函数，
 *         计数没到间隔就直接返回缓存——"30 分钟天气"实际是 30 次唤醒才联网 1 次。
 */
weatherData Watchy::_getWeatherData(String cityID, String lat, String lon, String units, String lang,
                                   String url, String apiKey,
                                   uint8_t updateInterval) {
  currentWeather.isMetric = units == String("metric");
  /* 计数器 -1 表示"还没联网过"（setupWifi 成功后也会重置为 -1 强拉一次） */
  if (weatherIntervalCounter < 0) { //-1 on first run, set to updateInterval
    weatherIntervalCounter = updateInterval;
  }
  if (weatherIntervalCounter >=
      updateInterval) { // only update if WEATHER_UPDATE_INTERVAL has elapsed
                        // i.e. 30 minutes
    if (connectWiFi()) {
      HTTPClient http; // Use Weather API for live data if WiFi is connected
      /* 连接超时 3s：联网失败不能拖垮每分钟一次的刷新节拍 */
      http.setConnectTimeout(3000); // 3 second max timeout
      String weatherQueryURL = url;
      if(cityID != ""){
        weatherQueryURL.replace("{cityID}", cityID);
      }else{
        weatherQueryURL.replace("{lat}", lat);
        weatherQueryURL.replace("{lon}", lon);
      }
      weatherQueryURL.replace("{units}", units);
      weatherQueryURL.replace("{lang}", lang);
      weatherQueryURL.replace("{apiKey}", apiKey);
      http.begin(weatherQueryURL.c_str());
      int httpResponseCode = http.GET();
      if (httpResponseCode == 200) {
        String payload             = http.getString();
        JSONVar responseObject     = JSON.parse(payload);
        currentWeather.temperature = int(responseObject["main"]["temp"]);
        currentWeather.weatherConditionCode =
            int(responseObject["weather"][0]["id"]);
        currentWeather.weatherDescription =
		        JSONVar::stringify(responseObject["weather"][0]["main"]);
	      currentWeather.external = true;
		        breakTime((time_t)(int)responseObject["sys"]["sunrise"], currentWeather.sunrise);
		        breakTime((time_t)(int)responseObject["sys"]["sunset"], currentWeather.sunset);
        // sync NTP during weather API call and use timezone of lat & lon
        gmtOffset = int(responseObject["timezone"]);
        syncNTP(gmtOffset);
      } else {
        // http error
      }
      http.end();
      /* 每次联网必关射频：WiFi+BLE 常开的功耗（~100mA 级）是深睡的几千倍，
         忘记关射频是手表类项目续航的头号杀手 */
      // turn off radios
      WiFi.mode(WIFI_OFF);
      btStop();
    } else { // No WiFi, use internal temperature sensor
      /* 无网退化路径：拿 BMA423 内置温度传感器当"体感天气"（约等于腕表外壳温），
         天气码固定 800（Clear），external=false 提示表盘"这不是真天气" */
      uint8_t temperature = sensor.readTemperature(); // celsius
      if (!currentWeather.isMetric) {
        temperature = temperature * 9. / 5. + 32.; // fahrenheit
      }
      currentWeather.temperature          = temperature;
      currentWeather.weatherConditionCode = 800;
      currentWeather.external             = false;
    }
    weatherIntervalCounter = 0;
  } else {
    weatherIntervalCounter++;
  }
  return currentWeather;
}

/**
 * @brief  读电池电压（V）：ADC 采样毫伏值 × 分压比还原开路电压
 * @note   V1/V2：电池经 1/2 电阻分压进 ADC 引脚，故 ×2.0；
 *         V3(S3)：分压比是 (360k+100k)/360k ≈ 1.278（宏 ADC_VOLTAGE_DIVIDER）。
 *         analogReadMilliVolts() 自带校准（读 eFuse 出厂校准值），比裸 ADC 值准。
 *         锂电满电 4.2V / 3.5V 左右半 / <3.2V 即将关机。
 */
float Watchy::getBatteryVoltage() {
  #ifdef ARDUINO_ESP32S3_DEV
    return analogReadMilliVolts(BATT_ADC_PIN) / 1000.0f * ADC_VOLTAGE_DIVIDER;
  #else
  if (RTC.rtcType == DS3231) {
    return analogReadMilliVolts(BATT_ADC_PIN) / 1000.0f *
           2.0f; // Battery voltage goes through a 1/2 divider.
  } else {
    return analogReadMilliVolts(BATT_ADC_PIN) / 1000.0f * 2.0f;
  }
  #endif
}

/**
 * @brief  硬件版本探测：返回 10/15/20/30（v1.0/v1.5/v2.0/v3.0），失败返回 -1(溢出为255)
 * @note   判定树：ESP32 芯片 → I2C 探测 0x68 有 ACK = DS3231 = v1.0；
 *         否则探测 0x51 有 ACK = PCF8563 = v1.5 或 v2.0，再用 GPIO35 电平区分
 *         （v2.0 上 35 号脚是按键 3，带下拉读到 0；v1.5 上 35 是电池 ADC 悬空读 1）。
 *         S3 芯片直接 = v3.0。运行期硬件自识别，同一份固件兼容全系列板子。
 */
uint8_t Watchy::getBoardRevision() {
  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);
  if(chip_info.model == CHIP_ESP32){ //Revision 1.0 - 2.0
    Wire.beginTransmission(0x68); //v1.0 has DS3231
    if (Wire.endTransmission() == 0){
      return 10;
    }
    delay(1);
    Wire.beginTransmission(0x51); //v1.5 and v2.0 have PCF8563
    if (Wire.endTransmission() == 0){
        pinMode(35, INPUT);
        if(digitalRead(35) == 0){
          return 20; //in rev 2.0, pin 35 is BTN 3 and has a pulldown
        }else{
          return 15; //in rev 1.5, pin 35 is the battery ADC
        }
    }
  }
  if(chip_info.model == CHIP_ESP32S3){ //Revision 3.0
    return 30;
  }
  return -1;
}

/**
 * @brief  BMA423 的 I2C 读回调：先写寄存器地址，再连读 len 字节
 * @param  address  7 位 I2C 地址（BMA4_I2C_ADDR_PRIMARY=0x18）
 * @param  reg      目标寄存器起始地址
 * @param  data     读出缓冲
 * @param  len      期望长度
 * @return 0=固定成功（Bosch 驱动约定的返回码；未做逐字节 ACK 校验）
 * @note   本函数指针被注册进 bma4_dev.bus_read，由 Bosch 驱动库在内部回调；
 *         必须是 static（无 this）——I2C 总线是全局资源（Wire）。
 */
uint16_t Watchy::_readRegister(uint8_t address, uint8_t reg, uint8_t *data,
                               uint16_t len) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.endTransmission();
  Wire.requestFrom((uint8_t)address, (uint8_t)len);
  uint8_t i = 0;
  while (Wire.available()) {
    data[i++] = Wire.read();
  }
  return 0;
}

/**
 * @brief  BMA423 的 I2C 写回调：寄存器地址 + len 字节一次性写出
 * @return 0=ACK 正常；非 0=endTransmission NACK/超时错误码
 * @note   与 _readRegister 成对，注册进 bma4_dev.bus_write。Bosch 驱动
 *         通过这对回调操作传感器，硬件细节全部隔离在 Watchy 侧。
 */
uint16_t Watchy::_writeRegister(uint8_t address, uint8_t reg, uint8_t *data,
                                uint16_t len) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(data, len);
  return (0 != Wire.endTransmission());
}

/**
 * @brief  BMA423 一站式配置（仅冷启动跑一次）：加速度计 ODR/量程 → INT1 引脚
 *         → 轴重映射 → 使能计步/翻腕/抬腕特性 → 中断映射到 INT1
 * @note   配置好的 sensor 对象存于 RTC 慢存，深睡期间 BMA423 芯片自己
 *         继续跑特征引擎（独立供电计数步数），ESP32 醒来直接 getCounter() 读结果。
 *         注意：中断只映射到芯片 INT1 引脚，并未注册为 ESP32 深睡唤醒源
 *         （deepSleep() 里只有 RTC 闹钟和按键）——双击/翻腕"唤醒"需自行加
 *         esp_sleep_enable_ext0_wakeup(ACC_INT_1_PIN, ...) 才生效。
 */
void Watchy::_bmaConfig() {

  if (sensor.begin(_readRegister, _writeRegister, delay) == false) {
    // fail to init BMA
    return;
  }

  // Accel parameter structure
  Acfg cfg;
  /*!
      Output data rate in Hz, Optional parameters:
          - BMA4_OUTPUT_DATA_RATE_0_78HZ
          - BMA4_OUTPUT_DATA_RATE_1_56HZ
          - BMA4_OUTPUT_DATA_RATE_3_12HZ
          - BMA4_OUTPUT_DATA_RATE_6_25HZ
          - BMA4_OUTPUT_DATA_RATE_12_5HZ
          - BMA4_OUTPUT_DATA_RATE_25HZ
          - BMA4_OUTPUT_DATA_RATE_50HZ
          - BMA4_OUTPUT_DATA_RATE_100HZ
          - BMA4_OUTPUT_DATA_RATE_200HZ
          - BMA4_OUTPUT_DATA_RATE_400HZ
          - BMA4_OUTPUT_DATA_RATE_800HZ
          - BMA4_OUTPUT_DATA_RATE_1600HZ
  */
  /* 100Hz：计步特征引擎的推荐采样率；再高只会费电（每档翻倍约多耗电） */
  cfg.odr = BMA4_OUTPUT_DATA_RATE_100HZ;
  /*!
      G-range, Optional parameters:
          - BMA4_ACCEL_RANGE_2G
          - BMA4_ACCEL_RANGE_4G
          - BMA4_ACCEL_RANGE_8G
          - BMA4_ACCEL_RANGE_16G
  */
  cfg.range = BMA4_ACCEL_RANGE_2G;
  /*!
      Bandwidth parameter, determines filter configuration, Optional parameters:
          - BMA4_ACCEL_OSR4_AVG1
          - BMA4_ACCEL_OSR2_AVG2
          - BMA4_ACCEL_NORMAL_AVG4
          - BMA4_ACCEL_CIC_AVG8
          - BMA4_ACCEL_RES_AVG16
          - BMA4_ACCEL_RES_AVG32
          - BMA4_ACCEL_RES_AVG64
          - BMA4_ACCEL_RES_AVG128
  */
  cfg.bandwidth = BMA4_ACCEL_NORMAL_AVG4;

  /*! Filter performance mode , Optional parameters:
      - BMA4_CIC_AVG_MODE
      - BMA4_CONTINUOUS_MODE
  */
  /* 连续模式：特征引擎（计步/翻腕）需要不间断数据流；低功耗采样可用 CIC_AVG_MODE */
  cfg.perf_mode = BMA4_CONTINUOUS_MODE;

  // Configure the BMA423 accelerometer
  sensor.setAccelConfig(cfg);

  // Enable BMA423 accelerometer
  // Warning : Need to use feature, you must first enable the accelerometer
  // Warning : Need to use feature, you must first enable the accelerometer
  sensor.enableAccel();

  struct bma4_int_pin_config config;
  /* INT1 = 电平触发、高有效、推挽输出：与 ESP32 深睡 ext 唤醒的电平语义对齐 */
  config.edge_ctrl = BMA4_LEVEL_TRIGGER;
  config.lvl       = BMA4_ACTIVE_HIGH;
  config.od        = BMA4_PUSH_PULL;
  config.output_en = BMA4_OUTPUT_ENABLE;
  config.input_en  = BMA4_INPUT_DISABLE;
  // The correct trigger interrupt needs to be configured as needed
  sensor.setINTPinConfig(config, BMA4_INTR1_MAP);

  struct bma423_axes_remap remap_data;
  /* 轴重映射：芯片贴片方向与表壳坐标系不一致，这里把传感器 Y→X、X→Y 对调
     （0xFF 表示符号取反），让"抬腕翻腕"的判定方向与用户视角一致 */
  remap_data.x_axis      = 1;
  remap_data.x_axis_sign = 0xFF;
  remap_data.y_axis      = 0;
  remap_data.y_axis_sign = 0xFF;
  remap_data.z_axis      = 2;
  remap_data.z_axis_sign = 0xFF;
  // Need to raise the wrist function, need to set the correct axis
  sensor.setRemapAxes(&remap_data);

  // Enable BMA423 isStepCounter feature
  sensor.enableFeature(BMA423_STEP_CNTR, true);
  // Enable BMA423 isTilt feature
  sensor.enableFeature(BMA423_TILT, true);
  // Enable BMA423 isDoubleClick feature
  sensor.enableFeature(BMA423_WAKEUP, true);

  // Reset steps
  sensor.resetStepCounter();

  // Turn on feature interrupt
  /* 三路中断映射到 INT1：计步翻腕抬腕。醒来后用 sensor.getINT() 读
     __IRQ_MASK 再 isStepCounter()/isTilt()/isDoubleClick() 区分来源 */
  sensor.enableStepCountInterrupt();
  sensor.enableTiltInterrupt();
  // It corresponds to isDoubleClick interrupt
  sensor.enableWakeupInterrupt();
}

/**
 * @brief  WiFi 配网页面（菜单项 Setup WiFi）：起 WiFiManager AP 热点让手机配网
 * @note   流程：resetSettings 清掉旧凭据 → autoConnect("Watchy AP") 起热点并
 *         阻塞等待（WIFI_AP_TIMEOUT=60s），手机连热点后弹 Portal 填 WiFi 密码；
 *         成功则把 SSID/IP 记进 RTC 慢存（About 页展示），失败显示超时。
 *         配网期间临时关掉屏幕 busy 回调的 light sleep（高频重绘冲突），
 *         结束后恢复。全程阻塞式，结束才把控制权交回按键循环。
 */
void Watchy::setupWifi() {
  display.epd2.setBusyCallback(0); // temporarily disable lightsleep on busy
  WiFiManager wifiManager;
  wifiManager.resetSettings();
  wifiManager.setTimeout(WIFI_AP_TIMEOUT);
  wifiManager.setAPCallback(_configModeCallback);
  display.setFullWindow();
  display.fillScreen(GxEPD_BLACK);
  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_WHITE);
  /* autoConnect 阻塞期间内嵌了 connecting→AP Portal 状态机，成功返回 true */
  if (!wifiManager.autoConnect(WIFI_AP_SSID)) { // WiFi setup failed
    display.println("Setup failed &");
    display.println("timed out!");
  } else {
    display.println("Connected to:");
    display.println(WiFi.SSID());
		display.println("Local IP:");
		display.println(WiFi.localIP());
    weatherIntervalCounter = -1; // Reset to force weather to be read again
    lastIPAddress = WiFi.localIP();
    WiFi.SSID().toCharArray(lastSSID, 30);
  }
  display.display(false); // full refresh
  // turn off radios
  WiFi.mode(WIFI_OFF);
  btStop();
  // enable lightsleep on busy
  display.epd2.setBusyCallback(WatchyDisplay::busyCallback);
  guiState = APP_STATE;
}

/**
 * @brief  WiFiManager 进入 AP 配网模式时的回调：屏幕上显示热点名与登录 IP
 * @note   用户照着屏幕连 "Watchy AP" 热点、浏览器开 192.168.4.1 填家里 WiFi。
 *         static 成员：WiFiManager 只持函数指针，无法传 this。
 */
void Watchy::_configModeCallback(WiFiManager *myWiFiManager) {
  display.setFullWindow();
  display.fillScreen(GxEPD_BLACK);
  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_WHITE);
  display.setCursor(0, 30);
  display.println("Connect to");
  display.print("SSID: ");
  display.println(WIFI_AP_SSID);
  display.print("IP: ");
  display.println(WiFi.softAPIP());
	display.println("MAC address:");
	display.println(WiFi.softAPmacAddress().c_str());
  display.display(false); // full refresh
}

/**
 * @brief  用已存凭据连 WiFi（同步阻塞，最长 ~10s），成功与否写进 WIFI_CONFIGURED
 * @return true=已连上（并记录 IP/SSID 到 RTC 慢存供 About 页显示）
 * @note   天气/NTP 之前的统一入口。WiFi.begin() 不带参数 = 用 NVS 里的凭据；
 *         失败路径务必关射频再返回（在调用方 _getWeatherData 里也会兜底关一次）。
 */
bool Watchy::connectWiFi() {
  if (WL_CONNECT_FAILED ==
      WiFi.begin()) { // WiFi not setup, you can also use hard coded credentials
                      // with WiFi.begin(SSID,PASS);
    WIFI_CONFIGURED = false;
  } else {
    if (WL_CONNECTED ==
        WiFi.waitForConnectResult()) { // attempt to connect for 10s
      lastIPAddress = WiFi.localIP();
      WiFi.SSID().toCharArray(lastSSID, 30);
      WIFI_CONFIGURED = true;
    } else { // connection failed, time out
      WIFI_CONFIGURED = false;
      // turn off radios
      WiFi.mode(WIFI_OFF);
      btStop();
    }
  }
  return WIFI_CONFIGURED;
}
/*
void Watchy::showUpdateFW() {
  display.setFullWindow();
  display.fillScreen(GxEPD_BLACK);
  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_WHITE);
  display.setCursor(0, 30);
  display.println("Please visit");
  display.println("watchy.sqfmi.com");
  display.println("with a Bluetooth");
  display.println("enabled device");
  display.println(" ");
  display.println("Press menu button");
  display.println("again when ready");
  display.println(" ");
  display.println("Keep USB powered");
  display.display(false); // full refresh

  guiState = FW_UPDATE_STATE;
}

void Watchy::updateFWBegin() {
  display.setFullWindow();
  display.fillScreen(GxEPD_BLACK);
  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_WHITE);
  display.setCursor(0, 30);
  display.println("Bluetooth Started");
  display.println(" ");
  display.println("Watchy BLE OTA");
  display.println(" ");
  display.println("Waiting for");
  display.println("connection...");
  display.display(false); // full refresh

  BLE BT;
  BT.begin("Watchy BLE OTA");
  int prevStatus = -1;
  int currentStatus;

  while (1) {
    currentStatus = BT.updateStatus();
    if (prevStatus != currentStatus || prevStatus == 1) {
      if (currentStatus == 0) {
        display.setFullWindow();
        display.fillScreen(GxEPD_BLACK);
        display.setFont(&FreeMonoBold9pt7b);
        display.setTextColor(GxEPD_WHITE);
        display.setCursor(0, 30);
        display.println("BLE Connected!");
        display.println(" ");
        display.println("Waiting for");
        display.println("upload...");
        display.display(false); // full refresh
      }
      if (currentStatus == 1) {
        display.setFullWindow();
        display.fillScreen(GxEPD_BLACK);
        display.setFont(&FreeMonoBold9pt7b);
        display.setTextColor(GxEPD_WHITE);
        display.setCursor(0, 30);
        display.println("Downloading");
        display.println("firmware:");
        display.println(" ");
        display.print(BT.howManyBytes());
        display.println(" bytes");
        display.display(true); // partial refresh
      }
      if (currentStatus == 2) {
        display.setFullWindow();
        display.fillScreen(GxEPD_BLACK);
        display.setFont(&FreeMonoBold9pt7b);
        display.setTextColor(GxEPD_WHITE);
        display.setCursor(0, 30);
        display.println("Download");
        display.println("completed!");
        display.println(" ");
        display.println("Rebooting...");
        display.display(false); // full refresh

        delay(2000);
        esp_restart();
      }
      if (currentStatus == 4) {
        display.setFullWindow();
        display.fillScreen(GxEPD_BLACK);
        display.setFont(&FreeMonoBold9pt7b);
        display.setTextColor(GxEPD_WHITE);
        display.setCursor(0, 30);
        display.println("BLE Disconnected!");
        display.println(" ");
        display.println("exiting...");
        display.display(false); // full refresh
        delay(1000);
        break;
      }
      prevStatus = currentStatus;
    }
    delay(100);
  }

  // turn off radios
  WiFi.mode(WIFI_OFF);
  btStop();
  showMenu(menuIndex, false);
}
*/
/**
 * @brief  "Sync NTP" 菜单页：连网 → NTP 对时 → 屏上回显新时间
 * @note   用户手动触发的对时入口；gmtOffset 优先用天气 API 学到的时区
 *         （RTC 慢存全局），没有联网过天气就是 settings.gmtOffset 的出厂值。
 *         结尾 delay(3000) 给用户读屏时间，然后回菜单。
 */
void Watchy::showSyncNTP() {
  display.setFullWindow();
  display.fillScreen(GxEPD_BLACK);
  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_WHITE);
  display.setCursor(0, 30);
  display.println("Syncing NTP... ");
  display.print("GMT offset: ");
  display.println(gmtOffset);
  display.display(false); // full refresh
  if (connectWiFi()) {
    if (syncNTP()) {
      display.println("NTP Sync Success\n");
      display.println("Current Time Is:");

      RTC.read(currentTime);

      display.print(tmYearToCalendar(currentTime.Year));
      display.print("/");
      display.print(currentTime.Month);
      display.print("/");
      display.print(currentTime.Day);
      display.print(" - ");

      if (currentTime.Hour < 10) {
        display.print("0");
      }
      display.print(currentTime.Hour);
      display.print(":");
      if (currentTime.Minute < 10) {
        display.print("0");
      }
      display.println(currentTime.Minute);
    } else {
      display.println("NTP Sync Failed");
    }
    WiFi.mode(WIFI_OFF);
    btStop();
  } else {
    display.println("WiFi Not Configured");
  }
  display.display(true); // full refresh
  delay(3000);
  showMenu(menuIndex, false);
}

/**
 * @brief  NTP 对时三连重载的无参版：用当前 gmtOffset + settings.ntpServer
 * @return true=对时成功（时间已写进 RTC 芯片）
 * @note   必须在 connectWiFi() 成功后调用，用完记得关射频（调用方负责）。
 */
bool Watchy::syncNTP() { // NTP sync - call after connecting to WiFi and
                         // remember to turn it back off
  return syncNTP(gmtOffset,
                 settings.ntpServer.c_str());
}

/**
 * @brief  指定时区的 NTP 对时（服务器仍用 settings.ntpServer）
 * @param  gmt  UTC 偏移秒数（如北京时间 3600*8）
 */
bool Watchy::syncNTP(long gmt) {
  return syncNTP(gmt, settings.ntpServer.c_str());
}

/**
 * @brief  NTP 对时实现：UDP 向 NTP 服务器取 Unix 时间 → 折算 tmElements_t → 写 RTC
 * @param  gmt        UTC 偏移秒数
 * @param  ntpServer  NTP 服务器域名（默认 pool.ntp.org）
 * @return false=UDP 请求失败/超时（NTPClient 内部 2 次重试）
 * @note   天气流程里会以响应里的 timezone 字段调本重载，实现"漂到哪自动对到当地时间"。
 *         breakTime 把偏移后的 epoch 拆成日历字段再写芯片，RTC 里存的是本地时间。
 */
bool Watchy::syncNTP(long gmt, String ntpServer) {
  // NTP sync - call after connecting to
  // WiFi and remember to turn it back off
  WiFiUDP ntpUDP;
  NTPClient timeClient(ntpUDP, ntpServer.c_str(), gmt);
  timeClient.begin();
  if (!timeClient.forceUpdate()) {
    return false; // NTP sync failed
  }
  tmElements_t tm;
  breakTime((time_t)timeClient.getEpochTime(), tm);
  RTC.set(tm);
  return true;
}
