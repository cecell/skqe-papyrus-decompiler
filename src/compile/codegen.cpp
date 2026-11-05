// Function bodies (codegen.hpp). The shape of every construct follows
// docs/COMPILER_PATTERNS.md; where that catalog is silent the choice is
// marked "unsettled" and the oracle (check --compile) decides.
// SPDX-License-Identifier: GPL-3.0-only
#include "codegen.hpp"

#include <deque>
#include <map>
#include <set>
#include <unordered_map>

namespace papyrus::psc {

namespace {

using pex::Instruction;
using pex::Opcode;
using pex::StrIdx;
using pex::Value;

// --- types -----------------------------------------------------------------

bool is_int(const TypeRef& t) { return !t.isArray && ieq(t.name, "int"); }
bool is_float(const TypeRef& t) { return !t.isArray && ieq(t.name, "float"); }
bool is_bool(const TypeRef& t) { return !t.isArray && ieq(t.name, "bool"); }
bool is_string(const TypeRef& t) { return !t.isArray && ieq(t.name, "string"); }
bool is_none(const TypeRef& t) { return !t.isArray && (t.name.empty() || ieq(t.name, "none")); }
bool is_value_type(const TypeRef& t) { return is_int(t) || is_float(t) || is_bool(t) || is_string(t); }
bool is_object(const TypeRef& t) { return !t.isArray && !is_value_type(t) && !is_none(t); }
bool same_type(const TypeRef& a, const TypeRef& b) { return a.isArray == b.isArray && ieq(a.name, b.name); }
TypeRef type(const char* name, bool arr = false) { return TypeRef{name, arr}; }
std::string type_text(const TypeRef& t) { return t.name + (t.isArray ? "[]" : ""); }

// An expression's value: a literal, a named slot, or a temp.
struct Operand {
    Value v;
    TypeRef type;
    bool isTemp = false;
    StrIdx temp = 0;  // the temp's name when isTemp
};

class FunctionCompiler {
public:
    FunctionCompiler(ObjectContext& ctx, const Function& f, const FuncInfo& fi, pex::Function& pf, std::vector<uint16_t>& lines)
        : m_c(ctx), m_f(f), m_fi(fi), m_pf(pf), m_lines(lines), m_prog(ctx.prog), m_self(ctx.self) {}

    void run() {
        m_scopes.emplace_back();
        for (const Param& p : m_f.params) {
            Slot s;
            s.pexName = m_c.intern(p.name);
            s.type = TypeRef::of(p.type);
            m_scopes.back()[lower(p.name)] = s;
        }
        m_scopes.emplace_back();  // the body's own scope, apart from the parameters
        plan_mangling();
        for (const StmtPtr& st : m_f.body) statement(*st);
        resolve_jumps();
    }

private:
    [[noreturn]] void error(int line, const std::string& what) { throw LowerError{line, what}; }

    // --- emission ---------------------------------------------------------------

    Instruction& emit(Opcode op, std::vector<Value> args) {
        Instruction ins;
        ins.op = op;
        ins.args = std::move(args);
        m_pf.code.push_back(std::move(ins));
        m_lines.push_back(static_cast<uint16_t>(m_line));
        return m_pf.code.back();
    }
    Value id(StrIdx s) { return Value::identifier(s); }
    Value id(std::string_view name) { return Value::identifier(m_c.intern(name)); }

    int new_label() {
        m_labels.push_back(size_t(-1));
        return int(m_labels.size() - 1);
    }
    void place(int label) { m_labels[size_t(label)] = m_pf.code.size(); }
    void jump(Opcode op, const Operand* cond, int label) {
        std::vector<Value> args;
        if (cond) args.push_back(cond->v);
        args.push_back(Value::integer(0));
        emit(op, std::move(args));
        m_jumps.emplace_back(m_pf.code.size() - 1, label);
    }
    // Code generated now but emitted later: the compiler allocates temps in
    // generation order (left operand, right operand, result) while the
    // instructions may come out in another order (the right operand of
    // && / || after the short-circuit jump; the object of a property
    // assignment after the value). A Cut holds a generated stretch with its
    // labels and jumps until it is pasted back.
    struct Cut {
        std::vector<Instruction> code;
        std::vector<uint16_t> lines;
        std::vector<std::pair<int, size_t>> labels;  // label, position relative to the cut
        std::vector<std::pair<size_t, int>> jumps;   // ip relative to the cut, label
    };
    struct Mark {
        size_t ip;
        size_t label;
    };
    Mark mark() const { return Mark{m_pf.code.size(), m_labels.size()}; }
    Cut cut_from(const Mark& m) {
        Cut c;
        c.code.assign(m_pf.code.begin() + static_cast<long long>(m.ip), m_pf.code.end());
        c.lines.assign(m_lines.begin() + static_cast<long long>(m.ip), m_lines.end());
        m_pf.code.resize(m.ip);
        m_lines.resize(m.ip);
        for (size_t i = m.label; i < m_labels.size(); ++i) {
            if (m_labels[i] != size_t(-1) && m_labels[i] >= m.ip) {
                c.labels.emplace_back(int(i), m_labels[i] - m.ip);
                m_labels[i] = size_t(-1);
            }
        }
        std::vector<std::pair<size_t, int>> keep;
        for (const auto& j : m_jumps) {
            if (j.first >= m.ip) c.jumps.emplace_back(j.first - m.ip, j.second);
            else keep.push_back(j);
        }
        m_jumps = std::move(keep);
        return c;
    }
    void paste(Cut& c) {
        const size_t base = m_pf.code.size();
        for (Instruction& ins : c.code) m_pf.code.push_back(std::move(ins));
        m_lines.insert(m_lines.end(), c.lines.begin(), c.lines.end());
        for (const auto& [label, rel] : c.labels) m_labels[size_t(label)] = base + rel;
        for (const auto& [rel, label] : c.jumps) m_jumps.emplace_back(base + rel, label);
    }

