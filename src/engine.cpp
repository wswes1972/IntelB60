// IntelB60 - Pearl (PRL) miner for Intel Arc GPUs (fork of IAminer)
// Copyright (C) 2026 IAminer contributors. GPL-3.0-or-later (see LICENSE).
//
// The only SYCL translation unit. It compiles the vendored PearlHash engine (MoneroOcean mo-miner,
// GPL-3.0) for Intel GPUs -- ESIMD DPAS search on Level Zero -- and exposes it through engine.hpp.

#define PEARLHASH_STANDALONE   // engine without mo-miner's Node.js addon glue
#define PEARLHASH_ESIMD        // Intel register-resident DPAS search path
#define IAMINER_ENGINE         // skip mo-miner's standalone test harness, parallel proof builder.
                               // NOTE: this name is a deliberate keep - the vendored engine files in
                               // engine/pearlhash/ key on it (#ifdef IAMINER_ENGINE). Not branding.
#include "../engine/pearlhash/pearlhash.cpp"

// Unambiguous aliases for engine helpers whose names IntelB60's own API reuses.
static void ph_derive_key(const uint8_t* header76, int k, int rank, uint8_t key32[32]) {
  derive_key(header76, k, rank, key32);
}

#include "engine.hpp"

#include <stdexcept>

namespace intelb60 {

namespace {

std::vector<sycl::device> level_zero_gpus() {
  std::vector<sycl::device> out;
  for (const auto& platform : sycl::platform::get_platforms()) {
    if (platform.get_backend() != sycl::backend::ext_oneapi_level_zero) continue;
    for (const auto& dev : platform.get_devices(sycl::info::device_type::gpu)) out.push_back(dev);
  }
  return out;
}

bool is_integrated(const sycl::device& dev) {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  return dev.get_info<sycl::info::device::host_unified_memory>();
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
}

}  // namespace

std::vector<GpuInfo> list_gpus() {
  std::vector<GpuInfo> out;
  int index = 0;
  for (const auto& dev : level_zero_gpus()) {
    GpuInfo g;
    g.index = index++;
    g.name = dev.get_info<sycl::info::device::name>();
    g.memory = dev.get_info<sycl::info::device::global_mem_size>();
    g.integrated = is_integrated(dev);
    out.push_back(g);
  }
  return out;
}

struct Engine::Impl {
  sycl::queue queue;
  Shape shape;
  Buffers b{};
  std::string name;
  uint8_t key[32] = {0};
  uint8_t header[76] = {0};
  bool have_header = false;

  Impl(const sycl::device& dev, const Shape& s)
      : queue(dev, sycl::property::queue::in_order{}), shape(s) {
    name = dev.get_info<sycl::info::device::name>();
    b = alloc_buffers(queue, shape.m, shape.n, shape.k, shape.rank);
    if (!b.Ap || !b.Bp || !b.result) throw std::runtime_error("GPU buffer allocation failed");
  }
  ~Impl() {
    try {
      queue.wait();
      for (void* p : {(void*)b.EAL, (void*)b.EBR, (void*)b.EBRt, (void*)b.Ap, (void*)b.Bp,
                      (void*)b.EARp1, (void*)b.EARp2, (void*)b.EBLq1, (void*)b.EBLq2,
                      (void*)b.cA, (void*)b.cB, (void*)b.key, (void*)b.target,
                      (void*)b.CVA, (void*)b.CVB, (void*)b.transcript, (void*)b.result})
        if (p) sycl::free(p, queue);
    } catch (...) {
    }
  }
};

Engine::Engine(int gpu_index, const Shape& shape) {
  auto gpus = level_zero_gpus();
  if (gpu_index < 0 || gpu_index >= (int)gpus.size())
    throw std::runtime_error("GPU " + std::to_string(gpu_index) + " not found");
  if ((int64_t)shape.m * shape.k >= (1LL << 31) || (int64_t)shape.n * shape.k >= (1LL << 31))
    throw std::runtime_error("shape too large: m*k and n*k must be below 2^31");
  impl_ = std::make_unique<Impl>(gpus[gpu_index], shape);
}

Engine::~Engine() = default;

const std::string& Engine::device_name() const { return impl_->name; }
const Shape& Engine::shape() const { return impl_->shape; }
const uint8_t* Engine::job_key() const { return impl_->key; }

void Engine::derive_key(const uint8_t header76[76], const Shape& shape, uint8_t key[32]) {
  ph_derive_key(header76, shape.k, shape.rank, key);
}

void Engine::set_job(const uint8_t header76[76], const uint8_t bound_le[32]) {
  Impl& s = *impl_;
  if (!s.have_header || std::memcmp(s.header, header76, 76) != 0) {
    std::memcpy(s.header, header76, 76);
    derive_key(header76, s.shape, s.key);
    s.queue.memcpy(s.b.key, s.key, 32);
    s.have_header = true;
  }
  s.queue.memcpy(s.b.target, bound_le, 32);
  s.queue.wait();
}

bool Engine::attempt(uint32_t seed, Found& out) {
  Impl& s = *impl_;
  s.b.result->found = 0;
  s.b.result->chk = 0;
  ::attempt(s.queue, s.b, seed, s.shape.m, s.shape.n, s.shape.k, s.shape.rank);
  s.queue.wait_and_throw();
  if (!s.b.result->found) return false;
  out.seed = s.b.result->seed;
  out.row = (int)s.b.result->row;
  out.col = (int)s.b.result->col;
  return true;
}

std::string Engine::build_proof(const Shape& shape, const uint8_t key[32], const Found& f) {
  return build_plain_proof(f.seed, shape.m, shape.n, shape.k, shape.rank, key, f.row, f.col);
}

}  // namespace intelb60
