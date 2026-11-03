// The .pex writer: the exact inverse of the reader. Object sizes are
// back-patched after the object's content is written.
// SPDX-License-Identifier: GPL-3.0-only
#include "papyrus/pex.hpp"

#include "cursor.hpp"

#include <cstdio>
#include <fstream>

namespace papyrus::pex {

namespace {

using detail::Writer;

class Serializer {
public:
    Serializer(const Script& s, std::vector<uint8_t>& out) noexcept : m_s(s), m_w(out) {}

    const std::string& error() const noexcept { return m_error; }

    bool fail(const char* what) {
        if (m_error.empty()) m_error = what;
        return false;
    }

    bool count16(size_t n, const char* what) {
        if (n > 0xFFFF) {
            char buf[128];
            std::snprintf(buf, sizeof buf, "%s has %zu entries, the limit is 65535", what, n);
            return fail(buf);
        }
        m_w.u16(static_cast<uint16_t>(n));
        return true;
    }

    bool string(const std::string& s, const char* what) {
        if (s.size() > 0xFFFF) {
            char buf[128];
            std::snprintf(buf, sizeof buf, "%s is %zu bytes long, the limit is 65535", what, s.size());
            return fail(buf);
        }
        m_w.string(s);
        return true;
    }

    bool str_idx(StrIdx i, const char* what) {
        if (i >= m_s.strings.size()) {
            char buf[128];
            std::snprintf(buf, sizeof buf, "%s refers to string %u but the table has %zu", what, unsigned(i),
                          m_s.strings.size());
            return fail(buf);
        }
        m_w.u16(i);
        return true;
    }

    bool value(const Value& v) {
        m_w.u8(static_cast<uint8_t>(v.type));
        switch (v.type) {
        case ValueType::None: return true;
        case ValueType::Identifier:
        case ValueType::String: return str_idx(v.str(), "value");
        case ValueType::Integer:
        case ValueType::Float: m_w.u32(v.bits); return true;
        case ValueType::Bool: m_w.u8(static_cast<uint8_t>(v.bits)); return true;
        }
        return fail("value has an unknown type");
    }

    bool header() {
        const Header& h = m_s.header;
        m_w.u32(0xFA57C0DE);
        m_w.u8(h.major);
        m_w.u8(h.minor);
        m_w.u16(h.gameId);
        m_w.u64(h.compileTime);
        return string(h.sourceName, "source name") && string(h.userName, "user name") &&
               string(h.computerName, "computer name");
    }

    bool string_table() {
        if (!count16(m_s.strings.size(), "string table")) return false;
        for (const std::string& s : m_s.strings) {
            if (!string(s, "string table entry")) return false;
        }
        return true;
    }

    bool debug_info() {
        m_w.u8(m_s.debug.present ? 1 : 0);
        if (!m_s.debug.present) return true;
        m_w.u64(m_s.debug.modificationTime);
        if (!count16(m_s.debug.functions.size(), "debug function table")) return false;
        for (const FunctionDebug& f : m_s.debug.functions) {
            if (!str_idx(f.object, "debug function object") || !str_idx(f.state, "debug function state") ||
                !str_idx(f.name, "debug function name"))
                return false;
            m_w.u8(static_cast<uint8_t>(f.kind));
            if (!count16(f.lines.size(), "debug line table")) return false;
            for (uint16_t line : f.lines) m_w.u16(line);
        }
        return true;
    }

    bool user_flags() {
        if (!count16(m_s.userFlags.size(), "user flag table")) return false;
        for (const UserFlag& f : m_s.userFlags) {
            if (!str_idx(f.name, "user flag name")) return false;
            m_w.u8(f.bit);
        }
        return true;
    }

    bool function_body(const Function& f) {
        if (!str_idx(f.returnType, "function return type") || !str_idx(f.docString, "function doc string"))
            return false;
        m_w.u32(f.userFlags);
        m_w.u8(f.flags);
        if (!count16(f.params.size(), "parameter table")) return false;
        for (const Param& p : f.params) {
            if (!str_idx(p.name, "parameter name") || !str_idx(p.type, "parameter type")) return false;
        }
        if (!count16(f.locals.size(), "local table")) return false;
        for (const Param& p : f.locals) {
            if (!str_idx(p.name, "local name") || !str_idx(p.type, "local type")) return false;
        }
        if (!count16(f.code.size(), "instruction stream")) return false;
        for (const Instruction& ins : f.code) {
            if (!instruction(ins)) return false;
        }
        return true;
    }

