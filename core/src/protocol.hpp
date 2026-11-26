// Protocol encode/decode (PROTOCOL.md §4–§8).
#pragma once

#include <string>

#include "json.hpp"
#include "model.hpp"

namespace lt {

json::Value entry_to_json(const Entry& e, bool with_added_by);
bool entry_from_json(const json::Value& v, Entry& out);

// Op/record <-> JSON object (without the "type" field). Server stamps
// (rev, ts, by, seed) are written when present.
json::Value op_to_json(const Op& op);
bool op_from_json(const json::Value& v, Op& out);

std::string encode_op_message(const Op& op);
std::string encode_hello(int version, const std::string& token, const std::string& device_id, int64_t since_rev,
                         int64_t t0);
std::string encode_ping(int64_t t0);
std::string encode_sync(int64_t since_rev);
std::string encode_status(const char* state);

// Canonical JSON of the whole reducer state (including tombstones, history and
// the anchor). Two states are equal iff these strings are equal.
std::string state_digest(const State& s);

}  // namespace lt
