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
const char* const kConditionImpossible = "condition-impossible";
const char* const kConditionAlwaysTrue = "condition-always-true";
const char* const kCodeUnreachable = "code-unreachable";
const char* const kPropertyUnused = "property-unused";
const char* const kPropertyUnfilled = "property-unfilled";
const char* const kStateUnreachable = "state-unreachable";

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
    {kConditionImpossible, Severity::Warning, "results", "an If, ElseIf or While whose condition can never be true (x == 1 && x == 2, x > 5 && x < 3, an ElseIf an earlier test already covers)"},
    {kConditionAlwaysTrue, Severity::Warning, "results", "an If or While whose condition is always true (x != 1 || x != 2, x == x)"},
    {kCodeUnreachable, Severity::Warning, "results", "statements after a Return (or after a While True), or an Else the tests before it leave no value for"},
    {kPropertyUnfilled, Severity::Warning, "results", "an object or array property the script reads that no plugin attaching the script fills and nothing assigns (needs --fills)"},
    {kPropertyUnused, Severity::Info, "results", "a property no script of the audited set reads or writes"},
    {kStateUnreachable, Severity::Info, "results", "a state no GotoState in the audited set enters, that is not the auto state"},
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

// --- conditions that can never be true, or are always true -----------------------

// A condition is read as a formula over atoms "slot op literal". Only what can
// be decided from the condition's own text is decided: an atom on anything but
// a plain variable or an array's Length is unknown, and unknown atoms never
// make a condition impossible or always true. Literal comparisons follow
// Papyrus: strings compare without regard to case, None is a value of its own.
struct Atom {
    std::string key;     // "v:<slot>" or "len:<slot>"
    std::string op;      // == != < <= > >=
    Value val;
    bool numeric = false;
    double num = 0;      // when numeric
    bool isInt = false;  // an Int slot against an Int literal: strict bounds tighten by one
};

struct Formula {
    enum class K { Atom, And, Or, Unknown, Const } k = K::Unknown;
    Atom atom;
    bool constant = false;
    bool selfCompare = false;  // Const from comparing a slot with itself
    std::vector<Formula> parts;
};

Formula unknown_f() { return Formula{}; }
Formula const_f(bool b) {
    Formula f;
    f.k = Formula::K::Const;
    f.constant = b;
    return f;
}

std::string negate_op(const std::string& op) {
    if (op == "==") return "!=";
    if (op == "!=") return "==";
    if (op == "<") return ">=";
    if (op == ">=") return "<";
    if (op == ">") return "<=";
    return ">";  // "<="
}
std::string flip_op(const std::string& op) {  // a op b  ==  b flip(op) a
    if (op == "<") return ">";
    if (op == ">") return "<";
    if (op == "<=") return ">=";
    if (op == ">=") return "<=";
    return op;
}

Formula negate(const Formula& f) {
    Formula r = f;
    switch (f.k) {
    case Formula::K::Atom: r.atom.op = negate_op(f.atom.op); break;
    case Formula::K::Const: r.constant = !f.constant; break;
    case Formula::K::And:
    case Formula::K::Or:
        r.k = f.k == Formula::K::And ? Formula::K::Or : Formula::K::And;
        for (Formula& p : r.parts) p = negate(p);
        break;
    case Formula::K::Unknown: break;
    }
    return r;
}

// Through the conversions the compiler inserted (Int to Float, a value to
// Bool for a test). A cast the author wrote, (item as Weapon), changes the
// value and stays.
const Expr& strip_implicit(const Expr& e) {
    const Expr* p = &e;
    while (p->kind == ExprKind::Cast && p->implicitCast && !p->explicitCast && !p->kids.empty()) p = p->kids[0].get();
    return *p;
}

// A literal operand: a literal or a negated number, through casts.
bool literal_value(const Expr& e, Value& out, double& num, bool& numeric) {
    const Expr& x = strip_casts(e);
    if (x.kind == ExprKind::Lit) {
        out = x.lit;
        numeric = x.lit.type == ValueType::Integer || x.lit.type == ValueType::Float;
        num = x.lit.type == ValueType::Integer ? double(x.lit.as_int()) : x.lit.type == ValueType::Float ? double(x.lit.as_float()) : 0;
        return true;
    }
    if (x.kind == ExprKind::Unary && x.op == "-" && !x.kids.empty()) {
        if (!literal_value(*x.kids[0], out, num, numeric) || !numeric) return false;
        num = -num;
        if (out.type == ValueType::Integer) out.bits = static_cast<uint32_t>(-out.as_int());
        return true;
    }
    return false;
}

