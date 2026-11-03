// Canonical comparison (see include/papyrus/canon.hpp).
// SPDX-License-Identifier: GPL-3.0-only
#include "papyrus/canon.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>

namespace papyrus::pex {

const char* to_string(DiffKind k) noexcept {
    switch (k) {
    case DiffKind::Header: return "header";
    case DiffKind::UserFlags: return "user flags";
    case DiffKind::Object: return "object";
    case DiffKind::Variable: return "variable";
    case DiffKind::Property: return "property";
    case DiffKind::Function: return "function";
    case DiffKind::Locals: return "locals";
    case DiffKind::Instruction: return "instruction";
    case DiffKind::DebugInfo: return "debug info";
    }
    return "?";
}

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool is_temp(std::string_view name) {
    return name.size() > 6 && (name[0] == ':' && name[1] == ':') && lower(name.substr(0, 6)) == "::temp";
}

// One operand in canonical text form.
struct CValue {
    ValueType type;
    std::string text;  // identifier lowered, literal exact, numbers/bits
    bool operator==(const CValue& o) const { return type == o.type && text == o.text; }
    bool operator!=(const CValue& o) const { return !(*this == o); }
};

struct CInstruction {
    Opcode op;
    std::vector<CValue> args;
    bool operator==(const CInstruction& o) const { return op == o.op && args == o.args; }
};

struct CFunction {
    std::string returnType, doc;
    uint32_t userFlags = 0;
    uint8_t flags = 0;
    std::vector<std::pair<std::string, std::string>> params;
    std::set<std::pair<std::string, std::string>> locals;
    std::vector<CInstruction> code;
    bool signature_equal(const CFunction& o) const {
        return returnType == o.returnType && doc == o.doc && userFlags == o.userFlags && flags == o.flags &&
               params == o.params;
    }
};

struct CVariable {
    std::string type;
    uint32_t userFlags = 0;
    CValue initial;
    bool operator==(const CVariable& o) const {
        return type == o.type && userFlags == o.userFlags && initial == o.initial;
    }
};

struct CProperty {
    std::string type, doc, autoVar;
    uint32_t userFlags = 0;
    uint8_t flags = 0;
    std::optional<CFunction> getter, setter;
};

struct CObject {
    std::string parent, doc, autoState;
    uint32_t userFlags = 0;
    std::map<std::string, CVariable> variables;
    std::map<std::string, CProperty> properties;
    std::map<std::pair<std::string, std::string>, CFunction> functions;  // (state, name)
    std::set<std::string> states;
};

struct CScript {
    uint8_t major = 0, minor = 0;
    uint16_t gameId = 0;
    std::set<std::pair<std::string, uint8_t>> userFlags;
    std::map<std::string, CObject> objects;
    bool debug = false;
    // (object, state, name, kind) -> line count
    std::map<std::tuple<std::string, std::string, std::string, int>, size_t> debugLines;
};

class Canonicalizer {
public:
    Canonicalizer(const Script& s, const CompareOptions& opt) : m_s(s), m_opt(opt) {}

    CScript run() {
        CScript c;
        c.major = m_s.header.major;
        c.minor = m_s.header.minor;
        c.gameId = m_s.header.gameId;
        for (const UserFlag& f : m_s.userFlags) c.userFlags.emplace(lower(m_s.str(f.name)), f.bit);
        for (const Object& o : m_s.objects) c.objects.emplace(lower(m_s.str(o.name)), object(o));
        c.debug = m_s.debug.present;
        if (m_s.debug.present) {
            for (const FunctionDebug& f : m_s.debug.functions) {
                if (m_opt.ignoreGeneratedStateFunctions && is_generated(m_s.str(f.state), m_s.str(f.name))) continue;
                c.debugLines[{lower(m_s.str(f.object)), lower(m_s.str(f.state)), lower(m_s.str(f.name)),
                              int(f.kind)}] = f.lines.size();
            }
        }
        return c;
    }

private:
    static bool is_generated(std::string_view state, std::string_view name) {
        if (!state.empty()) return false;
        const std::string n = lower(name);
        return n == "getstate" || n == "gotostate";
    }

    CValue value(const Value& v, std::map<std::string, std::string>* temps) const {
        switch (v.type) {
        case ValueType::None: return {v.type, "None"};
        case ValueType::Identifier: {
            std::string name = lower(m_s.str(v.str()));
            if (temps && is_temp(name)) {
                auto it = temps->find(name);
                if (it == temps->end()) {
                    it = temps->emplace(name, "::temp#" + std::to_string(temps->size())).first;
                }
                name = it->second;
            }
            return {v.type, name};
        }
        case ValueType::String: return {v.type, std::string(m_s.str(v.str()))};
        case ValueType::Integer: return {v.type, std::to_string(v.as_int())};
        case ValueType::Float: {
            char buf[16];
            std::snprintf(buf, sizeof buf, "0x%08X", unsigned(v.bits));
            return {v.type, buf};
        }
        case ValueType::Bool: return {v.type, v.as_bool() ? "true" : "false"};
        }
        return {v.type, "?"};
    }

