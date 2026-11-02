# Dev: launch a co-op host and up to three clients on this machine (TCP over localhost), each with its own
# profile under build\test-profiles (host, client, client2, client3), invisibly unless -Windowed. Stops only the
# games already on those profiles. Drive them with ctl.ps1 (-Name host / -Name client...).
# -Port keeps this set of games apart from anyone else's test games on the same machine.
param([switch]$Windowed, [int]$JoinDelay = 6, [switch]$HostOnly, [int]$Clients = 1, [int]$Port = 6112, [string[]]$HostArgs = @(), [string[]]$ClientArgs = @())
. "$PSScriptRoot\common.ps1"
$clientNames = @('client', 'client2', 'client3')
Stop-ProfileGames (@('host') + $clientNames)
Sync-GameData
$hostProfile = New-TestProfile 'host' -Port $Port
$h = Start-TestGame $hostProfile (@('--keeper-host') + $HostArgs) -Windowed:$Windowed
"host pid $($h.Id)"
if (-not $HostOnly) {
    foreach ($name in ($clientNames | Select-Object -First $Clients)) {
        Start-Sleep $JoinDelay
        $profileDir = New-TestProfile $name -Port $Port
        $address = if ($Port -eq 6112) { '127.0.0.1' } else { "127.0.0.1:$Port" }
        $c = Start-TestGame $profileDir (@('--keeper-join', $address) + $ClientArgs) -Windowed:$Windowed
        "$name pid $($c.Id)"
    }
}
