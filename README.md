# skqe-papyrus-decompiler

A C++17 library and command line tool for compiled Papyrus scripts (`.pex`)
of Skyrim LE/SE. GPL-3.0-only. CMake target `skqe::papyrus`, namespace
`papyrus::pex`. The library has no dependencies; the tool uses
[bsa-reader](https://github.com/cecell/bsa-reader) to read scripts out of
archives.

Everything in it is checked against Bethesda's own tools over every compiled
script on a modded machine, and the numbers are in the docs. What it does:

1. **Typed model, reader, byte-exact writer.** A `.pex` becomes a
   `papyrus::pex::Script` (header, string table, debug info, user flags,
   objects with variables, properties, states, functions, instructions).
   Writing it back reproduces the file byte for byte: 77,031 of 77,033
   scripts in a modded Data folder; the other two are a truncated backup and
   a file that is not a script. Damaged files are refused with a sentence
   naming what was wrong and at which byte; nothing throws.
2. **Papyrus assembly** in the syntax `PapyrusAssembler.exe` reads, and an
   **assembler** for it. Every one of the 14,026 vanilla scripts assembles
   back to the identical model through Bethesda's assembler and through
   ours, and the two agree on all 14,026 (ours takes 0.2 s for the lot).
3. **Decompiler.** Bytecode to structured source: `If`/`ElseIf`/`Else`,
   `While`, `&&`/`||`, compiler temporaries folded away, implicit casts and
   omitted arguments restored, locals declared in their scopes. All 26,657
   vanilla functions and 156,994 of 156,995 in the modded folder structure;
   a function that does not is emitted as a stub with its assembly, never
   taking the file with it. The score that matters is the round trip:
   decompile, compile with `PapyrusCompiler.exe`, compare with the original
   at model level: **14,021 of the 14,026 vanilla scripts (99.96%) come back
   identical**, and over the whole modded folder **75,125 of 76,593 (98.08%;
   99.0% of those whose dependencies exist at all)**; docs/ROUNDTRIP.md
   explains every class of the rest.
4. **A compiler** for Skyrim's Papyrus (`pex compile`), proved by compiling
   every vanilla source and comparing with the shipped compiled file at
   model level: **13,711 of 13,975 (98.1%) come back identical**, in 2.9 s
   for the lot, and **every one of the scripts Bethesda's compiler can build
   from those sources** (the shipped sources have drifted for 131, 133 lack
   a dependency's source, none differs). Over the two mod source folders of
   the modded `Data` the same holds: 6,101 of 6,883 and 9,373 of 10,168
   identical, none differing from what `PapyrusCompiler.exe` produces today.
   docs/COMPILER.md and docs/ROUNDTRIP.md say what the rest is.
5. **The analyses the ESL-compaction tooling needs**, with no compiler
   involved: `refs` lists every FormID literal paired with its plugin name,
   `patch --map` renumbers them in the compiled file, `closure` tells whether
   everything a script needs is present before anyone tries to compile it,
   `deps` says what a script needs an extender for.
6. **A script auditor** (`pex audit`): twelve rules over the decompiler's
   view of a script that tell authors where it can misbehave at run time:
   loops that cannot end or spin without a `Wait`, update timers never
   unregistered or re-armed without a stop, calls on results that can be None
   (`GetFormFromFile` first of all), recursion, arrays used before they
   exist, `GotoState` to a state that does not exist. Grounded in what the
   save games and the Papyrus logs on this machine actually show, and
   measured: 168 findings in 119 of the 14,026 vanilla scripts (every class
   inspected, Bethesda's own spin loops and undeclared states among them),
   3,428 in 1,066 of the 73,690 scripts the game would load from the modded
   folder (the winning copy of each; `--all-copies` audits all 77,047 files:
   3,815 in 1,215), in 1.1 s.
   [docs/AUDIT.md](docs/AUDIT.md) explains every rule in plain language.

## Use

```cpp
#include "papyrus/pex.hpp"
#include "papyrus/decompile.hpp"
#include "papyrus/analysis.hpp"
using namespace papyrus::pex;

Script s;
std::string err;
if (read_file("Foo.pex", s, &err) != Status::Ok) { /* err says why */ }

for (const FormRef& r : find_form_refs(s))              // 0x00000D62 in "Skyrim.esm", literal
    printf("%s %08X %s\n", r.callee.c_str(), r.formId, r.plugin.c_str());

FormIdMap map;                                           // EslMapLib format
load_formid_map(mapText, map);
PatchResult p = apply_formid_map(s, map);               // edits the model
write_file(s, "Foo.pex");                                // byte-exact apart from the edits

DecompileResult d = decompile(s);                        // d.source, d.functions (per-function report)
```

## Integrating

```cmake
include(FetchContent)
FetchContent_Declare(skqe_papyrus
    GIT_REPOSITORY https://github.com/cecell/skqe-papyrus-decompiler.git
    GIT_TAG <commit>)
FetchContent_MakeAvailable(skqe_papyrus)
target_link_libraries(your_target PRIVATE skqe::papyrus)
```

## The tool

```
pex info <input>                 header facts and counts (--json)
pex json <input> [--out <path>]  the whole model as JSON
pex asm <input> [--out <path>]   Papyrus assembly (PapyrusAssembler syntax)
pex assemble <listing> [--out <path>]
                                 the listing back to a .pex (our assembler)
pex compile <psc|dir> --flags <flg> [--import <dirs>] [--resolve <Data>] [--out <dir>]
                                 Papyrus source to .pex (our compiler)
pex decompile <input> [--out <path>] [--resolve <Data>]
                                 Papyrus source; --resolve lets it consult the
                                 other scripts (parent chains, declarations)
pex refs <input> [--csv]         FormID literals and their plugin names
pex deps <input> [--resolve <Data>] [--vanilla <Misc.bsa>]
pex audit <input> [--resolve <Data>] [--json|--csv] [--rules]
                                 risks for script authors, one line per finding
                                 (file, function, source line, rule, advice) and
                                 a summary by rule; docs/AUDIT.md
pex closure <script.pex> --data <Data> [--import <dirs>] [--emit <dir>]
pex patch <input> --map <FormIDMap> [--dry-run] [--backup]
pex check --rewrite <input>      read + write, compare byte for byte
pex check --asm <input> --tools "<game>\Papyrus Compiler"
pex check --assemble <input> [--tools "<game>\Papyrus Compiler"]
pex check --compile <SourceDir> --pex <Misc.bsa> --flags <flg> [--tools "<game>\Papyrus Compiler"]
pex check --roundtrip <input> --tools "<game>\Papyrus Compiler" [--resolve <Data>]
                                 [--per-script-imports]   (mixed sets: mods)
```

`<input>` is a `.pex`, a directory (every `.pex` below it, loose and inside
every `.bsa`) or one `.bsa`. Work is spread over all cores. `pex help` has
the details.

Speed (modded Data folder, 77,000 scripts, 32 threads, 2026-11-03): parsing
everything for `refs` 3.4 s; byte-exact rewrite check 1.3 s; listing,
assembling and comparing all 77,000 (`check --assemble`) 2.7 s; decompiling the
14,026 vanilla scripts to files 6.8 s, of which the decompiler itself is about
one millisecond per script single-threaded and the rest is creating 14,000
files (writing the same files as assembly takes 7.5 s); the dependency
closure of all 77,000 scripts (`closure`) 60 s single-threaded; auditing all
77,047 script files (`audit --all-copies`, 321,124 function bodies lifted and structured) 1.1 s,
the 14,026 vanilla ones 0.1 s (0.4 s on one thread). The checks
that run Bethesda's compiler take long because each script is a separate
compiler run (the whole folder: 72 minutes, of which decompiling is 2);
nothing in the normal workflow calls a compiler.

## Format notes that matter

[docs/PEX_FORMAT.md](docs/PEX_FORMAT.md), [docs/OPCODES.md](docs/OPCODES.md),
[docs/ASM_SYNTAX.md](docs/ASM_SYNTAX.md) (including what Bethesda's assembler
does to a listing) and
[docs/COMPILER_PATTERNS.md](docs/COMPILER_PATTERNS.md), the last being the
catalog of how Bethesda's compiler lowers each construct, found by probing
and by the round trip. It is also the specification a future compiler has to
meet. [docs/DESIGN.md](docs/DESIGN.md) explains the passes.

## Build

```
build.cmd [Debug|Release]
```

MSVC 14.44 (pinned by the script), Ninja, CMake 3.21+. Unit tests run as part
of the build. Elsewhere: `cmake -B build && cmake --build build && ctest --test-dir build`.

## Layout

```
include/papyrus/   pex.hpp (model, read/write, views), canon.hpp (comparison),
                   layout.hpp (source order), assemble.hpp (assembler),
                   compile.hpp (compiler), decompile.hpp, analysis.hpp,
                   audit.hpp (the script auditor)
src/               reader, writer, JSON, assembly writer and assembler, canon,
                   layout, analysis, audit (the rules), decompile/ (lift,
                   structure, psc_writer, event_names.inc/.hpp), compile/
                   (lexer, parser, interfaces, lowering, codegen)
tools/             pex.cpp (CLI), inputs.hpp, process.hpp, resolver.hpp,
                   roundtrip.hpp, gen_event_names.py, resaver/ (DumpStacks:
                   the Papyrus VM state of a save game, through ReSaver)
tests/             unit tests on synthetic scripts, fuzz target
probes/            Papyrus sources compiled to learn the compiler's patterns
docs/              format, assembly syntax, compiler catalog, design, round trip
WISHLIST.md        the request this project answers
```