// The slot a comparison side reads, when it is one the rule may reason about.
bool slot_key(const Expr& e, const FnContext& fc, bool callsInCondition, std::string& key, bool& isInt) {
    const Expr& x = strip_implicit(e);
    if (x.kind == ExprKind::Var && !fc.is_self(x.name) && !fc.is_nonevar(x.name)) {
        // A script variable another thread can change while a call in the same
        // condition has released the script's lock.
        if (callsInCondition && !is_local_or_param(fc, x.name)) return false;
        key = "v:" + std::to_string(x.name);
        isInt = fc.type_of(x.name) == "int";
        return true;
    }
    if (x.kind == ExprKind::ArrayLen && !x.kids.empty()) {
        const Expr& a = strip_implicit(*x.kids[0]);
        if (a.kind != ExprKind::Var) return false;
        if (callsInCondition && !is_local_or_param(fc, a.name)) return false;
        key = "len:" + std::to_string(a.name);
        isInt = true;
        return true;
    }
    return false;
}

Formula to_formula(const Expr& e0, const FnContext& fc, bool calls) {
    const Expr& e = strip_implicit(e0);
    if (e.kind == ExprKind::Lit) {
        if (e.lit.type == ValueType::Bool) return const_f(e.lit.as_bool());
        if (e.lit.type == ValueType::Integer) return const_f(e.lit.as_int() != 0);
        return unknown_f();
    }
    if (e.kind == ExprKind::And || e.kind == ExprKind::Or) {
        Formula f;
        f.k = e.kind == ExprKind::And ? Formula::K::And : Formula::K::Or;
        for (const ExprPtr& k : e.kids) f.parts.push_back(to_formula(*k, fc, calls));
        return f;
    }
    if (e.kind == ExprKind::Unary && e.op == "!" && !e.kids.empty()) return negate(to_formula(*e.kids[0], fc, calls));
    // A bare Bool variable: `If x` is `x == True`.
    if (e.kind == ExprKind::Var) {
        std::string key;
        bool isInt = false;
        if (fc.type_of(e.name) == "bool" && slot_key(e, fc, calls, key, isInt)) {
            Formula f;
            f.k = Formula::K::Atom;
            f.atom.key = key;
            f.atom.op = "==";
            f.atom.val.type = ValueType::Bool;
            f.atom.val.bits = 1;
            return f;
        }
        return unknown_f();
    }
    if (e.kind != ExprKind::Binary || e.kids.size() != 2) return unknown_f();
    static const std::set<std::string> kCmp = {"==", "!=", "<", "<=", ">", ">="};
    if (!kCmp.count(e.op)) return unknown_f();
    // The same slot on both sides: x == x is always true, x != x never.
    {
        std::string ka, kb;
        bool ia = false, ib = false;
        if (slot_key(*e.kids[0], fc, calls, ka, ia) && slot_key(*e.kids[1], fc, calls, kb, ib) && ka == kb) {
            Formula f = const_f(e.op == "==" || e.op == "<=" || e.op == ">=");
            f.selfCompare = true;
            return f;
        }
    }
    for (int side = 0; side < 2; ++side) {
        std::string key;
        bool isInt = false;
        Value v;
        double num = 0;
        bool numeric = false;
        if (!slot_key(*e.kids[side], fc, calls, key, isInt)) continue;
        if (!literal_value(*e.kids[1 - side], v, num, numeric)) continue;
        Formula f;
        f.k = Formula::K::Atom;
        f.atom.key = key;
        f.atom.op = side == 0 ? e.op : flip_op(e.op);
        f.atom.val = v;
        f.atom.numeric = numeric;
        f.atom.num = num;
        f.atom.isInt = isInt && v.type == ValueType::Integer;
        // Ordering against something that is not a number is left alone.
        if (!numeric && f.atom.op != "==" && f.atom.op != "!=") return unknown_f();
        // A Bool slot compared with 0 / 1.
        const Expr& slot = strip_implicit(*e.kids[side]);
        if (slot.kind == ExprKind::Var && fc.type_of(slot.name) == "bool" && numeric && (f.atom.op == "==" || f.atom.op == "!=")) {
            f.atom.val.type = ValueType::Bool;
            f.atom.val.bits = num != 0 ? 1 : 0;
            f.atom.numeric = false;
        }
        return f;
    }
    return unknown_f();
}

