#ifndef WEB_UI_H
#define WEB_UI_H

#include <Arduino.h>
#include <WebServer.h>
#include "wifi_manager.h"
#include "usb_printer.h"
#include "print_server.h"
#include "config.h"

class WebUI {
public:
  WebUI(WiFiManager& wifi, UsbPrinter& printer, PrintServer& printSvr);

  void begin();
  void task();

private:
  WebServer _server;
  WiFiManager& _wifi;
  UsbPrinter& _printer;
  PrintServer& _printSvr;

  // 路由處理
  void _handleRoot();
  void _handleStatus();
  void _handleWifiScan();
  void _handleWifiConnect();
  void _handleRestart();
  void _handleNotFound();

  // 生成 HTML
  String _generatePage();
};

#endif // WEB_UI_H
