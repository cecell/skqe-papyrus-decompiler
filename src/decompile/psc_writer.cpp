// Decompiler passes P7-P8 and the object-level output: cleanup rewrites,
// cast elision, declarations, precedence-aware rendering, the Event /
// Function choice, property forms, and the per-function failure stub.
// SPDX-License-Identifier: GPL-3.0-only
#include "ir.hpp"
#include "papyrus/decompile.hpp"
#include "papyrus/layout.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace papyrus::pex {

const char* to_string(FnStatus s) noexcept {
    switch (s) {
    case FnStatus::Ok: return "ok";
    case FnStatus::OkMaterialisedTemps: return "ok (named temps)";
    case FnStatus::Failed: return "failed";
    }
    return "?";
}

namespace {

using namespace detail;

const char* const kEventNames[] = {
#include "event_names.inc"
};

bool is_known_event(const std::string& lowerName) {
    return std::binary_search(std::begin(kEventNames), std::end(kEventNames), lowerName.c_str(),
                              [](const char* a, const char* b) { return std::strcmp(a, b) < 0; });
}

std::string lower(std::string_view v) { return FnContext::lower(v); }

bool starts_with_ci(std::string_view v, std::string_view p) {
    if (v.size() < p.size()) return false;
    for (size_t i = 0; i < p.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(v[i])) != std::tolower(static_cast<unsigned char>(p[i]))) return false;
    }
    return true;
}

std::string float_text(float f) {
    if (std::isnan(f)) return "0.0";
    if (std::isinf(f)) return f > 0 ? "340282346638528859811704183484516925440.0" : "-340282346638528859811704183484516925440.0";
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

std::string quoted(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': break;
        default: out += c;
        }
    }
    return out + "\"";
}

// Operator binding strength, high binds tighter.
int prec_of(const Expr& e) {
    switch (e.kind) {
    case ExprKind::Or: return 1;
    case ExprKind::And: return 2;
    case ExprKind::Binary:
        if (e.op == "==" || e.op == "!=" || e.op == "<" || e.op == "<=" || e.op == ">" || e.op == ">=") return 3;
        if (e.op == "+" || e.op == "-") return 4;
        return 5;
    case ExprKind::Unary: return 6;
    case ExprKind::Cast: return 7;
    default: return 8;
    }
}

bool is_zero_int(const Expr& e) { return e.kind == ExprKind::Lit && e.lit.type == ValueType::Integer && e.lit.as_int() == 0; }
bool is_minus_one_int(const Expr& e) { return e.kind == ExprKind::Lit && e.lit.type == ValueType::Integer && e.lit.as_int() == -1; }

class Writer {
public:
    Writer(const Script& s, const DecompileOptions& opt) : m_s(s), m_opt(opt) {
        for (const UserFlag& f : s.userFlags) {
            const std::string n = lower(s.str(f.name));
            if (n == "hidden") m_hiddenMask |= 1u << f.bit;
            if (n == "conditional") m_conditionalMask |= 1u << f.bit;
        }
    }

    DecompileResult run() {
        // The compiler stores strings as UTF-8 and reads a source file by its
        // byte order mark; without one it assumes the system code page and
        // re-encodes, which changes every non-ASCII doc string and literal.
        m_out = "\xEF\xBB\xBF";
        for (const Object& o : m_s.objects) object(o);
        DecompileResult r;
        r.source = std::move(m_out);
        r.functions = std::move(m_reports);
        return r;
    }

private:
    // --- text helpers ---------------------------------------------------------
    void line(int depth, const std::string& text) {
        m_out.append(size_t(depth) * 4, ' ');
        m_out += text;
        m_out += '\n';
    }
    std::string str(StrIdx i) const { return std::string(m_s.str(i)); }

