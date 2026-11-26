import { describe, expect, it } from "vitest";

import {
  TELEGRAM_PUBLIC_KEY_PROD,
  TELEGRAM_PUBLIC_KEY_TEST,
  issueToken,
  telegramPublicKeyHex,
  verifyInitData,
  verifyToken,
} from "../src/auth";
import type { Env } from "../src/env";
import { api, makeInitData, tenv } from "./helpers";

const now = () => Math.floor(Date.now() / 1000);

describe("initData verification", () => {
  it("accepts Ed25519 third-party signatures", async () => {
    const r = await verifyInitData(tenv as unknown as Env, await makeInitData(5550001, "Ada"), now());
    expect(r.ok).toBe(true);
    if (r.ok) {
      expect(r.method).toBe("ed25519");
      expect(r.user).toEqual({ id: "5550001", name: "Ada", username: "u5550001" });
    }
  });

  it("accepts the bot-token HMAC path (with and without a signature field)", async () => {
    for (const method of ["hmac", "both"] as const) {
      const env = { ...tenv, TELEGRAM_PUBLIC_KEY: "00".repeat(32) } as unknown as Env; // Ed25519 fails, HMAC must carry it
      const r = await verifyInitData(env, await makeInitData(42, "Bo", { method }), now());
      expect(r.ok, method).toBe(true);
      if (r.ok) expect(r.method).toBe("hmac");
    }
  });

  it("rejects tampered data, wrong bot id and missing keys", async () => {
    const env = tenv as unknown as Env;
    expect((await verifyInitData(env, await makeInitData(1, "A", { tamper: true }), now())).ok).toBe(false);
    expect((await verifyInitData(env, await makeInitData(1, "A", { botId: "7000000002" }), now())).ok).toBe(false);
    const noToken = { ...tenv, BOT_TOKEN: undefined } as unknown as Env;
    expect((await verifyInitData(noToken, await makeInitData(1, "A", { method: "hmac" }), now())).ok).toBe(false);
    const noBotId = { ...tenv, BOT_ID: "" } as unknown as Env;
    expect((await verifyInitData(noBotId, await makeInitData(1, "A"), now())).ok).toBe(false);
    expect((await verifyInitData(env, "", now())).ok).toBe(false);
    expect((await verifyInitData(env, "user=%7B%7D&auth_date=1", now())).ok).toBe(false);
  });

  it("checks auth_date freshness", async () => {
    const env = tenv as unknown as Env;
    const old = await verifyInitData(env, await makeInitData(1, "A", { authDate: now() - 3601 }), now());
    expect(old).toMatchObject({ ok: false, code: "init_data_expired" });
    const future = await verifyInitData(env, await makeInitData(1, "A", { authDate: now() + 120 }), now());
    expect(future).toMatchObject({ ok: false, code: "init_data_expired" });
    const recent = await verifyInitData(env, await makeInitData(1, "A", { authDate: now() - 3500 }), now());
    expect(recent.ok).toBe(true);
  });

  it("requires a user with a numeric id", async () => {
    const env = tenv as unknown as Env;
    expect((await verifyInitData(env, await makeInitData(1, "A", { user: null }), now())).ok).toBe(false);
    expect((await verifyInitData(env, await makeInitData(1, "A", { user: { id: "1" } }), now())).ok).toBe(false);
  });

  it("selects Telegram's production or test public key", () => {
    expect(telegramPublicKeyHex({ TELEGRAM_ENV: "prod" } as Env)).toBe(TELEGRAM_PUBLIC_KEY_PROD);
    expect(telegramPublicKeyHex({ TELEGRAM_ENV: "test" } as Env)).toBe(TELEGRAM_PUBLIC_KEY_TEST);
    expect(telegramPublicKeyHex({} as Env)).toBe(TELEGRAM_PUBLIC_KEY_PROD);
    expect(TELEGRAM_PUBLIC_KEY_PROD).toBe("e7bf03a2fa4602af4580703d88dda5bb59f32ed8b02a56c187fe7d34caed242d");
    expect(TELEGRAM_PUBLIC_KEY_TEST).toBe("40055058a4ee38156a06562e52eece92a771bcd8346a8c4615cb7376eddf72ec");
  });
});

describe("session tokens", () => {
  it("round-trips, expires and resists tampering", async () => {
    const env = tenv as unknown as Env;
    const t = await issueToken(env, { id: "9", name: "N", username: "" }, now());
    expect(t.token.startsWith("v1.")).toBe(true);
    expect((await verifyToken(env, t.token, now()))?.sub).toBe("9");
    expect(await verifyToken(env, t.token, t.exp)).toBeNull(); // expired
    const parts = t.token.split(".");
    const forged = `v1.${btoa(JSON.stringify({ sub: "1", name: "x", iat: 0, exp: 9e9 })).replace(/=+$/, "")}.${parts[2]}`;
    expect(await verifyToken(env, forged, now())).toBeNull();
    const other = { ...tenv, TOKEN_SECRET: "another-secret-0123456789" } as unknown as Env;
    expect(await verifyToken(other, t.token, now())).toBeNull();
    expect(await verifyToken(env, "garbage", now())).toBeNull();
  });
});

describe("POST /v1/auth", () => {
  it("issues a short-lived token for valid initData", async () => {
    const r = await api("/v1/auth", "POST", { init_data: await makeInitData(777, "Cy") });
    expect(r.status).toBe(200);
    expect(r.body.user).toEqual({ id: "777", name: "Cy", username: "u777" });
    expect(r.body.expires_at - now()).toBeGreaterThan(3500);
    expect(r.body.expires_at - now()).toBeLessThanOrEqual(3600);
  });

  it("rejects bad and stale initData", async () => {
    expect((await api("/v1/auth", "POST", { init_data: await makeInitData(1, "A", { tamper: true }) })).body.error.code).toBe(
      "bad_init_data",
    );
    const stale = await api("/v1/auth", "POST", { init_data: await makeInitData(1, "A", { authDate: now() - 7200 }) });
    expect(stale.status).toBe(401);
    expect(stale.body.error.code).toBe("init_data_expired");
    expect((await api("/v1/auth", "POST", { nope: 1 })).status).toBe(400);
  });

  it("protects session endpoints with the bearer token", async () => {
    expect((await api("/v1/sessions", "POST", { host_chat_id: "-1" })).status).toBe(401);
    expect((await api("/v1/sessions", "POST", { host_chat_id: "-1" }, "v1.bad.token")).status).toBe(401);
    expect((await api("/health")).body).toBe("ok");
    expect((await api("/nope")).status).toBe(404);
  });
});
