#ifndef IPP_SESSION_H
#define IPP_SESSION_H

// ============================================================
//  極簡 IPP/1.1 + 2.0 伺服器（IPP Everywhere 子集），單檔、無 Arduino 相依。
//  用途：讓 Android（預設列印服務 / Mopria）與 Windows（IPP Class Driver）
//        能自動發現並列印到這台 ESP32，文件（PDF）原封不動轉送給 USB 印表機。
//  結構：HTTP 解析（含 chunked / Expect: 100-continue）→ IPP 解析 → 處理 → 回應
//  Client 型別需提供：connected() available() read(buf,n) write(buf,n) stop()
//  （WiFiClient 符合；測試用 MockClient 也符合）
// ============================================================

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>
#include <initializer_list>

namespace ipp {

// ---- 常數 ----
enum : uint8_t {
  TAG_OPERATION = 0x01, TAG_JOB = 0x02, TAG_END = 0x03, TAG_PRINTER = 0x04,
  VT_INTEGER = 0x21, VT_BOOLEAN = 0x22, VT_ENUM = 0x23,
  VT_RESOLUTION = 0x32, VT_RANGE = 0x33,
  VT_TEXT = 0x41, VT_NAME = 0x42, VT_KEYWORD = 0x44, VT_URI = 0x45,
  VT_CHARSET = 0x47, VT_LANG = 0x48, VT_MIME = 0x49
};

enum : uint16_t {
  OP_PRINT_JOB = 0x0002, OP_VALIDATE_JOB = 0x0004, OP_CREATE_JOB = 0x0005,
  OP_SEND_DOCUMENT = 0x0006, OP_CANCEL_JOB = 0x0008, OP_GET_JOB_ATTRS = 0x0009,
  OP_GET_JOBS = 0x000A, OP_GET_PRINTER_ATTRS = 0x000B
};

enum : uint16_t {
  ST_OK = 0x0000,
  ST_BAD_REQUEST = 0x0400, ST_NOT_POSSIBLE = 0x0404, ST_NOT_FOUND = 0x0406,
  ST_FORMAT_UNSUPPORTED = 0x040A,
  ST_INTERNAL_ERROR = 0x0500, ST_OP_UNSUPPORTED = 0x0501,
  ST_SERVICE_UNAVAILABLE = 0x0502, ST_VERSION_UNSUPPORTED = 0x0503,
  ST_BUSY = 0x0507
};

// 後端介面：ESP32 實作會接 USB；測試用 Mock。
enum JobBeginResult { JB_OK = 0, JB_BUSY = 1, JB_OFFLINE = 2 };

class Backend {
public:
  virtual ~Backend() {}
  virtual uint32_t nowMs() = 0;
  virtual void pump() = 0;                       // 等待時讓出 CPU / 餵看門狗
  virtual std::string hostIp() = 0;
  virtual std::string printerName() = 0;
  virtual std::string makeModel() = 0;
  virtual std::string deviceId() = 0;
  virtual std::string uuid() = 0;
  virtual int printerState() = 0;                // 3 idle, 4 processing, 5 stopped
  virtual JobBeginResult jobBegin(uint32_t waitMs) = 0;
  virtual bool jobWrite(const uint8_t* data, size_t len) = 0;
  virtual void jobEnd(bool ok) = 0;
};

struct JobRec {
  uint32_t id;
  int state;          // 3 pending, 5 processing, 8 aborted, 9 completed
  std::string name, user;
  uint32_t bytes;
  uint32_t createdSec, doneSec;
};

struct State {
  uint32_t nextJobId = 1;
  std::vector<JobRec> jobs;   // 最近 8 筆
  JobRec* find(uint32_t id) {
    for (auto& j : jobs) if (j.id == id) return &j;
    return nullptr;
  }
  JobRec& add(const std::string& name, const std::string& user, uint32_t nowSec) {
    if (jobs.size() >= 8) jobs.erase(jobs.begin());
    JobRec r; r.id = nextJobId++; r.state = 3; r.name = name; r.user = user;
    r.bytes = 0; r.createdSec = nowSec; r.doneSec = 0;
    jobs.push_back(r);
    return jobs.back();
  }
};

// ---- IPP 編碼器 ----
class Writer {
public:
  std::vector<uint8_t> b;
  const std::vector<std::string>* filter = nullptr;  // requested-attributes 過濾
  bool lastAllowed = true;

