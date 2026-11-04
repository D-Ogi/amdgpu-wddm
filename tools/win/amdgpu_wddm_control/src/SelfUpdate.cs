// Self-update, stage 1 (docs/design/self-update.md): "Install update" on the Driver page. The control application
// reads the update manifest of the newer release that the update check found (nagi-update.json and its detached
// signature nagi-update.json.p7s, assets of that GitHub release), checks the signature against the pinned publisher
// certificates and every field the installer reads, makes the same plan as the installer (which component kinds
// changed), downloads only the assets of that plan into the user's own folder and checks each size and SHA-256. Then
// it starts the setup window of the installed release's kept repair set as administrator (one prompt) with
// --apply <folder> --wait-pid <this process>, and closes itself so that its files can be replaced. Setup checks
// everything again in a folder only administrators can write (installer\apply.ps1).
//
// The rules of the plan are those of installer\nagi-update.ps1 Get-NagiApplyPlan; tools/release/nagi-update-plan-cases.json
// holds the cases both must answer the same way (test/SelfUpdateTests.cs, tools/release/test-nagi-update.ps1).
// Network rules: HTTPS only, only this repository's release download address, redirects followed by hand to GitHub's
// HTTPS download hosts only, each asset no larger than its manifest size.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Security.Cryptography.Pkcs;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;
using Microsoft.Win32;

namespace AmdgpuWddmControl
{
    public sealed class SelfUpdatePlan
    {
        public string Outcome;          // apply, up-to-date, refused
        public string Reason;           // refused: unknown-installed, downgrade, too-old, incompatible
        public List<IDictionary<string, object>> Actions = new List<IDictionary<string, object>>();
        public bool Restart;
        public IEnumerable<string> ActionNames { get { return Actions.Select(a => SelfUpdate.Str(a, "name")); } }
    }

    public sealed class SelfUpdateResult
    {
        public string Outcome;          // ready, up-to-date, refused, failed, no-manifest, no-setup
        public string Reason, Dir, Release, Detail;
        public bool Restart;
        // The words of the Driver card for this result (upd.self.*).
        public string MessageId { get { return Outcome == "refused" ? "upd.self.refused-" + Reason : "upd.self." + Outcome; } }
    }

    public static class SelfUpdate
    {
        // The certificates that may sign an update manifest, by SHA-1 thumbprint: the same list as
        // installer\nagi-update.ps1 $NagiPinnedSigners (tools/release/test-nagi-update.ps1 holds the two together).
        public static readonly string[] PinnedSigners = { "71B3BFDDB5EFC22F0C133AD55D865D48AD352981" };
        public const string Schema = "nagi.update/1", InstalledSchema = "nagi.installed/1";
        public const string ManifestAsset = "nagi-update.json", SignatureAsset = "nagi-update.json.p7s";
        public const int MaxManifestBytes = 1 << 20, MaxSignatureBytes = 64 << 10;
        public const string Sha256Oid = "2.16.840.1.101.3.4.2.1";
        static readonly string[] Kinds = { "driver", "apps", "data" };
        static readonly string[] DownloadHosts = { "github.com", "objects.githubusercontent.com", "release-assets.githubusercontent.com" };

        // ---- JSON helpers -------------------------------------------------------------------------------------------
        public static IDictionary<string, object> Parse(string json)
        {
            try { return new JavaScriptSerializer { MaxJsonLength = MaxManifestBytes * 2 }.DeserializeObject(json ?? "") as IDictionary<string, object>; }
            catch (Exception) { return null; }
        }
        public static object Get(IDictionary<string, object> o, string key) { object v; return o != null && o.TryGetValue(key, out v) ? v : null; }
        public static string Str(IDictionary<string, object> o, string key) { var v = Get(o, key); return v == null ? null : Convert.ToString(v, CultureInfo.InvariantCulture); }
        public static IDictionary<string, object> Obj(IDictionary<string, object> o, string key) { return Get(o, key) as IDictionary<string, object>; }
        public static object[] Arr(IDictionary<string, object> o, string key) { return Get(o, key) as object[] ?? new object[0]; }
        public static long? Long(object v)
        {
            if (v == null || v is bool) return null;
            long n;
            return long.TryParse(Convert.ToString(v, CultureInfo.InvariantCulture), NumberStyles.Integer, CultureInfo.InvariantCulture, out n) ? (long?)n : null;
        }
        static bool SameText(string a, string b) { return string.Equals(a ?? "", b ?? "", StringComparison.OrdinalIgnoreCase); }

