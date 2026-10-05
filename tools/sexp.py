#!/usr/bin/env python3
"""sexp.py -- s-expression reader/writer for the Hard Truck 2 .sch/.CNF scripts.

Pure data (no evaluation forms).  Lexical rules mirror src/script/Sexp.cpp:
  * `;` starts a comment to end of line
  * `"` quotes a string; `\\` escapes the next byte, so `\\"` is a literal quote,
    `\\n` a line break and `\\\\` a backslash
  * atoms are separated by whitespace or parentheses
  * the data is cp1251; the reader/writer work on **bytes**, so no byte is lost

Two distinct notions must not be confused (they used to be):
  * `is_atom`       -- "every child is a str" (convenience, kept for old callers)
  * `is_empty_list` -- an explicit `()`.  An empty list is *not* an atom: `town`
    and `parking` contain several `()` fields and `()` must survive a round trip.
    Parsed nodes carry `kind` ('atom' | 'list') so `()` stays a list.

Round trip.  `to_bytes(root)` prints the *canonical* form of a tree (single spaces,
one top-level form per line) and is the reference for the `sch -> lua -> sch` test:
canonical(original) == canonical(parse(lua_dump(original))).

Usage:
    from sexp import parse_file, parse_bytes, to_bytes, Sexp
    forms = parse_file('original/ENV/mod_aa.sch')
    assert to_bytes(forms) == to_bytes(parse_bytes(some_other_source))
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# A canonical atom is only digits with an optional sign, fraction and exponent.
# Everything else (identifiers such as `node_store_1000`, `FallAB1Key`, and
# `1#e+006`, whose `#` is not part of the C number grammar) stays an identifier
# and is emitted verbatim; `game_number` reads the `#` form the way the game does.
_NUMBER_CHARS = frozenset('.+-eE0123456789')


class Sexp(list):
    """An s-expression node.

    Attributes:
      quoted -- the atom was written as a "..." string and must be printed as one.
      kind   -- 'list' (default) or 'atom'.  Needed because `()` is a list that
                has no children and would otherwise look like an atom.
    """

    __slots__ = ('quoted', 'kind')

    def __init__(self, kids=(), quoted=False, kind=None):
        super().__init__(kids)
        self.quoted = quoted
        self.kind = kind if kind is not None else ('list' if kids else 'atom')

    # ---- construction -----------------------------------------------------
    @classmethod
    def atom(cls, text, quoted=False):
        e = cls([text], quoted=quoted, kind='atom')
        return e

    @classmethod
    def list(cls, kids=()):
        return cls(kids, kind='list')

    # ---- predicates -------------------------------------------------------
    @property
    def is_atom(self):
        """True for a parsed atom.  `()` is a list, not an atom."""
        return self.kind == 'atom'

    @property
    def is_list(self):
        return self.kind == 'list'

    def is_empty_list(self):
        return self.kind == 'list' and len(self) == 0

    def is_string(self):
        return self.kind == 'atom' and self.quoted

    @property
    def text(self):
        """Atom text, or '' for a list."""
        return self[0] if self.kind == 'atom' and len(self) else ''

    @property
    def kids(self):
        return list(self)

    def head(self):
        """First element of a list, as text ('' when not a list / empty)."""
        if self.kind != 'list' or not len(self):
            return ''
        k = self[0]
        if not isinstance(k, Sexp) or k.kind != 'atom':
            return ''
        return k.text

    def str(self, i, default=''):
        if i < 0 or i >= len(self):
            return default
        k = self[i]
        return k.text if isinstance(k, Sexp) and k.kind == 'atom' else default

    def num(self, i, default=0.0):
        v = _to_number(self.str(i, None))
        return default if v is None else v

    def is_number(self, i):
        return _to_number(self.str(i, None)) is not None

    def find(self, head):
        for k in self:
            if isinstance(k, Sexp) and k.kind == 'list' and k.head() == head:
                return k
        return None

    def findall(self, head):
        return [k for k in self
                if isinstance(k, Sexp) and k.kind == 'list' and k.head() == head]

    # ---- printing ---------------------------------------------------------
    def dump(self, depth=0, indent=2):
        """Human readable tree, one node per line (the old `_pp` format)."""
        pad = ' ' * (depth * indent)
        if self.kind == 'atom':
            return pad + _quote(self.text) if self.quoted else pad + self.text
        inner = [k.dump(depth + 1, indent) if isinstance(k, Sexp) else str(k)
                 for k in self]
        return pad + '(\n' + '\n'.join(inner) + '\n' + pad + ')'

    def to_text(self):
        return to_text(self)

    def __repr__(self):
        if self.kind == 'atom':
            return '"%s"' % self.text if self.quoted else self.text
        return '(%s)' % ' '.join(repr(k) for k in self)


# --- canonical serialisation -------------------------------------------------
def _quote(text):
    """Canonical spelling of a quoted atom (escapes only what is required)."""
    out = []
    for ch in text:
        if ch == '"':
            out.append('\\"')
        elif ch == '\\':
            out.append('\\\\')
        elif ch == '\n':
            out.append('\\n')
        elif ch == '\r':
            out.append('\\r')
        elif ch == '\t':
            out.append('\\t')
        else:
            out.append(ch)
    return '"' + ''.join(out) + '"'


def to_text(root):
    """Canonical text of a form (or list of forms).  Stable, single-spaced."""
    if isinstance(root, Sexp) and root.kind == 'atom':
        return _quote(root.text) if root.quoted else root.text
    if isinstance(root, Sexp):
        return '(' + ' '.join(to_text(k) for k in root) + ')'
    # a list of top-level forms
    return '\r\n'.join(to_text(f) for f in root) + ('\r\n' if len(root) else '')


def to_bytes(root, encoding='cp1251'):
    """Canonical cp1251 bytes of a form (or list of top-level forms)."""
    return to_text(root).encode(encoding, errors='strict')


# --- reader ------------------------------------------------------------------
def _tokenize(text):
    """Split into '(', ')', and atoms.  A quoted atom is one token.

    Returns a list of `(token, quoted)`.  Escapes are resolved here, so the
    caller never sees a backslash: `\\"` becomes `"`, `\\n` a newline.
    """
    toks = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in ' \t\r\n\f\v':
            i += 1
        elif c == ';':
            while i < n and text[i] not in '\r\n':
                i += 1
        elif c in '()':
            toks.append((c, False))
            i += 1
        elif c == '"':
            i += 1
            buf = []
            while True:
                if i >= n:
                    raise SyntaxError('unterminated string')
                c = text[i]
                if c == '"':
                    i += 1
                    break
                if c == '\\' and i + 1 < n:
                    i += 1
                    c = text[i]
                    buf.append({'n': '\n', 't': '\t', 'r': '\r'}.get(c, c))
                else:
                    buf.append(c)
                i += 1
            toks.append((''.join(buf), True))
        else:
            j = i
            while j < n and text[j] not in ' \t\r\n()':
                j += 1
            toks.append((text[i:j], False))
            i = j
    return toks


def parse(text):
    """Parse a whole document -> list of top-level Sexp forms."""
    toks = _tokenize(text)
    pos = 0

    def rd():
        nonlocal pos
        if pos >= len(toks):
            raise SyntaxError('unexpected EOF')
        tok, quoted = toks[pos]
        pos += 1
        if tok == '(':
            kids = []
            while True:
                if pos >= len(toks):
                    raise SyntaxError('unbalanced (')
                if toks[pos][0] == ')':
                    pos += 1
                    return Sexp.list(kids)
                kids.append(rd())
        if tok == ')':
            raise SyntaxError('unexpected )')
        return Sexp.atom(tok, quoted)

    out = []
    while pos < len(toks):
        out.append(rd())
    return out


def parse_bytes(data: bytes, encoding='cp1251'):
    return parse(decode(data, encoding))


def decode(data: bytes, encoding='cp1251'):
    """cp1251 decode with a latin-1 fallback for stray out-of-range bytes."""
    try:
        return data.decode(encoding, errors='strict')
    except UnicodeDecodeError:
        sys.stderr.write('sexp: %s has bytes outside %s, using latin-1\n'
                         % (encoding, encoding))
        return data.decode('latin-1')


def parse_file(path, base=None, encoding='cp1251'):
    """Decrypt (SDTE) if needed, then parse."""
    from decrypt_sch import decrypt
    raw = open(path, 'rb').read()
    if raw[:4] == b'SDTE':
        raw = decrypt(raw)
    return parse_bytes(raw, encoding)


def parse_decrypted(base, relpath, encoding='cp1251'):
    """Parse the pre-decrypted copy: base/relpath with '/' replaced by '_'."""
    p = os.path.join(base, relpath.replace('/', '_') + '.txt')
    return parse_bytes(open(p, 'rb').read(), encoding)


def walk(forms):
    """Yield every list form in the tree, depth-first."""
    stack = list(forms)
    while stack:
        e = stack.pop()
        if not isinstance(e, Sexp) or e.kind != 'list':
            continue
        yield e
        stack.extend(e)


# --- numbers -----------------------------------------------------------------
def canonical_number(tok):
    """Canonical numeric spelling, or None when `tok` is an identifier.

    Strict on purpose: only the C `strtod` grammar.  `1#e+006` in
    merchandise.sch therefore returns None -- `#` is not part of a number for
    strtod -- while `game_number` reads it the way the game does.
    """
    if not tok or tok == '.':
        return None
    if not tok[0] in _NUMBER_CHARS:
        return None
    if not set(tok) <= _NUMBER_CHARS:
        return None
    try:
        v = float(tok)
    except ValueError:
        return None
    return _spell(v)


def game_number(tok):
    """The value the *game* reads, i.e. with `#` as the exponent marker.

    Authored `1#e+006` denotes 1000000. C++ Sexp::gameNum handles the same
    spelling; the strict accessor intentionally leaves it non-numeric.
    """
    if tok is None:
        return None
    if '#' in tok:
        # `1#e+006`: the # marks the exponent, and the e follows it.  Replace it
        # with 'e' only when there is no exponent letter yet, drop it otherwise.
        tok = tok.replace('#', '') if ('e' in tok or 'E' in tok) else tok.replace('#', 'e')
    tail = tok[1:] if tok[:1] in '+-' else tok
    if not tail or not set(tail) <= _NUMBER_CHARS or tail[0] not in '.0123456789':
        return None
    try:
        return float(tok)
    except ValueError:
        return None


def _spell(v):
    if v == int(v) and abs(v) < 1e16:
        return str(int(v))
    return repr(v)


def _to_number(tok):
    """Strict numeric value (see `canonical_number`), or None."""
    if tok is None:
        return None
    c = canonical_number(tok)
    return float(c) if c is not None else None


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser(description='parse/print .sch s-expressions')
    ap.add_argument('path')
    ap.add_argument('--canonical', action='store_true',
                    help='print the canonical form instead of the tree')
    a = ap.parse_args()
    forms = parse_file(a.path)
    if a.canonical:
        sys.stdout.buffer.write(to_bytes(forms))
    else:
        for f in forms:
            print(f.dump())
