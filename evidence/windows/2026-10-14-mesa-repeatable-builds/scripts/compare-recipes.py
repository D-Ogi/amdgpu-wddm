"""Compare two build records of build-mesa.ps1 (recipe.json) and print every difference.

    python compare-recipes.py <old recipe.json> <new recipe.json>

build-mesa.ps1 records what it built with: the source commit and worktree state, the meson options, the tool
versions, the environment it set up, the hashes of the recipe scripts themselves, and the hash of every artifact.
Two records of one commit therefore say why two builds of that commit differ, which is what a reproduction test
needs: a size difference with no source difference is a toolchain or recipe change, and this names it.
"""

import io
import json
import sys


def flatten(value, prefix=""):
    if isinstance(value, dict):
        for key in value:
            yield from flatten(value[key], prefix + ("." if prefix else "") + str(key))
    elif isinstance(value, list):
        for index, item in enumerate(value):
            yield from flatten(item, "{}[{}]".format(prefix, index))
    else:
        yield prefix, value


def main(old_path, new_path):
    old = dict(flatten(json.load(io.open(old_path, encoding="utf-8"))))
    new = dict(flatten(json.load(io.open(new_path, encoding="utf-8"))))
    same, changed, only_old, only_new = 0, [], [], []
    for key in sorted(set(old) | set(new)):
        if key not in new:
            only_old.append(key)
        elif key not in old:
            only_new.append(key)
        elif old[key] != new[key]:
            changed.append(key)
        else:
            same += 1
    print("{} field(s) equal, {} different, {} only in the old record, {} only in the new one".format(
        same, len(changed), len(only_old), len(only_new)))
    for key in changed:
        print("  {}\n      old: {}\n      new: {}".format(key, str(old[key])[:160], str(new[key])[:160]))
    for key in only_old:
        print("  only old: {} = {}".format(key, str(old[key])[:120]))
    for key in only_new:
        print("  only new: {} = {}".format(key, str(new[key])[:120]))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        raise SystemExit(2)
    raise SystemExit(main(sys.argv[1], sys.argv[2]))
