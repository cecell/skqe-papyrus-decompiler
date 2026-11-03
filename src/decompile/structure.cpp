// Decompiler pass P6: structured statements from basic blocks.
//
// Bethesda's compiler emits every construct as one contiguous interval:
//   If c:        JUMPF c Lelse; then; JUMP Lend; Lelse: [else;] Lend:
//   While c:     Ltop: c; JUMPF c Lexit; body; JUMP Ltop; Lexit:
// It never threads jumps or removes dead code, and Papyrus has no break,
// continue or goto, so a backward jump is always a While latch and a JUMPF
// is always an If or a While test (after the && / || shapes were collapsed
// in lift.cpp). The structurer walks block ranges, recognises those two
// shapes, and refuses anything else with the instruction index where the
// shape broke (docs/DESIGN.md).
// SPDX-License-Identifier: GPL-3.0-only
#include "ir.hpp"

#include <limits>

namespace papyrus::pex::detail {

namespace {

constexpr size_t kNone = std::numeric_limits<size_t>::max();

class Structurer {
public:
    Structurer(const FnContext& c, FunctionIR& ir) : m_c(c), m_blocks(ir.blocks) {
        m_latch.assign(m_blocks.size(), kNone);
        for (size_t i = 0; i < m_blocks.size(); ++i) {
            const Block& b = m_blocks[i];
            if (b.term != Term::Jump) continue;
            const size_t h = block_at(b.target);
            if (h < m_blocks.size() && h <= i) {
                if (m_latch[h] == kNone || i > m_latch[h]) m_latch[h] = i;
            }
        }
    }

    std::vector<StmtPtr> run() { return range(0, m_blocks.size(), kNone); }

private:
    size_t block_at(size_t ip) const {
        for (size_t i = 0; i < m_blocks.size(); ++i) {
            if (m_blocks[i].start == ip) return i;
        }
        return m_blocks.size();  // the end of the function
    }

    size_t code_end() const { return m_blocks.empty() ? 0 : m_blocks.back().end; }

    [[noreturn]] void fail(const char* what, size_t bi) const {
        throw DecompileError(what, bi < m_blocks.size() ? m_blocks[bi].start : code_end());
    }

    static void move_stmts(std::vector<StmtPtr>& out, std::vector<StmtPtr>& from) {
        for (StmtPtr& s : from) out.push_back(std::move(s));
        from.clear();
    }

    // Structures blocks [lo, hi). A plain jump inside the range is legal only
    // when it goes to `exitIp`, the range's natural continuation (the end of
    // the enclosing If, or the loop test).
    std::vector<StmtPtr> range(size_t lo, size_t hi, size_t exitIp) {
        std::vector<StmtPtr> out;
        size_t i = lo;
        while (i < hi) {
            Block& b = m_blocks[i];
            if (m_latch[i] != kNone && m_latch[i] < hi) {
                i = emit_while(out, i, hi);
                continue;
            }
            switch (b.term) {
            case Term::JumpF:
                move_stmts(out, b.stmts);
                i = emit_if(out, i, hi);
                break;
            case Term::JumpT: fail("a JUMPT outside an || expression", i);
            case Term::Jump:
                move_stmts(out, b.stmts);
                if (b.target != exitIp) fail("a jump that is not an If end or a loop latch", i);
                ++i;
                break;
            case Term::Fall:
                move_stmts(out, b.stmts);
                ++i;
                break;
            }
        }
        return out;
    }

    size_t emit_while(std::vector<StmtPtr>& out, size_t hi_block, size_t hi) {
        const size_t h = hi_block;
        Block& head = m_blocks[h];
        const size_t latch = m_latch[h];
        if (head.term != Term::JumpF) fail("loop header does not end in a JUMPF", h);
        if (!head.stmts.empty()) fail("statements inside a loop test", h);
        const size_t exitBlock = block_at(head.target);
        if (exitBlock != latch + 1 || exitBlock > hi) fail("loop exit is not right after its latch", h);
        auto w = std::make_unique<Stmt>(StmtKind::While, head.start);
        w->rhs = std::move(head.cond);
        w->body = range(h + 1, latch + 1, head.start);
        out.push_back(std::move(w));
        return latch + 1;
    }

    size_t emit_if(std::vector<StmtPtr>& out, size_t bi, size_t hi) {
        Block& b = m_blocks[bi];
        const size_t fIdx = block_at(b.target);
        if (fIdx <= bi || fIdx > hi) fail("If jumps outside its range", bi);
        if (fIdx == bi + 1) fail("If with no then-range", bi);
        Block& thenLast = m_blocks[fIdx - 1];
        auto st = std::make_unique<Stmt>(StmtKind::If, b.start);
        st->rhs = std::move(b.cond);
        if (thenLast.term == Term::Fall || (thenLast.term == Term::Jump && thenLast.target <= thenLast.start)) {
            // Bethesda's compiler always jumps to the end label; other
            // compilers leave the jump out when it would go to the next
            // instruction, so the then-range falls through, or ends in the
            // latch of a loop that is its last statement. Same meaning: an
            // If without Else.
            st->body = range(bi + 1, fIdx, b.target);
            out.push_back(std::move(st));
            return fIdx;
        }
        if (thenLast.term != Term::Jump) fail("then-range does not end in a jump", fIdx - 1);
        const size_t j = thenLast.target;
        if (j == b.target) {
            st->body = range(bi + 1, fIdx, j);
            out.push_back(std::move(st));
            return fIdx;
        }
        const size_t jIdx = block_at(j);
        if (jIdx <= fIdx || jIdx > hi) fail("If end label is not after the else-range", fIdx - 1);
        st->body = range(bi + 1, fIdx, j);
        st->elseBody = range(fIdx, jIdx, j);
        out.push_back(std::move(st));
        return jIdx;
    }

    const FnContext& m_c;
    std::vector<Block>& m_blocks;
    std::vector<size_t> m_latch;
};

}  // namespace

std::vector<StmtPtr> structure_function(const FnContext& c, FunctionIR& ir) {
    return Structurer(c, ir).run();
}

}  // namespace papyrus::pex::detail
