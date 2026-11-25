using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Management;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using Microsoft.Win32;

namespace WyVehicleAssetBridge.Setup
{
    internal sealed class EnvironmentCheckResult
    {
        internal readonly Dictionary<string, string> Paths = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        internal readonly Dictionary<string, string> Issues = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
    }

    // Discovery only: never installs software or changes machine settings.
    internal static class Detection
    {
        private const string RequiredWindowsSdk = "10.0.22621.0";
        private static readonly string[] Keys = { "CMake", "MaxSdk", "WindowsSdk", "Toolchain", "Engine", "Max" };

        // Shared by GUI background checks and headless CLI polling. This method
        // discovers/validates only; callers decide whether to save the result.
        internal static EnvironmentCheckResult CheckEnvironment(string root, IDictionary<string, string> configured)
        {
            string repoRoot = FullPath(root, Environment.CurrentDirectory);
            if (repoRoot.Length == 0) repoRoot = Environment.CurrentDirectory;
            var input = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (string key in LocalConfig.Applications) input[key] = Configured(configured, key);
            foreach (string key in LocalConfig.Dependencies) input[key] = Configured(configured, key);

            // The WangYue project must remain an explicit user choice. Normalize
            // that choice relative to the supplied repository, but never discover it.
            string project = FullPath(input["Project"], repoRoot);
            if (project.Length != 0) input["Project"] = project;
            var result = new EnvironmentCheckResult();
            foreach (var pair in input) result.Paths[pair.Key] = pair.Value;
            foreach (var pair in Scan(repoRoot, input)) result.Paths[pair.Key] = pair.Value;

            foreach (string key in LocalConfig.Applications)
                result.Issues[key] = LocalConfig.ValidateApplication(key, result.Paths[key]);
            if (result.Issues["Project"].Length == 0 && result.Issues["Engine"].Length == 0)
                result.Issues["Engine"] = LocalConfig.EngineMatch(result.Paths["Project"], result.Paths["Engine"]);
            foreach (string key in LocalConfig.Dependencies)
                result.Issues[key] = result.Paths[key].Length == 0 ? "可选择安装目录或使用自动安装" : Validate(key, result.Paths[key]);
            return result;
        }

        internal static Dictionary<string, string> Scan(string repoRoot, IDictionary<string, string> configured)
        {
            string root = FullPath(repoRoot, Environment.CurrentDirectory);
            if (root.Length == 0) root = Environment.CurrentDirectory;
            var candidates = new Dictionary<string, List<string>>(StringComparer.OrdinalIgnoreCase);
            var found = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            var missing = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (string key in Keys)
            {
                candidates[key] = new List<string>();
                string value = Configured(configured, key);
                found[key] = "";
                if (!String.IsNullOrWhiteSpace(value))
                {
                    // An explicit value is authoritative even when it is invalid.
                    // Only normalization (or resolving that exact command) is allowed.
                    string path = ConfiguredPath(key, value, root);
                    found[key] = path.Length != 0 && ValidateCandidate(key, path).Length == 0 ? path : value;
                }
                else missing.Add(key);
            }
            // Do not query registry, vswhere or running applications unnecessarily.
            if (missing.Count == 0) return found;

            var programRoots = new List<string>();
            Add(programRoots, Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles));
            Add(programRoots, Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86));
            Add(programRoots, Environment.GetEnvironmentVariable("ProgramW6432"));

