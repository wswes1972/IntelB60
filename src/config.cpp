// IntelB60 - Pearl (PRL) miner for Intel Arc GPUs (fork of IAminer)
// Copyright (C) 2026 IAminer contributors. GPL-3.0-or-later (see LICENSE).
#include "config.hpp"

#include <filesystem>
#include <fstream>

#include "nlohmann/json.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

using json = nlohmann::json;

namespace intelb60 {

std::string executable_dir() {
#ifdef _WIN32
  char buf[MAX_PATH];
  DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  std::filesystem::path p(std::string(buf, n));
#else
  std::error_code ec;
  std::filesystem::path p = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec) return ".";
#endif
  return p.parent_path().string();
}

std::string default_config_path() {
  return (std::filesystem::path(executable_dir()) / "intelb60.json").string();
}

Shape default_shape(uint64_t memory) {
  Shape s;
  // ~2.2 * m * k bytes of device memory at m = n.
  if (memory < (6ULL << 30)) { s.m = s.n = 65536; s.k = 8192; }
  return s;
}

bool load_config(const std::string& path, Config& cfg, std::string& error) {
  std::ifstream in(path);
  if (!in) { error = "cannot open " + path; return false; }
  try {
    json j = json::parse(in);
    cfg.pool = j.value("pool", cfg.pool);
    cfg.tls = j.value("tls", cfg.tls);
    cfg.wallet = j.value("wallet", cfg.wallet);
    cfg.worker = j.value("worker", cfg.worker);
    cfg.password = j.value("password", cfg.password);
    cfg.stats_interval = j.value("stats_interval", cfg.stats_interval);
    cfg.log_file = j.value("log_file", cfg.log_file);
    cfg.color = j.value("color", cfg.color);
    cfg.verbose = j.value("verbose", cfg.verbose);
    cfg.devices.clear();
    if (j.contains("devices")) {
      for (const auto& d : j["devices"]) {
        DeviceConfig dc;
        dc.index = d.value("index", 0);
        dc.name = d.value("name", std::string());
        dc.enabled = d.value("enabled", true);
        dc.shape.m = d.value("m", dc.shape.m);
        dc.shape.n = d.value("n", dc.shape.m);
        dc.shape.k = d.value("k", dc.shape.k);
        dc.shape.rank = d.value("rank", dc.shape.rank);
        dc.tuned_rate = d.value("tuned_rate", 0.0);
        cfg.devices.push_back(dc);
      }
    }
  } catch (const std::exception& e) {
    error = path + ": " + e.what();
    return false;
  }
  return true;
}

bool save_config(const std::string& path, const Config& cfg, std::string& error) {
  json j;
  j["pool"] = cfg.pool;
  j["tls"] = cfg.tls;
  j["wallet"] = cfg.wallet;
  j["worker"] = cfg.worker;
  j["password"] = cfg.password;
  j["stats_interval"] = cfg.stats_interval;
  j["log_file"] = cfg.log_file;
  j["color"] = cfg.color;
  j["verbose"] = cfg.verbose;
  j["devices"] = json::array();
  for (const auto& d : cfg.devices) {
    j["devices"].push_back({{"index", d.index},
                            {"name", d.name},
                            {"enabled", d.enabled},
                            {"m", d.shape.m},
                            {"n", d.shape.n},
                            {"k", d.shape.k},
                            {"rank", d.shape.rank},
                            {"tuned_rate", d.tuned_rate}});
  }
  std::ofstream out(path);
  if (!out) { error = "cannot write " + path; return false; }
  out << j.dump(2) << "\n";
  return true;
}

}  // namespace intelb60
