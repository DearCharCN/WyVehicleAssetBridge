using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Runtime.InteropServices;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using Microsoft.Win32;

namespace WyVehicleAssetBridge.Setup
{
    // Called on the UI's worker thread. Download/verification never elevates this app.
    // Only the selected vendor installer is elevated, after its signature is verified.
    internal static class Installers
    {
        // Official sources / supported unattended installer arguments:
        // https://cmake.org/download/
        // https://aps.autodesk.com/developer/overview/3ds-max-api
        // https://learn.microsoft.com/en-us/windows/apps/windows-sdk/downloads
        // https://learn.microsoft.com/windows-server/administration/windows-commands/msiexec
        // https://learn.microsoft.com/en-us/visualstudio/install/command-line-parameter-examples
        private const string CMakeUrl = "https://github.com/Kitware/CMake/releases/download/v3.31.12/cmake-3.31.12-windows-x86_64.msi";
        private const string MaxSdkUrl = "https://autodesk-adn-transfer.s3.us-west-2.amazonaws.com/ADN+Extranet/M%26E/Max/Autodesk+3ds+Max+2025/SDK_3dsMax2025.msi";
        // Microsoft's archive entry: Windows SDK 10.0.22621.5040 (April 2025 servicing).
        // The installed headers/libraries retain the 10.0.22621.0 target directory.
        private const string WindowsSdkUrl = "https://go.microsoft.com/fwlink/?linkid=2311806";
        private const string SdkVersion = "10.0.22621.0";
        private static readonly object InstallLock = new object();

        internal static bool RestartRequired { get; private set; }

        internal static void Install(string key, Action<string> progress)
        {
            if (progress == null) progress = delegate { };
            if (!Environment.Is64BitOperatingSystem)
                throw new NotSupportedException("此项目需要 64 位 Windows。");
            if (!System.Threading.Monitor.TryEnter(InstallLock))
                throw new InvalidOperationException("已有环境安装正在进行，请等待它结束。");
            try
            {
                string cache = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "WyVehicleAssetBridge", "Installers", DateTime.UtcNow.ToString("yyyyMMdd-HHmmss") + "-" + Guid.NewGuid().ToString("N"));
                Directory.CreateDirectory(cache);
                progress("安装文件和日志目录：" + cache);
                ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
                switch (key)
                {
                    case "CMake": InstallCMake(cache, progress); break;
                    case "MaxSdk": InstallMaxSdk(cache, progress); break;
                    case "WindowsSdk": InstallWindowsSdk(cache, progress); break;
                    case "Toolchain": InstallToolchain(cache, progress); break;
                    default: throw new ArgumentException("此项目不支持自动安装：" + key, "key");
                }
                if (RestartRequired)
                    progress("安装器要求重启 Windows。请先保存工作并自行重启；此工具不会自动重启。");
            }
            finally { System.Threading.Monitor.Exit(InstallLock); }
        }

        private static void InstallCMake(string cache, Action<string> progress)
        {
            string installed = FindCMake();
            if (installed != null)
            {
                progress("已找到 CMake 3.21 或更新版本：" + installed);
                return;
            }
            string package = Download(CMakeUrl, Path.Combine(cache, "cmake-3.31.12-windows-x86_64.msi"), "Kitware, Inc.", progress);
            string log = Path.Combine(cache, "cmake-install.log");
            RunElevated(MsiExecPath(), "/i " + Quote(package) +
                " ALLUSERS=1 ADD_CMAKE_TO_PATH=System /passive /norestart REBOOT=ReallySuppress /L*v " + Quote(log), cache, log, progress);
            RefreshProcessPath();
            installed = FindCMake();
            if (installed == null) throw new InvalidOperationException("CMake 安装器结束，但没有找到可运行的 CMake 3.21+。请查看：" + log);
            progress("CMake 已验证：" + installed);
        }

        private static void InstallMaxSdk(string cache, Action<string> progress)
        {
            string package = Download(MaxSdkUrl, Path.Combine(cache, "SDK_3dsMax2025.msi"), "Autodesk, Inc.", progress);
            string log = Path.Combine(cache, "max2025-sdk-install.log");
            RunElevated(MsiExecPath(), "/i " + Quote(package) +
                " /passive /norestart REBOOT=ReallySuppress /L*v " + Quote(log), cache, log, progress);
            // The package's own Directory table selects the supported default SDK location.
            // The UI re-scans the machine and writes the chosen SDK path to the project INI.
            string sdk = Path.Combine(ProgramFiles64(), "Autodesk", "3ds Max 2025 SDK", "maxsdk");
            if (Detection.Validate("MaxSdk", sdk).Length == 0) progress("3ds Max 2025 SDK 已验证：" + sdk);
            else progress("3ds Max 2025 SDK 安装器已完成。请在刷新检测后确认 SDK 路径；若使用了自定义安装目录，请选择其 maxsdk 文件夹。日志：" + log);
        }

