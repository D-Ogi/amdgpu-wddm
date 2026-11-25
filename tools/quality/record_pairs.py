#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The gate of the record pair registry (driver/contract/record-pairs.json).

Static check (no argument but --root):
  1. Every RULE line of driver/contract/test/record-pair-test.cpp names a file of the tree that still contains its
     text. A restated rule that no longer matches the rule it states fails here.
  2. Every write of a record magic (LB7A, GDI1, E26R, BC2A, S26R) in driver/, by its macro, its number or an alias,
     lies in a function that the registry lists as a producer site, and every listed site still writes one.
     A new producer of a record without its registry entry fails here.
  3. The registry lists every producer and consumer pair, each with the rows of the test that cover it or the
     reason why no caller reaches it.
--results <pairs.tsv>: the output of record-pair-test.exe against the registry:
  4. Every row of the output is covered by a pair of the registry, and every row prefix of a pair matches a row.
  5. A FAIL row fails the gate unless it is in expected_red. An expected-red row has an owner and an "until" text:
     while the text is not in its file, the row must be FAIL (the fix has not landed); once the text is there, the
     row must pass. So an expected-red row turns green with the landing of its fix, and a row that passes while
     its fix is absent fails the gate: the registry entry is wrong.
--self-test: plants an unlisted producer, a dead RULE text and the wrong expected-red states in a small tree of its
  own, and fails unless each of them fails the check.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import tempfile
from pathlib import Path

REGISTRY = 'driver/contract/record-pairs.json'
TEST = 'driver/contract/test/record-pair-test.cpp'

# The magics of the five record forms: the macro of the contract and the number.
MAGICS = {
    'LB7A': ('BC250_WDDM_ALLOCATION_PRIVATE_MAGIC', 0x4137424C),
    'GDI1': ('BC250_GDI_PRIVATE_MAGIC', 0x31494447),
    'E26R': ('BC250_SURFACE_RESOURCE_MAGIC', 0x52363245),
    'BC2A': ('BC250_UMD_ALLOC_MAGIC', 0x41324342),
    'S26R': ('BC250_SHARED_RESOURCE_MAGIC', 0x52363253),
}
SOURCE_SUFFIXES = {'.c', '.h', '.cpp', '.hpp', '.inl', '.patch'}
RULE = re.compile(r'^\s*//\s*RULE\s+(\S+)\s+"(.*)"\s*$')
KEYWORDS = {'if', 'for', 'while', 'switch', 'return', 'else', 'case', 'sizeof', 'catch', 'do', 'goto', 'throw',
            'static_assert', 'decltype', 'new', 'delete', 'co_return'}
HEADER = re.compile(r'^(\s*)((?:[A-Za-z_][\w:<>,\*&]*\s+)+)[\*&]*\s*(~?[A-Za-z_]\w*(?:::~?\w+)*)\s*\(')


def is_test_path(rel: str) -> bool:
    parts = rel.replace('\\', '/').split('/')
    name = parts[-1]
    return ('test' in parts or 'tests' in parts or '-test.' in name or '_test.' in name or
            name.startswith('test-') or name.startswith('test_'))


def source_lines(path: Path) -> list[str]:
    """The lines of a source file. A patch gives its added lines only, without the '+'."""
    text = path.read_text(encoding='utf-8', errors='replace').splitlines()
    if path.suffix != '.patch':
        return text
    # The function name that git writes after a hunk header stands for the header of the function, which the
    # hunk often does not contain.
    out = []
    for line in text:
        hunk = re.match(r'@@ [^@]* @@\s*(.*\S)?', line)
        if hunk:
            out.append('hunk ' + hunk.group(1) if hunk.group(1) else '')
        elif line.startswith('+') and not line.startswith('+++'):
            out.append(line[1:])
        else:
            out.append('')
    return out


def magic_names(files: dict[str, list[str]]) -> dict[str, str]:
    """Every name that means a record magic: the contract macro, and each alias the tree defines for a magic
    (a #define or a constexpr of the macro or of the number)."""
    names = {macro: form for form, (macro, _) in MAGICS.items()}
    numbers = {value: form for form, (_, value) in MAGICS.items()}
    alias = re.compile(r'(?:#\s*define\s+(\w+)\s+(\S+)|constexpr\s+[\w:]+\s+(\w+)\s*=\s*([^;]+);)')
    for lines in files.values():
        for line in lines:
            for m in alias.finditer(line):
                name, value = (m.group(1), m.group(2)) if m.group(1) else (m.group(3), m.group(4))
                value = value.strip().rstrip('uUlL').strip('()')
                form = names.get(value)
                if form is None:
                    try:
                        form = numbers.get(int(value.rstrip('uUlL'), 0))
                    except ValueError:
                        form = None
                if form and name not in names:
                    names[name] = form
    return names


