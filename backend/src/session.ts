// One Durable Object per Listen Together session: the single writer that
// sequences ops into the log (PROTOCOL.md §4–§7, §12–§15). It validates shape
// and permissions, stamps rev/ts/by, persists, and broadcasts. It never
// interprets queue or playback semantics.
import { DurableObject } from "cloudflare:workers";

import { verifyToken } from "./auth";
import type { Env } from "./env";
import {
  CREATOR_KINDS,
  HELLO_DEADLINE_MS,
  LOG_CHUNK,
  MAX_FRAME,
  RATE_BURST,
  RATE_PER_S,
  checkOp,
  type Json,
  type Rec,
} from "./protocol";
import { b64urlDecode, b64urlEncode, intVar, randomBytes, sha256Hex } from "./util";

interface Meta {
  session_id: string;
  creator: string;
  host_chat_id: string;
  title: string;
  created_at: number;
  status: "active" | "ended";
  ended_at: number;
  idle_since: number; // 0 while someone is connected
}

interface Member {
  name: string;
  kicked?: boolean;
}

interface Invite {
  exp: number;
  created_by: string;
  used_by: string | null;
}

interface Attachment {
  authed: boolean;
  user: string;
  name: string;
  device: string;
  state: "active" | "interrupted";
  reason: "" | "call" | "other_audio";
  hello_by: number;
  tokens: number;
  refill_at: number;
}

const OP_KEY = (rev: number) => "op:" + String(rev).padStart(10, "0");

export type RpcResult<T> = { ok: true; value: T } | { ok: false; status: number; code: string };

export class SessionDO extends DurableObject<Env> {
  private meta: Meta | null = null;
  private members = new Map<string, Member>();
  private participants: string[] = [];
  private log: string[] = []; // serialized records, index = rev - 1
  private opIds = new Map<string, number>();

  constructor(ctx: DurableObjectState, env: Env) {
    super(ctx, env);
    ctx.blockConcurrencyWhile(() => this.load());
  }

  // -------------------------------------------------------------------------
  // Config

  private get maxParticipants() {
    return intVar(this.env.MAX_PARTICIPANTS, 10);
  }
  private get maxLog() {
    return intVar(this.env.MAX_LOG_OPS, 20000);
  }
  private get idleEndMs() {
    return intVar(this.env.IDLE_END_S, 6 * 3600) * 1000;
  }
  private get retentionMs() {
    return intVar(this.env.RETENTION_S, 24 * 3600) * 1000;
  }
  private get minV() {
    return intVar(this.env.MIN_PROTOCOL_VERSION, 1);
  }
  private get maxV() {
    return intVar(this.env.MAX_PROTOCOL_VERSION, 1);
  }

  // -------------------------------------------------------------------------
  // Persistence

  private async load(): Promise<void> {
    const st = this.ctx.storage;
    this.meta = (await st.get<Meta>("meta")) ?? null;
    if (!this.meta) return;
    this.members = new Map(Object.entries((await st.get<Record<string, Member>>("members")) ?? {}));
    this.participants = (await st.get<string[]>("participants")) ?? [];
    const ops = await st.list<string>({ prefix: "op:" });
    this.log = [...ops.values()];
    this.opIds.clear();
    this.log.forEach((r, i) => {
      const id = (JSON.parse(r) as { op_id?: string }).op_id;
      if (id) this.opIds.set(id, i + 1);
    });
  }

  private saveMembership(): Record<string, unknown> {
    return { members: Object.fromEntries(this.members), participants: this.participants };
  }

  /** Stamps, persists and broadcasts one record. Returns its rev. */
  private append(fields: Rec, by: string, extraPuts: Record<string, unknown> = {}): number {
    const rev = this.log.length + 1;
    const now = Date.now();
    const rec: Rec = { rev, ts: now, by };
    for (const [k, v] of Object.entries(fields)) {
      if (k !== "rev" && k !== "ts" && k !== "by" && k !== "seed" && k !== "type") rec[k] = v;
    }
    if (typeof rec.op_id !== "string" || rec.op_id === "") rec.op_id = `s:${rev}`;
    if (rec.kind === "set_shuffle" && rec.on === true) {
      rec.seed = crypto.getRandomValues(new Uint32Array(1))[0] ?? 0;
    }
    const s = JSON.stringify(rec);
    this.log.push(s);
    this.opIds.set(rec.op_id as string, rev);
    // Storage writes are covered by the output gate: nothing below reaches a
    // client before the write is durable.
    void this.ctx.storage.put({ [OP_KEY(rev)]: s, ...extraPuts });
    this.broadcastRaw(`{"type":"event","server_time":${now},"op":${s}}`);
    return rev;
  }