        private static void InstallWindowsSdk(string cache, Action<string> progress)
        {
            string package = Download(WindowsSdkUrl, Path.Combine(cache, "winsdksetup-22621.exe"), "Microsoft Corporation", progress);
            FileVersionInfo version = FileVersionInfo.GetVersionInfo(package);
            if (version.FileMajorPart != 10 || version.FileBuildPart != 22621)
                throw new InvalidOperationException("官方 SDK 下载地址返回了意外版本：" + version.FileVersion + "。未执行安装。");
            string log = Path.Combine(cache, "windows-sdk-install.log");
            // Real SDK installation registers KitsRoot10 and installs desktop headers,
            // x64/x86 libraries and tools. Merely unpacking an SDK would not do this.
            RunElevated(package, "/features OptionId.DesktopCPPx64 OptionId.DesktopCPPx86" +
                " /quiet /norestart /log " + Quote(log), cache, log, progress);
            string root = FindWindowsSdkRoot();
            if (root == null)
                throw new InvalidOperationException("Windows SDK 安装器结束，但未检测到完整的 " + SdkVersion +
                    " 头文件、x64 库和 rc.exe。请查看：" + log);
            progress("Windows SDK " + SdkVersion + " 已安装并注册：" + root);
        }

        private static void InstallToolchain(string cache, Action<string> progress)
        {
            string vswhere = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86),
                "Microsoft Visual Studio", "Installer", "vswhere.exe");
            string instance = null;
            string product = null;
            if (File.Exists(vswhere))
            {
                VerifyPublisher(vswhere, "Microsoft Corporation");
                const string filter = "-latest -products * -version [17.0,18.0) ";
                instance = Capture(vswhere, filter + "-property installationPath").Trim();
                product = Capture(vswhere, filter + "-property productId").Trim();
            }
            string edition = "buildtools";
            if (!String.IsNullOrWhiteSpace(instance))
            {
                if (String.Equals(product, "Microsoft.VisualStudio.Product.Community", StringComparison.OrdinalIgnoreCase)) edition = "community";
                else if (String.Equals(product, "Microsoft.VisualStudio.Product.Professional", StringComparison.OrdinalIgnoreCase)) edition = "professional";
                else if (String.Equals(product, "Microsoft.VisualStudio.Product.Enterprise", StringComparison.OrdinalIgnoreCase)) edition = "enterprise";
                else if (!String.Equals(product, "Microsoft.VisualStudio.Product.BuildTools", StringComparison.OrdinalIgnoreCase))
                    throw new InvalidOperationException("发现不支持自动修改的 VS 2022 产品：" + product + "，路径：" + instance);
                progress("为现有 Visual Studio 2022 补齐 v143 C++ 工具：" + instance);
            }
            else
            {
                instance = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86),
                    "Microsoft Visual Studio", "2022", "BuildTools");
                progress("未找到 VS 2022，将安装 Visual Studio 2022 Build Tools：" + instance);
            }
            string bootstrapper = Download("https://aka.ms/vs/17/release/vs_" + edition + ".exe",
                Path.Combine(cache, "vs_" + edition + ".exe"), "Microsoft Corporation", progress);
            string workload = edition == "buildtools" ? "Microsoft.VisualStudio.Workload.VCTools" :
                "Microsoft.VisualStudio.Workload.NativeDesktop";
            string args = (!String.IsNullOrWhiteSpace(product) ? "modify " : "") + "--installPath " + Quote(instance) +
                " --add " + workload +
                " --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64" +
                " --add Microsoft.Component.MSBuild" +
                " --passive --norestart --wait";
            // --wait is a bootstrapper option (not supported by Installer/setup.exe).
            // It makes the exit code describe the finished install/modify operation.
            string vsLog = Path.Combine(Path.GetTempPath(), "dd_setup_*.log");
            RunElevated(bootstrapper, args, cache, vsLog, progress);
            if (Detection.Validate("Toolchain", instance).Length != 0)
                throw new InvalidOperationException("VS 安装器结束，但目标实例缺少 v143 x64 编译器或 MSBuild：" + instance +
                    "。请查看 " + vsLog + " 并重新检测。");
            progress("VS 2022 v143 x64 工具链与 MSBuild 已验证：" + instance);
        }

        private static string Download(string url, string destination, string publisher, Action<string> progress)
        {
            progress("下载官方安装器：" + url);
            string partial = destination + ".partial";
            try
            {
                HttpWebRequest request = (HttpWebRequest)WebRequest.Create(url);
                request.AllowAutoRedirect = true;
                request.MaximumAutomaticRedirections = 8;
                request.Timeout = 60000;
                request.ReadWriteTimeout = 120000;
                request.UserAgent = "WyVehicleAssetBridge-EnvironmentSetup/0.1";
                using (HttpWebResponse response = (HttpWebResponse)request.GetResponse())
                {
                    if (response.ResponseUri.Scheme != Uri.UriSchemeHttps)
                        throw new InvalidOperationException("安装器下载被重定向到非 HTTPS 地址，已拒绝。");
                    using (Stream input = response.GetResponseStream())
                    using (FileStream output = new FileStream(partial, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                    {
                        byte[] buffer = new byte[128 * 1024];
                        long total = 0;
                        long nextReport = 16L * 1024 * 1024;
                        int read;
                        while ((read = input.Read(buffer, 0, buffer.Length)) != 0)
                        {
                            output.Write(buffer, 0, read);
                            total += read;
                            if (total >= nextReport)
                            {
                                progress(String.Format("已下载 {0:N1} MB{1}", total / 1048576.0,
                                    response.ContentLength > 0 ? " / " + (response.ContentLength / 1048576.0).ToString("N1") + " MB" : ""));
                                nextReport = total + 16L * 1024 * 1024;
                            }
                        }
                        if (total == 0 || (response.ContentLength >= 0 && total != response.ContentLength))
                            throw new IOException("安装器下载不完整，请重试。");
                    }
                }
                File.Move(partial, destination);
                VerifyPublisher(destination, publisher);
                progress("已验证安装器的有效数字签名，发布者：" + publisher);
                return destination;
            }
            catch
            {
                if (File.Exists(partial)) File.Delete(partial);
                throw;
            }
        }

        private static void RunElevated(string executable, string arguments, string workingDirectory,
            string log, Action<string> progress)
        {
            progress("开始安装；Windows 可能请求管理员授权。请等待安装器完成。");
            progress("安装日志：" + log);
            ProcessStartInfo info = new ProcessStartInfo(executable, arguments);
            info.UseShellExecute = true;
            info.Verb = "runas";
            info.WorkingDirectory = workingDirectory;
            // UAC/vendor progress may be displayed; do not elevate the parent UI.
            try
            {
                using (Process process = Process.Start(info))
                {
                    if (process == null) throw new InvalidOperationException("无法启动安装器。");
                    while (!process.WaitForExit(30000)) progress("安装仍在进行，请勿关闭安装器。日志：" + log);
                    int code = process.ExitCode;
                    if (code == 3010 || code == 1641) { RestartRequired = true; return; }
                    if (code == 0) return;
                    if (code == 1602 || code == 1223) throw new OperationCanceledException("安装已由用户取消。");
                    if (code == 1618) throw new InvalidOperationException("Windows 中另一个安装任务尚未完成。请等待后重试。日志：" + log);
                    throw new InvalidOperationException("安装失败，退出码 " + code + "（0x" + code.ToString("X8") + "）。日志：" + log);
                }
            }
            catch (Win32Exception error)
            {
                if (error.NativeErrorCode == 1223) throw new OperationCanceledException("管理员授权已取消，未执行安装。", error);
                throw;
            }
        }

        private static string Quote(string value)
        {
            // CommandLineToArgvW-compatible quoting, including trailing backslashes.
            StringBuilder result = new StringBuilder("\"");
            int backslashes = 0;
            foreach (char c in value)
            {
                if (c == '\\') { backslashes++; continue; }
                if (c == '"') { result.Append('\\', backslashes * 2 + 1).Append(c); backslashes = 0; continue; }
                result.Append('\\', backslashes).Append(c);
                backslashes = 0;
            }
            return result.Append('\\', backslashes * 2).Append('"').ToString();
        }

        private static string Capture(string executable, string arguments)
        {
            ProcessStartInfo info = new ProcessStartInfo(executable, arguments);
            info.UseShellExecute = false;
            info.CreateNoWindow = true;
            info.RedirectStandardOutput = true;
            using (Process process = Process.Start(info))
            {
                string text = process.StandardOutput.ReadToEnd();
                process.WaitForExit();
                if (process.ExitCode != 0) throw new InvalidOperationException("检测命令失败：" + executable + "，退出码 " + process.ExitCode);
                return text;
            }
        }

        private static string ProgramFiles64()
        {
            return Environment.GetEnvironmentVariable("ProgramW6432") ?? Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);
        }

        private static string MsiExecPath()
        {
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "msiexec.exe");
        }

        private static void RefreshProcessPath()
        {
            string machine = Environment.GetEnvironmentVariable("PATH", EnvironmentVariableTarget.Machine) ?? "";
            string user = Environment.GetEnvironmentVariable("PATH", EnvironmentVariableTarget.User) ?? "";
            string current = Environment.GetEnvironmentVariable("PATH") ?? "";
            Environment.SetEnvironmentVariable("PATH", machine + ";" + user + ";" + current);
        }

        private static string FindCMake()
        {
            string search = Path.Combine(ProgramFiles64(), "CMake", "bin") + ";" + (Environment.GetEnvironmentVariable("PATH") ?? "");
            foreach (string raw in search.Split(';'))
            {
                string directory = Environment.ExpandEnvironmentVariables(raw.Trim().Trim('"'));
                if (String.IsNullOrWhiteSpace(directory)) continue;
                try
                {
                    string file = Path.Combine(directory, "cmake.exe");
                    if (!File.Exists(file)) continue;
                    if (Detection.Validate("CMake", file).Length == 0) return file;
                }
                catch (Exception error) { if (!(error is IOException) && !(error is Win32Exception) && !(error is ArgumentException) && !(error is InvalidOperationException)) throw; }
            }
            return null;
        }

        private static string FindWindowsSdkRoot()
        {
            foreach (RegistryView view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
                using (RegistryKey machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, view))
                using (RegistryKey key = machine.OpenSubKey(@"SOFTWARE\Microsoft\Windows Kits\Installed Roots"))
                {
                    string root = key == null ? null : key.GetValue("KitsRoot10") as string;
                    if (root != null && Detection.Validate("WindowsSdk", root).Length == 0) return root;
                }
            return null;
        }

        private static void VerifyPublisher(string path, string expectedPublisher)
        {
            WinTrustFile file = new WinTrustFile();
            file.Size = (uint)Marshal.SizeOf(typeof(WinTrustFile));
            file.Path = path;
            IntPtr filePointer = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(WinTrustFile)));
            Marshal.StructureToPtr(file, filePointer, false);
            WinTrustData data = new WinTrustData();
            data.Size = (uint)Marshal.SizeOf(typeof(WinTrustData));
            data.UIChoice = 2; // WTD_UI_NONE
            data.RevocationChecks = 1; // WTD_REVOKE_WHOLECHAIN
            data.UnionChoice = 1; // WTD_CHOICE_FILE
            data.File = filePointer;
            data.StateAction = 1; // WTD_STATEACTION_VERIFY; close state in finally.
            data.ProviderFlags = 0x80; // WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT
            Guid policy = new Guid("00AAC56B-CD44-11D0-8CC2-00C04FC295EE");
            try
            {
                int result = WinVerifyTrust(new IntPtr(-1), ref policy, ref data);
                if (result != 0)
                    throw new InvalidOperationException("安装器数字签名或证书链校验失败（0x" + result.ToString("X8") + "），未运行：" + path);
                using (X509Certificate2 certificate = new X509Certificate2(X509Certificate.CreateFromSignedFile(path)))
                {
                    string signer = certificate.GetNameInfo(X509NameType.SimpleName, false);
                    if (!String.Equals(signer, expectedPublisher, StringComparison.OrdinalIgnoreCase))
                        throw new InvalidOperationException("安装器发布者不符合预期。预期：" + expectedPublisher + "；实际：" + signer + "。未运行安装器。");
                }
            }
            finally
            {
                data.StateAction = 2; // WTD_STATEACTION_CLOSE
                WinVerifyTrust(new IntPtr(-1), ref policy, ref data);
                Marshal.DestroyStructure(filePointer, typeof(WinTrustFile));
                Marshal.FreeHGlobal(filePointer);
            }
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct WinTrustFile
        {
            public uint Size;
            [MarshalAs(UnmanagedType.LPWStr)] public string Path;
            public IntPtr FileHandle;
            public IntPtr KnownSubject;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct WinTrustData
        {
            public uint Size;
            public IntPtr PolicyCallbackData;
            public IntPtr SIPClientData;
            public uint UIChoice;
            public uint RevocationChecks;
            public uint UnionChoice;
            public IntPtr File;
            public uint StateAction;
            public IntPtr StateData;
            public IntPtr URLReference;
            public uint ProviderFlags;
            public uint UIContext;
        }

        [DllImport("wintrust.dll", ExactSpelling = true, PreserveSig = true)]
        private static extern int WinVerifyTrust(IntPtr window, ref Guid action, ref WinTrustData data);
    }
}
