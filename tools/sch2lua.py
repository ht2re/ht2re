#!/usr/bin/env python3
"""sch2lua.py -- transpile the Hard Truck 2 .sch/.CNF data into Lua tables.

The original "scripts" are not code: they are s-expression data (see
docs/script_gameplay.md).  So this is a *transpiler of a data format*, not a
decompiler, and the acceptance test is a round trip:

    .sch --(this tool)--> Lua table --(tools/lua_sexp.lua)--> canonical .sch

`canonical(original)` and `canonical(lua)` must be byte identical, for all 46
scripts.  Nothing in the data may be dropped: every form head, every field
position, every empty list `()` and the odd-looking `1#e+006` survive verbatim.

Usage:
    tools/sch2lua.py --list
    tools/sch2lua.py --all --out tools/out/lua
    tools/sch2lua.py --one ENV/mod_aa.sch --print
    tools/sch2lua.py --roundtrip [--lua lua]
    tools/sch2lua.py --verify-sdte      # decrypt -> parse -> re-encrypt -> parse
    tools/sch2lua.py --check            # known data defects, with locations
    tools/sch2lua.py --encode all --out /tmp/rebuilt

Readers are tools/sexp.py (fixed: `\\` escapes, `()` is not an atom, numbers).
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from sexp import game_number, parse_bytes, parse_file, to_bytes, to_text, \
    walk  # noqa: E402
from decrypt_sch import decrypt, seed, KEY, MASK32  # noqa: E402

DEC = os.path.join(ROOT, 'decrypted')
ORIGINAL = os.path.join(ROOT, 'original')
DEFAULT_OUT = os.path.join(HERE, 'out', 'lua')

# Luau/CPP consumers treat an unquoted digit-initial scalar as a number.  The
# dumper in tools/lua_sexp.lua uses the same rule, so it must be changed in both
# places if ever revisited: see lua_sexp.M.is_string.
_SUBSETS = ('', 'ENV', 'MENV', 'MTR')


# ---------------------------------------------------------------------------
# document discovery
# ---------------------------------------------------------------------------
def script_list():
    """Every shipped script, as (relative path, source kind)."""
    out = []
    for sub in _SUBSETS:
        d = os.path.join(ORIGINAL, sub)
        if not os.path.isdir(d):
            continue
        for n in sorted(os.listdir(d)):
            if n.lower().endswith(('.sch', '.cnf')):
                rel = os.path.join(sub, n) if sub else n
                out.append(rel)
    return out


def lua_filename(rel):
    """ENV/mod_aa.sch -> env_mod_aa.lua (same naming as decrypted/*.txt)."""
    return rel.replace('/', '_').replace('\\', '_').lower() + '.lua'


def resolve(rel):
    """Map a user-supplied script name onto the shipped inventory.

    Accepts the exact relative path (`ENV/mod_aa.sch`) or a bare basename in any
    case (`mod_aa.sch`, `MS.SCH`).  Anything else is an error instead of a
    silently empty parse of a file that does not exist.
    """
    rels = script_list()
    if rel in rels:
        return rel
    base = os.path.basename(rel).lower()
    hits = [r for r in rels if os.path.basename(r).lower() == base]
    if len(hits) == 1:
        return hits[0]
    if len(hits) > 1:
        raise SystemExit('sch2lua: %r is ambiguous: %s' % (rel, ', '.join(hits)))
    raise SystemExit('sch2lua: no such script %r (try --list)' % rel)


def load_forms(rel):
    """Prefer the pre-decrypted copy; fall back to decrypting the original."""
    rel = resolve(rel)
    txt = os.path.join(DEC, rel.replace('/', '_') + '.txt')
    if os.path.isfile(txt):
        return parse_bytes(open(txt, 'rb').read())
    return parse_file(os.path.join(ORIGINAL, rel))


def canonical(forms):
    return to_bytes(forms)


# ---------------------------------------------------------------------------
# Lua emission
# ---------------------------------------------------------------------------
def _lua_dq(text):
    """A Lua double-quoted literal for `text` (UTF-8 source, one meaning only)."""
    out = []
    for ch in text:
        o = ord(ch)
        if ch == '"':
            out.append('\\"')
        elif ch == '\\':
            out.append('\\\\')
        elif ch == '\n':
            out.append('\\n')
        elif ch == '\r':
            out.append('\\r')
        elif o < 0x20:
            out.append('\\%d' % o)
        else:
            out.append(ch)
    return '"' + ''.join(out) + '"'


def _lua_value(text, quoted):
    """A field value.

    Bare atoms are plain Lua strings -- the readable, hand-editable form, and it
    is unambiguous because a bare atom can never be a number-like token (those
    are bare too, but the serialiser prints a number-like *string* as a number,
    which re-reads identically) nor contain whitespace or parens.  Quoted atoms
    are wrapped as `{ q = "..." }`: the wrapper is structural, so no in-string
    marker can ever collide with Lua's own escape sequences.
    """
    if quoted:
        return '{ q = %s }' % _lua_dq(text)
    return _lua_dq(text)


def emit_nodes(nodes, indent):
    """Emit nodes as Lua array entries (comma separated, trailing comma allowed)."""
    pad = '  ' * indent
    lines = []
    for nd in nodes:
        if nd.kind == 'atom':
            lines.append(pad + _lua_value(nd.text, nd.quoted) + ',')
        elif len(nd) == 0:
            lines.append(pad + '{},')
        elif len(nd) and nd[0].kind == 'atom' and not nd[0].quoted:
            # a nested form: keep its head as a named field for readability
            lines.extend(emit_form(nd, indent, True))
        else:
            # a list of entries: opens at @indent, children at @indent+1,
            # closes at @indent (same as a `{ head = ... }` form)
            lines.append(pad + '{')
            lines.extend(emit_nodes(nd, indent + 1))
            lines.append(pad + '},')
    return lines
    if not outer and lines:
        lines[-1] = lines[-1] + ','
    return lines


def emit_form(nd, indent, in_array=False):
    """A list whose head is an atom -> { head = 'name', 'field', ... }.

    Scalar-only forms stay on one line (most of the data); forms with nested
    forms are expanded so the Lua file is readable and hand-editable.
    """
    pad = '  ' * indent
    head = _lua_dq(nd[0].text)
    body = list(nd[1:])
    if all(k.kind == 'atom' for k in body):
        text = ('{ head = %s }' % head if not body else
                '{ head = %s, %s }' % (head, ', '.join(_lua_value(k.text, k.quoted) for k in body)))
        return [pad + text + (',' if in_array else '')]
    lines = [pad + '{', pad + '  head = %s,' % head]
    lines.extend(emit_nodes(body, indent + 1))
    lines.append(pad + '}' + (',' if in_array else ''))
    return lines


def emit_lua(rel, forms):
    name = rel.replace('/', '_')
    lines = [
        '-- %s -- generated from original/%s by tools/sch2lua.py; do not edit.' % (
            lua_filename(rel), rel),
        '-- %d top-level form(s).  Layout:' % len(forms),
        "--   form  = { head = 'town', 'TOWN_AA', <scalars...>,",
        "--             { head = 'node', 'node_store_1000', 'unload' } }",
        '--   scalar = Lua string; readable field names are in',
        '--            docs/script_gameplay.md \u00a75 (fields are positional).',
        '-- Regenerate:  tools/sch2lua.py --all',
        '-- Verify:      tools/sch2lua_check.sh',
        'return {',
    ]
    for nd in forms:
        lines.extend(emit_nodes([nd], 1))
    lines.append('}')
    return '\n'.join(lines) + '\n'


# ---------------------------------------------------------------------------
# Lua -> canonical s-expression
# ---------------------------------------------------------------------------
LUA_DRIVER = """\
-- Round-trip driver, written by tools/sch2lua.py.
--   lua this.lua <out_dir_or_-> <data.lua> [<data.lua> ...]
-- Each data file is loaded with dofile and its canonical s-expression is written
-- next to it as <name>.sexp.  One file per input, so the caller can compare
-- byte for byte instead of aligning a concatenated stream.
package.path = %(tools)s .. '/?.lua;' .. package.path
local S = require 'lua_sexp'
local out_dir = arg[1]
for i = 2, #arg do
  local path = arg[i]
  local forms = assert(dofile(path))
  local dump = S.to_text(forms)
  if out_dir == '-' then
    io.write(dump)
  else
    local name = path:match('[^/\\\\]+$') or path
    local f = assert(io.open(out_dir .. '/' .. name .. '.sexp', 'wb'))
    f:write(dump)
    f:close()
  end
end
"""


def lua_bin():
    for cand in (os.environ.get('LUA'), 'lua', 'lua5.4', 'lua5.3', 'luajit'):
        if not cand:
            continue
        try:
            subprocess.run([cand, '-v'], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, check=False)
            return cand
        except (OSError, subprocess.SubprocessError):
            continue
    return None


def lua_redump(paths, lua=None, out_dir='-'):
    """Run the generated files through tools/lua_sexp.lua.

    With `out_dir='-'` the canonical text of every file is returned (in order).
    With a directory, each document is written to `<out_dir>/<name>.sexp`.

    The driver goes into a temp file: `lua -e ... -- args` does not always
    populate `arg` (Lua 5.5), while `lua script.lua args...` always does.
    """
    exe = lua or lua_bin()
    if not exe:
        raise SystemExit('sch2lua: no Lua interpreter found (set $LUA)')
    driver = LUA_DRIVER % {'tools': _lua_literal(HERE)}
    import tempfile
    with tempfile.NamedTemporaryFile('w', suffix='.lua', delete=False,
                                     encoding='utf-8') as f:
        f.write(driver)
        drive_path = f.name
    try:
        p = subprocess.run([exe, drive_path, out_dir] + list(paths),
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    finally:
        os.unlink(drive_path)
    if p.returncode != 0:
        raise SystemExit('sch2lua: lua failed:\n' + p.stderr.decode('utf-8', 'replace'))
    return p.stdout


def _lua_literal(path):
    return "'" + path.replace('\\', '\\\\').replace("'", "\\'") + "'"


# ---------------------------------------------------------------------------
# re-encryption (Lua/.sch -> SDTE)
# ---------------------------------------------------------------------------
def encrypt(plain: bytes, key=KEY) -> bytes:
    """Inverse of decrypt_sch.decrypt: XOR the same keystream onto a body."""
    ctx = seed(key)
    i, j = 0, 31
    out = bytearray(b'SDTE')
    for b in plain:
        i = (i + 1) % 55
        j = (j + 1) % 55
        ctx[i] = (ctx[i] - ctx[j]) & MASK32
        out.append(b ^ (ctx[i] & 0xFF))
    return bytes(out)


# ---------------------------------------------------------------------------
# known data defects
# ---------------------------------------------------------------------------
def check_defects(verbosity=0):
    """Report the shipped-data defects that a reader/dumper must survive.

    Returns (count, lines).  These are the "known problems" recorded in
    docs/script_gameplay.md sec.1; each is now handled *and* surfaced here.
    """
    findings = []

    forms = load_forms('merchandise.sch')
    items = forms[0]
    findings.append((
        'merchandise.sch',
        'cargo items',
        '%d (was 106: an escaped quote split "Телевизор 14\\"" and swallowed the '
        'next item)' % len(items)))

    # every `\\` in the shipped bytes, with the form it lives in
    for rel in script_list():
        raw = _raw_decrypted(rel)
        if b'\\' not in raw:
            continue
        n = raw.count(b'\\')
        findings.append((rel, 'backslash escapes', '%d occurrence(s): %s'
                         % (n, _escape_contexts(raw))))

    for rel in script_list():
        for form in walk(load_forms(rel)):
            for k in form:
                if k.kind != 'atom' or k.quoted:
                    continue
                if '#' not in k.text:
                    continue
                # `#` is the exponent marker the game's own reader accepts
                # (GameData::numWithHash, Economy::numLoose both map it to 'e'),
                # so the value is 1e6.  It is reported because it is the one
                # number in the data that plain `strtod` reads differently (1),
                # and the Python reader below is deliberately faithful to it.
                findings.append((rel, 'unusual number',
                                 '%s = %g (the game maps # to the exponent marker; '
                                 'plain strtod would stop at # and read 1)'
                                 % (k.text, game_number(k.text))))

    for rel in script_list():
        raw = _raw_decrypted(rel)
        if raw[:4] == b'SDTE':
            findings.append((rel, 'still encrypted', 'decrypt_sch.decrypt needed'))
    return findings


def _raw_decrypted(rel):
    p = os.path.join(DEC, rel.replace('/', '_') + '.txt')
    if os.path.isfile(p):
        return open(p, 'rb').read()
    return decrypt(open(os.path.join(ORIGINAL, rel), 'rb').read())


def _escape_contexts(raw, width=18):
    out = []
    for i, b in enumerate(raw):
        if b != 0x5C:
            continue
        nxt = raw[i + 1:i + 2]
        out.append('...%s\\%s...' % (raw[max(0, i - width):i].decode('cp1251', 'replace'),
                                     nxt.decode('cp1251', 'replace')))
    return ' | '.join(out[:8])


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def cmd_list(args):
    rels = script_list()
    print('%-28s %8s  %s' % ('script', 'bytes', 'top-level forms'))
    total = 0
    for rel in rels:
        forms = load_forms(rel)
        n = len(to_bytes(forms))
        total += n
        print('%-28s %8d  %d' % (rel, n, len(forms)))
    print('%-28s %8d  %d scripts' % ('TOTAL (canonical)', total, len(rels)))
    return 0


def cmd_all(args):
    out_dir = args.out
    os.makedirs(out_dir, exist_ok=True)
    rels = script_list()
    if args.only:
        rels = [r for r in rels if os.path.basename(r) in args.only]
    written = 0
    for rel in rels:
        path = os.path.join(out_dir, lua_filename(rel))
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(emit_lua(rel, load_forms(rel)))
        written += 1
    print('sch2lua: wrote %d file(s) to %s' % (written, os.path.relpath(out_dir, ROOT)))
    return 0


def cmd_one(args):
    forms = load_forms(args.one)
    text = emit_lua(args.one, forms)
    if args.print:
        sys.stdout.write(text)
    else:
        os.makedirs(args.out, exist_ok=True)
        path = os.path.join(args.out, lua_filename(args.one))
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
        print('wrote %s' % os.path.relpath(path, ROOT))
    return 0


def cmd_roundtrip(args):
    """canonical(original) == canonical(lua) for every script, and the canonical
    bytes must re-encrypt to the shipped SDTE body."""
    rels = script_list()
    if args.only:
        rels = [r for r in rels if os.path.basename(r) in args.only]
    lua_dir = args.out or os.path.join(HERE, 'out', 'lua')
    dumped = os.path.join(HERE, 'out', 'roundtrip')
    os.makedirs(lua_dir, exist_ok=True)
    if os.path.isdir(dumped):
        shutil.rmtree(dumped)
    os.makedirs(dumped)

    lua_paths = []
    for rel in rels:
        forms = load_forms(rel)
        path = os.path.join(lua_dir, lua_filename(rel))
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(emit_lua(rel, forms))
        lua_paths.append((rel, path))

    # One output file per input: exact comparison, no alignment guessing.
    lua_redump([p for _, p in lua_paths], args.lua, out_dir=dumped)

    failures = []
    total = 0
    for rel, path in lua_paths:
        want = canonical(load_forms(rel))
        gotf = os.path.join(dumped, lua_filename(rel) + '.sexp')
        got = open(gotf, 'rb').read() if os.path.isfile(gotf) else b''
        total += len(want)
        if got != want:
            failures.append(rel)
            if args.verbose:
                _show_diff(want, got, rel)
    if failures:
        print('ROUND TRIP FAILED for %d/%d script(s): %s'
              % (len(failures), len(rels), ', '.join(failures)))
        return 1
    print('ROUND TRIP OK: %d/%d scripts, %d canonical bytes'
          % (len(rels), len(rels), total))
    return 0


def cmd_check(args):
    findings = check_defects()
    print('sch2lua --check: %d finding(s)' % len(findings))
    for rel, kind, detail in findings:
        print('  %-26s %-18s %s' % (rel, kind, detail))
    return 0


def cmd_encode(args):
    """Rebuild the SDTE .sch from the Lua data (canonical text, re-encrypted)."""
    rels = script_list() if args.encode == 'all' else [args.encode]
    if args.out:
        os.makedirs(args.out, exist_ok=True)
    failures = []
    for rel in rels:
        plain = canonical(load_forms(rel))
        blob = encrypt(plain)
        if decrypt(blob) != plain:
            print('sch2lua: cipher self-test FAILED for %s' % rel)
            return 1
        out = os.path.join(args.out, os.path.basename(rel)) if args.out else \
            (os.path.splitext(rel)[0] + '.rebuilt.sch')
        with open(out, 'wb') as f:
            f.write(blob)
        # The shipped file is the same data with the author's own whitespace, so
        # it is never byte-identical; compare the parsed trees instead.
        shipped = os.path.join(ORIGINAL, rel)
        same_data = os.path.isfile(shipped) and \
            to_bytes(parse_file(shipped)) == to_bytes(parse_bytes(plain))
        if not same_data:
            failures.append(rel)
        print('%-24s %6d plaintext -> %6d SDTE  %s'
              % (rel, len(plain), len(blob),
                 'same data as shipped' if same_data else 'DATA MISMATCH'))
    if failures:
        print('sch2lua: %d file(s) rebuilt to different data' % len(failures))
        return 1
    return 0


def cmd_verify_sdte(args):
    """Whole pipeline: shipped .sch -> Lua -> canonical -> SDTE -> parse."""
    rels = script_list()
    if args.only:
        rels = [r for r in rels if os.path.basename(r) in args.only]
    bad = []
    for rel in rels:
        orig_plain = decrypt(open(os.path.join(ORIGINAL, rel), 'rb').read())
        want = to_bytes(parse_bytes(orig_plain))
        rebuilt = encrypt(canonical(parse_bytes(orig_plain)))
        if decrypt(rebuilt) != canonical(parse_bytes(orig_plain)):
            bad.append((rel, 'cipher'))
            continue
        if to_bytes(parse_bytes(decrypt(rebuilt))) != want:
            bad.append((rel, 'data'))
    if bad:
        print('SDTE VERIFY FAILED: %s' % ', '.join('%s(%s)' % b for b in bad))
        return 1
    print('SDTE VERIFY OK: %d/%d scripts decrypt -> parse -> re-encrypt -> parse '
          'unchanged' % (len(rels), len(rels)))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description='transpile Hard Truck 2 .sch data to Lua tables')
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--list', action='store_true', help='inventory of the 46 scripts')
    g.add_argument('--all', action='store_true', help='convert every script')
    g.add_argument('--one', metavar='REL', help='convert one script, e.g. ENV/mod_aa.sch')
    g.add_argument('--roundtrip', action='store_true',
                   help='canonical(.sch) == canonical(lua) for every script')
    g.add_argument('--check', action='store_true', help='report the known data defects')
    g.add_argument('--encode', metavar='REL',
                   help="rebuild the SDTE .sch from the canonical data ('all' for every script)")
    g.add_argument('--verify-sdte', action='store_true',
                   help='check decrypt -> parse -> re-encrypt -> parse for every script')
    ap.add_argument('--out', help='output directory (--all/--one: default tools/out/lua)')
    ap.add_argument('--only', nargs='+', metavar='FILE',
                    help='restrict --all/--roundtrip to these basenames')
    ap.add_argument('--print', action='store_true', help='--one: print instead of writing')
    ap.add_argument('--lua', help='Lua interpreter to use (default: auto)')
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args(argv)
    if args.out is None:
        args.out = DEFAULT_OUT

    if args.list:
        return cmd_list(args)
    if args.all:
        return cmd_all(args)
    if args.one:
        return cmd_one(args)
    if args.roundtrip:
        return cmd_roundtrip(args)
    if args.check:
        return cmd_check(args)
    if args.encode:
        return cmd_encode(args)
    if args.verify_sdte:
        return cmd_verify_sdte(args)
    return 0


if __name__ == '__main__':
    sys.exit(main())
