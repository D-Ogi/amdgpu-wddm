// The window's text in four languages (WU-003): one table per language, embedded in the exe as strings.<lang>.txt
// (tools/win/amdgpu_wddm_control/strings/). Every visible sentence of the window, the recovery view and the help comes
// from here; the support report and the command line stay English (they are for people who fix things).
//
// Line format, one string per line, UTF-8:   id|status|source-hash|text
//   status       src (English source), rev (a named reviewer read it), mt (machine translation), mt-safe (machine
//                translation of a short label that cannot mislead). Review status says nothing about freshness.
//   source-hash  English: "-". A translation: the first 8 hex digits of SHA-256 over the UTF-8 bytes of the English
//                text it was made from. When the English text changes, the hash no longer matches: the translation
//                is stale, the build fails (G-STR, A6) and the window shows the English text until a new translation
//                comes with its new hash. Nothing refreshes a hash without a new translation.
//   text         "\n" is a line break; {0} {1} ... are the arguments, the same set in every language.
// Lines starting with # and empty lines are comments.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

namespace AmdgpuWddmControl
{
    public sealed class StringEntry
    {
        public string Id, Status, Hash, Text;
        public int Line;
    }

    public sealed class StringTable
    {
        public string Language;
        public readonly Dictionary<string, StringEntry> Entries = new Dictionary<string, StringEntry>(StringComparer.Ordinal);
        public readonly List<string> Errors = new List<string>();
    }

    public static class Strings
    {
        public static readonly string[] Languages = { "en", "pl", "ja", "ko" };
        public static readonly string[] Statuses = { "src", "rev", "mt", "mt-safe" };

        static readonly object Gate = new object();
        static Dictionary<string, StringTable> _tables;
        static string _language = "en";

        // AMDGPU_WDDM_CONTROL_STRINGS: a directory with strings.<lang>.txt read instead of the embedded tables (tests).
        public static string Directory { get; set; }

        public static event Action LanguageChanged;

        public static string Language
        {
            get { return _language; }
            set
            {
                var v = Languages.Contains(value) ? value : "en";
                if (v == _language) return;
                _language = v;
                var e = LanguageChanged;
                if (e != null) e();
            }
        }

        // The language of Windows' user interface when it is one of the four, else English.
        public static string SystemLanguage()
        {
            var name = CultureInfo.CurrentUICulture.TwoLetterISOLanguageName;
            return Languages.Contains(name) ? name : "en";
        }

        public static string Hash8(string english)
        {
            using (var sha = SHA256.Create())
                return BitConverter.ToString(sha.ComputeHash(Encoding.UTF8.GetBytes(english ?? ""))).Replace("-", "").Substring(0, 8).ToLowerInvariant();
        }

        public static StringTable Parse(string language, string text)
        {
            var t = new StringTable { Language = language };
            var lines = (text ?? "").Replace("\r\n", "\n").Split('\n');
            for (int i = 0; i < lines.Length; i++)
            {
                var l = lines[i];
                if (l.Length > 0 && l[0] == '﻿') l = l.Substring(1);
                if (l.Trim().Length == 0 || l.StartsWith("#", StringComparison.Ordinal)) continue;
                var parts = l.Split(new[] { '|' }, 4);
                if (parts.Length != 4) { t.Errors.Add(language + ":" + (i + 1) + ": not id|status|hash|text"); continue; }
                var e = new StringEntry { Id = parts[0], Status = parts[1], Hash = parts[2], Text = parts[3].Replace("\\n", "\n"), Line = i + 1 };
                if (!Regex.IsMatch(e.Id, @"^[a-z0-9]+(\.[a-z0-9-]+)+$")) t.Errors.Add(language + ":" + e.Line + ": bad id " + e.Id);
                if (t.Entries.ContainsKey(e.Id)) t.Errors.Add(language + ":" + e.Line + ": duplicate id " + e.Id);
                t.Entries[e.Id] = e;
            }
            return t;
        }

        static string ReadSource(string language)
        {
            var dir = Directory ?? Environment.GetEnvironmentVariable("AMDGPU_WDDM_CONTROL_STRINGS");
            if (!string.IsNullOrEmpty(dir))
            {
                var path = Path.Combine(dir, "strings." + language + ".txt");
                return File.Exists(path) ? File.ReadAllText(path, Encoding.UTF8) : null;
            }
            using (var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("strings." + language + ".txt"))
            {
                if (s == null) return null;
                using (var r = new StreamReader(s, Encoding.UTF8)) return r.ReadToEnd();
            }
        }

