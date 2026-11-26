#include "usb_printer.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "esp_task_wdt.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

// ============================================================
//  編譯期檢查：直接接線到 GPIO19/20 時，USB 周邊必須是 OTG 模式
// ============================================================
#if defined(ARDUINO_USB_MODE) && (ARDUINO_USB_MODE == 1)
#warning "Tools -> USB Mode 目前是 Hardware CDC and JTAG。請改成 USB-OTG (TinyUSB)，否則 D+ 上拉會跟印表機打架，造成 Root port reset failed。"
#endif
#if defined(ARDUINO_USB_CDC_ON_BOOT) && (ARDUINO_USB_CDC_ON_BOOT == 1)
#warning "Tools -> USB CDC On Boot 請設為 Disabled：Serial 要走 COM/UART 口，GPIO19/20 要留給印表機。"
#endif

// ============================================================
//  "HUB: Root port reset failed" 偵測
//  這個錯誤只會被 ESP-IDF 印到 log，不會回報給應用程式，
//  之後整個 USB 堆疊就停在那裡，直到重新插拔。
//  這裡掛一個 vprintf hook 去偵測它，然後由 host 任務重啟 USB Host。
// ============================================================
static vprintf_like_t   s_origVprintf = nullptr;
static volatile bool    s_rootPortFailed = false;

static int usbLogHook(const char* fmt, va_list args) {
  if (fmt && strstr(fmt, "Root port reset failed")) {
    s_rootPortFailed = true;
  }
  return s_origVprintf ? s_origVprintf(fmt, args) : vprintf(fmt, args);
}

// ============================================================
//  Enumeration filter callback
//  Arduino-ESP32 3.3.x / ESP-IDF 5.5.x 預設開啟
//  CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK。此選項開啟時，
//  enum_filter_cb 若為 nullptr 就不會完成列舉，印表機（或 Dock）永遠偵測不到。
//  回傳 true = 繼續列舉；*bConfigurationValue 指定要啟用的 configuration。
//  （感謝 HL-L2300D 使用者回報）
//  注意：此回呼在 USB Host Library 內部執行，不要在裡面 Serial.print 或阻塞。
// ============================================================
#ifdef CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
static bool usbEnumFilter(const usb_device_desc_t* dev_desc, uint8_t* bConfigurationValue) {
  (void)dev_desc;
  *bConfigurationValue = 1;  // 印表機與 Hub/Dock 都使用 configuration 1
  return true;               // 接受所有裝置（是否為印表機稍後在 _openDevice 判斷）
}
#endif

// 單次傳輸的完成旗標；放在 heap，避免逾時後回呼寫到已經消失的 stack
struct XferCtx {
  volatile bool done;
};

UsbPrinter::UsbPrinter()
  : _state(PRINTER_DISCONNECTED),
    _hostTask(nullptr), _ioMutex(nullptr), _jobMutex(nullptr), _jobLocked(false),
    _clientHandle(nullptr), _deviceHandle(nullptr),
    _epOut(0), _epIn(0), _epOutMPS(64), _ifaceNum(0), _altSetting(0), _ifaceClaimed(false),
    _qHead(0), _qTail(0), _pendingGone(false), _lastEventMs(0) {
  memset(_addrQueue, 0, sizeof(_addrQueue));
}

// ============================================================
//  啟動
// ============================================================
bool UsbPrinter::begin() {
  if (_hostTask != nullptr) return true;

  _ioMutex = xSemaphoreCreateMutex();
  if (_ioMutex == nullptr) {
    Serial.println("[USB] ✗ 無法建立 mutex");
    return false;
  }

  _jobMutex = xSemaphoreCreateMutex();
  if (_jobMutex == nullptr) {
    Serial.println("[USB] ✗ 無法建立 job mutex");
    return false;
  }

  if (s_origVprintf == nullptr) {
    s_origVprintf = esp_log_set_vprintf(usbLogHook);
  }

  BaseType_t ok = xTaskCreatePinnedToCore(
    _taskEntry, "usb_host", USB_HOST_TASK_STACK_SIZE, this,
    USB_HOST_TASK_PRIORITY, &_hostTask, USB_HOST_TASK_CORE);

  if (ok != pdPASS) {
    Serial.println("[USB] ✗ 無法建立 USB Host 任務");
    _hostTask = nullptr;
    return false;
  }
  return true;
}

