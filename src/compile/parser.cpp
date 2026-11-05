// Recursive-descent parser for Skyrim's Papyrus (docs/COMPILER.md, "The
// language accepted"). Builds the tree of ast.hpp; the first error stops the
// script and comes back as a Diagnostic.
// SPDX-License-Identifier: GPL-3.0-only
#include "ast.hpp"
#include "lexer.hpp"
#include "papyrus/compile.hpp"

#include <cstring>

namespace papyrus::psc {

void ScriptDeleter::operator()(Script* s) const noexcept { delete s; }

std::string Diagnostic::text() const {
    return file + "(" + std::to_string(line) + "," + std::to_string(column) + "): " + message;
}

namespace {

struct ParseError {
    int line;
    int column;
    std::string message;
};

class Parser {
public:
    explicit Parser(std::vector<Token>& toks) : m_t(toks) {}

    ScriptPtr run() {
        ScriptPtr s(new Script());
        skip_newlines();
        if (is_kw("scriptname")) header(*s);
        for (;;) {
            skip_newlines();
            if (at(Tok::End)) break;
            if (is_kw("import")) {
                next();
                s->imports.push_back(expect_ident("a script name"));
                end_of_line();
            } else if (is_kw("auto") || is_kw("state")) {
                s->order.emplace_back(3, s->states.size());
                s->states.push_back(state());
            } else if (is_kw("event") || is_kw("function")) {
                s->order.emplace_back(2, s->functions.size());
                s->functions.push_back(function(TypeName{}));
            } else if (at(Tok::Ident)) {
                // Type [ "[]" ] Function | Property | Ident
                TypeName t = type_name();
                if (is_kw("function")) {
                    s->order.emplace_back(2, s->functions.size());
                    s->functions.push_back(function(t));
                } else if (is_kw("property")) {
                    s->order.emplace_back(1, s->properties.size());
                    s->properties.push_back(property(t));
                } else {
                    s->order.emplace_back(0, s->variables.size());
                    s->variables.push_back(variable(t));
                }
            } else {
                error("unexpected " + describe(cur()));
            }
        }
        return s;
    }

private:
    // --- tokens --------------------------------------------------------------

    const Token& cur() const { return m_t[m_pos]; }
    const Token& peek(size_t n = 1) const { return m_t[std::min(m_pos + n, m_t.size() - 1)]; }
    bool at(Tok k) const { return cur().kind == k; }
    bool at_op(const char* op) const { return cur().kind == Tok::Op && cur().text == op; }
    bool is_kw(const char* kw) const { return cur().kind == Tok::Ident && ieq(cur().text, kw); }
    static bool is_kw(const Token& t, const char* kw) { return t.kind == Tok::Ident && ieq(t.text, kw); }
    void next() {
        if (m_pos + 1 < m_t.size()) ++m_pos;
    }
    void skip_newlines() {
        while (at(Tok::Newline)) next();
    }
    [[noreturn]] void error(const std::string& what) const { throw ParseError{cur().line, cur().column, what}; }
    static std::string describe(const Token& t) {
        switch (t.kind) {
        case Tok::Ident: return "'" + t.text + "'";
        case Tok::Int: case Tok::Float: return "number " + t.text;
        case Tok::String: return "string literal";
        case Tok::Doc: return "documentation string";
        case Tok::Op: return "'" + t.text + "'";
        case Tok::Newline: return "end of line";
        case Tok::End: return "end of file";
        }
        return "token";
    }
    void expect_op(const char* op) {
        if (!at_op(op)) error(std::string("expected '") + op + "', got " + describe(cur()));
        next();
    }
    void expect_kw(const char* kw) {
        if (!is_kw(kw)) error(std::string("expected '") + kw + "', got " + describe(cur()));
        next();
    }
    std::string expect_ident(const char* what) {
        if (!at(Tok::Ident)) error(std::string("expected ") + what + ", got " + describe(cur()));
        if (is_keyword(cur().text)) error("'" + cur().text + "' is a keyword, expected " + what);
        std::string s = cur().text;
        next();
        return s;
    }
    void end_of_line() {
        if (at(Tok::End)) return;
        if (!at(Tok::Newline)) error("expected end of line, got " + describe(cur()));
        next();
    }
    static bool is_keyword(std::string_view s) {
        static const char* const kKeywords[] = {"as",          "auto",       "autoreadonly", "bool",   "else",       "elseif",
                                                "endevent",    "endfunction", "endif",       "endproperty", "endstate", "endwhile",
                                                "event",       "extends",    "false",        "float",  "function",   "global",
                                                "if",          "import",     "int",          "native", "new",        "none",
                                                "parent",      "property",   "return",       "scriptname", "self",   "state",
                                                "string",      "true",       "while"};
        for (const char* k : kKeywords) {
            if (ieq(s, k)) return true;
        }
        return false;
    }
    static bool is_type_keyword(std::string_view s) { return ieq(s, "bool") || ieq(s, "int") || ieq(s, "float") || ieq(s, "string"); }

