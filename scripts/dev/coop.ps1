# Dev: launch a co-op host and a client on this machine (TCP over localhost), each with
# its own profile under build\test-profiles. Drive them with gamectl.ps1 ($env:GAMEIDX=0/1).
# Windows may show a firewall prompt; localhost play works without allowing it.
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
Get-Process devilutionx -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Copy-Item -Recurse -Force "$root\build\assets\*" (New-Item -ItemType Directory -Force "$root\build\Release\assets")
if (Test-Path "$root\build\Release\mods") { Remove-Item -Recurse -Force "$root\build\Release\mods" }
Copy-Item -Recurse -Force "$root\build\mods" "$root\build\Release\mods"
$exe = "$root\build\Release\devilutionx.exe"
foreach ($name in 'host', 'client') {
    $p = "$root\build\test-profiles\$name"
    New-Item -ItemType Directory -Force $p | Out-Null
    if (-not (Test-Path "$p\diablo.ini")) {
        Set-Content -Encoding ascii "$p\diablo.ini" "[Graphics]`r`nFullscreen=0`r`n[StartUp]`r`nDiablo Intro=0`r`nHellfire Intro=0`r`nSplash=0`r`n"
    }
}
Start-Process -FilePath $exe -WorkingDirectory "$root\build\Release" -ArgumentList @('--keeper-host', '--config-dir', "$root\build\test-profiles\host", '--save-dir', "$root\build\test-profiles\host")
Start-Sleep 6
Start-Process -FilePath $exe -WorkingDirectory "$root\build\Release" -ArgumentList @('--keeper-join', '127.0.0.1', '--config-dir', "$root\build\test-profiles\client", '--save-dir', "$root\build\test-profiles\client")
