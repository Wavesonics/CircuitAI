"use strict";
// BARb brain viewer. Talks to bridge.py; message shapes are in PROTOCOL.md.

const FPS = 30;
const MAX_DECISIONS = 400;
const MAX_HISTORY = 900; // state samples kept for the economy chart

// Branches each decision can take, keyed by the path node names the AI emits.
// Only used to show the alternatives next to the branch actually taken.
const TREES = {
  factory: {
    "assist unit": ["yes"],
    "queued recruit fits": ["take it", "none"],
    "air allowed vs enemy AA": ["yes", "no"],
    "need more build power": ["yes", "no"],
    "economy ready": ["yes", "no"],
    "continue response batch": [],
    "continue regular batch": [],
    "weighted roll": ["picked", "no pick", "all weights zero, uniform pick"],
    "pick fighting unit": ["picked", "nothing available"],
  },
  builder: {
    "enemy to fight near base": ["yes", "no"],
    "travel cost map": ["first run, wait"],
    "commander with hide rules": ["yes"],
    "early game or few workers": ["yes, peace radius"],
    "commander in danger": ["yes, stay close to base", "no, peace radius"],
    "base builder (energizer)": ["yes", "no"],
    "score queued jobs": [],
    "no job fits": ["keep current task", "create a new one"],
  },
  attackTarget: { "score enemy groups": [] },
  retreat: {
    "retreat gate": ["health at or below retreat threshold", "shield not charged", "disarmed"],
    "retreat trigger": ["health below 20%", "damaged with no visible target", "target out of range", "local threat over half own power"],
  },
};
// Lower score wins for these kinds; factory scores are roll weights
const LOWER_WINS = new Set(["builder", "attackTarget"]);

const LAYERS = {
  influence: { grid: "infl", ramp: "diverging", label: "ally minus enemy" },
  inflEnemy: { grid: "infl", ramp: "enemy", label: "enemy influence" },
  inflAlly: { grid: "infl", ramp: "ally", label: "ally influence" },
  inflAllyDefend: { grid: "infl", ramp: "defend", label: "ally defence" },
  threatSurf: { grid: "threat", ramp: "threat", label: "surface threat" },
  threatAir: { grid: "threat", ramp: "threat", label: "air threat" },
  threatAmph: { grid: "threat", ramp: "threat", label: "amphibious threat" },
  threatCloak: { grid: "threat", ramp: "cloak", label: "decloak radius" },
};

const OVERLAYS = [
  ["units", "Our units", true],
  ["enemies", "Enemies", true],
  ["groups", "Enemy groups", true],
  ["squads", "Squad targets", true],
  ["jobs", "Build jobs", true],
  ["metal", "Metal spots", true],
  ["clusters", "Cluster graph", false],
  ["base", "Base", true],
];

const S = {
  ais: [],
  port: null,
  source: null,
  connected: false,
  meta: null,
  defs: new Map(),
  state: null,
  stateCache: new Map(), // decoded layer name -> Float32Array, for S.state
  history: [],
  decisions: [],
  kinds: new Set(Object.keys(TREES)),
  selected: null,
  highlight: null, // option index under the mouse in the detail table
  layer: "influence",
  overlays: Object.fromEntries(OVERLAYS.map(([k, , on]) => [k, on])),
  paused: false,
  view: null, // {scale, ox, oy}
  terrain: null, // offscreen canvas with shaded heightmap
  mouse: null,
};

