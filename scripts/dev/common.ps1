# Dev: shared by testrun.ps1, coop.ps1 and gamectl.ps1. Only ever touches this checkout's own game:
# another devilutionx on this PC (the user's, another worktree's) must never be stopped or driven.
$script:Root = (Resolve-Path "$PSScriptRoot\..\..").Path
$script:ExeDir = "$script:Root\build\Release"
$script:Exe = "$script:ExeDir\devilutionx.exe"

function Get-OwnGames {
    # Oldest first. The path is checked, not just the name: this script may share the PC with a game in use.
    @(Get-CimInstance Win32_Process -Filter "Name='devilutionx.exe'" |
        Where-Object { $_.ExecutablePath -ieq $script:Exe -and $_.CommandLine -like "*$script:Root*" } |
        Sort-Object CreationDate)
}

function Stop-OwnGames {
    foreach ($game in Get-OwnGames) { Stop-Process -Id $game.ProcessId -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 300
}

function Sync-GameData([string]$DataDir = "E:\Games\Diablo") {
    # DataDir is read-only: its MPQs are hardlinked next to our exe (as scripts\run-desktop.ps1 does).
    Copy-Item -Recurse -Force "$script:Root\build\assets\*" (New-Item -ItemType Directory -Force "$script:ExeDir\assets")
    if (Test-Path "$script:ExeDir\mods") { Remove-Item -Recurse -Force "$script:ExeDir\mods" }
    Copy-Item -Recurse -Force "$script:Root\build\mods" "$script:ExeDir\mods"
    Get-ChildItem -Path $DataDir -Filter *.mpq | ForEach-Object {
        $dst = Join-Path $script:ExeDir $_.Name
        if (-not (Test-Path $dst)) {
            try { New-Item -ItemType HardLink -Path $dst -Target $_.FullName | Out-Null } catch { Copy-Item $_.FullName $dst }
        }
    }
}

function New-TestProfile([string]$Name, [int]$Width = 1280, [int]$Height = 720) {
    # A windowed profile with its own saves. Bound to localhost, so hosting raises no firewall prompt.
    $profileDir = "$script:Root\build\test-profiles\$Name"
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
"@
    $profileDir
}

function Start-TestGame([string]$ProfileDir, [string[]]$GameArgs, [switch]$Windowed) {
    # Without -Windowed nothing shows on the desktop and no sound plays: drive it with ctl.ps1.
    Remove-Item -ErrorAction SilentlyContinue "$ProfileDir\ctl.txt", "$ProfileDir\ctl.txt.out"
    New-Item -ItemType File -Force "$ProfileDir\ctl.txt" | Out-Null
    $a = @('--config-dir', $ProfileDir, '--save-dir', $ProfileDir, '--keeper-devctl', "$ProfileDir\ctl.txt") + $GameArgs
    $savedVideo = $env:SDL_VIDEODRIVER; $savedAudio = $env:SDL_AUDIODRIVER
    $env:SDL_AUDIODRIVER = 'dummy'
    if (-not $Windowed) { $env:SDL_VIDEODRIVER = 'dummy' }
    # The exe's full path in the arguments is what Get-OwnGames recognises.
    if ($env:DK_CDB) {
        # Crash hunting: runs under cdb, which logs a symbolized stack to $ProfileDir\cdb.log on an access violation.
        $cdb = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe'
        $debugArgs = @('-G', '-hd', '-lines', '-logo', "`"$ProfileDir\cdb.log`"", '-c', '"sxe -c \"kn 40; q\" av; sxd eh; g"', "`"$script:Exe`"") + ($a | ForEach-Object { "`"$_`"" })
        $process = Start-Process -FilePath $cdb -WorkingDirectory $script:ExeDir -ArgumentList $debugArgs -WindowStyle Hidden -PassThru
    } else {
        $process = Start-Process -FilePath $script:Exe -WorkingDirectory $script:ExeDir -ArgumentList $a -WindowStyle Minimized -PassThru
    }
    $env:SDL_VIDEODRIVER = $savedVideo; $env:SDL_AUDIODRIVER = $savedAudio
    $process
}