void UsbPrinter::_taskEntry(void* arg) {
  ((UsbPrinter*)arg)->_hostTaskLoop();
  vTaskDelete(nullptr);
}

// ============================================================
//  Host 任務：唯一呼叫 usb_host_*_handle_events() 的地方
// ============================================================
void UsbPrinter::_hostTaskLoop() {
  Serial.println("[USB] Host 任務已啟動");
  vTaskDelay(pdMS_TO_TICKS(USB_HOST_START_DELAY_MS));

  uint32_t failCount = 0;

  while (true) {
    if (!_installHost()) {
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }

    s_rootPortFailed = false;
    _lastEventMs = millis();
    bool restart = false;
    bool isFailure = false;

    while (!restart) {
      _pump(5);

      if (_pendingGone) {
        _pendingGone = false;
        _closeDevice();
      }

      // 逐一處理所有排隊的新裝置（Dock/Hub 會一次冒出好幾個）
      uint8_t addr;
      while (_state == PRINTER_DISCONNECTED && _popPendingAddr(&addr)) {
        if (_openDevice(addr)) failCount = 0;
      }

      if (s_rootPortFailed) {
        restart = true;
        isFailure = true;
      } else if (USB_RESCAN_INTERVAL_MS > 0 && _state == PRINTER_DISCONNECTED &&
                 (millis() - _lastEventMs) > (unsigned long)USB_RESCAN_INTERVAL_MS) {
        // 一段時間沒有任何事件且沒有裝置：重新 install，讓 root port 重新偵測連接
        usb_host_lib_info_t info;
        if (usb_host_lib_info(&info) == ESP_OK && info.num_devices == 0) {
          restart = true;
        }
        _lastEventMs = millis();
      }
    }

    if (isFailure) {
      failCount++;
      Serial.printf("[USB] ⚠ HUB: Root port reset failed（第 %u 次），重啟 USB Host...\n",
                    (unsigned)failCount);
      if (failCount >= 3) {
        Serial.println("[USB] ⚠ 連續失敗：直接接線時幾乎都是接線/供電問題，請檢查：");
        Serial.printf("[USB]    1) 綠線 D+ → GPIO%d，白線 D- → GPIO%d（接反會一直失敗）\n", USB_DP_PIN, USB_DM_PIN);
        Serial.println("[USB]    2) 紅線 VBUS → 板子 5V 腳；用三用電表量印表機端 VBUS 應為 ~5V");
        Serial.println("[USB]    3) 黑線 GND 一定要接；線越短越好（<15cm、D+/D- 絞在一起）");
        Serial.println("[USB]    4) 原生 USB 口要空接；板子由 COM 口供電；VBUS/GND 間加 100~470uF");
        Serial.println("[USB]    5) Tools -> USB Mode 必須是 USB-OTG (TinyUSB)");
      }
    }

    _teardownHost();

    // 退避：1s, 2s, 4s, 8s, 上限 10s（非失敗情況固定 1s）
    uint32_t backoff = 1000;
    if (isFailure) {
      uint32_t shift = (failCount > 4) ? 4 : failCount;
      backoff = 1000u << shift;
      if (backoff > 10000) backoff = 10000;
    }
    vTaskDelay(pdMS_TO_TICKS(backoff));
  }
}

bool UsbPrinter::_installHost() {
  usb_host_config_t hostConfig = {};
  hostConfig.skip_phy_setup = false;
  hostConfig.intr_flags = ESP_INTR_FLAG_LEVEL1;
#ifdef CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
  hostConfig.enum_filter_cb = usbEnumFilter;
#endif

  esp_err_t err = usb_host_install(&hostConfig);
  if (err != ESP_OK) {
    Serial.printf("[USB] Host install failed: %s\n", esp_err_to_name(err));
    return false;
  }

  usb_host_client_config_t clientConfig = {};
  clientConfig.is_synchronous = false;
  clientConfig.max_num_event_msg = 5;
  clientConfig.async.client_event_callback = _clientEventCallback;
  clientConfig.async.callback_arg = this;

  err = usb_host_client_register(&clientConfig, &_clientHandle);
  if (err != ESP_OK) {
    Serial.printf("[USB] Client register failed: %s\n", esp_err_to_name(err));
    _clientHandle = nullptr;
    usb_host_uninstall();
    return false;
  }

  Serial.printf("[USB] Host 初始化完成（D+=GPIO%d, D-=GPIO%d），等待印表機連接...\n",
                USB_DP_PIN, USB_DM_PIN);
  return true;
}

