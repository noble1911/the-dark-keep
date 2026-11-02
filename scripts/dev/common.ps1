# Dev: shared by testrun.ps1, coop.ps1, ctl.ps1 and gamectl.ps1. Only ever touches this checkout's test
# games: any other devilutionx on this PC (a real session of this same build included) must never be
# stopped or driven. A test game is one whose command line names a profile under build\test-profiles.
$script:Root = (Resolve-Path "$PSScriptRoot\..\..").Path
# DK_BUILD: another build folder of this checkout, for example build-asan.
$script:ExeDir = "$script:Root\$(if ($env:DK_BUILD) { $env:DK_BUILD } else { 'build' })\Release"
$script:Exe = "$script:ExeDir\devilutionx.exe"

function Get-ExeInstances {
    # Everything running from this checkout's exe, test or not.
    @(Get-CimInstance Win32_Process -Filter "Name='devilutionx.exe'" | Where-Object { $_.ExecutablePath -ieq $script:Exe })
}

function Get-TestGames {
    # Oldest first. The exe's own path doesn't tell: it is in every command line, a real session's too.
    @(Get-ExeInstances | Where-Object { $_.CommandLine -like "*$script:Root\build\test-profiles\*" } | Sort-Object CreationDate)
}

function Get-ProfileGames([string]$ProfileDir) {
    # The whole folder name: a profile "solo" must not match "solo2".
    $pattern = [regex]::Escape($ProfileDir) + '([\\"\s]|$)'
    @(Get-TestGames | Where-Object { $_.CommandLine -match $pattern })
}

function Stop-ProfileGames([string[]]$Names) {
    foreach ($name in $Names) {
        foreach ($game in Get-ProfileGames "$script:Root\build\test-profiles\$name") {
            Stop-Process -Id $game.ProcessId -Force -ErrorAction SilentlyContinue
            Wait-Process -Id $game.ProcessId -Timeout 10 -ErrorAction SilentlyContinue
        }
    }
}

function Sync-GameData([string]$DataDir = "E:\Games\Diablo", [switch]$NoSync) {
    # DataDir is read-only: its MPQs are hardlinked next to our exe (as scripts\run-desktop.ps1 does).
    # A running game may be reading the assets and mods, and each checkout has its own, so only this
    # build's games matter. They stay as they are if the caller says stale data is fine.
    $running = @(Get-ExeInstances)
    if ($running -and -not $NoSync) {
        throw "$($running.Count) game(s) running from this build (pid $($running.ProcessId -join ', ')) would keep assets and mods from refreshing. Stop them, or pass -NoSync to run on the data as it is."
    }
    if (-not $NoSync) {
        Copy-Item -Recurse -Force "$script:Root\build\assets\*" (New-Item -ItemType Directory -Force "$script:ExeDir\assets")
        if (Test-Path "$script:ExeDir\mods") { Remove-Item -Recurse -Force "$script:ExeDir\mods" }
        Copy-Item -Recurse -Force "$script:ExeDir\..\mods" "$script:ExeDir\mods"
    }
    foreach ($file in Get-ChildItem -Path $DataDir -Filter *.mpq) {
        $dst = Join-Path $script:ExeDir $file.Name
        if (-not (Test-Path $dst)) {
            # A hard link needs the same drive.
            try { New-Item -ItemType HardLink -Path $dst -Target $file.FullName -ErrorAction Stop | Out-Null } catch { Copy-Item $file.FullName $dst }
            Write-Host "    linked $($file.Name)"
        }
    }
}

function Get-CheckoutPort {
    # Each checkout its own port: on the default one, a co-op test joins whichever checkout's host got there first.
    $sum = 0; foreach ($c in $script:Root.ToLowerInvariant().ToCharArray()) { $sum = ($sum * 31 + [int]$c) % 20011 }
    20000 + $sum
}

function New-TestProfile([string]$Name, [int]$Width = 1280, [int]$Height = 720) {
    # A windowed profile with its own saves. Bound to localhost, so hosting raises no firewall prompt.
    $profileDir = "$script:Root\build\test-profiles\$Name"
    $port = Get-CheckoutPort
    New-Item -ItemType Directory -Force $profileDir | Out-Null
    Set-Content -Encoding ascii "$profileDir\diablo.ini" @"
[Graphics]
Fullscreen=0
Fit to Screen=0
Width=$Width
Height=$Height
[Audio]
Sound Volume=-1600
Music Volume=-1600
[StartUp]
Diablo Intro=0
Hellfire Intro=0
Splash=0
[Network]
Bind Address=127.0.0.1
Port=$port
"@
    $profileDir
}

function Start-TestGame([string]$ProfileDir, [string[]]$GameArgs, [switch]$Windowed) {
    # Without -Windowed nothing shows on the desktop and no sound plays: drive it with ctl.ps1.
    if (-not (Select-String -Path "$script:Root\build\CMakeCache.txt" -Pattern '^KEEPER_DEVCTL:BOOL=ON' -Quiet)) {
        throw "This build has no --keeper-devctl. Build with: scripts\build-desktop.ps1 -Dev (or cmake build -DKEEPER_DEVCTL=ON)"
    }
    Remove-Item -ErrorAction SilentlyContinue "$ProfileDir\ctl.txt", "$ProfileDir\ctl.txt.out"
    New-Item -ItemType File -Force "$ProfileDir\ctl.txt" | Out-Null
    $a = @('--config-dir', $ProfileDir, '--save-dir', $ProfileDir, '--keeper-devctl', "$ProfileDir\ctl.txt") + $GameArgs
    $savedVideo = $env:SDL_VIDEODRIVER; $savedAudio = $env:SDL_AUDIODRIVER
    $env:SDL_AUDIODRIVER = 'dummy'
    if (-not $Windowed) { $env:SDL_VIDEODRIVER = 'dummy' }
    if ($env:DK_CDB) {
        # Crash hunting: runs under cdb, which logs a symbolized stack to $ProfileDir\cdb.log on an access violation.
        $cdb = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe'
        $debugArgs = @('-G', '-hd', '-lines', '-logo', "`"$ProfileDir\cdb.log`"", '-c', '"sxe -c \"kn 40; q\" av; sxd eh; g"', "`"$script:Exe`"") + ($a | ForEach-Object { "`"$_`"" })
        $process = Start-Process -FilePath $cdb -WorkingDirectory $script:ExeDir -ArgumentList $debugArgs -WindowStyle Hidden -PassThru
    } else {
        # Without a window the console that would flash up stays minimised.
        $style = if ($Windowed) { 'Normal' } else { 'Minimized' }
        $process = Start-Process -FilePath $script:Exe -WorkingDirectory $script:ExeDir -ArgumentList $a -WindowStyle $style -PassThru
    }
    $env:SDL_VIDEODRIVER = $savedVideo; $env:SDL_AUDIODRIVER = $savedAudio
    # An exe older than the cache, or built without the option, quits on the flag.
    Start-Sleep -Milliseconds 800
    if ($process.HasExited) { throw "The game quit at once (exit $($process.ExitCode)). If the exe was built without KEEPER_DEVCTL, rebuild: scripts\build-desktop.ps1 -Dev" }
    $process
}
