// Papyrus assembly view of the model, in the form PapyrusAssembler reads
// (docs/ASM_SYNTAX.md). Labels are generated for jump targets; debug line
// numbers become ";@line N" comments, which the assembler turns back into
// debug info.
// SPDX-License-Identifier: GPL-3.0-only
#include "papyrus/layout.hpp"
#include "papyrus/pex.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace papyrus::pex {

namespace {

class Asm {
public:
    explicit Asm(const Script& s) : m_s(s) {}
    static std::string quoted_text(std::string_view s) { return quoted(s); }
    static std::string float_literal(float f) { return float_text(f); }

    std::string run() {
        const Header& h = m_s.header;
        line(0, ".info");
        line(1, ".source " + quoted(h.sourceName));
        line(1, ".modifyTime " + std::to_string(m_s.debug.present ? m_s.debug.modificationTime : 0));
        line(1, ".compileTime " + std::to_string(h.compileTime));
        line(1, ".user " + quoted(h.userName));
        line(1, ".computer " + quoted(h.computerName));
        line(0, ".endInfo");
        line(0, ".userFlagsRef");
        for (const UserFlag& f : m_s.userFlags) {
            line(1, ".flag " + str(f.name) + " " + std::to_string(f.bit));
        }
        line(0, ".endUserFlagsRef");
        line(0, ".objectTable");
        for (const Object& o : m_s.objects) object(o);
        line(0, ".endObjectTable");
        return std::move(m_out);
    }

private:
    void line(int depth, const std::string& text) {
        m_out.append(size_t(depth) * 2, ' ');
        m_out += text;
        m_out += '\n';
    }
    std::string str(StrIdx i) const { return std::string(m_s.str(i)); }

