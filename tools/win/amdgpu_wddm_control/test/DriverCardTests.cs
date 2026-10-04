// G-VER (plan v7 WU-043, F-VER; plan 497 decisions 3 and 5-8; review 927 R4-R6): the running release only from a
// complete witness of this boot bound to one known manifest and to the loaded image; ambiguity never resolved to the
// newer package; the installed release from Release\Version only; verification only from a valid report of the
// installed package.
using System;
using System.Collections.Generic;
using System.Linq;
using AmdgpuWddmControl;

static partial class UnitTests
{
    const uint Reply199 = 0x000700C7;     // KMD 0.7.199
    static readonly string ShaM = new string('A', 64), ShaI = new string('D', 64), ShaM2 = new string('B', 64), ShaI2 = new string('E', 64);

    static KnownPackage Pkg(string version, string manifest, string image, string build = "0.7.199.1", string abi = "0x000700C7")
    {
        return new KnownPackage { Name = "amdgpu-wddm-tester-" + version, Version = version, KmdBuild = build, KmdAbi = abi, ImageSha256 = image, ManifestSha256 = manifest };
    }

    static DriverFacts Facts()
    {
        return new DriverFacts
        {
            ReplyAbi = Reply199, BootId = 53, InstalledVersion = "0.7.199.100-tester.11", InstalledManifestSha256 = ShaM, DriverDate = "10-3-2026",
            Witness = new ReleaseWitness { BootId = 53, RecordedUtc = "2026-10-04T08:00:00.000Z", RecordedBy = "verify", Release = "amdgpu-wddm-tester-0.7.199.100-tester.11",
                Version = "0.7.199.100-tester.11", ManifestSha256 = ShaM, KmdImageSha256 = ShaI, KmdBuild = "0.7.199.1", KmdAbi = "0x000700C7" },
            Phase = "verified", StatePackageVersion = "0.7.199.100-tester.11",
            Mapping = MappingStatus.Complete,
            Packages = new List<KnownPackage> { Pkg("0.7.199.100-tester.11", ShaM, ShaI) },
            Image = new LoadedImage { Available = true, OfStartedDevice = true, BootId = 53, Sha256 = ShaI, KmdBuild = "0.7.199.1" },
            Reports = new List<VerifyReport> { new VerifyReport { Valid = true, Passed = true, Utc = "2026-10-04T08:00:00Z", PackageVersion = "0.7.199.100-tester.11", ManifestSha256 = ShaM, PassedCount = 9 } },
        };
    }

