#include "config_replay.h"

#include <string.h>

#include <utility>
#include <vector>

static std::string g_replay_mode;
static std::vector<std::pair<std::string, std::string>> g_replay_keys;

void config_apply_replay_note(const std::string &key, const std::string &val)
{
    for (auto &kv : g_replay_keys) {
        if (kv.first == key) {
            kv.second = val;
            return;
        }
    }
    g_replay_keys.emplace_back(key, val);
}

void config_apply_replay_start_mode(const char *mode)
{
    g_replay_mode = mode;
    g_replay_keys.clear();
}

const char *config_apply_replay_value(const char *key)
{
    for (const auto &kv : g_replay_keys) {
        if (strcmp(kv.first.c_str(), key) == 0)
            return kv.second.c_str();
    }
    return NULL;
}

std::string config_apply_replay_command(void)
{
    if (g_replay_mode.empty() && g_replay_keys.empty())
        return "";
    std::string cmd = "SET";
    if (!g_replay_mode.empty())
        cmd += " MODE=" + g_replay_mode;
    for (const auto &kv : g_replay_keys)
        cmd += " " + kv.first + "=" + kv.second;
    return cmd;
}

void config_apply_replay_reset(void)
{
    g_replay_mode.clear();
    g_replay_keys.clear();
}