bool same_literal(const Value& a, const Value& b, const Script& s) {
    const bool an = a.type == ValueType::Integer || a.type == ValueType::Float;
    const bool bn = b.type == ValueType::Integer || b.type == ValueType::Float;
    if (an && bn) {
        const double x = a.type == ValueType::Integer ? a.as_int() : a.as_float();
        const double y = b.type == ValueType::Integer ? b.as_int() : b.as_float();
        return x == y;
    }
    if (a.type != b.type) return false;
    if (a.type == ValueType::String) return eq_ci(s.str(a.str()), s.str(b.str()));
    if (a.type == ValueType::Bool) return a.as_bool() == b.as_bool();
    if (a.type == ValueType::None) return true;
    return false;
}

// Two literals that are certainly different values (same kind, not equal).
bool surely_different(const Atom& a, const Atom& b, const Script& s) {
    const bool comparable = a.val.type == b.val.type || (a.numeric && b.numeric);
    if (!comparable || a.val.type == ValueType::Identifier) return false;
    return !same_literal(a.val, b.val, s);
}

// Can every atom about one slot hold at once?
bool atoms_satisfiable(const std::vector<const Atom*>& as, const Script& s) {
    std::vector<const Atom*> eqs, nes;
    double lo = -1e300, hi = 1e300;
    bool loOpen = false, hiOpen = false, anyBound = false, allInt = true;
    const bool isLen = !as.empty() && as[0]->key.compare(0, 4, "len:") == 0;
    if (isLen) {
        lo = 0;
        anyBound = true;
    }
    for (const Atom* a : as) {
        if (a->op == "==") {
            eqs.push_back(a);
            continue;
        }
        if (a->op == "!=") {
            nes.push_back(a);
            continue;
        }
        anyBound = true;
        if (!a->isInt) allInt = false;
        double v = a->num;
        bool open = a->op == ">" || a->op == "<";
        if (a->isInt && open) {
            v += (a->op == ">") ? 1 : -1;
            open = false;
        }
        if (a->op == ">" || a->op == ">=") {
            if (v > lo || (v == lo && open)) {
                lo = v;
                loOpen = open;
            }
        } else if (v < hi || (v == hi && open)) {
            hi = v;
            hiOpen = open;
        }
    }
    if (anyBound && (lo > hi || (lo == hi && (loOpen || hiOpen)))) return false;
    for (size_t i = 1; i < eqs.size(); ++i) {
        if (surely_different(*eqs[0], *eqs[i], s)) return false;
    }
    if (!eqs.empty()) {
        const Atom* p = eqs[0];
        for (const Atom* n : nes) {
            if (same_literal(p->val, n->val, s)) return false;
        }
        if (p->numeric && anyBound) {
            const double x = p->num;
            if (x < lo || (x == lo && loOpen) || x > hi || (x == hi && hiOpen)) return false;
        }
    } else if (anyBound && allInt && !loOpen && !hiOpen && lo == hi) {
        for (const Atom* n : nes) {
            if (n->numeric && n->num == lo) return false;  // the one integer left is excluded
        }
    }
    // A Bool slot that must be neither True nor False.
    if (eqs.empty()) {
        bool t = false, f = false;
        for (const Atom* n : nes) {
            if (n->val.type == ValueType::Bool) (n->val.as_bool() ? t : f) = true;
        }
        if (t && f) return false;
    }
    return true;
}

