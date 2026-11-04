// Papyrus assembly listing -> Script (include/papyrus/assemble.hpp).
//
// Line based: a line is a directive (".word args"), a label ("name:"), an
// instruction ("MNEMONIC operands") or blank; ";" starts a comment and a
// comment of the form ";@line N" after an instruction is its debug line.
// Whitespace is free, directive and mnemonic case is free, quoted strings
// carry the escapes the writer emits (\" \\ \n \r \t).
// SPDX-License-Identifier: GPL-3.0-only
#include "papyrus/assemble.hpp"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace papyrus::pex {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

bool all_digits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

// [-]digits
bool looks_integer(std::string_view s) {
    if (!s.empty() && s[0] == '-') s.remove_prefix(1);
    return all_digits(s);
}

// [-]digits.digits (either side may be empty, not both). "nan" and "inf" are
// identifiers to PapyrusAssembler (a mod names a variable inf), so here too.
bool looks_float(std::string_view s) {
    if (!s.empty() && s[0] == '-') s.remove_prefix(1);
    const size_t dot = s.find('.');
    if (dot == std::string_view::npos) return false;
    const std::string_view a = s.substr(0, dot), b = s.substr(dot + 1);
    if (a.empty() && b.empty()) return false;
    return (a.empty() || all_digits(a)) && (b.empty() || all_digits(b));
}

constexpr uint16_t kNoLine = 0xFFFF;  // "no ;@line on this instruction" while a function is read

struct Tok {
    std::string text;
    bool quoted = false;
};

struct Line {
    size_t number = 0;
    std::vector<Tok> toks;
    bool hasDebugLine = false;
    uint16_t debugLine = 0;
};

class Assembler {
public:
    Assembler(std::string_view text, Script& out) : m_text(text), m_s(out) {}

    Status run() {
        m_s = Script{};
        m_s.header.major = 3;
        m_s.header.minor = 2;
        m_s.header.gameId = 1;
        StrIdx empty;
        if (!intern("", empty)) return m_status;
        bool sawObjects = false;
        while (next()) {
            const std::string d = directive();
            if (d == ".info") {
                if (!info()) return m_status;
            } else if (d == ".userflagsref") {
                if (!user_flags()) return m_status;
            } else if (d == ".objecttable") {
                if (!objects()) return m_status;
                sawObjects = true;
            } else {
                return error("expected .info, .userFlagsRef or .objectTable, got " + m_cur.toks[0].text);
            }
        }
        if (m_status != Status::Ok) return m_status;
        if (!sawObjects) return error("no .objectTable");
        // PapyrusAssembler always writes debug info: one record per function,
        // empty for a function without ";@line" comments (every script in a
        // 77,000-file Data folder carries it, so this mirrors every file).
        m_s.debug.present = true;
        return Status::Ok;
    }

    const std::string& error_text() const { return m_error; }

private:
    // --- errors ------------------------------------------------------------

    Status error(const std::string& what, Status st = Status::Syntax) {
        if (m_status == Status::Ok) {
            m_status = st;
            m_error = "line " + std::to_string(m_cur.number) + ": " + what;
        }
        return st;
    }
    bool fail(const std::string& what, Status st = Status::Syntax) {
        error(what, st);
        return false;
    }

    // --- strings -----------------------------------------------------------

    // Case-insensitive, first spelling wins, identifiers and literals in one
    // table (PapyrusAssembler's behaviour). An identifier that names a
    // built-in type, the ::State variable, or is the one letter "s" is stored
    // in the reference tool's own spelling, whatever the listing wrote
    // (docs/ASM_SYNTAX.md, "What the assembler changes").
    bool intern(std::string_view s, StrIdx& out, bool identifier = false) {
        const std::string key = lower(s);
        auto it = m_interned.find(key);
        if (it != m_interned.end()) {
            out = it->second;
            return true;
        }
        if (m_s.strings.size() >= 0xFFFF) return fail("string table full (65535 entries)", Status::TooLarge);
        if (s.size() > 0xFFFF) return fail("string longer than 65535 bytes", Status::TooLarge);
        const char* canonical = identifier ? builtin_spelling(key) : nullptr;
        m_s.strings.emplace_back(canonical ? std::string_view(canonical) : s);
        out = static_cast<StrIdx>(m_s.strings.size() - 1);
        m_interned.emplace(key, out);
        return true;
    }