const $ = (id) => document.getElementById(id);
const el = (tag, cls, text) => {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text != null) e.textContent = text;
  return e;
};
const fmt = (v, d = 1) => (v == null || Number.isNaN(v) ? "-" : Math.abs(v) >= 1000 ? (v / 1000).toFixed(1) + "k" : (+v).toFixed(d));
const clock = (frame) => {
  const s = Math.max(0, Math.floor(frame / FPS));
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`;
};
const css = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

function decodeFloats(b64) {
  if (!b64) return null;
  const bin = atob(b64);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  return new Float32Array(bytes.buffer);
}

function layerData(name) {
  if (!S.state || !S.state.layers) return null;
  if (!S.stateCache.has(name)) S.stateCache.set(name, decodeFloats(S.state.layers[name]));
  return S.stateCache.get(name);
}

function defName(id) {
  const d = S.defs.get(id);
  return d ? d.name : id >= 0 ? `def ${id}` : "unknown";
}

// ---------------------------------------------------------------- connection

async function refreshAIs() {
  try {
    const res = await fetch("/api/ais");
    S.ais = await res.json();
  } catch {
    S.ais = [];
  }
  const live = S.ais.filter((a) => a.connected && a.hello);
  const tabs = $("ai-tabs");
  tabs.replaceChildren();
  if (!live.length) {
    tabs.append(el("span", "empty", "Waiting for an AI. Start a match with BARB_BRAIN=1."));
  }
  for (const a of live) {
    const h = a.hello;
    const b = el("button", a.port === S.port ? "active" : "", `AI ${h.ai} · team ${h.team} · ${h.side}`);
    b.onclick = () => selectAI(a.port);
    tabs.append(b);
  }
  if ((S.port == null || !live.some((a) => a.port === S.port)) && live.length) selectAI(live[0].port);
}

async function selectAI(port) {
  if (S.port === port && S.source) return;
  S.port = port;
  if (S.source) S.source.close();
  Object.assign(S, { meta: null, state: null, history: [], decisions: [], selected: null, terrain: null, view: null });
  S.stateCache.clear();
  renderFeed();
  closeDetail();
  await loadMeta();
  const src = new EventSource(`/events?port=${port}`);
  S.source = src;
  src.addEventListener("state", (e) => onState(JSON.parse(e.data)));
  src.addEventListener("decision", (e) => onDecision(JSON.parse(e.data)));
  src.addEventListener("meta", () => loadMeta());
  src.addEventListener("status", (e) => setConnected(JSON.parse(e.data).connected));
  src.onopen = () => setConnected(true);
  src.onerror = () => setConnected(false);
  refreshAIs();
}

async function loadMeta() {
  if (S.port == null) return;
  try {
    const res = await fetch(`/api/meta?port=${S.port}`);
    if (!res.ok) return;
    S.meta = await res.json();
  } catch {
    return;
  }
  S.defs = new Map(S.meta.defs.map(([id, name, role, cost]) => [id, { name, role, cost }]));
  S.terrain = buildTerrain(S.meta.heights);
  S.view = null;
  draw();
}

function setConnected(on) {
  S.connected = on;
  $("conn").classList.toggle("on", on);
}

function onState(st) {
  S.history.push({ frame: st.frame, eco: st.eco });
  if (S.history.length > MAX_HISTORY) S.history.shift();
  if (S.paused) return;
  S.state = st;
  S.stateCache.clear();
  S.tasks = new Map(st.tasks.map((t) => [t.id, t]));
  $("clock").textContent = clock(st.frame);
  renderEconomy();
  renderMilitary();
  draw();
}

// Decisions can arrive dozens of times a second; repaint at most once per frame
// and move the followed detail at most every FOLLOW_MS so it stays readable.
const FOLLOW_MS = 1500;
let feedQueued = false;
let lastFollow = 0;
function onDecision(d) {
  S.decisions.push(d);
  if (S.decisions.length > MAX_DECISIONS) S.decisions.shift();
  if (S.paused || feedQueued) return;
  feedQueued = true;
  requestAnimationFrame(() => {
    feedQueued = false;
    const now = performance.now();
    if ($("follow").checked && now - lastFollow > FOLLOW_MS) {
      const last = S.decisions.findLast((x) => S.kinds.has(x.kind));
      if (last) {
        lastFollow = now;
        showDetail(last); // also repaints the feed
        return;
      }
    }
    renderFeed();
  });
}

// ---------------------------------------------------------------- map

function fitView(canvas) {
  const m = S.meta.map;
  const scale = Math.min(canvas.width / m.width, canvas.height / m.height) * 0.96;
  S.view = { scale, ox: (canvas.width - m.width * scale) / 2, oy: (canvas.height - m.height * scale) / 2 };
}
const toScreen = (x, z) => [S.view.ox + x * S.view.scale, S.view.oy + z * S.view.scale];
const toWorld = (sx, sy) => [(sx - S.view.ox) / S.view.scale, (sy - S.view.oy) / S.view.scale];

function buildTerrain(h) {
  const data = decodeFloats(h.data);
  if (!data || !h.w) return null;
  const c = document.createElement("canvas");
  c.width = h.w;
  c.height = h.h;
  const ctx = c.getContext("2d");
  const img = ctx.createImageData(h.w, h.h);
  const range = Math.max(1, h.max - Math.max(0, h.min));
  for (let z = 0; z < h.h; z++) {
    for (let x = 0; x < h.w; x++) {
      const i = z * h.w + x;
      const v = data[i];
      // Hillshade from a light in the north-west
      const dx = data[z * h.w + Math.min(h.w - 1, x + 1)] - data[z * h.w + Math.max(0, x - 1)];
      const dz = data[Math.min(h.h - 1, z + 1) * h.w + x] - data[Math.max(0, z - 1) * h.w + x];
      const shade = Math.max(0, Math.min(1, 0.55 - (dx + dz) / (h.cell * 1.5)));
      const o = i * 4;
      if (v < 0) {
        const depth = Math.min(1, -v / 150);
        img.data[o] = 20; img.data[o + 1] = 45 + 30 * (1 - depth); img.data[o + 2] = 80 + 50 * (1 - depth);
      } else {
        const t = (v - Math.max(0, h.min)) / range;
        const g = 40 + 70 * t;
        img.data[o] = g * (0.6 + shade * 0.8);
        img.data[o + 1] = (g + 6) * (0.6 + shade * 0.8);
        img.data[o + 2] = (g - 4) * (0.6 + shade * 0.8);
      }
      img.data[o + 3] = 255;
    }
  }
  ctx.putImageData(img, 0, 0);
  return c;
}

const RAMPS = {
  diverging: (t) => (t >= 0 ? [63, 167, 255, t] : [255, 90, 79, -t]),
  enemy: (t) => [255, 90, 79, t],
  ally: (t) => [63, 167, 255, t],
  defend: (t) => [79, 209, 139, t],
  threat: (t) => [255, 140 - 100 * t, 40, t],
  cloak: (t) => [200, 120, 255, t],
};

function layerScale(data, diverging) {
  // 98th percentile of magnitude, so a single hot cell does not wash out the map
  const mags = [];
  for (let i = 0; i < data.length; i++) if (data[i] !== 0) mags.push(Math.abs(data[i]));
  if (!mags.length) return 1;
  mags.sort((a, b) => a - b);
  const p = mags[Math.min(mags.length - 1, Math.floor(mags.length * 0.98))];
  return Math.max(p, diverging ? 0.5 : 0.01);
}

function layerImage(name) {
  const def = LAYERS[name];
  const data = layerData(name);
  const grid = def && S.meta && (def.grid === "infl" ? S.meta.inflGrid : S.meta.threatGrid);
  if (!data || !grid || data.length !== grid.w * grid.h) return null;
  const diverging = def.ramp === "diverging";
  const max = layerScale(data, diverging);
  const c = document.createElement("canvas");
  c.width = grid.w;
  c.height = grid.h;
  const ctx = c.getContext("2d");
  const img = ctx.createImageData(grid.w, grid.h);
  const ramp = RAMPS[def.ramp];
  for (let i = 0; i < data.length; i++) {
    const t = Math.max(-1, Math.min(1, data[i] / max));
    const [r, g, b, a] = ramp(diverging ? t : Math.max(0, t));
    const o = i * 4;
    img.data[o] = r; img.data[o + 1] = g; img.data[o + 2] = b;
    img.data[o + 3] = Math.round(Math.sqrt(Math.abs(a)) * 200);
  }
  ctx.putImageData(img, 0, 0);
  return { canvas: c, grid, max, diverging, ramp: def.ramp, label: def.label };
}

function draw() {
  const canvas = $("map");
  const dpr = window.devicePixelRatio || 1;
  const w = Math.round(canvas.clientWidth * dpr);
  const h = Math.round(canvas.clientHeight * dpr);
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w;
    canvas.height = h;
    S.view = null;
  }
  const ctx = canvas.getContext("2d");
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.clearRect(0, 0, w, h);
  if (!S.meta) return;
  if (!S.view) fitView(canvas);
  const m = S.meta.map;
  const [x0, y0] = toScreen(0, 0);
  const sw = m.width * S.view.scale;
  const sh = m.height * S.view.scale;

  ctx.imageSmoothingEnabled = true;
  if (S.terrain) ctx.drawImage(S.terrain, x0, y0, sw, sh);
  const li = S.layer !== "none" ? layerImage(S.layer) : null;
  if (li) {
    const gw = li.grid.w * li.grid.cell * S.view.scale;
    const gh = li.grid.h * li.grid.cell * S.view.scale;
    ctx.drawImage(li.canvas, x0, y0, gw, gh);
  }
  renderLegend(li);
  ctx.strokeStyle = css("--line");
  ctx.strokeRect(x0, y0, sw, sh);

  const st = S.state;
  const px = dpr;
  if (S.overlays.clusters) drawClusters(ctx, px);
  if (S.overlays.metal) drawMetal(ctx, px, st);
  if (st) {
    if (S.overlays.base) drawBase(ctx, px, st);
    if (S.overlays.jobs) drawJobs(ctx, px, st);
    if (S.overlays.groups) drawGroups(ctx, px, st);
    if (S.overlays.enemies) drawEnemies(ctx, px, st);
    if (S.overlays.units) drawUnits(ctx, px, st);
    if (S.overlays.squads) drawSquads(ctx, px, st);
  }
  if (S.selected) drawDecision(ctx, px, S.selected);
}

function dot(ctx, x, z, r, fill, stroke) {
  const [sx, sy] = toScreen(x, z);
  ctx.beginPath();
  ctx.arc(sx, sy, r, 0, Math.PI * 2);
  if (fill) { ctx.fillStyle = fill; ctx.fill(); }
  if (stroke) { ctx.strokeStyle = stroke; ctx.stroke(); }
}

function drawClusters(ctx, px) {
  const cl = S.meta.clusters;
  ctx.strokeStyle = "rgba(255,255,255,0.18)";
  ctx.lineWidth = px;
  for (const [a, b] of S.meta.clusterEdges) {
    if (!cl[a] || !cl[b]) continue;
    const [ax, ay] = toScreen(cl[a][0], cl[a][1]);
    const [bx, by] = toScreen(cl[b][0], cl[b][1]);
    ctx.beginPath(); ctx.moveTo(ax, ay); ctx.lineTo(bx, by); ctx.stroke();
  }
  for (const c of cl) {
    dot(ctx, c[0], c[1], Math.max(3 * px, c[2] * S.view.scale), null, "rgba(255,255,255,0.25)");
  }
}

function drawMetal(ctx, px, st) {
  const open = st ? st.openSpots : null;
  S.meta.metalSpots.forEach(([x, z], i) => {
    const free = !open || open[i] === "1";
    const [sx, sy] = toScreen(x, z);
    const r = 3 * px;
    ctx.fillStyle = free ? "rgba(230,230,230,0.85)" : "rgba(120,120,120,0.6)";
    ctx.fillRect(sx - r, sy - r, r * 2, r * 2);
  });
}

function drawBase(ctx, px, st) {
  const mark = (p, color, label) => {
    if (!p || p[0] < 0) return;
    const [sx, sy] = toScreen(p[0], p[1]);
    ctx.strokeStyle = color; ctx.lineWidth = 2 * px;
    ctx.beginPath(); ctx.arc(sx, sy, 10 * px, 0, Math.PI * 2); ctx.stroke();
    ctx.fillStyle = color; ctx.font = `${11 * px}px ${css("--sans")}`;
    ctx.fillText(label, sx + 13 * px, sy + 4 * px);
  };
  mark(st.basePos, css("--ally"), "base");
  mark(st.enemyPos, css("--enemy"), "enemy centre");
}

const JOB_COLORS = {
  factory: "#c9a6ff", nano: "#9be1ff", energy: "#ffe066", mex: "#e6e6e6", mexup: "#e6e6e6",
  defence: "#ff9b94", bunker: "#ff9b94", radar: "#9bf5c4", store: "#d1b48c",
};
function drawJobs(ctx, px, st) {
  const names = S.meta.buildTypes;
  for (const t of st.tasks) {
    if (t.build == null || !t.pos || t.pos[0] < 0) continue;
    const name = names[t.build] || "";
    const [sx, sy] = toScreen(t.pos[0], t.pos[1]);
    const r = 4 * px;
    ctx.strokeStyle = JOB_COLORS[name] || "#8ccbff";
    ctx.lineWidth = 1.5 * px;
    ctx.setLineDash(t.n > 0 ? [] : [2 * px, 2 * px]);
    ctx.strokeRect(sx - r, sy - r, r * 2, r * 2);
  }
  ctx.setLineDash([]);
}

function drawGroups(ctx, px, st) {
  ctx.lineWidth = 1.5 * px;
  for (const [x, z, cost, infl, , n] of st.groups) {
    if (!n) continue;
    const r = Math.max(6 * px, Math.sqrt(Math.max(infl, 0)) * 4 * px);
    dot(ctx, x, z, r, "rgba(255,90,79,0.10)", "rgba(255,90,79,0.7)");
    const [sx, sy] = toScreen(x, z);
    ctx.fillStyle = "rgba(255,160,150,0.9)";
    ctx.font = `${10 * px}px ${css("--mono")}`;
    ctx.fillText(`${n}u ${fmt(cost, 0)}m`, sx + r + 2 * px, sy);
  }
}

function drawEnemies(ctx, px, st) {
  const enemy = css("--enemy");
  for (const [id, , x, z, los] of st.enemies) {
    const [sx, sy] = toScreen(x, z);
    if (id < 0) { // inferred from damage, never seen
      ctx.strokeStyle = enemy; ctx.lineWidth = px;
      const r = 3 * px;
      ctx.beginPath(); ctx.moveTo(sx - r, sy - r); ctx.lineTo(sx + r, sy + r);
      ctx.moveTo(sx + r, sy - r); ctx.lineTo(sx - r, sy + r); ctx.stroke();
    } else if (los & 1) {
      dot(ctx, x, z, 2.5 * px, enemy);
    } else if (los & 2) {
      dot(ctx, x, z, 2.5 * px, null, enemy);
    } else {
      dot(ctx, x, z, 2 * px, "rgba(255,90,79,0.35)"); // remembered ghost
    }
  }
}

function drawUnits(ctx, px, st) {
  const types = S.meta.taskTypes;
  const color = { builder: "#8ccbff", factory: "#c9a6ff", fighter: "#ffd27a", retreat: "#ff9b94", idle: "#9aa3ad", wait: "#9aa3ad" };
  for (const [, defId, x, z, hp, taskId] of st.units) {
    const t = S.tasks.get(taskId);
    const name = t ? types[t.type] : "idle";
    const cost = S.defs.get(defId)?.cost || 50;
    const r = Math.max(2, Math.min(7, Math.log10(cost + 10) * 1.8)) * px;
    dot(ctx, x, z, r, color[name] || "#dfe4ea", hp < 0.5 ? "#ff5a4f" : "rgba(0,0,0,0.6)");
  }
}

function drawSquads(ctx, px, st) {
  ctx.lineWidth = 1.5 * px;
  for (const t of st.tasks) {
    if (t.fight == null || !t.targetPos) continue;
    const [ax, ay] = toScreen(t.pos[0], t.pos[1]);
    const [bx, by] = toScreen(t.targetPos[0], t.targetPos[1]);
    ctx.strokeStyle = "rgba(255,210,122,0.8)";
    arrow(ctx, ax, ay, bx, by, 6 * px);
  }
}

function arrow(ctx, ax, ay, bx, by, head) {
  ctx.beginPath(); ctx.moveTo(ax, ay); ctx.lineTo(bx, by); ctx.stroke();
  const a = Math.atan2(by - ay, bx - ax);
  ctx.beginPath();
  ctx.moveTo(bx, by);
  ctx.lineTo(bx - head * Math.cos(a - 0.4), by - head * Math.sin(a - 0.4));
  ctx.moveTo(bx, by);
  ctx.lineTo(bx - head * Math.cos(a + 0.4), by - head * Math.sin(a + 0.4));
  ctx.stroke();
}

function drawDecision(ctx, px, d) {
  const chosenColor = css("--chosen");
  if (d.pos) {
    dot(ctx, d.pos[0], d.pos[1], 9 * px, null, css("--accent"));
  }
  d.options.forEach((o, i) => {
    if (!o.pos) return;
    const chosen = i === d.chosen;
    const hot = i === S.highlight;
    const [sx, sy] = toScreen(o.pos[0], o.pos[1]);
    if (d.pos && (chosen || hot)) {
      const [ux, uy] = toScreen(d.pos[0], d.pos[1]);
      ctx.strokeStyle = chosen ? chosenColor : css("--accent");
      ctx.lineWidth = 2 * px;
      arrow(ctx, ux, uy, sx, sy, 7 * px);
    }
    ctx.lineWidth = (chosen || hot ? 2.5 : 1.2) * px;
    ctx.strokeStyle = o.reject ? "rgba(160,160,160,0.8)" : chosen ? chosenColor : hot ? css("--accent") : "rgba(140,203,255,0.8)";
    ctx.beginPath(); ctx.arc(sx, sy, (chosen || hot ? 8 : 5) * px, 0, Math.PI * 2); ctx.stroke();
  });
}

function renderLegend(li) {
  const lg = $("legend");
  if (!li) { lg.hidden = true; return; }
  lg.hidden = false;
  const stops = li.diverging ? [-1, 0, 1] : [0, 1];
  const grad = stops.map((t) => {
    const [r, g, b, a] = RAMPS[li.ramp](t);
    return `rgba(${r},${g},${b},${Math.max(0.05, Math.sqrt(Math.abs(a)))})`;
  });
  lg.innerHTML = "";
  lg.append(el("div", "", li.label));
  const ramp = el("div", "ramp");
  ramp.style.background = `linear-gradient(90deg, ${grad.join(",")})`;
  lg.append(ramp);
  const ends = el("div", "ends");
  ends.append(el("span", "", li.diverging ? `enemy ${fmt(li.max)}` : "0"), el("span", "", li.diverging ? `ally ${fmt(li.max)}` : fmt(li.max)));
  lg.append(ends);
}

// ---------------------------------------------------------------- hover

function sampleGrid(name, x, z) {
  const def = LAYERS[name];
  const grid = def.grid === "infl" ? S.meta.inflGrid : S.meta.threatGrid;
  const data = layerData(name);
  if (!data) return null;
  const gx = Math.floor(x / grid.cell);
  const gz = Math.floor(z / grid.cell);
  if (gx < 0 || gz < 0 || gx >= grid.w || gz >= grid.h) return null;
  return data[gz * grid.w + gx];
}

function nearest(list, x, z, radius, getXZ) {
  let best = null;
  let bestD = radius * radius;
  for (const item of list) {
    const [ix, iz] = getXZ(item);
    const d = (ix - x) ** 2 + (iz - z) ** 2;
    if (d < bestD) { bestD = d; best = item; }
  }
  return best;
}

function updateTooltip() {
  const tip = $("tooltip");
  if (!S.mouse || !S.meta || !S.view || !S.state) { tip.hidden = true; return; }
  const dpr = window.devicePixelRatio || 1;
  const [x, z] = toWorld(S.mouse.x * dpr, S.mouse.y * dpr);
  const m = S.meta.map;
  if (x < 0 || z < 0 || x > m.width || z > m.height) { tip.hidden = true; return; }
  const rows = [];
  for (const name of Object.keys(LAYERS)) {
    const v = sampleGrid(name, x, z);
    if (v != null && v !== 0) rows.push([LAYERS[name].label, fmt(v, 2)]);
  }
  const pick = 14 / S.view.scale * dpr;
  const unit = nearest(S.state.units, x, z, pick, (u) => [u[2], u[3]]);
  const foe = nearest(S.state.enemies, x, z, pick, (e) => [e[2], e[3]]);
  tip.replaceChildren();
  tip.append(el("div", "", `x ${Math.round(x)}  z ${Math.round(z)}`));
  if (unit) {
    const t = S.tasks.get(unit[5]);
    const what = t ? describeTask(t) : "no task";
    const b = el("div");
    b.append(el("b", "", defName(unit[1])), ` #${unit[0]} · ${Math.round(unit[4] * 100)}% · ${what}`);
    tip.append(b);
  }
  if (foe) {
    const los = foe[4];
    const seen = foe[0] < 0 ? "inferred from damage" : los & 1 ? "in sight" : los & 2 ? "radar blip" : "last seen here";
    const b = el("div");
    b.append(el("b", "", foe[1] >= 0 ? defName(foe[1]) : "unknown enemy"), ` · ${seen} · ${fmt(foe[5], 0)}m`);
    tip.append(b);
  }
  if (rows.length) {
    const kv = el("div", "kv");
    for (const [k, v] of rows) kv.append(el("span", "", k), el("span", "", v));
    tip.append(kv);
  }
  tip.hidden = false;
  const pane = $("map").getBoundingClientRect();
  const left = Math.min(S.mouse.x + 14, pane.width - tip.offsetWidth - 8);
  const top = Math.min(S.mouse.y + 14, pane.height - tip.offsetHeight - 8);
  tip.style.left = `${left}px`;
  tip.style.top = `${top}px`;
}

