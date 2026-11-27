using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;

namespace WyVehicleAssetBridge.Setup
{
    internal static class LocalConfig
    {
        internal static readonly string[] Dependencies = { "CMake", "MaxSdk", "WindowsSdk", "Toolchain" };
        internal static readonly string[] Applications = { "Engine", "Project", "Max" };
        private static readonly Dictionary<string, string> IniKeys = new Dictionary<string, string> {
            {"Engine", "UE.EnginePath"}, {"Project", "UE.ProjectPath"}, {"Max", "Max.InstallPath"},
            {"CMake", "Build.CMakePath"}, {"MaxSdk", "Max.SdkPath"},
            {"WindowsSdk", "Build.WindowsSdkPath"}, {"Toolchain", "Build.VisualStudioPath"}
        };

        internal static string FileName(string root) { return Path.Combine(root, "build_and_deploy.ini"); }
        internal static string IniKey(string key) { return IniKeys[key]; }

        internal static bool ReopenUEAfterPublish(string root)
        {
            string value;
            var settings = ReadSettings(Lines(FileName(root), false));
            if (!settings.TryGetValue("UE.ReopenAfterPublish", out value)) return false;
            bool enabled;
            if (Boolean.TryParse(value, out enabled)) return enabled;
            if (value == "1" || value == "0") return value == "1";
            throw new InvalidDataException("UE.ReopenAfterPublish 需要为 true 或 false。");
        }

        internal static void SaveReopenUEAfterPublish(string root, bool enabled)
        {
            var lines = Lines(FileName(root), false);
            SetValue(lines, "UE.ReopenAfterPublish", enabled ? "true" : "false");
            WriteLines(FileName(root), lines);
        }

        // Preserve user comments, unknown settings and alternate build configurations on save.
        private static List<string> Lines(string file, bool allowMissing)
        {
            try { return new List<string>(File.ReadAllLines(file, Encoding.UTF8)); }
            catch (FileNotFoundException) { if (!allowMissing) throw; }
            catch (DirectoryNotFoundException) { if (!allowMissing) throw; }
            return new List<string>();
        }

        private static Dictionary<string, int> Index(List<string> lines)
        {
            var index = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
            var sections = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            string section = "";
            for (int i = 0; i < lines.Count; i++)
            {
                string line = lines[i].Trim();
                if (line.Length == 0 || line.StartsWith(";") || line.StartsWith("#")) continue;
                if (line.StartsWith("[") && line.EndsWith("]"))
                {
                    section = line.Substring(1, line.Length - 2).Trim();
                    if (section.Length > 0 && sections.Add(section)) continue;
                }
                else
                {
                    int equals = line.IndexOf('=');
                    if (section.Length > 0 && equals > 0)
                    {
                        string key = section + "." + line.Substring(0, equals).Trim();
                        if (!index.ContainsKey(key)) { index.Add(key, i); continue; }
                    }
                }
                throw new InvalidDataException("INI 第 " + (i + 1) + " 行格式错误或存在重复项。");
            }
            return index;
        }

        internal static Dictionary<string, string> Read(string root)
        {
            return ReadFile(FileName(root), true);
        }

        internal static Dictionary<string, string> ReadFile(string file, bool allowMissing)
        {
            string folder = Path.GetDirectoryName(Path.GetFullPath(file));
            var settings = ReadSettings(Lines(file, allowMissing));
            var result = new Dictionary<string, string>();
            foreach (var pair in IniKeys)
            {
                string value = "";
                string setting;
                if (settings.TryGetValue(pair.Value, out setting))
                {
                    value = Environment.ExpandEnvironmentVariables(setting);
                    try
                    {
                        if (value.Length > 0 && !(pair.Key == "CMake" && Path.GetFileName(value) == value))
                            value = Path.GetFullPath(Path.Combine(folder, value));
                    }
                    // Keep an invalid configured path visible so it can be corrected in the picker.
                    catch (ArgumentException) { }
                    catch (NotSupportedException) { }
                    catch (PathTooLongException) { }
                }
                result[pair.Key] = value;
            }
            return result;
        }

