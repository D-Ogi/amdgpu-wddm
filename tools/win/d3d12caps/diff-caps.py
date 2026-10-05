"""Print the leaves that differ between two d3d12caps JSON documents.

Usage: python diff-caps.py LEFT.json RIGHT.json [--ignore REGEX ...]

One line per differing leaf, sorted by path: `path: left -> right`. A leaf present on one side only shows
`<absent>` on the other. Flag lists written as `A|B|C` also show the names added and removed. The last line is
the count. The exit code is 0 always; a document that cannot be read is reported as such on the last line.
"""
import json
import re
import sys

_ABSENT = object()


def flatten(node, prefix='', out=None):
    if out is None:
        out = {}
    if isinstance(node, dict) and node:
        for k, v in node.items():
            flatten(v, f'{prefix}.{k}' if prefix else k, out)
    elif isinstance(node, list) and node:
        for i, v in enumerate(node):
            flatten(v, f'{prefix}[{i}]', out)
    else:
        out[prefix] = node
    return out


def show(value):
    if value is _ABSENT:
        return '<absent>'
    return json.dumps(value, sort_keys=True)


def is_flag_path(path):
    return path.endswith('Names')


def flag_delta(left, right):
    if not (isinstance(left, str) and isinstance(right, str)):
        return ''
    a = set(filter(None, left.split('|')))
    b = set(filter(None, right.split('|')))
    parts = [f'+{n}' for n in sorted(b - a)] + [f'-{n}' for n in sorted(a - b)]
    return f'  ({" ".join(parts)})' if parts else ''


def diff(left, right, ignore=()):
    lf, rf = flatten(left), flatten(right)
    lines = []
    for path in sorted(set(lf) | set(rf)):
        if any(p.search(path) for p in ignore):
            continue
        a, b = lf.get(path, _ABSENT), rf.get(path, _ABSENT)
        if a is not _ABSENT and b is not _ABSENT and a == b and type(a) is type(b):
            continue
        line = f'{path}: {show(a)} -> {show(b)}'
        if is_flag_path(path):
            line += flag_delta(a if a is not _ABSENT else '', b if b is not _ABSENT else '')
        lines.append(line)
    return lines


def main(argv):
    args, ignore = [], []
    it = iter(argv)
    for a in it:
        if a == '--ignore':
            ignore.append(re.compile(next(it, '')))
        else:
            args.append(a)
    if len(args) != 2:
        print('usage: diff-caps.py LEFT.json RIGHT.json [--ignore REGEX ...]')
        print('differences: not computed (usage)')
        return 0
    docs = []
    for path in args:
        try:
            with open(path, encoding='utf-8') as f:
                docs.append(json.load(f))
        except (OSError, ValueError) as e:
            print(f'differences: not computed ({path}: {e})')
            return 0
    lines = diff(docs[0], docs[1], ignore)
    for line in lines:
        print(line)
    print(f'differences: {len(lines)}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
