// IntelB60 - Pearl (PRL) miner for Intel Arc GPUs (fork of IAminer)
// Copyright (C) 2026 IAminer contributors. GPL-3.0-or-later (see LICENSE).
#pragma once

#include <string>
#include <vector>

#include "engine.hpp"

namespace intelb60 {

struct DeviceConfig {
  int index = 0;
  std::string name;
  bool enabled = true;
  Shape shape;
  double tuned_rate = 0;   // MAC/s measured by `intelb60 tune` (0 = not tuned)
};

struct Config {
  std::string pool = "prl.kryptex.network:7048";  // B60j build-in: Kryptex PRL
  bool tls = false;          // B60j build-in: plain port
  std::string wallet;              // public build: no baked user - wizard or -u
  std::string worker;             // public build: defaults to hostname
  std::string password = "x";
  std::vector<DeviceConfig> devices;
  int stats_interval = 30;   // seconds between status reports
  std::string log_file;      // empty = console only
  bool color = true;
  bool verbose = false;
};

std::string executable_dir();
std::string default_config_path();
bool load_config(const std::string& path, Config& cfg, std::string& error);
bool save_config(const std::string& path, const Config& cfg, std::string& error);
// Default shape for a GPU with `memory` bytes (used when the GPU has not been tuned).
Shape default_shape(uint64_t memory);

}  // namespace intelb60
