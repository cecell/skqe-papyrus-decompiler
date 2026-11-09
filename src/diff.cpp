// The semantic diff (include/papyrus/diff.hpp). The canonical comparison
// says where the two versions differ; this file says what the difference
// is, in one sentence per change, after lifting both versions of each
// differing body into the decompiler's statement trees and aligning them.
// docs/DIFF.md documents the sentences and the numbers.
// SPDX-License-Identifier: GPL-3.0-only
#include "papyrus/diff.hpp"

#include "decompile/event_names.hpp"
#include "decompile/ir.hpp"
#include "papyrus/canon.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace papyrus::pex {

namespace {

using namespace detail;

// --- names --------------------------------------------------------------------

std::string lower(std::string_view v) { return FnContext::lower(v); }

bool eq_ci(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

bool starts_with_ci(std::string_view v, std::string_view p) { return v.size() >= p.size() && eq_ci(v.substr(0, p.size()), p); }
bool ends_with_ci(std::string_view v, std::string_view p) { return v.size() >= p.size() && eq_ci(v.substr(v.size() - p.size()), p); }

std::string bt(std::string_view s) { return "`" + std::string(s) + "`"; }

// Identifiers are case-insensitive in Papyrus and the compiler records the
// spelling at each use, so two versions are compared with everything outside
// string literals lowercased.
// An integer literal where the source wrote a float (`Wait(1)` for
// `Wait(1.0)`) compiles differently and means the same, so integers are
// spelled as floats too.
std::string fold(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    bool quoted = false;
    auto is_word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.'; };
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            out += c;
            if (c == '\\' && i + 1 < text.size()) out += text[++i];
            else if (c == '"') quoted = false;
            continue;
        }
        if (c == '"') {
            quoted = true;
            out += c;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) && (i == 0 || !is_word(text[i - 1]))) {
            size_t j = i;
            while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) ++j;
            out.append(text, i, j - i);
            if (j == text.size() || !is_word(text[j])) out += ".0";
            i = j - 1;
            continue;
        }
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string plural(size_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

// "A, B and C"
std::string join_and(const std::vector<std::string>& items) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += i + 1 == items.size() ? " and " : ", ";
        out += items[i];
    }
    return out;
}

// Flag bits as names through the script's own flag table (as canon does).
std::set<std::string> flag_names(const Script& s, uint32_t bits) {
    std::set<std::string> out;
    for (unsigned bit = 0; bit < 32; ++bit) {
        if (!(bits & (1u << bit))) continue;
        std::string name;
        for (const UserFlag& f : s.userFlags) {
            if (f.bit == bit) name = std::string(s.str(f.name));
        }
        out.insert(name.empty() ? "bit " + std::to_string(bit) : name);
    }
    return out;
}

std::string flags_text(const std::set<std::string>& f) {
    if (f.empty()) return "no flags";
    std::vector<std::string> v(f.begin(), f.end());
    return join_and(v);
}

// The source spelling of a literal value (through the decompiler's writer).
std::string value_text(const FnContext& fc, const Value& v) {
    ExprPtr e = make_lit(v);
    return render_expr(fc, *e);
}

// A function the decompiler would print as an Event: an event name with no
// return type.
std::string function_word(const Script& s, std::string_view name, const Function& f) {
    return lower(s.str(f.returnType)) == "none" && is_event_name(lower(name)) ? "Event" : "Function";
}

std::string type_text(const Script& s, StrIdx t);

std::string signature_text(const Script& s, std::string_view name, const Function& f) {
    std::string t(name);
    t += '(';
    for (size_t i = 0; i < f.params.size(); ++i) {
        if (i) t += ", ";
        t += type_text(s, f.params[i].type) + " " + std::string(s.str(f.params[i].name));
    }
    return t + ')';
}

// A type name with the spelling a declaration in the file used, when the
// compiler lowercased this occurrence (as the decompiler does it).
std::string type_text(const Script& s, StrIdx t) {
    const std::string_view raw = s.str(t);
    std::string best(raw);
    bool hasUpper = false;
    for (char c : best) hasUpper = hasUpper || std::isupper(static_cast<unsigned char>(c));
    if (hasUpper || best.empty()) return best;
    for (const std::string& str : s.strings) {
        if (str.size() != raw.size() || !eq_ci(str, raw)) continue;
        bool up = false;
        for (char c : str) up = up || std::isupper(static_cast<unsigned char>(c));
        if (up) return str;
    }
    best[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(best[0])));
    return best;
}

// --- the function tables of both versions --------------------------------------

// A key that names the same function in both versions.
struct FnKey {
    std::string object, state, name;  // lowercased; accessors: name "prop.get" / "prop.set"
    bool operator<(const FnKey& o) const { return std::tie(object, state, name) < std::tie(o.object, o.state, o.name); }
    bool operator==(const FnKey& o) const { return object == o.object && state == o.state && name == o.name; }
};

struct FnRef {
    const Object* o = nullptr;
    const State* st = nullptr;   // null for accessors
    const Function* f = nullptr;
    std::string name;            // "OnUpdate", "Prop.get"
    std::string stateName;       // "" for the default state
    StrIdx nameIdx = 0, stateIdx = 0;
    FunctionKind kind = FunctionKind::Method;
};

StrIdx empty_string_index(const Script& s) {
    for (size_t i = 0; i < s.strings.size(); ++i) {
        if (s.strings[i].empty()) return static_cast<StrIdx>(i);
    }
    return 0;
}

// Every function of a script, in the file's order, keyed for matching.
std::vector<std::pair<FnKey, FnRef>> functions_of(const Script& s) {
    std::vector<std::pair<FnKey, FnRef>> out;
    const StrIdx empty = empty_string_index(s);
    for (const Object& o : s.objects) {
        auto add = [&](const State* st, const Function& f, StrIdx nameIdx, const std::string& name, FunctionKind kind) {
            FnRef r;
            r.o = &o;
            r.st = st;
            r.f = &f;
            r.name = name;
            r.nameIdx = nameIdx;
            r.stateIdx = st ? st->name : empty;
            r.stateName = st ? std::string(s.str(st->name)) : std::string();
            r.kind = kind;
            out.push_back({{lower(s.str(o.name)), lower(r.stateName), lower(name)}, r});
        };
        for (const Property& p : o.properties) {
            if (p.getter) add(nullptr, *p.getter, p.name, std::string(s.str(p.name)) + ".get", FunctionKind::Getter);
            if (p.setter) add(nullptr, *p.setter, p.name, std::string(s.str(p.name)) + ".set", FunctionKind::Setter);
        }
        for (const State& st : o.states) {
            for (const NamedFunction& nf : st.functions) add(&st, nf.body, nf.name, std::string(s.str(nf.name)), FunctionKind::Method);
        }
    }
    return out;
}

// The debug record of a function (as the auditor finds it: only the first
// function of a state block carries the state label).
const FunctionDebug* debug_record(const Script& s, const FnRef& r) {
    if (!s.debug.present) return nullptr;
    const FunctionDebug* exact = s.debug_for(r.o->name, r.stateIdx, r.nameIdx, r.kind);
    if (exact && exact->lines.size() == r.f->code.size()) return exact;
    for (const FunctionDebug& d : s.debug.functions) {
        if (d.object == r.o->name && d.kind == r.kind && d.lines.size() == r.f->code.size() && eq_ci(s.str(d.name), s.str(r.nameIdx))) return &d;
    }
    return nullptr;
}

// One version of one body: its context, statements and lines.
struct Body {
    const Script* s = nullptr;
    FnRef ref;
    std::unique_ptr<FnContext> fc;
    FunctionIR ir;
    std::vector<StmtPtr> stmts;
    const FunctionDebug* dbg = nullptr;
    bool ok = false;

    uint16_t line(size_t ip) const {
        if (!dbg || ip >= dbg->lines.size()) return 0;
        return dbg->lines[ip];
    }
    uint16_t line(const Stmt& st) const { return line(st.ip); }
};

bool lift(const Script& s, const FnRef& r, ScriptResolver* resolver, Body& b) {
    b.s = &s;
    b.ref = r;
    b.fc = std::make_unique<FnContext>(s, *r.o, *r.f, resolver);
    b.dbg = debug_record(s, r);
    try {
        b.ir = lift_function(*b.fc);
        b.stmts = structure_function(*b.fc, b.ir);
        b.ok = true;
    } catch (const DecompileError&) {
        b.ok = false;
    } catch (...) {
        b.ok = false;
    }
    return b.ok;
}

// --- walkers --------------------------------------------------------------------

template <class F>
void each_expr(const Expr& e, F&& f) {
    f(e);
    for (const ExprPtr& k : e.kids) each_expr(*k, f);
}

template <class F>
void each_stmt(const std::vector<StmtPtr>& ss, F&& f) {
    for (const StmtPtr& st : ss) {
        f(*st);
        each_stmt(st->body, f);
        each_stmt(st->elseBody, f);
    }
}

template <class F>
void each_expr_in(const Stmt& st, F&& f) {
    if (st.lhs) each_expr(*st.lhs, f);
    if (st.rhs) each_expr(*st.rhs, f);
    each_stmt(st.body, [&](const Stmt& inner) {
        if (inner.lhs) each_expr(*inner.lhs, f);
        if (inner.rhs) each_expr(*inner.rhs, f);
    });
    each_stmt(st.elseBody, [&](const Stmt& inner) {
        if (inner.lhs) each_expr(*inner.lhs, f);
        if (inner.rhs) each_expr(*inner.rhs, f);
    });
}

size_t count_stmts(const std::vector<StmtPtr>& ss) {
    size_t n = 0;
    each_stmt(ss, [&](const Stmt&) { ++n; });
    return n;
}

// A bare `Return` at the end of a None function is the compiler's, not the author's.
bool is_bare_return(const Stmt& st) {
    if (st.kind != StmtKind::Return) return false;
    if (!st.rhs) return true;
    return st.rhs->kind == ExprKind::Lit && st.rhs->lit.type == ValueType::None;
}

