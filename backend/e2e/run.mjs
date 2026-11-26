#!/usr/bin/env node
// End-to-end test: real C++ core clients (core/tools/lt_stdio, one process per
// device) bridged over WebSockets to this Worker running under `wrangler dev`.
// One command: `npm run e2e` (from backend/). Local only; keys are generated
// per run.
import { spawn, spawnSync } from "node:child_process";
import { generateKeyPairSync, randomBytes, sign } from "node:crypto";
import { existsSync, mkdirSync, rmSync } from "node:fs";
import { createServer } from "node:net";
import { dirname, join, resolve } from "node:path";
import { createInterface } from "node:readline";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const backendDir = resolve(here, "..");
const coreDir = resolve(backendDir, "../core");
const buildDir = join(coreDir, "build-e2e");
const binary = join(buildDir, "lt_stdio");
const tmpDir = join(here, ".tmp");
const BOT_ID = "7000000001";
const verbose = process.env.LT_E2E_VERBOSE === "1";

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (...a) => console.log(...a);
const results = [];
const metrics = {};

function check(name, ok, detail = "") {
  results.push({ name, ok, detail });
  log(`${ok ? "PASS" : "FAIL"}  ${name}${detail ? "  (" + detail + ")" : ""}`);
  return ok;
}

async function waitFor(cond, timeoutMs, step = 50) {
  const end = Date.now() + timeoutMs;
  while (Date.now() < end) {
    if (await cond()) return true;
    await sleep(step);
  }
  return Boolean(await cond());
}

// ---------------------------------------------------------------------------
// 1. Build the core bridge binary.

function buildCore() {
  const t0 = Date.now();
  const cfg = spawnSync(
    "cmake",
    ["-S", coreDir, "-B", buildDir, "-DCMAKE_BUILD_TYPE=Release", "-DLT_CORE_BUILD_TESTS=OFF"],
    { stdio: verbose ? "inherit" : "pipe" },
  );
  if (cfg.status !== 0) throw new Error("cmake configure failed: " + (cfg.stderr ?? ""));
  const b = spawnSync("cmake", ["--build", buildDir, "--target", "lt_stdio", "-j8"], { stdio: verbose ? "inherit" : "pipe" });
  if (b.status !== 0) throw new Error("cmake build failed: " + (b.stderr ?? ""));
  if (!existsSync(binary)) throw new Error("lt_stdio missing after build");
  log(`built core/build-e2e/lt_stdio in ${Date.now() - t0} ms`);
}

// ---------------------------------------------------------------------------
// 2. Start the Worker locally.

async function freePort() {
  return new Promise((res, rej) => {
    const s = createServer();
    s.listen(0, "127.0.0.1", () => {
      const port = s.address().port;
      s.close(() => res(port));
    });
    s.on("error", rej);
  });
}

async function startWorker(port, vars, persistDir) {
  const args = ["wrangler", "dev", "--local", "--ip", "127.0.0.1", "--port", String(port), "--persist-to", persistDir,
    "--show-interactive-dev-session=false", "--log-level", verbose ? "info" : "warn"];
  for (const [k, v] of Object.entries(vars)) args.push("--var", `${k}:${v}`);
  const proc = spawn("npx", args, {
    cwd: backendDir,
    env: { ...process.env, WRANGLER_SEND_METRICS: "false", NO_D1_WARNING: "true" },
    stdio: ["ignore", "pipe", "pipe"],
    detached: true,
  });
  let output = "";
  proc.stdout.on("data", (d) => { output += d; if (verbose) process.stdout.write("[wrangler] " + d); });
  proc.stderr.on("data", (d) => { output += d; if (verbose) process.stderr.write("[wrangler] " + d); });
  const t0 = Date.now();
  const up = await waitFor(async () => {
    try {
      const r = await fetch(`http://127.0.0.1:${port}/health`);
      return r.ok && (await r.text()) === "ok";
    } catch {
      return false;
    }
  }, 90000, 250);
  if (!up) throw new Error("wrangler dev did not come up:\n" + output.slice(-3000));
  log(`wrangler dev --local up on :${port} in ${Date.now() - t0} ms`);
  return proc;
}

