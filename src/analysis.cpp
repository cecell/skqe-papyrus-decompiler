// refs / deps / closure / patch (see include/papyrus/analysis.hpp).
// SPDX-License-Identifier: GPL-3.0-only
#include "papyrus/analysis.hpp"

#include "papyrus/decompile.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>

namespace papyrus::pex {

namespace {

std::string lower(std::string_view v) {
    std::string out(v);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool ends_with_ci(std::string_view v, std::string_view suffix) {
    if (v.size() < suffix.size()) return false;
    return lower(v.substr(v.size() - suffix.size())) == lower(suffix);
}

bool looks_like_plugin(std::string_view v) {
    return ends_with_ci(v, ".esp") || ends_with_ci(v, ".esm") || ends_with_ci(v, ".esl");
}

bool is_primitive(const std::string& t) {
    return t.empty() || t == "int" || t == "float" || t == "bool" || t == "string" || t == "none";
}

std::string base_type(std::string t) {
    if (t.size() > 2 && t.compare(t.size() - 2, 2, "[]") == 0) t.resize(t.size() - 2);
    return t;
}

// Index of the destination operand of an instruction, or -1.
int dest_operand(Opcode op) {
    switch (op) {
    case Opcode::IAdd: case Opcode::FAdd: case Opcode::ISub: case Opcode::FSub: case Opcode::IMul: case Opcode::FMul:
    case Opcode::IDiv: case Opcode::FDiv: case Opcode::IMod: case Opcode::Not: case Opcode::INeg: case Opcode::FNeg:
    case Opcode::Assign: case Opcode::Cast: case Opcode::CmpEq: case Opcode::CmpLt: case Opcode::CmpLe:
    case Opcode::CmpGt: case Opcode::CmpGe: case Opcode::StrCat: case Opcode::ArrayCreate: case Opcode::ArrayLength:
    case Opcode::ArrayGetElement:
        return 0;
    case Opcode::CallMethod: case Opcode::CallStatic: case Opcode::PropGet: return 2;
    case Opcode::CallParent: return 1;
    case Opcode::ArrayFindElement: case Opcode::ArrayRFindElement: return 1;
    default: return -1;
    }
}

// Visits every function body with its names.
struct FnRef {
    const Object* object;
    const State* state;      // null for accessors
    const Function* function;
    std::string stateName;
    std::string name;
};
template <class F>
void for_each_function(const Script& s, F&& fn) {
    for (const Object& o : s.objects) {
        for (const Property& p : o.properties) {
            if (p.getter) fn(FnRef{&o, nullptr, &*p.getter, "", std::string(s.str(p.name)) + ".get"});
            if (p.setter) fn(FnRef{&o, nullptr, &*p.setter, "", std::string(s.str(p.name)) + ".set"});
        }
        for (const State& st : o.states) {
            for (const NamedFunction& f : st.functions) {
                fn(FnRef{&o, &st, &f.body, std::string(s.str(st.name)), std::string(s.str(f.name))});
            }
        }
    }
}

// Mutable twin, for patching.
template <class F>
void for_each_function_mut(Script& s, F&& fn) {
    for (Object& o : s.objects) {
        for (Property& p : o.properties) {
            if (p.getter) fn(o, "", std::string(s.str(p.name)) + ".get", *p.getter);
            if (p.setter) fn(o, "", std::string(s.str(p.name)) + ".set", *p.setter);
        }
        for (State& st : o.states) {
            for (NamedFunction& f : st.functions) fn(o, std::string(s.str(st.name)), std::string(s.str(f.name)), f.body);
        }
    }
}

bool is_jump(Opcode op) { return op == Opcode::Jmp || op == Opcode::JmpT || op == Opcode::JmpF; }

// Which instructions are jump targets (labels).
std::vector<bool> labels_of(const Function& f) {
    std::vector<bool> lab(f.code.size() + 1, false);
    for (size_t ip = 0; ip < f.code.size(); ++ip) {
        const Instruction& ins = f.code[ip];
        if (!is_jump(ins.op) || ins.args.empty() || ins.args.back().type != ValueType::Integer) continue;
        const long long t = static_cast<long long>(ip) + ins.args.back().as_int();
        if (t >= 0 && t <= static_cast<long long>(f.code.size())) lab[static_cast<size_t>(t)] = true;
    }
    return lab;
}

// Is `name` written anywhere in the script (any function)?
bool written_anywhere(const Script& s, StrIdx name) {
    bool found = false;
    for_each_function(s, [&](const FnRef& fr) {
        if (found) return;
        for (const Instruction& ins : fr.function->code) {
            const int d = dest_operand(ins.op);
            if (d >= 0 && size_t(d) < ins.args.size() && ins.args[d].type == ValueType::Identifier && ins.args[d].str() == name) {
                found = true;
                return;
            }
            if (ins.op == Opcode::PropSet && ins.args.size() > 2) {
                // PROPSET writes a property, which may be an autovar's property.
            }
        }
    });
    return found;
}

struct Resolved {
    RefKind kind = RefKind::Dynamic;
    Value value;
    uint32_t literalIp = kNoIp;
    uint32_t literalOperand = 0;
    std::string variable;
};

// Follows an operand of instruction `useIp` back to a literal when that is
// safe: a literal operand, or an identifier whose nearest earlier assignment
// in the function assigns a literal and is either in straight-line code
// (no label between) or the only write to that name in the function; or a
// script variable with an initial value that nothing in the script writes.
Resolved resolve_operand(const Script& s, const Function& f, size_t useIp, size_t operand, ValueType wanted) {
    Resolved r;
    const Value& v = f.code[useIp].args[operand];
    if (v.type == wanted) {
        r.kind = RefKind::Literal;
        r.value = v;
        r.literalIp = static_cast<uint32_t>(useIp);
        r.literalOperand = static_cast<uint32_t>(operand);
        return r;
    }
    if (v.type != ValueType::Identifier) return r;
    const StrIdx name = v.str();
    r.variable = std::string(s.str(name));
    const std::vector<bool> labels = labels_of(f);
    size_t writes = 0;
    long long nearest = -1;
    for (size_t ip = 0; ip < f.code.size(); ++ip) {
        const Instruction& ins = f.code[ip];
        const int d = dest_operand(ins.op);
        if (d < 0 || size_t(d) >= ins.args.size()) continue;
        if (ins.args[d].type == ValueType::Identifier && ins.args[d].str() == name) {
            ++writes;
            if (ip < useIp) nearest = static_cast<long long>(ip);
        }
    }
    if (nearest >= 0) {
        const Instruction& w = f.code[static_cast<size_t>(nearest)];
        if (w.op == Opcode::Assign && w.args[1].type == wanted) {
            bool labelBetween = false;
            for (size_t ip = static_cast<size_t>(nearest) + 1; ip <= useIp; ++ip) labelBetween = labelBetween || labels[ip];
            if (!labelBetween || writes == 1) {
                r.kind = RefKind::Propagated;
                r.value = w.args[1];
                r.literalIp = static_cast<uint32_t>(nearest);
                r.literalOperand = 1;
            }
        }
        return r;
    }
    if (writes > 0) return r;  // written only after the use: loop-carried; unknown
    // Not written in this function: a script variable with an initial value?
    for (const Object& o : s.objects) {
        for (const Variable& var : o.variables) {
            if (var.name != name && lower(s.str(var.name)) != lower(s.str(name))) continue;
            if (var.initial.type == wanted && !written_anywhere(s, var.name)) {
                r.kind = RefKind::PropertyDefault;
                r.value = var.initial;
                r.literalIp = kNoIp;
            }
            return r;
        }
    }
    return r;
}

struct CallSite {
    std::string callee;
    size_t intArg;     // operand index of the integer
    long long strArg;  // operand index of the plugin string, or -1
};

bool recognise_call(const Script& s, const Instruction& ins, const std::set<std::string>& names, CallSite& out) {
    size_t first;  // index of the first call argument
    std::string callee;
    if (ins.op == Opcode::CallStatic) {
        first = 3;
        callee = std::string(s.str(ins.args[0].str())) + "." + std::string(s.str(ins.args[1].str()));
    } else if (ins.op == Opcode::CallMethod) {
        first = 3;
        callee = std::string(s.str(ins.args[0].str()));
    } else if (ins.op == Opcode::CallParent) {
        first = 2;
        callee = "Parent." + std::string(s.str(ins.args[0].str()));
    } else {
        return false;
    }
    const std::string fn = lower(callee.substr(callee.rfind('.') == std::string::npos ? 0 : callee.rfind('.') + 1));
    out.callee = callee;
    if (names.count(fn)) {
        if (ins.args.size() <= first) return false;
        out.intArg = first;
        out.strArg = ins.args.size() > first + 1 ? static_cast<long long>(first + 1) : -1;
        // GetForm / GetFormEx take no plugin.
        if (fn == "getform" || fn == "getformex") out.strArg = -1;
        return true;
    }
    // Generic shape: an int followed by a plugin-looking string literal.
    for (size_t i = first; i + 1 < ins.args.size(); ++i) {
        const Value& a = ins.args[i];
        const Value& b = ins.args[i + 1];
        if (b.type == ValueType::String && looks_like_plugin(s.str(b.str())) &&
            (a.type == ValueType::Integer || a.type == ValueType::Identifier)) {
            out.intArg = i;
            out.strArg = static_cast<long long>(i + 1);
            return true;
        }
    }
    return false;
}

}  // namespace

const char* to_string(RefKind k) noexcept {
    switch (k) {
    case RefKind::Literal: return "literal";
    case RefKind::Propagated: return "propagated";
    case RefKind::PropertyDefault: return "property-default";
    case RefKind::Dynamic: return "dynamic";
    }
    return "?";
}

std::vector<FormRef> find_form_refs(const Script& s, const std::vector<std::string>& extraCallees) {
    std::set<std::string> names = {"getformfromfile", "getformex", "getform"};
    for (const std::string& n : extraCallees) names.insert(lower(n.substr(n.rfind('.') == std::string::npos ? 0 : n.rfind('.') + 1)));
    std::vector<FormRef> out;
    for_each_function(s, [&](const FnRef& fr) {
        const Function& f = *fr.function;
        for (size_t ip = 0; ip < f.code.size(); ++ip) {
            CallSite cs;
            if (!recognise_call(s, f.code[ip], names, cs)) continue;
            FormRef r;
            r.object = std::string(s.str(fr.object->name));
            r.state = fr.stateName;
            r.function = fr.name;
            r.ip = static_cast<uint32_t>(ip);
            r.callee = cs.callee;
            const Resolved id = resolve_operand(s, f, ip, cs.intArg, ValueType::Integer);
            r.kind = id.kind;
            if (id.kind != RefKind::Dynamic) r.formId = id.value.as_int();
            r.literalIp = id.literalIp;
            r.literalOperand = id.literalOperand;
            r.variable = id.variable;
            if (cs.strArg >= 0) {
                const Resolved pl = resolve_operand(s, f, ip, static_cast<size_t>(cs.strArg), ValueType::String);
                if (pl.kind != RefKind::Dynamic) {
                    r.plugin = std::string(s.str(pl.value.str()));
                    r.pluginLiteral = true;
                } else {
                    r.plugin = pl.variable;
                }
            }
            out.push_back(std::move(r));
        }
    });
    return out;
}

// --- deps -------------------------------------------------------------------------

namespace {

struct TypeTable {
    std::map<std::string, std::string> byName;  // lowercased identifier -> lowercased type
};

TypeTable types_in(const Script& s, const Object& o, const Function& f) {
    TypeTable t;
    t.byName["self"] = lower(s.str(o.name));
    for (const Variable& v : o.variables) t.byName[lower(s.str(v.name))] = lower(s.str(v.type));
    for (const Param& p : f.params) t.byName[lower(s.str(p.name))] = lower(s.str(p.type));
    for (const Param& p : f.locals) t.byName[lower(s.str(p.name))] = lower(s.str(p.type));
    return t;
}

const Function* find_in_chain(ScriptResolver* resolver, const Script* start, std::string type, const std::string& fn,
                              bool* chainComplete) {
    const std::string want = lower(fn);
    const Script* cur = start;
    std::string curName = lower(type);
    if (chainComplete) *chainComplete = true;
    for (int guard = 0; guard < 32; ++guard) {
        if (!cur) {
            if (!resolver || curName.empty()) break;
            cur = resolver->find(curName);
            if (!cur) {
                if (chainComplete) *chainComplete = false;
                break;
            }
        }
        if (cur->objects.empty()) break;
        const Object& o = cur->objects[0];
        for (const State& st : o.states) {
            for (const NamedFunction& nf : st.functions) {
                if (lower(cur->str(nf.name)) == want) return &nf.body;
            }
        }
        curName = lower(cur->str(o.parent));
        cur = nullptr;
        if (curName.empty()) break;
    }
    return nullptr;
}

}  // namespace

Dependencies find_dependencies(const Script& s, ScriptResolver* resolver, ScriptResolver* vanilla) {
    Dependencies d;
    if (s.objects.empty()) return d;
    const Object& o = s.objects[0];
    d.name = std::string(s.str(o.name));
    // Parent chain.
    std::string cur = lower(s.str(o.parent));
    d.parentChainComplete = true;
    for (int guard = 0; guard < 32 && !cur.empty(); ++guard) {
        d.parents.push_back(cur);
        const Script* ps = resolver ? resolver->find(cur) : nullptr;
        if (!ps || ps->objects.empty()) {
            d.parentChainComplete = false;
            break;
        }
        cur = lower(ps->str(ps->objects[0].parent));
    }
    auto addType = [&](std::string_view t) {
        const std::string b = base_type(lower(t));
        if (!is_primitive(b)) d.types.insert(b);
    };
    if (!s.str(o.parent).empty()) addType(s.str(o.parent));
    for (const Variable& v : o.variables) addType(s.str(v.type));
    for (const Property& p : o.properties) addType(s.str(p.type));
    for_each_function(s, [&](const FnRef& fr) {
        const Function& f = *fr.function;
        // The compiler-generated state functions are not the script's own dependencies.
        if (fr.state && fr.stateName.empty()) {
            const std::string n = lower(fr.name);
            if (n == "getstate" || n == "gotostate") return;
        }
        addType(s.str(f.returnType));
        for (const Param& p : f.params) addType(s.str(p.type));
        for (const Param& p : f.locals) addType(s.str(p.type));
        const TypeTable tt = types_in(s, *fr.object, f);
        for (const Instruction& ins : f.code) {
            if (ins.op == Opcode::CallStatic) {
                const std::string cls = lower(s.str(ins.args[0].str()));
                d.staticClasses.insert(cls);
                addType(cls);
                d.callsByType[cls].insert(std::string(s.str(ins.args[1].str())));
            } else if (ins.op == Opcode::CallMethod) {
                std::string objType;
                if (ins.args[1].type == ValueType::Identifier) {
                    auto it = tt.byName.find(lower(s.str(ins.args[1].str())));
                    if (it != tt.byName.end()) objType = base_type(it->second);
                }
                const std::string fn = std::string(s.str(ins.args[0].str()));
                if (!objType.empty()) d.callsByType[objType].insert(fn);
                if (lower(fn) == "registerformodevent" && ins.args.size() > 3 && ins.args[3].type == ValueType::String) {
                    d.modEvents.insert(std::string(s.str(ins.args[3].str())));
                }
            } else if (ins.op == Opcode::CallParent) {
                d.callsByType[lower(s.str(o.parent))].insert(std::string(s.str(ins.args[0].str())));
            }
        }
    });
    if (vanilla) {
        for (const auto& [type, fns] : d.callsByType) {
            if (is_primitive(type)) continue;
            for (const std::string& fn : fns) {
                // Calls on the script itself: its own functions count, then its
                // parents through the vanilla set; other types through vanilla only.
                const Script* start = type == lower(s.str(o.name)) ? &s : vanilla->find(type);
                if (!start) {
                    d.notInVanilla.insert(type + "." + fn);
                    continue;
                }
                if (!find_in_chain(vanilla, start, type, fn, nullptr)) d.notInVanilla.insert(type + "." + fn);
            }
        }
    }
    return d;
}

// --- closure ------------------------------------------------------------------------

const char* to_string(DepState s) noexcept {
    switch (s) {
    case DepState::Pex: return "pex";
    case DepState::Source: return "source";
    case DepState::Missing: return "missing";
    }
    return "?";
}

Closure compute_closure(const Script& s, ScriptResolver& resolver,
                        const std::function<bool(const std::string& type, std::string& where)>& hasSource,
                        bool checkMembers) {
    Closure c;
    if (s.objects.empty()) return c;
    std::map<std::string, size_t> seen;
    std::vector<std::pair<std::string, std::string>> queue;  // (type, neededBy)
    const std::string root = lower(s.str(s.objects[0].name));
    queue.emplace_back(root, "");
    while (!queue.empty()) {
        const auto [type, by] = queue.front();
        queue.erase(queue.begin());
        if (seen.count(type)) continue;
        seen[type] = c.entries.size();
        ClosureEntry e;
        e.type = type;
        e.neededBy = by;
        const Script* ps = type == root ? &s : resolver.find(type);
        std::string where;
        if (ps) {
            e.state = DepState::Pex;
        } else if (hasSource && hasSource(type, where)) {
            e.state = DepState::Source;
            e.where = where;
        } else {
            e.state = DepState::Missing;
        }
        c.entries.push_back(e);
        if (!ps) continue;  // a source-only or missing type cannot be walked further
        Dependencies d = find_dependencies(*ps, &resolver, nullptr);
        for (const std::string& t : d.types) {
            if (!seen.count(t)) queue.emplace_back(t, type);
        }
    }
    if (!checkMembers) return c;
    // Members used by the root script: every called function and accessed
    // property must exist somewhere in the static type's chain, when that
    // chain is fully known.
    const Object& o = s.objects[0];
    std::set<std::string> reported;
    for_each_function(s, [&](const FnRef& fr) {
        const Function& f = *fr.function;
        const TypeTable tt = types_in(s, *fr.object, f);
        for (const Instruction& ins : f.code) {
            std::string type, member;
            bool property = false;
            if (ins.op == Opcode::CallStatic) {
                type = lower(s.str(ins.args[0].str()));
                member = std::string(s.str(ins.args[1].str()));
            } else if (ins.op == Opcode::CallMethod || ins.op == Opcode::PropGet || ins.op == Opcode::PropSet) {
                if (ins.args[1].type != ValueType::Identifier) continue;
                auto it = tt.byName.find(lower(s.str(ins.args[1].str())));
                if (it == tt.byName.end()) continue;
                type = base_type(it->second);
                member = std::string(s.str(ins.args[0].str()));
                property = ins.op != Opcode::CallMethod;
            } else if (ins.op == Opcode::CallParent) {
                type = lower(s.str(o.parent));
                member = std::string(s.str(ins.args[0].str()));
            } else {
                continue;
            }
            if (is_primitive(type)) continue;
            const std::string key = type + "." + lower(member);
            if (reported.count(key)) continue;
            // Walk the chain.
            bool complete = true, found = false;
            std::string cur = type;
            for (int guard = 0; guard < 32 && !cur.empty() && !found; ++guard) {
                const Script* ps = cur == root ? &s : resolver.find(cur);
                if (!ps || ps->objects.empty()) {
                    complete = false;
                    break;
                }
                const Object& po = ps->objects[0];
                if (property) {
                    for (const Property& p : po.properties) found = found || lower(ps->str(p.name)) == lower(member);
                } else {
                    for (const State& st : po.states) {
                        for (const NamedFunction& nf : st.functions) found = found || lower(ps->str(nf.name)) == lower(member);
                    }
                }
                cur = lower(ps->str(po.parent));
            }
            if (found) continue;
            reported.insert(key);
            MissingMember m;
            m.script = std::string(s.str(o.name));
            m.type = type;
            m.member = member;
            m.reason = complete ? (property ? "no such property in the chain" : "no such function in the chain")
                                : "chain incomplete (a parent is not available as a compiled script)";
            if (complete) c.missingMembers.push_back(std::move(m));
        }
    });
    return c;
}

// --- patch ---------------------------------------------------------------------------

bool load_formid_map(const std::string& text, FormIdMap& out, std::string* error) {
    out = FormIdMap{};
    size_t pos = 0;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        pos = 3;
    }
    size_t lineNo = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineNo;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t b = 0;
        while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
        line.erase(0, b);
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '!') {
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(1, eq - 1);
            for (char& ch : key) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            const std::string value = line.substr(eq + 1);
            if (key == "PLUGIN") out.plugin = value;
            else if (key == "NEWPLUGIN") out.newPlugin = value;
            continue;
        }
        const size_t bar = line.find('|');
        if (bar == std::string::npos) continue;
        std::string a = line.substr(0, bar);
        std::string rest = line.substr(bar + 1);
        const size_t bar2 = rest.find('|');
        std::string bstr = bar2 == std::string::npos ? rest : rest.substr(0, bar2);
        auto hex = [](const std::string& h, uint32_t& v) {
            if (h.empty()) return false;
            v = 0;
            for (char ch : h) {
                if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
                v = (v << 4) | static_cast<uint32_t>(std::isdigit(static_cast<unsigned char>(ch)) ? ch - '0' : (std::tolower(static_cast<unsigned char>(ch)) - 'a' + 10));
            }
            return true;
        };
        uint32_t oldV, newV;
        if (!hex(a, oldV) || !hex(bstr, newV)) continue;  // bad rows are skipped, as EslMapLib does
        out.oldToNew[oldV & 0xFFFFFFu] = newV & 0xFFFFFFu;
    }
    if (out.plugin.empty()) {
        if (error) *error = "map has no !PLUGIN= line";
        return false;
    }
    if (out.newPlugin.empty()) out.newPlugin = out.plugin;
    return true;
}

