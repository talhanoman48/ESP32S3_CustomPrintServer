// 主機端測試：g++ -std=gnu++17 -I.. ipp_test.cpp -o ipp_test && ./ipp_test
#include "../ipp_session.h"
#include <stdio.h>
#include <map>
#include <set>
#include <functional>
using namespace ipp;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, msg) do { if (cond) { g_pass++; } else { g_fail++; printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } } while (0)

// ---- Mock 網路客戶端：把輸入切成不規則小片段，模擬 TCP 到達 ----
struct MockClient {
  std::vector<uint8_t> in; size_t pos = 0; size_t piece;
  std::vector<uint8_t> out; bool stopped = false;
  explicit MockClient(std::vector<uint8_t> data, size_t pieceSize = 7) : in(std::move(data)), piece(pieceSize) {}
  bool connected() { return pos < in.size(); }
  int available() { return (int)std::min(piece, in.size() - pos); }
  int read(uint8_t* b, size_t n) { size_t k = std::min(n, std::min(piece, in.size() - pos)); memcpy(b, in.data() + pos, k); pos += k; return (int)k; }
  size_t write(const uint8_t* b, size_t n) { out.insert(out.end(), b, b + n); return n; }
  void stop() { stopped = true; }
};

// ---- Mock 後端 ----
struct MockBackend : Backend {
  uint32_t t = 100000;
  JobBeginResult beginResult = JB_OK; int state = 3;
  std::vector<uint8_t> received; int begins = 0, ends = 0; bool lastOk = false; bool failWrite = false;
  uint32_t nowMs() override { return t += 1; }
  void pump() override {}
  std::string hostIp() override { return "192.168.1.50"; }
  std::string printerName() override { return "HP_4003dn_WiFi"; }
  std::string makeModel() override { return "HP LaserJet Pro 4003dn"; }
  std::string deviceId() override { return "MFG:HP;MDL:HP LaserJet Pro 4003dn;CMD:PJL,PCL,PDF;"; }
  std::string uuid() override { return "urn:uuid:00000000-0000-1000-8000-AABBCCDDEEFF"; }
  int printerState() override { return state; }
  JobBeginResult jobBegin(uint32_t) override { if (beginResult == JB_OK) begins++; return beginResult; }
  bool jobWrite(const uint8_t* d, size_t n) override { if (failWrite) return false; received.insert(received.end(), d, d + n); return true; }
  void jobEnd(bool ok) override { ends++; lastOk = ok; }
};

// ---- 建立 IPP 請求 ----
static std::vector<uint8_t> ippReq(uint16_t op, uint32_t id, std::function<void(Writer&)> extra = nullptr, bool opGroup = true) {
  Writer w; w.begin(2, 0, op, id);
  w.group(TAG_OPERATION);
  w.str(VT_CHARSET, "attributes-charset", "utf-8");
  w.str(VT_LANG, "attributes-natural-language", "en");
  w.str(VT_URI, "printer-uri", "ipp://192.168.1.50:631/ipp/print");
  if (extra) extra(w);
  (void)opGroup;
  w.end();
  return w.b;
}
static std::string httpHead(const char* m, size_t len, bool chunked = false, bool expect = false) {
  std::string s = std::string(m) + " /ipp/print HTTP/1.1\r\nHost: hp4003dn.local:631\r\nContent-Type: application/ipp\r\n";
  if (chunked) s += "Transfer-Encoding: chunked\r\n"; else s += "Content-Length: " + std::to_string(len) + "\r\n";
  if (expect) s += "Expect: 100-continue\r\n";
  return s + "\r\n";
}
static std::vector<uint8_t> asBytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }
static std::vector<uint8_t> cat(std::vector<uint8_t> a, const std::vector<uint8_t>& b) { a.insert(a.end(), b.begin(), b.end()); return a; }
static std::vector<uint8_t> chunkify(const std::vector<uint8_t>& d, size_t csz) {
  std::vector<uint8_t> o; size_t off = 0;
  while (off < d.size()) { size_t k = std::min(csz, d.size() - off); char h[32]; snprintf(h, sizeof h, "%zx\r\n", k);
    auto hs = asBytes(h); o.insert(o.end(), hs.begin(), hs.end()); o.insert(o.end(), d.begin() + off, d.begin() + off + k); o.push_back('\r'); o.push_back('\n'); off += k; }
  auto e = asBytes("0\r\n\r\n"); o.insert(o.end(), e.begin(), e.end()); return o;
}

