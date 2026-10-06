"""Build the sweep list and the inventory of sparse cases.

    python make_caselist.py --src SRC --binary-caselist BIN_SPARSE_TXT --out lists

SRC is the VK-GL-CTS checkout (external/vulkancts/mustpass/main/vk-default*). BIN_SPARSE_TXT is the output of
    deqp-vk --deqp-runmode=txt-caselist --deqp-case=dEQP-VK.sparse_resources.*
(lines "TEST: <name>"). Writes:
    lists/sparse-resources.txt          the sweep list: vk-default/sparse-resources.txt in mustpass order
    lists/binary-sparse-resources.txt   what the built binary registers under dEQP-VK.sparse_resources
    lists/sparse-extended.txt           vk-default cases outside sparse_resources that run on sparse resources
                                        (by name, rule in inventory.json), optional second sweep
    lists/inventory.json                counts, set differences, per-group tallies
"""

import argparse
import glob
import hashlib
import json
import os
from collections import Counter


def read(path):
    with open(path, encoding="utf-8") as f:
        return [l.strip() for l in f if l.strip()]


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest().upper()


def write(path, lines):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--binary-caselist", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    mp = os.path.join(a.src, "external", "vulkancts", "mustpass", "main")
    sparse_file = os.path.join(mp, "vk-default", "sparse-resources.txt")
    must = read(sparse_file)
    binary = [l[len("TEST: "):].strip() for l in read(a.binary_caselist) if l.startswith("TEST: ")]

    # All vk-default lists, as referenced by vk-default.txt (one file name per line).
    top = read(os.path.join(mp, "vk-default.txt"))
    all_cases, files = [], {}
    for name in top:
        p = os.path.join(mp, name.replace("/", os.sep))
        lst = read(p)
        files[name] = len(lst)
        all_cases += lst
    in_sparse = [c for c in all_cases if c.startswith("dEQP-VK.sparse_resources.")]
    # Outside sparse_resources, by name: any component naming sparse marks a variant on sparse-bound or
    # sparse-resident resources (acceleration structures on sparse buffers, descriptor buffers, image atomics,
    # multisample "_sparse" image backing, texture gather/functions "sparse_" residency variants, copy/blit,
    # sparse queue priority, sparse format queries), except name uses that are not sparse binding:
    # render-pass suballocation "sparserendertarget" / "attachment_sparse_filling", the ray-tracing
    # control-flow "loop_double_call_sparse", data-graph tensor "sparseConstants" and SPIR-V "sparse_ids".
    not_binding = ("sparserendertarget", "attachment_sparse_filling", "loop_double_call_sparse",
                   "sparseConstants", "sparse_ids")

    def is_not_binding(c):
        return any(m in p for p in c.split(".")[1:] for m in not_binding)

    named = [c for c in all_cases if not c.startswith("dEQP-VK.sparse_resources.")
             and any("sparse" in p for p in c.split(".")[1:])]
    other = [c for c in named if not is_not_binding(c)]
    excluded = [c for c in named if is_not_binding(c)]
    leaf_only = [c for c in other if not any("sparse" in p for p in c.split(".")[1:-1])]

    write(os.path.join(a.out, "sparse-resources.txt"), must)
    write(os.path.join(a.out, "binary-sparse-resources.txt"), binary)
    write(os.path.join(a.out, "sparse-extended.txt"), other)

    def tally(lst, n):
        return dict(Counter(".".join(c.split(".")[:n]) for c in lst).most_common())

    sm, sb = set(must), set(binary)
    inv = {
        "mustpass_sparse_resources_file": "external/vulkancts/mustpass/main/vk-default/sparse-resources.txt",
        "mustpass_sparse_resources_sha256": sha(sparse_file),
        "mustpass_sparse_resources_count": len(must),
        "mustpass_sparse_resources_unique": len(sm),
        "vk_default_total_cases": len(all_cases),
        "vk_default_files": len(files),
        "sparse_resources_cases_in_other_vk_default_files": len(in_sparse) - len(must),
        "binary_sparse_resources_count": len(binary),
        "binary_only_not_in_mustpass": sorted(sb - sm),
        "mustpass_only_not_in_binary": sorted(sm - sb),
        "groups_mustpass": tally(must, 3),
        "groups_binary_only": tally(sorted(sb - sm), 4),
        "extended_rule": "vk-default cases outside sparse_resources with a name component containing 'sparse', "
                         "minus components " + ", ".join(not_binding),
        "extended_count": len(other),
        "extended_groups": tally(other, 3),
        "extended_sparse_only_in_leaf_count": len(leaf_only),
        "excluded_not_sparse_binding_count": len(excluded),
        "excluded_groups": tally(excluded, 4),
    }
    with open(os.path.join(a.out, "inventory.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(inv, f, indent=1)
        f.write("\n")
    print(f"mustpass sparse_resources {len(must)} (unique {len(sm)}), binary {len(binary)}, "
          f"binary-only {len(sb - sm)}, mustpass-only {len(sm - sb)}, extended (outside sparse_resources) "
          f"{len(other)}, excluded by name {len(excluded)}, vk-default total {len(all_cases)}")


if __name__ == "__main__":
    main()
