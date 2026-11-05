// Tokens of a Papyrus source file. Newlines are tokens (statements end at
// the end of a line); a backslash before the end of a line joins it to the
// next; ";" starts a line comment, ";/ ... /;" a block comment, "{ ... }" a
// documentation string (a token of its own). Keywords are identifiers; the
// parser compares them case-insensitively.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace papyrus::psc {

enum class Tok { Ident, Int, Float, String, Doc, Op, Newline, End };

struct Token {
    Tok kind = Tok::End;
    std::string text;  // identifier / operator spelling / decoded string / doc text
    int32_t ival = 0;
    float fval = 0.f;
    int line = 0;
    int column = 0;
};

struct LexError {
    int line = 0;
    int column = 0;
    std::string message;
};

// Tokenizes the whole text. Returns false with `error` set on the first
// problem (unterminated string or block comment, bad number).
bool lex(std::string_view text, std::vector<Token>& out, LexError& error);
// A source file's bytes as Bethesda's compiler reads them, as the compiled
// mod corpus shows: a UTF-8 byte order mark is dropped, a UTF-16 one is
// decoded, and anything else is the system code page, Windows-1252, whose
// bytes above 0x7F come out as UTF-8 (0x92 is stored as E2 80 99; a source
// saved as UTF-8 without a BOM is re-encoded byte by byte, as the compiled
// mod corpus shows).
std::string decode_source(std::string_view bytes);

// Case-insensitive ASCII comparison and lowering (identifiers and keywords).
bool ieq(std::string_view a, std::string_view b) noexcept;
std::string lower(std::string_view s);

}  // namespace papyrus::psc
