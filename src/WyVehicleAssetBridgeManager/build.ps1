$ErrorActionPreference = 'Stop'
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sources = @(Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.cs' -File | Sort-Object Name | ForEach-Object { $_.FullName })
$output = Join-Path $repoRoot 'WyVehicleAssetBridgeManager.exe'
$manifest = Join-Path $PSScriptRoot 'app.manifest'
& $compiler /nologo /target:winexe /platform:x64 /optimize+ /codepage:65001 "/win32manifest:$manifest" "/out:$output" /reference:System.dll,System.Core.dll,System.Windows.Forms.dll,System.Drawing.dll,System.Web.Extensions.dll,System.Management.dll @sources
if ($LASTEXITCODE -ne 0) { throw "Manager compiler failed ($LASTEXITCODE)." }
Write-Host $output
