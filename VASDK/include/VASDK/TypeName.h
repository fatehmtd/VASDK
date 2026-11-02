#pragma once

#include <cstring>
#include <string>
#include <typeinfo>
#if defined(__GNUG__)
#include <cstdlib>
#include <cxxabi.h>
#endif

namespace va {

/**
 * @brief Class name without namespaces or template arguments ("SileroVAD" for va::audio::SileroVAD),
 *        on GCC, Clang and MSVC. The default name providers report to logs and UIs.
 */
inline std::string shortTypeName(const std::type_info& type) {
    std::string name = type.name();
#if defined(__GNUG__)
    int status = 0;
    if (char* demangled = abi::__cxa_demangle(name.c_str(), nullptr, nullptr, &status)) {
        if (status == 0) name = demangled;
        std::free(demangled);
    }
#endif
    for (const char* prefix : {"class ", "struct "}) {  // MSVC: "class va::audio::SileroVAD"
        if (name.rfind(prefix, 0) == 0) name.erase(0, std::strlen(prefix));
    }
    if (const auto angle = name.find('<'); angle != std::string::npos) name.erase(angle);
    if (const auto colon = name.rfind("::"); colon != std::string::npos) name.erase(0, colon + 2);
    return name;
}

} // namespace va
