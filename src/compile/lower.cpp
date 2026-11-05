// Lowering of a script's declarations into the pex model (docs/COMPILER.md
// milestone 2). Function bodies follow in a later milestone; until then every
// body is empty and compile() says so unless declarationsOnly was asked for.
// SPDX-License-Identifier: GPL-3.0-only
#include "lower.hpp"

#include "codegen.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <tuple>
#include <unordered_map>

namespace papyrus::psc {

namespace {

using pex::Instruction;
using pex::Opcode;
using pex::StrIdx;
using pex::Value;

class Lowerer {
public:
    Lowerer(const ScriptInfo& info, Program& program, const CompileOptions& options, const SourceFile& source, pex::Script& out)
        : m_info(info), m_ast(*info.ast), m_prog(program), m_opt(options), m_src(source), m_s(out) {}

    void run() {
        m_s = pex::Script{};
        m_s.header.major = 3;
        m_s.header.minor = 2;
        m_s.header.gameId = 1;
        m_s.header.compileTime = m_opt.compileTime ? m_opt.compileTime : now();
        m_s.header.sourceName = m_src.path.empty() ? m_src.name + ".psc" : m_src.path;
        m_s.header.userName = m_opt.userName;
        m_s.header.computerName = m_opt.computerName;
        intern("");
        m_s.debug.present = true;
        m_s.debug.modificationTime = m_src.modified;
        for (const UserFlagDef& f : m_opt.flags) {
            pex::UserFlag uf;
            uf.name = intern(f.name);
            uf.bit = static_cast<uint8_t>(f.bit);
            m_s.userFlags.push_back(uf);
        }
        object();
    }

private:
    [[noreturn]] void error(int line, const std::string& what) { throw LowerError{line, what}; }

    static uint64_t now() {
        return static_cast<uint64_t>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    }

    // --- strings ---------------------------------------------------------------

    StrIdx intern(std::string_view text) {
        const std::string key = lower(text);
        auto it = m_interned.find(key);
        if (it != m_interned.end()) return it->second;
        StrIdx idx;
        // The one-letter string "s" is stored as "S" whatever its case, as
        // the assembler does (docs/ASM_SYNTAX.md; vanilla TrapNorPlatform
        // concatenates "s" and the file holds "S").
        if (key == "s") text = "S";
        if (!m_s.intern(text, idx)) error(0, "string table full");
        m_interned.emplace(key, idx);
        return idx;
    }
    static std::string type_text(const TypeRef& t) { return t.name + (t.isArray ? "[]" : ""); }
    static std::string type_text(const TypeName& t) { return t.name + (t.isArray ? "[]" : ""); }

    uint32_t user_flags(const std::vector<std::string>& names, int line, bool (*allowed)(const UserFlagDef&), const char* onWhat) {
        uint32_t mask = 0;
        for (const std::string& n : names) {
            bool found = false;
            for (const UserFlagDef& f : m_opt.flags) {
                if (!ieq(f.name, n)) continue;
                found = true;
                if (!allowed(f)) error(line, "flag " + n + " is not allowed on a " + onWhat);
                mask |= 1u << f.bit;
            }
            if (!found) error(line, "unknown flag " + n);
        }
        return mask;
    }

    // A literal as the initial value of a slot of type `t` (int literals
    // become floats for float slots).
    Value literal_value(const Literal& l, const TypeRef& t) {
        const std::string tk = t.key();
        switch (l.kind) {
        case LitKind::None: return Value::none();
        case LitKind::Int:
            if (tk == "float") return Value::floating(static_cast<float>(l.i));
            if (tk != "int") error(l.line, "an integer cannot initialise a " + type_text(t));
            return Value::integer(l.i);
        case LitKind::Float:
            if (tk != "float") error(l.line, "a float cannot initialise a " + type_text(t));
            return Value::floating(l.f);
        case LitKind::Bool:
            if (tk != "bool") error(l.line, "a bool cannot initialise a " + type_text(t));
            return Value::boolean(l.b);
        case LitKind::String:
            if (tk != "string") error(l.line, "a string cannot initialise a " + type_text(t));
            return Value::string(intern(l.s));
        }
        return Value::none();
    }

    // --- debug records ---------------------------------------------------------

