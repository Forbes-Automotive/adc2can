// adc2can web UI - talks to the JSON API in src/adc2can_web.cpp
const POLL_MS = 500;
let settingsCache = null; // /api/settings
let statusCache = null;   // /api/status

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
const $ = (id) => document.getElementById(id);
async function api(path, method = "GET", body = null) {
  const opt = { method, headers: {} };
  if (body !== null) { opt.headers["Content-Type"] = "application/json"; opt.body = JSON.stringify(body); }
  const r = await fetch(path, opt);
  return r.json();
}
async function action(name, extra = {}) {
  const res = await api("/api/action", "POST", Object.assign({ action: name }, extra));
  toast(res.status || res.error || "ok");
  return res;
}
function toast(msg) {
  let t = $("toast");
  if (!t) { t = document.createElement("div"); t.id = "toast"; t.style.cssText = "position:fixed;bottom:18px;left:50%;transform:translateX(-50%);background:var(--panel);border:1px solid var(--accent);color:var(--text);padding:8px 16px;border-radius:10px;font-size:13px;z-index:99;"; document.body.appendChild(t); }
  t.textContent = msg; t.style.display = "block";
  clearTimeout(t._h); t._h = setTimeout(() => t.style.display = "none", 2500);
}
function esc(s) { return String(s == null ? "" : s).replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c])); }
function setVal(id, v) { const e = $(id); if (!e) return; if (e.dataset.hex) e.value = Number(v).toString(16).toUpperCase().padStart(8, "0"); else e.value = v == null ? "" : v; }
function getVal(id) { const e = $(id); if (!e) return undefined; if (e.dataset.hex) return parseInt(e.value, 16); if (e.type === "number" || e.tagName === "SELECT") { const n = Number(e.value); return isNaN(n) ? e.value : n; } return e.value; }
const uShown = (u) => (u === "C" ? "°C" : u === "F" ? "°F" : u === "deg" ? "°" : u || ""); // as can2gauge shows units
const numOrNull = (v) => (v === "" || isNaN(Number(v)) ? null : Number(v));
const hex = (id) => "0x" + (id >>> 0).toString(16).toUpperCase().padStart(8, "0");
function badge(id, text, cls) { const e = $(id); e.textContent = text; e.className = "status-badge " + (cls || ""); }
function chip(id, cls) { const e = $(id); if (e) e.className = "chip " + (cls || ""); }

// ---------------------------------------------------------------------------
// navigation
// ---------------------------------------------------------------------------
function showPage(name) {
  document.querySelectorAll(".nav-tab").forEach(t => t.classList.toggle("active", t.dataset.page === name));
  document.querySelectorAll(".page").forEach(p => p.classList.toggle("active", p.id === name + "-page"));
  if (name === "can") pollFrames();
  if (name === "logging" && window.logTabOpened) window.logTabOpened();
}
document.querySelectorAll(".nav-tab").forEach(tab => tab.addEventListener("click", () => showPage(tab.dataset.page)));
document.querySelectorAll("[data-goto]").forEach(a => a.addEventListener("click", (e) => {
  if (a.getAttribute("href") === "#") e.preventDefault();
  showPage(a.dataset.goto);
}));

// ---------------------------------------------------------------------------
// settings: they apply as they change - no Save buttons (as can2gauge). A select / checkbox saves at once; typing
// saves 0.8 s after the last key (and on leaving the field). collect() returns the partial settings JSON for that card.
// ---------------------------------------------------------------------------
async function saveSettings(partial, msg) {
  const r = await api("/api/settings", "POST", partial);
  toast(r.error || msg || "saved");
  settingsCache = await api("/api/settings");
  if (partial.ch) { renderTiles(); renderGaugeOpts(); } // names, units, scaling and warnings feed the dials
  return !r.error;
}
function autoSave(root, collect, msg) {
  let timer = null, last = "";
  const fire = async () => {
    clearTimeout(timer); timer = null;
    const body = collect(), key = JSON.stringify(body);
    if (key === last) return;
    last = key;
    await saveSettings(body, msg);
  };
  const isSetting = (el) => el.matches("input, select, textarea") && el.type !== "file" && el.type !== "button" && !el.closest("[data-nosave]");
  root.addEventListener("change", (e) => { if (isSetting(e.target)) fire(); });
  root.addEventListener("input", (e) => {
    if (!isSetting(e.target) || e.target.tagName === "SELECT" || e.target.type === "checkbox") return;
    clearTimeout(timer); timer = setTimeout(fire, 800);
  });
}
function fillSimple(obj, prefix) { Object.keys(obj).forEach(k => { if ($(prefix + "." + k)) setVal(prefix + "." + k, obj[k]); }); }
function collectSimple(prefix, keys) { const o = {}; keys.forEach(k => { const v = getVal(prefix + "." + k); if (v !== undefined && v !== "" && !(typeof v === "number" && isNaN(v))) o[k] = v; }); return o; }

