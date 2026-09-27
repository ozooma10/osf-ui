(() => {
    // Native picker HWNDs are outside the visual captured by OSF UI. Keep real
    // single-line selects, but ask Chromium to paint their picker in the page.
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

    // Let Chromium dismiss the picker without also invoking an authored
    // page/game Escape handler. Do not cancel the browser's default action.
    window.addEventListener('keydown', event => {
        if (event.isTrusted && event.key === 'Escape' && document.querySelector('select:open')) {
            event.stopImmediatePropagation();
        }
    }, true);
})();