void UsbPrinter::_teardownHost() {
  if (_deviceHandle != nullptr) _closeDevice();
  _qHead = _qTail = 0;
  _pendingGone = false;

  if (_clientHandle != nullptr) {
    esp_err_t e = ESP_FAIL;
    for (int i = 0; i < 50 && e != ESP_OK; i++) {
      _pump(10);
      e = usb_host_client_deregister(_clientHandle);
    }
    if (e != ESP_OK) {
      Serial.printf("[USB] ✗ Client deregister 失敗 (%s)，重新啟動系統\n", esp_err_to_name(e));
      delay(100);
      ESP.restart();
    }
    _clientHandle = nullptr;
  }

  esp_err_t e = usb_host_device_free_all();
  if (e == ESP_ERR_NOT_FINISHED) {
    uint32_t flags = 0;
    unsigned long t0 = millis();
    while (!(flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) && (millis() - t0) < 3000) {
      usb_host_lib_handle_events(pdMS_TO_TICKS(50), &flags);
    }
  }

  e = ESP_FAIL;
  for (int i = 0; i < 20 && e != ESP_OK; i++) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(pdMS_TO_TICKS(20), &flags);
    e = usb_host_uninstall();
  }
  if (e != ESP_OK) {
    Serial.printf("[USB] ✗ Host uninstall 失敗 (%s)，重新啟動系統\n", esp_err_to_name(e));
    delay(100);
    ESP.restart();
  }

  _state = PRINTER_DISCONNECTED;
}

void UsbPrinter::_pump(uint32_t ms) {
  uint32_t flags = 0;
  TickType_t ticks = pdMS_TO_TICKS(ms);
  if (ticks == 0) ticks = 1;
  usb_host_lib_handle_events(ticks, &flags);
  if (_clientHandle != nullptr) {
    usb_host_client_handle_events(_clientHandle, 0);
  }
}

// 等待某個 transfer 完成。在 host 任務內要自己推進事件，在其他任務內只要睡覺。
bool UsbPrinter::_waitDone(volatile bool* done, uint32_t timeoutMs) {
  const bool inHostTask = (xTaskGetCurrentTaskHandle() == _hostTask);
  unsigned long t0 = millis();
  while (!*done) {
    if ((millis() - t0) > timeoutMs) return false;
    if (inHostTask) {
      _pump(5);
    } else {
      esp_task_wdt_reset();  // 呼叫端（loop）在等印表機時不能被看門狗重啟
      vTaskDelay(1);
    }
  }
  return true;
}

bool UsbPrinter::_popPendingAddr(uint8_t* addr) {
  if (_qHead == _qTail) return false;
  *addr = _addrQueue[_qTail];
  _qTail = (_qTail + 1) % ADDR_QUEUE_SIZE;
  return true;
}

// ============================================================
//  事件回呼：只記錄，不做任何 USB 操作（回呼內不可再呼叫 handle_events）
// ============================================================
void UsbPrinter::_clientEventCallback(const usb_host_client_event_msg_t* eventMsg, void* arg) {
  UsbPrinter* self = (UsbPrinter*)arg;
  self->_lastEventMs = millis();

  switch (eventMsg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
      Serial.printf("[USB] 偵測到新裝置，位址: %d\n", eventMsg->new_dev.address);
      {
        uint8_t next = (self->_qHead + 1) % ADDR_QUEUE_SIZE;
        if (next != self->_qTail) {  // 佇列滿了就丟棄（理論上不會發生）
          self->_addrQueue[self->_qHead] = eventMsg->new_dev.address;
          self->_qHead = next;
        }
      }
      break;

    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      // 接 Dock/Hub 時，其他裝置（HDMI、網卡…）拔除也會觸發 DEV_GONE。
      // 只有「我們正在使用的那個裝置」消失才需要關閉。
      if (self->_deviceHandle != nullptr &&
          eventMsg->dev_gone.dev_hdl == self->_deviceHandle) {
        Serial.println("[USB] 印表機已斷開");
        self->_pendingGone = true;
      } else {
        Serial.println("[USB] 其他裝置已斷開（忽略）");
      }
      break;

    default:
      break;
  }
}

