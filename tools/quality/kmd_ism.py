"""Implicit state machine (ISM) candidates in the kernel-mode driver, from the clang AST.

    kmd_ism.py --database <out>/kmd/compile_commands.json --out <dir>       the gate
    kmd_ism.py ... --accept                                                write today's candidates into the baseline
    kmd_ism.py --files a.c b.c --out <dir> --no-baseline                   any C files (the tests), no database

An implicit state machine is a state that no one type names: two flags that together mean a phase, a flag that
only a restart clears, an integer that holds phase numbers, a field that is written and never read. The detector
reads every translation unit of driver/kmd and driver/shim with clang (-ast-dump=json, the commands of the build,
kmd_clang.py) and lists, for each field of a struct that the driver declares, every write (function, value) and
every read. From those it reports five kinds of candidate:

  cluster    two state fields that the same functions write (outside start, stop and init functions), or that one
             function writes and one condition reads together. A pair, so that a new member of a cluster is a new
             line. Fields of two structs pair only when one function writes one of them and calls the writer of
             the other.
  dead       a field that the driver writes and never reads (and never passes by address or copies out).
  sticky     a flag that the driver sets to a non-zero constant, and that only start or init functions (or nothing)
             set back to zero.
  int-state  an integer field that the driver writes only with constants, three or more different ones: an enum
             without a name.
  switch     a switch over an enum type that has no case for one or more of its values.

Each candidate has a key without line numbers. The baseline (kmd_ism_baseline.txt) lists the keys of the day the
gate started; a key that is not in it fails the gate. A candidate is not a defect: a person reads it and either
changes the code or accepts the key (--accept, and the review of that diff). docs/testing/kmd-static-checks.md
tells how to read the report.
"""
import argparse
import bisect
from concurrent.futures import ProcessPoolExecutor
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kmd_clang  # noqa: E402

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
BASELINE = HERE / 'kmd_ism_baseline.txt'
CONTROLS = HERE / 'kmd_ism_controls.txt'
MATCH = r'/driver/(kmd|shim)/[^/]+\.c$'
# The files whose structs and functions are the driver's own. Imported AMD code (amdgpu-import, dml201,
# third_party) has its own state rules and is not judged here.
OWN = re.compile(r'[\\/]driver[\\/](kmd|shim)[\\/](?!dml201[\\/])', re.I)
# Functions whose writes are a life-cycle edge, not a transition: a field that only these write together is not a
# cluster, and a flag that only these clear is sticky.
LIFECYCLE = re.compile(r'(start|stop|init|fini|create|destroy|adddevice|remove|setup|teardown|cleanup|alloc|free|'
                       r'release|unload|probe|attach|detach|open|close|reset)', re.I)
# Structs that a reader outside this driver reads (the escape interface of bc250kmd_escape.h).
# Journals and progress records are read from shared memory and crash dumps, outside the driver, by design.
INTERFACE = re.compile(r'escape|journal|progress', re.I)
CHAIN_FAN_IN = 3
START = re.compile(r'(start|init|create|adddevice|setup|alloc|probe|attach|open)', re.I)

_TOP = re.compile(r'\n    \{\n')
_RECORD_IN_TYPE = re.compile(r'\b(?:struct|union)\s+([A-Za-z_]\w*)')
_FILE = re.compile(r'"includedFrom": \{|"file": "((?:[^"\\]|\\.)*)"')

INTEGER_TYPES = {'char', 'signed char', 'unsigned char', 'short', 'unsigned short', 'int', 'unsigned int', 'long',
                 'unsigned long', 'long long', 'unsigned long long', '__int64', 'unsigned __int64'}
FLAG_NAMES = {'BOOLEAN', 'BOOL', 'bool', '_Bool'}
ZERO = {'0', 'false', '0u', '0U'}


# ---------------------------------------------------------------- the AST pass of one translation unit

def type_class(field):
    t = field.get('type', {})
    qual = t.get('qualType', '')
    sugar = t.get('desugaredQualType', qual)
    bare = re.sub(r'\b(volatile|const)\b', '', qual).strip()
    dbare = re.sub(r'\b(volatile|const)\b', '', sugar).strip()
    if field.get('isBitfield') and _bit_width(field) == 1:
        return 'flag'
    if bare in FLAG_NAMES or dbare in ('_Bool', 'bool'):
        return 'flag'
    if dbare.startswith('enum ') or bare.startswith('enum '):
        return 'enum'
    if '[' in dbare:
        return 'array'
    if '*' in dbare or '(' in dbare:
        return 'pointer'
    if dbare.startswith(('struct ', 'union ')):
        return 'record'
    if dbare in INTEGER_TYPES:
        return 'int'
    return 'other'


def _bit_width(field):
    for c in field.get('inner', []):
        if c.get('kind') == 'ConstantExpr' and 'value' in c:
            return int(c['value'])
        for d in c.get('inner', []):
            if d.get('kind') == 'IntegerLiteral':
                return int(d['value'])
    return 0


def record_type_name(type_dict):
    """The struct tag or typedef name of a type string, or None when the type is not a struct."""
    if not type_dict:
        return None
    for key in ('desugaredQualType', 'qualType'):
        s = type_dict.get(key)
        if not s:
            continue
        s = re.sub(r'\b(volatile|const)\b', '', s).strip()
        m = re.match(r'^(?:struct|union)\s+([A-Za-z_]\w*)\s*\*?\s*$', s)
        if m:
            return m.group(1)
    # A pointer to a typedef name is not desugared in the dump (BC250_WDDM *): the name is a candidate, which the
    # caller looks up among the typedefs of structs.
    s = re.sub(r'\b(volatile|const)\b', '', type_dict.get('qualType', '')).strip()
    m = re.match(r'^([A-Za-z_]\w*)\s*\*?\s*$', s)
    if m and m.group(1) not in INTEGER_TYPES and m.group(1) not in FLAG_NAMES and m.group(1) != 'void':
        return m.group(1)
    return None


def target_name(type_dict):
    """The struct that a field has, points to or keeps an array of (a tag or a typedef name), or None."""
    s = _desugared(type_dict or {})
    m = _RECORD_IN_TYPE.search(s)
    if m:
        return m.group(1)
    m = re.match(r'^(?:(?:volatile|const)\s+)*([A-Za-z_]\w*)\s*(?:\*|\[)', (type_dict or {}).get('qualType', ''))
    return m.group(1) if m else None


