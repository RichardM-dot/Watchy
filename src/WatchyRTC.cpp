/*
 * 【中文导读】WatchyRTC.cpp —— RTC 抽象层
 * 兼容 PCF8563(新款) 与 DS3231(旧款) 两颗 RTC 的统一接口：读秒/分/时、
 * 设定每分钟中断（表盘刷新的唤醒源）。
 */

#include "WatchyRTC.h"

WatchyRTC::WatchyRTC(){}

/**
 * @brief  探测 RTC 型号：先敲 DS3231(0x68) 的门，无 ACK 再敲 PCF8563(0x51)
 * @note   利用 I2C endTransmission 的 ACK/NACK 做存在性探测，两颗芯片
 *         不会同时在线（一块板只焊一颗）。两个都没 ACK 时静默失败
 *         （rtcType 保持 0，后续按 PCF8563 分支走）——排查硬件先看这里。
 *         调用时机：Watchy::init() 每次唤醒都调（探测很便宜，就一次 I2C 事务）。
 */
void WatchyRTC::init() {
  byte error;
  Wire.beginTransmission(RTC_DS_ADDR);
  error = Wire.endTransmission();
  if (error == 0) {
    rtcType = DS3231;
    rtc_ds.begin();
  } else {
    Wire.beginTransmission(RTC_PCF_ADDR);
    error = Wire.endTransmission();
    if (error == 0) {
      rtcType = PCF8563;
    } else {
      // RTC Error
    }
  }
}

/**
 * @brief  RTC 首次配置分派（按 rtcType 走对应芯片的私有配置）
 * @param  datetime  "YYYY:MM:DD:HH:MM:SS"（空串 = 只配闹钟不改时间）
 * @note   仅冷启动（init 的 default 分支）调用；深睡唤醒路径不经过这里，
 *         所以 RTC 芯片的闹钟配置在整个生命周期里只发生一次。
 */
void WatchyRTC::config(
    String datetime) { // String datetime format is YYYY:MM:DD:HH:MM:SS
  if (rtcType == DS3231) {
    _DSConfig(datetime);
  } else {
    _PCFConfig(datetime);
  }
}

/**
 * @brief  清掉闹钟触发标志，并预约"下一分钟"的闹钟——深睡前必调
 * @note   Watchy 的心跳源头：PCF8563 的闹钟只比较 分/时/日/星期 里给出的
 *         字段，这里给 (nextMinute, 99, 99, 99)——99 是"不比较"哨兵值，
 *         即每逢 nextMinute:00 就拉低 INT；DS3231 则用 ALM2_EVERY_MINUTE 模式。
 *         标志位不清的话 INT 会一直保持有效电平，醒来立刻又"被唤醒"。
 */
void WatchyRTC::clearAlarm() {
  if (rtcType == DS3231) {
    rtc_ds.alarm(DS3232RTC::ALARM_2);
  } else {
    /* PCF8563：clearAlarm 复位标志后，把闹钟设到下一分钟（59 分则绕回 0 点） */
    int nextAlarmMinute = 0;
    rtc_pcf.clearAlarm(); // resets the alarm flag in the RTC
    nextAlarmMinute = rtc_pcf.getMinute();
    nextAlarmMinute =
        (nextAlarmMinute == 59)
            ? 0
            : (nextAlarmMinute + 1); // set alarm to trigger 1 minute from now
    rtc_pcf.setAlarm(nextAlarmMinute, 99, 99, 99);
  }
}

/**
 * @brief  读当前时间进 tmElements_t（Watchy::currentTime 的数据源）
 * @note   PCF8563 分支要手动拼 tmElements_t：它的驱动一次只回一个字段
 *         （getDate/getHour/...各触发一次 I2C 读），共 6~7 次 I2C 事务；
 *         Wday 域差异：TimeLib 用 1-7（周日=1），PCF8563 寄存器是 0-6，须 +1。
 *         年份域差异：tm.Year 存"自 1970"（DS3231 路径）或"自 2000"换算前
 *         （PCF8563 路径 y2kYearToTm），显示时用 tmYearToCalendar 还原。
 */
void WatchyRTC::read(tmElements_t &tm) {
  if (rtcType == DS3231) {
    rtc_ds.read(tm);
  } else {
    rtc_pcf.getDate();
    tm.Year  = y2kYearToTm(rtc_pcf.getYear());
    tm.Month = rtc_pcf.getMonth();
    tm.Day   = rtc_pcf.getDay();
    tm.Wday =
        rtc_pcf.getWeekday() + 1; // TimeLib & DS3231 has Wday range of 1-7, but
                                  // PCF8563 stores day of week in 0-6 range
    tm.Hour   = rtc_pcf.getHour();
    tm.Minute = rtc_pcf.getMinute();
    tm.Second = rtc_pcf.getSecond();
  }
}

/**
 * @brief  写时间进芯片（NTP 对时/手动对时的落点）
 * @note   PCF8563 分支先 makeTime→breakTime 往返一次，是为了让 TimeLib
 *         替你算出星期几（tm.Wday）——芯片要求写入时带 Wday。
 *         写完顺手 clearAlarm()：改写寄存器过程中闹钟标志可能被扰动，重约一次。
 */