size_t rename_plugin(Script& s, std::string_view oldName, std::string_view newName) {
    size_t n = 0;
    const std::string want = lower(oldName);
    for (std::string& str : s.strings) {
        if (lower(str) == want) {
            str = std::string(newName);
            ++n;
        }
    }
    return n;
}

PatchResult apply_formid_map(Script& s, const FormIdMap& map, const std::vector<std::string>& extraCallees) {
    PatchResult res;
    const std::vector<FormRef> refs = find_form_refs(s, extraCallees);
    const std::string plugin = lower(map.plugin);
    std::set<std::pair<std::string, uint32_t>> done;  // (function key, literalIp) already edited (two refs may share an assignment)
    for (const FormRef& r : refs) {
        if (!r.pluginLiteral || lower(r.plugin) != plugin) continue;
        if (r.kind == RefKind::Dynamic) {
            res.skipped.push_back(r);
            continue;
        }
        const uint32_t low = static_cast<uint32_t>(r.formId) & 0xFFFFFFu;
        auto it = map.oldToNew.find(low);
        if (it == map.oldToNew.end()) {
            res.skipped.push_back(r);
            continue;
        }
        const int32_t newValue = static_cast<int32_t>((static_cast<uint32_t>(r.formId) & 0xFF000000u) | it->second);
        if (newValue == r.formId) continue;
        bool applied = false;
        if (r.kind == RefKind::PropertyDefault) {
            for (Object& o : s.objects) {
                for (Variable& v : o.variables) {
                    if (lower(s.str(v.name)) == lower(r.variable) && v.initial.type == ValueType::Integer) {
                        v.initial = Value::integer(newValue);
                        applied = true;
                    }
                }
            }
        } else {
            const std::string key = r.object + "|" + r.state + "|" + r.function;
            if (done.count({key, r.literalIp})) continue;
            for_each_function_mut(s, [&](Object& o, const std::string& st, const std::string& name, Function& f) {
                if (applied) return;
                if (std::string(s.str(o.name)) != r.object || st != r.state || name != r.function) return;
                if (r.literalIp < f.code.size() && r.literalOperand < f.code[r.literalIp].args.size()) {
                    Value& v = f.code[r.literalIp].args[r.literalOperand];
                    if (v.type == ValueType::Integer && v.as_int() == r.formId) {
                        v = Value::integer(newValue);
                        applied = true;
                    }
                }
            });
            done.insert({key, r.literalIp});
        }
        if (!applied) continue;
        PatchEdit e;
        e.object = r.object;
        e.state = r.state;
        e.function = r.function;
        e.ip = r.ip;
        e.callee = r.callee;
        e.oldValue = r.formId;
        e.newValue = newValue;
        e.plugin = r.plugin;
        res.edits.push_back(std::move(e));
    }
    if (lower(map.newPlugin) != plugin) {
        const size_t n = rename_plugin(s, map.plugin, map.newPlugin);
        for (size_t i = 0; i < n; ++i) res.renamedStrings.push_back(map.plugin + " -> " + map.newPlugin);
    }
    return res;
}

}  // namespace papyrus::pex
