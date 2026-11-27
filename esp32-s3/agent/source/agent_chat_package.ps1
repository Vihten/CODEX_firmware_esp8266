function Get-AgentChatIpcInstallBytes([string]$SourceDirectory) {
    $ipcPath = Join-Path $SourceDirectory 'agent_chat_ipc.ps1'
    Assert-AgentPlainPath $ipcPath
    $encoding = [Text.UTF8Encoding]::new($true, $true)
    $source = [IO.File]::ReadAllText($ipcPath, $encoding)
    $include = ". (Join-Path `$PSScriptRoot 'agent_chat_peer.ps1')"
    $lines = [regex]::Split($source, '\r?\n')
    $includes = @($lines | Where-Object { $_ -ceq $include })
    $marker = '# Release-embedded agent_chat_peer.ps1'
    if ($includes.Count -eq 0 -and $lines -ccontains $marker) {
        return ,([IO.File]::ReadAllBytes($ipcPath))
    }
    if ($includes.Count -ne 1 -or $lines -ccontains $marker) { throw 'Expected one peer source include in agent_chat_ipc.ps1' }
    $peerPath = Join-Path $SourceDirectory 'agent_chat_peer.ps1'
    Assert-AgentPlainPath $peerPath
    $peer = [IO.File]::ReadAllText($peerPath, $encoding)
    if ([string]::IsNullOrWhiteSpace($peer)) { throw 'Empty peer source' }
    $position = [Array]::IndexOf($lines, $include)
    $lines[$position] = "$marker`n$peer"
    $combined = $lines -join "`n"
    $tokens = $null
    $errors = $null
    [void][Management.Automation.Language.Parser]::ParseInput($combined, [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw "Invalid embedded IPC source: $($errors[0].Message)" }
    return ,([byte[]]($encoding.GetPreamble() + $encoding.GetBytes($combined)))
}