        public static Dictionary<string, StringTable> Tables
        {
            get
            {
                lock (Gate)
                {
                    if (_tables == null)
                    {
                        var all = new Dictionary<string, StringTable>(StringComparer.Ordinal);
                        foreach (var lang in Languages)
                        {
                            var text = ReadSource(lang);
                            all[lang] = text == null ? new StringTable { Language = lang } : Parse(lang, text);
                            if (text == null) all[lang].Errors.Add(lang + ": strings." + lang + ".txt is missing");
                        }
                        _tables = all;
                    }
                    return _tables;
                }
            }
        }

        public static void Reload() { lock (Gate) _tables = null; }

        public static bool Has(string id) { return Tables["en"].Entries.ContainsKey(id); }

        // The text of id in the current language: the translation when it exists and is fresh, else English; an id
        // that English lacks shows as [id] (the build's G-STR check makes that impossible in a release).
        public static string T(string id, params object[] args) { return In(_language, id, args); }

        public static string In(string language, string id, params object[] args)
        {
            var tables = Tables;
            StringEntry en, tr;
            if (!tables["en"].Entries.TryGetValue(id, out en)) return "[" + id + "]";
            string text = en.Text;
            StringTable table;
            if (language != "en" && tables.TryGetValue(language, out table) && table.Entries.TryGetValue(id, out tr) && tr.Hash == Hash8(en.Text))
                text = tr.Text;
            if (args == null || args.Length == 0) return text;
            try { return string.Format(CultureInfo.InvariantCulture, text, args); }
            catch (FormatException) { return string.Format(CultureInfo.InvariantCulture, en.Text, args); }
        }

        // The placeholders a text uses, as a sorted set.
        public static string Placeholders(string text)
        {
            return string.Join(",", Regex.Matches(text ?? "", @"\{(\d+)(:[^}]*)?\}").Cast<Match>().Select(m => int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture))
                .Distinct().OrderBy(n => n));
        }

        // G-STR: every language has every English id with the same placeholders and a fresh source hash, no extra ids,
        // known statuses only, English marked src. Returns the problems; empty = pass.
        public static List<string> Validate(Dictionary<string, StringTable> tables)
        {
            var problems = new List<string>();
            foreach (var t in tables.Values) problems.AddRange(t.Errors);
            StringTable en;
            if (!tables.TryGetValue("en", out en)) { problems.Add("no English table"); return problems; }
            foreach (var e in en.Entries.Values)
            {
                if (e.Status != "src") problems.Add("en:" + e.Line + ": " + e.Id + " status " + e.Status + ", src expected");
                if (e.Hash != "-") problems.Add("en:" + e.Line + ": " + e.Id + " has a source hash");
                if (e.Text.Trim().Length == 0) problems.Add("en:" + e.Line + ": " + e.Id + " is empty");
            }
            foreach (var lang in Languages.Where(l => l != "en"))
            {
                StringTable t;
                if (!tables.TryGetValue(lang, out t)) { problems.Add(lang + ": table missing"); continue; }
                foreach (var e in en.Entries.Values)
                {
                    StringEntry tr;
                    if (!t.Entries.TryGetValue(e.Id, out tr)) { problems.Add(lang + ": " + e.Id + " missing"); continue; }
                    if (!Statuses.Contains(tr.Status) || tr.Status == "src") problems.Add(lang + ":" + tr.Line + ": " + e.Id + " status " + tr.Status);
                    if (tr.Hash != Hash8(e.Text)) problems.Add(lang + ":" + tr.Line + ": " + e.Id + " is stale (source hash " + tr.Hash + ", English now " + Hash8(e.Text) + ")");
                    if (Placeholders(tr.Text) != Placeholders(e.Text)) problems.Add(lang + ":" + tr.Line + ": " + e.Id + " placeholders {" + Placeholders(tr.Text) + "} differ from English {" + Placeholders(e.Text) + "}");
                    if (tr.Text.Trim().Length == 0) problems.Add(lang + ":" + tr.Line + ": " + e.Id + " is empty");
                }
                foreach (var id in t.Entries.Keys.Where(k => !en.Entries.ContainsKey(k))) problems.Add(lang + ": " + id + " is not an English id");
            }
            return problems;
        }

        // The review queue: every translation not yet reviewed (mt first, then mt-safe), for the Ph 2 native review.
        public static List<string> ReviewQueue(Dictionary<string, StringTable> tables)
        {
            return Languages.Where(l => l != "en" && tables.ContainsKey(l))
                .SelectMany(l => tables[l].Entries.Values.Where(e => e.Status == "mt" || e.Status == "mt-safe")
                    .OrderBy(e => e.Status == "mt" ? 0 : 1).ThenBy(e => e.Id, StringComparer.Ordinal).Select(e => l + " " + e.Status + " " + e.Id)).ToList();
        }
    }
}