def _desugared(type_dict):
    return type_dict.get('desugaredQualType', type_dict.get('qualType', ''))


def _is_void_pointer(type_dict):
    return re.sub(r'\b(const|volatile)\b', '', _desugared(type_dict)).replace(' ', '') in ('void*', 'PVOID')


def strip(node):
    while node and node.get('kind') in ('ImplicitCastExpr', 'ParenExpr', 'CStyleCastExpr', 'ConstantExpr',
                                        'FullExpr', 'ExprWithCleanups'):
        if node.get('kind') == 'ConstantExpr' and 'value' in node:
            return node
        inner = node.get('inner') or []
        node = inner[0] if inner else None
    return node


def const_value(node):
    """A constant as a string (a number or an enumerator name), or 'expr'."""
    if not node:
        return 'expr'
    k = node.get('kind')
    if k == 'ConstantExpr' and 'value' in node:
        return str(node['value'])
    if k in ('ImplicitCastExpr', 'ParenExpr', 'CStyleCastExpr', 'ConstantExpr'):
        inner = node.get('inner') or []
        return const_value(inner[0]) if inner else 'expr'
    if k in ('IntegerLiteral', 'CharacterLiteral'):
        return str(node.get('value'))
    if k == 'CXXBoolLiteralExpr':
        return '1' if node.get('value') else '0'
    if k == 'DeclRefExpr':
        ref = node.get('referencedDecl') or {}
        if ref.get('kind') == 'EnumConstantDecl':
            return ref.get('name', 'expr')
    if k == 'UnaryOperator' and node.get('opcode') == '-':
        v = const_value((node.get('inner') or [None])[0])
        return 'expr' if v == 'expr' else '-' + v
    return 'expr'


def callee_name(call):
    inner = call.get('inner') or []
    n = strip(inner[0]) if inner else None
    if n and n.get('kind') == 'DeclRefExpr':
        return (n.get('referencedDecl') or {}).get('name')
    return None


EXPR_SUFFIX = ('Expr', 'Operator', 'Literal')


