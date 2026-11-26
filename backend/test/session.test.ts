import { evictDurableObject, runDurableObjectAlarm, runInDurableObject } from "cloudflare:test";
import { describe, expect, it } from "vitest";

import { checkOp } from "../src/protocol";
import type { SessionDO } from "../src/session";
import { api, connect, createSession, entry, invite, join, login, op, stubFor } from "./helpers";

let nextUser = 100000;
const newUser = async (name = "U") => {
  const id = ++nextUser;
  return { id: String(id), token: await login(id, name + id) };
};

async function sessionWith(n: number) {
  const creator = await newUser("Creator");
  const sid = await createSession(creator.token);
  const others = [];
  for (let i = 1; i < n; i++) {
    const u = await newUser();
    await invite(sid, creator.token, u.token);
    others.push(u);
  }
  return { sid, creator, others };
}

describe("sessions and invites", () => {
  it("creates a session with an init record", async () => {
    const creator = await newUser();
    const sid = await createSession(creator.token);
    expect(sid).toMatch(/^[A-Za-z0-9_-]{22}$/);
    const log = await runInDurableObject(stubFor(sid), (o: SessionDO) => o.debugLog());
    expect(log).toHaveLength(1);
    expect(log[0]).toMatchObject({ rev: 1, by: creator.id, op_id: "s:1", kind: "init", host_chat_id: "-1001234567890" });
    expect((await api("/v1/sessions", "POST", { host_chat_id: "" }, creator.token)).status).toBe(400);
  });

  it("builds t.me links with single-use invite tokens", async () => {
    const creator = await newUser();
    const sid = await createSession(creator.token);
    const inv = await api(`/v1/sessions/${sid}/invites`, "POST", { ttl_s: 600 }, creator.token);
    expect(inv.status).toBe(201);
    expect(inv.body.invite_token).toMatch(/^[A-Za-z0-9_-]{43}$/);
    expect(inv.body.link).toBe(`https://t.me/TunegramSyncBot/listen?startapp=${inv.body.invite_token}`);
    expect(inv.body.expires_at - Date.now()).toBeGreaterThan(590_000);
    const a = await newUser();
    const b = await newUser();
    const r1 = await api("/v1/invites/redeem", "POST", { invite_token: inv.body.invite_token }, a.token);
    expect(r1.status).toBe(200);
    expect(r1.body).toMatchObject({ session_id: sid, host_chat_id: "-1001234567890", ws_path: `/v1/sessions/${sid}/ws` });
    const r2 = await api("/v1/invites/redeem", "POST", { invite_token: inv.body.invite_token }, b.token);
    expect(r2.status).toBe(410);
    expect(r2.body.error.code).toBe("invite_used");
  });

  it("rejects expired, malformed and foreign invites", async () => {
    const creator = await newUser();
    const sid = await createSession(creator.token);
    const inv = await api(`/v1/sessions/${sid}/invites`, "POST", { ttl_s: 60 }, creator.token);
    await runInDurableObject(stubFor(sid), async (_o: SessionDO, state) => {
      const list = await state.storage.list<{ exp: number }>({ prefix: "inv:" });
      for (const [k, v] of list) await state.storage.put(k, { ...v, exp: Date.now() - 1 });
    });
    const u = await newUser();
    const r = await api("/v1/invites/redeem", "POST", { invite_token: inv.body.invite_token }, u.token);
    expect(r.status).toBe(410);
    expect(r.body.error.code).toBe("invite_expired");
    expect((await api("/v1/invites/redeem", "POST", { invite_token: "abc" }, u.token)).body.error.code).toBe("invite_invalid");
    const bogus = inv.body.invite_token.slice(0, 22) + "A".repeat(21);
    expect((await api("/v1/invites/redeem", "POST", { invite_token: bogus }, u.token)).status).toBe(404);
    // A non-member cannot mint invites.
    expect((await api(`/v1/sessions/${sid}/invites`, "POST", {}, u.token)).status).toBe(403);
  });
});