// Is a conjunction of formulas unsatisfiable? Ors are split case by case
// (bounded); unknown parts are dropped, since they could be anything.
bool unsat(std::vector<const Formula*> conj, const Script& s, int budget = 64) {
    std::vector<const Formula*> flat;
    for (size_t i = 0; i < conj.size(); ++i) {
        const Formula* f = conj[i];
        if (f->k == Formula::K::And) {
            for (const Formula& p : f->parts) conj.push_back(&p);
        } else {
            flat.push_back(f);
        }
    }
    std::map<std::string, std::vector<const Atom*>> bySlot;
    const Formula* firstOr = nullptr;
    for (const Formula* f : flat) {
        if (f->k == Formula::K::Const && !f->constant) return true;
        if (f->k == Formula::K::Atom) bySlot[f->atom.key].push_back(&f->atom);
        if (f->k == Formula::K::Or && !firstOr) firstOr = f;
    }
    for (const auto& [k, as] : bySlot) {
        if (!atoms_satisfiable(as, s)) return true;
    }
    if (!firstOr || budget <= 0) return false;
    for (const Formula& d : firstOr->parts) {  // unsat only when every disjunct is
        std::vector<const Formula*> next;
        for (const Formula* f : flat) {
            if (f != firstOr) next.push_back(f);
        }
        next.push_back(&d);
        if (!unsat(next, s, budget / int(firstOr->parts.size() + 1) - 1)) return false;
    }
    return true;
}

bool decidable(const Formula& f) {
    if (f.k == Formula::K::Atom || (f.k == Formula::K::Const && f.selfCompare)) return true;
    for (const Formula& p : f.parts) {
        if (decidable(p)) return true;
    }
    return false;
}
bool self_compared(const Formula& f) {
    if (f.selfCompare) return true;
    for (const Formula& p : f.parts) {
        if (self_compared(p)) return true;
    }
    return false;
}

bool calls_in(const Expr& e) {
    bool found = false;
    each_expr(e, [&](const Expr& x) { if (x.kind == ExprKind::Call || x.kind == ExprKind::PropGet) found = true; });
    return found;
}

// condition-impossible / condition-always-true, with what an If/ElseIf chain
// has already ruled out carried into each later branch.
void rule_conditions(Ctx& c, const FnView& v, const FnContext& fc) {
    auto check = [&](const Stmt& st, const std::vector<Formula>* ctx) {
        if (!st.rhs) return;
        const Formula f = to_formula(*st.rhs, fc, calls_in(*st.rhs));
        const Expr& cond = strip_casts(*st.rhs);
        const bool isWhile = st.kind == StmtKind::While;
        const std::string what = isWhile ? "This While" : "This If";
        if (cond.kind == ExprKind::Lit) {
            if (isWhile && f.k == Formula::K::Const && f.constant) return;  // While True: the loop rules judge it
            Severity info = Severity::Info;
            if (f.k == Formula::K::Const && !f.constant)
                c.report(v, st.ip, kConditionImpossible,
                         what + " tests a literal False, so its block never runs; if it is switched off on purpose, a comment saying so helps the next reader", &info);
            else if (f.k == Formula::K::Const)
                c.report(v, st.ip, kConditionAlwaysTrue, what + " tests a literal True, so the test does nothing", &info);
            return;
        }
        if (!decidable(f)) return;
        if (unsat({&f}, c.s)) {
            c.report(v, st.ip, kConditionImpossible,
                     what + "'s condition can never be true (" +
                         (self_compared(f) ? std::string("it compares a value with itself") : std::string("its comparisons contradict each other")) +
                         "), so its block never runs; a common cause is && written where || was meant, or a wrong value in one comparison");
            return;
        }
        if (ctx && !ctx->empty()) {
            std::vector<const Formula*> conj;
            for (const Formula& g : *ctx) conj.push_back(&g);
            conj.push_back(&f);
            if (unsat(conj, c.s)) {
                c.report(v, st.ip, kConditionImpossible,
                         "This ElseIf can never be true: every value it accepts is already taken by an earlier If or ElseIf of the same chain, so its block never runs; reorder the tests or fix the bounds");
                return;
            }
        }
        if (isWhile && contains_return(st.body)) return;  // a loop left by Return may test True on purpose
        const Formula nf = negate(f);
        if (unsat({&nf}, c.s)) {
            c.report(v, st.ip, kConditionAlwaysTrue,
                     what + "'s condition is always true (" +
                         (self_compared(f) ? std::string("it compares a value with itself") : std::string("its comparisons cover every value")) +
                         "), so the test does nothing" + (!isWhile && !st.elseBody.empty() ? " and its Else never runs" : "") +
                         "; a common cause is || written where && was meant");
        }
    };
    std::function<void(const std::vector<StmtPtr>&, const std::vector<Formula>*)> walk =
        [&](const std::vector<StmtPtr>& ss, const std::vector<Formula>* ctx) {
            for (size_t i = 0; i < ss.size(); ++i) {
                const Stmt& st = *ss[i];
                // Only the first statement of an Else is reached with nothing run
                // in between (an ElseIf is exactly that).
                const std::vector<Formula>* here = i == 0 ? ctx : nullptr;
                if (st.kind == StmtKind::If || st.kind == StmtKind::While) check(st, here);
                if (st.kind == StmtKind::If && st.rhs) {
                    std::vector<Formula> elseCtx = here ? *here : std::vector<Formula>{};
                    elseCtx.push_back(negate(to_formula(*st.rhs, fc, calls_in(*st.rhs))));
                    // A plain Else after tests that leave no value never runs.
                    if (!st.elseBody.empty() && st.elseBody[0]->kind != StmtKind::If && elseCtx.size() > 1) {
                        std::vector<const Formula*> conj;
                        bool anyDecidable = false;
                        for (const Formula& g : elseCtx) {
                            conj.push_back(&g);
                            anyDecidable = anyDecidable || decidable(g);
                        }
                        if (anyDecidable && unsat(conj, c.s))
                            c.report(v, st.elseBody[0]->ip, kCodeUnreachable,
                                     "This Else never runs: the If and ElseIf tests before it already take every value");
                    }
                    walk(st.body, nullptr);
                    walk(st.elseBody, &elseCtx);
                } else {
                    walk(st.body, nullptr);
                    walk(st.elseBody, nullptr);
                }
            }
        };
    walk(v.stmts, nullptr);
}

