// The script auditor (include/papyrus/audit.hpp): rules over the decompiler's
// intermediate representation. Each function is lifted and structured exactly
// as the decompiler does it (lift_function, structure_function); a function
// the structurer cannot explain is skipped, never guessed at. docs/AUDIT.md
// explains every rule, its grounding and its false-positive rate on vanilla.
// SPDX-License-Identifier: GPL-3.0-only
#include "papyrus/audit.hpp"

#include "decompile/event_names.hpp"
#include "decompile/ir.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace papyrus::pex {

const char* to_string(Severity s) noexcept {
    switch (s) {
    case Severity::Info: return "info";
    case Severity::Warning: return "warning";
    case Severity::Error: return "error";
    }
    return "?";
}

namespace {

using namespace detail;

// --- names ------------------------------------------------------------------

std::string lower(std::string_view v) { return FnContext::lower(v); }

bool eq_ci(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

bool ends_with(std::string_view v, std::string_view suffix) {
    return v.size() >= suffix.size() && v.substr(v.size() - suffix.size()) == suffix;
}

// Rule ids (stable; documented in docs/AUDIT.md in this order).
const char* const kLoopNoExit = "loop-no-exit";
const char* const kLoopPolls = "loop-polls-without-wait";
const char* const kLoopPollsUnbounded = "loop-polls-unbounded";
const char* const kLatentInInit = "latent-call-in-init-event";
const char* const kCellEventLinkedRef = "cell-event-calls-linked-ref";
const char* const kInventoryUnfiltered = "inventory-event-unfiltered";
const char* const kEventCallsQuest = "event-calls-quest-per-actor";
const char* const kUpdateNeverUnregistered = "update-never-unregistered";
const char* const kOnUpdateRearms = "onupdate-reregisters-unconditionally";
const char* const kUpdateIntervalShort = "update-interval-too-short";
const char* const kWaitInOnUpdate = "wait-in-onupdate";
const char* const kNoneDeref = "none-deref-unchecked";
const char* const kRecursionUnconditional = "recursion-unconditional";
const char* const kRecursion = "recursion";
const char* const kArrayOffByOne = "array-index-off-by-one";
const char* const kGotoStateUnknown = "gotostate-unknown";
const char* const kArrayNone = "array-none";

// Two groups, in the order the summary prints them: first what leaves threads
// in the save or starves the VM (what the save-game corpus shows), then what
// gives wrong results and fills the log.
const std::vector<AuditRule> kRules = {
    {kLoopNoExit, Severity::Error, "threads", "a While whose condition nothing in the body can change, with no Return out of it"},
    {kLoopPolls, Severity::Warning, "threads", "a While that waits for state another thread must change, with no call (no Wait) in its body"},
    {kLoopPollsUnbounded, Severity::Warning, "threads", "a While that waits with Utility.Wait for state outside the function, with no attempt limit (info on single-instance script types)"},
    {kUpdateNeverUnregistered, Severity::Warning, "threads", "RegisterForUpdate / RegisterForUpdateGameTime with no matching Unregister anywhere in the script"},
    {kOnUpdateRearms, Severity::Info, "threads", "OnUpdate re-registers itself outside any If, in a script that never leaves the state or unregisters"},
    {kUpdateIntervalShort, Severity::Warning, "threads", "RegisterForUpdate / RegisterForSingleUpdate with a literal interval under 0.1 s"},
    {kWaitInOnUpdate, Severity::Info, "threads", "Utility.Wait (or WaitMenuMode / WaitGameTime) inside OnUpdate"},
    {kLatentInInit, Severity::Info, "threads", "a latent call (Wait, SetStage, MoveTo, Enable, ...) in OnInit / OnLoad / OnCellAttach of a script type attached to many objects"},
    {kCellEventLinkedRef, Severity::Info, "threads", "OnCellAttach / OnLoad calling into a linked reference: every reference in the cell calls the same object at once"},
    {kInventoryUnfiltered, Severity::Warning, "threads", "OnItemAdded / OnItemRemoved that does work, in a script that never calls AddInventoryEventFilter: one thread per item moved"},
    {kEventCallsQuest, Severity::Info, "threads", "a per-item or per-actor event (OnItemAdded, OnDeath, OnHit, ...) calling into a quest script: every instance queues on the quest's lock"},
    {kRecursionUnconditional, Severity::Error, "threads", "a function that calls itself outside any If or While"},
    {kRecursion, Severity::Info, "threads", "a function that calls itself"},
    {kNoneDeref, Severity::Warning, "results", "a call or property access on the result of a function that returns None in ordinary situations, with no check before it"},
    {kArrayNone, Severity::Error, "results", "an array indexed before anything creates it"},
    {kArrayOffByOne, Severity::Error, "results", "a loop that indexes an array up to and including its Length"},
    {kGotoStateUnknown, Severity::Error, "results", "GotoState to a state neither the script nor its ancestors declare"},
};

// Natives that park the thread until the game thread answers (latent), by
// lowercased name. Kept by hand; docs/AUDIT.md lists it.
const char* const kLatentMethods[] = {
    "setstage", "setcurrentstageid", "setobjectivedisplayed", "setobjectivecompleted", "setobjectivefailed", "start", "stop", "reset",
    "moveto", "movetomyeditorlocation", "movetonode", "movetointeractionlocation", "enable", "disable", "delete", "placeatme",
    "placeactoratme", "activate", "playanimationandwait", "playgamebryoanimation", "waitforanimationevent", "translateto",
    "translatetoref", "splinetranslateto", "splinetranslatetoref", "splinetranslatetorefnode", "pushactoraway", "setposition", "setangle",
    "removeallitems", "resurrect", "kill", "startcombat", "evaluatepackage", "sendassaultalarm", "setscale", "setmotiontype",
};
bool is_latent_method(std::string_view lowerName) {
    for (const char* n : kLatentMethods) {
        if (lowerName == n) return true;
    }
    return false;
}
bool is_wait_call(const Expr& e, std::string_view cls, std::string_view name) {
    return eq_ci(cls, "utility") && (eq_ci(name, "wait") || eq_ci(name, "waitmenumode") || eq_ci(name, "waitgametime")) && e.kind == ExprKind::Call;
}

Severity severity_of(const char* rule) {
    for (const AuditRule& r : kRules) {
        if (r.id == rule) return r.severity;
    }
    return Severity::Warning;
}

// Functions that return None in ordinary situations (not only on error),
// lowercased, with the reason the advice names. Static ones carry the class.
struct NoneProne {
    const char* callee;
    const char* reason;
};
// Not in the list on purpose: the alias getters (GetReference, GetActorRef),
// GetLinkedRef and GetParentCell. Vanilla dereferences them unchecked in over
// 500 scripts because the quest or the Creation Kit guarantees the fill; a
// rule on them would be noise (docs/AUDIT.md).
const NoneProne kNoneProne[] = {
    {"game.getformfromfile", "the plugin is not loaded"},
    {"getcurrentlocation", "the reference is nowhere in particular"},
    {"getworldspace", "the reference is in an interior"},
    {"getcombattarget", "the actor is not in combat"},
    {"getequippedweapon", "nothing is equipped in that hand"},
    {"getequippedshield", "no shield is equipped"},
    {"getequippedobject", "nothing is equipped in that slot"},
    {"getequippedarmorinslot", "nothing is worn in that slot"},
    {"getfurniturereference", "the actor is not using furniture"},
    {"getcurrentscene", "no scene is running"},
    {"getcurrentpackage", "the actor has no package"},
    {"findclosestreferenceoftype", "nothing of that type is within the radius"},
    {"findclosestreferenceoftypefromref", "nothing of that type is within the radius"},
    {"findclosestreferenceofanytypeinlist", "nothing of those types is within the radius"},
    {"findclosestreferenceofanytypeinlistfromref", "nothing of those types is within the radius"},
    {"findclosestactor", "no actor is within the radius"},
    {"findclosestactorfromref", "no actor is within the radius"},
    {"findrandomreferenceoftype", "nothing of that type is within the radius"},
    {"findrandomactor", "no actor is within the radius"},
};
// Also left out: GetAt / GetNthForm / GetNthRef, which return None only for an
// index past the end (an index bug, not a None-check matter); a rule on them
// flagged 1,200 ordinary FormList loops in the mod corpus.

// --- IR walkers -------------------------------------------------------------

template <class F>
void each_expr(const Expr& e, F&& f) {
    f(e);
    for (const ExprPtr& k : e.kids) each_expr(*k, f);
}

// Every statement, nested ones included, with its nesting depth (0 = the
// function body itself).
template <class F>
void each_stmt(const std::vector<StmtPtr>& ss, int depth, F&& f) {
    for (const StmtPtr& st : ss) {
        f(*st, depth);
        each_stmt(st->body, depth + 1, f);
        each_stmt(st->elseBody, depth + 1, f);
    }
}

// Every expression node of a statement list, nested statements included.
template <class F>
void each_expr_in(const std::vector<StmtPtr>& ss, F&& f) {
    each_stmt(ss, 0, [&](const Stmt& st, int) {
        if (st.lhs) each_expr(*st.lhs, f);
        if (st.rhs) each_expr(*st.rhs, f);
    });
}

const Expr& strip_casts(const Expr& e) {
    const Expr* p = &e;
    while (p->kind == ExprKind::Cast && !p->kids.empty()) p = p->kids[0].get();
    return *p;
}

bool contains_call(const std::vector<StmtPtr>& ss) {
    bool found = false;
    each_expr_in(ss, [&](const Expr& e) { if (e.kind == ExprKind::Call) found = true; });
    return found;
}

bool contains_return(const std::vector<StmtPtr>& ss) {
    bool found = false;
    each_stmt(ss, 0, [&](const Stmt& st, int) { if (st.kind == StmtKind::Return) found = true; });
    return found;
}

void vars_in(const Expr& e, std::set<StrIdx>& out) {
    each_expr(e, [&](const Expr& x) { if (x.kind == ExprKind::Var) out.insert(x.name); });
}

// Slots a statement list writes: Assign to a Var, and the array behind an
// element store (its contents change).
void assigned_in(const std::vector<StmtPtr>& ss, std::set<StrIdx>& out) {
    each_stmt(ss, 0, [&](const Stmt& st, int) {
        if (st.kind != StmtKind::Assign || !st.lhs) return;
        if (st.lhs->kind == ExprKind::Var) out.insert(st.lhs->name);
        else if (st.lhs->kind == ExprKind::ArrayGet && !st.lhs->kids.empty() && st.lhs->kids[0]->kind == ExprKind::Var)
            out.insert(st.lhs->kids[0]->name);
    });
}

// --- the per-function view ----------------------------------------------------

struct FnView {
    const Object* o = nullptr;
    const State* st = nullptr;      // null for accessors
    const Function* f = nullptr;
    std::string name;               // "OnUpdate", "Prop.get"
    std::string lowerName;          // the function name alone, lowercased ("onupdate", "prop")
    std::string stateName;          // "" for the default state and for accessors
    StrIdx nameIdx = 0, stateIdx = 0;
    FunctionKind kind = FunctionKind::Method;
    std::vector<StmtPtr> stmts;     // structured body
    const FunctionDebug* dbg = nullptr;
};

struct Ctx {
    const Script& s;
    const AuditOptions& opt;
    std::vector<FnView> fns;
    std::vector<Finding> out;

    // Script-wide facts.
    std::set<std::string> selfCalls;        // lowercased names of method calls on self, any function
    std::set<std::string> anyCalls;         // lowercased names of every method call
    std::set<StrIdx> assignedVars;          // Var slots assigned anywhere
    std::set<std::string> states;           // lowercased, own and inherited
    std::set<std::string> stateFlags;       // lowercased literals compared with GetState()
    std::set<StrIdx> linkedSlots;           // variables / autovars assigned from GetLinkedRef anywhere in the script
    std::set<std::string> latentFunctions;  // lowercased names of this script's functions that make a latent call (transitively)
    bool statesComplete = true;             // false when an ancestor could not be resolved

    Ctx(const Script& script, const AuditOptions& o) : s(script), opt(o) {}

    std::string_view str(StrIdx i) const { return s.str(i); }

    uint16_t line_of(const FnView& v, size_t ip) const {
        if (!v.dbg || ip >= v.dbg->lines.size()) return 0;
        return v.dbg->lines[ip];
    }

    void report(const FnView& v, size_t ip, const char* rule, std::string message, Severity* override = nullptr) {
        Finding f;
        f.object = std::string(str(v.o->name));
        f.state = v.st ? std::string(str(v.st->name)) : std::string();
        f.function = v.name;
        f.ip = static_cast<uint32_t>(ip);
        f.line = line_of(v, ip);
        f.severity = override ? *override : severity_of(rule);
        f.rule = rule;
        f.message = std::move(message);
        out.push_back(std::move(f));
    }
};

// True for script types the game attaches to many objects at once
// (ObjectReference, Actor, ActiveMagicEffect and everything derived from them),
// where one thread per instance becomes hundreds of threads in a cell. Quest,
// alias and form scripts have one instance each. Decided from the parent
// chain through the resolver; without one, from the direct parent's name.
bool many_instance_type(const FnContext& fc) {
    auto is_many = [](const std::string& t) {
        return t == "objectreference" || t == "actor" || t == "activemagiceffect";
    };
    if (is_many(fc.objectType)) return true;
    const std::string parent = lower(fc.s.str(fc.o.parent));
    if (is_many(parent)) return true;
    for (const std::string& a : fc.ancestors_of(fc.objectType)) {
        if (is_many(a)) return true;
    }
    return false;
}

bool is_init_event(const FnView& v) {
    return v.lowerName == "oninit" || v.lowerName == "onload" || v.lowerName == "oncellattach" || v.lowerName == "oncellload" ||
           v.lowerName == "onattachedtocell";
}
bool is_cell_event(const FnView& v) {
    return v.lowerName == "onload" || v.lowerName == "oncellattach" || v.lowerName == "oncellload" || v.lowerName == "onattachedtocell";
}

// The debug record of a function. Only the first function of a state block
// carries the state label (docs/COMPILER_PATTERNS.md), so fall back to any
// record with the same object, name and kind whose length fits.
const FunctionDebug* debug_record(const Script& s, StrIdx object, StrIdx state, StrIdx name, FunctionKind kind, const Function& f) {
    if (!s.debug.present) return nullptr;
    const FunctionDebug* exact = s.debug_for(object, state, name, kind);
    if (exact && exact->lines.size() == f.code.size()) return exact;
    for (const FunctionDebug& d : s.debug.functions) {
        if (d.object == object && d.kind == kind && d.lines.size() == f.code.size() && eq_ci(s.str(d.name), s.str(name))) return &d;
    }
    return nullptr;
}

StrIdx empty_string_index(const Script& s) {
    for (size_t i = 0; i < s.strings.size(); ++i) {
        if (s.strings[i].empty()) return static_cast<StrIdx>(i);
    }
    return 0;
}

// Call shape tests. All names compared case-insensitively.
bool is_method_named(const Expr& e, std::string_view name, const Ctx& c) {
    return e.kind == ExprKind::Call && e.call != CallKind::Static && eq_ci(c.str(e.name), name);
}
// `self`, possibly cast to its own type or an ancestor (`(self as Quest)`
// still dispatches to this script). A cast to any other type names another
// script attached to the same form, so the call is not a self-call.
bool is_self_var(const Expr& e, const FnContext& fc) {
    const Expr* p = &e;
    while (p->kind == ExprKind::Cast && !p->kids.empty()) {
        const std::string target = lower(fc.s.str(p->name));
        if (target != fc.objectType && !fc.is_subtype(fc.objectType, target)) return false;
        p = p->kids[0].get();
    }
    return p->kind == ExprKind::Var && fc.is_self(p->name);
}
bool is_self_call_named(const Expr& e, std::string_view name, const Ctx& c, const FnContext& fc) {
    return is_method_named(e, name, c) && e.call == CallKind::Method && !e.kids.empty() && is_self_var(*e.kids[0], fc);
}
bool is_static_call(const Expr& e, std::string_view cls, std::string_view name, const Ctx& c) {
    return e.kind == ExprKind::Call && e.call == CallKind::Static && eq_ci(c.str(e.cls), cls) && eq_ci(c.str(e.name), name);
}

// "Game.GetFormFromFile" for statics, "GetLinkedRef" for methods: the key of kNoneProne.
std::string callee_key(const Expr& e, const Ctx& c) {
    if (e.kind != ExprKind::Call) return "";
    if (e.call == CallKind::Static) return lower(c.str(e.cls)) + "." + lower(c.str(e.name));
    return lower(c.str(e.name));
}
const NoneProne* none_prone(const Expr& e, const Ctx& c) {
    const std::string key = callee_key(e, c);
    if (key.empty()) return nullptr;
    for (const NoneProne& n : kNoneProne) {
        if (key == n.callee) return &n;
    }
    return nullptr;
}

// Source-like spelling of a call for messages: "Game.GetFormFromFile(...)".
std::string call_spelling(const Expr& e, const Ctx& c) {
    if (e.kind != ExprKind::Call) return "the call";
    if (e.call == CallKind::Static) return std::string(c.str(e.cls)) + "." + std::string(c.str(e.name)) + "()";
    return std::string(c.str(e.name)) + "()";
}

bool literal_true(const Expr& e) {
    if (e.kind != ExprKind::Lit) return false;
    if (e.lit.type == ValueType::Bool) return e.lit.as_bool();
    if (e.lit.type == ValueType::Integer) return e.lit.as_int() != 0;
    return false;
}

bool is_local_or_param(const FnContext& fc, StrIdx name) {
    const std::string_view n = fc.s.str(name);
    for (const Param& p : fc.f.params) {
        if (p.name == name || eq_ci(fc.s.str(p.name), n)) return true;
    }
    for (const Param& p : fc.f.locals) {
        if (p.name == name || eq_ci(fc.s.str(p.name), n)) return true;
    }
    return false;
}

std::string fmt_interval(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

// --- rules ------------------------------------------------------------------

// loop-no-exit, loop-polls-without-wait, array-index-off-by-one (the loop form).
void rule_loops(Ctx& c, const FnView& v, const FnContext& fc) {
    each_stmt(v.stmts, 0, [&](const Stmt& st, int) {
        if (st.kind != StmtKind::While || !st.rhs) return;
        const Expr& cond = *st.rhs;
        std::set<StrIdx> assigned;
        assigned_in(st.body, assigned);
        const bool returns = contains_return(st.body);

        if (literal_true(cond)) {
            // `While True` with a call inside is a deliberate forever loop
            // (vanilla polls that way); without one it is a spin.
            if (!returns && !contains_call(st.body))
                c.report(v, st.ip, kLoopNoExit, "This loop runs forever: its condition is always true, nothing returns out of it, and it never yields.");
            return;
        }
        bool external = false;
        std::set<StrIdx> condVars;
        each_expr(cond, [&](const Expr& e) {
            if (e.kind == ExprKind::Call || e.kind == ExprKind::PropGet) external = true;
            if (e.kind == ExprKind::Var) {
                condVars.insert(e.name);
                if (!is_local_or_param(fc, e.name)) external = true;  // script variable, autovar, self
            }
        });
        bool anyAssigned = false;
        for (StrIdx n : condVars) {
            if (assigned.count(n)) anyAssigned = true;
        }
        if (anyAssigned || returns) {
            // A counted loop. The off-by-one form: While i <= a.Length ... a[i].
            const Expr* idx = nullptr;
            const Expr* arr = nullptr;
            auto arrayLenOf = [&](const Expr& e) -> const Expr* {
                const Expr& b = strip_casts(e);
                if (b.kind == ExprKind::ArrayLen && !b.kids.empty() && strip_casts(*b.kids[0]).kind == ExprKind::Var) return &strip_casts(*b.kids[0]);
                // a.Length + 1 (either order)
                if (b.kind == ExprKind::Binary && b.op == "+" && b.kids.size() == 2) {
                    for (int side = 0; side < 2; ++side) {
                        const Expr& l = strip_casts(*b.kids[side]);
                        const Expr& r = strip_casts(*b.kids[1 - side]);
                        if (l.kind == ExprKind::ArrayLen && !l.kids.empty() && strip_casts(*l.kids[0]).kind == ExprKind::Var &&
                            r.kind == ExprKind::Lit && r.lit.type == ValueType::Integer && r.lit.as_int() == 1)
                            return &strip_casts(*l.kids[0]);
                    }
                }
                return nullptr;
            };
            if (cond.kind == ExprKind::Binary && cond.kids.size() == 2) {
                const Expr& l = strip_casts(*cond.kids[0]);
                const Expr& r = strip_casts(*cond.kids[1]);
                if (cond.op == "<=" && l.kind == ExprKind::Var) { idx = &l; arr = arrayLenOf(r); if (arr && strip_casts(r).kind != ExprKind::ArrayLen) arr = nullptr; }
                else if (cond.op == ">=" && r.kind == ExprKind::Var) { idx = &r; arr = arrayLenOf(l); if (arr && strip_casts(l).kind != ExprKind::ArrayLen) arr = nullptr; }
                else if (cond.op == "<" && l.kind == ExprKind::Var) { idx = &l; arr = arrayLenOf(r); if (arr && strip_casts(r).kind == ExprKind::ArrayLen) arr = nullptr; }
                else if (cond.op == ">" && r.kind == ExprKind::Var) { idx = &r; arr = arrayLenOf(l); if (arr && strip_casts(l).kind == ExprKind::ArrayLen) arr = nullptr; }
            }
            if (idx && arr) {
                bool reported = false;
                each_expr_in(st.body, [&](const Expr& e) {
                    if (reported || e.kind != ExprKind::ArrayGet || e.kids.size() != 2) return;
                    const Expr& a = strip_casts(*e.kids[0]);
                    const Expr& i = strip_casts(*e.kids[1]);
                    if (a.kind == ExprKind::Var && i.kind == ExprKind::Var && a.name == arr->name && i.name == idx->name) {
                        reported = true;
                        c.report(v, e.ip ? e.ip : st.ip, kArrayOffByOne,
                                 fc.display_name(a.name) + "[" + fc.display_name(i.name) + "] runs past the end: indices go from 0 to Length - 1, so the loop should stop at < Length.");
                    }
                });
            }
            return;
        }
        if (!external) {
            c.report(v, st.ip, kLoopNoExit, "This loop can never end: nothing in its body changes what the condition reads, and nothing returns out of it.");
        } else if (!contains_call(st.body)) {
            c.report(v, st.ip, kLoopPolls, "This loop waits for something outside the function by spinning: its body makes no call, so it never yields; put Utility.Wait in the body or react to an event instead.");
        } else {
            // A poll with Utility.Wait: fine while whatever it waits for
            // arrives; unbounded when no local or parameter takes part in the
            // condition (nothing counts the attempts), so a state that never
            // comes leaves one thread per instance in the save forever
            // (CWFieldCOCrimeFactionScript.OnInit: 21,517 threads in 788 saves
            // of the corpus).
            bool hasWait = false;
            each_expr_in(st.body, [&](const Expr& e) {
                if (e.kind == ExprKind::Call && e.call == CallKind::Static && is_wait_call(e, c.str(e.cls), c.str(e.name))) hasWait = true;
            });
            bool anyLocalInCond = false;
            for (StrIdx n : condVars) {
                if (is_local_or_param(fc, n)) anyLocalInCond = true;
            }
            if (!hasWait || anyLocalInCond) return;
            // A counter the body increments but nothing reads: the author
            // meant a limit and never applied it.
            std::string counter;
            for (StrIdx a : assigned) {
                if (!is_local_or_param(fc, a)) continue;
                // Reads of the slot (an assignment's own target is not one);
                // `x += 1` reads x once per write, anything beyond that is real.
                size_t reads = 0, writes = 0;
                each_stmt(v.stmts, 0, [&](const Stmt& s2, int) {
                    const bool plainTarget = s2.kind == StmtKind::Assign && s2.lhs && s2.lhs->kind == ExprKind::Var;
                    if (plainTarget && s2.lhs->name == a) ++writes;
                    if (s2.lhs && !plainTarget) each_expr(*s2.lhs, [&](const Expr& e) { if (e.kind == ExprKind::Var && e.name == a) ++reads; });
                    if (s2.rhs) each_expr(*s2.rhs, [&](const Expr& e) { if (e.kind == ExprKind::Var && e.name == a) ++reads; });
                });
                if (reads <= writes) counter = fc.display_name(a);
            }
            // Who owns what the loop waits for? A flag of this script, or a
            // call on self, is the author's own off switch (vanilla's
            // `While isActive && !breakLoop` update loops): info. Only
            // another object's state (`CWs.Init`, `myQuest.GetStage()`) with
            // no own flag beside it is a wait on something the script does
            // not control: warning on many-instance types.
            bool ownState = false, otherState = false;
            each_expr(cond, [&](const Expr& e) {
                if (e.kind == ExprKind::Var && !is_local_or_param(fc, e.name) && !fc.is_self(e.name)) ownState = true;
                if ((e.kind == ExprKind::PropGet || (e.kind == ExprKind::Call && e.call == CallKind::Method)) && !e.kids.empty()) {
                    if (is_self_var(*e.kids[0], fc)) ownState = true;
                    else otherState = true;
                }
                if (e.kind == ExprKind::Call && e.call == CallKind::Static) otherState = true;
            });
            const bool many = many_instance_type(fc);
            Severity sev = (many && otherState && !ownState) ? Severity::Warning : Severity::Info;
            std::string msg = "This loop waits with Utility.Wait for something outside the function and nothing limits the attempts";
            if (!counter.empty()) msg += " (" + counter + " counts them but is never used to stop)";
            if (sev == Severity::Warning)
                msg += "; what it waits for belongs to another object, and one thread per instance stays in the save for as long as that never comes; add a limit or react to an event.";
            else
                msg += "; if the state never comes the thread stays in the save; add a limit or react to an event.";
            c.report(v, st.ip, kLoopPollsUnbounded, msg, &sev);
        }
    });
}

// Events the game raises once per item, per actor or per hit: a burst of
// threads during a bulk transfer, a battle, a cell load.
bool is_burst_event(const FnView& v) {
    static const char* const names[] = {"onitemadded", "onitemremoved", "oncontainerchanged", "ondeath", "ondying", "onhit",
                                        "oncellattach", "oncellload", "onload", "onattachedtocell", "onmagiceffectapply"};
    for (const char* n : names) {
        if (v.lowerName == n) return true;
    }
    return false;
}

// True when the lowercased type is a quest script (Quest or derived).
bool is_quest_type(const FnContext& fc, const std::string& type) {
    if (type.empty()) return false;
    if (type == "quest") return true;
    for (const std::string& a : fc.ancestors_of(type)) {
        if (a == "quest") return true;
    }
    return false;
}

// inventory-event-unfiltered, event-calls-quest-per-actor.
void rule_burst_events(Ctx& c, const FnView& v, const FnContext& fc) {
    const bool inventory = v.lowerName == "onitemadded" || v.lowerName == "onitemremoved";
    const bool alias = fc.is_subtype(fc.objectType, "referencealias") || lower(fc.s.str(fc.o.parent)) == "referencealias";
    if (inventory && !c.anyCalls.count("addinventoryeventfilter") && !c.anyCalls.count("addallinventoryeventfilters") && contains_call(v.stmts)) {
        // On the player's alias every item the player ever touches raises it:
        // warning. On a container or an actor the burst is bounded by that
        // inventory: info.
        Severity sev = alias ? Severity::Warning : Severity::Info;
        c.report(v, v.stmts.empty() ? 0 : v.stmts.front()->ip, kInventoryUnfiltered,
                 v.name + " runs once for every item type that moves, and this script never narrows it with AddInventoryEventFilter; a bulk transfer starts one thread per item, all waiting on whatever this handler calls. Filter to the forms you care about, or do the work from one place.",
                 &sev);
    }
    if (!is_burst_event(v)) return;
    const bool manyThreads = many_instance_type(fc) || inventory || alias;
    if (!manyThreads) return;
    bool done = false;
    each_expr_in(v.stmts, [&](const Expr& e) {
        if (done || e.kind != ExprKind::Call || e.call != CallKind::Method || e.kids.empty()) return;
        const Expr& obj = strip_casts(*e.kids[0]);
        if (is_self_var(obj, fc)) return;
        std::string type = obj.type;
        if (type.empty() && obj.kind == ExprKind::Var) type = fc.type_of(obj.name);
        if (!is_quest_type(fc, type)) return;
        done = true;
        c.report(v, e.ip, kEventCallsQuest,
                 v.name + " calls " + std::string(c.str(e.name)) + " on a quest script; every instance that gets this event queues on that quest's lock, and a burst of them (a bulk transfer, a battle, a cell load) parks them all. Keep the handler to a filter and a flag, and let the quest do the work once.");
    });
}

// latent-call-in-init-event, cell-event-calls-linked-ref.
void rule_init_events(Ctx& c, const FnView& v, const FnContext& fc) {
    if (!is_init_event(v) || !many_instance_type(fc)) return;
    bool latentDone = false;
    std::set<StrIdx> linked = c.linkedSlots;  // slots holding a GetLinkedRef result (this function or anywhere in the script)
    bool linkedDone = false;
    each_stmt(v.stmts, 0, [&](const Stmt& st, int) {
        if (st.kind == StmtKind::Assign && st.lhs && st.lhs->kind == ExprKind::Var && st.rhs) {
            const Expr& r = strip_casts(*st.rhs);
            if (r.kind == ExprKind::Call && (eq_ci(c.str(r.name), "getlinkedref") || eq_ci(c.str(r.name), "getnthlinkedref"))) linked.insert(st.lhs->name);
            else linked.erase(st.lhs->name);
        }
        auto visit = [&](const Expr& root) {
            each_expr(root, [&](const Expr& e) {
                if (e.kind != ExprKind::Call) return;
                const std::string name = lower(c.str(e.name));
                if (!latentDone) {
                    const bool latent = (e.call == CallKind::Static && is_wait_call(e, c.str(e.cls), c.str(e.name))) ||
                                        (e.call != CallKind::Static && is_latent_method(name));
                    // A call to one of this script's own functions that makes a
                    // latent call somewhere inside (OnCellAttach -> TriggerSetup -> Wait).
                    const bool viaOwn = e.call == CallKind::Method && !e.kids.empty() && is_self_var(*e.kids[0], fc) && c.latentFunctions.count(name);
                    if (latent || viaOwn) {
                        latentDone = true;
                        c.report(v, e.ip, kLatentInInit,
                                 v.name + " runs once per object this script is attached to, and " + std::string(c.str(e.name)) +
                                     (viaOwn ? " ends in a call that parks each thread until the game thread answers" : " parks each thread until the game thread answers") +
                                     "; when a cell attaches, every instance queues up at once. Keep the event short and do the latent work from one place.");
                    }
                }
                if (!linkedDone && is_cell_event(v) && e.call == CallKind::Method && !e.kids.empty()) {
                    const Expr& b = strip_casts(*e.kids[0]);
                    const bool viaLinked = (b.kind == ExprKind::Var && linked.count(b.name)) ||
                                           (b.kind == ExprKind::Call && (eq_ci(c.str(b.name), "getlinkedref") || eq_ci(c.str(b.name), "getnthlinkedref")));
                    if (viaLinked) {
                        linkedDone = true;
                        c.report(v, e.ip, kCellEventLinkedRef,
                                 v.name + " calls " + std::string(c.str(e.name)) + " on a linked reference: every reference in the cell that carries this script calls into the same object at the same moment; let the linked object do the work once, or stagger it.");
                    }
                }
            });
        };
        if (st.lhs) visit(*st.lhs);
        if (st.rhs) visit(*st.rhs);
    });
}

// array-index-off-by-one, the direct form a[a.Length].
void rule_array_length_index(Ctx& c, const FnView& v, const FnContext& fc) {
    each_expr_in(v.stmts, [&](const Expr& e) {
        if (e.kind != ExprKind::ArrayGet || e.kids.size() != 2) return;
        const Expr& a = strip_casts(*e.kids[0]);
        const Expr& i = strip_casts(*e.kids[1]);
        if (a.kind == ExprKind::Var && i.kind == ExprKind::ArrayLen && !i.kids.empty() && strip_casts(*i.kids[0]).kind == ExprKind::Var &&
            strip_casts(*i.kids[0]).name == a.name) {
            c.report(v, e.ip, kArrayOffByOne, fc.display_name(a.name) + "[" + fc.display_name(a.name) + ".Length] is past the end: the last element is at Length - 1.");
        }
    });
}

// update-never-unregistered (one finding per script per pair).
void rule_update_pairs(Ctx& c) {
    struct Pair { const char* reg; const char* unreg; const char* single; };
    const Pair pairs[] = {{"registerforupdate", "unregisterforupdate", "RegisterForSingleUpdate"},
                          {"registerforupdategametime", "unregisterforupdategametime", "RegisterForSingleUpdateGameTime"}};
    for (const Pair& p : pairs) {
        if (!c.selfCalls.count(p.reg) || c.anyCalls.count(p.unreg)) continue;
        // Anchor: the first registering call.
        for (const FnView& v : c.fns) {
            FnContext fc(c.s, *v.o, *v.f, c.opt.resolver);
            bool done = false;
            each_expr_in(v.stmts, [&](const Expr& e) {
                if (done || !is_self_call_named(e, p.reg, c, fc)) return;
                done = true;
                const std::string reg(c.str(e.name));
                const std::string un = std::string("Unregister") + reg.substr(8);
                c.report(v, e.ip, kUpdateNeverUnregistered,
                         reg + " repeats until " + un + " is called, and this script never calls it; add " + un + " where the work ends, or use " + p.single + ".");
            });
            if (done) break;
        }
    }
}

bool is_on_update(const FnView& v) { return v.lowerName == "onupdate" || v.lowerName == "onupdategametime"; }

// onupdate-reregisters-unconditionally, wait-in-onupdate.
void rule_on_update(Ctx& c, const FnView& v, const FnContext& fc) {
    if (!is_on_update(v) || v.f->is_native()) return;
    const bool hasExit = c.anyCalls.count("gotostate") || c.anyCalls.count("unregisterforupdate") || c.anyCalls.count("unregisterforupdategametime");
    if (!hasExit) {
        for (const StmtPtr& st : v.stmts) {
            if (st->kind != StmtKind::Expr || !st->rhs) continue;
            const Expr& e = *st->rhs;
            static const char* const regs[] = {"registerforsingleupdate", "registerforupdate", "registerforsingleupdategametime", "registerforupdategametime"};
            for (const char* r : regs) {
                if (is_self_call_named(e, r, c, fc)) {
                    c.report(v, e.ip, kOnUpdateRearms,
                             v.name + " re-arms itself with " + std::string(c.str(e.name)) + " on every run and nothing in the script stops it; guard the registration or leave the state when the work is done.");
                    break;
                }
            }
        }
    }
    bool waited = false;
    each_expr_in(v.stmts, [&](const Expr& e) {
        if (waited) return;
        if (is_static_call(e, "utility", "wait", c) || is_static_call(e, "utility", "waitmenumode", c) || is_static_call(e, "utility", "waitgametime", c)) {
            waited = true;
            c.report(v, e.ip, kWaitInOnUpdate,
                     "Utility." + std::string(c.str(e.name)) + " inside " + v.name + " holds this thread while the next update may already be queued; do the waiting in a state of its own or shorten the work.");
        }
    });
}

// update-interval-too-short. A single update with a tiny interval is the
// idiom for "run OnUpdate as soon as possible, once" (vanilla critters); it
// only matters when OnUpdate itself re-arms it, so RegisterForSingleUpdate
// counts inside OnUpdate only, RegisterForUpdate anywhere.
void rule_update_interval(Ctx& c, const FnView& v) {
    each_expr_in(v.stmts, [&](const Expr& e) {
        if (!(is_method_named(e, "registerforupdate", c) || (is_on_update(v) && is_method_named(e, "registerforsingleupdate", c)))) return;
        const size_t argAt = e.call == CallKind::Method ? 1 : 0;
        if (e.kids.size() <= argAt) return;
        const Expr& a = strip_casts(*e.kids[argAt]);
        if (a.kind != ExprKind::Lit) return;
        double val = 0;
        if (a.lit.type == ValueType::Float) val = a.lit.as_float();
        else if (a.lit.type == ValueType::Integer) val = a.lit.as_int();
        else return;
        if (val >= 0.1) return;
        c.report(v, e.ip, kUpdateIntervalShort,
                 std::string(c.str(e.name)) + "(" + fmt_interval(val) + ") runs this handler every " + fmt_interval(val) +
                     " s and competes with every other script for the VM's 1.2 ms per frame; use a longer interval or react to an event.");
    });
}

// none-deref-unchecked: an ordered walk that tracks which locals hold a
// None-prone result and which have since been tested in a condition.
struct NoneState {
    std::map<StrIdx, const Expr*> nullable;  // local -> the call that produced it
    std::set<StrIdx> checked;
    std::set<std::string> pluginsChecked;    // lowercased plugin names the function asked Game about
};

// The plugin literal of a Game.GetFormFromFile call, lowercased ("" if none).
std::string plugin_of(const Expr& call, const Ctx& c) {
    if (callee_key(call, c) != "game.getformfromfile" || call.kids.size() < 2) return "";
    const Expr& a = strip_casts(*call.kids[1]);
    if (a.kind != ExprKind::Lit || a.lit.type != ValueType::String) return "";
    return lower(c.str(a.lit.str()));
}

void none_deref_in_expr(Ctx& c, const FnView& v, const FnContext& fc, NoneState& ns, const Expr& e, bool& stop);

void none_check_base(Ctx& c, const FnView& v, const FnContext& fc, NoneState& ns, const Expr& base, const Expr& user, const char* what) {
    const Expr& b = strip_casts(base);
    if (b.kind == ExprKind::Var) {
        auto it = ns.nullable.find(b.name);
        if (it == ns.nullable.end() || ns.checked.count(b.name)) return;
        const NoneProne* np = none_prone(*it->second, c);
        const std::string reason = np ? np->reason : "it has nothing to return";
        c.report(v, user.ip ? user.ip : it->second->ip, kNoneDeref,
                 fc.display_name(b.name) + " comes from " + call_spelling(*it->second, c) + ", which returns None when " + reason +
                     "; test it before " + what + " on it.");
        ns.nullable.erase(it);  // one finding per value
    } else if (const NoneProne* np = none_prone(b, c)) {
        if (ns.pluginsChecked.count(plugin_of(b, c))) return;
        c.report(v, user.ip, kNoneDeref,
                 call_spelling(b, c) + " returns None when " + np->reason + "; keep the result in a variable and test it before " + what + " on it.");
    }
}

void none_deref_in_expr(Ctx& c, const FnView& v, const FnContext& fc, NoneState& ns, const Expr& e, bool& stop) {
    if (stop) return;
    // Short circuits: the left operand is evaluated first and guards the right.
    if (e.kind == ExprKind::And || e.kind == ExprKind::Or) {
        if (e.kids.size() == 2) {
            none_deref_in_expr(c, v, fc, ns, *e.kids[0], stop);
            std::set<StrIdx> guard;
            vars_in(*e.kids[0], guard);
            for (StrIdx g : guard) ns.checked.insert(g);
            none_deref_in_expr(c, v, fc, ns, *e.kids[1], stop);
        }
        return;
    }
    // Operands first (inner calls happen before the outer one).
    for (const ExprPtr& k : e.kids) none_deref_in_expr(c, v, fc, ns, *k, stop);
    if (e.kind == ExprKind::Call) {
        // A GetFormFromFile result handed to another call unchecked: the
        // callee gets None ("Cannot add None to a container" in the logs).
        // Only for that callee; passing a possibly-None reference is otherwise
        // ordinary (ForceRefTo(None) is how an alias is cleared).
        const size_t firstArg = e.call == CallKind::Method ? 1 : 0;
        for (size_t k = firstArg; k < e.kids.size(); ++k) {
            const Expr& a = strip_casts(*e.kids[k]);
            const Expr* producer = nullptr;
            if (a.kind == ExprKind::Var) {
                auto it = ns.nullable.find(a.name);
                if (it != ns.nullable.end() && !ns.checked.count(a.name)) producer = it->second;
            } else if (none_prone(a, c)) {
                producer = &a;
            }
            if (producer && callee_key(*producer, c) == "game.getformfromfile")
                none_check_base(c, v, fc, ns, a, e, ("passing it to " + std::string(c.str(e.name)) + "()").c_str());
        }
    }
    if (e.kind == ExprKind::Call && e.call == CallKind::Method && !e.kids.empty())
        none_check_base(c, v, fc, ns, *e.kids[0], e, ("calling " + std::string(c.str(e.name)) + "()").c_str());
    else if (e.kind == ExprKind::PropGet && !e.kids.empty())
        none_check_base(c, v, fc, ns, *e.kids[0], e, ("reading " + std::string(c.str(e.name))).c_str());
    else if ((e.kind == ExprKind::ArrayGet || e.kind == ExprKind::ArrayLen || e.kind == ExprKind::ArrayFind) && !e.kids.empty())
        none_check_base(c, v, fc, ns, *e.kids[0], e, "indexing it");
}

void none_deref_walk(Ctx& c, const FnView& v, const FnContext& fc, NoneState& ns, const std::vector<StmtPtr>& ss) {
    bool stop = false;
    for (const StmtPtr& sp : ss) {
        const Stmt& st = *sp;
        switch (st.kind) {
        case StmtKind::If:
        case StmtKind::While: {
            if (st.rhs) {
                none_deref_in_expr(c, v, fc, ns, *st.rhs, stop);
                std::set<StrIdx> tested;
                vars_in(*st.rhs, tested);
                for (StrIdx t : tested) ns.checked.insert(t);
            }
            none_deref_walk(c, v, fc, ns, st.body);
            none_deref_walk(c, v, fc, ns, st.elseBody);
            break;
        }
        case StmtKind::Assign: {
            if (st.rhs) none_deref_in_expr(c, v, fc, ns, *st.rhs, stop);
            if (!st.lhs) break;
            if (st.lhs->kind == ExprKind::Var) {
                const Expr& r = st.rhs ? strip_casts(*st.rhs) : *st.lhs;
                if (st.rhs && none_prone(r, c) && !ns.pluginsChecked.count(plugin_of(r, c))) {
                    ns.nullable[st.lhs->name] = &r;
                    ns.checked.erase(st.lhs->name);
                } else {
                    ns.nullable.erase(st.lhs->name);
                }
            } else if (!st.lhs->kids.empty()) {
                // x.Prop = v / x[i] = v: a store through a possibly None base.
                for (size_t k = 1; k < st.lhs->kids.size(); ++k) none_deref_in_expr(c, v, fc, ns, *st.lhs->kids[k], stop);
                none_check_base(c, v, fc, ns, *st.lhs->kids[0], *st.lhs,
                                st.lhs->kind == ExprKind::PropGet ? ("setting " + std::string(c.str(st.lhs->name))).c_str() : "storing into it");
            }
            break;
        }
        default:
            if (st.rhs) none_deref_in_expr(c, v, fc, ns, *st.rhs, stop);
            break;
        }
    }
}

void rule_none_deref(Ctx& c, const FnView& v, const FnContext& fc) {
    NoneState ns;
    // A function that asks whether the plugin is loaded (Game.GetModByName,
    // SKSE's Game.IsPluginInstalled) has checked every form it then takes
    // from that plugin, whatever variable carries the answer.
    each_expr_in(v.stmts, [&](const Expr& e) {
        if (!(is_static_call(e, "game", "getmodbyname", c) || is_static_call(e, "game", "isplugininstalled", c)) || e.kids.empty()) return;
        const Expr& a = strip_casts(*e.kids[0]);
        if (a.kind == ExprKind::Lit && a.lit.type == ValueType::String) ns.pluginsChecked.insert(lower(c.str(a.lit.str())));
    });
    none_deref_walk(c, v, fc, ns, v.stmts);
}

// recursion-unconditional, recursion. A function the script also defines in
// another state is dispatched by state: `GotoState("Waiting"); OnActivate(x)`
// runs the Waiting handler, not this one again.
void rule_recursion(Ctx& c, const FnView& v, const FnContext& fc) {
    if (v.kind != FunctionKind::Method) return;
    int definitions = 0;
    for (const State& st : v.o->states) {
        for (const NamedFunction& nf : st.functions) {
            if (eq_ci(c.str(nf.name), v.name)) ++definitions;
        }
    }
    if (definitions > 1) return;
    bool done = false;
    bool guarded = false;  // a Return before the call (an early-exit guard) makes it conditional
    each_stmt(v.stmts, 0, [&](const Stmt& st, int depth) {
        if (done) return;
        if (st.kind == StmtKind::Return) guarded = true;
        auto visit = [&](const Expr& root) {
            each_expr(root, [&](const Expr& e) {
                if (done || e.kind != ExprKind::Call) return;
                bool self = false;
                if (e.call == CallKind::Method && !e.kids.empty()) self = is_self_var(*e.kids[0], fc) && eq_ci(c.str(e.name), v.name);
                else if (e.call == CallKind::Static) self = eq_ci(c.str(e.cls), c.str(v.o->name)) && eq_ci(c.str(e.name), v.name);
                if (!self) return;
                done = true;
                if (depth == 0 && !guarded)
                    c.report(v, e.ip, kRecursionUnconditional, v.name + " calls itself on every run, so nothing stops it before the VM's stack limit; put the call under a condition that eventually fails.");
                else
                    c.report(v, e.ip, kRecursion, v.name + " calls itself; make sure an argument gets closer to the stopping case every time, since every level stays in the save until it returns.");
            });
        };
        if (st.lhs) visit(*st.lhs);
        if (st.rhs) visit(*st.rhs);
    });
}

// gotostate-unknown.
void rule_gotostate(Ctx& c, const FnView& v, const FnContext& fc) {
    if (!c.statesComplete) return;
    auto check = [&](const Expr& at, std::string_view name) {
        if (name.empty()) return;  // "" is the default state
        const std::string key = lower(name);
        if (c.states.count(key)) return;
        // GotoState("None") is a common spelling of the empty state. It is not
        // one (GetState() answers "None"), but an undeclared state runs the
        // empty state's handlers, which is what the author meant.
        if (key == "none") return;
        // A state name the script only ever compares GetState() with is a flag,
        // not a mistake: the handlers do not change, and the author knows it.
        if (c.stateFlags.count(key)) return;
        c.report(v, at.ip, kGotoStateUnknown,
                 "No state named \"" + std::string(name) + "\" is declared in this script, so its handlers keep running as in the empty state; declare the state or fix the spelling.");
    };
    each_expr_in(v.stmts, [&](const Expr& e) {
        if (is_self_call_named(e, "gotostate", c, fc) && e.kids.size() >= 2) {
            const Expr& a = strip_casts(*e.kids[1]);
            if (a.kind == ExprKind::Lit && a.lit.type == ValueType::String) check(e, c.str(a.lit.str()));
        }
    });
}

// array-none: a local array read before its first assignment; a script
// variable of array type that no function ever assigns.
void rule_array_none(Ctx& c, const FnView& v, const FnContext& fc) {
    std::set<StrIdx> assignedLocals;
    std::set<StrIdx> reported;
    auto is_array_local = [&](StrIdx n) {
        if (!is_local_or_param(fc, n)) return false;
        for (const Param& p : fc.f.params) {
            if (p.name == n) return false;  // parameters arrive initialised by the caller
        }
        return ends_with(fc.type_of(n), "[]");
    };
    auto is_never_created_var = [&](StrIdx n) {
        if (is_local_or_param(fc, n) || fc.is_self(n)) return false;
        if (fc.property_of_autovar(n)) return false;  // the plugin fills properties
        const std::string_view name = c.str(n);
        for (const Variable& var : v.o->variables) {
            if (var.name == n || eq_ci(c.str(var.name), name)) return ends_with(lower(c.str(var.type)), "[]") && !c.assignedVars.count(var.name);
        }
        return false;
    };
    auto use = [&](const Expr& e) {
        if (!(e.kind == ExprKind::ArrayGet || e.kind == ExprKind::ArrayLen || e.kind == ExprKind::ArrayFind) || e.kids.empty()) return;
        const Expr& a = strip_casts(*e.kids[0]);
        if (a.kind != ExprKind::Var || reported.count(a.name)) return;
        if (is_array_local(a.name) && !assignedLocals.count(a.name)) {
            reported.insert(a.name);
            c.report(v, e.ip, kArrayNone, fc.display_name(a.name) + " is used before anything creates it, and a local array starts as None; assign new Type[n] or a call result first.");
        } else if (is_never_created_var(a.name)) {
            reported.insert(a.name);
            c.report(v, e.ip, kArrayNone, fc.display_name(a.name) + " is never assigned anywhere in this script, so it is always None here; create it (new Type[n]) in OnInit or make it a property.");
        }
    };
    std::function<void(const std::vector<StmtPtr>&)> walk = [&](const std::vector<StmtPtr>& ss) {
        for (const StmtPtr& sp : ss) {
            const Stmt& st = *sp;
            if (st.rhs) each_expr(*st.rhs, use);
            if (st.lhs) {
                if (st.lhs->kind == ExprKind::Var) assignedLocals.insert(st.lhs->name);
                else each_expr(*st.lhs, use);
            }
            walk(st.body);
            walk(st.elseBody);
        }
    };
    walk(v.stmts);
}

// Does function `name` of another script type end in a latent call? Scans the
// callee's instructions (no lifting), following its own self calls `depth`
// levels down. Conservative: an unresolvable type or function is not latent.
bool foreign_function_is_latent(const FnContext& fc, const std::string& type, const std::string& lowerName, int depth) {
    if (depth <= 0) return false;
    const FnContext::FoundFunction found = fc.find_function(type, lowerName);
    if (!found.script || !found.function) return false;
    const Script& fs = *found.script;
    for (const Instruction& ins : found.function->code) {
        if (ins.op == Opcode::CallStatic && ins.args.size() >= 2 && ins.args[0].is_string_ref() && ins.args[1].is_string_ref()) {
            if (eq_ci(fs.str(ins.args[0].str()), "utility")) {
                const std::string_view n = fs.str(ins.args[1].str());
                if (eq_ci(n, "wait") || eq_ci(n, "waitmenumode") || eq_ci(n, "waitgametime")) return true;
            }
        } else if ((ins.op == Opcode::CallMethod || ins.op == Opcode::CallParent) && !ins.args.empty() && ins.args[0].is_string_ref()) {
            const std::string n = lower(fs.str(ins.args[0].str()));
            if (is_latent_method(n)) return true;
            // A self call inside the callee: one more level down the same type.
            if (ins.op == Opcode::CallMethod && ins.args.size() >= 2 && ins.args[1].type == ValueType::Identifier && eq_ci(fs.str(ins.args[1].str()), "self") &&
                foreign_function_is_latent(fc, type, n, depth - 1))
                return true;
        }
    }
    return false;
}

// --- driver -------------------------------------------------------------------

void collect_states(Ctx& c, const Object& o) {
    for (const State& st : o.states) c.states.insert(lower(c.str(st.name)));
    std::string cur = lower(c.str(o.parent));
    std::set<std::string> seen;
    for (int guard = 0; guard < 32 && !cur.empty(); ++guard) {
        if (!seen.insert(cur).second) break;
        const Script* ps = c.opt.resolver ? c.opt.resolver->find(cur) : nullptr;
        if (!ps || ps->objects.empty()) {
            c.statesComplete = false;
            return;
        }
        for (const State& st : ps->objects[0].states) c.states.insert(lower(ps->str(st.name)));
        cur = lower(ps->str(ps->objects[0].parent));
    }
}

}  // namespace

const std::vector<AuditRule>& audit_rules() { return kRules; }

AuditResult audit(const Script& s, const AuditOptions& opt) {
    AuditResult result;
    Ctx c(s, opt);
    const StrIdx empty = empty_string_index(s);

    // 1. Structure every body the way the decompiler does.
    for (const Object& o : s.objects) {
        auto add = [&](const State* st, const Function& f, StrIdx nameIdx, const std::string& name, FunctionKind kind) {
            if (f.is_native()) return;
            ++result.functions;
            FnView v;
            v.o = &o;
            v.st = st;
            v.f = &f;
            v.name = name;
            v.nameIdx = nameIdx;
            v.stateIdx = st ? st->name : empty;
            v.stateName = st ? std::string(s.str(st->name)) : std::string();
            v.kind = kind;
            const size_t dot = name.find('.');
            v.lowerName = lower(dot == std::string::npos ? name : name.substr(0, dot));
            v.dbg = debug_record(s, o.name, v.stateIdx, nameIdx, kind, f);
            FnContext fc(s, o, f, opt.resolver);
            try {
                FunctionIR ir = lift_function(fc);
                v.stmts = structure_function(fc, ir);
            } catch (const DecompileError&) {
                ++result.skipped;
                return;
            } catch (...) {
                ++result.skipped;
                return;
            }
            c.fns.push_back(std::move(v));
        };
        for (const Property& p : o.properties) {
            if (p.getter) add(nullptr, *p.getter, p.name, std::string(s.str(p.name)) + ".get", FunctionKind::Getter);
            if (p.setter) add(nullptr, *p.setter, p.name, std::string(s.str(p.name)) + ".set", FunctionKind::Setter);
        }
        for (const State& st : o.states) {
            for (const NamedFunction& nf : st.functions) add(&st, nf.body, nf.name, std::string(s.str(nf.name)), FunctionKind::Method);
        }
        collect_states(c, o);
    }

    // 2. Script-wide facts.
    for (const FnView& v : c.fns) {
        FnContext fc(s, *v.o, *v.f, opt.resolver);
        each_expr_in(v.stmts, [&](const Expr& e) {
            if (e.kind == ExprKind::Call && e.call != CallKind::Static) {
                const std::string n = lower(c.str(e.name));
                c.anyCalls.insert(n);
                if (e.call == CallKind::Method && !e.kids.empty() && is_self_var(*e.kids[0], fc)) c.selfCalls.insert(n);
            }
            if (e.kind == ExprKind::Binary && (e.op == "==" || e.op == "!=") && e.kids.size() == 2) {
                for (int side = 0; side < 2; ++side) {
                    const Expr& l = strip_casts(*e.kids[side]);
                    const Expr& r = strip_casts(*e.kids[1 - side]);
                    if (is_self_call_named(l, "getstate", c, fc) && r.kind == ExprKind::Lit && r.lit.type == ValueType::String)
                        c.stateFlags.insert(lower(c.str(r.lit.str())));
                }
            }
        });
        each_stmt(v.stmts, 0, [&](const Stmt& st, int) {
            if (st.kind == StmtKind::Assign && st.lhs && st.lhs->kind == ExprKind::Var) {
                c.assignedVars.insert(st.lhs->name);
                if (st.rhs && !is_local_or_param(fc, st.lhs->name)) {
                    const Expr& r = strip_casts(*st.rhs);
                    if (r.kind == ExprKind::Call && (eq_ci(c.str(r.name), "getlinkedref") || eq_ci(c.str(r.name), "getnthlinkedref"))) c.linkedSlots.insert(st.lhs->name);
                }
            }
        });
    }
    // Which of the script's own functions end in a latent call, directly,
    // through other own functions, or through a call into another script's
    // function (WeaponRackTriggerSCRIPT.OnCellAttach -> TriggerSetup ->
    // ActivatorScript.ActivatorSetup -> Utility.Wait): a fixpoint over self
    // calls, with other scripts' functions scanned at the instruction level
    // through the resolver, two levels deep.
    {
        std::map<std::string, std::set<std::string>> ownCalls;  // function -> own functions it calls
        for (const FnView& v : c.fns) {
            FnContext fc(s, *v.o, *v.f, opt.resolver);
            const std::string me = lower(v.name);
            each_expr_in(v.stmts, [&](const Expr& e) {
                if (e.kind != ExprKind::Call) return;
                const std::string n = lower(c.str(e.name));
                if (e.call == CallKind::Static && is_wait_call(e, c.str(e.cls), c.str(e.name))) c.latentFunctions.insert(me);
                else if (e.call != CallKind::Static && is_latent_method(n)) c.latentFunctions.insert(me);
                else if (e.call == CallKind::Method && !e.kids.empty() && is_self_var(*e.kids[0], fc)) ownCalls[me].insert(n);
                else if (e.call == CallKind::Method && !e.kids.empty() && opt.resolver && !c.latentFunctions.count(me)) {
                    const Expr& obj = strip_casts(*e.kids[0]);
                    std::string type = obj.type;
                    if (type.empty() && obj.kind == ExprKind::Var) type = fc.type_of(obj.name);
                    if (!type.empty() && foreign_function_is_latent(fc, type, n, 2)) c.latentFunctions.insert(me);
                }
            });
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (const auto& [fn, callees] : ownCalls) {
                if (c.latentFunctions.count(fn)) continue;
                for (const std::string& callee : callees) {
                    if (c.latentFunctions.count(callee)) {
                        c.latentFunctions.insert(fn);
                        changed = true;
                        break;
                    }
                }
            }
        }
    }

    // 3. Rules.
    for (const FnView& v : c.fns) {
        FnContext fc(s, *v.o, *v.f, opt.resolver);
        rule_loops(c, v, fc);
        rule_init_events(c, v, fc);
        rule_burst_events(c, v, fc);
        rule_array_length_index(c, v, fc);
        rule_on_update(c, v, fc);
        rule_update_interval(c, v);
        rule_none_deref(c, v, fc);
        rule_recursion(c, v, fc);
        rule_gotostate(c, v, fc);
        rule_array_none(c, v, fc);
    }
    rule_update_pairs(c);

    // Function order, then instruction order (the per-script rule last).
    std::stable_sort(c.out.begin(), c.out.end(), [&](const Finding& a, const Finding& b) {
        if (a.function != b.function) {
            size_t ia = 0, ib = 0;
            for (size_t i = 0; i < c.fns.size(); ++i) {
                if (c.fns[i].name == a.function && c.fns[i].stateName == a.state) ia = i;
                if (c.fns[i].name == b.function && c.fns[i].stateName == b.state) ib = i;
            }
            return ia < ib;
        }
        return a.ip < b.ip;
    });
    result.findings = std::move(c.out);
    return result;
}

}  // namespace papyrus::pex
