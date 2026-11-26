// Worker entry: HTTP API (auth, sessions, invites) and WebSocket routing to the
// per-session Durable Object (PROTOCOL.md §3).
import { bearer, issueToken, verifyInitData } from "./auth";
import type { Env } from "./env";
import { landingPage } from "./landing";
import { b64urlDecode, b64urlEncode, errorResponse, json, randomBytes, readJson } from "./util";

export { SessionDO } from "./session";

const SESSION_RE = /^[A-Za-z0-9_-]{22}$/;

function stubFor(env: Env, sessionId: string) {
  return env.SESSION.get(env.SESSION.idFromName(sessionId));
}

function inviteLink(env: Env, token: string): string {
  const bot = env.BOT_USERNAME || "TunegramSyncBot";
  const app = env.MINI_APP_SHORT_NAME || "listen";
  return `https://t.me/${bot}/${app}?startapp=${token}`;
}

async function handle(req: Request, env: Env): Promise<Response> {
  const url = new URL(req.url);
  const path = url.pathname;
  const nowS = Math.floor(Date.now() / 1000);

  if (path === "/health" && req.method === "GET") return new Response("ok");
  if ((path === "/" || path === "/app") && req.method === "GET") return landingPage();

  if (path === "/v1/auth" && req.method === "POST") {
    const body = await readJson(req);
    if (!body || typeof body.init_data !== "string") return errorResponse(400, "bad_request", "init_data required");
    const r = await verifyInitData(env, body.init_data, nowS);
    if (!r.ok) return errorResponse(401, r.code, r.message);
    const t = await issueToken(env, r.user, nowS);
    return json({ token: t.token, expires_at: t.exp, user: r.user });
  }

  if (path === "/v1/sessions" && req.method === "POST") {
    const tok = await bearer(env, req, nowS);
    if (!tok) return errorResponse(401, "unauthorized");
    const body = await readJson(req);
    const host = body?.host_chat_id;
    const title = body?.title ?? "";
    if (typeof host !== "string" || host.length < 1 || host.length > 32 || typeof title !== "string" || title.length > 256) {
      return errorResponse(400, "bad_request", "host_chat_id (1-32 chars) required");
    }
    const sessionId = b64urlEncode(randomBytes(16));
    const r = await stubFor(env, sessionId).init(sessionId, tok.sub, tok.name ?? "", host, title);
    if (!r.ok) return errorResponse(r.status, r.code);
    return json({ session_id: sessionId, ws_path: `/v1/sessions/${sessionId}/ws` }, 201);
  }

  const inviteMatch = /^\/v1\/sessions\/([^/]+)\/invites$/.exec(path);
  if (inviteMatch && req.method === "POST") {
    const sessionId = inviteMatch[1] ?? "";
    if (!SESSION_RE.test(sessionId)) return errorResponse(404, "not_found");
    const tok = await bearer(env, req, nowS);
    if (!tok) return errorResponse(401, "unauthorized");
    const body = (await readJson(req)) ?? {};
    const ttl = typeof body.ttl_s === "number" ? body.ttl_s : 21600;
    const r = await stubFor(env, sessionId).createInvite(tok.sub, ttl);
    if (!r.ok) return errorResponse(r.status, r.code);
    const sid = b64urlDecode(sessionId);
    const secret = b64urlDecode(r.value.secret);
    if (!sid || !secret) return errorResponse(500, "internal");
    const raw = new Uint8Array(32);
    raw.set(sid, 0);
    raw.set(secret, 16);
    const token = b64urlEncode(raw);
    return json({ invite_token: token, link: inviteLink(env, token), expires_at: r.value.expires_at }, 201);
  }

  if (path === "/v1/invites/redeem" && req.method === "POST") {
    const tok = await bearer(env, req, nowS);
    if (!tok) return errorResponse(401, "unauthorized");
    const body = await readJson(req);
    const raw = typeof body?.invite_token === "string" ? b64urlDecode(body.invite_token) : null;
    if (!raw || raw.length !== 32) return errorResponse(404, "invite_invalid");
    const sessionId = b64urlEncode(raw.slice(0, 16));
    const r = await stubFor(env, sessionId).redeem(tok.sub, tok.name ?? "", b64urlEncode(raw.slice(16)));
    if (!r.ok) return errorResponse(r.status, r.code);
    return json({ ...r.value, ws_path: `/v1/sessions/${sessionId}/ws` });
  }

  const wsMatch = /^\/v1\/sessions\/([^/]+)\/ws$/.exec(path);
  if (wsMatch && req.method === "GET") {
    const sessionId = wsMatch[1] ?? "";
    if (!SESSION_RE.test(sessionId)) return errorResponse(404, "not_found");
    if (req.headers.get("upgrade")?.toLowerCase() !== "websocket") return errorResponse(426, "upgrade_required");
    return stubFor(env, sessionId).fetch(req);
  }

  return errorResponse(404, "not_found");
}

export default {
  async fetch(req: Request, env: Env): Promise<Response> {
    try {
      return await handle(req, env);
    } catch (e) {
      console.error("internal error", e instanceof Error ? e.message : String(e));
      return errorResponse(500, "internal");
    }
  },
} satisfies ExportedHandler<Env>;
