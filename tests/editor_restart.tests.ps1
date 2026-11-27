# Isolated restart-plan and UE INI checks. Never starts or closes Unreal Editor.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$source = Get-Content -LiteralPath (Join-Path $repoRoot 'src\WyVehicleAssetBridgeManager\EditorRestart.cs') -Raw -Encoding UTF8
$fixture = Join-Path $env:TEMP ('wy-editor-restart-' + [Guid]::NewGuid().ToString('N'))
$harness = @'
namespace WyVehicleAssetBridge.Setup
{
    internal sealed class BuildTarget { internal string Name, ProjectFile; }
    internal static class RunningApplications
    {
        internal static void EnsureClosed(System.Collections.Generic.IEnumerable<BuildTarget> targets) { }
    }

    public static class EditorRestartFixture
    {
        private static void Check(bool condition, string message)
        {
            if (!condition) throw new System.Exception(message);
        }

        public static void Run(string root)
        {
            var restart = new EditorRestart { Executable = @"C:\UE\UnrealEditor.exe", ProjectFile = @"C:\My Project\Car.uproject" };
            Check(restart.Executable.EndsWith("UnrealEditor.exe") && restart.ProjectFile.EndsWith(".uproject"), "Invalid restart fixture");
            System.IO.Directory.CreateDirectory(root);
            string text = "; 中文注释\r\n[AssetEditorSubsystem]\r\nOpenAssetsAtExit=/Game/A.A\r\nOpenAssetsAtExit=/Game/B.B\r\nCleanShutdown=True\r\n"
                + "[EditorStartup]\r\nLastLevel=/Game/地图/L_Test\r\n"
                + "[/Script/UnrealEd.EditorLoadingSavingSettings]\r\n\tRestoreOpenAssetTabsOnRestart = NeverRestore  ; keep this\r\nUnknown=Value\r\n";
            string expected = text.Replace("NeverRestore", "AlwaysRestore");
            int index = 0;
            foreach (System.Text.Encoding encoding in new System.Text.Encoding[] {
                new System.Text.UTF8Encoding(false), new System.Text.UTF8Encoding(true),
                new System.Text.UnicodeEncoding(false, true), new System.Text.UnicodeEncoding(true, true),
                new System.Text.UTF32Encoding(false, true) })
            {
                string path = System.IO.Path.Combine(root, "encoding-" + index++ + ".ini");
                System.IO.File.WriteAllText(path, text, encoding);
                byte[] original = System.IO.File.ReadAllBytes(path);
                var settings = new EditorRestartSettings(path);
                Check(settings.LastLevel == "/Game/地图/L_Test", "LastLevel was not retained");
                settings.Apply();
                byte[] wanted = encoding.GetPreamble().Concat(encoding.GetBytes(expected)).ToArray();
                Check(System.IO.File.ReadAllBytes(path).SequenceEqual(wanted), "INI content or encoding was changed outside the requested scalar key");
                Check(System.IO.File.ReadAllBytes(settings.BackupFile).SequenceEqual(original), "Atomic replacement did not retain the original backup");
                settings.Rollback();
                Check(System.IO.File.ReadAllBytes(path).SequenceEqual(original), "Rollback did not restore exact original bytes");
            }

            string fresh = System.IO.Path.Combine(root, "new.ini");
            var added = new EditorRestartSettings(fresh);
            added.Apply();
            Check(System.IO.File.ReadAllText(fresh).Contains("RestoreOpenAssetTabsOnRestart=AlwaysRestore"), "Missing INI was not initialized");
            added.Rollback();
            Check(!System.IO.File.Exists(fresh), "Rollback retained a newly created INI");

            string absent = System.IO.Path.Combine(root, "absent-key.ini");
            const string absentText = "[AssetEditorSubsystem]\nOpenAssetsAtExit=/Game/A.A\nOpenAssetsAtExit=/Game/B.B\n";
            System.IO.File.WriteAllText(absent, absentText, new System.Text.UTF8Encoding(false));
            var appended = new EditorRestartSettings(absent);
            appended.Apply();
            Check(System.IO.File.ReadAllText(absent).StartsWith(absentText), "Appending the setting damaged original repeated keys");

            string race = System.IO.Path.Combine(root, "race.ini");
            System.IO.File.WriteAllText(race, text, new System.Text.UTF8Encoding(false));
            var changedBefore = new EditorRestartSettings(race);
            System.IO.File.AppendAllText(race, "; another writer\r\n");
            string newer = System.IO.File.ReadAllText(race);
            bool rejected = false;
            try { changedBefore.Apply(); } catch (System.IO.IOException) { rejected = true; }
            Check(rejected && System.IO.File.ReadAllText(race) == newer, "Concurrent changes before writing were overwritten");

            var changedAfter = new EditorRestartSettings(race);
            changedAfter.Apply();
            System.IO.File.AppendAllText(race, "; changed after launch attempt\r\n");
            newer = System.IO.File.ReadAllText(race);
            rejected = false;
            try { changedAfter.Rollback(); } catch (System.IO.IOException) { rejected = true; }
            Check(rejected && System.IO.File.ReadAllText(race) == newer, "Rollback overwrote a concurrent edit");

            var plan = EditorRestart.CreateStartInfo(@"C:\UE\UnrealEditor.exe", @"C:\My Project\Car.uproject", "/Game/Maps/L_Test");
            Check(plan.FileName == @"C:\UE\UnrealEditor.exe" && plan.UseShellExecute && plan.WindowStyle == System.Diagnostics.ProcessWindowStyle.Normal, "Incorrect editor launch mode");
            Check(plan.Arguments == "\"C:\\My Project\\Car.uproject\" \"/Game/Maps/L_Test\"", "Project or map arguments were incorrectly quoted");
            Check(plan.WorkingDirectory == @"C:\My Project", "Incorrect working directory");
            plan = EditorRestart.CreateStartInfo(@"C:\UE\UnrealEditor.exe", @"C:\My Project\Car.uproject", "/Game/Map\" -ExecCmds=quit");
            Check(!plan.Arguments.Contains("ExecCmds"), "An invalid map injected additional startup arguments");
            Check(!EditorRestart.IsMapPath("/Game/../Other") && !EditorRestart.IsMapPath("/Game/Map?game=x"), "Invalid map names were accepted");
        }
    }
}
'@
try {
    Add-Type -TypeDefinition ($source + [Environment]::NewLine + $harness) -Language CSharp
    [WyVehicleAssetBridge.Setup.EditorRestartFixture]::Run($fixture)
    Write-Host 'PASS: UE restart arguments, repeated asset arrays, comments, encoding, atomic backup, rollback and concurrent-edit guards.'
} finally {
    $resolved = [IO.Path]::GetFullPath($fixture)
    $tempRoot = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
    if ($resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $resolved)) {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
