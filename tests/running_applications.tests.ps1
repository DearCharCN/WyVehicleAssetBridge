# Detection tests only: no editor, Max, save or shutdown operation is performed.
param([string]$AssemblyPath = '')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'test_support.ps1')
if (!$AssemblyPath) { $AssemblyPath = Join-Path $repoRoot 'WyVehicleAssetBridgeManager.exe' }
$assembly = [Reflection.Assembly]::LoadFrom([IO.Path]::GetFullPath($AssemblyPath))
$flags = [Reflection.BindingFlags]'Instance,Static,NonPublic'
$targetType = $assembly.GetType('WyVehicleAssetBridge.Setup.BuildTarget', $true)
$appType = $assembly.GetType('WyVehicleAssetBridge.Setup.RunningApplication', $true)
$detectorType = $assembly.GetType('WyVehicleAssetBridge.Setup.RunningApplications', $true)
$classify = $detectorType.GetMethod('Classify', $flags)
$split = $detectorType.GetMethod('SplitCommandLine', $flags)
$buildRoot = Join-Path $repoRoot 'ue_plugins\build'
$testRoot = Join-Path $buildRoot ('tests-app-identity-' + [Guid]::NewGuid().ToString('N'))
Assert-TestChild $testRoot $buildRoot
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
$script:passed = 0
function Test-Case([string]$Name, [scriptblock]$Action) {
    & $Action
    $script:passed++
    Write-Host "PASS $Name"
}
function New-InternalObject($Type, [hashtable]$Values) {
    $result = [Activator]::CreateInstance($Type, $true)
    foreach ($key in $Values.Keys) { $Type.GetField($key, $flags).SetValue($result, $Values[$key]) }
    return $result
}
function Read-App($App, [string]$Field) { return $appType.GetField($Field, $flags).GetValue($App) }
function Check-Match($Target, [string]$Name, [string]$Executable, [string]$CommandLine, [bool]$Included, [bool]$Known, [bool]$Headless=$false) {
    $app = New-InternalObject $appType @{Name=$Name; Executable=$Executable; CommandLine=$CommandLine}
    $actual = $classify.Invoke($null, @($Target,$app))
    Assert-TestTrue ($actual -eq $Included) "Unexpected process inclusion for $CommandLine"
    Assert-TestTrue ((Read-App $app 'IdentityKnown') -eq $Known) "Unexpected identity certainty for $CommandLine"
    if ($Included) { Assert-TestTrue ((Read-App $app 'Headless') -eq $Headless) "Unexpected headless classification for $CommandLine" }
    if ($Included -and !$Known) { Assert-TestTrue (![string]::IsNullOrWhiteSpace((Read-App $app 'IdentityIssue'))) 'Unknown identity has no actionable explanation.' }
}
try {
    $projectDir = Join-Path $testRoot 'target project'
    $project = Join-Path $projectDir 'Target.uproject'
    $otherProject = Join-Path $testRoot 'other project\Other.uproject'
    $editor = Join-Path $testRoot 'engine folder\UnrealEditor.exe'
    $maxDir = Join-Path $testRoot 'max 2025'
    $maxExe = Join-Path $maxDir '3dsmax.exe'
    foreach ($file in @($project,$otherProject,$editor,$maxExe)) { Write-TestFile $file 'fixture' }
    $ueTarget = New-InternalObject $targetType @{Name='UE';ProjectFile=$project}
    $maxTarget = New-InternalObject $targetType @{Name='Max';MaxInstall=$maxDir}
    $quotedEditor = ConvertTo-TestProcessArgument $editor
    Test-Case 'UE matches a quoted absolute project with spaces and mixed path casing' {
        Check-Match $ueTarget 'UnrealEditor.exe' $editor "$quotedEditor `"$project`" -log" $true $true
        Check-Match $ueTarget 'UnrealEditor.exe' $editor "$quotedEditor `"$($project.ToUpperInvariant().Replace('\','/'))`"" $true $true
    }
    Test-Case 'UE matches an explicit project option and project directory launch' {
        Check-Match $ueTarget 'UnrealEditor.exe' $editor "$quotedEditor -log -project=`"$project`"" $true $true
        Check-Match $ueTarget 'UnrealEditor.exe' $editor "$quotedEditor `"$projectDir`"" $true $true
    }
    Test-Case 'UE excludes a different project and ignores target paths inside other options' {
        Check-Match $ueTarget 'UnrealEditor.exe' $editor "$quotedEditor `"$otherProject`" -abslog=`"$project`"" $false $false
        Check-Match $ueTarget 'UnrealEditor.exe' $editor "$quotedEditor `"$($project + '.backup.uproject')`"" $false $false
        Check-Match $ueTarget 'UnrealEditor.exe' $editor "$quotedEditor `"$otherProject`" -ExecCmds=`"echo $project`"" $false $false
    }
    Test-Case 'Missing and relative UE identity block deployment without automatic close' {
        Check-Match $ueTarget 'UnrealEditor.exe' $editor '' $true $false
        Check-Match $ueTarget 'UnrealEditor.exe' $editor "$quotedEditor Target.uproject" $true $false
        Check-Match $ueTarget 'UnrealEditor.exe' $editor "$quotedEditor D:Target.uproject" $true $false
        Check-Match $ueTarget 'UnrealEditor.exe' '' "$quotedEditor `"$project`"" $true $false
        Check-Match $ueTarget 'UnrealEditor.exe' $editor "$quotedEditor -project=`"$project`" -project=`"$otherProject`"" $true $false
    }
    Test-Case 'UE commandlet and unattended editors are identified for manual close' {
        $cmdEditor = Join-Path (Split-Path $editor) 'UnrealEditor-Cmd.exe'
        Check-Match $ueTarget 'UnrealEditor-Cmd.exe' $cmdEditor "`"$cmdEditor`" `"$project`"" $true $true $true
        Check-Match $ueTarget 'UnrealEditor.exe' $editor "$quotedEditor `"$project`" -unattended" $true $true $true
        Check-Match $ueTarget 'UnrealEditor.exe' $editor "$quotedEditor `"$project`" -run=ResavePackages" $true $true $true
    }
    Test-Case 'Max installation matching is exact, including batch and unknown identity' {
        Check-Match $maxTarget '3dsmax.exe' $maxExe "`"$maxExe`"" $true $true
        $otherMax = Join-Path ($maxDir + ' extra') '3dsmax.exe'
        Check-Match $maxTarget '3dsmax.exe' $otherMax "`"$otherMax`"" $false $false
        Check-Match $maxTarget '3dsmax.exe' '' '' $true $false
        $batch = Join-Path $maxDir '3dsmaxbatch.exe'
        Check-Match $maxTarget '3dsmaxbatch.exe' $batch "`"$batch`"" $true $true $true
        Check-Match $maxTarget 'UnrealEditor.exe' $editor "$quotedEditor `"$project`"" $false $false
        Check-Match $ueTarget '3dsmax.exe' $maxExe "`"$maxExe`"" $false $false
    }
    Test-Case 'Junction aliases resolve to the same project and Max installation' {
        $projectLink = Join-Path $testRoot 'project alias'
        $maxLink = Join-Path $testRoot 'max alias'
        New-Item -ItemType Junction -Path $projectLink -Target $projectDir | Out-Null
        New-Item -ItemType Junction -Path $maxLink -Target $maxDir | Out-Null
        try {
            $linkedProject = Join-Path $projectLink 'Target.uproject'
            $linkedMax = Join-Path $maxLink '3dsmax.exe'
            Check-Match $ueTarget 'UnrealEditor.exe' $editor "$quotedEditor `"$linkedProject`"" $true $true
            Check-Match $maxTarget '3dsmax.exe' $linkedMax "`"$linkedMax`"" $true $true
        } finally {
            Remove-TestJunction $projectLink $testRoot
            Remove-TestJunction $maxLink $testRoot
        }
    }
    Test-Case 'Native Windows command-line parsing preserves quoted paths and trailing slash' {
        $values = @($editor,$project,($maxDir + '\'),'-ExecCmds=echo "two words"')
        $command = ($values | ForEach-Object { ConvertTo-TestProcessArgument $_ }) -join ' '
        $parsed = $split.Invoke($null, @($command))
        Assert-TestTrue ($parsed.Length -eq $values.Length) 'Command line token count changed.'
        for ($i=0; $i -lt $values.Length; $i++) { Assert-TestTrue ($parsed[$i] -ceq $values[$i]) "Token changed at $i." }
    }
    Write-Host "All $script:passed process identity tests passed."
} finally { Remove-TestTree $testRoot $buildRoot }