function describeTask(t) {
  const type = S.meta.taskTypes[t.type] || "task";
  if (t.build != null) return `${S.meta.buildTypes[t.build] || type}${t.def != null ? " " + defName(t.def) : ""}`;
  if (t.fight != null) return `fight ${S.meta.fightTypes[t.fight]}`;
  return type;
}

// ---------------------------------------------------------------- panels

function renderEconomy() {
  const e = S.state.eco;
  const stats = $("eco-stats");
  stats.replaceChildren();
  const stat = (k, v) => {
    const d = el("div", "stat");
    d.append(el("div", "k", k), el("div", "", v));
    stats.append(d);
  };
  stat("metal income", fmt(e.mInc));
  stat("metal pull", fmt(e.mPull));
  stat("energy income", fmt(e.eInc));
  stat("metal stored", `${fmt(e.mCur, 0)} / ${fmt(e.mStore, 0)}`);
  stat("energy stored", `${fmt(e.eCur, 0)} / ${fmt(e.eStore, 0)}`);
  stat("eco factor", fmt(e.ecoFactor, 2));

  const flags = $("eco-flags");
  flags.replaceChildren();
  for (const [k, label] of [["excessed", "producing > using"], ["metalEmpty", "metal empty"], ["metalFull", "metal full"],
    ["energyStalling", "energy stalling"], ["energyEmpty", "energy empty"], ["energyFull", "energy full"]]) {
    flags.append(el("span", "flag" + (e[k] ? " on" : ""), label));
  }
  drawEcoChart();
}

