// IntelB60 - Pearl (PRL) miner for Intel Arc GPUs (fork of IAminer)
// Copyright (C) 2026 IAminer contributors. GPL-3.0-or-later (see LICENSE).
#pragma once

#include <atomic>

#include "config.hpp"

namespace intelb60 {

// Runs until `stop` becomes true. Returns a process exit code.
int run_miner(const Config& cfg, std::atomic<bool>& stop);

}  // namespace intelb60
