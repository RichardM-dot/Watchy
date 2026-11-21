/*
 * 【中文导读】BLE.cpp —— 蓝牙低功耗服务（B级导读）
 * ESP32 BLE 服务端，手机 App 经 BLE 与手表通信（对时/控制）。
 */

/*
 * 【B级导读·扩展】BLE.cpp —— BLE OTA 固件升级实现
 * ----------------------------------------------------------------------------
 * GATT 布局：
 *   SERVICE_UUID_ESPOTA（cd77498e-…）+ ID 只读特征：OTA 服务的"门牌"
 *   SERVICE_UUID_OTA（86b12865-…）下挂 3 特征：
 *     HW_VERSION 只读     5 字节 {HW_major,HW_minor,SW_major,SW_minor,SW_patch}
 *     WATCHFACE_NAME 只读 当前表盘名
 *     FW 可写+Notify      固件数据通道（每包 512 字节，收满前一直写 OTA 分区）
 * OTA 状态机：status 全局变量 0/1/2/4（连接/传输/完成/断开），
 *   updateFlag 标记"本包之后 esp_ota_begin 已调"；最后一包（长度≠512）
 *   触发 esp_ota_end + set_boot_partition → status=READY → 主调方重启换区。
 * 数据流位置：BLE 栈回调（BT 上下文）→ esp_ota_write → flash OTA 分区；
 *   默认固件未接入（updateFWBegin 注释停用），Q: 每包回写 5 字节 Notify
 *   作为 App 侧 ACK。
 */

#include "BLE.h"

#define SERVICE_UUID_ESPOTA    "cd77498e-1ac8-48b6-aba8-4161c7342fce"
#define CHARACTERISTIC_UUID_ID "cd77498f-1ac8-48b6-aba8-4161c7342fce"

#define SERVICE_UUID_OTA               "86b12865-4b70-4893-8ce6-9864fc00374d"
#define CHARACTERISTIC_UUID_FW         "86b12866-4b70-4893-8ce6-9864fc00374d"
#define CHARACTERISTIC_UUID_HW_VERSION "86b12867-4b70-4893-8ce6-9864fc00374d"
#define CHARACTERISTIC_UUID_WATCHFACE_NAME                                     \
  "86b12868-4b70-4893-8ce6-9864fc00374d"

#define FULL_PACKET         512
#define CHARPOS_UPDATE_FLAG 5

#define STATUS_CONNECTED    0
#define STATUS_DISCONNECTED 4
#define STATUS_UPDATING     1
#define STATUS_READY        2

esp_ota_handle_t otaHandler = 0;

int status        = -1;
int bytesReceived = 0;
bool updateFlag   = false;

class BLECustomServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) { status = STATUS_CONNECTED; };

  void onDisconnect(BLEServer *pServer) { status = STATUS_DISCONNECTED; }
};

class otaCallback : public BLECharacteristicCallbacks {
public:
  otaCallback(BLE *ble) { _p_ble = ble; }
  BLE *_p_ble;

  void onWrite(BLECharacteristic *pCharacteristic);
};

void otaCallback::onWrite(BLECharacteristic *pCharacteristic) {
  auto rxData = pCharacteristic->getValue();
  if (!updateFlag) { // If it's the first packet of OTA since bootup, begin OTA
    // Serial.println("Begin FW Update");
    esp_ota_begin(esp_ota_get_next_update_partition(NULL), OTA_SIZE_UNKNOWN,
                  &otaHandler);
    updateFlag = true;
    status     = STATUS_UPDATING;
  }
  if (_p_ble != NULL) {
    if (rxData.length() > 0) {
      esp_ota_write(otaHandler, rxData.c_str(), rxData.length());
      bytesReceived = bytesReceived + rxData.length();
      if (rxData.length() != FULL_PACKET) {
        esp_ota_end(otaHandler);
        // Serial.println("End FW Update");
        if (ESP_OK == esp_ota_set_boot_partition(
                          esp_ota_get_next_update_partition(NULL))) {
          status = STATUS_READY;
        } else {
          // Serial.println("Upload Error");
        }
      }
    }
  }

  uint8_t txData[5] = {1, 2, 3, 4, 5};
  // delay(1000);
  pCharacteristic->setValue((uint8_t *)txData, 5);
  pCharacteristic->notify();
}

//
// Constructor
BLE::BLE(void) {}

//
// Destructor
BLE::~BLE(void) {}

//
// begin
bool BLE::begin(const char *localName = "Watchy BLE OTA") {
  // Create the BLE Device
  BLEDevice::init(localName);

  // Create the BLE Server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new BLECustomServerCallbacks());

  // Create the BLE Service
  pESPOTAService = pServer->createService(SERVICE_UUID_ESPOTA);
  pService       = pServer->createService(SERVICE_UUID_OTA);

  // Create a BLE Characteristic
  pESPOTAIdCharacteristic = pESPOTAService->createCharacteristic(
      CHARACTERISTIC_UUID_ID, BLECharacteristic::PROPERTY_READ);

  pVersionCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID_HW_VERSION, BLECharacteristic::PROPERTY_READ);

  pWatchFaceNameCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID_WATCHFACE_NAME, BLECharacteristic::PROPERTY_READ);

  pOtaCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID_FW,
      BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_WRITE);

  pOtaCharacteristic->addDescriptor(new BLE2902());
  pOtaCharacteristic->setCallbacks(new otaCallback(this));

  // Start the service(s)
  pESPOTAService->start();
  pService->start();

  // Start advertising
  pServer->getAdvertising()->addServiceUUID(SERVICE_UUID_ESPOTA);
  pServer->getAdvertising()->start();

  uint8_t hardwareVersion[5] = {HARDWARE_VERSION_MAJOR, HARDWARE_VERSION_MINOR,
                                SOFTWARE_VERSION_MAJOR, SOFTWARE_VERSION_MINOR,
                                SOFTWARE_VERSION_PATCH};
  pVersionCharacteristic->setValue((uint8_t *)hardwareVersion, 5);
  pWatchFaceNameCharacteristic->setValue("Watchy 7 Segment");

  return true;
}

int BLE::updateStatus() { return status; }

int BLE::howManyBytes() { return bytesReceived; }