            if (missing.Contains("Toolchain"))
            {
                AddEnvironment(candidates["Toolchain"], "VSINSTALLDIR");
                var vswherePaths = new List<string>();
                foreach (string programRoot in programRoots)
                    Add(vswherePaths, Path.Combine(programRoot, @"Microsoft Visual Studio\Installer\vswhere.exe"));
                AddCommand(vswherePaths, "vswhere.exe");
                foreach (string vswhere in vswherePaths)
                {
                    if (!File.Exists(vswhere)) continue;
                    string output;
                    if (!Run(vswhere, "-products * -version [17.0,18.0) -prerelease -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath", out output)) continue;
                    foreach (string line in output.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries))
                        Add(candidates["Toolchain"], FullPath(line, root));
                }
            }
            else Add(candidates["Toolchain"], FullPath(found["Toolchain"], root));

            foreach (RegistryView view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
            {
                foreach (RegistryHive hive in new[] { RegistryHive.LocalMachine, RegistryHive.CurrentUser })
                {
                    if (missing.Contains("Toolchain")) Add(candidates["Toolchain"], RegistryString(hive, view, @"SOFTWARE\Microsoft\VisualStudio\SxS\VS7", "17.0"));
                    if (missing.Contains("CMake")) Add(candidates["CMake"], RegistryString(hive, view, @"SOFTWARE\Kitware\CMake", "InstallDir"));
                    if (missing.Contains("WindowsSdk")) Add(candidates["WindowsSdk"], RegistryString(hive, view, @"SOFTWARE\Microsoft\Windows Kits\Installed Roots", "KitsRoot10"));
                    if (missing.Contains("MaxSdk"))
                    {
                        Add(candidates["MaxSdk"], RegistryString(hive, view, @"SOFTWARE\Autodesk\3dsMax\27.0\SDK", "InstallDir"));
                        AddMaxSdkUninstallEntries(candidates["MaxSdk"], hive, view);
                    }
                }
            }

            if (missing.Contains("Toolchain"))
                foreach (string programRoot in programRoots)
                    foreach (string instance in Subdirectories(Path.Combine(programRoot, @"Microsoft Visual Studio\2022")))
                        Add(candidates["Toolchain"], instance);

            if (missing.Contains("CMake"))
            {
                AddEnvironment(candidates["CMake"], "CMAKE_EXE");
                AddEnvironment(candidates["CMake"], "CMAKE_PATH");
                AddCommand(candidates["CMake"], "cmake.exe");
            }
            if (missing.Contains("MaxSdk")) AddEnvironment(candidates["MaxSdk"], "ADSK_3DSMAX_SDK_2025");
            if (missing.Contains("WindowsSdk")) AddEnvironment(candidates["WindowsSdk"], "WindowsSdkDir");
            foreach (string programRoot in programRoots)
            {
                if (missing.Contains("CMake")) Add(candidates["CMake"], Path.Combine(programRoot, @"CMake\bin\cmake.exe"));
                if (missing.Contains("MaxSdk"))
                {
                    Add(candidates["MaxSdk"], Path.Combine(programRoot, @"Autodesk\3ds Max 2025 SDK\maxsdk"));
                    Add(candidates["MaxSdk"], Path.Combine(programRoot, @"Autodesk\3ds Max 2025\maxsdk"));
                }
                if (missing.Contains("WindowsSdk")) Add(candidates["WindowsSdk"], Path.Combine(programRoot, @"Windows Kits\10"));
            }
            if (missing.Contains("CMake"))
            {
                Add(candidates["CMake"], Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), @"Programs\CMake\bin\cmake.exe"));
                foreach (string instance in candidates["Toolchain"])
                    Add(candidates["CMake"], Path.Combine(instance, @"Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"));
            }

            // Compatibility with this repository's previously extracted SDK.
            // System installs and explicit configuration remain discoverable independently.
            if (missing.Contains("MaxSdk"))
            {
                string cache = Path.Combine(root, @"3ds_plugins\build\dependencies\max2025-sdk");
                Add(candidates["MaxSdk"], Path.Combine(cache, @"Program Files\Autodesk\3ds Max 2025 SDK\maxsdk"));
                Add(candidates["MaxSdk"], Path.Combine(cache, "maxsdk"));
                Add(candidates["MaxSdk"], cache);
            }

            if (missing.Contains("Engine")) FindEngines(candidates["Engine"], Configured(configured, "Project"), root, programRoots);
            if (missing.Contains("Max")) FindMax(candidates["Max"], programRoots);

            foreach (string key in Keys)
            {
                if (!missing.Contains(key)) continue;
                foreach (string candidate in candidates[key])
                {
                    foreach (string path in Variants(key, candidate, root))
                    {
                        if (ValidateCandidate(key, path).Length != 0) continue;
                        found[key] = path;
                        break;
                    }
                    if (found[key].Length != 0) break;
                }
            }
            return found;
        }

        private static string ConfiguredPath(string key, string value, string root)
        {
            if (key != "CMake" || !IsCommandName(value)) return FullPath(value, root);
            var paths = new List<string>();
            AddCommand(paths, value);
            // Match command lookup order. A broken first match must not silently
            // select another installation farther down PATH.
            foreach (string path in paths)
                if (File.Exists(path)) return FullPath(path, root);
            return "";
        }

        private static string ValidateCandidate(string key, string path)
        {
            if (key == "Engine" || key == "Max") return LocalConfig.ValidateApplication(key, path);
            return Validate(key, path);
        }

        private static void FindMax(List<string> candidates, IList<string> programRoots)
        {
            foreach (RegistryView view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
                foreach (RegistryHive hive in new[] { RegistryHive.LocalMachine, RegistryHive.CurrentUser })
                {
                    Add(candidates, RegistryString(hive, view, @"SOFTWARE\Autodesk\3dsMax\27.0", "Installdir"));
                    Add(candidates, RegistryString(hive, view, @"SOFTWARE\Autodesk\3dsMax\27.0", "Location"));
                }
            AddEnvironment(candidates, "ADSK_3DSMAX_x64_2025");
            foreach (string executable in RunningExecutables("3dsmax")) Add(candidates, Path.GetDirectoryName(executable));
            foreach (string programRoot in programRoots) Add(candidates, Path.Combine(programRoot, @"Autodesk\3ds Max 2025"));
            foreach (string drive in FixedDrives()) Add(candidates, Path.Combine(drive, @"Program Files\Autodesk\3ds Max 2025"));
        }

        private static void FindEngines(List<string> candidates, string project, string root, IList<string> programRoots)
        {
            string projectFile = "", association = "";
            try
            {
                projectFile = LocalConfig.ProjectFile(FullPath(project, root));
                object value;
                if (ReadJson(projectFile).TryGetValue("EngineAssociation", out value)) association = Convert.ToString(value);
            }
            catch (Exception) { }

            var matchingRegistration = new List<string>();
            var installed = new List<string>();
            var running = new List<string>();
            foreach (var process in RunningEditors(projectFile.Length != 0))
            {
                string engine = EngineFromExecutable(process.Item1);
                if (engine.Length == 0) continue;
                if (projectFile.Length != 0 && CommandUsesProject(process.Item2, projectFile)) Add(candidates, engine);
                Add(running, engine);
            }
            foreach (RegistryView view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
                foreach (RegistryHive hive in new[] { RegistryHive.CurrentUser, RegistryHive.LocalMachine })
                    FindRegisteredEngines(installed, matchingRegistration, hive, view, association);

            try
            {
                string launcher = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), @"Epic\UnrealEngineLauncher\LauncherInstalled.dat");
                object entries;
                if (File.Exists(launcher) && ReadJson(launcher).TryGetValue("InstallationList", out entries) && entries is object[])
                {
                    foreach (object item in (object[])entries)
                    {
                        var entry = item as Dictionary<string, object>;
                        object location, name;
                        if (entry == null || !entry.TryGetValue("InstallLocation", out location)) continue;
                        Add(installed, Convert.ToString(location));
                        if (association.Length != 0 && entry.TryGetValue("AppName", out name) &&
                            String.Equals(Convert.ToString(name), "UE_" + association, StringComparison.OrdinalIgnoreCase))
                            Add(matchingRegistration, Convert.ToString(location));
                    }
                }
            }
            catch (Exception) { }

            foreach (string programRoot in programRoots)
                foreach (string folder in Subdirectories(Path.Combine(programRoot, "Epic Games"))) Add(installed, folder);
            foreach (string drive in FixedDrives())
            {
                Add(installed, Path.Combine(drive, "UE"));
                Add(installed, Path.Combine(drive, "UnrealEngine"));
                foreach (string folder in Subdirectories(drive))
                    if (Regex.IsMatch(Path.GetFileName(folder), @"^(UE[_-]?5|UnrealEngine[_-]?5)", RegexOptions.IgnoreCase)) Add(installed, folder);
                foreach (string container in new[] { "Epic Games", @"Program Files\Epic Games" })
                    foreach (string folder in Subdirectories(Path.Combine(drive, container))) Add(installed, folder);
            }

            foreach (string path in matchingRegistration) Add(candidates, path);
            // Numeric associations can also match source builds absent from the launcher.
            if (Regex.IsMatch(association, @"^5\.\d+(?:\.\d+)?$"))
            {
                foreach (string path in running)
                    if (LocalConfig.EngineMatch(projectFile, path).Length == 0) Add(candidates, path);
                foreach (string path in installed)
                    if (LocalConfig.EngineMatch(projectFile, path).Length == 0) Add(candidates, path);
            }
            foreach (string path in running) Add(candidates, path);
            foreach (string path in installed) Add(candidates, path);
        }

        private static void FindRegisteredEngines(List<string> installed, List<string> matching,
            RegistryHive hive, RegistryView view, string association)
        {
            try
            {
                using (RegistryKey root = RegistryKey.OpenBaseKey(hive, view))
                {
                    using (RegistryKey builds = root.OpenSubKey(@"SOFTWARE\Epic Games\Unreal Engine\Builds"))
                    {
                        if (builds != null)
                            foreach (string name in builds.GetValueNames())
                            {
                                string path = Convert.ToString(builds.GetValue(name, ""));
                                Add(installed, path);
                                if (AssociationEquals(name, association)) Add(matching, path);
                            }
                    }
                    using (RegistryKey engines = root.OpenSubKey(@"SOFTWARE\EpicGames\Unreal Engine"))
                    {
                        if (engines != null)
                            foreach (string name in engines.GetSubKeyNames())
                                using (RegistryKey engine = engines.OpenSubKey(name))
                                {
                                    if (engine == null) continue;
                                    string path = Convert.ToString(engine.GetValue("InstalledDirectory", ""));
                                    Add(installed, path);
                                    if (AssociationEquals(name, association)) Add(matching, path);
                                }
                    }
                }
            }
            catch (Exception) { }
        }

        private static bool AssociationEquals(string left, string right)
        {
            return !String.IsNullOrWhiteSpace(right) &&
                String.Equals(left.Trim().Trim('{', '}'), right.Trim().Trim('{', '}'), StringComparison.OrdinalIgnoreCase);
        }

        private static Dictionary<string, object> ReadJson(string path)
        {
            return (Dictionary<string, object>)new JavaScriptSerializer().DeserializeObject(File.ReadAllText(path));
        }

        private static List<Tuple<string, string>> RunningEditors(bool includeCommandLine)
        {
            var result = new List<Tuple<string, string>>();
            if (includeCommandLine)
            {
                try
                {
                    using (var searcher = new ManagementObjectSearcher("SELECT ExecutablePath, CommandLine FROM Win32_Process WHERE Name='UnrealEditor.exe'"))
                    {
                        searcher.Options.Timeout = TimeSpan.FromSeconds(3);
                        using (ManagementObjectCollection processes = searcher.Get())
                        foreach (ManagementObject process in processes)
                        {
                            using (process)
                            {
                                string executable = Convert.ToString(process["ExecutablePath"]);
                                string commandLine = Convert.ToString(process["CommandLine"]);
                                if (executable.Length != 0) result.Add(Tuple.Create(executable, commandLine));
                            }
                        }
                    }
                }
                catch (Exception) { }
            }
            if (result.Count == 0)
                foreach (string path in RunningExecutables("UnrealEditor")) result.Add(Tuple.Create(path, ""));
            return result;
        }

        private static List<string> RunningExecutables(string name)
        {
            var result = new List<string>();
            try
            {
                foreach (Process process in Process.GetProcessesByName(name))
                    using (process)
                    {
                        try { Add(result, process.MainModule.FileName); }
                        catch (Exception) { }
                    }
            }
            catch (Exception) { }
            return result;
        }

        private static string EngineFromExecutable(string path)
        {
            const string suffix = @"\Engine\Binaries\Win64\UnrealEditor.exe";
            path = path.Replace('/', '\\');
            return path.EndsWith(suffix, StringComparison.OrdinalIgnoreCase) ? path.Substring(0, path.Length - suffix.Length) : "";
        }

        private static bool CommandUsesProject(string command, string project)
        {
            foreach (Match match in Regex.Matches(command ?? "", "\\\"([^\\\"]+\\.uproject)\\\"|([^\\s\\\"]+\\.uproject)", RegexOptions.IgnoreCase))
            {
                string candidate = match.Groups[1].Success ? match.Groups[1].Value : match.Groups[2].Value;
                if (candidate.StartsWith("-project=", StringComparison.OrdinalIgnoreCase)) candidate = candidate.Substring(9);
                if (String.Equals(FullPath(candidate, Environment.CurrentDirectory), project, StringComparison.OrdinalIgnoreCase)) return true;
            }
            return false;
        }

        private static List<string> FixedDrives()
        {
            var result = new List<string>();
            try
            {
                foreach (DriveInfo drive in DriveInfo.GetDrives())
                    if (drive.DriveType == DriveType.Fixed && drive.IsReady) Add(result, drive.RootDirectory.FullName);
            }
            catch (Exception) { }
            return result;
        }

        internal static string Validate(string key, string path)
        {
            if (String.IsNullOrWhiteSpace(path)) return "未设置路径。";
            string full = FullPath(path, Environment.CurrentDirectory);
            if (full.Length == 0) return "路径格式无效。";
            try
            {
                if (String.Equals(key, "CMake", StringComparison.OrdinalIgnoreCase))
                    return ValidateCMake(full);
                if (String.Equals(key, "MaxSdk", StringComparison.OrdinalIgnoreCase))
                    return ValidateMaxSdk(full);
                if (String.Equals(key, "WindowsSdk", StringComparison.OrdinalIgnoreCase))
                    return ValidateWindowsSdk(full);
                if (String.Equals(key, "Toolchain", StringComparison.OrdinalIgnoreCase))
                    return ValidateToolchain(full);
                return "未知的环境项目：" + key;
            }
            catch (Exception ex)
            {
                return "无法检查该路径：" + ex.Message;
            }
        }

        private static string ValidateCMake(string path)
        {
            if (!File.Exists(path) || !String.Equals(Path.GetFileName(path), "cmake.exe", StringComparison.OrdinalIgnoreCase))
                return "请选择实际的 cmake.exe 文件。";
            string output;
            if (!Run(path, "--version", out output)) return "无法运行 CMake，或版本查询超时。";
            // Accept stable versions and prereleases, e.g. 4.3.0-rc1.
            Match version = Regex.Match(output, @"(?im)^\s*cmake version\s+(\d+)\.(\d+)(?:\.\d+)?(?:[-+][^\s]+)?\s*$");
            int major, minor;
            if (!version.Success || !Int32.TryParse(version.Groups[1].Value, out major) || !Int32.TryParse(version.Groups[2].Value, out minor))
                return "无法识别 CMake 的版本输出。";
            if (major < 3 || (major == 3 && minor < 21)) return "CMake 版本过低，需要 3.21 或更新版本。";
            if (!Run(path, "--help", out output)) return "无法读取 CMake 支持的生成器。";
            if (output.IndexOf("Visual Studio 17 2022", StringComparison.OrdinalIgnoreCase) < 0)
                return "该 CMake 不支持 Visual Studio 17 2022 生成器。";
            return "";
        }

        private static string ValidateMaxSdk(string path)
        {
            string missing = FirstMissing(path, new[] {
                @"include\max.h", @"include\maxversion.h", @"include\plugapi.h", @"include\gup.h",
                @"lib\x64\Release\core.lib", @"lib\x64\Release\maxutil.lib", @"lib\x64\Release\gup.lib"
            });
            if (missing.Length != 0) return "3ds Max SDK 不完整，请选择包含 include 和 lib 的 maxsdk 目录；缺少：" + missing;
            string header = StripComments(File.ReadAllText(Path.Combine(path, @"include\maxversion.h")));
            string api = StripComments(File.ReadAllText(Path.Combine(path, @"include\plugapi.h")));
            if (!Regex.IsMatch(header, @"(?m)^\s*#\s*define\s+MAX_VERSION_MAJOR\s+27\b") ||
                !Regex.IsMatch(header, @"(?m)^\s*#\s*define\s+MAX_PRODUCT_YEAR_NUMBER\s+2025\b") ||
                !Regex.IsMatch(api, @"(?m)^\s*#\s*define\s+MAX_RELEASE\s+(?:MAX_RELEASE_R27|27000)\s*$"))
                return "SDK 版本不匹配，需要 3ds Max 2025（27.x）的 C++ SDK。";
            return "";
        }

        private static string ValidateWindowsSdk(string path)
        {
            string missing = FirstMissing(path, new[] {
                @"Include\" + RequiredWindowsSdk + @"\um\Windows.h",
                @"Include\" + RequiredWindowsSdk + @"\shared\sdkddkver.h",
                @"Include\" + RequiredWindowsSdk + @"\ucrt\stdio.h",
                @"Lib\" + RequiredWindowsSdk + @"\um\x64\kernel32.lib",
                @"Lib\" + RequiredWindowsSdk + @"\ucrt\x64\ucrt.lib",
                @"bin\" + RequiredWindowsSdk + @"\x64\rc.exe"
            });
            return missing.Length == 0 ? "" : "需要完整的 Windows SDK " + RequiredWindowsSdk + "，请选择 Windows Kits\\10 根目录；缺少：" + missing;
        }

        private static string ValidateToolchain(string path)
        {
            string msbuild = Path.Combine(path, @"MSBuild\Current\Bin\MSBuild.exe");
            if (!File.Exists(msbuild)) return "该目录不是完整的 Visual Studio 2022 实例，缺少 MSBuild。";
            if (FileVersionInfo.GetVersionInfo(msbuild).FileMajorPart != 17)
                return "需要 Visual Studio 2022（MSBuild 17.x），不支持其他主版本。";
            string toolset = @"MSBuild\Microsoft\VC\v170\Platforms\x64\PlatformToolsets\v143\Toolset.props";
            if (!File.Exists(Path.Combine(path, toolset))) return "Visual Studio 2022 未安装 v143 x64 C++ 工具集。";
            var versions = new List<string>();
            foreach (string file in new[] { "Microsoft.VCToolsVersion.v143.default.txt", "Microsoft.VCToolsVersion.default.txt" })
            {
                string versionFile = Path.Combine(path, @"VC\Auxiliary\Build\" + file);
                if (File.Exists(versionFile)) Add(versions, File.ReadAllText(versionFile).Trim());
            }
            foreach (string folder in Subdirectories(Path.Combine(path, @"VC\Tools\MSVC")))
                Add(versions, Path.GetFileName(folder));
            foreach (string text in versions)
            {
                Version version;
                if (!Version.TryParse(text, out version) || version.Major != 14 || version.Minor < 30 || version.Minor >= 50) continue;
                string compiler = Path.Combine(path, @"VC\Tools\MSVC\" + text);
                if (FirstMissing(compiler, new[] { @"bin\Hostx64\x64\cl.exe", @"bin\Hostx64\x64\link.exe", @"include\vcruntime.h", @"lib\x64\vcruntime.lib" }).Length == 0)
                    return "";
            }
            return "未找到完整的 v143 x64 编译器、链接器及 C++ 头文件/库，请在 VS Installer 中安装 MSVC v143。";
        }

        private static IEnumerable<string> Variants(string key, string candidate, string root)
        {
            string path = FullPath(candidate, root);
            if (path.Length == 0) yield break;
            yield return path;
            if (key == "CMake" && Directory.Exists(path))
            {
                yield return Path.Combine(path, "cmake.exe");
                yield return Path.Combine(path, @"bin\cmake.exe");
            }
            else if (key == "MaxSdk" && Directory.Exists(path))
                yield return Path.Combine(path, "maxsdk");
        }

        private static string Configured(IDictionary<string, string> configured, string key)
        {
            if (configured == null) return "";
            foreach (var pair in configured)
                if (String.Equals(pair.Key, key, StringComparison.OrdinalIgnoreCase)) return pair.Value ?? "";
            return "";
        }

        private static void Add(List<string> list, string value)
        {
            if (String.IsNullOrWhiteSpace(value)) return;
            value = Environment.ExpandEnvironmentVariables(value.Trim().Trim('"'));
            foreach (string existing in list)
                if (String.Equals(existing, value, StringComparison.OrdinalIgnoreCase)) return;
            list.Add(value);
        }

        private static void AddEnvironment(List<string> list, string variable)
        {
            foreach (EnvironmentVariableTarget target in new[] { EnvironmentVariableTarget.Process, EnvironmentVariableTarget.User, EnvironmentVariableTarget.Machine })
            {
                try { Add(list, Environment.GetEnvironmentVariable(variable, target)); }
                catch (System.Security.SecurityException) { }
            }
        }

        private static void AddCommand(List<string> list, string command)
        {
            command = command.Trim().Trim('"');
            if (!IsCommandName(command)) return;
            if (!command.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)) command += ".exe";
            string searchPath = Environment.GetEnvironmentVariable("PATH") ?? "";
            foreach (string entry in searchPath.Split(';'))
            {
                string directory = FullPath(entry, Environment.CurrentDirectory);
                if (directory.Length != 0) Add(list, Path.Combine(directory, command));
            }
        }

        private static bool IsCommandName(string value)
        {
            return value.IndexOfAny(new[] { '\\', '/', ':' }) < 0;
        }

        private static string FullPath(string path, string root)
        {
            if (String.IsNullOrWhiteSpace(path)) return "";
            try
            {
                path = Environment.ExpandEnvironmentVariables(path.Trim().Trim('"'));
                if (!Path.IsPathRooted(path)) path = Path.Combine(root, path);
                return Path.GetFullPath(path);
            }
            catch (Exception) { return ""; }
        }

        private static string[] Subdirectories(string path)
        {
            try { return Directory.GetDirectories(path); }
            catch (Exception) { return new string[0]; }
        }

        private static string RegistryString(RegistryHive hive, RegistryView view, string path, string value)
        {
            try
            {
                using (RegistryKey root = RegistryKey.OpenBaseKey(hive, view))
                using (RegistryKey key = root.OpenSubKey(path))
                    return key == null ? "" : Convert.ToString(key.GetValue(value, ""));
            }
            catch (Exception) { return ""; }
        }

        private static void AddMaxSdkUninstallEntries(List<string> candidates, RegistryHive hive, RegistryView view)
        {
            try
            {
                using (RegistryKey root = RegistryKey.OpenBaseKey(hive, view))
                using (RegistryKey uninstall = root.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall"))
                {
                    if (uninstall == null) return;
                    foreach (string name in uninstall.GetSubKeyNames())
                    {
                        using (RegistryKey item = uninstall.OpenSubKey(name))
                        {
                            if (item == null) continue;
                            string displayName = Convert.ToString(item.GetValue("DisplayName", ""));
                            if (displayName.IndexOf("3ds Max", StringComparison.OrdinalIgnoreCase) >= 0 && displayName.Contains("2025") && displayName.IndexOf("SDK", StringComparison.OrdinalIgnoreCase) >= 0)
                                Add(candidates, Convert.ToString(item.GetValue("InstallLocation", "")));
                        }
                    }
                }
            }
            catch (Exception) { }
        }

        private static string FirstMissing(string root, IEnumerable<string> files)
        {
            foreach (string file in files)
                if (!File.Exists(Path.Combine(root, file))) return file;
            return "";
        }

        private static string StripComments(string text)
        {
            return Regex.Replace(Regex.Replace(text, @"/\*.*?\*/", "", RegexOptions.Singleline), @"//[^\r\n]*", "");
        }

        private static bool Run(string executable, string arguments, out string output)
        {
            output = "";
            try
            {
                using (var process = new Process())
                {
                    process.StartInfo = new ProcessStartInfo(executable, arguments) {
                        UseShellExecute = false, CreateNoWindow = true,
                        RedirectStandardOutput = true, RedirectStandardError = true,
                        WorkingDirectory = Path.GetDirectoryName(executable)
                    };
                    if (!process.Start()) return false;
                    Task<string> stdout = process.StandardOutput.ReadToEndAsync();
                    Task<string> stderr = process.StandardError.ReadToEndAsync();
                    if (!process.WaitForExit(10000))
                    {
                        try { process.Kill(); } catch (Exception) { }
                        return false;
                    }
                    if (!Task.WaitAll(new Task[] { stdout, stderr }, 1000)) return false;
                    output = stdout.Result + "\n" + stderr.Result;
                    return process.ExitCode == 0;
                }
            }
            catch (Exception) { return false; }
        }
    }
}
