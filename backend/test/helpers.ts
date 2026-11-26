// Test helpers: generated Telegram initData, API calls and a WebSocket client.
import { env, SELF } from "cloudflare:test";

import { ed25519CheckString, hmacCheckString } from "../src/auth";
import { hexEncode, hmacSha256 } from "../src/util";

export type TestEnv = typeof env & { BOT_ID: string; BOT_TOKEN: string };
export const tenv = env as TestEnv;

function b64urlEncode(bytes: Uint8Array): string {
  let bin = "";
  for (const b of bytes) bin += String.fromCharCode(b);
  return btoa(bin).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

export interface InitDataOptions {
  authDate?: number;
  method?: "ed25519" | "hmac" | "both";
  botId?: string;
  user?: Record<string, unknown> | null;
  tamper?: boolean;
}

/** Builds initData the way Telegram does, signed with the test keys. */
export async function makeInitData(userId: number, firstName: string, opts: InitDataOptions = {}): Promise<string> {
  const params = new URLSearchParams();
  params.set("query_id", "AAH-test-" + userId);
  const user = opts.user === undefined ? { id: userId, first_name: firstName, username: "u" + userId } : opts.user;
  if (user !== null) params.set("user", JSON.stringify(user));
  params.set("auth_date", String(opts.authDate ?? Math.floor(Date.now() / 1000)));
  const method = opts.method ?? "ed25519";
  if (method === "ed25519" || method === "both") {
    const der = Uint8Array.from(atob(tenv.TEST_ED25519_PKCS8), (c) => c.charCodeAt(0));
    const key = await crypto.subtle.importKey("pkcs8", der, { name: "Ed25519" }, false, ["sign"]);
    const dcs = ed25519CheckString(opts.botId ?? tenv.BOT_ID, params);
    const sig = new Uint8Array(await crypto.subtle.sign("Ed25519", key, new TextEncoder().encode(dcs)));
    params.set("signature", b64urlEncode(sig));
  }
  if (method === "hmac" || method === "both") {
    const secret = await hmacSha256("WebAppData", tenv.BOT_TOKEN);
    params.set("hash", hexEncode(await hmacSha256(secret, hmacCheckString(params))));
  }
  if (opts.tamper) params.set("auth_date", String(Number(params.get("auth_date")) - 1));
  return params.toString();
}

export async function api(
  path: string,
  method = "GET",
  body?: unknown,
  token?: string,
): Promise<{ status: number; body: any }> {
  const headers: Record<string, string> = { "content-type": "application/json" };
  if (token) headers.authorization = `Bearer ${token}`;
  const res = await SELF.fetch(`http://lt.test${path}`, {
    method,
    headers,
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const text = await res.text();
  let parsed: unknown = text;
  try {
    parsed = JSON.parse(text);
  } catch {
    // plain text
  }
  return { status: res.status, body: parsed };
}

export async function login(userId: number, name = "User" + userId): Promise<string> {
  const r = await api("/v1/auth", "POST", { init_data: await makeInitData(userId, name) });
  if (r.status !== 200) throw new Error("login failed: " + JSON.stringify(r.body));
  return r.body.token as string;
}

export async function createSession(token: string, host = "-1001234567890"): Promise<string> {
  const r = await api("/v1/sessions", "POST", { host_chat_id: host, title: "Test" }, token);
  if (r.status !== 201) throw new Error("create failed: " + JSON.stringify(r.body));
  return r.body.session_id as string;
}

/** Creator invites `token`'s user and that user redeems. */
export async function invite(sessionId: string, creatorToken: string, token: string): Promise<void> {
  const inv = await api(`/v1/sessions/${sessionId}/invites`, "POST", {}, creatorToken);
  if (inv.status !== 201) throw new Error("invite failed: " + JSON.stringify(inv.body));
  const red = await api("/v1/invites/redeem", "POST", { invite_token: inv.body.invite_token }, token);
  if (red.status !== 200) throw new Error("redeem failed: " + JSON.stringify(red.body));
}

export class Client {
  messages: any[] = [];
  closed: { code: number } | null = null;
  private waiters: (() => void)[] = [];

  constructor(public ws: WebSocket) {
    ws.addEventListener("message", (e) => {
      this.messages.push(JSON.parse(e.data as string));
      this.wake();
    });
    ws.addEventListener("close", (e) => {
      this.closed = { code: e.code };
      this.wake();
    });
  }

  private wake() {
    const w = this.waiters;
    this.waiters = [];
    for (const f of w) f();
  }

  send(msg: unknown) {
    this.ws.send(typeof msg === "string" ? msg : JSON.stringify(msg));
  }

  /** Waits for (and consumes up to) the first message matching `pred`. */
  async next(pred: (m: any) => boolean = () => true, timeoutMs = 3000): Promise<any> {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      const i = this.messages.findIndex(pred);
      if (i >= 0) return this.messages.splice(0, i + 1)[i];
      if (Date.now() > deadline) throw new Error("timeout; have: " + JSON.stringify(this.messages).slice(0, 500));
      await new Promise<void>((resolve) => {
        this.waiters.push(resolve);
        setTimeout(resolve, 50);
      });
    }
  }

  async type(t: string, timeoutMs = 3000): Promise<any> {
    return this.next((m) => m.type === t, timeoutMs);
  }

  async waitClosed(timeoutMs = 3000): Promise<number> {
    const deadline = Date.now() + timeoutMs;
    while (!this.closed) {
      if (Date.now() > deadline) throw new Error("not closed");
      await new Promise<void>((resolve) => {
        this.waiters.push(resolve);
        setTimeout(resolve, 50);
      });
    }
    return this.closed.code;
  }

  close() {
    try {
      this.ws.close(1000, "bye");
    } catch {
      // ignore
    }
  }
}

export async function connect(sessionId: string): Promise<Client> {
  const res = await SELF.fetch(`http://lt.test/v1/sessions/${sessionId}/ws`, { headers: { upgrade: "websocket" } });
  const ws = res.webSocket;
  if (!ws) throw new Error("no websocket: " + res.status);
  ws.accept();
  return new Client(ws);
}

let deviceCounter = 0;
export async function join(sessionId: string, token: string, sinceRev = 0, v = 1): Promise<{ c: Client; welcome: any; log: any[] }> {
  const c = await connect(sessionId);
  c.send({ type: "hello", v, token, device_id: "dev-" + ++deviceCounter, since_rev: sinceRev, t0: 1 });
  const welcome = await c.type("welcome");
  const log: any[] = [];
  for (;;) {
    const l = await c.type("log");
    log.push(...l.ops);
    if (!l.more) break;
  }
  return { c, welcome, log };
}

let opCounter = 0;
export function op(kind: string, fields: Record<string, unknown> = {}): Record<string, unknown> {
  return { type: "op", op_id: `t:${++opCounter}:${Math.random().toString(36).slice(2, 8)}`, base_rev: 0, kind, ...fields };
}

export function entry(id: string, extra: Record<string, unknown> = {}): Record<string, unknown> {
  return { item_id: id, chat_id: "-1001234567890", msg_id: 1, doc_id: "5368712345678901234", title: "T", performer: "P", duration_ms: 180000, ...extra };
}

export function stubFor(sessionId: string) {
  return tenv.SESSION.get(tenv.SESSION.idFromName(sessionId));
}
