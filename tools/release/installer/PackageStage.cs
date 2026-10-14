// BD-152: private package hydration. No driver install or certificate-store mutation.
// Win32 declarations: SDK 10.0.26100, fileapi.h, mscat.h and mssip.h.
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Principal;
using Microsoft.Win32.SafeHandles;

namespace AmdgpuWddm
{
    // Hold every ancestor against rename while the package is used. A fresh child
    // gets its protected ACL at creation; refuse reparses rather than resolving them.
    public sealed class PackageStage : IDisposable
    {
        readonly List<SafeFileHandle> parents = new List<SafeFileHandle>();
        readonly List<FileStream> files = new List<FileStream>();
        readonly List<string> owned = new List<string>();
        readonly List<string> subdirectories = new List<string>();
        readonly List<SafeFileHandle> subdirectoryLocks = new List<SafeFileHandle>();
        SafeFileHandle directory;
        bool sealedFiles, created;
        public string DirectoryPath { get; private set; }

        [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern SafeFileHandle CreateFileW(string path, uint access, uint share,
            IntPtr security, uint creation, uint flags, IntPtr template);
        [StructLayout(LayoutKind.Sequential)]
        struct SecurityAttributes { public uint Length; public IntPtr Descriptor; public int Inherit; }
        [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool CreateDirectoryW(string path, ref SecurityAttributes security);
        [StructLayout(LayoutKind.Sequential)]
        struct AttributeTag { public uint Attributes, ReparseTag; }
        [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool GetFileInformationByHandleEx(SafeFileHandle file, int cls,
            out AttributeTag result, uint size);

        static SafeFileHandle LockDirectory(string path)
        {
            // FILE_READ_ATTRIBUTES|READ_CONTROL; FILE_SHARE_READ|FILE_SHARE_WRITE,
            // no FILE_SHARE_DELETE; OPEN_EXISTING; BACKUP_SEMANTICS|OPEN_REPARSE_POINT.
            var h = CreateFileW(path, 0x20080, 3, IntPtr.Zero, 3, 0x02200000, IntPtr.Zero);
            if (h.IsInvalid) { int error = Marshal.GetLastWin32Error(); h.Dispose(); throw new Win32Exception(error); }
            try {
                AttributeTag tag;
                if (!GetFileInformationByHandleEx(h, 9, out tag, 8)) throw new Win32Exception(Marshal.GetLastWin32Error());
                if ((tag.Attributes & 0x410) != 0x10) throw new IOException("Package path is not a plain directory");
                return h;
            } catch { h.Dispose(); throw; }
        }

        public PackageStage(string parent)
        {
            try {
                string full = Path.GetFullPath(parent).TrimEnd('\\');
                if (full.Length < 3 || full[1] != ':' || full[2] != '\\' || full.IndexOf(':', 2) >= 0)
                    throw new IOException("Package staging needs a local absolute path");
                string at = full.Substring(0, 3);
                parents.Add(LockDirectory(at));
                foreach (string part in full.Substring(3).Split('\\')) {
                    if (part.Length == 0) throw new IOException("Empty package path component");
                    at = Path.Combine(at, part);
                    parents.Add(LockDirectory(at));
                }
                DirectoryPath = Path.Combine(full, "driver-package-" + Guid.NewGuid().ToString("N"));
                var acl = new DirectorySecurity();
                acl.SetSecurityDescriptorSddlForm("O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)");
                byte[] descriptor = acl.GetSecurityDescriptorBinaryForm();
                var pinned = GCHandle.Alloc(descriptor, GCHandleType.Pinned);
                try {
                    var sa = new SecurityAttributes { Length = (uint)Marshal.SizeOf(typeof(SecurityAttributes)), Descriptor = pinned.AddrOfPinnedObject() };
                    // CreateDirectory, unlike DirectoryInfo.Create, refuses an
                    // existing name. Never take ownership of someone else's child.
                    if (!CreateDirectoryW(DirectoryPath, ref sa)) throw new Win32Exception(Marshal.GetLastWin32Error());
                    created = true;
                } finally { pinned.Free(); }
                directory = LockDirectory(DirectoryPath);
                // The directory is held against rename, so this path query names
                // the same opened object. No inherited or additional ACE is accepted.
                var actual = new DirectoryInfo(DirectoryPath).GetAccessControl(AccessControlSections.Owner | AccessControlSections.Access);
                if (!actual.AreAccessRulesProtected || actual.GetOwner(typeof(SecurityIdentifier)).Value != "S-1-5-32-544")
                    throw new UnauthorizedAccessException("Package directory owner or inheritance differs");
                var rules = actual.GetAccessRules(true, true, typeof(SecurityIdentifier));
                bool system = false, admin = false;
                foreach (FileSystemAccessRule rule in rules) {
                    string sid = rule.IdentityReference.Value;
                    if (rule.AccessControlType != AccessControlType.Allow || rule.IsInherited ||
                        rule.FileSystemRights != FileSystemRights.FullControl ||
                        rule.InheritanceFlags != (InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit) ||
                        rule.PropagationFlags != PropagationFlags.None || (sid != "S-1-5-18" && sid != "S-1-5-32-544"))
                        throw new UnauthorizedAccessException("Unexpected package directory access");
                    system |= sid == "S-1-5-18"; admin |= sid == "S-1-5-32-544";
                }
                if (rules.Count != 2 || !system || !admin) throw new UnauthorizedAccessException("Incomplete package directory access");
            } catch { Dispose(); throw; }
        }

        public static string MemberPath(string member)
        {
            // Canonical package-relative paths only. Reject ADS, traversal,
            // drive/UNC paths, Win32 aliases, trailing dots and empty segments.
            if (String.IsNullOrEmpty(member) || member.Length > 240)
                throw new IOException("Invalid package member path");
            string normal = member.Replace('/', '\\');
            foreach (string part in normal.Split('\\')) {
                if (!System.Text.RegularExpressions.Regex.IsMatch(part, @"\A[A-Za-z0-9_-]+(?:\.[A-Za-z0-9_-]+)*\z") ||
                    System.Text.RegularExpressions.Regex.IsMatch(part, @"\A(?:CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(?:\.|\z)",
                        System.Text.RegularExpressions.RegexOptions.IgnoreCase))
                    throw new IOException("Invalid package member path");
            }
            return normal;
        }

        void AddMemberDirectories(string member)
        {
            string relative = Path.GetDirectoryName(member);
            if (String.IsNullOrEmpty(relative)) return;
            string at = DirectoryPath;
            foreach (string part in relative.Split('\\')) {
                at = Path.Combine(at, part);
                if (subdirectories.Contains(at)) continue;
                var acl = new DirectorySecurity();
                acl.SetSecurityDescriptorSddlForm("O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)");
                byte[] descriptor = acl.GetSecurityDescriptorBinaryForm();
                var pinned = GCHandle.Alloc(descriptor, GCHandleType.Pinned);
                try {
                    var sa = new SecurityAttributes { Length = (uint)Marshal.SizeOf(typeof(SecurityAttributes)), Descriptor = pinned.AddrOfPinnedObject() };
                    if (!CreateDirectoryW(at, ref sa)) throw new Win32Exception(Marshal.GetLastWin32Error());
                    subdirectories.Add(at);
                    subdirectoryLocks.Add(LockDirectory(at));
                } finally { pinned.Free(); }
            }
        }

        public void Copy(string source, string member, long length)
        {
            if (directory == null || sealedFiles) throw new InvalidOperationException("Package is closed or sealed");
            string relative = MemberPath(member);
            AddMemberDirectories(relative);
            string target = Path.Combine(DirectoryPath, relative);
            using (var src = new FileStream(source, FileMode.Open, FileAccess.Read, FileShare.Read)) {
                if (length <= 0 || src.Length != length) throw new IOException("Package member length differs from manifest");
                using (var dst = new FileStream(target, FileMode.CreateNew, FileAccess.Write, FileShare.None)) {
                    owned.Add(target);
                    src.CopyTo(dst); dst.Flush(true);
                    if (dst.Length != length) throw new IOException("Package member length changed during copy");
                }
            }
        }

        public void Seal()
        {
            if (directory == null || sealedFiles) throw new InvalidOperationException("Package is closed or sealed");
            foreach (string path in owned) {
                if ((File.GetAttributes(path) & (FileAttributes.ReparsePoint | FileAttributes.Directory)) != 0)
                    throw new IOException("Package member is not a plain file");
                files.Add(new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read));
            }
            sealedFiles = true;
        }

        public void Dispose()
        {
            foreach (var f in files) f.Dispose(); files.Clear();
            // Delete only files this object created; never recurse through an
            // unexpected child. Parent handles still prevent path replacement.
            if (DirectoryPath != null && created) {
                try {
                    if ((File.GetAttributes(DirectoryPath) & FileAttributes.ReparsePoint) == 0) {
                        foreach (string path in owned) File.Delete(path);
                        for (int i = subdirectories.Count - 1; i >= 0; --i) {
                            if (i < subdirectoryLocks.Count) subdirectoryLocks[i].Dispose();
                            Directory.Delete(subdirectories[i], false);
                        }
                        if (directory != null) { directory.Dispose(); directory = null; }
                        Directory.Delete(DirectoryPath, false);
                    }
                } catch (IOException) { } catch (UnauthorizedAccessException) { }
                DirectoryPath = null;
                created = false;
            }
            foreach (var h in subdirectoryLocks) h.Dispose(); subdirectoryLocks.Clear(); subdirectories.Clear();
            if (directory != null) { directory.Dispose(); directory = null; }
            for (int i = parents.Count - 1; i >= 0; --i) parents[i].Dispose(); parents.Clear();
        }
    }

    public static class CatalogMembership
    {
        [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
        [DllImport("wintrust.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr CryptCATOpen(string path, uint flags, IntPtr provider, uint version, uint encoding);
        [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
        [DllImport("wintrust.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)] static extern bool CryptCATClose(IntPtr catalog);
        [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
        [DllImport("wintrust.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr CryptCATGetMemberInfo(IntPtr catalog, string tag);
        [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
        [DllImport("wintrust.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool CryptCATAdminAcquireContext2(out IntPtr context, IntPtr subsystem, string algorithm, IntPtr policy, uint flags);
        [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
        [DllImport("wintrust.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)] static extern bool CryptCATAdminReleaseContext(IntPtr context, uint flags);
        [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
        [DllImport("wintrust.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool CryptCATAdminCalcHashFromFileHandle2(IntPtr context, SafeFileHandle file, ref uint length, [Out] byte[] hash, uint flags);
        [StructLayout(LayoutKind.Sequential)] struct Blob { public uint Length; public IntPtr Bytes; }
        [StructLayout(LayoutKind.Sequential)] struct Attribute { public IntPtr Oid; public Blob Value; }
        [StructLayout(LayoutKind.Sequential)] struct Indirect { public Attribute Data, Algorithm; public Blob Digest; }
        [StructLayout(LayoutKind.Sequential)] struct Member {
            public uint Size; public IntPtr ReferenceTag, FileName; public Guid Subject;
            public uint Flags; public IntPtr IndirectData; public uint Version, Reserved;
            public IntPtr Internal; public Blob EncodedIndirectData, EncodedMemberInfo;
        }

        // Membership of these exact bytes, separately from certificate trust.
        // SHA256 is the only admitted catalog digest for newly built packages.
        public static bool Contains(string catalogPath, string filePath)
        {
            IntPtr context = IntPtr.Zero, catalog = IntPtr.Zero;
            try {
                if (!CryptCATAdminAcquireContext2(out context, IntPtr.Zero, "SHA256", IntPtr.Zero, 0))
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                byte[] hash = new byte[32]; uint length = 32;
                using (var f = new FileStream(filePath, FileMode.Open, FileAccess.Read, FileShare.Read)) {
                    if (!CryptCATAdminCalcHashFromFileHandle2(context, f.SafeFileHandle, ref length, hash, 0) || length != 32)
                        throw new IOException("Catalog SHA256 calculation failed");
                }
                catalog = CryptCATOpen(catalogPath, 0, IntPtr.Zero, 0, 0);
                if (catalog == IntPtr.Zero || catalog == new IntPtr(-1)) throw new Win32Exception(Marshal.GetLastWin32Error());
                IntPtr p = CryptCATGetMemberInfo(catalog, BitConverter.ToString(hash).Replace("-", ""));
                if (p == IntPtr.Zero) return false;
                var member = (Member)Marshal.PtrToStructure(p, typeof(Member));
                if (member.IndirectData == IntPtr.Zero) return false;
                var data = (Indirect)Marshal.PtrToStructure(member.IndirectData, typeof(Indirect));
                if (Marshal.PtrToStringAnsi(data.Algorithm.Oid) != "2.16.840.1.101.3.4.2.1" || data.Digest.Length != 32 || data.Digest.Bytes == IntPtr.Zero)
                    return false;
                var digest = new byte[32]; Marshal.Copy(data.Digest.Bytes, digest, 0, 32);
                for (int i = 0; i < 32; i++) if (digest[i] != hash[i]) return false;
                return true;
            } finally {
                if (catalog != IntPtr.Zero && catalog != new IntPtr(-1)) CryptCATClose(catalog);
                if (context != IntPtr.Zero) CryptCATAdminReleaseContext(context, 0);
            }
        }
    }
}
