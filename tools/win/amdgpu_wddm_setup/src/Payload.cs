// The single-file installer (owner, 2026-10-12: "dlaczego setup.exe nie zawiera zipa w sobie? to lepsza metoda!").
//
// One file, amdgpu-wddm-setup-<version>.exe, carries the whole release: this program, then the package as a zip
// section, then a list of every file with its SHA-256, then a 120-byte trailer that says where the two sections are
// and what they hash to. The sections come after the end of the program image, so Windows loads the exe exactly as
// it loads the plain one and the appended bytes are never executed. tools\release\make-setup-exe.ps1 writes it, and
// the release recipe proves that the files inside it are the files of the zip, byte for byte.
//
// What happens when it is started:
//   1. a setup.exe that has its files\ folder beside it (the unpacked zip, a prepared folder, a repair set) carries
//      no payload and nothing here runs: that layout is unchanged;
//   2. otherwise the payload is unpacked to %ProgramData%\amdgpu-wddm\setup\<version>\, a folder only
//      administrators and SYSTEM may write (Access): the files run elevated after the restarts of the install, so a
//      standard user must not be able to plant or replace one of them. The parent folder carries that permission
//      list, protected from inheritance, so a standard user cannot even create the version folder inside it;
//   3. every unpacked file is checked against the list before anything uses it, and a file that does not match ends
//      the run with a message instead of installing an unknown file;
//   4. the folder is removed when the run ends (Cleanup), and kept only while a restart of this install is pending.
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;

namespace AmdgpuWddmSetup
{
    public static class Payload
    {
        // "AMDGPU-WDDM-PKG" and the format number, 16 bytes.
        public static readonly byte[] Magic = Encoding.ASCII.GetBytes("AMDGPU-WDDM-PKG1");
        public const int TrailerSize = 120;     // magic 16, version 4, reserved 4, four offsets 32, two hashes 64
        public const int Format = 1;

