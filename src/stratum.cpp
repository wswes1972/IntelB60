// IntelB60 - Pearl (PRL) miner for Intel Arc GPUs (fork of IAminer)
// Copyright (C) 2026 IAminer contributors. GPL-3.0-or-later (see LICENSE).
//
// Pearl stratum (object-param dialect used by Kryptex, HeroMiners and others):
//   -> mining.subscribe ["IntelB60/x.y.z"]
//   -> mining.authorize {wallet, worker, pass, type:"v2"}      type:"v2" requests gzip proofs
//   <- {"id":2,"result":true,"type":"v2"}                      pool confirms gzip
//   <- mining.notify {header, job_id, target, height, cert_version}
//   -> mining.submit {worker, job_id, plain_proof}             base64(PlainProof) or base64(gzip(..))
#include "stratum.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "nlohmann/json.hpp"
#include "net.hpp"
#include "version.hpp"

using json = nlohmann::json;

namespace intelb60 {

bool parse_pool_url(const std::string& url_in, std::string& host, int& port, bool& tls_hint) {
  std::string url = url_in;
  tls_hint = false;
  static const std::pair<const char*, bool> schemes[] = {
      {"stratum+ssl://", true}, {"stratum+tls://", true}, {"ssl://", true}, {"tls://", true},
      {"stratum+tcp://", false}, {"stratum://", false}, {"tcp://", false}};
  for (const auto& s : schemes) {
    size_t n = std::strlen(s.first);
    if (url.size() > n && std::equal(url.begin(), url.begin() + n, s.first,
                                     [](char a, char b) { return std::tolower((unsigned char)a) == b; })) {
      url = url.substr(n);
      tls_hint = s.second;
      break;
    }
  }
  while (!url.empty() && url.back() == '/') url.pop_back();
  size_t colon = url.rfind(':');
  if (colon == std::string::npos || colon == 0) return false;
  host = url.substr(0, colon);
  try {
    port = std::stoi(url.substr(colon + 1));
  } catch (...) {
    return false;
  }
  return port > 0 && port < 65536;
}

PoolClient::PoolClient(PoolConfig cfg, Callbacks cb, bool verbose)
    : cfg_(std::move(cfg)), cb_(std::move(cb)), verbose_(verbose) {}

PoolClient::~PoolClient() { stop(); }

void PoolClient::start() {
  if (thread_.joinable()) return;
  stop_ = false;
  thread_ = std::thread([this] { run(); });
}

void PoolClient::stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  connected_ = false;
}

bool PoolClient::has_job() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return have_job_;
}

PoolJob PoolClient::current_job() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return job_;
}

bool PoolClient::submit(const PoolJob& job, const std::string& proof_b64) {
  if (!connected_) return false;
  std::string proof = proof_b64;
  if (gzip_) proof = base64_encode(gzip_compress(base64_decode(proof_b64)));
  std::lock_guard<std::mutex> lock(mutex_);
  int id = next_id_++;
  json msg = {{"id", id},
              {"method", "mining.submit"},
              {"params", {{"worker", cfg_.login()}, {"job_id", job.job_id}, {"plain_proof", proof}}}};
  outbox_.push_back(msg.dump() + "\n");
  pending_[id] = Clock::now();
  return true;
}

void PoolClient::run() {
  int backoff = 5;
  while (!stop_) {
    bool worked = session();
    connected_ = false;
    if (cb_.on_connection) cb_.on_connection(false);
    if (stop_) break;
    if (worked) backoff = 5;
    log_line(Color::yellow, "[" + cfg_.label + "] reconnecting in " + std::to_string(backoff) + " s");
    for (int i = 0; i < backoff * 10 && !stop_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    backoff = std::min(backoff * 2, 60);
  }
}

bool PoolClient::session() {
  LineConnection conn;
  std::string err;
  log_line(Color::cyan, "[" + cfg_.label + "] connecting to " + cfg_.host + ":" + std::to_string(cfg_.port) +
                            (cfg_.tls ? " (TLS)" : ""));
  if (!conn.open(cfg_.host, cfg_.port, cfg_.tls, err)) {
    log_line(Color::red, "[" + cfg_.label + "] " + err);
    return false;
  }
  gzip_ = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    outbox_.clear();
    pending_.clear();
    have_job_ = false;
  }
  json sub = {{"id", 1}, {"method", "mining.subscribe"}, {"params", {std::string("IntelB60/") + INTELB60_VERSION}}};
  json auth = {{"id", 2},
               {"method", "mining.authorize"},
               {"params", {{"wallet", cfg_.login()}, {"worker", cfg_.worker}, {"pass", cfg_.password}, {"type", "v2"}}}};
  if (!conn.write(sub.dump() + "\n") || !conn.write(auth.dump() + "\n")) {
    log_line(Color::red, "[" + cfg_.label + "] connection lost during login");
    return false;
  }
  bool got_any = false;
  auto last_rx = Clock::now();
  while (!stop_) {
    // Flush queued submissions (written only from this thread; OpenSSL objects are not shared).
    std::deque<std::string> out;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      out.swap(outbox_);
    }
    for (const auto& m : out) {
      if (!conn.write(m)) {
        log_line(Color::red, "[" + cfg_.label + "] send failed");
        return got_any;
      }
    }
    std::string line;
    int rc = conn.read_line(line, 100);
    if (rc < 0) {
      log_line(Color::yellow, "[" + cfg_.label + "] connection closed by pool");
      return got_any;
    }
    if (rc == 0) {
      if (Clock::now() - last_rx > std::chrono::minutes(10)) {
        log_line(Color::yellow, "[" + cfg_.label + "] no data for 10 minutes");
        return got_any;
      }
      continue;
    }
    last_rx = Clock::now();
    got_any = true;
    if (verbose_) log_line(Color::gray, "[" + cfg_.label + "] <- " + line.substr(0, 300));
    try {
      handle_line(line);
    } catch (const std::exception& e) {
      log_line(Color::red, "[" + cfg_.label + "] bad message: " + std::string(e.what()));
    }
  }
  return got_any;
}

