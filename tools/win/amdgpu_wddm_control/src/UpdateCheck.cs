// The update check (WU-043, WU-046, review 916 A5). The ONLY file of this app that talks to the network, and the only
// place its web addresses live (G-SRC). No account, no token, no upload: one unauthenticated GET per page of
// https://api.github.com/repos/D-Ogi/amdgpu-wddm/releases, at start (when the user has not turned it off) and on a
// click; and the browser opened on the release page the user asks for.
//
// Rules (A5):
//   * TLS: the operating system's choice (SecurityProtocolType.SystemDefault, with the .NET 4.8 target declared in
//     AssemblyInfo.cs); machine TLS settings are never changed.
//   * Tags: v?A.B.C.D (stable) or v?A.B.C.D-tester.N; numeric order; for the same A.B.C.D a stable release is newer
//     than every tester build; tester.10 > tester.9. A release counts only with its package asset
//     (amdgpu-wddm-<version>.zip or amdgpu-wddm-tester-<version>.zip) and not as a draft.
//   * Never a downgrade: only a release newer than the INSTALLED one is offered; with an unknown installed version
//     the card says only which release is the newest.
//   * Completeness: up to 3 pages of 100; an empty, malformed or capped result never says "up to date".
//   * One check in flight; clicks during a check join it; Retry-After / X-RateLimit-Reset set the next allowed
//     time; responses above 2 MB are refused.
//   * Time (927 R7): one absolute 20 s deadline per check, checked between pages and after every read of the body
//     (a trickling answer cannot stretch it); at the deadline the request in flight is aborted and the check is
//     recorded as failed. The gate stays closed until the worker has really ended, so a second check can never
//     run beside a late one; its result is dropped.
//   * The cache keeps the last attempt apart from the last success and the newest release seen; a candidate is
//     compared with the installed release again on every read, so an upgrade never leaves a stale "available".
//     "Up to date" and the installed release's date hold only for the installed release that check compared
//     against (927 R8): a rollback, another install or an unreadable Release\Version voids them.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Web.Script.Serialization;
using Microsoft.Win32;

namespace AmdgpuWddmControl
{
    public sealed class ReleaseVersion : IComparable<ReleaseVersion>
    {
        public int A, B, C, D;
        public int? Tester;              // null: a stable release
        public string Text;

        static readonly Regex Grammar = new Regex(@"^v?(?<a>\d{1,6})\.(?<b>\d{1,6})\.(?<c>\d{1,6})\.(?<d>\d{1,6})(-tester\.(?<t>\d{1,6}))?$", RegexOptions.CultureInvariant);

        public static ReleaseVersion Parse(string tag)
        {
            var m = Grammar.Match(tag ?? "");
            if (!m.Success) return null;
            Func<string, int> n = g => int.Parse(m.Groups[g].Value, CultureInfo.InvariantCulture);
            return new ReleaseVersion { A = n("a"), B = n("b"), C = n("c"), D = n("d"), Tester = m.Groups["t"].Success ? (int?)n("t") : null, Text = (tag ?? "").TrimStart('v') };
        }

        public int CompareTo(ReleaseVersion o)
        {
            foreach (var d in new[] { A.CompareTo(o.A), B.CompareTo(o.B), C.CompareTo(o.C), D.CompareTo(o.D) }) if (d != 0) return d;
            if (Tester == null && o.Tester == null) return 0;
            if (Tester == null) return 1;          // stable after every tester build of the same base
            if (o.Tester == null) return -1;
            return Tester.Value.CompareTo(o.Tester.Value);
        }

        public string AssetName { get { return Tester == null ? "amdgpu-wddm-" + Text + ".zip" : "amdgpu-wddm-tester-" + Text + ".zip"; } }
    }

    public sealed class ReleaseEntry
    {
        public string Tag, Url, PublishedUtc;
        public ReleaseVersion Version;
    }

    public enum UpdateOutcome { None, Available, UpToDate, NewestOnly, Incomplete, Failed, RateLimited }