    static void DriverCardTests()
    {
        Strings.Language = "en";
        var v = DriverCard.Decide(Facts());
        Equal(RunningKind.Exact, v.Running, "G-VER witness of this boot -> exact: " + v.ReportText);
        Equal("Running version: 0.7.199.100-tester.11", v.RunningText, "G-VER exact text");
        Equal(VerifyKind.Verified, v.Verification, "G-VER verified"); Equal("Driver date: 2026-10-03", v.DriverDateText, "G-VER INF date");
        var f = Facts(); f.Packages.Add(Pkg("0.7.199.100-tester.11", ShaM.ToLowerInvariant(), ShaI));
        Equal(RunningKind.Exact, DriverCard.Decide(f).Running, "G-VER the installed and the kept copy of one manifest are one package");

        // Two packages sharing the reply ABI, no witness: ambiguous, never the newer one.
        f = Facts(); f.Witness = null;
        f.Packages.Add(Pkg("0.7.199.101-tester.12", ShaM2, ShaI));
        v = DriverCard.Decide(f);
        Equal(RunningKind.Ambiguous, v.Running, "G-VER two packages, one reply ABI -> ambiguous");
        Equal("Running version: cannot be determined exactly.", v.RunningText, "G-VER ambiguous text");
        Check(!v.RunningText.Contains("tester.12") && v.ReportText.Contains("0.7.199.101-tester.12") && v.ReportText.Contains("0.7.199.100-tester.11"), "G-VER candidates only in the report");
        f.Mapping = MappingStatus.Incomplete;
        Equal(RunningKind.Unknown, DriverCard.Decide(f).Running, "497.3 an incomplete mapping is unknown, not ambiguous");
        f.Mapping = MappingStatus.Conflicting;
        Equal(RunningKind.Unknown, DriverCard.Decide(f).Running, "497.3 a conflicting mapping is unknown");
        // With the witness, the second package does not stop the exact answer: the witness binds one manifest.
        f = Facts(); f.Packages.Add(Pkg("0.7.199.101-tester.12", ShaM2, ShaI));
        Equal(RunningKind.Exact, DriverCard.Decide(f).Running, "G-VER the witness picks its own package among two");

        // A newer installed package pending the restart: installed shown pending; running from the witness only.
        f = Facts(); f.InstalledVersion = "0.7.200.100-tester.12"; f.Phase = "driver-pending-restart"; f.StatePackageVersion = "0.7.200.100-tester.12";
        v = DriverCard.Decide(f);
        Check(v.InstalledPending && v.InstalledText == "Installed: 0.7.200.100-tester.12, waiting for the restart of Windows.", "G-VER installed pending: " + v.InstalledText);
        Equal("0.7.199.100-tester.11", v.RunningVersion, "G-VER running stays from the witness");
        Equal(VerifyKind.Unknown, v.Verification, "G-VER a report of the running package does not verify the pending one");
        f.Witness = null; v = DriverCard.Decide(f);
        Check(v.Running != RunningKind.Exact && !v.RunningText.Contains("0.7.200"), "G-VER Release\\Version never used as running");

        // No mapping, one candidate without a witness: unknown (497.3).
        f = Facts(); f.Witness = null; f.Packages.Clear(); f.Mapping = MappingStatus.Absent;
        Equal(RunningKind.Unknown, DriverCard.Decide(f).Running, "G-VER no package for the reply -> unknown");
        f = Facts(); f.Witness = null;
        Equal(RunningKind.Unknown, DriverCard.Decide(f).Running, "497.3 one candidate without a witness is unknown");

        // R4: every binding of the witness.
        Action<string, Action<DriverFacts>> notExact = (what, change) => { var x = Facts(); change(x); var d = DriverCard.Decide(x); Check(d.Running != RunningKind.Exact, "R4 " + what + ": " + d.ReportText); };
        notExact("a witness of an earlier boot", x => x.Witness.BootId = 52);
        notExact("an unreadable current boot", x => x.BootId = null);
        notExact("an install action after the witness in this boot", x => x.InstallActions.Add(new InstallAction { BootId = 53, Utc = "2026-10-04T09:00:00.000Z" }));
        notExact("an install action of this boot without a time", x => x.InstallActions.Add(new InstallAction { BootId = 53 }));
        notExact("the reply ABI differs", x => x.Witness.KmdAbi = "0x000700C6");
        notExact("no driver reply", x => x.ReplyAbi = null);
        notExact("a manifest SHA256 no package has", x => x.Witness.ManifestSha256 = new string('F', 64));
        notExact("a release name of another package", x => x.Witness.Release = "amdgpu-wddm-tester-other");
        notExact("a version of another package", x => x.Witness.Version = "0.7.199.101-tester.12");
        notExact("a full build the manifest does not have", x => { x.Witness.KmdBuild = "0.7.199.2"; x.Image.KmdBuild = "0.7.199.2"; });
        notExact("an image hash the manifest does not have", x => { x.Witness.KmdImageSha256 = ShaI2; x.Image.Sha256 = ShaI2; });
        notExact("the loaded image differs from the witness", x => x.Image.Sha256 = ShaI2);
        notExact("the loaded image build differs", x => x.Image.KmdBuild = "0.7.199.0");
        notExact("no loaded-image evidence", x => x.Image = null);
        notExact("the image evidence is not of the started device", x => x.Image.OfStartedDevice = false);
        notExact("the image evidence is of another boot", x => x.Image.BootId = 52);
        notExact("an incomplete mapping", x => x.Mapping = MappingStatus.Incomplete);
        notExact("a witness without a manifest hash", x => x.Witness.ManifestSha256 = null);
        notExact("a witness without a time", x => x.Witness.RecordedUtc = null);
        notExact("a witness with a three-part build", x => { x.Witness.KmdBuild = "0.7.199"; x.Image.KmdBuild = "0.7.199"; x.Packages[0].KmdBuild = "0.7.199"; });
        notExact("a writer other than verify or start-confirm", x => x.Witness.RecordedBy = "someone");
        notExact("a second, different manifest of the same release (conflicting mapping)", x => { x.Packages.Add(Pkg("0.7.199.100-tester.11", ShaM2, ShaI)); x.Mapping = DriverCard.Mapping(2, x.Packages); });
        f = Facts(); f.InstallActions.Add(new InstallAction { BootId = 53, Utc = "2026-10-04T07:00:00.000Z" });
        Equal(RunningKind.Exact, DriverCard.Decide(f).Running, "G-VER an install before the witness keeps it");
        f = Facts(); f.InstallActions.Add(new InstallAction { BootId = 52, Utc = "2026-10-04T09:00:00.000Z" });
        Equal(RunningKind.Exact, DriverCard.Decide(f).Running, "G-VER an install of an earlier boot does not void");
        Equal((uint?)199, DriverCard.ParseAbi("199"), "kmd_abi decimal"); Check(DriverCard.ParseAbi("0xZZ") == null, "kmd_abi junk");

        // R5 / 497.8: verification only from a valid report of the installed package; the phase never decides it.
        f = Facts(); f.Reports[0].Passed = false; f.Reports[0].FailedCount = 1;
        Equal("Verification: not verified (a check failed).", DriverCard.Decide(f).VerificationText, "G-VER a matching failed report");
        f = Facts(); f.Reports.Clear();
        Equal("Verification: could not be checked.", DriverCard.Decide(f).VerificationText, "R5 phase 'verified' without a report is not verified");
        f = Facts(); f.Phase = "verify-failed";
        Equal(VerifyKind.Verified, DriverCard.Decide(f).Verification, "R5 the phase does not override a matching passed report");
        f = Facts(); f.Reports[0].PackageVersion = "0.7.198.100-tester.10";
        Equal(VerifyKind.Unknown, DriverCard.Decide(f).Verification, "R5 a report of another version does not count");
        f = Facts(); f.Reports[0].ManifestSha256 = ShaM2;
        Equal(VerifyKind.Unknown, DriverCard.Decide(f).Verification, "R5 a report of another manifest with the same version does not count");
        f = Facts(); f.Reports[0].Valid = false;
        Equal(VerifyKind.Unknown, DriverCard.Decide(f).Verification, "R5 an invalid report does not count");
        f = Facts(); f.InstalledManifestSha256 = null;
        Equal(VerifyKind.Unknown, DriverCard.Decide(f).Verification, "R5 an unreadable installed manifest: could not be checked");
        f = Facts(); f.Reports.Add(new VerifyReport { Valid = true, Passed = false, Utc = "2026-10-04T09:00:00Z", PackageVersion = "0.7.199.100-tester.11", ManifestSha256 = ShaM, FailedCount = 1 });
        Equal(VerifyKind.Failed, DriverCard.Decide(f).Verification, "R5 the newest valid report decides");

        // Release\Version missing (497.7): installed unknown, never the state's package version; verification unknown.
        f = Facts(); f.InstalledVersion = null;
        v = DriverCard.Decide(f);
        Check(v.InstalledText == "Installed version: not known." && v.Verification == VerifyKind.Unknown && v.InstalledVersion == null, "497.7 Release\\Version missing");
        Check(v.ReportText.Contains("installer state verified for 0.7.199.100-tester.11"), "497.7 the state's package version goes to the report only");
        // 936: Release\Version and InstallDir are REG_SZ only; any other kind is unknown.
        Equal("0.7.199.100-tester.11", DriverCard.SzOnly(Microsoft.Win32.RegistryValueKind.String, "0.7.199.100-tester.11"), "Release\\Version REG_SZ is read");
        Equal("", DriverCard.SzOnly(Microsoft.Win32.RegistryValueKind.ExpandString, "0.7.199.100-tester.11"), "Release\\Version REG_EXPAND_SZ is unknown");
        Equal("", DriverCard.SzOnly(Microsoft.Win32.RegistryValueKind.MultiString, new[] { "0.7.199.100-tester.11" }), "Release\\Version REG_MULTI_SZ is unknown");
        Equal("", DriverCard.SzOnly(Microsoft.Win32.RegistryValueKind.DWord, 199), "Release\\Version REG_DWORD is unknown");
        Equal("", DriverCard.SzOnly(Microsoft.Win32.RegistryValueKind.Binary, new byte[] { 0x30 }), "Release\\Version REG_BINARY is unknown");
        f = Facts(); f.StatePackageVersion = "0.7.200.100-tester.12"; f.Phase = "installed";
        Check(!DriverCard.Decide(f).InstalledPending, "a phase of another package does not mark the installed one pending");
        f = Facts(); f.Phase = "install-incomplete";
        Check(DriverCard.Decide(f).InstallStopped, "install stopped");

        // Parsers.
        Check(DriverCard.ParseWitness("{\"schema\":2,\"boot_id\":53,\"recorded_by\":\"verify\"}") == null, "witness schema 2 refused");
        Check(DriverCard.ParseWitness("{not json") == null, "damaged witness refused");
        Check(DriverCard.ParseWitness("{\"schema\":1,\"boot_id\":53,\"recorded_by\":\"someone\"}") == null, "witness of an unknown recorder refused");
        var w = DriverCard.ParseWitness("{\"schema\":1,\"boot_id\":53,\"recorded_utc\":\"2026-10-04T08:00:00.000Z\",\"recorded_by\":\"start-confirm\",\"release\":\"r\",\"version\":\"0.7.199.100-tester.11\",\"manifest_sha256\":\"a\",\"kmd_image_sha256\":\"b\",\"kmd_build\":\"0.7.199.1\",\"kmd_abi\":\"0x000700C7\"}");
        Check(w != null && w.BootId == 53 && w.KmdAbi == "0x000700C7" && w.RecordedBy == "start-confirm", "witness parsed");
        Equal("manifest_sha256", DriverCard.WitnessIncomplete(w), "R4 a witness with a short hash is incomplete");
        Func<string, string> report = tail => "{\"schema\":\"amdgpu-wddm.verify-report/1\",\"utc\":\"2026-10-04T08:00:00Z\",\"dry_run\":false,\"package_version\":\"x\",\"manifest_sha256\":\"" + ShaM + "\"," + tail + "}";
        var r = DriverCard.ParseVerify(report("\"outcome\":\"passed\",\"complete\":true,\"results\":[{\"pass\":true},{\"pass\":true}]"));
        Check(r.Valid && r.Passed && r.PassedCount == 2 && r.ManifestSha256 == ShaM, "verify report: a passed run");
        r = DriverCard.ParseVerify(report("\"outcome\":\"failed\",\"complete\":true,\"results\":[{\"pass\":true},{\"pass\":false}]"));
        Check(r.Valid && !r.Passed && r.FailedCount == 1, "verify report: a failed run");
        Check(!DriverCard.ParseVerify(report("\"outcome\":\"passed\",\"complete\":true,\"results\":[]")).Valid, "R5 an empty result list is not a valid report");
        Check(!DriverCard.ParseVerify(report("\"outcome\":\"passed\",\"complete\":false,\"results\":[{\"pass\":true}]")).Valid, "R5 'passed' without complete is not valid");
        Check(!DriverCard.ParseVerify(report("\"outcome\":\"passed\",\"complete\":true,\"results\":[{\"pass\":true},{\"pass\":false}]")).Valid, "R5 'passed' with a failed result is not valid");
        Check(!DriverCard.ParseVerify(report("\"outcome\":\"passed\",\"complete\":true,\"results\":[{\"name\":\"no flag\"}]")).Valid, "R5 a result without pass is not valid");
        Check(!DriverCard.ParseVerify(report("\"outcome\":\"passed\",\"complete\":true,\"results\":[{\"pass\":true}]").Replace("\"dry_run\":false", "\"dry_run\":true")).Valid, "R5 a dry run is not a verification");
        Check(!DriverCard.ParseVerify(report("\"outcome\":\"passed\",\"complete\":true,\"results\":[{\"pass\":true}]").Replace("verify-report/1", "verify-report/2")).Valid, "R5 another schema is not valid");
        var good = report("\"outcome\":\"passed\",\"complete\":true,\"manifest_source\":\"install-root\",\"results\":[{\"pass\":true}]");
        Check(DriverCard.ParseVerify(good).Valid, "verify report: manifest_source install-root is valid");
        Check(!DriverCard.ParseVerify(good.Replace("\"schema\":\"amdgpu-wddm.verify-report/1\",", "")).Valid, "a report without schema is not valid");
        Check(!DriverCard.ParseVerify(good.Replace("\"dry_run\":false,", "")).Valid, "a report without dry_run is not valid");
        Check(!DriverCard.ParseVerify(good.Replace("\"outcome\":\"passed\"", "\"outcome\":\"ok\"")).Valid, "an outcome other than passed or failed is not valid");
        Check(!DriverCard.ParseVerify(good.Replace("\"complete\":true,", "")).Valid, "'passed' without a complete flag is not valid");
        Check(!DriverCard.ParseVerify(good.Replace(ShaM, ShaM.ToLowerInvariant())).Valid, "a lowercase manifest_sha256 is not valid (64 uppercase hex)");
        Check(!DriverCard.ParseVerify(good.Replace(ShaM, ShaM.Substring(1))).Valid, "a short manifest_sha256 is not valid");
        Check(!DriverCard.ParseVerify(good.Replace("\"manifest_sha256\":\"" + ShaM + "\",", "")).Valid, "a report without manifest_sha256 is not valid");
        Check(!DriverCard.ParseVerify(good.Replace("install-root", "package")).Valid, "manifest_source package never binds the installed release");
        var fv = Facts(); fv.Reports.Clear(); fv.InstalledManifestSha256 = ShaM.ToLowerInvariant();
        var bound = DriverCard.ParseVerify(good); bound.PackageVersion = fv.InstalledVersion; fv.Reports.Add(bound);
        Check(DriverCard.InstalledReport(fv) == null, "the report's manifest_sha256 binds the installed manifest by ordinal comparison");
        fv.InstalledManifestSha256 = ShaM;
        Check(DriverCard.InstalledReport(fv) == bound, "the same manifest_sha256 binds");
        var fs = new DriverFacts();
        DriverCard.ParseState("{\"phase\":\"installed\",\"package_version\":\"x\",\"mutation_boot_id\":53,\"mutation_utc\":\"2026-10-04T09:00:00Z\"}", fs);
        Check(fs.Phase == "installed" && fs.StatePackageVersion == "x" && fs.InstallActions.Count == 1 && fs.InstallActions[0].BootId == 53, "state.json with the mutation fields");
        var manifest = "{\"name\":\"n\",\"version\":\"0.7.199.100-tester.11\",\"kmd_build\":\"0.7.199.1\",\"kmd_abi\":\"0x000700C7\",\"kmd_version\":\"0.7.199.100\",\"components\":[{\"package_path\":\"payload/kmd/bc250kmd.sys\",\"sha256\":\"" + ShaI + "\"}]}";
        var pk = DriverCard.ParsePackage(manifest, ShaM, "m");
        Check(pk != null && pk.Name == "n" && pk.ImageSha256 == ShaI && pk.KmdBuild == "0.7.199.1", "manifest parsed as a known package");
        Check(DriverCard.ParsePackage(manifest.Replace("payload/kmd/bc250kmd.sys", "payload/other.sys"), ShaM, "m") == null, "a manifest without the KMD image hash is not a known package");
        Equal(MappingStatus.Absent, DriverCard.Mapping(0, new List<KnownPackage>()), "mapping absent");
        Equal(MappingStatus.Incomplete, DriverCard.Mapping(2, new List<KnownPackage> { pk, null }), "mapping incomplete");
        Equal(MappingStatus.Complete, DriverCard.Mapping(2, new List<KnownPackage> { pk, Pkg("v2", ShaM2, ShaI2) }), "mapping complete");
        Equal(MappingStatus.Conflicting, DriverCard.Mapping(2, new List<KnownPackage> { Pkg("v", ShaM, ShaI), Pkg("v", ShaM2, ShaI2) }), "mapping conflicting: one release, two manifests");
        Equal(MappingStatus.Conflicting, DriverCard.Mapping(2, new List<KnownPackage> { Pkg("v", ShaM, ShaI), Pkg("w", ShaM2, ShaI, "0.7.199.2") }), "mapping conflicting: one image, two builds");
        Equal("0.7.199.1", DriverCard.BuildOfImage(ShaI.ToLowerInvariant(), new List<KnownPackage> { Pkg("v", ShaM, ShaI) }), "the build of a loaded image comes from the manifests");
        Check(DriverCard.BuildOfImage(ShaI2, new List<KnownPackage> { Pkg("v", ShaM, ShaI) }) == null, "an image no manifest knows has no build");

        // R6 / 497.5: "upgrade done" needs an exact witness of this boot naming the installed release.
        v = DriverCard.Decide(Facts());
        Check(DriverCard.UpgradeDone(v, "0.7.198.100-tester.10") && !DriverCard.UpgradeDone(v, "0.7.199.100-tester.11") && !DriverCard.UpgradeDone(v, null), "upgrade done only after verified completion of a new release");
        f = Facts(); f.ReplyAbi = null;
        Check(!DriverCard.UpgradeDone(DriverCard.Decide(f), "0.7.198.100-tester.10"), "R6 no reply: no upgrade-done");
        f = Facts(); f.Witness.BootId = 52;
        Check(!DriverCard.UpgradeDone(DriverCard.Decide(f), "0.7.198.100-tester.10"), "R6 a stale witness: no upgrade-done");
        f = Facts(); f.Witness = null;
        Check(!DriverCard.UpgradeDone(DriverCard.Decide(f), "0.7.198.100-tester.10"), "R6 an unknown witness: no upgrade-done");
        f = Facts(); f.InstalledVersion = "0.7.200.100-tester.12";
        f.Reports.Add(new VerifyReport { Valid = true, Passed = true, Utc = "2026-10-04T09:00:00Z", PackageVersion = "0.7.200.100-tester.12", ManifestSha256 = ShaM, PassedCount = 3 });
        v = DriverCard.Decide(f);
        Check(v.Running == RunningKind.Exact && v.Verification == VerifyKind.Verified && !DriverCard.UpgradeDone(v, "0.7.198.100-tester.10"), "R6 an exact witness naming an older package: no upgrade-done");
        f = Facts(); f.Phase = "installed";
        Check(!DriverCard.UpgradeDone(DriverCard.Decide(f), "0.7.198.100-tester.10"), "no upgrade-done while the restart is pending");
        UpgradeLatchTests();
        LoadedImageTests();
        ProbeTests();
    }