    void record(StrIdx label, StrIdx name, pex::FunctionKind kind, std::vector<uint16_t> lines) {
        const auto key = std::make_tuple(label, name, int(kind));
        auto it = m_records.find(key);
        if (it != m_records.end()) {
            m_s.debug.functions[it->second].lines = std::move(lines);
            return;
        }
        pex::FunctionDebug d;
        d.object = m_objectName;
        d.state = label;
        d.name = name;
        d.kind = kind;
        d.lines = std::move(lines);
        m_records.emplace(key, m_s.debug.functions.size());
        m_s.debug.functions.push_back(std::move(d));
    }

    // --- the object ------------------------------------------------------------

    void object() {
        pex::Object o;
        m_objectName = o.name = intern(m_ast.name);
        o.parent = intern(m_ast.parent);
        o.docString = intern(m_ast.doc);
        o.userFlags = user_flags(m_ast.flags, m_ast.line, [](const UserFlagDef& f) { return f.onScript; }, "script");
        o.autoState = intern("");
        for (const State& st : m_ast.states) {
            if (st.isAuto) o.autoState = intern(st.name);
        }

        // The compiler's listing has the variable table first, autovars
        // (lowercased types) ahead of the script's variables, so those
        // spellings are interned before any property type or doc string
        // (vanilla MS03OnUnloadScript: the doc string "Quest" is stored as
        // "quest", the autovar's type).
        for (const Property& p : m_ast.properties) {
            if (!p.isAuto) continue;
            intern("::" + p.name + "_var");
            intern(lower(type_text(p.type)));
        }
        for (const Variable& v : m_ast.variables) {
            pex::Variable pv;
            pv.name = intern(v.name);
            pv.type = intern(type_text(v.type));
            pv.userFlags = user_flags(v.flags, v.line, [](const UserFlagDef& f) { return f.onVariable; }, "variable");
            pv.initial = v.hasInit ? literal_value(v.init, TypeRef::of(v.type)) : Value::none();
            o.variables.push_back(pv);
        }

        // The default state: the generated GetState / GotoState, then the
        // script's own functions. Bodies are compiled in source order across
        // properties and state blocks (temps are numbered in that order:
        // vanilla BlackreachLockScript's SetOpen, declared after its state
        // blocks, starts at ::temp5; ObjectReference's X getter is numbered
        // after the functions declared before it).
        pex::State def;
        def.name = intern("");
        generated_state_functions(def);
        std::vector<pex::State> states(m_ast.states.size());
        for (size_t i = 0; i < m_ast.states.size(); ++i) {
            // The compiler's listing writes state names lowercased; a literal
            // spelled like a state merges to that spelling when the state
            // block precedes it in the compiler's (hash-ordered) listing,
            // which is not reproduced here (docs/COMPILER_PATTERNS.md).
            states[i].name = intern(lower(m_ast.states[i].name));
        }
        for (const auto& [kind, index] : m_ast.order) {
            if (kind == 1) {
                property(o, m_ast.properties[index]);
            } else if (kind == 2) {
                def.functions.push_back(function(m_ast.functions[index], def.name, false));
                m_pendingRecords.push_back({0, def.name, def.functions.back().name, m_lines});
            } else if (kind == 3) {
                const State& st = m_ast.states[index];
                pex::State& ps = states[index];
                bool first = true;
                for (const Function& f : st.functions) {
                    ps.functions.push_back(function(f, first ? ps.name : intern(""), false));
                    m_pendingRecords.push_back({index + 1, first ? ps.name : intern(""), ps.functions.back().name, m_lines});
                    first = false;
                }
            }
        }
        o.states.push_back(std::move(def));
        for (pex::State& ps : states) o.states.push_back(std::move(ps));
        // Debug records in the compiler's listing order: accessors (already
        // recorded), the default state's functions, then the other states.
        // The merge of same-named records ("last lines win", the label bug)
        // depends on this order, not on source order.
        for (size_t block = 0; block <= m_ast.states.size(); ++block) {
            for (Pending& r : m_pendingRecords) {
                if (r.block == block) record(r.label, r.name, pex::FunctionKind::Method, std::move(r.lines));
            }
        }
        m_pendingRecords.clear();
        m_s.objects.push_back(std::move(o));
    }

