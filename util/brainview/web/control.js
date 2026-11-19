"use strict";
// Control tab and map orders. Loaded before app.js; uses its globals (S, $, el,
// fmt, toWorld, toScreen, draw, defName) only from functions called after load.

const Control = (() => {
  const LOG_MAX = 60;
  const DRAG_PX = 4; // movement below this is a click, not a drag
  const ACK_TIMEOUT_MS = 10000;
  // Acks reach every client of an AI, so ids carry a per-page prefix
  const SESSION = Math.random().toString(36).slice(2, 8);

  const C = {
    control: null, // control state from the latest state or ack
    selected: new Set(),
    mode: null, // null | "focus" | "build"
    buildDef: null,
    nextId: 1,
    pending: new Map(), // command id -> summary text
    log: [],
    press: null, // mousedown {x, y, shift} in CSS px
    box: null, // shift-drag selection rectangle in CSS px
    hover: null, // world [x, z] under the mouse
  };

  // Tuning keys grouped for display; role keys are added from the live list
  const GROUPS = [
    ["Production", [
      ["factory.buildPowerRatio", "Builders per metal income", 0.05],
      ["factory.responseWeight", "Counter the enemy's army", 0.05],
    ]],
    ["Army", [
      ["quota.attack", "Power a squad needs to attack", 5],
      ["quota.raidMin", "Raid group minimum power", 1],
      ["quota.raidAvg", "Raid group average power", 1],
      ["quota.scouts", "Scouts", 1],
    ]],
  ];

  // ---------------------------------------------------------------- commands

  async function send(op, args = {}, summary = op) {
    if (S.port == null) return;
    const id = `${SESSION}-${C.nextId++}`;
    C.pending.set(id, summary);
    log(`${summary}…`, "pending", id);
    setTimeout(() => { if (C.pending.has(id)) settle(id, false, "no reply from the AI"); }, ACK_TIMEOUT_MS);
    try {
      const res = await fetch(`/api/send?port=${S.port}`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ t: "cmd", op, id, ...args }),
      });
      if (!res.ok) settle(id, false, `bridge answered ${res.status}`);
    } catch (err) {
      settle(id, false, `bridge unreachable: ${err.message}`);
    }
  }

  function onAck(a) {
    if (a.t === "error") {
      log(`AI rejected a message: ${a.error}`, "bad");
      return;
    }
    if (a.control) applyControl(a.control);
    if (C.pending.has(a.id)) {
      settle(a.id, a.ok, a.error);
    }
  }

  function settle(id, ok, error) {
    const summary = C.pending.get(id) || "command";
    C.pending.delete(id);
    const entry = C.log.find((e) => e.id === id);
    const text = ok ? summary : `${summary}: ${error}`;
    if (entry) Object.assign(entry, { text, kind: ok ? "ok" : "bad" });
    else log(text, ok ? "ok" : "bad");
    renderLog();
  }

  function log(text, kind, id) {
    C.log.push({ text, kind, id, frame: S.state ? S.state.frame : 0 });
    if (C.log.length > LOG_MAX) C.log.shift();
    renderLog();
  }

  // ---------------------------------------------------------------- state

  function onState(st) {
    if (st.control) applyControl(st.control);
    // Drop selected units that died
    const alive = new Set(st.units.map((u) => u[0]));
    for (const id of [...C.selected]) if (!alive.has(id)) C.selected.delete(id);
    renderSelection();
  }

  function applyControl(c) {
    C.control = c;
    renderTuning();
    renderLimits();
    renderFocus();
  }

  // A new AI tab or match: nothing from the previous one applies
  function reset() {
    Object.assign(C, { control: null, mode: null, box: null, log: [] });
    C.selected.clear();
    C.pending.clear();
    renderAll();
  }

  // ---------------------------------------------------------------- rendering

  function renderAll() {
    renderTuning();
    renderLimits();
    renderFocus();
    renderSelection();
    renderModeBar();
    renderLog();
  }

  function renderLog() {
    const box = $("cmd-log");
    box.replaceChildren();
    for (const e of [...C.log].reverse()) {
      const li = el("li", e.kind);
      li.append(el("span", "t", clock(e.frame)), el("span", "", e.text));
      box.append(li);
    }
  }

  function tuningRow(key, label, step) {
    const tun = C.control.tuning;
    if (!(key in tun)) return null;
    const changed = C.control.changed[key];
    const row = el("div", "tune" + (changed != null ? " changed" : ""));
    row.append(el("span", "lbl", label));
    const input = el("input");
    Object.assign(input, { type: "number", step: String(step), value: String(+tun[key].toFixed(4)) });
    input.onchange = () => {
      const v = Number(input.value);
      if (Number.isFinite(v)) send("set", { key, value: v }, `${label} → ${v}`);
    };
    row.append(input);
    const undo = el("button", "undo", changed != null ? `↺ ${fmt(changed, 2)}` : "");
    undo.type = "button";
    undo.title = "Restore the original value";
    undo.disabled = changed == null;
    undo.onclick = () => send("reset", { key }, `restore ${label}`);
    row.append(undo);
    return row;
  }

  function renderTuning() {
    const box = $("tuning");
    if (!C.control) { box.replaceChildren(el("div", "muted-text", "Waiting for the AI…")); return; }
    // Keep focus on an input being edited
    if (box.contains(document.activeElement) && document.activeElement.tagName === "INPUT") return;
    box.replaceChildren();
    for (const [title, keys] of GROUPS) {
      box.append(el("h3", "", title));
      for (const [key, label, step] of keys) {
        const row = tuningRow(key, label, step);
        if (row) box.append(row);
      }
    }

    box.append(el("h3", "", "Army composition by role"));
    const head = el("div", "tune head");
    head.append(el("span", "lbl", "role"), el("span", "", "max share"), el("span", "", "counter weight"));
    box.append(head);
    const showAll = $("roles-all").checked;
    const mil = S.state && S.state.military;
    const roles = (S.meta ? S.meta.roles : []).map((name, i) => ({
      name, active: mil && ((mil.roleCost[i] || 0) > 0 || (mil.enemyRoleCost[i] || 0) > 0),
    }));
    for (const { name, active } of roles) {
      const pKey = `role.${name}.maxPercent`;
      const fKey = `role.${name}.factor`;
      if (!(pKey in C.control.tuning)) continue;
      const changed = C.control.changed[pKey] != null || C.control.changed[fKey] != null;
      if (!showAll && !active && !changed) continue;
      const row = el("div", "tune role" + (changed ? " changed" : ""));
      row.append(el("span", "lbl", name));
      const share = el("input");
      Object.assign(share, { type: "range", min: "0", max: "1", step: "0.05", value: String(C.control.tuning[pKey]) });
      share.title = `${Math.round(C.control.tuning[pKey] * 100)}% of the army at most`;
      share.onchange = () => send("set", { key: pKey, value: Number(share.value) }, `${name} max share → ${Math.round(share.value * 100)}%`);
      const factor = el("input");
      Object.assign(factor, { type: "number", min: "0", max: "100", step: "0.1", value: String(+C.control.tuning[fKey].toFixed(3)) });
      factor.onchange = () => send("set", { key: fKey, value: Number(factor.value) }, `${name} counter weight → ${factor.value}`);
      row.append(share, factor);
      box.append(row);
    }
  }

  function renderLimits() {
    const box = $("limits");
    box.replaceChildren();
    if (!C.control) return;
    for (const [defId, limit, original] of C.control.limits) {
      const li = el("li");
      li.append(`${defName(defId)}: ${limit === 0 ? "banned" : `max ${limit}`} (was ${original}) `);
      const b = el("button", "", "restore");
      b.type = "button";
      b.onclick = () => send("limit", { def: defName(defId), limit: null }, `restore limit of ${defName(defId)}`);
      li.append(b);
      box.append(li);
    }
  }

  function renderFocus() {
    const f = C.control && C.control.focus;
    const status = $("focus-status");
    if (!f) {
      status.textContent = "No focus: squads pick targets on their own.";
    } else {
      const left = S.state ? Math.max(0, Math.round((f.until - S.state.frame) / FPS)) : 0;
      status.textContent = `Squads focus within ${Math.round(f.radius)} of (${Math.round(f.x)}, ${Math.round(f.z)})` +
        `${f.force ? ", ignoring strength" : ""}. Lapses in ${Math.floor(left / 60)}:${String(left % 60).padStart(2, "0")}.`;
    }
  }

  function renderSelection() {
    const box = $("sel-summary");
    if (!C.selected.size || !S.state) {
      box.textContent = "No units selected. Click a unit, or shift-drag a box, on the map.";
      return;
    }
    const counts = new Map();
    for (const u of S.state.units) {
      if (!C.selected.has(u[0])) continue;
      const name = defName(u[1]);
      counts.set(name, (counts.get(name) || 0) + 1);
    }
    const held = new Set((C.control ? C.control.controlled : []).map(([id]) => id));
    const nHeld = [...C.selected].filter((id) => held.has(id)).length;
    box.textContent = `${C.selected.size} selected: ` +
      [...counts].map(([n, c]) => `${c} ${n}`).join(", ") +
      (nHeld ? `. ${nHeld} under your control.` : ".");
  }

  function renderModeBar() {
    const bar = $("mode-bar");
    if (!C.mode) { bar.hidden = true; return; }
    bar.hidden = false;
    bar.replaceChildren(el("span", "", C.mode === "focus"
      ? "Click the map to set the attack focus"
      : `Click the map to place ${C.buildDef}`));
    const cancel = el("button", "", "Cancel");
    cancel.type = "button";
    cancel.onclick = () => setMode(null);
    bar.append(cancel);
  }

  function setMode(mode) {
    C.mode = mode;
    $("map").classList.toggle("placing", !!mode);
    renderModeBar();
    draw();
  }

  // ---------------------------------------------------------------- map

  function worldAt(e) {
    const dpr = window.devicePixelRatio || 1;
    return toWorld(e.offsetX * dpr, e.offsetY * dpr);
  }

  function unitAt(x, z) {
    if (!S.state) return null;
    const pick = (14 / S.view.scale) * (window.devicePixelRatio || 1);
    return nearest(S.state.units, x, z, pick, (u) => [u[2], u[3]]);
  }

  function selectedIds() {
    return [...C.selected];
  }

  function orderUnits(action, x, z) {
    const units = selectedIds();
    if (!units.length) return;
    const seconds = Number($("sel-seconds").value) || 120;
    const args = { action, units, seconds };
    if (x != null) Object.assign(args, { x: Math.round(x), z: Math.round(z) });
    const what = action === "fight" ? "fight-move" : action;
    send("units", args, `${what} ${units.length} unit${units.length > 1 ? "s" : ""}`);
  }

  function setFocus(x, z) {
    send("focus", {
      x: Math.round(x), z: Math.round(z),
      radius: Number($("focus-radius").value) || 1000,
      seconds: Number($("focus-seconds").value) || 300,
      force: $("focus-force").checked,
    }, `attack focus at (${Math.round(x)}, ${Math.round(z)})`);
  }

  function build(def, x, z) {
    send("build", { def, x: Math.round(x), z: Math.round(z) }, `build ${def}`);
  }

  function openMenu(e) {
    const [x, z] = worldAt(e);
    const menu = $("ctxmenu");
    menu.replaceChildren();
    const item = (label, fn, disabled) => {
      const b = el("button", "", label);
      b.type = "button";
      b.disabled = !!disabled;
      b.onclick = () => { closeMenu(); fn(); };
      menu.append(b);
    };
    const n = C.selected.size;
    item(`Fight-move ${n || ""} selected here`, () => orderUnits("fight", x, z), !n);
    item(`Move ${n || ""} selected here`, () => orderUnits("move", x, z), !n);
    item("Set attack focus here", () => setFocus(x, z));
    const def = $("build-def").value.trim();
    item(def ? `Build ${def} here` : "Build here (pick a structure first)", () => build(def, x, z), !def);
    menu.hidden = false;
    const pane = $("map").getBoundingClientRect();
    menu.style.left = `${Math.min(e.offsetX, pane.width - menu.offsetWidth - 8)}px`;
    menu.style.top = `${Math.min(e.offsetY, pane.height - menu.offsetHeight - 8)}px`;
  }

  function closeMenu() {
    $("ctxmenu").hidden = true;
  }

  function setupMap() {
    const canvas = $("map");
    canvas.addEventListener("contextmenu", (e) => {
      e.preventDefault();
      if (S.view) openMenu(e);
    });
    canvas.addEventListener("mousedown", (e) => {
      // A click that only dismisses the menu must not also change the selection
      if (!$("ctxmenu").hidden) {
        closeMenu();
        return;
      }
      if (e.button !== 0) return;
      C.press = { x: e.offsetX, y: e.offsetY, shift: e.shiftKey };
    });
    canvas.addEventListener("mousemove", (e) => {
      if (S.view) C.hover = worldAt(e);
      if (C.press && C.press.shift) {
        C.box = { x0: C.press.x, y0: C.press.y, x1: e.offsetX, y1: e.offsetY };
        draw();
      } else if (C.mode) {
        draw();
      }
    });
    window.addEventListener("mouseup", (e) => {
      const press = C.press;
      C.press = null;
      if (!press || !S.view || e.target !== canvas) { C.box = null; return; }
      const moved = Math.hypot(e.offsetX - press.x, e.offsetY - press.y) > DRAG_PX;
      if (press.shift && moved) {
        boxSelect(press, e);
        C.box = null;
        draw();
        return;
      }
      C.box = null;
      if (moved) return; // a pan, handled by app.js
      const [x, z] = worldAt(e);
      if (C.mode === "focus") {
        setFocus(x, z);
        setMode(null);
      } else if (C.mode === "build") {
        build(C.buildDef, x, z);
        if (!e.shiftKey) setMode(null); // shift keeps placing
      } else {
        clickSelect(x, z, press.shift);
      }
    });
    window.addEventListener("keydown", (e) => {
      if (e.key === "Escape") { setMode(null); closeMenu(); }
    });
  }

  function clickSelect(x, z, additive) {
    const u = unitAt(x, z);
    if (!additive) C.selected.clear();
    if (u) {
      if (additive && C.selected.has(u[0])) C.selected.delete(u[0]);
      else C.selected.add(u[0]);
    }
    renderSelection();
    draw();
  }

  function boxSelect(a, e) {
    const [ax, az] = worldAt({ offsetX: a.x, offsetY: a.y });
    const [bx, bz] = worldAt(e);
    const [x0, x1] = [Math.min(ax, bx), Math.max(ax, bx)];
    const [z0, z1] = [Math.min(az, bz), Math.max(az, bz)];
    C.selected.clear();
    for (const [id, defId, x, z] of S.state ? S.state.units : []) {
      const def = S.defs.get(defId);
      // Box selection takes mobile units; structures only by clicking them
      if (x >= x0 && x <= x1 && z >= z0 && z <= z1 && !isStructure(def)) C.selected.add(id);
    }
    renderSelection();
  }

  function isStructure(def) {
    return !def || !def.mobile;
  }

  function drawOverlay(ctx, px) {
    if (!S.view) return;
    const c = C.control;
    if (c && c.focus) {
      const [sx, sy] = toScreen(c.focus.x, c.focus.z);
      ctx.save();
      ctx.setLineDash([6 * px, 4 * px]);
      ctx.strokeStyle = "rgba(255,181,71,0.95)";
      ctx.lineWidth = 2 * px;
      ctx.beginPath(); ctx.arc(sx, sy, c.focus.radius * S.view.scale, 0, Math.PI * 2); ctx.stroke();
      ctx.restore();
      ctx.fillStyle = "rgba(255,181,71,0.95)";
      ctx.font = `${11 * px}px ${css("--sans")}`;
      ctx.fillText("attack focus", sx + 6 * px, sy - 6 * px);
    }
    if (S.state) {
      const held = new Set((c ? c.controlled : []).map(([id]) => id));
      for (const [id, , x, z] of S.state.units) {
        const sel = C.selected.has(id);
        if (!sel && !held.has(id)) continue;
        const [sx, sy] = toScreen(x, z);
        ctx.lineWidth = 2 * px;
        ctx.strokeStyle = held.has(id) ? "rgba(255,181,71,1)" : "rgba(92,200,255,1)";
        ctx.beginPath(); ctx.arc(sx, sy, (sel ? 8 : 6) * px, 0, Math.PI * 2); ctx.stroke();
      }
    }
    if (C.box) {
      ctx.strokeStyle = "rgba(92,200,255,0.9)";
      ctx.fillStyle = "rgba(92,200,255,0.12)";
      ctx.lineWidth = px;
      const x = Math.min(C.box.x0, C.box.x1) * px;
      const y = Math.min(C.box.y0, C.box.y1) * px;
      const w = Math.abs(C.box.x1 - C.box.x0) * px;
      const h = Math.abs(C.box.y1 - C.box.y0) * px;
      ctx.fillRect(x, y, w, h);
      ctx.strokeRect(x, y, w, h);
    }
    if (C.mode && C.hover) {
      const [sx, sy] = toScreen(C.hover[0], C.hover[1]);
      ctx.strokeStyle = "rgba(255,255,255,0.9)";
      ctx.lineWidth = 1.5 * px;
      if (C.mode === "focus") {
        const r = (Number($("focus-radius").value) || 1000) * S.view.scale;
        ctx.setLineDash([4 * px, 4 * px]);
        ctx.beginPath(); ctx.arc(sx, sy, r, 0, Math.PI * 2); ctx.stroke();
        ctx.setLineDash([]);
      } else {
        ctx.strokeRect(sx - 8 * px, sy - 8 * px, 16 * px, 16 * px);
      }
    }
  }

  // ---------------------------------------------------------------- panel

  function fillDefNames() {
    const list = $("def-names");
    list.replaceChildren();
    if (!S.meta) return;
    const names = S.meta.defs.map((d) => d[1]).sort();
    for (const n of names) {
      const o = el("option");
      o.value = n;
      list.append(o);
    }
  }

  function setupPanel() {
    for (const b of document.querySelectorAll(".side-tabs button")) {
      b.onclick = () => {
        for (const o of document.querySelectorAll(".side-tabs button")) o.classList.toggle("active", o === b);
        $("tab-watch").hidden = b.dataset.tab !== "watch";
        $("tab-control").hidden = b.dataset.tab !== "control";
        if (b.dataset.tab === "control") { fillDefNames(); renderAll(); }
      };
    }
    for (const b of document.querySelectorAll("[data-unit-action]")) {
      b.onclick = () => orderUnits(b.dataset.unitAction);
    }
    $("sel-clear").onclick = () => { C.selected.clear(); renderSelection(); draw(); };
    $("focus-place").onclick = () => setMode("focus");
    $("focus-clear").onclick = () => send("focus", { clear: true }, "clear attack focus");
    $("recruit-go").onclick = () => {
      const def = $("recruit-def").value.trim();
      const count = Number($("recruit-count").value) || 1;
      if (def) send("recruit", { def, count }, `queue ${count} ${def}`);
    };
    $("build-place").onclick = () => {
      const def = $("build-def").value.trim();
      if (!def) return;
      C.buildDef = def;
      setMode("build");
    };
    $("limit-go").onclick = () => {
      const def = $("limit-def").value.trim();
      const limit = Math.max(0, Math.floor(Number($("limit-value").value) || 0));
      if (def) send("limit", { def, limit }, limit === 0 ? `ban ${def}` : `limit ${def} to ${limit}`);
    };
    $("reset-all").onclick = () => send("reset", {}, "reset everything");
    $("roles-all").onchange = renderTuning;
  }

  document.addEventListener("DOMContentLoaded", () => {
    setupPanel();
    setupMap();
    renderAll();
  });

  return { onAck, onState, reset, draw: drawOverlay, fillDefNames };
})();
