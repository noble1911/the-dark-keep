# Dev: launch a test keep with an isolated profile (build\test-profiles\<profile>), invisibly unless
# -Windowed. Stops only the game already on that profile. Drive it with ctl.ps1.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\dev\testrun.ps1 [-Windowed] [-Name solo] [-Width 1280 -Height 720] [-Menu] [game args]
[CmdletBinding(PositionalBinding = $false)]
param(
    [switch]$Windowed,
    [string]$Name = 'solo',
    [int]$Width = 1280,
    [int]$Height = 720,
    # Start at the main menu instead of straight in the keep.
    [switch]$Menu,
    # Leave assets and mods as they are, for running beside another game from this build.
    [switch]$NoSync,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$GameArgs
)
. "$PSScriptRoot\common.ps1"
Stop-ProfileGames $Name
Sync-GameData -NoSync:$NoSync
$profileDir = New-TestProfile $Name $Width $Height
$a = @(); if (-not $Menu) { $a += '--keeper-autostart' }
if ($GameArgs) { $a += $GameArgs }
$process = Start-TestGame $profileDir $a -Windowed:$Windowed
"pid $($process.Id) profile $profileDir"
