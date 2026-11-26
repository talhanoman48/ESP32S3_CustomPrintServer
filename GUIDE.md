# ESP32-S3 WiFi Print Server — 完整指南

> 讓 Brother HL-1110 透過 WiFi 從 Mac 列印

---

## 硬體需求

| 項目 | 規格 | 備註 |
|------|------|------|
| 開發板 | ESP32-S3-N16R8 (雙 Type-C) | 你已有 |
| 印表機 | Brother HL-1110 | USB 連接 |
| USB OTG 轉接線 | Type-C 公 → USB-A 母 | **關鍵零件** |
| USB 線 | USB-A 公 → USB-B 公 | 印表機原裝線 |
| 電源 | 5V USB 充電器 + Type-C 線 | 供電給 ESP32 |

### 接線方式

```
[Mac] ~~~WiFi~~~ [ESP32-S3] ---USB OTG--- [Brother HL-1110]
                     |
              (另一個 Type-C 供電/燒錄)
```

**ESP32-S3 雙 Type-C 接口：**
- **USB 接口（標示 USB）：** 接 OTG 轉接線 → 接印表機（USB Host 模式）
- **UART 接口（標示 COM/UART）：** 接電腦燒錄，之後當供電用

> ⚠️ **重要：** USB OTG 轉接線一定要是「Type-C 公 → USB-A 母」的 OTG 線，不是普通充電線！可搜尋「USB-C OTG 轉接頭」購買，約 NT$50-100。

---

## 軟體安裝

### Step 1：安裝 Arduino IDE