// ---- 獨立的 IPP 回應解析器（驗證編碼是否合法）----
struct Parsed { int http = 0; std::string ctype; bool ippOk = false; uint16_t status = 0xFFFF; uint32_t reqId = 0;
  std::multimap<std::string, std::string> attrs; std::vector<std::string> order; int jobGroups = 0; bool has100 = false; std::string bodyText; };
static Parsed parseResp(const std::vector<uint8_t>& out) {
  Parsed p; std::string s(out.begin(), out.end());
  if (s.rfind("HTTP/1.1 100 Continue\r\n\r\n", 0) == 0) { p.has100 = true; s.erase(0, 25); }
  size_t he = s.find("\r\n\r\n"); if (he == std::string::npos) return p;
  p.http = atoi(s.c_str() + 9);
  size_t ct = s.find("Content-Type: "); if (ct != std::string::npos) p.ctype = s.substr(ct + 14, s.find("\r\n", ct) - ct - 14);
  size_t cl = s.find("Content-Length: "); size_t clen = cl != std::string::npos ? (size_t)atol(s.c_str() + cl + 16) : 0;
  std::string body = s.substr(he + 4);
  if (body.size() != clen) { printf("  content-length mismatch %zu vs %zu\n", body.size(), clen); return p; }
  if (p.ctype != "application/ipp") { p.bodyText = body; return p; }
  const uint8_t* b = (const uint8_t*)body.data(); size_t n = body.size(), i = 8;
  if (n < 8) return p;
  p.status = (b[2] << 8) | b[3]; p.reqId = ((uint32_t)b[4] << 24) | (b[5] << 16) | (b[6] << 8) | b[7];
  std::string cur;
  while (i < n) {
    uint8_t tag = b[i++];
    if (tag == 0x03) { p.ippOk = (i == n); return p; }
    if (tag <= 0x0F) { if (tag == 0x02) p.jobGroups++; continue; }
    if (i + 2 > n) return p; size_t nl = (b[i] << 8) | b[i + 1]; i += 2;
    if (i + nl + 2 > n) return p; if (nl) { cur.assign((const char*)b + i, nl); } i += nl;
    size_t vl = (b[i] << 8) | b[i + 1]; i += 2; if (i + vl > n) return p;
    std::string v;
    if (tag == VT_INTEGER || tag == VT_ENUM) { uint32_t x = ((uint32_t)b[i] << 24) | (b[i+1] << 16) | (b[i+2] << 8) | b[i+3]; v = std::to_string((int32_t)x); }
    else if (tag == VT_BOOLEAN) v = b[i] ? "true" : "false";
    else if (tag == VT_RESOLUTION) { if (vl != 9) return p; v = "res"; }
    else if (tag == VT_RANGE) { if (vl != 8) return p; v = "range"; }
    else v.assign((const char*)b + i, vl);
    if (nl) p.order.push_back(cur);
    p.attrs.insert({cur, v}); i += vl;
  }
  return p;  // end tag 未出現 → ippOk=false
}
static bool has(const Parsed& p, const char* k) { return p.attrs.count(k) > 0; }
static std::string get(const Parsed& p, const char* k) { auto it = p.attrs.find(k); return it == p.attrs.end() ? "" : it->second; }

static Parsed run(MockBackend& be, State& st, std::vector<uint8_t> wire, size_t piece = 7, MockClient** keep = nullptr) {
  static MockClient* last = nullptr; delete last; last = new MockClient(std::move(wire), piece);
  Session<MockClient> s(*last, be, st); s.run(); if (keep) *keep = last;
  return parseResp(last->out);
}