    void resolve_jumps() {
        for (const auto& [ip, label] : m_jumps) {
            const size_t target = m_labels[size_t(label)];
            if (target == size_t(-1)) error(m_f.line, "internal: unplaced label");
            m_pf.code[ip].args.back() = Value::integer(static_cast<int32_t>(static_cast<long long>(target) - static_cast<long long>(ip)));
        }
    }

    // --- temps and locals -------------------------------------------------------

    struct Slot {
        StrIdx pexName = 0;
        TypeRef type;
        bool mangled = false;  // ::mangled_<name>_N: uses keep that name, not the source spelling
    };

    Operand alloc(const TypeRef& t) {
        const std::string key = t.key();
        Operand o;
        o.type = t;
        o.isTemp = true;
        auto& q = m_free[key];
        if (!q.empty()) {
            o.temp = q.front();
            q.pop_front();
        } else {
            o.temp = m_c.intern("::temp" + std::to_string(m_c.tempCounter++));
            pex::Param l;
            l.name = o.temp;
            l.type = m_c.intern(lower(type_text(t)));
            m_pf.locals.push_back(l);
        }
        o.v = Value::identifier(o.temp);
        return o;
    }
    void release(Operand& o) {
        if (!o.isTemp) return;
        m_free[o.type.key()].push_back(o.temp);
        o.isTemp = false;
    }
    Value none_var() {
        if (!m_haveNoneVar) {
            pex::Param l;
            l.name = m_c.intern("::NoneVar");
            l.type = m_c.intern("None");
            m_pf.locals.push_back(l);
            m_haveNoneVar = true;
        }
        return Value::identifier(m_c.intern("::NoneVar"));
    }

    const Slot* find_param(std::string_view name) const {
        auto it = m_scopes.front().find(lower(name));
        return it != m_scopes.front().end() ? &it->second : nullptr;
    }
    const Slot* find_local(std::string_view name) const {
        const std::string key = lower(name);
        for (size_t i = m_scopes.size(); i-- > 0;) {
            auto it = m_scopes[i].find(key);
            if (it != m_scopes[i].end()) return &it->second;
        }
        return nullptr;
    }

    // Local renaming, as the shipped scripts show it: before a body is generated, the
    // function's scopes are walked in pre-order (a scope's own declarations
    // in source order, then its child blocks in order), and a local whose
    // lowercased name was met anywhere earlier in that walk becomes
    // ::mangled_<name>_N, N counting over the whole script (mod
    // TT_Skyrim_QuestScript reaches _71). So the inner block's local loses
    // even when it is declared first (mod OL11_KatriaMezzanineRadarScript:
    // `Actor KatriaActor` inside an If, then again at function level, and
    // the inner one is ::mangled_katriaactor_0); vanilla
    // ArenaScript.PickNextFight: fightIndex, _0, _1, _2.
    void plan_mangling() {
        std::set<std::string> seen;
        for (const Param& p : m_f.params) seen.insert(lower(p.name));
        plan_block(m_f.body, seen);
    }

    void plan_block(const std::vector<StmtPtr>& body, std::set<std::string>& seen) {
        for (const StmtPtr& st : body) {
            if (st->kind != StmtKind::Decl) continue;
            const std::string key = lower(st->declName);
            if (seen.count(key)) {
                // The name is the scope dictionary's key, which is lowercased
                // (mod TT_Skyrim_QuestScript: `FormList akList` -> ::mangled_aklist_0).
                m_mangledNames[st.get()] = "::mangled_" + key + "_" + std::to_string(m_c.mangleCounter++);
            } else {
                seen.insert(key);
            }
        }
        for (const StmtPtr& st : body) {
            if (st->kind == StmtKind::If) {
                plan_block(st->body, seen);
                for (const ElseIf& ei : st->elseIfs) plan_block(ei.body, seen);
                if (st->hasElse) plan_block(st->elseBody, seen);
            } else if (st->kind == StmtKind::While) {
                plan_block(st->body, seen);
            }
        }
    }

    void declare_local(const Stmt& st) {
        const std::string& name = st.declName;
        const std::string key = lower(name);
        if (m_scopes.back().count(key)) error(st.line, "variable " + name + " already declared in this scope");
        if (find_local(name)) error(st.line, "variable " + name + " hides a variable of an enclosing scope");
        Slot s;
        s.type = TypeRef::of(st.declType);
        std::string pexName = name;
        auto m = m_mangledNames.find(&st);
        if (m != m_mangledNames.end()) {
            pexName = m->second;
            s.mangled = true;
        }
        s.pexName = m_c.intern(pexName);
        pex::Param l;
        l.name = s.pexName;
        l.type = m_c.intern(type_text(s.type));
        m_pf.locals.push_back(l);
        m_scopes.back()[key] = s;
    }

    // --- conversions --------------------------------------------------------------

    // Can a value of type `from` stand where `to` is expected without a
    // written cast? (docs/COMPILER_PATTERNS.md, "Casts")
    bool implicit_ok(const TypeRef& from, const TypeRef& to) {
        if (same_type(from, to)) return true;
        if (is_none(from)) return !is_value_type(to) || is_string(to) || is_bool(to);
        if (is_bool(to)) return true;
        if (is_string(to)) return !to.isArray;
        if (is_float(to) && is_int(from)) return true;
        if (is_object(from) && is_object(to)) return m_prog.is_subtype(from.name, to.name);
        return false;
    }

