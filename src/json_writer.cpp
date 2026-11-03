// JSON view of the model. Strings are resolved to text; bytes above 0x7F are
// escaped as \u00XX (the compiler stores the source's single-byte text as
// is), so the output is always valid JSON and loses nothing.
// SPDX-License-Identifier: GPL-3.0-only
#include "papyrus/pex.hpp"

#include <cmath>
#include <cstdio>

namespace papyrus::pex {

namespace {

class Json {
public:
    Json(const Script& s, bool pretty) : m_s(s), m_pretty(pretty) {}

    std::string run() {
        obj_begin();
        key("header");
        header();
        key("strings");
        arr_begin();
        for (const std::string& s : m_s.strings) {
            sep();
            quote(s);
        }
        arr_end();
        key("debug");
        debug();
        key("userFlags");
        arr_begin();
        for (const UserFlag& f : m_s.userFlags) {
            sep();
            obj_begin();
            key("name");
            quote(m_s.str(f.name));
            key("bit");
            num(f.bit);
            obj_end();
        }
        arr_end();
        key("objects");
        arr_begin();
        for (const Object& o : m_s.objects) {
            sep();
            object(o);
        }
        arr_end();
        key("notes");
        arr_begin();
        for (const std::string& n : m_s.notes) {
            sep();
            quote(n);
        }
        arr_end();
        obj_end();
        m_out += '\n';
        return std::move(m_out);
    }

private:
    // --- layout -------------------------------------------------------
    void nl() {
        if (!m_pretty) return;
        m_out += '\n';
        m_out.append(size_t(m_depth) * 2, ' ');
    }
    void sep() {
        if (m_first.back()) {
            m_first.back() = false;
        } else {
            m_out += ',';
        }
        nl();
    }
    void obj_begin() {
        m_out += '{';
        ++m_depth;
        m_first.push_back(true);
    }
    void obj_end() {
        --m_depth;
        if (!m_first.back()) nl();
        m_first.pop_back();
        m_out += '}';
    }
    void arr_begin() {
        m_out += '[';
        ++m_depth;
        m_first.push_back(true);
    }
    void arr_end() {
        --m_depth;
        if (!m_first.back()) nl();
        m_first.pop_back();
        m_out += ']';
    }
    void key(const char* k) {
        sep();
        quote(k);
        m_out += m_pretty ? ": " : ":";
    }

    // --- scalars ------------------------------------------------------
    void quote(std::string_view s) {
        m_out += '"';
        for (unsigned char c : s) {
            switch (c) {
            case '"': m_out += "\\\""; break;
            case '\\': m_out += "\\\\"; break;
            case '\n': m_out += "\\n"; break;
            case '\r': m_out += "\\r"; break;
            case '\t': m_out += "\\t"; break;
            default:
                if (c < 0x20 || c >= 0x7F) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04X", unsigned(c));
                    m_out += buf;
                } else {
                    m_out += static_cast<char>(c);
                }
            }
        }
        m_out += '"';
    }
    void num(uint64_t v) { m_out += std::to_string(v); }
    void num(int64_t v) { m_out += std::to_string(v); }
    void num(uint32_t v) { m_out += std::to_string(v); }
    void num(int32_t v) { m_out += std::to_string(v); }
    void num(uint16_t v) { m_out += std::to_string(v); }
    void num(uint8_t v) { m_out += std::to_string(unsigned(v)); }
    void boolean(bool b) { m_out += b ? "true" : "false"; }
    void flt(float f) {
        if (std::isfinite(f)) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.9g", double(f));
            m_out += buf;
        } else {
            quote(std::isnan(f) ? "nan" : (f > 0 ? "inf" : "-inf"));
        }
    }
    void value(const Value& v) {
        switch (v.type) {
        case ValueType::None: m_out += "null"; break;
        case ValueType::Identifier:
            obj_begin();
            key("id");
            quote(m_s.str(v.str()));
            obj_end();
            break;
        case ValueType::String:
            obj_begin();
            key("str");
            quote(m_s.str(v.str()));
            obj_end();
            break;
        case ValueType::Integer: num(v.as_int()); break;
        case ValueType::Float: flt(v.as_float()); break;
        case ValueType::Bool: boolean(v.as_bool()); break;
        }
    }
    void str(const char* k, StrIdx i) {
        key(k);
        quote(m_s.str(i));
    }

