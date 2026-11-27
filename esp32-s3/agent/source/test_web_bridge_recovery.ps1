param($TempRoot,$Bridge,$Chat)
$ErrorActionPreference = 'Stop'
$old = Join-Path $TempRoot 'old'
$root = Join-Path $TempRoot 'test'
Copy-Item -LiteralPath $old -Destination $root -Recurse
$helper = Join-Path $root 'agent_update.ps1'
$journal = Join-Path $root '.agent-update-journal.json'
. $helper
$oldFiles = @{}
foreach ($name in Get-AgentCodeFiles) { $oldFiles[$name] = [IO.File]::ReadAllBytes((Join-Path $old $name)) }
$bridgeFiles = (Test-AgentPackage $root $Bridge 20261123090908).Files
if ($bridgeFiles.Count -ne 8) { throw 'Wrong bridge count' }
function Assert-Files($RootPath,$Expected) {
    foreach ($name in $Expected.Keys) {
        $actual = [IO.File]::ReadAllBytes((Join-Path $RootPath $name))
        if ([Convert]::ToBase64String($actual) -cne [Convert]::ToBase64String($Expected[$name])) { throw "Wrong $name in $RootPath" }
    }
    foreach ($name in @('agent_chat_ipc.ps1','agent_chat_ui.ps1')) {
        if (-not $Expected.ContainsKey($name) -and [IO.File]::Exists((Join-Path $RootPath $name))) { throw "Unexpected $name" }
    }
}
# Snapshot every journaled durable write. Each simulated reboot loads the actual
# helper from the captured disk image, including either side of the helper swap.
$script:index = 0
function Invoke-WithCrashSnapshots($Action,$Expected) {
    $writer = ${function:Write-AgentDurable}
    function Write-AgentDurable([string]$Path,[byte[]]$Bytes) {
        & $writer $Path $Bytes
        if ([IO.File]::Exists($journal)) {
            $script:index++
            $copy = Join-Path $TempRoot "crash-$script:index"
            Copy-Item -LiteralPath $root -Destination $copy -Recurse
            & {
                . (Join-Path $copy 'agent_update.ps1')
                Restore-AgentUpdate $copy
                Restore-AgentUpdate $copy
                Assert-Files $copy $Expected
            }
        }
    }
    & $Action
}
Invoke-WithCrashSnapshots { Install-AgentStagedUpdate $root $Bridge } $oldFiles
Complete-AgentUpdate $root
Assert-Files $root $bridgeFiles
. $helper
$chatFiles = (Test-AgentPackage $root $Chat 20261126000000).Files
if ($chatFiles.Count -ne 10) { throw 'Wrong final count' }
Invoke-WithCrashSnapshots { Install-AgentStagedUpdate $root $Chat } $bridgeFiles
Invoke-WithCrashSnapshots { Complete-AgentUpdate $root } $chatFiles
Assert-Files $root $chatFiles

# Mixed presence independently exercises both absence bits and stale backups.
foreach ($mask in @(1,2,3)) {
    foreach ($name in $bridgeFiles.Keys) { [IO.File]::WriteAllBytes((Join-Path $root $name), $bridgeFiles[$name]) }
    $expected = $bridgeFiles.Clone()
    $bit = 1
    foreach ($name in @('agent_chat_ipc.ps1','agent_chat_ui.ps1')) {
        $path = Join-Path $root $name
        if ($mask -band $bit) {
            $expected[$name] = [byte[]]@(17,19,$mask)
            [IO.File]::WriteAllBytes($path,$expected[$name])
        } else { [IO.File]::Delete($path) }
        $bit *= 2
    }
    . $helper
    Install-AgentStagedUpdate $root $Chat
    Restore-AgentUpdate $root
    Assert-Files $root $expected
}
$future = Join-Path $TempRoot 'future'
try { Install-AgentStagedUpdate $root $future; throw 'Accepted wrong publisher' }
catch { if ($_.Exception.Message -ne 'Invalid signature') { throw } }
Copy-Item -LiteralPath (Join-Path $future 'agent-update-public.xml') -Destination $root -Force
Install-AgentStagedUpdate $root $future
Complete-AgentUpdate $root
if ((Get-AgentInstalledVersion $root) -ne 20261127120001) { throw 'Future release failed' }
Write-Host "PASS $script:index cold recovery boundaries, mixed-presence rollback, publisher rejection, later signed release"