// ============================================================
//  裝置開啟 / 描述符解析（在 host 任務內執行）
// ============================================================
bool UsbPrinter::_openDevice(uint8_t devAddr) {
  usb_device_handle_t handle = nullptr;
  esp_err_t err = usb_host_device_open(_clientHandle, devAddr, &handle);
  if (err != ESP_OK) {
    Serial.printf("[USB] 無法開啟裝置: %s\n", esp_err_to_name(err));
    return false;
  }
  _deviceHandle = handle;
  _state = PRINTER_CONNECTED;

  usb_device_info_t devInfo;
  if (usb_host_device_info(_deviceHandle, &devInfo) == ESP_OK) {
    Serial.printf("[USB] 速度: %s\n", devInfo.speed == USB_SPEED_LOW ? "Low-Speed" : "Full-Speed");
  }

  const usb_device_desc_t* devDesc = nullptr;
  err = usb_host_get_device_descriptor(_deviceHandle, &devDesc);
  if (err != ESP_OK || devDesc == nullptr) {
    Serial.printf("[USB] 取得裝置描述符失敗: %s\n", esp_err_to_name(err));
    _closeDevice();
    return false;
  }

  Serial.printf("[USB] VID: 0x%04X, PID: 0x%04X\n", devDesc->idVendor, devDesc->idProduct);

  if (devDesc->idVendor == 0x04F9) {
    _manufacturer = "Brother";
    if (devDesc->idProduct == 0x002D || devDesc->idProduct == 0x0074) {
      _product = "HL-1110 series";
    } else {
      _product = "Brother Printer";
    }
  } else if (devDesc->idVendor == 0x03F0) {
    _manufacturer = "HP";
    _product = "HP Printer";  // 連上後會用 Device ID 的 MDL 覆蓋
  } else {
    _manufacturer = String("VID:0x") + String(devDesc->idVendor, HEX);
    _product = String("PID:0x") + String(devDesc->idProduct, HEX);
  }
  Serial.printf("[USB] 製造商: %s\n", _manufacturer.c_str());
  Serial.printf("[USB] 產品: %s\n", _product.c_str());

  if (!_findPrinterInterface()) {
    Serial.println("[USB] 這不是印表機（可能是 Hub/Dock 內部裝置），略過");
    _closeDevice();
    return false;
  }

  if (!_claimInterface()) {
    Serial.println("[USB] 無法宣告印表機介面");
    _closeDevice();
    return false;
  }

  _getDeviceId();

  _state = PRINTER_READY;
  Serial.println("[USB] ✓ 印表機已就緒");
  return true;
}

