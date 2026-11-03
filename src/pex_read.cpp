// The .pex reader. Layout (big-endian, Skyrim):
//   header, string table, debug info, user flags, objects.
// Every count is a u16 except vararg counts (an Integer Value). Every
// structure is validated as it is read: string indices against the table,
// opcodes against the Skyrim set, vararg counts against the remaining bytes,
// and each object's declared size against its content.
// SPDX-License-Identifier: GPL-3.0-only
#include "papyrus/pex.hpp"

#include "cursor.hpp"

#include <cstdio>
#include <fstream>

namespace papyrus::pex {

namespace {

constexpr uint32_t kMagicSkyrim = 0xFA57C0DE;  // bytes FA 57 C0 DE on disk
constexpr uint32_t kMagicLittle = 0xDEC057FA;  // the same magic written little-endian

using detail::Cursor;

class Reader {
public:
    Reader(Cursor& c, Script& s) noexcept : m_c(c), m_s(s) {}

    bool str_idx(StrIdx& out, const char* what) {
        uint16_t v;
        if (!m_c.u16(v, what)) return false;
        if (v >= m_s.strings.size()) {
            char buf[128];
            std::snprintf(buf, sizeof buf, "%s refers to string %u but the table has %zu", what, unsigned(v),
                          m_s.strings.size());
            return m_c.fail(buf);
        }
        out = v;
        return true;
    }

    bool value(Value& v, const char* what) {
        uint8_t t;
        if (!m_c.u8(t, what)) return false;
        switch (static_cast<ValueType>(t)) {
        case ValueType::None: v = Value::none(); return true;
        case ValueType::Identifier:
        case ValueType::String: {
            StrIdx s;
            if (!str_idx(s, what)) return false;
            v = {static_cast<ValueType>(t), s};
            return true;
        }
        case ValueType::Integer:
        case ValueType::Float: {
            uint32_t b;
            if (!m_c.u32(b, what)) return false;
            v = {static_cast<ValueType>(t), b};
            return true;
        }
        case ValueType::Bool: {
            uint8_t b;
            if (!m_c.u8(b, what)) return false;
            v = {ValueType::Bool, b};
            return true;
        }
        default: {
            char buf[96];
            std::snprintf(buf, sizeof buf, "%s has value type %u (valid: 0..5)", what, unsigned(t));
            return m_c.fail(buf);
        }
        }
    }

    bool header() {
        Header& h = m_s.header;
        return m_c.u8(h.major, "header") && m_c.u8(h.minor, "header") && m_c.u16(h.gameId, "header") &&
               m_c.u64(h.compileTime, "header") && m_c.string(h.sourceName, "header source name") &&
               m_c.string(h.userName, "header user name") && m_c.string(h.computerName, "header computer name");
    }

    bool string_table() {
        uint16_t n;
        if (!m_c.u16(n, "string table")) return false;
        // Each entry costs at least its 2-byte length.
        if (!m_c.need(size_t(n) * 2, "string table")) return false;
        m_s.strings.resize(n);
        for (uint16_t i = 0; i < n; ++i) {
            if (!m_c.string(m_s.strings[i], "string table entry")) return false;
        }
        return true;
    }

    bool debug_info() {
        uint8_t has;
        if (!m_c.u8(has, "debug info flag")) return false;
        m_s.debug.present = has != 0;
        if (!m_s.debug.present) return true;
        if (!m_c.u64(m_s.debug.modificationTime, "debug info")) return false;
        uint16_t n;
        if (!m_c.u16(n, "debug function count")) return false;
        if (!m_c.need(size_t(n) * 9, "debug function table")) return false;
        m_s.debug.functions.resize(n);
        for (FunctionDebug& f : m_s.debug.functions) {
            uint8_t kind;
            if (!str_idx(f.object, "debug function object") || !str_idx(f.state, "debug function state") ||
                !str_idx(f.name, "debug function name") || !m_c.u8(kind, "debug function type"))
                return false;
            if (kind > 2) return m_c.fail("debug function type is not 0, 1 or 2");
            f.kind = static_cast<FunctionKind>(kind);
            uint16_t count;
            if (!m_c.u16(count, "debug line count")) return false;
            if (!m_c.need(size_t(count) * 2, "debug line numbers")) return false;
            f.lines.resize(count);
            for (uint16_t& line : f.lines) {
                if (!m_c.u16(line, "debug line number")) return false;
            }
        }
        return true;
    }