bool body_is_empty(const std::vector<StmtPtr>& ss) {
    if (ss.empty()) return true;
    return ss.size() == 1 && is_bare_return(*ss[0]);
}

// Which slots a name belongs to.
enum class SlotClass { None, Param, Local, ScriptVar };

SlotClass slot_class(const FnContext& fc, StrIdx name) {
    if (fc.is_self(name) || fc.is_nonevar(name) || fc.is_temp(name)) return SlotClass::None;
    const std::string_view n = fc.s.str(name);
    for (const Param& p : fc.f.params) {
        if (eq_ci(fc.s.str(p.name), n)) return SlotClass::Param;
    }
    for (const Param& p : fc.f.locals) {
        if (eq_ci(fc.s.str(p.name), n)) return SlotClass::Local;
    }
    for (const Variable& v : fc.o.variables) {
        if (eq_ci(fc.s.str(v.name), n)) return SlotClass::ScriptVar;
    }
    return SlotClass::None;
}

// "game.getformfromfile" for statics, "gotostate" for methods.
std::string callee_key(const Script& s, const Expr& e) {
    if (e.kind != ExprKind::Call) return "";
    if (e.call == CallKind::Static) return lower(s.str(e.cls)) + "." + lower(s.str(e.name));
    return lower(s.str(e.name));
}

// "Game.GetFormFromFile()" / "GotoState()" for sentences.
std::string callee_text(const Script& s, const Expr& e) {
    if (e.call == CallKind::Static) return type_text(s, e.cls) + "." + std::string(s.str(e.name)) + "()";
    if (e.call == CallKind::Parent) return "Parent." + std::string(s.str(e.name)) + "()";
    return std::string(s.str(e.name)) + "()";
}

bool is_formid_callee(const std::string& key) {
    return ends_with_ci(key, "getformfromfile") || ends_with_ci(key, ".getform") || key == "getform" || ends_with_ci(key, "getformex");
}
bool is_registration(const std::string& key) {
    return starts_with_ci(key, "registerfor") || starts_with_ci(key, "unregisterfor");
}
bool is_state_change(const std::string& key) { return key == "gotostate"; }

const Expr& strip_casts(const Expr& e) {
    const Expr* p = &e;
    while (p->kind == ExprKind::Cast && !p->kids.empty()) p = p->kids[0].get();
    return *p;
}

// --- the alignment ---------------------------------------------------------------

enum class Op { Keep, Del, Ins };
struct EditOp {
    Op op;
    size_t i = 0, j = 0;  // old index / new index (both valid for Keep)
};

// Longest common subsequence over two key sequences, as an edit script.
std::vector<EditOp> lcs(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    const size_t n = a.size(), m = b.size();
    std::vector<std::vector<uint32_t>> t(n + 1, std::vector<uint32_t>(m + 1, 0));
    for (size_t i = n; i-- > 0;) {
        for (size_t j = m; j-- > 0;) {
            t[i][j] = a[i] == b[j] ? t[i + 1][j + 1] + 1 : std::max(t[i + 1][j], t[i][j + 1]);
        }
    }
    std::vector<EditOp> out;
    size_t i = 0, j = 0;
    while (i < n && j < m) {
        if (a[i] == b[j]) {
            out.push_back({Op::Keep, i, j});
            ++i;
            ++j;
        } else if (t[i + 1][j] >= t[i][j + 1]) {
            out.push_back({Op::Del, i, 0});
            ++i;
        } else {
            out.push_back({Op::Ins, 0, j});
            ++j;
        }
    }
    while (i < n) out.push_back({Op::Del, i++, 0});
    while (j < m) out.push_back({Op::Ins, 0, j++});
    return out;
}

// --- the differ -------------------------------------------------------------------

class Differ {
public:
    Differ(const Script& a, const Script& b, const DiffOptions& opt) : m_a(a), m_b(b), m_opt(opt) {}

    DiffResult run() {
        DiffResult r;
        std::vector<Difference> diffs;
        CompareOptions copt;
        copt.maxDifferences = 0;
        copt.compareHeader = false;
        copt.renumberTemps = true;
        canonical_equal(m_a, m_b, &diffs, copt);
        bool equal = true;
        for (const Difference& d : diffs) {
            if (d.kind != DiffKind::DebugInfo) equal = false;
        }
        if (equal) {
            r.equal = true;
            return r;
        }

        m_fa = functions_of(m_a);
        m_fb = functions_of(m_b);

        // 1. Which bodies to align: every function the comparison named
        //    (signature, locals or instructions) that both versions have.
        std::set<FnKey> bodies;
        for (const Difference& d : diffs) {
            if (d.kind != DiffKind::Function && d.kind != DiffKind::Locals && d.kind != DiffKind::Instruction) continue;
            if (starts_with_ci(d.message, "state only in")) continue;
            FnKey k;
            if (!parse_function_where(d.where, k)) continue;
            if (find(m_fa, k) && find(m_fb, k)) bodies.insert(k);
        }
        for (const FnKey& k : bodies) {
            Pair p;
            p.key = k;
            p.a = std::make_unique<Body>();
            p.b = std::make_unique<Body>();
            lift(m_a, *find(m_fa, k), m_opt.resolver, *p.a);
            lift(m_b, *find(m_fb, k), m_opt.resolver, *p.b);
            m_pairs.push_back(std::move(p));
        }
        // New version's order.
        std::stable_sort(m_pairs.begin(), m_pairs.end(), [&](const Pair& x, const Pair& y) { return index_of(m_fb, x.key) < index_of(m_fb, y.key); });

        // 2. Renames: parameters by position, locals and script variables by
        //    the use the matched statements make of them.
        for (Pair& p : m_pairs) seed_param_renames(p);
        for (Pair& p : m_pairs) {
            if (p.a->ok && p.b->ok) collect_votes(p);
        }
        accept_renames();

        // 3. Sentences: script level, then each body. A state added or
        //    removed whole is one sentence, not one per function.
        for (const Difference& d : diffs) {
            if (d.kind == DiffKind::Function && starts_with_ci(d.message, "state only in")) {
                const size_t dot = d.where.find('.');
                if (dot != std::string::npos) m_wholeStates.insert(d.where.substr(dot + 1));
            }
        }
        for (const Difference& d : diffs) describe(d);
        for (Pair& p : m_pairs) body_pair(p, r);

        // Script-level first, then by function in the new version's order
        // (removed functions and states are script-level).
        std::stable_sort(m_out.begin(), m_out.end(), [&](const Change& x, const Change& y) { return fn_order(x) < fn_order(y); });
        r.changes = std::move(m_out);
        return r;
    }

private:
    struct Pair {
        FnKey key;
        std::unique_ptr<Body> a, b;
        // Display-name overrides for the old side: renamed parameters and
        // locals (old slot -> new display name).
        std::map<StrIdx, std::string> renames;
        std::vector<std::pair<std::string, std::string>> renamedLocals;  // (old display, new display)
        std::vector<std::pair<std::string, std::string>> renamedParams;
    };

    // --- lookups ------------------------------------------------------------------

    static const FnRef* find(const std::vector<std::pair<FnKey, FnRef>>& fs, const FnKey& k) {
        for (const auto& [key, ref] : fs) {
            if (key == k) return &ref;
        }
        return nullptr;
    }
    static size_t index_of(const std::vector<std::pair<FnKey, FnRef>>& fs, const FnKey& k) {
        for (size_t i = 0; i < fs.size(); ++i) {
            if (fs[i].first == k) return i;
        }
        return fs.size();
    }
    size_t fn_order(const Change& c) const {
        if (c.function.empty()) return 0;
        FnKey k{lower(c.object), lower(c.state), lower(c.function)};
        const size_t i = index_of(m_fb, k);
        if (i < m_fb.size()) return i + 1;
        return m_fb.size() + 1 + index_of(m_fa, k);
    }

    // canon's where: "obj.fn", "obj.state.fn", "obj.prop P.get" / ".set".
    static bool parse_function_where(const std::string& where, FnKey& k) {
        const size_t dot = where.find('.');
        if (dot == std::string::npos) return false;
        k.object = where.substr(0, dot);
        std::string rest = where.substr(dot + 1);
        if (rest.rfind("prop ", 0) == 0) {
            rest = rest.substr(5);
            const size_t d2 = rest.rfind('.');
            if (d2 == std::string::npos) return false;  // the property itself
            k.state.clear();
            k.name = rest;  // "p.get"
            return true;
        }
        const size_t d2 = rest.find('.');
        if (d2 == std::string::npos) {
            k.state.clear();
            k.name = rest;
        } else {
            k.state = rest.substr(0, d2);
            k.name = rest.substr(d2 + 1);
        }
        return true;
    }

    const Object* object_named(const Script& s, std::string_view lname) const {
        for (const Object& o : s.objects) {
            if (eq_ci(s.str(o.name), lname)) return &o;
        }
        return nullptr;
    }
    static const Variable* variable_named(const Script& s, const Object& o, std::string_view lname) {
        for (const Variable& v : o.variables) {
            if (eq_ci(s.str(v.name), lname)) return &v;
        }
        return nullptr;
    }
    static const Property* property_named(const Script& s, const Object& o, std::string_view lname) {
        for (const Property& p : o.properties) {
            if (eq_ci(s.str(p.name), lname)) return &p;
        }
        return nullptr;
    }
    // The property an autovar name (::X_var) backs, or null.
    static const Property* property_of_autovar(const Script& s, const Object& o, std::string_view varName) {
        for (const Property& p : o.properties) {
            if (p.is_auto() && eq_ci(s.str(p.autoVar), varName)) return &p;
        }
        return nullptr;
    }

    void add(const std::string& object, const std::string& state, const std::string& function, const char* kind, uint16_t line, uint16_t oldLine,
             std::string message) {
        Change c;
        c.object = object;
        c.state = state;
        c.function = function;
        c.kind = kind;
        c.line = line;
        c.oldLine = oldLine;
        c.message = std::move(message);
        m_out.push_back(std::move(c));
    }
    void add_script(const std::string& object, const char* kind, std::string message) { add(object, "", "", kind, 0, 0, std::move(message)); }
    void add_fn(const Pair& p, const char* kind, uint16_t line, uint16_t oldLine, std::string message) {
        add(std::string(m_b.str(p.b->ref.o->name)), p.b->ref.stateName, p.b->ref.name, kind, line, oldLine, std::move(message));
    }

