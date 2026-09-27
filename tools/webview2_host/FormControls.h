#pragma once

namespace osfui::wv2
{
    // Query at the time of Back, rather than mirroring popup-open state across
    // processes. A focused same-origin iframe owns its own select picker.
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