        // ---- versions -----------------------------------------------------------------------------------------------
        // -1, 0 or 1; null when either side is not of the tag grammar (UpdateCheck.cs ReleaseVersion).
        public static int? Compare(string a, string b)
        {
            var x = ReleaseVersion.Parse(a); var y = ReleaseVersion.Parse(b);
            if (x == null || y == null) return null;
            return Math.Sign(x.CompareTo(y));
        }

        // ---- the manifest -------------------------------------------------------------------------------------------
        static readonly Regex NamePattern = new Regex(@"^[a-z][a-z0-9-]{0,39}$", RegexOptions.CultureInvariant);
        static readonly Regex AssetPattern = new Regex(@"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}\.zip$", RegexOptions.CultureInvariant);
        static readonly Regex HashPattern = new Regex(@"^[0-9A-Fa-f]{64}$", RegexOptions.CultureInvariant);
        static readonly Regex AbiPattern = new Regex(@"^0x[0-9A-Fa-f]{8}$", RegexOptions.CultureInvariant);
        static readonly Regex CatalogPattern = new Regex(@"^[0-9a-f]{64}$", RegexOptions.CultureInvariant);

        // A path inside a pack: relative, forward slashes, no '..', no drive, no empty part (Test-NagiRelativePath).
        public static bool RelativePath(string p)
        {
            if (string.IsNullOrEmpty(p) || p.Length > 200 || p.StartsWith("/", StringComparison.Ordinal) || p.IndexOfAny(new[] { '\\', ':', '*', '?', '"', '<', '>', '|' }) >= 0) return false;
            return p.Split('/').All(s => s.Length > 0 && s != "." && s != "..");
        }

        // The checked manifest, or null with the reason (Read-NagiUpdateManifest).
        public static IDictionary<string, object> ReadManifest(byte[] bytes, out string reason)
        {
            reason = null;
            if (bytes == null) { reason = "the update manifest is missing"; return null; }
            if (bytes.Length > MaxManifestBytes) { reason = "the update manifest is larger than 1 MiB"; return null; }
            string text;
            try { text = new UTF8Encoding(false, true).GetString(bytes); } catch (Exception) { reason = "the update manifest is not UTF-8"; return null; }
            var m = Parse(text);
            if (m == null) { reason = "the update manifest is not JSON"; return null; }
            if (Str(m, "schema") != Schema) { reason = "unknown manifest schema '" + Str(m, "schema") + "'"; return null; }
            if (ReleaseVersion.Parse(Str(m, "release")) == null) { reason = "release '" + Str(m, "release") + "' is not of the tag grammar"; return null; }
            if (ReleaseVersion.Parse(Str(m, "min_installed")) == null) { reason = "min_installed '" + Str(m, "min_installed") + "' is not of the tag grammar"; return null; }
            var names = new HashSet<string>();
            foreach (var o in Arr(m, "components"))
            {
                var c = o as IDictionary<string, object>;
                if (c == null) { reason = "an empty component"; return null; }
                var name = Str(c, "name") ?? "";
                if (!NamePattern.IsMatch(name)) { reason = "component name '" + name + "' is not allowed"; return null; }
                if (!names.Add(name)) { reason = "component " + name + " is named twice"; return null; }
                var kind = Str(c, "kind");
                if (Array.IndexOf(Kinds, kind) < 0) { reason = "component " + name + " has the unknown kind '" + kind + "'"; return null; }
                if (ReleaseVersion.Parse(Str(c, "version")) == null) { reason = "component " + name + " has the version '" + Str(c, "version") + "'"; return null; }
                if (!AssetPattern.IsMatch(Str(c, "asset") ?? "")) { reason = "component " + name + " names the asset '" + Str(c, "asset") + "'"; return null; }
                if (!HashPattern.IsMatch(Str(c, "sha256") ?? "")) { reason = "component " + name + " has no SHA256"; return null; }
                if (!HashPattern.IsMatch(Str(c, "content_id") ?? "")) { reason = "component " + name + " has no content_id"; return null; }
                var size = Long(Get(c, "size"));
                if (size == null || size <= 0 || size > 4L << 30) { reason = "component " + name + " has the size '" + Str(c, "size") + "'"; return null; }
                if (kind == "driver")
                {
                    var d = Obj(c, "driver");
                    if (name != "driver") { reason = "the driver component must be named driver"; return null; }
                    if (!AbiPattern.IsMatch(Str(d, "kmd_abi") ?? "")) { reason = "the driver component has no kmd_abi"; return null; }
                    if (!CatalogPattern.IsMatch(Str(d, "catalog_id") ?? "")) { reason = "the driver component has no catalog_id"; return null; }
                    var schema = Long(Get(d, "state_schema"));
                    if (schema == null || schema < 1) { reason = "the driver component has no state_schema"; return null; }
                }
                else
                {
                    var body = Obj(c, kind);
                    var files = Arr(body, "files");
                    if (body == null || files.Length == 0) { reason = "component " + name + " lists no files"; return null; }
                    foreach (var fo in files)
                    {
                        var f = fo as IDictionary<string, object>;
                        if (!RelativePath(Str(f, "path"))) { reason = "component " + name + " names the file '" + Str(f, "path") + "'"; return null; }
                        if (!HashPattern.IsMatch(Str(f, "sha256") ?? "")) { reason = "component " + name + " has no SHA256 for " + Str(f, "path"); return null; }
                    }
                    if (kind == "apps")
                    {
                        if (name != "apps") { reason = "the apps component must be named apps"; return null; }
                        if (!Arr(body, "kmd_abis").Any(a => AbiPattern.IsMatch(Convert.ToString(a, CultureInfo.InvariantCulture) ?? ""))) { reason = "the apps component names no kmd_abis"; return null; }
                        if (Arr(body, "state_schemas").Length == 0) { reason = "the apps component names no state_schemas"; return null; }
                    }
                }
            }
            if (!names.Contains("driver")) { reason = "the manifest has no driver component"; return null; }
            if (!names.Contains("apps")) { reason = "the manifest has no apps component"; return null; }
            return m;
        }

