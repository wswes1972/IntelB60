// IntelB60 - Pearl (PRL) miner for Intel Arc GPUs (fork of IAminer)
// Copyright (C) 2026 IAminer contributors. GPL-3.0-or-later (see LICENSE).
//
// Mining core: one worker thread per GPU runs back-to-back NoisyGEMM attempts on the active job; a
// single proof thread rebuilds the PlainProof for each winning tile on the CPU (so the GPUs never wait
// on it) and submits it to the pool that issued the job. The main loop reports status and schedules
// the 1% developer fee.
#include "miner.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

#include "stratum.hpp"
#include "util.hpp"
#include "version.hpp"

namespace intelb60 {

namespace {

enum class Source { none, user, fee };

struct ActiveJob {
  Source source = Source::none;
  PoolJob job;
  uint64_t generation = 0;
};

struct FoundShare {
  Source source = Source::none;
  std::shared_ptr<PoolClient> pool;
  PoolJob job;
  Shape shape;
  uint8_t key[32] = {0};
  Found found;
  int gpu = 0;
};

struct GpuStats {
  std::atomic<uint64_t> attempts{0};
  std::atomic<bool> failed{false};
  uint64_t macs_per_attempt = 0;
  uint64_t last_attempts = 0;
};

class MinerCore {
 public:
  MinerCore(const Config& cfg, std::atomic<bool>& stop) : cfg_(cfg), stop_(stop) {}
  int run();

 private:
  std::shared_ptr<PoolClient> pool_for(Source src) {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    return src == Source::fee ? fee_pool_ : src == Source::user ? user_pool_ : nullptr;
  }
  void gpu_thread(int slot, DeviceConfig dc);
  void proof_thread();
  void set_active(Source src);
  void on_pool_job(Source src);
  void report();
  void fee_tick();
  void start_fee_pool();

  const Config& cfg_;
  std::atomic<bool>& stop_;

  std::mutex pool_mutex_;
  std::shared_ptr<PoolClient> user_pool_;
  std::shared_ptr<PoolClient> fee_pool_;
  std::shared_ptr<PoolClient> fee_retiring_;   // kept briefly so in-flight fee proofs can submit

  std::mutex job_mutex_;
  std::condition_variable job_cv_;
  ActiveJob active_;
  bool fee_active_ = false;                    // guarded by job_mutex_

  std::mutex found_mutex_;
  std::condition_variable found_cv_;
  std::deque<FoundShare> found_;

  std::vector<std::unique_ptr<GpuStats>> stats_;
  std::atomic<uint64_t> accepted_{0}, rejected_{0}, stale_{0};
  std::atomic<double> work_per_share_{0};
  Clock::time_point start_ = Clock::now();
  Clock::time_point last_report_ = Clock::now();