    // A throwaway context for rendering values of a script that may have no
    // function at all.
    FnContext value_context(const Script& s, const Object& o) const {
        static const Function kNoFunction;
        return FnContext(s, o, kNoFunction, m_opt.resolver);
    }

    // --- layer 1: declarations ------------------------------------------------------

    void describe(const Difference& d) {
        switch (d.kind) {
        case DiffKind::Object: describe_object(d); break;
        case DiffKind::Variable: describe_variable(d); break;
        case DiffKind::Property: describe_property(d); break;
        case DiffKind::Function: describe_function(d); break;
        default: break;  // Header (not compared), UserFlags (never reported), Locals and Instruction (the bodies), DebugInfo
        }
    }

    void describe_object(const Difference& d) {
        const Object* oa = object_named(m_a, d.where);
        const Object* ob = object_named(m_b, d.where);
        if (d.message == "only in A") {
            add_script(d.where, "object-removed", "Object " + bt(oa ? m_a.str(oa->name) : d.where) + " was removed");
            return;
        }
        if (d.message == "only in B") {
            add_script(d.where, "object-added", "Object " + bt(ob ? m_b.str(ob->name) : d.where) + " was added");
            return;
        }
        if (!oa || !ob) return;
        const std::string name(m_b.str(ob->name));
        if (starts_with_ci(d.message, "parent ")) {
            const std::string from(m_a.str(oa->parent)), to(m_b.str(ob->parent));
            if (from.empty()) add_script(name, "parent-changed", "The script now extends " + bt(to) + "; it extended nothing before");
            else if (to.empty()) add_script(name, "parent-changed", "The script no longer extends " + bt(from));
            else add_script(name, "parent-changed", "The script now extends " + bt(to) + " instead of " + bt(from));
        } else if (starts_with_ci(d.message, "doc string")) {
            add_script(name, "script-doc-changed", "The script's documentation string changed");
        } else if (starts_with_ci(d.message, "user flags")) {
            add_script(name, "script-flags-changed",
                       "The script's flags changed from " + flags_text(flag_names(m_a, oa->userFlags)) + " to " + flags_text(flag_names(m_b, ob->userFlags)));
        } else if (starts_with_ci(d.message, "auto state")) {
            const std::string from(m_a.str(oa->autoState)), to(m_b.str(ob->autoState));
            if (from.empty()) add_script(name, "auto-state-changed", "State " + bt(to) + " is now the auto state");
            else if (to.empty()) add_script(name, "auto-state-changed", "State " + bt(from) + " is no longer the auto state");
            else add_script(name, "auto-state-changed", "The auto state is now " + bt(to) + " instead of " + bt(from));
        }
    }

    void describe_variable(const Difference& d) {
        const size_t dot = d.where.find('.');
        if (dot == std::string::npos) return;
        const std::string oname = d.where.substr(0, dot), vname = d.where.substr(dot + 1);
        const Object* oa = object_named(m_a, oname);
        const Object* ob = object_named(m_b, oname);
        if (!oa || !ob) return;
        const Variable* va = variable_named(m_a, *oa, vname);
        const Variable* vb = variable_named(m_b, *ob, vname);
        const std::string name(m_b.str(ob->name));
        // The backing variable of an auto property: the property's sentence
        // covers it, except its default, which only the variable carries.
        const Property* pa = va ? property_of_autovar(m_a, *oa, m_a.str(va->name)) : nullptr;
        const Property* pb = vb ? property_of_autovar(m_b, *ob, m_b.str(vb->name)) : nullptr;
        if (d.message == "only in A") {
            if (!va || pa) return;
            if (m_varRenames.count(lower(vname))) {
                add_script(name, "variable-renamed", "Variable " + bt(m_a.str(va->name)) + " was renamed to " + bt(m_varRenames[lower(vname)]));
            } else {
                add_script(name, "variable-removed", "Variable " + bt(m_a.str(va->name)) + " (" + type_text(m_a, va->type) + ") was removed");
            }
            return;
        }
        if (d.message == "only in B") {
            if (!vb || pb) return;
            if (m_varRenamedTo.count(lower(vname))) return;  // reported as the rename
            std::string t = "Variable " + bt(m_b.str(vb->name)) + " (" + type_text(m_b, vb->type);
            if (!is_default_value(vb->initial)) t += ", default " + value_text(value_context(m_b, *ob), vb->initial);
            add_script(name, "variable-added", t + ") was added");
            return;
        }
        if (!va || !vb) return;
        const bool isProp = pa && pb;
        const std::string what = isProp ? "property " + bt(m_b.str(pb->name)) : "variable " + bt(m_b.str(vb->name));
        const std::string What = isProp ? "Property " + bt(m_b.str(pb->name)) : "Variable " + bt(m_b.str(vb->name));
        if (!eq_ci(m_a.str(va->type), m_b.str(vb->type))) {
            if (!isProp) add_script(name, "variable-retyped", What + " is now " + type_text(m_b, vb->type) + ", was " + type_text(m_a, va->type));
        }
        if (!(va->initial.type == vb->initial.type && value_equal(va->initial, vb->initial))) {
            add_script(name, isProp ? "property-default-changed" : "variable-default-changed",
                       "The default of " + what + " changed from " + value_text(value_context(m_a, *oa), va->initial) + " to " +
                           value_text(value_context(m_b, *ob), vb->initial));
        }
        if (!isProp && flag_names(m_a, va->userFlags) != flag_names(m_b, vb->userFlags)) {
            add_script(name, "variable-flags-changed",
                       "The flags of " + what + " changed from " + flags_text(flag_names(m_a, va->userFlags)) + " to " + flags_text(flag_names(m_b, vb->userFlags)));
        }
    }

    bool value_equal(const Value& a, const Value& b) const {
        if (a.type != b.type) return false;
        if (a.is_string_ref()) return m_a.str(a.str()) == m_b.str(b.str());
        return a.bits == b.bits;
    }
    static bool is_default_value(const Value& v) {
        switch (v.type) {
        case ValueType::None: return true;
        case ValueType::Integer: return v.as_int() == 0;
        case ValueType::Float: return v.as_float() == 0.0f;
        case ValueType::Bool: return !v.as_bool();
        default: return false;
        }
    }

    void describe_property(const Difference& d) {
        const size_t dot = d.where.find(".prop ");
        if (dot == std::string::npos) return;
        const std::string oname = d.where.substr(0, dot), pname = d.where.substr(dot + 6);
        const Object* oa = object_named(m_a, oname);
        const Object* ob = object_named(m_b, oname);
        if (!oa || !ob) return;
        const Property* pa = property_named(m_a, *oa, pname);
        const Property* pb = property_named(m_b, *ob, pname);
        const std::string name(m_b.str(ob->name));
        if (d.message == "only in A") {
            if (!pa) return;
            if (m_propRenames.count(lower(pname))) {
                add_script(name, "property-renamed", "Property " + bt(m_a.str(pa->name)) + " was renamed to " + bt(m_propRenames[lower(pname)]));
            } else {
                add_script(name, "property-removed", "Property " + bt(m_a.str(pa->name)) + " (" + type_text(m_a, pa->type) + ") was removed");
            }
            return;
        }
        if (d.message == "only in B") {
            if (!pb) return;
            if (m_propRenamedTo.count(lower(pname))) return;
            std::string t = "Property " + bt(m_b.str(pb->name)) + " (" + type_text(m_b, pb->type);
            if (pb->is_auto()) {
                if (const Variable* v = variable_named(m_b, *ob, m_b.str(pb->autoVar))) {
                    if (!is_default_value(v->initial)) t += ", default " + value_text(value_context(m_b, *ob), v->initial);
                }
            }
            add_script(name, "property-added", t + ") was added");
            return;
        }
        if (!pa || !pb) return;
        const std::string P = "Property " + bt(m_b.str(pb->name));
        if (d.message == "getter presence differs") {
            add_script(name, pb->getter ? "property-accessor-added" : "property-accessor-removed", P + (pb->getter ? " gained a getter" : " lost its getter"));
            return;
        }
        if (d.message == "setter presence differs") {
            add_script(name, pb->setter ? "property-accessor-added" : "property-accessor-removed", P + (pb->setter ? " gained a setter" : " lost its setter"));
            return;
        }
        // "declaration differs": type, doc, user flags, read/write/auto, autovar name.
        if (!eq_ci(m_a.str(pa->type), m_b.str(pb->type))) add_script(name, "property-retyped", P + " is now " + type_text(m_b, pb->type) + ", was " + type_text(m_a, pa->type));
        if (m_a.str(pa->docString) != m_b.str(pb->docString)) add_script(name, "property-doc-changed", "The documentation string of " + P.substr(9) + " changed");
        if (pa->is_auto() != pb->is_auto()) add_script(name, "property-flags-changed", P + (pb->is_auto() ? " is now Auto" : " is no longer Auto"));
        if (pa->writable() != pb->writable()) add_script(name, "property-flags-changed", P + (pb->writable() ? " is now writable" : " is now read-only"));
        if (pa->readable() != pb->readable()) add_script(name, "property-flags-changed", P + (pb->readable() ? " is now readable" : " is now write-only"));
        const std::set<std::string> fa = flag_names(m_a, pa->userFlags), fb = flag_names(m_b, pb->userFlags);
        if (fa != fb) {
            for (const std::string& f : fb) {
                if (!fa.count(f)) add_script(name, "property-flags-changed", P + " is now " + f);
            }
            for (const std::string& f : fa) {
                if (!fb.count(f)) add_script(name, "property-flags-changed", P + " is no longer " + f);
            }
        }
    }

