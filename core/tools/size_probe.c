/* Size probe: references the whole C API so the linker keeps everything an
 * app would need. Compare its stripped size with size_empty to see what the
 * core adds to an app binary. Not meant to be run. */
#include <stdio.h>

#include "tunegram/lt_core.h"

int main(int argc, char** argv) {
  (void)argv;
  lt_config cfg = {"probe", "token", 0, 1};
  lt_core* c = lt_core_create(&cfg);
  int64_t now = argc;
  lt_entry e = {0, "-1001", 1, "2", "t", "p", 1000};
  const char* ids = 0;
  const char* rm[1] = {"x"};
  lt_event ev;
  lt_core_start(c, now);
  lt_core_on_open(c, now);
  lt_core_on_message(c, now, "{}", 2);
  lt_core_on_close(c, now, 1006);
  lt_core_tick(c, now);
  lt_core_set_token(c, now, "t");
  lt_core_player_ready(c, now, "x");
  lt_core_player_started(c, now, "x", 0);
  lt_core_player_position(c, now, "x", 0, 1);
  lt_core_player_ended(c, now, "x");
  lt_core_player_buffering(c, now, "x", 0);
  lt_core_insert(c, now, 0, &e, 1, &ids);
  lt_core_remove(c, now, rm, 1);
  lt_core_move(c, now, "x", 0);
  lt_core_play(c, now);
  lt_core_pause(c, now);
  lt_core_seek(c, now, 5);
  lt_core_next(c, now);
  lt_core_previous(c, now);
  lt_core_jump(c, now, "x");
  lt_core_set_repeat(c, now, 1);
  lt_core_set_shuffle(c, now, 1);
  lt_core_kick(c, now, "u");
  lt_core_end_session(c, now);
  lt_core_set_interrupted(c, now, 0);
  lt_core_leave(c, now);
  while (lt_core_poll(c, &ev)) printf("%d\n", ev.type);
  printf("%s %lld %lld %lld %lld\n", lt_core_state_json(c, now), (long long)lt_core_next_timeout(c),
         (long long)lt_core_server_time(c, now), (long long)lt_core_clock_offset(c), (long long)lt_core_rev(c));
  lt_playback pb = lt_core_playback(c, now);
  printf("%lld %s\n", (long long)pb.pos_ms, lt_core_version());
  lt_core_destroy(c);
  return 0;
}
