#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace ht2 {

// Minimal s-expression reader for the game's configuration scripts.
//
// Every shipped .sch / .CNF is pure data in s-expression form (vehicle lists,
// nodes, prices, ...), so the engine only needs a reader plus typed accessors —
// no evaluator.  Lexical rules: `;` starts a comment to end of line, `"` quotes
// a string (bytes are kept as-is; the data is cp1251), atoms are separated by
// whitespace or parentheses.
struct Sexp {
    enum Kind { Atom, List } kind = Atom;
    std::string text;          // atom text (unquoted for strings)
    bool quoted = false;       // atom came from "..." 
    std::vector<Sexp> kids;    // list elements

    bool isList() const { return kind == List; }
    bool isAtom() const { return kind == Atom; }
    size_t size() const { return kids.size(); }
    const Sexp* at(size_t i) const { return i < kids.size() ? &kids[i] : nullptr; }
    const Sexp* find(const std::string& head) const;   // first child list with this head
    // first element of a list, as text ("" when not a list / empty)
    std::string head() const;
    std::string str(size_t i, const std::string& def = std::string()) const;
    double num(size_t i, double def = 0.0) const;
    bool isNumber(size_t i) const;
    // Authored game-data numbers also use 1#e+006 (and 1#006).
    // Unlike strtod prefix parsing, these require a complete finite number.
    double gameNum(size_t i, double def = 0.0) const;
    bool isGameNumber(size_t i) const;
    // debug
    std::string dump(int depth = 0) const;
};

// Parses a whole document into its top-level forms.  Returns false and fills
// `err` on a syntax error (unbalanced parentheses, unterminated string).
bool parseSexp(const std::string& text, std::vector<Sexp>& out, std::string* err = nullptr);

} // namespace ht2
