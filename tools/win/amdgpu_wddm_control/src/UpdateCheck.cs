// The update check (WU-043, WU-046, Codex 916 A5). The ONLY file of this app that talks to the network, and the only
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
//   * One request in flight; clicks during a check join it; Retry-After / X-RateLimit-Reset set the next allowed
//     time; a 20 s bound on the whole check (late results are dropped); responses above 2 MB are refused.
//   * The cache keeps the last attempt apart from the last success and the candidate's identity; a candidate is
//     compared with the installed release again on every read, so an upgrade never leaves a stale "available".
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
        public ReleaseEntry Installed;      // the installed release when the answer lists it (its publication date)
        public int Seen;                    // eligible releases seen
        public DateTime? NextAllowedUtc;
        public string Detail;               // for the report and the log
    }

    // HKCU\Software\amdgpu-wddm\Control\Update (docs/gui/interfaces.md section 5).
    public sealed class UpdateCache
    {
        public string LastAttemptUtc, LastAttemptOutcome, LastSuccessUtc, CandidateTag, CandidateVersion, CandidateUrl, CandidatePublishedUtc, NextAllowedUtc,
            InstalledTag, InstalledPublishedUtc;
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
            var res = new UpdateResult { Seen = all.Count };
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

        // The cache after an attempt: last attempt always; last success and the candidate only from a result.
        public static UpdateCache Record(UpdateCache before, UpdateResult r, DateTime nowUtc)
        {
            var c = before ?? new UpdateCache();
            c = new UpdateCache
            {
                LastAttemptUtc = Recovery.Stamp(nowUtc), LastAttemptOutcome = r.Outcome.ToString(), LastSuccessUtc = c.LastSuccessUtc,
                CandidateTag = c.CandidateTag, CandidateVersion = c.CandidateVersion, CandidateUrl = c.CandidateUrl, CandidatePublishedUtc = c.CandidatePublishedUtc,
                InstalledTag = c.InstalledTag, InstalledPublishedUtc = c.InstalledPublishedUtc,
                NextAllowedUtc = Recovery.Stamp(r.NextAllowedUtc ?? (r.Outcome == UpdateOutcome.Failed ? nowUtc.Add(FailureBackoff) : (DateTime?)null)),
            };
            if (r.Outcome == UpdateOutcome.Available || r.Outcome == UpdateOutcome.NewestOnly || r.Outcome == UpdateOutcome.UpToDate || r.Outcome == UpdateOutcome.Incomplete)
            {
                c.LastSuccessUtc = Recovery.Stamp(nowUtc);
                c.CandidateTag = r.Candidate != null ? r.Candidate.Tag : null;
                c.CandidateVersion = r.Candidate != null ? r.Candidate.Version.Text : null;
                c.CandidateUrl = r.Candidate != null ? r.Candidate.Url : null;
                c.CandidatePublishedUtc = r.Candidate != null ? r.Candidate.PublishedUtc : null;
                if (r.Installed != null) { c.InstalledTag = r.Installed.Tag; c.InstalledPublishedUtc = r.Installed.PublishedUtc; }
            }
            return c;
        }

        // What the Driver card shows from the cache, compared with the installed release NOW (never a stored "current").
        public static string CardText(UpdateCache c, string installed, bool checking)
        {
            if (checking) return Strings.T("upd.checking");
            if (c == null || c.LastAttemptUtc == null) return Strings.T("upd.never");
            var lines = new List<string>();
            var candidate = ReleaseVersion.Parse(c.CandidateTag);
            var mine = ReleaseVersion.Parse(installed);
            bool success = c.LastSuccessUtc != null;
            if (candidate != null && mine != null && candidate.CompareTo(mine) > 0) lines.Add(Strings.T("upd.available", candidate.Text));
            else if (candidate != null && mine == null) lines.Add(Strings.T("upd.newest", candidate.Text));
            else if (success && c.LastAttemptOutcome == "UpToDate" && c.LastSuccessUtc == c.LastAttemptUtc) lines.Add(Strings.T("upd.uptodate"));
            else if (success && c.LastAttemptOutcome == "Incomplete" && c.LastSuccessUtc == c.LastAttemptUtc) lines.Add(Strings.T("upd.incomplete", PerPage * MaxPages));
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

        // The "Released" date of the INSTALLED release, known only when a check saw it (a separate label from the INF
        // driver date); null otherwise.
        public static string InstalledReleased(UpdateCache c, string installed)
        {
            var tag = ReleaseVersion.Parse(c != null ? c.InstalledTag : null);
            var mine = ReleaseVersion.Parse(installed);
            var t = Recovery.Utc(c != null ? c.InstalledPublishedUtc : null);
            return tag != null && mine != null && tag.CompareTo(mine) == 0 && t != null ? t.Value.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture) : null;
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
                        LastAttemptUtc = s("LastAttemptUtc"), LastAttemptOutcome = s("LastAttemptOutcome"), LastSuccessUtc = s("LastSuccessUtc"), CandidateTag = s("CandidateTag"),
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

        static readonly object Gate = new object();
        static Thread _running;
        public static event Action<UpdateResult> Finished;

        public static bool Running { get { lock (Gate) return _running != null; } }

        // Starts a check unless one runs (a click during a check joins it). The result arrives through Finished, on a
        // worker thread; the window marshals it and drops it when it has closed.
        public static void Start(string installed)
        {
            lock (Gate)
            {
                if (_running != null) return;
                _running = new Thread(() => Run(installed)) { IsBackground = true, Name = "update check" };
                _running.Start();
            }
        }

        static void Run(string installed)
        {
            UpdateResult r;
            var box = new UpdateResult[1];
            var worker = new Thread(() => { box[0] = Fetch(installed); }) { IsBackground = true };
            worker.Start();
            if (!worker.Join(TimeSpan.FromSeconds(TotalSeconds + 2))) { lock (Gate) _abort = true; r = new UpdateResult { Outcome = UpdateOutcome.Failed, Detail = "no answer within " + TotalSeconds + " s" }; }
            else r = box[0] ?? new UpdateResult { Outcome = UpdateOutcome.Failed, Detail = "no result" };
            var now = DateTime.UtcNow;
            SaveCache(Record(LoadCache(), r, now));
            lock (Gate) { _running = null; _abort = false; }
            var f = Finished;
            if (f != null) try { f(r); } catch (Exception) { }
        }

        static volatile bool _abort;

        static UpdateResult Fetch(string installed)
        {
            ServicePointManager.SecurityProtocol = SecurityProtocolType.SystemDefault;
            var clock = Stopwatch.StartNew();
            var all = new List<ReleaseEntry>();
            bool complete = false;
            for (int page = 1; page <= MaxPages; page++)
            {
                int left = TotalSeconds * 1000 - (int)clock.ElapsedMilliseconds;
                if (left <= 0 || _abort) return new UpdateResult { Outcome = UpdateOutcome.Failed, Detail = "the check took longer than " + TotalSeconds + " s" };
                var request = (HttpWebRequest)WebRequest.Create(ApiUrl + "?per_page=" + PerPage + "&page=" + page);
                request.Method = "GET";
                request.UserAgent = "amdgpu-wddm-control/" + System.Reflection.Assembly.GetExecutingAssembly().GetName().Version;
                request.Accept = "application/vnd.github+json";
                request.Timeout = left; request.ReadWriteTimeout = left;
                request.AllowAutoRedirect = false;
                string body;
                try
                {
                    using (var response = (HttpWebResponse)request.GetResponse())
                    {
                        if (response.StatusCode != HttpStatusCode.OK) return new UpdateResult { Outcome = UpdateOutcome.Failed, Detail = "HTTP " + (int)response.StatusCode };
                        body = ReadBounded(response);
                        if (body == null) return new UpdateResult { Outcome = UpdateOutcome.Failed, Detail = "the answer is larger than " + MaxBytes + " bytes" };
                    }
                }
                catch (WebException e)
                {
                    var http = e.Response as HttpWebResponse;
                    if (http != null)
                        using (http)
                        {
                            var next = NextAllowed(http.Headers["Retry-After"], http.Headers["X-RateLimit-Reset"], http.Headers["X-RateLimit-Remaining"], DateTime.UtcNow);
                            int code = (int)http.StatusCode;
                            if (code == 403 || code == 429)
                                return new UpdateResult { Outcome = next != null ? UpdateOutcome.RateLimited : UpdateOutcome.Failed, NextAllowedUtc = next, Detail = "HTTP " + code };
                            return new UpdateResult { Outcome = UpdateOutcome.Failed, NextAllowedUtc = next, Detail = "HTTP " + code };
                        }
                    return new UpdateResult { Outcome = UpdateOutcome.Failed, Detail = e.Status.ToString() };
                }
                catch (Exception e) { return new UpdateResult { Outcome = UpdateOutcome.Failed, Detail = e.GetType().Name }; }
                var entries = ParsePage(body);
                if (entries == null) return new UpdateResult { Outcome = UpdateOutcome.Failed, Detail = "the answer is not a list of releases" };
                all.AddRange(entries);
                int items = CountItems(body);
                if (items < PerPage) { complete = true; break; }
            }
            return Decide(all, installed, complete);
        }

        static int CountItems(string json)
        {
            try { var a = new JavaScriptSerializer { MaxJsonLength = MaxBytes }.DeserializeObject(json) as object[]; return a == null ? 0 : a.Length; }
            catch (Exception) { return 0; }
        }

        static string ReadBounded(HttpWebResponse response)
        {
            using (var s = response.GetResponseStream())
            using (var m = new MemoryStream())
            {
                var buffer = new byte[65536];
                int n;
                while ((n = s.Read(buffer, 0, buffer.Length)) > 0)
                {
                    m.Write(buffer, 0, n);
                    if (m.Length > MaxBytes) return null;
                }
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
}
