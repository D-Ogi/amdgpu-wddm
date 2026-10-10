"""Read AMD's machine-readable GPU ISA (the XML files of gpuopen.com/machine-readable-isa).

The XML files are not in this repository. Get the package from
https://gpuopen.com/download/machine-readable-isa/latest/ (MIT licence, stated in each file) and pass its directory.

  python tools/isa/amd_isa_xml.py slots   <xml dir> <ENC_NAME> <opcode...>   which name each file gives each slot
  python tools/isa/amd_isa_xml.py describe <xml file> <NAME...>             description and operands, as Markdown

Opcodes take a 0x prefix for hex. Every run first checks a known slot (V_MOV_B32 is ENC_VOP1 1 in every RDNA file),
so a changed schema fails loudly instead of returning empty answers.
"""
import os
import sys
import xml.etree.ElementTree as ET


def load(path):
    return ET.parse(path).getroot()


def slot_names(root, enc, opcodes):
    """{opcode: [instruction names]} for the given encoding."""
    found = {op: [] for op in opcodes}
    for ins in root.iter("Instruction"):
        name = ins.findtext("InstructionName")
        for e in ins.iter("InstructionEncoding"):
            if e.findtext("EncodingName") != enc:
                continue
            o = e.find("Opcode")
            if o is None or o.text is None:
                continue
            value = int(o.text, int(o.get("Radix", "10")))
            if value in found:
                found[value].append(name)
    return found


def control_ok(root):
    return "V_MOV_B32" in slot_names(root, "ENC_VOP1", [1])[1]


def describe(root, name):
    for ins in root.iter("Instruction"):
        if ins.findtext("InstructionName") != name:
            continue
        encs = []
        for e in ins.iter("InstructionEncoding"):
            o = e.find("Opcode")
            ops = []
            for p in e.iter("Operand"):
                ops.append("%s %s %s bits%s" % (
                    p.findtext("FieldName"), "out" if p.get("Output") == "true" else "in",
                    p.findtext("OperandSize"), "" if p.get("IsImplicit") == "false" else " implicit"))
            encs.append((e.findtext("EncodingName"), int(o.text, int(o.get("Radix", "10"))), ops))
        return " ".join((ins.findtext("Description") or "").split()), encs
    return None


def main(argv):
    if len(argv) < 4 or argv[1] not in ("slots", "describe"):
        sys.stderr.write(__doc__)
        return 2
    if argv[1] == "slots":
        xml_dir, enc, opcodes = argv[2], argv[3], [int(x, 0) for x in argv[4:]]
        bad = 0
        for f in sorted(os.listdir(xml_dir)):
            if not f.endswith(".xml"):
                continue
            root = load(os.path.join(xml_dir, f))
            ok = control_ok(root)
            bad += not ok
            hits = {hex(k): v for k, v in slot_names(root, enc, opcodes).items() if v}
            print("%-26s %-14s %s" % (f, "control ok" if ok else "CONTROL FAILED", hits))
        return 1 if bad else 0
    root = load(argv[2])
    if not control_ok(root):
        sys.stderr.write("control failed: V_MOV_B32 is not ENC_VOP1 1 in %s\n" % argv[2])
        return 1
    missing = 0
    for name in argv[3:]:
        d = describe(root, name)
        if d is None:
            print("- `%s`: not in this file" % name)
            missing += 1
            continue
        desc, encs = d
        print("### `%s`\n" % name.lower())
        print(desc + "\n")
        for enc, op, ops in encs:
            print("- %s, opcode %d (0x%02x): %s" % (enc, op, op, ", ".join(ops)))
        print()
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
