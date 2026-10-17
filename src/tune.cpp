// IntelB60 - Pearl (PRL) miner for Intel Arc GPUs (fork of IAminer)
// Copyright (C) 2026 IAminer contributors. GPL-3.0-or-later (see LICENSE).
#include "tune.hpp"

#include <algorithm>
#include <cstring>
#include <random>

#include "util.hpp"

namespace intelb60 {

std::vector<TuneResult> tune_gpu(int index, uint64_t memory, bool quick, std::atomic<bool>& stop) {
  // Candidate shapes (m = n, rank 128). Engine limit: m*k < 2^31. Device memory ~2.2*m*k bytes.
  std::vector<Shape> candidates;
  const int ms[] = {65536, 131072, 262144};
  const int ks[] = {4096, 8192, 16384};
  for (int m : ms) for (int k : ks) {
    if ((int64_t)m * k >= (1LL << 31)) continue;
    if ((double)m * k * 2.4 > (double)memory * 0.85) continue;
    Shape s;
    s.m = s.n = m;
    s.k = k;
    candidates.push_back(s);
  }
  if (quick) {
    // Just the two shapes that win on current Arc cards.
    std::vector<Shape> q;
    for (const auto& s : candidates)
      if ((s.m == 131072 && (s.k == 8192 || s.k == 4096))) q.push_back(s);
    if (!q.empty()) candidates = q;
  }

  std::mt19937 rng(12345);
  uint8_t header[76];
  for (auto& b : header) b = (uint8_t)rng();
  uint8_t bound[32];
  std::memset(bound, 0, sizeof bound);  // unreachable target: measure pure search speed

  std::vector<TuneResult> results;
  for (size_t i = 0; i < candidates.size() && !stop; i++) {
    const Shape& s = candidates[i];
    char label[96];
    std::snprintf(label, sizeof label, "  [%zu/%zu] m=%d k=%d", i + 1, candidates.size(), s.m, s.k);
    TuneResult r;
    r.shape = s;
    try {
      Engine e(index, s);
      e.set_job(header, bound);
      Found f;
      e.attempt(1, f);  // warm-up (JIT + first-touch)
      const double min_seconds = quick ? 6.0 : 10.0;
      auto t0 = Clock::now();
      int n = 0;
      double secs = 0;
      while (!stop && (n < 3 || secs < min_seconds)) {
        e.attempt(2 + n, f);
        n++;
        secs = std::chrono::duration<double>(Clock::now() - t0).count();
      }
      r.rate = secs > 0 ? (double)n * (double)s.macs() / secs : 0;
      log_line(Color::white, std::string(label) + "  " + format_rate(r.rate));
    } catch (const std::exception& ex) {
      log_line(Color::yellow, std::string(label) + "  skipped (" + ex.what() + ")");
    }
    if (r.rate > 0) results.push_back(r);
  }
  std::sort(results.begin(), results.end(), [](const TuneResult& a, const TuneResult& b) { return a.rate > b.rate; });
  return results;
}

}  // namespace intelb60
