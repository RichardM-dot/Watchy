/*
 * 【中文导读】WatchyRTC.h —— RTC 抽象层头文件（B级导读）
 */

#ifndef WATCHY_RTC_H
#define WATCHY_RTC_H

#include "config.h"
#include "time.h"
#include <DS3232RTC.h>
#include <Rtc_Pcf8563.h>

/* 两代硬件的两颗 RTC：探测到谁就用谁（rtcType 记录），上层代码零感知 */
#define DS3231          1     // rtcType：Maxim DS3231（Watchy v1.0）
#define PCF8563         2     // rtcType：NXP PCF8563（Watchy v1.5 / v2.0）
#define RTC_DS_ADDR     0x68  // DS3231 的 7 位 I2C 地址
#define RTC_PCF_ADDR    0x51  // PCF8563 的 7 位 I2C 地址
#define YEAR_OFFSET_DS  1970  // DS3232RTC 库的时间基准年（time_t 纪元）
#define YEAR_OFFSET_PCF 2000  // PCF8563 寄存器里年份是 00-99 的 BCD（2000 基）

/**
 * @class WatchyRTC
 * @brief RTC 抽象层：把 DS3231 与 PCF8563 两颗芯片统一成 read/set/config/
 *        clearAlarm 五个接口，Watchy 主类完全不知道底下是哪颗芯片
 * @note  两个第三方库对象都直接作为成员存在（rtc_ds / rtc_pcf），
 *        init() 靠 I2C 探测决定用哪个。闹钟策略固定为"每分钟中断一次"，
 *        INT 引脚接 ESP32 的 RTC_INT_PIN(27)，是深睡唤醒的 ext0 源。
 *        注意：_getDayOfWeek() 在头文件声明但 .cpp 从未定义/调用（死声明）。
 */
class WatchyRTC {
public:
  DS3232RTC rtc_ds;     // DS3231 驱动对象（JChristensen/DS3232RTC 库）
  Rtc_Pcf8563 rtc_pcf;  // PCF8563 驱动对象（Rtc_Pcf8563 库）
  uint8_t rtcType;      // DS3231(1) / PCF8563(2)，init() 探测后赋值

public:
  WatchyRTC();
  /* I2C 探测两颗候选地址，确定 rtcType 并 begin 对应驱动 */
  void init();
  /* 首次配置：datetime 非空则写入初始时间，并配好"每分钟闹钟"（冷启动用） */
  void config(String datetime); // String datetime format is YYYY:MM:DD:HH:MM:SS
  /* 清闹钟标志并预约下一分钟的闹钟——每次深睡前必须调（deepSleep 调它） */
  void clearAlarm();
  /* 读当前时间进 tmElements_t（Watchy 的 currentTime） */
  void read(tmElements_t &tm);
  /* 把 tmElements_t 写进芯片（NTP 对时/手动对时之后调） */
  void set(tmElements_t tm);
  /* 芯片温度：DS3231 有真实温度传感器；PCF8563 没有则返回 255 表示无效 */
  uint8_t temperature();

private:
  void _DSConfig(String datetime);
  void _PCFConfig(String datetime);
  int _getDayOfWeek(int d, int m, int y);  // 死声明：无定义、无调用（勿删，仅记录）
  String _getValue(String data, char separator, int index);
};

#endif