// Canonical comparison of two scripts: "the same program" regardless of how
// the file was written. Ignores the header's times, user, machine and source
// path, the string table (every reference is resolved to text, identifiers
// case-insensitively), the order of the variable, property, state and
// function tables (name-keyed), and debug line numbers (debug presence and
// per-function line counts are kept). Everything else must match:
// declarations, flags, defaults, doc strings, parameters, locals (as a set)
// and the instruction streams operand for operand.
//
// This is what the round-trip oracles use (docs/ROUNDTRIP.md): the official
// assembler and compiler reorder those tables, so byte comparison is
// meaningless, and text comparison would hide real differences.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "papyrus/pex.hpp"

#include <string>
#include <vector>

namespace papyrus::pex {

enum class DiffKind {
    Header,        // version or game id
    UserFlags,     // the flag table
    Object,        // name, parent, doc, flags, auto state
    Variable,      // missing, extra or different variable
    Property,      // missing, extra or different property declaration
    Function,      // missing or extra function, or a different signature
    Locals,        // same function, different local set
    Instruction,   // same function, different instruction stream
    DebugInfo,     // debug presence or per-function line counts
};
const char* to_string(DiffKind k) noexcept;

struct Difference {
    DiffKind kind;
    std::string where;    // "Foo.Busy.OnActivate" / "Foo.prop Count" / ...
    std::string message;  // one sentence
};

struct CompareOptions {
    // Ignore the two functions the compiler itself adds to every script
    // (GetState / GotoState in the default state) when either side lacks
    // them. Different compiler builds disagree on emitting them.
    bool ignoreGeneratedStateFunctions = true;
    // Treat compiler temporaries (locals named ::tempN) as equal when their
    // numbering differs but the structure is the same (renumber by first
    // appearance before comparing). Off: temp names must match literally.
    bool renumberTemps = false;
    // Compare the header's version and game id. Off for the assembler check:
    // Bethesda's assembler always writes 3.2, and old mods carry 3.1.
    bool compareHeader = true;
    // Stop after this many differences (0 = all).
    size_t maxDifferences = 50;
};

// Returns true when the two scripts are canonically equal; differences are
// appended to `out` when it is given.
bool canonical_equal(const Script& a, const Script& b, std::vector<Difference>* out = nullptr,
                     const CompareOptions& opt = {});

}  // namespace papyrus::pex