function stopWorker(proc) {
  if (!proc || proc.exitCode !== null) return;
  try { process.kill(-proc.pid, "SIGTERM"); } catch { /* gone */ }
  setTimeout(() => { try { process.kill(-proc.pid, "SIGKILL"); } catch { /* gone */ } }, 3000).unref();
}

// ---------------------------------------------------------------------------
// 3. Telegram-like initData signed with a throwaway Ed25519 key.

function makeInitData(privateKey, userId, firstName) {
  const params = new URLSearchParams();
  params.set("query_id", "AAE2E" + userId);
  params.set("user", JSON.stringify({ id: userId, first_name: firstName, username: "e2e" + userId }));
  params.set("auth_date", String(Math.floor(Date.now() / 1000)));
  const pairs = [...params.entries()].sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
  const dcs = `${BOT_ID}:WebAppData\n` + pairs.map(([k, v]) => `${k}=${v}`).join("\n");
  params.set("signature", sign(null, Buffer.from(dcs), privateKey).toString("base64url"));
  return params.toString();
}

async function http(base, path, body, token) {
  const headers = { "content-type": "application/json" };
  if (token) headers.authorization = `Bearer ${token}`;
  const r = await fetch(base + path, { method: "POST", headers, body: JSON.stringify(body) });
  const j = await r.json();
  if (!r.ok) throw new Error(`${path} -> ${r.status} ${JSON.stringify(j)}`);
  return j;
}

// ---------------------------------------------------------------------------
// 4. A device: lt_stdio process + WebSocket bridge.

class Device {
  constructor(name, wsUrl, token, opts = {}) {
    this.name = name;
    this.wsUrl = wsUrl;
    this.state = null;
    this.errors = [];
    this.players = [];
    this.connects = 0;
    this.ws = null;
    this.offlineUntil = 0;
    this.cmdId = 0;
    this.pending = new Map();
    this.stateWaiters = [];
    this.proc = spawn(binary, [], { stdio: ["pipe", "pipe", "inherit"] });
    createInterface({ input: this.proc.stdout }).on("line", (l) => this.onLine(l));
    this.write({
      t: "init",
      device_id: `e2e-${name}`,
      token,
      seed: Math.floor(Math.random() * 1e9),
      load_ms: opts.loadMs ?? 120,
      start_ms: opts.startMs ?? 30,
      protocol_version: opts.protocolVersion ?? 0,
    });
    this.write({ t: "start" });
  }

  write(o) {
    if (this.proc.exitCode === null) this.proc.stdin.write(JSON.stringify(o) + "\n");
  }

  onLine(line) {
    let m;
    try { m = JSON.parse(line); } catch { return; }
    switch (m.ev) {
      case "connect": this.openSocket(); break;
      case "disconnect": {
        const ws = this.ws;
        this.ws = null;
        try { ws?.close(1000); } catch { /* ignore */ }
        break;
      }
      case "send":
        if (this.ws && this.ws.readyState === 1) this.ws.send(m.data);
        break;
      case "state": {
        this.state = m;
        if (m.why === "request") {
          const w = this.stateWaiters;
          this.stateWaiters = [];
          for (const f of w) f(m);
        }
        break;
      }
      case "error": this.errors.push(m); if (verbose) log(`[${this.name}] error`, m.code, m.fatal); break;
      case "player": this.players.push({ ...m, at: Date.now() }); break;
      case "result": this.pending.get(m.id)?.(m); this.pending.delete(m.id); break;
      default: break;
    }
  }

