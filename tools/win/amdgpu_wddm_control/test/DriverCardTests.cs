// G-VER (plan v7 WU-043, F-VER): the running release only from a witness of this boot; ambiguity never resolved to
// the newer package; the installed release pending a restart shown apart; verification outcomes; missing inputs.
using System;
using System.Collections.Generic;
using System.Linq;
using AmdgpuWddmControl;

static partial class UnitTests
{
    const uint Reply199 = 0x000700C7;     // KMD 0.7.199

    static DriverFacts Facts()
    {
        return new DriverFacts
        {
            ReplyVersion = Reply199, BootId = 53, InstalledVersion = "0.7.199.100-tester.11", DriverDate = "10-3-2026",
            Witness = new ReleaseWitness { BootId = 53, RecordedUtc = "2026-10-04T08:00:00.000Z", RecordedBy = "verify", Version = "0.7.199.100-tester.11", KmdBuild = "0.7.199.1", KmdAbi = "0x000700C7" },
            State = new InstallerState { Phase = "verified", PackageVersion = "0.7.199.100-tester.11" },
            Packages = new List<KnownPackage> { new KnownPackage { Version = "0.7.199.100-tester.11", KmdBuild = "0.7.199.1", KmdAbi = "0x000700C7" } },
            Reports = new List<VerifyReport> { new VerifyReport { Utc = "2026-10-04T08:00:00Z", PackageVersion = "0.7.199.100-tester.11", Passed = 9 } },
        };
    }

