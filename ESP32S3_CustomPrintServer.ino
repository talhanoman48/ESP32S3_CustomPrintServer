// ============================================================
//  ESP32-S3 WiFi Print Server
//  讓 Brother HL-1110 透過 WiFi 從 Mac 列印
//
//  硬體：ESP32-S3-N16R8（16MB Flash / 8MB PSRAM）
//  原理：ESP32-S3 USB Host → 印表機
//        WiFi Raw Socket (Port 9100) ← Mac
//        mDNS 自動發現 + Web 管理介面 + OTA
//
//  作者：MassTang
//  日期：2026-05
// ============================================================

#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "usb_printer.h"
#include "wifi_manager.h"
#include "print_server.h"
#include "ipp_server.h"
#include "web_ui.h"

// --- 全域物件 ---
UsbPrinter   usbPrinter;
WiFiManager  wifiMgr;
PrintServer  printSvr(usbPrinter);
IppServer    ippSvr(usbPrinter);
WebUI        webUI(wifiMgr, usbPrinter, printSvr);

// --- 設定 mDNS ---
void setupMDNS() {
  if (MDNS.begin(MDNS_HOSTNAME)) {
    // 廣播 Raw Socket 列印服務
    MDNS.addService("pdl-datastream", "tcp", RAW_SOCKET_PORT);

    // 廣播 HTTP 管理介面
    MDNS.addService("http", "tcp", WEB_SERVER_PORT);

    // 廣播 IPP 列印服務：Android 預設列印服務與 Windows「新增印表機」靠這個自動發現
    MDNS.setInstanceName(PRINTER_NAME);
    MDNS.addService("ipp", "tcp", IPP_PORT);
    MDNS.addServiceTxt("ipp", "tcp", "txtvers", "1");
    MDNS.addServiceTxt("ipp", "tcp", "qtotal", "1");
    MDNS.addServiceTxt("ipp", "tcp", "rp", "ipp/print");
    MDNS.addServiceTxt("ipp", "tcp", "ty", PRINTER_MODEL_NAME);
    MDNS.addServiceTxt("ipp", "tcp", "product", "(" PRINTER_MODEL_NAME ")");
    MDNS.addServiceTxt("ipp", "tcp", "note", "");
    MDNS.addServiceTxt("ipp", "tcp", "pdl", "application/pdf");
    MDNS.addServiceTxt("ipp", "tcp", "kind", "document");
    MDNS.addServiceTxt("ipp", "tcp", "PaperMax", "legal-A4");
    MDNS.addServiceTxt("ipp", "tcp", "Color", "F");
    MDNS.addServiceTxt("ipp", "tcp", "Duplex", "F");
    MDNS.addServiceTxt("ipp", "tcp", "Copies", "F");
    MDNS.addServiceTxt("ipp", "tcp", "Scan", "F");
    MDNS.addServiceTxt("ipp", "tcp", "Fax", "F");
    MDNS.addServiceTxt("ipp", "tcp", "priority", "0");
    MDNS.addServiceTxt("ipp", "tcp", "adminurl", "http://" MDNS_HOSTNAME ".local/");

    Serial.printf("[mDNS] ✓ 已註冊 %s.local\n", MDNS_HOSTNAME);
    Serial.printf("[mDNS]   列印: socket://%s.local:%d\n", MDNS_HOSTNAME, RAW_SOCKET_PORT);
    Serial.printf("[mDNS]   IPP:  ipp://%s.local:%d%s\n", MDNS_HOSTNAME, IPP_PORT, IPP_PATH);
    Serial.printf("[mDNS]   管理: http://%s.local\n", MDNS_HOSTNAME);
  } else {
    Serial.println("[mDNS] ✗ 啟動失敗");
  }
}

// --- 設定 OTA ---
void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);

  if (strlen(OTA_PASSWORD) > 0) {
    ArduinoOTA.setPassword(OTA_PASSWORD);
  }

  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH)
                  ? "sketch" : "filesystem";
    Serial.printf("[OTA] 開始更新 %s\n", type.c_str());
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\n[OTA] 更新完成！");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("[OTA] 進度: %u%%\r", (progress / (total / 100)));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] 錯誤 [%u]: ", error);
    switch (error) {
      case OTA_AUTH_ERROR:    Serial.println("認證失敗"); break;
      case OTA_BEGIN_ERROR:   Serial.println("開始失敗"); break;
      case OTA_CONNECT_ERROR: Serial.println("連線失敗"); break;
      case OTA_RECEIVE_ERROR: Serial.println("接收失敗"); break;
      case OTA_END_ERROR:     Serial.println("結束失敗"); break;
    }
  });

  ArduinoOTA.begin();
  Serial.println("[OTA] ✓ OTA 更新已啟用");
}