// --- code after a Return ------------------------------------------------------------

bool always_returns(const Stmt& st) {
    if (st.kind == StmtKind::Return) return true;
    if (st.kind == StmtKind::If && !st.elseBody.empty()) {
        auto ends = [](const std::vector<StmtPtr>& ss) {
            for (const StmtPtr& p : ss) {
                if (always_returns(*p)) return true;
            }
            return false;
        };
        return ends(st.body) && ends(st.elseBody);
    }
    // While True ends only through a Return.
    if (st.kind == StmtKind::While && st.rhs && literal_true(strip_casts(*st.rhs))) return true;
    return false;
}

void rule_unreachable(Ctx& c, const FnView& v) {
    std::function<void(const std::vector<StmtPtr>&)> walk = [&](const std::vector<StmtPtr>& ss) {
        bool ended = false;
        for (const StmtPtr& p : ss) {
            const Stmt& st = *p;
            if (ended) {
                if (st.kind == StmtKind::Decl && !st.rhs) continue;  // a bare declaration makes no code
                c.report(v, st.ip, kCodeUnreachable,
                         "This code never runs: every path before it ends in a Return (or in a While True that only a Return leaves); remove it, or move it before the Return");
                break;
            }
            walk(st.body);
            walk(st.elseBody);
            if (always_returns(st)) ended = true;
        }
    };
    walk(v.stmts);
}

// --- properties nothing uses, states nothing enters, properties nothing fills ------

// Scripts the Creation Kit writes for fragments: their properties are filled
// for the fragments, and quest fragment scripts get one Alias_ property per
// alias whether a fragment uses it or not.
bool is_fragment_script(std::string_view lowerName) {
    // QF_, TIF__, SF_, PF_, PRKF_, also after a prefix: DLC1_QF_..., BYOH_TIF__...
    for (const char* p : {"qf_", "tif__", "sf_", "pf_", "prkf_"}) {
        const size_t at = lowerName.find(p);
        if (at != std::string_view::npos && (at == 0 || lowerName[at - 1] == '_')) return true;
    }
    return false;
}

template <class F>
void each_function_of(const Object& o, F&& f) {
    for (const Property& p : o.properties) {
        if (p.getter) f(*p.getter, &p);
        if (p.setter) f(*p.setter, &p);
    }
    for (const State& st : o.states) {
        for (const NamedFunction& nf : st.functions) f(nf.body, nullptr);
    }
}

