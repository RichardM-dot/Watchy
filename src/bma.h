/*
 * 【中文导读】bma.h —— BMA423 封装类声明（B级导读）
 */

#pragma once

#ifdef ARDUINO
#include <Arduino.h>
#else
#include <stdlib.h>
#endif

#include "bma423.h"

/* getDirection() 的 6 个朝向：按"哪根轴的分量绝对值最大 + 正负"判定，
   表盘可用它做横竖屏切换；DISP_UP/DISP_DOWN = 屏幕朝上/朝下（z 轴主导） */
enum {
  DIRECTION_TOP_EDGE    = 0,
  DIRECTION_BOTTOM_EDGE = 1,
  DIRECTION_LEFT_EDGE   = 2,
  DIRECTION_RIGHT_EDGE  = 3,
  DIRECTION_DISP_UP     = 4,
  DIRECTION_DISP_DOWN   = 5
};

/* Bosch 驱动类型重命名：调用侧少写一串 struct 前缀 */
typedef struct bma4_accel Accel;        // 三轴原始读数 x/y/z（16bit 有符号）
typedef struct bma4_accel_config Acfg;  // 加速度计配置（ODR/量程/带宽/性能模式）

/**
 * @class BMA423
 * @brief Bosch BMA423 三轴加速度计的 Watchy 封装：原始驱动(bma4/bma423.c)
 *        之上的一层薄皮——I2C 回调注入、计步/翻腕/双击特性的开关与状态查询
 * @note  数据流位置：芯片 I2C(0x18) ← _readRegister/_writeRegister 回调
 *        （Watchy.cpp 提供）← 本类 ← Watchy::_bmaConfig() 冷启动配置，
 *        深睡期间芯片独立计步，ESP32 醒来 getCounter() 读累计值。
 *        对象本体放 RTC 慢存（RTC_DATA_ATTR BMA423 sensor），成员指针
 *        指向的静态回调函数地址跨深睡不变，因此醒来不必重新 begin()。
 *        IRQ 状态读取模型：中断发生时芯片置 INT 状态寄存器位，本类用
 *        getINT() 一次性读进 __IRQ_MASK，再由 is*() 系列位测试查询。
 */
class BMA423 {

public:
  BMA423();
  ~BMA423();

  /* 初始化：注入 I2C 读/写/延时三回调 → 软复位 → 载入特征引擎固件
     （config file ~1KB）→ 配 INT1 引脚。返回 false 时查 getErrorCode() */
  bool begin(bma4_com_fptr_t readCallBlack, bma4_com_fptr_t writeCallBlack,
             bma4_delay_fptr_t delayCallBlack,
             uint8_t address = BMA4_I2C_ADDR_PRIMARY);

  /* 复位与电源管理：shutDown/wakeUp 操作芯片的高级省电模式位 */
  void softReset();
  void shutDown();
  void wakeUp();
  bool selfTest();

  /* 由三轴分量判 6 朝向（见上方枚举），Show Accelerometer 页面用 */
  uint8_t getDirection();

  /* 加速度计的配置/读数/使能：ODR 默认 100Hz、量程 2G（_bmaConfig 配） */
  bool setAccelConfig(Acfg &cfg);
  bool getAccelConfig(Acfg &cfg);
  bool getAccel(Accel &acc);
  bool getAccelEnable();
  bool disableAccel();
  bool enableAccel(bool en = true);

  /* 中断引脚电气配置与 IRQ 状态：getINT 读一次 → is*() 系列判来源 */
  bool setINTPinConfig(struct bma4_int_pin_config config, uint8_t pinMap);
  bool getINT();
  uint8_t getIRQMASK();
  bool disableIRQ(uint16_t int_map = BMA423_STEP_CNTR_INT);
  bool enableIRQ(uint16_t int_map = BMA423_STEP_CNTR_INT);
  /* 中断来源查询：都是对 __IRQ_MASK 的位测试（getINT 之后才有效） */
  bool isStepCounter();
  bool isDoubleClick();
  bool isTilt();
  bool isActivity();
  bool isAnyNoMotion();

  /* 计步：芯片特征引擎自主累计，MCU 只读结果（跨深睡不丢，芯片不断电） */
  bool resetStepCounter();
  uint32_t getCounter();

  /* 芯片温度（BMA423 自带温度传感器，无网时的"天气"退化来源） */
  float readTemperature();
  float readTemperatureF();

  /* 错误码/状态/传感器时间（39bit 自由运行计数器，用于时序调试） */
  uint16_t getErrorCode();
  uint16_t getStatus();
  uint32_t getSensorTime();

  const char *getActivity();
  bool setRemapAxes(struct bma423_axes_remap *remap_data);

  /* 特性开关：enableFeature 开芯片内部特征引擎，enable*Interrupt 把
     对应中断映射到 INT1 引脚；默认参数恰好对应 Watchy 的三件套配置 */
  bool enableFeature(uint8_t feature, uint8_t enable);
  bool enableStepCountInterrupt(bool en = true);
  bool enableTiltInterrupt(bool en = true);
  bool enableWakeupInterrupt(bool en = true);
  bool enableAnyNoMotionInterrupt(bool en = true);
  bool enableActivityInterrupt(bool en = true);

private:
  bma4_com_fptr_t __readRegisterFptr;    // I2C 读回调（Watchy::_readRegister）
  bma4_com_fptr_t __writeRegisterFptr;   // I2C 写回调（Watchy::_writeRegister）
  bma4_delay_fptr_t __delayCallBlackFptr;// ms 级延时回调（Arduino delay）

  uint8_t __address;          // I2C 地址（默认 0x18）
  uint16_t __IRQ_MASK;        // 最近一次 getINT() 读到的中断状态位图
  bool __init;                // begin 成功标志（防重复初始化）
  struct bma4_dev __devFptr;  // Bosch 驱动的设备上下文（回调/参数都挂这里）
};
