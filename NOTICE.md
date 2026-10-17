# Third-party components

IntelB60 is a fork of IAminer, distributed under the GNU General Public License v3.0 or later. It includes or links the following components. Each keeps its own copyright and license.

| Component | Where | License | Source |
|---|---|---|---|
| IAminer (base program this fork derives from) | whole tree | GPL-3.0-or-later, © 2026 IAminer contributors | https://github.com/BigShoots/IAminer |
| PearlHash GPU engine (SYCL/ESIMD kernels, BLAKE3, commitment, noise and PlainProof builder) | `engine/pearlhash/` | GPL-3.0, © 2023–2026 MoneroOcean | https://github.com/MoneroOcean/mo-miner (v0.8.0, commit `d758fb9`) |
| nlohmann/json 3.11.3 | `third_party/nlohmann/` | MIT, © 2013–2023 Niels Lohmann | https://github.com/nlohmann/json |
| miniz 3.0.2 | `third_party/miniz/` | MIT, © 2013–2014 RAD Game Tools and Valve Software, © 2010–2014 Rich Geldreich and Tenacious Software LLC | https://github.com/richgel999/miniz |
| OpenSSL 3 | linked (system library on Linux, static on Windows) | Apache-2.0 | https://www.openssl.org |
| Intel oneAPI DPC++ runtime (`libsycl`, Unified Runtime, UMF, TBB, …) | bundled in release archives | Intel Simplified Software License / Apache-2.0 with LLVM exceptions | https://www.intel.com/content/www/us/en/developer/tools/oneapi/ |

The algorithm constants and protocol behaviour come from the Pearl reference implementation (https://github.com/pearl-research-labs/pearl, ISC license).
