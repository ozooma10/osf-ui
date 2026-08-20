import '/shared/osfui.js';

const clicks = document.querySelector('#clicks');
const bump = document.querySelector('#bump');
const status = document.querySelector('#status');
let clickTotal = 0;

window.osfui.state.on('clicks', (value) => {
  clickTotal = Number(value) || 0;
  clicks.textContent = String(clickTotal);
});

window.osfui.on('notice', ({ args }) => {
  status.textContent = String(args?.[0] ?? 'Papyrus sent an event.');
});

bump.addEventListener('click', () => {
  window.osfui.papyrus.call('__OSFUI_SCRIPT_NAME__', 'Bump', clickTotal + 1);
});

window.osfui.papyrus.call('__OSFUI_SCRIPT_NAME__', 'Refresh');
