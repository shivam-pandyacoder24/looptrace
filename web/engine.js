/*
 * engine.js - glue between the web page and the LoopTrace C engine
 *
 *  - makeWasm(bytes): runs src/fraud.c compiled to WebAssembly (preferred)
 *  - analyzeJS(...):  a line-by-line JavaScript port of src/fraud.c, used
 *                     only if the browser refuses to run WebAssembly
 *  - prepare(rows):   maps account names to ids, drops invalid rows
 *  - generate(...):   synthetic network with planted laundering rings
 */
(function (root) {
  "use strict";

  const LIMITS = { accounts: 1024, transfers: 8192, cycles: 500, maxLen: 8, budget: 3000000 };
  const FLAG = { ORDERED: 1, FAST: 2, SIMILAR: 4, ROUND_TRIP: 8 };

  /* rows: [{from, to, amount, time}] -> {names, edges, rowOf, skipped} */
  function prepare(rows) {
    const ids = new Map(), names = [], edges = [], rowOf = [];
    let skipped = 0;
    rows.forEach((r, i) => {
      const from = String(r.from || "").trim(), to = String(r.to || "").trim();
      const amount = Number(r.amount), time = Number(r.time);
      if (!from || !to || !(amount > 0) || !Number.isFinite(time)) { skipped++; return; }
      if (edges.length >= LIMITS.transfers) { skipped++; return; }
      const need = (ids.has(from) ? 0 : 1) + (ids.has(to) || to === from ? 0 : 1);
      if (names.length + need > LIMITS.accounts) { skipped++; return; }
      for (const n of [from, to]) if (!ids.has(n)) { ids.set(n, names.length); names.push(n); }
      edges.push({ from: ids.get(from), to: ids.get(to), amount, time });
      rowOf.push(i);
    });
    return { names, edges, rowOf, skipped };
  }

  function clampParams(p) {
    return {
      maxLen: Math.max(2, Math.min(LIMITS.maxLen, p.maxLen | 0)),
      window: p.window > 0 ? p.window : 1,
      minAmount: p.minAmount || 0,
      chrono: p.chrono ? 1 : 0,
    };
  }

  /* ---------------- WebAssembly engine ---------------- */
  function makeWasm(bytes) {
    const mod = new WebAssembly.Module(bytes);
    const w = new WebAssembly.Instance(mod, {}).exports;
    return {
      name: "wasm",
      analyze(nAccounts, edges, params) {
        const p = clampParams(params);
        w.lt_reset();
        for (const e of edges) w.lt_add(e.from, e.to, e.amount, e.time);
        const n = w.lt_analyze(p.maxLen, p.window, p.minAmount, p.chrono);
        const nA = w.lt_accounts();
        const sccOf = new Int32Array(nAccounts), hits = new Int32Array(nAccounts), risk = new Int32Array(nAccounts);
        for (let v = 0; v < nA; v++) { sccOf[v] = w.lt_scc_of(v); hits[v] = w.lt_account_hits(v); risk[v] = w.lt_account_risk(v); }
        const nSccs = w.lt_sccs(), sccSize = new Int32Array(nSccs);
        for (let s = 0; s < nSccs; s++) sccSize[s] = w.lt_scc_size(s);
        const cycles = [];
        for (let i = 0; i < n; i++) {
          const len = w.lt_cycle_len(i), ids = [];
          for (let j = 0; j < len; j++) ids.push(w.lt_cycle_edge(i, j));
          cycles.push({ len, edges: ids, score: w.lt_cycle_score(i), flags: w.lt_cycle_flags(i),
                        span: w.lt_cycle_span(i), min: w.lt_cycle_min(i), total: w.lt_cycle_total(i) });
        }
        return { nSccs, sccOf, sccSize, cycles, hits, risk, steps: w.lt_steps(), truncated: !!w.lt_truncated() };
      },
    };
  }

  /* ---------------- JavaScript port of fraud.c ---------------- */
  function analyzeJS(nAccounts, edges, params) {
    const p = clampParams(params);
    const n = nAccounts, E = edges.length;
    const head = new Int32Array(n).fill(-1), next = new Int32Array(E);
    let maxTransfer = 0;
    for (let e = 0; e < E; e++) {
      next[e] = head[edges[e].from]; head[edges[e].from] = e;
      if (edges[e].amount > maxTransfer) maxTransfer = edges[e].amount;
    }
    const ok = (e) => edges[e].amount >= p.minAmount && edges[e].from !== edges[e].to;

    /* Tarjan */
    const index = new Int32Array(n).fill(-1), low = new Int32Array(n), onStack = new Uint8Array(n);
    const stack = [], sccOf = new Int32Array(n), sccSizeArr = [];
    let counter = 0;
    function strongconnect(v) {
      index[v] = low[v] = counter++;
      stack.push(v); onStack[v] = 1;
      for (let e = head[v]; e !== -1; e = next[e]) {
        if (!ok(e)) continue;
        const w = edges[e].to;
        if (index[w] === -1) { strongconnect(w); if (low[w] < low[v]) low[v] = low[w]; }
        else if (onStack[w] && index[w] < low[v]) low[v] = index[w];
      }
      if (low[v] === index[v]) {
        const id = sccSizeArr.length; let size = 0, w;
        do { w = stack.pop(); onStack[w] = 0; sccOf[w] = id; size++; } while (w !== v);
        sccSizeArr.push(size);
      }
    }
    for (let v = 0; v < n; v++) if (index[v] === -1) strongconnect(v);
    const sccSize = Int32Array.from(sccSizeArr);

    /* scoring (same weights as score_cycle in fraud.c) */
    function score(c) {
      let tmin = edges[c.edges[0]].time, tmax = tmin, ordered = true;
      c.min = c.max = edges[c.edges[0]].amount; c.total = 0; c.flags = 0;
      for (let i = 0; i < c.len; i++) {
        const t = edges[c.edges[i]];
        if (t.amount < c.min) c.min = t.amount;
        if (t.amount > c.max) c.max = t.amount;
        if (t.time < tmin) tmin = t.time;
        if (t.time > tmax) tmax = t.time;
        c.total += t.amount;
        if (i > 0 && t.time < edges[c.edges[i - 1]].time) ordered = false;
      }
      c.span = tmax - tmin;
      const similarity = c.min / c.max;
      const sAmount = 45 * similarity;
      if (similarity >= 0.85) c.flags |= FLAG.SIMILAR;
      let sTime = 0;
      if (ordered) {
        c.flags |= FLAG.ORDERED;
        if (c.span <= p.window) { c.flags |= FLAG.FAST; sTime = 35; }
        else sTime = 17.5 + 17.5 * p.window / c.span;
      }
      let sShape;
      if (c.len === 2) { sShape = 5; c.flags |= FLAG.ROUND_TRIP; }
      else if (c.len <= 5) sShape = 20; else sShape = 12;
      const volume = Math.sqrt(c.min / maxTransfer);
      const s = Math.floor((sAmount + sTime + sShape) * (0.4 + 0.6 * volume) + 0.5);
      c.score = Math.max(0, Math.min(100, s));
    }

    function chronoStart(path, len) {
      let drops = 0, start = 0;
      for (let i = 0; i < len; i++) {
        const prev = edges[path[(i + len - 1) % len]].time;
        if (edges[path[i]].time < prev) { drops++; start = i; }
      }
      return drops <= 1 ? start : -1;
    }

    const cycles = [], path = new Int32Array(LIMITS.maxLen), onPath = new Uint8Array(n);
    let steps = 0, stepLimit = 0, truncated = false;

    function record(len) {
      let first = chronoStart(path, len);
      if (first < 0) {
        first = 0;
        for (let i = 1; i < len; i++) if (edges[path[i]].time < edges[path[first]].time) first = i;
      }
      const c = { len, edges: [] };
      for (let i = 0; i < len; i++) c.edges.push(path[(first + i) % len]);
      score(c);
      if (cycles.length < LIMITS.cycles) { cycles.push(c); return; }
      truncated = true;
      let weakest = 0;
      for (let i = 1; i < cycles.length; i++) if (cycles[i].score < cycles[weakest].score) weakest = i;
      if (c.score > cycles[weakest].score) cycles[weakest] = c;
    }

    function dfs(start, v, depth, tmin, tmax, tlast, drops) {
      for (let e = head[v]; e !== -1; e = next[e]) {
        if (steps >= stepLimit) { truncated = true; return; }
        steps++;
        if (!ok(e)) continue;
        const w = edges[e].to;
        if (sccOf[w] !== sccOf[start]) continue;
        if (w !== start && (w < start || onPath[w])) continue;
        const t = edges[e].time;
        let nmin = tmin, nmax = tmax, nd = drops;
        if (depth === 0) { nmin = nmax = t; }
        else { if (t < nmin) nmin = t; if (t > nmax) nmax = t; if (t < tlast) nd++; }
        if (p.chrono && (nmax - nmin > p.window || nd > 1)) continue;
        path[depth] = e;
        if (w === start) {
          const wrap = depth > 0 && t > edges[path[0]].time ? 1 : 0;
          if (!p.chrono || nd + wrap <= 1) record(depth + 1);
        } else if (depth + 1 < p.maxLen) {
          onPath[w] = 1;
          dfs(start, w, depth + 1, nmin, nmax, t, nd);
          onPath[w] = 0;
        }
      }
    }

    let startsLeft = 0;
    for (let v = 0; v < n; v++) if (sccSize[sccOf[v]] >= 2) startsLeft++;
    for (let s = 0; s < n; s++) {
      if (sccSize[sccOf[s]] < 2) continue;
      stepLimit = steps + Math.trunc((LIMITS.budget - steps) / startsLeft--);
      onPath[s] = 1;
      dfs(s, s, 0, 0, 0, 0, 0);
      onPath[s] = 0;
    }

    /* heap sort, riskiest first (same as sort_cycles) */
    const worse = (a, b) => a.score !== b.score ? a.score < b.score : a.total < b.total;
    function sift(root, m) {
      for (;;) {
        let child = 2 * root + 1;
        if (child >= m) return;
        if (child + 1 < m && worse(cycles[child + 1], cycles[child])) child++;
        if (!worse(cycles[child], cycles[root])) return;
        [cycles[root], cycles[child]] = [cycles[child], cycles[root]];
        root = child;
      }
    }
    for (let i = (cycles.length >> 1) - 1; i >= 0; i--) sift(i, cycles.length);
    for (let end = cycles.length - 1; end > 0; end--) {
      [cycles[0], cycles[end]] = [cycles[end], cycles[0]];
      sift(0, end);
    }

    const hits = new Int32Array(n), risk = new Int32Array(n);
    for (const c of cycles)
      for (const e of c.edges) {
        const a = edges[e].from;
        hits[a]++;
        if (c.score > risk[a]) risk[a] = c.score;
      }
    return { nSccs: sccSize.length, sccOf, sccSize, cycles, hits, risk, steps, truncated };
  }

  /* ---------------- synthetic data (mirrors tools/gen_data.c) ---------------- */
  function generate(seed, accounts, transfers, rings) {
    let s = (seed >>> 0) || 1;
    const rnd = () => {             /* mulberry32 */
      s = (s + 0x6D2B79F5) >>> 0;
      let t = s;
      t = Math.imul(t ^ (t >>> 15), t | 1);
      t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
    const int = (lo, hi) => lo + Math.floor(rnd() * (hi - lo + 1));
    const name = (i) => "AC-" + String(1000 + i);
    const rows = [];
    for (let i = 0; i < transfers; i++) {
      let a = int(0, accounts - 2), b = int(a + 1, accounts - 1);
      if (rnd() < 0.05) [a, b] = [b, a];
      rows.push({ from: name(a), to: name(b), amount: Math.round(200 + rnd() * rnd() * 30000), time: Math.round(rnd() * 7200) / 10 });
    }
    for (let r = 0; r < rings; r++) {
      const len = int(3, 6), members = [];
      while (members.length < len) { const m = int(0, accounts - 1); if (!members.includes(m)) members.push(m); }
      let amount = 20000 + rnd() * 30000, t = rnd() * 650;
      for (let k = 0; k < len; k++) {
        rows.push({ from: name(members[k]), to: name(members[(k + 1) % len]), amount: Math.round(amount), time: Math.round(t * 10) / 10 });
        amount *= 0.96 + rnd() * 0.03;
        t += 1 + rnd() * 10;
      }
    }
    return rows;
  }

  root.LoopTraceEngine = { LIMITS, FLAG, prepare, makeWasm, analyzeJS, generate };
})(typeof globalThis !== "undefined" ? globalThis : this);