    static std::string quoted(std::string_view s) {
        std::string out = "\"";
        for (char c : s) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
            }
        }
        out += '"';
        return out;
    }

    // Shortest plain decimal (no exponent: the assembler does not read
    // them) that parses back to the same single-precision value.
    static std::string float_text(float f) {
        if (std::isnan(f)) return "nan";
        if (std::isinf(f)) return f > 0 ? "inf" : "-inf";
        char buf[64];
        for (int prec = 1; prec <= 9; ++prec) {
            std::snprintf(buf, sizeof buf, "%.*g", prec, double(f));
            if (std::strtof(buf, nullptr) == f) break;
        }
        std::string s = buf;
        if (s.find_first_of("eE") != std::string::npos) {
            for (int decimals = 0; decimals <= 60; ++decimals) {
                std::snprintf(buf, sizeof buf, "%.*f", decimals, double(f));
                if (std::strtof(buf, nullptr) == f) break;
            }
            s = buf;
        }
        if (s.find('.') == std::string::npos) s += ".0";
        return s;
    }

    std::string value(const Value& v) const {
        switch (v.type) {
        case ValueType::None: return "None";
        case ValueType::Identifier: return str(v.str());
        case ValueType::String: return quoted(m_s.str(v.str()));
        case ValueType::Integer: return std::to_string(v.as_int());
        case ValueType::Float: return float_text(v.as_float());
        case ValueType::Bool: return v.as_bool() ? "True" : "False";
        }
        return "?";
    }

    // The debug lines to emit for a function. Bethesda's assembler keys
    // debug records by object, name and kind only: when a function exists in
    // the default state and again in another state, one record survives,
    // carrying the first function's state and the last one's lines. To make
    // the assembler recreate exactly the records the original has, every
    // such function must carry lines: the real ones when a record of the
    // right length exists, placeholders (0) otherwise. Functions with no
    // record at all (the compiler-generated GetState/GotoState) get none.
    struct Lines {
        const FunctionDebug* rec = nullptr;  // real lines
        bool placeholder = false;            // emit ";@line 0" for every instruction
    };
    Lines lines_for(StrIdx object, StrIdx state, StrIdx name, FunctionKind kind, const Function& f) const {
        Lines out;
        if (!m_s.debug.present) return out;
        const FunctionDebug* exact = m_s.debug_for(object, state, name, kind);
        if (exact && exact->lines.size() == f.code.size()) {
            out.rec = exact;
            return out;
        }
        bool sameName = exact != nullptr;
        for (const FunctionDebug& d : m_s.debug.functions) {
            if (d.object == object && d.name == name && d.kind == kind) {
                sameName = true;
                if (d.lines.size() == f.code.size()) {
                    out.rec = &d;
                    return out;
                }
            }
        }
        out.placeholder = sameName;
        return out;
    }

    void object(const Object& o) {
        line(1, ".object " + str(o.name) + (str(o.parent).empty() ? "" : " " + str(o.parent)));
        line(2, ".userFlags " + std::to_string(o.userFlags));
        line(2, ".docString " + quoted(m_s.str(o.docString)));
        line(2, ".autoState " + str(o.autoState));
        line(2, ".variableTable");
        for (const Variable& v : o.variables) {
            line(3, ".variable " + str(v.name) + " " + str(v.type));
            line(4, ".userFlags " + std::to_string(v.userFlags));
            line(4, ".initialValue " + value(v.initial));
            line(3, ".endVariable");
        }
        line(2, ".endVariableTable");
        line(2, ".propertyTable");
        for (const Property& p : o.properties) {  // order has no effect on the result
            line(3, ".property " + str(p.name) + " " + str(p.type) + (p.is_auto() ? " auto" : ""));
            line(4, ".userFlags " + std::to_string(p.userFlags));
            line(4, ".docString " + quoted(m_s.str(p.docString)));
            if (p.is_auto()) {
                line(4, ".autoVar " + str(p.autoVar));
            } else {
                if (p.getter) function("get", *p.getter, 4, lines_for(o.name, 0, p.name, FunctionKind::Getter, *p.getter));
                if (p.setter) function("set", *p.setter, 4, lines_for(o.name, 0, p.name, FunctionKind::Setter, *p.setter));
            }
            line(3, ".endProperty");
        }
        line(2, ".endPropertyTable");
        // States and functions in source order (recovered from the debug
        // records), so the assembler meets them in the sequence the compiler
        // did and recreates the same debug records.
        const SourceLayout layout = source_layout(m_s, o);
        line(2, ".stateTable");
        for (size_t si : layout.states) {
            const State& s = o.states[si];
            line(3, ".state" + (str(s.name).empty() ? "" : " " + str(s.name)));
            for (size_t fi : layout.functions[si]) {
                const NamedFunction& f = s.functions[fi];
                function(str(f.name), f.body, 4, lines_for(o.name, s.name, f.name, FunctionKind::Method, f.body));
            }
            line(3, ".endState");
        }
        line(2, ".endStateTable");
        line(1, ".endObject");
    }

    void function(const std::string& name, const Function& f, int depth, const Lines& dbg) {
        std::string head = ".function " + name;
        if (f.is_global()) head += " static";
        if (f.is_native()) head += " native";
        line(depth, head);
        line(depth + 1, ".userFlags " + std::to_string(f.userFlags));
        line(depth + 1, ".docString " + quoted(m_s.str(f.docString)));
        line(depth + 1, ".return " + str(f.returnType));
        line(depth + 1, ".paramTable");
        for (const Param& p : f.params) line(depth + 2, ".param " + str(p.name) + " " + str(p.type));
        line(depth + 1, ".endParamTable");
        if (!f.is_native()) {
            line(depth + 1, ".localTable");
            for (const Param& p : f.locals) line(depth + 2, ".local " + str(p.name) + " " + str(p.type));
            line(depth + 1, ".endLocalTable");
            line(depth + 1, ".code");
            code(f, depth + 2, dbg);
            line(depth + 1, ".endCode");
        }
        line(depth, ".endFunction");
    }

    void code(const Function& f, int depth, const Lines& dbg) {
        // Label every jump target, numbered in order of position.
        std::map<size_t, int> labels;
        for (size_t ip = 0; ip < f.code.size(); ++ip) {
            const Instruction& ins = f.code[ip];
            if (ins.op == Opcode::Jmp || ins.op == Opcode::JmpT || ins.op == Opcode::JmpF) {
                const Value& off = ins.args.back();
                if (off.type == ValueType::Integer) {
                    const long long target = static_cast<long long>(ip) + off.as_int();
                    if (target >= 0 && target <= static_cast<long long>(f.code.size())) {
                        labels.emplace(static_cast<size_t>(target), 0);
                    }
                }
            }
        }
        int n = 0;
        for (auto& [ip, id] : labels) id = n++;

        const bool haveLines = dbg.rec != nullptr || dbg.placeholder;
        for (size_t ip = 0; ip <= f.code.size(); ++ip) {
            auto it = labels.find(ip);
            if (it != labels.end()) line(depth, "label" + std::to_string(it->second) + ":");
            if (ip == f.code.size()) break;
            const Instruction& ins = f.code[ip];
            const OpcodeInfo* info = opcode_info(static_cast<uint8_t>(ins.op));
            std::string text = info ? info->mnemonic : "?";
            const bool isJump = ins.op == Opcode::Jmp || ins.op == Opcode::JmpT || ins.op == Opcode::JmpF;
            for (size_t a = 0; a < ins.args.size(); ++a) {
                text += ' ';
                if (isJump && a + 1 == ins.args.size() && ins.args[a].type == ValueType::Integer) {
                    const long long target = static_cast<long long>(ip) + ins.args[a].as_int();
                    auto lt = labels.find(static_cast<size_t>(target));
                    text += lt != labels.end() ? "label" + std::to_string(lt->second) : value(ins.args[a]);
                } else {
                    text += value(ins.args[a]);
                }
            }
            if (haveLines) text += " ;@line " + std::to_string(dbg.rec ? dbg.rec->lines[ip] : 0);
            line(depth, text);
        }
    }

    const Script& m_s;
    std::string m_out;
};

}  // namespace

std::string to_asm(const Script& script) {
    return Asm(script).run();
}

std::string format_instruction(const Script& script, const Instruction& ins, size_t ip) {
    const OpcodeInfo* info = opcode_info(static_cast<uint8_t>(ins.op));
    std::string text = info ? info->mnemonic : "?";
    const bool isJump = ins.op == Opcode::Jmp || ins.op == Opcode::JmpT || ins.op == Opcode::JmpF;
    for (size_t a = 0; a < ins.args.size(); ++a) {
        const Value& v = ins.args[a];
        text += ' ';
        if (isJump && a + 1 == ins.args.size() && v.type == ValueType::Integer) {
            text += "-> " + std::to_string(static_cast<long long>(ip) + v.as_int());
            continue;
        }
        switch (v.type) {
        case ValueType::None: text += "None"; break;
        case ValueType::Identifier: text += std::string(script.str(v.str())); break;
        case ValueType::String: text += Asm::quoted_text(script.str(v.str())); break;
        case ValueType::Integer: text += std::to_string(v.as_int()); break;
        case ValueType::Float: text += Asm::float_literal(v.as_float()); break;
        case ValueType::Bool: text += v.as_bool() ? "True" : "False"; break;
        }
    }
    return text;
}

}  // namespace papyrus::pex
