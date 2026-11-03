// Source order recovered from debug info.
//
// The tables in a .pex are in the writing tool's hash order, but the debug
// records are appended in the order the compiler met the functions: the
// property accessors first, then the default state's functions, then the
// other states as they appear in the source. Bethesda's tools also label
// only the first function of each state block with the state's name (the
// rest get the empty state) and merge records with the same label and name,
// keeping the first record's position and the last function's lines.
//
// From that, this recovers: the order of states, the order of functions in
// each state, and which function was first in its block. The assembly writer
// uses it so the official assembler recreates the same records; the
// decompiler uses it to put functions back where the source had them.
// Without debug info everything stays in file order.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "papyrus/pex.hpp"

#include <vector>

namespace papyrus::pex {

struct SourceLayout {
    // Indices into Object::states, default state first.
    std::vector<size_t> states;
    // For each state index: indices into State::functions in source order.
    std::vector<std::vector<size_t>> functions;
    // Indices into Object::properties in source order (best effort: accessor
    // records order those with accessors; auto properties keep file order).
    std::vector<size_t> properties;
    bool fromDebugInfo = false;
};

SourceLayout source_layout(const Script& script, const Object& object);

}  // namespace papyrus::pex