// ---------------------------------------------------------------------------
// sensors
// ---------------------------------------------------------------------------
const PRESETS = [ // linear presets: [label, scale (units / V), offset (units at 0 V), unit, ratiometric]
  ["Volts", 1, 0, "V", 1],
  ["Pressure 0-100 psi (0.5-4.5 V)", 25, -12.5, "psi", 1],
  ["Pressure 0-150 psi (0.5-4.5 V)", 37.5, -18.75, "psi", 1],
  ["Pressure 0-7 bar (0.5-4.5 V)", 1.75, -0.875, "bar", 1],
  ["Pressure 0-10 bar (0.5-4.5 V)", 2.5, -1.25, "bar", 1],
  ["MAP 3 bar abs (Bosch 0 261 230 119)", 0.6588, -0.0635, "bar", 1],
  ["Wideband AFR (0-5 V = 10-20)", 2, 10, "AFR", 0],
  ["Wideband lambda (0-5 V = 0.68-1.36)", 0.136, 0.68, "lambda", 0],
  ["EGT amplifier (0-5 V = 0-1250 C)", 250, 0, "C", 0],
];
const MODES = ["Linear", "Table", "NTC"];

// ---------------------------------------------------------------------------
// gauge readout - OpenHaldex's per-tile dial: each sensor tile shows a number or a 270-degree gauge, picked per
// sensor in Display options and saved in this browser. The range comes from the sensor's scaling unless set.
// ---------------------------------------------------------------------------
const TG_R = 40, TG_CIRC = 2 * Math.PI * TG_R, TG_ARC = TG_CIRC * 0.75, TG_GAP = TG_CIRC - TG_ARC;
const GAUGE_PREFS_KEY = "a2cGaugePrefs";
let gaugePrefs = loadGaugePrefs();
let sweeping = false;
function loadGaugePrefs() {
  const d = { on: Array(8).fill(true) }; // showcase: dials by default (1.4: the dial ranges are on the adc2can now)
  try {
    const p = JSON.parse(localStorage.getItem(GAUGE_PREFS_KEY) || "null");
    if (p && Array.isArray(p.on)) ["on"].forEach(k => { if (Array.isArray(p[k])) d[k] = d[k].map((x, i) => (p[k][i] === undefined ? x : p[k][i])); });
  } catch (e) { /* storage blocked: defaults */ }
  return d;
}
function saveGaugePrefs() { try { localStorage.setItem(GAUGE_PREFS_KEY, JSON.stringify(gaugePrefs)); } catch (e) { /* not persisted */ } }
// 1.4: a dial's range comes from the adc2can itself (dialRange in adc2can_can.cpp): its Dial Min / Max, or Auto from
// the sensor's scaling. The board sends the range in use as dialLo / dialHi - the same numbers can2gauge gets over CAN.
function gaugeRange(i) {
  const c = settingsCache.ch[i];
  if (c.dialLo !== null && c.dialLo !== undefined && c.dialHi > c.dialLo) {
    return [c.dialLo, c.dialHi];
  }
  return [0, 5.5]; // not sent (shouldn't happen): plain volts
}
const tgPoint = (f, r) => { const a = (135 + 270 * f) * Math.PI / 180; return [50 + r * Math.cos(a), 50 + r * Math.sin(a)]; };
const fmtRange = (v) => (Math.abs(v) >= 10000 ? Math.round(v / 1000) + "k" : String(Math.round(v * 100) / 100));
function gaugeSvg(i) {
  const c = settingsCache.ch[i], [lo, hi] = gaugeRange(i), frac = (v) => (v - lo) / (hi - lo);
  const ticks = [c.lowWarn, c.highWarn].filter(v => v !== null && v > lo && v < hi).map(v => {
    const [x1, y1] = tgPoint(frac(v), 33), [x2, y2] = tgPoint(frac(v), 47);
    return `<line class="tg-tick" x1="${x1.toFixed(1)}" y1="${y1.toFixed(1)}" x2="${x2.toFixed(1)}" y2="${y2.toFixed(1)}"/>`;
  }).join("");
  return `<svg viewBox="0 0 100 100" aria-hidden="true">` +
    `<circle class="tg-track" cx="50" cy="50" r="${TG_R}" transform="rotate(135 50 50)" stroke-dasharray="${TG_ARC.toFixed(2)} ${TG_GAP.toFixed(2)}"/>` +
    `<circle class="tg-fill" id="tgFill${i}" cx="50" cy="50" r="${TG_R}" transform="rotate(135 50 50)" stroke-dasharray="0 ${TG_CIRC.toFixed(2)}"/>${ticks}` +
    `<text class="tg-val" id="tgVal${i}" x="50" y="52" text-anchor="middle">--</text>` +
    `<text class="tg-unit" x="50" y="66" text-anchor="middle">${esc(uShown(c.unit))}</text>` +
    `<text class="tg-min" x="24" y="92" text-anchor="middle">${fmtRange(lo)}</text><text class="tg-max" x="76" y="92" text-anchor="middle">${fmtRange(hi)}</text></svg>`;
}
function setGauge(i, v, text, cls) {
  const fill = $("tgFill" + i); if (!fill) return;
  $("tgVal" + i).textContent = text;
  fill.closest(".tile-gauge").className = "tile-gauge" + (cls ? " " + cls : "");
  if (sweeping) return;
  const [lo, hi] = gaugeRange(i), f = v === null ? 0 : Math.max(0, Math.min(1, (v - lo) / (hi - lo)));
  fill.style.strokeDasharray = `${(TG_ARC * f).toFixed(2)} ${TG_CIRC.toFixed(2)}`;
}
// one 0 -> full -> live sweep of every dial on page load, like a cluster on ignition (OpenHaldex's intro sweep)
function introSweep() {
  const fills = settingsCache.ch.map((c, i) => (gaugePrefs.on[i] ? $("tgFill" + i) : null)).filter(Boolean);
  if (!fills.length || document.hidden) return; // a background tab gets no animation frames: no sweep
  sweeping = true;
  fills.forEach(f => (f.style.transition = "none"));
  const ease = (x) => (x < 0.5 ? 4 * x * x * x : 1 - Math.pow(-2 * x + 2, 3) / 2), t0 = performance.now();
  const done = () => {
    if (!sweeping) return;
    fills.forEach(el => (el.style.transition = ""));
    sweeping = false;
    pollStatus();
  };
  const frame = (now) => {
    if (!sweeping) return;
    const t = now - t0, f = t < 900 ? ease(t / 900) : 1 - ease(Math.min(1, (t - 900) / 800));
    fills.forEach(el => (el.style.strokeDasharray = `${(TG_ARC * f).toFixed(2)} ${TG_CIRC.toFixed(2)}`));
    if (t < 1700) requestAnimationFrame(frame); else done();
  };
  requestAnimationFrame(frame);
  setTimeout(done, 2000); // frames can stop (tab hidden mid-sweep): the dials never stay frozen
}
// 1.4: a Dial Min / Max box changed: saved on the adc2can (blank = Auto), then the dials and the boxes again
async function dialBoxChanged(event) {
  const box = event.target;
  let key = "dialMax";
  let index = Number(box.dataset.gmax);
  if (box.dataset.gmin !== undefined) {
    key = "dialMin";
    index = Number(box.dataset.gmin);
  }
  const channel = { i: index };
  if (box.value === "") {
    channel[key] = null; // Auto
  } else {
    channel[key] = Number(box.value);
  }
  await saveSettings({ ch: [channel] }, "dial range saved");
  renderGaugeOpts();
  renderTiles();
  pollStatus();
}