    static const char* builtin_spelling(const std::string& key) {
        static const char* const kNames[] = {"Int", "Float", "Bool", "String", "None", "Int[]", "Float[]",
                                             "Bool[]", "String[]", "::State", "S"};
        for (const char* n : kNames) {
            if (lower(n) == key) return n;
        }
        return nullptr;
    }

    // --- lines -------------------------------------------------------------

    // Reads the next line that holds anything; false at the end or on a
    // tokenizing error (status set).
    bool next() {
        while (m_pos < m_text.size()) {
            size_t end = m_text.find('\n', m_pos);
            if (end == std::string_view::npos) end = m_text.size();
            std::string_view raw = m_text.substr(m_pos, end - m_pos);
            m_pos = end + 1;
            ++m_lineNo;
            if (!tokenize(raw)) return false;
            if (!m_cur.toks.empty()) return true;
        }
        return false;
    }

    bool tokenize(std::string_view raw) {
        m_cur = Line{};
        m_cur.number = m_lineNo;
        size_t i = 0;
        const size_t n = raw.size();
        while (i < n) {
            const char c = raw[i];
            if (is_space(c)) {
                ++i;
                continue;
            }
            if (c == ';') {
                std::string_view comment = raw.substr(i + 1);
                while (!comment.empty() && is_space(comment.front())) comment.remove_prefix(1);
                if (comment.size() > 5 && comment.substr(0, 5) == "@line") {
                    std::string_view num = comment.substr(5);
                    while (!num.empty() && is_space(num.front())) num.remove_prefix(1);
                    size_t len = 0;
                    while (len < num.size() && num[len] >= '0' && num[len] <= '9') ++len;
                    if (len > 0) {
                        const unsigned long v = std::strtoul(std::string(num.substr(0, len)).c_str(), nullptr, 10);
                        m_cur.hasDebugLine = true;
                        m_cur.debugLine = static_cast<uint16_t>(v > 0xFFFE ? 0xFFFE : v);
                    }
                }
                break;
            }
            if (c == '"') {
                Tok t;
                t.quoted = true;
                ++i;
                bool closed = false;
                while (i < n) {
                    const char d = raw[i++];
                    if (d == '"') {
                        closed = true;
                        break;
                    }
                    if (d == '\\' && i < n) {
                        const char e = raw[i++];
                        switch (e) {
                        case 'n': t.text += '\n'; break;
                        case 'r': t.text += '\r'; break;
                        case 't': t.text += '\t'; break;
                        case '"': t.text += '"'; break;
                        case '\\': t.text += '\\'; break;
                        default: t.text += '\\'; t.text += e; break;
                        }
                        continue;
                    }
                    t.text += d;
                }
                if (!closed) return fail("unterminated string");
                m_cur.toks.push_back(std::move(t));
                continue;
            }
            size_t j = i;
            while (j < n && !is_space(raw[j]) && raw[j] != ';') ++j;
            Tok t;
            t.text.assign(raw.data() + i, j - i);
            m_cur.toks.push_back(std::move(t));
            i = j;
        }
        return true;
    }

    // The directive of the current line, lowercased; "" when the line is not one.
    std::string directive() const {
        const Tok& t = m_cur.toks[0];
        if (t.quoted || t.text.empty() || t.text[0] != '.') return std::string();
        return lower(t.text);
    }
    size_t argc() const { return m_cur.toks.size() - 1; }
    const Tok& arg(size_t i) const { return m_cur.toks[i + 1]; }

    bool want_args(size_t n) {
        if (argc() != n) return fail(m_cur.toks[0].text + " takes " + std::to_string(n) + " argument(s), got " + std::to_string(argc()));
        return true;
    }

    bool parse_u32(const Tok& t, uint32_t& out) {
        if (t.quoted || !all_digits(t.text)) return fail("expected a number, got " + t.text);
        errno = 0;
        const unsigned long long v = std::strtoull(t.text.c_str(), nullptr, 10);
        if (errno == ERANGE || v > 0xFFFFFFFFull) return fail("number out of range: " + t.text);
        out = static_cast<uint32_t>(v);
        return true;
    }
    bool parse_u64(const Tok& t, uint64_t& out) {
        if (t.quoted || !all_digits(t.text)) return fail("expected a number, got " + t.text);
        errno = 0;
        out = std::strtoull(t.text.c_str(), nullptr, 10);
        if (errno == ERANGE) return fail("number out of range: " + t.text);
        return true;
    }
    bool ident(const Tok& t, StrIdx& out) {
        if (t.quoted) return fail("expected an identifier, got a string");
        return intern(t.text, out, true);
    }
    bool text(const Tok& t, StrIdx& out) { return intern(t.text, out); }

