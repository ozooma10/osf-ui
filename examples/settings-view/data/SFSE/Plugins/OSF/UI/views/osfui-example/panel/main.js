"use strict";
osfui.state.on("showDetails", function (enabled) {
  document.getElementById("details").hidden = enabled !== true;
  document.getElementById("status").textContent = enabled === true ? "Details are on." : "Details are off.";
});
document.getElementById("close").addEventListener("click", function () { osfui.send("close"); });
document.getElementById("drag").addEventListener("input", function (event) {
  document.getElementById("dragValue").value = event.target.value;
});
document.addEventListener("keydown", function (event) {
  if (event.key === "Escape") { event.preventDefault(); osfui.send("close"); }
});