bool flag_set(const Script& s, uint32_t userFlags, std::string_view name) {
    for (const UserFlag& u : s.userFlags) {
        if (eq_ci(s.str(u.name), name)) return (userFlags >> u.bit) & 1u;
    }
    return false;
}

// Does the script's own code touch a property (its autovar, or PropGet /
// PropSet of that name), outside the property's own accessors?
void own_property_use(const Script& s, const Object& o, const Property& p, bool& reads, bool& writes) {
    reads = writes = false;
    const std::string_view name = s.str(p.name);
    each_function_of(o, [&](const Function& f, const Property* owner) {
        if (owner == &p) return;
        for (const Instruction& ins : f.code) {
            if ((ins.op == Opcode::PropGet || ins.op == Opcode::PropSet) && !ins.args.empty() && ins.args[0].is_string_ref() &&
                eq_ci(s.str(ins.args[0].str()), name)) {
                (ins.op == Opcode::PropGet ? reads : writes) = true;
                continue;
            }
            if (!p.is_auto()) continue;
            for (size_t i = 0; i < ins.args.size(); ++i) {
                const Value& a = ins.args[i];
                if (a.type != ValueType::Identifier || a.str() != p.autoVar) continue;
                // The destination operand of the opcodes that write one.
                const bool dest = (i == 0 && (ins.op == Opcode::Assign || ins.op == Opcode::Cast)) ||
                                  (i == 2 && (ins.op == Opcode::CallMethod || ins.op == Opcode::CallStatic || ins.op == Opcode::PropGet)) ||
                                  (i == 1 && ins.op == Opcode::CallParent);
                (dest ? writes : reads) = true;
            }
        }
    });
}

void report_object(Ctx& c, const Object& o, const std::string& where, const std::string& state, const char* rule, std::string message,
                   Severity* override = nullptr) {
    Finding f;
    f.object = std::string(c.str(o.name));
    f.state = state;
    f.function = where;
    f.severity = override ? *override : severity_of(rule);
    f.rule = rule;
    f.message = std::move(message);
    c.out.push_back(std::move(f));
}

// The scripts that extend `type`, directly or not (lowercased), from the index.
std::set<std::string> descendants_of(const AuditIndex& idx, const std::string& type) {
    std::set<std::string> out;
    std::vector<std::string> todo{type};
    while (!todo.empty()) {
        const std::string t = todo.back();
        todo.pop_back();
        auto it = idx.childrenOf.find(t);
        if (it == idx.childrenOf.end()) continue;
        for (const std::string& child : it->second) {
            if (out.insert(child).second) todo.push_back(child);
        }
    }
    return out;
}