void WatchyRTC::set(tmElements_t tm) {
  if (rtcType == DS3231) {
    time_t t = makeTime(tm);
    rtc_ds.set(t);
  } else {
    time_t t = makeTime(tm); // make and break to calculate tm.Wday
    breakTime(t, tm);
    // day, weekday, month, century(1=1900, 0=2000), year(0-99)
    rtc_pcf.setDate(
        tm.Day, tm.Wday - 1, tm.Month, 0,
        tmYearToY2k(tm.Year)); // TimeLib & DS3231 has Wday range of 1-7, but
                               // PCF8563 stores day of week in 0-6 range
    // hr, min, sec
    rtc_pcf.setTime(tm.Hour, tm.Minute, tm.Second);
    clearAlarm();
  }
}

/**
 * @brief  芯片温度（℃ 整数）：DS3231 内置温度传感器可用于粗略体温/气温
 * @return PCF8563 无温度传感器，返回 255 = 无效值（调用方需判断）
 */
uint8_t WatchyRTC::temperature() {
  if (rtcType == DS3231) {
    return rtc_ds.temperature();
  } else {
    return 255; // error
  }
}

/**
 * @brief  DS3231 专属配置：可选写入初始时间 + 配置"每分钟"闹钟中断
 * @param  datetime  非空则解析写入；空串跳过（保留芯片当前时间）
 * @note   ALARM_2 是"分钟匹配"闹钟（ALM2_EVERY_MINUTE = 秒匹配 00），
 *         触发时拉低 SQW/INT 引脚 → ESP32 ext0 唤醒。方波输出关掉
 *         （SQWAVE_NONE）省电且不与闹钟引脚冲突。
 */
void WatchyRTC::_DSConfig(
    String datetime) { // String datetime is YYYY:MM:DD:HH:MM:SS
  if (datetime != "") {
    tmElements_t tm;
    tm.Year = CalendarYrToTm(_getValue(datetime, ':', 0).toInt()); // YYYY -
                                                                   // 1970
    tm.Month  = _getValue(datetime, ':', 1).toInt();
    tm.Day    = _getValue(datetime, ':', 2).toInt();
    tm.Hour   = _getValue(datetime, ':', 3).toInt();
    tm.Minute = _getValue(datetime, ':', 4).toInt();
    tm.Second = _getValue(datetime, ':', 5).toInt();
    time_t t  = makeTime(tm);
    rtc_ds.set(t);
  }
  // https://github.com/JChristensen/DS3232RTC
  rtc_ds.squareWave(DS3232RTC::SQWAVE_NONE); // disable square wave output
  rtc_ds.setAlarm(DS3232RTC::ALM2_EVERY_MINUTE, 0, 0, 0,
                  0); // alarm wakes up Watchy every minute
  rtc_ds.alarmInterrupt(DS3232RTC::ALARM_2, true); // enable alarm interrupt
}

/**
 * @brief  PCF8563 专属配置：可选写入初始时间 + 借 clearAlarm() 配好每分钟闹钟
 * @param  datetime  "YYYY:MM:DD:HH:MM:SS"（空串 = 只配闹钟）
 * @note   上电复位（POR）后 PCF8563 的月份寄存器是 0（非法，合法域 1-12），
 *         所以无论如何都要调一次 clearAlarm() 把时间/闹钟寄存器拉回合法状态。
 */
void WatchyRTC::_PCFConfig(
    String datetime) { // String datetime is YYYY:MM:DD:HH:MM:SS
  if (datetime != "") {
    tmElements_t tm;
    tm.Year = CalendarYrToTm(_getValue(datetime, ':', 0).toInt()); // YYYY -
                                                                   // 1970
    tm.Month  = _getValue(datetime, ':', 1).toInt();
    tm.Day    = _getValue(datetime, ':', 2).toInt();
    tm.Hour   = _getValue(datetime, ':', 3).toInt();
    tm.Minute = _getValue(datetime, ':', 4).toInt();
    tm.Second = _getValue(datetime, ':', 5).toInt();
    time_t t  = makeTime(tm); // make and break to calculate tm.Wday
    breakTime(t, tm);
    // day, weekday, month, century(1=1900, 0=2000), year(0-99)
    rtc_pcf.setDate(
        tm.Day, tm.Wday - 1, tm.Month, 0,
        tmYearToY2k(tm.Year)); // TimeLib & DS3231 has Wday range of 1-7, but
                               // PCF8563 stores day of week in 0-6 range
    // hr, min, sec
    rtc_pcf.setTime(tm.Hour, tm.Minute, tm.Second);
  }
  // on POR event, PCF8563 sets month to 0, which will give an error since
  // months are 1-12
  clearAlarm();
}

/**
 * @brief  按 separator 切分字符串取第 index 段（解析 "YYYY:MM:DD:HH:MM:SS" 用）
 * @return 第 index 段；段数不够返回空串
 * @note   O(n) 单趟扫描；strIndex 记录"当前段起点/终点"滑动窗口。
 *         Arduino String 的土办法 split，避免引入 STL（嵌入式省 flash）。
 */
String WatchyRTC::_getValue(String data, char separator, int index) {
  int found      = 0;
  int strIndex[] = {0, -1};
  int maxIndex   = data.length() - 1;

  for (int i = 0; i <= maxIndex && found <= index; i++) {
    if (data.charAt(i) == separator || i == maxIndex) {
      found++;
      strIndex[0] = strIndex[1] + 1;
      strIndex[1] = (i == maxIndex) ? i + 1 : i;
    }
  }

  return found > index ? data.substring(strIndex[0], strIndex[1]) : "";
}
