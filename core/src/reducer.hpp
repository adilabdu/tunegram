// The deterministic reducer (PROTOCOL.md §9). Pure functions of (state, record).
#pragma once

#include <string>
#include <vector>

#include "model.hpp"

namespace lt {

struct Timeline {
  std::string item;
  int64_t pos = 0;
  bool playing = false;
};

uint64_t splitmix64(uint64_t& state);

const Entry* find_entry(const State& s, const std::string& id);  // alive or tombstone
const Entry* find_alive(const State& s, const std::string& id);
std::vector<std::string> play_order(const State& s);
std::string succ_cmd(const State& s, const std::string& id);
std::string succ_auto(const State& s, const std::string& id);
size_t alive_count(const State& s);

// Derived playback at server time t (automatic advance through durations).
Timeline timeline_at(const State& s, int64_t t);

// Applies one record. With optimistic=true (a pending local op) the op's `at`
// stands in for the server timestamp.
void apply(State& s, const Op& op, bool optimistic = false);

}  // namespace lt
