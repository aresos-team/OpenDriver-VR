#pragma once
// ============================================================================
// CROSS-PLATFORM DYNAMIC LIBRARY LOADING
// Replaces raw dlopen/dlclose/dlsym and LoadLibrary/FreeLibrary/GetProcAddress
// ============================================================================
#include <opendriver/core/platform.h>
#include <string>

namespace opendriver::core {

#if defined(OD_PLATFORM_WINDOWS)
    using LibHandle = HMODULE;
    constexpr LibHandle kNullHandle = nullptr;

    inline LibHandle DynOpen(const std::string& path) {
        return ::LoadLibraryA(path.c_str());
    }
    inline void DynClose(LibHandle h) {
        if (h) ::FreeLibrary(h);
    }
    inline void* DynSym(LibHandle h, const std::string& name) {
        return reinterpret_cast<void*>(::GetProcAddress(h, name.c_str()));
    }
    inline std::string DynError() {
        DWORD err = ::GetLastError();
        char buf[256] = {};
        ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, err, 0, buf, sizeof(buf), nullptr);
        return std::string(buf);
    }
#else
    #include <dlfcn.h>
    using LibHandle = void*;
    constexpr LibHandle kNullHandle = nullptr;

    inline LibHandle DynOpen(const std::string& path) {
        return dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    }
    inline void DynClose(LibHandle h) {
        if (h) dlclose(h);
    }
    inline void* DynSym(LibHandle h, const std::string& name) {
        return dlsym(h, name.c_str());
    }
    inline std::string DynError() {
        const char* e = dlerror();
        return e ? std::string(e) : "(unknown)";
    }
#endif

} // namespace opendriver::core