// property-unused, property-unfilled.
void rule_properties(Ctx& c, const Object& o) {
    const std::string self = lower(c.str(o.name));
    if (is_fragment_script(self)) return;
    const AuditIndex* idx = c.opt.index;
    std::set<std::string> family;  // this script and every script extending it
    bool attached = false;
    if (idx && idx->fillsKnown) {
        family = descendants_of(*idx, self);
        family.insert(self);
        for (const std::string& t : family) attached = attached || idx->filled.count(t);
    }
    for (const Property& p : o.properties) {
        const std::string name(c.str(p.name));
        const std::string key = lower(name);
        if (flag_set(c.s, p.userFlags, "conditional")) continue;  // read by plugin conditions
        bool reads = false, writes = false;
        own_property_use(c.s, o, p, reads, writes);
        const bool readElsewhere = idx && idx->propertiesRead.count(key);
        const bool writtenElsewhere = idx && idx->propertiesWritten.count(key);
        if (!reads && !writes && !readElsewhere && !writtenElsewhere) {
            report_object(c, o, name, "", kPropertyUnused,
                          "Property " + name + " is never read or written by this script" +
                              std::string(idx ? " or any other script audited or resolved with it" : "") +
                              "; unless a plugin condition (GetVMQuestVariable) or a script outside this set uses it, it can go, along with its fill in the plugin");
            continue;
        }
        // Never filled: an object or array the script reads, nothing assigns,
        // with no default, on a script some plugin attaches while no attachment
        // of it (or of a script extending it) fills the property.
        if (!idx || !idx->fillsKnown || !attached || !reads || writes || writtenElsewhere) continue;
        const std::string type = lower(c.str(p.type));
        if (type == "int" || type == "float" || type == "bool" || type == "string") continue;
        if (p.is_auto()) {
            bool hasDefault = false;
            for (const Variable& v : o.variables) {
                if (v.name == p.autoVar) hasDefault = v.initial.type != ValueType::None;
            }
            if (hasDefault || c.assignedVars.count(p.autoVar)) continue;
        }
        // A script that tests the property in a condition (If Prop, If Prop != None)
        // treats it as optional: leaving it empty is the plugin author's choice.
        bool tested = false;
        for (const FnView& fv : c.fns) {
            if (fv.o != &o) continue;
            each_stmt(fv.stmts, 0, [&](const Stmt& st, int) {
                if ((st.kind != StmtKind::If && st.kind != StmtKind::While) || !st.rhs) return;
                each_expr(*st.rhs, [&](const Expr& e) {
                    if (e.kind == ExprKind::Var && p.is_auto() && e.name == p.autoVar) tested = true;
                    if (e.kind == ExprKind::PropGet && eq_ci(c.str(e.name), name)) tested = true;
                });
            });
        }
        if (tested) continue;
        bool filled = false;
        for (const std::string& t : family) {
            auto it = idx->filled.find(t);
            if (it != idx->filled.end() && it->second.count(key)) filled = true;
        }
        if (filled) continue;
        report_object(c, o, name, "", kPropertyUnfilled,
                      "Property " + name + " is read by the script, but no plugin in the load order that attaches " + std::string(c.str(o.name)) +
                          " (or a script extending it) fills it and nothing assigns it, so it is None when read; fill it in the plugin, or assign it in OnInit");
    }
}

// state-unreachable.
void rule_states(Ctx& c, const Object& o) {
    // A GotoState with a computed name could enter any state: say nothing.
    std::set<std::string> ownLiterals;
    bool computed = false;
    each_function_of(o, [&](const Function& f, const Property*) {
        for (const Instruction& ins : f.code) {
            if (ins.op != Opcode::CallMethod || ins.args.size() < 4 || !ins.args[0].is_string_ref()) continue;
            if (!eq_ci(c.str(ins.args[0].str()), "gotostate")) continue;
            if (ins.args[3].type == ValueType::String) ownLiterals.insert(lower(c.str(ins.args[3].str())));
            else computed = true;
        }
        for (const Instruction& ins : f.code) {
            for (const Value& a : ins.args) {
                if (a.type == ValueType::String) ownLiterals.insert(lower(c.str(a.str())));
            }
        }
    });
    if (computed) return;
    // Nor when an ancestor does: SkyUI's SKI_ConfigBase enters a menu's option
    // states by a name it builds, so every state of an MCM script is reached.
    if (c.opt.resolver) {
        std::string cur = lower(c.str(o.parent));
        std::set<std::string> seen;
        for (int guard = 0; guard < 32 && !cur.empty() && seen.insert(cur).second; ++guard) {
            const Script* ps = c.opt.resolver->find(cur);
            if (!ps || ps->objects.empty()) break;
            const Object& po = ps->objects[0];
            bool dynamic = false;
            each_function_of(po, [&](const Function& f, const Property*) {
                for (const Instruction& ins : f.code) {
                    if (ins.op == Opcode::CallMethod && ins.args.size() >= 4 && ins.args[0].is_string_ref() &&
                        eq_ci(ps->str(ins.args[0].str()), "gotostate") && ins.args[3].type != ValueType::String)
                        dynamic = true;
                }
            });
            if (dynamic) return;
            cur = lower(ps->str(po.parent));
        }
    }
    const AuditIndex* idx = c.opt.index;
    // Another script enters this type's states (or its ancestors') by a built name.
    if (idx && !idx->dynamicStateTypes.empty()) {
        if (idx->dynamicStateTypes.count("*")) return;
        std::string cur = lower(c.str(o.name));
        std::set<std::string> seen;
        for (int guard = 0; guard < 32 && !cur.empty() && seen.insert(cur).second; ++guard) {
            if (idx->dynamicStateTypes.count(cur)) return;
            auto it = idx->parentOf.find(cur);
            cur = it == idx->parentOf.end() ? std::string() : it->second;
        }
    }
    const std::string autoState = lower(c.str(o.autoState));
    for (const State& st : o.states) {
        const std::string name(c.str(st.name));
        const std::string key = lower(name);
        if (key.empty() || key == autoState) continue;
        if (ownLiterals.count(key) || (idx && (idx->statesEntered.count(key) || idx->stringLiterals.count(key)))) continue;
        const std::string first = st.functions.empty() ? std::string() : std::string(c.str(st.functions[0].name));
        report_object(c, o, first, name, kStateUnreachable,
                      "State " + name + " is never entered: no GotoState in this script" +
                          std::string(idx ? " or any other script audited or resolved with it" : "") +
                          " names it, and it is not the auto state, so its functions never run; check the spelling where it should be entered");
    }
}

}  // namespace