// --- LED 狀態指示 ---
void setLED(int mode) {
  switch (mode) {
    case LED_WIFI_OK:
      digitalWrite(LED_PIN, HIGH);
      break;
    case LED_PRINTING:
      // 閃爍效果在 loop 中處理
      break;
    case LED_ERROR:
      digitalWrite(LED_PIN, LOW);
      break;
  }
}

// ============================================================
//  setup()
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("╔══════════════════════════════════════╗");
  Serial.println("║   ESP32-S3 WiFi Print Server v1.0   ║");
  Serial.println("║   Brother HL-1110                    ║");
  Serial.println("╚══════════════════════════════════════╝");
  Serial.println();

  // LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // 顯示記憶體資訊
  Serial.printf("[SYS] Free Heap:  %d KB\n", ESP.getFreeHeap() / 1024);
  Serial.printf("[SYS] Free PSRAM: %d KB\n", ESP.getFreePsram() / 1024);
  Serial.printf("[SYS] Flash Size: %d MB\n", ESP.getFlashChipSize() / (1024 * 1024));
  Serial.println();

  // 1. 初始化 WiFi
  Serial.println("=== 初始化 WiFi ===");
  wifiMgr.begin();
  Serial.println();

  // 2. 如果 WiFi 已連線，啟動網路服務
  if (wifiMgr.isConnected()) {
    setupMDNS();
    setupOTA();
  }

  // 3. 啟動列印伺服器
  Serial.println("=== 初始化列印伺服器 ===");
  printSvr.begin();
  Serial.println();

  // 4. 啟動 Web 管理介面
  Serial.println("=== 初始化 Web 管理介面 ===");
  webUI.begin();
  Serial.println();

  // 4b. 啟動 IPP 伺服器（Android / Windows 自動發現）
  Serial.println("=== 初始化 IPP 伺服器 ===");
  ippSvr.begin();
  Serial.println();

  // 5. 啟動 USB Host（刻意放在 WiFi 之後）
  //    WiFi 射頻校準/連線時電流會突然拉高，若剛好撞上 USB root port reset，
  //    3.3V 一掉就會得到 "HUB: Root port reset failed"。
  //    USB Host 任務內部還會再等 USB_HOST_START_DELAY_MS 讓電源穩定。
  Serial.println("=== 初始化 USB Host ===");
  if (!usbPrinter.begin()) {
    Serial.println("[USB] ✗ USB Host 任務建立失敗");
  }
  Serial.println();

  // 6. 啟動看門狗
  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = WDT_TIMEOUT_SEC * 1000,
    .idle_core_mask = (1 << 0),  // 監控核心 0
    .trigger_panic = true,
  };
  esp_task_wdt_init(&wdtConfig);
  esp_task_wdt_add(NULL);

  // 完成
  Serial.println("════════════════════════════════════════");
  Serial.println("  系統就緒！");
  if (wifiMgr.isConnected()) {
    Serial.printf("  列印位址: socket://%s:%d\n", wifiMgr.getIP().c_str(), RAW_SOCKET_PORT);
    Serial.printf("  管理介面: http://%s.local\n", MDNS_HOSTNAME);
  } else {
    Serial.printf("  請連線 WiFi '%s' 進行設定\n", AP_SSID);
    Serial.printf("  設定頁面: http://%s\n", wifiMgr.getIP().c_str());
  }
  Serial.println("════════════════════════════════════════");

  setLED(wifiMgr.isConnected() ? LED_WIFI_OK : LED_ERROR);
}

// ============================================================
//  loop()
// ============================================================
void loop() {
  // 餵看門狗
  esp_task_wdt_reset();

  // WiFi 管理（自動重連）
  wifiMgr.task();

  // 列印伺服器
  printSvr.task();

  // Web 管理介面
  webUI.task();

  // OTA 更新檢查
  if (wifiMgr.isConnected()) {
    ArduinoOTA.handle();
  }

  // LED 閃爍（列印中）
  static unsigned long lastBlink = 0;
  if (printSvr.getCurrentJob().active) {
    if (millis() - lastBlink > 200) {
      lastBlink = millis();
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    }
  }

  // 短暫延遲，避免 CPU 全速空轉
  delay(1);
}
