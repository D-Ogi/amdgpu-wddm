// A copy of the control app's Redactor.cs. Removes personal data from support-file text before it is written: the
// user name and profile path, the computer name, MAC addresses, serial numbers, machine ids and e-mail addresses.
// Pure: the caller passes the names.
// Driver and file versions, hashes, LUIDs and PnP hardware ids stay, because a report without them is useless.
using System;
using System.Collections.Generic;
using System.Text.RegularExpressions;

namespace AmdgpuWddmSetup
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
            // Lines that name a serial number or a machine id: keep the label, drop the value.
            Rule(@"^(?<label>[^\r\n:=]*(serial|machine id|machineguid|product id|uuid)[^\r\n:=]*[:=])[^\r\n]*$", "${label} <redacted>");
            // MAC addresses: six pairs with : or -, or twelve hex digits after a "MAC"/"address" label.
            Rule(@"(?<![0-9A-F:-])([0-9A-F]{2}[:-]){5}[0-9A-F]{2}(?![0-9A-F:-])", "<mac>");
            Rule(@"(?<=(mac|physical address|networkaddress)\W{1,4})[0-9A-F]{12}\b", "<mac>");
            Rule(@"\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}\b", "<email>");
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
