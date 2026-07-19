// Renderer stress view — the animation-heavy half of the OSF UI renderer
// benchmark (docs/renderer-benchmark.md).
//
// Six scenes, each isolating one stage of the pipeline, cycled on a fixed
// schedule so a WebView2 run and an Ultralight run see byte-identical work:
//
//   idle           static page                    — the floor
//   css-transform  keyframe transform/opacity     — compositing (GPU shortcut)
//   paint          forced property repaints       — rasterization
//   canvas         Canvas2D, fixed draw budget    — raster + JS
//   layout         reflow / text metrics / churn  — layout engine
//   fullscreen     whole viewport changes         — defeats dirty rects
//
// DETERMINISM RULES (a violated one silently invalidates a comparison):
//   - no Math.random: all placement comes from a seeded PRNG (same seed → same
//     layout every run, on every machine)
//   - all motion is a pure function of elapsed scene time, never of frame
//     count or delta accumulation, so a slow renderer draws the SAME animation
//     sampled less often rather than a different, shorter one
//   - scene time advances only while the overlay is visible, so both runs
//     align to overlay-open time regardless of when the view loaded
//
// The page reports its own throughput (what the WEB ENGINE managed) which is
// the other half of the story from the native Bench: channels (what the GAME
// paid). Stats go to the on-screen HUD and to console.log, which the runtime
// mirrors into OSF UI.log.

"use strict";

const SCENE_SECONDS = 20;      // per scene; a full cycle is 6 × this
const LONG_FRAME_MS = 33;      // "long" = dropped below 30 fps
const HUD_HZ = 4;              // HUD text refresh rate (kept low: it is overhead)

// ---- deterministic PRNG (mulberry32) -----------------------------------

