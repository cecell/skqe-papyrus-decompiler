// Internal representation of the decompiler: expression trees, statements,
// basic blocks, and the per-function context (declared types). Internal to
// the library.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "papyrus/decompile.hpp"
#include "papyrus/pex.hpp"

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace papyrus::pex::detail {

// Raised inside the per-function pipeline; caught at the function boundary
// and turned into a stub + FunctionReport. Never crosses the public API.
struct DecompileError : std::runtime_error {
    size_t ip;
    DecompileError(const std::string& what, size_t at) : std::runtime_error(what), ip(at) {}
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

enum class ExprKind {
    Lit,       // a literal Value (int, float, bool, string, none)
    Var,       // a named slot: parameter, local, temp, script variable, autovar, self
    Unary,     // op: "!" or "-"
    Binary,    // op: + - * / % == != < <= > >=   (string + too)
    Cast,      // kids[0] as type
    Call,      // method/static/parent call, kids = [object?] + args
    PropGet,   // kids[0].name
    ArrayNew,  // new elemType[kids[0]]
    ArrayLen,  // kids[0].Length
    ArrayGet,  // kids[0][kids[1]]
    ArrayFind, // kids[0].Find/RFind(kids[1], kids[2])
    And,       // kids[0] && kids[1]
    Or,        // kids[0] || kids[1]
};

enum class CallKind { Method, Static, Parent };

struct Expr {
    ExprKind kind;
    Value lit;                       // Lit
    StrIdx name = 0;                 // Var: the slot; Call: function; PropGet: property; Cast: target type
    StrIdx cls = 0;                  // Call (Static): class
    CallKind call = CallKind::Method;
    std::string op;                  // Unary / Binary
    bool reverse = false;            // ArrayFind: RFind
    bool implicitCast = false;       // Cast: the compiler inserted it (do not print)
    bool explicitCast = false;       // Cast: the source wrote it (print even when a rule says implicit)
    size_t ip = 0;                   // instruction that produced this node (operands: the consumer's)
    std::vector<ExprPtr> kids;
    // Static type as the compiler saw it (lowercased type name, "" unknown).
    // For results stored in temps it is the temp's declared type, which is
    // exact; for literals the literal's kind.
    std::string type;

    explicit Expr(ExprKind k) : kind(k) {}
};

enum class StmtKind {
    Expr,    // a call whose result is discarded
    Assign,  // lhs = rhs; lhs is Var, PropGet or ArrayGet
    Return,  // Return rhs (rhs may be null)
    If,      // cond, body, elseBody (ElseIf chains are nested Ifs in elseBody)
    While,   // cond, body
    Decl,    // local declaration: type/name in declType/declName, optional rhs as initialiser
};

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

struct Stmt {
    StmtKind kind;
    size_t ip = 0;            // first instruction (for diagnostics and line comments)
    size_t ipEnd = 0;         // If / While: one past the last instruction of the whole statement
    StrIdx condTemp = 0;      // If / While: the temp the jump tested (0 when the condition was a variable)
    ExprPtr lhs;              // Assign
    ExprPtr rhs;              // Assign / Expr / Return / If cond / While cond / Decl init
    std::vector<StmtPtr> body, elseBody;
    std::string declType, declName;  // Decl

    explicit Stmt(StmtKind k, size_t at = 0) : kind(k), ip(at) {}

    // True when this statement assigns a compiler temporary (`::tempN`).
    bool defines_temp(const Script& s) const noexcept;
    StrIdx temp() const noexcept { return lhs ? lhs->name : 0; }
};

enum class Term { Fall, Jump, JumpT, JumpF };

struct Block {
    size_t start = 0, end = 0;  // instruction range [start, end), terminator included
    std::vector<StmtPtr> stmts;
    Term term = Term::Fall;
    size_t target = 0;          // jump target ip (Jump/JumpT/JumpF)
    ExprPtr cond;               // JumpT/JumpF condition
    StrIdx condTemp = 0;        // the temp the condition was computed into (0 = none)
    bool condFromTemp = false;
    std::vector<size_t> preds;  // block indices
};

// Per-function knowledge: the declared types of every name.
struct FnContext {
    const Script& s;
    const Object& o;
    const Function& f;
    std::string objectType;  // lowercased object name
    ScriptResolver* resolver = nullptr;

    FnContext(const Script& script, const Object& object, const Function& fn, ScriptResolver* r = nullptr);

    static std::string lower(std::string_view v);
    bool is_temp(StrIdx name) const noexcept;
    bool is_nonevar(StrIdx name) const noexcept;
    bool is_self(StrIdx name) const noexcept;
    // Lowercased declared type of a named slot ("" when unknown).
    std::string type_of(StrIdx name) const;
    // The property an autovar belongs to, or nullptr.
    const Property* property_of_autovar(StrIdx var) const noexcept;
    // Lowercased parent chain of a script type (nearest first), through the
    // resolver; empty when unknown.
    std::vector<std::string> ancestors_of(std::string_view type) const;
    // True when `from` is `to` or derives from it (both lowercased).
    bool is_subtype(const std::string& from, const std::string& to) const;
    // A function declaration found through the resolver: searches `type`
    // and its ancestors, default state then other states, then property
    // accessors are not considered. Returns the script and function.
    struct FoundFunction {
        const Script* script = nullptr;
        const Function* function = nullptr;
    };
    FoundFunction find_function(std::string_view type, std::string_view name) const;
    // Display name of a slot: ::X_var -> X, ::mangled_X_N -> X, self -> Self.
    std::string display_name(StrIdx name) const;
};

// Helpers shared by the passes.
ExprPtr make_var(const FnContext& c, StrIdx name);
ExprPtr make_lit(const Value& v);
std::string type_of_expr(const Expr& e);
size_t count_reads(const Expr& e, StrIdx temp);
bool replace_read(ExprPtr& e, StrIdx temp, ExprPtr& replacement);

// Pipeline stages (each in its own file).
struct FunctionIR {
    std::vector<Block> blocks;
    std::vector<std::pair<StrIdx, std::string>> materialised;  // temps that became locals: (temp, type)
};
FunctionIR lift_function(const FnContext& c);               // lift.cpp: decode, blocks, fold, &&/||
std::vector<StmtPtr> structure_function(const FnContext& c, FunctionIR& ir);  // structure.cpp

// The decompiler's spelling of one expression, and of one statement's own
// line (an If or While gives its header, "If x > 0"; an Else part is not
// part of it). `names` overrides the display names of slots (renamed or
// materialised locals). For diagnostics and the diff (psc_writer.cpp).
std::string render_expr(const FnContext& c, const Expr& e, const std::map<StrIdx, std::string>* names = nullptr);
std::string render_stmt(const FnContext& c, const Stmt& st, const std::map<StrIdx, std::string>* names = nullptr);

}  // namespace papyrus::pex::detail