void PoolClient::handle_line(const std::string& line) {
  json msg = json::parse(line);
  if (msg.contains("method") && msg["method"].is_string()) {
    const std::string method = msg["method"];
    if (method == "mining.notify") {
      const json& p = msg["params"];
      if (!p.is_object()) throw std::runtime_error("mining.notify params are not an object");
      PoolJob job;
      job.job_id = p.value("job_id", std::string());
      if (!from_hex(p.value("header", std::string()), job.header) || job.header.size() != 76)
        throw std::runtime_error("bad header in mining.notify");
      if (!from_hex(p.value("target", std::string()), job.target_be) || job.target_be.empty())
        throw std::runtime_error("bad target in mining.notify");
      job.height = p.value("height", (uint64_t)0);
      job.cert_version = p.value("cert_version", 0);
      bool fresh;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        fresh = !have_job_ || job_.height != job.height;
        job.serial = ++serial_;
        job_ = job;
        have_job_ = true;
      }
      if (job.cert_version && job.cert_version < 3)
        log_line(Color::yellow, "[" + cfg_.label + "] pool sent cert_version " + std::to_string(job.cert_version) +
                                    "; IntelB60 mines certificate v3 (salted seeds)");
      if (fresh || verbose_)
      {
        std::string per_share = format_rate(work_per_share(job.target_be));
        per_share = per_share.substr(0, per_share.size() - 2);  // "9.01 PH/s" -> "9.01 PH"
        log_line(Color::blue, "[" + cfg_.label + "] new job at height " + std::to_string(job.height) +
                                  " (" + per_share + " MACs per share)");
      }
      if (cb_.on_job) cb_.on_job(job);
      return;
    }
    if (verbose_) log_line(Color::gray, "[" + cfg_.label + "] ignoring method " + method);
    return;
  }
  if (!msg.contains("id") || !msg["id"].is_number_integer()) return;
  int id = msg["id"];
  bool is_error = msg.contains("error") && !msg["error"].is_null();
  std::string reason;
  if (is_error) {
    const json& e = msg["error"];
    if (e.is_array() && e.size() > 1 && e[1].is_string()) reason = e[1];
    else if (e.is_object() && e.contains("message")) reason = e["message"].dump();
    else reason = e.dump();
  }
  if (id == 1) return;  // subscribe acknowledgement (some pools reject it; authorize follows regardless)
  if (id == 2) {
    bool ok = !is_error && msg.contains("result") && msg["result"] == true;
    if (!ok) {
      log_line(Color::red, "[" + cfg_.label + "] login rejected" + (reason.empty() ? "" : ": " + reason));
      return;
    }
    gzip_ = msg.contains("type") && msg["type"] == "v2";
    connected_ = true;
    log_line(Color::green, "[" + cfg_.label + "] logged in as " + cfg_.login() +
                               (gzip_ ? " (gzip proofs)" : ""));
    if (cb_.on_connection) cb_.on_connection(true);
    return;
  }
  Clock::time_point sent;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pending_.find(id);
    if (it == pending_.end()) return;
    sent = it->second;
    pending_.erase(it);
  }
  double ms = std::chrono::duration<double, std::milli>(Clock::now() - sent).count();
  bool accepted = !is_error && msg.contains("result") && !msg["result"].is_null() && msg["result"] != false;
  if (cb_.on_result) cb_.on_result(accepted, reason, ms);
}

}  // namespace intelb60