  // -------------------------------------------------------------------------
  // RPC from the Worker

  async init(sessionId: string, creator: string, name: string, hostChatId: string, title: string): Promise<RpcResult<null>> {
    if (this.meta) return { ok: false, status: 409, code: "conflict" };
    const now = Date.now();
    this.meta = {
      session_id: sessionId,
      creator,
      host_chat_id: hostChatId,
      title,
      created_at: now,
      status: "active",
      ended_at: 0,
      idle_since: now,
    };
    this.members.set(creator, { name });
    this.append({ kind: "init", host_chat_id: hostChatId, title }, creator, { meta: this.meta, ...this.saveMembership() });
    await this.scheduleAlarm();
    return { ok: true, value: null };
  }

  async createInvite(user: string, ttlS: number): Promise<RpcResult<{ secret: string; expires_at: number }>> {
    if (!this.meta) return { ok: false, status: 404, code: "not_found" };
    if (this.meta.status === "ended") return { ok: false, status: 410, code: "session_ended" };
    const m = this.members.get(user);
    if (!m || m.kicked) return { ok: false, status: 403, code: "forbidden" };
    const secret = randomBytes(16);
    const exp = Date.now() + Math.max(60, Math.min(86400, Math.floor(ttlS))) * 1000;
    const inv: Invite = { exp, created_by: user, used_by: null };
    await this.ctx.storage.put("inv:" + (await sha256Hex(secret)), inv);
    return { ok: true, value: { secret: b64urlEncode(secret), expires_at: exp } };
  }

  async redeem(user: string, name: string, secretB64: string): Promise<RpcResult<{ session_id: string; host_chat_id: string; title: string }>> {
    if (!this.meta) return { ok: false, status: 404, code: "invite_invalid" };
    const secret = b64urlDecode(secretB64);
    if (!secret || secret.length !== 16) return { ok: false, status: 404, code: "invite_invalid" };
    const key = "inv:" + (await sha256Hex(secret));
    const inv = await this.ctx.storage.get<Invite>(key);
    if (!inv) return { ok: false, status: 404, code: "invite_invalid" };
    if (inv.used_by !== null) return { ok: false, status: 410, code: "invite_used" };
    if (inv.exp < Date.now()) return { ok: false, status: 410, code: "invite_expired" };
    if (this.meta.status === "ended") return { ok: false, status: 410, code: "session_ended" };
    const existing = this.members.get(user);
    if (existing?.kicked) return { ok: false, status: 403, code: "kicked" };
    if (!this.participants.includes(user) && this.participants.length >= this.maxParticipants) {
      return { ok: false, status: 409, code: "session_full" };
    }
    inv.used_by = user;
    this.members.set(user, { name: name || existing?.name || "" });
    await this.ctx.storage.put({ [key]: inv, ...this.saveMembership() });
    return {
      ok: true,
      value: { session_id: this.meta.session_id, host_chat_id: this.meta.host_chat_id, title: this.meta.title },
    };
  }

  // -------------------------------------------------------------------------
  // WebSocket

  override async fetch(req: Request): Promise<Response> {
    if (req.headers.get("upgrade")?.toLowerCase() !== "websocket") {
      return new Response("expected websocket", { status: 426 });
    }
    const pair = new WebSocketPair();
    const [client, server] = [pair[0], pair[1]];
    this.ctx.acceptWebSocket(server);
    const now = Date.now();
    const att: Attachment = {
      authed: false,
      user: "",
      name: "",
      device: "",
      state: "active",
      reason: "",
      hello_by: now + HELLO_DEADLINE_MS,
      tokens: RATE_BURST,
      refill_at: now,
    };
    server.serializeAttachment(att);
    await this.scheduleAlarm();
    return new Response(null, { status: 101, webSocket: client });
  }

