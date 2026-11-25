# Black-box standalone EXE integration. Every compiler/deployment is a fake fixture.
param([string]$AssemblyPath = '')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'test_support.ps1')
if (!$AssemblyPath) { $AssemblyPath = Join-Path $repoRoot 'WyVehicleAssetBridgeManager.exe' }
$buildRoot = Join-Path $repoRoot 'ue_plugins\build'
$testRoot = Join-Path $buildRoot ('tests-cli-' + [Guid]::NewGuid().ToString('N'))
Assert-TestChild $testRoot $buildRoot
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
$script:passed = 0
$script:failed = 0
function Test-Case([string]$Name, [scriptblock]$Action) {
    try { & $Action; $script:passed++; Write-Host "PASS $Name" }
    catch { $script:failed++; Write-Host "FAIL $Name : $($_.Exception.Message)"; Write-Host $_.ScriptStackTrace }
}
function Assert-Code($Result, [bool]$Success) {
    Assert-TestTrue (($Result.ExitCode -eq 0) -eq $Success) ("Unexpected exit code $($Result.ExitCode): " + $Result.Output)
}
function Assert-Versions($Fixture, [string]$Version, [switch]$Deployed) {
    $packages = if ($Deployed) { @($Fixture.DeployUE,$Fixture.DeployMax) } else { @($Fixture.LatestUE,$Fixture.LatestMax) }
    foreach ($package in $packages) { Assert-TestTrue ((Read-TestVersion $package) -eq $Version) "Unexpected package version: $package" }
}
try {
    $unicodeName = ([string][char]0x8F7D) + [char]0x5177 + ' repo with spaces'
    $fixture = New-TestBridgeFixture $testRoot $unicodeName $repoRoot $AssemblyPath
    Test-Case 'EXE help, missing INI guidance, adjacent configuration and build alias' {
        $configuration = [IO.File]::ReadAllText($fixture.Config)
        Remove-Item -LiteralPath $fixture.Config
        try {
            Assert-Code (Invoke-TestBuild $fixture @('-Help')) $true
            $missing = Invoke-TestBuild $fixture @('-Check')
            Assert-Code $missing $false
            Assert-TestTrue ($missing.Output.Contains('WyVehicleAssetBridgeManager.exe') -and $missing.Output.Contains('build_and_deploy.ini')) 'Missing configuration did not identify the setup EXE and INI.'
        } finally { Write-TestFile $fixture.Config $configuration }
        Assert-Code (Invoke-TestBuild $fixture @('-Check')) $true
        Assert-Code (Invoke-TestBuild $fixture @('-Check') -BuildAlias) $true
        Assert-TestTrue (!(Test-Path -LiteralPath $fixture.LatestUE)) '-Check built UE artifacts.'
        Assert-TestTrue (!(Test-Path -LiteralPath $fixture.LatestMax)) '-Check built Max artifacts.'
    }
    Test-Case 'Malformed INI rejects duplicate keys and creates no artifacts' {
        $original = [IO.File]::ReadAllText($fixture.Config)
        try {
            Write-TestFile $fixture.Config ($original.Replace('[UE]', "[UE]`r`nProjectPath=duplicate"))
            Assert-Code (Invoke-TestBuild $fixture @('-Check')) $false
        } finally { Write-TestFile $fixture.Config $original }
        Assert-TestTrue (!(Test-Path -LiteralPath $fixture.LatestUE)) 'Malformed INI produced artifacts.'
    }
    Test-Case 'Single-target builds require only the selected side configuration' {
        Write-TestConfiguration $fixture 'UE'
        Set-TestRun $fixture @('UE') 'single-ue'
        Assert-Code (Invoke-TestBuild $fixture @('-Target','UE','-BuildOnly')) $true
        Assert-TestTrue ((Read-TestVersion $fixture.LatestUE) -eq 'single-ue') 'UE-only build failed to publish latest.'
        Assert-TestTrue (!(Test-Path -LiteralPath (Join-Path $fixture.Root 'events\Max.start'))) 'UE-only build launched Max.'
        Write-TestConfiguration $fixture 'Max' $false
        Set-TestRun $fixture @('Max') 'single-max'
        Assert-Code (Invoke-TestBuild $fixture @('-Target','Max','-BuildOnly')) $true
        Assert-TestTrue ((Read-TestVersion $fixture.LatestMax) -eq 'single-max') 'Legacy Max-only INI failed.'
        Assert-TestTrue (!(Test-Path -LiteralPath (Join-Path $fixture.Root 'events\UE.start'))) 'Max-only build launched UE.'
        Write-TestConfiguration $fixture
    }
    Test-Case 'Both builders overlap, update latest together and retain logs' {
        New-TestPackage $fixture.LatestUE UE 'old'
        New-TestPackage $fixture.LatestMax Max 'old'
        Set-TestRun $fixture @('UE','Max') 'both-v1' '' '' 'old'
        Assert-Code (Invoke-TestBuild $fixture @('-BuildOnly')) $true
        Assert-Versions $fixture 'both-v1'
        $ueStart = [long][IO.File]::ReadAllText((Join-Path $fixture.Root 'events\UE.start'))
        $maxStart = [long][IO.File]::ReadAllText((Join-Path $fixture.Root 'events\Max.start'))
        $ueEnd = [long][IO.File]::ReadAllText((Join-Path $fixture.Root 'events\UE.end'))
        $maxEnd = [long][IO.File]::ReadAllText((Join-Path $fixture.Root 'events\Max.end'))
        Assert-TestTrue ([Math]::Max($ueStart,$maxStart) -le [Math]::Min($ueEnd,$maxEnd)) 'Builders did not overlap.'
        foreach ($folder in @('ue_plugins','3ds_plugins')) {
            $logs = @(Get-ChildItem -LiteralPath (Join-Path $fixture.Root "$folder\build\logs") -File -Recurse)
            Assert-TestTrue ($logs.Count -gt 0) "No $folder build logs were retained."
        }
        Assert-TestTrue (!(Test-Path -LiteralPath $fixture.DeployUE)) '-BuildOnly deployed UE.'
        Assert-TestTrue (!(Test-Path -LiteralPath $fixture.DeployMax)) '-BuildOnly deployed Max.'
    }
    Test-Case 'Either compiler failure and missing artifacts preserve both latest packages' {
        foreach ($target in @('UE','Max')) {
            Set-TestRun $fixture @('UE','Max') 'must-not-replace' $target '' 'both-v1'
            Assert-Code (Invoke-TestBuild $fixture @('-BuildOnly')) $false
            Assert-Versions $fixture 'both-v1'
            $logs = @(Get-ChildItem -LiteralPath (Join-Path $fixture.Root 'ue_plugins\build\logs'),(Join-Path $fixture.Root '3ds_plugins\build\logs') -File -Recurse)
            $loggedFailure = @($logs | Where-Object { [IO.File]::ReadAllText($_.FullName).Contains("fake $target compiler failed") })
            Assert-TestTrue ($loggedFailure.Count -gt 0) "Failed $target compiler output was not retained."
        }
        Set-TestRun $fixture @('UE','Max') 'missing-artifact' '' 'Max' 'both-v1'
        Assert-Code (Invoke-TestBuild $fixture @('-BuildOnly')) $false
        Assert-Versions $fixture 'both-v1'
    }
    Test-Case 'Successful deployment replaces only managed plugin directories' {
        New-TestPackage $fixture.DeployUE UE 'deployed-old'
        New-TestPackage $fixture.DeployMax Max 'deployed-old'
        Write-TestFile (Join-Path $fixture.DeployUE 'obsolete.txt') 'old managed file'
        $unrelated = Join-Path $fixture.Project 'Plugins\Unrelated\keep.txt'
        Write-TestFile $unrelated 'unrelated plugin'
        Set-TestRun $fixture @('UE','Max') 'deployed-v1'
        Assert-Code (Invoke-TestBuild $fixture) $true
        Assert-Versions $fixture 'deployed-v1' -Deployed
        Assert-TestTrue (!(Test-Path -LiteralPath (Join-Path $fixture.DeployUE 'obsolete.txt'))) 'Old managed files survived replacement.'
        Assert-TestTrue ([IO.File]::ReadAllText($unrelated) -eq 'unrelated plugin') 'Another plugin was modified.'
        foreach ($package in @($fixture.DeployUE,$fixture.DeployMax)) {
            $marker = Get-Content -LiteralPath (Join-Path $package '.wy-vehicle-asset-bridge-managed.json') -Raw | ConvertFrom-Json
            Assert-TestTrue ($marker.Product -eq 'WyVehicleAssetBridge') 'Deployment ownership marker is missing.'
        }
    }
    Test-Case 'Locked second destination rolls back the first deployed plugin' {
        Set-TestRun $fixture @('UE','Max') 'rollback-candidate'
        $lock = [IO.File]::Open((Join-Path $fixture.DeployMax 'Contents\Win64\WyVehicleAssetBridge.gup'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        try { Assert-Code (Invoke-TestBuild $fixture) $false }
        finally { $lock.Dispose() }
        Assert-Versions $fixture 'deployed-v1' -Deployed
    }
    Test-Case 'An unowned destination fails before replacing user files' {
        $marker = Join-Path $fixture.DeployMax '.wy-vehicle-asset-bridge-managed.json'
        $saved = [IO.File]::ReadAllText($marker)
        Remove-Item -LiteralPath $marker
        Set-TestRun $fixture @('UE','Max') 'unowned-candidate'
        try {
            Assert-Code (Invoke-TestBuild $fixture) $false
            Assert-Versions $fixture 'deployed-v1' -Deployed
            Assert-TestTrue (!(Test-Path -LiteralPath (Join-Path $fixture.Root 'events\UE.start'))) 'Unowned preflight launched builds.'
        } finally { Write-TestFile $marker $saved }
    }
    Test-Case 'A Plugins junction remains intact and deployment reaches its physical target' {
        $linked = New-TestBridgeFixture $testRoot 'linked plugins repo' $repoRoot $AssemblyPath
        $physical = Join-Path $linked.Root 'physical plugins'
        $junction = Join-Path $linked.Project 'Plugins'
        New-Item -ItemType Directory -Path $physical -Force | Out-Null
        New-Item -ItemType Junction -Path $junction -Target $physical | Out-Null
        try {
            Write-TestConfiguration $linked 'UE'
            Set-TestRun $linked @('UE') 'through-junction'
            Assert-Code (Invoke-TestBuild $linked @('-Target','UE')) $true
            Assert-TestTrue ((Read-TestVersion (Join-Path $physical 'WyVehicleAssetBridge')) -eq 'through-junction') 'Package did not reach physical Plugins root.'
            Assert-TestTrue ([bool]((Get-Item -LiteralPath $junction).Attributes -band [IO.FileAttributes]::ReparsePoint)) 'Deployment replaced the Plugins junction.'
        } finally { Remove-TestJunction $junction $linked.Root }
    }
    Test-Case 'A linked plugin child is rejected without modifying its external target' {
        $linked = New-TestBridgeFixture $testRoot 'unsafe plugin child repo' $repoRoot $AssemblyPath
        $outside = Join-Path $linked.Root 'outside managed plugin'
        New-TestPackage $outside UE 'external'
        New-Item -ItemType Directory -Path (Split-Path -Parent $linked.DeployUE) -Force | Out-Null
        New-Item -ItemType Junction -Path $linked.DeployUE -Target $outside | Out-Null
        try {
            Set-TestRun $linked @('UE') 'must-not-touch'
            Assert-Code (Invoke-TestBuild $linked @('-Target','UE')) $false
            Assert-TestTrue ((Read-TestVersion $outside) -eq 'external') 'Linked plugin contents were modified.'
        } finally { Remove-TestJunction $linked.DeployUE $linked.Root }
    }
    Test-Case 'Chosen toolchains reach the builder, cache switches are isolated and SDK downloads survive' {
        $originalVs = $fixture.VisualStudio
        $originalSdk = $fixture.WindowsSdk
        $otherVs = Join-Path $fixture.Root 'another Visual Studio'
        $otherSdk = Join-Path $fixture.Root 'another Windows Kits\10'
        Copy-Item -LiteralPath $originalVs -Destination $otherVs -Recurse
        New-Item -ItemType Directory -Path (Split-Path -Parent $otherSdk) -Force | Out-Null
        Copy-Item -LiteralPath $originalSdk -Destination $otherSdk -Recurse
        $download = Join-Path $fixture.Root '3ds_plugins\build\dependencies\keep-sdk.msi'
        Write-TestFile $download 'cached SDK download'
        $cachePaths = @{}
        foreach ($selection in @('a','b','a')) {
            $fixture.VisualStudio = if ($selection -eq 'a') { $originalVs } else { $otherVs }
            $fixture.WindowsSdk = if ($selection -eq 'a') { $originalSdk } else { $otherSdk }
            Write-TestConfiguration $fixture 'Max'
            Set-TestRun $fixture @('Max') ('cache-' + $selection)
            Assert-Code (Invoke-TestBuild $fixture @('-Target','Max','-BuildOnly')) $true
            $settings = Get-Content -LiteralPath (Join-Path $fixture.Root 'cmake-settings.json') -Raw | ConvertFrom-Json
            Assert-TestTrue ($settings.Arguments -contains ('-DCMAKE_GENERATOR_INSTANCE=' + $fixture.VisualStudio)) 'Chosen Visual Studio did not reach CMake.'
            Assert-TestTrue ($settings.KitsRoot -eq $fixture.WindowsSdk) 'Chosen SDK did not reach CMake.'
            Assert-TestTrue ($settings.SdkRoot.TrimEnd('\','/') -eq $fixture.WindowsSdk) 'Chosen SDK did not reach MSBuild.'
            Assert-TestTrue ($settings.Version.TrimEnd('\','/') -eq '10.0.22621.0') 'Chosen SDK version did not reach builder.'
            if ($cachePaths.ContainsKey($selection)) {
                Assert-TestTrue ($cachePaths[$selection] -eq $settings.CachePath) 'Returning to a toolchain did not reuse its cache.'
                Assert-TestTrue (Test-Path -LiteralPath (Join-Path $settings.CachePath 'keep.txt')) 'Old compiler cache was discarded.'
            } else {
                foreach ($known in $cachePaths.Values) { Assert-TestTrue ($known -ne $settings.CachePath) 'Different toolchains shared a stale cache.' }
                $cachePaths[$selection] = $settings.CachePath
                Write-TestFile (Join-Path $settings.CachePath 'keep.txt') 'cached compiler state'
            }
            Assert-TestTrue ([IO.File]::ReadAllText($download) -eq 'cached SDK download') 'Changing toolchains removed downloaded SDK.'
        }
        $fixture.VisualStudio = $originalVs
        $fixture.WindowsSdk = $originalSdk
        Write-TestConfiguration $fixture
    }
    Test-Case 'Publishing migrates both owned legacy plugin names to the new product name' {
        $migration = New-TestBridgeFixture $testRoot 'legacy migration' $repoRoot $AssemblyPath
        $legacyUE = Join-Path $migration.Project 'Plugins\WyAssetBridge'
        $legacyMax = Join-Path $migration.Max 'ApplicationPlugins\WyAssetBridge'
        New-TestLegacyPackage $legacyUE UE 'legacy'
        New-TestLegacyPackage $legacyMax Max 'legacy'
        Set-TestRun $migration @('UE','Max') 'renamed'
        Assert-Code (Invoke-TestBuild $migration) $true
        Assert-Versions $migration 'renamed' -Deployed
        Assert-TestTrue (!(Test-Path -LiteralPath $legacyUE) -and !(Test-Path -LiteralPath $legacyMax)) 'Owned legacy plugin directories survived migration.'
    }
    Test-Case 'A failed second migration restores both old directory names and contents' {
        $migration = New-TestBridgeFixture $testRoot 'legacy migration rollback' $repoRoot $AssemblyPath
        $legacyUE = Join-Path $migration.Project 'Plugins\WyAssetBridge'
        $legacyMax = Join-Path $migration.Max 'ApplicationPlugins\WyAssetBridge'
        New-TestLegacyPackage $legacyUE UE 'legacy'
        New-TestLegacyPackage $legacyMax Max 'legacy'
        Set-TestRun $migration @('UE','Max') 'must-not-migrate'
        $lock = [IO.File]::Open((Join-Path $legacyMax 'Contents\Win64\WyAssetBridge.gup'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        try { Assert-Code (Invoke-TestBuild $migration) $false }
        finally { $lock.Dispose() }
        Assert-TestTrue ((Read-TestVersion $legacyUE) -eq 'legacy' -and (Read-TestVersion $legacyMax) -eq 'legacy') 'Migration rollback did not restore the old packages.'
        Assert-TestTrue (!(Test-Path -LiteralPath $migration.DeployUE) -and !(Test-Path -LiteralPath $migration.DeployMax)) 'Failed migration left a newly named package installed.'
    }
    Test-Case 'Source fingerprints track installed packages and ChangedOnly selects only known changes' {
        $incremental = New-TestBridgeFixture $testRoot 'incremental publishing' $repoRoot $AssemblyPath
        Set-TestRun $incremental @('UE','Max') 'fingerprint-baseline'
        Assert-Code (Invoke-TestBuild $incremental) $true
        $baselineUE = Read-TestFingerprint $incremental.DeployUE
        $baselineMax = Read-TestFingerprint $incremental.DeployMax
        Assert-TestTrue ($baselineUE -ne $baselineMax) 'Distinct UE and Max source trees received one shared fingerprint.'
        $maxMarker = Join-Path $incremental.DeployMax '.wy-vehicle-asset-bridge-managed.json'
        $maxInstalledBytes = [Convert]::ToBase64String([IO.File]::ReadAllBytes($maxMarker))
        Set-TestRun $incremental @() 'no-change'
        Assert-Code (Invoke-TestBuild $incremental @('-ChangedOnly')) $true
        Assert-TestTrue (@(Get-ChildItem -LiteralPath (Join-Path $incremental.Root 'events') -File).Count -eq 0) 'ChangedOnly rebuilt an unchanged installed version.'

        $ueSource = Get-TestSourceFile $incremental 'UE'
        [IO.File]::AppendAllText($ueSource, "`n// incremental UE change`n", (New-Object Text.UTF8Encoding $false))
        Set-TestRun $incremental @() 'check-only-new-source'
        Assert-Code (Invoke-TestBuild $incremental @('-ChangedOnly','-Target','UE','-Check')) $true
        Assert-TestTrue (@(Get-ChildItem -LiteralPath (Join-Path $incremental.Root 'events') -File).Count -eq 0) 'ChangedOnly -Check launched a compiler.'
        Set-TestRun $incremental @('UE') 'build-only-new-source'
        Assert-Code (Invoke-TestBuild $incremental @('-ChangedOnly','-Target','UE','-BuildOnly')) $true
        Assert-TestTrue ((Read-TestFingerprint $incremental.LatestUE) -ne $baselineUE) 'BuildOnly did not mark the new source fingerprint.'
        Assert-TestTrue ((Read-TestFingerprint $incremental.DeployUE) -eq $baselineUE) 'BuildOnly changed the installed fingerprint.'
        Set-TestRun $incremental @('UE') 'incremental-ue'
        Assert-Code (Invoke-TestBuild $incremental @('-ChangedOnly')) $true
        $newUE = Read-TestFingerprint $incremental.DeployUE
        Assert-TestTrue ($newUE -ne $baselineUE) 'UE incremental publication kept the old fingerprint.'
        Assert-TestTrue ($maxInstalledBytes -eq [Convert]::ToBase64String([IO.File]::ReadAllBytes($maxMarker))) 'UE-only incremental publication rewrote the installed Max baseline.'
        Assert-TestTrue (!(Test-Path -LiteralPath (Join-Path $incremental.Root 'events\Max.start'))) 'UE-only change launched a Max compiler.'

        [IO.File]::AppendAllText($ueSource, "`n// second UE change`n", (New-Object Text.UTF8Encoding $false))
        [IO.File]::AppendAllText((Get-TestSourceFile $incremental 'Max'), "`n// Max change`n", (New-Object Text.UTF8Encoding $false))
        Set-TestRun $incremental @('UE','Max') 'failed-incremental' 'Max'
        Assert-Code (Invoke-TestBuild $incremental @('-ChangedOnly')) $false
        Assert-TestTrue ((Read-TestFingerprint $incremental.DeployUE) -eq $newUE -and (Read-TestFingerprint $incremental.DeployMax) -eq $baselineMax) 'A failed incremental build changed installed baselines.'
        Set-TestRun $incremental @('UE','Max') 'incremental-both'
        Assert-Code (Invoke-TestBuild $incremental @('-ChangedOnly')) $true
        Assert-TestTrue ((Read-TestFingerprint $incremental.DeployUE) -ne $newUE -and (Read-TestFingerprint $incremental.DeployMax) -ne $baselineMax) 'Both mismatched plugins were not published.'
    }
    Test-Case 'Missing and old unversioned installations are unknown, not ChangedOnly mismatches' {
        $unknown = New-TestBridgeFixture $testRoot 'unknown installed versions' $repoRoot $AssemblyPath
        Set-TestRun $unknown @() 'not-installed'
        $noInstall = Invoke-TestBuild $unknown @('-ChangedOnly')
        Assert-TestTrue ($noInstall.Output.Length -gt 0) 'No-install incremental check did not explain its status.'
        Assert-TestTrue (@(Get-ChildItem -LiteralPath (Join-Path $unknown.Root 'events') -File).Count -eq 0) 'ChangedOnly compiled a plugin without a known installed baseline.'
        Assert-TestTrue (!(Test-Path -LiteralPath $unknown.DeployUE) -and !(Test-Path -LiteralPath $unknown.DeployMax)) 'ChangedOnly installed an unknown baseline.'
        New-TestPackage $unknown.DeployUE UE 'old-without-fingerprint'
        New-TestPackage $unknown.DeployMax Max 'old-without-fingerprint'
        Set-TestRun $unknown @() 'unversioned'
        $unversioned = Invoke-TestBuild $unknown @('-ChangedOnly')
        Assert-TestTrue ($unversioned.Output.Length -gt 0) 'Old unversioned incremental check did not explain its status.'
        Assert-TestTrue (@(Get-ChildItem -LiteralPath (Join-Path $unknown.Root 'events') -File).Count -eq 0) 'ChangedOnly compiled an old package lacking a source fingerprint.'
        Assert-Versions $unknown 'old-without-fingerprint' -Deployed
        Set-TestRun $unknown @('UE','Max') 'ordinary-publication'
        Assert-Code (Invoke-TestBuild $unknown) $true
        $null = Read-TestFingerprint $unknown.DeployUE
        $null = Read-TestFingerprint $unknown.DeployMax
    }
    Test-Case 'Repository lock rejects another build without changing latest output' {
        $old = Read-TestVersion $fixture.LatestUE
        Set-TestRun $fixture @('UE') 'concurrent-invocation'
        $lock = [IO.File]::Open((Join-Path $fixture.Root '.build.lock'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        try { Assert-Code (Invoke-TestBuild $fixture @('-Target','UE','-BuildOnly')) $false }
        finally { $lock.Dispose() }
        Assert-TestTrue ((Read-TestVersion $fixture.LatestUE) -eq $old) 'A locked invocation changed latest output.'
    }
} finally { Remove-TestTree $testRoot $buildRoot }
Write-Host "Standalone EXE integration tests: $script:passed passed, $script:failed failed."
if ($script:failed) { exit 1 }
exit 0