def enclosing_function(lines: list[str], index: int) -> str:
    """The name of the function that contains line index: the nearest line above it, less indented, that reads as
    a function header. A heuristic that the self-test pins."""
    site_indent = len(lines[index]) - len(lines[index].lstrip())
    for i in range(index - 1, -1, -1):
        line = lines[i]
        if not line.strip() or line.lstrip().startswith(('//', '/*', '*', '#')):
            continue
        indent = len(line) - len(line.lstrip())
        if indent >= site_indent:
            continue
        m = HEADER.match(line)
        if not m or '=' in line.split('(')[0] or line.rstrip().endswith(';'):
            continue
        first = m.group(2).split()[0]
        if first in KEYWORDS or m.group(3) in KEYWORDS:
            continue
        return m.group(3).split('::')[-1]
    return '<file scope>'


def write_sites(root: Path) -> list[tuple[str, str, str, int]]:
    """(file, function, form, line) of every write of a record magic in driver/, tests excluded."""
    files: dict[str, list[str]] = {}
    for path in sorted((root / 'driver').rglob('*')):
        rel = path.relative_to(root).as_posix()
        if path.is_file() and path.suffix in SOURCE_SUFFIXES and not is_test_path(rel):
            files[rel] = source_lines(path)
    names = magic_names(files)
    numbers = '|'.join(f'0x{value:08X}' for _, value in MAGICS.values())
    word = '|'.join(re.escape(n) for n in sorted(names, key=len, reverse=True))
    # A write: "= MAGIC" that is not a comparison, or MAGIC as the first value of a brace initializer.
    write = re.compile(rf'(?:(?<![=!<>])=\s*|\{{\s*)(?P<n>{word}|(?:{numbers})[uUlL]*)(?![\w(])', re.IGNORECASE)
    sites = []
    for rel, lines in files.items():
        for i, line in enumerate(lines):
            code = line.split('//')[0]
            if re.match(r'\s*#\s*define\b', code) or re.search(r'\bconstexpr\b', code):
                continue
            for m in write.finditer(code):
                token = m.group('n')
                form = names.get(token)
                if form is None:
                    form = next(f for f, (_, v) in MAGICS.items() if int(token.rstrip('uUlL'), 16) == v)
                sites.append((rel, enclosing_function(lines, i), form, i + 1))
    return sites


def check_rules(root: Path) -> list[str]:
    test = root / TEST
    if not test.is_file():
        return [f'{TEST} missing']
    errors, count = [], 0
    for n, line in enumerate(test.read_text(encoding='utf-8').splitlines(), 1):
        m = RULE.match(line)
        if not m:
            continue
        count += 1
        target = root / m.group(1)
        if not target.is_file():
            errors.append(f'{TEST}:{n}: RULE names {m.group(1)}, which is not in the tree')
        elif m.group(2) not in target.read_text(encoding='utf-8', errors='replace'):
            errors.append(f'{TEST}:{n}: RULE text is no longer in {m.group(1)}: "{m.group(2)}"')
    if not count:
        errors.append(f'{TEST}: no RULE line')
    return errors


def load_registry(root: Path) -> dict:
    return json.loads((root / REGISTRY).read_text(encoding='utf-8'))


