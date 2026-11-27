$ErrorActionPreference = 'Stop'
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('lfau-build-' + [guid]::NewGuid().ToString('N'))
$previousLocalAppData = $env:LOCALAPPDATA
try {
    $env:LOCALAPPDATA = Join-Path $testRoot 'local'
    $output = Join-Path $testRoot 'release'
    $builder = Join-Path $PSScriptRoot '../../tool/build_agent_release.ps1'
    if (-not (Test-Path -LiteralPath $builder -PathType Leaf)) {
        $builder = Join-Path $PSScriptRoot 'tool/build_agent_release.ps1'
    }
    & $builder -Version 20261015144138 -OutputDirectory $output
    if (-not $?) { throw 'First build failed' }
    $publicHash = (Get-FileHash -LiteralPath (Join-Path $output 'agent-update-public.xml')).Hash
    & $builder -Version 20261015144139 -OutputDirectory $output
    if (-not $? -or (Get-FileHash -LiteralPath (Join-Path $output 'agent-update-public.xml')).Hash -cne $publicHash) { throw 'Repeat build did not reuse publisher key' }
    . (Join-Path $PSScriptRoot 'agent_update.ps1')
    $package = Test-AgentPackage $output $output 0
    if ($package.Version -ne 20261015144139 -or (Get-Item -LiteralPath (Join-Path $output 'package.zip')).Length -gt 262144) { throw 'Release exceeds controller cap or wrong version' }
    $expected = @('agent-update-public.xml', 'agent_release.hpp', 'manifest.json', 'package.zip', 'bootstrap.zip')
    if ((((Get-ChildItem -LiteralPath $output).Name | Sort-Object) -join ',') -cne (($expected | Sort-Object) -join ',')) { throw 'Unexpected release artifact' }
    $header = [IO.File]::ReadAllText((Join-Path $output 'agent_release.hpp'))
    if ($header -cne "#pragma once`n#include <stdint.h>`nnamespace agent_release { constexpr uint64_t version = 20261015144139; }`n") { throw 'Header contract mismatch' }
    $key = Join-Path $env:LOCALAPPDATA 'LED Forge/Agent Publisher/agent-update-private.xml'
    if (-not (Test-Path -LiteralPath $key) -or -not (Get-Acl -LiteralPath $key).AreAccessRulesProtected -or @((Get-Acl -LiteralPath $key).Access).Count -ne 1) { throw 'Private publisher key location/ACL mismatch' }
    $bootstrap = [IO.Compression.ZipFile]::OpenRead((Join-Path $output 'bootstrap.zip'))
    try {
        if (@($bootstrap.Entries | Where-Object { $_.FullName -match 'private|identity|metadata\.json|config\.txt|weekly-token-state' }).Count) { throw 'Bootstrap contains private key/state' }
        if (@($bootstrap.Entries | Where-Object { $_.FullName -like 'setup_agent_updates.*' }).Count) { throw 'Bootstrap contains obsolete enrollment setup' }
        foreach ($name in @((Get-AgentCodeFiles)) + @('agent-update-public.xml')) {
            $entry = $bootstrap.GetEntry($name)
            if ($null -eq $entry) { throw "Bootstrap missing $name" }
            $memory = [IO.MemoryStream]::new()
            $stream = $entry.Open()
            try {
                $stream.CopyTo($memory)
                $expectedBytes = if ($name -eq 'agent-update-public.xml') { [IO.File]::ReadAllBytes((Join-Path $output $name)) } else { $package.Files[$name] }
                if ([Convert]::ToBase64String($memory.ToArray()) -cne [Convert]::ToBase64String($expectedBytes)) { throw "Bootstrap mismatch $name" }
            } finally { $stream.Dispose(); $memory.Dispose() }
        }
        $bootstrapRoot = Join-Path $testRoot 'bootstrap'
        [IO.Compression.ZipFile]::ExtractToDirectory((Join-Path $output 'bootstrap.zip'), $bootstrapRoot)
        $installer = Join-Path $bootstrapRoot 'install_codex_lan_agent.ps1'
        $command = Get-Command $installer
        if ($command.Parameters.ContainsKey('AgentUpdateKey')) { throw 'Installer still accepts operator key' }
        & $installer -AgentName 'Bootstrap test' -WhatIf
        if (-not $?) { throw 'Bootstrap installer preflight failed without setup scripts' }
        $installed = Join-Path $env:LOCALAPPDATA 'LED Forge/Codex LAN Agent'
        [void][IO.Directory]::CreateDirectory($installed)
        $metadataPath = Join-Path $installed 'agent-metadata.json'
        $configPath = Join-Path $installed 'agent-config.txt'
        '{"Name":"Existing agent","ColorHex6":"123456"}' | Set-Content -LiteralPath $metadataPath
        @('CollectorHost=192.0.2.1','Port=4212','DirectHost=192.0.2.2','DirectPort=4210','DirectHttpPort=80') | Set-Content -LiteralPath $configPath
        $before = @(Get-FileHash -LiteralPath $metadataPath,$configPath)
        & (Join-Path $bootstrapRoot 'update_codex_lan_agent.ps1') -WhatIf
        $after = @(Get-FileHash -LiteralPath $metadataPath,$configPath)
        if (($before.Hash -join ',') -cne ($after.Hash -join ',') -or @(Get-ChildItem -Force $installed).Count -ne 2) { throw 'Manual update preflight changed the fixture' }
        $missingChat = Join-Path $bootstrapRoot 'agent_chat_ui.ps1'
        [IO.File]::Move($missingChat, "$missingChat.saved")
        try {
            $rejected = $false
            try { & (Join-Path $bootstrapRoot 'update_codex_lan_agent.ps1') -WhatIf }
            catch { $rejected = $_.Exception.Message -like 'Missing agent package file: agent_chat_ui.ps1.*' }
            if (-not $rejected) { throw 'Incomplete chat bootstrap passed preflight' }
        } finally { [IO.File]::Move("$missingChat.saved", $missingChat) }
        $manual = [IO.File]::ReadAllText((Join-Path $bootstrapRoot 'README.txt'))
        if ($manual -match 'setup_agent_updates|AgentUpdateKey|&key=|Read-Host\s+-AsSecureString') { throw 'Bootstrap docs contain obsolete key/setup instructions' }
    } finally { $bootstrap.Dispose() }
    Write-Host 'PASS: real signing/repeat key reuse, five artifacts, manual update preflight, matching bootstrap public key and generated release version/code, 14-digit header, private key ACL and 256KB cap. No package execution.'
} finally {
    $env:LOCALAPPDATA = $previousLocalAppData
    $full = [IO.Path]::GetFullPath($testRoot)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $full.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe cleanup' }
    if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force }
}
