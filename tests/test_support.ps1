# Isolated black-box fixtures for WyVehicleAssetBridgeManager.exe. No production implementation
# is imported, and no real application, compiler toolchain or plugin is launched.
Set-StrictMode -Version Latest

function Assert-TestTrue([bool]$Condition, [string]$Message) {
    if (!$Condition) { throw $Message }
}

function Assert-TestChild([string]$Path, [string]$Root) {
    $fullRoot = [IO.Path]::GetFullPath($Root).TrimEnd('\','/')
    $fullPath = [IO.Path]::GetFullPath($Path).TrimEnd('\','/')
    if (!$fullPath.StartsWith($fullRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Test path escaped its fixture root: $fullPath"
    }
}

function Remove-TestTree([string]$Path, [string]$Root) {
    Assert-TestChild $Path $Root
    if (!(Test-Path -LiteralPath $Path)) { return }
    $links = @(Get-Item -LiteralPath $Path -Force; Get-ChildItem -LiteralPath $Path -Force -Recurse) |
        Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }
    if (@($links).Count) { throw "Remove test junctions explicitly before deleting fixture: $Path" }
    for ($attempt = 0; $attempt -lt 5; $attempt++) {
        try { Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop; return }
        catch { if ($attempt -eq 4) { throw }; Start-Sleep -Milliseconds 150 }
    }
}

function Remove-TestJunction([string]$Path, [string]$Root) {
    Assert-TestChild $Path $Root
    if (Test-Path -LiteralPath $Path) {
        Assert-TestTrue ([bool]((Get-Item -LiteralPath $Path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) 'Refusing to unlink a normal directory.'
        [IO.Directory]::Delete([IO.Path]::GetFullPath($Path))
    }
}

function Write-TestFile([string]$Path, [string]$Content) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
    [IO.File]::WriteAllText($Path, $Content, (New-Object Text.UTF8Encoding $false))
}

function New-TestPackage([string]$Path, [string]$Target, [string]$Version, [bool]$Owned=$true) {
    $files = if ($Target -eq 'UE') {
        @('WyVehicleAssetBridge.uplugin','Binaries\Win64\UnrealEditor-WyVehicleAssetBridge.dll','Binaries\Win64\UnrealEditor.modules')
    } else { @('PackageContents.xml','Contents\Win64\WyVehicleAssetBridge.gup') }
    foreach ($relative in $files + @('version.txt')) { Write-TestFile (Join-Path $Path $relative) $Version }
    if ($Owned) { Write-TestFile (Join-Path $Path '.wy-vehicle-asset-bridge-managed.json') (@{ Product='WyVehicleAssetBridge'; Target=$Target } | ConvertTo-Json -Compress) }
}

function Read-TestVersion([string]$Package) {
    return [IO.File]::ReadAllText((Join-Path $Package 'version.txt')).Trim()
}

function Read-TestFingerprint([string]$Package) {
    $marker = Get-Content -LiteralPath (Join-Path $Package '.wy-vehicle-asset-bridge-managed.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert-TestTrue ($null -ne $marker.PSObject.Properties['SourceFingerprint']) "Package has no SourceFingerprint: $Package"
    Assert-TestTrue ($marker.SourceFingerprint -is [string] -and $marker.SourceFingerprint -match '^[0-9a-f]{64}$') "Package fingerprint is not a SHA-256 hex string: $Package"
    return $marker.SourceFingerprint
}

function Get-TestSourceFile($Fixture, [string]$Target) {
    $folder = if ($Target -eq 'UE') { 'ue_plugins' } else { '3ds_plugins' }
    $files = @(Get-ChildItem -LiteralPath (Join-Path $Fixture.Root "$folder\WyVehicleAssetBridge") -Recurse -File -Filter '*.cpp')
    Assert-TestTrue ($files.Count -gt 0) "Missing test source file for $Target."
    return $files[0].FullName
}

# Compatibility fixture for packages deployed before the product was renamed.
function New-TestLegacyPackage([string]$Path, [string]$Target, [string]$Version) {
    $files = if ($Target -eq 'UE') {
        @('WyAssetBridge.uplugin','Binaries\Win64\UnrealEditor-WyAssetBridge.dll','Binaries\Win64\UnrealEditor.modules')
    } else { @('PackageContents.xml','Contents\Win64\WyAssetBridge.gup') }
    foreach ($relative in $files + @('version.txt')) { Write-TestFile (Join-Path $Path $relative) $Version }
    Write-TestFile (Join-Path $Path '.wy-asset-bridge-managed.json') (@{ Product='WyAssetBridge'; Target=$Target } | ConvertTo-Json -Compress)
}

function ConvertTo-TestProcessArgument([string]$Value) {
    # Windows CommandLineToArgvW quoting, including a quoted trailing backslash.
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    $quoted = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $quoted = [regex]::Replace($quoted, '(\\+)$', '$1$1')
    return '"' + $quoted + '"'
}

function Invoke-TestProcess([string]$FileName, [string[]]$Arguments, [string]$WorkingDirectory, [int]$TimeoutSeconds=45) {
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $FileName
    $start.Arguments = ($Arguments | ForEach-Object { ConvertTo-TestProcessArgument $_ }) -join ' '
    $start.WorkingDirectory = $WorkingDirectory
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $start
    try {
        Assert-TestTrue ($process.Start()) "Cannot start test process: $FileName"
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit($TimeoutSeconds * 1000)) {
            $process.Kill()
            throw "Test process timed out: $FileName"
        }
        [pscustomobject]@{ ExitCode=$process.ExitCode; Output=($stdout.Result + $stderr.Result) }
    } finally { $process.Dispose() }
}

function Invoke-TestBuild($Fixture, [string[]]$Arguments=@(), [switch]$BuildAlias) {
    # Work from the caller's directory, not the private repository: default INI
    # resolution must be based on the single executable's own location.
    $command = if ($BuildAlias) { 'build' } else { 'publish' }
    return Invoke-TestProcess (Join-Path $Fixture.Root 'WyVehicleAssetBridgeManager.exe') (@($command) + $Arguments) (Get-Location).ProviderPath
}

function Set-TestRun($Fixture, [string[]]$Targets=@('UE','Max'), [string]$Version='new', [string]$Failure='', [string]$Missing='', [string]$ExpectedLatest='') {
    $events = Join-Path $Fixture.Root 'events'
    if (Test-Path -LiteralPath $events) { Remove-TestTree $events $Fixture.Root }
    New-Item -ItemType Directory -Path $events -Force | Out-Null
    $plan = @{ Targets=$Targets; Version=$Version; Failure=$Failure; Missing=$Missing; ExpectedLatest=$ExpectedLatest }
    Write-TestFile (Join-Path $Fixture.Root 'run.json') ($plan | ConvertTo-Json)
}

function Write-TestConfiguration($Fixture, [string]$Target='Both', [bool]$ExplicitToolchain=$true) {
    $text = ''
    if ($Target -in @('Both','UE')) {
        $text += "[UE]`r`nProjectPath=$($Fixture.Project)`r`nEnginePath=$($Fixture.Engine)`r`n`r`n"
    }
    if ($Target -in @('Both','Max')) {
        $text += "[Max]`r`nInstallPath=$($Fixture.Max)`r`nSdkPath=$($Fixture.MaxSdk)`r`n`r`n[Build]`r`nCMakePath=$($Fixture.CMake)`r`nCMakeGenerator=Visual Studio 17 2022`r`nCMakeToolset=v143`r`nWindowsSdkVersion=10.0.22621.0`r`nMaxConfiguration=Release`r`n"
        if ($ExplicitToolchain) { $text += "VisualStudioPath=$($Fixture.VisualStudio)`r`nWindowsSdkPath=$($Fixture.WindowsSdk)`r`n" }
    }
    Write-TestFile $Fixture.Config $text
}

function New-TestBridgeFixture([string]$Parent, [string]$Name, [string]$RepositoryRoot, [string]$AssemblyPath='') {
    if (!$AssemblyPath) { $AssemblyPath = Join-Path $RepositoryRoot 'WyVehicleAssetBridgeManager.exe' }
    $root = Join-Path $Parent $Name
    Assert-TestChild $root $Parent
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    Copy-Item -LiteralPath $AssemblyPath -Destination (Join-Path $root 'WyVehicleAssetBridgeManager.exe')
    foreach ($folder in @('ue_plugins','3ds_plugins')) {
        New-Item -ItemType Directory -Path (Join-Path $root $folder) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $RepositoryRoot "$folder\WyVehicleAssetBridge") -Destination (Join-Path $root $folder) -Recurse
    }
    $fixture = [pscustomobject]@{
        Root=$root; Config=(Join-Path $root 'build_and_deploy.ini')
        Project=(Join-Path $root 'fake project'); Engine=(Join-Path $root 'fake engine')
        Max=(Join-Path $root 'fake max'); MaxSdk=(Join-Path $root 'fake max sdk')
        CMake=(Join-Path $root 'fake cmake\cmake.exe')
        VisualStudio=(Join-Path $root 'fake Visual Studio'); WindowsSdk=(Join-Path $root 'fake Windows Kits\10')
        LatestUE=(Join-Path $root 'ue_plugins\build\latest\WyVehicleAssetBridge')
        LatestMax=(Join-Path $root '3ds_plugins\build\latest\WyVehicleAssetBridge')
        DeployUE=(Join-Path $root 'fake project\Plugins\WyVehicleAssetBridge')
        DeployMax=(Join-Path $root 'fake max\ApplicationPlugins\WyVehicleAssetBridge')
    }
    Write-TestFile (Join-Path $fixture.Project 'WangYue.uproject') '{"FileVersion":3,"EngineAssociation":"5.8"}'
    Write-TestFile (Join-Path $fixture.Engine 'Engine\Build\Build.version') '{"MajorVersion":5,"MinorVersion":8}'
    Write-TestFile (Join-Path $fixture.Engine 'Engine\Binaries\Win64\UnrealEditor.exe') 'fake editor is never launched'
    foreach ($relative in @('include\max.h','include\gup.h','lib\x64\Release\core.lib','lib\x64\Release\maxutil.lib','lib\x64\Release\gup.lib')) {
        Write-TestFile (Join-Path $fixture.MaxSdk $relative) 'fake Max SDK file'
    }
    Write-TestFile (Join-Path $fixture.MaxSdk 'include\maxversion.h') "#define MAX_VERSION_MAJOR 27`n#define MAX_PRODUCT_YEAR_NUMBER 2025`n"
    Write-TestFile (Join-Path $fixture.MaxSdk 'include\plugapi.h') "#define MAX_RELEASE 27000`n"
    foreach ($relative in @('Include\10.0.22621.0\um\Windows.h','Include\10.0.22621.0\shared\sdkddkver.h','Include\10.0.22621.0\ucrt\stdio.h','Lib\10.0.22621.0\um\x64\kernel32.lib','Lib\10.0.22621.0\ucrt\x64\ucrt.lib','bin\10.0.22621.0\x64\rc.exe')) {
        Write-TestFile (Join-Path $fixture.WindowsSdk $relative) 'fake Windows SDK file'
    }
    foreach ($relative in @('VC\Auxiliary\Build\vcvarsall.bat','MSBuild\Microsoft\VC\v170\Platforms\x64\PlatformToolsets\v143\Toolset.props','VC\Tools\MSVC\14.38.33130\bin\Hostx64\x64\cl.exe','VC\Tools\MSVC\14.38.33130\bin\Hostx64\x64\link.exe','VC\Tools\MSVC\14.38.33130\include\vcruntime.h','VC\Tools\MSVC\14.38.33130\lib\x64\vcruntime.lib')) {
        Write-TestFile (Join-Path $fixture.VisualStudio $relative) 'fake Visual Studio file'
    }
    Write-TestFile (Join-Path $fixture.VisualStudio 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.v143.default.txt') '14.38.33130'

    # Small test-only executables provide version metadata and forward CMake
    # arguments to a fake builder. They never invoke a native compiler/toolchain.
    $fakeExeSource = @'
using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text.RegularExpressions;
[assembly: AssemblyFileVersion("27.0.0.0")]
[assembly: AssemblyVersion("27.0.0.0")]
class FakeTool {
    static string Quote(string value) {
        return "\"" + Regex.Replace(Regex.Replace(value, @"(\\*)""", "$1$1\\\""), @"(\\+)$", "$1$1") + "\"";
    }
    static int Main(string[] args) {
        if (args.Length == 1 && args[0] == "--version") { Console.WriteLine("cmake version 3.30.0"); return 0; }
        if (args.Length == 1 && args[0] == "--help") { Console.WriteLine("Visual Studio 17 2022"); return 0; }
        string script = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "fake-tool.ps1");
        if (!File.Exists(script)) return 0;
        var info = new ProcessStartInfo("powershell.exe", "-NoLogo -NoProfile -ExecutionPolicy Bypass -File " + Quote(script) + " " + String.Join(" ", args.Select(Quote))) {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true
        };
        using (var child = new Process()) {
            child.StartInfo = info;
            child.OutputDataReceived += (sender, e) => { if (e.Data != null) Console.Out.WriteLine(e.Data); };
            child.ErrorDataReceived += (sender, e) => { if (e.Data != null) Console.Error.WriteLine(e.Data); };
            child.Start(); child.BeginOutputReadLine(); child.BeginErrorReadLine(); child.WaitForExit(); return child.ExitCode;
        }
    }
}
'@
    $csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    $source = Join-Path $root 'fake-tool.cs'
    Write-TestFile $source $fakeExeSource
    New-Item -ItemType Directory -Path (Split-Path -Parent $fixture.CMake),$fixture.Max -Force | Out-Null
    $compile = Invoke-TestProcess $csc @('/nologo','/target:exe','/reference:System.Core.dll',('/out:' + $fixture.CMake),$source) $root
    Assert-TestTrue ($compile.ExitCode -eq 0) ('Cannot create the test executable: ' + $compile.Output)
    Copy-Item -LiteralPath $fixture.CMake -Destination (Join-Path $fixture.Max '3dsmax.exe')
    $msbuildSource = Join-Path $root 'fake-msbuild.cs'
    Write-TestFile $msbuildSource ($fakeExeSource.Replace('27.0.0.0','17.0.0.0'))
    $msbuild = Join-Path $fixture.VisualStudio 'MSBuild\Current\Bin\MSBuild.exe'
    New-Item -ItemType Directory -Path (Split-Path -Parent $msbuild) -Force | Out-Null
    $compile = Invoke-TestProcess $csc @('/nologo','/target:exe','/reference:System.Core.dll',('/out:' + $msbuild),$msbuildSource) $root
    Assert-TestTrue ($compile.ExitCode -eq 0) ('Cannot create the test MSBuild metadata: ' + $compile.Output)

    $fakeBuilder = @'
$ErrorActionPreference = 'Stop'
$context = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'context.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$root = $context.Root
$role = $context.Role
$arguments = @($args)
$plan = Get-Content -LiteralPath (Join-Path $root 'run.json') -Raw -Encoding UTF8 | ConvertFrom-Json
function Put([string]$Path, [string]$Content) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
    [IO.File]::WriteAllText($Path, $Content)
}
try {
    if ($role -eq 'Max' -and $arguments[0] -eq '-S') {
        $cache = $arguments[[Array]::IndexOf($arguments,'-B') + 1]
        New-Item -ItemType Directory -Path $cache -Force | Out-Null
        @{ Arguments=$arguments; CachePath=$cache; KitsRoot=$env:CMAKE_WINDOWS_KITS_10_DIR; SdkRoot=$env:WindowsSdkDir_10; UcrtRoot=$env:UniversalCRTSdkDir_10; Version=$env:WindowsSDKVersion } |
            ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $root 'cmake-settings.json') -Encoding UTF8
    }
    if ($role -eq 'UE' -or $arguments[0] -eq '--build') {
        Put (Join-Path $root "events\$role.start") ([DateTime]::UtcNow.Ticks.ToString())
        Put (Join-Path $root "events\$role.ready") 'ready'
        $deadline = [DateTime]::UtcNow.AddSeconds(20)
        foreach ($target in $plan.Targets) {
            while (!(Test-Path -LiteralPath (Join-Path $root "events\$target.ready"))) {
                if ([DateTime]::UtcNow -gt $deadline) { throw 'Selected builders did not execute concurrently.' }
                Start-Sleep -Milliseconds 50
            }
        }
        if ($plan.ExpectedLatest) {
            foreach ($target in $plan.Targets) {
                $folder = if ($target -eq 'UE') { 'ue_plugins' } else { '3ds_plugins' }
                $actual = [IO.File]::ReadAllText((Join-Path $root "$folder\build\latest\WyVehicleAssetBridge\version.txt")).Trim()
                if ($actual -ne $plan.ExpectedLatest) { throw 'Latest changed before all selected builds completed.' }
            }
        }
        Put (Join-Path $root "events\$role.end") ([DateTime]::UtcNow.Ticks.ToString())
        if ($plan.Failure -eq $role) { [Console]::Error.WriteLine("fake $role compiler failed"); exit 23 }
    }
    if ($role -eq 'UE' -or $arguments[0] -eq '--install') {
        if ($role -eq 'UE') {
            $packageArgument = @($arguments | Where-Object { $_ -like '-Package=*' })
            if ($packageArgument.Count -ne 1) { throw 'UAT did not receive one package path.' }
            $package = $packageArgument[0].Substring(9)
            $files = @('WyVehicleAssetBridge.uplugin','Binaries\Win64\UnrealEditor-WyVehicleAssetBridge.dll','Binaries\Win64\UnrealEditor.modules')
        } else {
            $prefix = [Array]::IndexOf($arguments,'--prefix')
            if ($prefix -lt 0) { throw 'CMake install did not receive --prefix.' }
            $package = $arguments[$prefix + 1]
            $files = @('PackageContents.xml','Contents\Win64\WyVehicleAssetBridge.gup')
        }
        foreach ($file in $files + @('version.txt')) {
            if ($plan.Missing -eq $role -and ($file.EndsWith('.dll') -or $file.EndsWith('.gup'))) { continue }
            Put (Join-Path $package $file) $plan.Version
        }
        Write-Output "fake $role package complete"
    }
    exit 0
} catch { [Console]::Error.WriteLine($_.Exception.Message); exit 24 }
'@
    $uatDirectory = Join-Path $fixture.Engine 'Engine\Build\BatchFiles'
    foreach ($tool in @(@{ Folder=$uatDirectory; Role='UE' }, @{ Folder=(Split-Path -Parent $fixture.CMake); Role='Max' })) {
        Write-TestFile (Join-Path $tool.Folder 'context.json') (@{ Root=$root; Role=$tool.Role } | ConvertTo-Json)
        Write-TestFile (Join-Path $tool.Folder 'fake-tool.ps1') $fakeBuilder
    }
    Write-TestFile (Join-Path $uatDirectory 'RunUAT.bat') "@echo off`r`npowershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File `"%~dp0fake-tool.ps1`" %*`r`nexit /b %ERRORLEVEL%`r`n"
    Write-TestConfiguration $fixture
    Set-TestRun $fixture
    return $fixture
}
