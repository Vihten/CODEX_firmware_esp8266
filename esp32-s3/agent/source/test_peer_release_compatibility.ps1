[CmdletBinding()]
param([string]$PreviousRelease = '')
$ErrorActionPreference = 'Stop'
if (-not $PreviousRelease) {
    $PreviousRelease = Join-Path $PSScriptRoot '../../build/agent-release-chat-20261127120000'
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('peer-release-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($fixture)
$savedData = $env:LOCALAPPDATA
try {
    $env:LOCALAPPDATA = Join-Path $fixture 'user'
    $source = Join-Path $fixture 'source'
    $tool = Join-Path $source 'tool'
    [void][IO.Directory]::CreateDirectory($tool)
    $builder = Join-Path $PSScriptRoot '../../tool/build_agent_release.ps1'
    if (-not [IO.File]::Exists($builder)) { $builder = Join-Path $PSScriptRoot 'tool/build_agent_release.ps1' }
    Copy-Item -LiteralPath $builder -Destination $tool
    . (Join-Path $PSScriptRoot 'agent_update.ps1')
    $names = @(Get-AgentCodeFiles)
    foreach ($name in $names) { [IO.File]::WriteAllText((Join-Path $source $name),"# isolated test fixture") }
    [IO.File]::WriteAllText((Join-Path $source 'agent-version.txt'),'1')
    foreach ($name in @('agent_update.ps1','agent_metadata.ps1','agent_chat_package.ps1',
        'install_codex_lan_agent.ps1','install_codex_lan_agent.cmd','update_codex_lan_agent.ps1',
        'update_codex_lan_agent.cmd','controller_rebind.ps1','rebind_to_s3.ps1','rebind_to_s3.cmd',
        'start_codex_lan_agent.cmd','uninstall_codex_lan_agent.ps1','uninstall_codex_lan_agent.cmd',
        'configure_collector_firewall.ps1','configure_collector_firewall.cmd','configure_peer_firewall.ps1',
        'configure_peer_firewall.cmd','import_agent_hub.ps1','import_agent_hub.cmd','DIRECT_PEER_INSTALL.md','OFFICE_CHAT.md','README.txt','TOKEN_ACCOUNTING_V2.md','CHAT_MAILBOX.md')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $source
    }
    foreach ($name in @('agent_chat_ipc.ps1','agent_chat_peer.ps1')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $source -Force
    }
    & {
        . (Join-Path $source 'agent_chat_ipc.ps1')
        if ((Get-Command Start-ChatPeerRuntime).ScriptBlock.File -ne (Join-Path $source 'agent_chat_peer.ps1')) {
            throw 'Peer functions missing from editable source'
        }
    }
    $out = Join-Path $fixture 'out'
    & (Join-Path $tool 'build_agent_release.ps1') -Version 20261128000000 -OutputDirectory $out
    # Run the immutable previous ten-file verifier against a fixture publisher.
    $old = [IO.Compression.ZipFile]::OpenRead((Join-Path $PreviousRelease 'package.zip'))
    try {
        $verifier = Join-Path $fixture 'previous-verifier.ps1'
        [IO.Compression.ZipFileExtensions]::ExtractToFile($old.GetEntry('agent_update.ps1'),$verifier)
        if ((Get-FileHash -LiteralPath $verifier).Hash -ne '5AA833D359EFB1A4EF7E430274F52BA72EC7F6A5E41F5D0D8CBA04ECE786EA4B') {
            throw 'Unexpected previous verifier'
        }
        . $verifier
        if ((Test-AgentPackage $PreviousRelease $PreviousRelease 0).Version -ne 20261127120000) {
            throw 'Expected the original signed chat release'
        }
        $pkg = Test-AgentPackage $out $out 20261127120000
        if ($pkg.Files.Count -ne 10 -or $pkg.Files.ContainsKey('agent_chat_peer.ps1')) { throw 'ZIP contract changed' }
        $installed = Join-Path $fixture 'old-install'
        [void][IO.Directory]::CreateDirectory($installed)
        foreach ($name in Get-AgentCodeFiles) {
            $baseline = if ($name -eq 'agent-version.txt') { '20261127120000' } else { "# baseline $name" }
            [IO.File]::WriteAllText((Join-Path $installed $name),$baseline)
        }
        Copy-Item -LiteralPath (Join-Path $out 'agent-update-public.xml') -Destination $installed
        Install-AgentStagedUpdate $installed $out
        Restore-AgentUpdate $installed
        if ((Get-AgentInstalledVersion $installed) -ne 20261127120000) { throw 'Previous verifier rollback failed' }
        Install-AgentStagedUpdate $installed $out
        Complete-AgentUpdate $installed
        foreach ($name in Get-AgentCodeFiles) {
            $bytes = [IO.File]::ReadAllBytes((Join-Path $installed $name))
            if ([Convert]::ToBase64String($bytes) -cne [Convert]::ToBase64String($pkg.Files[$name])) {
                throw "Installed bytes differ for $name"
            }
        }
    } finally { $old.Dispose() }
    $bootstrap = Join-Path $fixture 'bootstrap'
    [IO.Compression.ZipFile]::ExtractToDirectory((Join-Path $out 'bootstrap.zip'),$bootstrap)
    foreach ($name in @('configure_peer_firewall.ps1','configure_peer_firewall.cmd','DIRECT_PEER_INSTALL.md')) {
        if ((Get-FileHash (Join-Path $source $name)).Hash -ne (Get-FileHash (Join-Path $bootstrap $name)).Hash) {
            throw "Bootstrap manual helper mismatch: $name"
        }
    }
    if ([IO.File]::Exists((Join-Path $bootstrap 'agent_chat_peer.ps1'))) { throw 'Unexpected external runtime dependency' }
    . (Join-Path $bootstrap 'agent_chat_package.ps1')
    $signed = [Convert]::ToBase64String($pkg.Files['agent_chat_ipc.ps1'])
    foreach ($layout in @($source,$bootstrap)) {
        if ([Convert]::ToBase64String((Get-AgentChatIpcInstallBytes $layout)) -cne $signed) { throw 'Installer IPC differs from signed bytes' }
        & (Join-Path $layout 'install_codex_lan_agent.ps1') -WhatIf -AgentName Fixture
    }
    . (Join-Path $bootstrap 'agent_chat_ipc.ps1')
    if ((Get-Command Start-ChatPeerRuntime).ScriptBlock.File -ne (Join-Path $bootstrap 'agent_chat_ipc.ps1')) {
        throw 'Embedded peer runtime did not load'
    }
    Initialize-ChatPeerWire
    if (-not ('CodexLan.PeerWireV1' -as [type])) { throw 'Embedded wire component did not compile' }
    Write-Host 'PASS previous ten-file verifier, source/installed IPC, bootstrap firewall helper, two installer preflights'
} finally {
    $env:LOCALAPPDATA = $savedData
    $target = [IO.Path]::GetFullPath($fixture)
    $parent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    if ([IO.Path]::GetDirectoryName($target) -ne $parent -or [IO.Path]::GetFileName($target) -notmatch '^peer-release-[a-f0-9]{32}$') {
        throw 'Unexpected fixture cleanup path'
    }
    Remove-Item -LiteralPath $target -Force -Recurse
}
