#ifndef PRINT_SERVER_H
#define PRINT_SERVER_H

#include <Arduino.h>
#include <WiFi.h>
#include "usb_printer.h"
#include "config.h"

// 列印工作狀態
struct PrintJob {
  bool active;
  size_t bytesReceived;
  size_t bytesSent;
  unsigned long startTime;
  String clientIP;
};

class PrintServer {
public:
  PrintServer(UsbPrinter& printer);

  // 啟動 Raw Socket 伺服器（Port 9100）
  bool begin();

  // 主迴圈處理
  void task();

  // 取得目前列印工作資訊
  PrintJob getCurrentJob() const { return _currentJob; }
  uint32_t getTotalJobs() const { return _totalJobs; }
  uint32_t getTotalBytes() const { return _totalBytes; }

private:
  UsbPrinter& _printer;
  WiFiServer _server;
  WiFiClient _client;
  PrintJob _currentJob;
  bool _jobFailed;
  bool _hasLock;
  unsigned long _lastDataMs;

  uint32_t _totalJobs;
  uint32_t _totalBytes;

  // PSRAM 緩衝區
  uint8_t* _buffer;
  size_t _bufferSize;

  void _handleClient();
};

#endif // PRINT_SERVER_H
