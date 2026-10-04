#include "Sexp.h"
#include <cstdlib>
#include <cstdio>
#include <cmath>

namespace ht2 {

std::string Sexp::head() const {
    if (kind != List || kids.empty() || kids[0].kind != Atom) return std::string();
    return kids[0].text;
}

const Sexp* Sexp::find(const std::string& h) const {
    for (const auto& k : kids)
        if (k.isList() && k.head() == h) return &k;
    return nullptr;
}

std::string Sexp::str(size_t i, const std::string& def) const {
    const Sexp* e = at(i);
    return e && e->kind == Atom ? e->text : def;
}

bool Sexp::isNumber(size_t i) const {
    const Sexp* e = at(i);
    if (!e || e->kind != Atom || e->text.empty() || e->quoted) return false;
    char* end = nullptr;
    strtod(e->text.c_str(), &end);
    return end && *end == '\0';
}

double Sexp::num(size_t i, double def) const {
    const Sexp* e = at(i);
    if (!e || e->kind != Atom || e->quoted) return def;
    char* end = nullptr;
    double v = strtod(e->text.c_str(), &end);
    return (end && *end == '\0') ? v : def;
}

std::string Sexp::dump(int depth) const {
    std::string pad(depth * 2, ' ');
    if (kind == Atom) return pad + (quoted ? ("\"" + text + "\"") : text);
    std::string s = pad + "(";
    for (size_t i = 0; i < kids.size(); ++i) {
        if (i) s += " ";
        s += kids[i].kind == Atom ? (kids[i].quoted ? ("\"" + kids[i].text + "\"") : kids[i].text)
                                  : ("\n" + kids[i].dump(depth + 1));
    }
    return s + ")";
}

// --- reader -----------------------------------------------------------------
namespace {
struct Reader {
    const std::string& s;
    size_t p = 0;
    std::string err;

    explicit Reader(const std::string& src) : s(src) {}

    void skip() {
        while (p < s.size()) {
            unsigned char c = (unsigned char)s[p];
            if (c == ';') { while (p < s.size() && s[p] != '\n') ++p; }
            else if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') ++p;
            else break;
        }
    }

    bool atom(Sexp& out) {
        out.kind = Sexp::Atom;
        if (s[p] == '"') {
            out.quoted = true;
            ++p;
            std::string v;
            // `\` escapes the next byte: merchandise.sch contains "Телевизор 14\""
            // and Ms.sch uses \n - stopping at the first quote swallowed the next
            // cargo item (106 instead of 107).
            while (p < s.size() && s[p] != '"') {
                if (s[p] == '\\' && p + 1 < s.size()) {
                    ++p;
                    switch (s[p]) {
                        case 'n': v += '\n'; break;
                        case 't': v += '\t'; break;
                        case 'r': v += '\r'; break;
                        default: v += s[p]; break;
                    }
                } else v += s[p];
                ++p;
            }
            if (p >= s.size()) { err = "unterminated string"; return false; }
            ++p;                               // closing quote
            out.text = v;
            return true;
        }
        size_t start = p;
        while (p < s.size()) {
            char c = s[p];
            if (c == '(' || c == ')' || c == ';' || c == ' ' || c == '\t' || c == '\r' || c == '\n')
                break;
            ++p;
        }
        out.text = s.substr(start, p - start);
        return !out.text.empty();
    }

    bool list(Sexp& out) {
        out.kind = Sexp::List;
        ++p;                                   // '('
        while (true) {
            skip();
            if (p >= s.size()) { err = "unexpected end of input (unbalanced '(')"; return false; }
            if (s[p] == ')') { ++p; return true; }
            Sexp kid;
            if (s[p] == '(') { if (!list(kid)) return false; }
            else if (!atom(kid)) {
                if (err.empty()) err = "empty atom";
                return false;
            }
            out.kids.push_back(std::move(kid));
        }
    }
};
} // namespace

bool parseSexp(const std::string& text, std::vector<Sexp>& out, std::string* err) {
    Reader r(text);
    while (true) {
        r.skip();
        if (r.p >= text.size()) break;
        Sexp e;
        if (text[r.p] == '(') {
            if (!r.list(e)) { if (err) *err = r.err; return false; }
        } else if (text[r.p] == ')') {
            if (err) *err = "unexpected ')'";
            return false;
        } else {
            if (!r.atom(e)) { if (err) *err = r.err.empty() ? "empty atom" : r.err; return false; }
        }
        out.push_back(std::move(e));
    }
    return true;
}

} // namespace ht2