function renderGaugeOpts() {
  $("gaugeOpts").innerHTML = settingsCache.ch.map((c, i) => {
    const [lo, hi] = gaugeRange(i), v = (x) => (x === null || x === undefined ? "" : x);
    // 1.4: blank = Auto; saved on the adc2can (and sent to can2gauge), not in this browser
    return `<div class="gauge-opt"><label class="tile-opt"><input type="checkbox" data-g="${i}"${gaugePrefs.on[i] ? " checked" : ""}> <span>${i + 1} &middot; ${esc(c.name)}</span></label>
      <input type="number" step="any" data-gmin="${i}" placeholder="${lo}" value="${v(c.dialMin)}" title="Dial minimum (blank = Auto: ${lo})" aria-label="Dial minimum">
      <input type="number" step="any" data-gmax="${i}" placeholder="${hi}" value="${v(c.dialMax)}" title="Dial maximum (blank = Auto: ${hi})" aria-label="Dial maximum"></div>`;
  }).join("");
  const host = $("gaugeOpts");
  host.querySelectorAll("[data-g]").forEach(cb => cb.addEventListener("change", () => { gaugePrefs.on[cb.dataset.g] = cb.checked; saveGaugePrefs(); renderTiles(); pollStatus(); }));
  host.querySelectorAll("[data-gmin],[data-gmax]").forEach(inp => inp.addEventListener("change", dialBoxChanged));
}

