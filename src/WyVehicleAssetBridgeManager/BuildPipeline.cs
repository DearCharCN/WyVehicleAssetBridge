using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Management;
using System.Threading.Tasks;

namespace WyVehicleAssetBridge.Setup
{
    internal sealed class BuildTarget
    {
        internal string Name, Source, BuildRoot, Stage, LogPath, ToolPath, DeployRoot, Destination;
        internal string ProjectFile, MaxInstall, SdkPath, Generator, Toolset, WindowsSdk, Configuration;
        internal string VisualStudioPath = "", WindowsSdkPath = "";
    }

    internal sealed class PluginVersionInfo
    {
        internal string Destination = "", Installed = "", Current = "", Description = "";
        internal bool Mismatch, Failed;
    }

    internal sealed class BuildPipeline
    {
        private readonly string root, configFile, configFolder;
        private readonly Dictionary<string, string> settings;
        private readonly Action<string> report;
        private readonly string runId = DateTime.UtcNow.ToString("yyyyMMdd-HHmmss") + "-" + Guid.NewGuid().ToString("N").Substring(0, 8);

        internal BuildPipeline(string root, string config, Action<string> report)
        {
            this.root = Path.GetFullPath(root);
            this.report = report;
            configFile = Path.GetFullPath(String.IsNullOrEmpty(config) ? LocalConfig.FileName(root) : config);
            configFolder = Path.GetDirectoryName(configFile);
            if (!File.Exists(configFile)) throw new FileNotFoundException("找不到配置文件 " + configFile + "。请在 WyVehicleAssetBridgeManager.exe 的「环境配置」中保存本机路径。");
            settings = LocalConfig.ReadSettings(new List<string>(File.ReadAllLines(configFile)));
        }

        private string Read(string key, bool required)
        {
            string value;
            if (settings.TryGetValue(key, out value) && !String.IsNullOrWhiteSpace(value)) return value;
            if (required) throw new InvalidDataException("INI 缺少配置项 " + key);
            return "";
        }

        private string ReadPath(string key, bool required)
        {
            string value = Read(key, required);
            return value.Length == 0 ? "" : Path.GetFullPath(Path.Combine(configFolder, Environment.ExpandEnvironmentVariables(value)));
        }

        private static string RequireFile(string folder, string name)
        {
            string path = Path.Combine(folder, name);
            if (!File.Exists(path)) throw new FileNotFoundException("缺少文件 " + path);
            return path;
        }

        private string CMake()
        {
            string value = Environment.ExpandEnvironmentVariables(Read("Build.CMakePath", true));
            if (Path.GetFileName(value) != value) return RequireFile(configFolder, value);
            var extensions = Path.HasExtension(value) ? new[] { "" } : new[] { ".exe", ".cmd", ".bat" };
            foreach (string directory in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(';'))
                foreach (string ext in extensions)
                {
                    var path = Path.Combine(directory.Trim('"'), value + ext);
                    if (File.Exists(path)) return Path.GetFullPath(path);
                }
            throw new FileNotFoundException("PATH 中找不到 " + value + "，请在环境配置中选择 CMake。");
        }

        private BuildTarget Locate(string name)
        {
            string folder = Path.Combine(root, name == "UE" ? "ue_plugins" : "3ds_plugins");
            var t = new BuildTarget { Name = name, Source = Path.Combine(folder, "WyVehicleAssetBridge"), BuildRoot = Path.Combine(folder, "build") };
            if (name == "UE")
            {
                t.ProjectFile = LocalConfig.ProjectFile(ReadPath("UE.ProjectPath", true));
                t.DeployRoot = PackageStore.ResolveDirectory(Path.Combine(Path.GetDirectoryName(t.ProjectFile), "Plugins"));
            }
            else
            {
                t.MaxInstall = ReadPath("Max.InstallPath", true);
                RequireFile(t.MaxInstall, "3dsmax.exe");
                t.DeployRoot = PackageStore.ResolveDirectory(Path.Combine(t.MaxInstall, "ApplicationPlugins"));
            }
            t.Destination = Path.Combine(t.DeployRoot, "WyVehicleAssetBridge");
            return t;
        }

        internal static PluginVersionInfo InspectVersion(string root, string name)
        {
            try { return new BuildPipeline(root, null, null).Inspect(name); }
            catch (Exception ex) { return new PluginVersionInfo { Failed = true, Description = ex.Message }; }
        }