  // Dev fee schedule (main thread only).
  enum class FeeState { idle, connecting, active } fee_state_ = FeeState::idle;
  Clock::time_point next_fee_;
  Clock::time_point fee_deadline_;
  Clock::time_point fee_retire_at_;
};

void MinerCore::set_active(Source src) {
  auto pool = pool_for(src);
  std::lock_guard<std::mutex> lock(job_mutex_);
  if (src == Source::none || !pool || !pool->has_job()) {
    active_.source = Source::none;
  } else {
    active_.source = src;
    active_.job = pool->current_job();
  }
  active_.generation++;
  job_cv_.notify_all();
}

void MinerCore::on_pool_job(Source src) {
  if (src == Source::user) {
    auto pool = pool_for(Source::user);
    if (pool) work_per_share_ = work_per_share(pool->current_job().target_be);
  }
  bool follow;
  {
    std::lock_guard<std::mutex> lock(job_mutex_);
    Source want = fee_active_ ? Source::fee : Source::user;
    follow = src == want;
  }
  if (follow) set_active(src);
}

void MinerCore::gpu_thread(int slot, DeviceConfig dc) {
  GpuStats& st = *stats_[slot];
  std::unique_ptr<Engine> engine;
  const std::string tag = "GPU" + std::to_string(dc.index);
  try {
    engine = std::make_unique<Engine>(dc.index, dc.shape);
  } catch (const std::exception& e) {
    log_line(Color::red, tag + ": failed to start: " + e.what());
    st.failed = true;
    return;
  }
  st.macs_per_attempt = dc.shape.macs();
  log_line(Color::green, tag + ": " + engine->device_name() + " ready (m=" + std::to_string(dc.shape.m) +
                             " n=" + std::to_string(dc.shape.n) + " k=" + std::to_string(dc.shape.k) +
                             " rank=" + std::to_string(dc.shape.rank) + ")");
  // Seeds: a distinct 27-bit window per GPU, randomised per run so restarts never repeat work.
  std::mt19937 rng(std::random_device{}() ^ (uint32_t)(dc.index * 0x9E3779B9u));
  uint32_t seed = ((uint32_t)dc.index << 27) | (rng() & 0x07FFFFFFu);
  // Jackpot bound = T0 * tile(16x16) * dot-product length (k - k % rank).
  const uint64_t factor = 256ULL * (uint64_t)(dc.shape.k - dc.shape.k % dc.shape.rank);

  ActiveJob work;
  uint64_t loaded = ~0ULL;
  while (!stop_) {
    {
      std::unique_lock<std::mutex> lock(job_mutex_);
      if (active_.source == Source::none) {
        loaded = ~0ULL;
        job_cv_.wait_for(lock, std::chrono::milliseconds(500));
        continue;
      }
      if (active_.generation != loaded) work = active_;
    }
    try {
      if (work.generation != loaded) {
        auto bound = share_bound_le(work.job.target_be, factor);
        engine->set_job(work.job.header.data(), bound.data());
        loaded = work.generation;
      }
      Found f;
      bool hit = engine->attempt(seed++, f);
      st.attempts++;
      if (hit) {
        FoundShare fs;
        fs.source = work.source;
        fs.pool = pool_for(work.source);
        fs.job = work.job;
        fs.shape = dc.shape;
        std::memcpy(fs.key, engine->job_key(), 32);
        fs.found = f;
        fs.gpu = dc.index;
        std::lock_guard<std::mutex> lock(found_mutex_);
        found_.push_back(std::move(fs));
        found_cv_.notify_one();
      }
    } catch (const std::exception& e) {
      log_line(Color::red, tag + ": " + e.what());
      st.failed = true;
      return;
    }
  }
}

void MinerCore::proof_thread() {
  while (!stop_) {
    FoundShare fs;
    {
      std::unique_lock<std::mutex> lock(found_mutex_);
      found_cv_.wait_for(lock, std::chrono::milliseconds(500), [&] { return !found_.empty() || stop_.load(); });
      if (found_.empty()) continue;
      fs = std::move(found_.front());
      found_.pop_front();
    }
    if (!fs.pool) continue;
    auto t0 = Clock::now();
    std::string proof = Engine::build_proof(fs.shape, fs.key, fs.found);
    int ms = (int)std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    const bool mine = fs.source == Source::user;
    // Pools reject shares for a superseded block; don't send them.
    if (fs.pool->current_job().height != fs.job.height) {
      if (mine) {
        stale_++;
        log_line(Color::yellow, "GPU" + std::to_string(fs.gpu) + ": share for block " + std::to_string(fs.job.height) +
                                    " went stale (new block), not sent");
      }
      continue;
    }
    if (!fs.pool->submit(fs.job, proof)) {
      if (mine) log_line(Color::yellow, "share not sent: pool disconnected");
      continue;
    }
    if (mine)
      log_line(Color::cyan, "GPU" + std::to_string(fs.gpu) + ": share found, submitting (proof " + std::to_string(ms) + " ms)");
  }
}

// b60MinerLinux_forkryptex: fee destination decoded at runtime (XOR) so released binaries don't leak it to `strings`.
static std::string fee_wallet() {
  static const uint8_t enc[] = INTELB60_FEE_WALLET_XOR;
  std::string out;
  for (uint8_t b : enc) out.push_back(char(b ^ INTELB60_FEE_WALLET_KEY));
  return out;
}

void MinerCore::start_fee_pool() {
  PoolConfig fc;
  fc.host = INTELB60_FEE_POOL_HOST;
  fc.port = INTELB60_FEE_POOL_PORT;
  fc.tls = INTELB60_FEE_POOL_TLS;
  fc.wallet = fee_wallet();
  fc.worker = INTELB60_FEE_WORKER;
  fc.label = "dev fee";
  PoolClient::Callbacks cb;
  cb.on_job = [this](const PoolJob&) { on_pool_job(Source::fee); };
  auto pool = std::make_shared<PoolClient>(fc, cb, cfg_.verbose);
  {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    fee_pool_ = pool;
  }
  pool->start();
}

void MinerCore::fee_tick() {
  auto now = Clock::now();
  if (fee_retiring_ && now >= fee_retire_at_) {
    std::shared_ptr<PoolClient> old;
    {
      std::lock_guard<std::mutex> lock(pool_mutex_);
      old.swap(fee_retiring_);
    }
    old.reset();  // joins its thread
  }
  switch (fee_state_) {
    case FeeState::idle:
      if (now >= next_fee_) {
        start_fee_pool();
        fee_state_ = FeeState::connecting;
        fee_deadline_ = now + std::chrono::seconds(30);
      }
      break;
    case FeeState::connecting: {
      auto pool = pool_for(Source::fee);
      if (pool && pool->connected() && pool->has_job()) {
        {
          std::lock_guard<std::mutex> lock(job_mutex_);
          fee_active_ = true;
        }
        set_active(Source::fee);
        fee_state_ = FeeState::active;
        fee_deadline_ = now + std::chrono::seconds(INTELB60_FEE_LENGTH_S);
        log_line(Color::magenta, "dev fee: mining " + std::to_string(INTELB60_FEE_LENGTH_S) + " s for b60MinerLinux_forkryptex maintenance (1%)");
      } else if (now >= fee_deadline_) {
        // Fee pool unreachable: skip this round rather than disturb the user's mining.
        std::shared_ptr<PoolClient> old;
        {
          std::lock_guard<std::mutex> lock(pool_mutex_);
          old.swap(fee_pool_);
        }
        old.reset();
        fee_state_ = FeeState::idle;
        next_fee_ = now + std::chrono::seconds(INTELB60_FEE_INTERVAL_S);
      }
      break;
    }
    case FeeState::active:
      if (now >= fee_deadline_) {
        {
          std::lock_guard<std::mutex> lock(job_mutex_);
          fee_active_ = false;
        }
        set_active(Source::user);
        {
          std::lock_guard<std::mutex> lock(pool_mutex_);
          fee_retiring_ = std::move(fee_pool_);
          fee_pool_.reset();
        }
        fee_retire_at_ = now + std::chrono::seconds(30);
        fee_state_ = FeeState::idle;
        next_fee_ = now + std::chrono::seconds(INTELB60_FEE_INTERVAL_S - INTELB60_FEE_LENGTH_S);
        log_line(Color::magenta, "dev fee: done, back to your pool");
      }
      break;
  }
}

void MinerCore::report() {
  auto now = Clock::now();
  double dt = std::chrono::duration<double>(now - last_report_).count();
  last_report_ = now;
  if (dt <= 0) return;
  double total = 0;
  std::ostringstream gpus;
  for (size_t i = 0; i < stats_.size(); i++) {
    auto& st = *stats_[i];
    uint64_t a = st.attempts.load();
    double rate = (double)(a - st.last_attempts) * (double)st.macs_per_attempt / dt;
    st.last_attempts = a;
    total += rate;
    gpus << "  #" << i << " " << (st.failed ? std::string("FAILED") : format_rate(rate));
  }
  std::string state;
  {
    std::lock_guard<std::mutex> lock(job_mutex_);
    if (active_.source == Source::fee) state = "  [dev fee]";
    else if (active_.source == Source::none) state = "  [waiting for pool]";
  }
  double up = std::chrono::duration<double>(now - start_).count();
  double eff = up > 0 ? (double)accepted_.load() * work_per_share_.load() / up : 0;
  std::ostringstream line;
  line << "speed " << format_rate(total) << (stats_.size() > 1 ? gpus.str() : std::string())
       << " | shares " << accepted_.load() << " ok, " << rejected_.load() << " rejected, " << stale_.load()
       << " stale | pool-side ~" << format_rate(eff) << " | up " << format_duration(up) << state;
  log_line(Color::white, line.str());
}

int MinerCore::run() {
  PoolConfig pc;
  bool tls_hint = false;
  if (!parse_pool_url(cfg_.pool, pc.host, pc.port, tls_hint)) {
    log_line(Color::red, "invalid pool address '" + cfg_.pool + "' (expected host:port)");
    return 2;
  }
  pc.tls = cfg_.tls || tls_hint;
  pc.wallet = cfg_.wallet;
  pc.worker = cfg_.worker;
  pc.password = cfg_.password.empty() ? "x" : cfg_.password;
  pc.label = "pool";

  std::vector<DeviceConfig> devices;
  for (const auto& d : cfg_.devices) if (d.enabled) devices.push_back(d);
  if (devices.empty()) {
    log_line(Color::red, "no GPUs enabled; run `intelb60 setup`");
    return 2;
  }

  PoolClient::Callbacks cb;
  cb.on_job = [this](const PoolJob&) { on_pool_job(Source::user); };
  cb.on_result = [this](bool ok, const std::string& reason, double ms) {
    if (ok) {
      accepted_++;
      log_line(Color::green, "share accepted (" + std::to_string((int)ms) + " ms)");
    } else {
      rejected_++;
      log_line(Color::red, "share rejected: " + (reason.empty() ? std::string("no reason given") : reason));
    }
  };
  cb.on_connection = [this](bool up) {
    if (up) return;
    bool mine;
    {
      std::lock_guard<std::mutex> lock(job_mutex_);
      mine = !fee_active_;
    }
    if (mine) set_active(Source::none);
  };
  {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    user_pool_ = std::make_shared<PoolClient>(pc, cb, cfg_.verbose);
  }
  user_pool_->start();

  // First dev fee round after 30 minutes. INTELB60_FEE_FIRST_S can only bring it forward (testing).
  int first_fee_s = 30 * 60;
  if (const char* e = std::getenv("INTELB60_FEE_FIRST_S")) first_fee_s = std::clamp(std::atoi(e), 10, first_fee_s);
  next_fee_ = Clock::now() + std::chrono::seconds(first_fee_s);

  for (size_t i = 0; i < devices.size(); i++) stats_.push_back(std::make_unique<GpuStats>());
  std::vector<std::thread> threads;
  for (size_t i = 0; i < devices.size(); i++)
    threads.emplace_back([this, i, d = devices[i]] { gpu_thread((int)i, d); });
  std::thread prover([this] { proof_thread(); });

  auto next_report = Clock::now() + std::chrono::seconds(cfg_.stats_interval);
  while (!stop_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    fee_tick();
    if (Clock::now() >= next_report) {
      report();
      next_report = Clock::now() + std::chrono::seconds(cfg_.stats_interval);
    }
    bool all_failed = !stats_.empty();
    for (auto& s : stats_) all_failed = all_failed && s->failed.load();
    if (all_failed) {
      log_line(Color::red, "all GPUs failed; stopping");
      stop_ = true;
    }
  }
  log_line(Color::yellow, "shutting down...");
  job_cv_.notify_all();
  found_cv_.notify_all();
  for (auto& t : threads) t.join();
  prover.join();
  // Destroy clients outside pool_mutex_: their destructors join threads whose callbacks take it.
  std::shared_ptr<PoolClient> a, b, c;
  {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    a.swap(fee_pool_);
    b.swap(fee_retiring_);
    c.swap(user_pool_);
  }
  a.reset();
  b.reset();
  c.reset();
  log_line(Color::white, "session: " + std::to_string(accepted_.load()) + " accepted, " +
                             std::to_string(rejected_.load()) + " rejected, " + std::to_string(stale_.load()) + " stale");
  return 0;
}

}  // namespace

int run_miner(const Config& cfg, std::atomic<bool>& stop) {
  MinerCore core(cfg, stop);
  return core.run();
}

}  // namespace intelb60
