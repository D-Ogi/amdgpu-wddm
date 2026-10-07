# Provenance

`ste_lint.py` is `scripts/ste-lint.py` from https://github.com/danyuchn/asd-ste100-skill at commit
`7d4a135a199a5d7447c4886bcd7ffe742a627bc9` (2026-09-08), MIT licence (`LICENSE-asd-ste100-skill.txt`),
taken unmodified on 2026-10-07 and renamed only so that Python can import it.

| File | SHA-256 of the bytes, which is the SHA-256 of `git show 7d4a135a:<path>` |
|---|---|
| `ste_lint.py` (`scripts/ste-lint.py`) | `73bbd3de05b6517428ff6df73b789248a2d11f9eaf8d413fee56bbe71b5f863b` |
| `LICENSE-asd-ste100-skill.txt` (`LICENSE`) | `d3c674e8592076c9021a59b55177adbcba6fd7e1710082becb922522b22ec672` |

LF line endings, pinned in `.gitattributes`, so the digests hold on every machine.
`python tools/quality/third_party/ste_lint.py --selftest` is the upstream self-test.

Stdlib only. It checks the structural ASD-STE100 rules, and carries none of the ASD standard's
text or dictionary. `tools/quality/doclint.py` wraps it and adds the project rules of
`docs/style.md`. Our own changes belong in `doclint.py`, never in this directory, so that a newer
upstream revision drops in by one copy.