describe("WebSocket sequencer", () => {
  it("welcomes, replays the log and announces presence", async () => {
    const { sid, creator, others } = await sessionWith(2);
    const a = await join(sid, creator.token);
    expect(a.welcome).toMatchObject({ type: "welcome", v: 1, session_id: sid, user_id: creator.id, creator: creator.id, t0: 1 });
    expect(a.welcome.t1).toBe(a.welcome.t2);
    expect(a.log.map((r: any) => r.kind)).toEqual(["init", "join"]);
    const b = await join(sid, others[0]!.token);
    expect(b.welcome.online).toEqual([expect.objectContaining({ user_id: creator.id, state: "active" })]);
    expect(await a.c.next((m) => m.type === "event" && m.op.kind === "join")).toMatchObject({ op: { rev: 3, by: others[0]!.id } });
    expect(await a.c.type("presence")).toMatchObject({ user_id: others[0]!.id, online: true });
    b.c.send({ type: "status", state: "interrupted", reason: "other_audio" });
    expect(await a.c.type("presence")).toMatchObject({ user_id: others[0]!.id, state: "interrupted", reason: "other_audio" });
    b.c.close();
    expect(await a.c.type("presence")).toMatchObject({ user_id: others[0]!.id, online: false });
    a.c.close();
  });

  it("sequences ops with rev, ts, by and broadcasts them to everyone", async () => {
    const { sid, creator, others } = await sessionWith(2);
    const a = await join(sid, creator.token);
    const b = await join(sid, others[0]!.token);
    a.c.send(op("insert", { after: null, entries: [entry("i1", { chat_username: "somechannel" }), entry("i2")], at: 5 }));
    b.c.send(op("play", { item_id: "i1", pos: 0, at: Date.now() }));
    b.c.send(op("set_shuffle", { on: true }));
    const seenA = [];
    const seenB = [];
    for (let i = 0; i < 3; i++) {
      seenA.push((await a.c.next((m) => m.type === "event" && m.op.kind !== "join")).op);
      seenB.push((await b.c.next((m) => m.type === "event" && m.op.kind !== "join")).op);
    }
    expect(seenA).toEqual(seenB);
    expect(seenA.map((r) => r.rev)).toEqual([4, 5, 6]);
    expect(seenA[0]).toMatchObject({ by: creator.id, kind: "insert", after: null });
    expect(seenA[0].at).toBeUndefined(); // `at` only kept for playback kinds
    expect(seenA[0].entries[0]).toEqual(entry("i1", { chat_username: "somechannel" }));
    expect(seenA[0].entries[1].chat_username).toBeUndefined();
    expect(seenA[1]).toMatchObject({ by: others[0]!.id, kind: "play", item_id: "i1", pos: 0 });
    expect(seenA[2].seed).toBeGreaterThanOrEqual(0);
    expect(seenA[2].seed).toBeLessThan(2 ** 32);
    expect(seenA[1].ts).toBeGreaterThanOrEqual(seenA[0].ts);
    a.c.close();
    b.c.close();
  });

  it("is idempotent by op_id", async () => {
    const { sid, creator } = await sessionWith(1);
    const a = await join(sid, creator.token);
    const o = op("insert", { after: null, entries: [entry("x1")] });
    a.c.send(o);
    const ev = await a.c.type("event");
    a.c.send(o);
    a.c.send({ ...o, kind: "remove", item_ids: ["x1"] }); // same op_id, different body: still a duplicate
    const ack1 = await a.c.type("ack");
    const ack2 = await a.c.type("ack");
    expect(ack1).toEqual({ type: "ack", op_id: o.op_id, rev: ev.op.rev, dup: true });
    expect(ack2.rev).toBe(ev.op.rev);
    const st = await runInDurableObject(stubFor(sid), (x: SessionDO) => x.debugState());
    expect(st.head).toBe(3);
    a.c.close();
  });

  it("validates shape, size and permissions", async () => {
    const { sid, creator, others } = await sessionWith(2);
    const a = await join(sid, creator.token);
    const b = await join(sid, others[0]!.token);
    const cases: [Record<string, unknown>, string][] = [
      [op("insert", { after: null, entries: [] }), "bad_request"],
      [op("insert", { after: null, entries: [entry("y", { chat_id: "" })] }), "bad_request"],
      [op("insert", { after: null, entries: [entry("y", { chat_username: "has space" })] }), "bad_request"],
      [op("play", { item_id: "i", pos: 1 }), "bad_request"], // no `at`
      [op("seek", { item_id: "i", pos: -1, at: 1 }), "bad_request"],
      [op("set_repeat", { mode: "twice" }), "bad_request"],
      [op("dance"), "bad_request"],
      [op("join", { name: "x" }), "forbidden"],
      [op("end"), "forbidden"], // b is not the creator
      [op("kick", { user_id: creator.id }), "forbidden"],
    ];
    for (const [o, code] of cases) {
      b.c.send(o);
      expect(await b.c.type("reject"), JSON.stringify(o)).toMatchObject({ op_id: o.op_id, code });
    }
    b.c.send("not json");
    expect(await b.c.type("error")).toMatchObject({ code: "bad_request", fatal: false });
    b.c.send(JSON.stringify(op("insert", { after: null, entries: [entry("z", { title: "x".repeat(70000) })] })));
    expect(await b.c.type("error")).toMatchObject({ code: "too_large", fatal: false });
    b.c.send({ type: "dance" });
    expect(await b.c.type("error")).toMatchObject({ code: "bad_request" });
    expect(b.c.closed).toBeNull(); // none of these are fatal
    a.c.close();
    b.c.close();
  });

  it("answers ping, sync and catch-up from since_rev", async () => {
    const { sid, creator } = await sessionWith(1);
    const a = await join(sid, creator.token);
    a.c.send({ type: "ping", t0: 12345 });
    const pong = await a.c.type("pong");
    expect(pong).toMatchObject({ t0: 12345, head_rev: 2 });
    expect(pong.t1).toBe(pong.t2);
    for (let i = 0; i < 5; i++) a.c.send(op("insert", { after: null, entries: [entry("c" + i)] }));
    for (let i = 0; i < 5; i++) await a.c.type("event");
    a.c.send({ type: "sync", since_rev: 4 });
    const l = await a.c.type("log");
    expect(l.ops.map((r: any) => r.rev)).toEqual([5, 6, 7]);
    a.c.close();
    const again = await join(sid, creator.token, 6);
    expect(again.log.map((r: any) => r.rev)).toEqual([7]);
    expect(again.welcome.head_rev).toBe(7);
    const fresh = await join(sid, creator.token, 999); // unknown future rev -> full log
    expect(fresh.log).toHaveLength(7);
    again.c.close();
    fresh.c.close();
  });

  it("rejects bad hellos with the right close codes", async () => {
    const { sid, creator } = await sessionWith(1);
    const v2 = await connect(sid);
    v2.send({ type: "hello", v: 2, token: creator.token, device_id: "d", since_rev: 0 });
    expect(await v2.type("error")).toMatchObject({ code: "version_unsupported", fatal: true, min_v: 1, max_v: 1 });
    expect(await v2.waitClosed()).toBe(4009);

    const bad = await connect(sid);
    bad.send({ type: "hello", v: 1, token: "v1.x.y", device_id: "d" });
    expect(await bad.type("error")).toMatchObject({ code: "unauthorized" });
    expect(await bad.waitClosed()).toBe(4001);

    const early = await connect(sid);
    early.send({ type: "ping", t0: 1 });
    expect(await early.waitClosed()).toBe(4001);

    const stranger = await newUser();
    const nm = await connect(sid);
    nm.send({ type: "hello", v: 1, token: stranger.token, device_id: "d" });
    expect(await nm.type("error")).toMatchObject({ code: "not_member" });
    expect(await nm.waitClosed()).toBe(4003);

    const nf = await connect("A".repeat(22));
    nf.send({ type: "hello", v: 1, token: creator.token, device_id: "d" });
    expect(await nf.waitClosed()).toBe(4004);
  });

  it("enforces the participant cap", async () => {
    // MAX_PARTICIPANTS is 4 in the test config (production default 10).
    const { sid, creator, others } = await sessionWith(5);
    const joined = [await join(sid, creator.token)];
    for (const u of others.slice(0, 3)) joined.push(await join(sid, u.token));
    const fifth = await connect(sid);
    fifth.send({ type: "hello", v: 1, token: others[3]!.token, device_id: "d" });
    expect(await fifth.type("error")).toMatchObject({ code: "session_full", fatal: true });
    expect(await fifth.waitClosed()).toBe(4010);
    // A second device of an existing participant does not count.
    const second = await join(sid, others[0]!.token);
    expect(second.welcome.user_id).toBe(others[0]!.id);
    // Redeeming a new invite while full fails early.
    const sixth = await newUser();
    const inv = await api(`/v1/sessions/${sid}/invites`, "POST", {}, creator.token);
    expect((await api("/v1/invites/redeem", "POST", { invite_token: inv.body.invite_token }, sixth.token)).status).toBe(409);
    // After a leave there is room again.
    joined[1]!.c.send(op("leave"));
    expect(await joined[1]!.c.waitClosed()).toBe(4013);
    expect(await second.c.waitClosed()).toBe(4013); // the leaver's other device too
    const now5 = await join(sid, others[3]!.token);
    expect(now5.welcome.user_id).toBe(others[3]!.id);
    for (const j of [...joined, now5]) j.c.close();
  });

  it("rate-limits op floods per socket", async () => {
    const { sid, creator } = await sessionWith(1);
    const a = await join(sid, creator.token);
    for (let i = 0; i < 40; i++) a.c.send(op("set_repeat", { mode: "off" }));
    const reject = await a.c.next((m) => m.type === "reject", 5000);
    expect(reject.code).toBe("rate_limited");
    a.c.close();
  });

  it("caps the log", async () => {
    // MAX_LOG_OPS is 80 in the test config.
    const { sid, creator, others } = await sessionWith(4);
    const clients = [await join(sid, creator.token)];
    for (const u of others) clients.push(await join(sid, u.token));
    for (const c of clients) for (let i = 0; i < 20; i++) c.c.send(op("set_repeat", { mode: "all" }));
    const r = await clients[0]!.c.next((m) => m.type === "reject" && m.code === "log_full", 8000);
    expect(r.code).toBe("log_full");
    const st = await runInDurableObject(stubFor(sid), (x: SessionDO) => x.debugState());
    expect(st.head).toBe(80);
    for (const c of clients) c.c.close();
  });

  it("kick: creator only, closes the target, blocks rejoin", async () => {
    const { sid, creator, others } = await sessionWith(3);
    const a = await join(sid, creator.token);
    const b = await join(sid, others[0]!.token);
    a.c.send(op("kick", { user_id: others[0]!.id }));
    expect(await b.c.type("error")).toMatchObject({ code: "kicked", fatal: true });
    expect(await b.c.waitClosed()).toBe(4012);
    const back = await connect(sid);
    back.send({ type: "hello", v: 1, token: others[0]!.token, device_id: "d" });
    expect(await back.waitClosed()).toBe(4012);
    const inv = await api(`/v1/sessions/${sid}/invites`, "POST", {}, creator.token);
    expect((await api("/v1/invites/redeem", "POST", { invite_token: inv.body.invite_token }, others[0]!.token)).body.error.code).toBe("kicked");
    a.c.close();
  });

  it("survives eviction: state reloads from storage and hibernated sockets keep working", async () => {
    const { sid, creator } = await sessionWith(1);
    const a = await join(sid, creator.token);
    const o = op("insert", { after: null, entries: [entry("e1")] });
    a.c.send(o);
    await a.c.type("event");
    await evictDurableObject(stubFor(sid));
    a.c.send(o); // dedupe map rebuilt from the stored log
    expect(await a.c.type("ack")).toMatchObject({ rev: 3, dup: true });
    a.c.send(op("set_repeat", { mode: "one" }));
    expect((await a.c.type("event")).op.rev).toBe(4);
    a.c.close();
  });
});