    public sealed class UpdateResult
    {
        public UpdateOutcome Outcome;
        public ReleaseEntry Candidate;      // Available / NewestOnly
        public ReleaseEntry Newest;         // the newest eligible release seen, newer than the installed one or not
        public ReleaseEntry Installed;      // the installed release when the answer lists it (its publication date)
        public string CheckedInstalled;     // the installed release the decision compared against; null: unknown
        public int Seen;                    // eligible releases seen
        public DateTime? NextAllowedUtc;
        public string Detail;               // for the report and the log
    }

    // HKCU\Software\amdgpu-wddm\Control\Update (docs/gui/interfaces.md section 5).
    public sealed class UpdateCache
    {
        public string LastAttemptUtc, LastAttemptOutcome, LastSuccessUtc, CheckedInstalled, CandidateTag, CandidateVersion, CandidateUrl, CandidatePublishedUtc,
            NextAllowedUtc, InstalledTag, InstalledPublishedUtc;
    }

    public static class UpdateCheck
    {
        public const string Owner = "D-Ogi", Repository = "amdgpu-wddm";
        public const string ApiUrl = "https://api.github.com/repos/" + Owner + "/" + Repository + "/releases";
        public const string ReleasesPage = "https://github.com/" + Owner + "/" + Repository + "/releases";
        public const int PerPage = 100, MaxPages = 3, TotalSeconds = 20, MaxBytes = 2 << 20;
        public static readonly TimeSpan AutoInterval = TimeSpan.FromHours(1), FailureBackoff = TimeSpan.FromMinutes(15);

        // ---- pure ---------------------------------------------------------------------------------------------------

        // One page of the API's JSON array: the eligible releases on it. null when the page is not a JSON array.
        public static List<ReleaseEntry> ParsePage(string json)
        {
            object root;
            try { root = new JavaScriptSerializer { MaxJsonLength = MaxBytes }.DeserializeObject(json ?? ""); }
            catch (Exception) { return null; }
            var items = root as object[];
            if (items == null) return null;
            var list = new List<ReleaseEntry>();
            foreach (var o in items)
            {
                var r = o as IDictionary<string, object>;
                if (r == null) continue;
                object v;
                if (r.TryGetValue("draft", out v) && v is bool && (bool)v) continue;
                var tag = r.TryGetValue("tag_name", out v) ? v as string : null;
                var version = ReleaseVersion.Parse(tag);
                if (version == null) continue;
                var url = r.TryGetValue("html_url", out v) ? v as string : null;
                if (url != TagUrl(tag)) continue;           // only the expected HTTPS page of this repository
                bool asset = false;
                if (r.TryGetValue("assets", out v) && v is object[])
                    foreach (var a in (object[])v)
                    {
                        var ad = a as IDictionary<string, object>;
                        object name;
                        if (ad != null && ad.TryGetValue("name", out name) && name as string == version.AssetName) asset = true;
                    }
                if (!asset) continue;
                list.Add(new ReleaseEntry { Tag = tag, Url = url, Version = version, PublishedUtc = r.TryGetValue("published_at", out v) ? v as string : null });
            }
            return list;
        }

        public static string TagUrl(string tag) { return ReleaseVersion.Parse(tag) == null ? null : ReleasesPage + "/tag/" + tag; }

        // The decision over every page read. complete: the last page was short (fewer than PerPage items).
        public static UpdateResult Decide(IEnumerable<ReleaseEntry> releases, string installed, bool complete)
        {
            var all = releases.ToList();
            var newest = all.OrderByDescending(r => r.Version).FirstOrDefault();
            var mine = ReleaseVersion.Parse(installed);
            var res = new UpdateResult { Seen = all.Count, Newest = newest, CheckedInstalled = mine == null ? null : mine.Text };
            res.Installed = mine == null ? null : all.FirstOrDefault(r => r.Version.CompareTo(mine) == 0);
            if (newest == null) { res.Outcome = UpdateOutcome.Failed; res.Detail = "no eligible release in the answer"; return res; }
            if (mine == null) { res.Outcome = UpdateOutcome.NewestOnly; res.Candidate = newest; res.Detail = "installed version unknown"; return res; }
            if (newest.Version.CompareTo(mine) > 0) { res.Outcome = UpdateOutcome.Available; res.Candidate = newest; return res; }
            res.Outcome = complete ? UpdateOutcome.UpToDate : UpdateOutcome.Incomplete;
            return res;
        }