        // Where the window unpacks a single-file setup: one folder per release version.
        public static string Root { get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), @"amdgpu-wddm\setup"); } }

        // One unpacking of a single file at a time, for the whole computer. The folder of a version is reset before
        // it is written, so a second copy of the same file started while the first one installs would delete the
        // files the first one is installing from: the second window says setup is already running and ends instead.
        // Global\, not Local\: the two runs can be in different sessions.
        public const string RunMutexName = @"Global\amdgpu-wddm-setup-payload";

        // The folder of the package inside the unpacked release, beside setup.exe. SetupForm uses the same name.
        public const string FilesDir = "files";

        // Whether this run installs from the package inside the file. Three answers decide it:
        //   present:          the file carries a payload at all;
        //   explicitPackage:  --package, a folder the person named themselves (null when they named none);
        //   packageBesideExe: a files\ folder of a release stands beside the exe (the unpacked zip, a prepared
        //                     offline folder, a repair set).
        // A layout the person prepared wins over the payload, in both of its forms: that is the layout they asked
        // for, and it "behaves as today" (owner, 2026-10-12). --package is honoured even when the folder holds no
        // release, so that the window says what is wrong with the folder the person named instead of quietly
        // installing another one.
        public static bool Carries(bool present, string explicitPackage, bool packageBesideExe)
        {
            return present && explicitPackage == null && !packageBesideExe;
        }

        // A folder that holds a release's files: the manifest and the installer the window starts.
        public static bool HasPackage(string dir)
        {
            if (string.IsNullOrEmpty(dir)) return false;
            try { return File.Exists(Path.Combine(dir, "manifest.json")) && File.Exists(Path.Combine(dir, "installer", "install.ps1")); }
            catch (Exception) { return false; }
        }

        // The folder this file unpacks into cannot be made safe on this computer (Access.AssertLocation). It is not
        // a damaged download, so the window says something else about it.
        public sealed class FolderException : Exception
        {
            public FolderException(string message) : base(message) { }
        }

        public sealed class Entry
        {
            public string Path;                 // relative to the folder, with \ as the separator
            public string Sha256;               // upper-case hex
            public long Size;
        }

        public sealed class Head
        {
            public long PayloadOffset, PayloadLength, ListOffset, ListLength;
            public string PayloadSha256, ListSha256;
        }

        static string OwnFile { get { return Assembly.GetExecutingAssembly().Location; } }

        // Whether this exe carries a package. Only the trailer is read, so the answer costs nothing.
        public static bool Present() { return ReadHead(OwnFile) != null; }

        // The trailer of a file, or null when it has none (a plain setup.exe, or a file too short to hold one).
        public static Head ReadHead(string file)
        {
            try
            {
                using (var f = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.Read))
                {
                    if (f.Length < TrailerSize + 2) return null;
                    f.Seek(-TrailerSize, SeekOrigin.End);
                    var t = new byte[TrailerSize];
                    if (Read(f, t, t.Length) != t.Length) return null;
                    for (int i = 0; i < Magic.Length; i++) if (t[i] != Magic[i]) return null;
                    if (BitConverter.ToInt32(t, 16) != Format) return null;
                    var h = new Head
                    {
                        PayloadOffset = BitConverter.ToInt64(t, 24),
                        PayloadLength = BitConverter.ToInt64(t, 32),
                        ListOffset = BitConverter.ToInt64(t, 40),
                        ListLength = BitConverter.ToInt64(t, 48),
                        PayloadSha256 = Hex(t, 56, 32),
                        ListSha256 = Hex(t, 88, 32),
                    };
                    long end = f.Length - TrailerSize;
                    // Every bound is checked before a single byte is read: a trailer is data in a file, and a file
                    // can be damaged or doctored.
                    if (h.PayloadLength <= 0 || h.ListLength <= 0) return null;
                    if (h.PayloadOffset < 0 || h.PayloadOffset + h.PayloadLength > end) return null;
                    if (h.ListOffset < h.PayloadOffset + h.PayloadLength || h.ListOffset + h.ListLength > end) return null;
                    return h;
                }
            }
            catch (Exception) { return null; }
        }

        // The list of files, as make-setup-exe.ps1 writes it:
        //   version<tab><release version>
        //   file<tab><sha256><tab><size><tab><relative path>
        // Returns the entries and, in version, the release the payload holds. A line this reader does not know is a
        // problem, not something to skip: the list decides what is installed.
        public static List<Entry> ParseList(string text, out string version)
        {
            version = null;
            var list = new List<Entry>();
            foreach (var raw in text.Replace("\r\n", "\n").Split('\n'))
            {
                var line = raw.Trim();
                if (line.Length == 0) continue;
                var p = line.Split('\t');
                if (p[0] == "version" && p.Length == 2) { version = p[1]; continue; }
                if (p[0] == "file" && p.Length == 4)
                {
                    long size;
                    if (!long.TryParse(p[2], System.Globalization.NumberStyles.Integer, System.Globalization.CultureInfo.InvariantCulture, out size) || size < 0)
                        throw new InvalidDataException("the file list has a size that is not a number: " + line);
                    if (p[1].Length != 64) throw new InvalidDataException("the file list has a hash of the wrong length: " + line);
                    var path = p[3].Replace('/', '\\');
                    if (!SafeRelative(path)) throw new InvalidDataException("the file list names a path outside the folder: " + line);
                    list.Add(new Entry { Path = path, Sha256 = p[1].ToUpperInvariant(), Size = size });
                    continue;
                }
                throw new InvalidDataException("the file list has a line this version does not know: " + line);
            }
            if (version == null) throw new InvalidDataException("the file list names no version");
            if (list.Count == 0) throw new InvalidDataException("the file list is empty");
            return list;
        }

        // A relative path that cannot leave its folder: no root, no drive, no "..", no leading separator. A zip
        // entry's name comes from the file the packer read, and a path that escapes the folder would write anywhere.
        public static bool SafeRelative(string path)
        {
            if (string.IsNullOrEmpty(path)) return false;
            if (path.IndexOf(':') >= 0 || path.StartsWith("\\")) return false;
            foreach (var part in path.Split('\\'))
            {
                if (part.Length == 0 || part == "." || part == "..") return false;
                if (part.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0) return false;
            }
            return true;
        }

        // Unpacks this exe's payload and returns the package root inside it (the files\ folder), ready for the
        // engine. Throws with a sentence a person can read when anything does not match.
        public static string Extract() { return Extract(Root, true); }

        // root: the folder that holds the version folders. protect: give it and the version folder the permission
        // list of Access (an install always does; --smoke-payload unpacks into a plain folder of its own instead,
        // so that the reader can be tested without administrator rights).
        public static string Extract(string root, bool protect)
        {
            var head = ReadHead(OwnFile);
            if (head == null) throw new InvalidDataException("this setup program carries no package");
            string listText, version;
            using (var f = new FileStream(OwnFile, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                var payloadSha = Section(f, head.PayloadOffset, head.PayloadLength, null);
                if (payloadSha != head.PayloadSha256) throw new InvalidDataException("the package inside this setup program is damaged");
                var listBytes = new MemoryStream();
                var listSha = Section(f, head.ListOffset, head.ListLength, listBytes);
                if (listSha != head.ListSha256) throw new InvalidDataException("the file list inside this setup program is damaged");
                listText = new UTF8Encoding(false).GetString(listBytes.ToArray());
            }
            var entries = ParseList(listText, out version);
            if (!SafeRelative(version)) throw new InvalidDataException("the package inside this setup program names no usable version");
            var dir = Path.Combine(root, version);
            if (protect) { Access.PrepareRoot(root); Access.ResetFolder(dir); }
            else
            {
                Remove(dir);
                Directory.CreateDirectory(dir);
            }
            using (var f = new FileStream(OwnFile, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                f.Seek(head.PayloadOffset, SeekOrigin.Begin);
                using (var section = new Window(f, head.PayloadLength))
                using (var zip = new ZipArchive(section, ZipArchiveMode.Read))
                {
                    foreach (var e in zip.Entries)
                    {
                        if (e.FullName.EndsWith("/")) continue;
                        var rel = e.FullName.Replace('/', '\\');
                        if (!SafeRelative(rel)) throw new InvalidDataException("the package holds a path outside the folder: " + e.FullName);
                        var target = Path.Combine(dir, rel);
                        Directory.CreateDirectory(Path.GetDirectoryName(target));
                        e.ExtractToFile(target, true);
                    }
                }
            }
            // Every file against the list, after it is on the disk and before anything uses it.
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var e in entries)
            {
                var path = Path.Combine(dir, e.Path);
                var info = new FileInfo(path);
                if (!info.Exists) throw new InvalidDataException("the package is missing " + e.Path);
                if (info.Length != e.Size) throw new InvalidDataException("the unpacked " + e.Path + " has the wrong size");
                if (FileSha256(path) != e.Sha256) throw new InvalidDataException("the unpacked " + e.Path + " is not the file this setup was built with");
                seen.Add(e.Path);
            }
            foreach (var path in Directory.GetFiles(dir, "*", SearchOption.AllDirectories))
            {
                var rel = path.Substring(dir.Length).TrimStart('\\');
                if (!seen.Contains(rel)) throw new InvalidDataException("the package holds a file the list does not name: " + rel);
            }
            var package = Path.Combine(dir, FilesDir);
            if (!HasPackage(package)) throw new InvalidDataException("the package inside this setup program is not complete");
            return package;
        }

        // Removes the folder a single-file setup unpacked itself into. Called when the run is over; while a restart
        // of this install is pending the folder stays, because the install is not over. Never throws: a folder that
        // cannot be removed is not worth ending a finished installation for, and the next run resets it anyway.
        public static void Cleanup(string packageRoot) { Cleanup(packageRoot, Root); }

        // The same with the root named, which is what the tests use: the real root is %ProgramData%, which a test
        // must not touch.
        public static void Cleanup(string packageRoot, string root)
        {
            try
            {
                if (string.IsNullOrEmpty(packageRoot)) return;
                var dir = Path.GetDirectoryName(packageRoot.TrimEnd('\\'));
                if (string.IsNullOrEmpty(dir)) return;
                var parent = Path.GetDirectoryName(dir.TrimEnd('\\'));
                if (parent == null || !parent.TrimEnd('\\').Equals(Root.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase)) return;
                Remove(dir);
            }
            catch (Exception) { }
        }

        // The same removal from the release version alone, for a run that did not unpack the folder itself. Every
        // install of a single file is finished by a later phase, after a restart, and that phase runs from the
        // continuation closure: the window of the phase that unpacked the folder is long gone, so the window that
        // ends the install removes the folder of the version it installed. Without this the whole unpacked release
        // stayed in %ProgramData% for good, which is the one path every fresh install takes.
        //
        // A version that is not a plain name, or a folder that is not inside Root, is left alone: this is given a
                RemoveRootIfEmpty(root);
        // version read from a manifest on the disk, and a name like ..\.. must not reach Directory.Delete.
        public static void CleanupVersion(string version) { CleanupVersion(version, Root); }

        // The same with the root named, for the tests.
        public static void CleanupVersion(string version, string root)
        {
            try
            {
                if (string.IsNullOrEmpty(version) || !SafeRelative(version) || version.IndexOf('\\') >= 0) return;
                var dir = Path.Combine(Root, version);
                var parent = Path.GetDirectoryName(dir.TrimEnd('\\'));
                if (parent == null || !parent.TrimEnd('\\').Equals(Root.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase)) return;
                Remove(dir);
                RemoveRootIfEmpty(root);
            }
            catch (Exception) { }
        }

        // The folder that held the versions goes with the last of them, so an uninstall and every finished install
        // leave nothing of the single file behind. A folder that still holds another version stays.
        //
        // BD-163: this ran in CleanupVersion alone, and the install branch above (Cleanup, the window that unpacked
        // the folder) never reached it, so every install from a single file left the empty folder in %ProgramData%.
        // The second half of that defect is the permission list: the run that creates the folder is now on it
        // (Access.RootRules), because Access.Rules alone left a folder that only an administrator could remove, and
        // a run without administrator rights could not remove the folder it had made itself. A run that still may
        // not remove the folder leaves it, as before: a finished installation is not worth ending over it.
        static void RemoveRootIfEmpty(string root)
        {
            try
            {
                if (!Directory.Exists(root)) return;
                if (Directory.GetFileSystemEntries(root).Length != 0) return;
                Directory.Delete(root, false);
            }
            catch (Exception) { }
        }

        // Removes one directory, and never through a link: a junction or a symbolic link is unlinked, not followed,
        // so a link somebody put where our folder belongs cannot turn a cleanup into a removal somewhere else.
        static void Remove(string dir)
        {
            if (!Directory.Exists(dir)) return;
            if ((File.GetAttributes(dir) & FileAttributes.ReparsePoint) != 0) { Directory.Delete(dir, false); return; }
            Directory.Delete(dir, true);
        }

        // ---- bytes -------------------------------------------------------------------------------------------------

        static int Read(Stream s, byte[] buffer, int count)
        {
            int got = 0;
            while (got < count)
            {
                int n = s.Read(buffer, got, count - got);
                if (n <= 0) break;
                got += n;
            }
            return got;
        }

        // The SHA-256 of a section of the file, copied into sink when one is given.
        static string Section(FileStream f, long offset, long length, Stream sink)
        {
            f.Seek(offset, SeekOrigin.Begin);
            var buffer = new byte[64 * 1024];
            long left = length;
            using (var sha = SHA256.Create())
            {
                while (left > 0)
                {
                    int want = (int)Math.Min(buffer.Length, left);
                    int got = Read(f, buffer, want);
                    if (got != want) throw new InvalidDataException("this setup program ends before its package does");
                    sha.TransformBlock(buffer, 0, got, null, 0);
                    if (sink != null) sink.Write(buffer, 0, got);
                    left -= got;
                }
                sha.TransformFinalBlock(new byte[0], 0, 0);
                return Hex(sha.Hash, 0, sha.Hash.Length);
            }
        }

        public static string FileSha256(string path)
        {
            using (var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
            using (var sha = SHA256.Create())
                return Hex(sha.ComputeHash(f), 0, 32);
        }

        public static string Hex(byte[] b, int offset, int count)
        {
            var sb = new StringBuilder(count * 2);
            for (int i = 0; i < count; i++) sb.Append(b[offset + i].ToString("X2", System.Globalization.CultureInfo.InvariantCulture));
            return sb.ToString();
        }

        // A read-only view of one section of a file, so that ZipArchive sees the zip alone and not the program
        // image before it. ZipArchive reads the central directory from the end, so the length has to be the
        // section's and not the file's.
        sealed class Window : Stream
        {
            readonly Stream _inner;
            readonly long _start, _length;
            long _position;

            public Window(Stream inner, long length)
            {
                _inner = inner;
                _start = inner.Position;
                _length = length;
            }

            public override bool CanRead { get { return true; } }
            public override bool CanSeek { get { return true; } }
            public override bool CanWrite { get { return false; } }
            public override long Length { get { return _length; } }
            public override long Position { get { return _position; } set { Seek(value, SeekOrigin.Begin); } }
            public override void Flush() { }
            public override void SetLength(long value) { throw new NotSupportedException(); }
            public override void Write(byte[] buffer, int offset, int count) { throw new NotSupportedException(); }

            public override int Read(byte[] buffer, int offset, int count)
            {
                if (_position >= _length) return 0;
                int want = (int)Math.Min(count, _length - _position);
                _inner.Position = _start + _position;
                int got = _inner.Read(buffer, offset, want);
                if (got > 0) _position += got;
                return got;
            }

            public override long Seek(long offset, SeekOrigin origin)
            {
                long want = origin == SeekOrigin.Begin ? offset : origin == SeekOrigin.Current ? _position + offset : _length + offset;
                if (want < 0 || want > _length) throw new IOException("a read outside the package section");
                _position = want;
                _inner.Position = _start + _position;
                return _position;
            }
        }

        // ---- the permission list of the unpacked folder --------------------------------------------------------------
        public static class Access
        {
            // Administrators and SYSTEM may do anything, Users may read and run, nobody else is on the list, and the
            // list is not inherited from %ProgramData% - where a standard user may create folders, which is what the
            // protection here is about. The owner is Administrators where the process may set it.
            public static DirectorySecurity Rules()
            {
                var admins = new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null);
                var system = new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null);
                var users = new SecurityIdentifier(WellKnownSidType.BuiltinUsersSid, null);
                var s = new DirectorySecurity();
                s.SetAccessRuleProtection(true, false);
                s.AddAccessRule(new FileSystemAccessRule(admins, FileSystemRights.FullControl,
                    InheritanceFlags.ObjectInherit | InheritanceFlags.ContainerInherit, PropagationFlags.None, AccessControlType.Allow));
                s.AddAccessRule(new FileSystemAccessRule(system, FileSystemRights.FullControl,
                    InheritanceFlags.ObjectInherit | InheritanceFlags.ContainerInherit, PropagationFlags.None, AccessControlType.Allow));
                s.AddAccessRule(new FileSystemAccessRule(users, FileSystemRights.ReadAndExecute,
                    InheritanceFlags.ObjectInherit | InheritanceFlags.ContainerInherit, PropagationFlags.None, AccessControlType.Allow));
                return s;
            }

            // The accounts a folder on the way to the unpacked package may belong to: SYSTEM, the Administrators
            // group (which owns everything an elevated process creates), TrustedInstaller, and the account running
            // this setup, which is an administrator because setup runs elevated. Anybody else owning one of these
            // folders is the finding: an owner may rename a folder aside, so a standard user who owns the parent
            // could put their own directory where the checked files stand, between the check and their use.
            public static string[] TrustedOwners()
            {
                var list = new List<string>
                {
                    new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null).Value,
                    new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null).Value,
                    "S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464",   // NT SERVICE\TrustedInstaller
                };
                try { var me = WindowsIdentity.GetCurrent().User; if (me != null) list.Add(me.Value); }
                catch (Exception) { }
                return list.ToArray();
            }

            // One folder on the way to the unpacked package, checked as a location: a regular directory, no link,
            // and an owner of the list above. The sentence it throws names the folder, so a person can remove it.
            //
            // The access rules of %ProgramData%\amdgpu-wddm itself are not touched. That folder is shared: the
            // installer state, the start-confirm record, the DWM baseline and the control application's own folder
            // all live in it, and a strict permission list on it refused every uninstall of 0.7.216.100-tester.28
            // (BD-129; tools/release/installer/system-tuning.ps1 keeps the same stance for the same folder). The
            // folders of the package below it carry the permission list instead.
            public static void AssertLocation(string dir)
            {
                var info = new DirectoryInfo(dir);
                if (!info.Exists || (info.Attributes & FileAttributes.ReparsePoint) != 0)
                    throw new FolderException("the setup folder " + dir + " is not a regular folder; remove it and start this file again");
                string owner;
                try { owner = info.GetAccessControl().GetOwner(typeof(SecurityIdentifier)).Value; }
                catch (Exception e) { throw new FolderException("the setup folder " + dir + " cannot be read: " + e.Message); }
                foreach (var trusted in TrustedOwners()) if (owner == trusted) return;
                throw new FolderException("the setup folder " + dir + " belongs to another account (" + owner
                    + "), which could replace the files of this installation; remove it and start this file again,"
                    + " or install from the unpacked zip");
            }

            // The folder that holds the version folders. It gets the permission list above whether it is new or was
            // already there: a folder a standard user created before us stops being writable the moment this runs,
            // and the version folder inside it therefore cannot be planted either. Its parent, the shared
            // %ProgramData%\amdgpu-wddm, is created where it is missing and checked as a location (AssertLocation):
            // a folder owned by another account can be renamed aside by that account, whatever the rules of the
            // folders inside it say.
            public static void PrepareRoot(string root)
            {
                var shared = Path.GetDirectoryName(root.TrimEnd('\\'));
                if (!Directory.Exists(shared))
                {
                    Directory.CreateDirectory(shared);
                    SetOwnerToAdministrators(new DirectoryInfo(shared));
                }
                AssertLocation(shared);
                var info = new DirectoryInfo(root);
            // The rules of the folder that holds the version folders: the list above, and the account that creates
            // the folder, so that the run which made it can remove it again (BD-163). That access rule is not
            // inherited: the version folders and the files below them stay administrators and SYSTEM only, with
            // read and run for Users, so nobody can plant a file where the checked package goes. The owner is
            // Administrators where the process may set it, as before.
            public static DirectorySecurity RootRules()
            {
                var s = Rules();
                try
                {
                    var me = WindowsIdentity.GetCurrent().User;
                    if (me != null)
                        s.AddAccessRule(new FileSystemAccessRule(me, FileSystemRights.FullControl,
                            InheritanceFlags.None, PropagationFlags.None, AccessControlType.Allow));
                }
                catch (Exception) { }     // a run that cannot read its own account keeps the list above
                return s;
            }

            // The rules of the folder that holds the version folders: the list above, and the account that creates
            // the folder, so that the run which made it can remove it again (BD-163). That access rule is not
            // inherited: the version folders and the files below them stay administrators and SYSTEM only, with
            // read and run for Users, so nobody can plant a file where the checked package goes. The owner is
            // Administrators where the process may set it, as before.
            public static DirectorySecurity RootRules()
            {
                var s = Rules();
                try
                {
                    var me = WindowsIdentity.GetCurrent().User;
                    if (me != null)
                        s.AddAccessRule(new FileSystemAccessRule(me, FileSystemRights.FullControl,
                            InheritanceFlags.None, PropagationFlags.None, AccessControlType.Allow));
                }
                catch (Exception) { }     // a run that cannot read its own account keeps the list above
                return s;
            }

                if (!info.Exists) info.Create(RootRules());   // BD-163: with this run's account on the list
                else
                {
                    AssertLocation(root);
                    info.SetAccessControl(Rules());
                }
                SetOwnerToAdministrators(info);
            }

            static void SetOwnerToAdministrators(DirectoryInfo info)
            {
                try
                {
                    var own = new DirectorySecurity();
                    own.SetOwner(new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null));
                    info.SetAccessControl(own);
                }
                catch (Exception) { }     // a process that may not set the owner still has the permission list
            }

            // The version folder, empty and ours: whatever stood there is removed first, so no file of an earlier
            // run and nothing a standard user left behind can survive into this one. A link where the folder
            // belongs is unlinked, never followed.
            public static void ResetFolder(string dir)
            {
                Remove(dir);
                new DirectoryInfo(dir).Create(Rules());
            }
        }
    }
}
