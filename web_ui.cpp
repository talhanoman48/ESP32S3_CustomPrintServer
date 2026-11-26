#include "web_ui.h"

WebUI::WebUI(WiFiManager& wifi, UsbPrinter& printer, PrintServer& printSvr)
  : _server(WEB_SERVER_PORT), _wifi(wifi), _printer(printer), _printSvr(printSvr) {}

void WebUI::begin() {
  _server.on("/", HTTP_GET, [this]() { _handleRoot(); });
  _server.on("/api/status", HTTP_GET, [this]() { _handleStatus(); });
  _server.on("/api/wifi/scan", HTTP_GET, [this]() { _handleWifiScan(); });
  _server.on("/api/wifi/connect", HTTP_POST, [this]() { _handleWifiConnect(); });
  _server.on("/api/restart", HTTP_POST, [this]() { _handleRestart(); });
  _server.onNotFound([this]() { _handleNotFound(); });

  _server.begin();
  Serial.printf("[Web] ✓ 管理介面已啟動，埠: %d\n", WEB_SERVER_PORT);
}

void WebUI::task() {
  _server.handleClient();
}

void WebUI::_handleRoot() {
  _server.send(200, "text/html", _generatePage());
}

void WebUI::_handleStatus() {
  PrintJob job = _printSvr.getCurrentJob();

  String json = "{";
  json += "\"wifi\":{";
  json += "\"connected\":" + String(_wifi.isConnected() ? "true" : "false") + ",";
  json += "\"ssid\":\"" + _wifi.getSSID() + "\",";
  json += "\"ip\":\"" + _wifi.getIP() + "\",";
  json += "\"rssi\":" + String(_wifi.getRSSI()) + ",";
  json += "\"mode\":\"" + String(_wifi.getMode() == WSTATE_STATION ? "station" : "ap") + "\"";
  json += "},";

  json += "\"printer\":{";
  json += "\"connected\":" + String(_printer.isConnected() ? "true" : "false") + ",";
  json += "\"ready\":" + String(_printer.isReady() ? "true" : "false") + ",";
  json += "\"manufacturer\":\"" + _printer.getManufacturer() + "\",";
  json += "\"product\":\"" + _printer.getProduct() + "\",";
  json += "\"state\":\"" + String((int)_printer.getState()) + "\"";
  json += "},";

  json += "\"job\":{";
  json += "\"active\":" + String(job.active ? "true" : "false") + ",";
  json += "\"bytesReceived\":" + String(job.bytesReceived) + ",";
  json += "\"bytesSent\":" + String(job.bytesSent) + ",";
  json += "\"client\":\"" + job.clientIP + "\"";
  json += "},";

  json += "\"stats\":{";
  json += "\"totalJobs\":" + String(_printSvr.getTotalJobs()) + ",";
  json += "\"totalBytes\":" + String(_printSvr.getTotalBytes()) + ",";
  json += "\"uptime\":" + String(millis() / 1000) + ",";
  json += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"freePsram\":" + String(ESP.getFreePsram());
  json += "}";

  json += "}";

  _server.send(200, "application/json", json);
}

void WebUI::_handleWifiScan() {
  String networks = _wifi.scanNetworks();
  _server.send(200, "application/json", networks);
}

void WebUI::_handleWifiConnect() {
  String ssid = _server.arg("ssid");
  String pass = _server.arg("password");

  if (ssid.length() == 0) {
    _server.send(400, "application/json", "{\"error\":\"SSID is required\"}");
    return;
  }

  bool ok = _wifi.setCredentials(ssid, pass);
  String json = "{\"success\":" + String(ok ? "true" : "false");
  json += ",\"ip\":\"" + _wifi.getIP() + "\"}";
  _server.send(200, "application/json", json);
}

void WebUI::_handleRestart() {
  _server.send(200, "application/json", "{\"message\":\"Restarting...\"}");
  delay(500);
  ESP.restart();
}

void WebUI::_handleNotFound() {
  _server.send(404, "text/plain", "Not Found");
}

