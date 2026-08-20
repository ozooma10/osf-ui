"use strict";

const osfui = window.osfui;
const clicks = document.querySelector("#clicks");
const bump = document.querySelector("#bump");
const status = document.querySelector("#status");
let clickTotal = 0;

osfui.state.on("clicks", (value) => {
  clickTotal = Number(value) || 0;
  clicks.textContent = String(clickTotal);
});

osfui.on("notice", ({ args }) => {
  status.textContent = String(args?.[0] ?? "Papyrus sent an event.");
});

bump.addEventListener("click", () => {
  osfui.send("papyrus.call", {
    script: "__OSFUI_SCRIPT_NAME__",
    function: "Bump",
    args: [clickTotal + 1],
  });
});

osfui.send("papyrus.call", {
  script: "__OSFUI_SCRIPT_NAME__",
  function: "Refresh",
  args: [],
});