    bool instruction(const Instruction& ins) {
        const OpcodeInfo* info = opcode_info(static_cast<uint8_t>(ins.op));
        if (!info) return fail("instruction has an opcode outside 0x00..0x23");
        if (ins.args.size() < info->fixedArgs) return fail("instruction has fewer operands than its opcode needs");
        if (!info->varArgs && ins.args.size() != info->fixedArgs) {
            return fail("instruction has more operands than its opcode takes");
        }
        m_w.u8(static_cast<uint8_t>(ins.op));
        for (uint8_t a = 0; a < info->fixedArgs; ++a) {
            if (!value(ins.args[a])) return false;
        }
        if (info->varArgs) {
            const size_t n = ins.args.size() - info->fixedArgs;
            if (n > 0x7FFFFFFF) return fail("call has too many arguments");
            if (!value(Value::integer(static_cast<int32_t>(n)))) return false;
            for (size_t i = info->fixedArgs; i < ins.args.size(); ++i) {
                if (!value(ins.args[i])) return false;
            }
        }
        return true;
    }

    bool object(const Object& o) {
        if (!str_idx(o.name, "object name")) return false;
        const size_t sizeAt = m_w.size();  // the size counts from here
        m_w.u32(0);
        if (!str_idx(o.parent, "object parent") || !str_idx(o.docString, "object doc string")) return false;
        m_w.u32(o.userFlags);
        if (!str_idx(o.autoState, "object auto state")) return false;

        if (!count16(o.variables.size(), "variable table")) return false;
        for (const Variable& v : o.variables) {
            if (!str_idx(v.name, "variable name") || !str_idx(v.type, "variable type")) return false;
            m_w.u32(v.userFlags);
            if (!value(v.initial)) return false;
        }

        if (!count16(o.properties.size(), "property table")) return false;
        for (const Property& p : o.properties) {
            if (!str_idx(p.name, "property name") || !str_idx(p.type, "property type") ||
                !str_idx(p.docString, "property doc string"))
                return false;
            m_w.u32(p.userFlags);
            m_w.u8(p.flags);
            if (p.is_auto()) {
                if (!str_idx(p.autoVar, "property auto variable")) return false;
            } else {
                if (p.readable()) {
                    if (!p.getter) return fail("readable property has no getter body");
                    if (!function_body(*p.getter)) return false;
                }
                if (p.writable()) {
                    if (!p.setter) return fail("writable property has no setter body");
                    if (!function_body(*p.setter)) return false;
                }
            }
        }

        if (!count16(o.states.size(), "state table")) return false;
        for (const State& s : o.states) {
            if (!str_idx(s.name, "state name")) return false;
            if (!count16(s.functions.size(), "function table")) return false;
            for (const NamedFunction& f : s.functions) {
                if (!str_idx(f.name, "function name")) return false;
                if (!function_body(f.body)) return false;
            }
        }

        const size_t size = m_w.size() - sizeAt;
        if (size > 0xFFFFFFFFu) return fail("object is larger than 4 GiB");
        m_w.patch_u32(sizeAt, o.canonicalSize ? static_cast<uint32_t>(size) : o.declaredSize);
        return true;
    }

    bool objects() {
        if (!count16(m_s.objects.size(), "object table")) return false;
        for (const Object& o : m_s.objects) {
            if (!object(o)) return false;
        }
        return true;
    }

private:
    const Script& m_s;
    Writer m_w;
    std::string m_error;
};

}  // namespace

Status write(const Script& script, std::vector<uint8_t>& out, std::string* error) noexcept {
    out.clear();
    if (error) error->clear();
    Serializer s(script, out);
    const bool ok = s.header() && s.string_table() && s.debug_info() && s.user_flags() && s.objects();
    if (!ok) {
        if (error) *error = s.error();
        out.clear();
        return Status::BadModel;
    }
    out.insert(out.end(), script.trailing.begin(), script.trailing.end());
    return Status::Ok;
}

Status write_file(const Script& script, const std::filesystem::path& path, std::string* error) noexcept {
    std::vector<uint8_t> bytes;
    const Status st = write(script, bytes, error);
    if (st != Status::Ok) return st;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "cannot create " + path.string();
        return Status::IoError;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        if (error) *error = "cannot write " + path.string();
        return Status::IoError;
    }
    return Status::Ok;
}

}  // namespace papyrus::pex
