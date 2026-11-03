# skqe-papyrus-decompiler

A C++17 library and command line tool for compiled Papyrus scripts (`.pex`)
of Skyrim LE/SE. GPL-3.0-only. CMake target `skqe::papyrus`, namespace
`papyrus::pex`. No dependencies (the tool uses
[bsa-reader](https://github.com/cecell/bsa-reader) to read scripts out of
archives).

What it does, in the order it was built:

1. **Typed model, reader, writer.** A `.pex` becomes a `papyrus::pex::Script`
   (header, string table, debug info, user flags, objects with variables,
   properties, states, functions and their instructions). Writing the model
   back reproduces the file byte for byte: 77,031 of the 77,033 scripts in a
   modded Skyrim SE Data folder round-trip exactly; the other two are a
   truncated backup and a file that is not a script. Damaged files are
   refused with a sentence naming what was wrong and at which byte; nothing
   throws.
2. Assembly view, decompiler, round-trip corpus test, FormID reference
   report, dependency closure and in-place patching: in progress, see the
   docs.

## Use

```cpp
#include "papyrus/pex.hpp"
using namespace papyrus::pex;

Script s;
std::string err;
if (read_file("Foo.pex", s, &err) != Status::Ok) { /* err says why */ }
for (const Object& o : s.objects)
    for (const State& st : o.states)
        for (const NamedFunction& f : st.functions)
            printf("%s: %zu instructions\n", std::string(s.str(f.name)).c_str(), f.body.code.size());
std::vector<uint8_t> bytes;
write(s, bytes);   // == the input, byte for byte
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
pex info <input>                header facts and counts (--json)
pex json <input> [--out <path>] the whole model as JSON
pex check --rewrite <input>     read + write every script, compare byte for byte
```

`<input>` is a `.pex`, a directory (every `.pex` below it, loose and inside
every `.bsa`) or one `.bsa`. Work is spread over all cores; the whole Data
folder above takes under a second.

## Format notes that matter

See [docs/PEX_FORMAT.md](docs/PEX_FORMAT.md) and
[docs/OPCODES.md](docs/OPCODES.md). Two facts no other reader documents:
the object size field counts from itself, and some third-party compilers
write it little-endian without itself. The engine ignores the field; this
library accepts both and reproduces whichever it found.

## Build

```
build.cmd [Debug|Release]
```

MSVC 14.44 (pinned by the script), Ninja, CMake 3.21+. Unit tests run as part
of the build. Linux/other compilers: the usual `cmake -B build && cmake --build build && ctest --test-dir build`.

## Layout

```
include/papyrus/pex.hpp   the model and the read/write API
src/                      reader, writer, JSON view
tools/pex.cpp             the command line tool; tools/inputs.hpp enumerates inputs
tests/                    unit tests (synthetic scripts) and the fuzz target
docs/                     format, opcodes, design notes
WISHLIST.md               the request this project answers
```