function drawEcoChart() {
  const c = $("eco-chart");
  const dpr = window.devicePixelRatio || 1;
  c.width = Math.round(c.clientWidth * dpr);
  c.height = Math.round(c.clientHeight * dpr);
  const ctx = c.getContext("2d");
  const hist = S.history;
  if (hist.length < 2) return;
  const series = [
    ["mInc", css("--ally")],
    ["mPull", css("--warn")],
  ];
  let max = 1;
  for (const h of hist) for (const [k] of series) max = Math.max(max, h.eco[k] || 0);
  const f0 = hist[0].frame;
  const f1 = hist[hist.length - 1].frame;
  const xs = (f) => ((f - f0) / Math.max(1, f1 - f0)) * c.width;
  const ys = (v) => c.height - (v / max) * (c.height - 4 * dpr) - 2 * dpr;
  ctx.lineWidth = 1.5 * dpr;
  for (const [k, color] of series) {
    ctx.strokeStyle = color;
    ctx.beginPath();
    hist.forEach((h, i) => (i ? ctx.lineTo : ctx.moveTo).call(ctx, xs(h.frame), ys(h.eco[k] || 0)));
    ctx.stroke();
  }
  $("eco-peak").textContent = `peak ${fmt(max)}/s over ${clock(f1 - f0)}`;
}