  private att(ws: WebSocket): Attachment {
    return ws.deserializeAttachment() as Attachment;
  }

  private sockets(except?: WebSocket): WebSocket[] {
    return this.ctx.getWebSockets().filter((w) => w !== except && w.readyState === WebSocket.OPEN);
  }

  private authed(except?: WebSocket): WebSocket[] {
    return this.sockets(except).filter((w) => this.att(w).authed);
  }

  private send(ws: WebSocket, msg: Record<string, unknown> | string): void {
    try {
      ws.send(typeof msg === "string" ? msg : JSON.stringify(msg));
    } catch {
      // socket already closing
    }
  }

  private broadcastRaw(s: string, except?: WebSocket): void {
    for (const w of this.authed(except)) this.send(w, s);
  }

  private fatal(ws: WebSocket, code: string, closeCode: number, message?: string, extra: Record<string, unknown> = {}) {
    this.send(ws, { type: "error", code, message: message ?? code, fatal: true, ...extra });
    try {
      ws.close(closeCode, code);
    } catch {
      // already closed
    }
  }

  private userOnline(user: string, except?: WebSocket): boolean {
    return this.authed(except).some((w) => this.att(w).user === user);
  }

  private presence(a: Attachment, online: boolean, except?: WebSocket): void {
    this.broadcastRaw(
      JSON.stringify({
        type: "presence",
        user_id: a.user,
        device_id: a.device,
        online,
        state: a.state,
        ...(a.state === "interrupted" && a.reason ? { reason: a.reason } : {}),
        server_time: Date.now(),
      }),
      except,
    );
  }

  private sendLog(ws: WebSocket, since: number): void {
    const head = this.log.length;
    let from = Number.isSafeInteger(since) && since >= 0 && since <= head ? since : 0;
    do {
      const to = Math.min(head, from + LOG_CHUNK);
      const ops = this.log.slice(from, to).join(",");
      this.send(
        ws,
        `{"type":"log","since_rev":${from},"head_rev":${head},"server_time":${Date.now()},"more":${to < head},"ops":[${ops}]}`,
      );
      from = to;
    } while (from < head);
  }

  override async webSocketMessage(ws: WebSocket, message: string | ArrayBuffer): Promise<void> {
    if (typeof message !== "string") {
      this.send(ws, { type: "error", code: "bad_request", message: "text frames only", fatal: false });
      return;
    }
    if (message.length > MAX_FRAME) {
      this.send(ws, { type: "error", code: "too_large", message: "frame too large", fatal: false });
      return;
    }
    let m: Record<string, unknown>;
    try {
      const v: unknown = JSON.parse(message);
      if (!v || typeof v !== "object" || Array.isArray(v)) throw new Error("not an object");
      m = v as Record<string, unknown>;
    } catch {
      this.send(ws, { type: "error", code: "bad_request", message: "invalid JSON", fatal: false });
      return;
    }
    const a = this.att(ws);
    if (!a.authed) {
      await this.hello(ws, m);
      return;
    }
    switch (m.type) {
      case "ping": {
        const now = Date.now();
        this.send(ws, { type: "pong", t0: m.t0 ?? 0, t1: now, t2: now, head_rev: this.log.length });
        return;
      }
      case "sync":
        this.sendLog(ws, typeof m.since_rev === "number" ? m.since_rev : 0);
        return;
      case "status":
        a.state = m.state === "interrupted" ? "interrupted" : "active";
        a.reason = a.state === "interrupted" && (m.reason === "call" || m.reason === "other_audio") ? m.reason : "";
        ws.serializeAttachment(a);
        this.presence(a, true);
        return;
      case "op":
        await this.op(ws, a, m);
        return;
      default:
        this.send(ws, { type: "error", code: "bad_request", message: "unknown type", fatal: false });
    }
  }

