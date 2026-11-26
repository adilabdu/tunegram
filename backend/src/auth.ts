// Telegram Mini App initData verification and our session tokens (PROTOCOL.md §3.1).
import type { Env } from "./env";
import {
  b64urlDecode,
  b64urlEncode,
  hexDecode,
  hexEncode,
  hmacKey,
  hmacSha256,
  intVar,
  timingSafeEqual,
  utf8,
} from "./util";

export const TELEGRAM_PUBLIC_KEY_PROD = "e7bf03a2fa4602af4580703d88dda5bb59f32ed8b02a56c187fe7d34caed242d";
export const TELEGRAM_PUBLIC_KEY_TEST = "40055058a4ee38156a06562e52eece92a771bcd8346a8c4615cb7376eddf72ec";

export interface TelegramUser {
  id: string;
  name: string;
  username: string;
}

export type VerifyResult =
  | { ok: true; user: TelegramUser; method: "ed25519" | "hmac" }
  | { ok: false; code: "bad_init_data" | "init_data_expired"; message: string };

function sortedPairs(params: URLSearchParams, exclude: string[]): string {
  const pairs: [string, string][] = [];
  for (const [k, v] of params) {
    if (!exclude.includes(k)) pairs.push([k, v]);
  }
  pairs.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
  return pairs.map(([k, v]) => `${k}=${v}`).join("\n");
}

/** data-check-string for Telegram's third-party (Ed25519) validation. */
export function ed25519CheckString(botId: string, params: URLSearchParams): string {
  return `${botId}:WebAppData\n${sortedPairs(params, ["hash", "signature"])}`;
}

/** data-check-string for the bot-token HMAC validation (everything but `hash`). */
export function hmacCheckString(params: URLSearchParams): string {
  return sortedPairs(params, ["hash"]);
}

export function telegramPublicKeyHex(env: Env): string {
  if (env.TELEGRAM_PUBLIC_KEY) return env.TELEGRAM_PUBLIC_KEY;
  return env.TELEGRAM_ENV === "test" ? TELEGRAM_PUBLIC_KEY_TEST : TELEGRAM_PUBLIC_KEY_PROD;
}

async function verifyEd25519(env: Env, params: URLSearchParams): Promise<boolean> {
  const sigText = params.get("signature");
  const botId = env.BOT_ID ?? "";
  if (!sigText || !/^\d+$/.test(botId)) return false;
  const sig = b64urlDecode(sigText);
  const pub = hexDecode(telegramPublicKeyHex(env));
  if (!sig || sig.length !== 64 || !pub || pub.length !== 32) return false;
  try {
    const key = await crypto.subtle.importKey("raw", pub, { name: "Ed25519" }, false, ["verify"]);
    return await crypto.subtle.verify("Ed25519", key, sig, utf8(ed25519CheckString(botId, params)));
  } catch {
    return false;
  }
}

async function verifyHmac(env: Env, params: URLSearchParams): Promise<boolean> {
  const hash = params.get("hash");
  if (!hash || !env.BOT_TOKEN || !/^[0-9a-f]{64}$/.test(hash)) return false;
  const secret = await hmacSha256("WebAppData", env.BOT_TOKEN);
  const calc = await hmacSha256(secret, hmacCheckString(params));
  return timingSafeEqual(utf8(hexEncode(calc)), utf8(hash));
}

export async function verifyInitData(env: Env, initData: string, nowS: number): Promise<VerifyResult> {
  if (typeof initData !== "string" || initData.length === 0 || initData.length > 8192) {
    return { ok: false, code: "bad_init_data", message: "missing initData" };
  }
  const params = new URLSearchParams(initData);
  let method: "ed25519" | "hmac" | null = null;
  if (params.has("signature") && (await verifyEd25519(env, params))) method = "ed25519";
  else if (params.has("hash") && (await verifyHmac(env, params))) method = "hmac";
  if (!method) return { ok: false, code: "bad_init_data", message: "signature check failed" };

  const authDate = Number(params.get("auth_date"));
  const maxAge = intVar(env.INIT_DATA_MAX_AGE_S, 3600);
  if (!Number.isSafeInteger(authDate)) return { ok: false, code: "bad_init_data", message: "auth_date missing" };
  if (authDate > nowS + 60 || nowS - authDate > maxAge) {
    return { ok: false, code: "init_data_expired", message: "initData too old" };
  }
  let user: unknown;
  try {
    user = JSON.parse(params.get("user") ?? "null");
  } catch {
    user = null;
  }
  const u = user as { id?: unknown; first_name?: unknown; username?: unknown } | null;
  if (!u || typeof u.id !== "number" || !Number.isSafeInteger(u.id) || u.id <= 0) {
    return { ok: false, code: "bad_init_data", message: "user missing" };
  }
  return {
    ok: true,
    method,
    user: {
      id: String(u.id),
      name: typeof u.first_name === "string" ? u.first_name.slice(0, 256) : "",
      username: typeof u.username === "string" ? u.username.slice(0, 64) : "",
    },
  };
}

// ---------------------------------------------------------------------------
// Session tokens: "v1." + b64url(payload) + "." + b64url(HMAC-SHA256(TOKEN_SECRET, "v1." + b64url(payload)))

export interface TokenPayload {
  sub: string;
  name: string;
  iat: number;
  exp: number;
}

function tokenSecret(env: Env): string {
  if (!env.TOKEN_SECRET || env.TOKEN_SECRET.length < 16) throw new Error("TOKEN_SECRET is not configured");
  return env.TOKEN_SECRET;
}

export async function issueToken(env: Env, user: TelegramUser, nowS: number): Promise<{ token: string; exp: number }> {
  const ttl = intVar(env.TOKEN_TTL_S, 3600);
  const payload: TokenPayload = { sub: user.id, name: user.name, iat: nowS, exp: nowS + ttl };
  const body = "v1." + b64urlEncode(utf8(JSON.stringify(payload)));
  const sig = await hmacSha256(tokenSecret(env), body);
  return { token: body + "." + b64urlEncode(sig), exp: payload.exp };
}

export async function verifyToken(env: Env, token: unknown, nowS: number): Promise<TokenPayload | null> {
  if (typeof token !== "string" || token.length > 2048) return null;
  const parts = token.split(".");
  if (parts.length !== 3 || parts[0] !== "v1") return null;
  const sig = b64urlDecode(parts[2] ?? "");
  const payloadBytes = b64urlDecode(parts[1] ?? "");
  if (!sig || !payloadBytes) return null;
  let ok = false;
  try {
    const key = await hmacKey(tokenSecret(env));
    ok = await crypto.subtle.verify("HMAC", key, sig, utf8(`v1.${parts[1]}`));
  } catch {
    return null;
  }
  if (!ok) return null;
  try {
    const p = JSON.parse(new TextDecoder().decode(payloadBytes)) as TokenPayload;
    if (typeof p.sub !== "string" || typeof p.exp !== "number" || p.exp <= nowS) return null;
    return p;
  } catch {
    return null;
  }
}

export async function bearer(env: Env, req: Request, nowS: number): Promise<TokenPayload | null> {
  const h = req.headers.get("authorization") ?? "";
  const m = /^Bearer\s+(\S+)$/i.exec(h);
  return m ? verifyToken(env, m[1], nowS) : null;
}