  void u8(uint8_t v) { b.push_back(v); }
  void u16(uint16_t v) { u8(v >> 8); u8(v & 0xFF); }
  void u32(uint32_t v) { u16(v >> 16); u16(v & 0xFFFF); }
  void bytes(const void* p, size_t n) { const uint8_t* q = (const uint8_t*)p; b.insert(b.end(), q, q + n); }

  void begin(uint8_t vmaj, uint8_t vmin, uint16_t status, uint32_t reqId) { u8(vmaj); u8(vmin); u16(status); u32(reqId); }
  void group(uint8_t tag) { u8(tag); }
  void end() { u8(TAG_END); }

  bool allow(const char* name) {
    if (!filter || filter->empty()) return true;
    for (const auto& f : *filter) if (f == name) return true;
    return false;
  }
  void attr(uint8_t tag, const char* name, const void* v, size_t n) {
    lastAllowed = allow(name);
    if (!lastAllowed) return;
    u8(tag); u16((uint16_t)strlen(name)); bytes(name, strlen(name)); u16((uint16_t)n); bytes(v, n);
  }
  void more(uint8_t tag, const void* v, size_t n) {   // 同一屬性的下一個值
    if (!lastAllowed) return;
    u8(tag); u16(0); u16((uint16_t)n); bytes(v, n);
  }
  void str(uint8_t tag, const char* name, const std::string& s) { attr(tag, name, s.data(), s.size()); }
  static void be32(uint8_t* t, int32_t v) { t[0] = v >> 24; t[1] = v >> 16; t[2] = v >> 8; t[3] = v; }
  void i32(uint8_t tag, const char* name, int32_t v) { uint8_t t[4]; be32(t, v); attr(tag, name, t, 4); }
  void boolean(const char* name, bool v) { uint8_t t = v ? 1 : 0; attr(VT_BOOLEAN, name, &t, 1); }
  void strs(uint8_t tag, const char* name, std::initializer_list<const char*> vals) {
    bool first = true;
    for (const char* v : vals) {
      if (first) { attr(tag, name, v, strlen(v)); first = false; }
      else more(tag, v, strlen(v));
    }
  }
  void i32s(uint8_t tag, const char* name, std::initializer_list<int32_t> vals) {
    bool first = true;
    for (int32_t v : vals) {
      uint8_t t[4]; be32(t, v);
      if (first) { attr(tag, name, t, 4); first = false; }
      else more(tag, t, 4);
    }
  }
  void resolution(const char* name, int32_t x, int32_t y) {
    uint8_t t[9]; be32(t, x); be32(t + 4, y); t[8] = 3;  // 3 = dots per inch
    attr(VT_RESOLUTION, name, t, 9);
  }
  void range(const char* name, int32_t lo, int32_t hi) {
    uint8_t t[8]; be32(t, lo); be32(t + 4, hi); attr(VT_RANGE, name, t, 8);
  }
};

// ---- 解析後的請求（只保留我們需要的欄位）----
struct Request {
  uint8_t vmaj = 1, vmin = 1;
  uint16_t op = 0;
  uint32_t id = 0;
  std::string printerUri, docFormat, jobName, userName, which;
  int32_t jobId = 0;
  bool hasJobId = false;
  bool lastDoc = true;
  std::vector<std::string> requested;
};

static inline std::string lower(std::string s) {
  for (auto& c : s) if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
  return s;
}

// ============================================================
//  Session：處理一個連線上的一個 HTTP 請求
// ============================================================
template <class C>
class Session {
public:
  Session(C& client, Backend& be, State& st) : c(client), be(be), st(st) {}