        // ---- the signature ------------------------------------------------------------------------------------------
        // A detached CMS SignedData over the exact manifest bytes, SHA-256, one signer, whose certificate is pinned. The
        // chain is not asked: the test certificate is not in every computer's trusted roots, and a chain to a trusted
        // root alone would admit any publisher. Null when it holds, else the reason.
        public static string CheckSignature(byte[] manifest, byte[] signature, IEnumerable<string> pinned, out string thumbprint)
        {
            thumbprint = null;
            if (signature == null || signature.Length == 0) return "the signature file is missing";
            if (signature.Length > MaxSignatureBytes) return "the signature file is larger than 64 KiB";
            var cms = new SignedCms(new ContentInfo(manifest ?? new byte[0]), true);
            try { cms.Decode(signature); } catch (CryptographicException e) { return "the signature is not a CMS message: " + e.Message; }
            if (cms.SignerInfos.Count != 1) return "the signature has " + cms.SignerInfos.Count + " signers, one is required";
            var signer = cms.SignerInfos[0];
            if (signer.Certificate == null) return "the signature carries no signer certificate";
            if (signer.DigestAlgorithm.Value != Sha256Oid) return "the digest is " + signer.DigestAlgorithm.FriendlyName + ", SHA-256 is required";
            try { signer.CheckSignature(true); } catch (CryptographicException e) { return "the signature does not match the manifest: " + e.Message; }
            thumbprint = signer.Certificate.Thumbprint.ToUpperInvariant();
            var t = thumbprint;
            if (!(pinned ?? new string[0]).Any(p => string.Equals((p ?? "").ToUpperInvariant(), t, StringComparison.Ordinal))) return "the signer " + t + " is not a pinned publisher";
            return null;
        }