        // Retry-After (seconds or an HTTP date) or X-RateLimit-Reset (epoch seconds) -> the next allowed time.
        public static DateTime? NextAllowed(string retryAfter, string rateLimitReset, string remaining, DateTime nowUtc)
        {
            int seconds;
            if (!string.IsNullOrEmpty(retryAfter))
            {
                if (int.TryParse(retryAfter.Trim(), NumberStyles.None, CultureInfo.InvariantCulture, out seconds)) return nowUtc.AddSeconds(Math.Min(seconds, 86400));
                DateTime when;
                if (DateTime.TryParse(retryAfter, CultureInfo.InvariantCulture, DateTimeStyles.AdjustToUniversal | DateTimeStyles.AssumeUniversal, out when)) return when;
            }
            long reset;
            if (remaining == "0" && long.TryParse(rateLimitReset ?? "", NumberStyles.None, CultureInfo.InvariantCulture, out reset))
                return new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc).AddSeconds(reset);
            return null;
        }

        // Whether the start of the app may check: not turned off, the next allowed time passed, and no attempt in the
        // last hour (a shared IP has 60 requests an hour).
        public static bool AutoCheckDue(UpdateCache c, bool enabled, DateTime nowUtc)
        {
            if (!enabled) return false;
            var next = Recovery.Utc(c != null ? c.NextAllowedUtc : null);
            if (next != null && next.Value > nowUtc) return false;
            var last = Recovery.Utc(c != null ? c.LastAttemptUtc : null);
            return last == null || nowUtc - last.Value >= AutoInterval;
        }

        public static bool ManualCheckAllowed(UpdateCache c, DateTime nowUtc)
        {
            var next = Recovery.Utc(c != null ? c.NextAllowedUtc : null);
            return next == null || next.Value <= nowUtc;
        }

        // The cache after an attempt: last attempt always; last success, the installed release compared against, the
        // newest release seen and the installed release's entry only from a result (each replaced as a whole, so a
        // value of an older check never survives beside a newer one).
        public static UpdateCache Record(UpdateCache before, UpdateResult r, DateTime nowUtc)
        {
            var c = before ?? new UpdateCache();
            c = new UpdateCache
            {
                LastAttemptUtc = Recovery.Stamp(nowUtc), LastAttemptOutcome = r.Outcome.ToString(), LastSuccessUtc = c.LastSuccessUtc, CheckedInstalled = c.CheckedInstalled,
                CandidateTag = c.CandidateTag, CandidateVersion = c.CandidateVersion, CandidateUrl = c.CandidateUrl, CandidatePublishedUtc = c.CandidatePublishedUtc,
                InstalledTag = c.InstalledTag, InstalledPublishedUtc = c.InstalledPublishedUtc,
                NextAllowedUtc = Recovery.Stamp(r.NextAllowedUtc ?? (r.Outcome == UpdateOutcome.Failed ? nowUtc.Add(FailureBackoff) : (DateTime?)null)),
            };
            if (r.Outcome == UpdateOutcome.Available || r.Outcome == UpdateOutcome.NewestOnly || r.Outcome == UpdateOutcome.UpToDate || r.Outcome == UpdateOutcome.Incomplete)
            {
                var newest = r.Newest ?? r.Candidate;
                c.LastSuccessUtc = Recovery.Stamp(nowUtc);
                c.CheckedInstalled = r.CheckedInstalled;
                c.CandidateTag = newest != null ? newest.Tag : null;
                c.CandidateVersion = newest != null ? newest.Version.Text : null;
                c.CandidateUrl = newest != null ? newest.Url : null;
                c.CandidatePublishedUtc = newest != null ? newest.PublishedUtc : null;
                c.InstalledTag = r.Installed != null ? r.Installed.Tag : null;
                c.InstalledPublishedUtc = r.Installed != null ? r.Installed.PublishedUtc : null;
            }
            return c;
        }