    static void DriverCardTests()
    {
        Strings.Language = "en";
        var v = DriverCard.Decide(Facts());
        Equal(RunningKind.Exact, v.Running, "G-VER witness of this boot -> exact");
        Equal("Running version: 0.7.199.100-tester.11", v.RunningText, "G-VER exact text");
        Equal(VerifyKind.Verified, v.Verification, "G-VER verified"); Equal("Driver date: 2026-10-03", v.DriverDateText, "G-VER INF date");

        // Two packages sharing the KMD identity, no witness: ambiguous, never the newer one.
        var f = Facts(); f.Witness = null;
        f.Packages.Add(new KnownPackage { Version = "0.7.199.101-tester.12", KmdBuild = "0.7.199.1", KmdAbi = "0x000700C7" });
        v = DriverCard.Decide(f);
        Equal(RunningKind.Ambiguous, v.Running, "G-VER two packages, one KMD identity -> ambiguous");
        Equal("Running version: cannot be determined exactly.", v.RunningText, "G-VER ambiguous text");
        Check(!v.RunningText.Contains("tester.12") && v.ReportText.Contains("0.7.199.101-tester.12") && v.ReportText.Contains("0.7.199.100-tester.11"), "G-VER candidates only in the report");

        // A newer installed package pending the restart: installed shown pending; running from the witness only.
        f = Facts(); f.InstalledVersion = "0.7.200.100-tester.12"; f.State = new InstallerState { Phase = "driver-pending-restart", PackageVersion = "0.7.200.100-tester.12" };
        v = DriverCard.Decide(f);
        Check(v.InstalledPending && v.InstalledText == "Installed: 0.7.200.100-tester.12, waiting for the restart of Windows.", "G-VER installed pending: " + v.InstalledText);
        Equal("0.7.199.100-tester.11", v.RunningVersion, "G-VER running stays from the witness");
        f.Witness = null; v = DriverCard.Decide(f);
        Check(v.Running != RunningKind.Exact && !v.RunningText.Contains("0.7.200"), "G-VER Release\\Version never used as running");

        // No or ambiguous manifest mapping -> unknown.
        f = Facts(); f.Witness = null; f.Packages.Clear();
        Equal(RunningKind.Unknown, DriverCard.Decide(f).Running, "G-VER no package for the reply -> unknown");
        f = Facts(); f.Witness = null;
        Equal(RunningKind.Unwitnessed, DriverCard.Decide(f).Running, "G-VER one candidate without a witness is not exact");
        Equal("Running version: cannot be determined exactly.", DriverCard.Decide(f).RunningText, "G-VER unwitnessed text");
        f.Packages[0].KmdBuild = "0.7.198.1";
        Equal(RunningKind.Unknown, DriverCard.Decide(f).Running, "G-VER a package of another build does not map");

        // A witness from an earlier boot -> ignored.
        f = Facts(); f.Witness.BootId = 52;
        Check(DriverCard.Decide(f).Running != RunningKind.Exact, "G-VER witness of an earlier boot ignored");
        // An install action after the witness in this boot -> void.
        f = Facts(); f.State.MutationBootId = 53; f.State.MutationUtc = "2026-10-04T09:00:00.000Z";
        Check(DriverCard.Decide(f).Running != RunningKind.Exact && DriverCard.Decide(f).ReportText.Contains("install action"), "G-VER install after the witness voids it");
        f.State.MutationUtc = "2026-10-04T07:00:00.000Z";
        Equal(RunningKind.Exact, DriverCard.Decide(f).Running, "G-VER install before the witness keeps it");
        f.State.MutationBootId = 52; f.State.MutationUtc = "2026-10-04T09:00:00.000Z";
        Equal(RunningKind.Exact, DriverCard.Decide(f).Running, "G-VER an install of an earlier boot does not void");
        // Witness of another KMD identity.
        f = Facts(); f.Witness.KmdAbi = "0x000700C6";
        Check(DriverCard.Decide(f).Running != RunningKind.Exact, "G-VER witness abi mismatch");
        f = Facts(); f.Witness.KmdBuild = "0.7.198.1";
        Check(DriverCard.Decide(f).Running != RunningKind.Exact, "G-VER witness build mismatch");
        Equal((uint?)199, DriverCard.ParseAbi("199"), "kmd_abi decimal"); Check(DriverCard.ParseAbi("0xZZ") == null, "kmd_abi junk");

        // Verification failed / unknown.
        f = Facts(); f.State.Phase = "verify-failed";
        Equal("Verification: not verified (a check failed).", DriverCard.Decide(f).VerificationText, "G-VER verification failed");
        f = Facts(); f.State = null; f.Reports.Clear();
        Equal("Verification: could not be checked.", DriverCard.Decide(f).VerificationText, "G-VER verification unknown");
        f = Facts(); f.State.Phase = "installed"; f.Reports[0].Failed = 1;
        Equal(VerifyKind.Failed, DriverCard.Decide(f).Verification, "G-VER a failed check in the newest report");
        f = Facts(); f.Reports[0].PackageVersion = "0.7.198.100-tester.10"; f.State.Phase = "installed";
        Equal(VerifyKind.Unknown, DriverCard.Decide(f).Verification, "G-VER a report of another version does not count");

        // No driver reply.
        f = Facts(); f.ReplyVersion = null;
        v = DriverCard.Decide(f);
        Check(v.Running == RunningKind.NoDriver && v.RunningText == "Running version: the driver is not running.", "G-VER no driver reply");
        // Release\Version missing.
        f = Facts(); f.InstalledVersion = null;
        v = DriverCard.Decide(f);
        Check(v.InstalledText == "Installed version: not known." && v.Verification == VerifyKind.Unknown, "G-VER Release\\Version missing");

        // Parsers: unknown schema, a damaged file and a wrong recorder are no witness.
        Check(DriverCard.ParseWitness("{\"schema\":2,\"boot_id\":53,\"recorded_by\":\"verify\"}") == null, "witness schema 2 refused");
        Check(DriverCard.ParseWitness("{not json") == null, "damaged witness refused");
        Check(DriverCard.ParseWitness("{\"schema\":1,\"boot_id\":53,\"recorded_by\":\"someone\"}") == null, "witness of an unknown recorder refused");
        var w = DriverCard.ParseWitness("{\"schema\":1,\"boot_id\":53,\"recorded_utc\":\"2026-10-04T08:00:00.000Z\",\"recorded_by\":\"start-confirm\",\"release\":\"r\",\"version\":\"0.7.199.100-tester.11\",\"manifest_sha256\":\"a\",\"kmd_image_sha256\":\"b\",\"kmd_build\":\"0.7.199.1\",\"kmd_abi\":\"0x000700C7\"}");
        Check(w != null && w.BootId == 53 && w.KmdAbi == "0x000700C7" && w.RecordedBy == "start-confirm", "witness parsed");
        var r = DriverCard.ParseVerify("{\"utc\":\"2026-10-04T08:00:00Z\",\"package_version\":\"x\",\"results\":[{\"pass\":true},{\"pass\":false},{\"name\":\"no flag\"}]}");
        Check(r.Passed == 1 && r.Failed == 2, "verify report: a result without pass is a failure");
        var st = DriverCard.ParseState("{\"phase\":\"installed\",\"package_version\":\"x\",\"mutation_boot_id\":53,\"mutation_utc\":\"2026-10-04T09:00:00Z\"}");
        Check(st.Phase == "installed" && st.MutationBootId == 53, "state.json with the mutation fields");

        // The one-time "upgrade done": verified, a new release, not on a first run.
        v = DriverCard.Decide(Facts());
        Check(DriverCard.UpgradeDone(v, "0.7.198.100-tester.10") && !DriverCard.UpgradeDone(v, "0.7.199.100-tester.11") && !DriverCard.UpgradeDone(v, null), "upgrade done only after verified completion of a new release");
        f = Facts(); f.State.Phase = "installed";
        Check(!DriverCard.UpgradeDone(DriverCard.Decide(f), "0.7.198.100-tester.10"), "no upgrade-done before verification");
    }
}