void index_script(const Script& s, AuditIndex& idx) {
    for (const Object& o : s.objects) {
        idx.parentOf[lower(s.str(o.name))] = lower(s.str(o.parent));
        // The declared type of a slot a function names (parameter, local, temp,
        // script variable), for GotoState calls on other objects.
        auto type_in = [&](const Function& f, StrIdx name) -> std::string {
            for (const Param& p : f.params) if (p.name == name) return lower(s.str(p.type));
            for (const Param& p : f.locals) if (p.name == name) return lower(s.str(p.type));
            for (const Variable& v : o.variables) if (v.name == name) return lower(s.str(v.type));
            return std::string();
        };
        each_function_of(o, [&](const Function& f, const Property*) {
            for (const Instruction& ins : f.code) {
                if (ins.op == Opcode::CallMethod && ins.args.size() >= 4 && ins.args[0].is_string_ref() &&
                    eq_ci(s.str(ins.args[0].str()), "gotostate") && ins.args[3].type != ValueType::String &&
                    ins.args[1].type == ValueType::Identifier && !eq_ci(s.str(ins.args[1].str()), "self")) {
                    const std::string t = type_in(f, ins.args[1].str());
                    idx.dynamicStateTypes.insert(t.empty() ? std::string("*") : t);  // "*": a type not known
                }
                if ((ins.op == Opcode::PropGet || ins.op == Opcode::PropSet) && !ins.args.empty() && ins.args[0].is_string_ref())
                    (ins.op == Opcode::PropGet ? idx.propertiesRead : idx.propertiesWritten).insert(lower(s.str(ins.args[0].str())));
                else if (ins.op == Opcode::CallMethod && ins.args.size() >= 4 && ins.args[0].is_string_ref() &&
                         eq_ci(s.str(ins.args[0].str()), "gotostate") && ins.args[3].type == ValueType::String)
                    idx.statesEntered.insert(lower(s.str(ins.args[3].str())));
                for (const Value& a : ins.args) {
                    if (a.type == ValueType::String && s.str(a.str()).size() <= 64) idx.stringLiterals.insert(lower(s.str(a.str())));
                }
            }
        });
    }
}

void finish_index(AuditIndex& idx) {
    idx.childrenOf.clear();
    for (const auto& [child, parent] : idx.parentOf) {
        if (!parent.empty()) idx.childrenOf[parent].push_back(child);
    }
}

void merge_index(AuditIndex& into, AuditIndex&& from) {
    into.propertiesRead.merge(from.propertiesRead);
    into.propertiesWritten.merge(from.propertiesWritten);
    into.statesEntered.merge(from.statesEntered);
    into.stringLiterals.merge(from.stringLiterals);
    into.dynamicStateTypes.merge(from.dynamicStateTypes);
    into.parentOf.merge(from.parentOf);
    if (from.fillsKnown) into.fillsKnown = true;
    for (auto& [k, v] : from.filled) into.filled[k].merge(v);
}

namespace {

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
        rule_conditions(c, v, fc);
        rule_unreachable(c, v);
    }
    rule_update_pairs(c);
    for (const Object& o : s.objects) {
        rule_properties(c, o);
        rule_states(c, o);
    }

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
