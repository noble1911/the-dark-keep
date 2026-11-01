# Build DevilutionX for desktop (Windows / x64) using MSVC + vcpkg.
#
# Prerequisites (one-time):
#   - Visual Studio Build Tools (MSVC, "Desktop development with C++")
#   - CMake + Ninja (e.g. `scoop install cmake ninja`)
#   - vcpkg cloned & bootstrapped (default expected at C:\Users\<you>\vcpkg)
#
# Usage:  pwsh -File scripts\build-desktop.ps1
$ErrorActionPreference = "Stop"

$root   = (Resolve-Path "$PSScriptRoot\..").Path
$vcpkg  = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { "$env:USERPROFILE\vcpkg" }
$toolchain = "$vcpkg\scripts\buildsystems\vcpkg.cmake"

if (-not (Test-Path $toolchain)) {
    throw "vcpkg toolchain not found at $toolchain. Set `$env:VCPKG_ROOT or clone+bootstrap vcpkg."
}

# The devkitPro installer prepends its MSYS2 bin to PATH; that cmake lacks the
# Visual Studio generators and mangles Windows paths inside vcpkg. Make sure the
# native (scoop) cmake is found first for the desktop build.
$nativeCmakeBin = "$env:USERPROFILE\scoop\apps\cmake\current\bin"
if (Test-Path "$nativeCmakeBin\cmake.exe") { $env:Path = "$nativeCmakeBin;" + $env:Path }

# NOTE: vcpkg pins a baseline commit in vcpkg.json. If you cloned vcpkg shallow,
# fetch that commit first:  git -C $vcpkg fetch --depth 1 origin <baseline-sha>

Write-Host "==> Configuring desktop build..." -ForegroundColor Cyan
cmake -S $root -B "$root\build" -G "Visual Studio 18 2026" -A x64 `
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
    -DCMAKE_BUILD_TYPE=Release `
    -DBUILD_TESTING=OFF
if ($LASTEXITCODE -ne 0) { throw "configure failed" }

Write-Host "==> Building devilutionx (Release)..." -ForegroundColor Cyan
cmake --build "$root\build" --config Release --target devilutionx --parallel
if ($LASTEXITCODE -ne 0) { throw "build failed" }

Write-Host "==> Done: $root\build\Release\devilutionx.exe" -ForegroundColor Green
