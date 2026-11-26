import type { SessionDO } from "./session";

export interface Env {
  SESSION: DurableObjectNamespace<SessionDO>;
  TELEGRAM_ENV?: string; // "prod" | "test"
  TELEGRAM_PUBLIC_KEY?: string; // hex override (tests / e2e only)
  BOT_ID?: string;
  BOT_TOKEN?: string; // secret, optional (HMAC path)
  BOT_USERNAME?: string;
  MINI_APP_SHORT_NAME?: string;
  TOKEN_SECRET?: string; // secret, required
  MAX_PARTICIPANTS?: string;
  MIN_PROTOCOL_VERSION?: string;
  MAX_PROTOCOL_VERSION?: string;
  TOKEN_TTL_S?: string;
  INIT_DATA_MAX_AGE_S?: string;
  MAX_LOG_OPS?: string;
  IDLE_END_S?: string;
  RETENTION_S?: string;
}
