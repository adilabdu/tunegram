// Tests run inside the local Workers runtime (workerd via miniflare); no
// Cloudflare account is used. All keys below are generated per run or fake.
import { generateKeyPairSync, randomBytes } from "node:crypto";

import { cloudflareTest } from "@cloudflare/vitest-pool-workers";
import { defineConfig } from "vitest/config";

// A throwaway Ed25519 key pair stands in for Telegram's signing key.
const { publicKey, privateKey } = generateKeyPairSync("ed25519");
const jwk = publicKey.export({ format: "jwk" });
const publicHex = Buffer.from(jwk.x ?? "", "base64url").toString("hex");
const privatePkcs8 = privateKey.export({ format: "der", type: "pkcs8" }).toString("base64");

export default defineConfig({
  plugins: [
    cloudflareTest({
      wrangler: { configPath: "./wrangler.jsonc" },
      miniflare: {
        bindings: {
          TELEGRAM_PUBLIC_KEY: publicHex,
          TEST_ED25519_PKCS8: privatePkcs8,
          BOT_ID: "7000000001",
          BOT_TOKEN: "7000000001:TEST-fake-token-for-tests-only",
          TOKEN_SECRET: randomBytes(32).toString("hex"),
          MAX_PARTICIPANTS: "4",
          MAX_LOG_OPS: "80",
        },
      },
    }),
  ],
  test: {
    include: ["test/**/*.test.ts"],
    testTimeout: 20000,
  },
});