function renderTiles() {
  $("tiles").innerHTML = settingsCache.ch.map((c, i) => `<div class="tile sensor-tile${gaugePrefs.on[i] ? " as-gauge" : ""}" id="tile${i}">
      <div class="label"><span id="tName${i}">${esc(c.name)}</span><span class="num">${i + 1}</span></div>
      <div class="value" id="tVal${i}">--</div>
      <div class="tile-gauge">${gaugeSvg(i)}</div>
      <div class="sub"><span id="tPin${i}">-- V</span><span class="pill small" id="tChip${i}">--</span></div></div>`).join("");
}

function renderChannels() {
  $("chList").innerHTML = settingsCache.ch.map((c, i) => `<div class="ch-row" id="ch${i}">
      <h3><span>Sensor ${i + 1}</span><span class="ch-live" id="chLive${i}">--</span><span class="pill small" id="chChip${i}">--</span></h3>
      <div class="row-inline">
        <div class="form-group"><label>Name</label><input type="text" class="c-name" maxlength="14"></div>
        <div class="form-group narrow"><label>Unit</label><input type="text" class="c-unit" maxlength="7"></div>
        <div class="form-group narrow"><label>Mode</label><select class="c-mode">${MODES.map((m, k) => `<option value="${k}">${m}</option>`).join("")}</select></div>
        <div class="form-group wide c-lin"><label>Preset</label><select class="c-preset" data-nosave><option value="">Custom</option>${PRESETS.map((p, k) => `<option value="${k}">${p[0]}</option>`).join("")}</select></div>
      </div>
      <div class="row-inline">
        <div class="form-group narrow c-lin"><label>Scale (per V)</label><input type="number" step="any" class="c-scale"></div>
        <div class="form-group narrow c-lin"><label>Offset</label><input type="number" step="any" class="c-offset"></div>
        <div class="form-group wide c-tbl"><label>Table (volts:value, ...)</label><input type="text" class="c-table" placeholder="0.5:0, 2.5:5, 4.5:10"></div>
        <div class="form-group narrow c-ntc"><label>R25 (ohm)</label><input type="number" step="any" class="c-r25"></div>
        <div class="form-group narrow c-ntc"><label>Beta</label><input type="number" step="any" class="c-beta"></div>
        <div class="form-group narrow c-ntc"><label>Pull-up (ohm)</label><input type="number" step="any" class="c-pu"></div>
        <div class="form-group narrow"><label>Low warn</label><input type="number" step="any" class="c-lo" placeholder="none"></div>
        <div class="form-group narrow"><label>High warn</label><input type="number" step="any" class="c-hi" placeholder="none"></div>
      </div>
      <div class="row-inline">
        <div class="form-group narrow"><label>Decimals</label><input type="number" class="c-dec" min="0" max="4"></div>
        <div class="form-group narrow"><label>Smoothing</label><input type="number" class="c-sm" min="1" max="250" title="1 = none .. 250 = heavy"></div>
        <div class="form-group narrow"><label>Gain trim</label><input type="number" step="0.001" class="c-trim" min="0.8" max="1.2"></div>
        <label class="check"><input type="checkbox" class="c-rail"> 5 V Rail On</label>
        <label class="check"><input type="checkbox" class="c-ratio"> Ratiometric</label>
      </div></div>`).join("");
  settingsCache.ch.forEach((c, i) => {
    const row = $("ch" + i), q = (s) => row.querySelector(s);
    const modeShow = () => {
      const m = q(".c-mode").value;
      row.querySelectorAll(".c-lin").forEach(e => e.style.display = m === "0" ? "" : "none");
      row.querySelectorAll(".c-tbl").forEach(e => e.style.display = m === "1" ? "" : "none");
      row.querySelectorAll(".c-ntc").forEach(e => e.style.display = m === "2" ? "" : "none");
      q(".c-ratio").closest("label").style.display = m === "2" ? "none" : ""; // an NTC always works off its own rail
    };
    row._modeShow = modeShow;
    q(".c-mode").addEventListener("change", modeShow);
    q(".c-preset").addEventListener("change", (e) => {
      const p = PRESETS[Number(e.target.value)]; if (!p) return;
      q(".c-scale").value = p[1]; q(".c-offset").value = p[2]; q(".c-unit").value = p[3]; q(".c-ratio").checked = !!p[4];
      q(".c-scale").dispatchEvent(new Event("change", { bubbles: true }));
    });
    fillChannel(i);
    autoSave(row, () => ({ ch: [collectChannel(i)] }), "sensor " + (i + 1) + " saved");
  });
}
function fillChannel(i) {
  const c = settingsCache.ch[i], row = $("ch" + i), q = (s) => row.querySelector(s);
  const set = (s, v) => { q(s).value = v === undefined || v === null ? "" : v; };
  set(".c-name", c.name); set(".c-unit", c.unit); set(".c-mode", c.mode); set(".c-scale", c.scale); set(".c-offset", c.offset);
  set(".c-table", (c.table || []).map(p => p[0] + ":" + p[1]).join(", ")); set(".c-r25", c.ntcR25); set(".c-beta", c.ntcBeta); set(".c-pu", c.ntcPullup);
  set(".c-lo", c.lowWarn); set(".c-hi", c.highWarn); set(".c-dec", c.decimals); set(".c-sm", c.smoothing); set(".c-trim", c.trim);
  q(".c-rail").checked = !!c.railOn; q(".c-ratio").checked = !!c.ratiometric;
  const p = PRESETS.findIndex(p => Math.abs(p[1] - c.scale) < 1e-6 && Math.abs(p[2] - c.offset) < 1e-6);
  q(".c-preset").value = p >= 0 ? String(p) : "";
  row._modeShow();
}
function collectChannel(i) {
  const row = $("ch" + i), q = (s) => row.querySelector(s);
  const table = q(".c-table").value.split(",").map(x => x.split(":").map(s => Number(s.trim()))).filter(x => x.length === 2 && !isNaN(x[0]) && !isNaN(x[1])).slice(0, 8);
  return { i, name: q(".c-name").value, unit: q(".c-unit").value, mode: Number(q(".c-mode").value), scale: Number(q(".c-scale").value),
    offset: Number(q(".c-offset").value), table, ntcR25: Number(q(".c-r25").value), ntcBeta: Number(q(".c-beta").value), ntcPullup: Number(q(".c-pu").value),
    lowWarn: numOrNull(q(".c-lo").value), highWarn: numOrNull(q(".c-hi").value), decimals: Number(q(".c-dec").value) || 0,
    smoothing: Number(q(".c-sm").value) || 1, trim: Number(q(".c-trim").value) || 1, railOn: q(".c-rail").checked ? 1 : 0, ratiometric: q(".c-ratio").checked ? 1 : 0 };
}