    void describe_function(const Difference& d) {
        const size_t dot = d.where.find('.');
        if (dot == std::string::npos) return;
        const std::string oname = d.where.substr(0, dot);
        const Object* oa = object_named(m_a, oname);
        const Object* ob = object_named(m_b, oname);
        if (!oa || !ob) return;
        const std::string name(m_b.str(ob->name));
        if (starts_with_ci(d.message, "state only in")) {
            const std::string sname = d.where.substr(dot + 1);
            const bool inA = d.message.back() == 'A';
            const Object& o = inA ? *oa : *ob;
            const Script& s = inA ? m_a : m_b;
            for (const State& st : o.states) {
                if (!eq_ci(s.str(st.name), sname)) continue;
                const std::string S = "State " + bt(s.str(st.name));
                if (inA) add_script(name, "state-removed", S + " was removed with its " + plural(st.functions.size(), "function", "functions"));
                else add_script(name, "state-added", S + " was added with " + plural(st.functions.size(), "function", "functions"));
            }
            return;
        }
        FnKey k;
        if (!parse_function_where(d.where, k)) return;
        const FnRef* ra = find(m_fa, k);
        const FnRef* rb = find(m_fb, k);
        if (d.message == "only in A") {
            if (!ra || m_wholeStates.count(k.state)) return;
            add_script(name, "function-removed", removed_sentence(m_a, *ra));
            return;
        }
        if (d.message == "only in B") {
            if (!rb || m_wholeStates.count(k.state)) return;
            add_script(name, "function-added", added_sentence(m_b, *rb));
            return;
        }
        if (!ra || !rb) return;
        // A signature difference. The body is aligned separately.
        const Function& fa = *ra->f;
        const Function& fb = *rb->f;
        const std::string F = function_word(m_b, rb->name, fb) + " " + bt(rb->name);
        auto fn_add = [&](const char* kind, std::string msg) { add(name, rb->stateName, rb->name, kind, 0, 0, std::move(msg)); };
        if (!eq_ci(m_a.str(fa.returnType), m_b.str(fb.returnType))) {
            const std::string from = type_text(m_a, fa.returnType), to = type_text(m_b, fb.returnType);
            if (eq_ci(from, "none")) fn_add("return-type-changed", F + " now returns " + to + "; it returned nothing before");
            else if (eq_ci(to, "none")) fn_add("return-type-changed", F + " no longer returns a value (it returned " + from + ")");
            else fn_add("return-type-changed", F + " now returns " + to + ", was " + from);
        }
        describe_params(ra->name, *ra, *rb, fn_add);
        if (fa.is_global() != fb.is_global()) fn_add("function-flags-changed", F + (fb.is_global() ? " is now Global" : " is no longer Global"));
        if (fa.is_native() != fb.is_native()) fn_add("function-flags-changed", F + (fb.is_native() ? " is now Native" : " is no longer Native"));
        if (flag_names(m_a, fa.userFlags) != flag_names(m_b, fb.userFlags)) {
            fn_add("function-flags-changed", "The flags of " + bt(rb->name) + " changed from " + flags_text(flag_names(m_a, fa.userFlags)) + " to " + flags_text(flag_names(m_b, fb.userFlags)));
        }
        if (m_a.str(fa.docString) != m_b.str(fb.docString)) fn_add("function-doc-changed", "The documentation string of " + bt(rb->name) + " changed");
    }

    template <class Add>
    void describe_params(const std::string& fname, const FnRef& ra, const FnRef& rb, Add&& fn_add) {
        const std::vector<Param>& pa = ra.f->params;
        const std::vector<Param>& pb = rb.f->params;
        const std::string F = bt(fname);
        if (pa.size() == pb.size()) {
            for (size_t i = 0; i < pa.size(); ++i) {
                const bool sameName = eq_ci(m_a.str(pa[i].name), m_b.str(pb[i].name));
                const bool sameType = eq_ci(m_a.str(pa[i].type), m_b.str(pb[i].type));
                if (!sameName) fn_add("parameter-renamed", "Parameter " + bt(m_a.str(pa[i].name)) + " of " + F + " was renamed to " + bt(m_b.str(pb[i].name)));
                if (!sameType) fn_add("parameter-retyped", "Parameter " + bt(m_b.str(pb[i].name)) + " of " + F + " is now " + type_text(m_b, pb[i].type) + ", was " + type_text(m_a, pa[i].type));
            }
            return;
        }
        // Different counts: the parameters whose names the other side lacks.
        auto has = [](const Script& s, const std::vector<Param>& ps, std::string_view n) {
            for (const Param& p : ps) {
                if (eq_ci(s.str(p.name), n)) return true;
            }
            return false;
        };
        size_t reported = 0;
        for (const Param& p : pb) {
            if (!has(m_a, pa, m_b.str(p.name))) {
                fn_add("parameter-added", F + " gained the parameter " + bt(type_text(m_b, p.type) + " " + std::string(m_b.str(p.name))));
                ++reported;
            }
        }
        for (const Param& p : pa) {
            if (!has(m_b, pb, m_a.str(p.name))) {
                fn_add("parameter-removed", F + " lost its " + bt(m_a.str(p.name)) + " parameter");
                ++reported;
            }
        }
        if (reported == 0) fn_add("parameters-changed", "The parameters of " + F + " changed from (" + signature_text(m_a, "", *ra.f).substr(1) + " to (" + signature_text(m_b, "", *rb.f).substr(1));
    }

    std::string removed_sentence(const Script& s, const FnRef& r) const {
        std::string t = function_word(s, r.name, *r.f) + " " + bt(signature_text(s, r.name, *r.f)) + " was removed";
        if (!r.stateName.empty()) t += " from state " + bt(r.stateName);
        return t;
    }
    std::string added_sentence(const Script& s, const FnRef& r) const {
        std::string t = function_word(s, r.name, *r.f) + " " + bt(signature_text(s, r.name, *r.f)) + " was added";
        if (!r.stateName.empty()) t += " to state " + bt(r.stateName);
        if (r.f->is_native()) return t + " (native)";
        Body b;
        if (lift(s, r, m_opt.resolver, b)) {
            size_t n = count_stmts(b.stmts);
            if (!b.stmts.empty() && is_bare_return(*b.stmts.back())) --n;
            t += " (" + plural(n, "statement", "statements") + ")";
        }
        return t;
    }

    // --- layer 2: renames ------------------------------------------------------------

    void seed_param_renames(Pair& p) {
        const std::vector<Param>& pa = p.a->ref.f->params;
        const std::vector<Param>& pb = p.b->ref.f->params;
        if (pa.size() != pb.size()) return;
        for (size_t i = 0; i < pa.size(); ++i) {
            if (eq_ci(m_a.str(pa[i].name), m_b.str(pb[i].name))) continue;
            p.renames[pa[i].name] = p.b->fc->display_name(pb[i].name);
        }
    }

    // Shape keys: every parameter, local and script variable is spelled by its
    // type alone, so two versions that differ only in those names align.
    std::map<StrIdx, std::string> shape_names(const Body& b) const {
        std::map<StrIdx, std::string> names;
        auto abstract = [&](StrIdx n) {
            if (slot_class(*b.fc, n) == SlotClass::None) return;
            names[n] = "$" + b.fc->type_of(n);
        };
        for (const Param& p : b.ref.f->params) abstract(p.name);
        for (const Param& p : b.ref.f->locals) abstract(p.name);
        for (const Variable& v : b.ref.o->variables) abstract(v.name);
        return names;
    }

    struct Votes {
        std::map<std::string, std::map<std::string, size_t>> localVotes;  // old lowered name -> new lowered name -> count
        std::map<StrIdx, StrIdx> localIdx;                                 // old slot -> new slot (last seen)
    };

    void collect_votes(Pair& p) {
        const std::map<StrIdx, std::string> na = shape_names(*p.a);
        const std::map<StrIdx, std::string> nb = shape_names(*p.b);
        Votes v;
        vote_lists(p, p.a->stmts, p.b->stmts, na, nb, v);
        // Locals and parameters of this function.
        const FnContext& fa = *p.a->fc;
        const FnContext& fb = *p.b->fc;
        for (const auto& [oldName, newVotes] : v.localVotes) {
            if (newVotes.size() != 1) continue;
            const std::string& newName = newVotes.begin()->first;
            if (oldName == newName) continue;
            // Unanimous the other way too.
            size_t others = 0;
            for (const auto& [o2, nv] : v.localVotes) {
                if (o2 != oldName && nv.count(newName)) ++others;
            }
            if (others) continue;
            StrIdx oi = 0, ni = 0;
            bool found = false;
            for (const auto& [a, b] : v.localIdx) {
                if (lower(m_a.str(a)) == oldName && lower(m_b.str(b)) == newName) {
                    oi = a;
                    ni = b;
                    found = true;
                }
            }
            if (!found) continue;
            const SlotClass ca = slot_class(fa, oi), cb = slot_class(fb, ni);
            if (ca != cb || ca == SlotClass::None || ca == SlotClass::Param) continue;  // parameters: by position
            if (fa.type_of(oi) != fb.type_of(ni)) continue;
            // A rename, not a reuse: the old name is gone and the new one is new.
            if (ca == SlotClass::Local) {
                if (has_local(fb, m_b, oldName) || has_local(fa, m_a, newName)) continue;
                p.renames[oi] = fb.display_name(ni);
                // Another compiler's mangling of the same name is not a rename.
                if (!eq_ci(fa.display_name(oi), fb.display_name(ni))) p.renamedLocals.push_back({fa.display_name(oi), fb.display_name(ni)});
            } else {
                if (variable_named(m_b, fb.o, oldName) || variable_named(m_a, fa.o, newName)) continue;
                ++m_varVotes[oldName][newName];
                m_varVoteIdx[oldName] = {oi, ni};
            }
        }
    }

    static bool has_local(const FnContext& fc, const Script& s, const std::string& lname) {
        for (const Param& p : fc.f.locals) {
            if (lower(s.str(p.name)) == lname) return true;
        }
        for (const Param& p : fc.f.params) {
            if (lower(s.str(p.name)) == lname) return true;
        }
        return false;
    }

