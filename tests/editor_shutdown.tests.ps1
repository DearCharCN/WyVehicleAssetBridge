# Exercises the real shutdown implementation against short-lived WinForms fixtures.
# Only fixture windows receive input; no Unreal Editor or Max instance is targeted.
param([string]$AssemblyPath = '')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'test_support.ps1')
if (!$AssemblyPath) { $AssemblyPath = Join-Path $repoRoot 'WyVehicleAssetBridgeManager.exe' }
$AssemblyPath = [IO.Path]::GetFullPath($AssemblyPath)
$buildRoot = Join-Path $repoRoot 'ue_plugins\build'
$testRoot = Join-Path $buildRoot ('tests-editor-shutdown-' + [Guid]::NewGuid().ToString('N'))
Assert-TestChild $testRoot $buildRoot
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
$source = @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

internal static class ShutdownFixtureTests
{
    private const BindingFlags Flags = BindingFlags.Instance | BindingFlags.Static | BindingFlags.NonPublic;
    private static Type appType;
    private static MethodInfo shutdown;
    private static Form runner;
    private static string fixtureRoot;
    private static int passed;

    [STAThread]
    private static int Main(string[] args)
    {
        Application.EnableVisualStyles();
        if (args.Length > 0 && args[0] == "fixture")
        {
            Application.Run(new Fixture(args[1], args[2]));
            return 0;
        }
        try
        {
            var assembly = Assembly.LoadFrom(args[0]);
            appType = assembly.GetType("WyVehicleAssetBridge.Setup.RunningApplication", true);
            shutdown = assembly.GetType("WyVehicleAssetBridge.Setup.EditorShutdown", true).GetMethod("SaveAndCloseAsync", Flags);
            fixtureRoot = args[1];
            var input = assembly.GetType("WyVehicleAssetBridge.Setup.EditorShutdown+INPUT", true);
            Assert(Marshal.SizeOf(input) == 40, "x64 SendInput structure must have Windows INPUT size 40.");
            runner = new Form { Text = "Temporary editor lifecycle verification", ClientSize = new Size(410, 90), StartPosition = FormStartPosition.CenterScreen };
            runner.Controls.Add(new Label { Text = "Testing temporary windows only.\r\nPlease leave keyboard input idle during verification.", Dock = DockStyle.Fill, Padding = new Padding(12) });
            int exit = 0;
            runner.Shown += async delegate {
                try
                {
                    Assert(SynchronizationContext.Current is WindowsFormsSynchronizationContext, "Tests require the WinForms UI synchronization context.");
                    await RunCase("normal");
                    await RunCase("modal");
                    await RunCase("cancel");
                    await RunCase("mismatch");
                    await RunCase("crash");
                    Console.WriteLine("All " + passed + " editor shutdown tests passed.");
                }
                catch (Exception error) { Console.WriteLine("FAIL " + error); exit = 1; }
                finally { runner.Close(); }
            };
            Application.Run(runner);
            return exit;
        }
        catch (Exception error) { Console.WriteLine("FAIL " + error); return 1; }
    }

    private static async Task RunCase(string mode)
    {
        string log = Path.Combine(fixtureRoot, mode + ".log");
        var start = new ProcessStartInfo(Application.ExecutablePath, "fixture " + mode + " \"" + log + "\"") { UseShellExecute = false };
        using (var process = Process.Start(start))
        using (var cancellation = new CancellationTokenSource())
        {
            try
            {
                var deadline = DateTime.UtcNow.AddSeconds(8);
                while (!Contains(log, "ready"))
                {
                    Assert(!process.HasExited && DateTime.UtcNow < deadline, "Fixture did not become ready: " + mode);
                    await Task.Delay(50);
                }
                process.Refresh();
                Assert(process.MainWindowHandle != IntPtr.Zero, "Fixture has no main window.");
                object app = Activator.CreateInstance(appType, true);
                Set(app, "Id", process.Id);
                Set(app, "Name", Path.GetFileName(Application.ExecutablePath));
                Set(app, "Executable", Application.ExecutablePath);
                Set(app, "IdentityKnown", true);
                Set(app, "Headless", false);
                DateTime started = process.StartTime.ToUniversalTime();
                Set(app, "StartedUtc", (DateTime?)(mode == "mismatch" ? started.AddSeconds(-1) : started));
                if (mode == "cancel") cancellation.CancelAfter(1700);
                Exception failure = null;
                string result = null;
                var messages = new List<string>();
                try
                {
                    var task = (Task)shutdown.Invoke(null, new object[] { app, new Action<string>(messages.Add), cancellation.Token });
                    await task;
                    result = Convert.ToString(task.GetType().GetProperty("Result").GetValue(task, null));
                }
                catch (Exception error) { failure = error is TargetInvocationException ? error.InnerException : error; }
                if (mode == "normal")
                {
                    Assert(failure == null && result == "Closed", "Normal save/close did not complete: " + failure);
                    Assert(Before(log, "save", "close"), "WM_CLOSE arrived before Ctrl+Shift+S.");
                    Assert(process.HasExited && process.ExitCode == 0, "Normal fixture did not exit cleanly.");
                }
                else if (mode == "modal")
                {
                    Assert(failure == null && result == "Closed", "Modal save/close did not complete: " + failure);
                    Assert(Before(log, "save", "modal-open") && Before(log, "modal-open", "modal-close") && Before(log, "modal-close", "close"), "WM_CLOSE did not wait for the save dialog to finish.");
                }
                else if (mode == "cancel")
                {
                    Assert(failure is OperationCanceledException, "Cancellation did not interrupt the wait: " + failure);
                    Assert(Contains(log, "save") && Contains(log, "modal-open"), "Cancellation fixture never entered the save dialog.");
                    Assert(!Contains(log, "close") && !process.HasExited, "Cancellation closed or terminated the application.");
                }
                else if (mode == "mismatch")
                {
                    Assert(failure is InvalidOperationException, "Changed process start time was accepted.");
                    Assert(!Contains(log, "save") && !Contains(log, "close") && !process.HasExited, "PID mismatch still sent input or close.");
                }
                else if (mode == "crash")
                {
                    Assert(failure is InvalidOperationException, "Crash during save was treated as normal success: " + result);
                    Assert(Contains(log, "save") && process.HasExited && process.ExitCode == 17, "Crash fixture did not exercise the save-time failure path.");
                }
                passed++;
                Console.WriteLine("PASS " + mode);
            }
            finally
            {
                // Cleanup is strictly limited to the process created above from this
                // private test executable; production shutdown never uses Kill.
                if (!process.HasExited)
                {
                    Assert(String.Equals(process.MainModule.FileName, Application.ExecutablePath, StringComparison.OrdinalIgnoreCase), "Refusing cleanup of a non-fixture process.");
                    process.Kill();
                    process.WaitForExit(5000);
                }
                runner.Activate();
            }
            await Task.Delay(150);
        }
    }

