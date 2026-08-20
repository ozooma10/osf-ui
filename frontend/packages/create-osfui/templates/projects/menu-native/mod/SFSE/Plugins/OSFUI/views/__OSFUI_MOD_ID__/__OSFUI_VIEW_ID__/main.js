"use strict";

const osfui = window.osfui;
const count = document.querySelector("#count");
const lastAction = document.querySelector("#last-action");
const increment = document.querySelector("#increment");
const close = document.querySelector("#close");
const form = document.querySelector("#greeting");
const name = document.querySelector("#name");
const status = document.querySelector("#status");

function showState(state) {
  count.textContent = String(state.count);
  lastAction.textContent = state.lastAction;
  increment.disabled = !state.enabled;
  status.textContent = "State received from OSF UI";
}

function describe(error) {
  if (!(error instanceof Error)) return String(error);
  return error.code ? error.code + ": " + error.message : error.message;
}

// Retained state is replayed immediately and after every document reload.
osfui.state.on("state", showState);

// Events are one-shot happenings and are never replayed.
osfui.on("notice", (payload) => {
  status.textContent = payload.message;
});

increment.addEventListener("click", () => {
  if (!osfui.send("increment", { amount: 1 })) {
    status.textContent = "OSF UI bridge is unavailable";
  }
});

form.addEventListener("submit", async (event) => {
  event.preventDefault();
  try {
    const reply = await osfui.request("greet", { name: name.value });
    status.textContent = reply.message;
  } catch (error) {
    status.textContent = describe(error);
  }
});

close.addEventListener("click", () => osfui.send("close"));
