#ifndef LORAHAM_CONFIG_REPLAY_H
#define LORAHAM_CONFIG_REPLAY_H

#include <string>

/* The replay record (Meshstick unplug-wait): the MODE and, per key, the last
 * value a successful apply wrote since that MODE (a MODE switch reloads the
 * band defaults, so it starts the record over). Rejected and failed keys never
 * enter it; GETRSSI and other one-shot commands are not part of it. The CONF
 * apply path (config_apply.cpp) is its only writer; the re-init replays it and
 * GET STATUS reports FREQ=, SF= and POWER= from it. Main loop only. */

/* "SET [MODE=<m>] K=V ...", or "" when nothing was applied: the command that
 * rebuilds the radio's configuration through the same apply path. */
std::string config_apply_replay_command(void);
void config_apply_replay_reset(void);

/* A successful MODE switch: the record starts over with that MODE. */
void config_apply_replay_start_mode(const char *mode);
/* A successfully applied key: its value replaces the key's last one, or the
 * key is appended. Keys arrive upper-case from the parser. */
void config_apply_replay_note(const std::string &key, const std::string &val);
/* The key's recorded value, or NULL when no successful apply wrote it since
 * boot or the last MODE switch. Valid until the record next changes. */
const char *config_apply_replay_value(const char *key);

#endif
