/*
 * 【中文导读】bma.cpp —— BMA423 三轴加速度计驱动封装
 * 计步/翻腕/双击检测 + 低功耗特性（双击中断唤醒手表）。
 * bma423.h/bma4.h 是 Bosch 官方驱动头（第三方库，不注释）。
 */

#include "bma.h"

#define DEBUGPORT Serial
#ifdef DEBUGPORT
#define DEBUG(...) DEBUGPORT.printf(__VA_ARGS__)
#else
#define DEBUG(...)
#endif

BMA423::BMA423() {
  __readRegisterFptr   = nullptr;
  __writeRegisterFptr  = nullptr;
  __delayCallBlackFptr = nullptr;
  __init               = false;
}

BMA423::~BMA423() {}

/**
 * @brief  初始化传感器：注入通信回调 → 软复位 → 载入特征引擎固件 → 配 INT1
 * @param  readCallBlack/writeCallBlack  I2C 字节读写回调（Watchy 的 static 成员）
 * @param  delayCallBlack                ms 延时回调（传 Arduino 的 delay）
 * @param  address                       I2C 地址，默认 0x18（SDO 接地）
 * @return false = 探测/固件载入/INT 配置任一步失败（错误码另查 getErrorCode）
 * @note   已初始化（__init）时直接返回 true——深睡唤醒后不必也不能重配。
 *         bma423_write_config_file 会经 I2C 灌入约 1KB 特征引擎固件，
 *         计步/翻腕/双击算法全在这个固件里跑，这就是"传感器自己计步"的底气。
 *         调用时机：仅 Watchy::_bmaConfig()（冷启动分支）。
 */
bool BMA423::begin(bma4_com_fptr_t readCallBlack,
                   bma4_com_fptr_t writeCallBlack,
                   bma4_delay_fptr_t delayCallBlack, uint8_t address) {

  /* 幂等保护：重复 begin 直接当成功；任一回调整除同样拒绝 */
  if (__init || readCallBlack == nullptr || writeCallBlack == nullptr ||
      delayCallBlack == nullptr) {
    return true;
  }

  __readRegisterFptr   = readCallBlack;
  __writeRegisterFptr  = writeCallBlack;
  __delayCallBlackFptr = delayCallBlack;

  __devFptr.dev_addr       = address;
  __devFptr.interface      = BMA4_I2C_INTERFACE;
  __devFptr.bus_read       = readCallBlack;
  __devFptr.bus_write      = writeCallBlack;
  __devFptr.delay          = delayCallBlack;
  __devFptr.read_write_len = 8;   // 单次 I2C 事务最大 8 字节（驱动分片依据）
  __devFptr.resolution     = 12;  // 12bit ADC 分辨率
  __devFptr.feature_len    = BMA423_FEATURE_SIZE;

  /* 复位后须等 20ms 再访问芯片（数据手册时序要求） */
  softReset();

  __delayCallBlackFptr(20);

  /* 读芯片 ID 验证在线 + 复位内部状态 */
  if (bma423_init(&__devFptr) != BMA4_OK) {
    DEBUG("BMA423 FAIL\n");
    return false;
  }

  /* 载入特征引擎固件：计步/翻腕/抬腕唤醒算法都在其中 */
  if (bma423_write_config_file(&__devFptr) != BMA4_OK) {
    DEBUG("BMA423 Write Config FAIL\n");
    return false;
  }

  __init = true;

  struct bma4_int_pin_config config;
  /* INT1 电气形态：电平触发/高有效/推挽/使能输出——与 ESP32 侧预期一致 */
  config.edge_ctrl = BMA4_LEVEL_TRIGGER;
  config.lvl       = BMA4_ACTIVE_HIGH;
  config.od        = BMA4_PUSH_PULL;
  config.output_en = BMA4_OUTPUT_ENABLE;
  config.input_en  = BMA4_INPUT_DISABLE;

  if (bma4_set_int_pin_config(&config, BMA4_INTR1_MAP, &__devFptr) != BMA4_OK) {
    DEBUG("BMA423 SET INT FAIL\n");
    return false;
  }
  return true;
}

/**
 * @brief  软复位：写 RESET 寄存器魔数，让芯片回到上电默认态
 * @note   begin() 内部第一步；单独调用后需要重新走一遍 begin 流程。
 */
