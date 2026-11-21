/*
 * 【中文导读】Watchy.h —— 主类声明（B级导读）
 * 看这里就知道用户能重写什么：drawWatchFace/handleButton/wifi/wMenu 等。
 * API 设计样本：把"固件"做成库，用户继承定制。
 */

#ifndef WATCHY_H
#define WATCHY_H

#include <Arduino.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Arduino_JSON.h>
#include <GxEPD2_BW.h>
#include <Wire.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include "DSEG7_Classic_Bold_53.h"
#include "Display.h"
#include "BLE.h"
#include "bma.h"
#include "config.h"
#include "esp_chip_info.h"
#ifdef ARDUINO_ESP32S3_DEV
  #include "Watchy32KRTC.h"
  #include "soc/rtc.h"
  #include "soc/rtc_io_reg.h"
  #include "soc/sens_reg.h"
  #include "esp_sleep.h"
  #include "rom/rtc.h"
  #include "soc/soc.h"
  #include "soc/rtc_cntl_reg.h"
  #include "time.h"
  #include "esp_sntp.h"
  #include "hal/rtc_io_types.h"
  #include "driver/rtc_io.h"
  #define uS_TO_S_FACTOR 1000000ULL  //Conversion factor for micro seconds to seconds
  #define ADC_VOLTAGE_DIVIDER ((360.0f+100.0f)/360.0f) //Voltage divider at battery ADC  
#else
  #include "WatchyRTC.h"
#endif

/**
 * @brief 天气数据包：表盘绘制天气区域时的唯一输入
 * @note  由 Watchy::getWeatherData() 填充；无网时 temperature 来自 BMA423
 *        芯片温度、external=false。整个结构体在 RTC 慢存里缓存（Watchy.cpp
 *        的 currentWeather 全局），跨深睡保持。
 */
typedef struct weatherData {
  int8_t temperature;               // 气温：metric=℃，imperial=℉
  int16_t weatherConditionCode;     // OpenWeatherMap 天气码（800=晴，5xx=雨…）
  bool isMetric;                    // 单位制，决定表盘画 ℃ 还是 ℉ 图标
  String weatherDescription;        // 天气短语（"Clouds"等，JSONVar::stringify）
  bool external;                    // true=来自天气 API；false=本地温度退化值
  tmElements_t sunrise;             // 日出时刻（API 的 sys.sunrise 拆分）
  tmElements_t sunset;              // 日落时刻（API 的 sys.sunset 拆分）
} weatherData;

/**
 * @brief 用户配置包：examples/WatchFaces 各表盘目录的 settings.h 里用
 *        指定初始化器填好，经构造函数 Watchy(settings) 传入，库内部全部只读
 * @note  weatherURL 是带 {cityID}/{lat}/{lon}/{units}/{lang}/{apiKey} 占位符
 *        的 OpenWeatherMap 模板；gmtOffset 单位是秒，且会被天气 API 的
 *        timezone 字段覆盖（见 _getWeatherData）。
 */
typedef struct watchySettings {
  // Weather Settings
  String cityID;                    // OpenWeatherMap 城市 ID（与 lat/lon 二选一）
  String lat;                       // 纬度（cityID 为空时启用）
  String lon;                       // 经度
  String weatherAPIKey;             // OpenWeatherMap APPID
  String weatherURL;                // URL 模板（含占位符）
  String weatherUnit;               // "metric"/"imperial"
  String weatherLang;               // 天气描述语言
  int8_t weatherUpdateInterval;     // 天气刷新间隔（分钟），默认 30
  // NTP Settings
  String ntpServer;                 // NTP 服务器，默认 pool.ntp.org
  int gmtOffset;                    // UTC 偏移秒（出厂值，联网后被覆盖）
  //
  bool vibrateOClock;               // 整点是否震动提醒（init() 的 tick 分支用）
} watchySettings;

/**
 * @class Watchy
 * @brief 手表主类：把"固件"做成库的核心——用户继承并重写 drawWatchFace()
 *        即得自己的表盘，基类负责全部底层：唤醒分派/深睡/菜单/RTC/BMA/WiFi。
 * @note  典型用法见 examples/WatchFaces/Basic/Basic.ino：
 *          class WatchyBasic : public Watchy { void drawWatchFace() override; };
 *        可重写的虚函数只有两个：drawWatchFace() 与 handleButtonPress()，
 *        其余全是基类提供的服务（画菜单/对时/读电池/天气/震动）。
 *        生命周期：setup() 里 watchy.init() → 内部"深睡↔唤醒"循环永驻。
 */