    // The operand converted to `to`: a CAST into a new temp when the types
    // differ, the operand itself otherwise. The cast's temp is released at
    // once (the instruction that consumes it follows before anything else
    // is allocated), which is what makes a right operand's cast temp the
    // result temp of the operation (CAST t3 x; STRCAT t3 t2 t3) and puts it
    // ahead of the left operand in the free list (vanilla
    // BYOHRelationshipAdoptableAccessor: CAST t3 None; COMPAREEQ t4 t2 t3;
    // then the next actor temp is t3). docs/COMPILER_PATTERNS.md, "Temps".
    Operand convert(Operand o, const TypeRef& to, int line, bool releaseNow = true) {
        if (same_type(o.type, to)) return o;
        if (!implicit_ok(o.type, to)) error(line, "cannot convert " + type_text(o.type) + " to " + type_text(to) + " without a cast");
        release(o);
        Operand t = alloc(to);
        emit(Opcode::Cast, {t.v, o.v});
        if (releaseNow) release(t);
        return t;
    }

    // --- names --------------------------------------------------------------------

    enum class NameKind { None, Local, Variable, AutoProp, Property, Type };
    struct Name {
        NameKind kind = NameKind::None;
        StrIdx pexName = 0;   // local / variable / autovar
        TypeRef type;
        const PropInfo* prop = nullptr;
        const ScriptInfo* owner = nullptr;  // property owner
        std::string typeName;               // Type
    };

    // Name resolution as Bethesda's compiler does it, proved by the vanilla and
    // mod scripts named below: in a function that is not global
    // a property, own or inherited, comes first (vanilla PhysicsTrap: `int
    // damage` declared, `damage = ...` compiles to the parent's
    // `::damage_var`; dunMiddenHandSculptureSCRIPT: `float x = ...; return x`
    // reads ObjectReference's X property; mod DoorAutoCloseScript: the
    // parameter Door00 loses to the auto property's `::Door00_var`), then the
    // parameters and locals, then the script's own variables. A global
    // function sees only its parameters and locals (vanilla
    // dunYngolGlowAttractTrig: the parameter moteList, not the property of
    // that name).
    Name resolve(const std::string& ident, int line) {
        Name n;
        if (!m_f.isGlobal) {
            const ScriptInfo* owner = nullptr;
            if (const PropInfo* p = m_prog.find_property(m_self, ident, &owner)) {
                n.prop = p;
                n.owner = owner;
                n.type = p->type;
                // An auto property, own or inherited, is the backing variable
                // (vanilla RPDefault_LLInjection_Ammo reads the parent's
                // ::InjectAtLevel_var directly).
                if (p->isAuto) {
                    n.kind = NameKind::AutoProp;
                    n.pexName = m_c.intern("::" + p->name + "_var");
                } else {
                    n.kind = NameKind::Property;
                }
                return n;
            }
        }
        if (const Slot* s = find_param(ident)) {
            n.kind = NameKind::Local;
            n.pexName = m_c.intern(ident);
            n.type = s->type;
            return n;
        }
        if (const Slot* s = find_local(ident)) {
            n.kind = NameKind::Local;
            n.pexName = s->mangled ? s->pexName : m_c.intern(ident);
            n.type = s->type;
            return n;
        }
        if (!m_f.isGlobal) {
            if (const VarInfo* v = m_self.own_variable(ident)) {
                n.kind = NameKind::Variable;
                n.pexName = m_c.intern(ident);
                n.type = v->type;
                return n;
            }
        }
        if (m_prog.find(ident)) {
            n.kind = NameKind::Type;
            n.typeName = ident;
            return n;
        }
        error(line, "variable " + ident + " is undefined");
    }

    // --- expressions ----------------------------------------------------------------

    Operand literal(const Literal& l) {
        Operand o;
        switch (l.kind) {
        case LitKind::None: o.v = Value::none(); o.type = type("none"); break;
        case LitKind::Int: o.v = Value::integer(l.i); o.type = type("Int"); break;
        case LitKind::Float: o.v = Value::floating(l.f); o.type = type("Float"); break;
        case LitKind::Bool: o.v = Value::boolean(l.b); o.type = type("Bool"); break;
        case LitKind::String: o.v = Value::string(m_c.intern(l.s)); o.type = type("String"); break;
        }
        return o;
    }

    // Identifiers are written in the spelling the source uses at that
    // point, not the declared one (Probe04_Spelling: `hELPER.dOtHING(2)` ->
    // `CALLMETHOD dOtHING`, `PROPGET AUTOVAL`, `IADD ::temp2 LOC 1`); the
    // assembler then merges case variants to the first spelling in the
    // listing (vanilla CommentaryObjectScript: `enable()` before
    // `PlayAnimation("enable")` stores "enable", not the declared Enable).
    Operand self_operand(std::string_view spelling = "self") {
        Operand o;
        o.v = id(spelling);
        o.type = TypeRef{m_self.name, false};
        return o;
    }

