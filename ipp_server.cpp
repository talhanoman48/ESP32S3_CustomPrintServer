#include "ipp_server.h"
#include "config.h"
#include <esp_task_wdt.h>

// ------------------------------------------------------------
//  把 ipp::Backend 接到真正的 USB 印表機
// ------------------------------------------------------------
class EspBackend : public ipp::Backend {
public:
  explicit EspBackend(UsbPrinter& p) : _p(p) {}

  uint32_t nowMs() override { return millis(); }
  void pump() override { esp_task_wdt_reset(); vTaskDelay(1); }

  std::string hostIp() override {
    IPAddress ip = WiFi.localIP();
    if (ip == IPAddress((uint32_t)0)) ip = WiFi.softAPIP();
    return std::string(ip.toString().c_str());
  }
  std::string printerName() override { return PRINTER_NAME; }

  std::string makeModel() override {
    String mfg = _p.getManufacturer(), prod = _p.getProduct();
    if (prod.length() == 0) return PRINTER_MODEL_NAME;
    if (mfg.length() == 0 || prod.startsWith(mfg)) return std::string(prod.c_str());
    return std::string((mfg + " " + prod).c_str());
  }
  std::string deviceId() override { return std::string(_p.getDeviceId().c_str()); }

  std::string uuid() override {
    // urn:uuid:00000000-0000-1000-8000-<MAC>
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    mac.toLowerCase();
    return std::string("urn:uuid:00000000-0000-1000-8000-") + mac.c_str();
  }

  int printerState() override {
    if (!_p.isConnected()) return 5;                   // stopped
    if (_p.isJobLocked() || _p.getState() == PRINTER_BUSY) return 4;  // processing
    return 3;                                          // idle
  }

  ipp::JobBeginResult jobBegin(uint32_t waitMs) override {
    if (!_p.acquireJob(waitMs)) return ipp::JB_BUSY;
    if (!_p.isReady()) { _p.releaseJob(); return ipp::JB_OFFLINE; }
#if IPP_UEL_FRAMING
    String prefix = "\x1B%-12345X";
    if (strlen(IPP_PJL_LANGUAGE) > 0) {
      prefix += "@PJL ENTER LANGUAGE = ";
      prefix += IPP_PJL_LANGUAGE;
      prefix += "\r\n";
    }
    if (_p.sendData((const uint8_t*)prefix.c_str(), prefix.length()) != (int)prefix.length()) {
      _p.releaseJob();
      return ipp::JB_OFFLINE;
    }
#endif
    return ipp::JB_OK;
  }

  bool jobWrite(const uint8_t* data, size_t len) override {
    return _p.sendData(data, len) == (int)len;
  }

  void jobEnd(bool ok) override {
    if (ok) {
#if IPP_UEL_FRAMING
      const char* uel = "\x1B%-12345X";
      _p.sendData((const uint8_t*)uel, strlen(uel));
#endif
    } else {
      _p.softReset();   // 收到不完整的資料：清掉印表機裡的半份工作
    }
    _p.releaseJob();
  }

private:
  UsbPrinter& _p;
};

// ------------------------------------------------------------
IppServer::IppServer(UsbPrinter& printer)
  : _printer(printer), _server(IPP_PORT), _task(nullptr) {}

bool IppServer::begin() {
  if (_task) return true;
  _server.begin();
  _server.setNoDelay(true);
  BaseType_t ok = xTaskCreate(_entry, "ipp_server", IPP_TASK_STACK_SIZE, this, IPP_TASK_PRIORITY, &_task);
  if (ok != pdPASS) {
    Serial.println("[IPP] ✗ 無法建立 IPP 任務");
    _task = nullptr;
    return false;
  }
  Serial.printf("[IPP] ✓ 已啟動，ipp://<ip>:%d%s\n", IPP_PORT, IPP_PATH);
  return true;
}

void IppServer::_entry(void* arg) {
  ((IppServer*)arg)->_loop();
  vTaskDelete(nullptr);
}

void IppServer::_loop() {
  EspBackend backend(_printer);
  for (;;) {
    if (_server.hasClient()) {
      WiFiClient client = _server.accept();
      if (client) {
        client.setNoDelay(true);
        {
          ipp::Session<WiFiClient> session(client, backend, _state);
          session.run();
        }
        client.stop();
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}