    // A literal or identifier operand, typed as PapyrusAssembler types them.
    bool typed(const Tok& t, Value& out) {
        StrIdx s;
        if (t.quoted) {
            if (!intern(t.text, s)) return false;
            out = Value::string(s);
            return true;
        }
        const std::string l = lower(t.text);
        if (l == "none") {
            out = Value::none();
            return true;
        }
        if (l == "true" || l == "false") {
            out = Value::boolean(l == "true");
            return true;
        }
        if (looks_integer(t.text)) {
            errno = 0;
            const long long v = std::strtoll(t.text.c_str(), nullptr, 10);
            if (errno == ERANGE || v < -2147483648LL || v > 2147483647LL) return fail("integer out of range: " + t.text);
            out = Value::integer(static_cast<int32_t>(v));
            return true;
        }
        if (looks_float(t.text)) {
            out = Value::floating(std::strtof(t.text.c_str(), nullptr));
            return true;
        }
        if (!intern(t.text, s, true)) return false;
        out = Value::identifier(s);
        return true;
    }

    // --- sections ----------------------------------------------------------

    bool info() {
        while (next()) {
            const std::string d = directive();
            if (d == ".endinfo") return true;
            if (d == ".source") {
                if (!want_args(1)) return false;
                m_s.header.sourceName = arg(0).text;
            } else if (d == ".modifytime") {
                if (!want_args(1) || !parse_u64(arg(0), m_s.debug.modificationTime)) return false;
            } else if (d == ".compiletime") {
                if (!want_args(1) || !parse_u64(arg(0), m_s.header.compileTime)) return false;
            } else if (d == ".user") {
                if (!want_args(1)) return false;
                m_s.header.userName = arg(0).text;
            } else if (d == ".computer") {
                if (!want_args(1)) return false;
                m_s.header.computerName = arg(0).text;
            } else {
                return fail("unexpected " + m_cur.toks[0].text + " in .info");
            }
        }
        return m_status == Status::Ok ? fail("missing .endInfo") : false;
    }

    bool user_flags() {
        while (next()) {
            const std::string d = directive();
            if (d == ".enduserflagsref") return true;
            if (d != ".flag") return fail("unexpected " + m_cur.toks[0].text + " in .userFlagsRef");
            if (!want_args(2)) return false;
            UserFlag f;
            uint32_t bit;
            if (!ident(arg(0), f.name) || !parse_u32(arg(1), bit)) return false;
            if (bit > 31) return fail("flag bit out of range: " + arg(1).text);
            f.bit = static_cast<uint8_t>(bit);
            m_s.userFlags.push_back(f);
        }
        return m_status == Status::Ok ? fail("missing .endUserFlagsRef") : false;
    }

    bool objects() {
        while (next()) {
            const std::string d = directive();
            if (d == ".endobjecttable") return true;
            if (d != ".object") return fail("unexpected " + m_cur.toks[0].text + " in .objectTable");
            if (!object()) return false;
        }
        return m_status == Status::Ok ? fail("missing .endObjectTable") : false;
    }

    bool object() {
        if (argc() < 1 || argc() > 2) return fail(".object takes a name and an optional parent");
        Object o;
        if (!ident(arg(0), o.name)) return false;
        if (argc() == 2) {
            if (!ident(arg(1), o.parent)) return false;
        } else if (!intern("", o.parent)) {
            return false;
        }
        if (!intern("", o.docString) || !intern("", o.autoState)) return false;
        while (next()) {
            const std::string d = directive();
            if (d == ".endobject") {
                m_s.objects.push_back(std::move(o));
                return true;
            }
            if (d == ".userflags") {
                if (!want_args(1) || !parse_u32(arg(0), o.userFlags)) return false;
            } else if (d == ".docstring") {
                if (!want_args(1) || !text(arg(0), o.docString)) return false;
            } else if (d == ".autostate") {
                if (argc() > 1) return fail(".autoState takes at most one name");
                if (argc() == 1 && !ident(arg(0), o.autoState)) return false;
            } else if (d == ".variabletable") {
                if (!variables(o)) return false;
            } else if (d == ".propertytable") {
                if (!properties(o)) return false;
            } else if (d == ".statetable") {
                if (!states(o)) return false;
            } else {
                return fail("unexpected " + m_cur.toks[0].text + " in .object");
            }
        }
        return m_status == Status::Ok ? fail("missing .endObject") : false;
    }

