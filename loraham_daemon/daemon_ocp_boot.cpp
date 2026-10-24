#include "daemon_ocp_boot.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

/* --- Boot-time PA over-current limit (--ocp) ------------------------------ */

#define OCP_DEFAULT_MA 100

static bool g_ocp_given = false;
static int g_ocp_ma = 0;

bool daemon_set_ocp_boot_global(const char *arg)
{
    char *endptr;
    long v;

    /* Digits only: strtol alone would also take " 100" and "+100". */
    if (!arg || arg[0] < '0' || arg[0] > '9')
        return false;

    errno = 0;
    v = strtol(arg, &endptr, 10);
    if (errno == ERANGE || *endptr != '\0' || v < 0 || v > INT_MAX)
        return false;

    g_ocp_given = true;
    g_ocp_ma = (int)v;
    return true;
}

bool daemon_ocp_boot_given(void)
{
    return g_ocp_given;
}

int daemon_ocp_boot_requested_ma(void)
{
    return g_ocp_ma;
}

int daemon_ocp_default_ma(DaemonChipFamily family)
{
    (void)family;                 /* one default on every chip */
    return OCP_DEFAULT_MA;
}

/*
 * The representable sets follow the register encodings RadioLib writes:
 * SX127x OcpTrim is (mA - 45) / 5 up to 120 and (mA + 30) / 10 above, with 0
 * meaning OcpOn cleared; SX126x is mA / 2.5, which for an integer is exact
 * only on multiples of 5. Anything else would be truncated by RadioLib.
 */
bool daemon_ocp_valid_ma(DaemonChipFamily family, int ma)
{
    if (family == DAEMON_CHIP_FAMILY_SX1262)
        return ma >= 5 && ma <= 140 && ma % 5 == 0;

    if (ma == 0)
        return true;
    if (ma >= 45 && ma <= 120)
        return ma % 5 == 0;
    if (ma >= 130 && ma <= 240)
        return ma % 10 == 0;
    return false;
}

const char *daemon_ocp_valid_text(DaemonChipFamily family)
{
    if (family == DAEMON_CHIP_FAMILY_SX1262)
        return "5..140 in steps of 5";
    return "0 (off), 45..120 in steps of 5, 130..240 in steps of 10";
}

int daemon_ocp_effective_ma(DaemonChipFamily family)
{
    return g_ocp_given ? g_ocp_ma : daemon_ocp_default_ma(family);
}

bool daemon_ocp_notice(DaemonChipFamily family, bool high_power,
                       char *buf, size_t buf_size)
{
    const int ma = daemon_ocp_effective_ma(family);

    if (!buf || buf_size == 0)
        return false;

    /* --ocp 0 switches the protection off; only SX127x can (SX1262 refuses 0). */
    if (g_ocp_given && g_ocp_ma == 0 && family == DAEMON_CHIP_FAMILY_SX127X) {
        snprintf(buf, buf_size,
                 "WARNING OCP disabled (--ocp 0): warranty void, hardware "
                 "may be in danger");
        return true;
    }

    /* --high-power without --ocp, or with --ocp at or below the default --
     * on SX127x only: on SX1262 --high-power is inert and says so itself. */
    if (high_power && family == DAEMON_CHIP_FAMILY_SX127X && ma <= OCP_DEFAULT_MA) {
        snprintf(buf, buf_size,
                 "WARNING OCP %d mA under --high-power: may limit output "
                 "power (give --ocp above %d)", ma, OCP_DEFAULT_MA);
        return true;
    }
    return false;
}

void daemon_ocp_boot_reset(void)
{
    g_ocp_given = false;
    g_ocp_ma = 0;
}
