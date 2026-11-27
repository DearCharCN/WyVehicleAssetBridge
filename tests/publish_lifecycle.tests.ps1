# Exercises the production pipeline/config source with fake compiler packages and
# a deterministic process guard. No real UE/Max process, UI or deployment is used.
param([string]$AssemblyPath = '')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'test_support.ps1')
if (!$AssemblyPath) { $AssemblyPath = Join-Path $repoRoot 'WyVehicleAssetBridgeManager.exe' }
$buildRoot = Join-Path $repoRoot 'ue_plugins\build'
$testRoot = Join-Path $buildRoot ('tests-lifecycle-' + [Guid]::NewGuid().ToString('N'))
Assert-TestChild $testRoot $buildRoot
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
$script:passed = 0
$script:failed = 0
function Test-Case([string]$Name, [scriptblock]$Action) {
    try { & $Action; $script:passed++; Write-Host "PASS $Name" }
    catch { $script:failed++; Write-Host "FAIL $Name : $($_.Exception.Message)"; Write-Host $_.ScriptStackTrace }
}
$harnessSource = @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Web.Script.Serialization;

namespace WyVehicleAssetBridge.Setup
{
    internal static class RunningApplications
    {
        internal static int Calls;
        internal static string Scenario;
        internal static readonly List<string> Trace = new List<string>();
        internal static void EnsureClosed(IEnumerable<BuildTarget> targets)
        {
            Calls++;
            Trace.Add("guard:" + String.Join(",", targets.Select(t => t.Name)));
            if ((Scenario == "guard-first" && Calls == 1) || (Scenario == "guard-second" && Calls == 2))
                throw new InvalidOperationException("fixture process is still running");
        }
    }

    internal static class LifecycleHarness
    {
        private static void Check(bool value, string message) { if (!value) throw new Exception(message); }
        private static void CheckPreference(string root)
        {
            string file = LocalConfig.FileName(root);
            string original = File.ReadAllText(file);
            Check(!LocalConfig.ReopenUEAfterPublish(root), "Missing reopen preference must default to false");
            foreach (bool enabled in new[] { true, false, true, true, false })
            {
                LocalConfig.SaveReopenUEAfterPublish(root, enabled);
                Check(LocalConfig.ReopenUEAfterPublish(root) == enabled, "Reopen preference was not persisted");
            }
            string[] lines = File.ReadAllLines(file);
            Check(lines.Count(l => l.Trim().StartsWith("ReopenAfterPublish=", StringComparison.OrdinalIgnoreCase)) == 1,
                "Repeated saves duplicated the preference");
            string remaining = String.Join("\n", lines.Where(l => !l.Trim().StartsWith("ReopenAfterPublish=", StringComparison.OrdinalIgnoreCase)));
            Check(remaining.TrimEnd('\n') == original.Replace("\r\n", "\n").TrimEnd('\n'),
                "Saving the boolean altered comments, unrelated fields or unknown settings");
        }