        internal static Dictionary<string,string> ReadSettings(List<string> lines)
        {
            var values = new Dictionary<string,string>(StringComparer.OrdinalIgnoreCase);
            foreach (var entry in Index(lines))
            {
                string line = lines[entry.Value];
                values.Add(entry.Key, line.Substring(line.IndexOf('=') + 1).Trim().Trim('"'));
            }
            return values;
        }

        internal static Dictionary<string, object> Json(string file)
        {
            var obj = new JavaScriptSerializer().DeserializeObject(File.ReadAllText(file)) as Dictionary<string, object>;
            if (obj == null) throw new InvalidDataException("无效的 JSON 描述文件：" + file);
            return obj;
        }

        internal static bool PluginDisabled(Dictionary<string,object> descriptor)
        {
            object entries;
            if (!descriptor.TryGetValue("Plugins", out entries) || !(entries is object[])) return false;
            foreach (var item in (object[])entries)
            {
                var plugin = item as Dictionary<string, object>;
                object name, enabled;
                if (plugin != null && plugin.TryGetValue("Name", out name) &&
                    String.Equals(Convert.ToString(name), "WyVehicleAssetBridge", StringComparison.OrdinalIgnoreCase) &&
                    plugin.TryGetValue("Enabled", out enabled) && Object.Equals(enabled, false)) return true;
            }
            return false;
        }

