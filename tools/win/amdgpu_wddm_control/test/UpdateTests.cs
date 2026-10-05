// G-UPD (Codex 916 A5): tag grammar and order, eligible releases only, completeness, never a downgrade, unknown
// installed version, backoff and rate limits, the cache's attempt/success split and the recompare after an upgrade.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;
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
        Check(r.Outcome == UpdateOutcome.UpToDate && r.Candidate == null && r.Newest.Tag == "v0.7.199.100-tester.12", "never a downgrade: an older newest release is not offered");
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
        // R8: "up to date" and the Released date belong to the install the check compared against.
        var current = UpdateCheck.Record(null, UpdateCheck.Decide(two, "0.7.199.100-tester.12", true), now);
        Check(current.CheckedInstalled == "0.7.199.100-tester.12" && current.CandidateTag == "v0.7.199.100-tester.12" && current.InstalledTag == "v0.7.199.100-tester.12",
            "R8: an up-to-date check records the install it compared against and the newest release seen");
        Equal("2026-10-05", UpdateCheck.InstalledReleased(current, "0.7.199.100-tester.12"), "R8: the Released date of the checked install");
        text = UpdateCheck.CardText(current, "0.7.199.100-tester.11", false);
        Check(text.Contains("Update available: 0.7.199.100-tester.12") && !text.Contains("newest release."), "R8: after a rollback the newest seen is offered again, never \"up to date\": " + text);
        Check(UpdateCheck.InstalledReleased(current, "0.7.199.100-tester.11") == null, "R8: no Released date after a rollback");
        text = UpdateCheck.CardText(current, null, false);
        Check(text.Contains("The newest release is 0.7.199.100-tester.12.") && !text.Contains("You have"), "R8: an unreadable installed release voids \"up to date\": " + text);
        Check(UpdateCheck.InstalledReleased(current, null) == null && UpdateCheck.InstalledReleased(current, "garbage") == null, "R8: no Released date without a readable install");
        text = UpdateCheck.CardText(current, "0.7.200.0-tester.1", false);
        Check(!text.Contains("You have") && !text.Contains("Update available"), "R8: another (newer) install is not \"up to date\" until the next check: " + text);
        var legacy = new UpdateCache { LastAttemptUtc = current.LastAttemptUtc, LastSuccessUtc = current.LastSuccessUtc, LastAttemptOutcome = "UpToDate", InstalledTag = "v0.7.199.100-tester.12",
            InstalledPublishedUtc = "2026-10-05T00:00:00Z" };
        Check(!UpdateCheck.CardText(legacy, "0.7.199.100-tester.12", false).Contains("You have") && UpdateCheck.InstalledReleased(legacy, "0.7.199.100-tester.12") == null,
            "R8: a cache without CheckedInstalled never says up to date");
        var unknown = UpdateCheck.Record(current, UpdateCheck.Decide(two, null, true), now.AddHours(1));
        Check(unknown.CheckedInstalled == null && unknown.InstalledTag == null && UpdateCheck.InstalledReleased(unknown, "0.7.199.100-tester.12") == null,
            "R8: a check with an unreadable install replaces the earlier install's identity");
        var older = UpdateCheck.Record(current, UpdateCheck.Decide(UpdateCheck.ParsePage("[" + Release("v0.7.199.100-tester.11") + "]"), "0.7.199.100-tester.12", true), now.AddHours(1));
        Check(older.CandidateTag == "v0.7.199.100-tester.11" && older.InstalledTag == null && older.CheckedInstalled == "0.7.199.100-tester.12",
            "R8: the candidate is the newest seen by the last check, the installed entry only when that check listed it");

        Check(UpdateCheck.TagUrl("v1.2.3") == null && UpdateCheck.TagUrl("v0.7.199.100-tester.11") == "https://github.com/D-Ogi/amdgpu-wddm/releases/tag/v0.7.199.100-tester.11", "browser URLs only for tags of the grammar");

        // Static: one request in flight, SystemDefault TLS, bounded size and time, no redirects to elsewhere.
        var src = File.ReadAllText(Path.Combine(root, @"tools\win\amdgpu_wddm_control\src\UpdateCheck.cs"));
        Check(src.Contains("if (_busy != null) return false;") && src.Contains("SecurityProtocolType.SystemDefault") && !src.Contains("SecurityProtocolType.Tls") &&
            src.Contains("if (m.Length > MaxBytes) return null;") && src.Contains("AllowAutoRedirect = false") && src.Contains("run.OnAbort(request.Abort);") &&
            src.Contains("new HttpReleaseTransport(), TimeSpan.FromSeconds(TotalSeconds), Grace"),
            "update check: one in flight, OS TLS, size bound, the request aborted at the 20 s deadline, no redirects");
        UpdateRunnerTests();
        var info = File.ReadAllText(Path.Combine(root, @"tools\win\amdgpu_wddm_control\src\AssemblyInfo.cs"));
        Check(info.Contains("TargetFramework(\".NETFramework,Version=v4.8\""), "AssemblyInfo declares .NET Framework 4.8 (A5)");
    }

    // R7 with an injected transport: the absolute deadline against a trickling answer, and the gate kept closed while a
    // timed-out worker is still alive.
    sealed class FakeTransport : IReleaseTransport
    {
        public Func<string, CheckRun, ReleaseAnswer> OnGet;
        public int Gets;
        public ReleaseAnswer Get(string url, CheckRun run) { Interlocked.Increment(ref Gets); return OnGet(url, run); }
    }

    // A body that never ends: one byte every 20 ms, so no per-read timeout would ever fire.
    class TrickleStream : Stream
    {
        public override int Read(byte[] buffer, int offset, int count) { Thread.Sleep(20); buffer[offset] = (byte)' '; return 1; }
        public override bool CanRead { get { return true; } }
        public override bool CanSeek { get { return false; } }
        public override bool CanWrite { get { return false; } }
        public override long Length { get { throw new NotSupportedException(); } }
        public override long Position { get { throw new NotSupportedException(); } set { throw new NotSupportedException(); } }
        public override void Flush() { }
        public override long Seek(long o, SeekOrigin s) { throw new NotSupportedException(); }
        public override void SetLength(long v) { throw new NotSupportedException(); }
        public override void Write(byte[] b, int o, int c) { throw new NotSupportedException(); }
    }

    static void UpdateRunnerTests()
    {
        var budget = TimeSpan.FromMilliseconds(400);
        var grace = TimeSpan.FromMilliseconds(200);

        // A trickling answer ends at the absolute deadline, by the worker's own check.
        var trickle = new FakeTransport { OnGet = (u, run) => new ReleaseAnswer { Status = 200, Body = new TrickleStream() } };
        var records = new List<UpdateResult>();
        UpdateResult finished = null;
        var done = new ManualResetEvent(false);
        var runner = new UpdateRunner(trickle, budget, grace, r => { lock (records) records.Add(r); }, r => { finished = r; done.Set(); });
        var clock = System.Diagnostics.Stopwatch.StartNew();
        Check(runner.Start("0.7.199.100-tester.11"), "R7: the first Start runs");
        Check(done.WaitOne(10000), "R7: a trickling answer finishes");
        clock.Stop();
        Check(finished != null && finished.Outcome == UpdateOutcome.Failed && finished.Detail.Contains("longer than"), "R7: a trickling answer is a timeout: " + (finished != null ? finished.Detail : "-"));
        Check(clock.Elapsed < budget + grace + TimeSpan.FromMilliseconds(400), "R7: the trickle cannot stretch the absolute deadline (" + (int)clock.ElapsedMilliseconds + " ms)");
        Check(records.Count == 1 && !runner.Running, "R7: recorded once, the gate open after Finished");

        // A request that ignores the abort: the timeout is recorded, the gate stays closed, a second Start joins.
        var release = new ManualResetEvent(false);
        var aborted = new ManualResetEvent(false);
        var recorded = new ManualResetEvent(false);
        var page = "[" + Release("v0.7.199.100-tester.11") + "]";
        bool stuck = true;
        var transport = new FakeTransport
        {
            OnGet = (u, run) =>
            {
                if (!stuck) return new ReleaseAnswer { Status = 200, Body = new MemoryStream(System.Text.Encoding.UTF8.GetBytes(page)) };
                run.OnAbort(() => aborted.Set());
                release.WaitOne();
                return new ReleaseAnswer { Status = 200, Body = new MemoryStream(System.Text.Encoding.UTF8.GetBytes(page)) };
            }
        };
        records.Clear();
        int finishes = 0;
        finished = null; done.Reset();
        runner = new UpdateRunner(transport, budget, grace, r => { lock (records) records.Add(r); recorded.Set(); }, r => { finished = r; Interlocked.Increment(ref finishes); done.Set(); });
        Check(runner.Start("0.7.199.100-tester.11"), "R7: a stuck check starts");
        Check(recorded.WaitOne(10000), "R7: the stuck check is recorded at its deadline");
        Check(aborted.WaitOne(0), "R7: the request in flight was aborted at the deadline");
        Check(records.Count == 1 && records[0].Outcome == UpdateOutcome.Failed && records[0].Detail.Contains("longer than"), "R7: the deadline records a timeout");
        Check(!runner.Start("0.7.199.100-tester.11") && runner.Running && transport.Gets == 1 && finishes == 0,
            "R7: a second Start at the timeout joins: no second request while the old worker lives");
        release.Set();
        Check(done.WaitOne(10000), "R7: Finished fires when the old worker has ended");
        Check(finishes == 1 && finished.Outcome == UpdateOutcome.Failed && !runner.Running && records.Count == 1,
            "R7: the late answer is dropped (once Finished, the timeout, nothing recorded twice)");
        stuck = false; done.Reset();
        // 936 A5: Start returns before its worker reaches Get; the counter is read after the completion signal.
        Check(runner.Start("0.7.199.100-tester.11"), "R7: after the worker ended a new check may start");
        Check(done.WaitOne(10000) && finished.Outcome == UpdateOutcome.UpToDate, "R7: the new check runs to its result");
        Check(transport.Gets == 2, "R7: the new check made exactly one more request (" + transport.Gets + ")");
        DeadlineTests();
    }

    // A body whose Read blocks until the run is aborted; its abort time is the supervisor's cancellation time.
    sealed class BlockedStream : TrickleStream
    {
        public readonly ManualResetEvent Aborted = new ManualResetEvent(false);
        public override int Read(byte[] buffer, int offset, int count) { Aborted.WaitOne(); throw new IOException("aborted"); }
    }

    // A complete one-page answer whose disposal ends only after the deadline: the work completes just after it.
    sealed class LateStream : MemoryStream
    {
        readonly CheckRun _run;
        public LateStream(byte[] b, CheckRun run) : base(b) { _run = run; }
        protected override void Dispose(bool disposing) { while (_run.LeftMs > 0) Thread.Sleep(5); Thread.Sleep(30); base.Dispose(disposing); }
    }

    // 936 A4: the abort at the absolute deadline, not at budget + grace; a success after the deadline is a timeout.
    static void DeadlineTests()
    {
        var budget = TimeSpan.FromMilliseconds(400);
        var grace = TimeSpan.FromMilliseconds(1500);
        var clock = new System.Diagnostics.Stopwatch();
        long abortMs = -1;
        var body = new BlockedStream();
        var blocked = new FakeTransport { OnGet = (u, run) =>
        {
            run.OnAbort(() => { Interlocked.CompareExchange(ref abortMs, clock.ElapsedMilliseconds, -1); body.Aborted.Set(); });
            return new ReleaseAnswer { Status = 200, Body = body };
        } };
        UpdateResult finished = null;
        var done = new ManualResetEvent(false);
        var runner = new UpdateRunner(blocked, budget, grace, r => { }, r => { finished = r; done.Set(); });
        clock.Start();
        Check(runner.Start("0.7.199.100-tester.11"), "A4: a check with a blocked read starts");
        Check(done.WaitOne(10000), "A4: the blocked read ends");
        long at = Interlocked.Read(ref abortMs);
        Check(at >= (long)budget.TotalMilliseconds - 20 && at < (long)budget.TotalMilliseconds + 300,
            "A4: the abort comes at the deadline (" + at + " ms, budget " + (int)budget.TotalMilliseconds + ", grace " + (int)grace.TotalMilliseconds + ")");
        Check(finished != null && finished.Outcome == UpdateOutcome.Failed && finished.Detail.Contains("longer than"), "A4: a blocked read is a timeout");
        Check(clock.ElapsedMilliseconds < (long)(budget + grace).TotalMilliseconds, "A4: the blocked check finishes before budget + grace (" + clock.ElapsedMilliseconds + " ms)");

        // The whole answer is read in time, but the work completes just after the deadline.
        var page = System.Text.Encoding.UTF8.GetBytes("[" + Release("v0.7.199.100-tester.11") + "]");
        var late = new FakeTransport { OnGet = (u, run) => new ReleaseAnswer { Status = 200, Body = new LateStream(page, run) } };
        var direct = UpdateCheck.Fetch(late, new CheckRun(budget), "0.7.199.100-tester.11");
        Check(direct.Outcome == UpdateOutcome.Failed && direct.Detail.Contains("longer than"), "A4: Fetch rejects a success completed after the deadline: " + direct.Outcome);
        Check(UpdateCheck.Fetch(new FakeTransport { OnGet = (u, run) => new ReleaseAnswer { Status = 200, Body = new MemoryStream(page) } }, new CheckRun(budget), "0.7.199.100-tester.11").Outcome == UpdateOutcome.UpToDate,
            "A4: the same answer in time is a result");
        finished = null; done.Reset();
        runner = new UpdateRunner(late, budget, grace, r => { }, r => { finished = r; done.Set(); });
        Check(runner.Start("0.7.199.100-tester.11") && done.WaitOne(10000), "A4: the late check finishes");
        Check(finished.Outcome == UpdateOutcome.Failed && finished.Detail.Contains("longer than"), "A4: the runner publishes a timeout for a completion after the deadline: " + finished.Outcome);
    }
}
