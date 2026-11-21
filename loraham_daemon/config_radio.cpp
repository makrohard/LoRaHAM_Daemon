#include "config_radio.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config_replay.h"

/* The text after a parsed number: trailing whitespace only, else "?". */
static void tail_check(const char *end, char *out, size_t out_size)
{
    while (*end && isspace((unsigned char)*end))
        end++;
    if (*end)
        snprintf(out, out_size, "?");
}

/* A decimal with at most six places and at least one ("869.525", "868.0"). */
static void decimal_text(double d, int places, char *out, size_t out_size)
{
    snprintf(out, out_size, "%.*f", places, d);
    size_t n = strlen(out);
    while (n > 2 && out[n - 1] == '0' && out[n - 2] != '.')
        out[--n] = '\0';
}

void config_radio_applied(const char *key, bool decimal, char *out, size_t out_size)
{
    const char *v = config_apply_replay_value(key);
    char *end = NULL;

    snprintf(out, out_size, "-");
    if (!v || !v[0])
        return;
    errno = 0;
    if (decimal) {
        double d = strtod(v, &end);

        if (end == v || errno != 0 || !isfinite(d)) {
            snprintf(out, out_size, "?");
            return;
        }
        decimal_text(d, 6, out, out_size);
    } else {
        long l = strtol(v, &end, 10);

        if (end == v || errno != 0) {
            snprintf(out, out_size, "?");
            return;
        }
        snprintf(out, out_size, "%ld", l);
    }
    tail_check(end, out, out_size);
}

/* SYNC as recorded (the SET grammar: 0x-hex or decimal), printed 0x%02X. */
static void sync_applied(char *out, size_t out_size)
{
    const char *v = config_apply_replay_value("SYNC");
    char *end = NULL;

    snprintf(out, out_size, "-");
    if (!v || !v[0])
        return;
    while (isspace((unsigned char)*v))
        v++;
    bool hex = v[0] == '0' && (v[1] == 'x' || v[1] == 'X');
    errno = 0;
    unsigned long s = strtoul(hex ? v + 2 : v, &end, hex ? 16 : 10);
    if (end == (hex ? v + 2 : v) || errno != 0) {
        snprintf(out, out_size, "?");
        return;
    }
    snprintf(out, out_size, "0x%02lX", s);
    tail_check(end, out, out_size);
}

/* LDRO as recorded: "auto" (any case) or 0/1. */
static void ldro_applied(char *out, size_t out_size)
{
    const char *v = config_apply_replay_value("LDRO");

    snprintf(out, out_size, "-");
    if (!v || !v[0])
        return;
    while (isspace((unsigned char)*v))
        v++;
    if (strncasecmp(v, "auto", 4) == 0) {
        snprintf(out, out_size, "AUTO");
        tail_check(v + 4, out, out_size);
        return;
    }
    config_radio_applied("LDRO", false, out, out_size);
}

/* The record's value, else the default text (NULL: "-"). */
static void value_or(const char *rec, const char *dflt, char *out, size_t out_size)
{
    if (strcmp(rec, "-") != 0)
        snprintf(out, out_size, "%s", rec);
    else
        snprintf(out, out_size, "%s", dflt ? dflt : "-");
}

void config_radio_format(char *buf, size_t buf_size, RadioHealth health, RadioMode_t mode,
                         const RadioRfDefaults *d)
{
    bool lora = mode != RADIO_MODE_FSK;
    char rec[32];
    char df[32];
    char freq[32], sf[32], bw[32], cr[32], pre[32], sync[32], crc[32], ldro[32], power[32];   /* one size: no truncation */

    /* Present in both modes; the band's FREQ and POWER are applied in FSK too. */
    config_radio_applied("FREQ", true, rec, sizeof(rec));
    if (d)
        decimal_text(round((double)d->freq_mhz * 1000.0) / 1000.0, 3, df, sizeof(df));
    value_or(rec, d ? df : NULL, freq, sizeof(freq));
    config_radio_applied("POWER", false, rec, sizeof(rec));
    snprintf(df, sizeof(df), "%d", d ? d->power_dbm : 0);
    value_or(rec, d ? df : NULL, power, sizeof(power));

    /* PREAMBLE and SYNC: the LoRa default, never in FSK (no FSK default is reported). */
    config_radio_applied("PREAMBLE", false, rec, sizeof(rec));
    snprintf(df, sizeof(df), "%d", d ? d->preamble_len : 0);
    value_or(rec, d && lora ? df : NULL, pre, sizeof(pre));
    sync_applied(rec, sizeof(rec));
    snprintf(df, sizeof(df), "0x%02X", d ? (unsigned)d->sync_word : 0u);
    value_or(rec, d && lora ? df : NULL, sync, sizeof(sync));

    if (lora) {
        config_radio_applied("SF", false, rec, sizeof(rec));
        snprintf(df, sizeof(df), "%d", d ? d->spreading_factor : 0);
        value_or(rec, d ? df : NULL, sf, sizeof(sf));
        config_radio_applied("BW", true, rec, sizeof(rec));
        if (d)
            decimal_text(round((double)d->bandwidth_khz * 1000.0) / 1000.0, 3, df, sizeof(df));
        value_or(rec, d ? df : NULL, bw, sizeof(bw));
        config_radio_applied("CR", false, rec, sizeof(rec));
        snprintf(df, sizeof(df), "%d", d ? d->coding_rate : 0);
        value_or(rec, d ? df : NULL, cr, sizeof(cr));
        config_radio_applied("CRC", false, rec, sizeof(rec));
        snprintf(df, sizeof(df), "%d", d && d->crc_on ? 1 : 0);
        value_or(rec, d ? df : NULL, crc, sizeof(crc));
        ldro_applied(rec, sizeof(rec));
        if (d && d->ldro < 0)
            snprintf(df, sizeof(df), "AUTO");
        else
            snprintf(df, sizeof(df), "%d", d ? d->ldro : 0);
        value_or(rec, d ? df : NULL, ldro, sizeof(ldro));
    } else {
        snprintf(sf, sizeof(sf), "-");
        snprintf(bw, sizeof(bw), "-");
        snprintf(cr, sizeof(cr), "-");
        snprintf(crc, sizeof(crc), "-");
        snprintf(ldro, sizeof(ldro), "-");
    }

    snprintf(buf, buf_size,
             "RADIO RADIO=%s MODE=%s FREQ=%s SF=%s BW=%s CR=%s PREAMBLE=%s SYNC=%s CRC=%s LDRO=%s POWER=%s\n",
             radio_health_name(health), lora ? "LORA" : "FSK", freq, sf, bw, cr, pre, sync, crc, ldro, power);
}