    // --- sections -----------------------------------------------------
    void header() {
        const Header& h = m_s.header;
        obj_begin();
        key("version");
        char buf[16];
        std::snprintf(buf, sizeof buf, "%u.%u", unsigned(h.major), unsigned(h.minor));
        quote(buf);
        key("gameId");
        num(h.gameId);
        key("compileTime");
        num(h.compileTime);
        key("source");
        quote(h.sourceName);
        key("user");
        quote(h.userName);
        key("computer");
        quote(h.computerName);
        obj_end();
    }

    void debug() {
        obj_begin();
        key("present");
        boolean(m_s.debug.present);
        if (m_s.debug.present) {
            key("modificationTime");
            num(m_s.debug.modificationTime);
            key("functions");
            arr_begin();
            for (const FunctionDebug& f : m_s.debug.functions) {
                sep();
                obj_begin();
                str("object", f.object);
                str("state", f.state);
                str("name", f.name);
                key("kind");
                quote(f.kind == FunctionKind::Method ? "method" : f.kind == FunctionKind::Getter ? "getter" : "setter");
                key("lines");
                m_out += '[';
                for (size_t i = 0; i < f.lines.size(); ++i) {
                    if (i) m_out += ',';
                    m_out += std::to_string(f.lines[i]);
                }
                m_out += ']';
                obj_end();
            }
            arr_end();
        }
        obj_end();
    }

    void params(const char* k, const std::vector<Param>& ps) {
        key(k);
        arr_begin();
        for (const Param& p : ps) {
            sep();
            obj_begin();
            str("name", p.name);
            str("type", p.type);
            obj_end();
        }
        arr_end();
    }

    void function(const Function& f) {
        obj_begin();
        str("returnType", f.returnType);
        str("doc", f.docString);
        key("userFlags");
        num(f.userFlags);
        key("global");
        boolean(f.is_global());
        key("native");
        boolean(f.is_native());
        params("params", f.params);
        params("locals", f.locals);
        key("code");
        arr_begin();
        for (size_t ip = 0; ip < f.code.size(); ++ip) {
            const Instruction& ins = f.code[ip];
            sep();
            obj_begin();
            key("ip");
            num(static_cast<uint32_t>(ip));
            key("op");
            const OpcodeInfo* info = opcode_info(static_cast<uint8_t>(ins.op));
            quote(info ? info->mnemonic : "?");
            key("args");
            m_out += '[';
            for (size_t a = 0; a < ins.args.size(); ++a) {
                if (a) m_out += m_pretty ? ", " : ",";
                value(ins.args[a]);
            }
            m_out += ']';
            obj_end();
        }
        arr_end();
        obj_end();
    }

    void object(const Object& o) {
        obj_begin();
        str("name", o.name);
        str("parent", o.parent);
        str("doc", o.docString);
        key("userFlags");
        num(o.userFlags);
        str("autoState", o.autoState);
        key("variables");
        arr_begin();
        for (const Variable& v : o.variables) {
            sep();
            obj_begin();
            str("name", v.name);
            str("type", v.type);
            key("userFlags");
            num(v.userFlags);
            key("initial");
            value(v.initial);
            obj_end();
        }
        arr_end();
        key("properties");
        arr_begin();
        for (const Property& p : o.properties) {
            sep();
            obj_begin();
            str("name", p.name);
            str("type", p.type);
            str("doc", p.docString);
            key("userFlags");
            num(p.userFlags);
            key("read");
            boolean(p.readable());
            key("write");
            boolean(p.writable());
            if (p.is_auto()) {
                str("autoVar", p.autoVar);
            }
            if (p.getter) {
                key("getter");
                function(*p.getter);
            }
            if (p.setter) {
                key("setter");
                function(*p.setter);
            }
            obj_end();
        }
        arr_end();
        key("states");
        arr_begin();
        for (const State& s : o.states) {
            sep();
            obj_begin();
            str("name", s.name);
            key("functions");
            arr_begin();
            for (const NamedFunction& f : s.functions) {
                sep();
                obj_begin();
                str("name", f.name);
                key("body");
                function(f.body);
                obj_end();
            }
            arr_end();
            obj_end();
        }
        arr_end();
        obj_end();
    }

    const Script& m_s;
    bool m_pretty;
    std::string m_out;
    int m_depth = 0;
    std::vector<bool> m_first;
};

}  // namespace

std::string to_json(const Script& script, bool pretty) {
    return Json(script, pretty).run();
}

}  // namespace papyrus::pex