describe("session lifecycle", () => {
  it("end closes everyone and deletes all data 24 h later", async () => {
    const { sid, creator, others } = await sessionWith(2);
    const a = await join(sid, creator.token);
    const b = await join(sid, others[0]!.token);
    a.c.send(op("end"));
    expect(await b.c.type("error")).toMatchObject({ code: "session_ended" });
    expect(await b.c.waitClosed()).toBe(4011);
    expect(await a.c.waitClosed()).toBe(4011);
    const stub = stubFor(sid);
    const { endedAt, alarm } = await runInDurableObject(stub, async (o: SessionDO, state) => ({
      endedAt: (await o.debugState()).meta!.ended_at,
      alarm: await state.storage.getAlarm(),
    }));
    expect(alarm).toBe(endedAt + 24 * 3600 * 1000);
    const late = await connect(sid);
    late.send({ type: "hello", v: 1, token: others[0]!.token, device_id: "d" });
    expect(await late.waitClosed()).toBe(4011);
    expect((await api(`/v1/sessions/${sid}/invites`, "POST", {}, creator.token)).status).toBe(410);
    // Not yet: an early alarm only re-arms.
    await runInDurableObject(stub, (o: SessionDO) => o.runAlarm(endedAt + 3600_000));
    expect((await runInDurableObject(stub, (o: SessionDO) => o.debugState())).meta).not.toBeNull();
    // 24 h after the end: everything is gone.
    await runInDurableObject(stub, (o: SessionDO) => o.runAlarm(endedAt + 24 * 3600 * 1000));
    const after = await runInDurableObject(stub, async (o: SessionDO, state) => ({
      st: await o.debugState(),
      keys: (await state.storage.list()).size,
    }));
    expect(after.st.meta).toBeNull();
    expect(after.keys).toBe(0);
    const gone = await connect(sid);
    gone.send({ type: "hello", v: 1, token: creator.token, device_id: "d" });
    expect(await gone.waitClosed()).toBe(4004);
  });

  it("ends idle sessions and when the last participant leaves", async () => {
    const { sid, creator } = await sessionWith(1);
    const stub = stubFor(sid);
    const idleSince = (await runInDurableObject(stub, (o: SessionDO) => o.debugState())).meta!.idle_since;
    expect(idleSince).toBeGreaterThan(0);
    expect(await runDurableObjectAlarm(stub)).toBe(true); // early alarm: re-arms, nothing ends
    expect((await runInDurableObject(stub, (o: SessionDO) => o.debugState())).meta!.status).toBe("active");
    await runInDurableObject(stub, (o: SessionDO) => o.runAlarm(idleSince + 6 * 3600 * 1000));
    const st = await runInDurableObject(stub, (o: SessionDO) => o.debugState());
    expect(st.meta!.status).toBe("ended");
    const log = await runInDurableObject(stub, (o: SessionDO) => o.debugLog());
    expect(log.at(-1)).toMatchObject({ kind: "end", by: "" });

    const s2 = await sessionWith(1);
    const a = await join(s2.sid, s2.creator.token);
    a.c.send(op("leave"));
    expect(await a.c.waitClosed()).toBe(4013);
    const st2 = await runInDurableObject(stubFor(s2.sid), (o: SessionDO) => o.debugState());
    expect(st2.meta!.status).toBe("ended");
    expect(creator.id).not.toBe(s2.creator.id);
  });

  it("closes sockets that never say hello", async () => {
    const { sid } = await sessionWith(1);
    const silent = await connect(sid);
    await runInDurableObject(stubFor(sid), (o: SessionDO) => o.runAlarm(Date.now() + 11_000));
    expect(await silent.waitClosed()).toBe(4001);
  });
});

describe("op validation unit", () => {
  it("whitelists fields per kind", () => {
    const r = checkOp({ type: "op", op_id: "a", kind: "seek", item_id: "i", pos: 5, at: 9, junk: 1, by: "evil", rev: 99 });
    expect(r).toEqual({ ok: true, op: { op_id: "a", base_rev: 0, kind: "seek", at: 9, item_id: "i", pos: 5 } });
    expect(checkOp({ op_id: "a", kind: "advanced", from_item: "x", at: 1 })).toMatchObject({ ok: true, op: { to_item: null } });
    expect(checkOp({ op_id: "a", kind: "remove", item_ids: Array(101).fill("x") }).ok).toBe(false);
    expect(checkOp({ op_id: "", kind: "leave" }).ok).toBe(false);
    expect(checkOp({ op_id: "x".repeat(65), kind: "leave" }).ok).toBe(false);
  });
});
