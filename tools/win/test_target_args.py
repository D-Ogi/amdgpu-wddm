"""Host tests for the argument quoting of `target.py ps` (Target.run_script).

No network: the ssh call is replaced and only the command line it would send is checked. Run:
    python -m unittest discover -s tools/win -p "test_target*.py"
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import target  # noqa: E402

EXPECT = "amdgpu_wddm_d3d12.dll=" + "A" * 64 + ";amdgpu_wddm_vkd3d.dll=" + "B" * 64


class ScriptArg(unittest.TestCase):
    def test_a_plain_argument_stays_bare(self):
        self.assertEqual(target.script_arg("-Step"), "-Step")
        self.assertEqual(target.script_arg("putback"), "putback")
        self.assertEqual(target.script_arg(120), "120")
        self.assertEqual(target.script_arg("C:\\BC250\\tmp\\stage"), "C:\\BC250\\tmp\\stage")

    def test_a_semicolon_list_is_one_quoted_argument(self):
        # 2026-10-11: unquoted, the remote PowerShell ended the statement at the first ';'.
        self.assertEqual(target.script_arg(EXPECT), f'"{EXPECT}"')

    def test_spaces_and_other_metacharacters_are_quoted(self):
        for text in ("a b", "a,b", "a|b", "a&b", "(a)", "{a}", "@a", "a#b", "it's"):
            self.assertEqual(target.script_arg(text), f'"{text}"', text)

    def test_run_script_sends_the_list_whole(self):
        sent = []
        t = target.Target.__new__(target.Target)
        t.cfg = {"tmp_dir": "C:\\BC250\\tmp"}
        t.push = lambda paths, remote_dir=None, timeout=300: None
        t.ssh = lambda command, timeout=None: sent.append(command)
        t.run_script("lab/d3d12-putback.ps1", ["-Step", "putback", "-Expect", EXPECT])
        self.assertEqual(sent, ['powershell -NoProfile -ExecutionPolicy Bypass -File '
                                f'"C:\\BC250\\tmp\\d3d12-putback.ps1" -Step putback -Expect "{EXPECT}"'])


if __name__ == "__main__":
    unittest.main()