    // An optional documentation string on the following line(s).
    std::string doc() {
        size_t save = m_pos;
        skip_newlines();
        if (at(Tok::Doc)) {
            std::string d = cur().text;
            next();
            return d;
        }
        m_pos = save;
        return std::string();
    }

    // --- declarations --------------------------------------------------------

    void header(Script& s) {
        s.line = cur().line;
        expect_kw("scriptname");
        s.name = expect_ident("the script name");
        if (is_kw("extends")) {
            next();
            s.parent = expect_ident("the parent script name");
        }
        while (at(Tok::Ident)) s.flags.push_back(flag());
        end_of_line();
        s.doc = doc();
    }

    std::string flag() {
        if (is_keyword(cur().text)) error("unexpected keyword '" + cur().text + "'");
        std::string f = cur().text;
        next();
        return f;
    }

    TypeName type_name() {
        TypeName t;
        t.line = cur().line;
        if (!at(Tok::Ident)) error("expected a type, got " + describe(cur()));
        if (is_keyword(cur().text) && !is_type_keyword(cur().text)) error("'" + cur().text + "' is not a type");
        t.name = cur().text;
        next();
        if (at_op("[") && peek().kind == Tok::Op && peek().text == "]") {
            next();
            next();
            t.isArray = true;
        }
        return t;
    }

    Literal literal() {
        Literal l;
        l.line = cur().line;
        bool neg = false;
        if (at_op("-")) {
            neg = true;
            next();
        }
        if (at(Tok::Int)) {
            l.kind = LitKind::Int;
            l.i = neg ? -cur().ival : cur().ival;
            next();
        } else if (at(Tok::Float)) {
            l.kind = LitKind::Float;
            l.f = neg ? -cur().fval : cur().fval;
            next();
        } else if (neg) {
            error("expected a number after '-'");
        } else if (at(Tok::String)) {
            l.kind = LitKind::String;
            l.s = cur().text;
            next();
        } else if (is_kw("true") || is_kw("false")) {
            l.kind = LitKind::Bool;
            l.b = is_kw("true");
            next();
        } else if (is_kw("none")) {
            l.kind = LitKind::None;
            next();
        } else {
            error("expected a literal, got " + describe(cur()));
        }
        return l;
    }

    Variable variable(TypeName t) {
        Variable v;
        v.type = t;
        v.line = t.line;
        v.name = expect_ident("a variable name");
        if (at_op("=")) {
            next();
            v.init = literal();
            v.hasInit = true;
        }
        while (at(Tok::Ident)) v.flags.push_back(flag());
        end_of_line();
        return v;
    }

    Property property(TypeName t) {
        Property p;
        p.type = t;
        p.line = cur().line;
        expect_kw("property");
        p.name = expect_ident("a property name");
        if (at_op("=")) {
            next();
            p.init = literal();
            p.hasInit = true;
        }
        while (at(Tok::Ident)) {
            if (is_kw("auto")) {
                p.isAuto = true;
                next();
            } else if (is_kw("autoreadonly")) {
                p.isAutoReadOnly = true;
                next();
            } else {
                p.flags.push_back(flag());
            }
        }
        end_of_line();
        p.doc = doc();
        if (p.isAuto || p.isAutoReadOnly) return p;
        // Full property: Get / Set functions until EndProperty.
        for (;;) {
            skip_newlines();
            if (is_kw("endproperty")) {
                next();
                break;
            }
            if (at(Tok::End)) error("missing EndProperty for property " + p.name);
            TypeName rt;
            if (!is_kw("function")) rt = type_name();
            Function f = function(rt);
            if (ieq(f.name, "get")) {
                if (p.getter) error("property " + p.name + " has two Get functions");
                p.getter = std::make_unique<Function>(std::move(f));
            } else if (ieq(f.name, "set")) {
                if (p.setter) error("property " + p.name + " has two Set functions");
                p.setter = std::make_unique<Function>(std::move(f));
            } else {
                error("a property function is Get or Set, not " + f.name);
            }
        }
        end_of_line();
        return p;
    }

