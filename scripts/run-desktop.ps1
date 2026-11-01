# Launch the desktop build against retail Diablo data for play-testing.
#
# Usage:  powershell -ExecutionPolicy Bypass -File scripts\run-desktop.ps1 [path-to-folder-with-DIABDAT.MPQ]
# Default data dir: E:\Games\Diablo
#
# Isolation: the game data folder is treated as READ-ONLY. We do NOT pass --data-dir
# (that would make the engine load <DataDir>\mods\..., which other mods may have
# overwritten with their own item/monster tables). Instead the MPQs are hardlinked
# (or copied, if on another drive) next to our exe, and our own mods\ folder is
# refreshed from the build tree on every launch. Saves live in
# %AppData%\diasurgical\the-dark-keep\ (see Source/utils/paths.cpp).
param(
    [string]$DataDir = "E:\Games\Diablo",
    # Extra arguments for the game, e.g. --keeper-autostart (skip menus, straight into the keep).
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$GameArgs
)
$ErrorActionPreference = "Stop"
$root    = (Resolve-Path "$PSScriptRoot\..").Path
$exeDir  = "$root\build\Release"
$exe     = "$exeDir\devilutionx.exe"

if (-not (Test-Path $exe)) { throw "Desktop build not found. Run scripts\build-desktop.ps1 first." }
if (-not (Test-Path "$DataDir\DIABDAT.MPQ")) { throw "DIABDAT.MPQ not found in $DataDir" }

# The VS multi-config build copies engine assets to build\assets, but the exe in
# build\Release looks for build\Release\assets. Mirror them every launch (cheap, avoids stale data).
Write-Host "==> Syncing engine assets + mod data next to the exe..." -ForegroundColor Cyan
Copy-Item -Recurse -Force "$root\build\assets\*" (New-Item -ItemType Directory -Force "$exeDir\assets")
if (Test-Path "$exeDir\mods") { Remove-Item -Recurse -Force "$exeDir\mods" }
Copy-Item -Recurse -Force "$root\build\mods" "$exeDir\mods"

# Game MPQs: hardlink (same volume) or copy, once.
Get-ChildItem -Path $DataDir -Filter *.mpq | ForEach-Object {
    $dst = Join-Path $exeDir $_.Name
    if (-not (Test-Path $dst)) {
        try {
            New-Item -ItemType HardLink -Path $dst -Target $_.FullName | Out-Null
        } catch {
            Copy-Item $_.FullName $dst
        }
        Write-Host "    linked $($_.Name)"
    }
}

Write-Host "==> Launching The Dark Keep" -ForegroundColor Green
if ($GameArgs) {
    Start-Process -FilePath $exe -WorkingDirectory $exeDir -ArgumentList $GameArgs
} else {
    Start-Process -FilePath $exe -WorkingDirectory $exeDir
}