void BMA423::softReset() {
  uint8_t reg = BMA4_RESET_ADDR;
  __writeRegisterFptr(BMA4_I2C_ADDR_PRIMARY, BMA4_RESET_SET_MASK, &reg, 1);
}

/**
 * @brief  关闭高级省电模式之外的传感器供电（彻底停测，功耗最低档）
 * @note   命名与语义相反源自 Bosch 驱动：advance_power_save=DISABLE 即停机。
 *         Watchy 日常不用它——特征引擎常开才能在深睡中持续计步。
 */
void BMA423::shutDown() {
  bma4_set_advance_power_save(BMA4_DISABLE, &__devFptr);
}

/**
 * @brief  从 shutDown 状态唤醒芯片（恢复 advance power save）
 */
void BMA423::wakeUp() { bma4_set_advance_power_save(BMA4_ENABLE, &__devFptr); }

/**
 * @brief  读错误状态寄存器（fatal_error/cmd_error/error_code 三段位域）
 */
uint16_t BMA423::getErrorCode() {
  struct bma4_err_reg err;
  uint16_t rslt = bma4_get_error_status(&err, &__devFptr);
  return rslt;
}

uint16_t BMA423::getStatus() {
  uint8_t status;
  bma4_get_status(&status, &__devFptr);
  return status;
}

uint32_t BMA423::getSensorTime() {
  uint32_t ms;
  bma4_get_sensor_time(&ms, &__devFptr);
  return ms;
}

bool BMA423::selfTest() {
  return (BMA4_OK ==
          bma4_selftest_config(BMA4_ACCEL_SELFTEST_ENABLE_MSK, &__devFptr));
}

/**
 * @brief  判朝向：读三轴 → 绝对值最大的轴定主方向，再按符号分两面
 * @return bma.h 的 DIRECTION_* 枚举（TOP/BOTTOM/LEFT/RIGHT/DISP_UP/DISP_DOWN）
 * @note   传感器贴片方向已经过 _bmaConfig 的轴重映射，这里的坐标系就是
 *         "表盘坐标系"。abs(acc.x) 用了 int 绝对值再存 uint16_t（-32768
 *         边界理论上会溢出，实际加速度到不了）。Show Accelerometer 页面
 *         与可旋转表盘都靠它。
 */
uint8_t BMA423::getDirection() {
  Accel acc;
  if (bma4_read_accel_xyz(&acc, &__devFptr) != BMA4_OK) {
    return 0;
  }
  uint16_t absX = abs(acc.x);
  uint16_t absY = abs(acc.y);
  uint16_t absZ = abs(acc.z);

  if ((absZ > absX) && (absZ > absY)) {
    if (acc.z > 0) {
      return DIRECTION_DISP_DOWN;
    } else {
      return DIRECTION_DISP_UP;
    }
  } else if ((absY > absX) && (absY > absZ)) {
    if (acc.y > 0) {
      return DIRECTION_RIGHT_EDGE;
    } else {
      return DIRECTION_LEFT_EDGE;
    }
  } else {
    if (acc.x < 0) {
      return DIRECTION_BOTTOM_EDGE;
    } else {
      return DIRECTION_TOP_EDGE;
    }
  }
}

/**
 * @brief  芯片温度（℃）：BMA423 内置温度传感器
 * @return 摄氏度；读不到有效值返回 0
 * @note   驱动返回定点数，除以 BMA4_SCALE_TEMP(=512) 得 ℃，即 1/512 ℃/LSB；
 *         公式里的 23 是数据手册给的基准偏置。寄存器读到 0x80 表示
 *         "无有效数据"，换算结果恰好为 0 —— 与真 0℃ 无法区分，
 *         固件约定 0 视为无效（无网退化天气时会显示 0，属可容忍误差）。
 */
float BMA423::readTemperature() {
  int32_t data = 0;
  bma4_get_temperature(&data, BMA4_DEG, &__devFptr);
  float res = (float)data / (float)BMA4_SCALE_TEMP;
  /* 0x80 - temp read from the register and 23 is the ambient temp added.
   * If the temp read from register is 0x80, it means no valid
   * information is available */
  if (((data - 23) / BMA4_SCALE_TEMP) == 0x80) {
    res = 0;
  }
  return res;
}