    CFunction function(const Function& f) const {
        CFunction c;
        c.returnType = lower(m_s.str(f.returnType));
        c.doc = std::string(m_s.str(f.docString));
        c.userFlags = f.userFlags;
        c.flags = f.flags;
        for (const Param& p : f.params) c.params.emplace_back(lower(m_s.str(p.name)), lower(m_s.str(p.type)));
        std::map<std::string, std::string> temps;
        std::map<std::string, std::string>* tp = m_opt.renumberTemps ? &temps : nullptr;
        for (const Instruction& ins : f.code) {
            CInstruction ci;
            ci.op = ins.op;
            for (const Value& v : ins.args) ci.args.push_back(value(v, tp));
            c.code.push_back(std::move(ci));
        }
        for (const Param& p : f.locals) {
            std::string name = lower(m_s.str(p.name));
            if (tp && is_temp(name)) {
                auto it = temps.find(name);
                // A temp never used in the code keeps a stable synthetic name.
                name = it != temps.end() ? it->second : "::temp#unused";
            }
            c.locals.emplace(name, lower(m_s.str(p.type)));
        }
        return c;
    }

    CObject object(const Object& o) const {
        CObject c;
        c.parent = lower(m_s.str(o.parent));
        c.doc = std::string(m_s.str(o.docString));
        c.autoState = lower(m_s.str(o.autoState));
        c.userFlags = o.userFlags;
        for (const Variable& v : o.variables) {
            c.variables[lower(m_s.str(v.name))] = {lower(m_s.str(v.type)), v.userFlags, value(v.initial, nullptr)};
        }
        for (const Property& p : o.properties) {
            CProperty cp;
            cp.type = lower(m_s.str(p.type));
            cp.doc = std::string(m_s.str(p.docString));
            cp.userFlags = p.userFlags;
            cp.flags = p.flags;
            if (p.is_auto()) cp.autoVar = lower(m_s.str(p.autoVar));
            if (p.getter) cp.getter = function(*p.getter);
            if (p.setter) cp.setter = function(*p.setter);
            c.properties[lower(m_s.str(p.name))] = std::move(cp);
        }
        for (const State& s : o.states) {
            const std::string sname = lower(m_s.str(s.name));
            c.states.insert(sname);
            for (const NamedFunction& f : s.functions) {
                if (m_opt.ignoreGeneratedStateFunctions && is_generated(m_s.str(s.name), m_s.str(f.name))) continue;
                c.functions[{sname, lower(m_s.str(f.name))}] = function(f.body);
            }
        }
        return c;
    }

    const Script& m_s;
    const CompareOptions& m_opt;
};

std::string ins_text(const CInstruction& i) {
    const OpcodeInfo* info = opcode_info(static_cast<uint8_t>(i.op));
    std::string s = info ? info->mnemonic : "?";
    for (const CValue& v : i.args) {
        s += ' ';
        if (v.type == ValueType::String) {
            s += '"' + v.text + '"';
        } else {
            s += v.text;
        }
    }
    return s;
}

class Comparer {
public:
    Comparer(std::vector<Difference>* out, const CompareOptions& opt) : m_out(out), m_opt(opt) {}

    bool equal() const { return m_count == 0; }

    void add(DiffKind kind, std::string where, std::string message) {
        ++m_count;
        if (m_out && (m_opt.maxDifferences == 0 || m_out->size() < m_opt.maxDifferences)) {
            m_out->push_back({kind, std::move(where), std::move(message)});
        }
    }

    void function(const std::string& where, const CFunction& a, const CFunction& b) {
        if (!a.signature_equal(b)) {
            std::string msg;
            if (a.returnType != b.returnType) msg = "return type " + a.returnType + " vs " + b.returnType;
            else if (a.params != b.params) msg = "parameters differ";
            else if (a.flags != b.flags) msg = "global/native flags differ";
            else if (a.userFlags != b.userFlags) msg = "user flags differ";
            else msg = "doc string differs";
            add(DiffKind::Function, where, msg);
            return;
        }
        if (a.locals != b.locals) {
            std::string msg = "locals differ:";
            for (const auto& l : a.locals) {
                if (!b.locals.count(l)) msg += " A has " + l.second + " " + l.first + ";";
            }
            for (const auto& l : b.locals) {
                if (!a.locals.count(l)) msg += " B has " + l.second + " " + l.first + ";";
            }
            add(DiffKind::Locals, where, msg);
        }
        if (a.code != b.code) {
            size_t i = 0;
            while (i < a.code.size() && i < b.code.size() && a.code[i] == b.code[i]) ++i;
            std::string msg;
            if (i < a.code.size() && i < b.code.size()) {
                msg = "ip " + std::to_string(i) + ": A: " + ins_text(a.code[i]) + "  B: " + ins_text(b.code[i]);
            } else {
                msg = "A has " + std::to_string(a.code.size()) + " instructions, B has " +
                      std::to_string(b.code.size()) + " (first " + std::to_string(i) + " equal)";
            }
            add(DiffKind::Instruction, where, msg);
        }
    }