bool UsbPrinter::_findPrinterInterface() {
  const usb_config_desc_t* configDesc = nullptr;
  esp_err_t err = usb_host_get_active_config_descriptor(_deviceHandle, &configDesc);
  if (err != ESP_OK || configDesc == nullptr) {
    Serial.printf("[USB] 取得設定描述符失敗: %s\n", esp_err_to_name(err));
    return false;
  }

  _epOut = 0;
  _epIn = 0;

  // HP 等支援 IPP-USB 的印表機會同時提供：
  //   7/1/2 或 7/1/1 = 傳統列印介面（送 PCL/PDF/PS 原始資料）← 我們要這個
  //   7/1/4          = IPP-USB（裡面跑的是 HTTP，不是列印資料）← 絕對不能選
  //   7/1/3          = IEEE 1284.4 ← 不選
  // 所以掃描所有 Printer 介面（含 alternate setting），偏好 protocol 2 > 1。
  int bestScore = 0;
  uint8_t bestIface = 0, bestAlt = 0, bestOut = 0, bestIn = 0;
  uint16_t bestMps = 64;

  const uint8_t* p = (const uint8_t*)configDesc;
  const int totalLen = configDesc->wTotalLength;
  int offset = 0;

  while (offset + 2 <= totalLen) {
    const usb_standard_desc_t* desc = (const usb_standard_desc_t*)(p + offset);
    if (desc->bLength < 2) break;

    if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      const usb_intf_desc_t* ifaceDesc = (const usb_intf_desc_t*)desc;

      if (ifaceDesc->bInterfaceClass == USB_CLASS_PRINTER &&
          ifaceDesc->bInterfaceSubClass == USB_SUBCLASS_PRINTER) {

        Serial.printf("[USB] Printer 介面 #%d alt %d, Protocol: %d\n",
                      ifaceDesc->bInterfaceNumber, ifaceDesc->bAlternateSetting,
                      ifaceDesc->bInterfaceProtocol);

        int score = 0;
        if (ifaceDesc->bInterfaceProtocol == USB_PROTOCOL_BIDIR) score = 3;
        else if (ifaceDesc->bInterfaceProtocol == USB_PROTOCOL_UNIDIR) score = 2;
        // protocol 3 (1284.4) 與 4 (IPP-USB) 不使用

        uint8_t epOut = 0, epIn = 0;
        uint16_t mps = 64;

        // 掃描這個介面底下的所有端點，直到下一個介面描述符為止
        int epOffset = offset + desc->bLength;
        while (epOffset + 2 <= totalLen) {
          const usb_standard_desc_t* d = (const usb_standard_desc_t*)(p + epOffset);
          if (d->bLength < 2) break;
          if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) break;

          if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t* ep = (const usb_ep_desc_t*)d;
            if ((ep->bmAttributes & 0x03) == USB_BM_ATTRIBUTES_XFER_BULK) {
              if (ep->bEndpointAddress & 0x80) {
                if (epIn == 0) epIn = ep->bEndpointAddress;
              } else if (epOut == 0) {
                epOut = ep->bEndpointAddress;
                mps = ep->wMaxPacketSize;
              }
            }
          }
          epOffset += d->bLength;
        }

        // 單向介面可能沒有 IN 端點；必須有 OUT 端點才算可用
        // 同分時保留先找到的（通常 alt 0）
        if (score > bestScore && epOut != 0) {
          bestScore = score;
          bestIface = ifaceDesc->bInterfaceNumber;
          bestAlt = ifaceDesc->bAlternateSetting;
          bestOut = epOut;
          bestIn = epIn;
          bestMps = mps;
        }
      }
    }
    offset += desc->bLength;
  }

  if (bestScore == 0) {
    Serial.println("[USB] 找不到可用的 Printer 介面（只有 IPP-USB 或沒有 Bulk OUT）");
    return false;
  }

  _ifaceNum = bestIface;
  _altSetting = bestAlt;
  _epOut = bestOut;
  _epIn = bestIn;
  _epOutMPS = bestMps;
  Serial.printf("[USB] 使用介面 #%d alt %d（Bulk OUT 0x%02X, MPS %d%s）\n",
                _ifaceNum, _altSetting, _epOut, _epOutMPS,
                _epIn ? "，有 Bulk IN" : "，無 Bulk IN");
  return true;
}

bool UsbPrinter::_claimInterface() {
  esp_err_t err = usb_host_interface_claim(_clientHandle, _deviceHandle, _ifaceNum, _altSetting);
  if (err != ESP_OK) {
    Serial.printf("[USB] 宣告介面失敗: %s\n", esp_err_to_name(err));
    return false;
  }
  _ifaceClaimed = true;
  Serial.printf("[USB] 已宣告介面 #%d\n", _ifaceNum);
  return true;
}

void UsbPrinter::_closeDevice() {
  // 先把狀態改掉，讓正在送資料的 sendData() 在下一個 chunk 前離開
  _state = PRINTER_DISCONNECTED;

  // 等 sendData() 放開 mutex；它需要我們持續推進事件才能完成/取消 transfer
  bool gotMutex = false;
  unsigned long t0 = millis();
  while (!(gotMutex = (xSemaphoreTake(_ioMutex, 0) == pdTRUE))) {
    if ((millis() - t0) > 3000) break;
    _pump(5);
  }

  if (_deviceHandle != nullptr) {
    if (_ifaceClaimed) {
      usb_host_interface_release(_clientHandle, _deviceHandle, _ifaceNum);
      _ifaceClaimed = false;
    }
    usb_host_device_close(_clientHandle, _deviceHandle);
    _deviceHandle = nullptr;
  }

  _epOut = 0;
  _epIn = 0;
  _deviceId = "";
  _manufacturer = "";
  _product = "";

  if (gotMutex) xSemaphoreGive(_ioMutex);
  Serial.println("[USB] 裝置已關閉");
}