    // 936 A6: the witness's writer recorded the loaded image in this boot; the app's own list is a cross-check only
    // where Windows gives it the image bases.
    static void LoadedImageTests()
    {
        var f = Facts(); f.Image = new LoadedImage { BasesHidden = true, Detail = "Windows lists the loaded drivers without their image bases" };
        var v = DriverCard.Decide(f);
        Equal(RunningKind.Exact, v.Running, "A6 null bases from the app: the witness of this boot stands: " + v.ReportText);
        Check(v.ReportText.Contains("null driver bases"), "A6 null bases are named in the report");
        f.Witness.BootId = 52;
        Check(DriverCard.Decide(f).Running != RunningKind.Exact, "A6 null bases do not excuse a witness of another boot");
        Equal(RunningKind.Exact, DriverCard.Decide(Facts()).Running, "A6 matching bases: the cross-check agrees");
        f = Facts(); f.Image.Sha256 = ShaI2;
        v = DriverCard.Decide(f);
        Check(v.Running != RunningKind.Exact && v.ReportText.Contains("the loaded image has SHA256"), "A6 mismatching bases fail: " + v.ReportText);
        f = Facts(); f.Image = new LoadedImage { Detail = "bc250kmd.sys is not among the loaded drivers" };
        Check(DriverCard.Decide(f).Running != RunningKind.Exact, "A6 bases visible but no bc250kmd.sys among them: not exact");

        bool hidden;
        var names = DriverCardProbe.DriverNames(new[] { IntPtr.Zero, IntPtr.Zero, IntPtr.Zero }, 3, b => "x", out hidden);
        Check(hidden && names.Count == 0, "A6 all-null bases (24H2 without SeDebugPrivilege) are hidden");
        names = DriverCardProbe.DriverNames(new[] { new IntPtr(0x1000), new IntPtr(0x2000) }, 2, b => b.ToInt64() == 0x1000 ? @"\SystemRoot\System32\ntoskrnl.exe" : @"\SystemRoot\System32\drivers\bc250kmd.sys", out hidden);
        Check(!hidden && names.Count == 2, "A6 visible bases are listed by name");
        names = DriverCardProbe.DriverNames(new[] { new IntPtr(0x1000), new IntPtr(0x2000) }, 2, b => b.ToInt64() == 0x1000 ? "ntoskrnl.exe" : null, out hidden);
        Check(hidden && names.Count == 1, "A6 a base without a name proves nothing either");
    }

