// BD-138: a clock correction must not select an earlier boot's failed verify or retain a stale witness.
using System;
using System.Collections.Generic;
using System.Linq;
using AmdgpuWddmControl;

static partial class UnitTests
{
    static string TimedVerify(string version, string sha, string utc, string boot, string ticks, bool passed)
    {
        return VerifyJson(version, sha, passed ? "passed" : "failed").Replace("2026-10-04T08:00:00Z", utc)
            .Replace("\"dry_run\"", "\"boot_id\":" + boot + ",\"boot_elapsed_ms\":" + ticks + ",\"dry_run\"");
    }

    static void StaleVerifyTests()
    {
        var f = Facts();
        Func<string, string, string, bool, VerifyReport> make = (utc, boot, ticks, passed) =>
            DriverCard.ParseVerify(TimedVerify(f.InstalledVersion, ShaM, utc, boot, ticks, passed));
        // Synthetic fixture of the field report: one manifest, boots 5/7/9, a clock seven hours ahead in boot 5.
        var five = make("2026-10-13T07:39:34.016Z", "5", "null", false);
        var seven = make("2026-10-13T00:57:37.067Z", "7", "null", false);
        var nine = make("2026-10-13T01:40:16.845Z", "9", "null", true);
        foreach (var list in new[] { new[] { five, seven, nine }, new[] { nine, five, seven }, new[] { seven, nine, five } })
        {
            f.Reports = list.ToList();
            Check(DriverCard.InstalledReport(f) == nine, "BD-138 boot 9 wins over the fast-clock boot 5 in every input order");
            Equal(VerifyKind.Verified, DriverCard.Decide(f).Verification, "BD-138 the later passed verify clears the failed card");
        }
        var before = make("2026-10-13T08:00:00Z", "9", "1000", false);
        var after = make("2026-10-13T01:00:00Z", "9", "2000", true);
        f.Reports = new List<VerifyReport> { before, after };
        Check(DriverCard.InstalledReport(f) == after, "BD-138 uptime wins after an in-boot clock correction");
        after.Passed = false; before.Passed = true;
        Check(!DriverCard.InstalledReport(f).Passed, "BD-138 a later failure also wins after time moves backwards");
        var legacy = make("2099-10-13T23:00:00Z", "null", "999999", false);
        f.Reports.Add(legacy);
        Check(DriverCard.InstalledReport(f) == after, "BD-138 a report without BootId ranks below a known boot");
        before.BootElapsedMs = null;
        Check(DriverCard.InstalledReport(f) == before, "BD-138 a boot with legacy records falls back to UTC as one ordered set");
        f.Reports = new List<VerifyReport> { legacy, make("2026-10-13T01:00:00Z", "null", "null", true) };
        Check(DriverCard.InstalledReport(f) == legacy, "BD-138 reports without BootId retain legacy UTC order");
        foreach (string bad in new[] { "null", "-1", "4294967296", "\"9\"", "true", "1.5" })
            Check(make("2026-10-13T01:00:00Z", bad, "null", true).BootId == null, "BD-138 invalid boot is not ranked: " + bad);
        foreach (string bad in new[] { "null", "-1", "\"1000\"", "true", "1.5", "9223372036854775808" })
            Check(make("2026-10-13T01:00:00Z", "9", bad, true).BootElapsedMs == null, "BD-138 invalid elapsed value uses legacy ordering: " + bad);
        Check(make("2026-10-13T01:00:00Z", "0", "0", true).BootElapsedMs == 0, "BD-138 zero is a valid uptime");

        f = Facts(); f.Reports[0].Passed = false; f.Reports[0].FailedCount = 1; f.Reports[0].BootId = 52;
        var view = DriverCard.Decide(f);
        Equal(VerifyKind.EarlierBootFailed, view.Verification, "BD-138 exact installed release demotes an earlier-boot failure");
        Check(!DriverCard.UpgradeVerified(view) && DriverCard.UpgradeLatch(f.InstalledVersion, view, "older") == null,
            "BD-138 a confirmed start is not a passed full verify or an upgrade success");
        var card = HomeStatus.Compute(new StatusInputs { Snapshot = WithDefaults(), Driver = view });
        Check(!card.Items.Any(i => i.Cause == GuideCause.VerificationFailed) && card.Items.Any(i => i.Severity == "attention" && i.Cause == null),
            "BD-138 earlier-boot failure is an attention line, not a problem cause");
        foreach (string lang in Strings.Languages)
        {
            Strings.Language = lang;
            Check(DriverCard.Decide(f).VerificationText == Strings.T("drv.verify.earlier-boot") && Strings.Has("status.verification-earlier-boot"),
                "BD-138 earlier-boot explanation exists in " + lang);
        }
        Strings.Language = "en";
        foreach (long? boot in new long?[] { 53, 54, null })
        {
            f.Reports[0].BootId = boot;
            view = DriverCard.Decide(f);
            Equal(VerifyKind.Failed, view.Verification, "BD-138 same, future or unknown boot stays failed: " + boot);
            Check(HomeStatus.Compute(new StatusInputs { Snapshot = WithDefaults(), Driver = view }).Items.Any(i => i.Cause == GuideCause.VerificationFailed),
                "BD-138 a current or unbound failure still raises a problem");
        }
        f.Reports[0].BootId = 52; f.Witness = null;
        Equal(VerifyKind.Failed, DriverCard.Decide(f).Verification, "BD-138 without an exact witness the failure is not demoted");
        f = Facts(); f.Reports[0].BootId = 52; f.Reports[0].Passed = false;
        f.InstalledVersion = "newer"; f.Reports[0].PackageVersion = "newer";
        Equal(VerifyKind.Failed, DriverCard.Decide(f).Verification, "BD-138 another running version cannot excuse the installed package");

        f = Facts(); f.Witness.BootElapsedMs = 2000;
        f.InstallActions.Add(new InstallAction { BootId = 53, Utc = "2099-01-01T00:00:00Z", BootElapsedMs = 1000 });
        Equal(RunningKind.Exact, DriverCard.Decide(f).Running, "BD-138 earlier monotonic action keeps the witness despite future UTC");
        f.InstallActions[0].Utc = "2020-01-01T00:00:00Z";
        foreach (long? stamp in new long?[] { 2000, 3000, null })
        {
            f.InstallActions[0].BootElapsedMs = stamp;
            Check(DriverCard.Decide(f).Running != RunningKind.Exact, "BD-138 equal, newer or unknown action stamp voids the witness: " + stamp);
        }

        string sha;
        var files = Installed(out sha);
        string version = "0.7.199.100-tester.11";
        files.Put(Inst + @"\verify\verify-20261013T073934016Z.json", TimedVerify(version, sha, five.Utc, "5", "null", false));
        files.Put(Inst + @"\verify\verify-20261013T005737067Z.json", TimedVerify(version, sha, seven.Utc, "7", "null", false));
        files.Put(Inst + @"\verify\verify-20261013T014016845Z.json", TimedVerify(version, sha, nine.Utc, "9", "null", true));
        Equal(VerifyKind.Verified, Probe(files).Verification, "BD-138 file reader scans past the first matching future-named failure");
        DriverFacts read;
        Equal(VerifyKind.Unknown, Probe(files, out read, new ProbeBudget { MaxReports = 1 }).Verification,
            "BD-138 exhausted search never promotes the first stale candidate to a verdict");
        Check(read.ReportSearchNote != null, "BD-138 incomplete search has a support explanation");
        files.Put(Inst + @"\verify\verify-20000101T000000000Z.json", FileStatus.Failed);
        Equal(VerifyKind.Unknown, Probe(files).Verification, "BD-138 an unread older filename may conceal a later boot");
    }
}