// ============================================================
//  Control transfer 共用函式（EP0）
//  inBuf != nullptr 代表 IN 方向，wLength 必須是 64 的倍數
// ============================================================
bool UsbPrinter::_control(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                          uint8_t* inBuf, uint16_t wLength, size_t* gotBytes) {
  if (_deviceHandle == nullptr) return false;
  if (gotBytes) *gotBytes = 0;

  usb_transfer_t* transfer = nullptr;
  if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + wLength, 0, &transfer) != ESP_OK) return false;

  XferCtx* ctx = new XferCtx{false};

  usb_setup_packet_t* setup = (usb_setup_packet_t*)transfer->data_buffer;
  setup->bmRequestType = bmRequestType;
  setup->bRequest = bRequest;
  setup->wValue = wValue;
  setup->wIndex = wIndex;
  setup->wLength = wLength;

  transfer->device_handle = _deviceHandle;
  transfer->bEndpointAddress = 0;
  transfer->num_bytes = sizeof(usb_setup_packet_t) + wLength;
  transfer->timeout_ms = 1000;
  transfer->context = ctx;
  transfer->callback = [](usb_transfer_t* t) { ((XferCtx*)t->context)->done = true; };

  if (usb_host_transfer_submit_control(_clientHandle, transfer) != ESP_OK) {
    usb_host_transfer_free(transfer);
    delete ctx;
    return false;
  }

  if (!_waitDone(&ctx->done, 2000)) {
    // 逾時且 transfer 仍在飛：不能釋放（回呼之後還會用到），寧可洩漏
    Serial.println("[USB] ⚠ Control transfer 逾時");
    return false;
  }

  bool ok = (transfer->status == USB_TRANSFER_STATUS_COMPLETED);
  if (ok && inBuf && wLength > 0) {
    size_t n = transfer->actual_num_bytes;
    n = (n > sizeof(usb_setup_packet_t)) ? n - sizeof(usb_setup_packet_t) : 0;
    if (n > wLength) n = wLength;
    memcpy(inBuf, transfer->data_buffer + sizeof(usb_setup_packet_t), n);
    if (gotBytes) *gotBytes = n;
  }

  usb_host_transfer_free(transfer);
  delete ctx;
  return ok;
}

String UsbPrinter::_idField(const String& id, const char* key) {
  // IEEE 1284 Device ID 格式：MFG:HP;MDL:HP LaserJet Pro 4003dn;CMD:...;
  String k = String(key) + ":";
  int i = id.indexOf(k);
  if (i < 0) return "";
  i += k.length();
  int j = id.indexOf(';', i);
  if (j < 0) j = id.length();
  String v = id.substring(i, j);
  v.trim();
  return v;
}

bool UsbPrinter::_getDeviceId() {
  uint8_t buf[256];
  size_t got = 0;

  // wIndex 高位元組 = 介面編號，低位元組 = Alternate Setting（0）
  bool ok = _control(USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_CLASS |
                     USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
                     USB_PRINTER_GET_DEVICE_ID, 0, (uint16_t)((_ifaceNum << 8) | _altSetting), buf, 256, &got);

  if (ok && got > 2) {
    int len = (buf[0] << 8) | buf[1];  // 大端序，包含這 2 個位元組本身
    if (len > 2 && len <= (int)got) {
      _deviceId = String((char*)(buf + 2), len - 2);
      Serial.printf("[USB] Device ID: %s\n", _deviceId.c_str());
      String mfg = _idField(_deviceId, "MFG");
      if (mfg.length() == 0) mfg = _idField(_deviceId, "MANUFACTURER");
      String mdl = _idField(_deviceId, "MDL");
      if (mdl.length() == 0) mdl = _idField(_deviceId, "MODEL");
      if (mfg.length()) _manufacturer = mfg;
      if (mdl.length()) _product = mdl;
    }
  }
  return _deviceId.length() > 0;
}

