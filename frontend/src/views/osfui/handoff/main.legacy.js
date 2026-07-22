"use strict";

(function () {
  const root = document.documentElement;
  const body = document.body;
  const title = document.getElementById("title");
  const owner = document.getElementById("owner");
  const target = document.getElementById("target");
  const channel = document.getElementById("channel");
  const status = document.getElementById("status");
  const detail = document.getElementById("detail");
  const actions = document.getElementById("actions");
  const retry = document.getElementById("retry");
  const close = document.getElementById("close");

  const copy = {
    linking: {
      status: "ESTABLISHING LOCAL LINK",
      detail: "Negotiating display surface and local data channels."
    },
    retrying: {
      status: "SIGNAL INTERRUPTED // REACQUIRING",
      detail: "The interface carrier dropped. Automatic relink is in progress."
    },
    error: {
      status: "LINK FAILED // INTERFACE UNAVAILABLE",
      detail: "The local endpoint did not answer. Retry the link or return to the world."
    }
  };

  function setState(payload) {
    const p = payload || {};
    const phase = Object.prototype.hasOwnProperty.call(copy, p.phase) ? p.phase : "linking";
    const text = copy[phase];
    title.textContent = String(p.title || "INTERFACE").toUpperCase();
    owner.textContent = String(p.mod || "LOCAL SYSTEM").replace(/[._-]+/g, " ").toUpperCase();
    target.textContent = String(p.target || "UNRESOLVED").toUpperCase();
    channel.textContent = "OSF-LINK " + checksum(String(p.target || ""));
    status.textContent = text.status;
    detail.textContent = text.detail;
    actions.hidden = !p.retry;
    body.dataset.phase = phase;
    body.dataset.live = "true";
    window.osfui.applyAccent(root, p.accent);
    if (p.retry) retry.focus();
  }

  function checksum(value) {
    let n = 0;
    for (let i = 0; i < value.length; ++i) n = (n + value.charCodeAt(i) * (i + 1)) % 100;
    return String(n).padStart(2, "0");
  }

  window.osfui.on("handoff.state", setState);
  retry.addEventListener("click", () => window.osfui.send("osfui.handoffRetry"));
  close.addEventListener("click", () => window.osfui.send("close"));
  window.addEventListener("keydown", (event) => {
    if (event.key === "Escape") window.osfui.send("close");
  });
}());