        // ---- the installed view and the plan --------------------------------------------------------------------------
        // The installed view (Get-NagiInstalledView) from the install root's manifest.json, the update record
        // installed.json and the apps layout ("classic", "junction" with its target folder, or "absent").
        public static IDictionary<string, object> InstalledView(IDictionary<string, object> manifest, IDictionary<string, object> record, string layoutKind, string targetLeaf)
        {
            var uc = Obj(manifest, "update_components");
            var ucDriver = Obj(uc, "driver"); var ucApps = Obj(uc, "apps");
            var release = Str(manifest, "release");
            var abi = Str(manifest, "kmd_abi");
            string kmd = null;
            foreach (var o in Arr(manifest, "components"))
            {
                var c = o as IDictionary<string, object>;
                if (c != null && Str(c, "package_path") == "payload/kmd/bc250kmd.sys") { kmd = Str(c, "sha256"); break; }
            }
            long schema = Long(Get(ucDriver, "state_schema")) ?? 2;   // every release before the update identities wrote schema 2
            var driver = new Dictionary<string, object> { { "release", release }, { "content_id", Str(ucDriver, "content_id") }, { "kmd_abi", abi },
                { "kmd_sha256", kmd }, { "state_schema", schema }, { "catalog_id", Str(ucDriver, "catalog_id") } };
            var apps = new Dictionary<string, object> { { "version", release }, { "content_id", Str(ucApps, "content_id") },
                { "kmd_abis", abi != null ? new object[] { abi } : new object[0] }, { "state_schemas", new object[] { schema } }, { "source", "install" } };
            var effective = release;
            var data = new Dictionary<string, object>();
            var did = Str(ucDriver, "content_id");
            if (record != null && Str(record, "schema") == InstalledSchema && !string.IsNullOrEmpty(did) && SameText(Str(record, "driver_content_id"), did))
            {
                var ra = Obj(record, "apps");
                if (ra != null && layoutKind == "junction" && string.Equals(targetLeaf, Str(ra, "folder"), StringComparison.OrdinalIgnoreCase))
                    apps = new Dictionary<string, object> { { "version", Str(ra, "version") }, { "content_id", Str(ra, "content_id") },
                        { "kmd_abis", Arr(ra, "kmd_abis") }, { "state_schemas", Arr(ra, "state_schemas") }, { "source", "update" } };
                var rd = Obj(record, "data");
                if (rd != null) foreach (var kv in rd) { var e = kv.Value as IDictionary<string, object>; if (e != null) data[kv.Key] = new Dictionary<string, object> { { "version", Str(e, "version") }, { "content_id", Str(e, "content_id") } }; }
                if (Compare(Str(record, "release"), release) == 1) effective = Str(record, "release");
            }
            return new Dictionary<string, object> { { "release", effective }, { "driver", driver }, { "apps", apps }, { "data", data } };
        }

        // What an update does (Get-NagiApplyPlan): the driver component is the whole release package and the installer
        // installs it, apps included, so a plan with the driver never lists the apps pack.
        public static SelfUpdatePlan Plan(IDictionary<string, object> m, IDictionary<string, object> installed)
        {
            Func<string, string, SelfUpdatePlan> no = (o, r) => new SelfUpdatePlan { Outcome = o, Reason = r };
            var cmp = Compare(Str(m, "release"), Str(installed, "release"));
            if (cmp == null) return no("refused", "unknown-installed");
            if (cmp < 0) return no("refused", "downgrade");
            if (cmp == 0) return no("up-to-date", null);
            var min = Compare(Str(installed, "release"), Str(m, "min_installed"));
            if (min == null || min < 0) return no("refused", "too-old");
            var byName = new Dictionary<string, IDictionary<string, object>>();
            foreach (var o in Arr(m, "components")) { var c = o as IDictionary<string, object>; if (c != null) byName[Str(c, "name") ?? ""] = c; }
            IDictionary<string, object> drv, app;
            byName.TryGetValue("driver", out drv); byName.TryGetValue("apps", out app);
            var iDrv = Obj(installed, "driver"); var iApps = Obj(installed, "apps");
            bool driverChanged = string.IsNullOrEmpty(Str(iDrv, "content_id")) || !SameText(Str(drv, "content_id"), Str(iDrv, "content_id"));
            bool appsChanged = driverChanged || string.IsNullOrEmpty(Str(iApps, "content_id")) || !SameText(Str(app, "content_id"), Str(iApps, "content_id"));
            // The pair that runs after the update: the new driver or the installed one, and the new apps or the installed ones.
            var finalAbi = driverChanged ? Str(Obj(drv, "driver"), "kmd_abi") : Str(iDrv, "kmd_abi");
            var finalSchema = driverChanged ? Long(Get(Obj(drv, "driver"), "state_schema")) : Long(Get(iDrv, "state_schema"));
            var abis = appsChanged ? Arr(Obj(app, "apps"), "kmd_abis") : Arr(iApps, "kmd_abis");
            var schemas = appsChanged ? Arr(Obj(app, "apps"), "state_schemas") : Arr(iApps, "state_schemas");
            if (!abis.Any(a => SameText(Convert.ToString(a, CultureInfo.InvariantCulture), finalAbi)) || !schemas.Any(s => Long(s) == finalSchema))
                return no("refused", "incompatible");
            var plan = new SelfUpdatePlan { Outcome = "apply" };
            if (driverChanged) plan.Actions.Add(drv);
            else if (appsChanged) plan.Actions.Add(app);
            var iData = Obj(installed, "data");
            foreach (var o in Arr(m, "components"))
            {
                var c = o as IDictionary<string, object>;
                if (c == null || Str(c, "kind") != "data") continue;
                var have = Obj(iData, Str(c, "name"));
                if (have == null || !SameText(Str(have, "content_id"), Str(c, "content_id"))) plan.Actions.Add(c);
            }
            if (plan.Actions.Count == 0) return no("up-to-date", null);
            plan.Restart = driverChanged;
            return plan;
        }

