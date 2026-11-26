// Clock-offset estimation from ping/pong samples (PROTOCOL.md §6).
#pragma once

#include <cstdint>
#include <deque>

namespace lt {

class ClockSync {
 public:
  static constexpr size_t kWindow = 12;
  static constexpr int64_t kMaxRtt = 3000;

  struct Sample {
    int64_t offset;
    int64_t rtt;
  };

  // t0: local send, t1: server receive, t2: server send, t3: local receive.
  void add(int64_t t0, int64_t t1, int64_t t2, int64_t t3) {
    int64_t rtt = (t3 - t0) - (t2 - t1);
    if (rtt < 0) rtt = 0;
    if (rtt > kMaxRtt) return;
    int64_t offset = ((t1 - t0) + (t2 - t3)) / 2;
    samples_.push_back(Sample{offset, rtt});
    while (samples_.size() > kWindow) samples_.pop_front();
    best_ = samples_.front();
    for (const auto& s : samples_) {
      if (s.rtt < best_.rtt) best_ = s;
    }
    ++total_;
  }

  bool valid() const { return !samples_.empty(); }
  int64_t offset() const { return valid() ? best_.offset : 0; }
  int64_t rtt() const { return valid() ? best_.rtt : -1; }
  size_t window_size() const { return samples_.size(); }
  uint64_t total_samples() const { return total_; }

 private:
  std::deque<Sample> samples_;
  Sample best_{0, 0};
  uint64_t total_ = 0;
};

}  // namespace lt
