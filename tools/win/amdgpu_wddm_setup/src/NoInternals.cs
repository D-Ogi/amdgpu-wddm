// G-NOINT (plan v7 R1): words a user of the setup window never sees. The check runs over every string table and over
// the text of every rendered screen, in all four languages (Latin technical terms show up in JA and KO text too).
// The product name amdgpu-wddm is allowed; the release notes are release content and exempt, the support file is
// English technical detail and never shown in the window.
using System.Collections.Generic;
using System.Linq;
using System.Text.RegularExpressions;

namespace AmdgpuWddmSetup
{
    public static class NoInternals
    {
        static readonly Regex Banned = new Regex(
            @"\b(KMD|UMD|ICD|DWM|WDDM|DXGI|fences?|HRESULT|NTSTATUS|0x[0-9A-F]+|exit code|error code|escape|pnputil|bcdedit|" +
            @"registry|HKLM|HKCU|PowerShell|RunOnce|manifest|invocation|closure|SHA-?256|BD-\d+|stack trace|exception|" +
            @"[\w-]+\.(ps1|sys|inf|json|cmd|dll))\b",
            RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);

        public static List<string> Find(string text)
        {
            var clean = Regex.Replace(text ?? "", "amdgpu-wddm", "", RegexOptions.IgnoreCase);
            return Banned.Matches(clean).Cast<Match>().Select(m => m.Value).Distinct().ToList();
        }
    }
}