  openSocket() {
    this.connects++;
    if (Date.now() < this.offlineUntil) {
      setTimeout(() => this.write({ t: "close", code: 1006 }), 300);
      return;
    }
    const ws = new WebSocket(this.wsUrl);
    this.ws = ws;
    ws.addEventListener("open", () => { if (this.ws === ws) this.write({ t: "open" }); });
    ws.addEventListener("message", (e) => { if (this.ws === ws) this.write({ t: "msg", data: String(e.data) }); });
    ws.addEventListener("close", (e) => { if (this.ws === ws) { this.ws = null; this.write({ t: "close", code: e.code }); } });
    ws.addEventListener("error", () => { /* a close event follows */ });
  }

  /** Simulates losing the network for `ms`: the socket dies and reconnects fail meanwhile. */
  dropNetwork(ms) {
    this.offlineUntil = Date.now() + ms;
    const ws = this.ws;
    this.ws = null;
    try { ws?.close(1000); } catch { /* ignore */ }
    this.write({ t: "close", code: 1006 });
  }

  cmd(name, args = {}) {
    const id = ++this.cmdId;
    return new Promise((res) => {
      this.pending.set(id, res);
      this.write({ t: "cmd", id, name, ...args });
    });
  }

  snapshot() {
    return new Promise((res) => {
      this.stateWaiters.push(res);
      this.write({ t: "state" });
    });
  }

  get live() { return this.state?.state?.conn === "live"; }
  get rev() { return this.state?.state?.rev ?? -1; }
  get queue() { return (this.state?.state?.queue ?? []).map((e) => e.item_id); }

  kill() { try { this.proc.stdin.end(); this.proc.kill(); } catch { /* ignore */ } try { this.ws?.close(); } catch { /* ignore */ } }
}

const snaps = (devs) => Promise.all(devs.map((d) => d.snapshot()));
const allAudible = async (devs) => (await snaps(devs)).every((x) => x.player.audible);
const noneAudible = async (devs) => (await snaps(devs)).every((x) => !x.player.audible);

/** Positions of audible devices normalized to one server instant; returns spread and details. */
async function syncSpread(devices) {
  const snaps = await Promise.all(devices.map((d) => d.snapshot()));
  const tref = Math.max(...snaps.map((s) => s.server_now));
  const items = new Set(snaps.map((s) => s.player.item));
  const audible = snaps.every((s) => s.player.audible);
  const norm = snaps.map((s) => s.player.pos + (tref - s.server_now));
  const spread = Math.max(...norm) - Math.min(...norm);
  return { spread, audible, sameItem: items.size === 1, item: snaps[0].player.item, pos: norm[0], snaps };
}

async function sampleSpread(devices, n = 5, gap = 300) {
  let worst = 0;
  let ok = true;
  let info = null;
  for (let i = 0; i < n; i++) {
    info = await syncSpread(devices);
    ok = ok && info.audible && info.sameItem;
    worst = Math.max(worst, info.spread);
    await sleep(gap);
  }
  return { worst, ok, info };
}

const entry = (title, dur, extra = {}) => ({
  chat_id: "-1001234567890", msg_id: Math.floor(Math.random() * 1e6), doc_id: String(5e18 + Math.floor(Math.random() * 1e6)),
  title, performer: "E2E", duration_ms: dur, ...extra,
});

// ---------------------------------------------------------------------------

