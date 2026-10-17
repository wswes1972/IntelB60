// IntelB60 - Pearl (PRL) miner for Intel Arc GPUs (fork of IAminer)
// Copyright (C) 2026 IAminer contributors. GPL-3.0-or-later (see LICENSE).
//
// Thin C++ interface over the vendored PearlHash GPU engine (engine/pearlhash, derived from
// MoneroOcean mo-miner). This header has no SYCL dependency so the rest of IntelB60 compiles as plain
// C++; only engine.cpp is built with -fsycl.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace intelb60 {

struct GpuInfo {
  int index = 0;             // IntelB60 device index (stable order: Level Zero enumeration)
  std::string name;
  uint64_t memory = 0;       // bytes
  bool integrated = false;
};

// NoisyGEMM problem shape. m = rows of A, n = cols of B, k = common dimension, rank = noise rank.
struct Shape {
  int m = 131072;
  int n = 131072;
  int k = 8192;
  int rank = 128;            // ranks above 128 are penalised by Pearl consensus
  uint64_t macs() const { return (uint64_t)m * (uint64_t)n * (uint64_t)k; }
};

struct Found {
  uint32_t seed = 0;
  int row = 0;
  int col = 0;
};

std::vector<GpuInfo> list_gpus();

class Engine {
 public:
  Engine(int gpu_index, const Shape& shape);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  const std::string& device_name() const;
  const Shape& shape() const;

  // Select work: 76-byte Pearl block header and the 256-bit jackpot bound (little-endian).
  void set_job(const uint8_t header76[76], const uint8_t bound_le[32]);
  // Run one full seeded NoisyGEMM attempt (m*n*k MACs). Returns true and fills `out` on a winning tile.
  bool attempt(uint32_t seed, Found& out);
  // Job key of the header currently set (needed to rebuild the proof on the host).
  const uint8_t* job_key() const;

  // Pool-ready PlainProof (base64 bincode) for a winning tile. Host-only; safe to call from another
  // thread while the GPU keeps searching. `key` is job_key() captured when the tile was found.
  static std::string build_proof(const Shape& shape, const uint8_t key[32], const Found& f);
  static void derive_key(const uint8_t header76[76], const Shape& shape, uint8_t key[32]);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace intelb60
