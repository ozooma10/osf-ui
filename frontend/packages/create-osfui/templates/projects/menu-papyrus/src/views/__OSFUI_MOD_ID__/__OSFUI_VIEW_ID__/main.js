import '/shared/osfui.js';

const clicks = document.querySelector('#clicks');
const bump = document.querySelector('#bump');
let clickTotal = 0;

window.osfui.state.on('__OSFUI_MOD_ID_SQ__/clicks', (value) => {
  clickTotal = Number(value) || 0;
  clicks.textContent = String(clickTotal);
});

bump.addEventListener('click', () => {
  window.osfui.papyrus.call('__OSFUI_SCRIPT_NAME__', 'Bump', clickTotal + 1);
});

window.osfui.papyrus.call('__OSFUI_SCRIPT_NAME__', 'Refresh');
