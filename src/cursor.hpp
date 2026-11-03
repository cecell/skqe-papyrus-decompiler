// Bounded big-endian cursor over a byte buffer (reading) and a growing byte
// vector (writing). Every read reports failure instead of running off the end;
// the first failure's message and offset are kept for the caller.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace papyrus::pex::detail {

class Cursor {
public:
    Cursor(const uint8_t* data, size_t size) noexcept : m_data(data), m_size(size) {}

    size_t pos() const noexcept { return m_pos; }
    size_t remaining() const noexcept { return m_size - m_pos; }
    bool failed() const noexcept { return m_failed; }
    bool truncated() const noexcept { return m_truncated; }
    const std::string& error() const noexcept { return m_error; }

    // Record a structural error (bad value) at the current position.
    bool fail(const char* what) noexcept {
        if (!m_failed) {
            m_failed = true;
            m_truncated = false;
            m_error = what;
            append_offset();
        }
        return false;
    }
    bool fail(const std::string& what) noexcept { return fail(what.c_str()); }

    bool need(size_t n, const char* what) noexcept {
        if (m_failed) return false;
        if (n > remaining()) {
            m_failed = true;
            m_truncated = true;
            m_error = std::string("file ends inside ") + what;
            append_offset();
            return false;
        }
        return true;
    }

    bool u8(uint8_t& v, const char* what) noexcept {
        if (!need(1, what)) return false;
        v = m_data[m_pos++];
        return true;
    }
    bool u16(uint16_t& v, const char* what) noexcept {
        if (!need(2, what)) return false;
        v = static_cast<uint16_t>((m_data[m_pos] << 8) | m_data[m_pos + 1]);
        m_pos += 2;
        return true;
    }
    bool u32(uint32_t& v, const char* what) noexcept {
        if (!need(4, what)) return false;
        v = (uint32_t(m_data[m_pos]) << 24) | (uint32_t(m_data[m_pos + 1]) << 16) |
            (uint32_t(m_data[m_pos + 2]) << 8) | uint32_t(m_data[m_pos + 3]);
        m_pos += 4;
        return true;
    }
    bool u64(uint64_t& v, const char* what) noexcept {
        uint32_t hi, lo;
        if (!u32(hi, what) || !u32(lo, what)) return false;
        v = (uint64_t(hi) << 32) | lo;
        return true;
    }
    // u16 length + bytes.
    bool string(std::string& v, const char* what) noexcept {
        uint16_t len;
        if (!u16(len, what)) return false;
        if (!need(len, what)) return false;
        v.assign(reinterpret_cast<const char*>(m_data + m_pos), len);
        m_pos += len;
        return true;
    }

private:
    void append_offset() {
        char buf[48];
        std::snprintf(buf, sizeof buf, " (at byte %zu of %zu)", m_pos, m_size);
        m_error += buf;
    }

    const uint8_t* m_data;
    size_t m_size;
    size_t m_pos = 0;
    bool m_failed = false;
    bool m_truncated = false;
    std::string m_error;
};

class Writer {
public:
    explicit Writer(std::vector<uint8_t>& out) noexcept : m_out(out) {}

    size_t size() const noexcept { return m_out.size(); }

    void u8(uint8_t v) { m_out.push_back(v); }
    void u16(uint16_t v) {
        m_out.push_back(uint8_t(v >> 8));
        m_out.push_back(uint8_t(v));
    }
    void u32(uint32_t v) {
        m_out.push_back(uint8_t(v >> 24));
        m_out.push_back(uint8_t(v >> 16));
        m_out.push_back(uint8_t(v >> 8));
        m_out.push_back(uint8_t(v));
    }
    void u64(uint64_t v) {
        u32(uint32_t(v >> 32));
        u32(uint32_t(v));
    }
    // Caller has checked s.size() <= 0xFFFF.
    void string(std::string_view s) {
        u16(static_cast<uint16_t>(s.size()));
        m_out.insert(m_out.end(), s.begin(), s.end());
    }
    // Overwrite a u32 written earlier (object size back-patching).
    void patch_u32(size_t at, uint32_t v) noexcept {
        m_out[at] = uint8_t(v >> 24);
        m_out[at + 1] = uint8_t(v >> 16);
        m_out[at + 2] = uint8_t(v >> 8);
        m_out[at + 3] = uint8_t(v);
    }

private:
    std::vector<uint8_t>& m_out;
};

}  // namespace papyrus::pex::detail
