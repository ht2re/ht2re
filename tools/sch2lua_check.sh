#!/bin/sh
# sch2lua_check.sh -- verify the .sch -> Lua -> .sch pipeline end to end.
#
#   tools/sch2lua_check.sh            # everything
#   LUA=luajit tools/sch2lua_check.sh # pick the interpreter
#
# Steps:
#   1. reader invariants (escapes, `()` is not an atom, `1#e+006`, idempotency)
#   2. no regressions in the existing s-expression consumers
#   3. .sch -> Lua for all 46 scripts, then Lua -> canonical .sch
#      -> must be byte identical to canonical(original)
#   4. every generated file loads under the Lua interpreter (`dofile`)
#   5. decrypt -> parse -> re-encrypt -> parse keeps the data (SDTE round trip)
#   6. the known data defects are still reported with their locations
set -e
cd "$(dirname "$0")/.."

LUA=${LUA:-}
PY=${PY:-python3}
fail=0

say() { printf '\n== %s\n' "$1"; }
run() { if ! "$@"; then fail=1; fi; }

say "1. reader invariants (tools/sexp.py)"
$PY - <<'EOF'
import sys
sys.path.insert(0, 'tools')
from sexp import parse_bytes, to_text, canonical_number, game_number

CRLF = '\r\n'
cases = [
    # (source, canonical text) -- one top-level form per line, CRLF terminated
    (b'( A () ( "x\\"y" 1#e+006 ) )', '(A () ("x\\"y" 1#e+006))' + CRLF),
    (b'( X "a\\\\b" )',               '(X "a\\\\b")' + CRLF),
    (b'( X "a\\nb" )',               '(X "a\\nb")' + CRLF),
    # 0xFF is cp1251 U+044F; the canonical text for it is 'я'
    (b'( FIAT_1 . "\xff" )',         '(FIAT_1 . "\u044f")' + CRLF),
    (b'; only a comment\n',          ''),
    (b'( a b )\r\n( c )\r\n',        '(a b)' + CRLF + '(c)' + CRLF),
]
for src, want in cases:
    forms = parse_bytes(src)
    got = to_text(forms)
    assert got == want, 'canonical mismatch:\n  src  %r\n  got  %r\n  want %r' % (src, got, want)
    # idempotency: canonical text parses back to the same canonical text
    again = to_text(parse_bytes(got.encode('cp1251')))
    assert again == got, 'not idempotent for %r' % src

assert parse_bytes(b'""')[0].quoted, 'empty string must stay a quoted atom'
assert parse_bytes(b'()')[0].is_empty_list(), '() must parse as an empty list'
assert not parse_bytes(b'()')[0].is_atom, '() must not be an atom'
assert canonical_number('1#e+006') is None, '# is not part of the C number grammar'
assert game_number('1#e+006') == 1000000.0, 'the game reads # as the exponent marker'
assert canonical_number('node_store_1000') is None
print('reader invariants: ok (%d cases + 4 properties)' % len(cases))
EOF

say "1b. the shipped data carries no evaluation forms"
$PY - <<'EOF'
import collections, sys
sys.path.insert(0, 'tools')
import sch2lua
from sexp import walk

SCHEME = {'define', 'lambda', 'let', 'let*', 'letrec', 'if', 'cond', 'quote',
          'set!', 'begin', 'car', 'cdr', 'cons', 'apply', 'eval'}
heads = collections.Counter()
for rel in sch2lua.script_list():
    for form in walk(sch2lua.load_forms(rel)):
        heads[form.head()] += 1
bad = sorted(set(heads) & SCHEME)
assert not bad, 'scheme evaluation forms in the shipped data: %s' % bad
assert len(heads) > 700, 'head inventory shrank unexpectedly: %d' % len(heads)
print('no evaluation forms; %d distinct form heads across 46 scripts' % len(heads))
EOF

say "2. existing consumers still agree"
run $PY tools/script_survey.py > tools/out/survey.txt
run $PY tools/script_coverage.py > tools/out/coverage.txt
run $PY tools/gen_head_table.py > tools/out/head_table.txt
run $PY tools/verify_nodes.py > tools/out/verify_nodes.txt
run $PY tools/economy_check.py > tools/out/economy.txt
grep -q '^RESULT    : ok' tools/out/economy.txt
grep -q 'cargo' tools/out/economy.txt && grep -q 'cargo=107' tools/out/economy.txt
echo "survey/coverage/head-table/verify-nodes/economy: ok"

say "3. .sch -> Lua -> canonical .sch (all scripts)"
if [ -n "$LUA" ]; then
  run $PY tools/sch2lua.py --all
  run $PY tools/sch2lua.py --roundtrip --lua "$LUA"
else
  run $PY tools/sch2lua.py --all
  run $PY tools/sch2lua.py --roundtrip
fi

say "4. generated data loads under the Lua interpreter"
$PY - "$LUA" <<'EOF'
import glob, os, subprocess, sys
lua = sys.argv[1] or None
files = sorted(glob.glob('tools/out/lua/*.lua'))
assert files, 'no generated files: run sch2lua.py --all first'
driver = '''
package.path = %r .. '/?.lua;' .. package.path
local S = require 'lua_sexp'
for i = 1, #arg do
  local forms = assert(dofile(arg[i]))
  assert(type(forms) == 'table' and #forms > 0, arg[i] .. ': empty document')
  assert(S.to_text(forms) ~= '', arg[i] .. ': empty canonical text')
end
''' % os.path.abspath('tools').replace('\\', '\\\\').replace("'", "\\'")
p = subprocess.run([lua or 'lua', '-e', driver] + files,
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
# `lua -e` does not populate `arg` on 5.5; fall back to a script file
if p.returncode != 0:
    import tempfile
    with tempfile.NamedTemporaryFile('w', suffix='.lua', delete=False) as f:
        f.write(driver)
        path = f.name
    p = subprocess.run([lua or 'lua', path] + files,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    os.unlink(path)
if p.returncode != 0:
    sys.stderr.write(p.stderr.decode('utf-8', 'replace'))
    raise SystemExit('generated Lua failed to load')
print('dofile: %d generated file(s) ok' % len(files))
EOF

say "5. SDTE round trip (decrypt -> parse -> re-encrypt -> parse)"
run $PY tools/sch2lua.py --verify-sdte

say "6. known data defects"
run $PY tools/sch2lua.py --check

if [ "$fail" -ne 0 ]; then
  printf '\nSCH2LUA CHECK FAILED\n'
  exit 1
fi
printf '\nSCH2LUA CHECK OK\n'