        private PluginVersionInfo Inspect(string name)
        {
            var version = new PluginVersionInfo();
            try
            {
                if (Read(name == "UE" ? "UE.ProjectPath" : "Max.InstallPath", false).Length == 0)
                {
                    version.Description = "尚未配置目标目录";
                    return version;
                }
                var t = Locate(name);
                version.Destination = t.Destination;
                PackageStore.Guard(t.DeployRoot, t.Destination);
                if (!Directory.Exists(t.Destination) && !File.Exists(t.Destination))
                {
                    version.Description = "未安装";
                    return version;
                }
                version.Installed = PackageStore.ReadInstalledFingerprint(t.Destination, name);
                if (version.Installed.Length == 0)
                {
                    version.Description = "未记录源码指纹，请先发布一次";
                    return version;
                }
                version.Current = SourceFingerprint.Compute(t.Source);
                version.Mismatch = !String.Equals(version.Current, version.Installed, StringComparison.Ordinal);
                version.Description = version.Mismatch ? "源码与插件版本不一致" : "源码与已安装版本一致";
            }
            catch (Exception ex) { version.Failed = true; version.Description = "无法核对版本：" + ex.Message; }
            return version;
        }

        private BuildTarget Prepare(string name)
        {
            var t = Locate(name);
            PackageStore.Guard(root, t.BuildRoot);
            t.Stage = Path.Combine(t.BuildRoot, "staging", runId, "WyVehicleAssetBridge");
            t.LogPath = Path.Combine(t.BuildRoot, "logs", runId + ".log");
            if (name == "UE")
            {
                if (LocalConfig.PluginDisabled(LocalConfig.Json(t.ProjectFile))) throw new InvalidOperationException("该 UE 工程已禁用 WyVehicleAssetBridge。");
                string engine = ReadPath("UE.EnginePath", true);
                t.ToolPath = RequireFile(engine, @"Engine\Build\BatchFiles\RunUAT.bat");
                if (Convert.ToInt32(LocalConfig.Json(RequireFile(engine, @"Engine\Build\Build.version"))["MajorVersion"]) != 5)
                    throw new InvalidDataException("构建需要 Unreal Engine 5。");
            }
            else
            {
                var version = FileVersionInfo.GetVersionInfo(Path.Combine(t.MaxInstall, "3dsmax.exe"));
                if (!String.IsNullOrEmpty(version.ProductVersion) && version.ProductMajorPart != 27)
                    throw new InvalidOperationException("原生插件需要 3ds Max 2025。");
                t.SdkPath = ReadPath("Max.SdkPath", true);
                foreach (var relative in new[] { @"include\max.h", @"lib\x64\Release\core.lib", @"lib\x64\Release\maxutil.lib", @"lib\x64\Release\gup.lib" })
                    RequireFile(t.SdkPath, relative);
                t.ToolPath = CMake();
                t.Generator = Read("Build.CMakeGenerator", true);
                t.Toolset = Read("Build.CMakeToolset", true);
                t.WindowsSdk = Read("Build.WindowsSdkVersion", true);
                t.Configuration = Read("Build.MaxConfiguration", true);
                if (!new[] { "Release", "RelWithDebInfo" }.Contains(t.Configuration, StringComparer.OrdinalIgnoreCase))
                    throw new InvalidDataException("MaxConfiguration 需要是 Release 或 RelWithDebInfo。");
                t.VisualStudioPath = ReadPath("Build.VisualStudioPath", false);
                if (t.VisualStudioPath.Length != 0)
                {
                    RequireFile(t.VisualStudioPath, @"VC\Auxiliary\Build\vcvarsall.bat");
                    RequireFile(t.VisualStudioPath, @"MSBuild\Current\Bin\MSBuild.exe");
                }
                t.WindowsSdkPath = ReadPath("Build.WindowsSdkPath", false);
                if (t.WindowsSdkPath.Length != 0)
                    foreach (string relative in new[]{ @"Include\{0}\um\Windows.h", @"Include\{0}\ucrt\stdio.h", @"Lib\{0}\um\x64\kernel32.lib", @"Lib\{0}\ucrt\x64\ucrt.lib", @"bin\{0}\x64\rc.exe" })
                        RequireFile(t.WindowsSdkPath, String.Format(relative, t.WindowsSdk));
            }
            return t;
        }

        private static void EnsureApplicationsClosed(IEnumerable<BuildTarget> targets)
        {
            const string query = "SELECT Name,ExecutablePath,CommandLine FROM Win32_Process WHERE Name='UnrealEditor.exe' OR Name='UnrealEditor-Cmd.exe' OR Name='3dsmax.exe' OR Name='3dsmaxbatch.exe'";
            using (var searcher = new ManagementObjectSearcher(query))
            using (var processes = searcher.Get())
            {
                foreach (ManagementObject process in processes)
                using (process)
                {
                    string name = Convert.ToString(process["Name"]);
                    foreach (var t in targets)
                    {
                        if (t.Name == "Max" && name.StartsWith("3dsmax", StringComparison.OrdinalIgnoreCase))
                        {
                            string exe = Convert.ToString(process["ExecutablePath"]);
                            if (exe.Length == 0 || String.Equals(Path.GetDirectoryName(exe), t.MaxInstall.TrimEnd('\\', '/'), StringComparison.OrdinalIgnoreCase))
                                throw new InvalidOperationException("请先关闭目标 3ds Max，再发布 Max 插件。仅编译可使用 -BuildOnly。");
                        }
                        if (t.Name == "UE" && name.StartsWith("UnrealEditor", StringComparison.OrdinalIgnoreCase))
                        {
                            string args = Convert.ToString(process["CommandLine"]).Replace('/', '\\');
                            if (args.Length == 0 || args.IndexOf(t.ProjectFile, StringComparison.OrdinalIgnoreCase) >= 0 ||
                                args.IndexOf(Path.GetDirectoryName(t.ProjectFile) + "\\", StringComparison.OrdinalIgnoreCase) >= 0)
                                throw new InvalidOperationException("请先关闭目标 UE 工程的编辑器，再发布 UE 插件。仅编译可使用 -BuildOnly。");
                        }
                    }
                }
            }
        }

