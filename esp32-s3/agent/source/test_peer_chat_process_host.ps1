#requires -Version 5.1
param(
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$Client,
    [Parameter(Mandatory)][string]$Name,
    [Parameter(Mandatory)][ValidateSet('127.0.0.2','127.0.0.3')][string]$BindAddress,
    [Parameter(Mandatory)][ValidateSet('127.0.0.2','127.0.0.3')][string]$DiscoveryAddress,
    [Parameter(Mandatory)][int]$DiscoveryPort,
    [Parameter(Mandatory)][string]$HeadlessDirectory,
    [switch]$Session
)
$ErrorActionPreference = 'Stop'
. (Join-Path $HeadlessDirectory 'agent_update.ps1')
. (Join-Path $HeadlessDirectory 'agent_chat_ipc.ps1')
if ($BindAddress -eq $DiscoveryAddress) { throw 'Distinct loopback addresses required' }
Initialize-ChatMailbox $Root
$key = Get-ChatSecret $Root '123456' $Client -Create
if (Get-ChildItem Cert:\CurrentUser\My | Where-Object Subject -ceq "CN=Codex-LAN-Peer-v1-$Client") {
    throw 'Fixture identity already exists'
}
$cert = Get-ChatPeerCertificate $Client
try {
    $options = @{BindAddress=$BindAddress; DiscoveryAddress=$DiscoveryAddress; DiscoveryPort=$DiscoveryPort; TcpPort=0}
    if ($Session) {
        $sessionHandle = Start-AgentChatSession $HeadlessDirectory "$Root/endpoint.json" $Client $Root $Name $options
        while (-not [IO.File]::Exists("$Root/stop")) {
            if (-not [IO.File]::Exists("$Root/ready.json")) {
                try { $roster = Read-ChatObject "$Root/roster.json" } catch [IO.IOException] { $roster = $null }
                if ($roster.self_fingerprint) {
                    Write-ChatObject "$Root/ready.json" @{pid=$PID;pin=$roster.self_fingerprint;key=[Convert]::ToBase64String($key)}
                }
            }
            Start-Sleep -Milliseconds 100
        }
        return
    }
    $peer = Start-ChatPeerRuntime -Root $Root -Client $Client -Name $Name @options
    Write-ChatObject "$Root/ready.json" @{pid=$PID;pin=$peer.Pin;key=[Convert]::ToBase64String($key)}
    $ctx = @{Client=$Client;Name=$Name;Seq=0L;Peer=$peer}
    while (-not [IO.File]::Exists("$Root/stop")) {
        if ([IO.File]::Exists("$Root/retry.json")) {
            $m = Read-ChatObject "$Root/retry.json"
            [IO.File]::Delete("$Root/retry.json")
            try {
                $receipt = Send-ChatPeerMessage $peer $m.to $m.id $m.text
                Write-ChatObject "$Root/reply.json" @{receipt=$receipt}
            } catch { Write-ChatObject "$Root/reply.json" @{error=$_.Exception.Message} }
        }
        if ([IO.File]::Exists("$Root/use-session")) {
            Stop-ChatPeerRuntime $peer
            $peer = $null
            $sessionHandle = Start-AgentChatSession $HeadlessDirectory "$Root/endpoint.json" $Client $Root $Name $options
            while (-not [IO.File]::Exists("$Root/stop")) { Start-Sleep -Milliseconds 100 }
            return
        }
        try { Invoke-ChatCycle $Root "$Root/endpoint.json" $ctx }
        catch { [Console]::Error.WriteLine($_.Exception.Message) }
        Start-Sleep -Milliseconds 150
    }
} finally {
    try { Stop-AgentChatSession $sessionHandle; Stop-ChatPeerRuntime $peer }
    finally { Remove-Item -LiteralPath ("Cert:\CurrentUser\My\" + $cert.Thumbprint) -DeleteKey }
}
