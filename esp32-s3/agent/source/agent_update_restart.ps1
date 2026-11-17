[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][int]$OldProcessId,
    [Parameter(Mandatory = $true)][long]$OldStartTicks,
    [Parameter(Mandatory = $true)][ValidatePattern('\A[A-Za-z0-9+/=]+\z')][string]$ParametersBase64
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'agent_update.ps1')
$parameters = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($ParametersBase64)) | ConvertFrom-Json
$arguments = Get-AgentSupervisorArguments $parameters $PSScriptRoot
$old = $null
try { $old = [Diagnostics.Process]::GetProcessById($OldProcessId) }
catch [ArgumentException] { }
if ($null -ne $old) {
    try {
        if ($old.StartTime.ToUniversalTime().Ticks -ne $OldStartTicks) { throw 'Supervisor PID was reused' }
        if (-not $old.WaitForExit(90000)) { throw 'Old supervisor did not exit; refusing mutex race' }
    } finally { $old.Dispose() }
}
Start-Process "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -ArgumentList $arguments -WorkingDirectory $PSScriptRoot -WindowStyle Hidden