function renderMilitary() {
  const mil = S.state.military;
  const box = $("military");
  box.replaceChildren();
  const sum = el("div", "mil-sum");
  const item = (k, v) => { const s = el("span"); s.append(`${k} `, el("b", "", v)); sum.append(s); };
  item("army", fmt(mil.armyCost, 0));
  item("enemy mobile threat", fmt(mil.enemyMobileThreat, 0));
  item("air ok", mil.airValid ? "yes" : "no");
  box.append(sum);
  const rows = S.meta.roles.map((name, i) => [name, mil.roleCost[i] || 0, mil.enemyRoleCost[i] || 0])
    .filter(([, a, b]) => a > 0 || b > 0)
    .sort((p, q) => Math.max(q[1], q[2]) - Math.max(p[1], p[2]));
  const max = Math.max(1, ...rows.map(([, a, b]) => Math.max(a, b)));
  for (const [name, ally, enemy] of rows) {
    const r = el("div", "role");
    r.title = `ours ${fmt(ally, 0)}, enemy ${fmt(enemy, 0)}`;
    const bars = el("div", "bars");
    const a = el("div", "b ally"); a.style.width = `${(ally / max) * 100}%`;
    const b = el("div", "b enemy"); b.style.width = `${(enemy / max) * 100}%`;
    bars.append(a, b);
    r.append(el("span", "", name), bars);
    box.append(r);
  }
  if (!rows.length) box.append(el("div", "flag", "no armies yet"));
}

