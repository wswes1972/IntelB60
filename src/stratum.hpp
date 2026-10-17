// IntelB60 - Pearl (PRL) miner for Intel Arc GPUs (fork of IAminer)
// Copyright (C) 2026 IAminer contributors. GPL-3.0-or-later (see LICENSE).
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "util.hpp"

namespace intelb60 {

struct PoolConfig {
  std::string host;
  int port = 0;
  bool tls = false;
  std::string wallet;     // PRL address or pool account (e.g. Kryptex username)
  std::string worker;     // rig name
  std::string password = "x";
  std::string label;      // for logs: "pool" / "dev fee"
  std::string login() const { return worker.empty() ? wallet : wallet + "." + worker; }
};

// Parses "host:port" (optionally prefixed stratum+tcp:// / stratum+ssl:// / tls://).
bool parse_pool_url(const std::string& url, std::string& host, int& port, bool& tls_hint);

struct PoolJob {
  std::string job_id;
  std::vector<uint8_t> header;      // 76 bytes
  std::vector<uint8_t> target_be;   // base share target T0, big-endian
  uint64_t height = 0;
  int cert_version = 0;
  uint64_t serial = 0;              // increases with every job this client receives
};

class PoolClient {
 public:
  struct Callbacks {
    std::function<void(const PoolJob&)> on_job;
    std::function<void(bool accepted, const std::string& reason, double latency_ms)> on_result;
    std::function<void(bool connected)> on_connection;
  };

  PoolClient(PoolConfig cfg, Callbacks cb, bool verbose);
  ~PoolClient();

  void start();
  void stop();
  bool connected() const { return connected_.load(); }
  bool has_job() const;
  PoolJob current_job() const;
  const PoolConfig& config() const { return cfg_; }
  // Queue a proof for submission. Returns false when not connected (the share is dropped).
  bool submit(const PoolJob& job, const std::string& plain_proof_b64);

 private:
  void run();
  bool session();
  void handle_line(const std::string& line);

  PoolConfig cfg_;
  Callbacks cb_;
  bool verbose_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> connected_{false};
  std::atomic<bool> gzip_{false};

  mutable std::mutex mutex_;
  PoolJob job_;
  bool have_job_ = false;
  uint64_t serial_ = 0;
  std::deque<std::string> outbox_;
  std::map<int, Clock::time_point> pending_;
  int next_id_ = 100;
};

}  // namespace intelb60
