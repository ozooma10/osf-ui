#pragma once

namespace osfui::wv2
{
    // Check the focused page and its iframes for an open picker when Back is pressed.
    inline constexpr wchar_t kHasOpenSelectScript[] = LR"JS((() => {
        let root = document;
        while (root) {
            if (root.querySelector('select:open')) return true;
            const active = root.activeElement;
            root = active?.shadowRoot || active?.contentDocument || null;
        }
        return false;
    })())JS";
}
