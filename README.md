# ESP32-S3 WiFi Print Server

Turn a USB-only printer into a wireless network printer using an ESP32-S3 dev board.

> 用一塊 ESP32-S3 開發板，讓只有 USB 的印表機變成 WiFi 無線印表機。

<p align="center">
  <img src="images/setup.jpg" width="600" alt="ESP32-S3 connected to Brother HL-1110">
</p>

<p align="center">
  <img src="images/hello_world.jpg" width="400" alt="Hello World test print">
</p>

<p align="center"><i>ESP32-S3 + Brother HL-1110 = WiFi Printer. Hello, World!</i></p>

## What It Does

This project turns an **ESP32-S3** into a transparent WiFi-to-USB print bridge. Your computer sends print data over WiFi, and the ESP32 forwards it to the printer via USB Host — no Raspberry Pi needed, just a $10 microcontroller.

```
[Computer] ~~~WiFi~~~ [ESP32-S3] ---USB--- [Printer]
```

Originally built for the **Brother HL-1110**, but should work with any USB printer that supports standard USB Printer Class (most do).

## Features

- **Raw Socket (Port 9100)** — JetDirect/AppSocket protocol, works natively with macOS, Windows, and Linux
- **Web Management UI** — Dark-themed dashboard to monitor printer status, configure WiFi, and restart the device
- **Auto WiFi Setup** — Boots into AP mode on first run; connect and configure via browser
- **mDNS Discovery** — Access via `hl1110.local` instead of memorizing IP addresses
- **OTA Updates** — Update firmware wirelessly from Arduino IDE
- **PSRAM Buffer** — 512KB print buffer in PSRAM handles large print jobs
- **Auto Reconnect** — Automatically reconnects to WiFi if connection drops
- **Watchdog Timer** — Auto-recovers from crashes

## Hardware Required

| Component | Notes |
|-----------|-------|
| ESP32-S3 dev board (N16R8 recommended) | Dual Type-C, 16MB Flash, 8MB PSRAM |
| USB OTG adapter | Type-C male → USB-A female, **must support OTG/Host mode** |
| USB printer cable | Type-A to Type-B (usually comes with the printer) |
| 5V USB power supply | Any phone charger works |
| **VBUS source (required)** | The printer needs 5V on VBUS to enable its D+ pull-up. A dock with its own 5V PD input also works on core 3.3.x. See `USB_HOST_TROUBLESHOOTING.md` |

### Wiring

```
ESP32-S3 (Dual Type-C board)
├── USB Port ──► OTG Adapter (VBUS must be 5V) ──► Printer USB Cable ──► Printer
└── UART Port ──► 5V Power Supply (or computer for programming)
```

> **Important:** The USB OTG adapter must support Host mode. Regular charging adapters won't work. A self-powered USB-C dock/hub works on Arduino-ESP32 3.3.x (IDF 5.5.x) if the dock has its own 5 V supply; older cores only support a direct connection. See `USB_HOST_TROUBLESHOOTING.md`.

## Software Setup

### 1. Install Arduino IDE