    void vote_lists(Pair& p, const std::vector<StmtPtr>& la, const std::vector<StmtPtr>& lb, const std::map<StrIdx, std::string>& na,
                    const std::map<StrIdx, std::string>& nb, Votes& v) {
        std::vector<std::string> ka, kb;
        for (const StmtPtr& s : la) ka.push_back(fold(render_stmt(*p.a->fc, *s, &na)));
        for (const StmtPtr& s : lb) kb.push_back(fold(render_stmt(*p.b->fc, *s, &nb)));
        for (const EditOp& op : lcs(ka, kb)) {
            if (op.op != Op::Keep) continue;
            const Stmt& sa = *la[op.i];
            const Stmt& sb = *lb[op.j];
            if (sa.lhs && sb.lhs) vote_expr(p, *sa.lhs, *sb.lhs, v);
            if (sa.rhs && sb.rhs) vote_expr(p, *sa.rhs, *sb.rhs, v);
            vote_lists(p, sa.body, sb.body, na, nb, v);
            vote_lists(p, sa.elseBody, sb.elseBody, na, nb, v);
        }
    }

    void vote_expr(Pair& p, const Expr& a, const Expr& b, Votes& v) {
        if (a.kind != b.kind || a.kids.size() != b.kids.size()) return;
        if (a.kind == ExprKind::Var) {
            if (slot_class(*p.a->fc, a.name) != SlotClass::None && slot_class(*p.b->fc, b.name) != SlotClass::None) {
                const std::string oa = lower(m_a.str(a.name)), ob = lower(m_b.str(b.name));
                if (oa != ob) {
                    ++v.localVotes[oa][ob];
                    v.localIdx[a.name] = b.name;
                } else {
                    ++v.localVotes[oa][ob];  // an unchanged use counts against a rename of this name
                }
            }
            return;
        }
        for (size_t i = 0; i < a.kids.size(); ++i) vote_expr(p, *a.kids[i], *b.kids[i], v);
    }

    void accept_renames() {
        // Script variables: unanimous across every function.
        for (const auto& [oldName, newVotes] : m_varVotes) {
            if (newVotes.size() != 1) continue;
            const std::string& newName = newVotes.begin()->first;
            size_t others = 0;
            for (const auto& [o2, nv] : m_varVotes) {
                if (o2 != oldName && nv.count(newName)) ++others;
            }
            if (others) continue;
            const auto [oi, ni] = m_varVoteIdx[oldName];
            if (m_a.objects.empty() || m_b.objects.empty()) break;
            // Through an autovar, the property.
            const Object& oa = m_a.objects[0];
            const Object& ob = m_b.objects[0];
            const Property* pa = property_of_autovar(m_a, oa, m_a.str(oi));
            const Property* pb = property_of_autovar(m_b, ob, m_b.str(ni));
            if (pa && pb) {
                if (!eq_ci(m_a.str(pa->type), m_b.str(pb->type))) continue;
                m_propRenames[lower(m_a.str(pa->name))] = std::string(m_b.str(pb->name));
                m_propRenamedTo.insert(lower(m_b.str(pb->name)));
            } else if (!pa && !pb) {
                m_varRenames[oldName] = std::string(m_b.str(ni));
                m_varRenamedTo.insert(newName);
            } else {
                continue;
            }
            m_slotRenames[oi] = ni;
        }
        // Apply script-level renames to every pair's old side.
        for (Pair& p : m_pairs) {
            if (!p.a->ok || !p.b->ok) continue;
            for (const auto& [oi, ni] : m_slotRenames) p.renames[oi] = p.b->fc->display_name(ni);
        }
    }

    // --- layer 2 and 3: bodies ------------------------------------------------------

    struct Walk {
        Pair* p = nullptr;
        DiffResult* r = nullptr;
    };

    void body_pair(Pair& p, DiffResult& r) {
        if (!p.a->ok || !p.b->ok) {
            ++r.functionsSkipped;
            add_fn(p, "body-unreadable", 0, 0, "The body of " + bt(p.b->ref.name) + " could not be read on " + (!p.a->ok && !p.b->ok ? "either side" : !p.a->ok ? "the old side" : "the new side") + ", so its statements were not compared");
            return;
        }
        ++r.functionsCompared;
        for (const auto& [from, to] : p.renamedLocals) add_fn(p, "local-renamed", 0, 0, "Local " + bt(from) + " was renamed to " + bt(to));

        const bool emptyA = body_is_empty(p.a->stmts), emptyB = body_is_empty(p.b->stmts);
        Walk w;
        w.p = &p;
        w.r = &r;
        if (!emptyA && emptyB) {
            // The statements are named when there are few, else counted.
            std::vector<const Stmt*> gone;
            for (const StmtPtr& st : p.a->stmts) {
                if (!is_bare_return(*st)) gone.push_back(st.get());
            }
            size_t n = 0;
            for (const Stmt* st : gone) n += 1 + count_stmts(st->body) + count_stmts(st->elseBody);
            add_fn(p, "function-emptied", 0, p.a->line(*p.a->stmts[0]), "The body of " + bt(p.b->ref.name) + " was emptied (" + plural(n, "statement", "statements") + " removed)");
            if (gone.size() < m_opt.summariseFrom) {
                for (const Stmt* st : gone) single(w, *st, false);
            }
            return;
        }
        if (emptyA && !emptyB) {
            size_t n = count_stmts(p.b->stmts);
            if (!p.b->stmts.empty() && is_bare_return(*p.b->stmts.back())) --n;
            add_fn(p, "function-filled", p.b->line(*p.b->stmts[0]), 0, bt(p.b->ref.name) + ", which was empty, now has " + plural(n, "statement", "statements"));
        }
        diff_lists(w, p.a->stmts, p.b->stmts);
    }

    std::string exact_a(const Pair& p, const Stmt& st) const { return render_stmt(*p.a->fc, st, &p.renames); }
    std::string exact_b(const Pair& p, const Stmt& st) const { return render_stmt(*p.b->fc, st); }
    // The whole statement, nested ones included, folded: the alignment key.
    std::string deep_key(const Pair& p, const Stmt& st, bool isNew) const {
        std::string k = fold(isNew ? exact_b(p, st) : exact_a(p, st));
        if (st.body.empty() && st.elseBody.empty()) return k;
        k += " {";
        for (const StmtPtr& s : st.body) k += deep_key(p, *s, isNew) + ";";
        k += "}";
        if (!st.elseBody.empty()) {
            k += " else {";
            for (const StmtPtr& s : st.elseBody) k += deep_key(p, *s, isNew) + ";";
            k += "}";
        }
        return k;
    }
    std::string expr_a(const Pair& p, const Expr& e) const { return render_expr(*p.a->fc, e, &p.renames); }
    std::string expr_b(const Pair& p, const Expr& e) const { return render_expr(*p.b->fc, e); }
    // True when the two expressions read the same (spelling aside).
    bool same_text(const Pair& p, const Expr& a, const Expr& b) const { return fold(expr_a(p, a)) == fold(expr_b(p, b)); }

    // The same value spelled as another type: 1 for 1.0, 1 for True.
    static bool same_value(const Expr& a, const Expr& b) {
        if (a.kind != ExprKind::Lit || b.kind != ExprKind::Lit) return false;
        auto num = [](const Value& v, double& out) {
            if (v.type == ValueType::Integer) out = v.as_int();
            else if (v.type == ValueType::Float) out = v.as_float();
            else if (v.type == ValueType::Bool) out = v.as_bool() ? 1 : 0;
            else return false;
            return true;
        };
        double x = 0, y = 0;
        return num(a.lit, x) && num(b.lit, y) && x == y;
    }
    // True when the two expressions mean the same (spelling and literal types aside).
    bool equivalent(const Pair& p, const Expr& a0, const Expr& b0) const {
        const Expr& a = peel(p, a0, false);
        const Expr& b = peel(p, b0, true);
        if (same_text(p, a, b) || same_value(a, b)) return true;
        if (a.kids.empty() || !same_head(a, b, m_a, m_b)) return false;
        for (size_t i = 0; i < a.kids.size(); ++i) {
            if (!equivalent(p, *a.kids[i], *b.kids[i])) return false;
        }
        return true;
    }

    // A cast the writer leaves out (implicit, or an upcast it knows about)
    // is transparent: the expression under it is what the reader sees.
    const Expr& peel(const Pair& p, const Expr& e, bool isNew) const {
        const Expr* cur = &e;
        while (cur->kind == ExprKind::Cast && !cur->kids.empty()) {
            const std::string outer = isNew ? expr_b(p, *cur) : expr_a(p, *cur);
            const std::string inner = isNew ? expr_b(p, *cur->kids[0]) : expr_a(p, *cur->kids[0]);
            if (outer != inner) break;
            cur = cur->kids[0].get();
        }
        return *cur;
    }

    // What a statement is anchored by, for pairing a removed with an added one.
    std::string pair_key(const Script& s, const FnContext& fc, const Stmt& st, const std::map<StrIdx, std::string>* names) const {
        switch (st.kind) {
        case StmtKind::If: return "If";
        case StmtKind::While: return "While";
        case StmtKind::Return: return "Return";
        case StmtKind::Decl: return "Decl " + st.declName;
        case StmtKind::Expr: return "Call " + callee_key(s, strip_casts(*st.rhs));
        case StmtKind::Assign:
            if (st.lhs->kind == ExprKind::Var && fc.is_nonevar(st.lhs->name)) return "Call " + callee_key(s, strip_casts(*st.rhs));
            return "Assign " + render_expr(fc, *st.lhs, names);
        }
        return "";
    }

