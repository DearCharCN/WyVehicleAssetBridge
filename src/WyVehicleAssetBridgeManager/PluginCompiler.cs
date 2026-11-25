using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace WyVehicleAssetBridge.Setup
{
    internal static class PluginCompiler
    {
        private static readonly object ReportGate = new object();

        internal static void Build(BuildTarget t, string sourceFingerprint, Action<string> report)
        {
            if (t == null) throw new ArgumentNullException("t");
            if (t.Name != "UE" && t.Name != "Max") throw new ArgumentException("未知的构建目标：" + t.Name);
            PackageStore.Guard(t.BuildRoot, t.Stage);
            PackageStore.Guard(t.BuildRoot, t.LogPath);
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(t.Stage)));
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(t.LogPath)));

            using (var log = new BuildLog(t.Name, t.LogPath, report))
            {
                try
                {
                    log.Write("开始构建。日志：" + t.LogPath);
                    if (t.Name == "UE")
                    {
                        var arguments = new[] {
                            "BuildPlugin", "-Plugin=" + Path.Combine(t.Source, "WyVehicleAssetBridge.uplugin"),
                            "-Package=" + t.Stage, "-HostPlatforms=Win64", "-NoTargetPlatforms",
                            "-Unattended", "-UTF8Output"
                        };
                        CheckUnrealBatchCompatibility(t.ToolPath, arguments);
                        Run(t.ToolPath, arguments, t.Source, null, log);
                    }
                    else BuildMax(t, sourceFingerprint, log);
                    log.EnsureHealthy();
                    log.Write("编译命令已完成，等待产物校验。");
                    log.EnsureHealthy();
                }
                catch (Exception ex)
                {
                    log.Write("构建失败：" + ex.Message);
                    throw;
                }
            }
        }

        private static void BuildMax(BuildTarget t, string sourceFingerprint, BuildLog log)
        {
            // Child-only environment: UE and Max may build concurrently without
            // altering each other's compiler/SDK selection or the parent process.
            var environment = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            if (!String.IsNullOrWhiteSpace(t.WindowsSdkPath))
            {
                string sdk = t.WindowsSdkPath.TrimEnd('\\', '/') + "\\";
                environment["CMAKE_WINDOWS_KITS_10_DIR"] = t.WindowsSdkPath;
                environment["WindowsSdkDir_10"] = sdk;
                environment["UniversalCRTSdkDir_10"] = sdk;
            }
            environment["WindowsSDKVersion"] = t.WindowsSdk + "\\";
            environment["UCRTVersion"] = t.WindowsSdk;

            string cache = Path.Combine(t.BuildRoot, "cmake", Fingerprint(t, environment));
            PackageStore.Guard(t.BuildRoot, cache);
            log.Write("CMake 缓存：" + cache);
            var configure = new List<string> {
                "-S", t.Source, "-B", cache, "-G", t.Generator, "-A", "x64", "-T", t.Toolset,
                "-DMAX_SDK_PATH=" + t.SdkPath, "-DCMAKE_SYSTEM_VERSION=" + t.WindowsSdk,
                "-DWY_VEHICLE_SOURCE_FINGERPRINT=" + sourceFingerprint
            };
            if (!String.IsNullOrWhiteSpace(t.VisualStudioPath))
                configure.Add("-DCMAKE_GENERATOR_INSTANCE=" + t.VisualStudioPath);
            if (!String.IsNullOrWhiteSpace(t.WindowsSdkPath))
            {
                string sdk = t.WindowsSdkPath.Replace('\\', '/').TrimEnd('/') + "/";
                // CMAKE_VS_GLOBALS is a CMake list; semicolons inside a directory
                // name are data, while the separator between properties is not.
                sdk = sdk.Replace(";", @"\;");
                configure.Add("-DCMAKE_VS_GLOBALS=WindowsSdkDir_10=" + sdk + ";UniversalCRTSdkDir_10=" + sdk);
            }
            Run(t.ToolPath, configure, t.Source, environment, log);
            Run(t.ToolPath, new[] { "--build", cache, "--config", t.Configuration, "--parallel" }, t.Source, environment, log);
            Run(t.ToolPath, new[] { "--install", cache, "--config", t.Configuration, "--prefix", t.Stage }, t.Source, environment, log);
        }

        private static string Fingerprint(BuildTarget t, IDictionary<string, string> environment)
        {
            // A generator instance cannot safely change in an existing CMake
            // build tree. Reuse only an identically selected toolchain's cache.
            var identity = new StringBuilder();
            foreach (string value in new[] {
                "WyVehicleAssetBridge.CMakeCache.2", t.Source, t.ToolPath, t.Generator, t.Toolset, "x64", t.SdkPath,
                t.VisualStudioPath, t.WindowsSdkPath, t.WindowsSdk
            }) AppendIdentity(identity, value);
            foreach (string name in new[] {
                "CMAKE_GENERATOR_INSTANCE", "VS170COMNTOOLS", "CMAKE_WINDOWS_KITS_10_DIR",
                "WindowsSdkDir_10", "UniversalCRTSdkDir_10", "WindowsSDKVersion", "UCRTVersion"
            })
            {
                string value;
                if (!environment.TryGetValue(name, out value)) value = Environment.GetEnvironmentVariable(name);
                AppendIdentity(identity, name);
                AppendIdentity(identity, value);
            }
            using (var hash = SHA256.Create())
                return BitConverter.ToString(hash.ComputeHash(Encoding.UTF8.GetBytes(identity.ToString())))
                    .Replace("-", "").Substring(0, 16).ToLowerInvariant();
        }

        private static void AppendIdentity(StringBuilder identity, string value)
        {
            value = value ?? "";
            identity.Append(value.Length.ToString(CultureInfo.InvariantCulture)).Append(':').Append(value);
        }

        private static void CheckUnrealBatchCompatibility(string executable, IEnumerable<string> arguments)
        {
            string path = Path.GetFullPath(executable).Replace('/', '\\');
            if (!path.EndsWith(@"\Engine\Build\BatchFiles\RunUAT.bat", StringComparison.OrdinalIgnoreCase)) return;
            // RunUAT's own argument loop removes quotes in an unquoted SET;
            // its CALL statements also re-expand percent signs in the engine path.
            // Quoting at our process boundary cannot repair that upstream behavior.
            if (path.IndexOfAny(new[] { '&', '|', '<', '>', '^', '%' }) >= 0)
                throw new ArgumentException("UE 的 RunUAT.bat 无法可靠处理该引擎路径中的 &、|、<、>、^ 或 % 字符。请使用不含这些字符的引擎目录：" + executable);
            foreach (string argument in arguments)
                if (argument != null && argument.IndexOfAny(new[] { '&', '|', '<', '>', '^' }) >= 0)
                    throw new ArgumentException("UE 的 RunUAT.bat 会移除参数引号，不能安全处理插件或产物路径中的 &、|、<、> 或 ^ 字符：" + argument);
        }

        private static void Run(string executable, IEnumerable<string> arguments, string directory,
            IDictionary<string, string> environment, BuildLog log)
        {
            executable = Path.GetFullPath(executable);
            if (!File.Exists(executable)) throw new FileNotFoundException("找不到构建工具。", executable);
            var args = new List<string>(arguments);
            var start = new ProcessStartInfo {
                UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8,
                WorkingDirectory = Path.GetFullPath(directory)
            };
            if (environment != null)
                foreach (var pair in environment) start.EnvironmentVariables[pair.Key] = pair.Value;
            // cl/link may otherwise emit the local ANSI code page into a UTF-8
            // UAT/MSBuild stream. Preserve an explicitly chosen compiler language.
            if (String.IsNullOrWhiteSpace(start.EnvironmentVariables["VSLANG"]))
                start.EnvironmentVariables["VSLANG"] = "1033";

            string extension = Path.GetExtension(executable);
            if (String.Equals(extension, ".bat", StringComparison.OrdinalIgnoreCase) ||
                String.Equals(extension, ".cmd", StringComparison.OrdinalIgnoreCase))
            {
                // cmd receives a fixed expression containing only generated
                // environment names. Values containing %, !, &, ^ or parentheses
                // are substituted once, inside quotes; there is no CALL reparse.
                string prefix = "WYBRIDGE_" + Guid.NewGuid().ToString("N") + "_";
                start.FileName = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "cmd.exe");
                start.EnvironmentVariables[prefix + "TOOL"] = QuoteBatchValue(executable);
                var command = new StringBuilder("%" + prefix + "TOOL%");
                for (int i = 0; i < args.Count; i++)
                {
                    string name = prefix + i.ToString(CultureInfo.InvariantCulture);
                    start.EnvironmentVariables[name] = QuoteBatchValue(args[i]);
                    command.Append(" %").Append(name).Append('%');
                }
                start.Arguments = "/d /v:off /s /c \"" + command + "\"";
            }
            else
            {
                start.FileName = executable;
                start.Arguments = JoinArguments(args);
            }

            log.Write("> " + QuoteArgument(executable) + " " + JoinArguments(args));
            using (var process = new Process { StartInfo = start })
            {
                process.OutputDataReceived += delegate(object sender, DataReceivedEventArgs e) { if (e.Data != null) log.Write(e.Data); };
                process.ErrorDataReceived += delegate(object sender, DataReceivedEventArgs e) { if (e.Data != null) log.Write(e.Data); };
                if (!process.Start()) throw new InvalidOperationException("无法启动构建工具：" + executable);
                process.BeginOutputReadLine();
                process.BeginErrorReadLine();
                // The parameterless overload also drains asynchronous output events.
                process.WaitForExit();
                log.EnsureHealthy();
                if (process.ExitCode != 0)
                    throw new InvalidOperationException("构建工具退出码 " + process.ExitCode + "：" + executable + "。日志：" + log.Path);
            }
        }

        private static string QuoteBatchValue(string value)
        {
            // A quote is not legal in a Windows path. Unlike native argv quoting,
            // cmd does not interpret backslash-escaped quotes; reject one rather
            // than allowing it to turn subsequent data into shell syntax.
            if (value != null && value.IndexOf('"') >= 0)
                throw new ArgumentException("批处理参数不能包含双引号：" + value);
            return QuoteArgument(value);
        }

        private static string JoinArguments(IEnumerable<string> arguments)
        {
            var quoted = new List<string>();
            foreach (string argument in arguments) quoted.Add(QuoteArgument(argument));
            return String.Join(" ", quoted);
        }

        private static string QuoteArgument(string value)
        {
            value = value ?? "";
            if (value.IndexOfAny(new[] { '\0', '\r', '\n' }) >= 0)
                throw new ArgumentException("构建参数包含无效控制字符。");
            // Windows CRT/CommandLineToArgvW quoting: backslashes are doubled
            // before a quote and at the end of a quoted argument, not elsewhere.
            var result = new StringBuilder("\"");
            int slashes = 0;
            foreach (char ch in value)
            {
                if (ch == '\\') { slashes++; continue; }
                if (ch == '"') result.Append('\\', slashes * 2 + 1).Append('"');
                else result.Append('\\', slashes).Append(ch);
                slashes = 0;
            }
            result.Append('\\', slashes * 2).Append('"');
            return result.ToString();
        }

        private sealed class BuildLog : IDisposable
        {
            internal readonly string Path;
            private readonly string target;
            private readonly Action<string> report;
            private readonly StreamWriter writer;
            private readonly object gate = new object();
            private Exception failure;
            private bool reportDisabled;

            internal BuildLog(string target, string path, Action<string> report)
            {
                this.target = target;
                this.report = report;
                Path = path;
                writer = new StreamWriter(new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.Read), new UTF8Encoding(false));
                writer.AutoFlush = true;
            }

            internal void Write(string line)
            {
                lock (gate)
                {
                    try { writer.WriteLine(line); }
                    catch (Exception ex) { if (failure == null) failure = ex; }
                }
                if (report == null) return;
                // Two targets and both redirected streams can report concurrently.
                // UI callbacks must marshal to their own UI thread; serialization
                // here protects non-UI subscribers and prevents overlapping calls.
                lock (ReportGate)
                {
                    if (reportDisabled) return;
                    try { report("[" + target + "] " + line); }
                    catch (Exception ex)
                    {
                        reportDisabled = true;
                        lock (gate)
                        {
                            try { writer.WriteLine("进度回调已停止：" + ex.Message); }
                            catch (Exception io) { if (failure == null) failure = io; }
                        }
                    }
                }
            }

            internal void EnsureHealthy()
            {
                lock (gate)
                    if (failure != null) throw new IOException("无法写入构建日志：" + Path, failure);
            }

            public void Dispose() { writer.Dispose(); }
        }
    }
}
