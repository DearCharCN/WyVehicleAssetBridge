using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace WyVehicleAssetBridge.Setup
{
    internal sealed partial class SetupForm
    {
        private readonly Label ueDestination = new Label(), maxDestination = new Label(), publishStatus = new Label();
        private readonly Label ueVersion = new Label(), maxVersion = new Label();
        private readonly Label ueMismatch = new Label(), maxMismatch = new Label();
        private readonly TextBox publishLog = new TextBox();
        private readonly Button publishUE = new Button(), publishMax = new Button(), publishAll = new Button(), publishChanged = new Button();
        private bool publishing;
        private bool publishPageSelected, ueVersionMismatch, maxVersionMismatch;
        private readonly Timer publishVersionTimer = new Timer { Interval = 250 };
        private List<FileSystemWatcher> publishVersionWatchers = new List<FileSystemWatcher>();
        private readonly object publishVersionGate = new object();
        private bool publishVersionDisposed, publishVersionPending, publishVersionRefreshRunning;
        private long publishVersionRequest, publishVersionApplied;
        private DateTime publishVersionDueUtc;
        private DateTime publishWatcherRetryUtc;
        private string publishVersionWatchSignature = "";

        private void InstallNavigation(Control environmentPage)
        {
            var layout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1, Margin = Padding.Empty };
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 136));
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            var rail = new FlowLayoutPanel { Dock = DockStyle.Fill, BackColor = Color.FromArgb(235, 240, 241),
                Padding = new Padding(8, 18, 8, 8), Margin = Padding.Empty, FlowDirection = FlowDirection.TopDown, WrapContents = false };
            var pages = new Panel { Dock = DockStyle.Fill, Margin = Padding.Empty };
            Controls.Add(layout);
            layout.Controls.Add(rail, 0, 0);
            layout.Controls.Add(pages, 1, 0);
            var publishPage = CreatePublishPage();
            pages.Controls.Add(publishPage);
            pages.Controls.Add(environmentPage);
            var environment = Nav("EnvironmentNav", "环境配置");
            var publish = Nav("PublishNav", "发布插件");
            rail.Controls.Add(environment);
            rail.Controls.Add(publish);
            environment.CheckedChanged += delegate {
                if (environment.Checked) {
                    publishPageSelected = false;
                    publishPage.Visible = false;
                    environmentPage.Visible = true;
                }
            };
            publish.CheckedChanged += delegate {
                if (!publish.Checked) return;
                environmentPage.Visible = false;
                publishPage.Visible = true;
                publishPageSelected = true;
                ShowDestinations();
                UpdatePublishButtons();
            };
            publishVersionTimer.Tick += async delegate { await RefreshPublishVersionsIfReady(); };
            Activated += delegate { if (publishPageSelected) QueuePublishVersionRefresh(true); };
            publishVersionTimer.Start();
            environment.Checked = true;
        }

        private static RadioButton Nav(string name, string text)
        {
            var nav = new RadioButton { Name = name, Text = text, Appearance = Appearance.Button, AccessibleRole = AccessibleRole.PageTab,
                AutoSize = false, TextAlign = ContentAlignment.MiddleLeft, Size = new Size(114, 42), Padding = new Padding(9, 0, 0, 0),
                FlatStyle = FlatStyle.Flat, BackColor = Color.FromArgb(235, 240, 241), ForeColor = Ink, Cursor = Cursors.Hand };
            nav.FlatAppearance.BorderSize = 0;
            nav.FlatAppearance.CheckedBackColor = Color.White;
            nav.CheckedChanged += delegate { nav.ForeColor = nav.Checked ? Good : Ink; };
            return nav;
        }

        private Control CreatePublishPage()
        {
            var page = new TableLayoutPanel { Name = "PublishPage", Dock = DockStyle.Fill, Padding = new Padding(24, 12, 24, 14),
                ColumnCount = 1, RowCount = 7 };
            page.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            Add(page, Title("发布插件", 17), 0, 50);
            Add(page, PublishRow("Unreal Engine 5", ueDestination, ueVersion, ueMismatch, publishUE, "UE"), 1, 94);
            Add(page, PublishRow("3ds Max 2025", maxDestination, maxVersion, maxMismatch, publishMax, "Max"), 2, 94);
            var actions = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.LeftToRight,
                WrapContents = false, Padding = new Padding(0, 6, 0, 0), Margin = Padding.Empty };
            StyleButton(publishAll, "全部发布", false);
            publishAll.Name = "PublishAll";
            publishAll.Margin = new Padding(0, 0, 10, 0);
            publishAll.Click += async delegate { await PublishAsync("Both"); };
            StyleButton(publishChanged, "增量发布", true);
            publishChanged.Name = "PublishChanged";
            publishChanged.Width = 114;
            publishChanged.Margin = Padding.Empty;
            publishChanged.Visible = false;
            publishChanged.Click += async delegate { await PublishAsync("Changed"); };
            actions.Controls.Add(publishAll);
            actions.Controls.Add(publishChanged);
            Add(page, actions, 3, 54);
            Add(page, Title("发布日志", 11), 4, 32);
            publishLog.Name = "PublishLog";
            publishLog.Dock = DockStyle.Fill;
            publishLog.Multiline = true;
            publishLog.ReadOnly = true;
            publishLog.ScrollBars = ScrollBars.Both;
            publishLog.WordWrap = false;
            publishLog.BackColor = Color.FromArgb(246, 248, 249);
            publishLog.BorderStyle = BorderStyle.FixedSingle;
            page.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            page.Controls.Add(publishLog, 0, 5);
            publishStatus.Name = "PublishStatus";
            publishStatus.AutoEllipsis = true;
            publishStatus.Dock = DockStyle.Fill;
            publishStatus.TextAlign = ContentAlignment.MiddleLeft;
            Add(page, publishStatus, 6, 40);
            return page;
        }

        private Control PublishRow(string title, Label destination, Label version, Label mismatch, Button button, string target)
        {
            var row = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 3, RowCount = 3,
                Padding = new Padding(10, 4, 7, 4), BackColor = Color.FromArgb(245, 247, 248), Margin = new Padding(0, 3, 0, 3) };
            row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            row.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 176));
            row.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 122));
            row.RowStyles.Add(new RowStyle(SizeType.Percent, 34));
            row.RowStyles.Add(new RowStyle(SizeType.Percent, 33));
            row.RowStyles.Add(new RowStyle(SizeType.Percent, 33));
            row.Controls.Add(Title(title, 10), 0, 0);
            destination.Dock = DockStyle.Fill;
            destination.AutoEllipsis = true;
            destination.ForeColor = Muted;
            destination.TextAlign = ContentAlignment.MiddleLeft;
            destination.Text = "正在读取发布目录…";
            row.Controls.Add(destination, 0, 1);
            version.Name = "PublishVersion" + target;
            version.Dock = DockStyle.Fill;
            version.AutoEllipsis = true;
            version.TextAlign = ContentAlignment.MiddleLeft;
            version.ForeColor = Muted;
            version.Text = "已安装版本：正在读取…";
            row.Controls.Add(version, 0, 2);
            mismatch.Name = "PublishMismatch" + target;
            mismatch.Dock = DockStyle.Fill;
            mismatch.TextAlign = ContentAlignment.MiddleRight;
            mismatch.ForeColor = Good;
            mismatch.Visible = false;
            row.Controls.Add(mismatch, 1, 0);
            row.SetRowSpan(mismatch, 3);
            StyleButton(button, "发布 " + target + " 插件", false);
            button.Width = 114;
            button.Name = "Publish" + target;
            button.Anchor = AnchorStyles.None;
            button.Click += async delegate { await PublishAsync(target); };
            row.Controls.Add(button, 2, 0);
            row.SetRowSpan(button, 3);
            return row;
        }

        private void ShowDestinations()
        {
            // Resolving project junctions and hashing sources belongs to the worker,
            // not to the WinForms event handler.
            QueuePublishVersionRefresh(true);
            if (publishLog.TextLength == 0) publishStatus.Text = File.Exists(LocalConfig.FileName(root))
                ? "使用已保存的配置。发布会先编译插件，再安装到目标目录。"
                : "请先在「环境配置」创建本机 INI。";
        }

        private void UpdatePublishButtons()
        {
            bool enabled = !busy && File.Exists(LocalConfig.FileName(root));
            publishUE.Enabled = publishMax.Enabled = publishAll.Enabled = enabled;
            publishChanged.Enabled = enabled && (ueVersionMismatch || maxVersionMismatch);
            publishChanged.Visible = ueVersionMismatch || maxVersionMismatch;
            PublishButtonColor(publishUE, ueVersionMismatch);
            PublishButtonColor(publishMax, maxVersionMismatch);
            PublishButtonColor(publishAll, false);
            PublishButtonColor(publishChanged, true);
        }

        private static void PublishButtonColor(Button button, bool green)
        {
            button.BackColor = green ? Good : Color.White;
            button.ForeColor = green ? Color.White : Ink;
            button.FlatAppearance.BorderColor = green ? Good : Color.FromArgb(192, 202, 207);
        }

        private void QueuePublishVersionRefresh(bool immediate)
        {
            lock (publishVersionGate)
            {
                if (publishVersionDisposed) return;
                publishVersionRequest++;
                publishVersionPending = true;
                publishVersionDueUtc = DateTime.UtcNow.AddMilliseconds(immediate ? 0 : 600);
            }
        }

        private async Task RefreshPublishVersionsIfReady()
        {
            if (!publishPageSelected || busy || publishing || publishVersionRefreshRunning) return;
            long request;
            lock (publishVersionGate)
            {
                if (publishVersionDisposed || !publishVersionPending || DateTime.UtcNow < publishVersionDueUtc) return;
                request = publishVersionRequest;
                publishVersionPending = false;
            }
            publishVersionRefreshRunning = true;
            try
            {
                var snapshot = await Task.Run(() => {
                    var result = new PublishVersionSnapshot {
                        UE = BuildPipeline.InspectVersion(root, "UE"), Max = BuildPipeline.InspectVersion(root, "Max")
                    };
                    result.Accepted = ReplacePublishVersionWatchers(request, result);
                    return result;
                });
                lock (publishVersionGate)
                {
                    if (publishVersionDisposed || !snapshot.Accepted || request != publishVersionRequest) return;
                }
                if (!publishPageSelected || busy || publishing || snapshot.CoverageChanged)
                {
                    // New watcher coverage gets one confirmation read: a file may
                    // have changed between the first inspection and watcher setup.
                    QueuePublishVersionRefresh(true);
                    return;
                }
                RenderPublishVersion(snapshot.UE, ueDestination, ueVersion, ueMismatch, out ueVersionMismatch);
                RenderPublishVersion(snapshot.Max, maxDestination, maxVersion, maxMismatch, out maxVersionMismatch);
                lock (publishVersionGate) { publishVersionApplied = request; }
                UpdatePublishButtons();
            }
            catch (Exception error)
            {
                lock (publishVersionGate)
                {
                    if (publishVersionDisposed || request != publishVersionRequest) return;
                }
                var failed = new PluginVersionInfo { Description = "读取版本失败：" + error.Message };
                RenderPublishVersion(failed, ueDestination, ueVersion, ueMismatch, out ueVersionMismatch);
                RenderPublishVersion(failed, maxDestination, maxVersion, maxMismatch, out maxVersionMismatch);
                lock (publishVersionGate) { publishVersionApplied = request; }
                UpdatePublishButtons();
            }
            finally { publishVersionRefreshRunning = false; }
        }

        private void RenderPublishVersion(PluginVersionInfo info, Label destination, Label version, Label mismatch, out bool knownMismatch)
        {
            destination.Text = String.IsNullOrEmpty(info.Destination) ? "无法确定发布目录" : info.Destination;
            tips.SetToolTip(destination, String.IsNullOrEmpty(info.Destination) ? info.Description : info.Destination);
            bool installedKnown = !String.IsNullOrEmpty(info.Installed);
            bool comparisonFailed = installedKnown && info.Failed;
            version.Text = "已安装版本：" + (installedKnown ? info.Installed.Substring(0, 12) :
                (String.IsNullOrWhiteSpace(info.Description) ? "版本未知" : info.Description)) +
                (comparisonFailed ? " · 源码比对失败" : "");
            version.ForeColor = comparisonFailed ? Attention : Muted;
            tips.SetToolTip(version, installedKnown ? info.Installed +
                (comparisonFailed ? "\r\n源码比对失败：" + info.Description : "") : info.Description);
            knownMismatch = info.Mismatch;
            mismatch.Text = knownMismatch ? "源码与插件版本不一致" : "";
            mismatch.Visible = knownMismatch;
            tips.SetToolTip(mismatch, knownMismatch ? "当前源码版本：" + info.Current : "");
        }

        // Watchers provide change signals; the timer never polls or hashes sources.
        // They are created on the worker as UNC paths and directory handles can block.
        private bool ReplacePublishVersionWatchers(long request, PublishVersionSnapshot snapshot)
        {
            var watches = new List<PublishWatchSpec>();
            AddPublishWatch(watches, Path.Combine(root, "ue_plugins", "WyVehicleAssetBridge"), true, "Source");
            AddPublishWatch(watches, Path.Combine(root, "3ds_plugins", "WyVehicleAssetBridge"), true, "Source");
            AddPublishWatch(watches, LocalConfig.FileName(root), false, "Config");
            AddPublishDestinationWatches(watches, snapshot.UE.Destination);
            AddPublishDestinationWatches(watches, snapshot.Max.Destination);
            var candidate = new List<FileSystemWatcher>();
            var signature = new StringBuilder();
            foreach (PublishWatchSpec spec in watches)
            {
                signature.Append(spec.Directory).Append('|').Append(spec.Scope).Append('|').Append(spec.Kind).Append('|')
                    .Append(spec.Recursive).AppendLine();
                FileSystemWatcher watcher = null;
                try
                {
                    watcher = new FileSystemWatcher(spec.Directory) { IncludeSubdirectories = spec.Recursive,
                        NotifyFilter = NotifyFilters.FileName | NotifyFilters.DirectoryName | NotifyFilters.LastWrite | NotifyFilters.Size,
                        InternalBufferSize = 16384 };
                    candidate.Add(watcher);
                    FileSystemEventHandler changed = delegate(object sender, FileSystemEventArgs args) {
                        if (PublishWatchMatches(spec, args.FullPath)) QueuePublishVersionRefresh(false);
                    };
                    watcher.Changed += changed;
                    watcher.Created += changed;
                    watcher.Deleted += changed;
                    watcher.Renamed += delegate(object sender, RenamedEventArgs args) {
                        if (PublishWatchMatches(spec, args.FullPath) || PublishWatchMatches(spec, args.OldFullPath))
                            QueuePublishVersionRefresh(false);
                    };
                    watcher.Error += delegate {
                        // Overflow can mean missed events. Coalesce recovery and
                        // back off if a share repeatedly refuses watcher handles.
                        lock (publishVersionGate)
                        {
                            if (publishVersionDisposed || DateTime.UtcNow < publishWatcherRetryUtc) return;
                            publishWatcherRetryUtc = DateTime.UtcNow.AddSeconds(30);
                        }
                        QueuePublishVersionRefresh(false);
                    };
                    watcher.EnableRaisingEvents = true;
                }
                catch (IOException) { if (watcher != null) { candidate.Remove(watcher); watcher.Dispose(); } }
                catch (UnauthorizedAccessException) { if (watcher != null) { candidate.Remove(watcher); watcher.Dispose(); } }
                catch (ArgumentException) { if (watcher != null) { candidate.Remove(watcher); watcher.Dispose(); } }
                catch (System.ComponentModel.Win32Exception) { if (watcher != null) { candidate.Remove(watcher); watcher.Dispose(); } }
            }
            List<FileSystemWatcher> old;
            bool accepted;
            lock (publishVersionGate)
            {
                accepted = !publishVersionDisposed && request == publishVersionRequest;
                if (accepted)
                {
                    old = publishVersionWatchers;
                    publishVersionWatchers = candidate;
                    snapshot.CoverageChanged = !String.Equals(publishVersionWatchSignature, signature.ToString(), StringComparison.OrdinalIgnoreCase);
                    publishVersionWatchSignature = signature.ToString();
                }
                else old = candidate;
            }
            foreach (FileSystemWatcher watcher in old) watcher.Dispose();
            return accepted;
        }

        private static void AddPublishDestinationWatches(List<PublishWatchSpec> watches, string destination)
        {
            if (String.IsNullOrWhiteSpace(destination)) return;
            AddPublishWatch(watches, destination, false, "Parent");
            if (Directory.Exists(destination)) AddPublishWatch(watches, destination, true, "Installed");
        }

        private static void AddPublishWatch(List<PublishWatchSpec> watches, string scope, bool recursive, string kind)
        {
            string full = Path.GetFullPath(scope).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            string directory = kind == "Config" || kind == "Parent" ? Path.GetDirectoryName(full) : full;
            while (!String.IsNullOrEmpty(directory) && !Directory.Exists(directory)) directory = Path.GetDirectoryName(directory);
            if (String.IsNullOrEmpty(directory)) return;
            watches.Add(new PublishWatchSpec { Directory = directory, Scope = full, Recursive = recursive, Kind = kind });
        }

        private static bool PublishWatchMatches(PublishWatchSpec spec, string path)
        {
            if (spec.Kind == "Config") return String.Equals(path, spec.Scope, StringComparison.OrdinalIgnoreCase);
            string prefix = spec.Scope + Path.DirectorySeparatorChar;
            if (String.Equals(path, spec.Scope, StringComparison.OrdinalIgnoreCase) ||
                spec.Scope.StartsWith(path.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) return true;
            // Scope is the plugin source tree, not its sibling build/SDK cache.
            // SourceFingerprint alone owns the generated-directory exclusion rules.
            return path.StartsWith(prefix, StringComparison.OrdinalIgnoreCase);
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                List<FileSystemWatcher> old;
                lock (publishVersionGate)
                {
                    publishVersionDisposed = true;
                    publishVersionPending = false;
                    publishVersionRequest++;
                    old = publishVersionWatchers;
                    publishVersionWatchers = new List<FileSystemWatcher>();
                }
                publishVersionTimer.Stop();
                publishVersionTimer.Dispose();
                foreach (FileSystemWatcher watcher in old) watcher.Dispose();
                tips.Dispose();
            }
            base.Dispose(disposing);
        }

        private sealed class PublishWatchSpec
        {
            internal string Directory, Scope, Kind;
            internal bool Recursive;
        }

        private sealed class PublishVersionSnapshot
        {
            internal PluginVersionInfo UE, Max;
            internal bool Accepted, CoverageChanged;
        }

        private void AppendLog(ConcurrentQueue<string> pending)
        {
            string line;
            var chunk = new StringBuilder();
            while (pending.TryDequeue(out line)) chunk.AppendLine(line);
            if (chunk.Length == 0) return;
            const int limit = 80000;
            if (publishLog.TextLength + chunk.Length > limit)
            {
                var text = publishLog.Text + chunk;
                publishLog.Text = text.Substring(Math.Max(0, text.Length - limit));
            }
            else publishLog.AppendText(chunk.ToString());
            publishLog.SelectionStart = publishLog.TextLength;
            publishLog.ScrollToCaret();
        }

        private async Task PublishAsync(string target)
        {
            if (busy) return;
            publishing = true;
            SetBusy(true);
            ShowDestinations();
            publishLog.Clear();
            publishStatus.ForeColor = Muted;
            string selection = target == "Both" ? "全部插件" : target == "Changed" ? "有版本差异的插件" : target + " 插件";
            publishStatus.Text = "正在编译并发布" + selection + "…";
            var lines = new ConcurrentQueue<string>();
            using (var timer = new Timer { Interval = 120 })
            {
                timer.Tick += delegate { AppendLog(lines); };
                timer.Start();
                try
                {
                    await Task.Run(() => new BuildPipeline(root, null, lines.Enqueue)
                        .Run(target == "Changed" ? "Both" : target, false, false, target == "Changed"));
                    publishStatus.ForeColor = Good;
                    publishStatus.Text = target == "Changed" ? "增量发布完成。" : selection + "发布成功。";
                }
                catch (Exception ex)
                {
                    lines.Enqueue(ex.Message);
                    publishStatus.ForeColor = Attention;
                    publishStatus.Text = "发布失败：" + ex.Message;
                }
                finally
                {
                    timer.Stop();
                    AppendLog(lines);
                    tips.SetToolTip(publishStatus, publishStatus.Text);
                    publishing = false;
                    SetBusy(false);
                    QueuePublishVersionRefresh(true);
                }
            }
        }
    }
}