// one sensor's state in words, and the pill class for it
function chState(c, s) {
  if (s.latched) return ["LATCHED OFF", "bad"];
  if (s.fault) return ["RAIL FAULT", "bad"];
  if (statusCache && statusCache.backfeed && c.railOn) return ["BACK-FEED", "bad"];
  if (!s.en && c.railOn && statusCache && statusCache.no12v) return ["no 12 V", "warn"];
  if (!s.en) return c.railOn ? ["rail off", ""] : ["rail off", ""];
  if (!s.ok) return ["no value", "warn"];
  if (s.warn) return ["out of range", "warn"];
  return ["OK", "ok"];
}
const fmtV = (v, d = 3) => (v === null || v === undefined ? "--" : Number(v).toFixed(d).replace(/^-(0\.?0*)$/, "$1")); // no "-0.0"

// ---------------------------------------------------------------------------
// rails table
// ---------------------------------------------------------------------------
function renderRails() {
  $("railRows").innerHTML = settingsCache.ch.map((c, i) => `<tr><td>${i + 1}</td><td id="rName${i}">${esc(c.name)}</td><td><span class="led" id="rLed${i}"></span></td>
      <td class="num" id="rV${i}">--</td><td id="rSt${i}">--</td><td><span class="pill small" id="rChip${i}">--</span></td>
      <td><button class="btn-secondary btn-sm" data-clear="${i + 1}">Clear</button></td></tr>`).join("");
  $("railRows").querySelectorAll("[data-clear]").forEach(b => b.addEventListener("click", () => action("reset", { ch: Number(b.dataset.clear) })));
}
$("clearAll").addEventListener("click", () => action("reset", { ch: 255 }));

