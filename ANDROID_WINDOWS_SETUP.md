# Adding the printer on Android and Windows

The ESP32 now offers **two** ways to print:

| Port | Protocol | Used by |
|---|---|---|
| **631** | IPP (`ipp://<ip>:631/ipp/print`), advertised over mDNS | **Android**, Windows "Add printer", Chromebooks, Linux/CUPS |
| **9100** | Raw socket | **Windows with the HP driver** (most reliable), CUPS, scripts |

The ESP32 does not render anything. For IPP it forwards the **PDF** that Android
produces straight to the printer, which prints PDF natively (HP LaserJet Pro
4003dn languages: PCL 6, PCL 5e, PS3, PDF, URF, PWG Raster).

## Android (9+)
1. Phone/tablet on the **same Wi-Fi** as the ESP32 (guest/"client isolation" networks block discovery).
2. Settings → Connected devices (or "Connection & sharing") → Printing → **Default Print Service** → turn **On**.
3. The printer appears as `HP_LaserJet_4003dn_WiFi` within a few seconds.
4. If it does not appear: ⋮ menu → **Add printer by IP address** → enter the ESP32's IP
   (or `ipp://hp4003dn.local:631/ipp/print`).
5. Open any document/photo → Share/Print → choose the printer.

## Windows 10/11
**Option A, recommended: HP driver over raw port 9100** (the setup that is known to work)
1. Install the *HP Universal Print Driver (PCL6)* or the 4003dn driver from HP.
2. Add printer → Standard TCP/IP port → host = ESP32 IP → **RAW**, port **9100**.
3. Disable **SNMP status** on that port (the ESP32 does not answer SNMP).

**Option B: IPP (no driver)**
1. Settings → Bluetooth & devices → Printers & scanners → Add device. The printer may appear
   automatically (mDNS). If not: *Add manually* → *Select a shared printer by name* →
   `http://<ESP32-IP>:631/ipp/print`.
2. Windows uses the built-in *IPP Class Driver*. This path is best-effort: the ESP32 only
   advertises PDF, and I could not verify which document format the Windows class
   driver prefers. If the job disappears or prints nothing, use Option A.

## Check this BEFORE blaming IPP: does the printer take PDF over USB?
The IPP path assumes the printer detects raw PDF sent over USB. Verify it directly through the raw port
(PowerShell; replace the path and host):
```powershell
$c = New-Object Net.Sockets.TcpClient("hp4003dn.local", 9100)
$s = $c.GetStream(); $b = [IO.File]::ReadAllBytes("C:\test.pdf")
$s.Write($b, 0, $b.Length); $s.Close(); $c.Close()
```
- Page prints → IPP forwarding will work as well.
- Nothing prints → set `IPP_PJL_LANGUAGE "PDF"` in `config.h` (adds `@PJL ENTER LANGUAGE = PDF`), re-flash, retry.
- Still nothing → set `IPP_UEL_FRAMING 0`, or tell me what the serial log shows.

## Troubleshooting
| Symptom | Likely cause / fix |
|---|---|
| Android never lists the printer | Different Wi-Fi / client isolation. Use *Add printer by IP*. Check serial for `[mDNS] IPP:` and `[IPP] ✓ 已啟動`. |
| "Printer unavailable" / offline | Printer not enumerated over USB yet (`[USB] ✓ 印表機已就緒` missing) or powered off. IPP answers `0x0502`. |
| "Printer busy" | A raw-9100 job is running. IPP and raw share one lock; retry in a moment. |
| Job "completes" but nothing prints | See the PDF-over-USB test above. |
| Garbage / PCL text printed | The printer did not recognise PDF: set `IPP_PJL_LANGUAGE "PDF"`. |
| Only 1 copy / simplex | Intentional: copies and duplex are not forwarded (see limits). Set duplex in the printer's own menu. |

## Limits of this implementation
- Document format: **PDF only** (also accepts raw PCL/PS passthrough via `application/octet-stream`).
  Android uses PDF when the printer advertises it, which is why this works.
- `copies`, `sides` (duplex), `media` and `orientation` job options are **not applied** by the ESP32.
  Android bakes page size and orientation into the PDF; the printer uses its own defaults for the rest.
- No TLS (`ipp://`, not `ipps://`), no authentication, no iOS/AirPrint subtype registration.
- One IPP request is handled at a time; a second client waits.

## Easiest alternative: skip the ESP32
The HP LaserJet Pro 4003dn has built-in **Gigabit Ethernet** and supports AirPrint, Mopria and IPP itself.
If you can connect its Ethernet port to your router (or a cheap Wi-Fi bridge / travel router in client
mode), Windows and Android will find it natively with full duplex/copies/tray support. The ESP32 bridge is
only worth it when Ethernet is not an option.
