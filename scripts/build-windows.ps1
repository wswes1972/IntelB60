# Build IntelB60 for Windows x64 and package dist\IntelB60-<version>-windows-x64.zip
#
# Requirements: Visual Studio 2022 Build Tools (C++), Intel oneAPI DPC++/C++ Compiler 2026,
# vcpkg with openssl:x64-windows-static-md, CMake + Ninja (bundled with VS Build Tools).
param(
  [string]$VcpkgRoot = "$env:USERPROFILE\intelb60-build\vcpkg",
  [string]$BuildDir = "$env:USERPROFILE\intelb60-build\win-release",
  [string]$OneApiRoot = "C:\Program Files (x86)\Intel\oneAPI"
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$version = (Select-String -Path "$repo\src\version.hpp" -Pattern '#define INTELB60_VERSION "(.*)"').Matches[0].Groups[1].Value
$name = "IntelB60-$version-windows-x64"

# --- Import the Visual Studio x64 and oneAPI compiler environments into this session ---
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio 2022 C++ build tools not found" }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
# Only the compiler component's environment is needed (setvars.bat is fragile when nested).
$cmplrVars = Join-Path $OneApiRoot "compiler\latest\env\vars.bat"
if (-not (Test-Path $cmplrVars)) { throw "oneAPI DPC++/C++ compiler not found under $OneApiRoot" }
$envScript = Join-Path $env:TEMP "intelb60-env.cmd"
@"
@echo off
set "PATH=${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;%PATH%"
call "$vcvars" >nul || exit /b 1
call "$cmplrVars" intel64 vs2022 >nul || exit /b 1
set
"@ | Set-Content -Encoding ascii $envScript
$envDump = cmd /c $envScript
if ($LASTEXITCODE) { throw "failed to load the Visual Studio / oneAPI environment" }
foreach ($line in $envDump) { if ($line -match '^([^=]+)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] } }
if (-not (Get-Command icx -ErrorAction SilentlyContinue)) { throw "icx not on PATH after setvars" }
$ninja = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" }

# --- Configure + build ---
& $cmake -S $repo -B $BuildDir -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=icx -DCMAKE_CXX_COMPILER=icx `
  "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
if ($LASTEXITCODE) { throw "cmake configure failed" }
& $cmake --build $BuildDir
if ($LASTEXITCODE) { throw "build failed" }

# --- Stage ---
$dist = Join-Path $repo "dist"
$stage = Join-Path $dist $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item "$BuildDir\intelb60.exe" $stage
Copy-Item "$repo\README.md", "$repo\LICENSE", "$repo\NOTICE.md" $stage

# oneAPI runtime DLLs: walk dumpbin /dependents from intelb60.exe plus the dlopen()ed UR adapters.
$searchDirs = @(
  "$env:CMPLR_ROOT\bin", "$OneApiRoot\umf\latest\bin", "$OneApiRoot\tbb\latest\bin",
  "$OneApiRoot\tcm\latest\bin", "$OneApiRoot\compiler\latest\bin"
) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -Unique
function Find-Dll([string]$dll) {
  foreach ($d in $searchDirs) { $p = Join-Path $d $dll; if (Test-Path $p) { return $p } }
  return $null
}
$queue = [System.Collections.Generic.Queue[string]]::new()
$queue.Enqueue("$stage\intelb60.exe")
foreach ($a in "ur_loader.dll", "ur_adapter_level_zero.dll", "ur_adapter_level_zero_v2.dll") {
  $p = Find-Dll $a; if ($p) { $queue.Enqueue($p) }
}
$seen = @{}
while ($queue.Count) {
  $cur = $queue.Dequeue()
  if ($seen[$cur]) { continue }
  $seen[$cur] = $true
  if ($cur -notlike "$stage\intelb60.exe") {
    $dst = Join-Path $stage (Split-Path $cur -Leaf)
    if (-not (Test-Path $dst)) { Copy-Item $cur $dst }
  }
  $deps = & dumpbin /nologo /dependents $cur | Where-Object { $_ -match '^\s+(\S+\.dll)\s*$' } | ForEach-Object { $Matches[1] }
  foreach ($dep in $deps) {
    $p = Find-Dll $dep   # only oneAPI-provided DLLs; system + GPU driver DLLs (ze_loader) are not bundled
    if ($p) { $queue.Enqueue($p) }
  }
}
# MSVC runtime (redistributable) so users don't need the VC++ Redistributable installed.
$crt = Get-ChildItem "$env:VCToolsRedistDir\x64" -Directory -Filter "Microsoft.VC*.CRT" | Select-Object -First 1
if ($crt) { Copy-Item "$($crt.FullName)\vcruntime140.dll", "$($crt.FullName)\vcruntime140_1.dll", "$($crt.FullName)\msvcp140.dll" $stage }

# Double-click launchers.
@"
@echo off
rem Starts IntelB60 (the first run opens the setup wizard).
cd /d "%~dp0"
intelb60.exe %*
pause
"@ | Set-Content -Encoding ascii "$stage\start.cmd"
@"
@echo off
rem Re-run the setup wizard (pool, wallet, worker, GPUs, tuning).
cd /d "%~dp0"
intelb60.exe setup
pause
"@ | Set-Content -Encoding ascii "$stage\setup.cmd"

$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $stage -DestinationPath $zip
"built $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
Get-ChildItem $stage | Select-Object Name, @{n = 'KB'; e = { [math]::Round($_.Length / 1KB) } } | Format-Table -AutoSize | Out-String
