#pragma once
#include <filesystem>

namespace SFSEMenuFramework::Translations
{
    // Load once before UI registration. Returned strings live for the process.
    [[nodiscard]] bool Load(const std::filesystem::path& path);
    [[nodiscard]] const char* Get(const char* key, const char* english);
}
