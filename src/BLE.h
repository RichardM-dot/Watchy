/*
 * 【中文导读】BLE.h —— BLE 类声明（B级导读）
 */

/*
 * 【B级导读·扩展】BLE.h —— BLE OTA 蓝牙固件升级服务端声明
 * ----------------------------------------------------------------------------
 * 职责：把 ESP32 变成 BLE GATT 服务器，配套手机 App 可经蓝牙给手表
 *       推送新固件（OTA），也可读硬件/软件版本号与当前表盘名。
 * 主要接口（实现见 BLE.cpp）：
 *   begin(localName)  建 Server/Service/Characteristic 并开始广播
 *   updateStatus()    查 OTA 状态机（0 连接 /1 传输中 /2 完成 /4 断开）
 *   howManyBytes()    查已接收字节数（进度条用）
 * 数据流位置：手机 App → BLE(Notify/Write 特征) → otaCallback::onWrite
 *   → esp_ota_write → OTA 分区 → 重启换区。默认固件未启用此路径
 *   （Watchy.cpp 的 updateFWBegin 已整段注释），类保留备用。
 */

#ifndef _BLE_H_
#define _BLE_H_

#include "Arduino.h"

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#include "esp_ota_ops.h"

#include "config.h"

class BLE;

class BLE {
public:
  BLE(void);
  ~BLE(void);

  bool begin(const char *localName);
  int updateStatus();
  int howManyBytes();

private:
  String local_name;

  BLEServer *pServer = NULL;

  BLEService *pESPOTAService                 = NULL;
  BLECharacteristic *pESPOTAIdCharacteristic = NULL;

  BLEService *pService                            = NULL;
  BLECharacteristic *pVersionCharacteristic       = NULL;
  BLECharacteristic *pOtaCharacteristic           = NULL;
  BLECharacteristic *pWatchFaceNameCharacteristic = NULL;
};

#endif
