#ifndef USB_PRINTER_H
#define USB_PRINTER_H

#include <Arduino.h>
#include "usb/usb_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "config.h"

// USB Printer Class 定義
#define USB_CLASS_PRINTER       0x07
#define USB_SUBCLASS_PRINTER    0x01
#define USB_PROTOCOL_UNIDIR    0x01  // 單向
#define USB_PROTOCOL_BIDIR     0x02  // 雙向
#define USB_PROTOCOL_1284      0x03  // IEEE 1284.4

// USB Printer Class 請求
#define USB_PRINTER_GET_DEVICE_ID   0x00
#define USB_PRINTER_GET_PORT_STATUS 0x01
#define USB_PRINTER_SOFT_RESET     0x02

// 印表機狀態
enum PrinterState {
  PRINTER_DISCONNECTED,
  PRINTER_CONNECTED,
  PRINTER_READY,
  PRINTER_BUSY,
  PRINTER_ERROR
};

class UsbPrinter {
public:
  UsbPrinter();

  // 建立 USB Host 背景任務（任務內部負責 install / 事件處理 / 失敗後自動重啟）
  bool begin();

  // 相容舊程式：事件處理現在由內部任務負責，這個函式什麼都不做
  void task() {}

  // 傳送資料到印表機（可由任何任務呼叫，執行緒安全）
  int sendData(const uint8_t* data, size_t length);

  PrinterState getState() const { return _state; }
  bool isConnected() const { return _state >= PRINTER_CONNECTED; }
  bool isReady() const { return _state == PRINTER_READY; }

  String getDeviceId() const { return _deviceId; }
  String getManufacturer() const { return _manufacturer; }
  String getProduct() const { return _product; }

  // 工作層級鎖：Raw 9100 與 IPP 兩個入口共用同一台印表機，
  // 一份工作從頭到尾必須獨佔，否則兩份工作的資料會交錯。
  bool acquireJob(uint32_t timeoutMs);
  void releaseJob();
  bool isJobLocked() const { return _jobLocked; }

  // 軟體重置（Printer Class SOFT_RESET）
  bool softReset();

private:
  volatile PrinterState _state;
  String _deviceId;
  String _manufacturer;
  String _product;

  // USB Host 相關
  TaskHandle_t _hostTask;
  SemaphoreHandle_t _ioMutex;
  SemaphoreHandle_t _jobMutex;
  volatile bool _jobLocked;
  usb_host_client_handle_t _clientHandle;
  usb_device_handle_t _deviceHandle;
  uint8_t _epOut;
  uint8_t _epIn;
  uint16_t _epOutMPS;
  uint8_t _ifaceNum;
  uint8_t _altSetting;
  bool _ifaceClaimed;

  // 由回呼設定、由 host 任務處理的事件旗標
  // 接在 Hub / Dock 後面時會連續出現多個 NEW_DEV，所以用小型佇列而不是單一變數
  static const uint8_t ADDR_QUEUE_SIZE = 8;
  uint8_t _addrQueue[ADDR_QUEUE_SIZE];
  volatile uint8_t _qHead;
  volatile uint8_t _qTail;
  volatile bool _pendingGone;
  volatile unsigned long _lastEventMs;

  static void _taskEntry(void* arg);
  void _hostTaskLoop();
  bool _installHost();
  bool _popPendingAddr(uint8_t* addr);
  void _teardownHost();
  void _pump(uint32_t ms);
  bool _waitDone(volatile bool* done, uint32_t timeoutMs);

  static void _clientEventCallback(const usb_host_client_event_msg_t* eventMsg, void* arg);

  bool _openDevice(uint8_t devAddr);
  bool _findPrinterInterface();
  static String _idField(const String& id, const char* key);
  bool _claimInterface();
  void _closeDevice();
  bool _getDeviceId();
  bool _control(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                uint8_t* inBuf, uint16_t wLength, size_t* gotBytes);
};

#endif // USB_PRINTER_H
