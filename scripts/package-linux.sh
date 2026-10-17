#!/usr/bin/env bash
# Build IntelB60 and package a self-contained Linux x64 release:
#   dist/IntelB60-<version>-linux-x64.tar.gz
# Bundles the Intel oneAPI DPC++ runtime (libsycl, Unified Runtime + Level Zero adapters, UMF, ...)
# into lib/. The GPU driver itself (libze_loader + Intel compute runtime) comes from the system.
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
cd "$here"
if ! command -v icpx >/dev/null; then
  set +u; . /opt/intel/oneapi/setvars.sh --force >/dev/null; set -u
fi
version="$(sed -n 's/^#define INTELB60_VERSION "\(.*\)"/\1/p' src/version.hpp)"
build="${BUILD_DIR:-$here/build/linux-release}"
name="IntelB60-${version}-linux-x64"
stage="$here/dist/$name"

CXX=icpx CC=icx cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$build"

rm -rf "$stage" "$here/dist/$name.tar.gz"
mkdir -p "$stage/lib"
cp "$build/intelb60" "$stage/"
strip "$stage/intelb60"
cp README.md LICENSE NOTICE.md "$stage/"

# System libraries that must NOT be bundled (glibc, C++ runtime, GPU driver loaders).
is_system_lib() {
  case "$1" in
    ld-linux*|libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|libresolv.so*|libutil.so*|\
    libgcc_s.so*|libstdc++.so*|libze_loader.so*|libOpenCL.so*|libssl.so*|libcrypto.so*|libz.so*|\
    libdrm*.so*|libnuma.so*|libxml2.so*|libicu*.so*|liblzma.so*) return 0 ;;
  esac
  return 1
}

oneapi_lib="${CMPLR_ROOT:-/opt/intel/oneapi/compiler/latest}/lib"
queue=("$stage/intelb60")
# Unified Runtime adapters are dlopen()ed, so ldd can't see them; add them explicitly.
for extra in libur_loader.so.0 libur_adapter_level_zero.so.0 libur_adapter_level_zero_v2.so.0; do
  [ -e "$oneapi_lib/$extra" ] && queue+=("$oneapi_lib/$extra")
done
declare -A seen
while [ "${#queue[@]}" -gt 0 ]; do
  cur="${queue[0]}"; queue=("${queue[@]:1}")
  [ -n "${seen[$cur]:-}" ] && continue
  seen[$cur]=1
  if [ "$cur" != "$stage/intelb60" ]; then
    n="$(basename "$cur")"
    [ -e "$stage/lib/$n" ] || cp -L "$cur" "$stage/lib/$n"
  fi
  while read -r dep; do
    [ -n "$dep" ] || continue
    is_system_lib "$(basename "$dep")" && continue
    queue+=("$dep")
  done < <(LD_LIBRARY_PATH="$oneapi_lib:${LD_LIBRARY_PATH:-}" ldd "$cur" 2>/dev/null | awk '/=> \//{print $3}')
done

# Libraries next to each other find one another; make the bundled ones prefer ./lib too.
if command -v patchelf >/dev/null; then
  for so in "$stage"/lib/*.so*; do patchelf --set-rpath '$ORIGIN' "$so" 2>/dev/null || true; done
fi

cat > "$stage/start.sh" <<'EOF'
#!/bin/sh
# Convenience launcher: runs IntelB60 from this folder (first run opens the setup wizard).
cd "$(dirname "$0")" && exec ./intelb60 "$@"
EOF
chmod +x "$stage/start.sh" "$stage/intelb60"

tar -C "$here/dist" -czf "$here/dist/$name.tar.gz" "$name"
echo "built dist/$name.tar.gz ($(du -h "$here/dist/$name.tar.gz" | cut -f1))"
ls "$stage/lib"