class Watchy {
public:
  /* RTC 与 display 都是 static 单例：全工程共享一块屏幕和一颗 RTC，
     用户表盘里直接 Watchy::display / watchy.RTC 使用 */
  #ifdef ARDUINO_ESP32S3_DEV
   static Watchy32KRTC RTC;   // V3：用 ESP32-S3 内部 RTC + 32K 晶振（无外置 RTC）
  #else
   static WatchyRTC RTC;      // V1/V2：外置 DS3231 或 PCF8563（init 时探测）
  #endif
  static GxEPD2_BW<WatchyDisplay, WatchyDisplay::HEIGHT> display;
  tmElements_t currentTime;   // 最近一次 RTC.read() 的结果（表盘直接用）
  watchySettings settings;    // 构造时传入的用户配置（只读）

public:
  /* ---------- 生命周期与基础服务 ---------- */
  explicit Watchy(const watchySettings &s) : settings(s) {} // constructor
  /* init：总入口。.ino 的 setup() 调一次；内部按唤醒原因分派并深睡 */
  void init(String datetime = "");
  /* deepSleep：一切流程的终点，配置唤醒源后 esp_deep_sleep_start() */
  void deepSleep();
  /* 电池电压（V）；板卡版本号（10/15/20/30）；马达方波震动 */
  float getBatteryVoltage();
  uint8_t getBoardRevision();
  void vibMotor(uint8_t intervalMs = 100, uint8_t length = 20);

  /* ---------- 菜单与按键（guiState 状态机，详见 Docs/04） ---------- */
  /* handleButtonPress 是 virtual：用户可整盘接管按键语义 */
  virtual void handleButtonPress();
  void showMenu(byte menuIndex, bool partialRefresh);
  void showFastMenu(byte menuIndex);
  /* 内置 App 页面：About/Buzz/加速计/对时/WiFi配网/NTP；FW 更新页已注释停用 */
  void showAbout();
  void showBuzz();
  void showAccelerometer();
  void showUpdateFW();
  void showSyncNTP();
  /* NTP 三连重载：无参 / 指定时区 / 指定服务器；须先 connectWiFi() */
  bool syncNTP();
  bool syncNTP(long gmt);
  bool syncNTP(long gmt, String ntpServer);
  void setTime();
  void setupWifi();
  bool connectWiFi();
  weatherData getWeatherData();
  void updateFWBegin();

  /* ---------- 表盘渲染（用户重写点） ---------- */
  void showWatchFace(bool partialRefresh);
  virtual void drawWatchFace(); // override this method for different watch
                                // faces

private:
  /* BMA423 全配置（冷启动一次）；WiFiManager AP 模式回调（static） */
  void _bmaConfig();
  static void _configModeCallback(WiFiManager *myWiFiManager);
  /* Bosch 驱动的 I2C 读写回调（static，注册进 bma4_dev），详见 Docs/03 */
  static uint16_t _readRegister(uint8_t address, uint8_t reg, uint8_t *data,
                                uint16_t len);
  static uint16_t _writeRegister(uint8_t address, uint8_t reg, uint8_t *data,
                                 uint16_t len);
  weatherData _getWeatherData(String cityID, String lat, String lon, String units, String lang,
                             String url, String apiKey, uint8_t updateInterval);
};

/* ---------------------------------------------------------------------------
 * 跨深睡全局状态（定义在 Watchy.cpp，RTC 慢存里 preserved）：
 *   guiState       界面状态机：-1 表盘 / 0 主菜单 / 1 App页 / 2 固件更新
 *   menuIndex      当前菜单选中项 0~5
 *   sensor         BMA423 封装对象（深睡中芯片自己继续计步）
 *   WIFI_CONFIGURED / BLE_CONFIGURED  网络/蓝牙是否配置过（表盘画图标用）
 *   USB_PLUGGED_IN V3 专用：USB 插入标志
 * ------------------------------------------------------------------------- */
extern RTC_DATA_ATTR int guiState;
extern RTC_DATA_ATTR int menuIndex;
extern RTC_DATA_ATTR BMA423 sensor;
extern RTC_DATA_ATTR bool WIFI_CONFIGURED;
extern RTC_DATA_ATTR bool BLE_CONFIGURED;
extern RTC_DATA_ATTR bool USB_PLUGGED_IN;

#endif