function renderKindFilter() {
  const box = $("kind-filter");
  box.replaceChildren();
  for (const k of Object.keys(TREES)) {
    const c = el("span", "chip" + (S.kinds.has(k) ? " on" : ""), k);
    c.onclick = () => {
      if (S.kinds.has(k)) S.kinds.delete(k); else S.kinds.add(k);
      renderKindFilter();
      renderFeed();
    };
    box.append(c);
  }
}

function decisionSummary(d) {
  return d.result || (d.chosen >= 0 && d.options[d.chosen] ? d.options[d.chosen].label : "no result");
}

function renderFeed() {
  const feed = $("feed");
  const list = S.decisions.filter((d) => S.kinds.has(d.kind)).slice(-150).reverse();
  feed.replaceChildren();
  for (const d of list) {
    const li = el("li", d === S.selected ? "sel" : "");
    const r = el("span", "r");
    if (d.unitDef != null) r.append(el("span", "u", defName(d.unitDef) + " "));
    r.append(decisionSummary(d));
    li.append(el("span", "t", clock(d.frame)), el("span", `kind ${d.kind}`, d.kind), r);
    li.onclick = () => { $("follow").checked = false; showDetail(d); };
    feed.append(li);
  }
}

// ---------------------------------------------------------------- detail

function showDetail(d) {
  S.selected = d;
  S.highlight = null;
  $("detail").hidden = false;
  const title = $("detail-title");
  title.replaceChildren(el("span", `kind ${d.kind}`, d.kind), " ");
  if (d.unitDef != null) title.append(`${defName(d.unitDef)} #${d.unit} `);
  title.append(el("span", "muted", `at ${clock(d.frame)} → ${decisionSummary(d)}`));

  const path = $("detail-path");
  path.replaceChildren();
  const tree = TREES[d.kind] || {};
  for (const [node, outcome] of d.path) {
    const li = el("li");
    li.append(el("div", "q", node));
    const outs = el("div", "outs");
    const alts = new Set(tree[node] || []);
    alts.add(outcome);
    for (const o of alts) outs.append(el("span", "o" + (o === outcome ? " taken" : ""), o));
    li.append(outs);
    path.append(li);
  }
  const res = el("li", "result");
  res.append(el("div", "q", `result: ${decisionSummary(d)}`));
  path.append(res);

  const inputs = $("detail-inputs");
  inputs.replaceChildren();
  const entries = Object.entries(d.inputs || {});
  if (!entries.length) inputs.append(el("dd", "", "none"));
  for (const [k, v] of entries) inputs.append(el("dt", "", k), el("dd", "", typeof v === "number" ? fmt(v, 3) : String(v)));

  const rej = $("detail-rejected");
  rej.replaceChildren();
  const rejEntries = Object.entries(d.rejected || {});
  if (!rejEntries.length) rej.append(el("li", "", "none"));
  for (const [k, n] of rejEntries) rej.append(el("li", "", `${n} × ${k}`));

  renderOptions(d);
  renderFeed();
  draw();
}

