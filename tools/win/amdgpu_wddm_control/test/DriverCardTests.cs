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
    }
}
