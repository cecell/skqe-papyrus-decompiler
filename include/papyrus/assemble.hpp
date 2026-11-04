// The assembler: a Papyrus assembly listing (docs/ASM_SYNTAX.md, the form
// `pex asm` writes and PapyrusCompiler -keepasm writes) becomes a Script,
// which the byte-exact writer then serializes. It mirrors PapyrusAssembler:
// strings are interned case-insensitively with the first spelling winning,
// debug records are created from the ";@line" comments with the reference
// tool's labelling and merging quirks, so that the result equals what
// PapyrusAssembler produces from the same listing at model level.
//
// Nothing throws. A listing that cannot be read gives Status::Syntax and an
// error of the form "line N: <what was wrong>".
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "papyrus/pex.hpp"

#include <string>
#include <string_view>

namespace papyrus::pex {

Status assemble(std::string_view listing, Script& out, std::string* error = nullptr) noexcept;

}  // namespace papyrus::pex
