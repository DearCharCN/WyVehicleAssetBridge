using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Web.Script.Serialization;
using Microsoft.Win32.SafeHandles;

namespace WyVehicleAssetBridge.Setup
{
    internal sealed class PackageItem
    {
        public string Source;
        public string Destination;
        public string Root;
        public string Name;
    }

    internal static class PackageStore
    {
        private const string Product = "WyVehicleAssetBridge";
        private const string MarkerName = ".wy-vehicle-asset-bridge-managed.json";
        private const string LegacyProduct = "WyAssetBridge";
        private const string LegacyMarkerName = ".wy-asset-bridge-managed.json";
        private const uint InvalidAttributes = 0xffffffff;
        private static readonly StringComparison PathComparison = StringComparison.OrdinalIgnoreCase;

        // Resolve the existing prefix through a directory handle. This follows an
        // intentional project Plugins junction, including junctions in its ancestors.
        // A missing suffix is preserved; a dangling link is never treated as missing.
        internal static string ResolveDirectory(string path)
        {
            string full = Normalize(path);
            string current = full;
            var suffix = new List<string>();
            FileAttributes attributes;
            while (!TryAttributes(current, out attributes))
            {
                string parent = Parent(current);
                if (parent == null) throw new DirectoryNotFoundException("找不到路径所在的磁盘或共享目录：" + full);
                suffix.Add(Path.GetFileName(current));
                current = parent;
            }
            if ((attributes & FileAttributes.Directory) == 0)
                throw new IOException("预期为目录，实际为文件：" + current);
            using (SafeFileHandle handle = CreateFile(current, 0, 7, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero))
            {
                if (handle.IsInvalid)
                    throw NativeError("无法解析目录（链接可能已损坏）", current);
                var buffer = new StringBuilder(512);
                uint length = GetFinalPathNameByHandle(handle, buffer, (uint)buffer.Capacity, 0);
                if (length == 0) throw NativeError("无法取得目录的物理路径", current);
                if (length >= buffer.Capacity)
                {
                    buffer = new StringBuilder(checked((int)length + 1));
                    length = GetFinalPathNameByHandle(handle, buffer, (uint)buffer.Capacity, 0);
                    if (length == 0 || length >= buffer.Capacity)
                        throw NativeError("无法取得完整目录物理路径", current);
                }
                string resolved = buffer.ToString();
                if (resolved.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase)) resolved = @"\\" + resolved.Substring(8);
                else if (resolved.StartsWith(@"\\?\", StringComparison.OrdinalIgnoreCase))
                {
                    if (resolved.Length < 7 || !Char.IsLetter(resolved[4]) || resolved[5] != ':' || resolved[6] != '\\')
                        throw new IOException("目录物理路径没有可用的盘符或 UNC 共享名称：" + resolved);
                    resolved = resolved.Substring(4);
                }
                for (int i = suffix.Count - 1; i >= 0; i--) resolved = Path.Combine(resolved, suffix[i]);
                resolved = Normalize(resolved);
                CheckChain(resolved);
                return resolved;
            }
        }

        // This checks the path and ancestors only. It deliberately does not scan
        // unrelated build caches or SDKs. Recursive operations inspect their own
        // subtree separately, before traversing or removing any child.
        internal static void Guard(string root, string path)
        {
            string fullRoot = Normalize(root);
            string full = Normalize(path);
            if (!IsChild(fullRoot, full))
                throw new IOException("拒绝操作管理范围之外的路径：" + full + "；管理根目录：" + fullRoot);
            CheckChain(full);
            FileAttributes attributes;
            if (TryAttributes(fullRoot, out attributes) && (attributes & FileAttributes.Directory) == 0)
                throw new IOException("管理根路径不是目录：" + fullRoot);
        }

        internal static void AssertOwned(string path)
        {
            AssertOwnedProduct(path, MarkerName, Product);
        }

        private static void AssertOwnedProduct(string path, string markerName, string expectedProduct)
        {
            string full = Normalize(path);
            CheckChain(full);
            FileAttributes attributes;
            if (!TryAttributes(full, out attributes)) return;
            if ((attributes & FileAttributes.Directory) == 0) throw new IOException("不能替换现有文件：" + full);
            ReadOwnedMarker(full, markerName, expectedProduct);
        }

        private static Dictionary<string, object> ReadOwnedMarker(string full, string markerName, string expectedProduct)
        {
            string marker = Path.Combine(full, markerName);
            Guard(full, marker);
            FileAttributes attributes;
            if (!TryAttributes(marker, out attributes) || (attributes & FileAttributes.Directory) != 0)
                throw new IOException("现有目录不是此工具部署的插件，拒绝覆盖：" + full);
            if (new FileInfo(marker).Length > 65536) throw new IOException("插件管理标记过大：" + marker);
            try
            {
                // File.ReadAllText detects the UTF-8 BOM written by PowerShell 5.1.
                var serializer = new JavaScriptSerializer { MaxJsonLength = 65536, RecursionLimit = 8 };
                var data = serializer.Deserialize<Dictionary<string, object>>(File.ReadAllText(marker, Encoding.UTF8));
                object product;
                if (data == null || !data.TryGetValue("Product", out product) ||
                    !(product is string) || !String.Equals((string)product, expectedProduct, StringComparison.Ordinal))
                    throw new IOException("插件管理标记不匹配：" + marker);
                return data;
            }
            catch (IOException) { throw; }
            catch (Exception error) { throw new IOException("无法读取插件管理标记：" + marker, error); }
        }

        internal static void Verify(string directory, string name)
        {
            string full = Normalize(directory);
            RequireDirectory(full);
            foreach (string relative in RequiredFiles(name))
            {
                string file = Path.Combine(full, relative);
                Guard(full, file);
                FileAttributes attributes;
                if (!TryAttributes(file, out attributes) || (attributes & FileAttributes.Directory) != 0 || new FileInfo(file).Length == 0)
                    throw new IOException("缺少有效的 " + CanonicalName(name) + " 构建产物：" + file);
            }
        }

        internal static string ReadInstalledFingerprint(string directory, string name)
        {
            string full = Normalize(directory);
            RequireDirectory(full);
            var data = ReadOwnedMarker(full, MarkerName, Product);
            object target;
            if (!data.TryGetValue("Target", out target) || !(target is string) ||
                !String.Equals((string)target, CanonicalName(name), StringComparison.Ordinal))
                throw new IOException("插件管理标记的目标不匹配：" + Path.Combine(full, MarkerName));
            Verify(full, name);
            object fingerprint;
            // Older managed packages have Product/Target but no source identity.
            if (!data.TryGetValue("SourceFingerprint", out fingerprint)) return "";
            if (!(fingerprint is string) || !IsFingerprint((string)fingerprint))
                throw new IOException("插件管理标记中的源码指纹格式无效：" + Path.Combine(full, MarkerName));
            return (string)fingerprint;
        }

        private static bool IsFingerprint(string value)
        {
            if (value == null || value.Length != 64) return false;
            foreach (char c in value)
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
            return true;
        }

        internal static void Mark(string directory, string name, string fingerprint)
        {
            if (!IsFingerprint(fingerprint)) throw new ArgumentException("源码指纹必须是 64 位小写十六进制 SHA-256。", "fingerprint");
            string full = Normalize(directory);
            Verify(full, name);
            string marker = Path.Combine(full, MarkerName);
            string temporary = Path.Combine(full, "." + Product + "-marker-" + Guid.NewGuid().ToString("N") + ".tmp");
            Guard(full, marker);
            Guard(full, temporary);
            var data = new Dictionary<string, object> {
                { "Product", Product }, { "Target", CanonicalName(name) },
                { "SourceFingerprint", fingerprint },
                { "BuiltAtUtc", DateTime.UtcNow.ToString("o", CultureInfo.InvariantCulture) }
            };
            try
            {
                using (var stream = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                using (var writer = new StreamWriter(stream, new UTF8Encoding(false)))
                    writer.Write(new JavaScriptSerializer().Serialize(data));
                Guard(full, marker);
                FileAttributes attributes;
                if (TryAttributes(marker, out attributes))
                {
                    if ((attributes & FileAttributes.Directory) != 0) throw new IOException("管理标记路径是目录：" + marker);
                    File.Replace(temporary, marker, null);
                }
                else File.Move(temporary, marker);
            }
            finally
            {
                FileAttributes attributes;
                if (TryAttributes(temporary, out attributes))
                {
                    Guard(full, temporary);
                    File.Delete(temporary);
                }
            }
        }

        internal static void Remove(string root, string path)
        {
            string fullRoot = Normalize(root);
            string full = Normalize(path);
            Guard(fullRoot, full);
            FileAttributes attributes;
            if (!TryAttributes(full, out attributes)) return;
            // Preflight the complete target first: a link anywhere prevents even
            // partial deletion. Directory.Delete(..., true) is intentionally unused.
            var files = new List<string>();
            var directories = new List<string>();
            Inventory(fullRoot, full, files, directories);
            foreach (string file in files)
            {
                Guard(fullRoot, file);
                if (!TryAttributes(file, out attributes)) continue;
                if ((attributes & FileAttributes.Directory) != 0) throw new IOException("删除期间路径类型已改变：" + file);
                ClearReadOnly(file, attributes);
                File.Delete(file);
            }
            for (int i = directories.Count - 1; i >= 0; i--)
            {
                string directory = directories[i];
                Guard(fullRoot, directory);
                if (!TryAttributes(directory, out attributes)) continue;
                if ((attributes & FileAttributes.Directory) == 0) throw new IOException("删除期间路径类型已改变：" + directory);
                ClearReadOnly(directory, attributes);
                Directory.Delete(directory, false);
            }
        }

        internal static void Install(IEnumerable<PackageItem> items, Action<string> report)
        {
            if (items == null) throw new ArgumentNullException("items");
            var records = new List<InstallRecord>();
            string transaction = Guid.NewGuid().ToString("N");
            foreach (PackageItem input in items)
            {
                if (input == null) throw new ArgumentException("安装项不能为空。", "items");
                var item = new PackageItem { Source = Normalize(input.Source), Destination = Normalize(input.Destination),
                    Root = Normalize(input.Root), Name = CanonicalName(input.Name) };
                Guard(item.Root, item.Destination);
                if (SamePath(item.Source, item.Destination) || IsChild(item.Source, item.Destination) || IsChild(item.Destination, item.Source))
                    throw new IOException("插件来源与目标目录不能相同或互相包含：" + item.Destination);
                foreach (InstallRecord previous in records)
                    if (SamePath(previous.Item.Destination, item.Destination) || IsChild(previous.Item.Destination, item.Destination) ||
                        IsChild(item.Destination, previous.Item.Destination))
                        throw new IOException("安装目标重复或互相包含：" + item.Destination);
                string parent = Parent(item.Destination);
                string id = transaction + "-" + records.Count.ToString(CultureInfo.InvariantCulture);
                // Only the exact former product directory next to an official new
                // destination is eligible. Never search for or clean old test backups.
                string legacy = String.Equals(Path.GetFileName(item.Destination), Product, PathComparison)
                    ? Path.Combine(parent, LegacyProduct) : null;
                if (legacy != null)
                {
                    Guard(item.Root, legacy);
                    if (PathsOverlap(legacy, item.Source)) throw new IOException("旧版迁移目录不能包含插件来源：" + legacy);
                }
                foreach (InstallRecord previous in records)
                {
                    if ((legacy != null && (PathsOverlap(legacy, previous.Item.Destination) ||
                        (previous.Legacy != null && PathsOverlap(legacy, previous.Legacy)))) ||
                        (previous.Legacy != null && PathsOverlap(previous.Legacy, item.Destination)))
                        throw new IOException("安装目标与旧版迁移目录发生冲突：" + item.Destination);
                }
                records.Add(new InstallRecord { Item = item, Stage = Path.Combine(parent, "." + Product + "-stage-" + id),
                    Backup = Path.Combine(parent, "." + Product + "-backup-" + id), Legacy = legacy,
                    LegacyBackup = legacy == null ? null : Path.Combine(parent, "." + Product + "-legacy-backup-" + id) });
            }
            if (records.Count == 0) return;
            try
            {
                // Finish every copy/verification before replacing any destination.
                foreach (InstallRecord record in records)
                {
                    PackageItem item = record.Item;
                    Guard(item.Root, item.Destination);
                    AssertOwned(item.Destination);
                    Verify(item.Source, item.Name);
                    AssertOwned(item.Source);
                    record.LegacyPresent = InspectLegacy(record);
                    Guard(item.Root, record.Stage);
                    Guard(item.Root, record.Backup);
                    RequireMissing(record.Stage);
                    RequireMissing(record.Backup);
                    if (record.LegacyBackup != null)
                    {
                        Guard(item.Root, record.LegacyBackup);
                        RequireMissing(record.LegacyBackup);
                    }
                    EnsureDirectory(item.Root, Parent(item.Destination));
                    Guard(item.Root, record.Stage);
                    if (!CreateDirectory(record.Stage, IntPtr.Zero)) throw NativeError("无法独占创建安装临时目录", record.Stage);
                    record.StageCreated = true;
                    CopyTree(item.Source, record.Stage, item.Root);
                    Verify(record.Stage, item.Name);
                    AssertOwned(record.Stage);
                }
                foreach (InstallRecord record in records)
                {
                    PackageItem item = record.Item;
                    Guard(item.Root, item.Destination);
                    Guard(item.Root, record.Stage);
                    Guard(item.Root, record.Backup);
                    AssertOwned(item.Destination);
                    FileAttributes attributes;
                    bool hadDestination = TryAttributes(item.Destination, out attributes);
                    if (hadDestination)
                    {
                        // Do not move a tree whose later cleanup would traverse links.
                        Inventory(item.Root, item.Destination, new List<string>(), new List<string>());
                    }
                    bool hasLegacy = InspectLegacy(record);
                    if (hasLegacy != record.LegacyPresent)
                        throw new IOException("准备安装后旧版目录的存在状态发生变化，请重新运行：" + record.Legacy);
                    if (hasLegacy)
                    {
                        Guard(item.Root, record.LegacyBackup);
                        RequireMissing(record.LegacyBackup);
                        Directory.Move(record.Legacy, record.LegacyBackup);
                        record.LegacyMoved = true;
                        Report(report, "已备份旧版插件，准备迁移：" + record.Legacy);
                    }
                    if (hadDestination)
                    {
                        RequireMissing(record.Backup);
                        Directory.Move(item.Destination, record.Backup);
                        record.OldMoved = true;
                    }
                    RequireMissing(item.Destination);
                    Directory.Move(record.Stage, item.Destination);
                    record.NewMoved = true;
                    Report(report, item.Name + " 已更新：" + item.Destination);
                }
            }
            catch (Exception original)
            {
                var failures = new List<string>();
                for (int i = records.Count - 1; i >= 0; i--)
                {
                    InstallRecord record = records[i];
                    try
                    {
                        if (record.NewMoved)
                        {
                            AssertOwned(record.Item.Destination);
                            Remove(record.Item.Root, record.Item.Destination);
                        }
                        if (record.OldMoved)
                        {
                            Guard(record.Item.Root, record.Backup);
                            Guard(record.Item.Root, record.Item.Destination);
                            RequireMissing(record.Item.Destination);
                            Directory.Move(record.Backup, record.Item.Destination);
                            Report(report, "已恢复原插件：" + record.Item.Destination);
                        }
                    }
                    catch (Exception rollbackError)
                    {
                        string message = "自动恢复失败，请保留并检查备份 " + record.Backup + "：" + rollbackError.Message;
                        failures.Add(message);
                        Report(report, message);
                    }
                    // Restore the former product independently, even if restoring
                    // the current product failed. Both backups remain recoverable.
                    if (record.LegacyMoved)
                    {
                        try
                        {
                            Guard(record.Item.Root, record.LegacyBackup);
                            Guard(record.Item.Root, record.Legacy);
                            AssertOwnedProduct(record.LegacyBackup, LegacyMarkerName, LegacyProduct);
                            RequireMissing(record.Legacy);
                            Directory.Move(record.LegacyBackup, record.Legacy);
                            Report(report, "已恢复旧版插件：" + record.Legacy);
                        }
                        catch (Exception rollbackError)
                        {
                            string message = "旧版插件恢复失败，请保留并检查备份 " + record.LegacyBackup + "：" + rollbackError.Message;
                            failures.Add(message);
                            Report(report, message);
                        }
                    }
                }
                string suffix = failures.Count == 0 ? " 已恢复此前已替换的目录。" : Environment.NewLine + String.Join(Environment.NewLine, failures);
                throw new IOException("插件安装失败：" + original.Message + suffix, original);
            }
            finally
            {
                foreach (InstallRecord record in records)
                {
                    if (!record.StageCreated) continue;
                    try { Remove(record.Item.Root, record.Stage); }
                    catch (Exception error) { Report(report, "临时目录未能清理，已保留 " + record.Stage + "：" + error.Message); }
                }
            }
            // Only discard old versions after the entire transaction succeeded.
            foreach (InstallRecord record in records)
            {
                if (record.OldMoved)
                {
                    try { AssertOwned(record.Backup); Remove(record.Item.Root, record.Backup); }
                    catch (Exception error) { Report(report, "旧插件备份未能清理，已保留 " + record.Backup + "：" + error.Message); }
                }
                if (record.LegacyMoved)
                {
                    try
                    {
                        AssertOwnedProduct(record.LegacyBackup, LegacyMarkerName, LegacyProduct);
                        Remove(record.Item.Root, record.LegacyBackup);
                    }
                    catch (Exception error) { Report(report, "旧名称插件备份未能清理，已保留 " + record.LegacyBackup + "：" + error.Message); }
                }
            }
        }

        private static bool InspectLegacy(InstallRecord record)
        {
            if (record.Legacy == null) return false;
            try
            {
                Guard(record.Item.Root, record.Legacy);
                FileAttributes attributes;
                if (!TryAttributes(record.Legacy, out attributes)) return false;
                AssertOwnedProduct(record.Legacy, LegacyMarkerName, LegacyProduct);
                Inventory(record.Item.Root, record.Legacy, new List<string>(), new List<string>());
                return true;
            }
            catch (Exception error)
            {
                throw new IOException("发现冲突的旧名称目录，不能确认其为可安全迁移的 " + LegacyProduct +
                    " 插件，已保留且未覆盖：" + record.Legacy + "。请检查旧版管理标记及目录内容。", error);
            }
        }

        private static void CopyTree(string source, string destination, string destinationRoot)
        {
            RequireDirectory(source);
            var files = new List<string>();
            var directories = new List<string>();
            string sourceParent = Parent(source);
            if (sourceParent == null) throw new IOException("不能将磁盘或共享根目录作为插件来源：" + source);
            Inventory(sourceParent, source, files, directories);
            foreach (string directory in directories)
            {
                Guard(sourceParent, directory);
                string relative = directory.Substring(source.Length).TrimStart(Path.DirectorySeparatorChar);
                EnsureDirectory(destinationRoot, relative.Length == 0 ? destination : Path.Combine(destination, relative));
            }
            foreach (string file in files)
            {
                Guard(sourceParent, file);
                string target = Path.Combine(destination, file.Substring(source.Length).TrimStart(Path.DirectorySeparatorChar));
                Guard(destinationRoot, target);
                RequireMissing(target);
                File.Copy(file, target, false);
            }
        }

        private static void Inventory(string root, string path, List<string> files, List<string> directories)
        {
            var pending = new Stack<string>();
            pending.Push(path);
            while (pending.Count != 0)
            {
                string current = pending.Pop();
                Guard(root, current);
                FileAttributes attributes;
                if (!TryAttributes(current, out attributes)) throw new IOException("操作期间路径已消失：" + current);
                if ((attributes & FileAttributes.Directory) == 0) { files.Add(current); continue; }
                directories.Add(current);
                // TopDirectoryOnly: inspect each child before any recursion.
                foreach (string child in Directory.EnumerateFileSystemEntries(current))
                {
                    Guard(root, child);
                    pending.Push(child);
                }
            }
        }

        private static void EnsureDirectory(string root, string path)
        {
            string fullRoot = Normalize(root);
            string full = Normalize(path);
            if (!SamePath(fullRoot, full)) Guard(fullRoot, full);
            else CheckChain(fullRoot);
            var missing = new Stack<string>();
            string current = full;
            FileAttributes attributes;
            while (!TryAttributes(current, out attributes))
            {
                missing.Push(current);
                current = Parent(current);
                if (current == null) throw new DirectoryNotFoundException(full);
            }
            RequireDirectory(current);
            while (missing.Count != 0)
            {
                current = missing.Pop();
                // First-time deployment may need to create Plugins/ApplicationPlugins
                // itself. Create the missing chain one level at a time, after checking
                // every existing ancestor; never resolve a write through a junction.
                if (IsChild(fullRoot, current)) Guard(fullRoot, current);
                else CheckChain(current);
                Directory.CreateDirectory(current);
                RequireDirectory(current);
            }
        }

        private static void RequireDirectory(string path)
        {
            CheckChain(path);
            FileAttributes attributes;
            if (!TryAttributes(path, out attributes) || (attributes & FileAttributes.Directory) == 0)
                throw new DirectoryNotFoundException("目录不存在或不是普通目录：" + path);
        }

        private static void RequireMissing(string path)
        {
            CheckChain(path);
            FileAttributes attributes;
            if (TryAttributes(path, out attributes)) throw new IOException("临时路径或安装目标已被占用：" + path);
        }

        private static void CheckChain(string path)
        {
            string current = Normalize(path);
            bool first = true;
            while (current != null)
            {
                FileAttributes attributes;
                if (TryAttributes(current, out attributes))
                {
                    if ((attributes & FileAttributes.ReparsePoint) != 0)
                        throw new IOException("拒绝操作链接路径（包含损坏链接）：" + current);
                    if (!first && (attributes & FileAttributes.Directory) == 0)
                        throw new IOException("路径的父级不是目录：" + current);
                }
                current = Parent(current);
                first = false;
            }
        }

        private static bool TryAttributes(string path, out FileAttributes attributes)
        {
            uint value = GetFileAttributes(path);
            if (value != InvalidAttributes) { attributes = (FileAttributes)value; return true; }
            int error = Marshal.GetLastWin32Error();
            attributes = 0;
            if (error == 2 || error == 3) return false;
            throw new IOException("无法检查路径属性：" + path + "。" + new Win32Exception(error).Message, new Win32Exception(error));
        }

        private static string Normalize(string path)
        {
            if (String.IsNullOrWhiteSpace(path)) throw new ArgumentException("路径不能为空。", "path");
            string full = Path.GetFullPath(path).Replace(Path.AltDirectorySeparatorChar, Path.DirectorySeparatorChar);
            if (full.StartsWith(@"\\?\", StringComparison.Ordinal) || full.StartsWith(@"\\.\", StringComparison.Ordinal))
                throw new IOException("不支持设备命名空间路径：" + path);
            for (int i = 0; i < full.Length; i++)
                if (full[i] == ':' && i != 1) throw new IOException("不支持备用数据流路径：" + path);
            string root = Path.GetPathRoot(full);
            return full.Length > root.Length ? full.TrimEnd(Path.DirectorySeparatorChar) : full;
        }

        private static string Parent(string path)
        {
            string parent = Path.GetDirectoryName(path);
            return String.IsNullOrEmpty(parent) || SamePath(parent, path) ? null : parent;
        }

        private static bool SamePath(string a, string b) { return String.Equals(a, b, PathComparison); }

        private static bool PathsOverlap(string a, string b) { return SamePath(a, b) || IsChild(a, b) || IsChild(b, a); }

        private static bool IsChild(string root, string path)
        {
            string prefix = root.EndsWith(Path.DirectorySeparatorChar.ToString(), StringComparison.Ordinal) ? root : root + Path.DirectorySeparatorChar;
            return !SamePath(root, path) && path.StartsWith(prefix, PathComparison);
        }

        private static void ClearReadOnly(string path, FileAttributes attributes)
        {
            if ((attributes & FileAttributes.ReadOnly) != 0) File.SetAttributes(path, attributes & ~FileAttributes.ReadOnly);
        }

        private static string CanonicalName(string name)
        {
            if (String.Equals(name, "UE", StringComparison.OrdinalIgnoreCase)) return "UE";
            if (String.Equals(name, "Max", StringComparison.OrdinalIgnoreCase)) return "Max";
            throw new ArgumentException("未知插件类型：" + name, "name");
        }

        private static string[] RequiredFiles(string name)
        {
            return CanonicalName(name) == "UE"
                ? new[] { Product + ".uplugin", @"Binaries\Win64\UnrealEditor-" + Product + ".dll", @"Binaries\Win64\UnrealEditor.modules" }
                : new[] { "PackageContents.xml", @"Contents\Win64\" + Product + ".gup" };
        }

        private static IOException NativeError(string message, string path)
        {
            var error = new Win32Exception(Marshal.GetLastWin32Error());
            return new IOException(message + "：" + path + "。" + error.Message, error);
        }

        private static void Report(Action<string> report, string message)
        {
            if (report == null) return;
            // UI disposal/logging errors must not interrupt a filesystem transaction.
            try { report(message); } catch { }
        }

        private sealed class InstallRecord
        {
            internal PackageItem Item;
            internal string Stage;
            internal string Backup;
            internal string Legacy;
            internal string LegacyBackup;
            internal bool StageCreated;
            internal bool OldMoved;
            internal bool NewMoved;
            internal bool LegacyPresent;
            internal bool LegacyMoved;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "GetFileAttributesW")]
        private static extern uint GetFileAttributes(string path);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "CreateDirectoryW")]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CreateDirectory(string path, IntPtr security);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "CreateFileW")]
        private static extern SafeFileHandle CreateFile(string path, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "GetFinalPathNameByHandleW")]
        private static extern uint GetFinalPathNameByHandle(SafeFileHandle file, StringBuilder path, uint capacity, uint flags);
    }
}
