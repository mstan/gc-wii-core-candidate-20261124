// SPDX-License-Identifier: MIT
#pragma once

#include <sqlite3.h>
#ifdef _WIN32
#include <filesystem>
#endif

namespace gcnllr {
inline int open_shader_cache(const char* path, sqlite3** database) {
#ifdef _WIN32
    if (sqlite3_vfs_find("win32-longpath")) {
        std::error_code error;
        auto absolute = std::filesystem::absolute(
            std::filesystem::path{reinterpret_cast<const char8_t*>(path)}, error);
        if (error)
            return SQLITE_CANTOPEN;
        auto name = absolute.lexically_normal().make_preferred().u8string();
        if (!name.starts_with(u8R"(\\?\)") && !name.starts_with(u8R"(\\.\)")) {
            name = name.starts_with(u8R"(\\)")
                ? u8R"(\\?\UNC\)" + name.substr(2)
                : u8R"(\\?\)" + name;
        }
        return sqlite3_open_v2(reinterpret_cast<const char*>(name.c_str()), database,
                               SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "win32-longpath");
    }
#endif
    return sqlite3_open_v2(path, database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
}
}
