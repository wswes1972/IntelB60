# Building IntelB60

IntelB60 is a single C++20 program. The only SYCL source is `src/engine.cpp`, which contains the vendored PearlHash engine. It's compiled with the Intel oneAPI DPC++/C++ compiler.

## Linux (Ubuntu 24.04 or newer)

```bash
sudo apt install cmake ninja-build libssl-dev
# Intel oneAPI DPC++/C++ Compiler 2026: https://www.intel.com/content/www/us/en/developer/tools/oneapi/dpc-compiler-download.html
source /opt/intel/oneapi/setvars.sh
scripts/package-linux.sh
```

This produces `dist/IntelB60-<version>-linux-x64.tar.gz`. The archive contains:

- `intelb60`, with RPATH `$ORIGIN/lib`;
- `lib/`, the oneAPI runtime: `libsycl`, the Unified Runtime loader and Level Zero adapters, UMF, and Intel math libraries;
- `README.md`, `LICENSE`, `NOTICE.md`, and `start.sh`.

The Level Zero loader and Intel GPU driver come from the user's system. They are the `libze1` and `libze-intel-gpu1` packages.

## Windows 10/11 x64

You need:

1. Visual Studio 2022 Build Tools with the C++ workload. This also provides CMake and Ninja.
2. The Intel oneAPI DPC++/C++ Compiler 2026. The `intel.oneapi.win.cpp-dpcpp-common` component is enough.
3. vcpkg with OpenSSL built against the dynamic CRT. SYCL on Windows requires `/MD`.
   ```powershell
   git clone https://github.com/microsoft/vcpkg $env:USERPROFILE\intelb60-build\vcpkg
   & $env:USERPROFILE\intelb60-build\vcpkg\bootstrap-vcpkg.bat -disableMetrics
   & $env:USERPROFILE\intelb60-build\vcpkg\vcpkg.exe install openssl:x64-windows-static-md
   ```

Then run:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1
```

This produces `dist\IntelB60-<version>-windows-x64.zip`. The zip contains `intelb60.exe`, the oneAPI runtime DLLs (`sycl*.dll`, `ur_loader.dll`, the Level Zero UR adapters, `umf.dll`, and the Intel math runtime), and the MSVC runtime DLLs. It also includes `start.cmd` and `setup.cmd` for double-click use. The Level Zero loader (`ze_loader.dll`) comes with the Intel graphics driver.

## Checking a build

```bash
intelb60 devices        # should list your Arc GPU(s)
intelb60 tune --quick   # engine speed on each GPU
intelb60 -o prl-us.kryptex.network:7048 -u <wallet> -w test   # a few minutes should give accepted shares
```

## Engine updates

`engine/pearlhash/ENGINE.md` records where the engine came from upstream and the three IAminer modifications to re-apply after updating from mo-miner.
