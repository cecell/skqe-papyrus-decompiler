// SPDX-License-Identifier: GPL-3.0-only
#include "lexer.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace papyrus::psc {

bool ieq(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

namespace {

bool is_ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool is_ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

class Lexer {
public:
    Lexer(std::string_view text, std::vector<Token>& out) : m_t(text), m_out(out) {}

    bool run(LexError& err) {
        if (m_t.size() >= 3 && static_cast<unsigned char>(m_t[0]) == 0xEF && static_cast<unsigned char>(m_t[1]) == 0xBB &&
            static_cast<unsigned char>(m_t[2]) == 0xBF) {
            m_i = 3;  // UTF-8 byte order mark
        }
        while (m_i < m_t.size()) {
            const char c = m_t[m_i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
                ++m_i;
                ++m_col;
                continue;
            }
            if (c == '\n') {
                newline();
                continue;
            }
            if (c == '\\') {
                // Line continuation: a backslash, optional blanks and a comment, then the line end.
                size_t j = m_i + 1;
                while (j < m_t.size() && (m_t[j] == ' ' || m_t[j] == '\t' || m_t[j] == '\r')) ++j;
                if (j < m_t.size() && m_t[j] == ';' && !(j + 1 < m_t.size() && m_t[j + 1] == '/')) {
                    while (j < m_t.size() && m_t[j] != '\n') ++j;
                }
                if (j >= m_t.size()) {
                    m_i = j;
                    continue;
                }
                if (m_t[j] == '\n') {
                    m_i = j + 1;
                    ++m_line;
                    m_col = 1;
                    continue;
                }
                return fail(err, "unexpected backslash");
            }
            if (c == ';') {
                if (m_i + 1 < m_t.size() && m_t[m_i + 1] == '/') {
                    if (!block_comment(err)) return false;
                    continue;
                }
                while (m_i < m_t.size() && m_t[m_i] != '\n') ++m_i;
                continue;
            }
            if (c == '{') {
                if (!doc_string(err)) return false;
                continue;
            }
            if (c == '"') {
                if (!string(err)) return false;
                continue;
            }
            if (is_digit(c) || (c == '.' && m_i + 1 < m_t.size() && is_digit(m_t[m_i + 1]))) {
                if (!number(err)) return false;
                continue;
            }
            if (is_ident_start(c)) {
                size_t j = m_i;
                while (j < m_t.size() && is_ident_char(m_t[j])) ++j;
                push(Tok::Ident, std::string(m_t.substr(m_i, j - m_i)));
                m_col += int(j - m_i);
                m_i = j;
                continue;
            }
            if (!op(err)) return false;
        }
        if (m_out.empty() || m_out.back().kind != Tok::Newline) push(Tok::Newline, "\n");
        push(Tok::End, "");
        return true;
    }

private:
    bool fail(LexError& err, const std::string& what) {
        err.line = m_line;
        err.column = m_col;
        err.message = what;
        return false;
    }

    void push(Tok k, std::string text) {
        Token t;
        t.kind = k;
        t.text = std::move(text);
        t.line = m_line;
        t.column = m_col;
        m_out.push_back(std::move(t));
    }

    void newline() {
        if (m_out.empty() || m_out.back().kind != Tok::Newline) push(Tok::Newline, "\n");
        ++m_i;
        ++m_line;
        m_col = 1;
    }

    bool block_comment(LexError& err) {
        const int line = m_line, col = m_col;
        m_i += 2;
        while (m_i + 1 < m_t.size()) {
            if (m_t[m_i] == '/' && m_t[m_i + 1] == ';') {
                m_i += 2;
                m_col += 2;
                return true;
            }
            if (m_t[m_i] == '\n') {
                ++m_line;
                m_col = 1;
            } else {
                ++m_col;
            }
            ++m_i;
        }
        m_line = line;
        m_col = col;
        return fail(err, "unterminated block comment");
    }

    bool doc_string(LexError& err) {
        const int line = m_line, col = m_col;
        size_t j = m_i + 1;
        int ln = m_line;
        while (j < m_t.size() && m_t[j] != '}') {
            if (m_t[j] == '\n') ++ln;
            ++j;
        }
        if (j >= m_t.size()) return fail(err, "unterminated documentation string");
        std::string text(m_t.substr(m_i + 1, j - m_i - 1));
        // The compiler drops carriage returns and trims blanks and line
        // breaks at both ends (vanilla doc strings: " which \"number\" am
        // I? " is stored without the spaces, a CRLF inside as LF).
        text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
        const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\v'; };
        while (!text.empty() && blank(text.back())) text.pop_back();
        size_t lead = 0;
        while (lead < text.size() && blank(text[lead])) ++lead;
        text.erase(0, lead);
        Token t;
        t.kind = Tok::Doc;
        t.text = std::move(text);
        t.line = line;
        t.column = col;
        m_out.push_back(std::move(t));
        m_i = j + 1;
        m_line = ln;
        m_col = 1;
        return true;
    }

    bool string(LexError& err) {
        const int line = m_line, col = m_col;
        std::string s;
        size_t j = m_i + 1;
        while (j < m_t.size() && m_t[j] != '"') {
            if (m_t[j] == '\n') return fail(err, "unterminated string");
            if (m_t[j] == '\\' && j + 1 < m_t.size()) {
                const char e = m_t[j + 1];
                switch (e) {
                case 'n': s += '\n'; break;
                case 't': s += '\t'; break;
                case '"': s += '"'; break;
                case '\\': s += '\\'; break;
                default: s += '\\'; s += e; break;
                }
                j += 2;
                continue;
            }
            s += m_t[j++];
        }
        if (j >= m_t.size()) return fail(err, "unterminated string");
        Token t;
        t.kind = Tok::String;
        t.text = std::move(s);
        t.line = line;
        t.column = col;
        m_out.push_back(std::move(t));
        m_col += int(j + 1 - m_i);
        m_i = j + 1;
        return true;
    }

    bool number(LexError& err) {
        size_t j = m_i;
        Token t;
        t.line = m_line;
        t.column = m_col;
        if (m_t[j] == '0' && j + 1 < m_t.size() && (m_t[j + 1] == 'x' || m_t[j + 1] == 'X')) {
            j += 2;
            const size_t start = j;
            while (j < m_t.size() && std::isxdigit(static_cast<unsigned char>(m_t[j]))) ++j;
            if (j == start) return fail(err, "hexadecimal literal without digits");
            errno = 0;
            const unsigned long long v = std::strtoull(std::string(m_t.substr(start, j - start)).c_str(), nullptr, 16);
            if (errno == ERANGE || v > 0xFFFFFFFFull) return fail(err, "hexadecimal literal out of range");
            t.kind = Tok::Int;
            t.ival = static_cast<int32_t>(static_cast<uint32_t>(v));
        } else {
            while (j < m_t.size() && is_digit(m_t[j])) ++j;
            bool isFloat = false;
            if (j < m_t.size() && m_t[j] == '.' && j + 1 < m_t.size() && is_digit(m_t[j + 1])) {
                isFloat = true;
                ++j;
                while (j < m_t.size() && is_digit(m_t[j])) ++j;
            }
            const std::string text(m_t.substr(m_i, j - m_i));
            if (isFloat) {
                t.kind = Tok::Float;
                t.fval = std::strtof(text.c_str(), nullptr);
            } else {
                // An integer literal beyond int32 is clamped to 2147483647, as
                // PapyrusCompiler does (vanilla DLC1ChaurusCocoonScript passes
                // 10000000000 and the compiled file holds 2147483647).
                errno = 0;
                const long long v = std::strtoll(text.c_str(), nullptr, 10);
                t.kind = Tok::Int;
                t.ival = (errno == ERANGE || v > 2147483647LL) ? 2147483647 : static_cast<int32_t>(v);
            }
        }
        t.text = std::string(m_t.substr(m_i, j - m_i));
        m_out.push_back(std::move(t));
        m_col += int(j - m_i);
        m_i = j;
        return true;
    }

    bool op(LexError& err) {
        static const char* const kTwo[] = {"==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=", "%="};
        if (m_i + 1 < m_t.size()) {
            const std::string_view two = m_t.substr(m_i, 2);
            for (const char* o : kTwo) {
                if (two == o) {
                    push(Tok::Op, std::string(two));
                    m_i += 2;
                    m_col += 2;
                    return true;
                }
            }
        }
        const char c = m_t[m_i];
        if (std::strchr("()[],.=<>+-*/%!", c) && c != '\0') {
            push(Tok::Op, std::string(1, c));
            ++m_i;
            ++m_col;
            return true;
        }
        return fail(err, std::string("unexpected character '") + c + "'");
    }

    std::string_view m_t;
    std::vector<Token>& m_out;
    size_t m_i = 0;
    int m_line = 1;
    int m_col = 1;
};

}  // namespace

bool lex(std::string_view text, std::vector<Token>& out, LexError& error) {
    out.clear();
    Lexer l(text, out);
    return l.run(error);
}

}  // namespace papyrus::psc