    // The installer's files for the probe tests: content per path, directories that cannot be listed.
    sealed class FakeInstallerFiles : IInstallerFiles
    {
        public readonly Dictionary<string, FileBytes> Content = new Dictionary<string, FileBytes>(StringComparer.OrdinalIgnoreCase);
        public readonly List<string> Dirs = new List<string>();
        public readonly HashSet<string> Unlistable = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        public void Put(string path, string text) { Content[path] = new FileBytes { Status = FileStatus.Ok, Bytes = System.Text.Encoding.UTF8.GetBytes(text) }; }
        public void Put(string path, FileStatus status) { Content[path] = new FileBytes { Status = status, Error = status.ToString() }; }

        public FileBytes Read(string path, int max)
        {
            FileBytes b;
            return Content.TryGetValue(path, out b) ? b : new FileBytes { Status = FileStatus.Absent };
        }

        public string[] Directories(string dir)
        {
            if (Unlistable.Contains(dir)) throw new UnauthorizedAccessException("access denied");
            return Dirs.Where(d => string.Equals(System.IO.Path.GetDirectoryName(d), dir, StringComparison.OrdinalIgnoreCase)).ToArray();
        }

        public string[] Files(string dir, string pattern)
        {
            if (Unlistable.Contains(dir)) throw new UnauthorizedAccessException("access denied");
            return Content.Keys.Where(p => string.Equals(System.IO.Path.GetDirectoryName(p), dir, StringComparison.OrdinalIgnoreCase) &&
                System.IO.Path.GetFileName(p).StartsWith("verify-", StringComparison.OrdinalIgnoreCase) && p.EndsWith(".json", StringComparison.OrdinalIgnoreCase)).ToArray();
        }
    }