function renderOptions(d) {
  const table = $("detail-options");
  table.replaceChildren();
  const lower = LOWER_WINS.has(d.kind);
  $("options-title").textContent = d.kind === "factory"
    ? "Options (weights for a random roll; higher is likelier)"
    : lower ? "Options (lowest score wins)" : "Options";
  if (!d.options.length) {
    const tr = el("tr");
    tr.append(el("td", "", "No options recorded for this decision."));
    table.append(tr);
    return;
  }
  const scored = d.options.filter((o) => o.score != null && !o.reject);
  const total = scored.reduce((s, o) => s + o.score, 0);
  const maxScore = Math.max(...scored.map((o) => o.score), 1e-9);
  const minScore = Math.min(...scored.map((o) => o.score), Infinity);
  const head = el("tr");
  for (const h of ["option", d.kind === "factory" ? "chance" : "score", "", "components"]) head.append(el("th", "", h));
  table.append(head);

  const order = d.options.map((o, i) => i).sort((a, b) => {
    const oa = d.options[a]; const ob = d.options[b];
    if (!!oa.reject !== !!ob.reject) return oa.reject ? 1 : -1;
    const sa = oa.score ?? (lower ? Infinity : -Infinity);
    const sb = ob.score ?? (lower ? Infinity : -Infinity);
    return lower ? sa - sb : sb - sa;
  });
  for (const i of order) {
    const o = d.options[i];
    const tr = el("tr", (i === d.chosen ? "chosen" : "") + (o.reject ? " rej" : ""));
    const name = el("td");
    name.append(o.label);
    if (o.def != null && !o.label.includes(defName(o.def))) name.append(el("span", "u", " " + defName(o.def)));
    if (o.reject) name.append(el("div", "why", o.reject));
    const num = el("td", "num");
    const barCell = el("td");
    barCell.style.width = "16%";
    if (o.score != null) {
      num.textContent = d.kind === "factory" && total > 0 && !o.reject ? `${((o.score / total) * 100).toFixed(1)}%` : fmt(o.score, 3);
      const bar = el("div", "bar");
      // Bar length shows preference: longest is the most attractive option
      const pref = lower ? (minScore > 0 ? minScore / o.score : 1) : o.score / maxScore;
      bar.style.width = `${Math.max(1, Math.min(1, pref) * 100)}%`;
      if (!o.reject) barCell.append(bar);
    }
    const parts = el("td");
    const pw = el("div", "parts");
    for (const [k, v] of Object.entries(o.parts || {})) pw.append(el("span", "", `${k} ${fmt(v, 3)}`));
    parts.append(pw);
    tr.append(name, num, barCell, parts);
    tr.onmouseenter = () => { S.highlight = i; draw(); };
    tr.onmouseleave = () => { S.highlight = null; draw(); };
    tr.onclick = () => { if (o.pos) centerOn(o.pos[0], o.pos[1]); };
    table.append(tr);
  }
  if (d.omitted) {
    const tr = el("tr");
    tr.append(el("td", "u", `${d.omitted} more options not recorded`));
    table.append(tr);
  }
}

