#requires -Version 5.1
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'agent_update.ps1')
function Check($condition,$label) { if (-not $condition) { throw $label } }
function Reject([scriptblock]$operation) {
    try { & $operation | Out-Null } catch { return }
    throw 'Operation unexpectedly succeeded'
}
$tmp = Join-Path ([IO.Path]::GetTempPath()) ('agent-hub-binding-' + [guid]::NewGuid().ToString('N'))
$originalLocal = $env:LOCALAPPDATA
try {
    $env:LOCALAPPDATA = $tmp
    $root = Join-Path $tmp 'LED Forge\Codex LAN Agent'
    [void][IO.Directory]::CreateDirectory($root)
    foreach ($name in @('agent-version.txt','codex_lan_agent.ps1','agent_update.ps1','agent-update-public.xml',
                          'agent_update_download.ps1','agent_update_restart.ps1')) { [IO.File]::WriteAllText("$root/$name",'1') }
    $inv = Join-Path $tmp 'invite.json'
    $helper = Join-Path $PSScriptRoot 'import_agent_hub.ps1'
    $valid = @{Host='127.0.0.1';HttpPort=8080;Device='ABC123';Account='53197668';secret=('ba'*32);client=(Get-AgentManagementClient)}
    function Invitation($data) { [IO.File]::WriteAllText($inv,($data | ConvertTo-Json -Compress)) }
    foreach ($bad in @(@{Host='8.8.8.8'},@{Host='localhost'},@{Host='127.1'},@{Host='::1'},@{Host='169.254.1.1'},
        @{HttpPort=0},@{HttpPort=65536},@{client='bad'},@{Account=53197668},@{secret='bad'},@{extra=1})) {
        $v = $valid.Clone()
        foreach ($k in $bad.Keys) { $v[$k]=$bad[$k] }
        Invitation $v
        Reject { & $helper $inv }
        Check (-not [IO.File]::Exists("$root/hub-management.json")) 'Rejected invitation was stored'
    }
    Invitation $valid
    & (Join-Path $PSScriptRoot 'import_agent_hub.cmd') $inv
    Check ($LASTEXITCODE -eq 0) 'Manual CMD import failed'
    $old = Get-AgentHubBinding $root
    Check ($old.client -ceq $valid.client -and (Test-AgentUpdateReady $root)) 'Hub-only readiness failed'
    Reject { & $helper $inv }
    $offer = [pscustomobject]@{version=0;action='rename';action_id=12;name_hex='6465736b'}
    Save-AgentPollAction $root $offer $old.Device
    $state = Read-AgentActionState $root
    $ctx = Get-AgentRemovalContext $root $state
    $guard = Open-AgentManagementGuard $root
    try { Reject { & $helper $inv -Replace } } finally { $guard.Dispose() }
    $valid.secret = 'ac'*32
    Invitation $valid
    & $helper $inv -Replace
    Check ($null -eq (Read-AgentActionState $root)) 'Old action visible under rotated pin'
    Reject { Write-AgentActionState $root $state }
    Reject { Get-AgentRemovalContext $root $state }
    function Invoke-AgentUpdateRequest {
        param($Uri,$Body,$Maximum)
        $script:paths += ([uri]$Uri).AbsolutePath
        $e = [InvalidOperationException]::new('Synthetic rejection')
        $e.Data['AgentUpdateHttpStatus'] = 403
        throw $e
    }
    $script:paths = @()
    Reject { Save-AgentRemoteUpdate $root '' $ctx }
    Check ($paths.Count -eq 0) 'Stale removal context contacted the network'
    Reject { Save-AgentRemoteUpdate $root '' }
    Check (($paths -join ',') -ceq '/agent-update-challenge') 'Hub rejection triggered recovery or enrollment'
    [IO.File]::WriteAllText("$root/controller-endpoint.json",'{"Host":"10.0.0.1","HttpPort":80,"Device":"ABC123","Account":"53197668"}')
    [IO.File]::WriteAllText("$root/hub-management.json",'broken')
    $script:paths = @()
    Reject { Save-AgentRemoteUpdate $root '' }
    Check (-not (Test-AgentUpdateReady $root) -and $paths.Count -eq 0 -and
        -not [IO.File]::Exists("$root/agent-update-identity.json")) 'Invalid hub binding fell back to telemetry'
    [IO.File]::Delete("$root/hub-management.json")
    Reject { Save-AgentRemoteUpdate $root '' }
    Check (-not (Test-AgentUpdateReady $root) -and $paths.Count -eq 0) 'Missing imported binding fell back to telemetry'
    'PASS: local invitation validation, private ACL, readiness, exclusive import, rotation, stale action and ACK isolation, no fallback'
} finally {
    $env:LOCALAPPDATA = $originalLocal
    if ([IO.Path]::GetDirectoryName($tmp) -eq [IO.Path]::GetTempPath().TrimEnd('\') -and
        [IO.Path]::GetFileName($tmp) -cmatch '^agent-hub-binding-[0-9a-f]{32}$') {
        Remove-Item -LiteralPath $tmp -Recurse -Force
    }
}
