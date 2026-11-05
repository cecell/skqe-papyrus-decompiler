// Lowering: a checked syntax tree becomes a pex::Script, following
// docs/COMPILER_PATTERNS.md.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "interface.hpp"

namespace papyrus::psc {

// Compiles the script `info` describes (its tree is info.ast) into `out`.
// Returns false after appending diagnostics; `out` is then incomplete.
bool lower_script(const ScriptInfo& info, Program& program, const CompileOptions& options, const SourceFile& source,
                  pex::Script& out, std::vector<Diagnostic>& diagnostics);

}  // namespace papyrus::psc
