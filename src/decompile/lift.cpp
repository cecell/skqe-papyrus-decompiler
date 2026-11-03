// Decompiler passes P0-P5: decode, basic blocks, lifting instructions to
// statements, folding compiler temporaries back into expressions, and
// collapsing the short-circuit && / || shapes. Output: blocks whose
// statements are expression trees and whose conditional terminators carry a
// condition expression (docs/DESIGN.md).
// SPDX-License-Identifier: GPL-3.0-only
#include "ir.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace papyrus::pex::detail {

// --- FnContext ---------------------------------------------------------------

FnContext::FnContext(const Script& script, const Object& object, const Function& fn, ScriptResolver* r)
    : s(script), o(object), f(fn), objectType(lower(script.str(object.name))), resolver(r) {}

std::vector<std::string> FnContext::ancestors_of(std::string_view type) const {
    std::vector<std::string> out;
    std::string cur = lower(type);
    // The script itself is known without the resolver.
    if (cur == objectType) {
        cur = lower(s.str(o.parent));
        if (cur.empty()) return out;
        out.push_back(cur);
    }
    for (int guard = 0; guard < 32 && resolver && !cur.empty(); ++guard) {
        const Script* ps = resolver->find(cur);
        if (!ps || ps->objects.empty()) break;
        cur = lower(ps->str(ps->objects[0].parent));
        if (cur.empty()) break;
        out.push_back(cur);
    }
    return out;
}

bool FnContext::is_subtype(const std::string& from, const std::string& to) const {
    if (from.empty() || to.empty()) return false;
    if (from == to) return true;
    for (const std::string& a : ancestors_of(from)) {
        if (a == to) return true;
    }
    return false;
}

FnContext::FoundFunction FnContext::find_function(std::string_view type, std::string_view name) const {
    FoundFunction out;
    std::string cur = lower(type);
    const std::string n = lower(name);
    for (int guard = 0; guard < 32 && !cur.empty(); ++guard) {
        const Script* ps = cur == objectType ? &s : (resolver ? resolver->find(cur) : nullptr);
        if (!ps || ps->objects.empty()) break;
        const Object& po = ps->objects[0];
        // Default state first, then any state (the signature is the same).
        for (int pass = 0; pass < 2 && !out.function; ++pass) {
            for (const State& st : po.states) {
                const bool isDefault = ps->str(st.name).empty();
                if ((pass == 0) != isDefault) continue;
                for (const NamedFunction& nf : st.functions) {
                    if (lower(ps->str(nf.name)) == n) {
                        out.script = ps;
                        out.function = &nf.body;
                        break;
                    }
                }
                if (out.function) break;
            }
        }
        if (out.function) return out;
        cur = lower(ps->str(po.parent));
    }
    return out;
}

