// The label layout rule of fact M898, held for every window of the application by --smoke-labels in the build.
//
// Label.OnTextChanged calls Label.AdjustSize. With AutoSize off, and no Anchor that ties both sides of an axis, it
// sets Size to the size the program asked for: the last Size the code set, or 100x23, the default of a Label, when the
// code set none. The code never sets the size of a docked Label, its parent's layout does, so at every change of the
// text the Label becomes 100x23 and the parent docks it again right after. Between the two, the parent paints the area
// that the small Label uncovers and the Label keeps the top left 100x23 pixels of its old text: the partial picture of
// the start window of tester.31 rc4, which DWM composed when a composition fell into those 2 ms. Window position traces
// on the development PC with hidden windows (2026-10-14): a Label with AutoSize off and Dock Fill, Top or Left, also in
// a TableLayoutPanel cell, goes through 100x23; with AutoSize on, without Dock (whatever its Anchor), or with Anchor on
// all four sides, it gets its final size at once or keeps its size. Thus the rule is about Dock only, not Anchor. Make
// it wider only after a trace shows an intermediate size for the other layout.
//
// The rule: a Label (or LinkLabel) with AutoSize off has no Dock. Anchor it to the sides that must follow the parent,
// or turn AutoSize on. Check() holds the rule for every Label, and traces the mechanism itself on the labels whose
// text a timer changes while they are on the screen: it changes their digits, as a reading or a countdown does, and
// reports any size the Label had on the way to its final size. SelfCheck() runs both on the shape of M898 first, which
// must fail, and on the application's own label, which must pass; a gate that misses its negative control measures
// nothing. Not held here: a label whose text grows or shrinks across a line break can get an intermediate size too (an
// AutoSize Label with a maximum width in a FlowLayoutPanel, 2026-10-14: 388x34 before 423x17); the timer texts of
// this application change their digits, not their line count.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    static class LabelLayout
    {
        public static bool Docked(Label l) { return !l.AutoSize && l.Dock != DockStyle.None; }

        // What the gate saw: windows and pages checked, labels checked, and how many of them are labels whose text a
        // timer changes while they are on the screen (the positive check of the trace).
        public sealed class Tally
        {
            public int Windows, Labels, Live;
            public readonly List<string> Findings = new List<string>();
        }

        // Every Label under root: the rule; and for the labels in live, the trace of a change of the digits and back.
        public static void Check(Control root, string where, Tally t, ICollection<Label> live = null)
        {
            CreateHandles(root);
            t.Windows++;
            foreach (var l in Labels(root).ToList())
            {
                t.Labels++;
                if (Docked(l)) t.Findings.Add(where + ": " + Name(l) + " has Dock " + l.Dock + " with AutoSize off (M898); anchor it or turn AutoSize on");
                if (live == null || !live.Contains(l)) continue;
                t.Live++;
                string old = l.Text;
                string wrong = Transient(l, NextDigits(old)) ?? Transient(l, old);
                if (wrong != null) t.Findings.Add(where + ": " + Name(l) + " went through " + wrong + " at a change of its text (M898)");
            }
        }

        // The text with every digit one higher (9 becomes 0), as the next reading of a value or a countdown has it; a
        // text without digits gets one, so that the text changes in every case.
        static string NextDigits(string text)
        {
            var s = (text ?? "").ToCharArray();
            bool any = false;
            for (int i = 0; i < s.Length; i++)
                if (s[i] >= '0' && s[i] <= '9') { s[i] = s[i] == '9' ? '0' : (char)(s[i] + 1); any = true; }
            return any ? new string(s) : text + "0";
        }

        // The negative control and the positive control. An empty string is a pass.
        public static string SelfCheck()
        {
            var w = new StringBuilder();
            using (var f = new Form { ShowInTaskbar = false, FormBorderStyle = FormBorderStyle.None, ClientSize = new Size(488, 39) })
            {
                // The start window of rc4: one Label, Dock Fill, AutoSize off, a countdown in its text.
                var bad = new Label { Dock = DockStyle.Fill, Text = StartText };
                f.Controls.Add(bad);
                var t = new Tally();
                Check(f, "negative control", t, new[] { bad });
                if (!t.Findings.Any(x => x.Contains(" has Dock Fill "))) w.AppendLine("negative control: the rule does not find the docked Label");
                if (!t.Findings.Any(x => x.Contains(" went through 100x23 "))) w.AppendLine("negative control: the trace does not see the size 100x23");
            }
            using (var f = new Form { ShowInTaskbar = false, FormBorderStyle = FormBorderStyle.None, ClientSize = new Size(488, 120) })
            {
                // The application's own label (Ui.Label: AutoSize on, a maximum width) in a FlowLayoutPanel, as the
                // live labels of the pages are.
                var stack = Ui.Stack(Theme.S(440));
                var live = Ui.Label(StartText, null, null, Theme.S(440));
                stack.Controls.Add(live);
                f.Controls.Add(stack);
                var t = new Tally();
                Check(f, "positive control", t, new[] { live });
                if (t.Findings.Count > 0) w.AppendLine("positive control: " + string.Join("; ", t.Findings));
                if (t.Live != 1) w.AppendLine("positive control: the trace did not reach the label");
            }
            return w.ToString();
        }

        const string StartText = "Nagi start check: confirming that the GPU driver started correctly.\r\nNothing to do. This window closes by itself in 0:15.";

        static IEnumerable<Label> Labels(Control root)
        {
            foreach (Control c in root.Controls)
            {
                var l = c as Label;
                if (l != null) yield return l;
                foreach (var k in Labels(c)) yield return k;
            }
        }

        static void CreateHandles(Control c)
        {
            if (!c.IsHandleCreated) { var h = c.Handle; }
            foreach (Control k in c.Controls) CreateHandles(k);
        }

        static string Name(Label l)
        {
            string text = l.Text ?? "";
            return l.GetType().Name + (text.Length == 0 ? "" : " \"" + (text.Length > 40 ? text.Substring(0, 40) : text) + "\"");
        }

        // The sizes other than the final one that the Label had while its text changed to text; null for none.
        static string Transient(Label l, string text)
        {
            var trace = new PosTrace(l);
            try
            {
                l.Text = text;
                var wrong = trace.Sizes.Where(s => s != l.Size).Distinct().ToList();
                return wrong.Count == 0 ? null : string.Join(", ", wrong.Select(s => s.Width + "x" + s.Height)) + " before " + l.Width + "x" + l.Height;
            }
            finally { trace.ReleaseHandle(); }
        }

        // The sizes of WM_WINDOWPOSCHANGING that reach one control's window while it is subclassed.
        sealed class PosTrace : NativeWindow
        {
            const int WM_WINDOWPOSCHANGING = 0x0046;
            const uint SWP_NOSIZE = 0x0001;
            public readonly List<Size> Sizes = new List<Size>();

            [StructLayout(LayoutKind.Sequential)]
            struct WindowPos { public IntPtr Hwnd, After; public int X, Y, Cx, Cy; public uint Flags; }

            public PosTrace(Control c) { AssignHandle(c.Handle); }

            protected override void WndProc(ref Message m)
            {
                if (m.Msg == WM_WINDOWPOSCHANGING)
                {
                    var p = (WindowPos)Marshal.PtrToStructure(m.LParam, typeof(WindowPos));
                    if ((p.Flags & SWP_NOSIZE) == 0) Sizes.Add(new Size(p.Cx, p.Cy));
                }
                base.WndProc(ref m);
            }
        }
    }
}