    Operand expr(const Expr& e) {
        switch (e.kind) {
        case ExprKind::Lit: return literal(e.lit);
        case ExprKind::Self: return self_operand(e.text);
        case ExprKind::Parent: error(e.line, "'Parent' can only be used to call a function");
        case ExprKind::Ident: return ident_value(e);
        case ExprKind::Unary: return unary(e);
        case ExprKind::Binary:
            if (e.text == "&&" || e.text == "||") return logical(e);
            if (e.text == "+" || e.text == "-" || e.text == "*" || e.text == "/" || e.text == "%") return arithmetic(e);
            return comparison(e);
        case ExprKind::Cast: {
            Operand a = expr(*e.a);
            const TypeRef to = TypeRef::of(e.type);
            if (is_none(to)) error(e.line, "cannot cast to None");
            release(a);
            Operand t = alloc(to);
            emit(Opcode::Cast, {t.v, a.v});
            return t;
        }
        case ExprKind::Index: {
            Operand arr = expr(*e.a);
            if (!arr.type.isArray) error(e.line, "indexing a value that is not an array");
            Operand idx = convert(expr(*e.b), type("Int"), e.line);
            // The result temp is taken before the array and index temps are
            // released (vanilla
            // DLC1_NPCMentalModelScript.OnUpdate: `a[count] = a[count + 1]`
            // on an int array takes a fresh temp for the element).
            Operand t = alloc(TypeRef{arr.type.name, false});
            release(arr);
            release(idx);
            emit(Opcode::ArrayGetElement, {t.v, arr.v, idx.v});
            return t;
        }
        case ExprKind::Member: return member(e);
        case ExprKind::Call: return call(e, false);
        case ExprKind::New: {
            Operand n = convert(expr(*e.a), type("Int"), e.line);
            release(n);
            Operand t = alloc(TypeRef{e.type.name, true});
            emit(Opcode::ArrayCreate, {t.v, n.v});
            return t;
        }
        }
        error(e.line, "internal: unknown expression");
    }

    Operand ident_value(const Expr& e) {
        Name n = resolve(e.text, e.line);
        Operand o;
        switch (n.kind) {
        case NameKind::Local:
        case NameKind::Variable:
        case NameKind::AutoProp:
            o.v = id(n.pexName);
            o.type = n.type;
            return o;
        case NameKind::Property: {
            if (!n.prop->readable) error(e.line, "property " + n.prop->name + " cannot be read");
            Operand t = alloc(n.type);
            emit(Opcode::PropGet, {id(e.text), id("self"), t.v});
            return t;
        }
        case NameKind::Type: error(e.line, "type " + e.text + " used as a value");
        default: error(e.line, "internal: unresolved name");
        }
    }

    Operand member(const Expr& e) {
        // A static receiver (a script name that is not a variable) has no properties to read.
        Operand obj = receiver(*e.a, e.line);
        if (obj.type.isArray) {
            if (!ieq(e.text, "length")) error(e.line, "arrays only have Length");
            release(obj);
            Operand t = alloc(type("Int"));
            emit(Opcode::ArrayLength, {t.v, obj.v});
            return t;
        }
        if (!is_object(obj.type)) error(e.line, "a " + type_text(obj.type) + " has no properties");
        const ScriptInfo* info = m_prog.find(obj.type.name);
        if (!info) error(e.line, "unknown type " + obj.type.name);
        const PropInfo* p = m_prog.find_property(*info, e.text);
        if (!p) error(e.line, "type " + obj.type.name + " has no property " + e.text);
        if (!p->readable) error(e.line, "property " + e.text + " cannot be read");
        release(obj);
        Operand t = alloc(p->type);
        emit(Opcode::PropGet, {id(e.text), obj.v, t.v});
        return t;
    }

    // The receiver of a member access or method call: an object value.
    Operand receiver(const Expr& r, int line) {
        if (r.kind == ExprKind::Ident) {
            Name n = resolve(r.text, line);
            if (n.kind == NameKind::Type) error(line, "type " + r.text + " used as an object");
        }
        return expr(r);
    }

    Operand unary(const Expr& e) {
        if (e.text == "-") {
            Operand a = expr(*e.a);
            if (!is_int(a.type) && !is_float(a.type)) error(e.line, "unary minus needs an int or float");
            release(a);
            Operand t = alloc(a.type);
            emit(is_int(a.type) ? Opcode::INeg : Opcode::FNeg, {t.v, a.v});
            return t;
        }
        // NOT takes any operand without a cast (vanilla
        // BYOHPlanterContainerScript: NOT t ::PlantedFloraRef_var).
        Operand a = expr(*e.a);
        release(a);
        Operand t = alloc(type("Bool"));
        emit(Opcode::Not, {t.v, a.v});
        return t;
    }

    // a && b, a || b (docs/COMPILER_PATTERNS.md, probes/Probe03_Logic): both
    // operands are generated first (the right one's code is held back), the
    // left temp and then the right temp are released, the accumulator is
    // allocated, and the code comes out as CAST acc left; jump; <right>;
    // CAST acc right.
    Operand logical(const Expr& e) {
        Operand l = expr(*e.a);
        const Mark m = mark();
        Operand r = expr(*e.b);
        Cut right = cut_from(m);
        release(l);
        release(r);
        Operand acc = alloc(type("Bool"));
        emit(Opcode::Cast, {acc.v, l.v});
        const int skip = new_label();
        jump(e.text == "&&" ? Opcode::JmpF : Opcode::JmpT, &acc, skip);
        paste(right);
        emit(Opcode::Cast, {acc.v, r.v});
        place(skip);
        return acc;
    }

    Operand arithmetic(const Expr& e) {
        Operand l = expr(*e.a);
        Operand r = expr(*e.b);
        TypeRef rt;
        if (e.text == "+" && (is_string(l.type) || is_string(r.type))) rt = type("String");
        else if (is_float(l.type) || is_float(r.type)) rt = type("Float");
        else if (is_int(l.type) && is_int(r.type)) rt = type("Int");
        else error(e.line, "operator " + e.text + " cannot combine " + type_text(l.type) + " and " + type_text(r.type));
        if (!implicit_ok(l.type, rt) || !implicit_ok(r.type, rt)) {
            error(e.line, "operator " + e.text + " cannot combine " + type_text(l.type) + " and " + type_text(r.type));
        }
        Opcode op;
        if (is_string(rt)) op = Opcode::StrCat;
        else if (is_float(rt)) {
            op = e.text == "+" ? Opcode::FAdd : e.text == "-" ? Opcode::FSub : e.text == "*" ? Opcode::FMul : Opcode::FDiv;
            if (e.text == "%") error(e.line, "% needs two ints");
        } else {
            op = e.text == "+" ? Opcode::IAdd : e.text == "-" ? Opcode::ISub : e.text == "*" ? Opcode::IMul : e.text == "/" ? Opcode::IDiv : Opcode::IMod;
        }
        // An implicit cast of the left operand is emitted after the right
        // operand; both operands are released (left first), then the result
        // is allocated.
        if (!same_type(l.type, rt)) l = convert(l, rt, e.line);
        if (!same_type(r.type, rt)) r = convert(r, rt, e.line);
        release(l);
        release(r);
        Operand result = alloc(rt);
        emit(op, {result.v, l.v, r.v});
        return result;
    }

