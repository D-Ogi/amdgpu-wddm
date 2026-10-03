// The release installer's manifest (<InstallDir>\manifest.json, schema 1): every component's install path and
// SHA-256 as packaged. The bug report hashes each installed file and lists the ones that differ or are missing, so a
// report says at once whether the tester runs the files of the release. Parsing and path resolution are pure; the
// hashing takes a function, so the host tests need no files.
using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Web.Script.Serialization;

namespace AmdgpuWddmControl
{
    public sealed class ManifestComponent
    {
        public string Role, InstallPath, Version, Sha256;
    }

    public sealed class ManifestInfo
    {
        public string Release = "", Version = "", KmdVersion = "", KmdBuild = "", KmdAbi = "", ReleaseCertificate = "";
        public readonly List<ManifestComponent> Components = new List<ManifestComponent>();
        // "defaults": {"parameters": {name: DWORD}, "desktop_router": {name: DWORD}}, the values install.ps1 writes
        // for a fresh install. null when the manifest has none (releases before the control app's reset).
        public Dictionary<string, long> DefaultParameters, DefaultRouter;
        // "defaults"."d3d12_applications": {image: {"Experiment": "..."}}, the per-game switches the installer
        // recommends (origin label "recommended by the installer"). null when the manifest has none.
        public Dictionary<string, string> DefaultApplications;
    }

    public static class ManifestCheck
    {
        static string Field(IDictionary<string, object> d, string name)
        {
            object v;
            return d != null && d.TryGetValue(name, out v) && v != null ? Convert.ToString(v, System.Globalization.CultureInfo.InvariantCulture) : "";
        }

        public static ManifestInfo Parse(string json)
        {
            var root = new JavaScriptSerializer { MaxJsonLength = 64 << 20 }.DeserializeObject(json) as IDictionary<string, object>;
            if (root == null) throw new FormatException("the manifest is not a JSON object");
            if (Field(root, "schema") != "" && Field(root, "schema") != "1") throw new FormatException("manifest schema " + Field(root, "schema") + ", 1 expected");
            var m = new ManifestInfo { Release = Field(root, "release"), Version = Field(root, "version"), KmdVersion = Field(root, "kmd_version"),
                KmdBuild = Field(root, "kmd_build"), KmdAbi = Field(root, "kmd_abi"), ReleaseCertificate = Field(root, "release_certificate") };
            object list;
            if (root.TryGetValue("components", out list) && list is IEnumerable)
                foreach (var item in (IEnumerable)list)
                {
                    var c = item as IDictionary<string, object>;
                    if (c == null) continue;
                    m.Components.Add(new ManifestComponent
                    {
                        Role = Field(c, "role"), InstallPath = Field(c, "install_path"), Version = Field(c, "version"),
                        Sha256 = Field(c, "sha256").ToUpperInvariant(),
                    });
                }
            object defaults;
            if (root.TryGetValue("defaults", out defaults) && defaults != null)
            {
                var d = defaults as IDictionary<string, object>;
                if (d == null) throw new FormatException("manifest defaults is not an object");
                m.DefaultParameters = Dwords(d, "parameters");
                m.DefaultRouter = Dwords(d, "desktop_router");
                m.DefaultApplications = Applications(d);
            }
            return m;
        }

        static Dictionary<string, string> Applications(IDictionary<string, object> d)
        {
            object o;
            if (!d.TryGetValue("d3d12_applications", out o) || o == null) return null;
            var table = o as IDictionary<string, object>;
            if (table == null) throw new FormatException("manifest defaults.d3d12_applications is not an object");
            var result = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (var kv in table)
            {
                var app = kv.Value as IDictionary<string, object>;
                object value;
                if (!Profiles.IsValidImage(kv.Key) || app == null || !app.TryGetValue(Profiles.ValueName, out value) || !(value is string) || !Profiles.IsValidValue((string)value))
                    throw new FormatException("manifest defaults.d3d12_applications." + kv.Key + " is not an application with a valid Experiment");
                result[kv.Key] = (string)value;
            }
            return result;
        }

        static Dictionary<string, long> Dwords(IDictionary<string, object> d, string name)
        {
            object o;
            if (!d.TryGetValue(name, out o) || o == null) return null;
            var table = o as IDictionary<string, object>;
            if (table == null) throw new FormatException("manifest defaults." + name + " is not an object");
            var result = new Dictionary<string, long>(StringComparer.Ordinal);
            foreach (var kv in table)
            {
                if (!(kv.Value is int || kv.Value is long) || Convert.ToInt64(kv.Value) < 0 || Convert.ToInt64(kv.Value) > uint.MaxValue)
                    throw new FormatException("manifest defaults." + name + "." + kv.Key + " is not a DWORD number");
                result[kv.Key] = Convert.ToInt64(kv.Value);
            }
            return result;
        }

