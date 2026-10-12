// Removes personal data from bug report text before it is shown or written: the user name and profile path, the
// computer name, MAC addresses, serial numbers, machine ids and e-mail addresses. Pure: the caller passes the names.
// Driver and file versions, hashes, LUIDs and PnP hardware ids stay, because a report without them is useless.
using System;
using System.Collections.Generic;
using System.Text.RegularExpressions;

namespace AmdgpuWddmControl
{
    public sealed class Redactor
    {
        readonly List<KeyValuePair<Regex, string>> _rules = new List<KeyValuePair<Regex, string>>();

        const RegexOptions Options = RegexOptions.CultureInvariant | RegexOptions.IgnoreCase | RegexOptions.Multiline;

        public Redactor(string userName, string computerName, string userProfile)
        {
            // Longest first: the profile path contains the user name.
            if (!string.IsNullOrEmpty(userProfile) && userProfile.Length > 3)
                Literal(userProfile, @"%USERPROFILE%");
            if (!string.IsNullOrEmpty(userName) && userName.Length >= 2)
            {
                Rule(@"(?<=\\Users\\)" + Regex.Escape(userName) + @"(?=\\|\b)", "<user>");
                Rule(@"(?<![A-Za-z0-9])" + Regex.Escape(userName) + @"(?![A-Za-z0-9])", "<user>");
            }
            if (!string.IsNullOrEmpty(computerName) && computerName.Length >= 2)
                Rule(@"(?<![A-Za-z0-9])" + Regex.Escape(computerName) + @"(?![A-Za-z0-9])", "<computer>");
            // Lines that name a serial number or a machine id: keep the label, drop the value. The part before
            // the keyword may carry colons of its own, because almost every line of a collected log has a
            // prefix with one: a setupapi line starts "     dvi: ", an event line with an ISO timestamp, a
            // record line with "StartRecord log:". A label part that forbade a colon matched only when the
            // keyword stood before the first colon of the line, which left "     dvi: Device serial: <value>"
            // and every event message that names a serial number or a machine id in a report untouched. From
            // the keyword on, the label still ends at the first colon or equals sign, so the value goes.
            // No $ at the end either: under Multiline, $ matches before a line feed only, so in text with
            // Windows line endings - every log this reads - the position before the carriage return was no
            // line end and the rule matched nothing at all. The value part stops at the carriage return by
            // itself, so the line ending survives the replacement.
            // Neither half may cross an angle bracket. A record of an event is ONE line of XML, and without
            // that bound a keyword anywhere in it (an AdapterUuid attribute, say) let the label run to the
            // next "=" of the markup and the value swallow the rest of the line, so the report lost the
            // message it was collected for and kept a label such as "<Data Name=". Nothing leaked; the
            // reading was gone. The rule after it takes the value of such a markup element instead, which
            // is where that value really stands.
            Rule(@"^(?<label>[^\r\n]*?(serial|machine id|machineguid|product id|uuid)[^\r\n:=<>]*[:=])[^\r\n<>]*", "${label} <redacted>");
            // The same keywords inside one line of markup: an attribute or an element whose name carries
            // the keyword, and whose value stands between ">" and the next "<". A quote and a few
            // characters may stand between the keyword and that ">", and the value holds no angle bracket.
            Rule(@"(?<=(serial|machine id|machineguid|product id|uuid)[^\r\n<>]{0,48}>)[^<\r\n]+", "<redacted>");
            // MAC addresses: six pairs with : or -, or twelve hex digits after a "MAC"/"address" label.
            Rule(@"(?<![0-9A-F:-])([0-9A-F]{2}[:-]){5}[0-9A-F]{2}(?![0-9A-F:-])", "<mac>");
            Rule(@"(?<=(mac|physical address|networkaddress)\W{1,4})[0-9A-F]{12}\b", "<mac>");
            Rule(@"\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}\b", "<email>");
            // SetupAPI and event XML may name another account, not only the
            // person collecting the report. Keep the rest of the driver path.
            Rule(@"(?<=\\Users\\)[^\\\r\n<>""/]+", "<user>");
            Rule(@"\bS-1-\d+(?:-\d+)+\b", "<sid>");
            Rule(@"(?<=<Computer>)[^<]+(?=</Computer>)", "<computer>");
        }

        void Rule(string pattern, string replacement) { _rules.Add(new KeyValuePair<Regex, string>(new Regex(pattern, Options), replacement)); }
        void Literal(string text, string replacement) { Rule(Regex.Escape(text), replacement.Replace("$", "$$")); }

        public string Apply(string text)
        {
            if (string.IsNullOrEmpty(text)) return text ?? "";
            foreach (var r in _rules) text = r.Key.Replace(text, r.Value);
            return text;
        }

        public static Redactor ForThisPc()
        {
            return new Redactor(Environment.UserName, Environment.MachineName, Environment.GetFolderPath(Environment.SpecialFolder.UserProfile));
        }
    }
}