function closeDetail() {
  S.selected = null;
  $("detail").hidden = true;
  renderFeed();
  draw();
}

function centerOn(x, z) {
  const canvas = $("map");
  S.view.ox = canvas.width / 2 - x * S.view.scale;
  S.view.oy = canvas.height / 2 - z * S.view.scale;
  draw();
}

// ---------------------------------------------------------------- input

function setupMapInput() {
  const canvas = $("map");
  let drag = null;
  canvas.addEventListener("wheel", (e) => {
    if (!S.view) return;
    e.preventDefault();
    const dpr = window.devicePixelRatio || 1;
    const k = Math.exp(-e.deltaY * 0.0015);
    const mx = e.offsetX * dpr;
    const my = e.offsetY * dpr;
    S.view.ox = mx - (mx - S.view.ox) * k;
    S.view.oy = my - (my - S.view.oy) * k;
    S.view.scale *= k;
    draw();
  }, { passive: false });
  canvas.addEventListener("mousedown", (e) => {
    drag = { x: e.clientX, y: e.clientY };
    canvas.classList.add("dragging");
  });
  window.addEventListener("mouseup", () => { drag = null; canvas.classList.remove("dragging"); });
  canvas.addEventListener("mousemove", (e) => {
    S.mouse = { x: e.offsetX, y: e.offsetY };
    if (drag && S.view) {
      const dpr = window.devicePixelRatio || 1;
      S.view.ox += (e.clientX - drag.x) * dpr;
      S.view.oy += (e.clientY - drag.y) * dpr;
      drag = { x: e.clientX, y: e.clientY };
      draw();
    }
    updateTooltip();
  });
  canvas.addEventListener("mouseleave", () => { S.mouse = null; updateTooltip(); });
  canvas.addEventListener("dblclick", () => { S.view = null; draw(); });
  window.addEventListener("resize", () => draw());
}

function setupControls() {
  $("layer").onchange = (e) => { S.layer = e.target.value; draw(); updateTooltip(); };
  const ov = $("overlays");
  for (const [key, label] of OVERLAYS) {
    const c = el("span", "chip" + (S.overlays[key] ? " on" : ""), label);
    c.onclick = () => {
      S.overlays[key] = !S.overlays[key];
      c.classList.toggle("on", S.overlays[key]);
      draw();
    };
    ov.append(c);
  }
  $("pause").onclick = (e) => {
    S.paused = !S.paused;
    e.target.textContent = S.paused ? "Resume" : "Pause";
    if (!S.paused) renderFeed();
  };
  $("detail-close").onclick = closeDetail;
  $("follow").onchange = (e) => {
    if (e.target.checked) {
      const last = [...S.decisions].reverse().find((d) => S.kinds.has(d.kind));
      if (last) showDetail(last);
    }
  };
  renderKindFilter();
}

setupControls();
setupMapInput();
refreshAIs();
setInterval(refreshAIs, 3000);
