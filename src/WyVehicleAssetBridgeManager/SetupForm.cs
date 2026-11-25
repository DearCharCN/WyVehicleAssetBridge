using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace WyVehicleAssetBridge.Setup
{
    internal sealed partial class SetupForm : Form
    {
        private sealed class Entry
        {
            internal Label Path, Status;
            internal Button Action, Browse;
            internal bool Valid;
        }

        private readonly string root;
        private readonly Dictionary<string, Entry> entries = new Dictionary<string, Entry>();
        private Dictionary<string, string> paths;
        // Keep discoveries provisional until saved. Selecting the project can then
        // refine an automatically detected engine without replacing a manual choice.
        private Dictionary<string, string> configured;
        private readonly Label note = new Label();
        private readonly Button rescan = new Button(), save = new Button();
        private readonly ToolTip tips = new ToolTip();
        private bool busy, allValid;
        private static readonly Color Ink = Color.FromArgb(30, 44, 58);
        private static readonly Color Muted = Color.FromArgb(79, 96, 108);
        private static readonly Color Good = Color.FromArgb(29, 111, 82);
        private static readonly Color Attention = Color.FromArgb(150, 74, 22);

        internal SetupForm(string rootDirectory)
        {
            root = rootDirectory;
            Text = "WyVehicleAssetBridgeManager · 载具资源桥接管理器";
            Font = new Font("Microsoft YaHei UI", 9F);
            ForeColor = Ink;
            BackColor = Color.White;
            AutoScaleMode = AutoScaleMode.Dpi;
            StartPosition = FormStartPosition.CenterScreen;
            MinimumSize = new Size(855, 620);
            ClientSize = new Size(1040, 660);
            var body = new TableLayoutPanel {
                Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 13,
                Padding = new Padding(24, 12, 24, 12), BackColor = BackColor, AutoScroll = true
            };
            body.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            InstallNavigation(body);
            Add(body, Title("环境配置", 17), 0, 48);
            Add(body, Title("应用与工程路径", 11), 1, 34);
            AddEntry(body, "Engine", "Unreal Engine 5", 2, true);
            AddEntry(body, "Project", "望月 UE 工程", 3, true);
            AddEntry(body, "Max", "3ds Max 2025", 4, true);
            Add(body, Title("构建依赖", 11), 5, 38);
            AddEntry(body, "CMake", "CMake", 6, false);
            AddEntry(body, "MaxSdk", "3ds Max 2025 SDK", 7, false);
            AddEntry(body, "WindowsSdk", "Windows SDK 22621", 8, false);
            AddEntry(body, "Toolchain", "MSVC v143 · VS 2022", 9, false);
            body.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            note.Dock = DockStyle.Fill;
            note.TextAlign = ContentAlignment.MiddleLeft;
            note.AutoEllipsis = true;
            Add(body, note, 11, 44);
            var bar = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.RightToLeft, WrapContents = false };
            StyleButton(save, "保存配置", true);
            StyleButton(rescan, "重新检测", false);
            save.Name = "SaveConfig";
            rescan.Name = "Rescan";
            bar.Controls.Add(save);
            bar.Controls.Add(rescan);
            Add(body, bar, 12, 42);
            rescan.Click += async delegate { await RefreshPaths(null); };
            save.Click += async delegate { await SaveAsync(); };
            Shown += async delegate {
                try { paths = LocalConfig.Read(root); }
                catch (Exception ex) { SetBusy(false); rescan.Enabled = false; SetNote("读取 INI 失败，请修正后重新打开。" + ex.Message, true); return; }
                await RefreshPaths(null);
            };
            FormClosing += delegate(object sender, FormClosingEventArgs e) {
                if (busy && e.CloseReason == CloseReason.UserClosing)
                { e.Cancel = true; if (!publishing) SetNote("请等待当前检测或安装结束。", true); }
            };
            SetBusy(true);
        }

        private static void Add(TableLayoutPanel layout, Control control, int row, int height)
        {
            layout.RowStyles.Add(new RowStyle(SizeType.Absolute, height));
            layout.Controls.Add(control, 0, row);
        }

        private static Label Title(string text, float size)
        {
            return new Label { Text = text, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft,
                Font = new Font("Microsoft YaHei UI", size, FontStyle.Bold), ForeColor = Ink };
        }

        private static void StyleButton(Button button, string caption, bool primary)
        {
            button.Text = caption;
            button.Size = new Size(96, 34);
            button.FlatStyle = FlatStyle.Flat;
            button.FlatAppearance.BorderColor = primary ? Good : Color.FromArgb(192, 202, 207);
            button.BackColor = primary ? Good : Color.White;
            button.ForeColor = primary ? Color.White : Ink;
            button.Cursor = Cursors.Hand;
        }

        private void AddEntry(TableLayoutPanel body, string key, string label, int index, bool manual)
        {
            var row = new TableLayoutPanel { ColumnCount = 4, RowCount = 2, Dock = DockStyle.Fill,
                Margin = new Padding(0, 1, 0, 1), Padding = new Padding(9, 2, 4, 2),
                BackColor = Color.FromArgb(245, 247, 248) };
            row.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 170));
            row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            row.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, manual ? 0 : 102));
            row.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 105));
            row.RowStyles.Add(new RowStyle(SizeType.Percent, 54));
            row.RowStyles.Add(new RowStyle(SizeType.Percent, 46));
            var name = new Label { Text = label, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft };
            row.Controls.Add(name, 0, 0);
            row.SetRowSpan(name, 2);
            var entry = new Entry();
            entries.Add(key, entry);
            entry.Path = new Label { Dock = DockStyle.Fill, TextAlign = ContentAlignment.BottomLeft, AutoEllipsis = true,
                Name = key + "Path", Margin = new Padding(2, 0, 3, 0) };
            entry.Status = new Label { Dock = DockStyle.Fill, TextAlign = ContentAlignment.TopLeft, AutoEllipsis = true,
                Name = key + "Status", Margin = new Padding(2, 0, 3, 0), ForeColor = Muted };
            row.Controls.Add(entry.Path, 1, 0);
            row.Controls.Add(entry.Status, 1, 1);
            entry.Action = new Button { Name = key + "Action", Anchor = AnchorStyles.None };
            StyleButton(entry.Action, manual ? "选择路径…" : "自动安装", false);
            entry.Action.AccessibleName = label + " " + entry.Action.Text;
            entry.Action.Click += async delegate {
                if (manual) { if (Pick(key)) await RefreshPaths(null); }
                else await InstallAsync(key, label);
            };
            row.Controls.Add(entry.Action, 3, 0);
            row.SetRowSpan(entry.Action, 2);
            if (!manual)
            {
                entry.Browse = new Button { Name = key + "Browse", Anchor = AnchorStyles.None };
                StyleButton(entry.Browse, "选择路径…", false);
                entry.Browse.AccessibleName = label + " 选择路径";
                entry.Browse.Click += async delegate { if (Pick(key)) await RefreshPaths(null); };
                row.Controls.Add(entry.Browse, 2, 0);
                row.SetRowSpan(entry.Browse, 2);
            }
            Add(body, row, index, 54);
        }

        private bool Pick(string key)
        {
            string current = paths[key];
            if (key == "Project" || key == "CMake")
            {
                using (var dialog = new OpenFileDialog { Title = key == "Project" ? "选择望月的 UE 工程文件" : "选择 cmake.exe",
                    Filter = key == "Project" ? "Unreal 工程 (*.uproject)|*.uproject" : "CMake (cmake.exe)|cmake.exe",
                    CheckFileExists = true, RestoreDirectory = true })
                {
                    if (Directory.Exists(current)) dialog.InitialDirectory = current;
                    else if (File.Exists(current)) dialog.FileName = current;
                    if (dialog.ShowDialog(this) != DialogResult.OK) return false;
                    paths[key] = dialog.FileName;
                }
            }
            else
            {
                var descriptions = new Dictionary<string, string> {
                    { "Max", "选择含 3dsmax.exe 的安装目录" }, { "Engine", "选择含 Engine 文件夹的 UE5 安装目录" },
                    { "MaxSdk", "选择含 include 和 lib 的 maxsdk 目录" }, { "WindowsSdk", "选择 Windows Kits 的 10 目录" },
                    { "Toolchain", "选择含 VC 和 MSBuild 的 Visual Studio 2022 实例目录" }
                };
                using (var dialog = new FolderBrowserDialog { ShowNewFolderButton = false, Description = descriptions[key] })
                {
                    if (Directory.Exists(current)) dialog.SelectedPath = current;
                    if (dialog.ShowDialog(this) != DialogResult.OK) return false;
                    paths[key] = dialog.SelectedPath;
                }
            }
            configured[key] = paths[key];
            return true;
        }

        private void SetNote(string message, bool error)
        {
            note.ForeColor = error ? Attention : Muted;
            note.Text = message;
            tips.SetToolTip(note, message);
        }

        private void SetBusy(bool value)
        {
            busy = value;
            UpdatePublishButtons();
            rescan.Enabled = !value && paths != null;
            save.Enabled = !value && allValid;
            foreach (var pair in entries)
            {
                bool manual = Array.IndexOf(LocalConfig.Applications, pair.Key) >= 0;
                pair.Value.Action.Enabled = !value && paths != null && (manual || !pair.Value.Valid);
                if (pair.Value.Browse != null) pair.Value.Browse.Enabled = !value && paths != null;
            }
        }

        private void Render(string key, string issue, bool manual)
        {
            Entry entry = entries[key];
            string path = paths[key];
            bool ok = issue.Length == 0;
            entry.Valid = ok;
            entry.Path.Text = path.Length > 0 ? path : (manual ? "请选择本机路径" : "未找到可用的安装");
            entry.Status.Text = ok ? "已验证" : issue;
            entry.Status.ForeColor = ok ? Good : Attention;
            tips.SetToolTip(entry.Path, path);
            tips.SetToolTip(entry.Status, entry.Status.Text);
            if (!manual) entry.Action.Text = ok ? "已安装" : "自动安装";
        }

        private async Task RefreshPaths(string installMessage)
        {
            SetBusy(true);
            SetNote("正在检查路径并检测本机构建依赖…", false);
            try
            {
                if (configured == null) configured = new Dictionary<string, string>(paths);
                var input = new Dictionary<string, string>(configured, StringComparer.OrdinalIgnoreCase);
                var check = await Task.Run(() => Detection.CheckEnvironment(root, input));
                paths = check.Paths;
                allValid = true;
                foreach (string key in LocalConfig.Applications)
                {
                    Render(key, check.Issues[key], true);
                    allValid &= check.Issues[key].Length == 0;
                }
                foreach (string key in LocalConfig.Dependencies)
                {
                    Render(key, check.Issues[key], false);
                    allValid &= check.Issues[key].Length == 0;
                }
                if (allValid && !File.Exists(LocalConfig.FileName(root)))
                {
                    await Task.Run(() => LocalConfig.Save(root, paths));
                    configured = new Dictionary<string, string>(paths);
                    SetNote("已创建本地配置   " + LocalConfig.FileName(root), false);
                }
                else
                {
                    string hint = allValid ? "七项均已验证，可保存到 build_and_deploy.ini。" :
                        "七项全部通过后即可保存。首次配置会自动创建 build_and_deploy.ini。";
                    SetNote(String.IsNullOrEmpty(installMessage) ? hint : installMessage + "   " + hint, false);
                }
            }
            catch (Exception ex) { allValid = false; SetNote("检测或写入失败。" + ex.Message, true); }
            finally { SetBusy(false); }
        }

        private async Task SaveAsync()
        {
            SetBusy(true);
            try {
                await Task.Run(() => LocalConfig.Save(root, paths));
                configured = new Dictionary<string, string>(paths);
                SetNote("已保存本地配置   " + LocalConfig.FileName(root), false);
            }
            catch (Exception ex) { SetNote("保存失败。" + ex.Message, true); }
            finally { SetBusy(false); }
        }

        private async Task InstallAsync(string key, string label)
        {
            SetBusy(true);
            SetNote("正在准备安装 " + label + "…", false);
            var progress = new Progress<string>(message => SetNote(message, false));
            string last = "", failure = null;
            try
            {
                await Task.Run(() => Installers.Install(key, message => {
                    last = message;
                    ((IProgress<string>)progress).Report(message);
                }));
            }
            catch (Exception ex) { failure = label + " 安装失败。" + ex.Message; }
            await RefreshPaths(last);
            if (failure != null) { SetNote(failure, true); MessageBox.Show(this, failure, Text, MessageBoxButtons.OK, MessageBoxIcon.Warning); }
            else if (!entries[key].Valid && !String.IsNullOrEmpty(configured[key]))
                SetNote("安装器已结束。原配置路径仍不可用，请为 " + label + " 选择实际安装路径。" + last, true);
        }
    }
}