int main() {
  State st; MockBackend be;

  printf("1. GET /\n");
  { auto p = run(be, st, asBytes("GET / HTTP/1.1\r\nHost: x\r\n\r\n")); CHECK(p.http == 200, "GET 200"); CHECK(p.bodyText.find("ipp://") != std::string::npos, "info page mentions ipp uri"); }

  printf("2. Get-Printer-Attributes (all)\n");
  { auto req = ippReq(OP_GET_PRINTER_ATTRS, 77); auto p = run(be, st, cat(asBytes(httpHead("POST", req.size())), req));
    CHECK(p.http == 200 && p.ctype == "application/ipp", "http 200 ipp"); CHECK(p.ippOk, "response parses cleanly to end tag");
    CHECK(p.status == ST_OK, "status ok"); CHECK(p.reqId == 77, "request-id echoed");
    for (const char* k : {"printer-uri-supported","printer-name","printer-state","printer-state-reasons","ipp-versions-supported","operations-supported",
                          "document-format-supported","printer-is-accepting-jobs","charset-configured","natural-language-configured","media-supported",
                          "printer-resolution-supported","copies-supported","printer-uuid","printer-device-id","pdf-versions-supported","ipp-features-supported"})
      CHECK(has(p, k), k);
    CHECK(get(p, "printer-uri-supported") == "ipp://hp4003dn.local:631/ipp/print", "uri uses Host header");
    CHECK(get(p, "document-format-supported") == "application/pdf", "advertises pdf");
    CHECK(p.attrs.count("operations-supported") == 8, "8 operations");
    CHECK(p.order.size() > 5 && p.order[0] == "attributes-charset" && p.order[1] == "attributes-natural-language", "op-attrs first");
    printf("   response attrs: %zu\n", p.order.size()); }

  printf("3. Get-Printer-Attributes filtered\n");
  { auto req = ippReq(OP_GET_PRINTER_ATTRS, 5, [](Writer& w){ w.strs(VT_KEYWORD, "requested-attributes", {"printer-name", "printer-state"}); });
    auto p = run(be, st, cat(asBytes(httpHead("POST", req.size())), req));
    CHECK(p.ippOk, "parses"); CHECK(has(p, "printer-name") && has(p, "printer-state"), "requested present"); CHECK(!has(p, "media-supported"), "others filtered"); }

  printf("4. Print-Job, chunked + Expect:100-continue, 200KB\n");
  { std::vector<uint8_t> doc = asBytes("%PDF-1.7\n"); for (int i = 0; i < 200000; i++) doc.push_back((uint8_t)(i * 31 + 7)); auto e = asBytes("\n%%EOF\n"); doc.insert(doc.end(), e.begin(), e.end());
    auto req = ippReq(OP_PRINT_JOB, 9, [](Writer& w){ w.str(VT_MIME, "document-format", "application/pdf"); w.str(VT_NAME, "job-name", "test.pdf"); w.str(VT_NAME, "requesting-user-name", "tester"); });
    auto wire = cat(asBytes(httpHead("POST", 0, true, true)), chunkify(cat(req, doc), 1500));
    be.received.clear(); be.begins = be.ends = 0;
    auto p = run(be, st, wire, 13);
    CHECK(p.has100, "sent 100-continue"); CHECK(p.http == 200 && p.ippOk && p.status == ST_OK, "success");
    CHECK(be.received == doc, "document forwarded byte-for-byte"); CHECK(be.begins == 1 && be.ends == 1 && be.lastOk, "begin/end called once, ok");
    CHECK(get(p, "job-id") == "1", "job-id 1"); CHECK(get(p, "job-state") == "9", "completed"); CHECK(get(p, "job-name") == "test.pdf", "job-name"); }

  printf("5. Print-Job, Content-Length, bigger pieces\n");
  { std::vector<uint8_t> doc = asBytes("%PDF-1.4 small"); auto req = ippReq(OP_PRINT_JOB, 10); auto body = cat(req, doc);
    be.received.clear(); auto p = run(be, st, cat(asBytes(httpHead("POST", body.size())), body), 1000);
    CHECK(p.status == ST_OK && be.received == doc, "ok and exact"); CHECK(get(p, "job-id") == "2", "job-id 2"); }

  printf("6. Unsupported format\n");
  { auto req = ippReq(OP_PRINT_JOB, 11, [](Writer& w){ w.str(VT_MIME, "document-format", "image/png"); }); int b0 = be.begins;
    auto p = run(be, st, cat(asBytes(httpHead("POST", req.size())), req)); CHECK(p.status == ST_FORMAT_UNSUPPORTED, "0x040A"); CHECK(be.begins == b0, "no job started"); }

  printf("7. Offline / busy\n");
  { auto req = ippReq(OP_PRINT_JOB, 12); be.beginResult = JB_OFFLINE; auto p = run(be, st, cat(asBytes(httpHead("POST", req.size())), req)); CHECK(p.status == ST_SERVICE_UNAVAILABLE, "offline 0x0502");
    be.beginResult = JB_BUSY; p = run(be, st, cat(asBytes(httpHead("POST", req.size())), req)); CHECK(p.status == ST_BUSY, "busy 0x0507"); be.beginResult = JB_OK; }

  printf("8. Create-Job + Send-Document\n");
  { auto req = ippReq(OP_CREATE_JOB, 20, [](Writer& w){ w.str(VT_NAME, "job-name", "two-step"); }); auto p = run(be, st, cat(asBytes(httpHead("POST", req.size())), req));
    CHECK(p.status == ST_OK, "create ok"); int jid = atoi(get(p, "job-id").c_str()); CHECK(jid >= 3, "got job id"); CHECK(get(p, "job-state") == "3", "pending");
    std::vector<uint8_t> doc = asBytes("%PDF-1.7 two-step data");
    auto sreq = ippReq(OP_SEND_DOCUMENT, 21, [&](Writer& w){ w.i32(VT_INTEGER, "job-id", jid); w.boolean("last-document", true); w.str(VT_MIME, "document-format", "application/pdf"); });
    auto sb = cat(sreq, doc); be.received.clear(); p = run(be, st, cat(asBytes(httpHead("POST", sb.size())), sb));
    CHECK(p.status == ST_OK && be.received == doc, "send-document forwarded"); CHECK(get(p, "job-state") == "9", "completed");
    auto bad = ippReq(OP_SEND_DOCUMENT, 22, [](Writer& w){ w.i32(VT_INTEGER, "job-id", 9999); }); p = run(be, st, cat(asBytes(httpHead("POST", bad.size())), bad)); CHECK(p.status == ST_NOT_FOUND, "unknown job 0x0406"); }

  printf("9. Get-Job-Attributes / Get-Jobs / Cancel\n");
  { auto req = ippReq(OP_GET_JOB_ATTRS, 30, [](Writer& w){ w.i32(VT_INTEGER, "job-id", 1); }); auto p = run(be, st, cat(asBytes(httpHead("POST", req.size())), req));
    CHECK(p.status == ST_OK && get(p, "job-state") == "9" && has(p, "job-state-reasons"), "job 1 completed");
    req = ippReq(OP_GET_JOB_ATTRS, 31, [](Writer& w){ w.i32(VT_INTEGER, "job-id", 4242); }); p = run(be, st, cat(asBytes(httpHead("POST", req.size())), req)); CHECK(p.status == ST_NOT_FOUND, "missing job");
    req = ippReq(OP_GET_JOBS, 32, [](Writer& w){ w.str(VT_KEYWORD, "which-jobs", "completed"); }); p = run(be, st, cat(asBytes(httpHead("POST", req.size())), req)); CHECK(p.ippOk && p.jobGroups >= 3, "completed jobs listed");
    req = ippReq(OP_GET_JOBS, 33); p = run(be, st, cat(asBytes(httpHead("POST", req.size())), req)); CHECK(p.ippOk && p.jobGroups == 0, "not-completed empty");
    req = ippReq(OP_CANCEL_JOB, 34, [](Writer& w){ w.i32(VT_INTEGER, "job-id", 1); }); p = run(be, st, cat(asBytes(httpHead("POST", req.size())), req)); CHECK(p.status == ST_NOT_POSSIBLE, "cancel completed → not possible"); }

  printf("10. Unknown operation / bad version\n");
  { auto req = ippReq(0x0099, 40); auto p = run(be, st, cat(asBytes(httpHead("POST", req.size())), req)); CHECK(p.status == ST_OP_UNSUPPORTED, "0x0501");
    Writer w; w.begin(3, 0, OP_GET_PRINTER_ATTRS, 41); w.group(TAG_OPERATION); w.end(); p = run(be, st, cat(asBytes(httpHead("POST", w.b.size())), w.b)); CHECK(p.status == ST_VERSION_UNSUPPORTED, "version 3 rejected"); }

  printf("11. Truncated upload / write failure / garbage\n");
  { std::vector<uint8_t> doc(5000, 'A'); auto req = ippReq(OP_PRINT_JOB, 50); auto body = cat(req, doc); auto wire = cat(asBytes(httpHead("POST", body.size())), body);
    wire.resize(wire.size() - 2000); be.received.clear(); be.ends = 0; auto p = run(be, st, wire);
    CHECK(be.ends == 1 && !be.lastOk, "truncated upload → jobEnd(false) (releases lock)");
    be.failWrite = true; auto req2 = ippReq(OP_PRINT_JOB, 51); auto b2 = cat(req2, doc); p = run(be, st, cat(asBytes(httpHead("POST", b2.size())), b2)); be.failWrite = false;
    CHECK(p.status == ST_INTERNAL_ERROR && !be.lastOk, "USB write failure → 0x0500, jobEnd(false)");
    p = run(be, st, asBytes("\x16\x03\x01garbage\r\n\r\n")); CHECK(p.http == 400, "TLS/garbage → 400");
    std::string junk = httpHead("POST", 12) + "NOTANIPPBODY!"; p = run(be, st, asBytes(junk)); CHECK(p.status == ST_BAD_REQUEST || p.http == 400, "junk body rejected"); }

  printf("12. Job history capped at 8\n");
  { for (int i = 0; i < 12; i++) { auto req = ippReq(OP_PRINT_JOB, 60 + i); run(be, st, cat(asBytes(httpHead("POST", req.size() + 4)), cat(req, asBytes("%PDF")))); } CHECK(st.jobs.size() == 8, "ring of 8"); }

  printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