async function main() {
  const tStart = Date.now();
  buildCore();
  mkdirSync(tmpDir, { recursive: true });
  const persistDir = join(tmpDir, `state-${process.pid}`);
  const port = await freePort();
  const { publicKey, privateKey } = generateKeyPairSync("ed25519");
  const pubHex = Buffer.from(publicKey.export({ format: "jwk" }).x, "base64url").toString("hex");
  const worker = await startWorker(port, {
    TELEGRAM_PUBLIC_KEY: pubHex,
    TOKEN_SECRET: randomBytes(32).toString("hex"),
    BOT_ID,
  }, persistDir);
  const base = `http://127.0.0.1:${port}`;
  const devices = [];
  try {
    // Accounts, session, invites.
    const users = [[9100001, "Ada"], [9100002, "Bo"], [9100003, "Cy"]];
    const tokens = [];
    for (const [id, name] of users) tokens.push((await http(base, "/v1/auth", { init_data: makeInitData(privateKey, id, name) })).token);
    const { session_id: sid } = await http(base, "/v1/sessions", { host_chat_id: "-1001234567890", title: "E2E" }, tokens[0]);
    for (const t of tokens.slice(1)) {
      const inv = await http(base, `/v1/sessions/${sid}/invites`, {}, tokens[0]);
      check("invite link format", /^https:\/\/t\.me\/TunegramSyncBot\/listen\?startapp=[A-Za-z0-9_-]{43}$/.test(inv.link));
      await http(base, "/v1/invites/redeem", { invite_token: inv.invite_token }, t);
    }
    const wsUrl = `ws://127.0.0.1:${port}/v1/sessions/${sid}/ws`;
    const [A, B, C] = ["A", "B", "C"].map((n, i) => new Device(n, wsUrl, tokens[i]));
    devices.push(A, B, C);
    const all = [A, B, C];

    check("all devices join", await waitFor(() => all.every((d) => d.live && d.state.state.participants.length === 3), 10000));
    const t0 = Date.now();

    // Queue + play.
    const ins = await A.cmd("insert", { after: null, entries: [entry("One", 20000), entry("Two", 20000, { chat_username: "e2e_channel" }), entry("Three", 20000)] });
    const ids = ins.ids;
    check("queue converges after insert", await waitFor(() => all.every((d) => JSON.stringify(d.queue) === JSON.stringify(ids)), 5000));
    check("chat_username replicated", JSON.stringify(C.state.state.queue[1]).includes('"chat_username":"e2e_channel"'));
    await A.cmd("play");
    check("everyone starts playing", await waitFor(() => allAudible(all), 5000));
    await sleep(1000);
    let s = await sampleSpread(all);
    metrics.spread_after_play_ms = s.worst;
    check("in sync after play", s.ok && s.worst < 150, `max spread ${s.worst} ms over 5 samples`);

    // Seek by another device.
    await C.cmd("seek", { pos: 8000 });
    await sleep(1500);
    s = await sampleSpread(all, 3);
    metrics.spread_after_seek_ms = s.worst;
    check("seek applied everywhere", s.ok && s.info.pos > 8800 && s.info.pos < 12000 && s.worst < 150, `pos ${Math.round(s.info.pos)} spread ${s.worst} ms`);

    // Pause: measure propagation to the others.
    const before = all.map((d) => d.players.length);
    const tPause = Date.now();
    await B.cmd("pause");
    check("pause reaches everyone", await waitFor(() => noneAudible(all), 3000));
    const lat = [A, C].map((d, i) => {
      const ev = d.players.slice(before[i === 0 ? 0 : 2]).find((p) => p.what === "pause");
      return ev ? ev.at - tPause : NaN;
    });
    metrics.pause_propagation_ms = lat;
    const parked = (await Promise.all(all.map((d) => d.snapshot()))).map((x) => x.player.pos);
    check("everyone parks at the same position", Math.max(...parked) - Math.min(...parked) <= 50, `positions ${parked.join(", ")}; propagation ${lat.join("/")} ms`);

    // Concurrent edits from all three at once.
    await Promise.all([
      A.cmd("insert", { after: ids[0], entries: [entry("A1", 20000), entry("A2", 20000)] }),
      B.cmd("remove", { item_ids: [ids[2]] }),
      C.cmd("move", { item_id: ids[1], after: null }),
    ]);
    const converged = await waitFor(() => {
      const q = JSON.stringify(A.queue);
      return all.every((d) => JSON.stringify(d.queue) === q && d.rev === A.rev && d.state.state.pending === 0);
    }, 5000);
    check("concurrent edits converge", converged, `queue ${A.queue.length} items, rev ${A.rev}`);

    // Resume, then C loses the network while others keep changing things.
    await A.cmd("play");
    await sleep(800);
    C.dropNetwork(6000);
    await sleep(500);
    check("C notices the drop", await waitFor(() => C.state?.state?.conn !== "live", 3000), C.state?.state?.conn);
    await A.cmd("next");
    await B.cmd("insert", { after: null, entries: [entry("B1", 20000)] });
    await sleep(1000);
    const caught = await waitFor(() => C.live && C.rev === A.rev && C.state.state.pending === 0, 20000);
    metrics.caught_up_after_network_back_ms = Date.now() - C.offlineUntil;
    check("C reconnects and catches up", caught, `rev C=${C.rev} A=${A.rev}`);
    check("C converges on the same queue", JSON.stringify(C.queue) === JSON.stringify(A.queue));
    await sleep(1500);
    s = await sampleSpread(all, 3);
    metrics.spread_after_reconnect_ms = s.worst;
    if (!check("C plays at the live position", s.ok && s.worst < 150, `spread ${s.worst} ms`)) {
      for (const [i, d] of all.entries()) {
        log(`  ${d.name}: item=${s.info.snaps[i].player.item} pos=${s.info.snaps[i].player.pos} audible=${s.info.snaps[i].player.audible} now_playing=${JSON.stringify(s.info.snaps[i].state.now_playing)}`);
        log(`     players: ${JSON.stringify(d.players.slice(-6).map((p) => [p.what, p.item.slice(-6), p.pos, p.at - t0]))}`);
      }
    }

    // Automatic advance at the end of a track.
    const cur = A.state.state.now_playing.item_id;
    const dur = A.state.state.now_playing.duration_ms;
    await A.cmd("seek", { pos: dur - 1500 });
    const advanced = await waitFor(async () => (await snaps(all)).every((x) => x.state.now_playing.item_id !== cur && x.player.audible && x.player.item !== cur), 6000);
    check("automatic advance to the next track", advanced);
    await sleep(1000);
    s = await sampleSpread(all, 3);
    check("in sync after advance", s.ok && s.worst < 150, `spread ${s.worst} ms`);

    // Local interruption by other audio: others continue, B rejoins by itself.
    await B.cmd("interruption_began", { reason: "other_audio" });
    const seen = await waitFor(() => A.state?.state?.participants?.some((p) => p.user_id === "9100002" && p.state === "interrupted" && p.reason === "other_audio"), 3000);
    check("others see B interrupted (other_audio)", seen);
    const [sa, sb, sc] = await snaps([A, B, C]);
    check("others keep playing", sa.player.audible && sc.player.audible && !sb.player.audible);
    await sleep(1500);
    await B.cmd("interruption_ended");
    check("B rejoins without a tap", await waitFor(() => allAudible([B]), 3000));
    await sleep(1000);
    s = await sampleSpread(all, 3);
    check("B back in sync", s.ok && s.worst < 150, `spread ${s.worst} ms`);

    // Protocol version mismatch.
    const D = new Device("D", wsUrl, tokens[1], { protocolVersion: 2 });
    devices.push(D);
    const gotErr = await waitFor(() => D.errors.some((e) => e.code === "version_unsupported" && e.fatal), 5000);
    await sleep(2000);
    check("version mismatch is reported and terminal", gotErr && D.state?.state?.conn === "closed" && D.connects === 1, `connects ${D.connects}`);

    // End of session.
    await A.cmd("end");
    const ended = await waitFor(async () => [B, C].every((d) => d.errors.some((e) => e.code === "session_ended")) && (await noneAudible([B, C])), 5000);
    check("creator ends the session for everyone", ended);

    metrics.scenario_ms = Date.now() - t0;
  } finally {
    for (const d of devices) d.kill();
    stopWorker(worker);
    await sleep(300);
    try { rmSync(persistDir, { recursive: true, force: true }); } catch { /* ignore */ }
  }
  const failed = results.filter((r) => !r.ok).length;
  log(`\n${results.length - failed}/${results.length} checks passed in ${Date.now() - tStart} ms total`);
  log("metrics:", JSON.stringify(metrics));
  process.exit(failed ? 1 : 0);
}

main().catch((e) => {
  console.error("e2e crashed:", e);
  process.exit(2);
});
