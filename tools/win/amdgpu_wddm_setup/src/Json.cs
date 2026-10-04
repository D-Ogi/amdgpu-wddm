// Small readers over System.Web.Extensions' JavaScriptSerializer: objects come back as Dictionary<string, object>,
// arrays as object[], numbers as int, long or decimal. Every reader returns null (or the default) for a missing key
// or another type, never throws: a damaged record reads as "no record".
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Web.Script.Serialization;

namespace AmdgpuWddmSetup
{
    public static class Json
    {
        public static IDictionary<string, object> Parse(string text)
        {
            if (string.IsNullOrWhiteSpace(text)) return null;
            try { return new JavaScriptSerializer { MaxJsonLength = 32 * 1024 * 1024 }.DeserializeObject(text) as IDictionary<string, object>; }
            catch (Exception) { return null; }
        }

        static object Get(IDictionary<string, object> o, string key)
        {
            object v;
            return o != null && key != null && o.TryGetValue(key, out v) ? v : null;
        }

        public static string Str(IDictionary<string, object> o, string key)
        {
            var v = Get(o, key);
            if (v == null) return null;
            if (v is string) return (string)v;
            if (v is bool) return (bool)v ? "true" : "false";
            if (v is int || v is long || v is decimal || v is double) return Convert.ToString(v, CultureInfo.InvariantCulture);
            return null;
        }

        public static bool Bool(IDictionary<string, object> o, string key) { var v = Get(o, key); return v is bool && (bool)v; }

        public static long? Long(IDictionary<string, object> o, string key)
        {
            var v = Get(o, key);
            if (v is int) return (int)v;
            if (v is long) return (long)v;
            if (v is decimal) { var d = (decimal)v; if (d == Math.Truncate(d)) return (long)d; }
            return null;
        }

        public static int Int(IDictionary<string, object> o, string key, int fallback = 0) { var v = Long(o, key); return v.HasValue && v.Value >= int.MinValue && v.Value <= int.MaxValue ? (int)v.Value : fallback; }

        public static IDictionary<string, object> Obj(IDictionary<string, object> o, string key) { return Get(o, key) as IDictionary<string, object>; }

        public static object[] Arr(IDictionary<string, object> o, string key) { return Get(o, key) as object[] ?? new object[0]; }

        public static string[] Strs(IDictionary<string, object> o, string key) { return Arr(o, key).OfType<string>().ToArray(); }

        // A value as it is shown in the support file (numbers, strings, true/false, "-" for null).
        public static string Show(object v)
        {
            if (v == null) return "-";
            if (v is string) return (string)v;
            if (v is bool) return (bool)v ? "true" : "false";
            if (v is object[]) return string.Join(",", ((object[])v).Select(Show));
            return Convert.ToString(v, CultureInfo.InvariantCulture);
        }
    }
}