    const string Inst = @"X:\ProgramData\amdgpu-wddm\installer", AppDir = @"X:\Program Files\amdgpu-wddm";

    static string Sha256Hex(string text)
    {
        using (var sha = System.Security.Cryptography.SHA256.Create()) return BitConverter.ToString(sha.ComputeHash(System.Text.Encoding.UTF8.GetBytes(text))).Replace("-", "");
    }

    static string VerifyJson(string version, string manifestSha, string outcome = "passed", bool dry = false)
    {
        return "{\"schema\":\"amdgpu-wddm.verify-report/1\",\"utc\":\"2026-10-04T08:00:00Z\",\"dry_run\":" + (dry ? "true" : "false") + ",\"package_version\":\"" + version +
            "\",\"manifest_sha256\":\"" + manifestSha + "\",\"manifest_source\":\"install-root\",\"outcome\":\"" + outcome + "\",\"complete\":true,\"results\":[{\"pass\":" +
            (outcome == "passed" ? "true" : "false") + "}]}";
    }

    // A whole installer: the installed manifest, a kept copy of it, the witness, state.json and one passed report.
    static FakeInstallerFiles Installed(out string manifestSha)
    {
        const string B = "0.7.199.100-tester.11";
        var manifest = "{\"name\":\"amdgpu-wddm-tester-" + B + "\",\"version\":\"" + B + "\",\"kmd_build\":\"0.7.199.1\",\"kmd_abi\":\"0x000700C7\",\"kmd_version\":\"0.7.199.100\",\"components\":[{\"package_path\":\"payload/kmd/bc250kmd.sys\",\"sha256\":\"" + ShaI + "\"}]}";
        manifestSha = Sha256Hex(manifest);
        var files = new FakeInstallerFiles();
        files.Put(AppDir + @"\manifest.json", manifest);
        files.Dirs.Add(Inst + @"\packages\" + B);
        files.Put(Inst + @"\packages\" + B + @"\manifest.json", manifest);
        files.Put(Inst + @"\running-release.json", "{\"schema\":1,\"boot_id\":53,\"recorded_utc\":\"2026-10-04T08:00:00.000Z\",\"recorded_by\":\"verify\",\"release\":\"amdgpu-wddm-tester-" + B +
            "\",\"version\":\"" + B + "\",\"manifest_sha256\":\"" + manifestSha + "\",\"kmd_image_sha256\":\"" + ShaI + "\",\"kmd_build\":\"0.7.199.1\",\"kmd_abi\":\"0x000700C7\"}");
        files.Put(Inst + @"\state.json", "{\"phase\":\"verified\",\"package_version\":\"" + B + "\"}");
        files.Put(Inst + @"\verify\verify-20261004T080000000Z.json", VerifyJson(B, manifestSha));
        return files;
    }

    static DriverCardView Probe(FakeInstallerFiles files, ProbeBudget budget = null) { DriverFacts f; return Probe(files, out f, budget); }

    static DriverCardView Probe(FakeInstallerFiles files, out DriverFacts f, ProbeBudget budget = null)
    {
        f = DriverCardProbe.Read(files, Inst, Reply199, 53, "0.7.199.100-tester.11", AppDir, "10-3-2026", budget ?? new ProbeBudget());
        f.Image = new LoadedImage { Available = true, OfStartedDevice = true, BootId = 53, Sha256 = ShaI, KmdBuild = DriverCard.BuildOfImage(ShaI, f.Packages) };
        return DriverCard.Decide(f);
    }

    // 936 A2 (discovery failure is incomplete) and A3 (the newest valid report of the installed package, not the
    // newest twenty files), through DriverCardProbe.Read over injected files.
    static void ProbeTests()
    {
        const string B = "0.7.199.100-tester.11";
        string sha;
        DriverFacts f;
        var v = Probe(Installed(out sha), out f);
        Check(v.Running == RunningKind.Exact && v.Verification == VerifyKind.Verified && f.Mapping == MappingStatus.Complete, "probe: a whole installer reads exact and verified: " + v.ReportText);

        // A2
        var files = Installed(out sha); files.Unlistable.Add(Inst + @"\packages");
        v = Probe(files, out f);
        Check(f.Mapping == MappingStatus.Incomplete && v.Running != RunningKind.Exact && f.MappingNote.Contains("cannot be listed"),
            "A2 an unreadable packages directory leaves the mapping incomplete, with a readable installed manifest and a valid witness: " + v.ReportText);
        files = Installed(out sha); files.Dirs.Add(Inst + @"\packages\other"); files.Put(Inst + @"\packages\other\manifest.json", FileStatus.Failed);
        v = Probe(files, out f);
        Check(f.Mapping == MappingStatus.Incomplete && v.Running != RunningKind.Exact, "A2 a discovered package whose manifest cannot be read: incomplete, not dropped");
        files = Installed(out sha); files.Dirs.Add(Inst + @"\packages\other"); files.Put(Inst + @"\packages\other\manifest.json", FileStatus.Untrusted);
        Check(Probe(files).Running != RunningKind.Exact, "A2 a kept manifest with an untrusted owner: incomplete");
        files = Installed(out sha); files.Dirs.Add(Inst + @"\packages\empty");
        v = Probe(files, out f);
        Check(f.Mapping == MappingStatus.Complete && v.Running == RunningKind.Exact, "A2 a package directory known to have no manifest is no package");
        files = Installed(out sha); files.Put(AppDir + @"\manifest.json", FileStatus.Failed);
        v = Probe(files, out f);
        Check(f.Mapping == MappingStatus.Incomplete && f.InstalledManifestSha256 == null && v.Verification == VerifyKind.Unknown, "A2 an unreadable installed manifest: incomplete, verification unknown");
        files = Installed(out sha); files.Content.Remove(Inst + @"\packages\" + B + @"\manifest.json"); files.Dirs.Clear();
        Check(Probe(files).Running == RunningKind.Exact, "A2 no kept packages at all is a known absence");
        Equal(MappingStatus.Incomplete, DriverCard.Mapping(0, new List<KnownPackage>(), false), "A2 failed discovery with nothing found is incomplete, not absent");

        // state.json that exists but cannot be read may hide an install action of this boot.
        files = Installed(out sha); files.Put(Inst + @"\state.json", FileStatus.Failed);
        Check(Probe(files).Running != RunningKind.Exact, "an unreadable state.json voids the witness of this boot");
        files = Installed(out sha); files.Put(Inst + @"\state.json", "{damaged");
        Check(Probe(files).Running != RunningKind.Exact, "a damaged state.json voids the witness of this boot");
        files = Installed(out sha); files.Content.Remove(Inst + @"\state.json");
        Check(Probe(files).Running == RunningKind.Exact, "no state.json: no install action recorded");

        // A3: a matching report behind 25 newer nonmatching or invalid ones.
        files = Installed(out sha);
        for (int i = 0; i < 25; i++)
        {
            var name = Inst + @"\verify\verify-20261004T09" + i.ToString("00") + "00000Z.json";
            switch (i % 5)
            {
                case 0: files.Put(name, VerifyJson(B, sha, dry: true)); break;
                case 1: files.Put(name, VerifyJson("0.7.198.100-tester.10", sha)); break;
                case 2: files.Put(name, VerifyJson(B, ShaM2)); break;
                case 3: files.Put(name, "{not json"); break;
                default: files.Put(name, i % 2 == 0 ? FileStatus.TooLarge : FileStatus.Untrusted); break;
            }
        }
        v = Probe(files, out f);
        Check(v.Verification == VerifyKind.Verified && f.ReportSearchNote == null, "A3 the matching report behind 25 newer nonmatching or invalid ones is found: " + v.ReportText);
        v = Probe(files, out f, new ProbeBudget { MaxReports = 10 });
        Check(v.Verification == VerifyKind.Unknown && f.ReportSearchNote != null && v.ReportText.Contains("report search incomplete"), "A3 a search cut by its bound is incomplete: " + v.ReportText);
        files.Put(Inst + @"\verify\verify-20261004T100000000Z.json", VerifyJson(B, sha, "failed"));
        Equal(VerifyKind.Failed, Probe(files).Verification, "A3 the newest report of the installed package decides");
        files.Put(Inst + @"\verify\verify-20261004T110000000Z.json", FileStatus.Failed);
        v = Probe(files, out f);
        Check(v.Verification == VerifyKind.Unknown && f.ReportSearchNote.Contains("is not read"), "A3 a newer report that cannot be read ends the search as incomplete");
        files = Installed(out sha); files.Unlistable.Add(Inst + @"\verify");
        v = Probe(files, out f);
        Check(v.Verification == VerifyKind.Unknown && f.ReportSearchNote != null, "A3 a verify directory that cannot be listed: could not be checked");
    }

    // 936 A1: consecutive refreshes as MainForm.RefreshAll runs them; the verdict holds only while its evidence does.
    static void UpgradeLatchTests()
    {
        const string A = "0.7.198.100-tester.10", B = "0.7.199.100-tester.11";
        Func<string, DriverCardView, bool> shown = (latch, d) => HomeStatus.Compute(new StatusInputs { Snapshot = WithDefaults(), Driver = d, UpgradeDone = latch })
            .Items.Any(i => i.Cause == GuideCause.UpgradeDone);
        Action<string, Action<DriverFacts>> refreshes = (what, second) =>
        {
            string seen = A, latch = null;
            var d = DriverCard.Decide(Facts());
            latch = DriverCard.UpgradeLatch(latch, d, seen);
            if (DriverCard.UpgradeVerified(d)) seen = d.InstalledVersion;
            Check(latch == B && shown(latch, d), "A1 " + what + ": B qualifies on the first refresh");
            d = DriverCard.Decide(Facts());
            Check(DriverCard.UpgradeLatch(latch, d, seen) == B, "A1 " + what + ": B unchanged keeps the verdict on the next refresh");
            var f = Facts(); second(f); d = DriverCard.Decide(f);
            latch = DriverCard.UpgradeLatch(latch, d, seen);
            if (DriverCard.UpgradeVerified(d)) seen = d.InstalledVersion;
            Check(latch == null && !shown(latch, d), "A1 " + what + ": the verdict is gone");
            d = DriverCard.Decide(Facts());
            Check(DriverCard.UpgradeLatch(latch, d, seen) == null, "A1 " + what + ": a cleared verdict does not come back for the release already seen");
        };
        refreshes("B, then the witness becomes unknown", f => f.Witness = null);
        refreshes("B, then verification fails", f => { f.Reports[0].Passed = false; f.Reports[0].FailedCount = 1; });
        refreshes("B, then C is pending", f => { f.InstalledVersion = "0.7.200.100-tester.12"; f.Phase = "installed"; f.StatePackageVersion = "0.7.200.100-tester.12"; });
        // A stale latch handed to HomeStatus directly is not shown either.
        var stale = Facts(); stale.Witness = null;
        Check(!shown(B, DriverCard.Decide(stale)), "A1 HomeStatus shows upgrade-done only while UpgradeVerified holds for the latched release");
        Check(!shown("0.7.198.100-tester.10", DriverCard.Decide(Facts())), "A1 HomeStatus: a latch of another release is not shown");
    }
}