  private async hello(ws: WebSocket, m: Record<string, unknown>): Promise<void> {
    if (m.type !== "hello") return this.fatal(ws, "unauthorized", 4001, "hello first");
    const v = m.v;
    if (typeof v !== "number" || !Number.isInteger(v) || v < this.minV || v > this.maxV) {
      return this.fatal(ws, "version_unsupported", 4009, "Please update the app", { min_v: this.minV, max_v: this.maxV });
    }
    const nowMs = Date.now();
    const tok = await verifyToken(this.env, m.token, Math.floor(nowMs / 1000));
    if (!tok) return this.fatal(ws, "unauthorized", 4001, "invalid or expired token");
    if (!this.meta) return this.fatal(ws, "not_found", 4004);
    if (this.meta.status === "ended") return this.fatal(ws, "session_ended", 4011);
    const member = this.members.get(tok.sub);
    if (!member) return this.fatal(ws, "not_member", 4003);
    if (member.kicked) return this.fatal(ws, "kicked", 4012);
    const device = m.device_id;
    if (typeof device !== "string" || device.length < 1 || device.length > 64) {
      return this.fatal(ws, "unauthorized", 4001, "device_id");
    }
    const user = tok.sub;
    if (!this.participants.includes(user)) {
      if (this.participants.length >= this.maxParticipants) return this.fatal(ws, "session_full", 4010);
      this.participants.push(user);
      this.append({ kind: "join", name: tok.name ?? member.name }, user, this.saveMembership());
    }
    const wasOnline = this.userOnline(user, ws);
    const a = this.att(ws);
    a.authed = true;
    a.user = user;
    a.name = tok.name ?? member.name;
    a.device = device;
    ws.serializeAttachment(a);
    const online = this.authed(ws).map((w) => {
      const o = this.att(w);
      return {
        user_id: o.user,
        device_id: o.device,
        state: o.state,
        ...(o.state === "interrupted" && o.reason ? { reason: o.reason } : {}),
      };
    });
    const now = Date.now();
    const welcome: Record<string, unknown> = {
      type: "welcome",
      v,
      session_id: this.meta.session_id,
      user_id: user,
      creator: this.meta.creator,
      head_rev: this.log.length,
      server_time: now,
      t1: now,
      t2: now,
      max_participants: this.maxParticipants,
      online,
    };
    if (typeof m.t0 === "number") welcome.t0 = m.t0;
    this.send(ws, welcome);
    this.sendLog(ws, typeof m.since_rev === "number" ? m.since_rev : 0);
    if (!wasOnline) this.presence(a, true, ws);
    if (this.meta.idle_since !== 0) {
      this.meta.idle_since = 0;
      await this.ctx.storage.put("meta", this.meta);
    }
    await this.scheduleAlarm();
  }

  private async op(ws: WebSocket, a: Attachment, m: Record<string, unknown>): Promise<void> {
    const opId = typeof m.op_id === "string" ? m.op_id : "";
    const reject = (code: string, message?: string) =>
      this.send(ws, { type: "reject", op_id: opId, code, message: message ?? code });

    // Rate limit (token bucket kept in the hibernation-safe attachment).
    const now = Date.now();
    a.tokens = Math.min(RATE_BURST, a.tokens + ((now - a.refill_at) / 1000) * RATE_PER_S);
    a.refill_at = now;
    if (a.tokens < 1) {
      ws.serializeAttachment(a);
      return reject("rate_limited");
    }
    a.tokens -= 1;
    ws.serializeAttachment(a);

    const dup = this.opIds.get(opId);
    if (dup !== undefined) {
      this.send(ws, { type: "ack", op_id: opId, rev: dup, dup: true });
      return;
    }
    const checked = checkOp(m);
    if (!checked.ok) return reject(checked.code, checked.message);
    const op = checked.op;
    const kind = op.kind as string;
    if (!this.meta || this.meta.status === "ended") return reject("session_ended");
    if (CREATOR_KINDS.has(kind) && a.user !== this.meta.creator) return reject("forbidden", "creator only");
    if (this.log.length >= this.maxLog) return reject("log_full");

    if (kind === "leave") {
      this.participants = this.participants.filter((u) => u !== a.user);
      this.append(op, a.user, this.saveMembership());
      this.closeUser(a.user, "left", 4013);
      if (this.participants.length === 0) await this.end("");
    } else if (kind === "kick") {
      const target = op.user_id as string;
      this.participants = this.participants.filter((u) => u !== target);
      const mem = this.members.get(target);
      this.members.set(target, { name: mem?.name ?? "", kicked: true });
      this.append(op, a.user, this.saveMembership());
      this.closeUser(target, "kicked", 4012);
    } else if (kind === "end") {
      await this.end(a.user, op);
    } else {
      this.append(op, a.user);
    }
  }