    Operand comparison(const Expr& e) {
        Operand l = expr(*e.a);
        Operand r = expr(*e.b);
        // The common type of the two operands.
        TypeRef ct;
        if (same_type(l.type, r.type)) ct = l.type;
        else if (is_none(l.type)) ct = r.type;
        else if (is_none(r.type)) ct = l.type;
        else if ((is_int(l.type) && is_float(r.type)) || (is_float(l.type) && is_int(r.type))) ct = type("Float");
        else if (is_string(l.type) || is_string(r.type)) ct = type("String");
        else if (is_bool(l.type) || is_bool(r.type)) ct = type("Bool");
        else if (is_object(l.type) && is_object(r.type)) {
            if (m_prog.is_subtype(l.type.name, r.type.name)) ct = r.type;
            else if (m_prog.is_subtype(r.type.name, l.type.name)) ct = l.type;
            else error(e.line, "cannot compare " + type_text(l.type) + " with " + type_text(r.type));
        } else {
            error(e.line, "cannot compare " + type_text(l.type) + " with " + type_text(r.type));
        }
        const bool eq = e.text == "==" || e.text == "!=";
        if (!eq && !is_int(ct) && !is_float(ct) && !is_string(ct) && !is_bool(ct)) {
            error(e.line, "ordering comparison needs values");
        }
        Opcode op = Opcode::CmpEq;
        if (e.text == "<") op = Opcode::CmpLt;
        else if (e.text == "<=") op = Opcode::CmpLe;
        else if (e.text == ">") op = Opcode::CmpGt;
        else if (e.text == ">=") op = Opcode::CmpGe;
        if (!same_type(l.type, ct)) l = convert(l, ct, e.line);
        if (!same_type(r.type, ct)) r = convert(r, ct, e.line);
        release(l);
        release(r);
        Operand t = alloc(type("Bool"));
        emit(op, {t.v, l.v, r.v});
        if (e.text == "!=") emit(Opcode::Not, {t.v, t.v});
        return t;
    }

    // --- calls ----------------------------------------------------------------------

    struct Callee {
        enum Kind { Method, Parent, Static, ArrayFind, ArrayRFind } kind = Method;
        const FuncInfo* fn = nullptr;
        Operand obj;              // Method: the receiver
        std::string className;    // Static
    };

