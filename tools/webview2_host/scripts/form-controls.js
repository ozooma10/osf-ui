(() => {
    // Native select popups are not captured; draw single-line select pickers inside the page.
    if (!CSS.supports('appearance', 'base-select')) {
        console.error('OSF UI: in-page select controls require WebView2 135 or newer.');
        return;
    }
    const sheet = new CSSStyleSheet();
    sheet.replaceSync(`
        :where(select:not([multiple]):is(:not([size]), [size="0"], [size="1"])),
        :where(select:not([multiple]):is(:not([size]), [size="0"], [size="1"]))::picker(select) {
            appearance: base-select !important;
        }
    `);
    document.adoptedStyleSheets.push(sheet);

    // Escape dismisses the picker only; keep the browser default but hide it from page/game handlers.
    window.addEventListener('keydown', event => {
        if (event.isTrusted && event.key === 'Escape' && document.querySelector('select:open')) {
            event.stopImmediatePropagation();
        }
    }, true);
})();