/**
 * @brief  华氏版 readTemperature()：℃×1.8+32；无效值 0 原样返回不换算
 */
float BMA423::readTemperatureF() {
  float temp = readTemperature();
  if (temp != 0) {
    temp = temp * 1.8 + 32.0;
  }
  return (temp);
}

/**
 * @brief  读一次三轴原始值（16bit 有符号，量程由 Acfg.range 决定：2G 时 1LSB≈0.98mg）
 * @param  acc  输出参数，调用前 memset 清零保证失败路径也安全
 * @return false = I2C 读失败（acc 保持全 0）
 * @note   Show Accelerometer 页面 200ms 一次；频率受 _bmaConfig 的 ODR 限制。
 */
bool BMA423::getAccel(Accel &acc) {
  memset(&acc, 0, sizeof(acc));
  if (bma4_read_accel_xyz(&acc, &__devFptr) != BMA4_OK) {
    return false;
  }
  return true;
}

/**
 * @brief  查询加速度计使能位（ACC_ENABLE 位域是否置 1）
 */
bool BMA423::getAccelEnable() {
  uint8_t en;
  bma4_get_accel_enable(&en, &__devFptr);
  return (en & BMA4_ACCEL_ENABLE_POS) == BMA4_ACCEL_ENABLE_POS;
}

bool BMA423::disableAccel() { return enableAccel(false); }

/**
 * @brief  加速度计使能开关。注意 Bosch 的约定：要用任何特性（计步/翻腕）
 *         必须先使能加速度计本身，否则特征引擎没有数据源
 */
bool BMA423::enableAccel(bool en) {
  return (BMA4_OK ==
          bma4_set_accel_enable(en ? BMA4_ENABLE : BMA4_DISABLE, &__devFptr));
}

/* ---- 以下 4 个是 Bosch 驱动调用的一对一薄封装：成功即 true ---- */

bool BMA423::setAccelConfig(Acfg &cfg) {
  return (BMA4_OK == bma4_set_accel_config(&cfg, &__devFptr));
}

bool BMA423::getAccelConfig(Acfg &cfg) {
  return (BMA4_OK == bma4_get_accel_config(&cfg, &__devFptr));
}

/**
 * @brief  轴重映射：把芯片物理轴映射到"表盘坐标系"（_bmaConfig 里 x/y 对调）
 */
bool BMA423::setRemapAxes(struct bma423_axes_remap *remap_data) {
  return (BMA4_OK == bma423_set_remap_axes(remap_data, &__devFptr));
}

/**
 * @brief  计步器清零（7_SEG 表盘每天 0 点调，见 examples）
 */
bool BMA423::resetStepCounter() {
  return BMA4_OK == bma423_reset_step_counter(&__devFptr);
}

/**
 * @brief  读步数：芯片特征引擎自主累计，深睡期间也在走（芯片不断电）
 * @return 步数；I2C 失败返回 0（调用方无法区分"真 0 步"与"读失败"）
 */
uint32_t BMA423::getCounter() {
  uint32_t stepCount;
  if (bma423_step_counter_output(&stepCount, &__devFptr) == BMA4_OK) {
    return stepCount;
  }
  return 0;
}

bool BMA423::setINTPinConfig(struct bma4_int_pin_config config,
                             uint8_t pinMap) {
  return BMA4_OK == bma4_set_int_pin_config(&config, pinMap, &__devFptr);
}

/**
 * @brief  读中断状态：芯片 INT 状态寄存器（读后自动清零）快照进 __IRQ_MASK
 * @note   "读后清"语义——同一中断只能消费一次；之后用 is*() 系列查询。
 *         这是 Watchy 的中断消费模型：醒来后调一次 getINT()，再逐位判断
 *         是计步/翻腕/双击谁触发的。
 */
bool BMA423::getINT() {
  return bma423_read_int_status(&__IRQ_MASK, &__devFptr) == BMA4_OK;
}

uint8_t BMA423::getIRQMASK() { return __IRQ_MASK; }

/**
 * @brief  把 int_map 指定的中断从 INT1 引脚解除映射（不再拉引脚）
 * @param  int_map  位掩码，默认 BMA423_STEP_CNTR_INT
 */
bool BMA423::disableIRQ(uint16_t int_map) {
  return (BMA4_OK == bma423_map_interrupt(BMA4_INTR1_MAP, int_map, BMA4_DISABLE,
                                          &__devFptr));
}

