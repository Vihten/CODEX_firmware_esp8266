[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PriorDirectory,
    [Parameter(Mandatory = $true)][string]$ReleaseDirectory
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$helperPath = Join-Path ([IO.Path]::GetTempPath()) ("lfau-prior-" + [guid]::NewGuid() + ".ps1")
try {
    $zip = [IO.Compression.ZipFile]::OpenRead((Join-Path $PriorDirectory 'package.zip'))
    try {
        [IO.Compression.ZipFileExtensions]::ExtractToFile($zip.GetEntry('agent_update.ps1'), $helperPath)
    } finally { $zip.Dispose() }
    # Exact helper shipped in signed release 20261123090908.
    if ((Get-FileHash -LiteralPath $helperPath).Hash -ne '446D5E0D5C7B63164B06D7230C4DBF4640A1623F53772FC3D794BBDEEBACB661') {
        throw 'Unexpected prior updater'
    }
    . $helperPath
    $prior = Test-AgentPackage $PriorDirectory $PriorDirectory 0
    if ($prior.Version -ne 20261123090908 -or $prior.Files.Count -ne 8) { throw 'Wrong baseline' }
    try {
        $null = Test-AgentPackage $PriorDirectory $ReleaseDirectory $prior.Version
        throw 'Prior updater unexpectedly accepted the expanded package'
    } catch {
        if ($_.Exception.Message -ne 'ZIP entry rejected') { throw }
    }
    . (Join-Path $PSScriptRoot 'agent_update.ps1')
    $current = Test-AgentPackage $PriorDirectory $ReleaseDirectory $prior.Version
    Write-Host "PASS: baseline verifier accepts its own archive and rejects the new entry set. New verifier accepts $($current.Version) using the prior publisher public key."
} finally {
    [IO.File]::Delete($helperPath)
}
