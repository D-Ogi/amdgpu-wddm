// The release installer's manifest (<InstallDir>\manifest.json, schema 1): every component's install path and
// SHA-256 as packaged. The bug report hashes each installed file and lists the ones that differ or are missing, so a
// report says at once whether the tester runs the files of the release. Parsing and path resolution are pure; the
// hashing takes a function, so the host tests need no files.
using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
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
        public string Release = "", Version = "", KmdVersion = "";
        public readonly List<ManifestComponent> Components = new List<ManifestComponent>();
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
            var m = new ManifestInfo { Release = Field(root, "release"), Version = Field(root, "version"), KmdVersion = Field(root, "kmd_version") };
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
            return m;
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

        // One line per component: OK, MISMATCH, MISSING or UNRESOLVED, then a count line.
        public static string Report(ManifestInfo m, string installDir, string kmdImage, Func<string, string> expand,
            Func<string, bool> exists, Func<string, string> sha256)
        {
            var w = new StringBuilder();
            w.AppendLine("release " + m.Release + " version " + m.Version + " kmd " + m.KmdVersion + ", " + m.Components.Count + " components");
            int ok = 0, bad = 0;
            foreach (var c in m.Components)
            {
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