    void property(pex::Object& o, const Property& p) {
        pex::Property pp;
        pp.name = intern(p.name);
        pp.type = intern(type_text(p.type));
        pp.docString = intern(p.doc);
        std::vector<std::string> propFlags, varFlags;
        for (const std::string& f : p.flags) {
            // Conditional belongs to the backing variable (TESV flags: Variable), Hidden to the property.
            bool onProp = false, onVar = false;
            for (const UserFlagDef& d : m_opt.flags) {
                if (ieq(d.name, f)) {
                    onProp = d.onProperty;
                    onVar = d.onVariable;
                }
            }
            if (onProp) propFlags.push_back(f);
            else if (onVar) varFlags.push_back(f);
            else error(p.line, "flag " + f + " is not allowed on a property");
        }
        pp.userFlags = user_flags(propFlags, p.line, [](const UserFlagDef& f) { return f.onProperty; }, "property");
        const TypeRef type = TypeRef::of(p.type);
        if (p.isAuto) {
            pp.flags = pex::kPropRead | pex::kPropWrite | pex::kPropAutoVar;
            pex::Variable v;
            v.name = intern("::" + p.name + "_var");
            v.type = intern(lower(type_text(p.type)));
            v.userFlags = user_flags(varFlags, p.line, [](const UserFlagDef& f) { return f.onVariable; }, "variable");
            v.initial = p.hasInit ? literal_value(p.init, type) : Value::none();
            pp.autoVar = v.name;
            o.variables.push_back(v);
        } else if (p.isAutoReadOnly) {
            if (!varFlags.empty()) error(p.line, "flag " + varFlags[0] + " is not allowed on a read-only property");
            pp.flags = pex::kPropRead;
            pex::Function g;
            g.returnType = intern(type_text(p.type));
            g.docString = intern("");
            Instruction ret;
            ret.op = Opcode::Return;
            ret.args.push_back(p.hasInit ? literal_value(p.init, type) : Value::none());
            g.code.push_back(std::move(ret));
            record(intern(""), pp.name, pex::FunctionKind::Getter, {static_cast<uint16_t>(p.line)});
            pp.getter = std::move(g);
        } else {
            if (!varFlags.empty()) error(p.line, "flag " + varFlags[0] + " is not allowed on a property with accessors");
            pp.flags = 0;
            if (p.getter) {
                pp.flags |= pex::kPropRead;
                pp.getter = accessor(*p.getter, pp.name, pex::FunctionKind::Getter);
            }
            if (p.setter) {
                pp.flags |= pex::kPropWrite;
                pp.setter = accessor(*p.setter, pp.name, pex::FunctionKind::Setter);
            }
        }
        o.properties.push_back(std::move(pp));
    }

    pex::Function accessor(const Function& f, StrIdx propName, pex::FunctionKind kind) {
        pex::NamedFunction nf = function(f, intern(""), false);
        record(intern(""), propName, kind, m_lines);
        return std::move(nf.body);
    }

    // Compiles a function; `withRecord` adds its debug record under `label`.
    pex::NamedFunction function(const Function& f, StrIdx label, bool withRecord) {
        pex::NamedFunction nf;
        nf.name = intern(f.name);
        pex::Function& pf = nf.body;
        pf.returnType = intern(f.returnType.empty() ? "None" : type_text(f.returnType));
        pf.docString = intern(f.doc);
        pf.userFlags = user_flags(f.flags, f.line, [](const UserFlagDef& d) { return d.onFunction; }, "function");
        if (f.isGlobal) pf.flags |= pex::kFnGlobal;
        if (f.isNative) pf.flags |= pex::kFnNative;
        for (const Param& p : f.params) {
            pex::Param pp;
            pp.name = intern(p.name);
            pp.type = intern(type_text(p.type));
            pf.params.push_back(pp);
        }
        m_lines.clear();
        if (!f.isNative && !m_opt.declarationsOnly) {
            ObjectContext ctx{m_s, m_prog, m_info, [this](std::string_view t) { return intern(t); }, m_tempCounter};
            // A property accessor is not in the interface: describe it here.
            FuncInfo own;
            const FuncInfo* fi = m_info.own_function(f.name);
            if (!fi || fi->ast != &f) {
                own.name = f.name;
                own.ret = TypeRef::of(f.returnType);
                for (const Param& p : f.params) {
                    ParamInfo pi;
                    pi.name = p.name;
                    pi.type = TypeRef::of(p.type);
                    own.params.push_back(pi);
                }
                fi = &own;
            }
            compile_body(ctx, f, *fi, pf, m_lines);
            m_tempCounter = ctx.tempCounter;
        }
        if (withRecord) record(label, nf.name, pex::FunctionKind::Method, m_lines);
        return nf;
    }