class Extractor:
    def __init__(self, own):
        self.own = own
        self.file = None
        self.records = {}      # record AST id -> record dict
        self.fields = {}       # field AST id -> (record dict, field name)
        self.enums = {}        # enum tag -> {name: value}
        self.typedef_record = {}  # typedef name -> record dict
        self.functions = []
        self.events = []
        self.globals = set()
        self.links = set()
        self.link_records = {}
        self.lines = {}
        self.func = None

    # -- locations
    def loc(self, d):
        if not isinstance(d, dict):
            return None
        if 'spellingLoc' in d or 'expansionLoc' in d:
            self.loc(d.get('spellingLoc'))
            return self.loc(d.get('expansionLoc'))
        if 'offset' in d:
            if 'file' in d:
                self.file = d['file']
            return (self.file, d['offset'])
        return None

    def scan(self, value):
        """Every location in a value that is not a child node, in document order."""
        if isinstance(value, dict):
            if 'offset' in value or 'spellingLoc' in value:
                self.loc(value)
                return
            for k, v in value.items():
                if k != 'includedFrom':
                    self.scan(v)
        elif isinstance(value, list):
            for v in value:
                self.scan(v)

    def line_of(self, place):
        if not place or not place[0]:
            return 0
        path, offset = place
        if path not in self.lines:
            try:
                data = Path(path).read_bytes()
            except OSError:
                data = b''
            self.lines[path] = [i for i, b in enumerate(data) if b == 10]
        return bisect.bisect_left(self.lines[path], offset) + 1

    # -- the walk
    def visit(self, node, parents):
        place = None
        for k, v in node.items():
            if k == 'loc':
                p = self.loc(v)
                place = place or p
            elif k == 'range':
                if isinstance(v, dict):
                    p = self.loc(v.get('begin'))
                    self.loc(v.get('end'))
                    place = p or place
            elif k == 'inner':
                break
            elif k not in ('includedFrom',) and isinstance(v, (dict, list)):
                self.scan(v)
        node['_place'] = place
        kind = node.get('kind')
        pre = getattr(self, 'pre_' + kind, None) if kind else None
        if pre:
            pre(node, parents)
        inner = node.get('inner')
        if inner:
            parents.append(node)
            for child in inner:
                if isinstance(child, dict):
                    self.visit(child, parents)
            parents.pop()
        for k in ('array_filler',):
            if isinstance(node.get(k), list):
                parents.append(node)
                for child in node[k]:
                    if isinstance(child, dict):
                        self.visit(child, parents)
                parents.pop()
        post = getattr(self, 'post_' + kind, None) if kind else None
        if post:
            post(node, parents)

    # -- declarations
    def pre_RecordDecl(self, node, parents):
        place = node.get('_place')
        rec = {'name': node.get('name') or '', 'file': place[0] if place else '', 'line': self.line_of(place),
               'fields': {}, 'id': node['id'], 'complete': bool(node.get('completeDefinition')),
               'union': node.get('tagUsed') == 'union'}
        if not rec['complete']:
            return
        self.records[node['id']] = rec
        node['_rec'] = rec
        parent = parents[-1] if parents else None
        if not rec['name'] and parent is not None and parent.get('_rec'):
            # An unnamed struct inside a struct: named after the field that has it (set when that field comes).
            parent['_rec'].setdefault('_pending', []).append(rec)

    def pre_FieldDecl(self, node, parents):
        parent = parents[-1] if parents else None
        rec = parent.get('_rec') if parent else None
        if rec is None:
            return
        name = node.get('name') or ''
        cls = type_class(node)
        qual = node.get('type', {}).get('qualType', '')
        pend = rec.get('_pending') or []
        if pend and ('unnamed' in qual or not name):
            child = pend.pop(0)
            child['name'] = (rec['name'] or '?') + '.' + (name or '(anonymous)')
            child['_member'] = name
        rec['fields'][name] = {'class': cls, 'type': qual, 'record': record_type_name(node.get('type'))
                               if cls == 'record' else None, 'target': target_name(node.get('type')),
                               'line': self.line_of(node.get('_place'))}
        self.fields[node['id']] = (rec, name)

    def pre_TypedefDecl(self, node, parents):
        # typedef struct { ... } NAME; and typedef struct _TAG NAME;
        def find(n):
            if n.get('kind') == 'RecordType' and isinstance(n.get('decl'), dict):
                return n['decl'].get('id')
            for c in n.get('inner') or []:
                r = find(c)
                if r:
                    return r
            return None
        rid = find(node)
        rec = self.records.get(rid)
        if rec is not None:
            if not rec['name']:
                rec['name'] = node.get('name')
            self.typedef_record[node.get('name')] = rec

    def pre_EnumDecl(self, node, parents):
        values, nxt = {}, 0
        for c in node.get('inner') or []:
            if c.get('kind') != 'EnumConstantDecl':
                continue
            v = None
            for d in c.get('inner') or []:
                if d.get('kind') == 'ConstantExpr' and 'value' in d:
                    v = int(d['value'])
            nxt = nxt if v is None else v
            values[c.get('name')] = nxt
            nxt += 1
        name = node.get('name') or ''
        if name:
            self.enums[name] = values
        node['_enum'] = values
        self._last_enum = values

    def pre_VarDecl(self, node, parents):
        # A struct at file scope (static g_Hang, g_VidMm): a root of the driver's state, like the device extension.
        if parents or self.func is not None:
            return
        place = node.get('_place')
        t = node.get('type') or {}
        m = _RECORD_IN_TYPE.search(t.get('desugaredQualType', t.get('qualType', '')))
        if m and place and self.own.search(place[0] or '') and '*' not in t.get('qualType', ''):
            self.globals.add(m.group(1))

    def pre_FunctionDecl(self, node, parents):
        if not any(c.get('kind') == 'CompoundStmt' for c in node.get('inner') or []):
            return
        place = node.get('_place')
        if not place or not self.own.search(place[0] or ''):
            node['_skip'] = True
            return
        self.func = {'name': node.get('name'), 'file': place[0], 'line': self.line_of(place),
                     'static': node.get('storageClass') == 'static', 'calls': [], 'switches': [],
                     'roots': {}}
        self.functions.append(self.func)
        self._switches = []

    def post_FunctionDecl(self, node, parents):
        if self.func is not None and node.get('name') == self.func['name'] and not node.get('_skip'):
            self.func = None

    # -- statements and expressions inside a function body
    def pre_CallExpr(self, node, parents):
        if self.func is None:
            return
        name = callee_name(node)
        if name:
            self.func['calls'].append(name)

    def pre_SwitchStmt(self, node, parents):
        if self.func is None:
            return
        cond = (node.get('inner') or [None])[0]
        enum, field = None, None
        n = cond
        while n:
            t = (n.get('type') or {})
            s = t.get('desugaredQualType', t.get('qualType', ''))
            m = re.match(r'^(?:volatile\s+|const\s+)*enum\s+(\w+)', s or '')
            if m:
                enum = m.group(1)
                if n.get('kind') == 'MemberExpr':
                    field = self.field_of(n)
                break
            if n.get('kind') not in ('ImplicitCastExpr', 'ParenExpr', 'CStyleCastExpr', 'MemberExpr',
                                     'DeclRefExpr'):
                break
            n = (n.get('inner') or [None])[0]
        sw = {'enum': enum, 'field': field, 'cases': set(), 'default': False,
              'line': self.line_of(node.get('_place')), 'file': (node.get('_place') or [self.file])[0]}
        self._switches.append(sw)
        node['_sw'] = sw

    def post_SwitchStmt(self, node, parents):
        sw = node.get('_sw')
        if sw is None:
            return
        self._switches.remove(sw)
        if sw['enum']:
            sw['cases'] = sorted(sw['cases'])
            self.func['switches'].append(sw)

    def pre_CaseStmt(self, node, parents):
        if self.func is None or not self._switches:
            return
        sw = self._switches[-1]
        first = (node.get('inner') or [None])[0]
        v = first.get('value') if first and first.get('kind') == 'ConstantExpr' else None
        if v is None:
            v = const_value(first)
        sw['cases'].add(str(v))

    def pre_DefaultStmt(self, node, parents):
        if self.func is not None and self._switches:
            self._switches[-1]['default'] = True

    def find_record(self, name):
        if name in self.typedef_record:
            return self.typedef_record[name]
        for rec in self.records.values():
            if rec['name'] == name:
                return rec
        return None

    def pre_ImplicitCastExpr(self, node, parents):
        """A cast between a pointer to a driver struct and another pointer type.

        void* <-> struct: the driver keeps a sub-object behind a PVOID field (Device->Wddm). A cast of that field
        to the struct, or a store of the struct pointer into it, links the field to the struct (state reached
        from the device extension). In a call: memset is a reset of every field, memcpy a write (destination) or
        a read (source), another function a pass by address. A byte view (UCHAR*) is a read of every field."""
        if node.get('castKind') != 'BitCast':
            return
        src = (node.get('inner') or [None])[0] or {}
        src_type, dst_type = src.get('type') or {}, node.get('type') or {}
        src_name, dst_name = record_type_name(src_type), record_type_name(dst_type)
        if src_name == dst_name:
            return
        child, i = node, len(parents) - 1
        while i >= 0 and parents[i].get('kind') in ('ParenExpr', 'ImplicitCastExpr', 'CStyleCastExpr'):
            child, i = parents[i], i - 1
        parent = parents[i] if i >= 0 else None
        if dst_name and self.find_record(dst_name) is not None:
            member = strip(src)
            if member and member.get('kind') == 'MemberExpr':
                self.link(member, dst_name)
        if not src_name or not _desugared(src_type).rstrip().endswith('*') or self.func is None:
            return
        rec = self.find_record(src_name)
        if rec is None:
            return
        if parent is not None and parent.get('kind') == 'BinaryOperator' and parent.get('opcode') == '=':
            lhs = strip((parent.get('inner') or [None])[0])
            if lhs and lhs.get('kind') == 'MemberExpr':
                self.link(lhs, src_name)
        kind, value, callee = None, None, None
        if parent is not None and parent.get('kind') == 'CallExpr':
            args = (parent.get('inner') or [])[1:]
            index = next((n for n, a in enumerate(args) if a is child), -1)
            callee = callee_name(parent) or ''
            low = callee.lower()
            if low in ('memset', 'rtlsecurezeromemory'):
                kind, value = 'write', '0'
            elif low in ('memcpy', 'memmove') and index == 0:
                kind, value = 'write', 'expr'
            elif low in ('memcpy', 'memmove'):
                kind = 'read'
            elif index >= 0 and not _is_void_pointer(dst_type):
                kind = 'addr'
            # A struct pointer passed as a PVOID context (KeInitializeDpc, a work item, a callback) is a handle:
            # the code that reads the fields casts it back, and its own accesses are in this pass.
        elif not _is_void_pointer(dst_type):
            kind = 'escape'
        if kind is None:
            return
        self.events.append({'rec': rec, 'field': '*', 'kind': kind, 'value': value, 'callee': callee,
                            'func': self.func, 'line': self.line_of(node.get('_place')),
                            'file': (node.get('_place') or [self.file])[0], 'root': None, 'whole': None})

    def link(self, member, target):
        hit = self.field_of(member)
        if hit is not None:
            self.links.add((id(hit[0]), hit[1], target))
            self.link_records[id(hit[0])] = hit[0]

    pre_CStyleCastExpr = pre_ImplicitCastExpr

    def field_of(self, member):
        hit = self.fields.get(member.get('referencedMemberDecl'))
        if hit is None:
            return None
        return hit

    def pre_MemberExpr(self, node, parents):
        if self.func is None:
            return
        hit = self.field_of(node)
        if hit is None:
            return
        rec, fname = hit
        kind, value, callee = self.classify(node, parents)
        if kind == 'base':
            return
        root = self.condition_root(parents)
        t = node.get('type') or {}
        whole = record_type_name(t) if rec['fields'].get(fname, {}).get('class') == 'record' else None
        self.events.append({'rec': rec, 'field': fname, 'kind': kind, 'value': value, 'callee': callee,
                            'func': self.func, 'line': self.line_of(node.get('_place')),
                            'file': (node.get('_place') or [self.file])[0], 'root': root, 'whole': whole})

    def condition_root(self, parents):
        root = None
        child_ids = []
        for p in reversed(parents):
            k = p.get('kind', '')
            if k in ('IfStmt', 'WhileStmt', 'DoStmt', 'ForStmt', 'SwitchStmt'):
                cond = None
                inner = p.get('inner') or []
                if k == 'DoStmt' and len(inner) > 1:
                    cond = inner[1]
                elif k == 'ForStmt':
                    cond = next((c for c in inner[1:3] if c), None)
                elif inner:
                    cond = inner[0]
                if cond is not None and cond.get('id') in child_ids:
                    return p['id']
                return root
            if not k.endswith(EXPR_SUFFIX):
                return root
            if (k == 'BinaryOperator' and p.get('opcode') in ('&&', '||')) or k == 'ConditionalOperator':
                root = p['id']
            child_ids.append(p.get('id'))
        return root

    def classify(self, node, parents):
        """('read'|'write'|'rmw'|'addr'|'escape'|'base'|'none', value, callee) of a member access."""
        child = node
        i = len(parents) - 1
        while i >= 0:
            p = parents[i]
            k = p.get('kind')
            inner = p.get('inner') or []
            if k == 'ParenExpr':
                child = p
                i -= 1
                continue
            if k == 'ImplicitCastExpr':
                ck = p.get('castKind')
                if ck == 'LValueToRValue':
                    return ('read', None, None)
                if ck == 'ArrayToPointerDecay':
                    if i > 0 and parents[i - 1].get('kind') == 'ArraySubscriptExpr' and \
                            (parents[i - 1].get('inner') or [None])[0] is p:
                        child = parents[i - 1]
                        i -= 2
                        continue
                    return self.address(parents, i - 1, p)
                child = p
                i -= 1
                continue
            if k == 'ArraySubscriptExpr':
                if inner and inner[0] is child:
                    child = p
                    i -= 1
                    continue
                return ('read', None, None)
            if k == 'MemberExpr':
                return ('base', None, None)
            if k == 'BinaryOperator':
                if p.get('opcode') == '=' and inner and inner[0] is child:
                    return ('write', const_value(inner[1]) if len(inner) > 1 else 'expr', None)
                return ('read', None, None)
            if k == 'CompoundAssignOperator':
                return ('rmw', 'expr', None) if inner and inner[0] is child else ('read', None, None)
            if k == 'UnaryOperator':
                op = p.get('opcode')
                if op in ('++', '--'):
                    return ('rmw', 'expr', None)
                if op == '&':
                    return self.address(parents, i - 1, p)
                return ('read', None, None)
            if k in ('UnaryExprOrTypeTraitExpr', 'OffsetOfExpr'):
                return ('none', None, None)
            return ('read', None, None)
        return ('read', None, None)

    def address(self, parents, i, child):
        """The use of &field: an atomic read or write, a reset by memset, or a pass to a function."""
        while i >= 0 and parents[i].get('kind') in ('ParenExpr', 'ImplicitCastExpr', 'CStyleCastExpr'):
            child = parents[i]
            i -= 1
        if i < 0 or parents[i].get('kind') != 'CallExpr':
            return ('escape', None, None)
        call = parents[i]
        args = (call.get('inner') or [])[1:]
        try:
            index = next(n for n, a in enumerate(args) if a is child)
        except StopIteration:
            return ('escape', None, None)
        name = callee_name(call) or ''
        low = name.lower()
        if 'interlocked' in low:
            if 'compareexchange' in low:
                a = const_value(args[1]) if len(args) > 1 else 'expr'
                b = const_value(args[2]) if len(args) > 2 else 'expr'
                if a != 'expr' and a == b:
                    return ('read', None, name)
                return ('rmw', a, name)
            if re.search(r'exchange(pointer)?(nofence|acquire|release)?$|exchange8|exchange16|exchange64', low) \
                    and 'add' not in low:
                # if (InterlockedExchange(&x->Flag, 0)) reads the old value too.
                return ('rmw' if _result_used(parents, i) else 'write',
                        const_value(args[1]) if len(args) > 1 else 'expr', name)
            return ('rmw', 'expr', name)
        if low.startswith('read') or 'load' in low:
            return ('read', None, name)
        if low.startswith('write') or 'store' in low:
            return ('write', const_value(args[1]) if len(args) > 1 else 'expr', name)
        if low in ('memset', 'rtlsecurezeromemory', '__stosb'):
            if low == 'memset' and len(args) > 1 and const_value(args[1]) not in ZERO:
                return ('write', 'expr', name)
            return ('write', '0', name)
        if low in ('memcpy', 'memmove', 'rtlcopymemory', 'rtlmovememory'):
            return ('write', 'expr', name) if index == 0 else ('read', None, name)
        return ('addr', None, name)


