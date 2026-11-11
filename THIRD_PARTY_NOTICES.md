# Credits and third-party notices

`pex` and the `skqe::papyrus` library are licensed under the GNU General Public License v3.0
([`LICENSE`](LICENSE)). The library has no dependencies and contains no one else's code. `pex.exe`
also includes the work below, whose authors keep their copyright and whose terms still apply to it.

## Skyrim Query Engine, and through it xEdit (TES5Edit / SSEEdit)

`pex.exe` links the library of [Skyrim Query Engine](https://github.com/cecell/skyrim-query-engine)
(by the author of `pex`, GPL-3.0) to read the load order and tell which archives the game loads. Parts
of that library come from xEdit's definitions (`Core/wbDefinitionsTES5.pas`,
`Core/wbDefinitionsCommon.pas`): condition function names and parameter types, the subrecords xEdit
labels unused, the rules for comparing records, and xEdit's names for subrecords.

`pex.exe` carries Skyrim Query Engine's catalog of plugin record layouts (`patterns.json`, built in),
which `pex audit --fills` reads plugins with. Its `_display` and `_names` sections hold xEdit's names
for subrecords and remain available under the MPL 2.0.

xEdit is by ElminsterAU, with updates since 2012 by Hlp, Zilav and Sharlikran.
Source: <https://github.com/TES5Edit/TES5Edit>. Licence: Mozilla Public License 2.0, a copy of which is
in [`LICENSES/MPL-2.0.txt`](LICENSES/MPL-2.0.txt). No xEdit source file is included, and those parts
remain available under the MPL 2.0 from the Skyrim Query Engine source linked above.

## puff (the inflate in bsa-reader and Skyrim Query Engine)

The zlib decoder in both libraries is altered from `contrib/puff/puff.c` in the zlib distribution:
restructured as C++, with bounds checks, an output limit and the zlib wrapper added. It is not the
original puff.

```
Copyright (C) 2002-2013 Mark Adler, all rights reserved
version 2.3, 21 Jan 2013

This software is provided 'as-is', without any express or implied
warranty.  In no event will the author be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.

Mark Adler    madler@alumni.caltech.edu
```

## SQLite

Skyrim Query Engine's library carries the SQLite 3.54.1 amalgamation, by D. Richard Hipp and the
SQLite developers (<https://sqlite.org>). It is in the public domain.

## Companion libraries

By the author of `pex`, under GPL-3.0, and compiled into `pex.exe`:

- [bsa-reader](https://github.com/cecell/bsa-reader): reads scripts out of BSA archives.
- [Skyrim Query Engine](https://github.com/cecell/skyrim-query-engine): the load order.
- [beth-asset-metadata](https://github.com/cecell/beth-asset-metadata): part of Skyrim Query Engine's
  library.

## Credits, with no code included

- **Champollion** by Orvid and contributors (<https://github.com/Orvid/Champollion>, LGPL-3.0): the
  decompiler the community has relied on for years, and the inspiration for this one. Nothing was
  copied from it.
- **FallrimTools ReSaver** by Mark Fairchild (Apache-2.0; the copy used here is
  <https://github.com/AI-Underdog/FallrimTools-Re-Saver>): `tools/resaver/DumpStacks.java` in the source drives its save-game parser to print what
  the scripts in a save were doing. ReSaver itself isn't included, and that tool isn't in the release
  zip.
- **Bethesda's Papyrus compiler and assembler**: every rule `pex` follows is checked against what those
  tools produce, over every script of the game. None of their code is included.

## Trademarks

The Elder Scrolls, Skyrim and Creation Kit are trademarks of ZeniMax Media Inc. or its affiliates. This
project isn't affiliated with or endorsed by Bethesda Softworks or ZeniMax Media, and it includes no
game files.
