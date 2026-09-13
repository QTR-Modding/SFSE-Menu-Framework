#include "localization/Translations.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <map>
#include <string>
#include <string_view>

namespace SFSEMenuFramework::Translations
{
    namespace
    {
        // Adapts SKSE-MF Translations.cpp at 4c11522539153ceaa4076ac03403de55838ee0ca (GPL-3.0).
        std::map<std::string, std::string, std::less<>> entries;

        // Translated printf text must retain the exact argument types/order.
        std::string_view NextFormat(std::string_view& text)
        {
            while (!text.empty()) {
                const auto begin = text.find('%');
                if (begin == std::string_view::npos) { text = {}; return {}; }
                text.remove_prefix(begin);
                if (text.starts_with("%%")) { text.remove_prefix(2); continue; }
                std::size_t i = 1;
                while (i < text.size() && std::string_view{"-+ #0.*0123456789hljztL"}.find(text[i]) != std::string_view::npos) ++i;
                const auto token = text.substr(0, i + (i < text.size()));
                text.remove_prefix(token.size());
                return token;
            }
            return {};
        }

        bool SameFormats(std::string_view left, std::string_view right)
        {
            while (!left.empty() || !right.empty()) {
                if (NextFormat(left) != NextFormat(right)) return false;
            }
            return true;
        }
    }

    bool Load(const std::filesystem::path& path)
    {
        entries.clear();
        std::ifstream file{path};
        if (!file) return false;
        const auto data = nlohmann::json::parse(file, nullptr, false);
        if (!data.is_object()) {
            return false;
        }
        for (const auto& [key, value] : data.items()) {
            if (value.is_string()) {
                auto text = value.get<std::string>();
                // NUL truncates C strings; ImGui ID markers must remain code-owned.
                if (!text.empty() && text.find('\0') == std::string::npos && text.find("##") == std::string::npos)
                    entries.emplace(key, std::move(text));
            }
        }
        return true;
    }

    const char* Get(const char* key, const char* english)
    {
        if (std::string_view{key} == "Freeze time while menu is open") key = "Settings.FreezeTime";
        if (std::string_view{key} == "Blur background while menu is open") key = "Settings.BlurBackground";
        const auto it = entries.find(key);
        if (it == entries.end() || !SameFormats(it->second, english)) return english;
        return it->second.c_str();
    }
}