Download [Arduino IDE 2.x](https://www.arduino.cc/en/software) and install the ESP32 board package:

- Go to **File → Preferences**
- Add to **Additional Board Manager URLs**:
  ```
  https://espressif.github.io/arduino-esp32/package_esp32_index.json
  ```
- Go to **Tools → Board → Board Manager**, search `esp32`, install **esp32 by Espressif Systems**

### 2. Board Settings

| Setting | Value |
|---------|-------|
| Board | ESP32S3 Dev Module |
| USB CDC On Boot | Disabled |
| USB Mode | USB-OTG (TinyUSB) |
| PSRAM | OPI PSRAM |
| Flash Size | 16MB (128Mb) |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
| Upload Mode | UART0 / Hardware CDC |

> **USB Mode must be USB-OTG (TinyUSB)** for USB Host to work.

### 3. Upload

1. Connect ESP32's **UART port** to your computer
2. Open `ESP32S3_PrintServer.ino` in Arduino IDE
3. Click **Upload**
4. If upload fails, hold **BOOT**, press **RST**, release RST, wait 2 seconds, release BOOT

## First-Time Setup

### 1. Connect Printer

1. Plug the printer into the ESP32's **USB port** via OTG adapter 
2. Power the ESP32 via the **UART port**
3. Open Serial Monitor (115200 baud) — you should see:
   ```
   [USB] VID: 0x04F9, PID: 0x002D
   [USB] Manufacturer: Brother
   [USB] Product: HL-1110 series
   [USB] ✓ Printer ready
   ```

### 2. Configure WiFi

1. Connect your phone/computer to WiFi network **PrintServer-Setup** (password: `12345678`)
2. Open **http://192.168.4.1** in your browser
3. Click **Scan** to find available networks
4. Select your WiFi, enter password, click **Connect**
5. The page will show the new IP address

### 3. Add Printer on Mac

1. Open **System Settings → Printers & Scanners**
2. Click **Add Printer**
3. Select the **IP** tab
4. Configure:

| Field | Value |
|-------|-------|
| Protocol | HP Jetdirect - Socket |
| Address | `hl1110.local` or the ESP32's IP address |
| Queue | (leave empty) |
| Name | Brother HL-1110 WiFi |
| Use | Select Brother HL-1110 driver |

5. Click **Add** — done!

### Add Printer on Windows

1. Open **Settings → Devices → Printers & Scanners**
2. Click **Add a printer or scanner**
3. Click **The printer that I want isn't listed**
4. Select **Add a printer using TCP/IP address**
5. Device type: **TCP/IP Device**
6. Hostname or IP address: ESP32's IP
7. Port: `9100`
8. Select the appropriate Brother driver

## Web Management UI

Access the dashboard at `http://hl1110.local` (or the ESP32's IP address):

- **Printer Status** — Connected/Disconnected, model info, active print job progress
- **WiFi Config** — Scan and connect to networks, signal strength
- **System Info** — Uptime, memory usage, total print jobs
- **Device Restart** — Remote restart button

## Project Structure

```
ESP32S3_PrintServer/
├── ESP32S3_PrintServer.ino   # Main sketch — setup & loop
├── config.h                  # All configuration constants
├── usb_printer.h / .cpp      # USB Host driver for printer class devices
├── wifi_manager.h / .cpp     # WiFi STA/AP management with NVS persistence
├── print_server.h / .cpp     # Raw Socket server (port 9100)
├── web_ui.h / .cpp           # Web dashboard (HTML/CSS/JS embedded)
└── GUIDE.md                  # Detailed setup guide (Chinese)
```

## Architecture

```
┌─────────────┐        WiFi         ┌──────────────────┐
│  Computer   │ ◄──────────────────► │    ESP32-S3      │
│             │  Raw Socket :9100   │                  │
│  Printer    │                     │ ┌──────────────┐ │    USB     ┌────────────┐
│  Driver     │ ── print data ────► │ │ Print Server │ │ ────────► │  Printer   │
│             │                     │ └──────────────┘ │ USB Host  │            │
│  Browser    │ ◄──── HTTP ───────► │ ┌──────────────┐ │           └────────────┘
│             │  Dashboard :80      │ │   Web UI     │ │
└─────────────┘                     │ └──────────────┘ │
                                    │ ┌──────────────┐ │
                                    │ │ WiFi Manager │ │
                                    │ └──────────────┘ │
                                    │ ┌──────────────┐ │
                                    │ │  mDNS + OTA  │ │
                                    │ └──────────────┘ │
                                    └──────────────────┘
```

## Troubleshooting

**Printer not detected:**
- Make sure you're using a real OTG adapter (not a charging adapter)
- Measure VBUS at the adapter's USB-A socket (should be ~5V). A hub/dock only works on core 3.3.x and needs its own 5V supply
- On core 3.3.x the firmware must set an enumeration filter callback (included)
- See `USB_HOST_TROUBLESHOOTING.md` for `HUB: Root port reset failed`
- Try unplugging and replugging the USB cable after ESP32 boots
- Check Serial Monitor for `[USB]` messages

**Can't connect to WiFi:**
- ESP32 only supports **2.4GHz WiFi** (not 5GHz)
- Check SSID and password are correct
- If stuck, re-flash the firmware to reset WiFi settings

**Mac can't find printer:**
- Make sure Mac and ESP32 are on the same network
- Try using IP address instead of `.local` hostname
- Test connectivity: `nc -z <ESP32_IP> 9100`
- Make sure the Brother driver is installed

**Prints garbled text or blank pages:**
- Use **HP Jetdirect - Socket** protocol (not IPP or LPD)
- Select the correct printer driver (e.g., Brother HL-1110, not Generic PostScript)

## Adapting for Other Printers

This project should work with most USB printers that implement the standard USB Printer Class (bInterfaceClass = 0x07). To adapt:

1. In `usb_printer.cpp`, update the VID/PID identification in `_openDevice()` for your printer model
2. In `config.h`, update `PRINTER_NAME` and `MDNS_HOSTNAME`
3. On your computer, select the correct driver for your printer model

## Configuration

Edit `config.h` to customize:

| Constant | Default | Description |
|----------|---------|-------------|
| `AP_SSID` | `PrintServer-Setup` | AP mode network name |
| `AP_PASSWORD` | `12345678` | AP mode password |
| `MDNS_HOSTNAME` | `hl1110` | mDNS hostname (access via `<name>.local`) |
| `RAW_SOCKET_PORT` | `9100` | Print server port |
| `PRINT_BUFFER_SIZE` | `512 * 1024` | Print buffer size (in PSRAM) |
| `LED_PIN` | `2` | Status LED GPIO pin |
| `OTA_PASSWORD` | `""` | OTA update password (empty = no password) |
| `WDT_TIMEOUT_SEC` | `30` | Watchdog timeout in seconds |

## License

MIT License — see [LICENSE](LICENSE) for details.


## Tested configurations

| Printer | Board | Core | Notes |
|---------|-------|------|-------|
| Brother HL-1110 | ESP32-S3 N16R8 | — | original target |
| HP LaserJet Pro 4003dn | any ESP32-S3 | 3.3.x | IPP + raw 9100 code added; **not yet verified on hardware** |
| Brother HL-L2300D | dual-USB-C ESP32-S3 N16R8 | Arduino-ESP32 3.3.12 / IDF 5.5.5 | via USB-C dock powered from its own 5 V PD input; needed the enum filter callback |

### Windows setup (verified with HL-L2300D)
1. Install the official Brother driver for your model.
2. Add a printer using a **Standard TCP/IP port**: IP of the ESP32, protocol **RAW**, port **9100**.
3. **Disable SNMP status** on that port (the ESP32 does not answer SNMP).
4. Print a test page; also test after restarting both the ESP32 and the printer.


## Direct wiring (USB-A breakout to ESP32-S3)

| USB-A wire | ESP32-S3 |
|---|---|
| Red (VBUS) | 5V |
| White (D-) | GPIO19 |
| Green (D+) | GPIO20 |
| Black (GND) | GND |

Power the board via the COM/UART port, leave the native USB port empty, and set
`Tools -> USB Mode -> USB-OTG (TinyUSB)`, `USB CDC On Boot -> Disabled`.
Details in `USB_HOST_TROUBLESHOOTING.md`.


## Android / Windows (IPP)

The firmware also runs an IPP server on port 631 and advertises it over mDNS, so Android's
Default Print Service and Windows "Add printer" can discover it. Setup, limits and
troubleshooting: see `ANDROID_WINDOWS_SETUP.md`. Target printer for this release:
HP LaserJet Pro 4003dn (prints PDF natively; the ESP32 forwards the PDF unchanged).

Host-side protocol tests (no hardware needed):
`cd test && g++ -std=gnu++17 -I.. ipp_test.cpp -o ipp_test && ./ipp_test`