        internal static string ProjectFile(string path)
        {
            if (Directory.Exists(path))
            {
                var files = Directory.GetFiles(path, "*.uproject");
                if (files.Length != 1) throw new InvalidDataException("请直接选择望月的 .uproject 文件。");
                path = files[0];
            }
            if (!File.Exists(path) || !String.Equals(Path.GetExtension(path), ".uproject", StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("请选择 WangYue 的 .uproject 工程文件。");
            return path;
        }

        internal static string ValidateApplication(string key, string path)
        {
            if (String.IsNullOrWhiteSpace(path)) return "待选择";
            try
            {
                if (key == "Max")
                {
                    string exe = Path.Combine(path, "3dsmax.exe");
                    if (!File.Exists(exe)) return "目录下缺少 3dsmax.exe";
                    if (FileVersionInfo.GetVersionInfo(exe).ProductMajorPart != 27) return "当前插件需要 3ds Max 2025";
                }
                else if (key == "Engine")
                {
                    if (!File.Exists(Path.Combine(path, @"Engine\Build\BatchFiles\RunUAT.bat")) ||
                        !File.Exists(Path.Combine(path, @"Engine\Binaries\Win64\UnrealEditor.exe")))
                        return "请选择包含 Engine 文件夹的 UE5 根目录";
                    if (Convert.ToInt32(Json(Path.Combine(path, @"Engine\Build\Build.version"))["MajorVersion"]) != 5)
                        return "引擎主版本必须为 5";
                }
                else if (key == "Project")
                {
                    string file = ProjectFile(path);
                    var descriptor = Json(file);
                    if (!descriptor.ContainsKey("FileVersion") || Convert.ToInt32(descriptor["FileVersion"]) <= 0)
                        return "无效的 .uproject 描述";
                    bool isWangYue = String.Equals(Path.GetFileNameWithoutExtension(file), "WangYue", StringComparison.OrdinalIgnoreCase);
                    object modules;
                    if (!isWangYue && descriptor.TryGetValue("Modules", out modules) && modules is object[])
                    {
                        foreach (var item in (object[])modules)
                        {
                            var module = item as Dictionary<string, object>;
                            object moduleName;
                            if (module != null && module.TryGetValue("Name", out moduleName) &&
                                String.Equals(Convert.ToString(moduleName), "WangYue", StringComparison.OrdinalIgnoreCase)) isWangYue = true;
                        }
                    }
                    if (!isWangYue) return "请选择望月工程的 .uproject 文件";
                    if (PluginDisabled(descriptor)) return "工程已显式禁用 WyVehicleAssetBridge 插件";
                }
                else throw new ArgumentException("未知的路径类型");
                return "";
            }
            catch (Exception ex) { return ex.Message; }
        }

        internal static string EngineMatch(string project, string engine)
        {
            try
            {
                object association;
                if (!Json(ProjectFile(project)).TryGetValue("EngineAssociation", out association)) return "";
                var version = Regex.Match(Convert.ToString(association), @"^(\d+)\.(\d+)(?:\.\d+)?$");
                if (!version.Success) return ""; // A custom build ID does not specify a numeric version.
                var engineInfo = Json(Path.Combine(engine, @"Engine\Build\Build.version"));
                if (version.Groups[1].Value + "." + version.Groups[2].Value != engineInfo["MajorVersion"] + "." + engineInfo["MinorVersion"])
                    return "工程指定的 UE 版本为 " + association + "，请核对引擎目录";
                return "";
            }
            catch (Exception ex) { return ex.Message; }
        }

        private static void SetValue(List<string> lines, string key, string value)
        {
            if (value.IndexOfAny(new char[] { '\r', '\n' }) >= 0) throw new InvalidDataException("INI 路径包含换行符。");
            int separator = key.IndexOf('.');
            string section = "[" + key.Substring(0, separator) + "]";
            string entry = key.Substring(separator + 1) + "=" + value;
            var index = Index(lines);
            int position;
            if (index.TryGetValue(key, out position)) { lines[position] = entry; return; }
            position = lines.FindIndex(delegate(string line) { return String.Equals(line.Trim(), section, StringComparison.OrdinalIgnoreCase); });
            if (position < 0)
            {
                if (lines.Count > 0) lines.Add("");
                lines.Add(section);
                lines.Add(entry);
                return;
            }
            position++;
            while (position < lines.Count && !lines[position].TrimStart().StartsWith("[")) position++;
            while (position > 0 && lines[position-1].Trim().Length == 0) position--;
            lines.Insert(position, entry);
        }

        internal static void Save(string root, IDictionary<string, string> paths)
        {
            foreach (string key in Applications)
            {
                string issue = ValidateApplication(key, paths[key]);
                if (issue.Length > 0) throw new InvalidDataException(issue);
            }
            string mismatch = EngineMatch(paths["Project"], paths["Engine"]);
            if (mismatch.Length > 0) throw new InvalidDataException(mismatch);
            foreach (string key in Dependencies)
            {
                string issue = Detection.Validate(key, paths[key]);
                if (issue.Length > 0) throw new InvalidDataException(issue);
            }
            var lines = Lines(FileName(root), true);
            foreach (var pair in IniKeys) SetValue(lines, pair.Value, paths[pair.Key]);
            SetValue(lines, "Build.CMakeGenerator", "Visual Studio 17 2022");
            SetValue(lines, "Build.CMakeToolset", "v143");
            SetValue(lines, "Build.WindowsSdkVersion", "10.0.22621.0");
            int configuration = -1;
            if (!Index(lines).TryGetValue("Build.MaxConfiguration", out configuration) ||
                !Regex.IsMatch(lines[configuration], @"=\s*""?(Release|RelWithDebInfo)""?\s*$", RegexOptions.IgnoreCase))
                SetValue(lines, "Build.MaxConfiguration", "Release");
            WriteLines(FileName(root), lines);
        }

        private static void WriteLines(string target, List<string> lines)
        {
            string temp = target + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                File.WriteAllLines(temp, lines, new UTF8Encoding(false));
                if (File.Exists(target)) File.Replace(temp, target, null);
                else File.Move(temp, target);
            }
            finally { if (File.Exists(temp)) File.Delete(temp); }
        }
    }
}
