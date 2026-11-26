# `HUB: Root port reset failed` — troubleshooting

## What the error means
ESP-IDF's USB host stack saw something attach to the root port, drove a bus
reset (SE0 for ~10 ms), and after the reset the port did **not** come up
enabled. In plain terms: the printer's D+ pull-up appeared, then did not
survive the reset. That is almost always electrical, not logical.

The error is only logged. The stack then sits idle until you replug, which is
why it looks like "nothing happens". As of this version the firmware watches
for the message and restarts the USB Host automatically (backoff 1 → 10 s),
but it cannot fix a hardware fault.

## Check in this order

### 1. VBUS at the printer (most likely cause)
A self-powered device (every mains printer) still watches VBUS: below ~4.4 V
many of them drop their D+ pull-up. The ESP32-S3 has **no VBUS output** — it is
an input on the dev board's USB connector. Whether 5 V reaches the OTG adapter
depends on the board.

Test: power the board from the UART port, plug only the OTG adapter into the
USB port, and measure between the VBUS (+) and GND pins of the adapter's
USB-A socket.

| Reading | Meaning |
|---|---|
| 4.75–5.25 V | Fine, go to step 2 |
| ~0 V | **This is your bug.** Feed VBUS from the board's 5V pin (see wiring) |
| 3.8–4.5 V | Diode drop / weak supply. Use the 5V pin directly |

### 2. Direct wiring (recommended; no Type-C adapter, no dock)
Use a USB-A female breakout (or cut the A end off a printer cable) and wire:

| USB wire | USB-A pin | ESP32-S3 |
|---|---|---|
| Red (VBUS) | 1 | **5V** pin |
| White (D-) | 2 | **GPIO19** |
| Green (D+) | 3 | **GPIO20** |
| Black (GND) | 4 | **GND** |
| Shield | - | leave open (or GND at ESP32 end only) |

- Power the board from the **COM/UART** port (gives Serial Monitor and puts 5 V on the 5V pin).
- Leave the native **USB** port empty: it shares GPIO19/20.
- Verify cable colours by continuity from the B plug (pin 1 VBUS, 2 D-, 3 D+, 4 GND) before connecting.
- D+/D- are 3.3 V logic. Never connect VBUS to 3V3.
- Keep D+/D- under ~15 cm, twisted, with GND alongside.
- Add 100-470 uF across VBUS/GND at the connector; use a >=1 A supply.
- D+/D- swapped, or GND missing, gives `Root port reset failed` every time.

### 3. Enumeration filter callback (Arduino-ESP32 3.3.x / IDF 5.5.x)
On these cores `CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK` is enabled, and a
null `enum_filter_cb` means **nothing ever finishes enumerating** — no printer,
no dock, no error. `usb_printer.cpp` now installs a filter that accepts the
device and selects configuration 1 (guarded by `#ifdef`, so older cores still
build). Found and verified by a user on core 3.3.12 / IDF 5.5.5.

### 3b. USB hubs and docks
Older cores (IDF 5.1–5.3) only enumerate devices directly on the root port.
With a current core (3.3.x / IDF 5.5.x) a hub or USB-C dock **can** work
(reported working with an HL-L2300D). Things that matter:

- A dock needs its **own 5 V** (its PD / USB-C input). It will not enumerate if
  it only gets whatever the ESP32 board passes through.
- A dock exposes several internal devices (hub, HDMI, ...). The firmware now
  queues them, ignores non-printers, and ignores `DEV_GONE` for devices it does
  not own.
- Because the dock is the VBUS source, this also sidesteps cause 1 above.

Tested arrangement (HL-L2300D, dual-USB-C ESP32-S3 N16R8):
```
5 V supply ─► dock PD input (USB-C)
dock USB-C plug ─► ESP32 native "USB" port
dock USB-A socket ─► printer (printer on mains)
ESP32 COM/UART port: empty      HDMI: disconnected
```
VBUS was not measured in that setup, so the exact electrical reason is
unconfirmed. Note that with the COM port empty there is no Serial Monitor;
use the web UI or OTA, or power-cycle with the COM port connected when you
need logs.

### 4. Arduino IDE settings
`Tools → USB Mode → USB-OTG (TinyUSB)` and `USB CDC On Boot → Disabled`.
With "Hardware CDC and JTAG" the chip's USB-Serial-JTAG block keeps a pull-up
on D+ and fights the host, which produces exactly this error. Use the **UART**
port for Serial Monitor.

### 5. Power quality
Use a ≥1 A supply and a short, decent cable on the UART port. WiFi start-up
can pull >300 mA in bursts; a sagging 3.3 V rail during the reset window will
fail it. The firmware now starts USB only after WiFi is up and waits 1.5 s more.

## Reading the new log output
```
[USB] Host 初始化完成，等待印表機連接...
E (xxxx) HUB: Root port reset failed
[USB] ⚠ HUB: Root port reset failed（第 1 次），重啟 USB Host...
...
[USB] 偵測到新裝置，位址: 1
[USB] 速度: Full-Speed
[USB] ✓ 印表機已就緒
```
- Fails once then succeeds → marginal power/contact; fix VBUS/cable anyway.
- Fails 3+ times → hardware; the log prints the checklist.
- Never even prints a connect → no VBUS reaches the printer (step 1).

## Printer prints on its own / keeps re-printing

First decide **who** is sending. Watch the serial log:
- A new `[Print] ← 新列印工作，來自 <PC IP>` line appears each time → the PC is
  re-sending (stuck/retrying job in the Windows queue).
- No new job lines but the printer keeps going → the printer is holding a
  partial job in its own buffer.

### Clear it (do all three)
1. Windows: cancel every document, then in an **admin** command prompt:
   ```
   net stop spooler
   del /Q /F /S %systemroot%\System32\spool\PRINTERS\*
   net start spooler
   ```
2. Power-cycle the printer (clears its internal buffer).
3. Reset the ESP32.

### Firmware causes fixed in this version
- **Watchdog reboot loop:** a busy laser printer NAKs data; if that lasted over
  30 s the ESP32 rebooted mid-job, dropped the TCP connection, and Windows
  re-sent the whole job. The watchdog is now fed while waiting.
- **Dropped data:** the old 5 s USB abort cancelled the chunk and silently
  skipped the rest of the buffer, giving corrupt/partial pages. The driver
  now waits patiently (printers NAK while printing) and only aborts after
  `USB_STALL_ABORT_MS` (120 s) with no progress (jam / no paper / cover open).
- **Busy connections were refused**, which Windows treats as a failed job and
  retries. New connections now queue until the current job ends.
- **Printer offline:** data is no longer swallowed and reported as success;
  the connection is closed so the PC keeps the job and retries later.
- **Failed job:** the rest of the stream is discarded (so the PC doesn't
  re-send) and the printer is sent a Printer-Class SOFT_RESET to clear the
  half-received job.
- Idle connections are closed after `PRINT_IDLE_TIMEOUT_MS`.

Log lines to look for: `⏳ 印表機暫時不收資料` (printer busy, normal),
`✗ 印表機超過 120 秒沒有收資料` (physical printer problem: check paper, jam,
cover, toner).
