// IntelB60 - Pearl (PRL) miner for Intel Arc GPUs (fork of IAminer)
// Copyright (C) 2026 IAminer contributors. GPL-3.0-or-later (see LICENSE).
#pragma once

#include <atomic>
#include <vector>

#include "engine.hpp"

namespace intelb60 {

struct TuneResult {
  Shape shape;
  double rate = 0;   // MAC/s; 0 = failed / did not fit
};

// Benchmarks the candidate shapes that fit `memory` on GPU `index`. Prints progress. Returns results
// sorted fastest first (may be empty if nothing could run).
std::vector<TuneResult> tune_gpu(int index, uint64_t memory, bool quick, std::atomic<bool>& stop);

}  // namespace intelb60
