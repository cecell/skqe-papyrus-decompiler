// The script auditor: static rules over the decompiler's view of a compiled
// script that point out where it can misbehave at run time (loops that cannot
// end, update timers that never stop, calls on values that can be None,
// recursion, arrays used before they exist, states that do not exist).
//
// Each finding names the function, the instruction and, when the file carries
// debug information, the source line, with one sentence of advice an author
// can act on. docs/AUDIT.md explains every rule, what it is grounded in
// (save-game VM state, Papyrus logs) and how often it fires on Bethesda's own
// scripts. Nothing here throws across the API.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "papyrus/decompile.hpp"
#include "papyrus/pex.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace papyrus::pex {

enum class Severity { Info, Warning, Error };
const char* to_string(Severity s) noexcept;  // "info", "warning", "error"

struct Finding {
    std::string object;
    std::string state;      // "" for the default state
    std::string function;   // accessors: "Prop.get" / "Prop.set"
    uint32_t ip = 0;        // the instruction the finding is anchored to
    uint16_t line = 0;      // source line from the debug info; 0 when unknown
    Severity severity = Severity::Warning;
    std::string rule;       // stable id, e.g. "loop-no-exit"
    std::string message;    // one sentence for the author
};

struct AuditRule {
    std::string id;
    Severity severity;
    std::string summary;    // one line, for `pex audit --rules`
};

struct AuditOptions {
    // Other scripts, for inherited states and declared types (optional; see
    // ScriptResolver in decompile.hpp). Not owned.
    ScriptResolver* resolver = nullptr;
};

struct AuditResult {
    std::vector<Finding> findings;  // in function order, then by instruction
    size_t functions = 0;           // bodies examined
    size_t skipped = 0;             // bodies the decompiler could not structure (no findings for them)
};

AuditResult audit(const Script& script, const AuditOptions& opt = {});

// Every rule the auditor knows, in the order they are documented.
const std::vector<AuditRule>& audit_rules();

}  // namespace papyrus::pex
