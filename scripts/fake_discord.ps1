# Project Oogie: a stand-in for the Discord app, to test Rich Presence
# (port/src/pc_discord.c) on a PC without Discord. Listens on
# \\.\pipe\discord-ipc-0, answers the handshake with a READY event and
# each command with an empty reply, and writes every frame it gets to -Out.
# Usage: powershell -File scripts\fake_discord.ps1 -Out logs\fake_discord.txt -Seconds 120
param([string]$Out = "logs\fake_discord.txt", [int]$Seconds = 120)
$ErrorActionPreference = "Stop"
"" | Set-Content $Out
$end = (Get-Date).AddSeconds($Seconds)
function Send-Frame($s, [int]$op, [string]$json) {
    $b = [Text.Encoding]::UTF8.GetBytes($json)
    $s.Write([BitConverter]::GetBytes($op), 0, 4)
    $s.Write([BitConverter]::GetBytes($b.Length), 0, 4)
    $s.Write($b, 0, $b.Length)
    $s.Flush()
}
function Read-Exact($s, [int]$n) {
    $buf = New-Object byte[] $n; $got = 0
    while ($got -lt $n) {
        $r = $s.Read($buf, $got, $n - $got)
        if ($r -le 0) { return $null }
        $got += $r
    }
    return ,$buf
}
while ((Get-Date) -lt $end) {
    $pipe = New-Object System.IO.Pipes.NamedPipeServerStream("discord-ipc-0", [System.IO.Pipes.PipeDirection]::InOut, 1,
        [System.IO.Pipes.PipeTransmissionMode]::Byte, [System.IO.Pipes.PipeOptions]::Asynchronous, 65536, 65536)
    $wait = $pipe.WaitForConnectionAsync()
    while (-not $wait.IsCompleted -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 200 }
    if (-not $wait.IsCompleted) { $pipe.Dispose(); break }
    "$(Get-Date -Format HH:mm:ss) connected" | Add-Content $Out
    try {
        while ((Get-Date) -lt $end) {
            $h = Read-Exact $pipe 8
            if (-not $h) { break }
            $op = [BitConverter]::ToInt32($h, 0); $len = [BitConverter]::ToInt32($h, 4)
            $body = if ($len -gt 0) { [Text.Encoding]::UTF8.GetString((Read-Exact $pipe $len)) } else { "" }
            "$(Get-Date -Format HH:mm:ss) op $op $body" | Add-Content $Out
            if ($op -eq 0) { Send-Frame $pipe 1 '{"cmd":"DISPATCH","evt":"READY","data":{"v":1}}' }
            else { Send-Frame $pipe 1 '{"cmd":"SET_ACTIVITY","evt":null,"data":{}}' }
        }
    } catch { "$(Get-Date -Format HH:mm:ss) error: $_" | Add-Content $Out }
    "$(Get-Date -Format HH:mm:ss) disconnected" | Add-Content $Out
    $pipe.Dispose()
}
