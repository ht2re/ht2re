#!/usr/bin/env python3
"""sexp.py -- tiny s-expression reader for the Hard Truck 2 .sch/.CNF scripts.

Pure data (no evaluation forms).  Lexical rules mirror src/script/Sexp.cpp:
  * `;` starts a comment to end of line
  * `"` quotes a string; bytes are kept as-is, the data is cp1251
  * atoms are separated by whitespace or parentheses

Usage:
    from sexp import parse_file, Sexp
    forms = parse_file('original/ENV/mod_aa.sch')

`Sexp` is a list subclass:  atom -> Sexp with kind='atom', list -> kind='list'
and kids.  Convenience accessors match the C++ ones (head/str/num/find).
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


class Sexp(list):
    """An s-expression node.  `quoted` marks a "..." string atom."""

    __slots__ = ('quoted',)

    def __init__(self, kids=(), quoted=False):
        super().__init__(kids)
        self.quoted = quoted

    # ---- predicates -------------------------------------------------------
    @property
    def is_atom(self):
        return all(isinstance(k, str) for k in self)

    @property
    def is_list(self):
        return not self.is_atom

    @property
    def text(self):
        """Atom text, or '' for a list."""
        return self[0] if self.is_atom and self else ''

    @property
    def kids(self):
        return list(self)

    def head(self):
        """First element of a list, as text ('' when not a list / empty)."""
        if self.is_atom or not self:
            return ''
        k = self[0]
        return k.text if isinstance(k, Sexp) else str(k)

    def str(self, i, default=''):
        if i < 0 or i >= len(self):
            return default
        k = self[i]
        return k.text if isinstance(k, Sexp) else str(k)

    def num(self, i, default=0.0):
        try:
            return float(self.str(i))
        except (TypeError, ValueError):
            return default

    def is_number(self, i):
        if i < 0 or i >= len(self):
            return False
        k = self[i]
        if not isinstance(k, Sexp) or not k.is_atom:
            return False
        try:
            float(k.text)
            return True
        except ValueError:
            return False

    def find(self, head):
        for k in self:
            if isinstance(k, Sexp) and k.is_list and k.head() == head:
                return k
        return None

    def findall(self, head):
        return [k for k in self if isinstance(k, Sexp) and k.is_list and k.head() == head]

    def __repr__(self):
        if self.is_atom:
            return '"%s"' % self.text if self.quoted else self.text
        return '(%s)' % ' '.join(repr(k) for k in self)


def parse(text):
    """Parse a whole document -> list of top-level Sexp forms."""
    toks = _tokenize(text)
    pos = 0
    out = []

    def rd():
        nonlocal pos
        if pos >= len(toks):
            raise SyntaxError('unexpected EOF')
        t = toks[pos]
        pos += 1
        if t == '(':
            kids = []
            while pos < len(toks) and toks[pos] != ')':
                kids.append(rd())
            if pos >= len(toks):
                raise SyntaxError('unbalanced (')
            pos += 1
            return Sexp(kids)
        if t == ')':
            raise SyntaxError('unexpected )')
        return Sexp([t[1:-1] if t.startswith('"') else t], quoted=t.startswith('"'))

    while pos < len(toks):
        out.append(rd())
    return out


def _tokenize(text):
    toks = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in ' \t\r\n':
            i += 1
        elif c == ';':
            while i < n and text[i] not in '\r\n':
                i += 1
        elif c in '()':
            toks.append(c)
            i += 1
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 1
            toks.append('"' + text[i + 1:j] + '"')
            i = j + 1
        else:
            j = i
            while j < n and text[j] not in ' \t\r\n()':
                j += 1
            toks.append(text[i:j])
            i = j
    return toks


def parse_bytes(data: bytes):
    return parse(data.decode('cp1251'))


def parse_file(path, base=None):
    """Decrypt (SDTE) if needed, then parse."""
    from decrypt_sch import decrypt
    raw = open(path, 'rb').read()
    if raw[:4] == b'SDTE':
        raw = decrypt(raw)
    return parse_bytes(raw)


def parse_decrypted(base, relpath):
    """Parse the pre-decrypted copy: base/relpath with '/' replaced by '_'."""
    p = os.path.join(base, relpath.replace('/', '_') + '.txt')
    return parse_bytes(open(p, 'rb').read())


def walk(forms):
    """Yield every list form in the tree, depth-first."""
    stack = list(forms)
    while stack:
        e = stack.pop()
        if not isinstance(e, Sexp) or e.is_atom:
            continue
        yield e
        stack.extend(e)


if __name__ == '__main__':
    import json
    for f in parse_file(sys.argv[1]):
        print(json.dumps(f, default=str))
