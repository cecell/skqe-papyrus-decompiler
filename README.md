# skqe-papyrus-decompiler

A C++17 library and command line tool for compiled Papyrus scripts (`.pex`)
of Skyrim LE/SE. GPL-3.0-only. CMake target `skqe::papyrus`, namespace
`papyrus::pex`. The library has no dependencies; the tool uses
[bsa-reader](https://github.com/cecell/bsa-reader) to read scripts out of
archives and the library of [Skyrim Query Engine](https://github.com/cecell/skyrim-query-engine)
for the load order (which archives the game loads, for `patch --out` and `winners`).

Everything in it is checked against Bethesda's own tools over every compiled
script on a modded machine, and the numbers are below. What it does:

1. **Typed model, reader, byte-exact writer.** A `.pex` becomes a
   `papyrus::pex::Script` (header, string table, debug info, user flags,
   objects with variables, properties, states, functions, instructions).
   Writing it back reproduces the file byte for byte: 77,064 of 77,066
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
   at model level: **14,024 of the 14,026 vanilla scripts (99.99%) come back
   identical**, and over the whole modded folder **75,125 of 76,593 (98.08%;
   99.0% of those whose dependencies exist at all)**.
4. **A compiler** for Skyrim's Papyrus (`pex compile`), proved by compiling
   every vanilla source and comparing with the shipped compiled file at
   model level: **13,711 of 13,975 (98.1%) come back identical**, in 2.9 s
   for the lot, and **every one of the scripts Bethesda's compiler can build
   from those sources** (the shipped sources have drifted for 131, 133 lack
   a dependency's source, none differs). Over the two mod source folders of
   the modded `Data` the same holds: 6,101 of 6,883 and 9,373 of 10,168
   identical, none differing from what `PapyrusCompiler.exe` produces today.
   The header of a compiled script names its source file, a user and a
   computer; Bethesda's compiler writes the path it was given and the
   Windows account and machine name there. `pex compile` writes the file
   name alone and `skqe` / `papyrus`, or whatever `--user` and `--computer`
   say. `pex recompile` sets the same three fields in scripts that already
   exist, yours or anyone's, without touching anything else in them.
5. **The analyses the ESL-compaction tooling needs**, with no compiler
   involved: `refs` lists every FormID literal paired with its plugin name,
   `patch --map` renumbers them in the compiled file, `closure` tells whether
   everything a script needs is present before anyone tries to compile it,
   `deps` says what a script needs an extender for.
6. **A script auditor** (`pex audit`): twenty-three rules over the decompiler's
   view of a script that tell authors where it can misbehave at run time,
   in two groups. Threads that never end or pile up: loops that cannot end,
   spin without a `Wait`, or poll another object's state with no bound; update
   timers never unregistered or re-armed without a stop; latent work and
   linked-reference calls in events that fire for every object in a cell;
   unfiltered inventory handlers; events calling into one quest per actor or
   item; recursion. Wrong results: calls on values that can be None
   (`GetFormFromFile` first of all), arrays used before they exist,
   off-by-one array loops, `GotoState` to a state that does not exist,
   conditions that can never be true or are always true (`x == 1 && x == 2`,
   `x != 1 || x != 2`, an `ElseIf` an earlier test already covers), code after
   a `Return`, properties nothing reads or writes, states nothing enters, and,
   with `--fills`, object properties a script reads that no plugin attaching
   it fills (read from every active plugin's script data). The run-time
   rules are grounded in a corpus of 3,783 community save games read with
   ReSaver (`tools/resaver`) and in the author's own saves and Papyrus logs.
   Measured on the 14,026 vanilla scripts: 1,269 findings from the original
   seventeen rules; the new ones find 8 condition and dead-code mistakes in
   Bethesda's scripts (all eight confirmed in the sources: two dead `ElseIf`s
   in the Companions' reward code, an always-true angle test in
   `SlowdownStrikeScript`, code after a `Return` in four scripts), 13 states
   never entered, 1,963 unused properties and, with `--fills`, 243 properties
   read but never filled. The whole modded folder (73,728 distinct scripts,
   2,424 plugins) audits in 8.4 s with `--fills`. `pex audit --rules` lists
   the rules.
7. **A semantic diff** (`pex diff`): what changed between two versions of a
   script, in plain English and with no AI involved: declarations (a property
   added, a parameter lost, a return type changed), then each function's
   statements aligned (added, removed, changed, with the new version's source
   line), with the meaning named where the trees allow it (a literal, FormID
   or plugin name that changed, a condition that gained a term, a call or
   registration added, an `If` wrapped around existing code, a local renamed
   rather than changed). Measured on every vanilla script the Unofficial
   Patch edits: 1,420 scripts, 1,225 changed, 5,319 sentences (median 2 per
   script), 0.1 s; and on 308 pairs of mod scripts from the Vortex staging
   folder.

## Download

Get `pex-<version>.zip` from [Nexus Mods](https://www.nexusmods.com/skyrimspecialedition/mods/198554)
or the [releases page](https://github.com/cecell/skqe-papyrus-decompiler/releases), unzip it anywhere
and run `pex.exe` from a command prompt. `pex help` lists the commands. It needs no installer, no
runtime and no Creation Kit.

To decompile a script without a command prompt, open the `.pex` with `pex.exe` (right-click,
Open with, or drag the file onto the exe): the source lands next to it as `<name>.psc`, and it
asks before overwriting one that exists.

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
pex compile <psc|dir> --flags <flg> [--import <dirs>] [--resolve <Data>] [--out <dir>] [--user <name>] [--computer <name>]
                                 Papyrus source to .pex (our compiler)
pex decompile <input> [--out <path>] [--resolve <Data>]
                                 Papyrus source; --resolve lets it consult the
                                 other scripts (parent chains, declarations)
pex refs [<input>] [--csv] [--plugin <Name.esp>] [--out <folder>]
                                 FormID literals and their plugin names; with no
                                 input, only the copy of each script the game loads,
                                 with a hash of it; --plugin keeps the call sites
                                 naming that plugin and prints a count to stderr
pex deps <input> [--resolve <Data>] [--vanilla <Misc.bsa>]
pex diff <old> <new> [--resolve <Data>] [--json] [--quiet]
                                 what changed between two versions of a script
                                 (two files, or two folders or archives matched
                                 by name), one sentence per change
pex audit <input> [--resolve <Data>] [--json|--csv] [--rules] [--fills]
                                 risks for script authors, one line per finding
                                 (file, function, source line, rule, advice) and
                                 a summary by rule; --fills reads the load order's
                                 plugins for properties no attachment fills
pex closure <script.pex> --data <Data> [--import <dirs>] [--emit <dir>]
pex patch <input> --plugin <Name.esp> --formid OLD:NEW[,OLD:NEW] [--formids <file>]
                  [--log <file>] [--dry-run] [--backup]
pex patch <input> --map <FormIDMap> [--log <file>] [--dry-run] [--backup]
                                 scans a file, folder or archive and renumbers
                                 FormID literals, loose and inside archives: 4
                                 bytes per literal overwritten in place, journaled,
                                 read back; a compressed member is extracted,
                                 patched, to Scripts\ beside its archive; refused
                                 if another archive there has a different copy, or
                                 for a shared data block or a rename in an archive;
                                 a log by archive and folder. Refuses to write
                                 while the game runs, with no override
pex patch --undo <journal> [--dry-run]
                                 restores every byte a journal's patches changed
                                 and deletes what they extracted; a run writes one
                                 journal per archive and folder
pex patch --out <folder> --plugin <Name.esp> --formid OLD:NEW [--update]
                                 changes nothing in Data; --update overwrites a
                                 copy whose winner has changed: each script's one winning
                                 copy, patched, goes to <folder>\scripts\ to install
                                 as a mod. Finds the game (Steam, else GOG) through
                                 the registry and plugins.txt in %LOCALAPPDATA%, or
                                 refuses and says why. Output copy, then loose, then the last loaded
                                 archive; the mode for a whole setup
pex winners [<Data>] [--load-order <plugins.txt>] [--all-plugins]
                                 the copy of each script the game loads, as CSV
pex recompile <input> [--user <name>] [--computer <name>] [--source-filename] [--out <dir>] [--dry-run]
                                 rewrites the header fields that say who built a
                                 script and from where, in every script of the input;
                                 nothing else changes. In place for loose files (not
                                 while the game runs), to --out for archive members
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

`pex help` (or `pex` alone) lists the ops, each with its main usage and one sentence. `pex <op>
--help` prints that op's full help with examples (an op that needs an input also prints it when
given none), and `pex help --all` the full text of every op. `--json` prints any of them as one
line of JSON: `pex help --json` for every op, `pex <op> --help --json` for one, each with its
summary, examples, usage forms, descriptions, notes, common options and exit codes.

Speed (modded Data folder, 77,000 scripts, 32 threads, 2026-11-03 unless
said): decompiling the 67,937 scripts of a Vortex staging folder, loose and
in archives, 27 s (35 s with the 67,913 source files written; 2026-11-09);
compiling 21,463 of the 23,144 sources of a game's `Source\Scripts` to disk
57 s, and `check --compile` over the 13,975 vanilla sources 3.2 s (2026-11-09);
parsing
everything for `refs` 3.4 s; byte-exact rewrite check 1.3 s; listing,
assembling and comparing all 77,000 (`check --assemble`) 2.7 s; decompiling the
14,026 vanilla scripts to files 6.8 s, of which the decompiler itself is about
one millisecond per script single-threaded and the rest is creating 14,000
files (writing the same files as assembly takes 7.5 s); the dependency
closure of all 77,000 scripts (`closure`) 60 s single-threaded; auditing all
77,047 script files (`audit --all-copies`, 321,124 function bodies lifted and structured) 1.1 s,
the 14,026 vanilla ones 0.1 s (0.4 s on one thread); the semantic diff of the
1,420 vanilla scripts the Unofficial Patch ships against their originals
(`diff`) 0.1 s (0.8 s on one thread). The checks
that run Bethesda's compiler take long because each script is a separate
compiler run (the whole folder: 72 minutes, of which decompiling is 2);
nothing in the normal workflow calls a compiler.

## Build

```
build.cmd [Debug|Release]
```

MSVC 14.44 (pinned by the script), Ninja, CMake 3.21+. Unit tests run as part
of the build. Elsewhere: `cmake -B build && cmake --build build && ctest --test-dir build`.

## Releases

A version tag runs `.github/workflows/release.yml`, which builds, tests, packages, hashes and scans on
GitHub's runners, publishes the release, and uploads the same zip to Nexus Mods. To release:

1. Set the version in `CMakeLists.txt` and `kVersion` in `include/papyrus/pex.hpp` (the build refuses
   to configure if they differ).
2. Add `release_notes/<version>.md`. Its first line and `## ` sections become the Nexus changelog.
3. Run the corpus checks (`pex check --rewrite`, `--assemble`, `--compile`, `--roundtrip`) locally; they
   need the game.
4. Push to main, then tag: `git tag v<version>` and `git push origin v<version>`.

To check a download, compare it with `SHA256SUMS.txt` on the release: `sha256sum -c SHA256SUMS.txt`,
or `Get-FileHash <file>` in PowerShell. The release notes on GitHub link each file's VirusTotal report.
Credits and licences of included work are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

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
tools/             pex.cpp (CLI), archive_patch.cpp (patching inside archives,
                   journals, undo), out_patch.cpp (--out folder, load order,
                   winners), inputs.hpp, process.hpp, resolver.hpp,
                   roundtrip.hpp, gen_event_names.py, resaver/ (DumpStacks:
                   the Papyrus VM state of a save game, through ReSaver)
tests/             unit tests on synthetic scripts, fuzz target
probes/            Papyrus sources compiled to learn the compiler's patterns
```