    void diff_lists(Walk& w, const std::vector<StmtPtr>& la, const std::vector<StmtPtr>& lb) {
        Pair& p = *w.p;
        std::vector<std::string> ka, kb;
        for (const StmtPtr& s : la) ka.push_back(deep_key(p, *s, false));
        for (const StmtPtr& s : lb) kb.push_back(deep_key(p, *s, true));
        const std::vector<EditOp> ops = lcs(ka, kb);
        size_t i = 0;
        while (i < ops.size()) {
            if (ops[i].op == Op::Keep) {
                ++i;
                continue;
            }
            // A gap: the removed and the added statements between two kept ones.
            std::vector<const Stmt*> removed, added;
            size_t j = i;
            while (j < ops.size() && ops[j].op != Op::Keep) {
                if (ops[j].op == Op::Del) removed.push_back(la[ops[j].i].get());
                else added.push_back(lb[ops[j].j].get());
                ++j;
            }
            gap(w, removed, added);
            i = j;
        }
    }

    // Lines of a run, for the summary sentence.
    static void line_range(const Body& b, const std::vector<const Stmt*>& ss, uint16_t& lo, uint16_t& hi) {
        lo = hi = 0;
        for (const Stmt* s : ss) {
            const uint16_t l = b.line(*s);
            if (!l) continue;
            if (!lo || l < lo) lo = l;
            if (l > hi) hi = l;
        }
    }


    static void calls_in(const Script& s, const Stmt& st, std::set<std::string>& out) {
        each_expr_in(st, [&](const Expr& e) {
            if (e.kind == ExprKind::Call) out.insert(callee_text(s, e));
        });
    }

    void call_sentences(Walk& w, const std::set<std::string>& added, const std::set<std::string>& removed, uint16_t line, uint16_t oldLine, bool inRun) {
        std::vector<std::string> onlyAdded, onlyRemoved;
        for (const std::string& c : added) {
            if (!removed.count(c)) onlyAdded.push_back(bt(c));
        }
        for (const std::string& c : removed) {
            if (!added.count(c)) onlyRemoved.push_back(bt(c));
        }
        auto list = [](std::vector<std::string>& v) {
            if (v.size() <= 6) return join_and(v);
            std::string out;
            for (size_t i = 0; i < 6; ++i) out += (i ? ", " : "") + v[i];
            return out + " and " + std::to_string(v.size() - 6) + " more";
        };
        const char* where = inRun ? " in that run" : "";
        if (!onlyAdded.empty()) add_fn(*w.p, "calls-added", line, 0, (onlyAdded.size() == 1 ? "A call to " : "Calls to ") + list(onlyAdded) + (onlyAdded.size() == 1 ? " was" : " were") + " added" + where);
        if (!onlyRemoved.empty()) add_fn(*w.p, "calls-removed", 0, oldLine, (onlyRemoved.size() == 1 ? "A call to " : "Calls to ") + list(onlyRemoved) + (onlyRemoved.size() == 1 ? " was" : " were") + " removed" + where);
    }

    // `branches`: the statements are ElseIf branches of two chains (spelled
    // ElseIf, their own else parts not compared here).
    void gap(Walk& w, const std::vector<const Stmt*>& removed, const std::vector<const Stmt*>& added, bool branches = false) {
        Pair& p = *w.p;
        // Pair removed with added statements by what they anchor on, in order;
        // a lone removed and a lone added statement pair up regardless.
        std::vector<std::pair<const Stmt*, const Stmt*>> pairs;
        std::vector<const Stmt*> onlyRemoved, onlyAdded;
        if (removed.size() == 1 && added.size() == 1 && removed[0]->kind == added[0]->kind) {
            pairs.push_back({removed[0], added[0]});
        } else {
            // Best match first: the same anchor (assigned slot, callee) or the
            // same header, and for If / While how much of the bodies is shared.
            std::vector<std::vector<double>> score(removed.size(), std::vector<double>(added.size(), 0));
            for (size_t i = 0; i < removed.size(); ++i) {
                const Stmt& r = *removed[i];
                const std::string kr = fold(pair_key(m_a, *p.a->fc, r, &p.renames));
                const std::string hr = fold(exact_a(p, r));
                std::vector<std::string> br;
                for (const StmtPtr& s : r.body) br.push_back(deep_key(p, *s, false));
                for (size_t k = 0; k < added.size(); ++k) {
                    const Stmt& a = *added[k];
                    if (a.kind != r.kind) continue;
                    double sc = 0;
                    if (fold(pair_key(m_b, *p.b->fc, a, nullptr)) == kr) sc += 1;
                    if (fold(exact_b(p, a)) == hr) sc += 1;
                    if (r.kind == StmtKind::If || r.kind == StmtKind::While) {
                        std::vector<std::string> ba;
                        for (const StmtPtr& s : a.body) ba.push_back(deep_key(p, *s, true));
                        size_t common = 0;
                        for (const EditOp& op : lcs(br, ba)) common += op.op == Op::Keep;
                        if (!br.empty() || !ba.empty()) sc += 2.0 * common / double(br.size() + ba.size());
                    }
                    score[i][k] = sc;
                }
            }
            std::vector<bool> usedR(removed.size(), false), usedA(added.size(), false);
            for (;;) {
                double best = 0;
                size_t bi = 0, bk = 0;
                for (size_t i = 0; i < removed.size(); ++i) {
                    if (usedR[i]) continue;
                    for (size_t k = 0; k < added.size(); ++k) {
                        if (!usedA[k] && score[i][k] > best) {
                            best = score[i][k];
                            bi = i;
                            bk = k;
                        }
                    }
                }
                if (best <= 0) break;
                usedR[bi] = usedA[bk] = true;
                pairs.push_back({removed[bi], added[bk]});
            }
            std::sort(pairs.begin(), pairs.end(), [&](const auto& x, const auto& y) {
                return std::find(added.begin(), added.end(), x.second) < std::find(added.begin(), added.end(), y.second);
            });
            for (size_t i = 0; i < removed.size(); ++i) {
                if (!usedR[i]) onlyRemoved.push_back(removed[i]);
            }
            for (size_t k = 0; k < added.size(); ++k) {
                if (!usedA[k]) onlyAdded.push_back(added[k]);
            }
        }
        // An If or While added around statements that were there before, or
        // removed from around statements that stay: the statements moved, they
        // did not change.
        std::vector<std::string> removedKeys, addedKeys;
        for (const Stmt* s : onlyRemoved) removedKeys.push_back(fold(exact_a(p, *s)));
        for (const Stmt* s : onlyAdded) addedKeys.push_back(fold(exact_b(p, *s)));
        auto body_keys = [&](const Stmt& st, bool isNew) {
            std::vector<std::string> keys;
            for (const StmtPtr& s : st.body) keys.push_back(fold(isNew ? exact_b(p, *s) : exact_a(p, *s)));
            return keys;
        };
        // The wrapped statements are a subsequence of the new If's body in the
        // old order, and every removed statement is among them.
        auto covers = [](const std::vector<std::string>& body, const std::vector<std::string>& moved) {
            if (moved.empty() || moved.size() > body.size()) return false;
            size_t j = 0;
            for (const std::string& k : moved) {
                while (j < body.size() && body[j] != k) ++j;
                if (j == body.size()) return false;
                ++j;
            }
            return true;
        };
        std::vector<const Stmt*> wrapped, unwrapped;
        for (const Stmt* s : onlyAdded) {
            if (branches) break;
            if ((s->kind != StmtKind::If && s->kind != StmtKind::While) || !s->elseBody.empty()) continue;
            if (!covers(body_keys(*s, true), removedKeys)) continue;
            const size_t n = onlyRemoved.size();
            std::string what = n == 1 ? bt(exact_a(p, *onlyRemoved[0])) : plural(n, "statement that was there before", "statements that were there before");
            const size_t extra = s->body.size() - n;
            if (extra) what += " (with " + plural(extra, "new statement", "new statements") + " beside " + (n == 1 ? "it" : "them") + ")";
            add_fn(p, s->kind == StmtKind::If ? "if-wrapped" : "loop-wrapped", p.b->line(*s), p.a->line(*onlyRemoved[0]), bt(exact_b(p, *s)) + " now guards " + what);
            wrapped.push_back(s);
            onlyRemoved.clear();
            break;
        }
        for (const Stmt* s : onlyRemoved) {
            if (branches) break;
            if ((s->kind != StmtKind::If && s->kind != StmtKind::While) || !s->elseBody.empty()) continue;
            if (!covers(body_keys(*s, false), addedKeys)) continue;
            const size_t n = onlyAdded.size();
            const std::string what = n == 1 ? bt(exact_b(p, *onlyAdded[0])) : plural(n, "statement", "statements");
            add_fn(p, s->kind == StmtKind::If ? "if-unwrapped" : "loop-unwrapped", p.b->line(*onlyAdded[0]), p.a->line(*s), bt(exact_a(p, *s)) + " was removed from around " + what + ", which now " + (n == 1 ? "runs" : "run") + " unconditionally");
            unwrapped.push_back(s);
            onlyAdded.clear();
            break;
        }
        for (const Stmt* s : wrapped) onlyAdded.erase(std::find(onlyAdded.begin(), onlyAdded.end(), s));
        for (const Stmt* s : unwrapped) onlyRemoved.erase(std::find(onlyRemoved.begin(), onlyRemoved.end(), s));

        const size_t sentences = pairs.size() + onlyRemoved.size() + onlyAdded.size();
        if (m_opt.summariseFrom && sentences >= m_opt.summariseFrom) {
            uint16_t lo = 0, hi = 0, olo = 0, ohi = 0;
            line_range(*p.b, added, lo, hi);
            line_range(*p.a, removed, olo, ohi);
            const bool onlyAdds = onlyRemoved.empty() && pairs.empty(), onlyRemoves = onlyAdded.empty() && pairs.empty();
            const char* what = branches ? "ElseIf branches" : "statements";
            std::string t;
            if (onlyAdds) t = !lo ? std::string("A run of ") + what + " was added: " : hi > lo ? "Lines " + std::to_string(lo) + "-" + std::to_string(hi) + " were added: " : "Line " + std::to_string(lo) + " was added: ";
            else if (onlyRemoves) t = !olo ? std::string("A run of ") + what + " was removed: " : ohi > olo ? "Old lines " + std::to_string(olo) + "-" + std::to_string(ohi) + " were removed: " : "Old line " + std::to_string(olo) + " was removed: ";
            else t = !lo ? std::string("A run of ") + what + " was rewritten: " : hi > lo ? "Lines " + std::to_string(lo) + "-" + std::to_string(hi) + " were rewritten: " : "Line " + std::to_string(lo) + " was rewritten: ";
            const char* one = branches ? "branch" : "statement";
            const char* many = branches ? "branches" : "statements";
            std::vector<std::string> parts;
            if (!onlyRemoved.empty()) parts.push_back(plural(onlyRemoved.size(), (std::string(one) + " removed").c_str(), (std::string(many) + " removed").c_str()));
            if (!onlyAdded.empty()) parts.push_back(plural(onlyAdded.size(), (std::string(one) + " added").c_str(), (std::string(many) + " added").c_str()));
            if (!pairs.empty()) parts.push_back(plural(pairs.size(), (std::string(one) + " changed").c_str(), (std::string(many) + " changed").c_str()));
            add_fn(p, "statements-changed", lo, olo, t + join_and(parts));
            std::set<std::string> ca, cr;
            for (const Stmt* s : added) calls_in(m_b, *s, ca);
            for (const Stmt* s : removed) calls_in(m_a, *s, cr);
            call_sentences(w, ca, cr, lo, olo, true);
            return;
        }
        // In the new version's order: pairs and additions by their new
        // statement, removals where they were.
        for (const auto& [a, b] : pairs) changed(w, *a, *b, !branches);
        for (const Stmt* s : onlyRemoved) single(w, *s, false, branches);
        for (const Stmt* s : onlyAdded) single(w, *s, true, branches);
    }