        // Whether the last successful check compared against the installed release read now (R8). An unreadable
        // installed release never matches.
        public static bool CheckedThisInstall(UpdateCache c, string installed)
        {
            var checkedOne = ReleaseVersion.Parse(c != null ? c.CheckedInstalled : null);
            var mine = ReleaseVersion.Parse(installed);
            return checkedOne != null && mine != null && checkedOne.CompareTo(mine) == 0;
        }

        // What the Driver card shows from the cache, compared with the installed release NOW (never a stored "current").
        public static string CardText(UpdateCache c, string installed, bool checking)
        {
            if (checking) return Strings.T("upd.checking");
            if (c == null || c.LastAttemptUtc == null) return Strings.T("upd.never");
            var lines = new List<string>();
            var candidate = ReleaseVersion.Parse(c.CandidateTag);
            var mine = ReleaseVersion.Parse(installed);
            bool success = c.LastSuccessUtc != null, latest = success && c.LastSuccessUtc == c.LastAttemptUtc, same = CheckedThisInstall(c, installed);
            if (candidate != null && mine != null && candidate.CompareTo(mine) > 0) lines.Add(Strings.T("upd.available", candidate.Text));
            else if (candidate != null && mine == null) lines.Add(Strings.T("upd.newest", candidate.Text));
            else if (latest && same && c.LastAttemptOutcome == "UpToDate") lines.Add(Strings.T("upd.uptodate"));
            else if (latest && same && c.LastAttemptOutcome == "Incomplete") lines.Add(Strings.T("upd.incomplete", PerPage * MaxPages));
            if (c.LastAttemptOutcome == "Failed") lines.Add(Strings.T("upd.failed"));
            if (c.LastAttemptOutcome == "RateLimited") lines.Add(Strings.T("upd.rate-limited", Local(c.NextAllowedUtc)));
            if (success && c.LastSuccessUtc != c.LastAttemptUtc) lines.Add(Strings.T("upd.last-success", Local(c.LastSuccessUtc)));
            if (candidate != null && Recovery.Utc(c.CandidatePublishedUtc) != null && (mine == null || candidate.CompareTo(mine) > 0))
                lines.Add(Strings.T("upd.released", Recovery.Utc(c.CandidatePublishedUtc).Value.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture)));
            return string.Join(" ", lines);
        }

        static string Local(string utc)
        {
            var t = Recovery.Utc(utc);
            return t == null ? "-" : t.Value.ToLocalTime().ToString("yyyy-MM-dd HH:mm", CultureInfo.InvariantCulture);
        }

        // The "Released" date of the INSTALLED release, known only when the last successful check compared against this
        // very install and saw it in the list (a separate label from the INF driver date); null otherwise.
        public static string InstalledReleased(UpdateCache c, string installed)
        {
            var tag = ReleaseVersion.Parse(c != null ? c.InstalledTag : null);
            var mine = ReleaseVersion.Parse(installed);
            var t = Recovery.Utc(c != null ? c.InstalledPublishedUtc : null);
            return tag != null && mine != null && tag.CompareTo(mine) == 0 && CheckedThisInstall(c, installed) && t != null
                ? t.Value.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture) : null;
        }

        public static bool IsCandidateNewer(UpdateCache c, string installed)
        {
            var candidate = ReleaseVersion.Parse(c != null ? c.CandidateTag : null);
            var mine = ReleaseVersion.Parse(installed);
            return candidate != null && mine != null && candidate.CompareTo(mine) > 0;
        }

        // ---- the cache in HKCU ----------------------------------------------------------------------------------------

        public const string CachePath = @"Software\amdgpu-wddm\Control\Update";

        public static UpdateCache LoadCache()
        {
            try
            {
                using (var k = Registry.CurrentUser.OpenSubKey(CachePath))
                {
                    if (k == null) return null;
                    Func<string, string> s = n => k.GetValue(n) as string;
                    return new UpdateCache
                    {
                        LastAttemptUtc = s("LastAttemptUtc"), LastAttemptOutcome = s("LastAttemptOutcome"), LastSuccessUtc = s("LastSuccessUtc"),
                        CheckedInstalled = s("CheckedInstalled"), CandidateTag = s("CandidateTag"),
                        CandidateVersion = s("CandidateVersion"), CandidateUrl = s("CandidateUrl"), CandidatePublishedUtc = s("CandidatePublishedUtc"), NextAllowedUtc = s("NextAllowedUtc"),
                        InstalledTag = s("InstalledTag"), InstalledPublishedUtc = s("InstalledPublishedUtc"),
                    };
                }
            }
            catch (Exception) { return null; }
        }

        public static void SaveCache(UpdateCache c)
        {
            try
            {
                using (var k = Registry.CurrentUser.CreateSubKey(CachePath))
                    foreach (var f in typeof(UpdateCache).GetFields())
                    {
                        var v = f.GetValue(c) as string;
                        if (v == null) k.DeleteValue(f.Name, false); else k.SetValue(f.Name, v, RegistryValueKind.String);
                    }
            }
            catch (Exception) { }
        }

        // ---- the network ----------------------------------------------------------------------------------------------

        public static readonly TimeSpan Grace = TimeSpan.FromSeconds(2);
        static readonly UpdateRunner Default = new UpdateRunner(new HttpReleaseTransport(), TimeSpan.FromSeconds(TotalSeconds), Grace,
            r => SaveCache(Record(LoadCache(), r, DateTime.UtcNow)), r => { var f = Finished; if (f != null) f(r); });
        public static event Action<UpdateResult> Finished;

        public static bool Running { get { return Default.Running; } }

        // Starts a check unless one runs (a click during a check joins it). The result arrives through Finished, on a
        // worker thread, once the gate has opened again; the window marshals it and drops it when it has closed.
        public static void Start(string installed) { Default.Start(installed); }

        public static UpdateResult TimedOut() { return new UpdateResult { Outcome = UpdateOutcome.Failed, Detail = "the check took longer than its time bound" }; }

        // One check over a transport: the pages, each started and read only before the run's deadline. Every failure is
        // a result, never an exception.
        public static UpdateResult Fetch(IReleaseTransport transport, CheckRun run, string installed)
        {
            var all = new List<ReleaseEntry>();
            bool complete = false;
            for (int page = 1; page <= MaxPages; page++)
            {
                if (run.Over) return TimedOut();
                string body;
                try
                {
                    using (var answer = transport.Get(ApiUrl + "?per_page=" + PerPage + "&page=" + page, run))
                    {
                        if (run.Over) return TimedOut();
                        if (answer.Failure != null) return new UpdateResult { Outcome = UpdateOutcome.Failed, Detail = answer.Failure };
                        if (answer.Status != 200)
                        {
                            var next = NextAllowed(answer.Header("Retry-After"), answer.Header("X-RateLimit-Reset"), answer.Header("X-RateLimit-Remaining"), DateTime.UtcNow);
                            bool limited = (answer.Status == 403 || answer.Status == 429) && next != null;
                            return new UpdateResult { Outcome = limited ? UpdateOutcome.RateLimited : UpdateOutcome.Failed, NextAllowedUtc = next, Detail = "HTTP " + answer.Status };
                        }
                        body = ReadBounded(answer.Body, run);
                        if (body == null) return new UpdateResult { Outcome = UpdateOutcome.Failed, Detail = "the answer is larger than " + MaxBytes + " bytes" };
                    }
                }
                catch (Exception e) { return run.Over ? TimedOut() : new UpdateResult { Outcome = UpdateOutcome.Failed, Detail = e.GetType().Name }; }
                var entries = ParsePage(body);
                if (entries == null) return new UpdateResult { Outcome = UpdateOutcome.Failed, Detail = "the answer is not a list of releases" };
                all.AddRange(entries);
                int items = CountItems(body);
                if (items < PerPage) { complete = true; break; }
            }
            var result = Decide(all, installed, complete);
            return run.Over ? TimedOut() : result;      // a result finished after the deadline is not a result (936 A4)
        }

        static int CountItems(string json)
        {
            try { var a = new JavaScriptSerializer { MaxJsonLength = MaxBytes }.DeserializeObject(json) as object[]; return a == null ? 0 : a.Length; }
            catch (Exception) { return 0; }
        }

        // The body up to MaxBytes (null above it). The deadline is checked after every read, so an answer that trickles
        // in a byte at a time cannot outlive it; a read blocked past it is ended by the run's abort.
        static string ReadBounded(Stream s, CheckRun run)
        {
            using (var m = new MemoryStream())
            {
                var buffer = new byte[65536];
                int n;
                while ((n = s.Read(buffer, 0, buffer.Length)) > 0)
                {
                    if (run.Over) throw new TimeoutException();
                    m.Write(buffer, 0, n);
                    if (m.Length > MaxBytes) return null;
                }
                if (run.Over) throw new TimeoutException();
                return Encoding.UTF8.GetString(m.ToArray());
            }
        }

        // The browser, only on this repository's release pages: the list, or the page of a tag of the grammar.
        public static bool OpenReleasePage(string tag)
        {
            string url = tag == null ? ReleasesPage : TagUrl(tag);
            if (url == null || !url.StartsWith("https://github.com/" + Owner + "/" + Repository + "/releases", StringComparison.Ordinal)) return false;
            try { using (Process.Start(new ProcessStartInfo(url) { UseShellExecute = true })) { } return true; }
            catch (Exception) { return false; }
        }
    }

    // The deadline and the abort of one check (927 R7). The deadline is absolute: it counts from the start of the run,
    // never from the last byte read.
    public sealed class CheckRun
    {
        readonly Stopwatch _clock = Stopwatch.StartNew();
        readonly TimeSpan _budget;
        readonly object _lock = new object();
        Action _cancel;
        bool _aborted;

        public CheckRun(TimeSpan budget) { _budget = budget; }

        public bool Over { get { lock (_lock) return _aborted || _clock.Elapsed >= _budget; } }
        public bool Aborted { get { lock (_lock) return _aborted; } }
        public int LeftMs { get { return (int)Math.Max(0, Math.Min(int.MaxValue, (_budget - _clock.Elapsed).TotalMilliseconds)); } }

        // How to end the request in flight; run at once when the run was already aborted.
        public void OnAbort(Action cancel)
        {
            bool now;
            lock (_lock) { _cancel = cancel; now = _aborted; }
            if (now && cancel != null) try { cancel(); } catch (Exception) { }
        }

        public void Abort()
        {
            Action cancel;
            lock (_lock) { if (_aborted) return; _aborted = true; cancel = _cancel; }
            if (cancel != null) try { cancel(); } catch (Exception) { }
        }
    }

    // One GET of the release list. The tests inject their own; the app uses HttpReleaseTransport.
    public interface IReleaseTransport
    {
        ReleaseAnswer Get(string url, CheckRun run);
    }

    public sealed class ReleaseAnswer : IDisposable
    {
        public string Failure;                          // no HTTP answer (the network's status)
        public int Status;
        public Func<string, string> Header = n => null;
        public Stream Body;                             // only with Status 200
        public IDisposable Owner;

        public void Dispose()
        {
            if (Body != null) try { Body.Dispose(); } catch (Exception) { }
            if (Owner != null) try { Owner.Dispose(); } catch (Exception) { }
        }
    }

    public sealed class HttpReleaseTransport : IReleaseTransport
    {
        public ReleaseAnswer Get(string url, CheckRun run)
        {
            ServicePointManager.SecurityProtocol = SecurityProtocolType.SystemDefault;
            var request = (HttpWebRequest)WebRequest.Create(url);
            request.Method = "GET";
            request.UserAgent = "amdgpu-wddm-control/" + System.Reflection.Assembly.GetExecutingAssembly().GetName().Version;
            request.Accept = "application/vnd.github+json";
            int left = Math.Max(1, run.LeftMs);
            request.Timeout = left; request.ReadWriteTimeout = left;
            request.AllowAutoRedirect = false;
            run.OnAbort(request.Abort);
            try { return Answer((HttpWebResponse)request.GetResponse(), true); }
            catch (WebException e)
            {
                var http = e.Response as HttpWebResponse;
                return http != null ? Answer(http, false) : new ReleaseAnswer { Failure = e.Status.ToString() };
            }
        }

        static ReleaseAnswer Answer(HttpWebResponse response, bool ok)
        {
            var headers = response.Headers;
            int status = (int)response.StatusCode;
            return new ReleaseAnswer { Status = status, Header = n => headers[n], Body = ok && status == 200 ? response.GetResponseStream() : null, Owner = response };
        }
    }

    // One check at a time (927 R7). A supervisor aborts the request at the deadline and records the timeout; the gate
    // opens only when the worker has ended AND the result is recorded, and Finished fires once, at that moment. A
    // worker's result after the deadline is dropped.
    public sealed class UpdateRunner
    {
        readonly IReleaseTransport _transport;
        readonly TimeSpan _budget, _grace;
        readonly Action<UpdateResult> _record, _finished;
        readonly object _gate = new object();
        Job _busy;

        sealed class Job { public CheckRun Run; public Thread Worker; public UpdateResult Result, Published; public int Holds = 2; }

        public UpdateRunner(IReleaseTransport transport, TimeSpan budget, TimeSpan grace, Action<UpdateResult> record, Action<UpdateResult> finished)
        {
            _transport = transport; _budget = budget; _grace = grace; _record = record; _finished = finished;
        }

        public bool Running { get { lock (_gate) return _busy != null; } }

        // false: a check holds the gate (the caller joins it).
        public bool Start(string installed)
        {
            lock (_gate)
            {
                if (_busy != null) return false;
                var job = new Job { Run = new CheckRun(_budget) };
                job.Worker = new Thread(() => Work(job, installed)) { IsBackground = true, Name = "update check" };
                _busy = job;
                job.Worker.Start();
                new Thread(() => Supervise(job)) { IsBackground = true, Name = "update check deadline" }.Start();
                return true;
            }
        }

        void Work(Job job, string installed)
        {
            try { job.Result = UpdateCheck.Fetch(_transport, job.Run, installed); }
            catch (Exception e) { job.Result = new UpdateResult { Outcome = UpdateOutcome.Failed, Detail = e.GetType().Name }; }
            finally { Release(job); }
        }

        void Supervise(Job job)
        {
            // The abort comes at the absolute deadline itself (936 A4); the grace is only for the cleanup after it.
            UpdateResult r;
            if (job.Worker.Join(job.Run.LeftMs)) r = job.Result ?? new UpdateResult { Outcome = UpdateOutcome.Failed, Detail = "no result" };
            else
            {
                job.Run.Abort();
                job.Worker.Join(_grace);        // an aborted request ends at once; one that does not keeps the gate closed
                r = UpdateCheck.TimedOut();
            }
            try { _record(r); } catch (Exception) { }
            job.Published = r;
            Release(job);
        }

        void Release(Job job)
        {
            bool last;
            lock (_gate)
            {
                last = --job.Holds == 0;
                if (last && _busy == job) _busy = null;
            }
            if (last && _finished != null) try { _finished(job.Published); } catch (Exception) { }
        }
    }
}