def _result_used(parents, i):
    """Whether the value of the call parents[i] is used (not a statement of its own, not cast to void)."""
    child, j = parents[i], i - 1
    while j >= 0 and parents[j].get('kind') in ('ParenExpr', 'ImplicitCastExpr', 'CStyleCastExpr'):
        if parents[j].get('kind') == 'CStyleCastExpr' and _desugared(parents[j].get('type') or {}) == 'void':
            return False
        child, j = parents[j], j - 1
    if j < 0:
        return False
    kind = parents[j].get('kind')
    if kind in ('IfStmt', 'WhileStmt', 'SwitchStmt'):
        return (parents[j].get('inner') or [None])[0] is child
    return kind not in ('CompoundStmt', 'CaseStmt', 'DefaultStmt', 'LabelStmt', 'DoStmt', 'ForStmt')


def scan_translation_unit(text, own):
    """The facts of one -ast-dump=json text: the driver's records, functions and field accesses."""
    starts = [m.start() + 1 for m in _TOP.finditer(text)]
    end_all = text.rfind('\n  ]')
    files = []
    pending_include = False
    for m in _FILE.finditer(text):
        if m.group(1) is None:
            pending_include = True
            continue
        if pending_include:
            pending_include = False
            continue
        files.append((m.start(), json.loads('"' + m.group(1) + '"')))
    positions = [p for p, _ in files]
    ex = Extractor(own)
    sys.setrecursionlimit(20000)
    for n, s in enumerate(starts):
        e = starts[n + 1] if n + 1 < len(starts) else end_all
        a = bisect.bisect_left(positions, s)
        b = bisect.bisect_left(positions, e)
        current = files[a - 1][1] if a > 0 else None
        inside = [f for _, f in files[a:b]]
        if not (current and own.search(current)) and not any(own.search(f) for f in inside):
            continue
        chunk = text[s:e].rstrip().rstrip(',')
        ex.file = current
        ex.visit(json.loads(chunk), [])
    return ex