std::string FnContext::lower(std::string_view v) {
    std::string out(v);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

static bool starts_with_ci(std::string_view v, std::string_view prefix) {
    if (v.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(v[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

static bool equals_ci(std::string_view a, std::string_view b) {
    return a.size() == b.size() && starts_with_ci(a, b);
}

static bool is_temp_name(std::string_view v) {
    if (v.size() <= 6 || !starts_with_ci(v, "::temp")) return false;
    for (size_t i = 6; i < v.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(v[i]))) return false;
    }
    return true;
}

bool FnContext::is_temp(StrIdx name) const noexcept {
    const std::string_view v = s.str(name);
    return is_temp_name(v) || equals_ci(v, "::nonevar");
}
bool FnContext::is_nonevar(StrIdx name) const noexcept { return equals_ci(s.str(name), "::nonevar"); }
bool FnContext::is_self(StrIdx name) const noexcept { return equals_ci(s.str(name), "self"); }

std::string FnContext::type_of(StrIdx name) const {
    if (is_self(name)) return objectType;
    if (is_nonevar(name)) return "none";
    const std::string_view n = s.str(name);
    for (const Param& p : f.params) {
        if (p.name == name || equals_ci(s.str(p.name), n)) return lower(s.str(p.type));
    }
    for (const Param& p : f.locals) {
        if (p.name == name || equals_ci(s.str(p.name), n)) return lower(s.str(p.type));
    }
    for (const Variable& v : o.variables) {
        if (v.name == name || equals_ci(s.str(v.name), n)) return lower(s.str(v.type));
    }
    // A parent's variable (an inherited auto property's backing variable).
    if (resolver && n.size() > 2 && n[0] == ':' && n[1] == ':') {
        std::string cur = lower(s.str(o.parent));
        for (int guard = 0; guard < 32 && !cur.empty(); ++guard) {
            const Script* ps = resolver->find(cur);
            if (!ps || ps->objects.empty()) break;
            for (const Variable& v : ps->objects[0].variables) {
                if (equals_ci(ps->str(v.name), n)) return lower(ps->str(v.type));
            }
            cur = lower(ps->str(ps->objects[0].parent));
        }
    }
    return "";
}

const Property* FnContext::property_of_autovar(StrIdx var) const noexcept {
    const std::string_view n = s.str(var);
    for (const Property& p : o.properties) {
        if (p.is_auto() && (p.autoVar == var || equals_ci(s.str(p.autoVar), n))) return &p;
    }
    return nullptr;
}

std::string FnContext::display_name(StrIdx name) const {
    const std::string_view v = s.str(name);
    if (is_self(name)) return "Self";
    if (is_nonevar(name)) return "None";  // a read of the discarded-result slot is the value None
    if (v.size() > 2 && v[0] == ':' && v[1] == ':') {
        if (const Property* p = property_of_autovar(name)) return std::string(s.str(p->name));
        if (starts_with_ci(v, "::mangled_")) {
            const std::string_view rest = v.substr(10);
            const size_t us = rest.rfind('_');
            if (us != std::string_view::npos && us > 0) return std::string(rest.substr(0, us));
            return std::string(rest);
        }
        if (is_temp_name(v)) return "_temp_" + std::string(v.substr(6));
        // An auto property of a parent script: its backing variable is
        // addressed directly, by the compiler's ::Name_var convention.
        if (v.size() > 6 && equals_ci(v.substr(v.size() - 4), "_var")) return std::string(v.substr(2, v.size() - 6));
        return std::string(v);
    }
    return std::string(v);
}

// --- expression helpers -------------------------------------------------------

ExprPtr make_var(const FnContext& c, StrIdx name) {
    auto e = std::make_unique<Expr>(ExprKind::Var);
    e->name = name;
    e->type = c.type_of(name);
    return e;
}

ExprPtr make_lit(const Value& v) {
    auto e = std::make_unique<Expr>(ExprKind::Lit);
    e->lit = v;
    switch (v.type) {
    case ValueType::Integer: e->type = "int"; break;
    case ValueType::Float: e->type = "float"; break;
    case ValueType::Bool: e->type = "bool"; break;
    case ValueType::String: e->type = "string"; break;
    default: e->type = "none"; break;
    }
    return e;
}

std::string type_of_expr(const Expr& e) { return e.type; }

size_t count_reads(const Expr& e, StrIdx temp) {
    size_t n = e.kind == ExprKind::Var && e.name == temp ? 1 : 0;
    for (const ExprPtr& k : e.kids) n += count_reads(*k, temp);
    return n;
}

bool replace_read(ExprPtr& e, StrIdx temp, ExprPtr& replacement) {
    if (e->kind == ExprKind::Var && e->name == temp) {
        e = std::move(replacement);
        return true;
    }
    for (ExprPtr& k : e->kids) {
        if (replace_read(k, temp, replacement)) return true;
    }
    return false;
}

bool Stmt::defines_temp(const Script& s) const noexcept {
    if (kind != StmtKind::Assign || !lhs || lhs->kind != ExprKind::Var) return false;
    const std::string_view v = s.str(lhs->name);
    return is_temp_name(v) || equals_ci(v, "::nonevar");
}

// --- the lifter -------------------------------------------------------------

namespace {

class Lifter {
public:
    explicit Lifter(const FnContext& c) : m_c(c), m_f(c.f) {}

    FunctionIR run() {
        decode_and_split();
        for (Block& b : m_blocks) fold_block(b);
        compute_preds();
        collapse_short_circuits();
        resolve_leftover_temps();
        FunctionIR ir;
        ir.blocks = std::move(m_blocks);
        ir.materialised = std::move(m_materialised);
        return ir;
    }

private:
    static bool is_jump(Opcode op) { return op == Opcode::Jmp || op == Opcode::JmpT || op == Opcode::JmpF; }

    size_t jump_target(const Instruction& ins, size_t ip) const {
        const Value& off = ins.args.back();
        if (off.type != ValueType::Integer) throw DecompileError("jump offset is not an integer", ip);
        const long long t = static_cast<long long>(ip) + off.as_int();
        if (t < 0 || t > static_cast<long long>(m_f.code.size())) throw DecompileError("jump target outside the function", ip);
        return static_cast<size_t>(t);
    }

    // P0 + P1: leaders -> blocks; P3: lift each block's instructions.
    void decode_and_split() {
        const size_t n = m_f.code.size();
        std::set<size_t> leaders{0};
        for (size_t ip = 0; ip < n; ++ip) {
            const Instruction& ins = m_f.code[ip];
            if (is_jump(ins.op)) {
                leaders.insert(ip + 1);
                leaders.insert(jump_target(ins, ip));
            }
        }
        leaders.erase(n);  // the end is not a block
        std::vector<size_t> starts(leaders.begin(), leaders.end());
        for (size_t i = 0; i < starts.size(); ++i) {
            Block b;
            b.start = starts[i];
            b.end = i + 1 < starts.size() ? starts[i + 1] : n;
            for (size_t ip = b.start; ip < b.end; ++ip) {
                const Instruction& ins = m_f.code[ip];
                if (is_jump(ins.op)) {
                    b.term = ins.op == Opcode::Jmp ? Term::Jump : ins.op == Opcode::JmpT ? Term::JumpT : Term::JumpF;
                    b.target = jump_target(ins, ip);
                    if (b.term != Term::Jump) {
                        b.cond = operand(ins.args[0]);
                        // The accumulator of a short-circuit expression is the
                        // temp the jump tests, whatever gets folded into the
                        // condition later.
                        if (ins.args[0].type == ValueType::Identifier && m_c.is_temp(ins.args[0].str())) {
                            b.condTemp = ins.args[0].str();
                        }
                    }
                    break;  // a jump always ends its block
                }
                if (StmtPtr st = lift(ins, ip)) b.stmts.push_back(std::move(st));
            }
            m_blocks.push_back(std::move(b));
        }
    }

    ExprPtr operand(const Value& v) const {
        return v.type == ValueType::Identifier ? make_var(m_c, v.str()) : make_lit(v);
    }
    ExprPtr operand(const Value& v, size_t ip) const {
        ExprPtr e = operand(v);
        e->ip = ip;
        return e;
    }

    StmtPtr assign_to(const Value& dest, ExprPtr rhs, size_t ip, bool typeFromDest) const {
        if (dest.type != ValueType::Identifier) throw DecompileError("destination operand is not an identifier", ip);
        rhs->ip = ip;
        if (m_c.is_nonevar(dest.str())) {
            // The discarded-result slot. Treated as a temp: almost always
            // unread (then a bare statement), occasionally read as None.
            if (rhs->type.empty()) rhs->type = "none";
            auto st = std::make_unique<Stmt>(StmtKind::Assign, ip);
            st->lhs = make_var(m_c, dest.str());
            st->rhs = std::move(rhs);
            return st;
        }
        if (typeFromDest || rhs->type.empty()) {
            const std::string t = m_c.type_of(dest.str());
            if (!t.empty()) rhs->type = t;
        }
        auto st = std::make_unique<Stmt>(StmtKind::Assign, ip);
        st->lhs = make_var(m_c, dest.str());
        st->rhs = std::move(rhs);
        return st;
    }

    ExprPtr binary(const char* op, const Instruction& ins, const char* type) const {
        auto e = std::make_unique<Expr>(ExprKind::Binary);
        e->op = op;
        e->kids.push_back(operand(ins.args[1]));
        e->kids.push_back(operand(ins.args[2]));
        e->type = type;
        return e;
    }

    ExprPtr unary(const char* op, const Instruction& ins, const std::string& type) const {
        auto e = std::make_unique<Expr>(ExprKind::Unary);
        e->op = op;
        e->kids.push_back(operand(ins.args[1]));
        e->type = type;
        return e;
    }

    StmtPtr lift(const Instruction& ins, size_t ip) const {
        const auto& a = ins.args;
        switch (ins.op) {
        case Opcode::Nop: return nullptr;
        case Opcode::IAdd: return assign_to(a[0], binary("+", ins, "int"), ip, false);
        case Opcode::FAdd: return assign_to(a[0], binary("+", ins, "float"), ip, false);
        case Opcode::ISub: return assign_to(a[0], binary("-", ins, "int"), ip, false);
        case Opcode::FSub: return assign_to(a[0], binary("-", ins, "float"), ip, false);
        case Opcode::IMul: return assign_to(a[0], binary("*", ins, "int"), ip, false);
        case Opcode::FMul: return assign_to(a[0], binary("*", ins, "float"), ip, false);
        case Opcode::IDiv: return assign_to(a[0], binary("/", ins, "int"), ip, false);
        case Opcode::FDiv: return assign_to(a[0], binary("/", ins, "float"), ip, false);
        case Opcode::IMod: return assign_to(a[0], binary("%", ins, "int"), ip, false);
        case Opcode::StrCat: return assign_to(a[0], binary("+", ins, "string"), ip, false);
        case Opcode::CmpEq: return assign_to(a[0], binary("==", ins, "bool"), ip, false);
        case Opcode::CmpLt: return assign_to(a[0], binary("<", ins, "bool"), ip, false);
        case Opcode::CmpLe: return assign_to(a[0], binary("<=", ins, "bool"), ip, false);
        case Opcode::CmpGt: return assign_to(a[0], binary(">", ins, "bool"), ip, false);
        case Opcode::CmpGe: return assign_to(a[0], binary(">=", ins, "bool"), ip, false);
        case Opcode::Not: return assign_to(a[0], unary("!", ins, "bool"), ip, false);
        case Opcode::INeg: return assign_to(a[0], unary("-", ins, "int"), ip, false);
        case Opcode::FNeg: return assign_to(a[0], unary("-", ins, "float"), ip, false);
        case Opcode::Assign: return assign_to(a[0], operand(a[1]), ip, false);
        case Opcode::Cast: {
            auto e = std::make_unique<Expr>(ExprKind::Cast);
            e->kids.push_back(operand(a[1]));
            if (a[0].type != ValueType::Identifier) throw DecompileError("cast destination is not an identifier", ip);
            e->name = type_index_of(a[0].str());
            return assign_to(a[0], std::move(e), ip, true);
        }
        case Opcode::CallMethod:
        case Opcode::CallParent:
        case Opcode::CallStatic: {
            auto e = std::make_unique<Expr>(ExprKind::Call);
            size_t first;
            const Value* dest;
            if (ins.op == Opcode::CallMethod) {
                e->call = CallKind::Method;
                e->name = ident(a[0], ip);
                e->kids.push_back(operand(a[1]));
                dest = &a[2];
                first = 3;
            } else if (ins.op == Opcode::CallParent) {
                e->call = CallKind::Parent;
                e->name = ident(a[0], ip);
                dest = &a[1];
                first = 2;
            } else {
                e->call = CallKind::Static;
                e->cls = ident(a[0], ip);
                e->name = ident(a[1], ip);
                dest = &a[2];
                first = 3;
            }
            for (size_t i = first; i < a.size(); ++i) e->kids.push_back(operand(a[i]));
            return assign_to(*dest, std::move(e), ip, true);
        }
        case Opcode::Return: {
            auto st = std::make_unique<Stmt>(StmtKind::Return, ip);
            st->rhs = operand(a[0]);
            return st;
        }
        case Opcode::PropGet: {
            auto e = std::make_unique<Expr>(ExprKind::PropGet);
            e->name = ident(a[0], ip);
            e->kids.push_back(operand(a[1]));
            return assign_to(a[2], std::move(e), ip, true);
        }
        case Opcode::PropSet: {
            auto st = std::make_unique<Stmt>(StmtKind::Assign, ip);
            auto lhs = std::make_unique<Expr>(ExprKind::PropGet);
            lhs->name = ident(a[0], ip);
            lhs->ip = ip;
            lhs->kids.push_back(operand(a[1]));
            st->lhs = std::move(lhs);
            st->rhs = operand(a[2]);
            return st;
        }
        case Opcode::ArrayCreate: {
            auto e = std::make_unique<Expr>(ExprKind::ArrayNew);
            e->kids.push_back(operand(a[1]));
            return assign_to(a[0], std::move(e), ip, true);
        }
        case Opcode::ArrayLength: {
            auto e = std::make_unique<Expr>(ExprKind::ArrayLen);
            e->kids.push_back(operand(a[1]));
            e->type = "int";
            return assign_to(a[0], std::move(e), ip, false);
        }
        case Opcode::ArrayGetElement: {
            auto e = std::make_unique<Expr>(ExprKind::ArrayGet);
            e->kids.push_back(operand(a[1]));
            e->kids.push_back(operand(a[2]));
            return assign_to(a[0], std::move(e), ip, true);
        }
        case Opcode::ArraySetElement: {
            auto st = std::make_unique<Stmt>(StmtKind::Assign, ip);
            auto lhs = std::make_unique<Expr>(ExprKind::ArrayGet);
            lhs->ip = ip;
            lhs->kids.push_back(operand(a[0]));
            lhs->kids.push_back(operand(a[1]));
            st->lhs = std::move(lhs);
            st->rhs = operand(a[2]);
            return st;
        }
        case Opcode::ArrayFindElement:
        case Opcode::ArrayRFindElement: {
            auto e = std::make_unique<Expr>(ExprKind::ArrayFind);
            e->reverse = ins.op == Opcode::ArrayRFindElement;
            e->kids.push_back(operand(a[0]));
            e->kids.push_back(operand(a[2]));
            e->kids.push_back(operand(a[3]));
            e->type = "int";
            return assign_to(a[1], std::move(e), ip, false);
        }
        case Opcode::Jmp:
        case Opcode::JmpT:
        case Opcode::JmpF: break;
        }
        throw DecompileError("unexpected opcode", ip);
    }

    StrIdx ident(const Value& v, size_t ip) const {
        if (v.type != ValueType::Identifier) throw DecompileError("operand is not an identifier", ip);
        return v.str();
    }

    // String index of a slot's declared type (for cast targets).
    StrIdx type_index_of(StrIdx name) const {
        for (const Param& p : m_f.params) {
            if (p.name == name) return p.type;
        }
        for (const Param& p : m_f.locals) {
            if (p.name == name) return p.type;
        }
        for (const Variable& v : m_c.o.variables) {
            if (v.name == name) return v.type;
        }
        return 0;
    }

    // --- P4: temp folding ----------------------------------------------------

    static size_t reads_in_stmt(const Stmt& st, StrIdx temp) {
        size_t n = 0;
        if (st.rhs) n += count_reads(*st.rhs, temp);
        if (st.lhs && st.lhs->kind != ExprKind::Var) n += count_reads(*st.lhs, temp);
        return n;
    }

    static bool replace_in_stmt(Stmt& st, StrIdx temp, ExprPtr& repl) {
        if (st.rhs && replace_read(st.rhs, temp, repl)) return true;
        if (st.lhs && st.lhs->kind != ExprKind::Var && replace_read(st.lhs, temp, repl)) return true;
        return false;
    }

    // Folds pending temp definitions at the back of `out` into `st`, from
    // the most recent backwards: the compiler evaluates operands left to
    // right into fresh temps, so the last operand's definition is the last
    // statement, and so on. A definition that is not at the back, or a
    // temp read more than once, stays put (and becomes a named local).
    void fold_into(std::vector<StmtPtr>& out, Stmt& st) const {
        while (!out.empty()) {
            Stmt& d = *out.back();
            if (!d.defines_temp(m_c.s)) break;
            const StrIdx t = d.temp();
            if (reads_in_stmt(st, t) != 1) break;
            replace_in_stmt(st, t, d.rhs);
            out.pop_back();
        }
    }

    void fold_cond(std::vector<StmtPtr>& out, Block& b) const {
        if (b.term != Term::JumpT && b.term != Term::JumpF) return;
        while (!out.empty()) {
            Stmt& d = *out.back();
            if (!d.defines_temp(m_c.s)) break;
            const StrIdx t = d.temp();
            if (count_reads(*b.cond, t) != 1) break;
            replace_read(b.cond, t, d.rhs);
            b.condFromTemp = true;
            out.pop_back();
        }
    }

    void fold_block(Block& b) const {
        std::vector<StmtPtr> out;
        for (StmtPtr& st : b.stmts) {
            fold_into(out, *st);
            out.push_back(std::move(st));
        }
        fold_cond(out, b);
        b.stmts = std::move(out);
    }

    // --- P5: && and || --------------------------------------------------------

    size_t block_at(size_t ip) const {
        for (size_t i = 0; i < m_blocks.size(); ++i) {
            if (m_blocks[i].start == ip) return i;
        }
        return m_blocks.size();  // the end
    }

    void compute_preds() {
        for (Block& b : m_blocks) b.preds.clear();
        for (size_t i = 0; i < m_blocks.size(); ++i) {
            const Block& b = m_blocks[i];
            if (b.term != Term::Jump && i + 1 < m_blocks.size()) m_blocks[i + 1].preds.push_back(i);
            if (b.term != Term::Fall) {
                const size_t t = block_at(b.target);
                if (t < m_blocks.size()) m_blocks[t].preds.push_back(i);
            }
        }
    }

    static ExprPtr strip_bool_cast(ExprPtr e) {
        if (e->kind == ExprKind::Cast && e->type == "bool" && !e->kids.empty()) return std::move(e->kids[0]);
        return e;
    }

    bool try_collapse(size_t bi) {
        Block& b = m_blocks[bi];
        if ((b.term != Term::JumpT && b.term != Term::JumpF) || !b.condFromTemp || b.condTemp == 0) return false;
        const size_t fi = bi + 1, ji = bi + 2;
        if (ji >= m_blocks.size()) return false;
        Block& f = m_blocks[fi];
        Block& j = m_blocks[ji];
        if (f.term != Term::Fall || f.stmts.size() != 1) return false;
        Stmt& s = *f.stmts[0];
        if (!s.defines_temp(m_c.s) || s.temp() != b.condTemp) return false;
        if (j.start != b.target) return false;
        if (f.preds.size() != 1 || f.preds[0] != bi) return false;
        if (j.preds.size() != 2) return false;
        if (!((j.preds[0] == bi && j.preds[1] == fi) || (j.preds[0] == fi && j.preds[1] == bi))) return false;

        auto comb = std::make_unique<Expr>(b.term == Term::JumpF ? ExprKind::And : ExprKind::Or);
        comb->kids.push_back(strip_bool_cast(std::move(b.cond)));
        comb->kids.push_back(strip_bool_cast(std::move(s.rhs)));
        comb->type = "bool";
        auto def = std::make_unique<Stmt>(StmtKind::Assign, b.start);
        def->lhs = make_var(m_c, b.condTemp);
        def->rhs = std::move(comb);

        std::vector<StmtPtr> merged;
        for (StmtPtr& st : b.stmts) merged.push_back(std::move(st));
        merged.push_back(std::move(def));
        for (StmtPtr& st : j.stmts) merged.push_back(std::move(st));
        b.stmts.clear();
        b.term = j.term;
        b.target = j.target;
        b.cond = std::move(j.cond);
        b.condTemp = j.condTemp;
        b.condFromTemp = j.condFromTemp;
        b.end = j.end;
        // Re-fold: the new definition flows into its first reader.
        std::vector<StmtPtr> out;
        for (StmtPtr& st : merged) {
            fold_into(out, *st);
            out.push_back(std::move(st));
        }
        fold_cond(out, b);
        b.stmts = std::move(out);
        m_blocks.erase(m_blocks.begin() + static_cast<long long>(fi), m_blocks.begin() + static_cast<long long>(ji) + 1);
        compute_preds();
        return true;
    }

    void collapse_short_circuits() {
        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t i = 0; i < m_blocks.size(); ++i) {
                if (try_collapse(i)) {
                    changed = true;
                    break;
                }
            }
        }
    }

    // --- P5b: whatever temp is still assigned ---------------------------------

    bool temp_read_elsewhere(StrIdx t, const Stmt* except) const {
        for (const Block& b : m_blocks) {
            for (const StmtPtr& st : b.stmts) {
                if (st.get() == except) continue;
                if (reads_in_stmt(*st, t) > 0) return true;
            }
            if (b.cond && count_reads(*b.cond, t) > 0) return true;
        }
        return false;
    }

    void resolve_leftover_temps() {
        for (Block& b : m_blocks) {
            for (StmtPtr& st : b.stmts) {
                if (!st->defines_temp(m_c.s)) continue;
                const StrIdx t = st->temp();
                if (!temp_read_elsewhere(t, st.get())) {
                    // Nobody reads it: the source had a bare expression
                    // statement (a call, or a comparison typed where an
                    // assignment was meant; vanilla has dozens of those).
                    st->kind = StmtKind::Expr;
                    st->lhs.reset();
                    continue;
                }
                materialise(t);
            }
            if (b.cond) collect_temps(*b.cond);
            for (StmtPtr& st : b.stmts) {
                if (st->rhs) collect_temps(*st->rhs);
                if (st->lhs && st->lhs->kind != ExprKind::Var) collect_temps(*st->lhs);
            }
        }
    }

    void collect_temps(const Expr& e) {
        if (e.kind == ExprKind::Var && m_c.is_temp(e.name)) materialise(e.name);
        for (const ExprPtr& k : e.kids) collect_temps(*k);
    }

    void materialise(StrIdx t) {
        if (m_c.is_nonevar(t)) return;  // reads of ::NoneVar print as None
        for (const auto& m : m_materialised) {
            if (m.first == t) return;
        }
        m_materialised.emplace_back(t, m_c.type_of(t));
    }

    const FnContext& m_c;
    const Function& m_f;
    std::vector<Block> m_blocks;
    std::vector<std::pair<StrIdx, std::string>> m_materialised;
};

}  // namespace

FunctionIR lift_function(const FnContext& c) {
    return Lifter(c).run();
}

}  // namespace papyrus::pex::detail
