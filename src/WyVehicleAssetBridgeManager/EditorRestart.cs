using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

namespace WyVehicleAssetBridge.Setup
{
    internal sealed class EditorRestart
    {
        internal string Executable, ProjectFile;

        internal void Start()
        {
            string executable = Path.GetFullPath(Executable);
            string project = Path.GetFullPath(ProjectFile);
            if (!File.Exists(executable) || !String.Equals(Path.GetFileName(executable), "UnrealEditor.exe", StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("找不到用于重新打开 UE 的 UnrealEditor.exe：" + executable);
            if (!File.Exists(project) || !String.Equals(Path.GetExtension(project), ".uproject", StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("找不到需要重新打开的 UE 工程：" + project);

            var target = new BuildTarget { Name = "UE", ProjectFile = project };
            RunningApplications.EnsureClosed(new[] { target });
            var settings = new EditorRestartSettings(Path.Combine(Path.GetDirectoryName(project), "Saved", "Config", "WindowsEditor", "EditorPerProjectUserSettings.ini"));
            var start = CreateStartInfo(executable, project, settings.LastLevel);
            try
            {
                settings.Apply();
                RunningApplications.EnsureClosed(new[] { target });
                using (Process process = Process.Start(start))
                    if (process == null) throw new InvalidOperationException("Windows 未返回 UE 启动进程。");
            }
            catch (Exception startError)
            {
                try { settings.Rollback(); }
                catch (Exception restoreError)
                {
                    throw new InvalidOperationException("UE 未能重新打开，恢复窗口设置时也遇到错误。原设置备份：" + settings.BackupFile,
                        new AggregateException(startError, restoreError));
                }
                throw;
            }
        }

        internal static ProcessStartInfo CreateStartInfo(string executable, string project, string lastLevel)
        {
            // Reconstruct only the editor/project/map arguments. Replaying an old
            // command line could repeat a commandlet, unattended job or exit command.
            string arguments = Quote(project);
            if (IsMapPath(lastLevel)) arguments += " " + Quote(lastLevel);
            return new ProcessStartInfo {
                FileName = executable, Arguments = arguments,
                WorkingDirectory = Path.GetDirectoryName(project),
                UseShellExecute = true, WindowStyle = ProcessWindowStyle.Normal
            };
        }

        internal static bool IsMapPath(string value)
        {
            return !String.IsNullOrEmpty(value) && Regex.IsMatch(value, @"\A/(?:[\p{L}\p{N}_-]+/)*[\p{L}\p{N}_-]+\z");
        }

        private static string Quote(string value)
        {
            // Windows command-line quoting; the executable is never passed to a shell.
            return "\"" + Regex.Replace(Regex.Replace(value, @"(\\*)""", "$1$1\\\""), @"(\\+)$", "$1$1") + "\"";
        }
    }

    // UE INIs allow repeated keys (notably OpenAssetsAtExit), unlike the Manager
    // settings file. Change only this scalar key without parsing into a dictionary.
    internal sealed class EditorRestartSettings
    {
        private const string Section = "/Script/UnrealEd.EditorLoadingSavingSettings";
        private const string Key = "RestoreOpenAssetTabsOnRestart";
        private readonly string file;
        private byte[] before;
        private readonly byte[] after;
        private bool applied;
        internal readonly string LastLevel;
        internal string BackupFile { get; private set; }

        internal EditorRestartSettings(string path)
        {
            file = Path.GetFullPath(path);
            before = File.Exists(file) ? File.ReadAllBytes(file) : null;
            Encoding encoding;
            int preamble;
            string text = Decode(before ?? new byte[0], out encoding, out preamble);
            string newline = text.Contains("\r\n") ? "\r\n" : "\n";
            var lines = new List<string>(Regex.Split(text, "(?<=\\n)"));
            string section = "", lastLevel = "";
            int insert = -1;
            bool found = false;
            for (int i = 0; i < lines.Count; i++)
            {
                string trimmed = lines[i].Trim();
                if (trimmed.StartsWith("[") && trimmed.EndsWith("]"))
                    section = trimmed.Substring(1, trimmed.Length - 2).Trim();
                if (String.Equals(section, Section, StringComparison.OrdinalIgnoreCase)) insert = i + 1;
                if (trimmed.Length == 0 || trimmed.StartsWith(";") || trimmed.StartsWith("#")) continue;
                int equals = lines[i].IndexOf('=');
                if (equals < 0) continue;
                string key = lines[i].Substring(0, equals).Trim();
                if (String.Equals(section, "EditorStartup", StringComparison.OrdinalIgnoreCase) && String.Equals(key, "LastLevel", StringComparison.OrdinalIgnoreCase))
                    lastLevel = lines[i].Substring(equals + 1).Trim().Trim('"');
                if (!String.Equals(section, Section, StringComparison.OrdinalIgnoreCase) || !String.Equals(key, Key, StringComparison.OrdinalIgnoreCase)) continue;

                // Keep indentation, comments, line endings and all unrelated bytes.
                Match match = Regex.Match(lines[i], @"\A([^=]*=[ \t]*)([^;\r\n]*?)([ \t]*(?:;[^\r\n]*)?)(\r?\n)?\z");
                if (!match.Success) throw new InvalidDataException("UE 窗口恢复设置格式无法安全修改：" + file);
                lines[i] = match.Groups[1].Value + "AlwaysRestore" + match.Groups[3].Value + match.Groups[4].Value;
                found = true;
            }
            if (!found)
            {
                if (insert < 0)
                {
                    if (text.Length > 0 && !text.EndsWith("\n")) lines.Add(newline);
                    lines.Add("[" + Section + "]" + newline + Key + "=AlwaysRestore" + newline);
                }
                else
                {
                    if (insert > 0 && !lines[insert - 1].EndsWith("\n")) lines[insert - 1] += newline;
                    lines.Insert(insert, Key + "=AlwaysRestore" + newline);
                }
            }
            LastLevel = EditorRestart.IsMapPath(lastLevel) ? lastLevel : "";
            byte[] body = encoding.GetBytes(String.Concat(lines));
            after = new byte[preamble + body.Length];
            if (preamble > 0) Buffer.BlockCopy(before, 0, after, 0, preamble);
            Buffer.BlockCopy(body, 0, after, preamble, body.Length);
        }

        internal void Apply()
        {
            if (Equal(before, after)) return;
            Directory.CreateDirectory(Path.GetDirectoryName(file));
            string temporary = file + ".wy-manager-" + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                File.WriteAllBytes(temporary, after);
                if (!Equal(ReadCurrent(), before)) throw new IOException("UE 配置在重开前被其它程序修改，请重新发布或手动打开 UE。");
                if (before == null) File.Move(temporary, file);
                else
                {
                    BackupFile = file + ".wy-manager-" + DateTime.UtcNow.ToString("yyyyMMdd-HHmmss") + "-" + Guid.NewGuid().ToString("N").Substring(0, 8) + ".bak";
                    File.Replace(temporary, file, BackupFile);
                }
                applied = true;
                // Detect a writer racing the check and atomic replacement. The
                // replacement backup is authoritative and must not be discarded.
                if (BackupFile != null)
                {
                    byte[] replaced = File.ReadAllBytes(BackupFile);
                    if (!Equal(replaced, before))
                    {
                        before = replaced;
                        throw new IOException("UE 配置在原子替换时发生变化，已取消重新打开。");
                    }
                }
            }
            finally { if (File.Exists(temporary)) File.Delete(temporary); }
        }

        internal void Rollback()
        {
            if (!applied) return;
            if (!Equal(ReadCurrent(), after)) throw new IOException("UE 配置已有后续改动，未覆盖这些改动；请从备份恢复窗口偏好。");
            if (before == null) File.Delete(file);
            else
            {
                string temporary = file + ".wy-manager-" + Guid.NewGuid().ToString("N") + ".tmp";
                try
                {
                    File.WriteAllBytes(temporary, before);
                    if (!Equal(ReadCurrent(), after)) throw new IOException("UE 配置在回退前发生变化，已保留现有文件。");
                    File.Replace(temporary, file, null);
                }
                finally { if (File.Exists(temporary)) File.Delete(temporary); }
            }
            applied = false;
        }

        private byte[] ReadCurrent() { return File.Exists(file) ? File.ReadAllBytes(file) : null; }
        private static bool Equal(byte[] a, byte[] b) { return a == null || b == null ? a == b : a.SequenceEqual(b); }

        private static string Decode(byte[] bytes, out Encoding encoding, out int preamble)
        {
            encoding = new UTF8Encoding(false, true);
            preamble = 0;
            foreach (Encoding candidate in new Encoding[] { new UTF32Encoding(false, true, true), new UTF32Encoding(true, true, true), new UTF8Encoding(true, true), new UnicodeEncoding(false, true, true), new UnicodeEncoding(true, true, true) })
            {
                byte[] prefix = candidate.GetPreamble();
                if (bytes.Length >= prefix.Length && bytes.Take(prefix.Length).SequenceEqual(prefix))
                {
                    encoding = candidate; preamble = prefix.Length; break;
                }
            }
            try { return encoding.GetString(bytes, preamble, bytes.Length - preamble); }
            catch (DecoderFallbackException)
            {
                // Never rewrite an unknown legacy encoding with guessed characters.
                throw new InvalidDataException("UE 用户配置不是有效的 UTF-8/UTF-16/UTF-32，未修改：请手动打开 UE。");
            }
        }
    }
}
