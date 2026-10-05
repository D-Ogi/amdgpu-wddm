#!/usr/bin/env bash
# Round trip of the emergency channel in C:\BC250\tmp\emerg-test (no other path touched); usb-boot status only.
# THIS TOUCHES THE LAB: it writes, reads, copies, moves and deletes files on unit A and runs two short scripts
# there. It is not a host test and it is not part of tools/quality/quick.ps1. Scratch files stay under
# <BC250_ROOT>/scratch; BC250_ROOT defaults to the parent directory of this repository.
here=$(cd "$(dirname "$0")" && pwd) || exit 1
root=${BC250_ROOT:-$(cd "$here/../../../.." && pwd)}
cd "$here" || exit 1
e() { python lab-emerg.py "$@"; }
t='C:\BC250\tmp\emerg-test'
mkdir -p "$root/scratch" || exit 1
s=$(mktemp -d -p "$root/scratch")
head -c 300000 /dev/urandom > "$s/blob.bin"
printf 'line one\nline two\n' > "$s/a.txt"
printf 'replaced\n' > "$s/b.txt"
echo "== status";  e status | grep -E '"ok"|"sshd"|"boot"'
echo "== mkdir";   e mkdir "$t" | grep -E '"ok"|Administrators|BUILTIN|bc250' | head -6
echo "== put new"; e put "$s/blob.bin" "$t\\blob.bin" | grep -E '"ok"|sha256|replaced|bytes'
sha256sum "$s/blob.bin" | cut -c1-64
echo "== get";     e get "$t\\blob.bin" "$s/back.bin"; cmp "$s/blob.bin" "$s/back.bin" && echo "get identical"
echo "== put a";   e put "$s/a.txt" "$t\\a.txt" | grep -E '"ok"|replaced'
echo "== acl a";   e acl "$t\\a.txt" | grep -vE '^\[|^\]|^ *"(ok|action|path)"|[{}]' | head -8
echo "== put over"; e put "$s/b.txt" "$t\\a.txt" | grep -E '"ok"|replaced|backup'
echo "== tail";    e tail "$t\\a.txt" 5 | grep -E 'replaced|line'
echo "== copy";    e copy "$t\\a.txt" "$t\\c.txt" | grep -E '"ok"|sha256'
echo "== move";    e move "$t\\c.txt" "$t\\d.txt" | grep -E '"ok"|"to"'
echo "== list";    e list "$t" | grep -E '\.(txt|bin|bak)'
echo "== ps";      e ps -c 'whoami; (Get-Item C:\BC250\tmp\emerg-test\a.txt).GetAccessControl().Owner' 30 | grep -E '"state"|\\|system'
echo "== usb";     e usb-boot status
echo "== clean";   e ps -c 'Remove-Item C:\BC250\tmp\emerg-test -Recurse -Force; Test-Path C:\BC250\tmp\emerg-test' | grep -E '"state"|True|False'
rm -rf "$s"