/**
 * @brief  把 int_map 指定的中断映射到 INT1 引脚（触发时拉高）
 */
bool BMA423::enableIRQ(uint16_t int_map) {
  return (BMA4_OK == bma423_map_interrupt(BMA4_INTR1_MAP, int_map, BMA4_ENABLE,
                                          &__devFptr));
}

/**
 * @brief  开关芯片内部特征引擎的某项特性（位掩码 BMA423_STEP_CNTR 等）
 * @note   注意与 enable*Interrupt 的分工：本函数让"算法"开始跑并置状态位，
 *         enable*Interrupt 只决定"要不要把中断导到 INT1 引脚"。
 *         STEP_CNTR 还额外要先开 step_detector（本函数已处理）。
 */
bool BMA423::enableFeature(uint8_t feature, uint8_t enable) {
  if ((feature & BMA423_STEP_CNTR) == BMA423_STEP_CNTR) {
    bma423_step_detector_enable(enable ? BMA4_ENABLE : BMA4_DISABLE,
                                &__devFptr);
  }
  return (BMA4_OK == bma423_feature_enable(feature, enable, &__devFptr));
}

/* ---- is*() 系列：对最近一次 getINT() 快照的位测试，不含 I2C 访问 ---- */

bool BMA423::isStepCounter() {
  return (bool)(BMA423_STEP_CNTR_INT & __IRQ_MASK);
}

/* 双击 = WAKEUP 特性中断（"抬腕唤醒"算法，双击是它的典型触发） */
bool BMA423::isDoubleClick() { return (bool)(BMA423_WAKEUP_INT & __IRQ_MASK); }

bool BMA423::isTilt() { return (bool)(BMA423_TILT_INT & __IRQ_MASK); }

bool BMA423::isActivity() { return (bool)(BMA423_ACTIVITY_INT & __IRQ_MASK); }

bool BMA423::isAnyNoMotion() {
  return (bool)(BMA423_ANY_NO_MOTION_INT & __IRQ_MASK);
}

/* ---- enable*Interrupt 系列：中断→INT1 引脚映射的开关（薄封装） ---- */

bool BMA423::enableStepCountInterrupt(bool en) {
  return (BMA4_OK == bma423_map_interrupt(BMA4_INTR1_MAP, BMA423_STEP_CNTR_INT,
                                          en, &__devFptr));
}

bool BMA423::enableTiltInterrupt(bool en) {
  return (BMA4_OK == bma423_map_interrupt(BMA4_INTR1_MAP, BMA423_TILT_INT, en,
                                          &__devFptr));
}

bool BMA423::enableWakeupInterrupt(bool en) {
  return (BMA4_OK == bma423_map_interrupt(BMA4_INTR1_MAP, BMA423_WAKEUP_INT, en,
                                          &__devFptr));
}

bool BMA423::enableAnyNoMotionInterrupt(bool en) {
  return (BMA4_OK == bma423_map_interrupt(BMA4_INTR1_MAP,
                                          BMA423_ANY_NO_MOTION_INT, en,
                                          &__devFptr));
}

bool BMA423::enableActivityInterrupt(bool en) {
  return (BMA4_OK == bma423_map_interrupt(BMA4_INTR1_MAP, BMA423_ACTIVITY_INT,
                                          en, &__devFptr));
}

/**
 * @brief  查询活动状态字符串（静止/步行/跑步，由 ACTIVITY 特性引擎输出）
 * @return "BMA423_USER_STATIONARY" 等常量串；未识别返回 "None"
 * @note   需先 enableFeature(BMA423_ACTIVITY, 1)，Watchy 默认未启用。
 */
const char *BMA423::getActivity() {
  uint8_t activity;
  bma423_activity_output(&activity, &__devFptr);
  if (activity & BMA423_USER_STATIONARY) {
    return "BMA423_USER_STATIONARY";
  } else if (activity & BMA423_USER_WALKING) {
    return "BMA423_USER_WALKING";
  } else if (activity & BMA423_USER_RUNNING) {
    return "BMA423_USER_RUNNING";
  } else if (activity & BMA423_STATE_INVALID) {
    return "BMA423_STATE_INVALID";
  }
  return "None";
}