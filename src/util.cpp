// IntelB60 - Pearl (PRL) miner for Intel Arc GPUs (fork of IAminer)
// Copyright (C) 2026 IAminer contributors. GPL-3.0-or-later (see LICENSE).
#include "util.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>

#include "miniz.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace intelb60 {

std::string to_hex(const uint8_t* data, size_t len) {
  static const char* d = "0123456789abcdef";
  std::string s(len * 2, '0');
  for (size_t i = 0; i < len; i++) { s[2 * i] = d[data[i] >> 4]; s[2 * i + 1] = d[data[i] & 15]; }
  return s;
}

bool from_hex(const std::string& hex, std::vector<uint8_t>& out) {
  std::string h = hex;
  if (h.rfind("0x", 0) == 0 || h.rfind("0X", 0) == 0) h = h.substr(2);
  if (h.size() % 2) return false;
  out.resize(h.size() / 2);
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < out.size(); i++) {
    int a = nib(h[2 * i]), b = nib(h[2 * i + 1]);
    if (a < 0 || b < 0) return false;
    out[i] = (uint8_t)(a << 4 | b);
  }
  return true;
}

std::array<uint8_t, 32> share_bound_le(const std::vector<uint8_t>& target_be, uint64_t factor) {
  // Load the (up to) 32-byte big-endian target into eight little-endian 32-bit limbs.
  uint32_t t[8] = {0};
  size_t n = target_be.size() > 32 ? 32 : target_be.size();
  for (size_t i = 0; i < n; i++) {
    size_t byte_from_lsb = n - 1 - i;
    t[byte_from_lsb / 4] |= (uint32_t)target_be[i] << (8 * (byte_from_lsb % 4));
  }
  // Multiply by a 64-bit factor as two 32-bit halves (portable; no __int128).
  uint32_t f[2] = {(uint32_t)factor, (uint32_t)(factor >> 32)};
  uint32_t r[10] = {0};
  for (int j = 0; j < 2; j++) {
    uint64_t carry = 0;
    for (int i = 0; i < 8; i++) {
      uint64_t cur = (uint64_t)t[i] * f[j] + r[i + j] + carry;
      r[i + j] = (uint32_t)cur;
      carry = cur >> 32;
    }
    for (int i = 8 + j; carry && i < 10; i++) {
      uint64_t cur = (uint64_t)r[i] + carry;
      r[i] = (uint32_t)cur;
      carry = cur >> 32;
    }
  }
  std::array<uint8_t, 32> out{};
  if (r[8] || r[9]) { out.fill(0xff); return out; }  // saturate at 2^256-1
  for (int i = 0; i < 32; i++) out[i] = (uint8_t)(r[i / 4] >> (8 * (i % 4)));
  return out;
}

double work_per_share(const std::vector<uint8_t>& target_be) {
  double t = 0;
  for (uint8_t b : target_be) t = t * 256.0 + b;
  if (t <= 0) return 0;
  return std::ldexp(1.0, 8 * (int)target_be.size()) / t;
}

std::vector<uint8_t> gzip_compress(const std::vector<uint8_t>& data) {
  mz_ulong bound = mz_deflateBound(nullptr, (mz_ulong)data.size());
  std::vector<uint8_t> raw(bound);
  // Raw deflate stream (no zlib wrapper), wrapped in a gzip header/trailer below.
  mz_stream s{};
  if (mz_deflateInit2(&s, 9, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS, 9, MZ_DEFAULT_STRATEGY) != MZ_OK) return {};
  s.next_in = data.data();
  s.avail_in = (unsigned int)data.size();
  s.next_out = raw.data();
  s.avail_out = (unsigned int)raw.size();
  int rc = mz_deflate(&s, MZ_FINISH);
  size_t raw_len = s.total_out;
  mz_deflateEnd(&s);
  if (rc != MZ_STREAM_END) return {};
  std::vector<uint8_t> out = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 2, 255};  // magic, deflate, no flags, OS unknown
  out.insert(out.end(), raw.begin(), raw.begin() + raw_len);
  uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, data.data(), data.size());
  uint32_t isize = (uint32_t)data.size();
  for (int i = 0; i < 4; i++) out.push_back((uint8_t)(crc >> (8 * i)));
  for (int i = 0; i < 4; i++) out.push_back((uint8_t)(isize >> (8 * i)));
  return out;
}