  void run() {
    Http h;
    if (!readHttpHeader(h)) { sendHttp(400, "Bad Request", "text/plain", "bad request\n"); return; }

    if (h.method == "GET" || h.method == "HEAD") {
      std::string body =
        "<html><head><title>" + be.printerName() + "</title></head><body><h1>" + be.printerName() +
        "</h1><p>IPP printer endpoint. Print URI: <code>ipp://" + hostOf(h) + ":631/ipp/print</code></p></body></html>\n";
      sendHttp(200, "OK", "text/html", h.method == "HEAD" ? std::string() : body, h.method == "HEAD" ? body.size() : 0);
      return;
    }
    if (h.method == "OPTIONS") { sendHttp(200, "OK", "text/plain", ""); return; }
    if (h.method != "POST") { sendHttp(405, "Method Not Allowed", "text/plain", "method not allowed\n"); return; }

    body.chunked = h.chunked;
    body.hasLen = h.contentLength >= 0;
    body.remain = h.contentLength;

    if (h.expect100 && (h.chunked || h.contentLength != 0)) {
      writeAll((const uint8_t*)"HTTP/1.1 100 Continue\r\n\r\n", 25);
    }

    Request q;
    if (!parseRequest(q)) { respondStatus(q, ST_BAD_REQUEST, "malformed IPP request", h); return; }
    dispatch(q, h);
  }

private:
  struct Http {
    std::string method, path, host;
    long contentLength = -1;
    bool chunked = false, expect100 = false;
  };
  struct Body { bool chunked = false, hasLen = false, done = false, needCrlf = false; long remain = 0; size_t chunkLeft = 0; };

  C& c;
  Backend& be;
  State& st;
  uint8_t rb[1024];
  size_t rbPos = 0, rbLen = 0;
  Body body;

  static constexpr uint32_t kReadTimeoutMs = 20000;

  // ---------- 低階讀寫 ----------
  bool fill() {
    if (rbPos < rbLen) return true;
    rbPos = rbLen = 0;
    uint32_t t0 = be.nowMs();
    for (;;) {
      int a = c.available();
      if (a > 0) {
        int r = c.read(rb, (size_t)a > sizeof(rb) ? sizeof(rb) : (size_t)a);
        if (r > 0) { rbLen = (size_t)r; return true; }
      } else if (!c.connected()) {
        return false;
      }
      if (be.nowMs() - t0 > kReadTimeoutMs) return false;
      be.pump();
    }
  }
  int getByte() { if (!fill()) return -1; return rb[rbPos++]; }
  int readRaw(uint8_t* out, size_t n) {
    if (!fill()) return -1;
    size_t k = std::min(n, rbLen - rbPos);
    memcpy(out, rb + rbPos, k);
    rbPos += k;
    return (int)k;
  }
  bool readLine(std::string& out, size_t maxLen) {
    out.clear();
    for (;;) {
      int ch = getByte();
      if (ch < 0) return false;
      if (ch == '\n') { if (!out.empty() && out.back() == '\r') out.pop_back(); return true; }
      out.push_back((char)ch);
      if (out.size() > maxLen) return false;
    }
  }
  bool writeAll(const uint8_t* p, size_t n) {
    size_t off = 0;
    uint32_t t0 = be.nowMs();
    while (off < n) {
      size_t k = std::min<size_t>(1024, n - off);
      size_t w = c.write(p + off, k);
      if (w > 0) { off += w; t0 = be.nowMs(); continue; }
      if (!c.connected() || be.nowMs() - t0 > kReadTimeoutMs) return false;
      be.pump();
    }
    return true;
  }

  // ---------- HTTP ----------
  bool readHttpHeader(Http& h) {
    std::string line;
    if (!readLine(line, 2048)) return false;
    size_t s1 = line.find(' ');
    size_t s2 = (s1 == std::string::npos) ? s1 : line.find(' ', s1 + 1);
    if (s1 == std::string::npos || s2 == std::string::npos) return false;
    h.method = line.substr(0, s1);
    h.path = line.substr(s1 + 1, s2 - s1 - 1);
    for (int i = 0; i < 64; i++) {
      if (!readLine(line, 2048)) return false;
      if (line.empty()) return true;
      size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      std::string k = lower(line.substr(0, colon));
      std::string v = line.substr(colon + 1);
      while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0, 1);
      if (k == "content-length") h.contentLength = atol(v.c_str());
      else if (k == "transfer-encoding") h.chunked = (lower(v).find("chunked") != std::string::npos);
      else if (k == "expect") h.expect100 = (lower(v).find("100-continue") != std::string::npos);
      else if (k == "host") h.host = v;
    }
    return false;  // header 太多
  }

  std::string hostOf(const Http& h) {
    std::string host = h.host;
    size_t colon = host.rfind(':');
    if (!host.empty() && host[0] != '[' && colon != std::string::npos) host = host.substr(0, colon);
    if (host.empty()) host = be.hostIp();
    return host;
  }

