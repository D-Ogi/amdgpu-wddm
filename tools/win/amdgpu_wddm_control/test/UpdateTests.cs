// G-UPD (Codex 916 A5): tag grammar and order, eligible releases only, completeness, never a downgrade, unknown
// installed version, backoff and rate limits, the cache's attempt/success split and the recompare after an upgrade.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using AmdgpuWddmControl;

static partial class UnitTests
{
    static string Release(string tag, bool draft = false, string asset = null, string url = null, string published = "2026-10-03T12:00:00Z")
    {
        var v = ReleaseVersion.Parse(tag);
        asset = asset ?? (v != null ? v.AssetName : "x.zip");
        url = url ?? "https://github.com/D-Ogi/amdgpu-wddm/releases/tag/" + tag;
        return "{\"tag_name\":\"" + tag + "\",\"draft\":" + (draft ? "true" : "false") + ",\"prerelease\":true,\"html_url\":\"" + url + "\",\"published_at\":\"" + published +
            "\",\"assets\":[{\"name\":\"" + asset + "\",\"browser_download_url\":\"https://github.com/x\"}]}";
    }

    static void UpdateTests(string root)
    {
        Strings.Language = "en";
        // Grammar and order.
        Check(ReleaseVersion.Parse("v0.7.199.100-tester.11") != null && ReleaseVersion.Parse("0.7.199.100") != null, "tags of both channels parse");
        foreach (var bad in new[] { "v1.2.3", "0.7.199.100-tester.x", "0.7.199.100-beta.1", "latest", "", null, "0.7.199.100-tester.11 " })
            Check(ReleaseVersion.Parse(bad) == null, "tag refused: " + bad);
        Func<string, string, int> cmp = (a, b) => Math.Sign(ReleaseVersion.Parse(a).CompareTo(ReleaseVersion.Parse(b)));
        Equal(1, cmp("0.7.199.100-tester.10", "0.7.199.100-tester.9"), "tester.10 > tester.9 (numeric)");
        Equal(1, cmp("0.7.199.100", "0.7.199.100-tester.99"), "stable > every tester build of the same base");
        Equal(1, cmp("0.7.200.0-tester.1", "0.7.199.100"), "a newer base wins over a stable");
        Equal(0, cmp("v0.7.199.100-tester.11", "0.7.199.100-tester.11"), "the v prefix does not change the version");
        Equal("amdgpu-wddm-tester-0.7.199.100-tester.11.zip", ReleaseVersion.Parse("v0.7.199.100-tester.11").AssetName, "tester asset name");
        Equal("amdgpu-wddm-0.8.0.0.zip", ReleaseVersion.Parse("0.8.0.0").AssetName, "stable asset name");

        // Eligible entries only.
        var page = "[" + string.Join(",", Release("v0.7.199.100-tester.11"), Release("v0.7.199.101-tester.12", draft: true), Release("v0.7.199.102-tester.13", asset: "other.zip"),
            Release("v0.7.199.103-tester.14", url: "https://github.com/someone/amdgpu-wddm/releases/tag/v0.7.199.103-tester.14"),
            Release("v0.7.199.104-tester.15", url: "http://github.com/D-Ogi/amdgpu-wddm/releases/tag/v0.7.199.104-tester.15"), Release("nightly")) + "]";
        var list = UpdateCheck.ParsePage(page);
        Check(list.Count == 1 && list[0].Tag == "v0.7.199.100-tester.11", "drafts, missing assets, foreign or plain-HTTP pages and odd tags are not eligible");
        Check(UpdateCheck.ParsePage("{\"message\":\"API rate limit exceeded\"}") == null && UpdateCheck.ParsePage("[{") == null && UpdateCheck.ParsePage("") == null, "a non-list or malformed answer is no result");

        // Decisions.
        var two = UpdateCheck.ParsePage("[" + Release("v0.7.199.100-tester.11") + "," + Release("v0.7.199.100-tester.12", published: "2026-10-05T00:00:00Z") + "]");
        var r = UpdateCheck.Decide(two, "0.7.199.100-tester.11", true);
        Check(r.Outcome == UpdateOutcome.Available && r.Candidate.Tag == "v0.7.199.100-tester.12" && r.Installed.Tag == "v0.7.199.100-tester.11", "a newer release is available");
        Equal(UpdateOutcome.UpToDate, UpdateCheck.Decide(two, "0.7.199.100-tester.12", true).Outcome, "the newest installed, complete list: up to date");
        Equal(UpdateOutcome.Incomplete, UpdateCheck.Decide(two, "0.7.199.100-tester.12", false).Outcome, "a capped list never says up to date");
        r = UpdateCheck.Decide(two, "0.7.200.0-tester.1", true);
        Check(r.Outcome == UpdateOutcome.UpToDate && r.Candidate == null, "never a downgrade: an older newest release is not offered");
        r = UpdateCheck.Decide(two, null, true);
        Check(r.Outcome == UpdateOutcome.NewestOnly && r.Candidate.Tag == "v0.7.199.100-tester.12", "unknown installed version: only the newest is named");
        Equal(UpdateOutcome.NewestOnly, UpdateCheck.Decide(two, "custom-build", true).Outcome, "an installed version outside the grammar is unknown");
        Equal(UpdateOutcome.Failed, UpdateCheck.Decide(new List<ReleaseEntry>(), "0.7.199.100-tester.11", true).Outcome, "an empty result never says up to date");

        // Rate limits and backoff.
        var now = new DateTime(2026, 10, 4, 12, 0, 0, DateTimeKind.Utc);
        Equal(now.AddSeconds(120), UpdateCheck.NextAllowed("120", null, null, now), "Retry-After seconds");
        Equal(new DateTime(2026, 10, 4, 13, 0, 0, DateTimeKind.Utc), UpdateCheck.NextAllowed("Sun, 04 Oct 2026 13:00:00 GMT", null, null, now), "Retry-After date");
        Equal(new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc).AddSeconds(1791115200), UpdateCheck.NextAllowed(null, "1791115200", "0", now), "X-RateLimit-Reset with none left");
        Check(UpdateCheck.NextAllowed(null, "1791115200", "5", now) == null, "a reset time with requests left is not a limit");
        var cache = new UpdateCache { LastAttemptUtc = Recovery.Stamp(now.AddMinutes(-30)) };
        Check(!UpdateCheck.AutoCheckDue(cache, true, now) && UpdateCheck.AutoCheckDue(new UpdateCache { LastAttemptUtc = Recovery.Stamp(now.AddHours(-2)) }, true, now), "start check at most once an hour");
        Check(!UpdateCheck.AutoCheckDue(null, false, now) && UpdateCheck.AutoCheckDue(null, true, now), "start check: off when unchecked, on by default");
        Check(!UpdateCheck.AutoCheckDue(new UpdateCache { NextAllowedUtc = Recovery.Stamp(now.AddMinutes(5)) }, true, now) &&
            !UpdateCheck.ManualCheckAllowed(new UpdateCache { NextAllowedUtc = Recovery.Stamp(now.AddMinutes(5)) }, now), "the next allowed time holds both checks");

