import type { Env as AppEnv } from "../src/env";

declare global {
  namespace Cloudflare {
    interface Env extends AppEnv {
      TEST_ED25519_PKCS8: string;
    }
  }
}
export {};
