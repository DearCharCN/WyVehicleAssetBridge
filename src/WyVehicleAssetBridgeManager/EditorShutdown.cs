using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace WyVehicleAssetBridge.Setup
{
    internal enum EditorShutdownResult { Cancelled, AlreadyClosed, Closed }

    // Uses only Windows input/window messages and UE's built-in Save All/normal
    // close path. No plugin, scripting server, force-exit or discard confirmation.
    internal sealed class EditorShutdown : Form
    {
        private readonly Label status = new Label();
        private readonly CancellationTokenSource cancellation = new CancellationTokenSource();
        private bool finished;
        private EditorShutdownResult result = EditorShutdownResult.Cancelled;

        private EditorShutdown(RunningApplication app, Action<string> report)
        {
            Text = "保存并关闭 UE";
            Name = "UEPublishShutdownProgress";
            Font = new Font("Microsoft YaHei UI", 9F);
            ClientSize = new Size(510, 168);
            FormBorderStyle = FormBorderStyle.FixedDialog;
            StartPosition = FormStartPosition.CenterParent;
            MaximizeBox = MinimizeBox = false;
            ShowInTaskbar = false;
            AutoScaleMode = AutoScaleMode.Dpi;
            BackColor = Color.White;
            status.SetBounds(22, 20, 466, 91);
            status.Text = "正在保存 UE 的全部资源和地图…\r\n若 UE 出现保存位置、检出等提示，请在 UE 中完成。";
            Controls.Add(status);
            var cancel = new Button { Text = "停止等待", Name = "CancelUECloseWait", Size = new Size(104, 34), Location = new Point(384, 118) };
            cancel.Click += delegate { cancellation.Cancel(); };
            Controls.Add(cancel);
            CancelButton = cancel;
            FormClosing += delegate(object sender, FormClosingEventArgs e) {
                if (!finished) { e.Cancel = true; cancellation.Cancel(); }
            };
            Shown += async delegate {
                try
                {
                    result = await SaveAndCloseAsync(app, message => {
                        status.Text = message;
                        if (report != null) report(message.Replace("\r\n", " "));
                    }, cancellation.Token);
                }
                catch (OperationCanceledException)
                {
                    if (report != null) report("已停止等待 UE 退出；已发出的保存或正常关闭请求不会被撤销。");
                }
                catch (Exception error)
                {
                    if (report != null) report("UE 自动保存关闭未完成：" + error.Message);
                    status.Text = error.Message + "\r\n请处理后重试，或手动保存关闭 UE。";
                    cancel.Text = "返回";
                    // Keep the explanation visible until the user returns.
                    return;
                }
                finally
                {
                    finished = true;
                    if (cancellation.IsCancellationRequested || result != EditorShutdownResult.Cancelled)
                        Close();
                }
                Close();
            };
            // After a failed automatic attempt, the same button returns to the
            // original confirmation dialog, including its manual-close option.
            cancel.Click += delegate { if (finished) Close(); };
        }

        internal static EditorShutdownResult Execute(IWin32Window owner, RunningApplication app, Action<string> report)
        {
            using (var dialog = new EditorShutdown(app, report))
            {
                dialog.ShowDialog(owner);
                return dialog.result;
            }
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing) cancellation.Dispose();
            base.Dispose(disposing);
        }

        internal static async Task<EditorShutdownResult> SaveAndCloseAsync(RunningApplication app,
            Action<string> report, CancellationToken cancellation)
        {
            if (!app.IdentityKnown || !app.StartedUtc.HasValue || app.Headless)
                throw new InvalidOperationException("无法安全自动操作这个 UE 进程，请使用“我已手动关闭”。");
            Process process;
            try { process = Process.GetProcessById(app.Id); }
            catch (ArgumentException) { return EditorShutdownResult.AlreadyClosed; }
            using (process)
            {
                if (process.HasExited) return EditorShutdownResult.AlreadyClosed;
                if (process.StartTime.ToUniversalTime() != app.StartedUtc.Value)
                    throw new InvalidOperationException("UE 进程已发生变化，请重新检查。");
                // Hold the process handle throughout the operation (PID reuse must
                // never redirect keyboard input or a close request to another app).
                IntPtr processHandle = process.Handle;
                process.Refresh();
                IntPtr window = process.MainWindowHandle;
                if (window == IntPtr.Zero || !OwnsWindow(window, app.Id))
                    throw new InvalidOperationException("找不到目标 UE 的主窗口，请使用“我已手动关闭”。");
                if (!IsWindowEnabled(window))
                    throw new InvalidOperationException("UE 正在显示模态窗口，请先处理该窗口。");
                cancellation.ThrowIfCancellationRequested();
                if (IsIconic(window)) ShowWindowAsync(window, 9); // SW_RESTORE
                if (!SetForegroundWindow(window))
                    throw new InvalidOperationException("无法激活目标 UE 窗口，请关闭遮挡的弹窗后重试。");
                await Task.Delay(250, cancellation);
                if (process.HasExited) return EditorShutdownResult.AlreadyClosed;
                if (GetForegroundWindow() != window || !OwnsWindow(window, app.Id) || !IsWindowEnabled(window))
                    throw new InvalidOperationException("UE 未获得输入焦点，未发送保存快捷键。");
                // Do not release or combine with a physical modifier held by the user.
                if (IsKeyDown(0x10) || IsKeyDown(0x11) || IsKeyDown(0x12) || IsKeyDown(0x5B) || IsKeyDown(0x5C))
                    throw new InvalidOperationException("请松开键盘上的修饰键后重试。");
                SendSaveAll();
                report("已触发 UE“全部保存”。\r\n如出现保存路径、检出或错误提示，请先在 UE 中处理；可停止等待。");
                // Let queued input reach Slate, then require a responsive, enabled
                // main window before requesting normal close. WM_CLOSE itself runs
                // UE's dirty-package confirmation again, including save failures.
                await Task.Delay(1000, cancellation);
                var deadline = DateTime.UtcNow.AddMinutes(10);
                int readySamples = 0;
                while (!process.HasExited)
                {
                    cancellation.ThrowIfCancellationRequested();
                    if (DateTime.UtcNow >= deadline)
                        throw new TimeoutException("等待 UE 保存超时，进程未被强制关闭。");
                    bool ready = OwnsWindow(window, app.Id) && IsWindowEnabled(window);
                    if (ready) ready = await Task.Run(() => IsResponsive(window));
                    readySamples = ready ? readySamples + 1 : 0;
                    if (readySamples >= 4) break;
                    await Task.Delay(250, cancellation);
                }
                if (process.HasExited) return CheckExit(process);
                cancellation.ThrowIfCancellationRequested();
                if (!OwnsWindow(window, app.Id) || !IsWindowEnabled(window))
                    throw new InvalidOperationException("UE 窗口状态发生变化，请处理弹窗后重试。");
                if (!PostMessage(window, 0x0010, IntPtr.Zero, IntPtr.Zero)) // WM_CLOSE
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "发送 UE 正常关闭请求失败。");
                report("等待 UE 正常退出…\r\n如仍有未保存内容，请在 UE 中完成保存；发布会等进程退出后继续。");
                deadline = DateTime.UtcNow.AddMinutes(10);
                while (!process.HasExited)
                {
                    cancellation.ThrowIfCancellationRequested();
                    if (DateTime.UtcNow >= deadline)
                        throw new TimeoutException("UE 尚未退出，请手动完成保存关闭后重试。");
                    await Task.Delay(250, cancellation);
                }
                CheckExit(process);
                report("目标 UE 进程已退出。");
                return EditorShutdownResult.Closed;
            }
        }

        private static bool OwnsWindow(IntPtr window, int processId)
        {
            uint owner;
            return IsWindow(window) && GetWindowThreadProcessId(window, out owner) != 0 && owner == (uint)processId;
        }

        private static bool IsKeyDown(int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }

        private static EditorShutdownResult CheckExit(Process process)
        {
            if (process.ExitCode != 0)
                throw new InvalidOperationException("UE 未正常退出（退出码 " + process.ExitCode + "），已停止自动发布。");
            return EditorShutdownResult.Closed;
        }

        private static bool IsResponsive(IntPtr window)
        {
            UIntPtr result;
            return SendMessageTimeout(window, 0, UIntPtr.Zero, IntPtr.Zero, 2, 200, out result) != IntPtr.Zero;
        }

        private static void SendSaveAll()
        {
            var input = new[] { Key(0x11, false), Key(0x10, false), Key(0x53, false),
                Key(0x53, true), Key(0x10, true), Key(0x11, true) };
            if (SendInput((uint)input.Length, input, Marshal.SizeOf(typeof(INPUT))) != input.Length)
            {
                // Release only the modifiers that this operation may have pressed.
                var release = new[] { Key(0x53, true), Key(0x10, true), Key(0x11, true) };
                SendInput((uint)release.Length, release, Marshal.SizeOf(typeof(INPUT)));
                throw new Win32Exception("无法向 UE 发送全部保存快捷键，请检查窗口权限或使用手动关闭。");
            }
        }

        private static INPUT Key(ushort code, bool up)
        {
            return new INPUT { Type = 1, Data = new INPUTUNION { Keyboard = new KEYBDINPUT { Key = code, Flags = up ? 2u : 0u } } };
        }

        [StructLayout(LayoutKind.Sequential)] private struct INPUT { public uint Type; public INPUTUNION Data; }
        [StructLayout(LayoutKind.Explicit)] private struct INPUTUNION
        {
            [FieldOffset(0)] public KEYBDINPUT Keyboard;
            [FieldOffset(0)] public MOUSEINPUT Mouse;
        }
        [StructLayout(LayoutKind.Sequential)] private struct KEYBDINPUT { public ushort Key, Scan; public uint Flags, Time; public UIntPtr Extra; }
        [StructLayout(LayoutKind.Sequential)] private struct MOUSEINPUT { public int X, Y; public uint MouseData, Flags, Time; public UIntPtr Extra; }
        [DllImport("user32.dll", SetLastError = true)] private static extern uint SendInput(uint count, INPUT[] input, int size);
        [DllImport("user32.dll")] private static extern short GetAsyncKeyState(int key);
        [DllImport("user32.dll")] private static extern bool IsWindow(IntPtr window);
        [DllImport("user32.dll")] private static extern bool IsWindowEnabled(IntPtr window);
        [DllImport("user32.dll")] private static extern bool IsIconic(IntPtr window);
        [DllImport("user32.dll")] private static extern bool ShowWindowAsync(IntPtr window, int show);
        [DllImport("user32.dll")] private static extern bool SetForegroundWindow(IntPtr window);
        [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
        [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
        [DllImport("user32.dll", SetLastError = true)] private static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
        [DllImport("user32.dll", SetLastError = true)] private static extern IntPtr SendMessageTimeout(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam, uint flags, uint timeout, out UIntPtr result);
    }
}