def facts_of(ex, repo):
    """Plain data out of an Extractor: names instead of AST ids, repository-relative paths."""
    def rel(p):
        return kmd_clang.repo_relative(p, repo) if p else ''

    def rname(rec):
        return rec['name'] or ('(unnamed)@%s:%d' % (rel(rec['file']), rec['line']))
    records = {}
    for rec in ex.records.values():
        if not ex.own.search(rec['file'] or ''):
            continue
        records[rname(rec)] = {'file': rel(rec['file']), 'line': rec['line'], 'union': rec['union'],
                               'fields': {k: dict(v) for k, v in rec['fields'].items()}}
    aliases = {k: rname(v) for k, v in ex.typedef_record.items() if ex.own.search(v['file'] or '')}
    functions = []
    for f in ex.functions:
        functions.append({'name': f['name'], 'file': rel(f['file']), 'line': f['line'], 'static': f['static'],
                          'calls': sorted(set(c for c in f['calls'] if c)),
                          'switches': [{'enum': s['enum'], 'field': (rname(s['field'][0]) + '.' + s['field'][1])
                                        if s['field'] else None, 'cases': s['cases'], 'default': s['default'],
                                        'file': rel(s['file']), 'line': s['line']} for s in f['switches']]})
    events = []
    for e in ex.events:
        if not ex.own.search(e['rec']['file'] or ''):
            continue
        events.append({'field': rname(e['rec']) + '.' + e['field'], 'kind': e['kind'], 'value': e['value'],
                       'callee': e['callee'], 'func': e['func']['name'], 'func_file': rel(e['func']['file']),
                       'static': e['func']['static'], 'file': rel(e['file']), 'line': e['line'],
                       'root': e['root'], 'whole': e['whole']})
    return {'records': records, 'aliases': aliases, 'enums': ex.enums, 'functions': functions, 'events': events,
            'globals': sorted(ex.globals),
            'links': sorted({(rname(ex.link_records[r]) + '.' + f, t) for r, f, t in ex.links
                             if ex.own.search(ex.link_records[r]['file'] or '')})}


def ast_command(clang_cl, argv):
    return [str(clang_cl), *argv[1:-1], '/Zs', '-Wno-everything', '-Xclang', '-ast-dump=json', argv[-1]]


def extract(job):
    """Worker: one translation unit, run clang, return its facts (or an error)."""
    argv, repo, timeout = job
    start = time.monotonic()
    cmd = ast_command(argv[0], argv)
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {'file': argv[-1], 'error': 'timeout after %d s' % timeout}
    if r.returncode:
        err = r.stderr.decode('utf-8', errors='replace')
        lines = [x for x in err.splitlines() if 'error' in x][:3]
        return {'file': argv[-1], 'error': ' | '.join(lines) or err[:400]}
    ex = scan_translation_unit(r.stdout.decode('utf-8', errors='replace'), OWN)
    facts = facts_of(ex, repo)
    facts['file'] = argv[-1]
    facts['seconds'] = round(time.monotonic() - start, 2)
    return facts


# ---------------------------------------------------------------- merging and the rules

def merge(units):
    records, aliases, enums, functions, events, roots, links = {}, {}, {}, {}, set(), set(), {}
    for u in units:
        roots.update(u.get('globals', ()))
        for field, target in u.get('links', ()):
            links.setdefault(field, set()).add(target)
        for name, rec in u['records'].items():
            records.setdefault(name, rec)
        aliases.update(u['aliases'])
        enums.update(u['enums'])
        for f in u['functions']:
            key = (f['name'] + '@' + f['file']) if f['static'] or f['file'].endswith(('.h', '.inc')) else f['name']
            f['key'] = key
            functions.setdefault(key, f)
        for e in u['events']:
            events.add(json.dumps(e, sort_keys=True))
    # A PVOID field that the driver casts to a struct: the struct is the field's target.
    for field, targets in links.items():
        rname, _, fname = field.rpartition('.')
        info = records.get(rname, {}).get('fields', {}).get(fname)
        if info is not None and not info.get('target') and len(targets) == 1:
            info['target'] = next(iter(targets))
    return records, aliases, enums, functions, [json.loads(e) for e in sorted(events)], roots


