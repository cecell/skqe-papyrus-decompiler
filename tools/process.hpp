// Runs an external program with a working directory and captures its output.
// Used for the official PapyrusAssembler / PapyrusCompiler in the checks.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstdio>
#endif

namespace pextool {

struct RunResult {
    bool started = false;
    int exitCode = -1;
    std::string output;  // stdout and stderr, interleaved
};

#ifdef _WIN32
inline std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

inline std::wstring quote_arg(const std::wstring& a) {
    if (a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    for (wchar_t c : a) {
        if (c == L'"') out += L"\\\"";
        else out += c;
    }
    return out + L"\"";
}

inline RunResult run(const std::filesystem::path& exe, const std::vector<std::string>& args,
                     const std::filesystem::path& cwd) {
    RunResult r;
    std::wstring cmd = quote_arg(exe.wstring());
    for (const std::string& a : args) cmd += L" " + quote_arg(widen(a));

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return r;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    const std::wstring wcwd = cwd.wstring();
    const BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
    CloseHandle(writeEnd);
    if (!ok) {
        CloseHandle(readEnd);
        return r;
    }
    r.started = true;
    char chunk[4096];
    DWORD n = 0;
    while (ReadFile(readEnd, chunk, sizeof chunk, &n, nullptr) && n > 0) r.output.append(chunk, n);
    CloseHandle(readEnd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    r.exitCode = int(code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return r;
}
#else
inline RunResult run(const std::filesystem::path& exe, const std::vector<std::string>& args,
                     const std::filesystem::path& cwd) {
    RunResult r;
    std::string cmd = "cd '" + cwd.string() + "' && '" + exe.string() + "'";
    for (const std::string& a : args) cmd += " '" + a + "'";
    cmd += " 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return r;
    r.started = true;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, p)) > 0) r.output.append(chunk, n);
    r.exitCode = pclose(p);
    return r;
}
#endif

}  // namespace pextool