        // The manifest's path forms: "<InstallDir>\...", "%SystemRoot%\..." (any environment variable), absolute paths,
        // and "DriverStore (<inf>)\<file>", which is the file next to the running KMD image in the driver store.
        // null when the form is unknown or the driver store path is not known.
        public static string Resolve(string installPath, string installDir, string kmdImage, Func<string, string> expand)
        {
            if (string.IsNullOrEmpty(installPath)) return null;
            if (installPath.StartsWith("<InstallDir>", StringComparison.OrdinalIgnoreCase))
                return string.IsNullOrEmpty(installDir) ? null : installDir.TrimEnd('\\') + installPath.Substring("<InstallDir>".Length);
            if (installPath.StartsWith("DriverStore (", StringComparison.OrdinalIgnoreCase))
            {
                int close = installPath.IndexOf(")\\", StringComparison.Ordinal);
                if (close < 0 || string.IsNullOrEmpty(kmdImage)) return null;
                return Path.Combine(Path.GetDirectoryName(kmdImage), installPath.Substring(close + 2));
            }
            var expanded = expand(installPath);
            return Path.IsPathRooted(expanded) ? expanded : null;
        }

        // A certificate the installer imports instead of copying: install_path "LocalMachine <store> and <store>"
        // (for example "LocalMachine Root and TrustedPublisher"). The store names, or null for any other form.
        public static string[] CertificateStores(string installPath)
        {
            if (string.IsNullOrEmpty(installPath) || !installPath.StartsWith("LocalMachine ", StringComparison.OrdinalIgnoreCase)) return null;
            var names = installPath.Substring("LocalMachine ".Length).Split(new[] { " and ", ",", " " }, StringSplitOptions.RemoveEmptyEntries)
                .Select(s => s.Trim()).Where(s => s.Length > 0 && s != "and").ToArray();
            return names.Length > 0 && names.All(n => n.All(char.IsLetterOrDigit)) ? names : null;
        }

        // Whether a store holds the certificate: its SHA-256 over the DER bytes equals the component's sha256, or its
        // thumbprint equals the manifest's release_certificate.
        static bool Holds(IEnumerable<KeyValuePair<string, string>> certs, string sha256, string thumbprint)
        {
            foreach (var c in certs)
                if ((sha256.Length > 0 && string.Equals(c.Value, sha256, StringComparison.OrdinalIgnoreCase)) ||
                    (thumbprint.Length > 0 && string.Equals(c.Key, thumbprint, StringComparison.OrdinalIgnoreCase))) return true;
            return false;
        }

        // One line per component: OK, MISMATCH, MISSING or UNRESOLVED, then a count line. storeCerts lists a
        // LocalMachine store's certificates as (thumbprint, SHA-256 of the DER bytes).
        public static string Report(ManifestInfo m, string installDir, string kmdImage, Func<string, string> expand,
            Func<string, bool> exists, Func<string, string> sha256, Func<string, IEnumerable<KeyValuePair<string, string>>> storeCerts)
        {
            var w = new StringBuilder();
            w.AppendLine("release " + m.Release + " version " + m.Version + " kmd " + m.KmdVersion + ", " + m.Components.Count + " components");
            int ok = 0, bad = 0;
            string thumbprint = (m.ReleaseCertificate ?? "").Replace(" ", "").Replace(":", "");
            foreach (var c in m.Components)
            {
                var stores = c.Role == "certificate" ? CertificateStores(c.InstallPath) : null;
                if (stores != null)
                {
                    var absent = new List<string>();
                    foreach (var store in stores)
                    {
                        try { if (!Holds(storeCerts(store), c.Sha256, thumbprint)) absent.Add(store); }
                        catch (Exception) { absent.Add(store + " (unreadable)"); }
                    }
                    string certState = absent.Count == 0 ? "OK" : "MISSING";
                    if (absent.Count == 0) ok++; else bad++;
                    w.AppendLine(string.Format("{0,-10} {1,-14} {2} | expected {3} | {4}", certState, c.Role, c.InstallPath, c.Sha256,
                        absent.Count == 0 ? "in every store" : "not in " + string.Join(", ", absent)));
                    continue;
                }
                string path = Resolve(c.InstallPath, installDir, kmdImage, expand), state, actual = "";
                if (path == null) state = "UNRESOLVED";
                else if (!exists(path)) state = "MISSING";
                else
                {
                    try { actual = sha256(path).ToUpperInvariant(); state = actual == c.Sha256 ? "OK" : "MISMATCH"; }
                    catch (Exception e) { state = "UNREADABLE (" + e.Message + ")"; }
                }
                if (state == "OK") ok++; else bad++;
                w.AppendLine(string.Format("{0,-10} {1,-14} {2} | expected {3} | actual {4} | {5}", state, c.Role, path ?? c.InstallPath,
                    c.Sha256, actual, c.Version));
            }
            w.AppendLine(ok + " match, " + bad + " differ, missing or unresolved");
            return w.ToString();
        }
    }
}
