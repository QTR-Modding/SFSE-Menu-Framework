#include "localization/Translations.h"
#include <fstream>
#include <iostream>
#include <string_view>

int main()
{
    namespace T = SFSEMenuFramework::Translations;
    const auto path = std::filesystem::temp_directory_path() / "sfsemf-translations-test.json";
    const auto check = [](bool condition) { if (!condition) throw "Translation test failed"; };
    { std::ofstream file{path}; file << R"({"Options":"Opciones","Bad":2,"Null":"a\u0000b","ID":"text###id","Count":"Cuenta %d","Unsafe":"%n"})"; }
    check(T::Load(path));
    check(std::string_view{T::Get("Options", "Options")} == "Opciones");
    for (const auto* key : {"Missing", "Bad", "Null", "ID", "Unsafe"})
        check(std::string_view{T::Get(key, "English")} == "English");
    check(std::string_view{T::Get("Count", "Count %d")} == "Cuenta %d");
    check(std::string_view{T::Get("Count", "Count %s")} == "Count %s");
    { std::ofstream file{path}; file << "{bad"; }
    check(!T::Load(path));
    check(std::string_view{T::Get("Options", "Options")} == "Options");
    std::filesystem::remove(path);
    check(!T::Load(path));
    std::cout << "Translation tests passed\n";
}
