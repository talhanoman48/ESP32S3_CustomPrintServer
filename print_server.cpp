#include "print_server.h"
#include <esp_task_wdt.h>

PrintServer::PrintServer(UsbPrinter& printer)
  : _printer(printer),
    _server(RAW_SOCKET_PORT),
    _jobFailed(false), _hasLock(false), _lastDataMs(0),
    _totalJobs(0), _totalBytes(0),
    _buffer(nullptr), _bufferSize(0) {

  _currentJob = {false, 0, 0, 0, ""};
}

bool PrintServer::begin() {
  // 在 PSRAM 中配置列印緩衝區
  _bufferSize = PRINT_BUFFER_SIZE;
  _buffer = (uint8_t*)ps_malloc(_bufferSize);

  if (_buffer == nullptr) {
    // 如果 PSRAM 不可用，使用較小的 heap 緩衝區
    _bufferSize = 8192;
    _buffer = (uint8_t*)malloc(_bufferSize);
    Serial.println("[Print] ⚠ PSRAM 不可用，使用 8KB 緩衝區");
  } else {
    Serial.printf("[Print] ✓ 已在 PSRAM 配置 %dKB 緩衝區\n", _bufferSize / 1024);
  }

  if (_buffer == nullptr) {
    Serial.println("[Print] ✗ 無法配置緩衝區！");
    return false;
  }

  _server.begin();
  _server.setNoDelay(true);
  Serial.printf("[Print] ✓ Raw Socket 伺服器已啟動，埠: %d\n", RAW_SOCKET_PORT);
  return true;
}

void PrintServer::task() {
  // 只在沒有進行中的工作時才 accept。忙碌時讓新連線留在 backlog 排隊，
  // 不要拒絕：被拒絕的連線會讓 Windows 判定列印失敗並重送整份工作（造成重複列印）。
  if (!_currentJob.active && _server.hasClient()) {
    _client = _server.accept();
    if (_client) {
      _currentJob = {
        true,
        0, 0,
        millis(),
        _client.remoteIP().toString()
      };
      _jobFailed = false;
      _hasLock = false;
      _lastDataMs = millis();
      Serial.printf("[Print] ← 新列印工作，來自 %s\n", _currentJob.clientIP.c_str());
    }
  }

  if (!_currentJob.active) return;

  if (_client && _client.connected()) {
    _handleClient();

    // 連線開著卻長時間沒資料：關掉，不要卡住後面的工作
    if (_client.connected() && (millis() - _lastDataMs) > PRINT_IDLE_TIMEOUT_MS) {
      Serial.println("[Print] ⚠ 連線閒置逾時，關閉");
      _client.stop();
    }
  } else {
    // 客戶端已斷開，工作完成
    unsigned long elapsed = millis() - _currentJob.startTime;
    if (_jobFailed) {
      Serial.printf("[Print] ✗ 列印工作失敗: 收到 %d bytes，只送出 %d bytes\n",
                    _currentJob.bytesReceived, _currentJob.bytesSent);
      // 印表機收到的是不完整資料，重置它以清除半份工作
      _printer.softReset();
    } else {
      Serial.printf("[Print] ✓ 列印工作完成: %d bytes, 耗時 %lums\n",
                    _currentJob.bytesReceived, elapsed);
    }
    _totalJobs++;
    _totalBytes += _currentJob.bytesReceived;
    _currentJob.active = false;
    if (_hasLock) { _printer.releaseJob(); _hasLock = false; }
    _client.stop();
  }
}

void PrintServer::_handleClient() {
  if (!_client.available()) return;

  // 與 IPP 共用同一台印表機：先拿到工作鎖才開始讀資料。
  // 拿不到（IPP 工作進行中）就先不讀，資料留在 TCP 緩衝區，電腦端自然會等。
  if (!_hasLock && !_jobFailed) {
    if (!_printer.acquireJob(0)) return;
    _hasLock = true;
  }

  // 印表機沒就緒：不要吞掉資料還回報成功，直接中斷連線，讓電腦端保留工作、稍後重試
  if (!_jobFailed && !_printer.isReady()) {
    Serial.println("[Print] ✗ 印表機未就緒，中斷此連線");
    _jobFailed = true;
    _client.stop();
    return;
  }

  while (_client.available()) {
    esp_task_wdt_reset();  // 長工作期間不能讓看門狗重啟（重啟會斷線，Windows 就會重送整份工作）

    size_t toRead = min((size_t)_client.available(), _bufferSize);
    int bytesRead = _client.read(_buffer, toRead);
    if (bytesRead <= 0) break;

    _lastDataMs = millis();
    _currentJob.bytesReceived += bytesRead;

    // 已經失敗：把剩下的資料讀掉丟棄，避免電腦端認為失敗而整份重送
    if (_jobFailed) continue;

    int sent = _printer.sendData(_buffer, bytesRead);
    if (sent > 0) _currentJob.bytesSent += sent;

    if (sent != bytesRead) {
      _jobFailed = true;
      Serial.printf("[Print] ✗ USB 傳送失敗（送出 %d / %d），丟棄此工作剩餘資料\n", sent, bytesRead);
      continue;
    }

    // 每 100KB 報告一次進度
    if (_currentJob.bytesReceived % (100 * 1024) < (size_t)bytesRead) {
      Serial.printf("[Print] 進度: %dKB 接收 / %dKB 已送\n",
                    _currentJob.bytesReceived / 1024,
                    _currentJob.bytesSent / 1024);
    }

    yield();
  }
}