    Operand call(const Expr& e, bool discard) {
        Callee c;
        bool haveObj = false;
        if (!e.a) {
            // Own function (through the parent chain), else an imported global.
            const ScriptInfo* owner = nullptr;
            if (const FuncInfo* f = find_method(m_self, e.text, &owner)) {
                c.fn = f;
                if (f->isGlobal) {
                    c.kind = Callee::Static;
                    c.className = owner->name;
                } else {
                    c.kind = Callee::Method;
                    c.obj = self_operand();
                    haveObj = true;
                }
            } else {
                for (const std::string& imp : m_self.imports) {
                    const ScriptInfo* info = m_prog.find(imp);
                    if (!info) continue;
                    if (const FuncInfo* g = m_prog.find_function(*info, e.text, &owner)) {
                        if (!g->isGlobal) continue;
                        c.fn = g;
                        c.kind = Callee::Static;
                        c.className = owner->name;
                        break;
                    }
                }
                if (!c.fn) error(e.line, e.text + " is not a function or does not exist");
            }
        } else if (e.a->kind == ExprKind::Parent) {
            if (m_self.parent.empty()) error(e.line, "the script has no parent");
            const ScriptInfo* parent = m_prog.find(m_self.parent);
            if (!parent) error(e.line, "unable to find parent script " + m_self.parent);
            c.fn = m_prog.find_function(*parent, e.text);
            if (!c.fn) error(e.line, "parent has no function " + e.text);
            c.kind = Callee::Parent;
        } else if (e.a->kind == ExprKind::Ident && resolve(e.a->text, e.line).kind == NameKind::Type) {
            const ScriptInfo* info = m_prog.find(e.a->text);
            const ScriptInfo* owner = nullptr;
            c.fn = m_prog.find_function(*info, e.text, &owner);
            if (!c.fn) error(e.line, e.a->text + " has no function " + e.text);
            if (!c.fn->isGlobal) error(e.line, e.text + " is not a global function");
            c.kind = Callee::Static;
            c.className = owner->name;
        } else {
            c.obj = expr(*e.a);
            haveObj = true;
            if (c.obj.type.isArray) {
                if (ieq(e.text, "find")) c.kind = Callee::ArrayFind;
                else if (ieq(e.text, "rfind")) c.kind = Callee::ArrayRFind;
                else error(e.line, "arrays only have Find and RFind");
            } else {
                if (!is_object(c.obj.type)) error(e.line, "a " + type_text(c.obj.type) + " has no functions");
                const ScriptInfo* info = m_prog.find(c.obj.type.name);
                if (!info) error(e.line, "unknown type " + c.obj.type.name);
                const ScriptInfo* owner = nullptr;
                c.fn = find_method(*info, e.text, &owner);
                if (!c.fn) error(e.line, "type " + c.obj.type.name + " has no function " + e.text);
                if (c.fn->isGlobal) {
                    c.kind = Callee::Static;
                    c.className = owner->name;
                    release(c.obj);
                    haveObj = false;
                } else {
                    c.kind = Callee::Method;
                }
            }
        }

        if (c.kind == Callee::ArrayFind || c.kind == Callee::ArrayRFind) {
            // Find(value, startIndex = 0) / RFind(value, startIndex = -1)
            const TypeRef elem{c.obj.type.name, false};
            Operand value, start;
            bool haveValue = false, haveStart = false;
            for (const Arg& a : e.args) {
                if (a.name.empty()) {
                    if (!haveValue) {
                        value = convert(expr(*a.value), elem, a.line);
                        haveValue = true;
                    } else if (!haveStart) {
                        start = convert(expr(*a.value), type("Int"), a.line);
                        haveStart = true;
                    } else error(a.line, "too many arguments to Find");
                } else if (ieq(a.name, "akElement") || ieq(a.name, "aiElement") || ieq(a.name, "afElement") ||
                           ieq(a.name, "asElement") || ieq(a.name, "abElement") || ieq(a.name, "element")) {
                    value = convert(expr(*a.value), elem, a.line);
                    haveValue = true;
                } else if (ieq(a.name, "aiStartIndex") || ieq(a.name, "startIndex")) {
                    start = convert(expr(*a.value), type("Int"), a.line);
                    haveStart = true;
                } else error(a.line, "Find has no parameter " + a.name);
            }
            if (!haveValue) error(e.line, "Find needs the element to look for");
            if (!haveStart) {
                start.v = Value::integer(c.kind == Callee::ArrayFind ? 0 : -1);
                start.type = type("Int");
            }
            release(c.obj);
            release(value);
            release(start);
            Operand t = alloc(type("Int"));
            emit(c.kind == Callee::ArrayFind ? Opcode::ArrayFindElement : Opcode::ArrayRFindElement, {c.obj.v, t.v, value.v, start.v});
            return t;
        }

        // Arguments: positional, then named; defaults fill the rest.
        const FuncInfo& fn = *c.fn;
        std::vector<Operand> args(fn.params.size());
        std::vector<bool> given(fn.params.size(), false);
        size_t pos = 0;
        for (const Arg& a : e.args) {
            size_t index;
            if (a.name.empty()) {
                if (pos >= fn.params.size()) error(a.line, "too many arguments to " + fn.name);
                index = pos++;
            } else {
                index = fn.params.size();
                for (size_t i = 0; i < fn.params.size(); ++i) {
                    if (ieq(fn.params[i].name, a.name)) index = i;
                }
                if (index == fn.params.size()) error(a.line, fn.name + " has no parameter " + a.name);
            }
            if (given[index]) error(a.line, "argument " + fn.params[index].name + " given twice");
            given[index] = true;
            args[index] = expr(*a.value);
        }
        for (size_t i = 0; i < fn.params.size(); ++i) {
            if (given[i]) continue;
            const ParamInfo& p = fn.params[i];
            if (!p.hasDefault) error(e.line, "argument " + p.name + " of " + fn.name + " is missing");
            args[i] = default_value(p);
        }
        // The implicit casts of the arguments come after every argument has
        // been evaluated, in parameter order, each in a temp of its own that
        // lives until the call (vanilla AnimalTrainerSystemScript: PROPGET,
        // CAST of the second argument, then CAST of the first;
        // BYOHHouseBanditAttackPacifyOnLoad: two None arguments, two temps).
        // An argument's cast keeps its source temp allocated too: the source
        // is released with the other argument values after the casts
        // (vanilla DLC1_TIF__02012F5C: `RemoveItem(item, 1, false,
        // X.GetActorRef())` casts item and the actor into two fresh temps).
        std::vector<Operand> casts(fn.params.size());
        std::vector<bool> isCast(fn.params.size(), false);
        for (size_t i = 0; i < fn.params.size(); ++i) {
            if (!given[i] || same_type(args[i].type, fn.params[i].type)) continue;
            if (!implicit_ok(args[i].type, fn.params[i].type)) {
                error(e.line, "cannot convert " + type_text(args[i].type) + " to " + type_text(fn.params[i].type) + " without a cast");
            }
            casts[i] = alloc(fn.params[i].type);
            emit(Opcode::Cast, {casts[i].v, args[i].v});
            isCast[i] = true;
        }
        // The cast temps are released first (in order), then the other
        // argument temps left to right, then the result temp is taken, then
        // the receiver is released after the call. Vanilla:
        // DA13SpellbreakerStaticFX `RandomFloat(0.5, 1)` casts 1 into t0 and
        // the result lands in t0; dunHunterGuardians `TranslateTo(X, Y, Z +
        // offset, 180, 180, 180, ...)` leaves the list as casts then values;
        // BYOH_QF_BYOHHouseHjaalmarch keeps the receiver of GetRef() while
        // PlaceAtMe's result takes a fresh temp; ArenaCrowdQuestScript's
        // receiver and argument temps of consecutive calls alternate.
        for (size_t i = 0; i < args.size(); ++i) {
            if (isCast[i]) release(casts[i]);
        }
        for (Operand& a : args) release(a);
        Operand dest;
        if (is_none(fn.ret)) {
            dest.v = none_var();
            dest.type = type("none");
        } else {
            dest = alloc(fn.ret);
        }
        if (haveObj) release(c.obj);
        std::vector<Value> ops;
        switch (c.kind) {
        case Callee::Method:
            ops = {id(e.text), c.obj.v, dest.v};
            break;
        case Callee::Parent:
            ops = {id(e.text), dest.v};
            break;
        case Callee::Static:
            ops = {id(lower(c.className)), id(e.text), dest.v};
            break;
        default: break;
        }
        for (size_t i = 0; i < args.size(); ++i) ops.push_back(isCast[i] ? casts[i].v : args[i].v);
        emit(c.kind == Callee::Method ? Opcode::CallMethod : c.kind == Callee::Parent ? Opcode::CallParent : Opcode::CallStatic,
             std::move(ops));
        (void)discard;
        return dest;
    }