bool UsbPrinter::softReset() {
  bool ok = _control(USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS |
                     USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
                     USB_PRINTER_SOFT_RESET, 0, (uint16_t)((_ifaceNum << 8) | _altSetting), nullptr, 0, nullptr);
  if (ok) Serial.println("[USB] 印表機已重置");
  return ok;
}

// ============================================================
//  工作層級鎖
// ============================================================
bool UsbPrinter::acquireJob(uint32_t timeoutMs) {
  if (_jobMutex == nullptr) return false;
  if (xSemaphoreTake(_jobMutex, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) return false;
  _jobLocked = true;
  return true;
}

void UsbPrinter::releaseJob() {
  if (_jobMutex == nullptr) return;
  _jobLocked = false;
  xSemaphoreGive(_jobMutex);
}

// ============================================================
//  資料傳送（可由任何任務呼叫；完成事件由 host 任務推進）
// ============================================================
int UsbPrinter::sendData(const uint8_t* data, size_t length) {
  if (data == nullptr || length == 0) return 0;
  if (xSemaphoreTake(_ioMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return -1;

  if (_state != PRINTER_READY || _deviceHandle == nullptr || _epOut == 0) {
    xSemaphoreGive(_ioMutex);
    return -1;
  }
  _state = PRINTER_BUSY;

  usb_transfer_t* transfer = nullptr;
  if (usb_host_transfer_alloc(USB_BULK_CHUNK_SIZE, 0, &transfer) != ESP_OK) {
    _state = PRINTER_READY;
    xSemaphoreGive(_ioMutex);
    return 0;
  }
  XferCtx* ctx = new XferCtx{false};

  size_t total = 0;
  bool leak = false;

  while (total < length && _state == PRINTER_BUSY) {
    size_t n = length - total;
    if (n > (size_t)USB_BULK_CHUNK_SIZE) n = USB_BULK_CHUNK_SIZE;

    memcpy(transfer->data_buffer, data + total, n);
    transfer->device_handle = _deviceHandle;
    transfer->bEndpointAddress = _epOut;
    transfer->num_bytes = n;
    transfer->timeout_ms = 0;  // IDF 尚未實作 transfer timeout；等待與中止由下面處理
    transfer->context = ctx;
    transfer->callback = [](usb_transfer_t* t) { ((XferCtx*)t->context)->done = true; };
    ctx->done = false;

    if (usb_host_transfer_submit(transfer) != ESP_OK) break;

    // 印表機忙碌時會一直 NAK，transfer 就一直「進行中」。這是正常的，耐心等。
    bool finished = _waitDone(&ctx->done, 5000);
    if (!finished) {
      Serial.println("[USB] ⏳ 印表機暫時不收資料（忙碌/暖機/列印中？），等待中...");
      finished = _waitDone(&ctx->done, USB_STALL_ABORT_MS);
    }
    if (!finished) {
      Serial.printf("[USB] ✗ 印表機超過 %d 秒沒有收資料（卡紙/缺紙/蓋子開啟/離線？），中止工作，已送 %u / %u bytes\n",
                    USB_STALL_ABORT_MS / 1000, (unsigned)total, (unsigned)length);
      usb_host_endpoint_halt(_deviceHandle, _epOut);
      usb_host_endpoint_flush(_deviceHandle, _epOut);
      if (!_waitDone(&ctx->done, 2000)) leak = true;
      else usb_host_endpoint_clear(_deviceHandle, _epOut);
      break;
    }

    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED || transfer->actual_num_bytes == 0) {
      Serial.printf("[USB] 傳送失敗 (status %d)，已送 %u / %u bytes\n",
                    (int)transfer->status, (unsigned)total, (unsigned)length);
      if (transfer->status == USB_TRANSFER_STATUS_STALL) {
        usb_host_endpoint_clear(_deviceHandle, _epOut);
      }
      break;
    }

    total += transfer->actual_num_bytes;
  }

  if (!leak) {
    usb_host_transfer_free(transfer);
    delete ctx;
  }

  if (_state == PRINTER_BUSY) _state = PRINTER_READY;
  xSemaphoreGive(_ioMutex);
  return (int)total;
}
