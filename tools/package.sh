#!/usr/bin/env bash
# Builds the Windows installer (FitSuite-Setup.exe), the portable zip (FitSuite.zip) and a source zip
# into dist/. File names carry no version number: the version is in the release notes.
#   tools/package.sh                # uses an existing build-win/FitSuite.exe, or builds it
# Needs: cmake, ninja, llvm-mingw (LLVM_MINGW, default /opt/llvm-mingw), zip.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
name="FitSuite"
LLVM_MINGW="${LLVM_MINGW:-/opt/llvm-mingw}"

if [ ! -f build-win/FitSuite.exe ] || [ ! -f build-win/FitSuite-Setup.exe ] || [ "${REBUILD:-0}" = "1" ]; then
  PATH="$LLVM_MINGW/bin:$PATH" cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw-x86_64.cmake \
    -DCMAKE_BUILD_TYPE=Release -DFITSUITE_BUILD_APP=ON -DFITSUITE_BUILD_TESTS=OFF
  PATH="$LLVM_MINGW/bin:$PATH" cmake --build build-win
fi

stage="dist/${name}"
rm -rf "$stage" "dist/${name}.zip" "dist/${name}-source.zip" "dist/FitSuite-Setup.exe"
mkdir -p "$stage/docs" "$stage/tools"
# Both are linked without debug symbols (-s), so the FitSuite.exe here is the one inside the installer.
cp build-win/FitSuite.exe "$stage/"
cp build-win/FitSuite-Setup.exe dist/FitSuite-Setup.exe
cp packaging/README.txt "$stage/README.txt"
cp docs/MEASUREMENTS.md docs/MILESTONES.md "$stage/docs/"
printf 'Put PresentMon-2.x.x-x64.exe (github.com/GameTechDev/PresentMon/releases) in this folder.\r\n' > "$stage/tools/PUT-PRESENTMON-HERE.txt"

# Licence texts for everything compiled into FitSuite.exe, plus credits for tools it works with.
{
  echo "FitSuite includes the following third-party software."
  echo
  section() { echo "================================================================================"; echo "$1"; echo "================================================================================"; echo; cat "$2"; echo; echo; }
  section "Dear ImGui (MIT)" third_party/imgui/LICENSE.txt
  section "ImPlot (MIT)" third_party/implot/LICENSE
  section "nlohmann/json (MIT)" third_party/nlohmann/LICENSE.MIT
  section "Inter font (SIL Open Font License 1.1)" assets/fonts/OFL-Inter.txt
  section "IBM Plex Mono font (SIL Open Font License 1.1)" assets/fonts/OFL-IBMPlexMono.txt
  section "mingw-w64 runtime" third_party/licenses/mingw-w64-runtime.txt
  section "LLVM libc++ / libc++abi / libunwind (Apache 2.0 with LLVM Exceptions)" third_party/licenses/llvm-libcxx.txt
  echo "================================================================================"
  echo "Not included, credited"
  echo "================================================================================"
  echo
  echo "Raw Accel (MIT, github.com/RawAccelOfficial/rawaccel): FitSuite reproduces Raw Accel's order of"
  echo "operations (rotation, sensitivity, axis ratios) to validate compatible corrections and writes its"
  echo "settings.json format. Raw Accel itself is not included."
  echo
  echo "Intel PresentMon (MIT, github.com/GameTechDev/PresentMon): downloaded separately by the user and"
  echo "launched by FitSuite to record frame timing. Not included."
} | sed 's/$/\r/' > "$stage/THIRD_PARTY_NOTICES.txt"
sed -i 's/$/\r/' "$stage/README.txt"

(cd dist && zip -qr "${name}.zip" "${name}")

# Source archive (no build output).
src="dist/${name}-source"
rm -rf "$src"
mkdir -p "$src"
for p in CMakeLists.txt README.md app assets cmake core docs harness packaging platform tests third_party tools; do
  cp -r "$p" "$src/"
done
(cd dist && zip -qr "${name}-source.zip" "${name}-source")
rm -rf "$src"

ls -la dist/*.zip dist/FitSuite-Setup.exe