std::string base64_encode(const std::vector<uint8_t>& d) {
  static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  o.reserve((d.size() + 2) / 3 * 4);
  for (size_t i = 0; i < d.size(); i += 3) {
    uint32_t x = d[i] << 16 | (i + 1 < d.size() ? d[i + 1] << 8 : 0) | (i + 2 < d.size() ? d[i + 2] : 0);
    o += T[(x >> 18) & 63];
    o += T[(x >> 12) & 63];
    o += (i + 1 < d.size()) ? T[(x >> 6) & 63] : '=';
    o += (i + 2 < d.size()) ? T[x & 63] : '=';
  }
  return o;
}

std::vector<uint8_t> base64_decode(const std::string& in) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::vector<uint8_t> out;
  out.reserve(in.size() * 3 / 4);
  uint32_t buf = 0;
  int bits = 0;
  for (char c : in) {
    int v = val(c);
    if (v < 0) continue;
    buf = buf << 6 | (uint32_t)v;
    bits += 6;
    if (bits >= 8) { bits -= 8; out.push_back((uint8_t)(buf >> bits)); }
  }
  return out;
}

std::string format_rate(double r) {
  const char* units[] = {"H/s", "KH/s", "MH/s", "GH/s", "TH/s", "PH/s", "EH/s"};
  int u = 0;
  while (r >= 1000.0 && u < 6) { r /= 1000.0; u++; }
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.2f %s", r, units[u]);
  return buf;
}

std::string format_duration(double seconds) {
  long s = (long)seconds;
  long d = s / 86400; s %= 86400;
  char buf[64];
  if (d) std::snprintf(buf, sizeof buf, "%ldd %02ld:%02ld:%02ld", d, s / 3600, s / 60 % 60, s % 60);
  else std::snprintf(buf, sizeof buf, "%02ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
  return buf;
}

std::string hostname() {
  char buf[256] = {0};
#ifdef _WIN32
  DWORD n = sizeof buf;
  if (!GetComputerNameA(buf, &n)) return "rig";
#else
  if (gethostname(buf, sizeof buf - 1) != 0) return "rig";
#endif
  std::string h;
  for (char c : std::string(buf)) if (std::isalnum((unsigned char)c) || c == '-' || c == '_') h += c;
  return h.empty() ? "rig" : h;
}

// ---- console ----
namespace {
std::mutex g_log_mutex;
bool g_color = true;
std::ofstream g_log_file;
}  // namespace

void console_init(bool color) {
  g_color = color;
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode))
    SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
  else
    g_color = false;
#else
  if (!isatty(1)) g_color = false;
#endif
}

std::string colored(Color c, const std::string& s) {
  if (!g_color || c == Color::none) return s;
  const char* code = "";
  switch (c) {
    case Color::gray: code = "\x1b[90m"; break;
    case Color::red: code = "\x1b[91m"; break;
    case Color::green: code = "\x1b[92m"; break;
    case Color::yellow: code = "\x1b[93m"; break;
    case Color::blue: code = "\x1b[94m"; break;
    case Color::magenta: code = "\x1b[95m"; break;
    case Color::cyan: code = "\x1b[96m"; break;
    case Color::white: code = "\x1b[97m"; break;
    case Color::bold: code = "\x1b[1m"; break;
    default: break;
  }
  return std::string(code) + s + "\x1b[0m";
}

std::string timestamp() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
  return buf;
}

void log_open_file(const std::string& path) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  g_log_file.open(path, std::ios::app);
}

void log_line(Color c, const std::string& msg) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  std::string ts = timestamp();
  std::cout << colored(Color::gray, ts) << " " << colored(c, msg) << "\n" << std::flush;
  if (g_log_file.is_open()) g_log_file << ts << " " << msg << "\n" << std::flush;
}

}  // namespace intelb60
