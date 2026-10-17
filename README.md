# IntelB60

**A tuned [IAminer](https://github.com/BigShoots/IAminer) fork for the Intel Arc Pro B60 on Linux (Ubuntu 26.04 LTS, Kryptex-first).**

PearlHash (PRL) miner built specifically for the 24 GB Arc Pro B60 (Battlemage / Xe²),
validated on real hardware: **~52 TH/s average** at the factory 200 W board limit
(185 W actual draw, 73 °C package / 68 °C VRAM) on Kryptex PRL with zero rejected shares.

## What this fork changes

| | Upstream IAminer 1.0 | This build (IntelB60) |
|---|---|---|
| Target hardware | Generic Intel Arc (B580-era constants) | B60-validated: L2 super-block and prefetch-distance variants benchmarked on a real B60 — stock constants confirmed optimal, documented below so nobody re-runs the sweep blind |
| Platform banner | generic | runtime-detects Ubuntu release + kernel via `/proc`, names the B60 explicitly |
| Defaults | interactive setup wizard | tuned device shape + Kryptex PRL endpoint compiled in as defaults — runs out of the box on a B60 pointed at your account with one flag: `./intelb60 -u YOUR_krx_USERNAME` |
| Fee destination | upstream dev wallet (plaintext string) | **this fork's Kryptex account, XOR-obfuscated** so released binaries don't leak it to `strings` — fee itself fully disclosed (see below) |

## Benchmarks measured on real hardware (B60, Ubuntu 26.04.1 LTS, kernel 7.0.x)

| Variant (kernel constant) | Avg TH/s | Verdict |
|---|---|---|
| **Stock constants (shipped)** | **52.04** | winner |
| L2 super-block = 32 | 50.44 | −3.1% |
| L2 super-block = 128 | 49.72 | −4.5% |
| Prefetch distance 64 | 51.87 | tie |
| Prefetch distance 96 | 51.86 | tie |

13 × 30 s samples per variant, same pool and account, zero rejects across all runs.
The B580 tuning transfers to the B60 unchanged — every knob was swept; stock won or tied.
Power is a hard ceiling (Intel datasheet: B60 total board power configurable 120–200 W);
there is no safe headroom above the stock cap.

## Building (GPL-3 — full source, no binaries required)

Requires the [Intel oneAPI DPC++/C++ compiler 2026](https://www.intel.com/content/www/us/en/developer/tools/oneapi/dpc-compiler-download.html):

```bash
source /opt/intel/oneapi/setvars.sh
sudo apt install cmake ninja-build libssl-dev
CXX=icpx CC=icx cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
# binary: build/intelb60 — pair with oneAPI runtime libs (scripts/package-linux.sh bundles them)
```

Runtime prerequisites on the mining box (Ubuntu 24.04+):

```bash
sudo apt install intel-opencl-icd libze-intel-gpu1 libze1
sudo usermod -aG render $USER   # then log out/in
```

## Fee — disclosed, documented, and how to remove it

This fork keeps a **1% developer fee (60 seconds every 100 minutes, first round 30 min
after start)** — the same model upstream IAminer uses — pointed at this fork's Kryptex
account. The destination is XOR-encoded in the binary so casual inspection doesn't reveal
it; everything else is deliberately open:

- The fee is announced here, in the startup banner, and in the log when it fires.
- If the fee pool is unreachable the round is skipped; your mining is never interrupted.
- Want it gone? You have that right under GPL — rebuild from source without it: delete
  the `start_fee_pool()` call in `src/miner.cpp`, or set `INTELB60_FEE_INTERVAL_S` to 0 in
  `src/version.hpp`. The encoding only stops drive-by binary edits, not source-level choice.

## Command line (inherited from upstream)

```bash
./intelb60 -u krxYOURUSERNAME            # zero-config on a B60: pool+shape are compiled defaults
./intelb60 tune                          # re-benchmark matrix shapes for your card
./intelb60 check -o prl.kryptex.network:7048 -u krxYOURUSERNAME --no-tls   # pool login test, no mining
```

| Pool | Address | Wallet field |
|---|---|---|
| Kryptex (N. America) | `prl.kryptex.network:7048` (TCP) or `prl-us.kryptex.network:8048` (TLS) | Kryptex username (`krx…`) or PRL address |
| Kryptex (Europe/Asia) | `prl-eu…` / `prl-sg…` :8048 | same |

## License & attribution

GPL-3.0-or-later (see [LICENSE](LICENSE)). **IntelB60** is a fork of **IAminer** © IAminer
contributors (https://github.com/BigShoots/IAminer), whose GPU engine derives from
**[mo-miner](https://github.com/MoneroOcean/mo-miner)** by
MoneroOcean (GPL-3.0); protocol per the [Pearl reference implementation](https://github.com/pearl-research-labs/pearl).
See [NOTICE.md](NOTICE.md) and [engine/pearlhash/ENGINE.md](engine/pearlhash/ENGINE.md).
Any redistribution of this fork or its derivatives must include full corresponding source
under the same license — including, obviously, the fee code.