    // A statement only one side has.
    void single(Walk& w, const Stmt& st, bool isNew, bool branch = false) {
        Pair& p = *w.p;
        const Script& s = isNew ? m_b : m_a;
        std::string text = isNew ? exact_b(p, st) : exact_a(p, st);
        if (branch) text = "Else" + text;
        const uint16_t line = isNew ? p.b->line(st) : 0;
        const uint16_t oldLine = isNew ? 0 : p.a->line(st);
        const char* verb = isNew ? " was added" : " was removed";
        switch (st.kind) {
        case StmtKind::If:
        case StmtKind::While: {
            const size_t n = count_stmts(st.body) + (branch ? 0 : count_stmts(st.elseBody));
            const std::string inside = isNew ? " with " + plural(n, "statement", "statements") + " inside" : " with its " + plural(n, "statement", "statements");
            add_fn(p, isNew ? (st.kind == StmtKind::If ? "if-added" : "loop-added") : (st.kind == StmtKind::If ? "if-removed" : "loop-removed"), line, oldLine, bt(text) + verb + inside);
            return;
        }
        case StmtKind::Return: add_fn(p, isNew ? "return-added" : "return-removed", line, oldLine, bt(text) + verb); return;
        case StmtKind::Expr:
        case StmtKind::Assign: {
            const Expr& r = strip_casts(*st.rhs);
            const FnContext& fc = isNew ? *p.b->fc : *p.a->fc;
            const bool isCall = st.kind == StmtKind::Expr || (st.lhs->kind == ExprKind::Var && fc.is_nonevar(st.lhs->name));
            if (isCall && r.kind == ExprKind::Call) {
                const std::string key = callee_key(s, r);
                if (is_registration(key)) add_fn(p, isNew ? "registration-added" : "registration-removed", line, oldLine, "Registration " + bt(text) + verb);
                else if (is_state_change(key)) add_fn(p, isNew ? "state-change-added" : "state-change-removed", line, oldLine, "State change " + bt(text) + verb);
                else add_fn(p, isNew ? "call-added" : "call-removed", line, oldLine, "Call " + bt(text) + verb);
                return;
            }
            add_fn(p, isNew ? "assignment-added" : "assignment-removed", line, oldLine, "Assignment " + bt(text) + verb);
            return;
        }
        case StmtKind::Decl: add_fn(p, isNew ? "statement-added" : "statement-removed", line, oldLine, bt(text) + verb); return;
        }
    }

    // The deepest single place where two expressions differ.
    struct Spot {
        const Expr* a = nullptr;
        const Expr* b = nullptr;
        const Expr* parentA = nullptr;
        const Expr* parentB = nullptr;
        size_t index = 0;  // kid index in the parents
    };

    static bool same_head(const Expr& a, const Expr& b, const Script& sa, const Script& sb) {
        if (a.kind != b.kind || a.kids.size() != b.kids.size()) return false;
        switch (a.kind) {
        case ExprKind::Unary:
        case ExprKind::Binary: return a.op == b.op;
        case ExprKind::Call: return a.call == b.call && eq_ci(sa.str(a.name), sb.str(b.name)) && (a.call != CallKind::Static || eq_ci(sa.str(a.cls), sb.str(b.cls)));
        case ExprKind::PropGet: return eq_ci(sa.str(a.name), sb.str(b.name));
        case ExprKind::Cast: return eq_ci(a.type, b.type);
        case ExprKind::ArrayFind: return a.reverse == b.reverse;
        default: return true;
        }
    }

    void find_spot(const Pair& p, const Expr& a0, const Expr& b0, Spot& spot) {
        const Expr& a = peel(p, a0, false);
        const Expr& b = peel(p, b0, true);
        spot.a = &a;
        spot.b = &b;
        if (!same_head(a, b, m_a, m_b)) return;
        size_t differing = 0, which = 0;
        for (size_t i = 0; i < a.kids.size(); ++i) {
            if (!equivalent(p, *a.kids[i], *b.kids[i])) {
                ++differing;
                which = i;
            }
        }
        if (differing != 1) return;
        spot.parentA = &a;
        spot.parentB = &b;
        spot.index = which;
        find_spot(p, *a.kids[which], *b.kids[which], spot);
    }

    // The literal of a FormID call's first argument, in hex as sources write it.
    std::string literal_text(const Pair& p, const Expr& lit, const Expr* parent, size_t index, bool isNew) const {
        const Script& s = isNew ? m_b : m_a;
        if (parent && parent->kind == ExprKind::Call && lit.lit.type == ValueType::Integer && lit.lit.as_int() >= 0) {
            const size_t first = parent->call == CallKind::Method ? 1 : 0;
            if (index == first && is_formid_callee(callee_key(s, *parent))) {
                char hex[16];
                std::snprintf(hex, sizeof hex, "0x%06X", unsigned(lit.lit.as_int()));
                return hex;
            }
        }
        return isNew ? expr_b(p, lit) : expr_a(p, lit);
    }

    // A removed statement paired with an added one: one sentence on what
    // differs, then the bodies of a compound statement.
    void changed(Walk& w, const Stmt& a, const Stmt& b, bool recurseElse = true) {
        Pair& p = *w.p;
        const uint16_t line = p.b->line(b), oldLine = p.a->line(a);
        const std::string ta = exact_a(p, a), tb = exact_b(p, b);
        bool explained = a.kind == b.kind && (!a.rhs) == (!b.rhs) && (!a.lhs) == (!b.lhs) && (!a.rhs || equivalent(p, *a.rhs, *b.rhs)) &&
                         (!a.lhs || equivalent(p, *a.lhs, *b.lhs));
        if (!explained && a.kind == b.kind && a.rhs && b.rhs && (a.kind != StmtKind::Assign || (a.lhs && b.lhs && same_text(p, *a.lhs, *b.lhs)))) {
            Spot spot;
            find_spot(p, *a.rhs, *b.rhs, spot);
            explained = explain(w, a, b, spot, ta, tb, line, oldLine);
        }
        if (!explained) {
            const char* kind = a.kind == StmtKind::If || a.kind == StmtKind::While ? "condition-changed" : a.kind == StmtKind::Return ? "return-changed" : "statement-changed";
            add_fn(p, kind, line, oldLine, bt(ta) + " became " + bt(tb));
        }
        if (a.kind == StmtKind::If || a.kind == StmtKind::While) diff_compound(w, a, b, recurseElse);
    }

    // An If whose else part is one If is an ElseIf chain.
    static bool is_chain(const Stmt& st) { return st.kind == StmtKind::If && st.elseBody.size() == 1 && st.elseBody[0]->kind == StmtKind::If; }

    // The bodies of two paired compound statements. The ElseIf branches of two
    // chains are aligned as a list, so an inserted branch does not read as a
    // change to every branch after it.
    void diff_compound(Walk& w, const Stmt& a, const Stmt& b, bool withElse) {
        diff_lists(w, a.body, b.body);
        if (!withElse) return;
        if (!(is_chain(a) && is_chain(b))) {
            diff_lists(w, a.elseBody, b.elseBody);
            return;
        }
        Pair& p = *w.p;
        std::vector<const Stmt*> ba, bb;
        const Stmt* ta = &a;
        while (is_chain(*ta)) {
            ta = ta->elseBody[0].get();
            ba.push_back(ta);
        }
        const Stmt* tb = &b;
        while (is_chain(*tb)) {
            tb = tb->elseBody[0].get();
            bb.push_back(tb);
        }
        auto branch_key = [&](const Stmt& st, bool isNew) {
            std::string k = fold(isNew ? exact_b(p, st) : exact_a(p, st)) + " {";
            for (const StmtPtr& s : st.body) k += deep_key(p, *s, isNew) + ";";
            return k + "}";
        };
        std::vector<std::string> ka, kb;
        for (const Stmt* st : ba) ka.push_back(branch_key(*st, false));
        for (const Stmt* st : bb) kb.push_back(branch_key(*st, true));
        const std::vector<EditOp> ops = lcs(ka, kb);
        size_t i = 0;
        while (i < ops.size()) {
            if (ops[i].op == Op::Keep) {
                ++i;
                continue;
            }
            std::vector<const Stmt*> removed, added;
            size_t j = i;
            while (j < ops.size() && ops[j].op != Op::Keep) {
                if (ops[j].op == Op::Del) removed.push_back(ba[ops[j].i]);
                else added.push_back(bb[ops[j].j]);
                ++j;
            }
            gap(w, removed, added, true);
            i = j;
        }
        // The final Else parts.
        diff_lists(w, ta->elseBody, tb->elseBody);
    }

