[CmdletBinding(SupportsShouldProcess)]
param()
$ErrorActionPreference = 'Stop'
$python = Get-Command python.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1
$runtime = & $python.Source -c 'import sys,tkinter; assert sys.version_info >= (3,10); print(sys.executable)'
if ($LASTEXITCODE -ne 0 -or -not $runtime) { throw 'Python 3.10+ with Tk for Windows is required.' }
$pythonw = Join-Path (Split-Path $runtime -Parent) 'pythonw.exe'
if (-not [IO.File]::Exists($pythonw)) { throw 'pythonw.exe is missing.' }
$target = Join-Path $env:LOCALAPPDATA 'Programs/LED Forge/Agent Hub'
$files = @('hub_ui.py','hub_service.py','hub_release.py','agent-update-public.xml',
    'configure_hub_firewall.ps1','configure_hub_firewall.cmd','release/manifest.json','release/package.zip')
foreach ($file in $files) {
    if (-not [IO.File]::Exists((Join-Path $PSScriptRoot $file))) { throw "Extract the complete hub archive. Missing $file" }
}
Push-Location $PSScriptRoot
try {
    & $runtime -B -c "from pathlib import Path; from hub_service import verify_release; print(verify_release(Path('release')))"
    if ($LASTEXITCODE) { throw 'Bundled agent release verification failed.' }
} finally { Pop-Location }
if (-not $PSCmdlet.ShouldProcess($target, 'Install the hub and create its desktop shortcut')) { return }
[void][IO.Directory]::CreateDirectory((Join-Path $target 'release'))
foreach ($file in $files) {
    $from = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot $file))
    $to = [IO.Path]::GetFullPath((Join-Path $target $file))
    if ($from -ine $to) { [IO.File]::Copy($from,$to,$true) }
}
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) 'Хаб агентів.lnk'))
$shortcut.TargetPath = $pythonw
$shortcut.Arguments = '-B "' + (Join-Path $target 'hub_ui.py') + '" --start'
$shortcut.WorkingDirectory = $target
$shortcut.Save()
