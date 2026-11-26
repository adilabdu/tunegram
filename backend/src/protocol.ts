// Shape/size validation of client ops (PROTOCOL.md §8). The server never
// interprets semantics; it only checks fields and copies whitelisted ones
// into the log record.

export const MAX_FRAME = 64 * 1024;
export const MAX_BATCH = 100;
export const LOG_CHUNK = 500;
export const HELLO_DEADLINE_MS = 10_000;
export const RATE_BURST = 30;
export const RATE_PER_S = 10;

export type Json = null | boolean | number | string | Json[] | { [k: string]: Json };
export type Rec = { [k: string]: Json };

export const PLAYBACK_KINDS = new Set(["play", "pause", "seek", "next", "previous", "jump", "advanced"]);
export const CLIENT_KINDS = new Set([
  "leave",
  "kick",
  "end",
  "insert",
  "remove",
  "move",
  "play",
  "pause",
  "seek",
  "next",
  "previous",
  "jump",
  "advanced",
  "set_repeat",
  "set_shuffle",
]);
export const CREATOR_KINDS = new Set(["kick", "end"]);

const isId = (v: unknown): v is string => typeof v === "string" && v.length >= 1 && v.length <= 64;
const isShort = (v: unknown): v is string => typeof v === "string" && v.length >= 1 && v.length <= 32;
const isText = (v: unknown): v is string => typeof v === "string" && v.length <= 256;
const isUint = (v: unknown): v is number => typeof v === "number" && Number.isSafeInteger(v) && v >= 0;
const isIdOrNull = (v: unknown): v is string | null => v === null || isId(v);

export type OpCheck = { ok: true; op: Rec } | { ok: false; code: "bad_request" | "forbidden"; message: string };

function entry(v: unknown): Rec | null {
  if (!v || typeof v !== "object" || Array.isArray(v)) return null;
  const e = v as Record<string, unknown>;
  if (!isId(e.item_id) || !isShort(e.chat_id) || !isShort(e.doc_id) || !isUint(e.msg_id) || !isUint(e.duration_ms)) {
    return null;
  }
  const title = e.title ?? "";
  const performer = e.performer ?? "";
  if (!isText(title) || !isText(performer)) return null;
  const out: Rec = { item_id: e.item_id, chat_id: e.chat_id };
  if (e.chat_username !== undefined && e.chat_username !== "") {
    if (typeof e.chat_username !== "string" || !/^[A-Za-z0-9_]{1,32}$/.test(e.chat_username)) return null;
    out.chat_username = e.chat_username;
  }
  out.msg_id = e.msg_id;
  out.doc_id = e.doc_id;
  out.title = title;
  out.performer = performer;
  out.duration_ms = e.duration_ms;
  return out;
}

/** Validates a client `op` message and returns the whitelisted op fields. */
export function checkOp(m: Record<string, unknown>): OpCheck {
  const bad = (message: string): OpCheck => ({ ok: false, code: "bad_request", message });
  const kind = m.kind;
  if (!isId(m.op_id)) return bad("op_id");
  if (typeof kind !== "string") return bad("kind");
  if (kind === "init" || kind === "join") return { ok: false, code: "forbidden", message: "server-only kind" };
  if (!CLIENT_KINDS.has(kind)) return bad("unknown kind");
  const op: Rec = { op_id: m.op_id, base_rev: 0, kind };
  if (m.base_rev !== undefined) {
    if (!isUint(m.base_rev)) return bad("base_rev");
    op.base_rev = m.base_rev;
  }
  if (PLAYBACK_KINDS.has(kind)) {
    if (!isUint(m.at)) return bad("at");
    op.at = m.at;
  }
  switch (kind) {
    case "leave":
    case "end":
      break;
    case "kick":
      if (!isId(m.user_id)) return bad("user_id");
      op.user_id = m.user_id;
      break;
    case "insert": {
      if (!isIdOrNull(m.after)) return bad("after");
      if (!Array.isArray(m.entries) || m.entries.length < 1 || m.entries.length > MAX_BATCH) return bad("entries");
      const entries: Rec[] = [];
      for (const raw of m.entries) {
        const e = entry(raw);
        if (!e) return bad("entry");
        entries.push(e);
      }
      op.after = m.after;
      op.entries = entries;
      break;
    }
    case "remove":
      if (!Array.isArray(m.item_ids) || m.item_ids.length < 1 || m.item_ids.length > MAX_BATCH) return bad("item_ids");
      if (!m.item_ids.every(isId)) return bad("item_ids");
      op.item_ids = m.item_ids as string[];
      break;
    case "move":
      if (!isId(m.item_id) || !isIdOrNull(m.after)) return bad("move");
      op.item_id = m.item_id;
      op.after = m.after;
      break;
    case "play":
    case "pause":
      if (m.item_id !== undefined) {
        if (!isId(m.item_id)) return bad("item_id");
        op.item_id = m.item_id;
      }
      if (!isUint(m.pos)) return bad("pos");
      op.pos = m.pos;
      break;
    case "seek":
      if (!isId(m.item_id) || !isUint(m.pos)) return bad("seek");
      op.item_id = m.item_id;
      op.pos = m.pos;
      break;
    case "next":
      if (!isId(m.from_item)) return bad("from_item");
      op.from_item = m.from_item;
      break;
    case "previous":
      if (!isId(m.from_item) || !isUint(m.pos)) return bad("previous");
      op.from_item = m.from_item;
      op.pos = m.pos;
      break;
    case "jump":
      if (!isId(m.item_id)) return bad("item_id");
      op.item_id = m.item_id;
      break;
    case "advanced":
      if (!isId(m.from_item)) return bad("from_item");
      if (m.to_item !== undefined && !isIdOrNull(m.to_item)) return bad("to_item");
      op.from_item = m.from_item;
      op.to_item = (m.to_item as string | null | undefined) ?? null;
      break;
    case "set_repeat":
      if (m.mode !== "off" && m.mode !== "all" && m.mode !== "one") return bad("mode");
      op.mode = m.mode;
      break;
    case "set_shuffle":
      if (typeof m.on !== "boolean") return bad("on");
      op.on = m.on;
      break;
  }
  return { ok: true, op };
}