    // At "Function" or "Event" (the return type, if any, already read).
    Function function(TypeName returnType) {
        Function f;
        f.returnType = returnType;
        f.line = cur().line;
        if (is_kw("event")) {
            f.isEvent = true;
            if (!returnType.empty()) error("an event has no return type");
        } else if (!is_kw("function")) {
            error("expected 'Function', got " + describe(cur()));
        }
        next();
        f.name = expect_ident("a function name");
        expect_op("(");
        if (!at_op(")")) {
            for (;;) {
                Param p;
                p.type = type_name();
                p.line = p.type.line;
                p.name = expect_ident("a parameter name");
                if (at_op("=")) {
                    next();
                    p.def = literal();
                    p.hasDefault = true;
                }
                f.params.push_back(std::move(p));
                if (at_op(",")) {
                    next();
                    continue;
                }
                break;
            }
        }
        expect_op(")");
        while (at(Tok::Ident)) {
            if (is_kw("global")) {
                f.isGlobal = true;
                next();
            } else if (is_kw("native")) {
                f.isNative = true;
                next();
            } else {
                f.flags.push_back(flag());
            }
        }
        end_of_line();
        f.doc = doc();
        if (f.isNative) return f;
        const char* endKw = f.isEvent ? "endevent" : "endfunction";
        f.body = block({endKw});
        f.endLine = cur().line;
        expect_kw(endKw);
        end_of_line();
        return f;
    }

    State state() {
        State st;
        st.line = cur().line;
        if (is_kw("auto")) {
            st.isAuto = true;
            next();
        }
        expect_kw("state");
        st.name = expect_ident("a state name");
        end_of_line();
        for (;;) {
            skip_newlines();
            if (is_kw("endstate")) {
                next();
                break;
            }
            if (at(Tok::End)) error("missing EndState for state " + st.name);
            TypeName rt;
            if (!is_kw("function") && !is_kw("event")) rt = type_name();
            st.functions.push_back(function(rt));
        }
        end_of_line();
        return st;
    }

    // --- statements ----------------------------------------------------------

    // Statements until one of the terminator keywords (not consumed).
    std::vector<StmtPtr> block(std::initializer_list<const char*> terminators) {
        std::vector<StmtPtr> out;
        for (;;) {
            skip_newlines();
            if (at(Tok::End)) error("unexpected end of file inside a block");
            for (const char* t : terminators) {
                if (is_kw(t)) return out;
            }
            out.push_back(statement());
        }
    }

    StmtPtr statement() {
        const int line = cur().line;
        if (is_kw("if")) {
            next();
            auto st = std::make_unique<Stmt>(StmtKind::If, line);
            st->expr = expression();
            end_of_line();
            st->body = block({"elseif", "else", "endif"});
            while (is_kw("elseif")) {
                ElseIf e;
                e.line = cur().line;
                next();
                e.cond = expression();
                end_of_line();
                e.body = block({"elseif", "else", "endif"});
                st->elseIfs.push_back(std::move(e));
            }
            if (is_kw("else")) {
                next();
                st->hasElse = true;
                end_of_line();
                st->elseBody = block({"endif"});
            }
            st->endLine = cur().line;
            expect_kw("endif");
            end_of_line();
            return st;
        }
        if (is_kw("while")) {
            next();
            auto st = std::make_unique<Stmt>(StmtKind::While, line);
            st->expr = expression();
            end_of_line();
            st->body = block({"endwhile"});
            st->endLine = cur().line;
            expect_kw("endwhile");
            end_of_line();
            return st;
        }
        if (is_kw("return")) {
            next();
            auto st = std::make_unique<Stmt>(StmtKind::Return, line);
            if (!at(Tok::Newline) && !at(Tok::End)) st->expr = expression();
            end_of_line();
            return st;
        }
        // Declaration: Type [ "[]" ] Name [= expr]
        if (at(Tok::Ident) && (!is_keyword(cur().text) || is_type_keyword(cur().text))) {
            const Token& t1 = peek();
            const bool arr = t1.kind == Tok::Op && t1.text == "[" && peek(2).kind == Tok::Op && peek(2).text == "]";
            const Token& nameTok = arr ? peek(3) : t1;
            if (nameTok.kind == Tok::Ident && !is_keyword(nameTok.text)) {
                auto st = std::make_unique<Stmt>(StmtKind::Decl, line);
                st->declType = type_name();
                st->declName = expect_ident("a variable name");
                if (at_op("=")) {
                    next();
                    st->init = expression();
                }
                end_of_line();
                return st;
            }
        }
        ExprPtr e = expression();
        static const char* const kAssign[] = {"=", "+=", "-=", "*=", "/=", "%="};
        for (const char* op : kAssign) {
            if (at_op(op)) {
                next();
                auto st = std::make_unique<Stmt>(StmtKind::Assign, line);
                st->target = std::move(e);
                st->op = op;
                st->value = expression();
                end_of_line();
                return st;
            }
        }
        auto st = std::make_unique<Stmt>(StmtKind::ExprStmt, line);
        st->expr = std::move(e);
        end_of_line();
        return st;
    }

