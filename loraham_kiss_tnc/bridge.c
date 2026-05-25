#include "bridge.h"

#include "bridge_loop.h"

/*
 * Public bridge entry point.
 * The implementation lives in bridge_loop.c.
 */

int lhkt_bridge_run(const lhkt_config_t *cfg, lhkt_stats_t *stats)
{
    return bridge_loop_run(cfg, stats);
}
