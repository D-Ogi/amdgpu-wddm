// Install update (src/SelfUpdate.cs, docs/design/self-update.md): the manifest reader, the signature with its pinned
// signer, the plan (the same cases as installer\nagi-update.ps1, tools/release/nagi-update-plan-cases.json), the
// installed view, the download addresses and the words. Each rule has a negative control.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using AmdgpuWddmControl;

static partial class UnitTests
{
    static void SelfUpdateTests(string root)
    {
        Strings.Language = "en";
        var data = Path.Combine(root, @"tools\release\test-data\nagi-update");
        var manifest = File.ReadAllBytes(Path.Combine(data, "nagi-update.json"));
        var signature = File.ReadAllBytes(Path.Combine(data, "nagi-update.json.p7s"));
        var fixtureSigner = File.ReadAllText(Path.Combine(data, "signer.txt")).Trim();

        // The signature: the fixture signer pinned, the real pins, a changed byte, a missing signature.
        string thumb;
        Equal<string>(null, SelfUpdate.CheckSignature(manifest, signature, new[] { fixtureSigner }, out thumb), "self-update signature: the fixture manifest with its signer pinned");
        Equal(fixtureSigner, thumb, "self-update signature: the signer thumbprint");
        var refused = SelfUpdate.CheckSignature(manifest, signature, SelfUpdate.PinnedSigners, out thumb);
        Check(refused != null && refused.Contains("not a pinned publisher"), "self-update signature negative control: a signer that is not pinned is refused (" + refused + ")");
        var tampered = (byte[])manifest.Clone();
        int at = Encoding.ASCII.GetString(tampered).IndexOf("tester.37", StringComparison.Ordinal);
        tampered[at + 8] = (byte)'8';
        refused = SelfUpdate.CheckSignature(tampered, signature, new[] { fixtureSigner }, out thumb);
        Check(refused != null && refused.Contains("does not match"), "self-update signature negative control: one changed manifest byte is refused (" + refused + ")");
        Check(SelfUpdate.CheckSignature(manifest, null, new[] { fixtureSigner }, out thumb) != null, "self-update signature negative control: no signature is refused");
        Check(SelfUpdate.CheckSignature(manifest, new byte[] { 0x30, 0x03, 0x02, 0x01, 0x01 }, new[] { fixtureSigner }, out thumb) != null, "self-update signature negative control: a signature that is not CMS is refused");
        Check(SelfUpdate.PinnedSigners.Length >= 1 && !SelfUpdate.PinnedSigners.Contains(fixtureSigner), "self-update signature: the fixture signer is not a shipped pin");

        // The manifest reader: the fixture, then broken copies.
        string reason;
        var m = SelfUpdate.ReadManifest(manifest, out reason);
        Check(m != null && reason == null, "self-update manifest: the fixture reads (" + reason + ")");
        Func<string, string, string> broken = (from, to) =>
        {
            var text = Encoding.UTF8.GetString(manifest);
            Check(text.Contains(from), "self-update manifest mutation applies: " + from);
            string r;
            var x = SelfUpdate.ReadManifest(Encoding.UTF8.GetBytes(text.Replace(from, to)), out r);
            return x == null ? r : null;
        };
        Check(broken("\"nagi.update/1\"", "\"nagi.update/2\"") != null, "self-update manifest negative control: another schema");
        Check(broken("\"name\": \"apps\"", "\"name\": \"driver\"") != null, "self-update manifest negative control: a component named twice");
        Check(broken("\"kind\": \"apps\"", "\"kind\": \"tools\"") != null, "self-update manifest negative control: an unknown kind");
        Check(broken("\"min_installed\": \"0.7.216.108-tester.36\"", "\"min_installed\": \"latest\"") != null, "self-update manifest negative control: min_installed off the tag grammar");
        Check(broken("\"path\": \"", "\"path\": \"..\\\\") != null, "self-update manifest negative control: a file path that climbs out");
        Check(SelfUpdate.ReadManifest(Encoding.UTF8.GetBytes("not json"), out reason) == null, "self-update manifest negative control: not JSON");
        Check(SelfUpdate.ReadManifest(new byte[] { 0xC3, 0x28 }, out reason) == null, "self-update manifest negative control: not UTF-8");
        Check(SelfUpdate.ReadManifest(new byte[SelfUpdate.MaxManifestBytes + 1], out reason) == null, "self-update manifest negative control: larger than 1 MiB");

        // Relative paths of pack files.
        Check(SelfUpdate.RelativePath("control/bc250ctl.exe") && SelfUpdate.RelativePath("strings/strings.en.txt"), "self-update paths: plain relative paths");
        foreach (var bad in new[] { "", @"C:\x", "/x", "../x", "a/../../x", "a:b", @"\\srv\x", @"strings\strings.en.txt", "a//b" })
            Check(!SelfUpdate.RelativePath(bad), "self-update paths negative control: '" + bad + "' is refused");

        // The plan: every case of the shared file, as installer\nagi-update.ps1 answers it.
        var cases = SelfUpdate.Parse(File.ReadAllText(Path.Combine(root, @"tools\release\nagi-update-plan-cases.json")));
        int n = 0;
        foreach (var o in SelfUpdate.Arr(cases, "cases"))
        {
            var c = (IDictionary<string, object>)o;
            var name = SelfUpdate.Str(c, "name");
            var e = SelfUpdate.Obj(c, "expect");
            var p = SelfUpdate.Plan(SelfUpdate.Obj(c, "manifest"), SelfUpdate.Obj(c, "installed"));
            Equal(SelfUpdate.Str(e, "outcome"), p.Outcome, "self-update plan " + name + ": outcome");
            Equal(SelfUpdate.Str(e, "reason"), p.Reason, "self-update plan " + name + ": reason");
            Equal(string.Join(",", SelfUpdate.Arr(e, "actions").Select(a => Convert.ToString(a))), string.Join(",", p.Actions.Select(a => SelfUpdate.Str(a, "name"))), "self-update plan " + name + ": actions");
            Equal((bool)SelfUpdate.Get(e, "restart"), p.Restart, "self-update plan " + name + ": restart");
            n++;
        }
        Check(n >= 15, "self-update plan: the shared cases (" + n + ")");

        // The plan of the fixture manifest over a tester.36 install: a driver change. Negative controls: the same
        // release, a newer release, and an install older than min_installed.
        Func<string, string, IDictionary<string, object>> inst = (release, driverId) => new Dictionary<string, object>
        {
            { "release", release },
            { "driver", new Dictionary<string, object> { { "content_id", driverId }, { "kmd_abi", "0x000700D8" }, { "state_schema", 2L } } },
            { "apps", new Dictionary<string, object> { { "content_id", null }, { "kmd_abis", new object[] { "0x000700D8" } }, { "state_schemas", new object[] { 2L } } } },
            { "data", new Dictionary<string, object>() },
        };
        var fp = SelfUpdate.Plan(m, inst("0.7.216.108-tester.36", "00"));
        Check(fp.Outcome == "apply" && fp.Restart && fp.Actions.Count == 1 && SelfUpdate.Str(fp.Actions[0], "name") == "driver", "self-update plan: the fixture over tester.36 is a driver update (" + fp.Outcome + " " + fp.Reason + ")");
        Equal("up-to-date", SelfUpdate.Plan(m, inst("0.7.216.109-tester.37", "00")).Outcome, "self-update plan negative control: the same release");
        Equal("downgrade", SelfUpdate.Plan(m, inst("0.7.216.110-tester.38", "00")).Reason, "self-update plan negative control: a newer install");
        Equal("too-old", SelfUpdate.Plan(m, inst("0.7.216.107-tester.35", "00")).Reason, "self-update plan negative control: older than min_installed");

        // The installed view: an update record counts only with the same driver content and the apps link on its folder.
        var im = new Dictionary<string, object>
        {
            { "release", "0.7.216.109-tester.37" }, { "kmd_abi", "0x000700D8" },
            { "update_components", new Dictionary<string, object> {
                { "driver", new Dictionary<string, object> { { "content_id", "D1" }, { "catalog_id", "C1" }, { "state_schema", 2L } } },
                { "apps", new Dictionary<string, object> { { "content_id", "A1" } } } } },
            { "components", new object[] { new Dictionary<string, object> { { "package_path", "payload/kmd/bc250kmd.sys" }, { "sha256", "K1" } } } },
        };
        var record = new Dictionary<string, object>
        {
            { "schema", "nagi.installed/1" }, { "release", "0.7.216.110-tester.38" }, { "driver_content_id", "D1" },
            { "apps", new Dictionary<string, object> { { "version", "0.7.216.110-tester.38" }, { "content_id", "A2" }, { "folder", "0.7.216.110-tester.38" },
                { "kmd_abis", new object[] { "0x000700D8" } }, { "state_schemas", new object[] { 2L } } } },
        };
        var v = SelfUpdate.InstalledView(im, record, "junction", "0.7.216.110-tester.38");
        Check(SelfUpdate.Str(v, "release") == "0.7.216.110-tester.38" && SelfUpdate.Str(SelfUpdate.Obj(v, "apps"), "content_id") == "A2", "self-update installed view: an apps update on the same driver");
        Equal("K1", SelfUpdate.Str(SelfUpdate.Obj(v, "driver"), "kmd_sha256"), "self-update installed view: the KMD hash of the package manifest");
        v = SelfUpdate.InstalledView(im, record, "junction", "0.7.216.109-tester.37");
        Check(SelfUpdate.Str(SelfUpdate.Obj(v, "apps"), "content_id") == "A1", "self-update installed view negative control: the link on another folder keeps the installed apps");
        v = SelfUpdate.InstalledView(im, record, "classic", null);
        Check(SelfUpdate.Str(SelfUpdate.Obj(v, "apps"), "content_id") == "A1", "self-update installed view negative control: a classic folder keeps the installed apps");
        record["driver_content_id"] = "D0";
        v = SelfUpdate.InstalledView(im, record, "junction", "0.7.216.110-tester.38");
        Check(SelfUpdate.Str(v, "release") == "0.7.216.109-tester.37" && SelfUpdate.Str(SelfUpdate.Obj(v, "apps"), "content_id") == "A1",
            "self-update installed view negative control: a record of another driver is ignored");

        // The transaction line.
        Equal("upd.self.txn-pending", SelfUpdate.TransactionMessage(new Dictionary<string, object> { { "state", "driver-pending" } }), "self-update transaction: pending");
        Equal("upd.self.txn-manual", SelfUpdate.TransactionMessage(new Dictionary<string, object> { { "state", "manual" } }), "self-update transaction: manual");
        Equal<string>(null, SelfUpdate.TransactionMessage(new Dictionary<string, object> { { "state", "committed" } }), "self-update transaction negative control: a committed update says nothing");
        Equal<string>(null, SelfUpdate.TransactionMessage(null), "self-update transaction negative control: no transaction");

        // Download addresses and redirects.
        Check((SelfUpdate.DownloadUrl("0.7.216.109-tester.37", "nagi-update.json") ?? "").EndsWith("/releases/download/0.7.216.109-tester.37/nagi-update.json", StringComparison.Ordinal),
            "self-update address: the manifest of a release");
        Check(SelfUpdate.DownloadUrl("0.7.216.109-tester.37", "nagi-apps-0.7.216.109-tester.37.zip") != null, "self-update address: a pack");
        Check(SelfUpdate.DownloadUrl("0.7.216.109-tester.37", "../../evil.exe") == null, "self-update address negative control: an asset that climbs out");
        Check(SelfUpdate.DownloadUrl("latest", "nagi-update.json") == null, "self-update address negative control: a tag off the grammar");
        Check(SelfUpdate.RedirectAllowed(new Uri("https://objects.githubusercontent.com/x/y?z=1")), "self-update redirect: GitHub's download host");
        Check(!SelfUpdate.RedirectAllowed(new Uri("http://objects.githubusercontent.com/x")), "self-update redirect negative control: plain HTTP");
        Check(!SelfUpdate.RedirectAllowed(new Uri("https://objects.githubusercontent.com:8443/x")), "self-update redirect negative control: another port");
        Check(!SelfUpdate.RedirectAllowed(new Uri("https://github.com.example.net/x")), "self-update redirect negative control: another host");

        // The words: every id the code names has words in each language, and none speaks of internals.
        var ids = new[] { "upd.self.reading", "upd.self.downloading", "upd.self.checking-files", "upd.self.ready", "upd.self.declined", "upd.self.up-to-date",
            "upd.self.failed", "upd.self.no-manifest", "upd.self.no-setup", "upd.self.txn-pending", "upd.self.txn-rollback-pending", "upd.self.txn-rolled-back",
            "upd.self.txn-manual", "driver.updates.install" }
            .Concat(new[] { "signature", "hash", "downgrade", "too-old", "incompatible", "unknown-installed" }.Select(r => "upd.self.refused-" + r)).ToList();
        foreach (var o in new[] { "ready", "up-to-date", "failed", "no-manifest", "no-setup" })
            Check(ids.Contains(new SelfUpdateResult { Outcome = o }.MessageId), "self-update words: the outcome " + o + " has an id");
        var src = File.ReadAllText(Path.Combine(root, @"tools\win\amdgpu_wddm_control\src\SelfUpdate.cs"));
        foreach (System.Text.RegularExpressions.Match x in System.Text.RegularExpressions.Regex.Matches(src, "\"(upd\\.self\\.[a-z-]*[a-z])\""))
            Check(ids.Contains(x.Groups[1].Value), "self-update words: " + x.Groups[1].Value + " is in the checked list");
        var jargon = new[] { "manifest", "SHA", "hash", "signature", "ABI", "KMD", "junction", "CMS", "schema", "content_id", "transaction" };
        foreach (var id in ids)
            foreach (var lang in Strings.Languages)
            {
                var text = Strings.In(lang, id, "1", "2");
                Check(!string.IsNullOrEmpty(text) && !text.StartsWith("[", StringComparison.Ordinal) && text != id, "self-update words: " + lang + " " + id);
                Check(!jargon.Any(j => text.IndexOf(j, StringComparison.OrdinalIgnoreCase) >= 0), "self-update words: no internals in " + lang + " " + id + ": " + text);
            }
        Check(Strings.In("en", "upd.self.downloading", "3", "40").Contains("3 of 40"), "self-update words: the download progress names its numbers");
    }
}
