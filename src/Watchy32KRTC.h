/*
 * 【B级导读】Watchy32KRTC.h —— V3(S3) 用"ESP32-S3 内部 RTC + 外部 32K 晶振"的
 * WatchyRTC 等价实现（仅 ARDUINO_ESP32S3_DEV 编译时被 Watchy.h 选用）。
 * ----------------------------------------------------------------------------
 * 背景：Watchy v3.0 硬件上取消了独立 RTC 芯片（省成本省 I2C），改用
 *   S3 内部定时器 + 板载 32.768kHz 晶振计时；但接口签名与 WatchyRTC
 *   完全一致（init/config/clearAlarm/read/set/temperature），主类无感切换。
 * 数据流位置：time() 系统 API（内部分频自 32K 晶振）→ localtime_r →
 *   tmElements_t；写入走 settimeofday()。
 * 注意：clearAlarm() 是空实现——每分钟唤醒由 deepSleep() 里的
 *   esp_sleep_enable_timer_wakeup() 直接定到下一整分完成（见 Watchy.cpp）。
 */

#ifndef WATCHY_32K_RTC_H
#define WATCHY_32K_RTC_H

#include <Arduino.h>
#include <TimeLib.h>
#include "config.h"

class Watchy32KRTC {
public:
  Watchy32KRTC();
  void init();
  void config(String datetime); //datetime format is YYYY:MM:DD:HH:MM:SS
  void clearAlarm();
  void read(tmElements_t &tm);
  void set(tmElements_t tm);
  uint8_t temperature();

private:
  String _getValue(String data, char separator, int index);
  void _timeval_to_tm(struct timeval *tv, struct tm *tm);
};

#endif