    bool variables(Object& o) {
        while (next()) {
            const std::string d = directive();
            if (d == ".endvariabletable") return true;
            if (d != ".variable") return fail("unexpected " + m_cur.toks[0].text + " in .variableTable");
            if (!want_args(2)) return false;
            Variable v;
            if (!ident(arg(0), v.name) || !ident(arg(1), v.type)) return false;
            bool closed = false;
            while (next()) {
                const std::string e = directive();
                if (e == ".endvariable") {
                    closed = true;
                    break;
                }
                if (e == ".userflags") {
                    if (!want_args(1) || !parse_u32(arg(0), v.userFlags)) return false;
                } else if (e == ".initialvalue") {
                    if (!want_args(1) || !typed(arg(0), v.initial)) return false;
                } else {
                    return fail("unexpected " + m_cur.toks[0].text + " in .variable");
                }
            }
            if (!closed) return m_status == Status::Ok ? fail("missing .endVariable") : false;
            o.variables.push_back(v);
        }
        return m_status == Status::Ok ? fail("missing .endVariableTable") : false;
    }

    bool properties(Object& o) {
        while (next()) {
            const std::string d = directive();
            if (d == ".endpropertytable") return true;
            if (d != ".property") return fail("unexpected " + m_cur.toks[0].text + " in .propertyTable");
            if (argc() < 2 || argc() > 3) return fail(".property takes a name, a type and optionally auto");
            Property p;
            if (!ident(arg(0), p.name) || !ident(arg(1), p.type) || !intern("", p.docString)) return false;
            bool isAuto = false;
            if (argc() == 3) {
                if (lower(arg(2).text) != "auto") return fail("expected auto, got " + arg(2).text);
                isAuto = true;
            }
            bool closed = false;
            while (next()) {
                const std::string e = directive();
                if (e == ".endproperty") {
                    closed = true;
                    break;
                }
                if (e == ".userflags") {
                    if (!want_args(1) || !parse_u32(arg(0), p.userFlags)) return false;
                } else if (e == ".docstring") {
                    if (!want_args(1) || !text(arg(0), p.docString)) return false;
                } else if (e == ".autovar") {
                    if (!want_args(1) || !ident(arg(0), p.autoVar)) return false;
                } else if (e == ".function") {
                    if (argc() < 1) return fail(".function needs a name");
                    const std::string which = lower(arg(0).text);
                    if (which != "get" && which != "set") return fail("a property function is get or set, got " + arg(0).text);
                    Function f;
                    std::vector<uint16_t> lines;
                    bool hasLines = false;
                    if (!function(f, lines, hasLines)) return false;
                    const FunctionKind kind = which == "get" ? FunctionKind::Getter : FunctionKind::Setter;
                    fill_lines(lines);
                    record(o.name, empty_idx(), p.name, kind, std::move(lines));
                    if (kind == FunctionKind::Getter) p.getter = std::move(f);
                    else p.setter = std::move(f);
                } else {
                    return fail("unexpected " + m_cur.toks[0].text + " in .property");
                }
            }
            if (!closed) return m_status == Status::Ok ? fail("missing .endProperty") : false;
            if (isAuto) {
                p.flags = kPropRead | kPropWrite | kPropAutoVar;
            } else {
                p.flags = 0;
                if (p.getter) p.flags |= kPropRead;
                if (p.setter) p.flags |= kPropWrite;
            }
            o.properties.push_back(std::move(p));
        }
        return m_status == Status::Ok ? fail("missing .endPropertyTable") : false;
    }

    bool states(Object& o) {
        while (next()) {
            const std::string d = directive();
            if (d == ".endstatetable") return true;
            if (d != ".state") return fail("unexpected " + m_cur.toks[0].text + " in .stateTable");
            if (argc() > 1) return fail(".state takes at most one name");
            State st;
            if (argc() == 1) {
                if (!ident(arg(0), st.name)) return false;
            } else if (!intern("", st.name)) {
                return false;
            }
            bool closed = false;
            bool first = true;
            while (next()) {
                const std::string e = directive();
                if (e == ".endstate") {
                    closed = true;
                    break;
                }
                if (e != ".function") return fail("unexpected " + m_cur.toks[0].text + " in .state");
                if (argc() < 1) return fail(".function needs a name");
                NamedFunction nf;
                if (!ident(arg(0), nf.name)) return false;
                std::vector<uint16_t> lines;
                bool hasLines = false;
                if (!function(nf.body, lines, hasLines)) return false;
                // PapyrusAssembler labels only the first function of a state
                // block with the state's name; the rest get the default state.
                fill_lines(lines);
                record(o.name, first ? st.name : empty_idx(), nf.name, FunctionKind::Method, std::move(lines));
                first = false;
                st.functions.push_back(std::move(nf));
            }
            if (!closed) return m_status == Status::Ok ? fail("missing .endState") : false;
            o.states.push_back(std::move(st));
        }
        return m_status == Status::Ok ? fail("missing .endStateTable") : false;
    }

