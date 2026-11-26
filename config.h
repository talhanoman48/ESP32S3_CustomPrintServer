#ifndef CONFIG_H
#define CONFIG_H

// ============================================================
// ESP32-S3 WiFi Print Server — 設定檔
// 適用印表機：HP LaserJet Pro 4003dn（亦相容 Brother HL-1110 / HL-L2300D 的 Raw 9100 列印）
// 開發板：ESP32-S3-N16R8
// ============================================================

// --- WiFi 預設設定 ---
// 首次啟動時會進入 AP 模式讓你設定，這裡是備用預設值
#define DEFAULT_WIFI_SSID     ""
#define DEFAULT_WIFI_PASSWORD ""

// --- AP 模式設定（首次設定用）---
#define AP_SSID       "PrintServer-Setup"
#define AP_PASSWORD   "12345678"

// --- 列印伺服器設定 ---
#define PRINTER_NAME       "HP_LaserJet_4003dn_WiFi"
#define PRINTER_MODEL_NAME "HP LaserJet Pro 4003dn"   // USB 連上前 / 讀不到 Device ID 時使用
#define RAW_SOCKET_PORT    9100    // JetDirect/AppSocket 協定埠
#define WEB_SERVER_PORT    80      // Web 管理介面埠
#define MDNS_HOSTNAME      "hp4003dn"  // 可用 hp4003dn.local 存取

// --- IPP 伺服器（Android / Windows 自動發現與列印用）---
#define IPP_PORT                  631
#define IPP_PATH                  "/ipp/print"
#define IPP_TASK_PRIORITY         2
#define IPP_TASK_STACK_SIZE       10240
// HP 的標準做法：用 UEL (ESC%-12345X) 框住每份工作，讓印表機知道工作從哪裡開始、何時結束。
#define IPP_UEL_FRAMING           1
// 若 PDF 沒被自動辨識，可改成 "PDF"，會在工作開頭送 "@PJL ENTER LANGUAGE = PDF"。空字串 = 不送。
#define IPP_PJL_LANGUAGE          ""

// --- USB Host 接線（直接焊線/杜邦線接到 ESP32-S3，不經過 Type-C 轉接頭或 Dock）---
// ESP32-S3 的 USB 資料腳是固定的，不能改：
//   USB-A 白線 (D-)  → GPIO19
//   USB-A 綠線 (D+)  → GPIO20
//   USB-A 紅線 (VBUS)→ 板子的 5V 腳（板子要由 COM/UART 口供電）
//   USB-A 黑線 (GND) → GND
// 原生 "USB" Type-C 口必須空接（它跟 GPIO19/20 是同一組線）。
#define USB_DM_PIN                19
#define USB_DP_PIN                20

// --- USB Host 設定 ---
#define USB_HOST_TASK_PRIORITY    10      // 高於 loop()（優先權 1）
#define USB_HOST_TASK_STACK_SIZE  8192    // 4096 太小：回呼內有 Serial.printf + 描述符解析
#define USB_HOST_TASK_CORE        1
#define USB_HOST_START_DELAY_MS   1500    // WiFi 啟動完成、電源穩定後才啟動 USB Host
#define USB_RESCAN_INTERVAL_MS    20000   // 沒有任何裝置時，每隔多久重啟一次 Host（0 = 關閉）
#define USB_BULK_CHUNK_SIZE       1024    // 每次 Bulk OUT 傳輸大小（必須是 64 的倍數）
// 印表機忙碌時（正在印、暖機）會對資料 NAK，這是正常的，必須耐心等，不能丟資料。
// 只有「連續這麼久一個 1KB 都送不出去」才判定為卡紙/缺紙/蓋子開啟/離線並中止工作。
#define USB_STALL_ABORT_MS        120000
#define PRINT_IDLE_TIMEOUT_MS     120000  // 連線開著但這麼久沒資料 → 關閉，避免卡住後面的工作
#define PRINT_BUFFER_SIZE         (512 * 1024)  // 512KB 列印緩衝區（存在 PSRAM）

// --- 狀態 LED（可選）---
#define LED_PIN           2       // 板載 LED（依你的板子調整）
#define LED_WIFI_OK       1       // WiFi 已連線
#define LED_PRINTING      2       // 列印中
#define LED_ERROR         3       // 錯誤

// --- OTA 更新 ---
#define OTA_HOSTNAME      "printserver"
#define OTA_PASSWORD      ""      // 留空表示不需密碼

// --- 看門狗 ---
#define WDT_TIMEOUT_SEC   30      // 看門狗超時秒數

// --- NVS 儲存鍵名 ---
#define NVS_NAMESPACE     "printsvr"
#define NVS_KEY_SSID      "wifi_ssid"
#define NVS_KEY_PASS      "wifi_pass"
#define NVS_KEY_HOSTNAME  "hostname"

#endif // CONFIG_H