    // A function of a script, through its parent chain, or one of the two
    // the compiler gives every script (GetState / GotoState).
    const FuncInfo* find_method(const ScriptInfo& s, std::string_view name, const ScriptInfo** owner) {
        if (const FuncInfo* f = m_prog.find_function(s, name, owner)) return f;
        if (ieq(name, "GetState") || ieq(name, "GotoState")) {
            static const FuncInfo kGetState = [] {
                FuncInfo f;
                f.name = "GetState";
                f.ret = TypeRef{"String", false};
                return f;
            }();
            static const FuncInfo kGotoState = [] {
                FuncInfo f;
                f.name = "GotoState";
                ParamInfo p;
                p.name = "newState";
                p.type = TypeRef{"String", false};
                f.params.push_back(p);
                return f;
            }();
            if (owner) *owner = &s;
            return ieq(name, "GetState") ? &kGetState : &kGotoState;
        }
        return nullptr;
    }

    // An omitted argument: the declared default, as the compiler fills it in
    // (an object default is the raw None literal).
    Operand default_value(const ParamInfo& p) {
        Operand o;
        o.type = p.type;
        switch (p.def.kind) {
        case LitKind::None: o.v = Value::none(); break;
        case LitKind::Int: o.v = is_float(p.type) ? Value::floating(static_cast<float>(p.def.i)) : Value::integer(p.def.i); break;
        case LitKind::Float: o.v = Value::floating(p.def.f); break;
        case LitKind::Bool: o.v = Value::boolean(p.def.b); break;
        case LitKind::String: o.v = Value::string(m_c.intern(p.def.s)); break;
        }
        return o;
    }

    // --- statements -------------------------------------------------------------------

    void statement(const Stmt& st) {
        m_line = st.line;
        switch (st.kind) {
        case StmtKind::Decl: {
            declare_local(st);
            if (st.init) {
                const Slot* s = find_local(st.declName);
                Operand v = convert(expr(*st.init), s->type, st.line);
                emit(Opcode::Assign, {id(s->pexName), v.v});
                release(v);
            }
            return;
        }
        case StmtKind::Assign: assign(st); return;
        case StmtKind::ExprStmt: {
            Operand v = expr(*st.expr);
            release(v);
            return;
        }
        case StmtKind::Return: {
            if (st.expr) {
                if (is_none(m_fi.ret)) error(st.line, "this function does not return a value");
                // The compiler never releases the temp a Return consumes
                // (vanilla ArenaScript.PickNextFight: every `Return f()` gets
                // a fresh temp); a cast's temp is released at the cast as
                // always (CCHouseQuestScript: `Return None` then
                // `Return arr[i]` share the temp).
                Operand v = convert(expr(*st.expr), m_fi.ret, st.line);
                emit(Opcode::Return, {v.v});
            } else {
                if (!is_none(m_fi.ret)) error(st.line, "a value must be returned");
                emit(Opcode::Return, {Value::none()});
            }
            return;
        }
        case StmtKind::If: if_statement(st); return;
        case StmtKind::While: {
            const int top = new_label(), exit = new_label();
            place(top);
            Operand c = condition(*st.expr);
            jump(Opcode::JmpF, &c, exit);
            block(st.body);
            m_line = st.endLine;
            jump(Opcode::Jmp, nullptr, top);
            place(exit);
            release(c);
            return;
        }
        }
    }

    void block(const std::vector<StmtPtr>& body) {
        m_scopes.emplace_back();
        for (const StmtPtr& s : body) statement(*s);
        m_scopes.pop_back();
    }

    // A condition: a single non-bool value is tested as it is; && / || and
    // comparisons produce a bool temp.
    Operand condition(const Expr& e) { return expr(e); }

    void if_statement(const Stmt& st) {
        const int end = new_label();
        int next = new_label();
        Operand c = condition(*st.expr);
        jump(Opcode::JmpF, &c, next);
        block(st.body);
        jump(Opcode::Jmp, nullptr, end);
        for (const ElseIf& ei : st.elseIfs) {
            place(next);
            next = new_label();
            m_line = ei.line;
            Operand c2 = condition(*ei.cond);
            jump(Opcode::JmpF, &c2, next);
            block(ei.body);
            jump(Opcode::Jmp, nullptr, end);
            release(c2);  // an ElseIf holds its temp only through its own branch
        }
        place(next);
        if (st.hasElse) block(st.elseBody);
        place(end);
        release(c);  // the If holds its temp until EndIf
    }