    // Every function gets a record (PapyrusAssembler's behaviour, confirmed
    // on Probe01 and the 14,026 vanilla scripts): one line per instruction
    // when any instruction carries ";@line" (0 where one is missing), no
    // lines at all otherwise (GetState / GotoState, empty bodies). Records
    // with the same (object, label, name, kind) merge: the first keeps its
    // position, the last one's lines win. With the labelling above that
    // merges a state's second function into the default state's function of
    // the same name; a key without the label loses 1,016 vanilla scripts.
    void record(StrIdx object, StrIdx label, StrIdx name, FunctionKind kind, std::vector<uint16_t> lines) {
        const auto key = std::make_tuple(object, label, name, int(kind));
        auto it = m_records.find(key);
        if (it != m_records.end()) {
            m_s.debug.functions[it->second].lines = std::move(lines);
            return;
        }
        FunctionDebug d;
        d.object = object;
        d.state = label;
        d.name = name;
        d.kind = kind;
        d.lines = std::move(lines);
        m_records.emplace(key, m_s.debug.functions.size());
        m_s.debug.functions.push_back(std::move(d));
    }

    StrIdx empty_idx() {
        StrIdx i = 0;
        intern("", i);
        return i;
    }

    // At ".function Name [static] [native]" (property accessors: Name is get/set).
    bool function(Function& f, std::vector<uint16_t>& lines, bool& hasLines) {
        for (size_t i = 1; i < argc(); ++i) {
            const std::string flag = lower(arg(i).text);
            if (flag == "static") f.flags |= kFnGlobal;
            else if (flag == "native") f.flags |= kFnNative;
            else return fail("unknown function flag " + arg(i).text);
        }
        if (!intern("", f.docString) || !intern("", f.returnType)) return false;
        bool sawCode = false;
        hasLines = false;
        while (next()) {
            const std::string d = directive();
            if (d == ".endfunction") return true;
            if (d == ".userflags") {
                if (!want_args(1) || !parse_u32(arg(0), f.userFlags)) return false;
            } else if (d == ".docstring") {
                if (!want_args(1) || !text(arg(0), f.docString)) return false;
            } else if (d == ".return") {
                if (!want_args(1) || !ident(arg(0), f.returnType)) return false;
            } else if (d == ".paramtable") {
                if (!params(".endparamtable", ".param", f.params)) return false;
            } else if (d == ".localtable") {
                if (f.is_native()) return fail("a native function has no .localTable");
                if (!params(".endlocaltable", ".local", f.locals)) return false;
            } else if (d == ".code") {
                if (f.is_native()) return fail("a native function has no .code");
                if (sawCode) return fail("second .code in one function");
                sawCode = true;
                if (!code(f, lines, hasLines)) return false;
            } else {
                return fail("unexpected " + m_cur.toks[0].text + " in .function");
            }
        }
        return m_status == Status::Ok ? fail("missing .endFunction") : false;
    }

    bool params(const char* endDirective, const char* entry, std::vector<Param>& out) {
        while (next()) {
            const std::string d = directive();
            if (d == endDirective) return true;
            if (d != entry) return fail("unexpected " + m_cur.toks[0].text + ", expected " + entry);
            if (!want_args(2)) return false;
            Param p;
            if (!ident(arg(0), p.name) || !ident(arg(1), p.type)) return false;
            out.push_back(p);
        }
        return m_status == Status::Ok ? fail(std::string("missing ") + endDirective) : false;
    }

    static const OpcodeInfo* find_opcode(const std::string& mnemonicLower, uint8_t& op) {
        for (unsigned i = 0; i <= kMaxOpcode; ++i) {
            const OpcodeInfo* info = opcode_info(static_cast<uint8_t>(i));
            if (lower(info->mnemonic) == mnemonicLower) {
                op = static_cast<uint8_t>(i);
                return info;
            }
        }
        return nullptr;
    }