def check_registry(root: Path, registry: dict) -> list[str]:
    errors = []
    producers = {p['id']: p for p in registry.get('producers', [])}
    consumers = {c['id']: c for c in registry.get('consumers', [])}
    listed: dict[tuple[str, str], str] = {}
    for p in producers.values():
        for site in p.get('sites', []):
            key = (site['file'], site['function'])
            listed[key] = listed[key] + ',' + p['id'] if key in listed else p['id']
    seen = set()
    for rel, function, form, line in write_sites(root):
        key = (rel, function)
        if key not in listed:
            errors.append(f'{rel}:{line}: {function} writes the {form} magic and is no producer site of {REGISTRY}')
        seen.add(key)
    for key, pid in listed.items():
        if key not in seen:
            errors.append(f'{REGISTRY}: producer {pid} names {key[0]} {key[1]}, which writes no record magic')
    for c in consumers.values():
        path = root / c['file']
        if not path.is_file() or c['function'] not in path.read_text(encoding='utf-8', errors='replace'):
            errors.append(f'{REGISTRY}: consumer {c["id"]} names {c["file"]} {c["function"]}, which is not there')
    pairs = {}
    for entry in registry.get('pairs', []):
        key = (entry.get('producer'), entry.get('consumer'))
        if key[0] not in producers or key[1] not in consumers:
            errors.append(f'{REGISTRY}: pair {key} names an unknown producer or consumer')
            continue
        if key in pairs:
            errors.append(f'{REGISTRY}: pair {key} is listed twice')
        kinds = [k for k in ('rows', 'covered_by', 'unreachable') if entry.get(k)]
        if len(kinds) != 1:
            errors.append(f'{REGISTRY}: pair {key} needs exactly one of rows, covered_by and unreachable')
        elif kinds[0] == 'covered_by' and not (root / entry['covered_by'].split()[0]).is_file():
            errors.append(f'{REGISTRY}: pair {key} is covered by {entry["covered_by"].split()[0]}, which is not in the tree')
        elif kinds[0] == 'rows':
            for pattern in entry['rows']:
                try:
                    re.compile(pattern)
                except re.error as exc:
                    errors.append(f'{REGISTRY}: pair {key} row pattern "{pattern}": {exc}')
        pairs[key] = entry
    for pid in (p for p, entry in producers.items() if entry.get('wire', True)):
        for cid in consumers:
            if (pid, cid) not in pairs:
                errors.append(f'{REGISTRY}: no entry for the pair {pid} -> {cid}')
    for red in registry.get('expected_red', []):
        if not red.get('owner') or not red.get('reason') or not red.get('until', {}).get('text'):
            errors.append(f'{REGISTRY}: expected_red {red.get("row")} needs an owner, a reason and an until text')
    return errors


def read_results(path: Path) -> dict[str, str]:
    rows = {}
    for line in path.read_text(encoding='utf-8-sig').splitlines():
        fields = line.split('\t')
        if len(fields) >= 2 and fields[0] in ('ok', 'FAIL', 'NOTE'):
            rows[fields[1]] = fields[0]
    return rows


def check_results(root: Path, registry: dict, rows: dict[str, str]) -> tuple[list[str], list[str]]:
    errors, notes = [], []
    if not rows:
        return [f'no result rows'], notes
    patterns = [re.compile(p) for entry in registry.get('pairs', []) for p in entry.get('rows', [])]
    for pattern in patterns:
        if not any(pattern.fullmatch(name) for name in rows):
            errors.append(f'{REGISTRY}: row pattern "{pattern.pattern}" matches no row of the test')
    for name in rows:
        if not any(pattern.fullmatch(name) for pattern in patterns):
            errors.append(f'row "{name}" belongs to no pair of {REGISTRY}')
    red = {r['row']: r for r in registry.get('expected_red', [])}
    for name, red_entry in red.items():
        if name not in rows:
            errors.append(f'{REGISTRY}: expected_red row "{name}" is not a row of the test')
    for name, status in sorted(rows.items()):
        entry = red.get(name)
        if entry is None:
            if status == 'FAIL':
                errors.append(f'FAIL "{name}"')
            continue
        until = entry['until']
        target = root / until['file']
        landed = target.is_file() and until['text'] in target.read_text(encoding='utf-8', errors='replace')
        if landed and status == 'FAIL':
            errors.append(f'FAIL "{name}": its fix ({entry["owner"]}) is in the tree, the row must pass')
        elif not landed and status != 'FAIL':
            errors.append(f'"{name}" passes, but it is expected red until {entry["owner"]} lands '
                          f'("{until["text"]}" in {until["file"]}): correct the registry entry')
        elif landed:
            notes.append(f'landed: "{name}" ({entry["owner"]}); its expected_red entry can go')
        else:
            notes.append(f'expected red: "{name}" until {entry["owner"]}')
    return errors, notes


def run(root: Path, results: Path | None) -> int:
    registry = load_registry(root)
    errors = check_rules(root) + check_registry(root, registry)
    notes = []
    if results is not None:
        e, notes = check_results(root, registry, read_results(results))
        errors += e
    for n in notes:
        print(f'  {n}')
    for e in errors:
        print(f'record_pairs: {e}', file=sys.stderr)
    if errors:
        return 1
    print(f'record_pairs: ok ({len(registry.get("producers", []))} producers, '
          f'{len(registry.get("consumers", []))} consumers, {len(registry.get("pairs", []))} pairs'
          f'{", results " + str(results) if results else ""})')
    return 0