    bool user_flags() {
        uint16_t n;
        if (!m_c.u16(n, "user flag count")) return false;
        if (!m_c.need(size_t(n) * 3, "user flag table")) return false;
        m_s.userFlags.resize(n);
        for (UserFlag& f : m_s.userFlags) {
            if (!str_idx(f.name, "user flag name") || !m_c.u8(f.bit, "user flag bit")) return false;
            if (f.bit > 31) return m_c.fail("user flag bit index above 31");
        }
        return true;
    }

    bool function_body(Function& f, const char* what) {
        if (!str_idx(f.returnType, "function return type") || !str_idx(f.docString, "function doc string") ||
            !m_c.u32(f.userFlags, "function user flags") || !m_c.u8(f.flags, "function flags"))
            return false;
        uint16_t n;
        if (!m_c.u16(n, "parameter count")) return false;
        if (!m_c.need(size_t(n) * 4, "parameter table")) return false;
        f.params.resize(n);
        for (Param& p : f.params) {
            if (!str_idx(p.name, "parameter name") || !str_idx(p.type, "parameter type")) return false;
        }
        if (!m_c.u16(n, "local count")) return false;
        if (!m_c.need(size_t(n) * 4, "local table")) return false;
        f.locals.resize(n);
        for (Param& p : f.locals) {
            if (!str_idx(p.name, "local name") || !str_idx(p.type, "local type")) return false;
        }
        if (!m_c.u16(n, "instruction count")) return false;
        if (!m_c.need(size_t(n), "instruction stream")) return false;
        f.code.resize(n);
        for (uint16_t i = 0; i < n; ++i) {
            if (!instruction(f.code[i], what, i)) return false;
        }
        return true;
    }

    bool instruction(Instruction& ins, const char* what, unsigned index) {
        uint8_t op;
        if (!m_c.u8(op, "opcode")) return false;
        const OpcodeInfo* info = opcode_info(op);
        if (!info) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "%s: instruction %u has opcode 0x%02X (Skyrim uses 0x00..0x23)", what,
                          index, unsigned(op));
            return m_c.fail(buf);
        }
        ins.op = static_cast<Opcode>(op);
        ins.args.resize(info->fixedArgs);
        for (uint8_t a = 0; a < info->fixedArgs; ++a) {
            if (!value(ins.args[a], "operand")) return false;
        }
        if (info->varArgs) {
            Value count;
            if (!value(count, "argument count")) return false;
            if (count.type != ValueType::Integer) return m_c.fail("call argument count is not an integer value");
            const int32_t n = count.as_int();
            // Each argument is at least one byte.
            if (n < 0 || size_t(n) > m_c.remaining()) return m_c.fail("call argument count exceeds the file");
            ins.args.reserve(ins.args.size() + size_t(n));
            for (int32_t i = 0; i < n; ++i) {
                Value v;
                if (!value(v, "call argument")) return false;
                ins.args.push_back(v);
            }
        }
        return true;
    }