        // The cache: attempt apart from success; a failure keeps the dated last success and the candidate.
        var ok = UpdateCheck.Record(null, UpdateCheck.Decide(two, "0.7.199.100-tester.11", true), now);
        Check(ok.LastSuccessUtc == ok.LastAttemptUtc && ok.CandidateTag == "v0.7.199.100-tester.12" && ok.InstalledTag == "v0.7.199.100-tester.11", "a success records the candidate");
        var failed = UpdateCheck.Record(ok, new UpdateResult { Outcome = UpdateOutcome.Failed }, now.AddHours(2));
        Check(failed.LastSuccessUtc == ok.LastSuccessUtc && failed.LastAttemptUtc != ok.LastAttemptUtc && failed.CandidateTag == ok.CandidateTag &&
            Recovery.Utc(failed.NextAllowedUtc) == now.AddHours(2).Add(UpdateCheck.FailureBackoff), "a failure keeps the last success, backs off");
        var text = UpdateCheck.CardText(failed, "0.7.199.100-tester.11", false);
        Check(text.Contains("Update available: 0.7.199.100-tester.12") && text.Contains("Could not check for updates. Try again later.") && text.Contains("Last successful check:"),
            "offline after a success: the dated candidate stays, the failure is said: " + text);
        Check(!UpdateCheck.CardText(ok, "0.7.199.100-tester.12", false).Contains("Update available"), "after the upgrade the cached candidate is no longer available");
        Check(UpdateCheck.CardText(UpdateCheck.Record(null, UpdateCheck.Decide(two, "0.7.199.100-tester.12", true), now), "0.7.199.100-tester.12", false) == "You have the newest release.", "up to date text");
        var limited = UpdateCheck.Record(ok, new UpdateResult { Outcome = UpdateOutcome.RateLimited, NextAllowedUtc = now.AddHours(1) }, now);
        Check(UpdateCheck.CardText(limited, "0.7.199.100-tester.11", false).Contains("asked us to wait"), "rate limit is not called a connection problem");
        Equal("Not checked for updates yet.", UpdateCheck.CardText(null, "x", false), "never checked");
        Equal("2026-10-03", UpdateCheck.InstalledReleased(ok, "0.7.199.100-tester.11"), "Released date of the installed release after a check");
        Check(UpdateCheck.InstalledReleased(ok, "0.7.199.100-tester.12") == null, "Released date only for the release the check saw");
        Check(UpdateCheck.TagUrl("v1.2.3") == null && UpdateCheck.TagUrl("v0.7.199.100-tester.11") == "https://github.com/D-Ogi/amdgpu-wddm/releases/tag/v0.7.199.100-tester.11", "browser URLs only for tags of the grammar");

        // Static: one request in flight, SystemDefault TLS, bounded size and time, no redirects to elsewhere.
        var src = File.ReadAllText(Path.Combine(root, @"tools\win\amdgpu_wddm_control\src\UpdateCheck.cs"));
        Check(src.Contains("if (_running != null) return;") && src.Contains("SecurityProtocolType.SystemDefault") && !src.Contains("SecurityProtocolType.Tls") &&
            src.Contains("if (m.Length > MaxBytes) return null;") && src.Contains("AllowAutoRedirect = false") && src.Contains("worker.Join(TimeSpan.FromSeconds(TotalSeconds + 2))"),
            "update check: one in flight, OS TLS, size and time bounds, no redirects");
        var info = File.ReadAllText(Path.Combine(root, @"tools\win\amdgpu_wddm_control\src\AssemblyInfo.cs"));
        Check(info.Contains("TargetFramework(\".NETFramework,Version=v4.8\""), "AssemblyInfo declares .NET Framework 4.8 (A5)");
    }
}