    // A type name with the spelling a declaration in the file used, when the
    // compiler lowercased this occurrence (temps, cast targets).
    std::string nice_type(std::string_view raw) const {
        if (raw.empty()) return "";
        const std::string key = lower(raw);
        auto it = m_niceTypes.find(key);
        if (it != m_niceTypes.end()) return it->second;
        std::string best(raw);
        bool hasUpper = false;
        for (char c : best) hasUpper = hasUpper || std::isupper(static_cast<unsigned char>(c));
        if (!hasUpper) {
            for (const std::string& s : m_s.strings) {
                if (s.size() == raw.size() && lower(s) == key) {
                    bool up = false;
                    for (char c : s) up = up || std::isupper(static_cast<unsigned char>(c));
                    if (up) {
                        best = s;
                        break;
                    }
                }
            }
            if (best == std::string(raw) && !best.empty()) best[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(best[0])));
        }
        m_niceTypes[key] = best;
        return best;
    }
    std::string nice_type(StrIdx i) const { return nice_type(m_s.str(i)); }

    std::string flags_text(uint32_t userFlags, bool allowConditional) const {
        std::string out;
        if (m_hiddenMask && (userFlags & m_hiddenMask)) out += " Hidden";
        if (allowConditional && m_conditionalMask && (userFlags & m_conditionalMask)) out += " Conditional";
        return out;
    }

    std::string literal(const Value& v) const {
        switch (v.type) {
        case ValueType::None: return "None";
        case ValueType::Identifier: return str(v.str());
        case ValueType::String: return quoted(m_s.str(v.str()));
        case ValueType::Integer: return std::to_string(v.as_int());
        case ValueType::Float: return float_text(v.as_float());
        case ValueType::Bool: return v.as_bool() ? "True" : "False";
        }
        return "None";
    }

    // --- object ---------------------------------------------------------------
    void object(const Object& o) {
        std::string head = "Scriptname " + str(o.name);
        if (!str(o.parent).empty()) head += " extends " + str(o.parent);
        head += flags_text(o.userFlags, true);
        line(0, head);
        if (!str(o.docString).empty()) line(0, "{" + str(o.docString) + "}");
        line(0, "");

        // Script variables (not the backing variables of auto properties).
        bool any = false;
        for (const Variable& v : o.variables) {
            if (starts_with_ci(m_s.str(v.name), "::")) continue;
            std::string text = nice_type(v.type) + " " + str(v.name);
            if (v.initial.type != ValueType::None) text += " = " + literal(v.initial);
            text += flags_text(v.userFlags, true);
            line(0, text);
            any = true;
        }
        if (any) line(0, "");

        // Properties, default-state functions and state blocks in source
        // order. The debug records give the order within each group; the
        // compiler's temp counter, which runs across every body in source
        // order, tells how the groups interleave (a property with accessors
        // declared after a function has higher temp numbers). Items without
        // temps follow the previous item.
        const SourceLayout layout = source_layout(m_s, o);
        struct Section {
            int kind;  // 0 property, 1 default-state function, 2 state block
            size_t index;  // property index / function index / state index
            long long key;
        };
        std::vector<Section> sections;
        auto temp_key = [&](const Function& f) -> long long {
            long long best = -1;
            for (const Param& p : f.locals) {
                const std::string_view n = m_s.str(p.name);
                if (n.size() > 6 && starts_with_ci(n, "::temp")) {
                    long long v = 0;
                    bool digits = true;
                    for (size_t i = 6; i < n.size(); ++i) {
                        if (!std::isdigit(static_cast<unsigned char>(n[i]))) { digits = false; break; }
                        v = v * 10 + (n[i] - '0');
                    }
                    if (digits && (best < 0 || v < best)) best = v;
                }
            }
            return best;
        };
        for (size_t pi : layout.properties) {
            const Property& p = o.properties[pi];
            long long key = -1;
            if (p.getter) key = temp_key(*p.getter);
            if (p.setter) {
                const long long k = temp_key(*p.setter);
                if (k >= 0 && (key < 0 || k < key)) key = k;
            }
            sections.push_back({0, pi, key});
        }
        size_t defaultState = o.states.size();
        for (size_t si : layout.states) {
            if (str(o.states[si].name).empty()) {
                defaultState = si;
                break;
            }
        }
        if (defaultState < o.states.size()) {
            for (size_t fi : layout.functions[defaultState]) {
                const NamedFunction& f = o.states[defaultState].functions[fi];
                const std::string n = lower(m_s.str(f.name));
                if (!m_opt.keepGeneratedFunctions && (n == "getstate" || n == "gotostate")) continue;
                sections.push_back({1, fi, temp_key(f.body)});
            }
        }
        for (size_t si : layout.states) {
            if (si == defaultState) continue;
            long long key = -1;
            for (size_t fi : layout.functions[si]) {
                const long long k = temp_key(o.states[si].functions[fi].body);
                if (k >= 0 && (key < 0 || k < key)) key = k;
            }
            sections.push_back({2, si, key});
        }
        // Items without temps take the key of the previous item.
        long long last = -1;
        for (Section& s : sections) {
            if (s.key < 0) s.key = last;
            else last = s.key;
        }
        std::stable_sort(sections.begin(), sections.end(), [](const Section& a, const Section& b) { return a.key < b.key; });

        int prevKind = -1;
        for (const Section& sec : sections) {
            if (sec.kind == 0) {
                property(o, o.properties[sec.index]);
                prevKind = 0;
                continue;
            }
            if (prevKind == 0) line(0, "");
            if (sec.kind == 1) {
                function(o, o.states[defaultState], o.states[defaultState].functions[sec.index], 0);
            } else {
                const State& st = o.states[sec.index];
                const bool isAuto = lower(m_s.str(o.autoState)) == lower(m_s.str(st.name));
                line(0, std::string(isAuto ? "Auto State " : "State ") + str(st.name));
                for (size_t fi : layout.functions[sec.index]) function(o, st, st.functions[fi], 1);
                line(0, "EndState");
                line(0, "");
            }
            prevKind = sec.kind;
        }
    }

    const Variable* variable(const Object& o, StrIdx name) const {
        for (const Variable& v : o.variables) {
            if (v.name == name || lower(m_s.str(v.name)) == lower(m_s.str(name))) return &v;
        }
        return nullptr;
    }

    void property(const Object& o, const Property& p) {
        const std::string type = nice_type(p.type);
        const std::string name = str(p.name);
        if (p.is_auto()) {
            std::string text = type + " Property " + name;
            uint32_t varFlags = 0;
            if (const Variable* v = variable(o, p.autoVar)) {
                if (v->initial.type != ValueType::None) text += " = " + literal(v->initial);
                varFlags = v->userFlags;
            }
            text += " Auto";
            text += flags_text(p.userFlags, false);
            text += flags_text(varFlags & m_conditionalMask, true);
            line(0, text);
            if (!str(p.docString).empty()) line(0, "{" + str(p.docString) + "}");
            return;
        }
        // AutoReadOnly: a getter that returns one literal, no setter.
        if (p.getter && !p.setter && p.getter->code.size() == 1 && p.getter->code[0].op == Opcode::Return &&
            p.getter->code[0].args[0].type != ValueType::Identifier && p.getter->code[0].args[0].type != ValueType::None) {
            line(0, type + " Property " + name + " = " + literal(p.getter->code[0].args[0]) + " AutoReadOnly" +
                        flags_text(p.userFlags, false));
            if (!str(p.docString).empty()) line(0, "{" + str(p.docString) + "}");
            report_trivial(o, name + ".get", *p.getter);
            return;
        }
        line(0, type + " Property " + name + flags_text(p.userFlags, false));
        if (!str(p.docString).empty()) line(1, "{" + str(p.docString) + "}");
        if (p.getter) accessor(o, p, *p.getter, true);
        if (p.setter) accessor(o, p, *p.setter, false);
        line(0, "EndProperty");
    }

    void report_trivial(const Object& o, const std::string& name, const Function& f) {
        FunctionReport r;
        r.object = str(o.name);
        r.name = name;
        r.status = FnStatus::Ok;
        r.instructions = f.code.size();
        m_reports.push_back(std::move(r));
    }

    void accessor(const Object& o, const Property& p, const Function& f, bool getter) {
        std::string head;
        if (getter) {
            head = nice_type(f.returnType) + " Function Get()";
        } else {
            head = "Function Set(" + nice_type(f.params.empty() ? p.type : f.params[0].type) + " " +
                   (f.params.empty() ? std::string("value") : str(f.params[0].name)) + ")";
        }
        line(1, head);
        body(o, f, str(p.name) + (getter ? ".get" : ".set"), "", 2);
        line(1, "EndFunction");
    }

    bool is_event(const Function& f, const std::string& name) const {
        if (f.is_global() || f.is_native()) return false;
        // An event never returns a value: a function that does is a function
        // whatever its name (UIExtensions' OpenMenu, another mod's Register share
        // names with events of other scripts).
        if (lower(m_s.str(f.returnType)) != "none") return false;
        const std::string n = lower(name);
        if (is_known_event(n)) return true;
        return starts_with_ci(n, "on");
    }

    void function(const Object& o, const State& st, const NamedFunction& nf, int depth) {
        const Function& f = nf.body;
        const std::string name = str(nf.name);
        const bool event = is_event(f, name);
        std::string head;
        if (event) {
            head = "Event " + name + "(";
        } else {
            const std::string rt = lower(m_s.str(f.returnType));
            head = (rt == "none" ? std::string() : nice_type(f.returnType) + " ") + "Function " + name + "(";
        }
        for (size_t i = 0; i < f.params.size(); ++i) {
            if (i) head += ", ";
            head += nice_type(f.params[i].type) + " " + str(f.params[i].name);
            if (m_opt.optionalObjectParams) {
                // Defaults are not in the compiled file and change nothing in
                // it; declaring one for every parameter lets any call that
                // omitted an argument compile (the default itself never
                // matters: the compiler wrote the omitted value into the call).
                const std::string t = lower(m_s.str(f.params[i].type));
                if (t == "int") head += " = 0";
                else if (t == "float") head += " = 0.0";
                else if (t == "bool") head += " = False";
                else if (t == "string") head += " = \"\"";
                else head += " = None";
            }
        }
        head += ")";
        if (f.is_global()) head += " Global";
        if (f.is_native()) head += " Native";
        line(depth, head);
        if (f.is_native()) {
            if (!str(f.docString).empty()) line(depth, "{" + str(f.docString) + "}");
            report_trivial(o, name, f);
            line(depth, "");
            return;
        }
        body(o, f, name, str(st.name), depth + 1);
        line(depth, event ? "EndEvent" : "EndFunction");
        line(depth, "");
    }

    // --- function bodies --------------------------------------------------------

    struct Ctx {
        const FnContext* c = nullptr;
        const FunctionDebug* dbg = nullptr;
        std::map<StrIdx, std::string> names;  // display names of locals (after collision handling)
        bool returnsNone = true;
    };

    void body(const Object& o, const Function& f, const std::string& name, const std::string& state, int depth) {
        if (!str(f.docString).empty()) line(depth, "{" + str(f.docString) + "}");
        if (m_opt.declarationsOnly) {
            report_trivial(o, name, f);
            return;
        }
        FunctionReport rep;
        rep.object = str(o.name);
        rep.state = state;
        rep.name = name;
        rep.instructions = f.code.size();

        FnContext c(m_s, o, f, m_opt.resolver);
        Ctx ctx;
        ctx.c = &c;
        ctx.returnsNone = lower(m_s.str(f.returnType)) == "none";
        try {
            FunctionIR ir = lift_function(c);
            std::vector<StmtPtr> stmts = structure_function(c, ir);
            if (!ir.materialised.empty()) rep.status = FnStatus::OkMaterialisedTemps;
            cleanup(stmts);
            std::vector<StmtPtr> withDecls = declare_locals(c, ir, stmts, ctx);
            for (const StmtPtr& st : withDecls) statement(*st, depth, ctx);
        } catch (const DecompileError& e) {
            rep.status = FnStatus::Failed;
            rep.reason = e.what();
            rep.ip = static_cast<uint32_t>(e.ip);
            stub(f, e, depth, ctx);
        }
        m_reports.push_back(std::move(rep));
    }

    void stub(const Function& f, const DecompileError& e, int depth, const Ctx& ctx) {
        line(depth, "; DECOMPILE FAILED: " + std::string(e.what()) + " at instruction " + std::to_string(e.ip));
        line(depth, ";/");
        for (size_t ip = 0; ip < f.code.size(); ++ip) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "%4zu: ", ip);
            line(depth, std::string(buf) + format_instruction(m_s, f.code[ip], ip));
        }
        line(depth, "/;");
        const std::string rt = lower(m_s.str(f.returnType));
        if (rt == "none") return;
        std::string v = "None";
        if (rt == "int") v = "0";
        else if (rt == "float") v = "0.0";
        else if (rt == "bool") v = "False";
        else if (rt == "string") v = "\"\"";
        line(depth, "Return " + v);
        (void)ctx;
    }

    // --- P7: tree cleanup ------------------------------------------------------------

    static void cleanup(std::vector<StmtPtr>& stmts) {
        for (StmtPtr& st : stmts) {
            if (st->rhs) cleanup_expr(st->rhs);
            if (st->lhs) cleanup_expr(st->lhs);
            cleanup(st->body);
            cleanup(st->elseBody);
        }
    }

    static size_t max_ip(const Expr& e) {
        size_t m = e.ip;
        for (const ExprPtr& k : e.kids) m = std::max(m, max_ip(*k));
        return m;
    }

    static void cleanup_expr(ExprPtr& e) {
        for (ExprPtr& k : e->kids) cleanup_expr(k);
        // A cast the compiler inserted on the left operand of a comparison
        // (or an earlier call argument) is emitted after the operands to its
        // right were evaluated; an explicit one comes first. So a Cast child
        // produced after a later sibling was implicit: do not print it.
        if (e->kind == ExprKind::Binary || e->kind == ExprKind::Call) {
            for (size_t i = 0; i + 1 < e->kids.size(); ++i) {
                Expr& k = *e->kids[i];
                if (k.kind != ExprKind::Cast || k.ip == 0) continue;
                size_t later = 0;
                for (size_t j = i + 1; j < e->kids.size(); ++j) {
                    const Expr& s = *e->kids[j];
                    if (s.kind == ExprKind::Lit || s.kind == ExprKind::Var) continue;  // no instruction of their own
                    later = std::max(later, max_ip(s));
                }
                if (later != 0 && k.ip > later) k.implicitCast = true;
                // For comparison operands an implicit cast is deferred, so one
                // evaluated in operand order was written. Call arguments get
                // their implicit casts in order, so nothing follows from that.
                else if (later != 0 && k.ip < later && e->kind == ExprKind::Binary) k.explicitCast = true;
            }
        }
        // !(a == b) -> a != b
        if (e->kind == ExprKind::Unary && e->op == "!" && e->kids.size() == 1 && e->kids[0]->kind == ExprKind::Binary &&
            e->kids[0]->op == "==") {
            ExprPtr inner = std::move(e->kids[0]);
            inner->op = "!=";
            e = std::move(inner);
        }
    }

    // --- declarations ---------------------------------------------------------------

    // Every user local is declared in the innermost statement list that
    // contains all its references (so two locals that shared a name in
    // sibling blocks get the same name back and the compiler mangles them
    // again), at its first assignment there when that assignment is a direct
    // statement of that list, otherwise as a bare declaration before the
    // first statement that mentions it. A list inside a loop is not used
    // unless that first statement is the assignment itself, so a value
    // carried across iterations keeps its declaration outside the loop.
    struct PathStep {
        std::vector<StmtPtr>* list;
        size_t index;
        bool loopBody;
    };
    using Path = std::vector<PathStep>;

    static void collect_refs(std::vector<StmtPtr>& list, Path& path, bool loopBody,
                             std::map<StrIdx, std::vector<Path>>& refs) {
        for (size_t i = 0; i < list.size(); ++i) {
            Stmt& st = *list[i];
            path.push_back({&list, i, loopBody});
            std::vector<StrIdx> names;
            if (st.lhs) collect_vars(*st.lhs, names);
            if (st.rhs) collect_vars(*st.rhs, names);
            for (StrIdx n : names) refs[n].push_back(path);
            collect_refs(st.body, path, loopBody || st.kind == StmtKind::While, refs);
            collect_refs(st.elseBody, path, loopBody, refs);
            path.pop_back();
        }
    }

    std::vector<StmtPtr> declare_locals(const FnContext& c, const FunctionIR& ir, std::vector<StmtPtr>& stmts, Ctx& ctx) {
        struct Local {
            StrIdx name;
            std::string type;
        };
        std::vector<Local> locals;
        for (const Param& p : c.f.locals) {
            if (c.is_temp(p.name) || c.is_nonevar(p.name)) continue;
            locals.push_back({p.name, nice_type(p.type)});
        }
        for (const auto& m : ir.materialised) locals.push_back({m.first, nice_type(m.second)});

        std::map<StrIdx, std::vector<Path>> refs;
        Path path;
        collect_refs(stmts, path, false, refs);

        // Where each local is declared: (list, index), and whether the
        // statement there is its own first assignment.
        struct Placement {
            std::vector<StmtPtr>* list = nullptr;
            size_t index = 0;
            bool initHere = false;
            size_t depth = 0;
        };
        std::vector<Placement> where(locals.size());
        for (size_t li = 0; li < locals.size(); ++li) {
            const auto it = refs.find(locals[li].name);
            Placement& pl = where[li];
            if (it == refs.end() || it->second.empty()) {
                pl.list = &stmts;  // never referenced: declare at the top
                continue;
            }
            const std::vector<Path>& ps = it->second;
            // Longest common prefix of lists.
            size_t depth = 0;
            for (;; ++depth) {
                bool same = true;
                for (const Path& p : ps) {
                    if (depth >= p.size() || p[depth].list != ps[0][depth].list) {
                        same = false;
                        break;
                    }
                }
                if (!same) break;
            }
            // depth >= 1 (all paths start in `stmts`). Scope = list at depth-1.
            size_t level = depth - 1;
            auto first_index = [&](size_t lv) {
                size_t m = ps[0][lv].index;
                for (const Path& p : ps) m = std::min(m, p[lv].index);
                return m;
            };
            for (;;) {
                std::vector<StmtPtr>* list = ps[0][level].list;
                const size_t idx = first_index(level);
                Stmt& st = *(*list)[idx];
                const bool isAssign = st.kind == StmtKind::Assign && st.lhs->kind == ExprKind::Var &&
                                      st.lhs->name == locals[li].name && count_reads(*st.rhs, locals[li].name) == 0;
                if (isAssign || level == 0 || !ps[0][level].loopBody) {
                    pl.list = list;
                    pl.index = idx;
                    pl.initHere = isAssign;
                    pl.depth = level;
                    break;
                }
                --level;  // inside a loop and not initialised here: move out
            }
        }

        // A declaration placed in an else part that holds one If would turn
        // `ElseIf` into `Else` + `If`; put it before the outer If instead
        // (where a declaration stands changes nothing in the compiled code).
        for (Placement& pl : where) {
            for (;;) {
                if (pl.list->size() != 1 || (*pl.list)[0]->kind != StmtKind::If) break;
                std::vector<StmtPtr>* parent = nullptr;
                size_t at = 0;
                if (!find_else_parent(stmts, *pl.list, parent, at)) break;
                pl.list = parent;
                pl.index = at;
                pl.initHere = false;
                if (pl.depth) --pl.depth;
            }
        }

        // Display names: equal names are fine only in disjoint scopes.
        std::set<std::string> used;
        for (const Param& p : c.f.params) used.insert(lower(m_s.str(p.name)));
        for (const Property& p : c.o.properties) used.insert(lower(m_s.str(p.name)));
        for (const Variable& v : c.o.variables) used.insert(lower(m_s.str(v.name)));
        std::vector<std::string> names(locals.size());
        for (size_t li = 0; li < locals.size(); ++li) {
            const std::string base = c.display_name(locals[li].name);
            std::string cand = base;
            int n = 1;
            for (;;) {
                bool clash = used.count(lower(cand)) > 0;
                for (size_t lj = 0; lj < li && !clash; ++lj) {
                    if (lower(names[lj]) != lower(cand)) continue;
                    // Same name: allowed when neither scope contains the other.
                    const Placement& a = where[li];
                    const Placement& b = where[lj];
                    clash = a.list == b.list || contains(*a.list, *b.list) || contains(*b.list, *a.list);
                }
                if (!clash) break;
                cand = base + "_" + std::to_string(n++);
            }
            names[li] = cand;
            ctx.names[locals[li].name] = cand;
        }

        // First turn first assignments into declarations (no indices move),
        // then insert the bare declarations, deepest lists first and
        // descending indices within a list, so earlier insertions do not
        // move later ones.
        for (size_t li = 0; li < locals.size(); ++li) {
            Placement& pl = where[li];
            if (!pl.initHere) continue;
            Stmt& st = *(*pl.list)[pl.index];
            st.kind = StmtKind::Decl;
            st.declType = locals[li].type;
            st.declName = names[li];
            st.lhs.reset();
        }
        std::vector<size_t> order;
        for (size_t i = 0; i < locals.size(); ++i) {
            if (!where[i].initHere) order.push_back(i);
        }
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            if (where[a].list != where[b].list) return where[a].list > where[b].list;
            return where[a].index > where[b].index;
        });
        for (size_t li : order) {
            Placement& pl = where[li];
            auto d = std::make_unique<Stmt>(StmtKind::Decl);
            d->declType = locals[li].type;
            d->declName = names[li];
            pl.list->insert(pl.list->begin() + static_cast<long long>(std::min(pl.index, pl.list->size())), std::move(d));
        }
        std::vector<StmtPtr> out;
        for (StmtPtr& st : stmts) out.push_back(std::move(st));
        return out;
    }

    // Finds the If statement whose else part is `list`: its list and index.
    static bool find_else_parent(std::vector<StmtPtr>& outer, std::vector<StmtPtr>& list, std::vector<StmtPtr>*& parent,
                                 size_t& at) {
        for (size_t i = 0; i < outer.size(); ++i) {
            Stmt& st = *outer[i];
            if (&st.elseBody == &list) {
                parent = &outer;
                at = i;
                return true;
            }
            if (find_else_parent(st.body, list, parent, at) || find_else_parent(st.elseBody, list, parent, at)) return true;
        }
        return false;
    }

    // True when `inner` is a statement list nested somewhere inside `outer`.
    static bool contains(std::vector<StmtPtr>& outer, std::vector<StmtPtr>& inner) {
        if (&outer == &inner) return false;
        for (StmtPtr& st : outer) {
            if (&st->body == &inner || &st->elseBody == &inner) return true;
            if (contains(st->body, inner) || contains(st->elseBody, inner)) return true;
        }
        return false;
    }

    static void collect_vars(const Stmt& st, std::vector<StrIdx>& refs) {
        if (st.lhs) collect_vars(*st.lhs, refs);
        if (st.rhs) collect_vars(*st.rhs, refs);
        for (const StmtPtr& s : st.body) collect_vars(*s, refs);
        for (const StmtPtr& s : st.elseBody) collect_vars(*s, refs);
    }
    static void collect_vars(const Expr& e, std::vector<StrIdx>& refs) {
        if (e.kind == ExprKind::Var) refs.push_back(e.name);
        for (const ExprPtr& k : e.kids) collect_vars(*k, refs);
    }

    // --- statements -----------------------------------------------------------------

    std::string line_comment(const Stmt& st, const Ctx& ctx) const {
        if (!m_opt.lineComments || !ctx.dbg) return "";
        if (st.ip < ctx.dbg->lines.size()) return " ; #" + std::to_string(ctx.dbg->lines[st.ip]);
        return "";
    }

    void statement(const Stmt& st, int depth, const Ctx& ctx) {
        switch (st.kind) {
        case StmtKind::Decl: {
            std::string t = st.declType + " " + st.declName;
            if (st.rhs) t += " = " + expr(*st.rhs, 0, false, Context::Value, ctx);
            line(depth, t + line_comment(st, ctx));
            break;
        }
        case StmtKind::Expr: line(depth, expr(*st.rhs, 0, false, Context::Value, ctx) + line_comment(st, ctx)); break;
        case StmtKind::Assign: {
            if (st.lhs->kind == ExprKind::Var && ctx.c->is_nonevar(st.lhs->name)) {
                // "::NoneVar = f()": the discarded result of a call statement.
                line(depth, expr(*st.rhs, 0, false, Context::Value, ctx) + line_comment(st, ctx));
                break;
            }
            // x = x op e  ->  x op= e
            const Expr& r = *st.rhs;
            if (st.lhs->kind == ExprKind::Var && r.kind == ExprKind::Binary && r.kids.size() == 2 &&
                r.kids[0]->kind == ExprKind::Var && r.kids[0]->name == st.lhs->name &&
                (r.op == "+" || r.op == "-" || r.op == "*" || r.op == "/" || r.op == "%")) {
                line(depth, expr(*st.lhs, 0, false, Context::Value, ctx) + " " + r.op + "= " +
                                expr(*r.kids[1], 0, false, Context::Value, ctx) + line_comment(st, ctx));
                break;
            }
            line(depth, expr(*st.lhs, 0, false, Context::Value, ctx) + " = " + expr(*st.rhs, 0, false, Context::Value, ctx) +
                            line_comment(st, ctx));
            break;
        }
        case StmtKind::Return:
            if (ctx.returnsNone || !st.rhs) line(depth, "Return" + line_comment(st, ctx));
            else line(depth, "Return " + expr(*st.rhs, 0, false, Context::Value, ctx) + line_comment(st, ctx));
            break;
        case StmtKind::If: {
            line(depth, "If " + expr(*st.rhs, 0, false, Context::Condition, ctx) + line_comment(st, ctx));
            for (const StmtPtr& s : st.body) statement(*s, depth + 1, ctx);
            const Stmt* cur = &st;
            // ElseIf and a nested If differ only in temp allocation
            // (else_if_collapsible below, docs/COMPILER_PATTERNS.md "Temps").
            while (cur->elseBody.size() == 1 && cur->elseBody[0]->kind == StmtKind::If &&
                   else_if_collapsible(*cur->elseBody[0], ctx)) {
                cur = cur->elseBody[0].get();
                line(depth, "ElseIf " + expr(*cur->rhs, 0, false, Context::Condition, ctx) + line_comment(*cur, ctx));
                for (const StmtPtr& s : cur->body) statement(*s, depth + 1, ctx);
            }
            if (!cur->elseBody.empty()) {
                line(depth, "Else");
                for (const StmtPtr& s : cur->elseBody) statement(*s, depth + 1, ctx);
            }
            line(depth, "EndIf");
            break;
        }
        case StmtKind::While:
            line(depth, "While " + expr(*st.rhs, 0, false, Context::Condition, ctx) + line_comment(st, ctx));
            for (const StmtPtr& s : st.body) statement(*s, depth + 1, ctx);
            line(depth, "EndWhile");
            break;
        }
    }

    // The temp an instruction writes, or 0 (see analysis.cpp dest_operand).
    static StrIdx written_temp(const Instruction& ins, const Ctx& ctx) {
        int d = -1;
        switch (ins.op) {
        case Opcode::CallMethod: case Opcode::CallStatic: case Opcode::PropGet: d = 2; break;
        case Opcode::CallParent: case Opcode::ArrayFindElement: case Opcode::ArrayRFindElement: d = 1; break;
        case Opcode::Jmp: case Opcode::JmpT: case Opcode::JmpF: case Opcode::Return: case Opcode::PropSet:
        case Opcode::ArraySetElement: case Opcode::Nop: d = -1; break;
        default: d = 0; break;
        }
        if (d < 0 || size_t(d) >= ins.args.size() || ins.args[d].type != ValueType::Identifier) return 0;
        const StrIdx dest = ins.args[d].str();
        return ctx.c->is_temp(dest) ? dest : 0;
    }

    // ElseIf or a nested If inside Else? The compiled code is the same except
    // for temp allocation (docs/COMPILER_PATTERNS.md, "Temps"; probes/Probe02):
    // the allocator keeps one FIFO free list per type; an ElseIf's condition
    // temp is released at the end of its own branch, a nested If's is held
    // until its EndIf. So, for the inner If with condition temp T:
    //  - T allocated again inside its else part: it was free, so ElseIf;
    //  - a temp of T's type allocated fresh there (a number above every temp
    //    met so far): the free list was empty, so T was held, so nested;
    //  - otherwise the two readings differ only in where T sits in the free
    //    list: under ElseIf it is ahead of every temp of its type the else
    //    part releases, so none of those can be allocated again (in the else
    //    part or after the EndIf) before T is; a second allocation of one of
    //    them, or one of them allocated after the EndIf before T, means
    //    nested. Nothing of the kind means the readings compile the same,
    //    and ElseIf is kept.
    // A write into a live && / || accumulator (`CAST acc x` after a jump on
    // acc to the next instruction) is not an allocation and is skipped.
    static bool is_accumulator_write(const Function& f, size_t ip) {
        const Instruction& ins = f.code[ip];
        if (ins.op != Opcode::Cast || ins.args.size() != 2 || ins.args[0].type != ValueType::Identifier) return false;
        const StrIdx acc = ins.args[0].str();
        if (ins.args[1].type == ValueType::Identifier && ins.args[1].str() == acc) return false;
        for (size_t q = ip; q-- > 0;) {
            const Instruction& j = f.code[q];
            const bool jump = j.op == Opcode::JmpT || j.op == Opcode::JmpF;
            if (jump && j.args.size() == 2 && j.args[0].type == ValueType::Identifier && j.args[0].str() == acc &&
                j.args[1].type == ValueType::Integer && static_cast<long long>(q) + j.args[1].as_int() == static_cast<long long>(ip) + 1) {
                return true;
            }
            if (!jump && j.args.size() > 0 && j.args[0].type == ValueType::Identifier && j.args[0].str() == acc &&
                j.op != Opcode::Return) {
                return false;  // acc written in between: a new value
            }
        }
        return false;
    }

    // The `CAST acc x` that starts an && / || chain (followed by the jump on
    // acc): the accumulator does not come from the free list in FIFO order
    // (docs/COMPILER_PATTERNS.md, "Temps": not settled), so its freshness and
    // its repeats say nothing about what was free.
    static bool is_accumulator_alloc(const Function& f, size_t ip) {
        const Instruction& ins = f.code[ip];
        if (ins.op != Opcode::Cast || ins.args.size() != 2 || ins.args[0].type != ValueType::Identifier) return false;
        if (ip + 1 >= f.code.size()) return false;
        const Instruction& next = f.code[ip + 1];
        return (next.op == Opcode::JmpT || next.op == Opcode::JmpF) && next.args.size() == 2 &&
               next.args[0].type == ValueType::Identifier && next.args[0].str() == ins.args[0].str();
    }

    static bool else_if_collapsible(const Stmt& inner, const Ctx& ctx) {
        if (inner.condTemp == 0 || inner.elseBody.empty()) return true;
        const Function& f = ctx.c->f;
        if (inner.ipEnd > f.code.size()) return true;
        // The else part starts after the then-branch's jump to the end label
        // (a statement's own ip can sit after a folded temp's write).
        size_t elseStart = inner.elseBody.front()->ip;
        for (size_t ip = inner.ip; ip < inner.ipEnd; ++ip) {
            const Instruction& ins = f.code[ip];
            if (ins.op == Opcode::Jmp && !ins.args.empty() && ins.args[0].type == ValueType::Integer &&
                static_cast<long long>(ip) + ins.args[0].as_int() == static_cast<long long>(inner.ipEnd)) {
                elseStart = ip + 1;
                break;
            }
        }
        if (elseStart >= inner.ipEnd) return true;
        const std::string type = ctx.c->type_of(inner.condTemp);
        auto temp_number = [&](StrIdx name) -> long long {
            const std::string_view n = ctx.c->s.str(name);
            return n.size() > 6 ? std::atoll(std::string(n.substr(6)).c_str()) : -1;
        };
        long long maxSeen = -1;
        for (size_t ip = 0; ip < elseStart; ++ip) {
            for (const Value& v : f.code[ip].args) {
                if (v.type == ValueType::Identifier && ctx.c->is_temp(v.str())) maxSeen = std::max(maxSeen, temp_number(v.str()));
            }
        }
        std::vector<StrIdx> released;  // temps of T's type the else part allocated (and so released)
        for (size_t ip = elseStart; ip < inner.ipEnd; ++ip) {
            const Instruction& ins = f.code[ip];
            const StrIdx dest = written_temp(ins, ctx);
            if (dest == inner.condTemp) return true;
            if (dest && ctx.c->type_of(dest) == type && !is_accumulator_write(f, ip)) {
                const bool fifo = !is_accumulator_alloc(f, ip);
                if (fifo && temp_number(dest) > maxSeen) return false;
                if (fifo && std::find(released.begin(), released.end(), dest) != released.end()) return false;
                if (std::find(released.begin(), released.end(), dest) == released.end()) released.push_back(dest);
            }
            for (const Value& v : ins.args) {
                if (v.type == ValueType::Identifier && ctx.c->is_temp(v.str())) maxSeen = std::max(maxSeen, temp_number(v.str()));
            }
        }
        if (released.empty()) return true;
        for (size_t ip = inner.ipEnd; ip < f.code.size(); ++ip) {
            const StrIdx dest = written_temp(f.code[ip], ctx);
            if (dest == inner.condTemp) return true;
            if (dest && std::find(released.begin(), released.end(), dest) != released.end() && !is_accumulator_write(f, ip) &&
                !is_accumulator_alloc(f, ip)) {
                return false;
            }
        }
        return true;
    }

    // --- expressions ----------------------------------------------------------------

    // Value: assignment, argument, operand, return (the compiler inserts casts
    // here). Condition: If / While tests, operands of && || and !. Object: the
    // receiver of a call or property access, and the operand of another cast
    // (no cast is ever implicit there; the operand's exact type matters).
    enum class Context { Value, Condition, Object };

    std::string name_of(StrIdx v, const Ctx& ctx) const {
        auto it = ctx.names.find(v);
        if (it != ctx.names.end()) return it->second;
        return ctx.c->display_name(v);
    }

    static bool implicit_cast_ok(const std::string& from, const std::string& to) {
        if (to == "string" || to == "bool") return true;
        if (to == "float" && from == "int") return true;
        return false;
    }

    std::string expr(const Expr& e, int parentPrec, bool rightOperand, Context context, const Ctx& ctx) const {
        std::string text;
        int myPrec = prec_of(e);
        switch (e.kind) {
        case ExprKind::Lit:
            text = literal(e.lit);
            if (rightOperand && !text.empty() && text[0] == '-') return "(" + text + ")";
            return text;
        case ExprKind::Var: return name_of(e.name, ctx);
        case ExprKind::Unary: {
            const Expr& k = *e.kids[0];
            std::string inner = expr(k, 6, false, e.op == "!" ? Context::Condition : Context::Value, ctx);
            if (k.kind == ExprKind::Unary || (k.kind == ExprKind::Lit && !inner.empty() && inner[0] == '-')) inner = "(" + inner + ")";
            text = e.op + inner;
            break;
        }
        case ExprKind::Binary:
            text = expr(*e.kids[0], myPrec, false, Context::Value, ctx) + " " + e.op + " " +
                   expr(*e.kids[1], myPrec, true, Context::Value, ctx);
            break;
        case ExprKind::And:
        case ExprKind::Or:
            text = expr(*e.kids[0], myPrec, false, Context::Condition, ctx) + (e.kind == ExprKind::And ? " && " : " || ") +
                   expr(*e.kids[1], myPrec, true, Context::Condition, ctx);
            break;
        case ExprKind::Cast: {
            const Expr& k = *e.kids[0];
            const std::string to = lower(e.type);
            const std::string from = lower(k.type);
            if (k.kind == ExprKind::Lit && k.lit.type == ValueType::None) return "None";
            if (k.kind == ExprKind::Var && ctx.c->is_nonevar(k.name)) return "None";  // a read of the None slot
            // A call that returns nothing used as a value: the compiler casts
            // the None result itself (still accepted today).
            if (from == "none" && context == Context::Value) return expr(k, parentPrec, rightOperand, Context::Value, ctx);
            if (e.implicitCast) return expr(k, parentPrec, rightOperand, Context::Value, ctx);
            // A same-type cast is never inserted by the compiler (the && / ||
            // ones were removed earlier), so it was written and must stay.
            if (!e.explicitCast && context == Context::Value && to != from && implicit_cast_ok(from, to)) return expr(k, parentPrec, rightOperand, Context::Value, ctx);
            // An upcast to an ancestor type is inserted by the compiler too,
            // in value positions only: not on a call's receiver, and not under
            // another cast (sibling types cast through the common ancestor).
            if (!e.explicitCast && context == Context::Value && to != from && ctx.c->is_subtype(from, to)) return expr(k, parentPrec, rightOperand, Context::Value, ctx);
            text = expr(k, 8, false, Context::Object, ctx) + " as " + (e.name ? nice_type(e.name) : nice_type(e.type));
            break;
        }
        case ExprKind::Call: {
            std::string args;
            const size_t first = e.call == CallKind::Method ? 1 : 0;
            // A raw None literal argument is how the compiler fills an omitted
            // parameter (a written None gets a cast into a typed temp). Trailing
            // ones are left out. A middle one is left out too when the callee's
            // declaration is known: the arguments after it are then passed by
            // name. Without the declaration it stays as None.
            size_t last = e.kids.size();
            while (last > first && e.kids[last - 1]->kind == ExprKind::Lit && e.kids[last - 1]->lit.type == ValueType::None) --last;
            size_t firstOmitted = last;
            for (size_t i = first; i < last; ++i) {
                if (e.kids[i]->kind == ExprKind::Lit && e.kids[i]->lit.type == ValueType::None) {
                    firstOmitted = i;
                    break;
                }
            }
            const Function* decl = nullptr;
            if (firstOmitted < last) {
                std::string calleeType;
                if (e.call == CallKind::Static) calleeType = lower(m_s.str(e.cls));
                else if (e.call == CallKind::Parent) calleeType = lower(m_s.str(ctx.c->o.parent));
                else calleeType = e.kids[0]->type;
                const FnContext::FoundFunction found = ctx.c->find_function(calleeType, m_s.str(e.name));
                if (found.function && found.function->params.size() == e.kids.size() - first) decl = found.function;
                if (!decl) firstOmitted = last;  // cannot name them: keep the Nones
            }
            // FormID-taking calls print their integer literals in hex, the way
            // sources write them (0x000D62). Same compiled value either way.
            const std::string calleeName = lower(m_s.str(e.name));
            const bool formIdCall = calleeName == "getformfromfile" || calleeName == "getform" || calleeName == "getformex";
            for (size_t i = first; i < last; ++i) {
                const Expr& a = *e.kids[i];
                const bool omitted = i >= firstOmitted && a.kind == ExprKind::Lit && a.lit.type == ValueType::None;
                if (omitted) continue;
                if (!args.empty()) args += ", ";
                if (i > firstOmitted && decl) {
                    const Script* ds = ctx.c->find_function(e.call == CallKind::Static ? lower(m_s.str(e.cls)) : (e.call == CallKind::Parent ? lower(m_s.str(ctx.c->o.parent)) : e.kids[0]->type), m_s.str(e.name)).script;
                    args += std::string(ds->str(decl->params[i - first].name)) + " = ";
                }
                if (formIdCall && i == first && a.kind == ExprKind::Lit && a.lit.type == ValueType::Integer && a.lit.as_int() >= 0) {
                    char hex[16];
                    std::snprintf(hex, sizeof hex, "0x%06X", unsigned(a.lit.as_int()));
                    args += hex;
                    continue;
                }
                args += expr(a, 0, false, Context::Value, ctx);
            }
            const std::string fn = str(e.name);
            if (e.call == CallKind::Static) return nice_type(e.cls) + "." + fn + "(" + args + ")";
            if (e.call == CallKind::Parent) return "Parent." + fn + "(" + args + ")";
            const Expr& obj = *e.kids[0];
            if (obj.kind == ExprKind::Var && ctx.c->is_self(obj.name)) return fn + "(" + args + ")";
            return expr(obj, 8, false, Context::Object, ctx) + "." + fn + "(" + args + ")";
        }
        case ExprKind::PropGet: {
            const Expr& obj = *e.kids[0];
            if (obj.kind == ExprKind::Var && ctx.c->is_self(obj.name)) return str(e.name);
            return expr(obj, 8, false, Context::Object, ctx) + "." + str(e.name);
        }
        case ExprKind::ArrayNew: {
            std::string t = e.type;
            if (t.size() > 2 && t.compare(t.size() - 2, 2, "[]") == 0) t.resize(t.size() - 2);
            return "new " + nice_type(t) + "[" + expr(*e.kids[0], 0, false, Context::Value, ctx) + "]";
        }
        case ExprKind::ArrayLen: return expr(*e.kids[0], 8, false, Context::Object, ctx) + ".Length";
        case ExprKind::ArrayGet:
            return expr(*e.kids[0], 8, false, Context::Object, ctx) + "[" + expr(*e.kids[1], 0, false, Context::Value, ctx) + "]";
        case ExprKind::ArrayFind: {
            std::string t = expr(*e.kids[0], 8, false, Context::Object, ctx) + (e.reverse ? ".RFind(" : ".Find(") +
                            expr(*e.kids[1], 0, false, Context::Value, ctx);
            const Expr& start = *e.kids[2];
            const bool isDefault = e.reverse ? is_minus_one_int(start) : is_zero_int(start);
            if (!isDefault) t += ", " + expr(start, 0, false, Context::Value, ctx);
            return t + ")";
        }
        }
        const bool paren = myPrec < parentPrec || (myPrec == parentPrec && rightOperand);
        return paren ? "(" + text + ")" : text;
    }

    const Script& m_s;
    DecompileOptions m_opt;
    std::string m_out;
    std::vector<FunctionReport> m_reports;
    uint32_t m_hiddenMask = 0, m_conditionalMask = 0;
    mutable std::map<std::string, std::string> m_niceTypes;
};

}  // namespace

DecompileResult decompile(const Script& script, const DecompileOptions& opt) {
    return Writer(script, opt).run();
}

}  // namespace papyrus::pex