def function_key(name, file, static, functions):
    k = name + '@' + file
    if k in functions:
        return k
    return name


def reachable_records(records, aliases, roots):
    """The structs that hold the driver's lasting state: the roots and every struct that a field of one of them
    has, points to or keeps in an array, at any depth."""
    def real(name):
        return name if name in records else aliases.get(name)
    todo = [r for r in (real(n) for n in roots) if r]
    seen = set()
    while todo:
        name = todo.pop()
        if name in seen:
            continue
        seen.add(name)
        for info in records[name]['fields'].values():
            t = real(info.get('target') or '')
            if t and t not in seen:
                todo.append(t)
        todo += [o for o in records if o.startswith(name + '.') and o not in seen]
    return seen


def analyse(records, aliases, enums, functions, events, roots=None):
    def record_of(name):
        return records.get(name) or records.get(aliases.get(name, ''))

    def field_info(qual):
        rname, _, fname = qual.rpartition('.')
        rec = records.get(rname)
        return rec['fields'].get(fname) if rec else None

    def all_fields(rname, seen=None):
        """Every scalar field of a struct, through its struct members."""
        seen = seen or set()
        rec = record_of(rname)
        if rec is None or rname in seen:
            return []
        seen.add(rname)
        real = rname if rname in records else aliases.get(rname, rname)
        out = []
        for fname, info in rec['fields'].items():
            if info['class'] == 'record' and info.get('record'):
                out += all_fields(info['record'], seen)
            else:
                out.append(real + '.' + fname)
        for other, r in records.items():
            if other.startswith(real + '.') and other.count('.') == real.count('.') + 1 and other not in seen:
                out += all_fields(other, seen)
        return out

    enum_zero = {name for values in enums.values() for name, v in values.items() if v == 0}
    defined = {f['name'] for f in functions.values()}

    def fkey(e):
        return function_key(e['func'], e['func_file'], e['static'], functions)

    # Expand whole-struct events (memset of a sub-struct, a struct copy) into one event per field.
    per_field = {}
    for e in events:
        targets = [e['field']]
        info = field_info(e['field'])
        if e['field'].endswith('.*'):
            targets = all_fields(e['field'][:-2])
        elif info and info['class'] == 'record' and info.get('record'):
            targets = all_fields(info['record'])
        kind = e['kind']
        if kind == 'addr':
            if e['callee'] in defined and targets != [e['field']]:
                # A struct passed by address to a driver function: the function's own body has the accesses.
                continue
            kind = 'outparam' if e['callee'] in defined else 'escape'
        for t in targets:
            per_field.setdefault(t, []).append(dict(e, field=t, kind=kind, func_key=fkey(e)))

    if roots is not None:
        lasting = reachable_records(records, aliases, roots)
        per_field = {q: es for q, es in per_field.items() if q.rpartition('.')[0] in lasting}

    def cls(q):
        info = field_info(q)
        return info['class'] if info else 'other'

    def is_zero(v):
        return v in ZERO or v in enum_zero or v in ('0x0',)

    reads = {q for q, es in per_field.items() if any(x['kind'] in ('read', 'rmw', 'outparam', 'escape') for x in es)}
    # The members of a union share their bytes: a read of one member reads every member.
    for rname, rec in records.items():
        if rec.get('union'):
            members = set(all_fields(rname))
            if members & reads:
                reads |= members
    read_records = {q.rpartition('.')[0] for q in reads}
    candidates = []

    # dead
    for q, es in sorted(per_field.items()):
        rname = q.rpartition('.')[0]
        rec = records.get(rname) or {}
        # An interface struct (an escape packet, read by user mode) and a struct that the driver only fills
        # (a parameter block for firmware, an output record) have readers outside: only a struct that the driver
        # itself reads can show a field that nobody reads.
        if cls(q) in ('record', 'array') or INTERFACE.search(rname) or INTERFACE.search(rec.get('file', ''))                 or rname not in read_records:
            continue
        writes = [x for x in es if x['kind'] == 'write' and not x['callee'] in ('memset', 'memcpy', 'memmove',
                                                                                 'rtlsecurezeromemory')]
        if writes and q not in reads:
            candidates.append({'kind': 'dead', 'key': 'dead ' + q, 'fields': [q],
                               'why': 'written in %d place(s), read nowhere' % len(writes),
                               'sites': sorted({'%s:%d %s = %s' % (x['file'], x['line'], x['func'], x['value'])
                                                for x in writes})})

    # int-state
    for q, es in sorted(per_field.items()):
        if cls(q) != 'int':
            continue
        writes = [x for x in es if x['kind'] in ('write', 'rmw') and x['callee'] not in ('memset', 'memcpy',
                                                                                          'memmove')]
        values = {x['value'] for x in writes}
        if writes and len(values) >= 3 and 'expr' not in values and all(x['kind'] == 'write' or x['value'] != 'expr'
                                                                          for x in writes):
            candidates.append({'kind': 'int-state', 'key': 'int-state ' + q, 'fields': [q],
                               'why': 'an integer written only with constants: ' + ', '.join(sorted(values)),
                               'sites': sorted({'%s:%d %s = %s' % (x['file'], x['line'], x['func'], x['value'])
                                                for x in writes})})
    intstate = {c['fields'][0] for c in candidates if c['kind'] == 'int-state'}

    # sticky
    for q, es in sorted(per_field.items()):
        if cls(q) != 'flag':
            continue
        writes = [x for x in es if x['kind'] in ('write', 'rmw')]
        sets = [x for x in writes if x['value'] not in (None, 'expr') and not is_zero(x['value'])]
        clears = [x for x in writes if x not in sets]
        if sets and all(START.search(x['func']) for x in clears):
            candidates.append({'kind': 'sticky', 'key': 'sticky ' + q, 'fields': [q],
                               'why': 'set in %s; cleared %s' % (
                                   ', '.join(sorted({x['func'] for x in sets})),
                                   ('only in ' + ', '.join(sorted({x['func'] for x in clears}))) if clears
                                   else 'nowhere'),
                               'sites': sorted({'%s:%d %s = %s' % (x['file'], x['line'], x['func'], x['value'])
                                                for x in writes})})

    # clusters
    state = {q for q in per_field if cls(q) in ('flag', 'enum')} | intstate
    direct = {}      # function key -> state fields it writes directly (not a life-cycle function)
    for q in state:
        for x in per_field[q]:
            if x['kind'] in ('write', 'rmw', 'outparam') and x['callee'] not in ('memset', 'memcpy', 'memmove'):
                direct.setdefault(x['func_key'], set()).add(q)
    roots = {}
    for q in state:
        for x in per_field[q]:
            if x['kind'] == 'read' and x['root']:
                roots.setdefault((x['func_key'], x['root']), set()).add(q)

    def record_name(q):
        return q.rpartition('.')[0]
    pairs = {}
    callers = {}
    for k, f in functions.items():
        for c in f['calls']:
            callers.setdefault(c, set()).add(k)

    def add(a, b, how, where):
        if a == b:
            return
        a, b = sorted((a, b))
        pairs.setdefault((a, b), {'cowrite': set(), 'coread': set(), 'chain': set()})[how].add(where)
    for fk, qs in direct.items():
        if LIFECYCLE.search(fk.split('@')[0]):
            continue
        qs = sorted(qs)
        for i, a in enumerate(qs):
            for b in qs[i + 1:]:
                add(a, b, 'cowrite', fk.split('@')[0])
        # One level of calls: a field written here and a field that a called function writes. A callee with
        # more than CHAIN_FAN_IN callers is a service (a lock, a message, a log) and not one half of a state.
        f = functions.get(fk)
        for callee in (f['calls'] if f else []):
            ck = function_key(callee, f['file'], True, functions)
            if ck == fk or LIFECYCLE.search(callee) or len(callers.get(callee, ())) > CHAIN_FAN_IN:
                continue
            for a in qs:
                for b in direct.get(ck, ()):
                    add(a, b, 'chain', fk.split('@')[0] + '->' + callee)
    for (fk, _), qs in roots.items():
        qs = sorted(qs)
        for i, a in enumerate(qs):
            for b in qs[i + 1:]:
                add(a, b, 'coread', fk.split('@')[0])
    accepted = {}
    for (a, b), ev in pairs.items():
        same = record_name(a) == record_name(b)
        cw, cr, ch = len(ev['cowrite']), len(ev['coread']), len(ev['chain'])
        # Two enums that are written together are a configuration (a timing, a link mode). A pair needs a flag
        # or an integer that holds phases, which is the hidden half.
        if cls(a) == 'enum' and cls(b) == 'enum':
            continue
        if same and (cw >= 2 or (cw >= 1 and cr >= 1)):
            accepted[(a, b)] = ev
        elif not same and ch >= 1 and (ch >= 2 or cr >= 1):
            accepted[(a, b)] = ev
    # Connected groups, for the reader; the keys stay pairs.
    group = {}
    for a, b in accepted:
        ga, gb = group.get(a, {a}), group.get(b, {b})
        merged = ga | gb
        for q in merged:
            group[q] = merged
    seen = set()
    for members in sorted({frozenset(g) for g in group.values()}, key=lambda g: sorted(g)):
        if members in seen:
            continue
        seen.add(members)
        ps = sorted(p for p in accepted if p[0] in members)
        for a, b in ps:
            ev = accepted[(a, b)]
            candidates.append({'kind': 'cluster', 'key': 'cluster %s %s' % (a, b), 'fields': [a, b],
                               'group': sorted(members),
                               'why': 'written together in %s; read together in %s; written along a call in %s' % (
                                   ', '.join(sorted(ev['cowrite'])) or '-', ', '.join(sorted(ev['coread'])) or '-',
                                   ', '.join(sorted(ev['chain'])) or '-'),
                               'sites': []})

    # switch
    for f in functions.values():
        for s in f['switches']:
            values = enums.get(s['enum'])
            if not values:
                continue
            got = set()
            for c in s['cases']:
                if c in values:
                    got.add(values[c])
                else:
                    try:
                        got.add(int(c, 0))
                    except ValueError:
                        pass
            missing = sorted(n for n, v in values.items() if v not in got and not n.endswith(('_MAX', 'Max',
                                                                                              '_COUNT', 'Count')))
            if missing:
                candidates.append({'kind': 'switch', 'key': 'switch %s %s' % (f['name'], s['enum']),
                                   'fields': [s['field']] if s['field'] else [],
                                   'why': '%d of %d values without a case%s: %s' % (
                                       len(missing), len(values), ' (has default)' if s['default'] else '',
                                       ', '.join(missing[:8]) + (' ...' if len(missing) > 8 else '')),
                                   'sites': ['%s:%d %s' % (s['file'], s['line'], f['name'])]})
    # One key once (a switch twice in one function over one enum is one line).
    out, keys = [], set()
    for c in sorted(candidates, key=lambda c: (c['kind'], c['key'])):
        if c['key'] in keys:
            for o in out:
                if o['key'] == c['key']:
                    o['sites'] = sorted(set(o['sites']) | set(c['sites']))
            continue
        keys.add(c['key'])
        out.append(c)
    return out, per_field


