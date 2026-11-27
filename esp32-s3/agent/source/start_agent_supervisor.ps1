[CmdletBinding()]
param(
    [ValidatePattern('^[a-zA-Z0-9._:-]+$')]
    [string]$CollectorHost = '255.255.255.255',
    [ValidateRange(1, 65535)] [int]$Port = 4212,
    [ValidatePattern('^[a-zA-Z0-9._:-]+$')]
    [string]$DirectHost = '10.0.80.227',
    [ValidateRange(1, 65535)] [int]$DirectPort = 4210,
    [ValidateRange(1, 65535)] [int]$DirectHttpPort = 80,
    [string]$LogPath = '',
    [ValidatePattern('\A[a-zA-Z0-9_-]+\z')]
    [string]$InstanceName = 'LEDForgeCodexLanAgentSupervisor'
)

function Start-AgentUpdateDownloader([string]$Root) {
    # ConsoleHost can allocate conhost even with CREATE_NO_WINDOW and redirected
    # handles. Run the unchanged signed-update helper in an isolated runspace.
    $worker = [PowerShell]::Create()
    try {
        [void]$worker.AddScript({
            param($Root)
            $ErrorActionPreference = 'Stop'
            try {
                . (Join-Path $Root 'agent_update.ps1')
                if (Save-AgentRemoteUpdate $Root (Join-Path $Root '.agent-update-stage')) { return 42 }
                return 0
            } catch { return 1 }
        }).AddArgument($Root)
        $task = [pscustomobject]@{ Worker = $worker; Pending = $worker.BeginInvoke(); Received = $false;
            Code = 1; Closed = $false }
        $task | Add-Member ScriptProperty HasExited { $this.Pending.IsCompleted }
        $task | Add-Member ScriptProperty ExitCode {
            if (-not $this.Pending.IsCompleted) { return $null }
            if (-not $this.Received) {
                try {
                    $result = $this.Worker.EndInvoke($this.Pending)
                    if (-not $this.Worker.HadErrors -and $result.Count -eq 1 -and $result[0] -in @(0, 1, 42)) {
                        $this.Code = [int]$result[0]
                    }
                } catch { $this.Code = 1 }
                finally { $this.Received = $true }
            }
            return $this.Code
        }
        $task | Add-Member ScriptMethod WaitForExit {
            param([int]$Milliseconds = -1)
            return $this.Pending.AsyncWaitHandle.WaitOne($Milliseconds)
        }
        $task | Add-Member ScriptMethod Kill {
            if (-not $this.Pending.IsCompleted) { $this.Worker.Stop() }
        }
        $task | Add-Member ScriptMethod Dispose {
            if ($this.Closed) { return }
            try {
                if (-not $this.Pending.IsCompleted) { $this.Worker.Stop() }
                if (-not $this.Received) {
                    try { [void]$this.Worker.EndInvoke($this.Pending) } catch { }
                    $this.Received = $true
                }
                $this.Pending.AsyncWaitHandle.Close()
            } finally { $this.Worker.Dispose(); $this.Closed = $true }
        }
        return $task
    } catch { $worker.Dispose(); throw }
}

