# Exercise the real modal dialog using mock process/preference callbacks.
# Only these short-lived test dialogs are clicked; no Max or UE process is touched.
param([string]$AssemblyPath = '')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'test_support.ps1')
if (!$AssemblyPath) { $AssemblyPath = Join-Path $repoRoot 'WyVehicleAssetBridgeManager.exe' }
$AssemblyPath = [IO.Path]::GetFullPath($AssemblyPath)

# Windows PowerShell provides the same .NET Framework WinForms runtime as Manager.
if ($PSVersionTable.PSEdition -ne 'Desktop' -or [Threading.Thread]::CurrentThread.ApartmentState -ne 'STA') {
    $hostExe = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $result = Invoke-TestProcess $hostExe @('-NoProfile','-STA','-ExecutionPolicy','Bypass','-File',$PSCommandPath,'-AssemblyPath',$AssemblyPath) $repoRoot 30
    Write-Host $result.Output
    if ($result.ExitCode -ne 0) { throw "Publish close dialog tests failed ($($result.ExitCode))." }
    return
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.dll,System.Core.dll,System.Windows.Forms.dll,System.Drawing.dll -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Reflection;
using System.Windows.Forms;

public static class PublishCloseDialogTests
{
    private static Type dialogType;
    private static int passed, failed;

    private static void Assert(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    private static T Control<T>(Form dialog, string name) where T : System.Windows.Forms.Control
    {
        var matches = dialog.Controls.Find(name, true);
        Assert(matches.Length == 1 && matches[0] is T, "Missing or ambiguous control: " + name);
        return (T)matches[0];
    }

    private static int CheckBoxCount(System.Windows.Forms.Control parent)
    {
        int count = parent is CheckBox ? 1 : 0;
        foreach (System.Windows.Forms.Control child in parent.Controls) count += CheckBoxCount(child);
        return count;
    }

    private static void AssertStillOpen(Form dialog)
    {
        Assert(dialog.Visible && dialog.DialogResult == DialogResult.None, "Dialog closed while the process was still running.");
        var status = Control<Label>(dialog, "PublishCloseStatus");
        Assert(status.Visible && status.Text.Length > 0, "The same dialog did not display the error.");
    }

    private static object InvokeDialog(string methodName, object[] arguments, params Action<Form>[] steps)
    {
        Exception failure = null;
        int step = 0;
        var watch = Stopwatch.StartNew();
        using (var timer = new System.Windows.Forms.Timer { Interval = 40 })
        {
            timer.Tick += delegate {
                Form dialog = null;
                foreach (Form candidate in Application.OpenForms)
                    if (candidate.GetType() == dialogType) { dialog = candidate; break; }
                if (dialog == null) return;
                timer.Stop();
                try
                {
                    Assert(watch.ElapsedMilliseconds < 5000, "Timed out waiting for test dialog.");
                    Assert(step < steps.Length, "Final action did not close the test dialog.");
                    steps[step++](dialog);
                }
                catch (Exception ex)
                {
                    failure = ex;
                    dialog.Close();
                }
                if (failure == null) timer.Start();
            };
            timer.Start();
            object result;
            try
            {
                var method = dialogType.GetMethod(methodName, BindingFlags.NonPublic | BindingFlags.Static);
                Assert(method != null, "Missing dialog method: " + methodName);
                result = method.Invoke(null, arguments);
            }
            catch (TargetInvocationException ex) { throw ex.InnerException ?? ex; }
            finally { timer.Stop(); }
            if (failure != null) throw failure;
            Assert(step == steps.Length, "Dialog returned before every test action ran.");
            return result;
        }
    }

    private static void Case(string name, Action test)
    {
        try { test(); ++passed; Console.WriteLine("PASS " + name); }
        catch (Exception ex) { ++failed; Console.WriteLine("FAIL " + name + " : " + ex.Message); }
    }

    public static int Run(string assemblyPath)
    {
        Assert(System.Threading.Thread.CurrentThread.GetApartmentState() == System.Threading.ApartmentState.STA, "Tests require STA.");
        dialogType = Assembly.LoadFrom(assemblyPath).GetType("WyVehicleAssetBridge.Setup.PublishCloseDialog", true);
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);

        Case("Max stays open until process recheck passes; no reopen checkbox", delegate {
            bool running = true;
            int checks = 0;
            var arguments = new object[] { null, new Func<bool>(delegate { ++checks; return running; }) };
            var result = InvokeDialog("ConfirmMax", arguments,
                delegate(Form dialog) {
                    Assert(CheckBoxCount(dialog) == 0, "Max must not offer automatic reopen.");
                    Assert(Control<Button>(dialog, "ConfirmPublishClose").Text == "\u6211\u5df2\u5173\u95ed", "Wrong Max confirm caption.");
                    Assert(Control<Button>(dialog, "CancelPublishClose").Text == "\u4e0d\u53d1\u5e03", "Wrong Max cancel caption.");
                    Control<Button>(dialog, "ConfirmPublishClose").PerformClick();
                    AssertStillOpen(dialog);
                    Assert(checks == 1, "First click did not recheck Max.");
                },
                delegate(Form dialog) {
                    running = false;
                    Control<Button>(dialog, "ConfirmPublishClose").PerformClick();
                });
            Assert((bool)result && checks == 2, "Max did not confirm after the second recheck.");
        });

        Case("Max do not publish cancels without checking processes", delegate {
            var result = InvokeDialog("ConfirmMax", new object[] { null, new Func<bool>(delegate { throw new Exception("Unexpected process check"); }) },
                delegate(Form dialog) { Control<Button>(dialog, "CancelPublishClose").PerformClick(); });
            Assert(!(bool)result, "Max cancel returned success.");
        });

        Case("Max window close cancels", delegate {
            var result = InvokeDialog("ConfirmMax", new object[] { null, new Func<bool>(delegate { return false; }) },
                delegate(Form dialog) { dialog.Close(); });
            Assert(!(bool)result, "Max window close returned success.");
        });

        Case("UE manual close rechecks and never reopens or changes preference", delegate {
            bool running = true, savedPreference = true;
            int checks = 0, saves = 0;
            var arguments = new object[] { null, true,
                new Action<bool>(delegate(bool value) { ++saves; savedPreference = value; }),
                new Func<bool>(delegate { ++checks; return running; }), false };
            var result = InvokeDialog("ConfirmUE", arguments,
                delegate(Form dialog) {
                    Control<Button>(dialog, "ConfirmManualUEClose").PerformClick();
                    AssertStillOpen(dialog);
                    Assert(Control<CheckBox>(dialog, "ReopenUEAfterPublish").Checked, "Manual retry changed the checkbox.");
                },
                delegate(Form dialog) {
                    running = false;
                    Control<Button>(dialog, "ConfirmManualUEClose").PerformClick();
                });
            Assert(result.ToString() == "Manual" && !(bool)arguments[4], "Manual close enabled automatic reopen.");
            Assert(checks == 2 && saves == 0 && savedPreference, "Manual close changed saved preference or skipped recheck.");
        });

        Case("UE automatic close preserves initially checked reopen preference", delegate {
            int saves = 0, checks = 0;
            var arguments = new object[] { null, true, new Action<bool>(delegate { ++saves; }),
                new Func<bool>(delegate { ++checks; return true; }), false };
            var result = InvokeDialog("ConfirmUE", arguments,
                delegate(Form dialog) { Control<Button>(dialog, "ConfirmPublishClose").PerformClick(); });
            Assert(result.ToString() == "Automatic" && (bool)arguments[4], "Automatic close lost selected reopen.");
            Assert(saves == 0 && checks == 0, "Automatic confirmation unexpectedly saved preferences or performed manual close verification.");
        });

        Case("UE checkbox changes save immediately, including cancellation", delegate {
            var saved = new List<bool>();
            var arguments = new object[] { null, false, new Action<bool>(delegate(bool value) { saved.Add(value); }),
                new Func<bool>(delegate { return true; }), false };
            var result = InvokeDialog("ConfirmUE", arguments,
                delegate(Form dialog) {
                    var checkbox = Control<CheckBox>(dialog, "ReopenUEAfterPublish");
                    checkbox.Checked = true;
                    Assert(saved.Count == 1 && saved[0], "Checking did not save immediately.");
                    checkbox.Checked = false;
                    Assert(saved.Count == 2 && !saved[1], "Unchecking did not save immediately.");
                    Control<Button>(dialog, "CancelPublishClose").PerformClick();
                });
            Assert(result.ToString() == "Cancel" && !(bool)arguments[4], "UE cancellation returned a publish action.");
        });

        Case("UE preference write failure blocks automatic action but allows manual close", delegate {
            int saves = 0, checks = 0;
            var arguments = new object[] { null, false,
                new Action<bool>(delegate { ++saves; throw new System.IO.IOException("Simulated INI write failure"); }),
                new Func<bool>(delegate { ++checks; return false; }), false };
            var result = InvokeDialog("ConfirmUE", arguments,
                delegate(Form dialog) {
                    Control<CheckBox>(dialog, "ReopenUEAfterPublish").Checked = true;
                    AssertStillOpen(dialog);
                    Assert(!Control<Button>(dialog, "ConfirmPublishClose").Enabled, "Automatic action is enabled after failed preference save.");
                    Assert(Control<Button>(dialog, "ConfirmManualUEClose").Enabled, "Manual action was disabled by preference save failure.");
                    Control<Button>(dialog, "ConfirmPublishClose").PerformClick();
                    AssertStillOpen(dialog);
                    Control<Button>(dialog, "ConfirmManualUEClose").PerformClick();
                });
            Assert(result.ToString() == "Manual" && !(bool)arguments[4] && saves == 1 && checks == 1,
                "Manual fallback did not preserve failed-save behavior.");
        });

        Case("UE window close cancels and does not reopen", delegate {
            var arguments = new object[] { null, true, new Action<bool>(delegate { }), new Func<bool>(delegate { return false; }), false };
            var result = InvokeDialog("ConfirmUE", arguments, delegate(Form dialog) { dialog.Close(); });
            Assert(result.ToString() == "Cancel" && !(bool)arguments[4], "UE window close returned a publish action.");
        });

        Console.WriteLine("Publish close dialog tests: " + passed + " passed, " + failed + " failed.");
        return failed;
    }
}
'@

if ([PublishCloseDialogTests]::Run($AssemblyPath) -ne 0) { throw 'Publish close dialog tests failed.' }