    bool explain(Walk& w, const Stmt& sa, const Stmt& sb, const Spot& spot, const std::string& ta, const std::string& tb, uint16_t line, uint16_t oldLine) {
        Pair& p = *w.p;
        const Expr& a = *spot.a;
        const Expr& b = *spot.b;
        const bool isCond = sa.kind == StmtKind::If || sa.kind == StmtKind::While;
        const std::string in = " in " + bt(tb);
        const std::string condB = isCond ? expr_b(p, *sb.rhs) : "";

        // The receiver or an argument of a call.
        const Expr* callB = spot.parentB && spot.parentB->kind == ExprKind::Call ? spot.parentB : nullptr;
        const size_t firstArg = callB && callB->call == CallKind::Method ? 1 : 0;
        const bool isReceiver = callB && callB->call == CallKind::Method && spot.index == 0;
        const bool isArgument = callB && spot.index >= firstArg;

        if (isArgument && a.kind == ExprKind::Lit && a.lit.type == ValueType::None && b.kind != ExprKind::Lit) {
            add_fn(p, "argument-added", line, oldLine, bt(expr_b(p, b)) + " is now passed to " + bt(callee_text(m_b, *callB)) + " where that argument was omitted" + in);
            return true;
        }
        if (isArgument && b.kind == ExprKind::Lit && b.lit.type == ValueType::None && a.kind != ExprKind::Lit) {
            add_fn(p, "argument-removed", line, oldLine, bt(expr_a(p, a)) + " is no longer passed to " + bt(callee_text(m_b, *callB)) + " (that argument is now omitted)" + in);
            return true;
        }
        if (a.kind == ExprKind::Lit && b.kind == ExprKind::Lit) {
            const std::string from = literal_text(p, a, spot.parentA, spot.index, false);
            const std::string to = literal_text(p, b, spot.parentB, spot.index, true);
            const char* kind = "literal-changed";
            std::string what;
            if (spot.parentB && spot.parentB->kind == ExprKind::Call) {
                const std::string key = callee_key(m_b, *spot.parentB);
                const size_t first = spot.parentB->call == CallKind::Method ? 1 : 0;
                if (is_formid_callee(key) && spot.index == first) {
                    kind = "formid-changed";
                    what = "FormID ";
                } else if (ends_with_ci(key, "getformfromfile") && spot.index == first + 1 && b.lit.type == ValueType::String) {
                    kind = "plugin-changed";
                    what = "Plugin ";
                } else if (b.lit.type == ValueType::String) {
                    kind = "string-changed";
                    what = "Text ";
                }
            } else if (b.lit.type == ValueType::String) {
                kind = "string-changed";
                what = "Text ";
            }
            add_fn(p, kind, line, oldLine, what + bt(from) + " became " + bt(to) + in);
            return true;
        }
        // A term added to or dropped from an && / || chain.
        if ((b.kind == ExprKind::And || b.kind == ExprKind::Or) && b.kids.size() == 2) {
            const std::string sa_text = expr_a(p, a);
            for (size_t k = 0; k < 2; ++k) {
                if (!same_text(p, a, *b.kids[k])) continue;
                const std::string term = expr_b(p, *b.kids[1 - k]);
                const std::string op = b.kind == ExprKind::And ? "&& " : "|| ";
                if (isCond) add_fn(p, "condition-gained-term", line, oldLine, "Condition " + bt(condB) + " gained " + bt(op + term));
                else add_fn(p, "condition-gained-term", line, oldLine, bt(sa_text) + " gained " + bt(op + term) + in);
                return true;
            }
        }
        if ((a.kind == ExprKind::And || a.kind == ExprKind::Or) && a.kids.size() == 2) {
            const std::string sb_text = expr_b(p, b);
            for (size_t k = 0; k < 2; ++k) {
                if (!same_text(p, *a.kids[k], b)) continue;
                const std::string term = expr_a(p, *a.kids[1 - k]);
                const std::string op = a.kind == ExprKind::And ? "&& " : "|| ";
                if (isCond) add_fn(p, "condition-lost-term", line, oldLine, "Condition " + bt(condB) + " lost " + bt(op + term));
                else add_fn(p, "condition-lost-term", line, oldLine, bt(sb_text) + " lost " + bt(op + term) + in);
                return true;
            }
        }
        // Negated, or no longer.
        if (b.kind == ExprKind::Unary && b.op == "!" && !b.kids.empty() && same_text(p, a, *b.kids[0])) {
            add_fn(p, "condition-negated", line, oldLine, bt(expr_a(p, a)) + " is now negated" + (isCond ? ": " + bt(tb) : in));
            return true;
        }
        if (a.kind == ExprKind::Unary && a.op == "!" && !a.kids.empty() && same_text(p, *a.kids[0], b)) {
            add_fn(p, "condition-negated", line, oldLine, bt(expr_b(p, b)) + " is no longer negated" + (isCond ? ": " + bt(tb) : in));
            return true;
        }
        if (a.kind == ExprKind::Call && b.kind == ExprKind::Call) {
            if (!eq_ci(m_a.str(a.name), m_b.str(b.name)) || a.call != b.call || (a.call == CallKind::Static && !eq_ci(m_a.str(a.cls), m_b.str(b.cls)))) {
                add_fn(p, "callee-changed", line, oldLine, bt(callee_text(m_b, b)) + " is called instead of " + bt(callee_text(m_a, a)) + in);
                return true;
            }
            const size_t na = a.kids.size() - (a.call == CallKind::Method ? 1 : 0);
            const size_t nb = b.kids.size() - (b.call == CallKind::Method ? 1 : 0);
            if (na != nb) {
                add_fn(p, nb > na ? "argument-added" : "argument-removed", line, oldLine,
                       bt(callee_text(m_b, b)) + " is called with " + plural(nb, "argument", "arguments") + ", was " + std::to_string(na) + in);
                return true;
            }
        }
        if (isReceiver) {
            add_fn(p, "receiver-changed", line, oldLine, bt(callee_text(m_b, *callB)) + " is now called on " + bt(expr_b(p, b)) + " instead of " + bt(expr_a(p, a)) + in);
            return true;
        }
        if (isArgument) {
            add_fn(p, "argument-changed", line, oldLine, bt(expr_b(p, b)) + " is passed to " + bt(callee_text(m_b, *callB)) + " instead of " + bt(expr_a(p, a)) + in);
            return true;
        }
        if (a.kind == ExprKind::Binary && b.kind == ExprKind::Binary && a.op != b.op && a.kids.size() == 2 && b.kids.size() == 2 &&
            same_text(p, *a.kids[0], *b.kids[0]) && same_text(p, *a.kids[1], *b.kids[1])) {
            add_fn(p, "operator-changed", line, oldLine, bt(a.op) + " became " + bt(b.op) + in);
            return true;
        }
        if (a.kind == ExprKind::Var && b.kind == ExprKind::Var) {
            add_fn(p, "operand-changed", line, oldLine, bt(expr_b(p, b)) + " is used instead of " + bt(expr_a(p, a)) + in);
            return true;
        }
        if (a.kind == ExprKind::PropGet && b.kind == ExprKind::PropGet && !eq_ci(m_a.str(a.name), m_b.str(b.name))) {
            add_fn(p, "operand-changed", line, oldLine, bt(expr_b(p, b)) + " is read instead of " + bt(expr_a(p, a)) + in);
            return true;
        }
        // A sub-expression replaced inside a larger statement: name the piece
        // when the statement itself is long.
        if (spot.parentA && tb.size() > 60) {
            add_fn(p, isCond ? "condition-changed" : "expression-changed", line, oldLine, bt(expr_a(p, a)) + " became " + bt(expr_b(p, b)) + in);
            return true;
        }
        (void)ta;
        return false;
    }

    const Script& m_a;
    const Script& m_b;
    const DiffOptions& m_opt;
    std::vector<std::pair<FnKey, FnRef>> m_fa, m_fb;
    std::vector<Pair> m_pairs;
    std::vector<Change> m_out;
    // Script-variable rename votes and the accepted renames.
    std::map<std::string, std::map<std::string, size_t>> m_varVotes;
    std::map<std::string, std::pair<StrIdx, StrIdx>> m_varVoteIdx;
    std::map<StrIdx, StrIdx> m_slotRenames;
    std::map<std::string, std::string> m_varRenames;   // old lowered -> new spelling
    std::set<std::string> m_varRenamedTo;              // new lowered
    std::map<std::string, std::string> m_propRenames;  // old lowered -> new spelling
    std::set<std::string> m_propRenamedTo;
    std::set<std::string> m_wholeStates;               // lowercased states only one side has
};

}  // namespace

DiffResult diff(const Script& oldScript, const Script& newScript, const DiffOptions& opt) {
    return Differ(oldScript, newScript, opt).run();
}

std::string diff_text(const DiffResult& r) {
    std::string out;
    std::string current;  // "object|state|function" of the open block
    bool first = true;
    for (const Change& c : r.changes) {
        const std::string block = c.function.empty() ? std::string() : c.object + "|" + c.state + "|" + c.function;
        if (block != current || first) {
            if (!c.function.empty()) {
                const size_t dot = c.function.find('.');
                std::string head;
                if (dot != std::string::npos) head = "Property " + c.function.substr(0, dot) + (c.function.substr(dot) == ".get" ? " getter" : " setter");
                else head = (detail::is_event_name(detail::FnContext::lower(c.function)) ? "Event " : "Function ") + c.function;
                if (!c.state.empty()) head += " (state " + c.state + ")";
                out += head + ":\n";
            }
            current = block;
            first = false;
        }
        if (!c.function.empty()) {
            out += "    ";
            if (c.line) out += "line " + std::to_string(c.line) + ": ";
            else if (c.oldLine) out += "old line " + std::to_string(c.oldLine) + ": ";
        }
        out += c.message + "\n";
    }
    return out;
}

}  // namespace papyrus::pex
