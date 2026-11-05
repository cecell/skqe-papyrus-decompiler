// The syntax tree of a Papyrus source file (Skyrim's dialect), as the parser
// builds it and the checker and lowering read it. Every node carries its
// source line. Names keep the source spelling; comparisons elsewhere are
// case-insensitive.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace papyrus::psc {

struct TypeName {
    std::string name;      // "Int", "ObjectReference", ... ; empty = none (a Function without a return type)
    bool isArray = false;  // "Int[]"
    int line = 0;
    bool empty() const noexcept { return name.empty(); }
};

enum class LitKind { None, Int, Float, String, Bool };

struct Literal {
    LitKind kind = LitKind::None;
    int32_t i = 0;
    float f = 0.f;
    bool b = false;
    std::string s;  // String
    int line = 0;
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Arg {
    std::string name;  // named argument, or empty
    ExprPtr value;
    int line = 0;
};

enum class ExprKind {
    Lit,     // lit
    Ident,   // text = the name (variable, property, type for a static call receiver)
    Self,
    Parent,
    Unary,   // text = "-" | "!", a
    Binary,  // text = "+" "-" "*" "/" "%" "==" "!=" "<" ">" "<=" ">=" "&&" "||", a, b
    Cast,    // a as type
    Index,   // a[b]
    Member,  // a.text (property, or Length)
    Call,    // (a.)text(args): a = receiver or null (own function / imported global)
    New,     // new type[a]
};

struct Expr {
    ExprKind kind;
    int line = 0;
    int column = 0;
    std::string text;
    Literal lit;
    ExprPtr a, b;
    TypeName type;
    std::vector<Arg> args;

    explicit Expr(ExprKind k, int ln = 0, int col = 0) : kind(k), line(ln), column(col) {}
};

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

enum class StmtKind { Decl, Assign, ExprStmt, If, While, Return };

struct ElseIf {
    ExprPtr cond;
    std::vector<StmtPtr> body;
    int line = 0;
};

struct Stmt {
    StmtKind kind;
    int line = 0;
    // Decl
    TypeName declType;
    std::string declName;
    ExprPtr init;  // may be null
    // Assign: target op value; op is "=", "+=", "-=", "*=", "/=", "%="
    ExprPtr target;
    std::string op;
    ExprPtr value;
    // ExprStmt / Return (value may be null) / If and While condition
    ExprPtr expr;
    std::vector<StmtPtr> body;
    std::vector<ElseIf> elseIfs;
    bool hasElse = false;
    std::vector<StmtPtr> elseBody;
    int endLine = 0;  // EndIf / EndWhile line

    explicit Stmt(StmtKind k, int ln = 0) : kind(k), line(ln) {}
};

struct Param {
    TypeName type;
    std::string name;
    bool hasDefault = false;
    Literal def;
    int line = 0;
};

struct Function {
    std::string name;
    TypeName returnType;  // empty for no value
    std::vector<Param> params;
    bool isEvent = false;
    bool isGlobal = false;
    bool isNative = false;
    std::vector<std::string> flags;  // user flags written after the signature (none in Skyrim)
    std::string doc;
    std::vector<StmtPtr> body;
    int line = 0;
    int endLine = 0;
};

struct Property {
    TypeName type;
    std::string name;
    bool isAuto = false;          // Auto
    bool isAutoReadOnly = false;  // AutoReadOnly
    bool hasInit = false;
    Literal init;
    std::vector<std::string> flags;  // Hidden, Conditional, ... (validated against the .flg file)
    std::string doc;
    std::unique_ptr<Function> getter, setter;  // full property
    int line = 0;
};

struct Variable {
    TypeName type;
    std::string name;
    bool hasInit = false;
    Literal init;
    std::vector<std::string> flags;
    int line = 0;
};

struct State {
    std::string name;
    bool isAuto = false;
    std::vector<Function> functions;
    int line = 0;
};

struct Script {
    std::string name;
    std::string parent;  // empty when the script extends nothing
    std::vector<std::string> flags;
    std::string doc;
    std::vector<std::string> imports;
    std::vector<Variable> variables;
    std::vector<Property> properties;
    std::vector<Function> functions;  // the default state's
    std::vector<State> states;
    // Declarations in source order, for the compiler's traversal: each entry
    // is (kind, index) into the vectors above (0 variable, 1 property, 2
    // function, 3 state).
    std::vector<std::pair<int, size_t>> order;
    int line = 0;
};

}  // namespace papyrus::psc