  void sendHttp(int code, const char* reason, const char* ctype, const std::string& bodyStr, size_t lenOverride = 0) {
    sendHttpRaw(code, reason, ctype, (const uint8_t*)bodyStr.data(), bodyStr.size(), lenOverride);
  }
  void sendHttpRaw(int code, const char* reason, const char* ctype, const uint8_t* p, size_t n, size_t lenOverride = 0) {
    char hdr[256];
    int hl = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 %d %s\r\nServer: ESP32-IPP/1.0\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
                      code, reason, ctype, (unsigned)(lenOverride ? lenOverride : n));
    writeAll((const uint8_t*)hdr, (size_t)hl);
    if (n) writeAll(p, n);
  }

  // ---------- 本文讀取（處理 chunked / Content-Length）----------
  int bodyRead(uint8_t* out, size_t n) {
    if (body.done) return 0;
    if (body.chunked) {
      if (body.chunkLeft == 0) {
        std::string l;
        if (body.needCrlf) { if (!readLine(l, 16)) return -1; body.needCrlf = false; }
        if (!readLine(l, 64)) return -1;
        size_t sz = (size_t)strtoul(l.c_str(), nullptr, 16);
        if (sz == 0) {
          for (;;) { std::string t; if (!readLine(t, 1024)) break; if (t.empty()) break; }
          body.done = true;
          return 0;
        }
        body.chunkLeft = sz;
        body.needCrlf = true;
      }
      int r = readRaw(out, std::min(n, body.chunkLeft));
      if (r <= 0) return -1;
      body.chunkLeft -= (size_t)r;
      return r;
    }
    if (body.hasLen) {
      if (body.remain <= 0) { body.done = true; return 0; }
      int r = readRaw(out, std::min<size_t>(n, (size_t)body.remain));
      if (r <= 0) return -1;
      body.remain -= r;
      return r;
    }
    int r = readRaw(out, n);
    if (r <= 0) { body.done = true; return 0; }
    return r;
  }
  bool bodyExact(uint8_t* out, size_t n) {
    size_t got = 0;
    while (got < n) {
      int r = bodyRead(out + got, n - got);
      if (r <= 0) return false;
      got += (size_t)r;
    }
    return true;
  }
  bool bodySkip(size_t n) {
    uint8_t tmp[64];
    while (n > 0) {
      size_t k = std::min(n, sizeof(tmp));
      if (!bodyExact(tmp, k)) return false;
      n -= k;
    }
    return true;
  }

  // ---------- IPP 請求解析（串流；結束於 end-of-attributes）----------
  bool parseRequest(Request& q) {
    uint8_t hd[8];
    if (!bodyExact(hd, 8)) return false;
    q.vmaj = hd[0]; q.vmin = hd[1];
    q.op = (uint16_t)((hd[2] << 8) | hd[3]);
    q.id = ((uint32_t)hd[4] << 24) | ((uint32_t)hd[5] << 16) | ((uint32_t)hd[6] << 8) | hd[7];

    std::string curName;
    for (int guard = 0; guard < 4000; guard++) {
      uint8_t tag;
      if (!bodyExact(&tag, 1)) return false;
      if (tag == TAG_END) return true;
      if (tag <= 0x0F) continue;  // 其他群組分隔符

      uint8_t l2[2];
      if (!bodyExact(l2, 2)) return false;
      size_t nameLen = (size_t)((l2[0] << 8) | l2[1]);
      if (nameLen > 0) {
        if (nameLen > 255) { if (!bodySkip(nameLen)) return false; curName.clear(); }
        else {
          char nb[256];
          if (!bodyExact((uint8_t*)nb, nameLen)) return false;
          curName.assign(nb, nameLen);
        }
      }
      if (!bodyExact(l2, 2)) return false;
      size_t valLen = (size_t)((l2[0] << 8) | l2[1]);
      uint8_t vb[256];
      size_t keep = std::min(valLen, sizeof(vb));
      if (keep && !bodyExact(vb, keep)) return false;
      if (valLen > keep && !bodySkip(valLen - keep)) return false;

      std::string sv((char*)vb, keep);
      if (curName == "printer-uri") q.printerUri = sv;
      else if (curName == "document-format") q.docFormat = lower(sv);
      else if (curName == "job-name") q.jobName = sv;
      else if (curName == "requesting-user-name") q.userName = sv;
      else if (curName == "which-jobs") q.which = sv;
      else if (curName == "requested-attributes") q.requested.push_back(sv);
      else if (curName == "job-id" && keep == 4) {
        q.jobId = (int32_t)(((uint32_t)vb[0] << 24) | ((uint32_t)vb[1] << 16) | ((uint32_t)vb[2] << 8) | vb[3]);
        q.hasJobId = true;
      } else if (curName == "job-uri") {
        size_t sl = sv.rfind('/');
        if (sl != std::string::npos) { q.jobId = atoi(sv.c_str() + sl + 1); q.hasJobId = q.jobId > 0; }
      } else if (curName == "last-document" && keep == 1) q.lastDoc = vb[0] != 0;
    }
    return false;
  }

  // ---------- 回應 ----------
  void beginResponse(Writer& w, const Request& q, uint16_t status, const char* msg) {
    uint8_t vmaj = (q.vmaj == 1 || q.vmaj == 2) ? q.vmaj : 2;
    uint8_t vmin = (q.vmaj == 1 || q.vmaj == 2) ? q.vmin : 0;
    w.begin(vmaj, vmin, status, q.id);
    w.group(TAG_OPERATION);
    w.str(VT_CHARSET, "attributes-charset", "utf-8");
    w.str(VT_LANG, "attributes-natural-language", "en");
    if (msg && *msg) w.str(VT_TEXT, "status-message", msg);
  }
  void sendIpp(Writer& w) {
    w.end();
    sendHttpRaw(200, "OK", "application/ipp", w.b.data(), w.b.size());
  }
  void respondStatus(const Request& q, uint16_t status, const char* msg, const Http&) {
    Writer w;
    beginResponse(w, q, status, msg);
    sendIpp(w);
  }

  std::string printerUri(const Http& h) { return "ipp://" + hostOf(h) + ":631/ipp/print"; }
  std::string jobUri(const Http& h, uint32_t id) { return "ipp://" + hostOf(h) + ":631/ipp/jobs/" + std::to_string(id); }

  // ---------- 派送 ----------
  void dispatch(const Request& q, const Http& h) {
    if (q.vmaj > 2) { respondStatus(q, ST_VERSION_UNSUPPORTED, "IPP version not supported", h); return; }
    switch (q.op) {
      case OP_GET_PRINTER_ATTRS: opGetPrinterAttrs(q, h); break;
      case OP_VALIDATE_JOB: respondStatus(q, ST_OK, "", h); break;
      case OP_PRINT_JOB: opPrint(q, h, true); break;
      case OP_CREATE_JOB: opCreateJob(q, h); break;
      case OP_SEND_DOCUMENT: opPrint(q, h, false); break;
      case OP_GET_JOB_ATTRS: opGetJobAttrs(q, h); break;
      case OP_GET_JOBS: opGetJobs(q, h); break;
      case OP_CANCEL_JOB: opCancel(q, h); break;
      default: respondStatus(q, ST_OP_UNSUPPORTED, "operation not supported", h); break;
    }
  }

  void jobAttrs(Writer& w, const Http& h, const JobRec& j) {
    w.group(TAG_JOB);
    w.i32(VT_INTEGER, "job-id", (int32_t)j.id);
    w.str(VT_URI, "job-uri", jobUri(h, j.id));
    w.str(VT_URI, "job-printer-uri", printerUri(h));
    w.i32(VT_ENUM, "job-state", j.state);
    w.strs(VT_KEYWORD, "job-state-reasons",
           {j.state == 9 ? "job-completed-successfully" : (j.state == 8 ? "job-canceled-by-system" : "none")});
    w.str(VT_NAME, "job-name", j.name.empty() ? "Untitled" : j.name);
    w.str(VT_NAME, "job-originating-user-name", j.user.empty() ? "anonymous" : j.user);
    w.i32(VT_INTEGER, "time-at-creation", (int32_t)j.createdSec);
    w.i32(VT_INTEGER, "time-at-processing", (int32_t)j.createdSec);
    if (j.doneSec) w.i32(VT_INTEGER, "time-at-completed", (int32_t)j.doneSec);
    w.i32(VT_INTEGER, "job-k-octets", (int32_t)((j.bytes + 1023) / 1024));
  }

  void opGetPrinterAttrs(const Request& q, const Http& h) {
    Writer w;
    // requested-attributes 若是群組名或 all，等於不過濾
    std::vector<std::string> f;
    bool all = q.requested.empty();
    for (const auto& r : q.requested)
      if (r == "all" || r == "printer-description" || r == "job-template" || r == "job-description") all = true;
    if (!all) f = q.requested;
    w.filter = f.empty() ? nullptr : &f;

    w.filter = nullptr;  // operation-attributes 不過濾
    beginResponse(w, q, ST_OK, nullptr);
    w.filter = f.empty() ? nullptr : &f;

    std::string uri = printerUri(h);
    std::string model = be.makeModel();
    std::string name = be.printerName();
    int state = be.printerState();

    w.group(TAG_PRINTER);
    w.str(VT_URI, "printer-uri-supported", uri);
    w.strs(VT_KEYWORD, "uri-security-supported", {"none"});
    w.strs(VT_KEYWORD, "uri-authentication-supported", {"none"});
    w.str(VT_NAME, "printer-name", name);
    w.str(VT_TEXT, "printer-info", name);
    w.str(VT_TEXT, "printer-location", "");
    w.str(VT_TEXT, "printer-make-and-model", model);
    w.i32(VT_ENUM, "printer-state", state);
    w.strs(VT_KEYWORD, "printer-state-reasons", {"none"});
    w.str(VT_TEXT, "printer-state-message", "");
    w.strs(VT_KEYWORD, "ipp-versions-supported", {"1.0", "1.1", "2.0"});
    w.strs(VT_KEYWORD, "ipp-features-supported", {"ipp-everywhere"});
    w.i32s(VT_ENUM, "operations-supported", {0x02, 0x04, 0x05, 0x06, 0x08, 0x09, 0x0A, 0x0B});
    w.str(VT_CHARSET, "charset-configured", "utf-8");
    w.strs(VT_CHARSET, "charset-supported", {"utf-8"});
    w.str(VT_LANG, "natural-language-configured", "en");
    w.strs(VT_LANG, "generated-natural-language-supported", {"en"});
    w.str(VT_MIME, "document-format-default", "application/pdf");
    w.strs(VT_MIME, "document-format-supported", {"application/pdf"});
    w.strs(VT_KEYWORD, "pdf-versions-supported", {"adobe-1.4", "adobe-1.5", "adobe-1.6", "adobe-1.7"});
    w.boolean("printer-is-accepting-jobs", true);
    w.i32(VT_INTEGER, "queued-job-count", state == 4 ? 1 : 0);
    w.strs(VT_KEYWORD, "pdl-override-supported", {"not-attempted"});
    w.i32(VT_INTEGER, "printer-up-time", (int32_t)(be.nowMs() / 1000) + 1);
    w.strs(VT_KEYWORD, "compression-supported", {"none"});
    w.boolean("multiple-document-jobs-supported", false);
    w.i32(VT_INTEGER, "multiple-operation-time-out", 60);
    w.strs(VT_KEYWORD, "job-creation-attributes-supported", {"copies", "job-name"});
    w.strs(VT_KEYWORD, "which-jobs-supported", {"completed", "not-completed"});
    w.boolean("color-supported", false);
    w.strs(VT_KEYWORD, "print-color-mode-supported", {"monochrome"});
    w.str(VT_KEYWORD, "print-color-mode-default", "monochrome");
    w.strs(VT_KEYWORD, "sides-supported", {"one-sided"});
    w.str(VT_KEYWORD, "sides-default", "one-sided");
    w.range("copies-supported", 1, 1);
    w.i32(VT_INTEGER, "copies-default", 1);
    w.i32s(VT_ENUM, "orientation-requested-supported", {3, 4, 5, 6});
    w.i32(VT_ENUM, "orientation-requested-default", 3);
    w.i32s(VT_ENUM, "print-quality-supported", {3, 4, 5});
    w.i32(VT_ENUM, "print-quality-default", 4);
    w.resolution("printer-resolution-supported", 600, 600);
    w.resolution("printer-resolution-default", 600, 600);
    w.i32s(VT_ENUM, "finishings-supported", {3});
    w.i32(VT_ENUM, "finishings-default", 3);
    w.boolean("page-ranges-supported", false);
    w.strs(VT_KEYWORD, "media-supported",
           {"iso_a4_210x297mm", "na_letter_8.5x11in", "na_legal_8.5x14in", "iso_a5_148x210mm"});
    w.str(VT_KEYWORD, "media-default", "iso_a4_210x297mm");
    w.strs(VT_KEYWORD, "media-ready", {"iso_a4_210x297mm"});
    w.str(VT_TEXT, "printer-device-id", be.deviceId());
    w.str(VT_URI, "printer-uuid", be.uuid());
    w.str(VT_URI, "printer-more-info", "http://" + hostOf(h) + "/");
    sendIpp(w);
  }

  // Print-Job / Send-Document 共用
  void opPrint(const Request& q, const Http& h, bool isPrintJob) {
    JobRec* rec = nullptr;
    if (!isPrintJob) {
      rec = q.hasJobId ? st.find((uint32_t)q.jobId) : nullptr;
      if (!rec) { respondStatus(q, ST_NOT_FOUND, "job not found", h); return; }
    }

    // 文件格式：只收 PDF 與「自動偵測」的原始資料
    std::string fmt = q.docFormat;
    bool fmtOk = fmt.empty() || fmt == "application/pdf" || fmt == "application/octet-stream" ||
                 fmt == "application/postscript" || fmt == "application/vnd.hp-pcl";
    if (!fmtOk) { respondStatus(q, ST_FORMAT_UNSUPPORTED, "only application/pdf is supported", h); return; }

    JobBeginResult jb = be.jobBegin(3000);
    if (jb == JB_OFFLINE) { respondStatus(q, ST_SERVICE_UNAVAILABLE, "printer offline", h); return; }
    if (jb == JB_BUSY) { respondStatus(q, ST_BUSY, "printer busy", h); return; }

    if (isPrintJob) rec = &st.add(q.jobName, q.userName, be.nowMs() / 1000);
    rec->state = 5;
    uint32_t jobId = rec->id;

    std::vector<uint8_t> buf(4096);
    uint32_t total = 0;
    bool ok = true;
    for (;;) {
      int r = bodyRead(buf.data(), buf.size());
      if (r < 0) { ok = false; break; }
      if (r == 0) break;
      if (!be.jobWrite(buf.data(), (size_t)r)) { ok = false; break; }
      total += (uint32_t)r;
    }
    be.jobEnd(ok);

    rec = st.find(jobId);
    if (rec) { rec->bytes += total; rec->state = ok ? 9 : 8; rec->doneSec = be.nowMs() / 1000; }

    Writer w;
    beginResponse(w, q, ok ? ST_OK : ST_INTERNAL_ERROR, ok ? nullptr : "document transfer failed");
    if (rec) jobAttrs(w, h, *rec);
    sendIpp(w);
  }

  void opCreateJob(const Request& q, const Http& h) {
    JobRec& r = st.add(q.jobName, q.userName, be.nowMs() / 1000);
    Writer w;
    beginResponse(w, q, ST_OK, nullptr);
    jobAttrs(w, h, r);
    sendIpp(w);
  }

  void opGetJobAttrs(const Request& q, const Http& h) {
    JobRec* r = q.hasJobId ? st.find((uint32_t)q.jobId) : nullptr;
    if (!r) { respondStatus(q, ST_NOT_FOUND, "job not found", h); return; }
    Writer w;
    beginResponse(w, q, ST_OK, nullptr);
    jobAttrs(w, h, *r);
    sendIpp(w);
  }

  void opGetJobs(const Request& q, const Http& h) {
    Writer w;
    beginResponse(w, q, ST_OK, nullptr);
    // 工作是同步處理的，所以「未完成」永遠是空的；只回傳已完成/中止的
    if (q.which == "completed") {
      for (const auto& j : st.jobs) if (j.state == 9 || j.state == 8) jobAttrs(w, h, j);
    }
    sendIpp(w);
  }

  void opCancel(const Request& q, const Http& h) {
    JobRec* r = q.hasJobId ? st.find((uint32_t)q.jobId) : nullptr;
    if (!r) { respondStatus(q, ST_NOT_FOUND, "job not found", h); return; }
    respondStatus(q, ST_NOT_POSSIBLE, "job already processed", h);
  }
};

}  // namespace ipp

#endif  // IPP_SESSION_H