def self_test() -> int:
    """Each planted defect must fail the check; the clean tree must pass."""
    failures = []

    def tree(base: Path, producer_extra: str = '', rule_text: str = 'int f(void)') -> Path:
        (base / 'driver/x').mkdir(parents=True, exist_ok=True)
        (base / 'driver/contract/test').mkdir(parents=True, exist_ok=True)
        (base / 'driver/x/prod.h').write_text(
            '#define MY_ALIAS 0x41324342u\n'
            'static int write_one(struct blob *b)\n{\n    b->magic = BC250_UMD_ALLOC_MAGIC;\n    return 0;\n}\n'
            'struct S {\n    int prepare(int n) noexcept {\n        if (n) { x.magic=MY_ALIAS; }\n        return 0;\n    }\n};\n'
            'int read_one(const struct blob *b) { return b->magic == BC250_UMD_ALLOC_MAGIC; }\n' + producer_extra,
            encoding='utf-8')
        (base / 'driver/x/cons.h').write_text('int f(void) { return 0; }\n', encoding='utf-8')
        (base / TEST).write_text(f'// RULE driver/x/cons.h "{rule_text}"\n', encoding='utf-8')
        registry = {
            'producers': [{'id': 'P1', 'sites': [{'file': 'driver/x/prod.h', 'function': 'write_one'},
                                                 {'file': 'driver/x/prod.h', 'function': 'prepare'}]}],
            'consumers': [{'id': 'C1', 'file': 'driver/x/cons.h', 'function': 'f'}],
            'pairs': [{'producer': 'P1', 'consumer': 'C1', 'rows': ['P1 .*']}],
            'expected_red': [{'row': 'P1 red', 'owner': 'branch-x', 'reason': 'planted',
                              'until': {'file': 'driver/x/cons.h', 'text': 'FIXED'}}]}
        (base / REGISTRY).write_text(json.dumps(registry), encoding='utf-8')
        return base

    def expect(label: str, ok: bool) -> None:
        print(f'  {"ok  " if ok else "FAIL"} {label}')
        if not ok:
            failures.append(label)

    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        clean = tree(base / 'clean')
        reg = load_registry(clean)
        expect('clean tree passes', not check_rules(clean) and not check_registry(clean, reg))
        sites = sorted((f, fn) for f, fn, _, _ in write_sites(clean))
        expect('sites found by macro and by alias, not the comparison',
               sites == [('driver/x/prod.h', 'prepare'), ('driver/x/prod.h', 'write_one')])
        planted = tree(base / 'producer', 'void rogue(struct blob *b)\n{\n    b->magic = 0x41324342u;\n}\n')
        expect('a planted unlisted producer fails', bool(check_registry(planted, load_registry(planted))))
        dead = tree(base / 'rule', rule_text='int g(void)')
        expect('a planted dead RULE text fails', bool(check_rules(dead)))
        e, _ = check_results(clean, reg, {'P1 red': 'FAIL', 'P1 green': 'ok'})
        expect('an expected-red row that fails before its fix passes', not e)
        e, _ = check_results(clean, reg, {'P1 red': 'ok', 'P1 green': 'ok'})
        expect('an expected-red row that passes before its fix fails', bool(e))
        e, _ = check_results(clean, reg, {'P1 red': 'FAIL', 'P1 green': 'FAIL'})
        expect('an unexpected FAIL row fails', bool(e))
        e, _ = check_results(clean, reg, {'P1 red': 'FAIL', 'X other': 'ok'})
        expect('a row of no pair fails', bool(e))
        (clean / 'driver/x/cons.h').write_text('int f(void) { return 0; } /* FIXED */\n', encoding='utf-8')
        e, _ = check_results(clean, reg, {'P1 red': 'ok'})
        expect('an expected-red row that passes after its fix passes', not e)
        e, _ = check_results(clean, reg, {'P1 red': 'FAIL'})
        expect('an expected-red row that fails after its fix fails', bool(e))
    print(f'record_pairs self-test: {"FAIL" if failures else "ok"}')
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--results', type=Path)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--sites', action='store_true', help='print every record magic write site and exit')
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.sites:
        for site in write_sites(args.root):
            print('\t'.join(map(str, site)))
        return 0
    return run(args.root, args.results)


if __name__ == '__main__':
    sys.exit(main())