    void object(const std::string& name, const CObject& a, const CObject& b) {
        if (a.parent != b.parent) add(DiffKind::Object, name, "parent " + a.parent + " vs " + b.parent);
        if (a.doc != b.doc) add(DiffKind::Object, name, "doc string differs");
        if (a.userFlags != b.userFlags) add(DiffKind::Object, name, "user flags differ");
        if (a.autoState != b.autoState) add(DiffKind::Object, name, "auto state " + a.autoState + " vs " + b.autoState);

        for (const auto& [vn, va] : a.variables) {
            auto it = b.variables.find(vn);
            if (it == b.variables.end()) add(DiffKind::Variable, name + "." + vn, "only in A");
            else if (!(va == it->second)) add(DiffKind::Variable, name + "." + vn, "type, flags or default differ");
        }
        for (const auto& [vn, vb] : b.variables) {
            if (!a.variables.count(vn)) add(DiffKind::Variable, name + "." + vn, "only in B");
        }

        for (const auto& [pn, pa] : a.properties) {
            auto it = b.properties.find(pn);
            const std::string where = name + ".prop " + pn;
            if (it == b.properties.end()) {
                add(DiffKind::Property, where, "only in A");
                continue;
            }
            const CProperty& pb = it->second;
            if (pa.type != pb.type || pa.doc != pb.doc || pa.userFlags != pb.userFlags || pa.flags != pb.flags ||
                pa.autoVar != pb.autoVar) {
                add(DiffKind::Property, where, "declaration differs");
                continue;
            }
            if (pa.getter.has_value() != pb.getter.has_value()) add(DiffKind::Property, where, "getter presence differs");
            else if (pa.getter) function(where + ".get", *pa.getter, *pb.getter);
            if (pa.setter.has_value() != pb.setter.has_value()) add(DiffKind::Property, where, "setter presence differs");
            else if (pa.setter) function(where + ".set", *pa.setter, *pb.setter);
        }
        for (const auto& [pn, pb] : b.properties) {
            if (!a.properties.count(pn)) add(DiffKind::Property, name + ".prop " + pn, "only in B");
        }

        for (const std::string& s : a.states) {
            if (!b.states.count(s)) add(DiffKind::Function, name + "." + s, "state only in A");
        }
        for (const std::string& s : b.states) {
            if (!a.states.count(s)) add(DiffKind::Function, name + "." + s, "state only in B");
        }
        for (const auto& [key, fa] : a.functions) {
            const std::string where = name + "." + (key.first.empty() ? "" : key.first + ".") + key.second;
            auto it = b.functions.find(key);
            if (it == b.functions.end()) add(DiffKind::Function, where, "only in A");
            else function(where, fa, it->second);
        }
        for (const auto& [key, fb] : b.functions) {
            if (!a.functions.count(key)) {
                add(DiffKind::Function, name + "." + (key.first.empty() ? "" : key.first + ".") + key.second,
                    "only in B");
            }
        }
    }

    void script(const CScript& a, const CScript& b) {
        if (a.major != b.major || a.minor != b.minor || a.gameId != b.gameId) {
            add(DiffKind::Header, "", "version or game id differ");
        }
        if (a.userFlags != b.userFlags) add(DiffKind::UserFlags, "", "user flag table differs");
        for (const auto& [on, oa] : a.objects) {
            auto it = b.objects.find(on);
            if (it == b.objects.end()) add(DiffKind::Object, on, "only in A");
            else object(on, oa, it->second);
        }
        for (const auto& [on, ob] : b.objects) {
            if (!a.objects.count(on)) add(DiffKind::Object, on, "only in B");
        }
        if (a.debug != b.debug) {
            add(DiffKind::DebugInfo, "", std::string("debug info ") + (a.debug ? "present" : "absent") + " in A, " +
                                             (b.debug ? "present" : "absent") + " in B");
        } else if (a.debug && a.debugLines != b.debugLines) {
            for (const auto& [k, n] : a.debugLines) {
                auto it = b.debugLines.find(k);
                const std::string where = std::get<0>(k) + "." + std::get<1>(k) + "." + std::get<2>(k);
                if (it == b.debugLines.end()) add(DiffKind::DebugInfo, where, "debug record only in A");
                else if (it->second != n) add(DiffKind::DebugInfo, where, "debug line counts differ");
            }
            for (const auto& [k, n] : b.debugLines) {
                if (!a.debugLines.count(k)) {
                    add(DiffKind::DebugInfo, std::get<0>(k) + "." + std::get<1>(k) + "." + std::get<2>(k),
                        "debug record only in B");
                }
            }
        }
    }

private:
    std::vector<Difference>* m_out;
    const CompareOptions& m_opt;
    size_t m_count = 0;
};

}  // namespace

bool canonical_equal(const Script& a, const Script& b, std::vector<Difference>* out, const CompareOptions& opt) {
    const CScript ca = Canonicalizer(a, opt).run();
    const CScript cb = Canonicalizer(b, opt).run();
    Comparer cmp(out, opt);
    cmp.script(ca, cb);
    return cmp.equal();
}

}  // namespace papyrus::pex