// ---------------------------------------------------------------------------
// CAN frames
// ---------------------------------------------------------------------------
const FRAME_WHAT = ["Values: sensor 1 | 2", "Values: sensor 3 | 4", "Values: sensor 5 | 6", "Values: sensor 7 | 8", "Status (masks, flags, bus, battery)",
  "Name / unit descriptor", "Connector mV: sensors 1-4", "Connector mV: sensors 5-8", "Config part (shared settings)", "Config info (revision, firmware)"];
async function pollFrames() {
  try {
    const r = await api("/api/can/frames");
    $("frameRows").innerHTML = r.frames.map(f => `<tr><td>${hex(f.id)}</td><td style="font-family:inherit">${FRAME_WHAT[f.n]}</td><td>${f.count}</td><td>${f.data || "--"}</td></tr>`).join("")
      + `<tr><td>${hex(r.frames[0].id + 12)}</td><td style="font-family:inherit">Request from a gauge (config / clear fault)</td><td>rx</td><td></td></tr>`
      + `<tr><td>${hex(r.frames[0].id + 13)}</td><td style="font-family:inherit">Setting from a gauge (echoed as +8)</td><td>rx</td><td></td></tr>`;
  } catch (e) { /* the status poll reports the lost connection */ }
}

// ---------------------------------------------------------------------------
// status polling
// ---------------------------------------------------------------------------
const CAN_STATES = ["stopped", "running", "bus-off", "recovering"];
async function pollStatus() {
  let s;
  try { s = await api("/api/status"); } catch (e) { badge("bCan", "no connection", "error"); return; }
  statusCache = s;
  if (!settingsCache) return;
  const nOk = s.ch.filter(c => c.ok).length, nOn = settingsCache.ch.filter(c => c.railOn).length;
  const faults = s.ch.filter(c => c.fault || c.latched).length;
  badge("bSensors", "Sensors: " + nOk + "/" + nOn, faults || s.backfeed ? "error" : nOk < nOn ? "warn" : "ok");
  badge("bPower", s.no12v ? "Power: USB only" : "Power: " + s.vbat.toFixed(1) + " V", s.no12v || s.vbatLow || s.vbatHigh ? "warn" : "ok");
  badge("bCan", s.can.up && s.can.state === 1 ? "CAN: " + s.can.kbps + "k" : "CAN: " + (s.can.up ? CAN_STATES[s.can.state] : "down"), s.can.up && s.can.state === 1 ? "ok" : "error");
  // a gauge only talks to the adc2can when it needs the config (after a change or a power cycle), so this is "heard", not a live link
  const heard = s.can.rxAgo >= 0;
  badge("bGauge", heard ? "Gauge: heard" : "Gauge: not heard", heard ? "ok" : "");
  badge("bVer", "v" + s.version);
  badge("bLog", s.logging ? "Log: recording" : "Log: off", s.logging ? "error" : "");

  // alerts
  const alerts = [];
  if (s.backfeed) alerts.push(["<b>Sensor bus back-feed:</b> something has pushed 12 V onto a sensor's 5 V pin. Every rail is off until the bus is back to normal (and at least " + settingsCache.rails.backfeedRetryS + " s). Check the sensor wiring.", "bad"]);
  if (s.no12v) alerts.push(["<b>No 12 V:</b> the board is on USB power alone, so the sensors have no supply. Values return once 12 V is connected.", "warn"]);
  if (!s.adc[0] || !s.adc[1]) alerts.push(["<b>ADC chip not answering:</b> " + (!s.adc[0] ? "sensors 1-4 " : "") + (!s.adc[1] ? "sensors 5-8" : "") + " cannot be read.", "bad"]);
  if (!s.exp) alerts.push(["<b>Fault-flag expander (TCA9554) not answering:</b> rail faults are only seen from the rail voltage.", "warn"]);
  if (faults) alerts.push(["<b>Rail fault:</b> sensor " + s.ch.map((c, i) => c.fault || c.latched ? i + 1 : 0).filter(Boolean).join(", ") + ". A short or overload on that sensor's 5 V. See <a href='#' data-goto2='rails'>Rails &amp; Power</a>.", "bad"]);
  const ac = $("alertCard");
  ac.hidden = !alerts.length;
  ac.className = "card alert-card" + (alerts.length && alerts.every(a => a[1] === "warn") ? " warn" : "");
  const html = alerts.map(a => "<div>" + a[0] + "</div>").join("");
  if (ac._html !== html) { ac.innerHTML = html; ac._html = html; ac.querySelectorAll("[data-goto2]").forEach(a => a.addEventListener("click", (e) => { e.preventDefault(); showPage(a.dataset.goto2); })); }

  // sensors: tiles, editor live line, rails table
  s.ch.forEach((st, i) => {
    const c = settingsCache.ch[i], [txt, cls] = chState(c, st);
    const val = st.ok && st.v !== null ? fmtV(st.v, c.decimals) : "--";
    $("tName" + i).textContent = c.name;
    $("tVal" + i).innerHTML = esc(val) + (st.ok ? `<span class="unit">${esc(uShown(c.unit))}</span>` : "");
    $("tVal" + i).className = "value" + (!st.ok ? " dim" : st.warn ? " warn" : "");
    setGauge(i, st.ok ? st.v : null, val, cls === "bad" ? "bad" : st.warn ? "warn" : "");
    $("tPin" + i).textContent = fmtV(st.pin) + " V";
    $("tChip" + i).textContent = txt; $("tChip" + i).className = "pill small " + cls;
    $("tile" + i).className = "tile sensor-tile" + (gaugePrefs.on[i] ? " as-gauge" : "") + (cls === "bad" ? " bad" : cls === "warn" && st.warn ? " warn" : "");
    $("chLive" + i).textContent = fmtV(st.pin) + " V" + (c.ratiometric && c.mode !== 2 && st.ok ? " (" + fmtV(st.corr) + " V at 5.000)" : "") + "  ->  " + (st.ok ? val + " " + uShown(c.unit) : "--");
    $("chChip" + i).textContent = txt; $("chChip" + i).className = "pill small " + cls;
    $("rName" + i).textContent = c.name;
    $("rLed" + i).className = "led" + (st.fault || st.latched ? " bad" : st.en ? " on" : "");
    $("rV" + i).textContent = fmtV(st.rail, 2) + " V";
    $("rSt" + i).textContent = !s.exp ? "--" : (s.st >> i) & 1 ? "clear" : "FAULT";
    $("rChip" + i).textContent = txt; $("rChip" + i).className = "pill small " + cls;
  });

  // board chips
  $("vBus").textContent = s.bus.toFixed(2) + " V"; chip("cBus", s.backfeed ? "danger" : s.no12v ? "warn" : "ok");
  $("vVbat").textContent = s.vbat.toFixed(2) + " V"; chip("cVbat", s.no12v || s.vbatLow || s.vbatHigh ? "warn" : "ok");
  $("vCan").textContent = s.can.up ? CAN_STATES[s.can.state] + ", " + s.can.kbps + "k" : "down"; chip("cCan", s.can.up && s.can.state === 1 ? "ok" : "danger");
  $("vGauge").textContent = !heard ? "never" : s.can.rxAgo < 2 ? "just now" : s.can.rxAgo < 120 ? s.can.rxAgo + " s ago" : Math.round(s.can.rxAgo / 60) + " min ago"; chip("cGauge", heard ? "ok" : "");
  $("vAdc").textContent = (s.adc[0] && s.adc[1] ? "OK" : "FAIL") + (s.exp ? "" : ", flags FAIL"); chip("cAdc", s.adc[0] && s.adc[1] && s.exp ? "ok" : "danger");
  $("vRails").textContent = faults ? faults + " fault" + (faults > 1 ? "s" : "") : s.ch.filter(c => c.en).length + " on"; chip("cRails", faults || s.backfeed ? "danger" : "ok");
  // CAN tab
  $("vCanState").textContent = s.can.up ? CAN_STATES[s.can.state] : "driver failed"; chip("cCanState", s.can.up && s.can.state === 1 ? "ok" : "danger");
  $("vCanTx").textContent = s.can.tx; $("vCanErr").textContent = s.can.err; chip("cCanErr", s.can.err ? "warn" : "");
  $("vCanTec").textContent = s.can.tec + " / " + s.can.rec;
  $("vCanRx").textContent = s.can.rx + (s.can.rxAgo >= 0 ? " (" + s.can.rxAgo + " s ago)" : "");
  // settings tab
  $("wifiInfo").textContent = "Clients on the access point: " + s.wifi.apClients + "  |  http://" + s.wifi.mdns + ".local" + (s.wifi.staConnected ? "  |  home network: " + s.wifi.staIP : "  |  home network: not connected");
  $("nowInfo").textContent = s.wifi.espNow ? "Broadcasting on WiFi channel " + s.wifi.channel + "  |  " + s.wifi.nowTx + " packets" + (s.wifi.nowErr ? ", " + s.wifi.nowErr + " refused" : "") : "Not broadcasting.";
  const r = s.canRelay; // same wording as OpenHaldex's relay status
  $("canRelayStatus").textContent = !r || !r.rate ? "" :
    `Relaying ${r.fps} frames/s` + (r.lostPerSec ? `, ${r.lostPerSec} lost/s - cap each ID's rate` : "") +
    `  |  ${r.frames} relayed, ${r.lost} lost since boot`;
  $("deviceInfo").textContent = "Firmware " + s.version + "  |  up " + Math.floor(s.uptime / 60) + " min  |  heap " + Math.round(s.heap / 1024) + " kB  |  config revision " + (s.cfgRev >>> 0).toString(16).toUpperCase();
  if ($("can-page").classList.contains("active") && (pollStatus._n = (pollStatus._n || 0) + 1) % 2 === 0) pollFrames();
}