# ---------------------------------------------------------------- baseline and gate

BASELINE_HEAD = [
    '# kmd-ism ratchet baseline: the implicit-state-machine candidates of the kernel-mode driver on the day the',
    '# gate started (2026-11-04). The gate: tools/quality/kmd_ism.py, run by tools/quality/quick.ps1 (kmd-ism).',
    '# How to read a candidate: docs/testing/kmd-static-checks.md.',
    '#',
    '# One candidate key a line, without line numbers. A key that is not here fails the gate. A key here that the',
    '# driver no longer has is a note (--prune removes it). --accept writes the keys of today; the reviewer of',
    '# that change reads every new line and says why it is not a defect, or the code changes instead.',
]


def read_controls(path):
    """The positive controls: candidate keys that a human confirmed as hidden state, with the reason."""
    rows = []
    if not Path(path).is_file():
        raise FileNotFoundError('positive controls not found: ' + str(path))
    for raw in Path(path).read_text(encoding='utf-8').splitlines():
        if raw.startswith('## fixed'):
            break
        if raw.strip() and not raw.startswith('#'):
            key, _, why = raw.partition('\t')
            rows.append((key.strip(), why.strip()))
    if not rows:
        raise ValueError('no positive controls in ' + str(path))
    return rows


def read_baseline(path):
    if not Path(path).is_file():
        return set()
    return {x.strip() for x in Path(path).read_text(encoding='utf-8').splitlines()
            if x.strip() and not x.startswith('#')}