    // --- expressions ---------------------------------------------------------

    ExprPtr make(ExprKind k, const Token& at) { return std::make_unique<Expr>(k, at.line, at.column); }

    ExprPtr binary(const Token& opTok, ExprPtr a, ExprPtr b) {
        ExprPtr e = make(ExprKind::Binary, opTok);
        e->text = opTok.text;
        e->a = std::move(a);
        e->b = std::move(b);
        return e;
    }

    ExprPtr expression() { return or_expr(); }

    ExprPtr or_expr() {
        ExprPtr a = and_expr();
        while (at_op("||")) {
            const Token op = cur();
            next();
            a = binary(op, std::move(a), and_expr());
        }
        return a;
    }
    ExprPtr and_expr() {
        ExprPtr a = cmp_expr();
        while (at_op("&&")) {
            const Token op = cur();
            next();
            a = binary(op, std::move(a), cmp_expr());
        }
        return a;
    }
    ExprPtr cmp_expr() {
        ExprPtr a = add_expr();
        while (at_op("==") || at_op("!=") || at_op("<") || at_op(">") || at_op("<=") || at_op(">=")) {
            const Token op = cur();
            next();
            a = binary(op, std::move(a), add_expr());
        }
        return a;
    }
    ExprPtr add_expr() {
        ExprPtr a = mul_expr();
        while (at_op("+") || at_op("-")) {
            const Token op = cur();
            next();
            a = binary(op, std::move(a), mul_expr());
        }
        return a;
    }
    ExprPtr mul_expr() {
        ExprPtr a = unary_expr();
        while (at_op("*") || at_op("/") || at_op("%")) {
            const Token op = cur();
            next();
            a = binary(op, std::move(a), unary_expr());
        }
        return a;
    }
    ExprPtr unary_expr() {
        if (at_op("-") || at_op("!")) {
            const Token op = cur();
            next();
            ExprPtr a = unary_expr();
            // A negated number is a negative literal (the compiler folds it).
            if (op.text == "-" && a->kind == ExprKind::Lit && (a->lit.kind == LitKind::Int || a->lit.kind == LitKind::Float)) {
                if (a->lit.kind == LitKind::Int) a->lit.i = -a->lit.i;
                else a->lit.f = -a->lit.f;
                a->line = op.line;
                a->column = op.column;
                return a;
            }
            ExprPtr e = make(ExprKind::Unary, op);
            e->text = op.text;
            e->a = std::move(a);
            return e;
        }
        return cast_expr();
    }
    ExprPtr cast_expr() {
        ExprPtr a = postfix_expr();
        while (is_kw("as")) {
            const Token as = cur();
            next();
            ExprPtr e = make(ExprKind::Cast, as);
            e->a = std::move(a);
            e->type = type_name();
            a = std::move(e);
        }
        return a;
    }
    ExprPtr postfix_expr() {
        ExprPtr a = atom();
        for (;;) {
            if (at_op(".")) {
                const Token dot = cur();
                next();
                if (!at(Tok::Ident)) error("expected a member name after '.', got " + describe(cur()));
                const Token name = cur();
                next();
                if (at_op("(")) {
                    ExprPtr e = make(ExprKind::Call, name);
                    e->text = name.text;
                    e->a = std::move(a);
                    e->args = arguments();
                    a = std::move(e);
                } else {
                    ExprPtr e = make(ExprKind::Member, name);
                    e->text = name.text;
                    e->a = std::move(a);
                    a = std::move(e);
                }
                continue;
            }
            if (at_op("[")) {
                const Token br = cur();
                next();
                ExprPtr e = make(ExprKind::Index, br);
                e->a = std::move(a);
                e->b = expression();
                expect_op("]");
                a = std::move(e);
                continue;
            }
            return a;
        }
    }
    std::vector<Arg> arguments() {
        expect_op("(");
        std::vector<Arg> args;
        if (at_op(")")) {
            next();
            return args;
        }
        for (;;) {
            Arg g;
            g.line = cur().line;
            if (at(Tok::Ident) && peek().kind == Tok::Op && peek().text == "=") {
                g.name = cur().text;
                next();
                next();
            }
            g.value = expression();
            args.push_back(std::move(g));
            if (at_op(",")) {
                next();
                continue;
            }
            break;
        }
        expect_op(")");
        return args;
    }
    ExprPtr atom() {
        const Token t = cur();
        switch (t.kind) {
        case Tok::Int: {
            next();
            ExprPtr e = make(ExprKind::Lit, t);
            e->lit.kind = LitKind::Int;
            e->lit.i = t.ival;
            e->lit.line = t.line;
            return e;
        }
        case Tok::Float: {
            next();
            ExprPtr e = make(ExprKind::Lit, t);
            e->lit.kind = LitKind::Float;
            e->lit.f = t.fval;
            e->lit.line = t.line;
            return e;
        }
        case Tok::String: {
            next();
            ExprPtr e = make(ExprKind::Lit, t);
            e->lit.kind = LitKind::String;
            e->lit.s = t.text;
            e->lit.line = t.line;
            return e;
        }
        case Tok::Op:
            if (t.text == "(") {
                next();
                ExprPtr e = expression();
                expect_op(")");
                return e;
            }
            error("unexpected " + describe(t));
        case Tok::Ident: break;
        default: error("unexpected " + describe(t));
        }
        if (ieq(t.text, "true") || ieq(t.text, "false")) {
            next();
            ExprPtr e = make(ExprKind::Lit, t);
            e->lit.kind = LitKind::Bool;
            e->lit.b = ieq(t.text, "true");
            e->lit.line = t.line;
            return e;
        }
        if (ieq(t.text, "none")) {
            next();
            ExprPtr e = make(ExprKind::Lit, t);
            e->lit.kind = LitKind::None;
            e->lit.line = t.line;
            return e;
        }
        if (ieq(t.text, "self")) {
            next();
            return make(ExprKind::Self, t);
        }
        if (ieq(t.text, "parent")) {
            next();
            return make(ExprKind::Parent, t);
        }
        if (ieq(t.text, "new")) {
            next();
            ExprPtr e = make(ExprKind::New, t);
            e->type.line = cur().line;
            if (!at(Tok::Ident)) error("expected a type after 'new'");
            e->type.name = cur().text;
            next();
            expect_op("[");
            e->a = expression();
            expect_op("]");
            e->type.isArray = true;
            return e;
        }
        if (is_keyword(t.text)) error("unexpected keyword '" + t.text + "'");
        next();
        if (at_op("(")) {
            ExprPtr e = make(ExprKind::Call, t);
            e->text = t.text;
            e->args = arguments();
            return e;
        }
        ExprPtr e = make(ExprKind::Ident, t);
        e->text = t.text;
        return e;
    }

    std::vector<Token>& m_t;
    size_t m_pos = 0;
};

}  // namespace

bool parse(std::string_view text, const std::string& file, ScriptPtr& out, Diagnostic& error) {
    std::vector<Token> toks;
    LexError le;
    error.file = file;
    if (!lex(text, toks, le)) {
        error.line = le.line;
        error.column = le.column;
        error.message = le.message;
        return false;
    }
    try {
        Parser p(toks);
        out = p.run();
        return true;
    } catch (const ParseError& e) {
        error.line = e.line;
        error.column = e.column;
        error.message = e.message;
        return false;
    } catch (const std::exception& e) {
        error.message = e.what();
        return false;
    }
}

}  // namespace papyrus::psc