    bool object(Object& o, size_t index) {
        if (!str_idx(o.name, "object name")) return false;
        uint32_t declared;
        if (!m_c.u32(declared, "object size")) return false;
        const size_t start = m_c.pos();  // first byte after the size field
        if (!str_idx(o.parent, "object parent") || !str_idx(o.docString, "object doc string") ||
            !m_c.u32(o.userFlags, "object user flags") || !str_idx(o.autoState, "object auto state"))
            return false;

        uint16_t n;
        if (!m_c.u16(n, "variable count")) return false;
        if (!m_c.need(size_t(n) * 9, "variable table")) return false;
        o.variables.resize(n);
        for (Variable& v : o.variables) {
            if (!str_idx(v.name, "variable name") || !str_idx(v.type, "variable type") ||
                !m_c.u32(v.userFlags, "variable user flags") || !value(v.initial, "variable initial value"))
                return false;
        }

        if (!m_c.u16(n, "property count")) return false;
        if (!m_c.need(size_t(n) * 11, "property table")) return false;
        o.properties.resize(n);
        for (Property& p : o.properties) {
            if (!str_idx(p.name, "property name") || !str_idx(p.type, "property type") ||
                !str_idx(p.docString, "property doc string") || !m_c.u32(p.userFlags, "property user flags") ||
                !m_c.u8(p.flags, "property flags"))
                return false;
            if (p.flags & ~uint8_t(kPropRead | kPropWrite | kPropAutoVar)) {
                return m_c.fail("property flags use bits above 2");
            }
            if (p.is_auto()) {
                if (!str_idx(p.autoVar, "property auto variable")) return false;
            } else {
                if (p.readable()) {
                    p.getter.emplace();
                    if (!function_body(*p.getter, "property getter")) return false;
                }
                if (p.writable()) {
                    p.setter.emplace();
                    if (!function_body(*p.setter, "property setter")) return false;
                }
            }
        }

        if (!m_c.u16(n, "state count")) return false;
        if (!m_c.need(size_t(n) * 4, "state table")) return false;
        o.states.resize(n);
        for (State& s : o.states) {
            if (!str_idx(s.name, "state name")) return false;
            uint16_t fn;
            if (!m_c.u16(fn, "function count")) return false;
            if (!m_c.need(size_t(fn) * 17, "function table")) return false;
            s.functions.resize(fn);
            for (NamedFunction& f : s.functions) {
                if (!str_idx(f.name, "function name")) return false;
                if (!function_body(f.body, "function")) return false;
            }
        }

        // Bethesda's compiler counts from the size field itself (4 bytes) to
        // the end of the object; confirmed on all 14,026 vanilla scripts.
        // Other compilers differ (one writes the count little-endian and
        // without the field); the engine ignores the field either way.
        const size_t actual = m_c.pos() - start + 4;
        o.declaredSize = declared;
        o.canonicalSize = actual == declared;
        if (!o.canonicalSize) {
            const uint32_t swapped = ((declared & 0xFFu) << 24) | ((declared & 0xFF00u) << 8) |
                                     ((declared >> 8) & 0xFF00u) | (declared >> 24);
            char buf[200];
            if (swapped == actual - 4) {
                std::snprintf(buf, sizeof buf,
                              "object %zu: size written little-endian without the size field (%u; Bethesda "
                              "would write %zu); kept as is",
                              index, unsigned(swapped), actual);
            } else {
                std::snprintf(buf, sizeof buf, "object %zu: size field says %u, content is %zu bytes; kept as is",
                              index, unsigned(declared), actual);
            }
            m_s.notes.emplace_back(buf);
        }
        return true;
    }

    bool objects() {
        uint16_t n;
        if (!m_c.u16(n, "object count")) return false;
        if (!m_c.need(size_t(n) * 20, "object table")) return false;
        m_s.objects.resize(n);
        for (size_t i = 0; i < m_s.objects.size(); ++i) {
            if (!object(m_s.objects[i], i)) return false;
        }
        return true;
    }

    // Bytes after the last object: kept so a rewrite stays exact.
    void trailing(const uint8_t* data) {
        const size_t n = m_c.remaining();
        if (n == 0) return;
        m_s.trailing.assign(data + m_c.pos(), data + m_c.pos() + n);
        char buf[96];
        std::snprintf(buf, sizeof buf, "%zu trailing byte%s after the last object; kept as is", n, n == 1 ? "" : "s");
        m_s.notes.emplace_back(buf);
    }

private:
    Cursor& m_c;
    Script& m_s;
};

}  // namespace

Status read(const uint8_t* data, size_t size, Script& out, std::string* error) noexcept {
    out = Script{};
    if (error) error->clear();
    Cursor c(data, size);
    uint32_t magic;
    if (!c.u32(magic, "magic")) {
        if (error) *error = "file is shorter than the 4-byte magic";
        return Status::NotPex;
    }
    if (magic == kMagicLittle) {
        if (error) *error = "little-endian Papyrus file (Fallout 4 / 76 / Starfield), not a Skyrim script";
        return Status::NotSkyrim;
    }
    if (magic != kMagicSkyrim) {
        if (error) *error = "file does not start with the Papyrus magic FA 57 C0 DE";
        return Status::NotPex;
    }

    Reader r(c, out);
    const bool ok = r.header() && r.string_table() && r.debug_info() && r.user_flags() && r.objects();
    if (!ok) {
        if (error) *error = c.error();
        return c.truncated() ? Status::Truncated : Status::Corrupt;
    }
    r.trailing(data);
    return Status::Ok;
}

Status read_file(const std::filesystem::path& path, Script& out, std::string* error) noexcept {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open " + path.string();
        return Status::IoError;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        if (error) *error = "cannot read " + path.string();
        return Status::IoError;
    }
    return read(bytes.data(), bytes.size(), out, error);
}

}  // namespace papyrus::pex