        private static int Main(string[] args)
        {
            string root = args[0], scenario = args[1];
            if (scenario == "preference")
            {
                CheckPreference(root);
                Console.WriteLine("{\"PreferencePassed\":true}");
                return 0;
            }
            RunningApplications.Scenario = scenario;
            int callbacks = 0;
            var snapshots = new List<string>();
            PublishRunResult result = null;
            string error = "";
            try
            {
                result = new BuildPipeline(root, null, delegate(string _) { }).Run("Both", scenario == "build-only", scenario == "check", false,
                    delegate(List<BuildTarget> targets)
                    {
                        callbacks++;
                        RunningApplications.Trace.Add("callback:" + String.Join(",", targets.Select(t => t.Name)));
                        Check(RunningApplications.Calls == callbacks - 1, "Callback/guard ordering was changed");
                        if (callbacks == 1)
                        {
                            Check(!File.Exists(Path.Combine(root, "events", "UE.start")) && !File.Exists(Path.Combine(root, "events", "Max.start")),
                                "Compilation began before the first application callback");
                        }
                        else
                        {
                            foreach (BuildTarget target in targets)
                            {
                                Check(File.Exists(Path.Combine(root, "events", target.Name + ".end")), "Second callback ran before compilation finished");
                                Check(File.ReadAllText(Path.Combine(target.Destination, "version.txt")).Trim() == "old", "Deployment began before the second callback");
                            }
                        }
                        if (scenario == "skip-max" && callbacks == 1 || scenario == "skip-max-second" && callbacks == 2)
                            targets.RemoveAll(t => t.Name == "Max");
                        if (scenario == "cancel-first" && callbacks == 1 || scenario == "cancel-second" && callbacks == 2)
                            targets.Clear();
                        snapshots.Add(String.Join(",", targets.Select(t => t.Name)));
                    });
            }
            catch (Exception ex) { error = ex.Message; }
            Console.WriteLine(new JavaScriptSerializer().Serialize(new {
                Returned = result != null, Published = result == null ? null : result.Published,
                Cancelled = result != null && result.Cancelled,
                Callbacks = callbacks, Guards = RunningApplications.Calls,
                Trace = RunningApplications.Trace, Snapshots = snapshots, Error = error
            }));
            return 0;
        }
    }
}
'@
function Reset-Fixture([string[]]$Targets = @('UE','Max'), [string]$Failure = '') {
    foreach ($entry in @(@($fixture.LatestUE,'UE'), @($fixture.LatestMax,'Max'), @($fixture.DeployUE,'UE'), @($fixture.DeployMax,'Max'))) {
        if (Test-Path -LiteralPath $entry[0]) { Remove-TestTree $entry[0] $fixture.Root }
        New-TestPackage $entry[0] $entry[1] 'old'
    }
    Set-TestRun $fixture -Targets $Targets -Version 'new' -Failure $Failure
}
function Invoke-Lifecycle([string]$Scenario) {
    $run = Invoke-TestProcess $harness @($fixture.Root,$Scenario) $fixture.Root -TimeoutSeconds 50
    Assert-TestTrue ($run.ExitCode -eq 0) ('Lifecycle harness failed: ' + $run.Output)
    return ($run.Output.Trim() | ConvertFrom-Json)
}
function Assert-Deployed([string]$UE, [string]$Max) {
    Assert-TestTrue ((Read-TestVersion $fixture.DeployUE) -eq $UE) 'Unexpected UE deployment.'
    Assert-TestTrue ((Read-TestVersion $fixture.DeployMax) -eq $Max) 'Unexpected Max deployment.'
}
function Assert-NotBuilt {
    Assert-TestTrue (!(Test-Path -LiteralPath (Join-Path $fixture.Root 'events\UE.start'))) 'UE unexpectedly compiled.'
    Assert-TestTrue (!(Test-Path -LiteralPath (Join-Path $fixture.Root 'events\Max.start'))) 'Max unexpectedly compiled.'
}
try {
    $fixture = New-TestBridgeFixture $testRoot 'fixture with spaces' $repoRoot $AssemblyPath
    $source = Join-Path $testRoot 'LifecycleHarness.cs'
    Write-TestFile $source $harnessSource
    $harness = Join-Path $testRoot 'LifecycleHarness.exe'
    $sources = @('BuildPipeline.cs','LocalConfig.cs','Detection.cs','PackageStore.cs','SourceFingerprint.cs','PluginCompiler.cs') |
        ForEach-Object { Join-Path $repoRoot ('src\WyVehicleAssetBridgeManager\' + $_) }
    $csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    $compile = Invoke-TestProcess $csc (@('/nologo','/target:exe','/codepage:65001','/reference:System.dll,System.Core.dll,System.Management.dll,System.Web.Extensions.dll',('/out:' + $harness),$source) + $sources) $testRoot
    Assert-TestTrue ($compile.ExitCode -eq 0) ('Cannot compile lifecycle harness: ' + $compile.Output)

    Test-Case 'Application callbacks and closed-process guards bracket compilation/deployment' {
        Reset-Fixture
        $result = Invoke-Lifecycle 'success'
        Assert-TestTrue ($result.Returned -and !$result.Cancelled -and ($result.Published -join ',') -eq 'UE,Max') 'Published targets were not returned.'
        Assert-TestTrue (($result.Trace -join '|') -eq 'callback:UE,Max|guard:UE,Max|callback:UE,Max|guard:UE,Max') 'Application callbacks and guards ran in the wrong order.'
        Assert-Deployed 'new' 'new'
    }
    Test-Case 'Declining Max before compilation leaves UE publication active' {
        Reset-Fixture -Targets @('UE')
        $result = Invoke-Lifecycle 'skip-max'
        Assert-TestTrue ($result.Returned -and !$result.Cancelled -and ($result.Published -join ',') -eq 'UE') 'UE was not independently published.'
        Assert-TestTrue (($result.Trace -join '|') -eq 'callback:UE,Max|guard:UE|callback:UE|guard:UE') 'A skipped target reached a process guard.'
        Assert-TestTrue (!(Test-Path -LiteralPath (Join-Path $fixture.Root 'events\Max.start'))) 'Declined Max was compiled.'
        Assert-Deployed 'new' 'old'
    }
    Test-Case 'Declining both targets returns Cancelled without compilation or deployment' {
        Reset-Fixture
        $result = Invoke-Lifecycle 'cancel-first'
        Assert-TestTrue ($result.Returned -and $result.Cancelled -and @($result.Published).Count -eq 0 -and $result.Callbacks -eq 1 -and $result.Guards -eq 0) 'Cancellation result was incorrect.'
        Assert-NotBuilt
        Assert-Deployed 'old' 'old'
    }
    Test-Case 'Compile failure returns no successful publication result' {
        Reset-Fixture -Failure 'UE'
        $result = Invoke-Lifecycle 'compile-failure'
        Assert-TestTrue (!$result.Returned -and $null -eq $result.Published -and $result.Error.Length -gt 0 -and $result.Callbacks -eq 1 -and $result.Guards -eq 1) 'Compile failure reached the publication phase.'
        Assert-Deployed 'old' 'old'
        Assert-TestTrue ((Read-TestVersion $fixture.LatestUE) -eq 'old' -and (Read-TestVersion $fixture.LatestMax) -eq 'old') 'Failed build updated local latest.'
    }
    Test-Case 'Declining Max after compilation still deploys UE alone' {
        Reset-Fixture
        $result = Invoke-Lifecycle 'skip-max-second'
        Assert-TestTrue ($result.Returned -and !$result.Cancelled -and ($result.Published -join ',') -eq 'UE') 'Second-stage refusal suppressed the other target.'
        Assert-TestTrue (($result.Trace -join '|') -eq 'callback:UE,Max|guard:UE,Max|callback:UE,Max|guard:UE') 'The final process guard retained a refused target.'
        Assert-TestTrue ((Read-TestVersion $fixture.LatestMax) -eq 'new') 'Successfully compiled Max package was lost.'
        Assert-Deployed 'new' 'old'
    }
    Test-Case 'Declining both after compilation cancels deployment and retains local artifacts' {
        Reset-Fixture
        $result = Invoke-Lifecycle 'cancel-second'
        Assert-TestTrue ($result.Returned -and $result.Cancelled -and @($result.Published).Count -eq 0 -and $result.Callbacks -eq 2) 'Second-stage cancellation was not reported.'
        Assert-TestTrue ((Read-TestVersion $fixture.LatestUE) -eq 'new' -and (Read-TestVersion $fixture.LatestMax) -eq 'new') 'Completed build artifacts were lost.'
        Assert-Deployed 'old' 'old'
    }
    Test-Case 'A failed first process guard prevents compilation' {
        Reset-Fixture
        $result = Invoke-Lifecycle 'guard-first'
        Assert-TestTrue (!$result.Returned -and $result.Guards -eq 1 -and $result.Callbacks -eq 1) 'First guard failure was ignored.'
        Assert-NotBuilt
        Assert-Deployed 'old' 'old'
    }
    Test-Case 'A failed second process guard prevents deployment' {
        Reset-Fixture
        $result = Invoke-Lifecycle 'guard-second'
        Assert-TestTrue (!$result.Returned -and $result.Guards -eq 2 -and $result.Callbacks -eq 2) 'Second guard failure was ignored.'
        Assert-Deployed 'old' 'old'
    }
    Test-Case 'BuildOnly bypasses callbacks and process guards' {
        Reset-Fixture
        $result = Invoke-Lifecycle 'build-only'
        Assert-TestTrue ($result.Returned -and !$result.Cancelled -and @($result.Published).Count -eq 0 -and $result.Callbacks -eq 0 -and $result.Guards -eq 0) 'BuildOnly entered application lifecycle flow.'
        Assert-TestTrue ((Read-TestVersion $fixture.LatestUE) -eq 'new' -and (Read-TestVersion $fixture.LatestMax) -eq 'new') 'BuildOnly did not compile both targets.'
        Assert-Deployed 'old' 'old'
    }
    Test-Case 'Check bypasses callbacks, guards, compilation and deployment' {
        Reset-Fixture
        $result = Invoke-Lifecycle 'check'
        Assert-TestTrue ($result.Returned -and $result.Callbacks -eq 0 -and $result.Guards -eq 0 -and @($result.Published).Count -eq 0) 'Check entered the lifecycle flow.'
        Assert-NotBuilt
        Assert-Deployed 'old' 'old'
    }
    Test-Case 'Reopen preference defaults off and preserves comments/unknown INI settings' {
        $original = [IO.File]::ReadAllText($fixture.Config)
        Write-TestFile $fixture.Config ("; preserve this comment`r`n" + $original + "`r`n[Custom]`r`nKeepMe=unchanged`r`n")
        $result = Invoke-Lifecycle 'preference'
        Assert-TestTrue $result.PreferencePassed 'Preference round-trip failed.'
    }
} finally {
    Remove-TestTree $testRoot $buildRoot
}
Write-Host "$script:passed passed; $script:failed failed."
if ($script:failed) { throw 'Publish lifecycle checks failed.' }
