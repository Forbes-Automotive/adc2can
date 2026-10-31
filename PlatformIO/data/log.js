// adc2can web UI - the Logging tab: the recorder, the logs on the board (CSV / raw export) and the 10-minute trends.
// Talks to src/adc2can_log.cpp: GET/POST /api/log, GET /api/log/data?id=N (raw sectors), GET /api/trend?since=N.
// Uses the helpers in app.js ($, api, toast, esc, uShown, settingsCache).
(() => {
  let logCache = null;
  const isOpen = () => $("logging-page").classList.contains("active");

  // ---------------------------------------------------------------- formatting
  const hms = (ms) => { const s = Math.floor(ms / 1000), h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60; return (h ? h + ":" + String(m).padStart(2, "0") : m) + ":" + String(s % 60).padStart(2, "0"); };
  const span = (s) => (s >= 3600 ? Math.floor(s / 3600) + " h " + Math.floor(s / 60 % 60) + " min" : s >= 60 ? Math.floor(s / 60) + " min" : s + " s");
  const size = (b) => (b >= 1048576 ? (b / 1048576).toFixed(1) + " MB" : Math.round(b / 1024) + " kB");
  const n0 = (n) => Number(n).toLocaleString();
  const p2 = (x) => String(x).padStart(2, "0");
  const stamp = (d) => d.getFullYear() + "-" + p2(d.getMonth() + 1) + "-" + p2(d.getDate()) + "_" + p2(d.getHours()) + p2(d.getMinutes()); // local time

  // ---------------------------------------------------------------- recorder
  async function pollLog() {
    let L;
    try { L = await api("/api/log"); } catch (e) { return; }
    logCache = L;
    const sel = $("log.rateHz");
    if (!sel.options.length) sel.innerHTML = L.rates.map(r => `<option value="${r}">${r} Hz</option>`).join("");
    const idle = (el) => document.activeElement !== el;
    if (idle(sel)) sel.value = L.rateHz;
    if (idle($("log.autoStart"))) $("log.autoStart").value = L.autoStart;
    if (idle($("log.whenFull"))) $("log.whenFull").value = L.whenFull;
    sel.disabled = L.state === "recording";
    const rec = L.state === "recording";
    $("logStartBtn").textContent = rec ? "Stop Logging" : "Start Logging";
    $("logStartBtn").className = rec ? "btn danger" : "btn primary";
    $("logStartBtn").disabled = !L.available;
    $("logState").innerHTML = !L.available ? "No log memory: flash this firmware over USB once (it brings the new partition table)." :
      rec ? `<span class="rec-dot"></span>Recording log ${L.session} - ${hms(L.elapsedMs)}, ${n0(L.records)} records` :
      L.state === "full" ? "<b style='color:var(--danger)'>Stopped: the log memory is full</b> - export and delete logs, or set it to overwrite the oldest" : "Not logging";
    const total = L.sectors * 4096, used = L.usedSectors * 4096;
    $("memFill").style.width = (L.sectors ? (100 * L.usedSectors / L.sectors) : 0).toFixed(1) + "%";
    $("memInfo").textContent = size(used) + " of " + size(total) + " used" + (L.sectors ? " - room for " + span(L.secondsLeft) + " more at " + L.rateHz + " Hz" +
      (L.whenFull ? ", then the oldest data is overwritten" : "") + " (the whole memory holds " + span(Math.floor(L.sectors * L.recsPerSector / L.rateHz)) + ")" : "");
    const rows = L.logs.map(g => {
      const started = g.epochMs ? new Date(g.epochMs + g.startMs).toLocaleString() : "power-up (no clock)";
      return `<tr><td>${g.active ? '<span class="rec-dot"></span>' : ""}Log ${g.id}</td><td>${esc(started)}${g.startMs ? ' <span class="pill small warn">start overwritten</span>' : ""}</td>
        <td class="num">${hms(g.durationMs)}</td><td class="num">${Math.round(1000 / g.periodMs)} Hz</td><td class="num col-rec">${n0(g.records)}</td><td class="num">${size(g.bytes)}</td>
        <td><div class="log-acts"><button class="btn-secondary btn-sm" data-csv="${g.id}">CSV</button> <a class="btn-secondary btn-sm" href="/api/log/data?id=${g.id}" download="adc2can-log-${g.id}.a2c" style="text-decoration:none">Raw</a>
        <button class="btn-secondary btn-sm" data-del="${g.id}">Delete</button></div></td></tr>`;
    }).join("") || `<tr><td colspan="7" style="color:var(--muted)">No logs on the board.</td></tr>`;
    if ($("logRows")._html !== rows) {
      $("logRows").innerHTML = rows;
      $("logRows")._html = rows;
      $("logRows").querySelectorAll("[data-csv]").forEach(b => b.addEventListener("click", () => exportCsv(Number(b.dataset.csv), b)));
      $("logRows").querySelectorAll("[data-del]").forEach(b => b.addEventListener("click", async () => {
        if (!confirm("Delete log " + b.dataset.del + " from the board?")) return;
        const r = await api("/api/log", "POST", { action: "delete", id: Number(b.dataset.del) });
        toast(r.error || "log deleted"); setTimeout(pollLog, 300);
      }));
    }
    $("logDeleteAll").disabled = !L.logs.length;
  }
  $("logStartBtn").addEventListener("click", async () => {
    const rec = logCache && logCache.state === "recording";
    const r = await api("/api/log", "POST", rec ? { action: "stop" } : { action: "start", epochMs: Date.now() }); // the board has no clock: this page's
    toast(r.error || (rec ? "logging stopped" : "logging started"));
    setTimeout(pollLog, 300);
  });
  $("logDeleteAll").addEventListener("click", async () => {
    if (!confirm("Delete every log on the board? Export anything you want to keep first.")) return;
    const r = await api("/api/log", "POST", { action: "deleteAll" });
    toast(r.error || "all logs deleted"); setTimeout(pollLog, 300);
  });
  ["log.rateHz", "log.autoStart", "log.whenFull"].forEach(id => $(id).addEventListener("change", async () => {
    const r = await api("/api/log", "POST", { rateHz: Number($("log.rateHz").value), autoStart: Number($("log.autoStart").value), whenFull: Number($("log.whenFull").value) });
    toast(r.error || "saved"); pollLog();
  }));

  // ---------------------------------------------------------------- export: the raw sectors -> CSV (format: adc2can_log.cpp)
  const MAGIC = 0x4C433241, SEC = 4096, HDR = 256, REC = 40, RECS = 96;
  const cstr = (bytes) => { let s = ""; for (const b of bytes) { if (!b) break; s += String.fromCharCode(b); } return s; };
  async function exportCsv(id, btn) {
    btn.disabled = true; btn.textContent = "...";
    try {
      const res = await fetch("/api/log/data?id=" + id);
      if (!res.ok) throw new Error(await res.text());
      const buf = await res.arrayBuffer(), dv = new DataView(buf), u8 = new Uint8Array(buf);
      let meta = null;
      const out = [];
      for (let o = 0; o + SEC <= buf.byteLength; o += SEC) {
        if (dv.getUint32(o, true) !== MAGIC) continue;
        if (!meta) {
          meta = { epoch: Number(dv.getBigUint64(o + 16, true)), dec: [], names: [], units: [] };
          for (let i = 0; i < 8; i++) {
            meta.dec.push(Math.min(4, u8[o + 28 + i] + 1));
            meta.names.push(cstr(u8.subarray(o + 36 + i * 16, o + 52 + i * 16)));
            meta.units.push(cstr(u8.subarray(o + 164 + i * 8, o + 172 + i * 8)));
          }
          const q = (s) => '"' + s.replace(/"/g, '""') + '"';
          out.push(["time_s"].concat(meta.epoch ? ["date_time"] : [], meta.names.map((n, i) => q(n + (meta.units[i] ? " (" + meta.units[i] + ")" : ""))), ["battery_V", "valid_mask", "rail_fault_mask"]).join(",") + "\r\n");
        }
        for (let k = 0; k < RECS; k++) {
          const r = o + HDR + k * REC, t = dv.getUint32(r, true);
          if (t === 0xFFFFFFFF) break;
          const row = [(t / 1000).toFixed(3)];
          if (meta.epoch) { const d = new Date(meta.epoch + t); row.push(d.toLocaleDateString("en-CA") + " " + d.toLocaleTimeString("en-GB") + "." + String(d.getMilliseconds()).padStart(3, "0")); }
          for (let i = 0; i < 8; i++) { const v = dv.getFloat32(r + 4 + i * 4, true); row.push(Number.isFinite(v) ? v.toFixed(meta.dec[i]) : ""); }
          row.push((dv.getUint16(r + 38, true) / 1000).toFixed(2), u8[r + 36], u8[r + 37]);
          out.push(row.join(",") + "\r\n");
        }
      }
      if (!meta) throw new Error("the log is empty");
      const a = document.createElement("a");
      a.href = URL.createObjectURL(new Blob(out, { type: "text/csv" }));
      a.download = "adc2can-log-" + id + (meta.epoch ? "-" + stamp(new Date(meta.epoch)) : "") + ".csv";
      a.click();
      setTimeout(() => URL.revokeObjectURL(a.href), 10000);
      toast("exported " + n0(out.length - 1) + " records");
    } catch (e) { toast("export failed: " + e.message); }
    btn.disabled = false; btn.textContent = "CSV";
  }

  // ---------------------------------------------------------------- trends (one small chart per sensor)
  const WIN = 600; // points (1 s each)
  let tEnd = 0;    // seq after the newest point
  let tData = Array.from({ length: 8 }, () => []);
  const hover = Array(8).fill(null);
  async function pollTrend() {
    let r;
    try { r = await api("/api/trend?since=" + tEnd); } catch (e) { return; }
    if (r.seq < tEnd || r.from > tEnd) { tData = Array.from({ length: 8 }, () => []); } // board restarted / a gap: start over
    r.ch.forEach((a, i) => { tData[i] = tData[i].concat(a).slice(-WIN); });
    tEnd = r.seq;
    drawAll();
  }
  function buildTrends() {
    $("trendGrid").innerHTML = settingsCache.ch.map((c, i) => `<div class="trend" id="tr${i}">
        <div class="trend-head"><b>${esc(c.name)}${c.unit ? " (" + esc(uShown(c.unit)) + ")" : ""}</b><span class="now" id="trNow${i}">--</span></div>
        <div class="trend-sub" id="trSub${i}">--</div>
        <canvas id="trC${i}" aria-label="${esc(c.name)} over the last 10 minutes"></canvas>
        <div class="trend-tip" id="trTip${i}"></div></div>`).join("");
    settingsCache.ch.forEach((c, i) => {
      const cv = $("trC" + i);
      const move = (e) => { const b = cv.getBoundingClientRect(); hover[i] = (e.clientX - b.left) / b.width; draw(i); };
      cv.addEventListener("pointermove", move);
      cv.addEventListener("pointerdown", move);
      cv.addEventListener("pointerleave", () => { hover[i] = null; draw(i); });
    });
    buildTrends.names = JSON.stringify(settingsCache.ch.map(c => [c.name, c.unit, c.decimals, c.lowWarn, c.highWarn]));
  }
  function niceTicks(lo, hi) {
    if (!(hi > lo)) { lo -= 1; hi += 1; }
    const step = [1, 2, 2.5, 5, 10].map(m => m * Math.pow(10, Math.floor(Math.log10((hi - lo) / 3)))).find(x => (hi - lo) / x <= 4);
    const a = Math.floor(lo / step) * step, b = Math.ceil(hi / step) * step, t = [];
    for (let v = a; v <= b + step / 2; v += step) t.push(+v.toFixed(6));
    return t;
  }
  function css(name) { return getComputedStyle(document.documentElement).getPropertyValue(name).trim(); }
  function draw(i) {
    const cv = $("trC" + i); if (!cv) return;
    const c = settingsCache.ch[i], data = tData[i], dpr = window.devicePixelRatio || 1;
    const W = cv.clientWidth, H = cv.clientHeight;
    if (cv.width !== Math.round(W * dpr)) { cv.width = Math.round(W * dpr); cv.height = Math.round(H * dpr); }
    const g = cv.getContext("2d");
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, W, H);
    const line = css("--line"), muted = css("--muted"), accent = css("--accent"), warnC = css("--accent-2"), surf = css("--panel-2"), text = css("--text");
    const L = 40, R = 8, T = 6, B = 18, pw = W - L - R, ph = H - T - B;
    const vals = data.filter(v => v !== null);
    const dp = c.decimals;
    // header: now / min / avg / max over the window
    const last = [...data].reverse().find(v => v !== null);
    $("trNow" + i).textContent = last === undefined ? "--" : last.toFixed(dp) + (c.unit ? " " + uShown(c.unit) : "");
    $("trSub" + i).textContent = vals.length ? "10 min: min " + Math.min(...vals).toFixed(dp) + "  avg " + (vals.reduce((a, b) => a + b, 0) / vals.length).toFixed(dp) + "  max " + Math.max(...vals).toFixed(dp) : "no readings in the last 10 minutes";
    // y range: the data, plus a warning level when it sits close by
    let lo = vals.length ? Math.min(...vals) : 0, hi = vals.length ? Math.max(...vals) : 1;
    const pad = (hi - lo) * 0.5 || Math.abs(hi) * 0.1 || 1;
    [c.lowWarn, c.highWarn].forEach(w => { if (w !== null && w >= lo - pad && w <= hi + pad) { lo = Math.min(lo, w); hi = Math.max(hi, w); } });
    const ticks = niceTicks(lo, hi), y0 = ticks[0], y1 = ticks[ticks.length - 1];
    const X = (k) => L + pw * (k + (WIN - data.length)) / (WIN - 1), Y = (v) => T + ph * (1 - (v - y0) / (y1 - y0 || 1));
    // grid + axis labels (hairline, recessive)
    g.font = "10px Segoe UI, Roboto, system-ui, sans-serif";
    g.lineWidth = 1; g.strokeStyle = line; g.fillStyle = muted;
    g.textAlign = "right"; g.textBaseline = "middle";
    const tdp = Math.min(4, (String(+(ticks[1] - ticks[0]).toFixed(6)).split(".")[1] || "").length); // as many decimals as the step has
    ticks.forEach(v => { const y = Math.round(Y(v)) + 0.5; g.beginPath(); g.moveTo(L, y); g.lineTo(W - R, y); g.stroke(); g.fillText(v.toFixed(tdp), L - 6, y); });
    g.textAlign = "center"; g.textBaseline = "alphabetic";
    [10, 8, 6, 4, 2, 0].forEach(m => g.fillText(m ? "-" + m + " min" : "now", Math.min(W - R - 10, Math.max(L + 16, L + pw * (1 - m / 10))), H - 4));
    // warning levels
    g.setLineDash([4, 3]); g.strokeStyle = warnC;
    [c.lowWarn, c.highWarn].forEach(w => { if (w === null || w < y0 || w > y1) return; const y = Math.round(Y(w)) + 0.5; g.beginPath(); g.moveTo(L, y); g.lineTo(W - R, y); g.stroke(); });
    g.setLineDash([]);
    // the series: 2px, round joins, broken where there was no reading
    g.strokeStyle = accent; g.lineWidth = 2; g.lineJoin = "round"; g.lineCap = "round";
    g.beginPath();
    let pen = false;
    data.forEach((v, k) => { if (v === null) { pen = false; return; } const x = X(k), y = Y(v); if (pen) g.lineTo(x, y); else { g.moveTo(x, y); pen = true; } });
    g.stroke();
    if (!vals.length) { g.fillStyle = muted; g.textAlign = "center"; g.fillText("no readings", L + pw / 2, T + ph / 2); }
    // crosshair + tooltip: snaps to the nearest second
    const tip = $("trTip" + i);
    if (hover[i] === null || !data.length) { tip.style.display = "none"; return; }
    const k = Math.round(((hover[i] * W - L) / pw) * (WIN - 1)) - (WIN - data.length);
    if (k < 0 || k >= data.length) { tip.style.display = "none"; return; }
    const x = Math.round(X(k)) + 0.5, v = data[k], ago = data.length - 1 - k;
    g.strokeStyle = muted; g.lineWidth = 1; g.beginPath(); g.moveTo(x, T); g.lineTo(x, T + ph); g.stroke();
    if (v !== null) { g.fillStyle = surf; g.beginPath(); g.arc(x, Y(v), 6, 0, 7); g.fill(); g.fillStyle = accent; g.beginPath(); g.arc(x, Y(v), 4, 0, 7); g.fill(); }
    tip.innerHTML = `<span class="k">${ago ? "-" + Math.floor(ago / 60) + ":" + String(ago % 60).padStart(2, "0") : "now"}</span> &nbsp;<b style="color:${text}">${v === null ? "no reading" : v.toFixed(dp) + " " + esc(uShown(c.unit))}</b>`;
    tip.style.display = "block";
    const box = cv.parentNode.getBoundingClientRect(), cb = cv.getBoundingClientRect();
    tip.style.left = Math.min(box.width - tip.offsetWidth - 6, Math.max(6, cb.left - box.left + x + 10)) + "px";
    tip.style.top = (cb.top - box.top + 4) + "px";
  }
  function drawAll() {
    if (!isOpen() || !settingsCache) return;
    const key = JSON.stringify(settingsCache.ch.map(c => [c.name, c.unit, c.decimals, c.lowWarn, c.highWarn]));
    if (buildTrends.names !== key) buildTrends();
    for (let i = 0; i < 8; i++) draw(i);
  }
  window.addEventListener("resize", drawAll);

  // ---------------------------------------------------------------- polling while the tab is open
  window.logTabOpened = () => { pollLog(); pollTrend(); };
  let n = 0;
  setInterval(() => { if (!isOpen()) return; pollLog(); if (n++ % 2 === 0) pollTrend(); }, 1000);
  if (isOpen()) window.logTabOpened();
})();
