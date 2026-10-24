"""BD-235: extract display.c's own WddmDiagnosticAllowed for escape_admission_test.c.

    python generate_escape_admission_test.py <out.inc> <kmd source dir> <mutation>

Mutation admit-fence (the negative control) adds RUN_FENCE, a raw SDMA0 writer, to the commands admitted under
FullWddm in the extracted copy.
"""
from pathlib import Path
import sys

out, source, mutation = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
text = (source / 'display.c').read_text(encoding='utf-8')
start = text.index('static BOOLEAN WddmDiagnosticAllowed(')
body = text[start:text.index('\n}\n', start) + 3]
if mutation == 'admit-fence':
    anchor = '    case BC250_ESCAPE_GET_INFO:\n'
    if body.count(anchor) != 1:
        sys.exit('mutation anchor not found once: ' + anchor.strip())
    body = body.replace(anchor, '    case BC250_ESCAPE_RUN_FENCE:   /* negative control */\n' + anchor)
elif mutation != 'none':
    sys.exit('unknown mutation ' + mutation)
out.write_text(body, encoding='utf-8')
print('extracted WddmDiagnosticAllowed (%d lines), mutation %s' % (body.count('\n'), mutation))
