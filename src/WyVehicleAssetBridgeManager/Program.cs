using System;
using System.Windows.Forms;
using System.Reflection;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;

[assembly: AssemblyTitle("WyVehicleAssetBridgeManager · 载具资源桥接管理器")]
[assembly: AssemblyProduct("WyVehicleAssetBridgeManager")]
[assembly: AssemblyVersion("1.0.0.0")]
[assembly: System.Runtime.Versioning.TargetFramework(".NETFramework,Version=v4.8")]

namespace WyVehicleAssetBridge.Setup
{
    internal static class Program
    {
        [STAThread]
        private static int Main(string[] args)
        {
            string root = AppDomain.CurrentDomain.BaseDirectory;
            if (args.Length > 0) return CommandLine(root, args);
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new SetupForm(root));
            return 0;
        }

        private const string PublishUsage = "WyVehicleAssetBridgeManager.exe publish [-Target Both|UE|Max] [-ChangedOnly] [-Config path.ini] [-BuildOnly] [-Check] [-Help]";
        private const string EnvironmentUsage = "WyVehicleAssetBridgeManager.exe check-env [-Config path.ini] [-Help]";
        private static readonly string Usage = EnvironmentUsage + Environment.NewLine + PublishUsage;

        [DllImport("kernel32.dll")] private static extern bool AttachConsole(uint processId);
        [DllImport("kernel32.dll")] private static extern IntPtr GetStdHandle(int kind);
        [DllImport("kernel32.dll")] private static extern bool SetStdHandle(int kind, IntPtr handle);
        [DllImport("kernel32.dll")] private static extern uint GetFileType(IntPtr handle);

        private static void ConnectConsole()
        {
            // A windowed executable can inherit files/pipes or be started from an
            // interactive console. Attaching to the latter must preserve redirection.
            IntPtr stdout = GetStdHandle(-11), stderr = GetStdHandle(-12);
            uint outType = GetFileType(stdout), errType = GetFileType(stderr);
            if (AttachConsole(UInt32.MaxValue))
            {
                if (outType == 1 || outType == 3) SetStdHandle(-11, stdout);
                if (errType == 1 || errType == 3) SetStdHandle(-12, stderr);
            }
        }

        private static bool IsHelp(string value)
        {
            return value == "--help" || value == "-help" || value == "/?";
        }

        private static int CheckEnvironment(string root, string config)
        {
            string file = config ?? LocalConfig.FileName(root);
            Console.WriteLine("环境配置文件 " + file);
            var configured = LocalConfig.ReadFile(file, config == null);
            var result = Detection.CheckEnvironment(root, configured);
            int passed = 0, total = 0;
            foreach (string key in LocalConfig.Applications.Concat(LocalConfig.Dependencies))
            {
                string issue = result.Issues[key], path = result.Paths[key];
                bool valid = issue.Length == 0;
                Console.WriteLine("{0} {1} = {2}", valid ? "[通过]" : "[未通过]", LocalConfig.IniKey(key),
                    String.IsNullOrEmpty(path) ? "（未设置）" : path);
                if (valid) passed++;
                else Console.WriteLine("  " + issue);
                total++;
            }
            Console.WriteLine("环境检查 {0}/{1} 项通过。", passed, total);
            return passed == total ? 0 : 1;
        }

        private static int CommandLine(string root, string[] args)
        {
            ConnectConsole();
            try
            {
                string target = "Both", config = null;
                bool buildOnly = false, check = false, changedOnly = false;
                string command = args[0].ToLowerInvariant();
                if (IsHelp(command)) { Console.WriteLine(Usage); return 0; }
                bool environment = command == "check-env";
                if (!environment && command != "publish" && command != "build") throw new ArgumentException(Usage);
                string usage = environment ? EnvironmentUsage : PublishUsage;
                for (int i = 1; i < args.Length; i++)
                {
                    string key = args[i].ToLowerInvariant();
                    if (IsHelp(key)) { Console.WriteLine(usage); return 0; }
                    if (key == "-config")
                    {
                        if (++i >= args.Length || String.IsNullOrWhiteSpace(args[i])) throw new ArgumentException(usage);
                        config = Path.GetFullPath(args[i]);
                        continue;
                    }
                    if (environment) throw new ArgumentException("无法识别参数 " + args[i] + Environment.NewLine + usage);
                    switch (key)
                    {
                        case "-target":
                            if (++i >= args.Length) throw new ArgumentException(usage);
                            switch (args[i].ToUpperInvariant())
                            {
                                case "BOTH": target = "Both"; break;
                                case "UE": target = "UE"; break;
                                case "MAX": target = "Max"; break;
                                default: throw new ArgumentException(usage);
                            }
                            break;
                        case "-buildonly": buildOnly = true; break;
                        case "-changedonly": changedOnly = true; break;
                        case "-check": check = true; break;
                        default: throw new ArgumentException("无法识别参数 " + args[i] + Environment.NewLine + usage);
                    }
                }
                if (environment) return CheckEnvironment(root, config);
                new BuildPipeline(root, config, Console.WriteLine).Run(target, buildOnly, check, changedOnly);
                return 0;
            }
            catch (Exception ex) { Console.Error.WriteLine(ex.Message); return 1; }
        }
    }
}
