[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][long]$Version,
    [string]$OutputDirectory = '',
    [string]$PublisherDirectory = ''
)
$ErrorActionPreference = 'Stop'
if (-not $OutputDirectory) { $OutputDirectory = Join-Path (Split-Path $PSScriptRoot -Parent) 'build/agent-release' }
if ($Version -lt 1 -or $Version -gt 9007199254740991) { throw 'Version must be a positive safe integer' }
$source = Join-Path (Split-Path $PSScriptRoot -Parent) 'tools/codex-lan-agent'
if (-not (Test-Path -LiteralPath $source -PathType Container)) {
    # The standalone release source ships this tool one directory below its scripts.
    $source = Split-Path $PSScriptRoot -Parent
}
. (Join-Path $source 'agent_update.ps1')
. (Join-Path $source 'agent_chat_package.ps1')
$publisher = if ($PublisherDirectory) { $PublisherDirectory } else { Join-Path $env:LOCALAPPDATA 'LED Forge/Agent Publisher' }
$privatePath = Join-Path $publisher 'agent-update-private.xml'
Assert-AgentPlainPath $publisher
if ($PublisherDirectory -and -not (Test-Path -LiteralPath $privatePath -PathType Leaf)) { throw 'Explicit publisher key was not found' }
[void][IO.Directory]::CreateDirectory($publisher)
$acl = [IO.Directory]::GetAccessControl($publisher, [Security.AccessControl.AccessControlSections]::Access)
$acl.SetAccessRuleProtection($true, $false)
foreach ($access in @($acl.Access)) { [void]$acl.RemoveAccessRuleAll($access) }
$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User
$acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow'))
[IO.Directory]::SetAccessControl($publisher, $acl)
$rsa = [Security.Cryptography.RSACryptoServiceProvider]::new(3072)
$rsa.PersistKeyInCsp = $false
try {
    Assert-AgentPlainPath $privatePath
    if (Test-Path -LiteralPath $privatePath) { $rsa.FromXmlString([IO.File]::ReadAllText($privatePath)) }
    else { Write-AgentDurable $privatePath ([Text.Encoding]::UTF8.GetBytes($rsa.ToXmlString($true))) }
    $keyAcl = [IO.File]::GetAccessControl($privatePath, [Security.AccessControl.AccessControlSections]::Access)
    $keyAcl.SetAccessRuleProtection($true, $false)
    foreach ($access in @($keyAcl.Access)) { [void]$keyAcl.RemoveAccessRuleAll($access) }
    $keyAcl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid, 'FullControl', 'Allow'))
    [IO.File]::SetAccessControl($privatePath, $keyAcl)
    if ($rsa.PublicOnly -or $rsa.KeySize -lt 2048) { throw 'Invalid publisher key' }
    Assert-AgentPlainPath $OutputDirectory
    $outputFull = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\') + '\'
    if ($privatePath.StartsWith($outputFull, [StringComparison]::OrdinalIgnoreCase) -or $outputFull.StartsWith(([IO.Path]::GetFullPath($publisher).TrimEnd('\') + '\'), [StringComparison]::OrdinalIgnoreCase)) { throw 'Output must not contain publisher keys' }
    [void][IO.Directory]::CreateDirectory($OutputDirectory)
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zipPath = Join-Path $OutputDirectory 'package.zip'
    $temp = "$zipPath.new"
    Assert-AgentPlainPath $temp
    $file = [IO.FileStream]::new($temp, [IO.FileMode]::Create)
    $zip = [IO.Compression.ZipArchive]::new($file, [IO.Compression.ZipArchiveMode]::Create, $true)
    try {
        $total = 0L
        foreach ($name in Get-AgentCodeFiles) {
            $path = Join-Path $source $name
            Assert-AgentPlainPath $path
            if ($name.EndsWith('.ps1')) {
                $tokens = $null; $parseErrors = $null
                [void][System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$parseErrors)
                if ($parseErrors.Count) { throw "Invalid release script: $name" }
            }
            $bytes = if ($name -eq 'agent-version.txt') { [Text.Encoding]::ASCII.GetBytes("$Version`n") } elseif ($name -eq 'agent_chat_ipc.ps1') { Get-AgentChatIpcInstallBytes $source } else { [IO.File]::ReadAllBytes($path) }
            if ($bytes.Length -eq 0 -or [Array]::IndexOf($bytes, [byte]0) -ge 0) { throw "Empty or NUL-containing release source: $name" }
            $total += $bytes.Length
            if ($total -gt 2097152) { throw 'Expanded package exceeds 2MB' }
            $entry = $zip.CreateEntry($name, [IO.Compression.CompressionLevel]::Optimal)
            $stream = $entry.Open()
            try { $stream.Write($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
        }
    } finally { $zip.Dispose(); $file.Flush($true); $file.Dispose() }
    $size = (Get-Item -LiteralPath $temp).Length
    if ($size -gt 524288) { throw 'Compressed package exceeds 512KB' }
    $hash = (Get-FileHash -LiteralPath $temp -Algorithm SHA256).Hash.ToLowerInvariant()
    $signature = [Convert]::ToBase64String($rsa.SignData([Text.Encoding]::UTF8.GetBytes("LFAU1`n$Version`n$size`n$hash`n"), 'SHA256'))
    Write-AgentDurable $zipPath ([IO.File]::ReadAllBytes($temp))
    [IO.File]::Delete($temp)
    $manifest = @{ schema = 1; version = $Version; size = $size; sha256 = $hash; signature = $signature } | ConvertTo-Json -Compress
    Write-AgentDurable (Join-Path $OutputDirectory 'manifest.json') ([Text.Encoding]::UTF8.GetBytes($manifest))
    Write-AgentDurable (Join-Path $OutputDirectory 'agent-update-public.xml') ([Text.Encoding]::UTF8.GetBytes($rsa.ToXmlString($false)))
    Write-AgentDurable (Join-Path $OutputDirectory 'agent_release.hpp') ([Text.Encoding]::ASCII.GetBytes("#pragma once`n#include <stdint.h>`nnamespace agent_release { constexpr uint64_t version = $Version; }`n"))
    $verified = Test-AgentPackage $OutputDirectory $OutputDirectory 0
    $manualFiles = @('install_codex_lan_agent.ps1', 'install_codex_lan_agent.cmd',
        'update_codex_lan_agent.ps1', 'update_codex_lan_agent.cmd',
        'controller_rebind.ps1', 'agent_chat_package.ps1', 'configure_peer_firewall.ps1', 'configure_peer_firewall.cmd', 'DIRECT_PEER_INSTALL.md', 'OFFICE_CHAT.md',
        'rebind_to_s3.ps1', 'rebind_to_s3.cmd', 'start_codex_lan_agent.cmd',
        'uninstall_codex_lan_agent.ps1', 'uninstall_codex_lan_agent.cmd',
        'configure_collector_firewall.ps1', 'configure_collector_firewall.cmd', 'README.txt', 'TOKEN_ACCOUNTING_V2.md', 'CHAT_MAILBOX.md')
    $bootstrapPath = Join-Path $OutputDirectory 'bootstrap.zip'
    $bootstrapTemp = "$bootstrapPath.building"
    Assert-AgentPlainPath $bootstrapTemp
    $file = [IO.FileStream]::new($bootstrapTemp, [IO.FileMode]::Create)
    $zip = [IO.Compression.ZipArchive]::new($file, [IO.Compression.ZipArchiveMode]::Create, $true)
    try {
        foreach ($name in @((Get-AgentCodeFiles)) + $manualFiles + @('agent-update-public.xml')) {
            if ($verified.Files.ContainsKey($name)) { $bytes = $verified.Files[$name] }
            elseif ($name -eq 'agent-update-public.xml') { $bytes = [Text.Encoding]::UTF8.GetBytes($rsa.ToXmlString($false)) }
            else {
                $path = Join-Path $source $name
                Assert-AgentPlainPath $path
                $bytes = [IO.File]::ReadAllBytes($path)
            }
            $entry = $zip.CreateEntry($name, [IO.Compression.CompressionLevel]::Optimal)
            $stream = $entry.Open()
            try { $stream.Write($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
        }
    } finally { $zip.Dispose(); $file.Flush($true); $file.Dispose() }
    Write-AgentDurable $bootstrapPath ([IO.File]::ReadAllBytes($bootstrapTemp))
    [IO.File]::Delete($bootstrapTemp)
    Write-Host "Signed agent release $Version generated: $OutputDirectory"
} finally { $rsa.Dispose() }
