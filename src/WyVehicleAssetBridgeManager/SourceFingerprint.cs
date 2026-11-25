using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace WyVehicleAssetBridge.Setup
{
    internal static class SourceFingerprint
    {
        internal static string Compute(string sourceDirectory)
        {
            if (String.IsNullOrWhiteSpace(sourceDirectory))
                throw new ArgumentException("插件源码目录不能为空。", "sourceDirectory");
            string root = Path.GetFullPath(sourceDirectory).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            string parent = Path.GetDirectoryName(root);
            if (String.IsNullOrEmpty(parent)) throw new IOException("插件源码目录不能是磁盘根目录：" + sourceDirectory);
            PackageStore.Guard(parent, root);
            if ((File.GetAttributes(root) & FileAttributes.Directory) == 0)
                throw new IOException("插件源码路径不是目录：" + root);

            var files = new List<string>();
            Inventory(root, "", files);
            files.Sort(StringComparer.Ordinal);
            using (var hash = SHA256.Create())
            {
                Append(hash, Encoding.UTF8.GetBytes("WyVehicleAssetBridge.SourceFingerprint/1\0"));
                AppendLength(hash, files.Count);
                var buffer = new byte[65536];
                foreach (string relative in files)
                {
                    string path = Path.Combine(root, relative.Replace('/', Path.DirectorySeparatorChar));
                    PackageStore.Guard(root, path);
                    if ((File.GetAttributes(path) & FileAttributes.Directory) != 0)
                        throw new IOException("计算源码指纹期间文件变成了目录：" + path);
                    byte[] name = Encoding.UTF8.GetBytes(relative);
                    AppendLength(hash, name.Length);
                    Append(hash, name);
                    // Deny concurrent writes/deletion while reading each file.
                    // Access failures propagate; no source file is silently omitted.
                    using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
                    {
                        long length = stream.Length;
                        AppendLength(hash, length);
                        long read = 0;
                        int count;
                        while ((count = stream.Read(buffer, 0, buffer.Length)) != 0)
                        {
                            hash.TransformBlock(buffer, 0, count, buffer, 0);
                            read += count;
                        }
                        if (read != length || stream.Length != length)
                            throw new IOException("计算源码指纹期间文件发生变化：" + path);
                    }
                }
                hash.TransformFinalBlock(new byte[0], 0, 0);
                return BitConverter.ToString(hash.Hash).Replace("-", "").ToLowerInvariant();
            }
        }

        private static void Inventory(string root, string relativeDirectory, List<string> files)
        {
            string directory = relativeDirectory.Length == 0 ? root :
                Path.Combine(root, relativeDirectory.Replace('/', Path.DirectorySeparatorChar));
            foreach (string entry in Directory.GetFileSystemEntries(directory))
            {
                PackageStore.Guard(root, entry);
                FileAttributes attributes = File.GetAttributes(entry);
                string relative = relativeDirectory.Length == 0 ? Path.GetFileName(entry) :
                    relativeDirectory + "/" + Path.GetFileName(entry);
                if ((attributes & FileAttributes.Directory) != 0)
                {
                    if (!IsGeneratedDirectory(relative)) Inventory(root, relative, files);
                }
                else files.Add(relative);
            }
        }

        private static bool IsGeneratedDirectory(string relative)
        {
            // Match actual output locations, never a name at arbitrary depth.
            // In particular Binaries/ThirdParty and Source/**/Intermediate remain
            // inputs, as does Config/FilterPlugin.ini used by Unreal packaging.
            return String.Equals(relative, "build", StringComparison.OrdinalIgnoreCase) ||
                String.Equals(relative, "Intermediate", StringComparison.OrdinalIgnoreCase) ||
                String.Equals(relative, "Saved", StringComparison.OrdinalIgnoreCase) ||
                String.Equals(relative, "DerivedDataCache", StringComparison.OrdinalIgnoreCase) ||
                String.Equals(relative, ".vs", StringComparison.OrdinalIgnoreCase) ||
                String.Equals(relative, "Binaries/Win64", StringComparison.OrdinalIgnoreCase);
        }

        private static void Append(HashAlgorithm hash, byte[] value)
        {
            hash.TransformBlock(value, 0, value.Length, value, 0);
        }

        private static void AppendLength(HashAlgorithm hash, long value)
        {
            var bytes = new byte[8];
            for (int i = 7; i >= 0; i--) { bytes[i] = (byte)value; value >>= 8; }
            Append(hash, bytes);
        }
    }
}