1. 下載安裝 [Arduino IDE 2.x](https://www.arduino.cc/en/software)
2. 開啟 Arduino IDE

### Step 2：安裝 ESP32 開發板支援

1. **File → Preferences**
2. 在 **Additional Board Manager URLs** 加入：
   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
3. **Tools → Board → Board Manager**
4. 搜尋 `esp32`，安裝 **esp32 by Espressif Systems**（選最新版）

### Step 3：設定開發板

在 **Tools** 選單中設定：

| 設定項 | 選擇 |
|--------|------|
| Board | ESP32S3 Dev Module |
| USB CDC On Boot | Disabled |
| USB Mode | USB-OTG (TinyUSB) |
| PSRAM | OPI PSRAM |
| Flash Size | 16MB |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
| Upload Mode | UART0 / Hardware CDC |
| Port | 選擇你的 COM port |

> ⚠️ **USB Mode 一定要選 USB-OTG (TinyUSB)**，這樣 USB 接口才能當 Host 用。

### Step 4：開啟專案

1. 用 Arduino IDE 開啟 `ESP32S3_PrintServer.ino`
2. 同資料夾的 `.h` 和 `.cpp` 檔會自動載入
3. 確認所有分頁都有出現（config.h, usb_printer.h/cpp, wifi_manager.h/cpp, print_server.h/cpp, web_ui.h/cpp）

### Step 5：燒錄

1. 用 Type-C 線連接 ESP32 的 **UART 接口**到電腦
2. 按下 **Upload**（→ 按鈕）
3. 如果燒錄失敗，按住板子上的 **BOOT** 鍵，同時按 **RST** 鍵，然後放開 RST，等幾秒再放開 BOOT

---

## 首次設定

### Step 1：連接印表機

1. 燒錄完成後，用 OTG 轉接線把印表機接到 ESP32 的 **USB 接口**
2. 另一個 Type-C 接口接 5V 電源（或繼續接電腦）
3. 開啟 Arduino IDE 的 Serial Monitor（115200 baud）
4. 你應該會看到類似：
   ```
   [USB] 偵測到新裝置，位址: 1
   [USB] VID: 0x04F9, PID: 0x002D
   [USB] 製造商: Brother
   [USB] 產品: HL-1110 series
   [USB] ✓ 印表機已就緒
   ```

### Step 2：設定 WiFi

1. 用手機或電腦搜尋 WiFi，找到 **PrintServer-Setup**
2. 密碼：**12345678**
3. 連線後瀏覽器開啟 **http://192.168.4.1**
4. 在管理介面：
   - 點 **Scan** 掃描可用的 WiFi
   - 選擇你家的 WiFi
   - 輸入密碼
   - 點 **Connect**
5. 連線成功後，頁面會顯示新的 IP 位址

### Step 3：Mac 端設定

1. 先確認 Mac 有安裝 Brother HL-1110 驅動
   - 下載：[Brother 官網驅動頁面](https://support.brother.com/g/b/downloadtop.aspx?c=us_ot&lang=en&prod=hl1110_us_eu_as)
   - 或者 macOS 可能已內建

2. 開啟 **系統設定 → 印表機與掃描器**

3. 點 **加入印表機**（+ 按鈕）

4. 選擇 **IP** 分頁

5. 填寫：
   | 欄位 | 填入 |
   |------|------|
   | 協定 | HP Jetdirect - Socket |
   | 位址 | `hl1110.local` 或 ESP32 的 IP |
   | 佇列 | 留空 |
   | 名稱 | Brother HL-1110 WiFi |
   | 使用 | 選擇 Brother HL-1110 驅動 |

6. 點 **加入**

7. 完成！現在你可以從任何 App 選擇這台印表機列印了。

---

## 日常使用

- **列印：** 就像一般網路印表機一樣，從 Mac 選擇印表機直接列印
- **管理介面：** 瀏覽器開啟 `http://hl1110.local` 查看狀態
- **印表機沒回應：** 管理介面點 Restart，或拔插 ESP32 電源
- **OTA 更新：** Arduino IDE 選擇 Port 時會看到 `printserver`（網路），可以無線更新韌體

---

## 疑難排解

### 印表機偵測不到

- 確認用的是 OTG 轉接線（不是普通充電線）
- 確認接在 USB 接口（不是 UART 接口）
- 試著拔插 USB 線
- Serial Monitor 檢查有沒有 `[USB]` 相關訊息

### WiFi 連不上

- 確認 SSID 和密碼正確
- ESP32 只支援 **2.4GHz WiFi**，5GHz 的不行
- 如果完全連不上，按住 BOOT 鍵 10 秒可以重置 WiFi 設定（需要在程式中實作此功能）

### Mac 找不到印表機

- 確認 Mac 和 ESP32 在同一個 WiFi 網路
- 用 IP 位址而非 `.local` 試試
- 確認有安裝 Brother 驅動
- 開啟 Terminal 測試連線：`nc -z <ESP32_IP> 9100`（應該會成功連線）

### 列印出亂碼或空白頁

- 確認 Mac 端選的驅動是 Brother HL-1110（不是 Generic PostScript）
- 協定一定要選 **HP Jetdirect - Socket**，不要選 IPP 或 LPD

### 直接接線（USB-A 母座/剪線 → ESP32-S3）

| USB 線色 | ESP32-S3 |
|---|---|
| 紅 (VBUS) | 5V |
| 白 (D-) | GPIO19 |
| 綠 (D+) | GPIO20 |
| 黑 (GND) | GND |

板子由 COM 口供電，原生 USB 口空接。Arduino IDE：USB Mode = USB-OTG (TinyUSB)、USB CDC On Boot = Disabled。

### 經過 USB-C Dock / Hub 連接（Arduino-ESP32 3.3.x）

- Dock 必須自己有 5V 供電（接在 Dock 的 PD / USB-C 輸入）
- Dock 的 USB-C 插頭 → ESP32 的 **USB** 接口；Dock 的 USB-A → 印表機
- 此時 COM 接口可空接（就沒有 Serial Monitor）、HDMI 不接
- 舊版核心（IDF 5.1–5.3）不支援 Hub，請直接連接

### Windows 設定

標準 TCP/IP 連接埠 → 通訊協定 **RAW**、埠 **9100**，並**關閉 SNMP 狀態**。

---

## 進階：修改設定

編輯 `config.h` 可以調整：

- `AP_SSID` / `AP_PASSWORD`：AP 模式的名稱和密碼
- `MDNS_HOSTNAME`：mDNS 名稱（預設 `hl1110`）
- `RAW_SOCKET_PORT`：列印埠（預設 9100，通常不需要改）
- `PRINT_BUFFER_SIZE`：列印緩衝區大小（預設 512KB）
- `LED_PIN`：狀態 LED 的腳位

---

## 系統架構

```
┌─────────────┐         WiFi          ┌──────────────────┐
│    Mac      │ ◄───────────────────► │    ESP32-S3      │
│             │   Raw Socket :9100    │                  │
│ Brother     │                       │ ┌──────────────┐ │     USB      ┌────────────┐
│ Driver      │ ─── 列印資料 ──────► │ │ Print Server │ │ ──────────► │ Brother    │
│             │                       │ └──────────────┘ │  USB Host   │ HL-1110    │
│ Safari      │ ◄───── HTTP ────────► │ ┌──────────────┐ │             └────────────┘
│             │   管理介面 :80        │ │   Web UI     │ │
└─────────────┘                       │ └──────────────┘ │
                                      │ ┌──────────────┐ │
                                      │ │ WiFi Manager │ │
                                      │ └──────────────┘ │
                                      │ ┌──────────────┐ │
                                      │ │  mDNS + OTA  │ │
                                      │ └──────────────┘ │
                                      └──────────────────┘
```

---

## 零件購買建議

1. **USB-C OTG 轉接頭**（Type-C 公 → USB-A 母）
   - 蝦皮搜尋「USB-C OTG 轉接頭」，約 NT$50-100
   - 確認支援 OTG / Host 模式

2. **5V USB 充電器**（給 ESP32 供電用）
   - 手機充電器即可，1A 以上

---

*建立日期：2026-05-20*


---

## Android / Windows 自動發現（IPP）

韌體現在同時提供 IPP（埠 631）並透過 mDNS 廣播。Android：設定 → 列印 → 預設列印服務 → 開啟，
即可看到印表機；Windows：新增印表機（或用 HP 驅動 + RAW 9100，最穩）。
詳細步驟與疑難排解見 `ANDROID_WINDOWS_SETUP.md`。