function makeRandom(seed) {
  let a = seed >>> 0;
  return function random() {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// ---- element lookup ----------------------------------------------------

const stages = {
  "idle": document.getElementById("stage-idle"),
  "css-transform": document.getElementById("stage-css-transform"),
  "paint": document.getElementById("stage-paint"),
  "canvas": document.getElementById("stage-canvas"),
  "layout": document.getElementById("stage-layout"),
  "fullscreen": document.getElementById("stage-fullscreen"),
};

const hud = {
  index: document.getElementById("hud-index"),
  name: document.getElementById("hud-name"),
  state: document.getElementById("hud-state"),
  bar: document.getElementById("hud-bar"),
  desc: document.getElementById("hud-desc"),
  fps: document.getElementById("stat-fps"),
  avg: document.getElementById("stat-avg"),
  worst: document.getElementById("stat-worst"),
  long: document.getElementById("stat-long"),
};

// ---- scene builders ----------------------------------------------------
// Each scene builds its DOM once at boot (so a scene switch is a display
// toggle, never a construction cost) and exposes an optional per-frame
// update(t) where t is seconds since the scene became active.

const ORBITERS = 150;

function buildCssTransform() {
  const field = document.getElementById("orbit-field");
  const rand = makeRandom(0x51ce01);
  const frag = document.createDocumentFragment();
  for (let i = 0; i < ORBITERS; i++) {
    const node = document.createElement("div");
    node.className = "orbiter";
    const radius = 60 + rand() * 460;
    const scale = 0.4 + rand() * 1.5;
    const period = 3 + rand() * 9;
    const hue = Math.floor(170 + rand() * 80);
    node.style.setProperty("--radius", radius.toFixed(1) + "px");
    node.style.setProperty("--scale", scale.toFixed(2));
    node.style.animationDuration = period.toFixed(2) + "s";
    // Negative delay: every orbiter starts mid-flight, so the field is dense
    // from frame one instead of unfolding from a single point.
    node.style.animationDelay = "-" + (rand() * period).toFixed(2) + "s";
    node.style.background = `hsl(${hue} 70% 60%)`;
    node.style.boxShadow = `0 0 ${(6 + rand() * 14).toFixed(0)}px hsl(${hue} 80% 55%)`;
    frag.appendChild(node);
  }
  field.appendChild(frag);
  return null;  // pure CSS: no per-frame JS
}

const PAINT_CELLS = 60;

function buildPaint() {
  const grid = document.getElementById("paint-grid");
  const rand = makeRandom(0x9a17ed);
  const cells = [];
  const frag = document.createDocumentFragment();
  for (let i = 0; i < PAINT_CELLS; i++) {
    const node = document.createElement("div");
    node.className = "paint-cell";
    frag.appendChild(node);
    cells.push({ node, phase: rand() * Math.PI * 2, hue: 160 + rand() * 110 });
  }
  grid.appendChild(frag);

  return function updatePaint(t) {
    for (let i = 0; i < cells.length; i++) {
      const c = cells[i];
      const wave = Math.sin(t * 1.6 + c.phase);
      const angle = ((t * 40 + i * 6) % 360).toFixed(0);
      const light = (44 + wave * 18).toFixed(0);
      const spread = (10 + wave * 9).toFixed(1);
      const blur = (1.4 + wave * 1.2).toFixed(2);
      // Each of these invalidates the cell's raster; none can be handed to a
      // compositor as a transform.
      c.node.style.background =
        `linear-gradient(${angle}deg, hsl(${c.hue.toFixed(0)} 62% ${light}%), hsl(${(c.hue + 45).toFixed(0)} 55% 22%))`;
      c.node.style.boxShadow = `0 0 ${spread}px hsl(${c.hue.toFixed(0)} 70% 50%)`;
      c.node.style.filter = `blur(${blur}px)`;
    }
  };
}

const PARTICLES = 1400;

function buildCanvas() {
  const canvas = document.getElementById("particles");
  const ctx = canvas.getContext("2d");
  const rand = makeRandom(0x2b4d77);
  const seeds = [];
  for (let i = 0; i < PARTICLES; i++) {
    seeds.push({
      x: rand(), y: rand(),
      vx: (rand() - 0.5) * 0.22, vy: (rand() - 0.5) * 0.22,
      r: 1.5 + rand() * 3.5,
      hue: Math.floor(165 + rand() * 90),
    });
  }

  let w = 0, h = 0;
  function fit() {
    // Backing store follows the real device pixels: the runtime scales the
    // view, and a canvas that ignored that would quietly measure a smaller
    // raster than the other scenes.
    const dpr = window.devicePixelRatio || 1;
    w = Math.round(canvas.clientWidth * dpr);
    h = Math.round(canvas.clientHeight * dpr);
    if (canvas.width !== w || canvas.height !== h) {
      canvas.width = w;
      canvas.height = h;
    }
  }
  window.addEventListener("resize", fit);
  fit();

  return function updateCanvas(t) {
    fit();
    ctx.clearRect(0, 0, w, h);
    ctx.globalCompositeOperation = "lighter";
    for (let i = 0; i < seeds.length; i++) {
      const p = seeds[i];
      // Position is a closed-form function of t (wrapped), so frame rate
      // never changes where a particle is at a given moment.
      const x = (((p.x + p.vx * t) % 1) + 1) % 1;
      const y = (((p.y + p.vy * t) % 1) + 1) % 1;
      const px = x * w, py = y * h;
      const pulse = 0.55 + 0.45 * Math.sin(t * 2.2 + i);
      ctx.beginPath();
      ctx.arc(px, py, p.r * pulse * (window.devicePixelRatio || 1), 0, Math.PI * 2);
      ctx.fillStyle = `hsla(${p.hue} 80% 60% / ${(0.25 + pulse * 0.45).toFixed(3)})`;
      ctx.fill();
      // A short trail: doubles the path work per particle.
      ctx.beginPath();
      ctx.moveTo(px, py);
      ctx.lineTo(px - p.vx * w * 0.05, py - p.vy * h * 0.05);
      ctx.strokeStyle = `hsla(${p.hue} 80% 65% / 0.18)`;
      ctx.lineWidth = 1.2;
      ctx.stroke();
    }
    ctx.globalCompositeOperation = "source-over";
  };
}

const LAYOUT_ROWS = 240;

function buildLayout() {
  const list = document.getElementById("layout-list");
  const rand = makeRandom(0x7fd3a1);
  const rows = [];
  const frag = document.createDocumentFragment();
  const words = ["reactor", "grav drive", "shield", "cargo", "fuel", "hull",
    "sensor", "comms", "landing", "thruster", "coolant", "reserve"];
  for (let i = 0; i < LAYOUT_ROWS; i++) {
    const row = document.createElement("div");
    row.className = "layout-row";
    const label = document.createElement("span");
    label.className = "lr-label";
    label.textContent = words[Math.floor(rand() * words.length)] + " " + (i + 1);
    const meter = document.createElement("span");
    meter.className = "lr-meter";
    const value = document.createElement("span");
    value.className = "lr-value";
    row.append(label, meter, value);
    frag.appendChild(row);
    rows.push({ meter, value, phase: rand() * Math.PI * 2 });
  }
  list.appendChild(frag);

  return function updateLayout(t) {
    for (let i = 0; i < rows.length; i++) {
      const r = rows[i];
      const wave = 0.5 + 0.5 * Math.sin(t * 1.3 + r.phase);
      // Width in a flex row + changing text length: both force the line box to
      // be re-measured, which is the point of this scene.
      r.meter.style.width = (14 + wave * 150).toFixed(1) + "px";
      r.value.textContent = (wave * 1000).toFixed(1);
    }
  };
}

function buildFullscreen() {
  const grad = document.getElementById("fs-gradient");
  const sweep = document.getElementById("fs-sweep");

  return function updateFullscreen(t) {
    const a = (t * 24) % 360;
    const h1 = (t * 30) % 360;
    const h2 = (h1 + 120) % 360;
    const h3 = (h1 + 240) % 360;
    // A viewport-sized gradient whose stops move every frame: 100% of the
    // surface is invalidated, every frame, with no reusable tiles.
    grad.style.background =
      `linear-gradient(${a.toFixed(1)}deg,` +
      ` hsl(${h1.toFixed(0)} 55% 22%) 0%,` +
      ` hsl(${h2.toFixed(0)} 60% 34%) 45%,` +
      ` hsl(${h3.toFixed(0)} 55% 20%) 100%)`;
    sweep.style.transform = `translateX(${((t * 0.45) % 1.4 - 0.2) * 100}vw) skewX(-12deg)`;
  };
}

const SCENES = [
  { name: "idle", desc: "Static page. The floor cost of a loaded view that never changes.", build: () => null },
  { name: "css-transform", desc: "150 keyframe transform/opacity orbiters. A GPU browser composites these without repainting; a CPU renderer must repaint every frame.", build: buildCssTransform },
  { name: "paint", desc: "60 tiles whose gradient, shadow and blur change per frame. Forced rasterization — no compositor shortcut for anyone.", build: buildPaint },
  { name: "canvas", desc: "1400-particle Canvas2D field with trails. Fixed draw budget per frame: raster plus JS.", build: buildCanvas },
  { name: "layout", desc: "240 rows re-measured per frame (widths + text). Stresses layout and text metrics, not the rasterizer.", build: buildLayout },
  { name: "fullscreen", desc: "Whole-viewport gradient rewritten every frame. Defeats dirty-rect optimization — worst case for full-frame transports.", build: buildFullscreen },
];

for (const scene of SCENES) {
  scene.update = scene.build();
  scene.stage = stages[scene.name];
}

// ---- scene + stats state ----------------------------------------------

let sceneIndex = 0;
let sceneTime = 0;         // seconds of VISIBLE time in the current scene
let held = false;          // manual scene hold (auto-cycle off)
let paused = false;
// Overlay visibility. In game the view loads HIDDEN and the runtime only
// sends ui.visibility on an edge, so there is no "you are hidden" message to
// wait for — assume hidden whenever the bridge exists and let the first open
// start the clock. Standalone in a browser there is no overlay, so visible.
let visible = true;

let frames = 0;
let sceneFrameTimes = [];
let worstMs = 0;
let longFrames = 0;
let fpsNow = 0;
let lastHudAt = 0;
let lastFrameAt = 0;

function setScene(index, reason) {
  const previous = SCENES[sceneIndex];
  if (previous.stage) previous.stage.classList.remove("on");
  if (sceneFrameTimes.length > 0) reportScene(previous, reason);

  sceneIndex = ((index % SCENES.length) + SCENES.length) % SCENES.length;
  const scene = SCENES[sceneIndex];
  if (scene.stage) scene.stage.classList.add("on");
  sceneTime = 0;
  resetStats();

  hud.index.textContent = (sceneIndex + 1) + "/" + SCENES.length;
  hud.name.textContent = scene.name;
  hud.desc.textContent = scene.desc;
  // Draw the scene's first frame immediately so a switch never shows a stale
  // or blank surface for one frame.
  if (scene.update) scene.update(0);
}

function resetStats() {
  frames = 0;
  sceneFrameTimes = [];
  worstMs = 0;
  longFrames = 0;
}

function percentile(sorted, p) {
  if (sorted.length === 0) return 0;
  const i = Math.min(sorted.length - 1, Math.max(0, Math.round(p * (sorted.length - 1))));
  return sorted[i];
}

// One line per completed scene into OSF UI.log (the runtime mirrors console
// output). This is the WEB ENGINE's own account of throughput, to be read
// next to the native Bench: channels for the same window.
function reportScene(scene, reason) {
  const sorted = sceneFrameTimes.slice().sort((a, b) => a - b);
  const seconds = sceneFrameTimes.reduce((sum, ms) => sum + ms, 0) / 1000;
  const avgFps = seconds > 0 ? sceneFrameTimes.length / seconds : 0;
  console.log(
    "[stress] scene=" + scene.name +
    " reason=" + reason +
    " secs=" + seconds.toFixed(1) +
    " frames=" + sceneFrameTimes.length +
    " avgFps=" + avgFps.toFixed(1) +
    " p50=" + percentile(sorted, 0.5).toFixed(2) +
    " p95=" + percentile(sorted, 0.95).toFixed(2) +
    " worst=" + worstMs.toFixed(1) +
    " longFrames=" + longFrames);
}

function updateHud(now) {
  if (now - lastHudAt < 1000 / HUD_HZ) return;
  lastHudAt = now;
  const seconds = sceneFrameTimes.reduce((sum, ms) => sum + ms, 0) / 1000;
  const avgFps = seconds > 0 ? sceneFrameTimes.length / seconds : 0;
  hud.fps.textContent = fpsNow > 0 ? fpsNow.toFixed(0) : "—";
  hud.avg.textContent = avgFps > 0 ? avgFps.toFixed(1) : "—";
  hud.worst.textContent = worstMs > 0 ? worstMs.toFixed(0) + " ms" : "—";
  hud.long.textContent = String(longFrames);
  hud.bar.style.width = held || paused ? "100%" : ((sceneTime / SCENE_SECONDS) * 100).toFixed(1) + "%";
  hud.state.textContent = !visible ? "hidden" : paused ? "paused" : held ? "held" : "";
}

// ---- frame loop --------------------------------------------------------

function frame(now) {
  requestAnimationFrame(frame);

  const deltaMs = lastFrameAt > 0 ? now - lastFrameAt : 0;
  lastFrameAt = now;

  // While hidden: touch NOTHING. No scene time, no stats, and crucially no
  // DOM writes — the backends differ here (Chromium suspends rAF for a hidden
  // view; Ultralight keeps calling it), so a HUD update on this path would
  // dirty Ultralight's surface and charge the closed-overlay BASELINE for
  // repaints WebView2 never does. Paused-but-visible still refreshes, so the
  // "paused" badge appears.
  if (!visible) return;
  if (paused) {
    updateHud(now);
    return;
  }

  if (deltaMs > 0 && deltaMs < 2000) {
    frames++;
    sceneFrameTimes.push(deltaMs);
    fpsNow = 1000 / deltaMs;
    if (deltaMs > worstMs) worstMs = deltaMs;
    if (deltaMs > LONG_FRAME_MS) longFrames++;
    sceneTime += deltaMs / 1000;
  }

  const scene = SCENES[sceneIndex];
  if (scene.update) scene.update(sceneTime);

  if (!held && sceneTime >= SCENE_SECONDS) {
    setScene(sceneIndex + 1, "cycle");
  }

  updateHud(now);
}

// ---- input -------------------------------------------------------------

window.addEventListener("keydown", (event) => {
  const key = event.key;
  if (key >= "1" && key <= String(SCENES.length)) {
    held = true;
    setScene(Number(key) - 1, "manual");
  } else if (key === "0") {
    held = false;
  } else if (key === "p" || key === "P") {
    paused = !paused;
  } else if (key === "r" || key === "R") {
    resetStats();
    sceneTime = 0;
  }
});

// ---- bridge ------------------------------------------------------------
// Optional: the view runs standalone in a browser too (for smoke tests), so
// every bridge touch is guarded.

if (typeof osfui !== "undefined" && osfui.available && osfui.available()) {
  visible = false;  // in game: hidden until the first F10 (see the note above)
  osfui.on("ui.visibility", (payload) => {
    const nowVisible = !!(payload && payload.visible);
    if (nowVisible === visible) return;
    visible = nowVisible;
    // Restart the scene on reveal: a partial scene split across a hide would
    // mix two different measurement windows into one report line.
    if (visible) setScene(sceneIndex, "revealed");
  });
  osfui.ready.then((info) => {
    console.log("[stress] ready — OSF UI " + (info && info.version ? info.version : "?") +
      ", scenes=" + SCENES.length + " × " + SCENE_SECONDS + "s");
  }).catch(() => {});
}

setScene(0, "boot");
requestAnimationFrame(frame);
