// IntelB60 - Pearl (PRL) miner for Intel Arc GPUs (fork of IAminer)
// Copyright (C) 2026 IAminer contributors. GPL-3.0-or-later (see LICENSE).
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace intelb60 {

// ---- hex / bytes ----
std::string to_hex(const uint8_t* data, size_t len);
bool from_hex(const std::string& hex, std::vector<uint8_t>& out);

// ---- 256-bit share bound ----
// Pearl pools send the base target T0 (big-endian). The verifier accepts a tile when
// LE-uint256(jackpot) <= T0 * (tile_h * tile_w) * dot_product_length (rank-penalty neutral for
// rank = 128). Returns the bound little-endian for the GPU kernel, saturating at 2^256-1.
std::array<uint8_t, 32> share_bound_le(const std::vector<uint8_t>& target_be, uint64_t factor);
// Expected MACs of work per share for a base target: 2^256 / T0.
double work_per_share(const std::vector<uint8_t>& target_be);

// ---- gzip (RFC 1952) ----
std::vector<uint8_t> gzip_compress(const std::vector<uint8_t>& data);
std::vector<uint8_t> base64_decode(const std::string& in);
std::string base64_encode(const std::vector<uint8_t>& data);

// ---- formatting ----
std::string format_rate(double macs_per_sec);     // "70.12 TH/s"
std::string format_duration(double seconds);      // "1d 02:03:04"
std::string hostname();

// ---- console / logging ----
enum class Color { none, gray, red, green, yellow, blue, magenta, cyan, white, bold };
void console_init(bool color);
void log_line(Color c, const std::string& msg);          // timestamped line
void log_open_file(const std::string& path);
std::string colored(Color c, const std::string& s);
std::string timestamp();

using Clock = std::chrono::steady_clock;

}  // namespace intelb60