def write_baseline(path, keys):
    Path(path).write_text('\n'.join(BASELINE_HEAD + sorted(keys)) + '\n', encoding='utf-8', newline='\n')


def text_report(candidates):
    out = []
    for kind in ('cluster', 'dead', 'sticky', 'int-state', 'switch'):
        rows = [c for c in candidates if c['kind'] == kind]
        out.append('== %s (%d)' % (kind, len(rows)))
        for c in rows:
            out.append(c['key'])
            out.append('    ' + c['why'])
            for s in c['sites'][:12]:
                out.append('    ' + s)
            if len(c['sites']) > 12:
                out.append('    ... %d more' % (len(c['sites']) - 12))
    return '\n'.join(out) + '\n'


def run(args):
    started = time.monotonic()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    llvm = Path(args.llvm) if args.llvm else kmd_clang.default_llvm_bin()
    clang_cl = llvm / 'clang-cl.exe'
    if not clang_cl.is_file():
        raise FileNotFoundError('kmd-ism needs ' + str(clang_cl) + ' (--llvm or BC250_LLVM_BIN)')
    if args.files:
        jobs = [([str(clang_cl), *kmd_clang.CLANG_EXTRA, *args.flag, str(Path(f).resolve())], REPO, args.timeout)
                for f in args.files]
    else:
        entries = kmd_clang.load_entries(args.database, args.match)
        jobs = [(kmd_clang.clang_cl_argv(e, clang_cl), REPO, args.timeout) for e in entries]
    global OWN
    if args.own:
        OWN = re.compile(args.own, re.I)
    if args.jobs <= 1:
        units = [extract(j) for j in jobs]
    else:
        with ProcessPoolExecutor(max_workers=args.jobs, initializer=_set_own, initargs=(OWN.pattern,)) as pool:
            units = list(pool.map(extract, jobs))
    broken = [u for u in units if 'error' in u]
    good = [u for u in units if 'error' not in u]
    records, aliases, enums, functions, events, globals_ = merge(good)
    candidates, per_field = analyse(records, aliases, enums, functions, events, set(args.root) | globals_)
    keys = {c['key'] for c in candidates}
    status = 'PASS'
    new, gone = set(), set()
    if not args.no_baseline:
        if args.accept:
            write_baseline(args.baseline, keys)
            print('%s: %d keys written' % (args.baseline, len(keys)))
        allowed = read_baseline(args.baseline)
        new, gone = keys - allowed, allowed - keys
        if args.prune and not new:
            write_baseline(args.baseline, allowed & keys)
            gone = set()
    lost = []
    if args.controls and not args.files:
        lost = [(k, why) for k, why in read_controls(args.controls) if k not in keys]
    if new or broken or lost:
        status = 'FAIL'
    counts = {}
    for c in candidates:
        counts[c['kind']] = counts.get(c['kind'], 0) + 1
    report = {'status': status, 'translation_units': len(units), 'records': len(records),
              'functions': len(functions), 'events': len(events), 'per_kind': counts,
              'broken': [{'file': kmd_clang.repo_relative(u['file'], REPO), 'error': u['error']} for u in broken],
              'new': sorted(new), 'not_found_any_more': sorted(gone), 'controls_lost': [k for k, _ in lost],
              'candidates': candidates,
              'seconds': round(time.monotonic() - started, 1)}
    (out / 'report.json').write_text(json.dumps(report, indent=1, default=list), encoding='utf-8')
    (out / 'report.txt').write_text(text_report(candidates), encoding='utf-8')
    if args.facts:
        (out / 'fields.json').write_text(json.dumps(per_field, indent=1, default=list), encoding='utf-8')
    for u in report['broken']:
        print('FAIL clang cannot read %s: %s' % (u['file'], u['error']))
    for k in sorted(new):
        c = next(c for c in candidates if c['key'] == k)
        print('FAIL new candidate: %s\n    %s' % (k, c['why']))
        for s in c['sites'][:6]:
            print('    ' + s)
    for k, why in lost:
        print('FAIL positive control lost: %s (%s). Either the detector went blind, or a fix removed it: then the '
              'fix moves its line to the fixed section of %s.' % (k, why, Path(args.controls).name))
    for k in sorted(gone):
        print('note: %s is not a candidate any more (--prune)' % k)
    print('kmd-ism %s: %d translation units, %d structs, %d functions, %d candidates (%s), %d new, %.1f s; %s' % (
        status, len(units), len(records), len(functions), len(candidates),
        ', '.join('%s %d' % kv for kv in sorted(counts.items())), len(new), report['seconds'], out / 'report.txt'))
    return 0 if status == 'PASS' else 1


def _set_own(pattern):
    global OWN
    OWN = re.compile(pattern, re.I)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument('--database', type=Path)
    src.add_argument('--files', nargs='+')
    p.add_argument('--flag', action='append', default=[], help='a clang-cl option for --files (repeat)')
    p.add_argument('--root', action='append', default=None,
                   help='a struct that holds lasting state (repeat; default _BC250_DEVICE); file-scope structs too')
    p.add_argument('--own', help='regular expression of the files whose structs count (default driver/kmd|shim)')
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--match', default=MATCH)
    p.add_argument('--baseline', default=BASELINE, type=Path)
    p.add_argument('--no-baseline', action='store_true', help='report only, no gate')
    p.add_argument('--controls', default=CONTROLS, type=Path,
                   help='positive controls (with --database): keys the report must have')
    p.add_argument('--llvm', type=Path)
    p.add_argument('--jobs', type=int, default=4)
    p.add_argument('--timeout', type=int, default=120)
    p.add_argument('--facts', action='store_true', help='also write fields.json, every access of every field')
    g = p.add_mutually_exclusive_group()
    g.add_argument('--prune', action='store_true')
    g.add_argument('--accept', action='store_true')
    args = p.parse_args(argv)
    args.root = args.root or ['_BC250_DEVICE']
    return run(args)


if __name__ == '__main__':
    sys.exit(main())
