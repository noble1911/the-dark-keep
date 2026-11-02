# Dev: sends commands to a game started by testrun.ps1 or coop.ps1 and prints its new output.
# Commands (one per argument or per line of -File): see Execute() in Source/keeper/keeper_devctl.cpp.
#   ctl.ps1 -Name solo "key Return" "wait 500" "shot C:\tmp\a.png" "state"
#   ctl.ps1 -Name solo -Stop     stops that profile's game
[CmdletBinding(PositionalBinding = $false)]
param([string]$Name = 'solo', [string]$File, [int]$Timeout = 60, [switch]$Stop, [Parameter(ValueFromRemainingArguments = $true)][string[]]$Commands)
. "$PSScriptRoot\common.ps1"
$profileDir = "$script:Root\build\test-profiles\$Name"
if ($Stop) {
    foreach ($game in Get-ProfileGames $profileDir) { Stop-Process -Id $game.ProcessId -Force; "stopped $($game.ProcessId)" }
    return
}
$lines = @(); if ($File) { $lines += Get-Content $File }; if ($Commands) { $lines += $Commands }
$out = "$profileDir\ctl.txt.out"
$before = if (Test-Path $out) { (Get-Content $out).Count } else { 0 }
$mark = "done-$([guid]::NewGuid().ToString('N').Substring(0,8))"
# The game reads this file every frame, and a write can lose to that.
$written = $false
foreach ($try in 1..20) {
    try { Add-Content -Encoding ascii "$profileDir\ctl.txt" (($lines + "echo $mark") -join "`n") -ErrorAction Stop; $written = $true; break } catch { Start-Sleep -Milliseconds 50 }
}
if (-not $written) { throw "Could not write to $profileDir\ctl.txt" }
$deadline = (Get-Date).AddSeconds($Timeout)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 200
    if ((Test-Path $out) -and (Select-String -Path $out -SimpleMatch $mark -Quiet)) { break }
    if (-not (Get-ProfileGames $profileDir)) { "GAME NOT RUNNING"; break }
}
if ((Get-Date) -ge $deadline) { "TIMED OUT" }
if (Test-Path $out) { Get-Content $out | Select-Object -Skip $before | Where-Object { $_ -notlike "*$mark*" } }