    bool code(Function& f, std::vector<uint16_t>& lines, bool& hasLines) {
        struct Pending {
            size_t ip;
            size_t operand;
            std::string label;
            size_t line;
        };
        std::vector<Pending> pending;
        std::unordered_map<std::string, size_t> labels;
        while (next()) {
            const Tok& head = m_cur.toks[0];
            if (!head.quoted && !head.text.empty() && head.text[0] == '.') {
                if (directive() == ".endcode") {
                    for (const Pending& p : pending) {
                        auto it = labels.find(lower(p.label));
                        if (it == labels.end()) {
                            m_cur.number = p.line;
                            return fail("undefined label " + p.label);
                        }
                        const long long off = static_cast<long long>(it->second) - static_cast<long long>(p.ip);
                        f.code[p.ip].args[p.operand] = Value::integer(static_cast<int32_t>(off));
                    }
                    return true;
                }
                return fail("unexpected " + head.text + " in .code");
            }
            if (m_cur.toks.size() == 1 && !head.quoted && head.text.size() > 1 && head.text.back() == ':') {
                const std::string name = lower(head.text.substr(0, head.text.size() - 1));
                if (!labels.emplace(name, f.code.size()).second) return fail("label defined twice: " + head.text);
                continue;
            }
            if (head.quoted) return fail("expected a mnemonic, got a string");
            uint8_t op = 0;
            const OpcodeInfo* info = find_opcode(lower(head.text), op);
            if (!info) return fail("unknown mnemonic " + head.text);
            const size_t n = argc();
            if (info->varArgs ? n < info->fixedArgs : n != info->fixedArgs) {
                return fail(std::string(info->mnemonic) + " takes " + std::to_string(info->fixedArgs) +
                            (info->varArgs ? " or more" : "") + " operand(s), got " + std::to_string(n));
            }
            Instruction ins;
            ins.op = static_cast<Opcode>(op);
            ins.args.reserve(n);
            const size_t jumpOperand = ins.op == Opcode::Jmp ? 0 : (ins.op == Opcode::JmpT || ins.op == Opcode::JmpF) ? 1 : SIZE_MAX;
            for (size_t i = 0; i < n; ++i) {
                const Tok& t = arg(i);
                if (i == jumpOperand) {
                    if (t.quoted) return fail("a jump target is a label or an offset, got a string");
                    if (looks_integer(t.text)) {
                        Value v;
                        if (!typed(t, v)) return false;
                        ins.args.push_back(v);
                    } else {
                        pending.push_back({f.code.size(), i, t.text, m_cur.number});
                        ins.args.push_back(Value::integer(0));
                    }
                    continue;
                }
                Value v;
                if (!typed(t, v)) return false;
                ins.args.push_back(v);
            }
            if (m_cur.hasDebugLine) hasLines = true;
            lines.push_back(m_cur.hasDebugLine ? m_cur.debugLine : kNoLine);
            f.code.push_back(std::move(ins));
        }
        return m_status == Status::Ok ? fail("missing .endCode") : false;
    }

    // PapyrusAssembler gives an instruction without ";@line" the line of the
    // instruction before it, and the first instructions the first line found
    // (Probe01's "JUMP label6" after line 57 gets 57; a listing whose first
    // line comment is on the second instruction gives the first one that
    // line). A function with no line comment at all gets an empty record.
    static void fill_lines(std::vector<uint16_t>& lines) {
        size_t first = 0;
        while (first < lines.size() && lines[first] == kNoLine) ++first;
        if (first == lines.size()) {
            lines.clear();
            return;
        }
        for (size_t i = 0; i < first; ++i) lines[i] = lines[first];
        for (size_t i = first + 1; i < lines.size(); ++i) {
            if (lines[i] == kNoLine) lines[i] = lines[i - 1];
        }
    }

    std::string_view m_text;
    Script& m_s;
    size_t m_pos = 0;
    size_t m_lineNo = 0;
    Line m_cur;
    Status m_status = Status::Ok;
    std::string m_error;
    std::unordered_map<std::string, StrIdx> m_interned;
    std::map<std::tuple<StrIdx, StrIdx, StrIdx, int>, size_t> m_records;
};

}  // namespace

Status assemble(std::string_view listing, Script& out, std::string* error) noexcept {
    try {
        Assembler a(listing, out);
        const Status st = a.run();
        if (st != Status::Ok && error) *error = a.error_text();
        return st;
    } catch (...) {
        // Only allocation failure can get here.
        if (error) *error = "out of memory";
        return Status::TooLarge;
    }
}

}  // namespace papyrus::pex