        // The words of the driver transaction for the Driver card, or null when there is nothing to say.
        public static string TransactionMessage(IDictionary<string, object> txn)
        {
            switch (Str(txn, "state"))
            {
                case "driver-pending": return "upd.self.txn-pending";
                case "rollback-pending": return "upd.self.txn-rollback-pending";
                case "rolled-back": return "upd.self.txn-rolled-back";
                case "manual": return "upd.self.txn-manual";
                default: return null;
            }
        }

        // ---- addresses ------------------------------------------------------------------------------------------------
        public static string DownloadUrl(string tag, string asset)
        {
            if (ReleaseVersion.Parse(tag) == null || (!AssetPattern.IsMatch(asset ?? "") && asset != ManifestAsset && asset != SignatureAsset)) return null;
            return UpdateCheck.ReleasesPage + "/download/" + tag + "/" + asset;
        }

        // A redirect of a download: HTTPS, the default port, and one of GitHub's download hosts.
        public static bool RedirectAllowed(Uri u)
        {
            return u != null && u.IsAbsoluteUri && u.Scheme == Uri.UriSchemeHttps && u.IsDefaultPort &&
                DownloadHosts.Any(h => string.Equals(u.Host, h, StringComparison.OrdinalIgnoreCase));
        }

        // ---- this computer --------------------------------------------------------------------------------------------
        public static string InstallRoot()
        {
            try
            {
                using (var k = Registry.LocalMachine.OpenSubKey(HelpGuides.RepairPath))
                {
                    var v = k == null ? null : k.GetValue("InstallRoot") as string;
                    if (!string.IsNullOrEmpty(v) && Path.IsPathRooted(v)) return v;
                }
            }
            catch (Exception) { }
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "amdgpu-wddm");
        }

