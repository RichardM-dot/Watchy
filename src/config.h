/*
 * 【中文导读】config.h —— 引脚定义与硬件配置（B级导读）
 * 屏幕引脚、按键、RTC/BMA 地址等，改硬件接线从这入手。
 */

/*
 * 【B级导读·扩展】config.h —— 全工程的"硬件接线图"与常量表
 * ----------------------------------------------------------------------------
 * 四段内容：
 *   1) 板级宏（ARDUINO_WATCHY_V10/V15/V20 / ARDUINO_ESP32S3_DEV）：
 *      Arduino IDE 里选哪块 Watchy 板，这里就展开哪套引脚——同一份库
 *      覆盖 v1.0/v1.5/v2.0（ESP32）/v3.0（ESP32-S3）四种硬件。
 *   2) 引脚宏：屏幕 SPI(CS/DC/RST/BUSY)、4 按键、加速度计 INT1/INT2、
 *      马达、电池 ADC、RTC 中断。V3 的按键是低有效（active-low），
 *      V1/V2 高有效——Watchy.cpp 用 ACTIVE_LOW 宏吸收这个差异。
 *   3) 屏幕/WiFi/菜单常量：200x200 分辨率、AP 名、guiState 取值、
 *      菜单项高 25px 共 6 项、对时字段序号。
 *   4) BLE OTA 版本号常量（BLE.cpp 广播用）。
 * 数据流位置：被所有文件 include，是唯一知道"哪个功能接在哪个 GPIO"的文件。
 * 注意 BTN_PIN_MASK 等位掩码宏是单行定义，勿改成多行续行（\）。
 */

#ifndef CONFIG_H
#define CONFIG_H

// Versioning
#define WATCHY_LIB_VER "1.4.14"

//pins

#ifdef ARDUINO_ESP32S3_DEV //V3

#define WATCHY_V3_SDA 12
#define WATCHY_V3_SCL 11

#define WATCHY_V3_SS    33
#define WATCHY_V3_MOSI  48
#define WATCHY_V3_MISO  46
#define WATCHY_V3_SCK   47

#define MENU_BTN_PIN  7
#define BACK_BTN_PIN  6
#define UP_BTN_PIN    0
#define DOWN_BTN_PIN  8

#define DISPLAY_CS    33
#define DISPLAY_DC    34
#define DISPLAY_RES   35
#define DISPLAY_BUSY  36
#define ACC_INT_1_PIN 14
#define ACC_INT_2_PIN 13
#define VIB_MOTOR_PIN 17
#define BATT_ADC_PIN 9
#define CHRG_STATUS_PIN 10
#define USB_DET_PIN 21
#define RTC_INT_PIN -1 //not used

#define MENU_BTN_MASK (BIT64(7))
#define BACK_BTN_MASK (BIT64(6))
#define UP_BTN_MASK   (BIT64(0))
#define DOWN_BTN_MASK (BIT64(8))
#define ACC_INT_MASK  (BIT64(14))
#define BTN_PIN_MASK  MENU_BTN_MASK|BACK_BTN_MASK|UP_BTN_MASK|DOWN_BTN_MASK

#else //V1,V1.5,V2

#if !defined(ARDUINO_WATCHY_V10) && !defined(ARDUINO_WATCHY_V15) && !defined(ARDUINO_WATCHY_V20)

#pragma message "Please install the latest ESP32 Arduino Core (2.0.5+) and choose Watchy as the target board"
#pragma message "Hardware revision is not defined at the project level, please define in config.h. Defaulting to ARDUINO_WATCHY_V20"

#define ARDUINO_WATCHY_V20

#define MENU_BTN_PIN 26
#define BACK_BTN_PIN 25
#define DOWN_BTN_PIN 4
#define DISPLAY_CS 5
#define DISPLAY_RES 9
#define DISPLAY_DC 10
#define DISPLAY_BUSY 19
#define ACC_INT_1_PIN 14
#define ACC_INT_2_PIN 12
#define VIB_MOTOR_PIN 13
#define RTC_INT_PIN 27

#if defined (ARDUINO_WATCHY_V10)
    #define UP_BTN_PIN 32
    #define BATT_ADC_PIN 33
    #define UP_BTN_MASK  (BIT64(32))
    #define RTC_TYPE 1 //DS3231
#elif defined (ARDUINO_WATCHY_V15)
    #define UP_BTN_PIN 32
    #define BATT_ADC_PIN 35
    #define UP_BTN_MASK  (BIT64(32))
    #define RTC_TYPE 2 //PCF8563
#elif defined (ARDUINO_WATCHY_V20)
    #define UP_BTN_PIN 35
    #define BATT_ADC_PIN 34
    #define UP_BTN_MASK  (BIT64(35))
    #define RTC_TYPE 2 //PCF8563
#endif

#define MENU_BTN_MASK (BIT64(26))
#define BACK_BTN_MASK (BIT64(25))
#define DOWN_BTN_MASK (BIT64(4))
#define ACC_INT_MASK  (BIT64(14))
#define BTN_PIN_MASK  MENU_BTN_MASK|BACK_BTN_MASK|UP_BTN_MASK|DOWN_BTN_MASK

#endif

#endif

//display
#define DISPLAY_WIDTH 200
#define DISPLAY_HEIGHT 200
// wifi
#define WIFI_AP_TIMEOUT 60
#define WIFI_AP_SSID    "Watchy AP"
// menu
#define WATCHFACE_STATE -1
#define MAIN_MENU_STATE 0
#define APP_STATE       1
#define FW_UPDATE_STATE 2
#define MENU_HEIGHT     25
#define MENU_LENGTH     6
// set time
#define SET_HOUR   0
#define SET_MINUTE 1
#define SET_YEAR   2
#define SET_MONTH  3
#define SET_DAY    4
#define HOUR_12_24 24
// BLE OTA
#define BLE_DEVICE_NAME        "Watchy BLE OTA"
#define WATCHFACE_NAME         "Watchy 7 Segment"
#define SOFTWARE_VERSION_MAJOR 1
#define SOFTWARE_VERSION_MINOR 0
#define SOFTWARE_VERSION_PATCH 0
#define HARDWARE_VERSION_MAJOR 1
#define HARDWARE_VERSION_MINOR 0

#endif