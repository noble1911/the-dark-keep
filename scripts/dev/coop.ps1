# Dev: launch a co-op host and a client on this machine (TCP over localhost), each with its own
# profile under build\test-profiles, invisibly unless -Windowed. Stops only the games already on those two
# profiles. Drive them with ctl.ps1 (-Name host / -Name client).
param([switch]$Windowed, [switch]$NoSync, [int]$JoinDelay = 6, [switch]$HostOnly, [string[]]$HostArgs = @(), [string[]]$ClientArgs = @())
. "$PSScriptRoot\common.ps1"
Stop-ProfileGames 'host', 'client'
Sync-GameData -NoSync:$NoSync
$hostProfile = New-TestProfile 'host'
$clientProfile = New-TestProfile 'client'
$h = Start-TestGame $hostProfile (@('--keeper-host') + $HostArgs) -Windowed:$Windowed
"host pid $($h.Id)"
if (-not $HostOnly) {
    Start-Sleep $JoinDelay
    $c = Start-TestGame $clientProfile (@('--keeper-join', '127.0.0.1') + $ClientArgs) -Windowed:$Windowed
    "client pid $($c.Id)"
}