        private void Compile(BuildTarget t)
        {
            try
            {
                PackageStore.Guard(t.BuildRoot, t.Stage);
                PackageStore.Guard(t.BuildRoot, t.LogPath);
                string fingerprint = SourceFingerprint.Compute(t.Source);
                report(t.Name + " 开始编译，日志 " + t.LogPath);
                PluginCompiler.Build(t, fingerprint, report);
                PackageStore.Verify(t.Stage, t.Name);
                if (!String.Equals(fingerprint, SourceFingerprint.Compute(t.Source), StringComparison.Ordinal))
                    throw new IOException("编译期间插件源码发生变化，请重新编译；本轮产物不会发布。");
                PackageStore.Mark(t.Stage, t.Name, fingerprint);
                report(t.Name + " 编译通过。");
            }
            catch (Exception ex) { throw new InvalidOperationException(t.Name + " 编译失败：" + ex.Message + "。日志 " + t.LogPath, ex); }
        }

        private List<BuildTarget> SelectTargets(string target, bool buildOnly, bool changedOnly)
        {
            var targets = new List<BuildTarget>();
            foreach (string name in target == "Both" ? new[] { "UE", "Max" } : new[] { target })
            {
                if (changedOnly)
                {
                    var version = Inspect(name);
                    if (version.Failed) throw new InvalidOperationException(name + " " + version.Description);
                    report(name + " " + version.Description);
                    if (!version.Mismatch) continue;
                }
                var t = Prepare(name);
                if (!buildOnly) { PackageStore.Guard(t.DeployRoot, t.Destination); PackageStore.AssertOwned(t.Destination); }
                report(t.Name + " 目标目录 " + t.Destination);
                targets.Add(t);
            }
            if (targets.Count == 0) report("没有需要增量发布的插件。");
            return targets;
        }

        internal void Run(string target, bool buildOnly, bool check, bool changedOnly = false)
        {
            report("配置文件 " + configFile);
            if (check)
            {
                if (SelectTargets(target, buildOnly, changedOnly).Count != 0) report("配置检查通过。");
                return;
            }
            var lockPath = Path.Combine(root, ".build.lock");
            PackageStore.Guard(root, lockPath);
            FileStream handle;
            try { handle = File.Open(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None); }
            catch (IOException ex) { throw new IOException("无法取得构建锁，请检查是否另有构建正在运行。" + ex.Message, ex); }
            using (handle)
            {
                var targets = SelectTargets(target, buildOnly, changedOnly);
                if (targets.Count == 0) return;
                if (!buildOnly) EnsureApplicationsClosed(targets);
                var tasks = targets.Select(t => Task.Run(() => Compile(t))).ToArray();
                try { Task.WaitAll(tasks); }
                catch (AggregateException ex) {
                    throw new InvalidOperationException(String.Join(Environment.NewLine, ex.Flatten().InnerExceptions.Select(e => e.Message)), ex);
                }
                var latest = targets.Select(t => new PackageItem { Name=t.Name, Root=t.BuildRoot, Source=t.Stage,
                    Destination=Path.Combine(t.BuildRoot, "latest", "WyVehicleAssetBridge") }).ToList();
                PackageStore.Install(latest, report);
                report("已更新本地 build/latest。");
                if (!buildOnly)
                {
                    EnsureApplicationsClosed(targets);
                    var installs = targets.Select(t => new PackageItem { Name=t.Name, Root=t.DeployRoot,
                        Source=Path.Combine(t.BuildRoot, "latest", "WyVehicleAssetBridge"), Destination=t.Destination });
                    PackageStore.Install(installs, report);
                    report("发布完成，下次启动目标应用时加载新插件。");
                }
                foreach (var t in targets)
                {
                    try { PackageStore.Remove(t.BuildRoot, Path.GetDirectoryName(t.Stage)); }
                    catch (IOException ex) { report("无法清理临时产物：" + ex.Message); }
                }
            }
        }
    }
}
