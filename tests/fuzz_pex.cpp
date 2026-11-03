// libFuzzer entry: the reader and writer must never crash, overrun or
// allocate what a hostile file claims. Build with -DSKQE_PAPYRUS_FUZZ=ON.
// SPDX-License-Identifier: GPL-3.0-only
#include "papyrus/pex.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string err;
    papyrus::pex::rewrite_check(data, size, &err);
    papyrus::pex::Script s;
    if (papyrus::pex::read(data, size, s, &err) == papyrus::pex::Status::Ok) {
        (void)papyrus::pex::to_json(s, false);
    }
    return 0;
}
