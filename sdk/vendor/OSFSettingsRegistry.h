#pragma once

#include "OSFSettings.h"

namespace OSFSettings::API
{
    // Every pointer is read-only and valid only during the RegistryFn callback.
    // Text is UTF-8, with size in bytes excluding the trailing NUL. Use size when copying.
    struct TextView
    {
        const char* data{};
        std::uint32_t size{};
    };

    enum class SettingType : std::uint32_t
    {
        Bool = 0,
        Int = 1,
        Float = 2,
        Enum = 3,
        Key = 4,
        String = 5
    };

    // The containing SettingView::type selects the active member, including for defaultValue. Enum and String use text; Key uses a native VK / kUnboundKey.
    union RegistryValue
    {
        bool boolean{};
        std::int64_t integer;
        double number;
        TextView text;
        std::uint32_t key;
    };

    struct EnumOptionView
    {
        TextView value;
        TextView label;
    };

    struct SettingView
    {
        TextView key;
        TextView label;
        TextView hint;
        SettingType type{};
        bool requiresRestart{};
        RegistryValue value;
        RegistryValue defaultValue;

        // Int/Float only. Read minimum/maximum only when its presence flag is true, using integer/number respectively. Other types have no bounds.
        bool hasMinimum{};
        bool hasMaximum{};
        RegistryValue minimum;
        RegistryValue maximum;
        double step{}; // Float editor increment; Int has an implicit step of 1.
        const EnumOptionView* options{}; // Enum only; authored order.
        std::uint32_t optionCount{};
        std::uint32_t maxLength{}; // String only; UTF-8 bytes, excluding NUL.
        bool allowUnbound{}; // Key only.
        bool allowMouse{}; // Key only; permits the five physical mouse buttons.
    };

    struct GroupView
    {
        TextView id;
        TextView label;
        const SettingView* settings{};
        std::uint32_t settingCount{};
    };

    struct ModView
    {
        TextView id;
        TextView title;
        TextView description;
        const GroupView* groups{};
        std::uint32_t groupCount{};
    };

    struct RegistryView
    {
        const ModView* mods{};
        std::uint32_t modCount{};
    };
}
