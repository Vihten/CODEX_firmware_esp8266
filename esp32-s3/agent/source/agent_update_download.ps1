param([Parameter(Mandatory = $true)][string]$Root)
$ErrorActionPreference = 'Stop'
. (Join-Path $Root 'agent_update.ps1')
try {
    if (Save-AgentRemoteUpdate $Root (Join-Path $Root '.agent-update-stage')) { exit 42 }
    exit 0
} catch { Write-Error $_ -ErrorAction Continue; exit 1 }
