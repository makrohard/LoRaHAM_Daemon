#ifndef LORAHAM_CONFIG_PARSER_H
#define LORAHAM_CONFIG_PARSER_H

#include <string>
#include <utility>
#include <vector>

/* --- Parsed SET command --- */

struct ConfigCommand {
    bool is_set;
    bool has_params;
    std::string text;
    std::string mode;
    /* Number of MODE tokens seen (audit P1-3): MODE is extracted from the
     * token list, so plain duplicate-key detection never saw a second MODE
     * and silently applied last-one-wins. */
    int mode_count = 0;
    std::vector<std::pair<std::string, std::string>> tokens;
    std::vector<std::string> malformed_tokens;
};

/* --- SET KEY=VALUE tokenizer --- */

ConfigCommand config_parse_command(const char *cmd);

#endif