    void assign(const Stmt& st) {
        const Expr& target = *st.target;
        // Compound assignment: x op= e is x = x op e.
        ExprPtr rhsOwned;
        const Expr* rhs = st.value.get();
        if (st.op != "=") {
            auto bin = std::make_unique<Expr>(ExprKind::Binary, st.line, 0);
            bin->text = st.op.substr(0, 1);
            bin->a = clone(target);
            bin->b = clone(*st.value);
            rhsOwned = std::move(bin);
            rhs = rhsOwned.get();
        }
        if (target.kind == ExprKind::Ident) {
            Name n = resolve(target.text, st.line);
            switch (n.kind) {
            case NameKind::Local:
            case NameKind::Variable:
            case NameKind::AutoProp: {
                Operand v = convert(expr(*rhs), n.type, st.line);
                emit(Opcode::Assign, {id(n.pexName), v.v});
                release(v);
                return;
            }
            case NameKind::Property: {
                if (!n.prop->writable) error(st.line, "property " + n.prop->name + " cannot be written");
                Operand slot = alloc(n.type);
                Operand v = convert(expr(*rhs), n.type, st.line);
                emit(Opcode::Assign, {slot.v, v.v});
                emit(Opcode::PropSet, {id(target.text), id("self"), slot.v});
                release(slot);
                release(v);
                return;
            }
            default: error(st.line, "cannot assign to " + target.text);
            }
        }
        if (target.kind == ExprKind::Member) {
            // obj.Prop = v: the value temp first, then the value, then the object.
            Operand probe = receiver_type_only(*target.a, st.line);
            const ScriptInfo* info = is_object(probe.type) ? m_prog.find(probe.type.name) : nullptr;
            if (!info) error(st.line, "a " + type_text(probe.type) + " has no properties");
            const PropInfo* p = m_prog.find_property(*info, target.text);
            if (!p) error(st.line, "type " + probe.type.name + " has no property " + target.text);
            if (!p->writable) error(st.line, "property " + target.text + " cannot be written");
            // The object is generated first (its temps come first), the
            // value temp and the value next, and the code comes out value
            // first (vanilla BYOH_TIF__01003DEC: temps quest, script, then
            // the two floats; the quest call emitted after the value).
            const Mark m = mark();
            Operand obj = receiver(*target.a, st.line);
            Cut objCode = cut_from(m);
            // The object's temps are free again before the value is generated
            // (vanilla TIF__000534EE: `(GetOwningQuest() as X).Patronage += 3`
            // computes the right side's copy of the object into the same
            // temps); the code that recomputes the object comes after the value.
            release(obj);
            Operand slot = alloc(p->type);
            Operand v = convert(expr(*rhs), p->type, st.line);
            emit(Opcode::Assign, {slot.v, v.v});
            paste(objCode);
            emit(Opcode::PropSet, {id(target.text), obj.v, slot.v});
            // The slot is released before the value's temp (vanilla
            // BFBPuzzle01Pillar: two such statements in a row keep slot t9,
            // value t10).
            release(slot);
            release(v);
            return;
        }
        if (target.kind == ExprKind::Index) {
            Operand probe = receiver_type_only(*target.a, st.line);
            if (!probe.type.isArray) error(st.line, "indexing a value that is not an array");
            const TypeRef elem{probe.type.name, false};
            const Mark m = mark();
            Operand arr = expr(*target.a);
            Operand idx = convert(expr(*target.b), type("Int"), st.line);
            Cut arrCode = cut_from(m);
            release(arr);
            release(idx);
            Operand slot = alloc(elem);
            Operand v = convert(expr(*rhs), elem, st.line);
            emit(Opcode::Assign, {slot.v, v.v});
            paste(arrCode);
            emit(Opcode::ArraySetElement, {arr.v, idx.v, slot.v});
            release(slot);
            release(v);
            return;
        }
        error(st.line, "the left side of an assignment must be a variable, property or array element");
    }

    // The type an expression would have, without emitting code (names only;
    // anything else is evaluated for real, which is wrong for side effects,
    // and reported).
    Operand receiver_type_only(const Expr& e, int line) {
        if (e.kind == ExprKind::Ident) {
            Name n = resolve(e.text, line);
            Operand o;
            o.type = n.type;
            if (n.kind == NameKind::Type) error(line, "type " + e.text + " used as an object");
            return o;
        }
        if (e.kind == ExprKind::Self) return self_operand(e.text);
        // Evaluate and discard: only the type is needed here, the receiver is
        // evaluated again at its real position (unsettled: a receiver with
        // side effects would run twice).
        const size_t codeSize = m_pf.code.size(), lineCount = m_lines.size();
        auto freeSave = m_free;
        const int counter = m_c.tempCounter;
        const size_t localCount = m_pf.locals.size();
        Operand o = expr(e);
        m_pf.code.resize(codeSize);
        m_lines.resize(lineCount);
        m_free = freeSave;
        m_c.tempCounter = counter;
        m_pf.locals.resize(localCount);
        o.isTemp = false;
        return o;
    }

    static ExprPtr clone(const Expr& e) {
        auto c = std::make_unique<Expr>(e.kind, e.line, e.column);
        c->text = e.text;
        c->lit = e.lit;
        c->type = e.type;
        if (e.a) c->a = clone(*e.a);
        if (e.b) c->b = clone(*e.b);
        for (const Arg& a : e.args) {
            Arg g;
            g.name = a.name;
            g.line = a.line;
            g.value = clone(*a.value);
            c->args.push_back(std::move(g));
        }
        return c;
    }

    ObjectContext& m_c;
    const Function& m_f;
    const FuncInfo& m_fi;
    pex::Function& m_pf;
    std::vector<uint16_t>& m_lines;
    Program& m_prog;
    const ScriptInfo& m_self;
    int m_line = 0;
    std::vector<std::map<std::string, Slot>> m_scopes;
    std::unordered_map<const Stmt*, std::string> m_mangledNames;  // Decl -> ::mangled_<name>_N
    std::map<std::string, std::deque<StrIdx>> m_free;
    bool m_haveNoneVar = false;
    std::vector<size_t> m_labels;
    std::vector<std::pair<size_t, int>> m_jumps;
};

}  // namespace

void compile_body(ObjectContext& ctx, const Function& f, const FuncInfo& fi, pex::Function& pf, std::vector<uint16_t>& lines) {
    FunctionCompiler fc(ctx, f, fi, pf, lines);
    fc.run();
}

}  // namespace papyrus::psc