// ---------------------------------------------------------------------------
// device
// ---------------------------------------------------------------------------
$("rebootBtn").addEventListener("click", () => { if (confirm("Restart the adc2can?")) action("reboot"); });
$("factoryBtn").addEventListener("click", async () => {
  if (!confirm("Put every setting back to the factory defaults? Sensor names and scaling are lost.")) return;
  await action("factory");
});
$("backupBtn").addEventListener("click", async () => {
  const s = await api("/api/settings");
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([JSON.stringify(s, null, 2)], { type: "application/json" }));
  a.download = "adc2can-settings-" + new Date().toISOString().slice(0, 10) + ".json";
  a.click();
});
$("restoreFile").addEventListener("change", async (e) => {
  const f = e.target.files[0]; if (!f) return;
  let s;
  try { s = JSON.parse(await f.text()); } catch (err) { toast("not a settings backup"); return; }
  if (!Array.isArray(s.ch)) { toast("not a settings backup"); return; }
  delete s.version;
  if (await saveSettings(s, "settings restored")) loadAll();
  e.target.value = "";
});

// ---------------------------------------------------------------------------
// start
// ---------------------------------------------------------------------------
async function loadAll() {
  settingsCache = await api("/api/settings");
  renderTiles();
  renderGaugeOpts();
  renderChannels();
  renderRails();
  fillSimple(settingsCache.rails, "rails");
  fillSimple(settingsCache.power, "power");
  fillSimple(settingsCache.can, "can");
  fillSimple(settingsCache.wifi, "wifi");
}
(async () => {
  try { await loadAll(); } catch (e) { toast("cannot reach the adc2can"); }
  const start = location.hash.slice(1); // #sensors, #can ... opens that tab
  if (document.getElementById(start + "-page")) showPage(start);
  autoSave($("railsCard"), () => ({ rails: collectSimple("rails", ["faultPolicy", "faultHoldMs", "railLowV", "busHighV", "backfeedRetryS"]) }), "rails saved");
  autoSave($("powerCard"), () => ({ power: collectSimple("power", ["vbatLow", "vbatHigh", "no12vBelow"]) }), "power saved");
  autoSave($("canCard"), () => ({ can: collectSimple("can", ["kbps", "baseId", "boardId", "term", "txPeriodMs", "statusPeriodMs"]) }), "CAN saved");
  autoSave($("nowCard"), () => ({ wifi: { espNow: Number($("wifi.espNow").value), canRelay: Number($("wifi.canRelay").value) } }), "ESP-NOW saved");
  // the access point restarts on a new name / password, so only on leaving the field
  $("wifiCard").addEventListener("change", (e) => {
    if (!e.target.matches("input")) return;
    saveSettings({ wifi: { apSsid: $("wifi.apSsid").value, apPass: $("wifi.apPass").value } }, "WiFi restarting - rejoin the access point"); // blank password = open
  });
  await pollStatus();
  introSweep();
  setInterval(pollStatus, POLL_MS);
})();
