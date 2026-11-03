// Papyrus source (.psc) view of a compiled script.
//
// The decompiler works per function: bytecode -> basic blocks -> expression
// trees (compiler temporaries folded back into the expressions they came
// from) -> structured statements (If / ElseIf / Else, While, short-circuit
// && and ||) -> source text. A function the structurer cannot explain is
// emitted as a stub carrying its assembly in a comment, so one odd function
// never costs the file. The report says what happened to every function.
//
// Quality is measured, not assumed: pex check --roundtrip compiles the
// output with Bethesda's compiler and compares the result with the original
// (docs/ROUNDTRIP.md). docs/DESIGN.md describes the passes.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "papyrus/pex.hpp"

#include <string>
#include <vector>

namespace papyrus::pex {

enum class FnStatus {
    Ok,                   // fully structured
    OkMaterialisedTemps,  // structured, but a compiler temporary had to become a named local
    Failed,               // emitted as a stub with the assembly in a comment
};
const char* to_string(FnStatus s) noexcept;

struct FunctionReport {
    std::string object;
    std::string state;     // "" for the default state
    std::string name;      // property accessors: "Prop.get" / "Prop.set"
    FnStatus status = FnStatus::Ok;
    std::string reason;    // for Failed: one sentence
    uint32_t ip = 0;       // for Failed: the instruction where the structurer gave up
    size_t instructions = 0;
};

// Other scripts, by name, for the decompiler to consult: the parent chain
// (to leave out casts the compiler inserts on its own), the declarations of
// called functions (parameter names and types, so an omitted argument can be
// left out or passed by name) and inherited property types. Optional: without
// one, every object cast is printed and every argument is positional.
// Implementations must be safe to call from several threads at once.
struct ScriptResolver {
    virtual ~ScriptResolver() = default;
    // The compiled script called `name` (case-insensitive), or nullptr.
    virtual const Script* find(std::string_view name) = 0;
};

struct DecompileOptions {
    // Keep the compiler-generated GetState / GotoState functions in the output
    // (they are never written by hand; the compiler adds them back).
    bool keepGeneratedFunctions = false;
    // Append " ; #line N" comments from the debug info to statements.
    bool lineComments = false;
    // Declare every object- and array-typed parameter with "= None". Compiled
    // files do not store parameter defaults, and a call that omitted such a
    // parameter is decompiled without it, so a set of decompiled scripts
    // only compiles against itself when the declarations allow omission.
    // Changes nothing in the declaring script's compiled form. Used for
    // import copies (the round-trip check, the closure command), not for
    // output meant to be read.
    bool optionalObjectParams = false;
    // See ScriptResolver. Not owned.
    ScriptResolver* resolver = nullptr;
};

struct DecompileResult {
    std::string source;
    std::vector<FunctionReport> functions;
    bool all_ok() const noexcept {
        for (const FunctionReport& f : functions) {
            if (f.status == FnStatus::Failed) return false;
        }
        return true;
    }
};

DecompileResult decompile(const Script& script, const DecompileOptions& opt = {});

}  // namespace papyrus::pex
