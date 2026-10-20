"use strict";

// Optional genuine extension keyboard invocation. Native ExecuteScript calls to
// collector.startCapture intentionally do not pretend to grant activeTab.
chrome.commands.onCommand.addListener(async (command, tab) => {
  if (command !== "start-source-capture") return;
  try {
    const defaultSourceUrl = "https://osfui-tab-audio-source.test/source.html";
    const allowedSourceUrls = new Set([
      defaultSourceUrl,
      "https://media.w3.org/2010/05/sintel/trailer.mp4",
    ]);
    if (!tab || !allowedSourceUrls.has(tab.url) || !Number.isInteger(tab.id)) {
      throw new Error("The extension command must be invoked on an exact permitted source URL");
    }
    const collectorUrl = chrome.runtime.getURL("collector.html");
    const collectorBase = new URL(collectorUrl);
    const collectors = (await chrome.tabs.query({})).filter(item => {
      if (typeof item.url !== "string") return false;
      const candidate = new URL(item.url);
      return candidate.protocol === collectorBase.protocol &&
        candidate.host === collectorBase.host &&
        candidate.pathname === "/collector.html" &&
        (candidate.searchParams.get("source") ?? defaultSourceUrl) === tab.url;
    });
    if (collectors.length !== 1 || !Number.isInteger(collectors[0].id)) {
      throw new Error("Expected one collector extension tab with an integer ID");
    }
    const streamId = await chrome.tabCapture.getMediaStreamId({
      targetTabId: tab.id,
      consumerTabId: collectors[0].id,
    });
    await chrome.runtime.sendMessage({
      kind: "osfui-tab-audio-command-stream",
      streamId,
      targetTabId: tab.id,
      sourceUrl: tab.url,
    });
  } catch (failure) {
    try {
      await chrome.runtime.sendMessage({
        kind: "osfui-tab-audio-command-error",
        error: failure?.stack ?? failure?.message ?? String(failure),
      });
    } catch {
      // The standalone host can still inspect the collector's last state.
    }
  }
});
