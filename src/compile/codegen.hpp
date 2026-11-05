// Function bodies: statements and expressions into pex instructions, with
// the temp allocator and every rule of docs/COMPILER_PATTERNS.md.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "interface.hpp"

#include <functional>

namespace papyrus::psc {

struct LowerError {
    int line;
    std::string message;
};

// What a function body needs from its object: the script being compiled,
// the program for lookups, the string table, and the per-object temp counter.
struct ObjectContext {
    pex::Script& s;
    Program& prog;
    const ScriptInfo& self;
    std::function<pex::StrIdx(std::string_view)> intern;
    int tempCounter = 0;
};

// Compiles `f`'s body into `pf` (locals and code); one debug line per
// instruction is appended to `lines`. `fi` is f's interface (return type,
// parameters). Throws LowerError.
void compile_body(ObjectContext& ctx, const Function& f, const FuncInfo& fi, pex::Function& pf, std::vector<uint16_t>& lines);

}  // namespace papyrus::psc
