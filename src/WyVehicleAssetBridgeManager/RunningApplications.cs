using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Management;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace WyVehicleAssetBridge.Setup
{
    internal sealed class RunningApplication
    {
        internal int Id;
        internal string Name = "", Executable = "", CommandLine = "";
        internal DateTime? StartedUtc;
        internal bool IdentityKnown, Headless;
        internal string IdentityIssue = "";
    }

    internal static class RunningApplications
    {
        private const string Query = "SELECT ProcessId,Name,ExecutablePath,CommandLine,CreationDate FROM Win32_Process WHERE Name='UnrealEditor.exe' OR Name='UnrealEditor-Cmd.exe' OR Name='3dsmax.exe' OR Name='3dsmaxbatch.exe'";

        // An unknown identity is a deployment blocker, never a process to close.
        internal static List<RunningApplication> Find(BuildTarget target)
        {
            var result = new List<RunningApplication>();
            using (var searcher = new ManagementObjectSearcher(Query))
            using (var rows = searcher.Get())
            {
                foreach (ManagementObject row in rows)
                using (row)
                {
                    var app = new RunningApplication {
                        Id = Convert.ToInt32(row["ProcessId"]), Name = Convert.ToString(row["Name"]),
                        Executable = Convert.ToString(row["ExecutablePath"]), CommandLine = Convert.ToString(row["CommandLine"])
                    };
                    if (!Classify(target, app)) continue;
                    // The WMI snapshot may outlive its process. Compare creation times
                    // before attaching its identity to a possibly reused PID.
                    try
                    {
                        using (var process = Process.GetProcessById(app.Id))
                        {
                            if (process.HasExited) continue;
                            DateTime started = process.StartTime.ToUniversalTime();
                            string created = Convert.ToString(row["CreationDate"]);
                            if (String.IsNullOrWhiteSpace(created))
                                Unknown(app, "无法读取进程创建时间，不能自动关闭。");
                            else
                            {
                                try
                                {
                                    DateTime snapshotStarted = ManagementDateTimeConverter.ToDateTime(created).ToUniversalTime();
                                    if (Math.Abs((started - snapshotStarted).TotalMilliseconds) >= 1) continue;
                                    app.StartedUtc = started;
                                }
                                catch (ArgumentException) { Unknown(app, "进程创建时间无效，不能自动关闭。"); }
                                catch (FormatException) { Unknown(app, "进程创建时间无效，不能自动关闭。"); }
                            }
                        }
                    }
                    catch (ArgumentException) { continue; }
                    catch (InvalidOperationException) { continue; }
                    catch (Win32Exception) { Unknown(app, "无法核对进程创建时间，不能自动关闭。"); }
                    result.Add(app);
                }
            }
            result.Sort((a, b) => a.Id.CompareTo(b.Id));
            return result;
        }

        internal static void EnsureClosed(IEnumerable<BuildTarget> targets)
        {
            foreach (BuildTarget target in targets)
            {
                var running = Find(target);
                if (running.Count == 0) continue;
                RunningApplication app = running[0];
                string title = target.Name == "Max" ? "3ds Max" : "UE 工程编辑器";
                string reason = app.IdentityKnown ? "请先关闭目标 " + title : "无法确认 " + app.Name + " 是否属于目标 " + title + "，请手动检查并关闭";
                throw new InvalidOperationException(reason + "（进程 " + app.Id + "），再发布 " + target.Name + " 插件。仅编译可使用 -BuildOnly。");
            }
        }

        // Returns true for confirmed matches and unresolved potential matches.
        // Kept separate from WMI so identity and quoting edge cases can be tested.
        internal static bool Classify(BuildTarget target, RunningApplication app)
        {
            app.IdentityKnown = false;
            app.IdentityIssue = "";
            app.Headless = EqualsName(app.Name, "3dsmaxbatch.exe") || EqualsName(app.Name, "UnrealEditor-Cmd.exe");
            bool max = EqualsName(target.Name, "Max");
            if (max ? !EqualsName(app.Name, "3dsmax.exe") && !EqualsName(app.Name, "3dsmaxbatch.exe")
                    : !EqualsName(target.Name, "UE") || !EqualsName(app.Name, "UnrealEditor.exe") && !EqualsName(app.Name, "UnrealEditor-Cmd.exe"))
                return false;
            try
            {
                if (max)
                {
                    if (!IsAbsolutePath(app.Executable)) return Unknown(app, "无法读取 Max 可执行文件路径。");
                    if (!EqualsName(Path.GetFileName(app.Executable), app.Name)) return Unknown(app, "Max 进程名称与可执行文件路径不一致。");
                    if (!EqualsName(CanonicalPath(Path.GetDirectoryName(app.Executable)), CanonicalPath(target.MaxInstall))) return false;
                    app.IdentityKnown = true;
                    return true;
                }

                if (String.IsNullOrWhiteSpace(app.CommandLine)) return Unknown(app, "无法读取 UE 命令行，不能确定工程。");
                string[] arguments = SplitCommandLine(app.CommandLine);
                if (arguments.Length == 0) return Unknown(app, "无法解析 UE 命令行。");
                foreach (string argument in arguments)
                {
                    if (EqualsName(argument, "-unattended") || EqualsName(argument, "-nullrhi") ||
                        argument.StartsWith("-run=", StringComparison.OrdinalIgnoreCase)) app.Headless = true;
                }
                string project = null;
                for (int i = 1; i < arguments.Length; i++)
                {
                    string argument = arguments[i];
                    if (argument.StartsWith("-project=", StringComparison.OrdinalIgnoreCase) ||
                        argument.StartsWith("project=", StringComparison.OrdinalIgnoreCase))
                    {
                        if (project != null) return Unknown(app, "UE 命令行包含多个工程参数。");
                        project = argument.Substring(argument.IndexOf('=') + 1);
                    }
                }
                // UE uses the first positional token, not every path mentioned by
                // -ExecCmds, logs, scripts or other option values.
                if (project == null && arguments.Length > 1 && !arguments[1].StartsWith("-", StringComparison.Ordinal))
                    project = arguments[1];
                if (!IsAbsolutePath(project)) return Unknown(app, "UE 工程路径缺失或为相对路径，无法确定正在编辑的工程。");
                if (Directory.Exists(project)) project = LocalConfig.ProjectFile(project);
                if (!EqualsName(Path.GetExtension(project), ".uproject"))
                    return Unknown(app, "UE 命令行未提供可确认的工程文件。");
                if (!EqualsName(CanonicalPath(project), CanonicalPath(target.ProjectFile))) return false;
                if (!IsAbsolutePath(app.Executable) || !EqualsName(Path.GetFileName(app.Executable), app.Name))
                    return Unknown(app, "已识别目标工程，但无法核对 UE 可执行文件路径。");
                app.IdentityKnown = true;
                return true;
            }
            catch (IOException error) { return Unknown(app, "无法核对应用路径：" + error.Message); }
            catch (UnauthorizedAccessException error) { return Unknown(app, "无法读取应用路径：" + error.Message); }
            catch (ArgumentException error) { return Unknown(app, "应用路径无效：" + error.Message); }
            catch (NotSupportedException error) { return Unknown(app, "应用路径无法解析：" + error.Message); }
            catch (Win32Exception error) { return Unknown(app, "无法解析应用路径：" + error.Message); }
        }

        private static bool Unknown(RunningApplication app, string issue)
        {
            app.IdentityKnown = false;
            app.IdentityIssue = issue;
            return true;
        }

        private static bool EqualsName(string a, string b) { return String.Equals(a, b, StringComparison.OrdinalIgnoreCase); }

        internal static bool IsAbsolutePath(string path)
        {
            if (String.IsNullOrWhiteSpace(path)) return false;
            string value = path.Replace('/', '\\');
            return (value.Length >= 3 && Char.IsLetter(value[0]) && value[1] == ':' && value[2] == '\\') ||
                (value.StartsWith(@"\\", StringComparison.Ordinal) && !value.StartsWith(@"\\.\", StringComparison.Ordinal));
        }

        internal static string[] SplitCommandLine(string commandLine)
        {
            if (String.IsNullOrWhiteSpace(commandLine)) return new string[0];
            int count;
            IntPtr block = CommandLineToArgvW(commandLine, out count);
            if (block == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
            try
            {
                var arguments = new string[count];
                for (int i = 0; i < count; i++) arguments[i] = Marshal.PtrToStringUni(Marshal.ReadIntPtr(block, i * IntPtr.Size));
                return arguments;
            }
            finally { LocalFree(block); }
        }

        // Follow directory junctions and file symlinks before comparing identities.
        // Never resolve a relative process path against the Manager's own CWD.
        internal static string CanonicalPath(string path)
        {
            if (!IsAbsolutePath(path)) throw new ArgumentException("需要绝对路径。", "path");
            string full = Path.GetFullPath(path.Replace('/', '\\'));
            using (SafeFileHandle handle = CreateFile(full, 0, 7, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero))
            {
                if (handle.IsInvalid)
                {
                    int error = Marshal.GetLastWin32Error();
                    if (error != 2 && error != 3) throw new Win32Exception(error);
                    // A missing file may still be below an existing project junction.
                    return Path.Combine(PackageStore.ResolveDirectory(Path.GetDirectoryName(full)), Path.GetFileName(full)).TrimEnd('\\', '/');
                }
                var buffer = new StringBuilder(512);
                uint length = GetFinalPathNameByHandle(handle, buffer, (uint)buffer.Capacity, 0);
                if (length == 0) throw new Win32Exception(Marshal.GetLastWin32Error());
                if (length >= buffer.Capacity)
                {
                    buffer = new StringBuilder(checked((int)length + 1));
                    length = GetFinalPathNameByHandle(handle, buffer, (uint)buffer.Capacity, 0);
                    if (length == 0 || length >= buffer.Capacity) throw new Win32Exception(Marshal.GetLastWin32Error());
                }
                string value = buffer.ToString();
                if (value.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase)) value = @"\\" + value.Substring(8);
                else if (value.StartsWith(@"\\?\", StringComparison.OrdinalIgnoreCase)) value = value.Substring(4);
                return value.TrimEnd('\\', '/');
            }
        }

        [DllImport("shell32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr CommandLineToArgvW(string commandLine, out int count);
        [DllImport("kernel32.dll")]
        private static extern IntPtr LocalFree(IntPtr memory);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "CreateFileW")]
        private static extern SafeFileHandle CreateFile(string path, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "GetFinalPathNameByHandleW")]
        private static extern uint GetFinalPathNameByHandle(SafeFileHandle file, StringBuilder path, uint capacity, uint flags);
    }
}
