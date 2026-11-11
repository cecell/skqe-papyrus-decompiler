// Which script properties the plugins of a load order fill: the VMAD of every
// record of every active plugin, read with Skyrim Query Engine's catalog, for
// the auditor's property-unfilled rule (`pex audit --fills`).
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "papyrus/audit.hpp"

#include <cstddef>
#include <filesystem>
#include <string>

namespace pextool {

struct FillsReport {
    size_t plugins = 0;      // plugins read
    size_t attachments = 0;  // script entries found in VMADs
    size_t scripts = 0;      // distinct scripts attached somewhere
};

// Reads the load order (`pluginsTxt` against `data`) and records in
// `index.filled` every property some attachment of each script fills; sets
// index.fillsKnown. `patterns` is skqe's patterns.json. False with `why` set
// when the load order or the catalog cannot be read; a plugin that cannot be
// opened is skipped and named in `why` while the rest still counts.
bool load_vmad_fills(const std::filesystem::path& data, const std::filesystem::path& pluginsTxt, const std::filesystem::path& patterns,
                     unsigned threads, papyrus::pex::AuditIndex& index, FillsReport& report, std::string& why);

}  // namespace pextool