String WebUI::_generatePage() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-TW">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Print Server</title>
  <style>
    * { margin:0; padding:0; box-sizing:border-box; }
    body { font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
           background:#0f172a; color:#e2e8f0; min-height:100vh; }
    .container { max-width:640px; margin:0 auto; padding:20px; }

    h1 { font-size:1.5rem; font-weight:600; margin-bottom:24px;
         display:flex; align-items:center; gap:10px; }
    h1 .icon { font-size:1.8rem; }

    .card { background:#1e293b; border-radius:12px; padding:20px;
            margin-bottom:16px; border:1px solid #334155; }
    .card h2 { font-size:0.85rem; text-transform:uppercase; letter-spacing:0.05em;
               color:#94a3b8; margin-bottom:12px; }

    .status-row { display:flex; justify-content:space-between;
                  align-items:center; padding:8px 0;
                  border-bottom:1px solid #334155; }
    .status-row:last-child { border-bottom:none; }
    .status-label { color:#94a3b8; font-size:0.9rem; }
    .status-value { font-weight:500; }

    .badge { display:inline-block; padding:2px 10px; border-radius:9999px;
             font-size:0.8rem; font-weight:600; }
    .badge-green { background:#064e3b; color:#6ee7b7; }
    .badge-red { background:#7f1d1d; color:#fca5a5; }
    .badge-yellow { background:#713f12; color:#fde68a; }

    .wifi-form { margin-top:12px; }
    .wifi-form select, .wifi-form input {
      width:100%; padding:10px 12px; margin-bottom:10px;
      background:#0f172a; border:1px solid #475569; border-radius:8px;
      color:#e2e8f0; font-size:0.95rem; }
    .wifi-form select:focus, .wifi-form input:focus {
      outline:none; border-color:#3b82f6; }

    .btn { display:inline-block; padding:10px 20px; border:none;
           border-radius:8px; font-size:0.9rem; font-weight:600;
           cursor:pointer; transition:all 0.2s; }
    .btn-primary { background:#3b82f6; color:white; width:100%; }
    .btn-primary:hover { background:#2563eb; }
    .btn-danger { background:#991b1b; color:#fca5a5; }
    .btn-danger:hover { background:#7f1d1d; }
    .btn-sm { padding:6px 14px; font-size:0.8rem; }

    .btn-row { display:flex; gap:10px; margin-top:16px; }

    .progress-bar { height:6px; background:#334155; border-radius:3px;
                    overflow:hidden; margin-top:8px; }
    .progress-fill { height:100%; background:#3b82f6; border-radius:3px;
                     transition:width 0.3s; }

    .footer { text-align:center; color:#475569; font-size:0.8rem;
              margin-top:24px; }

    @keyframes pulse { 0%,100% { opacity:1; } 50% { opacity:0.5; } }
    .printing { animation:pulse 1.5s infinite; }
  </style>
</head>
<body>
  <div class="container">
    <h1><span class="icon">&#x1F5A8;</span> WiFi Print Server</h1>

    <!-- 印表機狀態 -->
    <div class="card">
      <h2>Printer</h2>
      <div class="status-row">
        <span class="status-label">Status</span>
        <span id="prn-status" class="badge badge-red">Disconnected</span>
      </div>
      <div class="status-row">
        <span class="status-label">Model</span>
        <span id="prn-model" class="status-value">—</span>
      </div>
      <div id="job-section" style="display:none;">
        <div class="status-row">
          <span class="status-label">Print Job</span>
          <span id="job-info" class="status-value printing">—</span>
        </div>
        <div class="progress-bar">
          <div id="job-progress" class="progress-fill" style="width:0%"></div>
        </div>
      </div>
    </div>

    <!-- WiFi 狀態 -->
    <div class="card">
      <h2>WiFi</h2>
      <div class="status-row">
        <span class="status-label">Status</span>
        <span id="wifi-status" class="badge badge-red">Disconnected</span>
      </div>
      <div class="status-row">
        <span class="status-label">Network</span>
        <span id="wifi-ssid" class="status-value">—</span>
      </div>
      <div class="status-row">
        <span class="status-label">IP Address</span>
        <span id="wifi-ip" class="status-value">—</span>
      </div>
      <div class="status-row">
        <span class="status-label">Signal</span>
        <span id="wifi-rssi" class="status-value">—</span>
      </div>

      <div class="wifi-form">
        <select id="wifi-select"><option value="">-- Scan to find networks --</option></select>
        <input type="password" id="wifi-pass" placeholder="Password">
        <div class="btn-row">
          <button class="btn btn-primary" style="flex:1" onclick="scanWifi()">Scan</button>
          <button class="btn btn-primary" style="flex:1" onclick="connectWifi()">Connect</button>
        </div>
      </div>
    </div>

    <!-- 系統資訊 -->
    <div class="card">
      <h2>System</h2>
      <div class="status-row">
        <span class="status-label">Uptime</span>
        <span id="sys-uptime" class="status-value">—</span>
      </div>
      <div class="status-row">
        <span class="status-label">Free RAM</span>
        <span id="sys-heap" class="status-value">—</span>
      </div>
      <div class="status-row">
        <span class="status-label">Free PSRAM</span>
        <span id="sys-psram" class="status-value">—</span>
      </div>
      <div class="status-row">
        <span class="status-label">Total Jobs</span>
        <span id="sys-jobs" class="status-value">0</span>
      </div>
      <div class="btn-row">
        <button class="btn btn-danger btn-sm" onclick="restart()">Restart Device</button>
      </div>
    </div>

    <!-- Mac 設定指引 -->
    <div class="card">
      <h2>Mac Setup Guide</h2>
      <div style="color:#94a3b8; font-size:0.9rem; line-height:1.6;">
        <p>1. Open <b>System Settings</b> &rarr; <b>Printers & Scanners</b></p>
        <p>2. Click <b>Add Printer</b></p>
        <p>3. Select <b>IP</b> tab</p>
        <p>4. Protocol: <b>HP Jetdirect - Socket</b></p>
        <p>5. Address: <b><span id="setup-addr">—</span></b></p>
        <p>6. Queue: leave empty</p>
        <p>7. Name: <b>Brother HL-1110 WiFi</b></p>
        <p>8. Use/Driver: <b>Select Software</b> &rarr; search <b>Brother HL-1110</b></p>
        <p style="margin-top:8px; color:#fde68a;">If "Brother HL-1110" is not listed, download the driver from Brother's website first.</p>
      </div>
    </div>

    <div class="footer">
      ESP32-S3 WiFi Print Server v1.0
    </div>
  </div>

  <script>
    function updateStatus() {
      fetch('/api/status')
        .then(r => r.json())
        .then(d => {
          // Printer
          const ps = document.getElementById('prn-status');
          if (d.printer.ready) {
            ps.textContent = 'Ready';
            ps.className = 'badge badge-green';
          } else if (d.printer.connected) {
            ps.textContent = 'Connected';
            ps.className = 'badge badge-yellow';
          } else {
            ps.textContent = 'Disconnected';
            ps.className = 'badge badge-red';
          }
          document.getElementById('prn-model').textContent =
            d.printer.product || '—';

          // Job
          const js = document.getElementById('job-section');
          if (d.job.active) {
            js.style.display = 'block';
            const kb = (d.job.bytesReceived / 1024).toFixed(1);
            document.getElementById('job-info').textContent =
              kb + ' KB from ' + d.job.client;
          } else {
            js.style.display = 'none';
          }

          // WiFi
          const ws = document.getElementById('wifi-status');
          if (d.wifi.connected) {
            ws.textContent = 'Connected';
            ws.className = 'badge badge-green';
          } else {
            ws.textContent = d.wifi.mode === 'ap' ? 'AP Mode' : 'Disconnected';
            ws.className = d.wifi.mode === 'ap' ? 'badge badge-yellow' : 'badge badge-red';
          }
          document.getElementById('wifi-ssid').textContent = d.wifi.ssid || '—';
          document.getElementById('wifi-ip').textContent = d.wifi.ip;
          document.getElementById('wifi-rssi').textContent =
            d.wifi.connected ? d.wifi.rssi + ' dBm' : '—';

          // Setup address
          const addr = d.wifi.connected ? d.wifi.ip : '192.168.4.1';
          document.getElementById('setup-addr').textContent = addr;

          // System
          const up = d.stats.uptime;
          const h = Math.floor(up/3600);
          const m = Math.floor((up%3600)/60);
          const s = up % 60;
          document.getElementById('sys-uptime').textContent =
            h + 'h ' + m + 'm ' + s + 's';
          document.getElementById('sys-heap').textContent =
            (d.stats.freeHeap / 1024).toFixed(0) + ' KB';
          document.getElementById('sys-psram').textContent =
            (d.stats.freePsram / 1024).toFixed(0) + ' KB';
          document.getElementById('sys-jobs').textContent = d.stats.totalJobs;
        })
        .catch(e => console.error('Status fetch error:', e));
    }

    function scanWifi() {
      const sel = document.getElementById('wifi-select');
      sel.innerHTML = '<option value="">Scanning...</option>';
      fetch('/api/wifi/scan')
        .then(r => r.json())
        .then(nets => {
          sel.innerHTML = '<option value="">-- Select Network --</option>';
          nets.sort((a,b) => b.rssi - a.rssi);
          nets.forEach(n => {
            const opt = document.createElement('option');
            opt.value = n.ssid;
            opt.textContent = n.ssid + ' (' + n.rssi + ' dBm)' +
                              (n.secure ? ' 🔒' : '');
            sel.appendChild(opt);
          });
        });
    }

    function connectWifi() {
      const ssid = document.getElementById('wifi-select').value;
      const pass = document.getElementById('wifi-pass').value;
      if (!ssid) { alert('Please select a network'); return; }

      fetch('/api/wifi/connect', {
        method: 'POST',
        headers: {'Content-Type':'application/x-www-form-urlencoded'},
        body: 'ssid=' + encodeURIComponent(ssid) +
              '&password=' + encodeURIComponent(pass)
      })
      .then(r => r.json())
      .then(d => {
        if (d.success) {
          alert('Connected! New IP: ' + d.ip +
                '\nRedirecting in 5 seconds...');
          setTimeout(() => {
            window.location.href = 'http://' + d.ip;
          }, 5000);
        } else {
          alert('Connection failed. Check password and try again.');
        }
      });
    }

    function restart() {
      if (confirm('Restart the print server?')) {
        fetch('/api/restart', {method:'POST'});
        alert('Restarting... Page will reload in 10 seconds.');
        setTimeout(() => location.reload(), 10000);
      }
    }

    // 每 2 秒更新狀態
    updateStatus();
    setInterval(updateStatus, 2000);
  </script>
</body>
</html>
)rawliteral";

  return html;
}
