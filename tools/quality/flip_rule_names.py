"""Hold the published DirectFlip refusal table against the rule the driver ships (audit finding K4).

`docs/design/direct-flip-handshake.md` carries the list of clause names "in order, and each clause has
a name that a trace prints". That table is what an operator reads a `check_direct_flip` trace against,
so a clause that exists in `driver/umd/router/front-direct-flip.h` and not in the table makes the
trace unreadable, and a clause in the wrong row makes the reader expect the wrong order. The K4 fix
added `immediate-swizzle` to the rule and left the table at fourteen rows; this gate is what stops the
next one.

It fails when
  1. a `FlipRefusal` enumerator has no printed name in `FlipRefusalText`, or two share one,
  2. `kFlipRules` of `front-flip-log.h` is not the number of enumerators (`rules[0]` is the TRUE count,
     so the counter array holds one slot per enumerator, `none` included),
  3. the table names and the printed refusal names are not the same set,
  4. the table order is not the order in which `FlipReason` applies the clauses. That order is read
     from the `return FlipRefusal::<name>;` statements of the function, first occurrence wins, which is
     the order a question actually meets them. It is deliberately NOT the enumeration's order: an
     enumerator a trace has already printed may not move, so a new clause is appended to the
     enumeration and inserted into the rule wherever it belongs.

It reads two headers and one document: no GPU, no lab, no build.
"""
import argparse
from pathlib import Path
import re
import sys

ENUM = re.compile(r"enum\s+class\s+FlipRefusal\s*\{(.*?)\}\s*;", re.S)
ENUM_MEMBER = re.compile(r"^\s*([a-z_][a-z0-9_]*)\s*,", re.M)
TEXT_CASE = re.compile(r"case\s+FlipRefusal::([a-z_][a-z0-9_]*)\s*:\s*return\s+\"([^\"]*)\"\s*;")
RULES_COUNT = re.compile(r"kFlipRules\s*=\s*(\d+)")
REASON = re.compile(r"inline\s+FlipRefusal\s+FlipReason\s*\(.*?\n\}", re.S)
REASON_RETURN = re.compile(r"return\s+FlipRefusal::([a-z_][a-z0-9_]*)\s*;")
# The table: the rows between the header separator and the first line that is not a row.
TABLE_ROW = re.compile(r"^\|\s*`([^`]+)`\s*\|", re.M)
TABLE = re.compile(r"^\|\s*Refusal\s*\|[^\n]*\n\|\s*-+\s*\|[^\n]*\n((?:\|[^\n]*\n)+)", re.M)


def strip_comments(text):
    """C and C++ comments out, so a name inside a comment never counts as code."""
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)


def read_enum(header):
    body = ENUM.search(header)
    if not body:
        raise ValueError("front-direct-flip.h: enum class FlipRefusal not found")
    members = ENUM_MEMBER.findall(strip_comments(body.group(1)))
    if not members:
        raise ValueError("front-direct-flip.h: enum class FlipRefusal has no members")
    return members


def read_names(header):
    pairs = TEXT_CASE.findall(strip_comments(header))
    names = {}
    for member, text in pairs:
        if member in names:
            raise ValueError("FlipRefusalText: %s has two cases" % member)
        names[member] = text
    return names


def read_clause_order(header):
    body = REASON.search(strip_comments(header))
    if not body:
        raise ValueError("front-direct-flip.h: FlipReason not found")
    order = []
    for member in REASON_RETURN.findall(body.group(0)):
        if member != "none" and member not in order:
            order.append(member)
    return order


def read_table(document):
    body = TABLE.search(document)
    if not body:
        raise ValueError("direct-flip-handshake.md: the refusal table not found")
    return TABLE_ROW.findall(body.group(1))


def check(router, log_header, document):
    header = Path(router).read_text(encoding="utf-8")
    members = read_enum(header)
    names = read_names(header)
    order = read_clause_order(header)
    count = RULES_COUNT.search(strip_comments(Path(log_header).read_text(encoding="utf-8")))
    table = read_table(Path(document).read_text(encoding="utf-8"))
    failures = []

    # 1. Every enumerator is printable, and no two print the same word.
    missing = [m for m in members if m not in names]
    if missing:
        failures.append("FlipRefusalText prints no name for: %s" % ", ".join(missing))
    printed = [names[m] for m in members if m in names]
    if len(set(printed)) != len(printed):
        failures.append("FlipRefusalText prints one name for two enumerators: %s" % ", ".join(sorted(printed)))

    # 2. The log's counter array holds one slot per enumerator.
    if not count:
        failures.append("front-flip-log.h: kFlipRules not found")
    elif int(count.group(1)) != len(members):
        failures.append("kFlipRules is %s and FlipRefusal has %d enumerators" % (count.group(1), len(members)))

    # 3. Every clause the rule can refuse with stands in the table, and nothing else does.
    want = [names[m] for m in order if m in names]
    if set(want) != set(table):
        absent = sorted(set(want) - set(table))
        extra = sorted(set(table) - set(want))
        if absent:
            failures.append("the table of %s does not list: %s" % (Path(document).name, ", ".join(absent)))
        if extra:
            failures.append("the table of %s lists what the rule cannot answer: %s"
                            % (Path(document).name, ", ".join(extra)))

    # 4. And in the order a question meets them.
    elif want != table:
        failures.append("the table of %s is out of order: it says %s, FlipReason applies %s"
                        % (Path(document).name, " ".join(table), " ".join(want)))

    # Every enumerator but `none` is a clause of the rule: one the rule can never answer is dead.
    unreachable = [m for m in members if m != "none" and m not in order]
    if unreachable:
        failures.append("FlipReason never answers: %s" % ", ".join(unreachable))
    return failures, members, order, table


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--router", required=True, help="driver/umd/router/front-direct-flip.h")
    parser.add_argument("--log", required=True, help="driver/umd/router/front-flip-log.h")
    parser.add_argument("--document", required=True, help="docs/design/direct-flip-handshake.md")
    args = parser.parse_args(argv)
    try:
        failures, members, order, table = check(args.router, args.log, args.document)
    except ValueError as error:
        print("FAIL %s" % error)
        return 1
    for failure in failures:
        print("FAIL %s" % failure)
    if failures:
        return 1
    print("PASS %d flip rules, %d clauses, %d table rows, in one order"
          % (len(members), len(order), len(table)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
