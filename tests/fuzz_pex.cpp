// libFuzzer entry: the reader, writer and assembler must never crash, overrun
// or allocate what a hostile input claims. Build with -DSKQE_PAPYRUS_FUZZ=ON.
// SPDX-License-Identifier: GPL-3.0-only
#include "papyrus/assemble.hpp"
#include "papyrus/pex.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string err;
    papyrus::pex::rewrite_check(data, size, &err);
    papyrus::pex::Script s;
    if (papyrus::pex::read(data, size, s, &err) == papyrus::pex::Status::Ok) {
        (void)papyrus::pex::to_json(s, false);
    }
    // The same bytes as a listing: whatever the assembler accepts must write.
    papyrus::pex::Script a;
    if (papyrus::pex::assemble(std::string_view(reinterpret_cast<const char*>(data), size), a, &err) ==
        papyrus::pex::Status::Ok) {
        std::vector<uint8_t> out;
        (void)papyrus::pex::write(a, out, &err);
    }
    return 0;
}
