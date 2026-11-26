#ifndef IPP_SERVER_H
#define IPP_SERVER_H

#include <Arduino.h>
#include <WiFi.h>
#include "usb_printer.h"
#include "ipp_session.h"

// IPP 伺服器（Android 預設列印服務 / Mopria / Windows IPP Class Driver 用）
// 自己一個 FreeRTOS 任務，不會卡住 loop()。文件（PDF）原封不動轉送到 USB 印表機。
class IppServer {
public:
  explicit IppServer(UsbPrinter& printer);
  bool begin();
  uint32_t jobsHandled() const { return _state.nextJobId - 1; }

private:
  UsbPrinter& _printer;
  WiFiServer _server;
  TaskHandle_t _task;
  ipp::State _state;

  static void _entry(void* arg);
  void _loop();
};

#endif
