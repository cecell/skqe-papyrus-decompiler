// The Papyrus compiler front end: source (.psc) to the pex::Script model
// (docs/COMPILER.md). Nothing throws across this API; every failure is a
// Diagnostic with a file, line and message.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "papyrus/pex.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace papyrus::psc {

struct Script;  // the syntax tree (src/compile/ast.hpp)

struct Diagnostic {
    std::string file;
    int line = 0;
    int column = 0;
    std::string message;
    std::string text() const;  // "file(line,column): message"
};

// The tree is an incomplete type to users of this header; this deleter lets
// them hold one anyway.
struct ScriptDeleter {
    void operator()(Script* s) const noexcept;
};
using ScriptPtr = std::unique_ptr<Script, ScriptDeleter>;

// Parses one source text. Returns false with the first error in `error`.
// `file` is only used in the diagnostic. The tree is owned by the caller.
bool parse(std::string_view text, const std::string& file, ScriptPtr& out, Diagnostic& error);

// --- user flags (the .flg file) ---------------------------------------------

struct UserFlagDef {
    std::string name;
    int bit = 0;
    // Where the flag may be written; all true when the file lists no types.
    bool onScript = true, onProperty = true, onVariable = true, onFunction = true;
};

// Parses "Flag <name> <bit> [{ Script Property Variable Function }]" entries
// with // and /* */ comments (TESV_Papyrus_Flags.flg).
bool load_flags(std::string_view text, std::vector<UserFlagDef>& out, std::string* error = nullptr);

// --- sources ---------------------------------------------------------------

struct SourceFile {
    std::string name;      // script name as the file is called: "Foo" for Foo.psc
    std::string path;      // where it was read from (diagnostics, the header's source name)
    std::string text;
    uint64_t modified = 0;  // time_t of the file, for the debug info; 0 = unknown
};

// Finds the source of a script by name for imports and the parent chain.
class SourceResolver {
public:
    virtual ~SourceResolver() = default;
    // Case-insensitive on the name. Returns false when there is no such source.
    virtual bool find(std::string_view scriptName, SourceFile& out) = 0;
};

// A resolver over import folders, searched in order; file names compared
// case-insensitively (each folder is listed once).
std::unique_ptr<SourceResolver> folder_resolver(const std::vector<std::filesystem::path>& folders);

// --- compiling -------------------------------------------------------------

struct CompileOptions {
    std::vector<UserFlagDef> flags;
    SourceResolver* resolver = nullptr;  // where imports and the parent chain come from
    // Compile every function with an empty body: the declarations alone
    // (milestone 2 of docs/COMPILER.md; used with CompareOptions::declarationsOnly).
    bool declarationsOnly = false;
    std::string userName = "skqe";
    std::string computerName = "papyrus";
    uint64_t compileTime = 0;  // 0 = now
};

// Compiles one script. Returns true when `out` holds the compiled script and
// no error was reported; diagnostics are appended either way.
bool compile(const SourceFile& source, const CompileOptions& options, pex::Script& out, std::vector<Diagnostic>& diagnostics);

}  // namespace papyrus::psc