    void generated_state_functions(pex::State& def) {
        {
            pex::NamedFunction nf;
            nf.name = intern("GetState");
            nf.body.returnType = intern("String");
            nf.body.docString = intern("Function that returns the current state");
            Instruction ret;
            ret.op = Opcode::Return;
            ret.args.push_back(Value::identifier(intern("::State")));
            nf.body.code.push_back(std::move(ret));
            record(def.name, nf.name, pex::FunctionKind::Method, {});
            def.functions.push_back(std::move(nf));
        }
        {
            pex::NamedFunction nf;
            nf.name = intern("GotoState");
            nf.body.returnType = intern("None");
            nf.body.docString = intern("Function that switches this object to the specified state");
            pex::Param p;
            p.name = intern("newState");
            p.type = intern("String");
            nf.body.params.push_back(p);
            pex::Param l;
            l.name = intern("::NoneVar");
            l.type = intern("None");
            nf.body.locals.push_back(l);
            const StrIdx self = intern("self");
            const StrIdx noneVar = intern("::NoneVar");
            Instruction a;
            a.op = Opcode::CallMethod;
            a.args = {Value::identifier(intern("onEndState")), Value::identifier(self), Value::identifier(noneVar)};
            Instruction b;
            b.op = Opcode::Assign;
            b.args = {Value::identifier(intern("::State")), Value::identifier(p.name)};
            Instruction c;
            c.op = Opcode::CallMethod;
            c.args = {Value::identifier(intern("onBeginState")), Value::identifier(self), Value::identifier(noneVar)};
            nf.body.code.push_back(std::move(a));
            nf.body.code.push_back(std::move(b));
            nf.body.code.push_back(std::move(c));
            record(def.name, nf.name, pex::FunctionKind::Method, {});
            def.functions.push_back(std::move(nf));
        }
        if (m_ast.parent.empty()) {
            // A script that extends nothing gets ScriptObject's two state events.
            for (const char* name : {"OnBeginState", "OnEndState"}) {
                if (m_info.own_function(name)) continue;
                pex::NamedFunction nf;
                nf.name = intern(name);
                nf.body.returnType = intern("None");
                nf.body.docString = intern(ieq(name, "OnBeginState") ? "Event received when this state is switched to"
                                                                      : "Event received when this state is switched away from");
                record(def.name, nf.name, pex::FunctionKind::Method, {});
                def.functions.push_back(std::move(nf));
            }
        }
    }

public:
    bool bodies_skipped() const { return m_bodiesSkipped; }

private:
    const ScriptInfo& m_info;
    const Script& m_ast;
    Program& m_prog;
    const CompileOptions& m_opt;
    const SourceFile& m_src;
    pex::Script& m_s;
    StrIdx m_objectName = 0;
    std::unordered_map<std::string, StrIdx> m_interned;
    std::map<std::tuple<StrIdx, StrIdx, int>, size_t> m_records;
    std::vector<uint16_t> m_lines;  // debug lines of the function being compiled
    struct Pending {
        size_t block;  // 0 = the default state, i + 1 = states[i]
        StrIdx label, name;
        std::vector<uint16_t> lines;
    };
    std::vector<Pending> m_pendingRecords;
    int m_tempCounter = 0;          // ::tempN numbering, one counter per object
    bool m_bodiesSkipped = false;
};

}  // namespace

bool lower_script(const ScriptInfo& info, Program& program, const CompileOptions& options, const SourceFile& source,
                  pex::Script& out, std::vector<Diagnostic>& diagnostics) {
    Lowerer l(info, program, options, source, out);
    try {
        l.run();
    } catch (const LowerError& e) {
        Diagnostic d;
        d.file = source.path;
        d.line = e.line;
        d.message = e.message;
        diagnostics.push_back(std::move(d));
        return false;
    } catch (const std::exception& e) {
        Diagnostic d;
        d.file = source.path;
        d.message = e.what();
        diagnostics.push_back(std::move(d));
        return false;
    }
    if (l.bodies_skipped() && !options.declarationsOnly) {
        Diagnostic d;
        d.file = source.path;
        d.message = "function bodies are not compiled yet (docs/COMPILER.md milestone 3)";
        diagnostics.push_back(std::move(d));
        return false;
    }
    return true;
}

}  // namespace papyrus::psc