$ErrorActionPreference = 'Stop'
$worker = Join-Path $PSScriptRoot 'codex_lan_agent.ps1'
if (-not (Test-Path -LiteralPath $worker)) { throw "Missing worker: $worker" }
if ([string]::IsNullOrWhiteSpace($LogPath)) { $LogPath = Join-Path $PSScriptRoot 'agent.log' }
$LogPath = [IO.Path]::GetFullPath($LogPath)
if ($LogPath.IndexOfAny([IO.Path]::GetInvalidPathChars()) -ge 0 -or $LogPath.Contains('"') -or $LogPath.EndsWith('\')) { throw 'Invalid log path' }
$mutex = [Threading.Mutex]::new($false, "Local\$InstanceName")
$ownsMutex = $false
$child = $null
$download = $null
$downloadStartedAt = [DateTime]::MinValue
$updateHelper = Join-Path $PSScriptRoot 'agent_update.ps1'
$updatesEnabled = $false
$pendingHealth = $false
$reloadRequired = $false
$healthDeadline = [DateTime]::MinValue
$healthySince = $null
$nextUpdatePoll = [DateTime]::MinValue
$loadedHelper = ''
$managementContext = $null
$control = Join-Path $PSScriptRoot ('.agent-worker-' + [guid]::NewGuid().ToString('N'))
$installedTray = $PSScriptRoot -eq (Join-Path $env:LOCALAPPDATA 'LED Forge\Codex LAN Agent')
try {
    try { $ownsMutex = $mutex.WaitOne(0) }
    catch [Threading.AbandonedMutexException] { $ownsMutex = $true }
    if (-not $ownsMutex) { return }
    if (Test-Path -LiteralPath $updateHelper) {
        . $updateHelper
        $loadedHelper = [IO.File]::ReadAllText($updateHelper)
        # Recover before launching any worker; no network operation precedes recovery.
        if (Test-Path -LiteralPath (Join-Path $PSScriptRoot '.agent-update-journal.json')) {
            Restore-AgentUpdate $PSScriptRoot
        }
        try { Repair-AgentGuiRecoveryTask $PSScriptRoot }
        catch { Add-Content -LiteralPath $LogPath -Value "Recovery launcher migration failed: $($_.Exception.Message)" }
        $updatesEnabled = Test-AgentUpdateReady $PSScriptRoot
    }
    $arguments = @(
        '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden'
        "-File `"$worker`""
        $(if ($installedTray) { '-EnableChat' })
        "-CollectorHost `"$CollectorHost`" -Port $Port"
        "-DirectHost `"$DirectHost`" -DirectPort $DirectPort -DirectHttpPort $DirectHttpPort"
        "-LogPath `"$LogPath`""
        $(if (Test-Path -LiteralPath $updateHelper) { "-UpdateControlPath `"$control`"" })
    ) -join ' '
    while ($true) {
        $startedProvisional = $pendingHealth
        $restartEvent = $null
        $trayRestart = $false
        try {
            if ($loadedHelper) {
                $state = Read-AgentActionState $PSScriptRoot
                if ($null -ne $state) {
                    $currentEndpoint = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'controller-endpoint.json')) | ConvertFrom-Json
                    if ($state.device -ine $currentEndpoint.Device) { $state = $null }
                }
                if ($null -ne $state -and $state.action -eq 'uninstall' -and -not $state.confirmed) {
                    $endpoint = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'controller-endpoint.json')) | ConvertFrom-Json
                    $identity = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'agent-update-identity.json')) | ConvertFrom-Json
                    $installedVersion = if ($state.PSObject.Properties['version']) { $state.version } else { Get-AgentInstalledVersion $PSScriptRoot }
                    $managementContext = [pscustomobject]@{ Version = $installedVersion; Endpoint = $endpoint; Identity = $identity }
                    if (-not $state.PSObject.Properties['version']) { $state | Add-Member NoteProperty version $installedVersion; Write-AgentActionState $PSScriptRoot $state }
                    $outcome = Invoke-AgentPendingAction $PSScriptRoot $null $control
                    if ($outcome -eq 'uninstall') { Wait-AgentUninstallAck $PSScriptRoot $managementContext $loadedHelper; return }
                } elseif ($null -ne $state -and $state.action -eq 'uninstall' -and $state.result -eq 'ok') { return }
                else { [void](Invoke-AgentPendingAction $PSScriptRoot $null $control) }
            }
            $previousRestart = $env:LED_FORGE_TRAY_RESTART
            try {
                $env:LED_FORGE_TRAY_RESTART = $null
                if ($installedTray -and $loadedHelper) {
                    $name = 'Local\LEDForgeTrayRestart-' + [guid]::NewGuid().ToString('N')
                    $acl = [Security.AccessControl.EventWaitHandleSecurity]::new()
                    $acl.AddAccessRule([Security.AccessControl.EventWaitHandleAccessRule]::new(
                        [Security.Principal.WindowsIdentity]::GetCurrent().User, 'FullControl', 'Allow'))
                    $created = $false
                    $restartEvent = [Threading.EventWaitHandle]::new($false,'ManualReset',$name,[ref]$created,$acl)
                    if (-not $created) { throw 'Worker restart event already exists' }
                    $env:LED_FORGE_TRAY_RESTART = $name
                }
                $child = Start-Process "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -ArgumentList $arguments -WorkingDirectory $PSScriptRoot -WindowStyle Hidden -RedirectStandardOutput (Join-Path $PSScriptRoot 'worker-output.log') -RedirectStandardError (Join-Path $PSScriptRoot 'worker-error.log') -PassThru
            } finally { $env:LED_FORGE_TRAY_RESTART = $previousRestart }
            while (-not $child.WaitForExit(250)) {
                if ($reloadRequired) { break }
                if ($pendingHealth) {
                    $health = "$control.health"
                    $fresh = (Test-Path -LiteralPath $health) -and ((Get-Item -LiteralPath $health).LastWriteTimeUtc -gt [DateTime]::UtcNow.AddSeconds(-8))
                    if ($fresh) {
                        if ($null -eq $healthySince) { $healthySince = [DateTime]::UtcNow }
                        if ([DateTime]::UtcNow -ge $healthySince.AddSeconds(15)) {
                            Complete-AgentUpdate $PSScriptRoot
                            $pendingHealth = $false
                            $reloadRequired = $true
                            break
                        }
                    } else { $healthySince = $null }
                    if ($pendingHealth -and [DateTime]::UtcNow -ge $healthDeadline) { break }
                    continue
                }
                if ($null -ne $download -and -not $download.HasExited -and [DateTime]::UtcNow -ge $downloadStartedAt.AddSeconds(30)) {
                    $download.Kill(); $download.WaitForExit()
                    $download.Dispose(); $download = $null
                    $nextUpdatePoll = [DateTime]::UtcNow.AddSeconds(15)
                }
                if ($null -ne $download -and $download.HasExited) {
                    $download.WaitForExit()
                    $ready = $download.ExitCode -eq 42
                    $download.Dispose(); $download = $null
                    $state = Read-AgentActionState $PSScriptRoot
                    if ($null -ne $state -and $state.result -eq 'pending') {
                        if ($state.action -eq 'uninstall') {
                            $endpoint = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'controller-endpoint.json')) | ConvertFrom-Json
                            $identity = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'agent-update-identity.json')) | ConvertFrom-Json
                            $installedVersion = Get-AgentInstalledVersion $PSScriptRoot
                            $managementContext = [pscustomobject]@{ Version = $installedVersion; Endpoint = $endpoint; Identity = $identity }
                            $state | Add-Member NoteProperty version $installedVersion -Force
                            Write-AgentActionState $PSScriptRoot $state
                        }
                        try { $outcome = Invoke-AgentPendingAction $PSScriptRoot $child $control }
                        catch { [IO.File]::Delete("$control.stop"); continue }
                        if ($outcome -eq 'uninstall') { Wait-AgentUninstallAck $PSScriptRoot $managementContext $loadedHelper; return }
                        if ($outcome -eq 'restart') { break }
                    }
                    if ($ready) {
                      try {
                        $stage = Join-Path $PSScriptRoot '.agent-update-stage'
                        $candidate = Test-AgentPackage $PSScriptRoot $stage (Get-AgentInstalledVersion $PSScriptRoot)
                        Write-AgentDurable "$control.stop" ([byte[]]@(1))
                        if (-not $child.WaitForExit(15000)) {
                            # Never install after a forced stop: a scanner may be committing state.
                            Write-AgentDurable (Join-Path $PSScriptRoot '.agent-update-failed-version.txt') ([Text.Encoding]::ASCII.GetBytes([string]$candidate.Version))
                            [IO.File]::Delete("$control.stop")
                            continue
                        }
                        Write-AgentDurable (Join-Path $PSScriptRoot '.agent-update-failed-version.txt') ([Text.Encoding]::ASCII.GetBytes([string]$candidate.Version))
                        Install-AgentStagedUpdate $PSScriptRoot $stage
                        $pendingHealth = $true
                        $healthySince = $null
                        $healthDeadline = [DateTime]::UtcNow.AddSeconds(60)
                        break
                      } catch {
                        try { Add-Content -LiteralPath $LogPath -Value "Update rejected: $($_.Exception.Message)" } catch { }
                        if ($child.HasExited) { break }
                      }
                    }
                }
                if ($restartEvent -and $restartEvent.WaitOne(0) -and $null -eq $download) {
                    Write-AgentDurable "$control.stop" ([byte[]]@(1))
                    [void]$child.WaitForExit(15000)
                    $trayRestart = $true
                    break
                }
                if ($null -eq $download -and [DateTime]::UtcNow -ge $nextUpdatePoll) {
                    $nextUpdatePoll = [DateTime]::UtcNow.AddSeconds(15)
                    if (Test-Path -LiteralPath $updateHelper) {
                        try { $updatesEnabled = Test-AgentUpdateReady $PSScriptRoot } catch { $updatesEnabled = $false }
                    }
                    if (-not $updatesEnabled) { continue }
                    try {
                        $download = Start-AgentUpdateDownloader $PSScriptRoot
                        $downloadStartedAt = [DateTime]::UtcNow
                    } catch {
                        try { Add-Content -LiteralPath $LogPath -Value "Update download launch failed: $($_.Exception.Message)" } catch { }
                    }
                }
            }
            if ($reloadRequired) {
                Write-AgentDurable "$control.stop" ([byte[]]@(1))
                if (-not $child.WaitForExit(15000)) {
                    $child.Kill()
                    if (-not $child.WaitForExit(5000)) { throw 'Cannot stop worker for supervisor reload' }
                }
                $parameters = [pscustomobject]@{ CollectorHost = $CollectorHost; Port = $Port; DirectHost = $DirectHost; DirectPort = $DirectPort; DirectHttpPort = $DirectHttpPort; LogPath = $LogPath; InstanceName = $InstanceName }
                [void](Get-AgentSupervisorArguments $parameters $PSScriptRoot)
                $encoded = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(($parameters | ConvertTo-Json -Compress)))
                $ticks = (Get-Process -Id $PID).StartTime.ToUniversalTime().Ticks
                $restartArgs = "-NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $PSScriptRoot 'agent_update_restart.ps1')`" -OldProcessId $PID -OldStartTicks $ticks -ParametersBase64 $encoded"
                $handoff = Start-Process "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -ArgumentList $restartArgs -WorkingDirectory $PSScriptRoot -WindowStyle Hidden -PassThru
                try { if ($handoff.WaitForExit(250)) { throw 'Restart helper exited before handoff' } } finally { $handoff.Dispose() }
                return
            }
            if ($pendingHealth -and -not $child.HasExited) {
                Write-AgentDurable "$control.stop" ([byte[]]@(1))
                if (-not $child.WaitForExit(15000)) { $child.Kill(); $child.WaitForExit() }
                Restore-AgentUpdate $PSScriptRoot
                $pendingHealth = $false
            } elseif ($pendingHealth -and $startedProvisional) {
                Restore-AgentUpdate $PSScriptRoot
                $pendingHealth = $false
            }
            $child.Refresh()
            $exitCode = $child.ExitCode
            if ($null -eq $exitCode) { $exitCode = 'unknown' }
            $message = if ($trayRestart) { 'Tray requested a local agent restart.' } else { "worker exited: $exitCode; restarting in 10 seconds" }
        }
        catch { $message = "worker launch failed: $($_.Exception.Message); retrying in 10 seconds" }
        finally {
            if ($restartEvent) { $restartEvent.Dispose() }
            if ($null -ne $child) {
                if (-not $child.HasExited) { $child.Kill(); $child.WaitForExit() }
                $child.Dispose(); $child = $null
            }
            foreach ($suffix in @('.stop', '.health')) { [IO.File]::Delete("$control$suffix") }
            if ($startedProvisional -and $pendingHealth) {
                Restore-AgentUpdate $PSScriptRoot
                $pendingHealth = $false
            }
            if (-not $pendingHealth -and (Test-Path -LiteralPath (Join-Path $PSScriptRoot '.agent-update-journal.json'))) { Restore-AgentUpdate $PSScriptRoot }
        }
        try { Add-Content -LiteralPath $LogPath -Value "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $message" -Encoding UTF8 }
        catch { }
        if (-not $trayRestart) { Start-Sleep -Seconds 10 }
    }
}
catch {
    try { Add-Content -LiteralPath $LogPath -Value "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') supervisor failed: $($_.Exception.Message)" -Encoding UTF8 }
    catch { }
    throw
}
finally {
    if ($null -ne $download) {
        if (-not $download.HasExited) { $download.Kill(); $download.WaitForExit() }
        $download.Dispose()
    }
    if ($null -ne $child) {
        if (-not $child.HasExited) { $child.Kill() }
        $child.Dispose()
    }
    if ($ownsMutex) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
