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
2. **Papyrus assembly** in the syntax `PapyrusAssembler.exe` reads. Every one
   of the 14,026 vanilla scripts assembles back to the identical model.
3. **Decompiler.** Bytecode to structured source: `If`/`ElseIf`/`Else`,
   `While`, `&&`/`||`, compiler temporaries folded away, implicit casts and
   omitted arguments restored, locals declared in their scopes. All 26,657
   vanilla functions and 156,994 of 156,995 in the modded folder structure;
   a function that does not is emitted as a stub with its assembly, never
   taking the file with it. The score that matters is the round trip:
   decompile, compile with `PapyrusCompiler.exe`, compare with the original
   at model level (docs/ROUNDTRIP.md).
4. **The analyses the ESL-compaction tooling needs**, with no compiler
   involved: `refs` lists every FormID literal paired with its plugin name,
   `patch --map` renumbers them in the compiled file, `closure` tells whether
   everything a script needs is present before anyone tries to compile it,
   `deps` says what a script needs an extender for.

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
pex decompile <input> [--out <path>] [--resolve <Data>]
                                 Papyrus source; --resolve lets it consult the
                                 other scripts (parent chains, declarations)
pex refs <input> [--csv]         FormID literals and their plugin names
pex deps <input> [--resolve <Data>] [--vanilla <Misc.bsa>]
pex closure <script.pex> --data <Data> [--import <dirs>] [--emit <dir>]
pex patch <input> --map <FormIDMap> [--dry-run] [--backup]
pex check --rewrite <input>      read + write, compare byte for byte
pex check --asm <input> --tools "<game>\Papyrus Compiler"
pex check --roundtrip <input> --tools "<game>\Papyrus Compiler" [--resolve <Data>]
```

`<input>` is a `.pex`, a directory (every `.pex` below it, loose and inside
every `.bsa`) or one `.bsa`. Work is spread over all cores; the whole Data
folder decompiles in under a minute. `pex help` has the details.

## Format notes that matter

[docs/PEX_FORMAT.md](docs/PEX_FORMAT.md), [docs/OPCODES.md](docs/OPCODES.md),
[docs/ASM_SYNTAX.md](docs/ASM_SYNTAX.md) and
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
                   layout.hpp (source order), decompile.hpp, analysis.hpp
src/               reader, writer, JSON, assembly, canon, layout, analysis,
                   decompile/ (lift, structure, psc_writer, event_names.inc)
tools/             pex.cpp (CLI), inputs.hpp, process.hpp, resolver.hpp,
                   roundtrip.hpp, gen_event_names.py
tests/             unit tests on synthetic scripts, fuzz target
probes/            Papyrus sources compiled to learn the compiler's patterns
docs/              format, assembly syntax, compiler catalog, design, round trip
WISHLIST.md        the request this project answers
```
