# Dev: launch a windowed test keep with an isolated profile (build\test-profiles\solo).
# Usage: powershell -ExecutionPolicy Bypass -File scripts\dev\testrun.ps1 [extra game args]
# Needs the MPQs linked once via scripts\run-desktop.ps1.
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$profile = "$root\build\test-profiles\solo"
New-Item -ItemType Directory -Force $profile | Out-Null
if (-not (Test-Path "$profile\diablo.ini")) {
    Set-Content -Encoding ascii "$profile\diablo.ini" "[Graphics]`r`nFullscreen=0`r`n[StartUp]`r`nDiablo Intro=0`r`nHellfire Intro=0`r`nSplash=0`r`n"
}
Get-Process devilutionx -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Copy-Item -Recurse -Force "$root\build\assets\*" (New-Item -ItemType Directory -Force "$root\build\Release\assets")
if (Test-Path "$root\build\Release\mods") { Remove-Item -Recurse -Force "$root\build\Release\mods" }
Copy-Item -Recurse -Force "$root\build\mods" "$root\build\Release\mods"
$a = @('--keeper-autostart', '--config-dir', $profile, '--save-dir', $profile) + $args
Start-Process -FilePath "$root\build\Release\devilutionx.exe" -WorkingDirectory "$root\build\Release" -ArgumentList $a
