#include "framework.hpp"
#include "json.hpp"
#include "protocol.hpp"
#include "tunegram/lt_core.h"

using lt::json::Value;

TEST(json_parse_roundtrip) {
  Value v;
  REQUIRE(lt::json::parse(R"( {"a":1,"b":[true,false,null],"c":"x\"y\\z\n","d":-9007199254740993,"e":{"f":[]}} )", v));
  CHECK_EQ(v.get_int("a"), 1);
  CHECK(v.get("b")->items()[0].as_bool());
  CHECK(v.get("b")->items()[2].is_null());
  CHECK_EQ(v.get_str("c"), std::string("x\"y\\z\n"));
  CHECK_EQ(v.get_int("d"), static_cast<int64_t>(-9007199254740993LL));
  CHECK_EQ(lt::json::dump(v),
           std::string(R"({"a":1,"b":[true,false,null],"c":"x\"y\\z\n","d":-9007199254740993,"e":{"f":[]}})"));
}

TEST(json_unicode_and_errors) {
  Value v;
  REQUIRE(lt::json::parse(R"("\u00e9\ud83c\udfb5\u0001")", v));
  CHECK_EQ(v.as_str(), std::string("\xC3\xA9\xF0\x9F\x8E\xB5\x01"));
  CHECK_EQ(lt::json::dump(v), std::string("\"\xC3\xA9\xF0\x9F\x8E\xB5\\u0001\""));
  const char* bad[] = {"", "{", "[1,]", "{\"a\" 1}", "01", "1.", "\"\x01\"", "tru", "{} x", "[1 2]", "\"\\q\""};
  for (const char* b : bad) CHECK_MSG(!lt::json::parse(b, v), b);
  // Depth limit.
  std::string deep(100, '[');
  deep += std::string(100, ']');
  CHECK(!lt::json::parse(deep, v));
  // Large and fractional numbers do not fail.
  REQUIRE(lt::json::parse("[18446744073709551616, 1.5e3, -0.25]", v));
  CHECK(v.items()[0].type() == lt::json::Type::Double);
  CHECK_EQ(v.items()[1].as_int(), 1500);
}

TEST(json_protocol_op_roundtrip) {
  lt::Op op;
  op.kind = lt::Kind::Insert;
  op.op_id = "d:1:x";
  op.base_rev = 4;
  op.after_null = false;
  op.after = "a";
  lt::Entry e;
  e.item_id = "i1";
  e.chat_id = "-1001234567890";
  e.msg_id = 77;
  e.doc_id = "5368712345678901234";
  e.title = "Song \"1\"";
  e.performer = "Artist";
  e.duration_ms = 200000;
  op.entries.push_back(e);
  std::string msg = lt::encode_op_message(op);
  Value v;
  REQUIRE(lt::json::parse(msg, v));
  CHECK_EQ(v.get_str("type"), std::string("op"));
  lt::Op back;
  REQUIRE(lt::op_from_json(v, back));
  CHECK(back.kind == lt::Kind::Insert);
  CHECK_EQ(back.after, std::string("a"));
  CHECK(!back.after_null);
  REQUIRE(back.entries.size() == 1);
  CHECK_EQ(back.entries[0].doc_id, std::string("5368712345678901234"));
  CHECK_EQ(back.entries[0].title, std::string("Song \"1\""));
  CHECK_EQ(back.entries[0].duration_ms, static_cast<int64_t>(200000));

  // Every kind survives a round trip.
  for (int k = 1; k <= static_cast<int>(lt::Kind::SetShuffle); ++k) {
    lt::Op o;
    o.kind = static_cast<lt::Kind>(k);
    o.op_id = "x";
    o.rev = 3;
    o.ts = 100;
    o.by = "u";
    o.has_at = true;
    o.at = 90;
    o.has_item = true;
    o.item_id = "it";
    o.from_item = "f";
    o.pos = 1234;
    o.on = true;
    o.seed = 0xDEADBEEF;
    o.mode = lt::Repeat::One;
    o.user_id = "u2";
    Value j = lt::op_to_json(o);
    lt::Op b;
    REQUIRE(lt::op_from_json(j, b));
    CHECK_MSG(b.kind == o.kind, lt::kind_name(o.kind));
    CHECK_EQ(lt::json::dump(lt::op_to_json(b)), lt::json::dump(j));
  }
}

TEST(capi_null_safety) {
  CHECK(lt_core_create(nullptr) == nullptr);
  lt_config bad{"", "t", 0, 0};
  CHECK(lt_core_create(&bad) == nullptr);
  lt_core_destroy(nullptr);
  lt_core_tick(nullptr, 0);
  CHECK_EQ(lt_core_play(nullptr, 0), LT_ERR_INVALID);
  lt_event ev;
  CHECK_EQ(lt_core_poll(nullptr, &ev), 0);
  lt_config cfg{"dev", "tok", 0, 0};
  lt_core* c = lt_core_create(&cfg);
  REQUIRE(c != nullptr);
  CHECK_EQ(lt_core_insert(c, 0, nullptr, nullptr, 0, nullptr), LT_ERR_INVALID);
  CHECK_EQ(lt_core_jump(c, 0, nullptr), LT_ERR_INVALID);
  CHECK_EQ(lt_core_set_repeat(c, 0, 7), LT_ERR_INVALID);
  CHECK_EQ(lt_core_play(c, 0), LT_ERR_NOOP);  // empty queue
  lt_core_on_message(c, 0, "garbage", 7);     // ignored, no crash
  lt_core_start(c, 1000);
  int connects = 0;
  while (lt_core_poll(c, &ev)) connects += ev.type == LT_EV_CONNECT;
  CHECK_EQ(connects, 1);
  CHECK(lt_core_next_timeout(c) == 11000);  // connect timeout
  lt_core_destroy(c);
}