  private closeUser(user: string, code: string, closeCode: number): void {
    for (const w of this.authed()) {
      if (this.att(w).user === user) this.fatal(w, code, closeCode);
    }
  }

  private async end(by: string, op?: Rec): Promise<void> {
    if (!this.meta || this.meta.status === "ended") return;
    const now = Date.now();
    this.meta.status = "ended";
    this.meta.ended_at = now;
    this.append(op ?? { kind: "end" }, by, { meta: this.meta });
    for (const w of this.sockets()) this.fatal(w, "session_ended", 4011);
    await this.scheduleAlarm();
  }

  override async webSocketClose(ws: WebSocket, code: number, reason: string, wasClean: boolean): Promise<void> {
    await this.socketGone(ws);
  }

  override async webSocketError(ws: WebSocket, error: unknown): Promise<void> {
    await this.socketGone(ws);
  }

  private async socketGone(ws: WebSocket): Promise<void> {
    try {
      ws.close(1000, "bye");
    } catch {
      // already closed
    }
    const a = this.att(ws);
    if (a?.authed && !this.userOnline(a.user, ws)) this.presence(a, false, ws);
    if (this.meta && this.meta.status === "active" && this.sockets(ws).length === 0 && this.meta.idle_since === 0) {
      this.meta.idle_since = Date.now();
      await this.ctx.storage.put("meta", this.meta);
    }
    await this.scheduleAlarm();
  }

  // -------------------------------------------------------------------------
  // Alarm: hello deadlines, idle end, deletion 24 h after the end.

  private nextDeadline(): number | null {
    let next: number | null = null;
    const consider = (t: number) => {
      if (next === null || t < next) next = t;
    };
    for (const w of this.sockets()) {
      const a = this.att(w);
      if (!a.authed) consider(a.hello_by);
    }
    if (this.meta) {
      if (this.meta.status === "ended") consider(this.meta.ended_at + this.retentionMs);
      else if (this.meta.idle_since !== 0) consider(this.meta.idle_since + this.idleEndMs);
    }
    return next;
  }

  private async scheduleAlarm(): Promise<void> {
    const next = this.nextDeadline();
    const current = await this.ctx.storage.getAlarm();
    if (next === null) {
      if (current !== null) await this.ctx.storage.deleteAlarm();
    } else if (current !== next) {
      await this.ctx.storage.setAlarm(next);
    }
  }

  override async alarm(): Promise<void> {
    await this.runAlarm(Date.now());
  }

  /** Alarm logic with an explicit clock (tests call this directly). */
  async runAlarm(now: number): Promise<void> {
    for (const w of this.sockets()) {
      const a = this.att(w);
      if (!a.authed && a.hello_by <= now) this.fatal(w, "unauthorized", 4001, "hello timeout");
    }
    if (this.meta?.status === "active" && this.meta.idle_since !== 0 && now >= this.meta.idle_since + this.idleEndMs) {
      await this.end("");
    }
    if (this.meta?.status === "ended" && now >= this.meta.ended_at + this.retentionMs) {
      for (const w of this.ctx.getWebSockets()) {
        try {
          w.close(4011, "session_ended");
        } catch {
          // ignore
        }
      }
      await this.ctx.storage.deleteAlarm();
      await this.ctx.storage.deleteAll();
      this.meta = null;
      this.members.clear();
      this.participants = [];
      this.log = [];
      this.opIds.clear();
      return;
    }
    const next = this.nextDeadline();
    if (next !== null) await this.ctx.storage.setAlarm(Math.max(next, now + 1));
  }

  // -------------------------------------------------------------------------
  // Introspection for tests.

  async debugState(): Promise<{ meta: Meta | null; participants: string[]; head: number; members: string[] }> {
    return { meta: this.meta, participants: [...this.participants], head: this.log.length, members: [...this.members.keys()] };
  }

  async debugLog(): Promise<Json[]> {
    return this.log.map((r) => JSON.parse(r) as Json);
  }
}
