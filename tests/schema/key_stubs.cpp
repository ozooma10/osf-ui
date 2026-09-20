// These fixtures contain booleans and native hotkey declarations, not key-valued settings.
// Abort if their validation starts depending on the game's key-name table.
#include "Input/KeyNames.h"
#include <cstdlib>
namespace OSFSettings
{
    bool IsBindableKey(std::uint32_t) { std::abort(); }
    std::optional<std::uint32_t> KeyCodeFromName(std::string_view) { std::abort(); }
}