    private static void Set(object app, string name, object value) { appType.GetField(name, Flags).SetValue(app, value); }
    private static void Assert(bool condition, string message) { if (!condition) throw new Exception(message); }
    private static bool Contains(string path, string value) { return Array.IndexOf(Read(path), value) >= 0; }
    private static bool Before(string path, string first, string second)
    {
        string[] events = Read(path);
        int a = Array.IndexOf(events, first), b = Array.IndexOf(events, second);
        return a >= 0 && b > a;
    }
    private static string[] Read(string path)
    {
        if (!File.Exists(path)) return new string[0];
        using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
        using (var reader = new StreamReader(stream)) return reader.ReadToEnd().Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
    }

    private sealed class Fixture : Form
    {
        private readonly string mode, log;
        private readonly System.Windows.Forms.Timer lifetime = new System.Windows.Forms.Timer { Interval = 15000 };
        internal Fixture(string mode, string log)
        {
            this.mode = mode;
            this.log = log;
            Text = "Temporary save-close fixture: " + mode;
            ClientSize = new Size(400, 100);
            StartPosition = FormStartPosition.CenterScreen;
            KeyPreview = true;
            Controls.Add(new Label { Text = "Disposable test data; no user project is loaded.", Dock = DockStyle.Fill, Padding = new Padding(12) });
            Shown += delegate { Activate(); Record("ready"); lifetime.Start(); };
            lifetime.Tick += delegate { Environment.Exit(20); };
            FormClosing += delegate { Record("close"); };
        }
        protected override bool ProcessCmdKey(ref Message message, Keys keys)
        {
            if (keys == (Keys.Control | Keys.Shift | Keys.S))
            {
                Record("save");
                if (mode == "crash")
                {
                    var crash = new System.Windows.Forms.Timer { Interval = 200 };
                    crash.Tick += delegate { Environment.Exit(17); };
                    crash.Start();
                }
                else if (mode == "modal" || mode == "cancel") BeginInvoke(new Action(ShowSaveDialog));
                return true;
            }
            return base.ProcessCmdKey(ref message, keys);
        }
        private void ShowSaveDialog()
        {
            using (var dialog = new Form { Text = "Temporary save dialog", ClientSize = new Size(310, 90), StartPosition = FormStartPosition.CenterParent })
            using (var timer = new System.Windows.Forms.Timer { Interval = 3500 })
            {
                dialog.Controls.Add(new Label { Text = "Simulating a save operation.", Dock = DockStyle.Fill, Padding = new Padding(12) });
                timer.Tick += delegate { timer.Stop(); dialog.Close(); };
                dialog.Shown += delegate { Record("modal-open"); if (mode == "modal") timer.Start(); };
                dialog.ShowDialog(this);
                Record("modal-close");
            }
        }
        private void Record(string value) { File.AppendAllText(log, value + Environment.NewLine); }
        protected override void Dispose(bool disposing) { if (disposing) lifetime.Dispose(); base.Dispose(disposing); }
    }
}
'@
try {
    $sourcePath = Join-Path $testRoot 'ShutdownFixtureTests.cs'
    $executable = Join-Path $testRoot 'ShutdownFixtureTests.exe'
    Write-TestFile $sourcePath $source
    $compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    & $compiler /nologo /target:exe /platform:x64 /codepage:65001 "/out:$executable" /reference:System.dll,System.Core.dll,System.Windows.Forms.dll,System.Drawing.dll $sourcePath
    if ($LASTEXITCODE -ne 0) { throw 'Shutdown fixture compilation failed.' }
    $result = Invoke-TestProcess $executable @($AssemblyPath,$testRoot) $testRoot 60
    Write-Host $result.Output
    Assert-TestTrue ($result.ExitCode -eq 0) 'Editor shutdown fixture tests failed.'
} finally { Remove-TestTree $testRoot $buildRoot }