        public static string UpdateRoot() { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "amdgpu-wddm", "update"); }

        static IDictionary<string, object> ReadJson(string path)
        {
            try { return File.Exists(path) && new FileInfo(path).Length <= MaxManifestBytes * 4L ? Parse(File.ReadAllText(path, Encoding.UTF8)) : null; }
            catch (Exception) { return null; }
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern uint GetFinalPathNameByHandleW(IntPtr file, StringBuilder path, uint size, uint flags);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool CloseHandle(IntPtr handle);

        // The layout of <install root>\control: absent, classic (a real folder) or junction, with the folder it names.
        public static string Layout(string installRoot, out string targetLeaf)
        {
            targetLeaf = null;
            var control = Path.Combine(installRoot, "control");
            try
            {
                var info = new DirectoryInfo(control);
                if (!info.Exists) return "absent";
                if ((info.Attributes & FileAttributes.ReparsePoint) == 0) return "classic";
                var h = CreateFileW(control, 0, 7, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero);   // OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS
                if (h == new IntPtr(-1)) return "junction";
                try
                {
                    var sb = new StringBuilder(1024);
                    var n = GetFinalPathNameByHandleW(h, sb, (uint)sb.Capacity, 0);
                    if (n > 0 && n < sb.Capacity) targetLeaf = Path.GetFileName(sb.ToString().TrimEnd('\\'));
                }
                finally { CloseHandle(h); }
                return "junction";
            }
            catch (Exception) { return "absent"; }
        }

        public static IDictionary<string, object> InstalledHere()
        {
            var root = InstallRoot();
            string leaf;
            var kind = Layout(root, out leaf);
            return InstalledView(ReadJson(Path.Combine(root, "manifest.json")), ReadJson(Path.Combine(UpdateRoot(), "installed.json")), kind, leaf);
        }

        // The release that counts for the update check: the driver release, or a newer one whose apps or data an update
        // installed on top of it. installedVersion (Release\Version) when nothing more is known.
        public static string EffectiveRelease(string installedVersion)
        {
            try
            {
                var r = Str(InstalledHere(), "release");
                if (ReleaseVersion.Parse(r) != null && (ReleaseVersion.Parse(installedVersion) == null || Compare(r, installedVersion) >= 0)) return r;
            }
            catch (Exception) { }
            return installedVersion;
        }

        public static string TransactionMessageHere() { return TransactionMessage(ReadJson(Path.Combine(UpdateRoot(), "transaction.json"))); }

        // ---- the download ---------------------------------------------------------------------------------------------
        public const int RequestSeconds = 60, MaxRedirects = 5;

        // One asset to a file, at most maxBytes; HTTPS only, redirects by hand to GitHub's download hosts. Returns null,
        // or why it failed; "missing" when the release has no such asset.
        public static string Download(string url, string file, long maxBytes, Action<long> progress)
        {
            ServicePointManager.SecurityProtocol = SecurityProtocolType.SystemDefault;
            var uri = new Uri(url);
            for (int hop = 0; hop <= MaxRedirects; hop++)
            {
                if (hop > 0 && !RedirectAllowed(uri)) return "a redirect to " + uri.Host + " is not allowed";
                if (uri.Scheme != Uri.UriSchemeHttps) return "not HTTPS";
                var request = (HttpWebRequest)WebRequest.Create(uri);
                request.Method = "GET";
                request.UserAgent = "amdgpu-wddm-control/" + System.Reflection.Assembly.GetExecutingAssembly().GetName().Version;
                request.AllowAutoRedirect = false;
                request.Timeout = RequestSeconds * 1000; request.ReadWriteTimeout = RequestSeconds * 1000;
                HttpWebResponse response;
                try { response = (HttpWebResponse)request.GetResponse(); }
                catch (WebException e)
                {
                    var http = e.Response as HttpWebResponse;
                    if (http == null) return e.Status.ToString();
                    response = http;
                }
                using (response)
                {
                    int status = (int)response.StatusCode;
                    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)
                    {
                        Uri next;
                        if (!Uri.TryCreate(uri, response.Headers["Location"] ?? "", out next)) return "a redirect without a location";
                        uri = next;
                        continue;
                    }
                    if (status == 404) return "missing";
                    if (status != 200) return "HTTP " + status;
                    if (response.ContentLength > maxBytes) return "the answer is larger than " + maxBytes + " bytes";
                    using (var body = response.GetResponseStream())
                    using (var output = new FileStream(file, FileMode.Create, FileAccess.Write, FileShare.None))
                    {
                        var buffer = new byte[1 << 16];
                        long total = 0;
                        int n;
                        while ((n = body.Read(buffer, 0, buffer.Length)) > 0)
                        {
                            total += n;
                            if (total > maxBytes) return "the answer is larger than " + maxBytes + " bytes";
                            output.Write(buffer, 0, n);
                            if (progress != null) progress(total);
                        }
                    }
                    return null;
                }
            }
            return "too many redirects";
        }

        public static string Sha256Of(string file)
        {
            using (var sha = SHA256.Create())
            using (var s = File.OpenRead(file))
                return BitConverter.ToString(sha.ComputeHash(s)).Replace("-", "");
        }

        public static string DownloadFolder(string release)
        {
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "amdgpu-wddm", "update", release);
        }

        // Everything up to the setup window: the manifest and its signature of the release tag, the checks, the plan
        // against what is installed now, and the assets of the plan. progress: a line for the Driver card.
        public static SelfUpdateResult Prepare(string tag, string installedVersion, Action<string> progress)
        {
            Func<string, string, string, SelfUpdateResult> end = (o, r, d) => new SelfUpdateResult { Outcome = o, Reason = r, Detail = d };
            var version = ReleaseVersion.Parse(tag);
            if (version == null) return end("failed", null, "the tag " + tag + " is not of the grammar");
            if (HelpGuides.RepairSetup() == null) return end("no-setup", null, "no kept repair set (Release\\RepairSetup)");
            var dir = DownloadFolder(version.Text);
            try
            {
                if (Directory.Exists(dir)) Directory.Delete(dir, true);
                Directory.CreateDirectory(dir);
                var mPath = Path.Combine(dir, ManifestAsset); var sPath = Path.Combine(dir, SignatureAsset);
                if (progress != null) progress(Strings.T("upd.self.reading"));
                var why = Download(DownloadUrl(tag, ManifestAsset), mPath, MaxManifestBytes, null);
                if (why == "missing") return end("no-manifest", null, "the release has no " + ManifestAsset);
                if (why != null) return end("failed", null, ManifestAsset + ": " + why);
                why = Download(DownloadUrl(tag, SignatureAsset), sPath, MaxSignatureBytes, null);
                if (why == "missing") return end("refused", "signature", "the release has no " + SignatureAsset);
                if (why != null) return end("failed", null, SignatureAsset + ": " + why);
                var mBytes = File.ReadAllBytes(mPath);
                string thumb;
                why = CheckSignature(mBytes, File.ReadAllBytes(sPath), PinnedSigners, out thumb);
                if (why != null) return end("refused", "signature", why);
                var m = ReadManifest(mBytes, out why);
                if (m == null) return end("refused", "signature", why);
                if (Compare(Str(m, "release"), version.Text) != 0) return end("refused", "signature", "the manifest is of " + Str(m, "release") + ", the release is " + version.Text);
                var installed = InstalledHere();
                if (ReleaseVersion.Parse(Str(installed, "release")) == null && ReleaseVersion.Parse(installedVersion) != null) installed["release"] = installedVersion;
                var plan = Plan(m, installed);
                if (plan.Outcome == "up-to-date") return end("up-to-date", null, null);
                if (plan.Outcome == "refused") return end("refused", plan.Reason, null);
                foreach (var c in plan.Actions)
                {
                    var asset = Str(c, "asset");
                    long size = Long(Get(c, "size")) ?? 0;
                    var file = Path.Combine(dir, asset);
                    var mb = (size + (1 << 20) - 1) >> 20;
                    if (progress != null) progress(Strings.T("upd.self.downloading", 0, mb));
                    long shown = -1;
                    why = Download(DownloadUrl(tag, asset), file, size, done =>
                    {
                        var now = done >> 20;
                        if (now != shown && progress != null) { shown = now; progress(Strings.T("upd.self.downloading", now, mb)); }
                    });
                    if (why != null) return end("failed", null, asset + ": " + why);
                    if (progress != null) progress(Strings.T("upd.self.checking-files"));
                    if (new FileInfo(file).Length != size) return end("refused", "hash", asset + " has another size than the manifest says");
                    if (!SameText(Sha256Of(file), Str(c, "sha256"))) return end("refused", "hash", asset + " has another SHA256 than the manifest says");
                }
                return new SelfUpdateResult { Outcome = "ready", Dir = dir, Release = Str(m, "release"), Restart = plan.Restart, Detail = string.Join(", ", plan.ActionNames) };
            }
            catch (Exception e) { return end("failed", null, e.GetType().Name + ": " + e.Message); }
        }

        // The setup window of the kept repair set, as administrator, on the prepared folder. False when it is not there
        // or the person declined the administrator prompt.
        public static bool Launch(string dir)
        {
            var setup = HelpGuides.RepairSetup();
            if (setup == null) return false;
            int pid;
            using (var me = Process.GetCurrentProcess()) pid = me.Id;
            var args = "--apply \"" + dir.TrimEnd('\\') + "\" --wait-pid " + pid.ToString(CultureInfo.InvariantCulture);
            try { using (Process.Start(new ProcessStartInfo(setup, args) { UseShellExecute = true, Verb = "runas" })) { } return true; }
            catch (Exception) { return false; }
        }
    }
}
