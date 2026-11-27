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
$healthySince = $null
$nextUpdatePoll = [DateTime]::MinValue
$stopRetrySeconds = 0
$stallRetrySeconds = 0
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
        "-UpdateControlPath `"$control`""
    ) -join ' '
    while ($true) {
        $startedProvisional = $pendingHealth
        $restartEvent = $null
        $trayRestart = $false
        $ownsStop = $false
        $stalled = $false
        $restartDelay = 10
        try {
            if ($loadedHelper) {
                $guard = Open-AgentManagementGuard $PSScriptRoot
                try {
                    $state = Read-AgentActionState $PSScriptRoot
                    if ($null -ne $state) {
                        $currentEndpoint = Get-AgentManagementEndpoint $PSScriptRoot
                        if ($state.device -ine $currentEndpoint.Device -or "$($state.scope)" -cne "$($currentEndpoint.Scope)") { $state = $null }
                    }
                    if ($null -ne $state -and $state.action -eq 'uninstall' -and -not $state.confirmed) {
                        $managementContext = Get-AgentRemovalContext $PSScriptRoot $state
                        $installedVersion = $managementContext.Version
                        if (-not $state.PSObject.Properties['version']) { $state | Add-Member NoteProperty version $installedVersion; Write-AgentActionState $PSScriptRoot $state }
                        $outcome = Invoke-AgentPendingAction $PSScriptRoot $null $control
                        if ($outcome -eq 'uninstall') { Wait-AgentUninstallAck $PSScriptRoot $managementContext $loadedHelper; return }
                    } elseif ($null -ne $state -and $state.action -eq 'uninstall' -and $state.result -eq 'ok') { return }
                    else { [void](Invoke-AgentPendingAction $PSScriptRoot $null $control) }
                } finally { $guard.Dispose() }
            }
            if ([IO.File]::Exists("$control.stop")) { return }
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
            $watch = [Diagnostics.Stopwatch]::StartNew()
            $lastCheck = 0
            $lastProgress = 0
            $healthyStart = 0
            $heartbeat = 0
            if ($pendingHealth) {
                $lastHealthCheck = [DateTime]::UtcNow
                $healthDeadline = $lastHealthCheck.AddSeconds(60)
            }
            while (-not $child.WaitForExit(250)) {
                if ([IO.File]::Exists("$control.stop")) {
                    [void]$child.WaitForExit(15000)
                    return
                }
                if ($reloadRequired) { break }
                if ($pendingHealth) {
                    $now = [DateTime]::UtcNow
                    if ($now -lt $lastHealthCheck -or ($now - $lastHealthCheck).TotalSeconds -ge 8) {
                        # Missed an entire heartbeat freshness window. Revalidate after resume.
                        $healthySince = $null
                        $healthDeadline = $now.AddSeconds(60)
                    }
                    $lastHealthCheck = $now
                    $stamp = [IO.File]::GetLastWriteTimeUtc("$control.health")
                    $fresh = $stamp -gt $now.AddSeconds(-8) -and $stamp -le $now
                    if ($fresh) {
                        if ($null -eq $healthySince) { $healthySince = $now }
                        if ($now -ge $healthySince.AddSeconds(15)) {
                            Complete-AgentUpdate $PSScriptRoot
                            $pendingHealth = $false
                            $reloadRequired = $true
                            break
                        }
                    } else { $healthySince = $null }
                    if ($pendingHealth -and $now -ge $healthDeadline) { break }
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
                    $guard = Open-AgentManagementGuard $PSScriptRoot
                    try {
                        $state = Read-AgentActionState $PSScriptRoot
                        if ($null -ne $state -and $state.result -eq 'pending') {
                            if ($state.action -eq 'uninstall') {
                                $managementContext = Get-AgentRemovalContext $PSScriptRoot $state
                                $installedVersion = $managementContext.Version
                                $state | Add-Member NoteProperty version $installedVersion -Force
                                Write-AgentActionState $PSScriptRoot $state
                            }
                            try { $outcome = Invoke-AgentPendingAction $PSScriptRoot $child $control }
                            catch { [IO.File]::Delete("$control.stop"); continue }
                            if ($outcome -eq 'uninstall') { Wait-AgentUninstallAck $PSScriptRoot $managementContext $loadedHelper; return }
                            if ($child.HasExited) { [IO.File]::Delete("$control.stop") }
                            if ($outcome -eq 'restart') { break }
                        }
                        if ($ready) {
                          $installLock = $null
                          try {
                            # No other poll may replace the staged files or queue a conflicting action during the stop.
                            $installLock = Open-AgentPollLock $PSScriptRoot
                            $state = Read-AgentActionState $PSScriptRoot
                            if (($state -and -not $state.confirmed) -or ($restartEvent -and $restartEvent.WaitOne(0))) { continue }
                            $stage = Join-Path $PSScriptRoot '.agent-update-stage'
                            $hub = Get-AgentHubBinding $PSScriptRoot
                            $scopePath = Join-Path $stage '.management-scope'
                            Assert-AgentPlainPath $scopePath
                            $scope = if ([IO.File]::Exists($scopePath)) { [IO.File]::ReadAllText($scopePath) }
                            if ("$($hub.Scope)" -cne "$($scope)") { throw 'Staged update belongs to a different binding' }
                            $candidate = Test-AgentPackage $PSScriptRoot $stage (Get-AgentInstalledVersion $PSScriptRoot)
                            try {
                                Write-AgentDurable "$control.stop" ([byte[]]@(1))
                                $stopped = $child.WaitForExit(15000) -or $child.WaitForExit(15000)
                            } finally { [IO.File]::Delete("$control.stop") }
                            if (-not $stopped) {
                                $delay = [Math]::Min(120, [Math]::Max(15, 2 * $stopRetrySeconds))
                                if ($delay -ne $stopRetrySeconds) {
                                    try { Add-Content -LiteralPath $LogPath -Value "Update $($candidate.Version) deferred: worker still running after 30s. Next poll in ${delay}s." } catch {}
                                }
                                $stopRetrySeconds = $delay
                                $nextUpdatePoll = [DateTime]::UtcNow.AddSeconds($delay)
                                continue
                            }
                            Install-AgentStagedUpdate $PSScriptRoot $stage
                            $stopRetrySeconds = 0
                            $pendingHealth = $true
                            $healthySince = $null
                            break
                          } catch {
                            try { Add-Content -LiteralPath $LogPath -Value "Update rejected: $($_.Exception.Message)" } catch { }
                            if ($child.HasExited) { break }
                          } finally { if ($installLock) { $installLock.Dispose() } }
                        }
                    } finally { $guard.Dispose() }
                }
                if ($restartEvent -and $restartEvent.WaitOne(0) -and $null -eq $download) {
                    Write-AgentDurable "$control.stop" ([byte[]]@(1))
                    $ownsStop = $true
                    [void]$child.WaitForExit(15000)
                    $trayRestart = $true
                    break
                }
                # Sample local progress independently of network traffic and wall-clock changes.
                $seconds = $watch.Elapsed.TotalSeconds
                if ($seconds - $lastCheck -ge 5) {
                    if ($seconds - $lastCheck -gt 15) {
                        # Resume or a blocked supervisor operation: observe a new full window.
                        $lastProgress = $healthyStart = $seconds
                    }
                    $lastCheck = $seconds
                    $stamp = [IO.File]::GetLastWriteTimeUtc("$control.health").Ticks
                    if ([IO.File]::Exists("$control.health") -and $stamp -ne $heartbeat) {
                        if ($heartbeat -eq 0 -or $seconds - $lastProgress -gt 15) { $healthyStart = $seconds }
                        $lastProgress = $seconds
                        $heartbeat = $stamp
                        if ($seconds - $healthyStart -ge 300) { $stallRetrySeconds = 0 }
                    }
                }
                if ($null -eq $download -and $lastCheck - $lastProgress -ge 300) {
                    if ([IO.File]::Exists("$control.stop")) { return }
                    # The existing finally block joins the owned worker before relaunch.
                    try { [IO.File]::Open("$control.stop", [IO.FileMode]::CreateNew).Dispose() }
                    catch [IO.IOException] { if ([IO.File]::Exists("$control.stop")) { return }; throw }
                    $ownsStop = $true
                    [void]$child.WaitForExit(15000)
                    $stalled = $true
                    $stallRetrySeconds = [Math]::Min(120, [Math]::Max(10, 2 * $stallRetrySeconds))
                    $restartDelay = $stallRetrySeconds
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
            if (-not $ownsStop -and [IO.File]::Exists("$control.stop")) { return }
            if ($reloadRequired) {
                Write-AgentDurable "$control.stop" ([byte[]]@(1))
                $ownsStop = $true
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
                $ownsStop = $true
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
            $message = if ($stalled) { "Worker heartbeat stalled for 300 seconds; restarting in $restartDelay seconds" } elseif ($trayRestart) { 'Tray requested a local agent restart.' } else { "worker exited: $exitCode; restarting in 10 seconds" }
        }
        catch { $message = "worker launch failed: $($_.Exception.Message); retrying in 10 seconds" }
        finally {
            if ($restartEvent) { $restartEvent.Dispose() }
            if ($null -ne $child) {
                if (-not $child.HasExited) { $child.Kill(); $child.WaitForExit() }
                $child.Dispose(); $child = $null
            }
            if ($ownsStop) { [IO.File]::Delete("$control.stop") }
            [IO.File]::Delete("$control.health")
            if ($startedProvisional -and $pendingHealth) {
                Restore-AgentUpdate $PSScriptRoot
                $pendingHealth = $false
            }
            if (-not $pendingHealth -and (Test-Path -LiteralPath (Join-Path $PSScriptRoot '.agent-update-journal.json'))) { Restore-AgentUpdate $PSScriptRoot }
        }
        try { Add-Content -LiteralPath $LogPath -Value "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $message" -Encoding UTF8 }
        catch { }
        if (-not $trayRestart) { Start-Sleep -Seconds $restartDelay }
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
    if ($ownsMutex) {
        try { [IO.File]::Delete("$control.stop") } finally { $mutex.ReleaseMutex() }
    }
    $mutex.Dispose()
}
