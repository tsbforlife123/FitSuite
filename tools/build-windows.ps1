<#
  Builds FitSuite.exe on Windows.

  Needs CMake 3.20+ and Ninja on PATH (Visual Studio 2022 includes both), plus a compiler:
    * llvm-mingw (the toolchain used for releases):
        .\tools\build-windows.ps1 -LlvmMingw C:\tools\llvm-mingw
    * Visual Studio 2022 "Desktop development with C++" (not tested yet): run this script from the
      "x64 Native Tools Command Prompt for VS 2022" (start PowerShell there first):
        .\tools\build-windows.ps1

  Options:
    -Tests      also build and run the unit tests
    -BuildDir   output folder (default: build)
#>
param(
    [string]$LlvmMingw = "",
    [switch]$Tests,
    [string]$BuildDir = "build"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDir

foreach ($tool in @("cmake", "ninja")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool was not found on PATH. Install it (for example: winget install Kitware.CMake Ninja-build.Ninja) or use the Visual Studio developer prompt."
    }
}

$cmakeArgs = @("-S", $root, "-B", $build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DFITSUITE_BUILD_APP=ON")
if ($Tests) { $cmakeArgs += "-DFITSUITE_BUILD_TESTS=ON" } else { $cmakeArgs += "-DFITSUITE_BUILD_TESTS=OFF" }

if ($LlvmMingw -ne "") {
    $bin = Join-Path $LlvmMingw "bin"
    if (-not (Test-Path (Join-Path $bin "x86_64-w64-mingw32-clang++.exe"))) {
        throw "No llvm-mingw compiler found in $bin. Use an 'ucrt-x86_64' release of llvm-mingw."
    }
    $env:PATH = "$bin;$env:PATH"
    $cmakeArgs += @(
        "-DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang.exe",
        "-DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++.exe",
        "-DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres.exe"
    )
} elseif (-not (Get-Command "cl" -ErrorAction SilentlyContinue)) {
    throw "No compiler found. Pass -LlvmMingw <folder>, or run from the 'x64 Native Tools Command Prompt for VS 2022'."
}

& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }
& cmake --build $build
if ($LASTEXITCODE -ne 0) { throw "The build failed." }

if ($Tests) {
    & (Join-Path $build "fitsuite_tests.exe")
    if ($LASTEXITCODE -ne 0) { throw "Unit tests failed." }
}
Write-Host ""
Write-Host "Built $(Join-Path $build 'FitSuite.exe')"
