# IntelB60 - Pearl (PRL) miner for Intel Arc GPUs (fork of IAminer). GPL-3.0-or-later.
#
# Build with the Intel oneAPI DPC++/C++ compiler:
#   Linux:   CXX=icpx CC=icx cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
#   Windows: see scripts/build-windows.ps1 (icx + vcpkg OpenSSL)
cmake_minimum_required(VERSION 3.20)
project(IntelB60 VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(WIN32)
  # Static OpenSSL built against the dynamic CRT (vcpkg triplet x64-windows-static-md): SYCL on
  # Windows requires /MD, so the whole program uses the DLL runtime.
  set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()
find_package(OpenSSL REQUIRED)

add_library(miniz STATIC third_party/miniz/miniz.c)
target_include_directories(miniz PUBLIC third_party/miniz)
target_compile_definitions(miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_ARCHIVE_APIS MINIZ_NO_TIME)

add_executable(intelb60
  src/main.cpp
  src/miner.cpp
  src/stratum.cpp
  src/net.cpp
  src/util.cpp
  src/config.cpp
  src/tune.cpp
  src/engine.cpp)
target_include_directories(intelb60 PRIVATE src third_party)
target_link_libraries(intelb60 PRIVATE miniz OpenSSL::SSL OpenSSL::Crypto)

# Only the engine translation unit is SYCL; the link step must also be -fsycl.
set_source_files_properties(src/engine.cpp PROPERTIES COMPILE_OPTIONS "-fsycl")
target_link_options(intelb60 PRIVATE -fsycl)
target_compile_definitions(intelb60 PRIVATE NDEBUG)

if(WIN32)
  target_compile_definitions(intelb60 PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(intelb60 PRIVATE ws2_32 crypt32 user32 advapi32)
  set_property(TARGET intelb60 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
  set_property(TARGET miniz PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
else()
  target_link_libraries(intelb60 PRIVATE pthread)
  # Find the bundled oneAPI runtime in ./lib next to the executable (DT_RPATH also covers the
  # libraries libsycl itself loads).
  set_target_properties(intelb60 PROPERTIES BUILD_RPATH "\$ORIGIN/lib" INSTALL_RPATH "\$ORIGIN/lib")
  target_link_options(intelb60 PRIVATE "-Wl,--disable-new-dtags")
endif()
