/*
 * A scripted Meshstick library for the REAL daemon binary (test_ch341_process):
 * every symbol the loader resolves, behind the adapter API of ch341_adapter.h.
 * Built by the test at run time as lib/libloraham-ch341.so (-DFAKE_PIN=...).
 *
 * The bus is a text file the test rewrites: $LORAHAM_FAKE_CH341/bus, one stick
 * per line, "<port> <serial|-> <gen> [flags...]". A stick whose line is gone
 * is unplugged; a new <gen> on the same port is a new connection (a replug).
 * Flags: access (scan and open see ACCESS), detachfail (pine64's claim fails),
 * badchip (the chip does not identify: begin() fails), ioerr (every transfer
 * fails while the stick stays present), failafterrx (transfers fail after the
 * first SetRx: lora_init() succeeds, the replay that follows fails),
 * unreadable (a serial index whose string cannot be read).
 *
 * A claim is an flock on $DIR/claim-<port>, so two daemons (and the test) see
 * each other's claims. $DIR/log gets one line per open/close/monitor event with
 * the pid. $DIR/nohotplug makes mon_open fail. $DIR/fdmode: "pipe" = one
 * readable pipe fd for the main loop (drained by dispatch); "fail" = the pipe
 * replaced by a new readable one at the same fd number and mon_fds() -1.
 * $DIR/scandelay: the next scan step logs "scanwait" and is held there.
 * $DIR/opendelay: the next pine64 open, after its claim, logs "openwait" and
 * is held there: a blocking pinedio_init.
 * $DIR/binddelay: the next bind logs "bindwait" and is held there: the bind's
 * own device-list read.
 * Held = entered/release: the step logs its marker, then waits until the test
 * removes the control file (the release); the test changes the bus before it
 * releases. 20 s is only a watchdog: then "holdtimeout <file>" is logged.
 * $DIR/overflow: the next dispatch reports lost events (once), as the real
 * monitor does on a queue overflow: one arrival, and "uncertain" (2) for the
 * bound stick.
 *
 * The SPI side is the SX126x responder of tests/fakes/sx126x_register_model.h
 * in C: status byte OK, registers stored and read back, the packet type kept.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#include "../../ch341_adapter.h"

#ifndef FAKE_PIN
#define FAKE_PIN "unset"
#endif

#define MAXS 16

typedef struct {
    char port[32];
    char serial[16];      /* "" = none */
    int gen;
    int access, detachfail, badchip, ioerr, failafterrx, unreadable;
} stick;

struct loraham_ch341 {
    stick s;
    int lockfd;
    int rx_seen;
    uint8_t *regs;
    uint8_t packet_type;
};

struct loraham_ch341_mon {
    stick last[MAXS];
    int nlast;
    int bound;            /* a bound connection: port + gen below */
    char bound_port[32];
    int bound_gen;
    int left_bound;
    int arrivals;
    int lefts;
    stick scan[MAXS];
    int nscan;
    int fd_r, fd_w;       /* the pipe handed out as a pollfd */
    unsigned gen;
    int failed_mode;
};

static const char *dir(void)
{
    const char *d = getenv("LORAHAM_FAKE_CH341");
    return d ? d : "/nonexistent";
}

static uint64_t now_ms(void);

/* "<pid> <monotonic ms> <event>" per line. */
static void logline(const char *fmt, const char *a)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/log", dir());
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    fprintf(f, "%d %llu ", (int)getpid(), (unsigned long long)now_ms());
    fprintf(f, fmt, a);
    fputc('\n', f);
    fclose(f);
}

static int read_bus(stick *out)
{
    char path[512], line[256];
    int n = 0;
    snprintf(path, sizeof(path), "%s/bus", dir());
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    while (n < MAXS && fgets(line, sizeof(line), f)) {
        stick s;
        char serial[16], flags[128] = "";
        memset(&s, 0, sizeof(s));
        if (sscanf(line, "%31s %15s %d %127[^\n]", s.port, serial, &s.gen, flags) < 3)
            continue;
        if (strcmp(serial, "-") != 0)
            snprintf(s.serial, sizeof(s.serial), "%s", serial);
        s.access = strstr(flags, "access") != NULL;
        s.detachfail = strstr(flags, "detachfail") != NULL;
        s.badchip = strstr(flags, "badchip") != NULL;
        s.ioerr = strstr(flags, "ioerr") != NULL;
        s.failafterrx = strstr(flags, "failafterrx") != NULL;
        s.unreadable = strstr(flags, "unreadable") != NULL;
        out[n++] = s;
    }
    fclose(f);
    return n;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* The current line of this connection, or NULL when it is gone (cached 20 ms). */
/* The bus as the device's transfers see it, re-read at most every 20 ms; a
 * miss is checked against a fresh read before the stick counts as gone (a
 * stick opened right after a cached read must not look unplugged). */
static const stick *current(loraham_ch341 *d)
{
    static stick bus[MAXS];
    static int n;
    static uint64_t at;
    uint64_t t = now_ms();
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1 || t - at >= 20 || at == 0) {
            n = read_bus(bus);
            at = t;
        }
        for (int i = 0; i < n; i++)
            if (strcmp(bus[i].port, d->s.port) == 0 && bus[i].gen == d->s.gen)
                return &bus[i];
    }
    return NULL;
}

const char *loraham_ch341_pin(void) { return FAKE_PIN; }

static int claim(const char *port)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/claim-%s", dir(), port);
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int file_exists(const char *name);
static void unlink_ctl(const char *name);

/* The entered/release handshake: the marker says the step is entered; the
 * step stays until the test removes ctl. No timing window decides a result. */
static void hold(const char *ctl, const char *marker, const char *arg)
{
    logline(marker, arg);
    for (int i = 0; i < 2000 && file_exists(ctl); i++) {
        struct timespec ts = { 0, 10000000L };
        nanosleep(&ts, NULL);
    }
    if (file_exists(ctl)) {
        logline("holdtimeout %s", ctl);
        unlink_ctl(ctl);
    }
}

loraham_ch341 *loraham_ch341_open(uint16_t vid, uint16_t pid, const char *serial)
{
    stick bus[MAXS];
    int n = read_bus(bus);
    (void)vid;
    (void)pid;
    for (int i = 0; i < n; i++) {
        if (bus[i].access || bus[i].detachfail)
            continue;
        if (serial && strncmp(bus[i].serial, serial, 8) != 0)
            continue;
        int fd = claim(bus[i].port);
        if (fd < 0)
            continue;
        loraham_ch341 *d = calloc(1, sizeof(*d));
        d->s = bus[i];
        d->lockfd = fd;
        d->regs = calloc(1, 0x10000);
        memcpy(&d->regs[0x0320], bus[i].badchip ? "XX0000 V0D 0D00" : "SX1261 V2D 2D02", 16);
        logline("open %s", bus[i].port);
        if (file_exists("opendelay"))               /* the test changes the bus during this open */
            hold("opendelay", "openwait %s", bus[i].port);
        return d;
    }
    logline("open-none %s", serial ? serial : "-");
    return NULL;
}

void loraham_ch341_close(loraham_ch341 *d)
{
    if (!d)
        return;
    logline("close %s", d->s.port);
    close(d->lockfd);
    free(d->regs);
    free(d);
}

static int gone_or_failing(loraham_ch341 *d)
{
    const stick *c = current(d);
    if (!c || c->ioerr)
        return 1;
    return c->failafterrx && d->rx_seen;
}

int loraham_ch341_transceive(loraham_ch341 *d, const uint8_t *out, uint8_t *in, uint32_t len)
{
    if (gone_or_failing(d))
        return -1;
    if (len == 0)
        return 0;
    uint8_t op = out[0];
    if (in) {
        memset(in, 0, len);
        if (len > 1)
            in[1] = 0x22;                         /* STBY_RC, no error */
    }
    if (op == 0x0D && len > 3) {                  /* WriteRegister */
        uint16_t a = (uint16_t)((out[1] << 8) | out[2]);
        for (uint32_t i = 3; i < len; i++)
            d->regs[a++] = out[i];
    } else if (op == 0x1D && len > 4 && in) {     /* ReadRegister */
        uint16_t a = (uint16_t)((out[1] << 8) | out[2]);
        for (uint32_t i = 4; i < len; i++)
            in[i] = d->regs[a++];
    } else if (op == 0x8A && len > 1) {
        d->packet_type = out[1];
    } else if (op == 0x11 && len > 2 && in) {
        in[2] = d->packet_type;
    } else if (op == 0x82) {                      /* SetRx */
        d->rx_seen = 1;
    } else if (op == 0xC5 || op == 0x83) {        /* SetCad / SetTx: a TX is in flight */
        logline("txpath %s", d->s.port);
    }
    return 0;
}

int loraham_ch341_pin_mode(loraham_ch341 *d, uint32_t p, int o) { (void)p; (void)o; return current(d) ? 0 : -1; }
int loraham_ch341_digital_write(loraham_ch341 *d, uint32_t p, int l) { (void)p; (void)l; return current(d) ? 0 : -1; }
int loraham_ch341_digital_read(loraham_ch341 *d, uint32_t p) { (void)p; return current(d) ? 0 : -1; }
int loraham_ch341_attach_interrupt(loraham_ch341 *d, uint32_t p, uint32_t m, void (*cb)(void))
{
    (void)p; (void)m; (void)cb;
    return current(d) ? 0 : -1;
}
int loraham_ch341_detach_interrupt(loraham_ch341 *d, uint32_t p) { (void)d; (void)p; return 0; }
int loraham_ch341_in_error(loraham_ch341 *d) { return current(d) ? 0 : 1; }

/* ---- the monitor ----------------------------------------------------------- */

static int file_exists(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir(), name);
    return access(path, F_OK) == 0;
}

static void unlink_ctl(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir(), name);
    unlink(path);
}

static int fdmode_is(const char *mode)
{
    char path[512], buf[16] = "";
    snprintf(path, sizeof(path), "%s/fdmode", dir());
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    if (!fgets(buf, sizeof(buf), f))
        buf[0] = '\0';
    fclose(f);
    return strncmp(buf, mode, strlen(mode)) == 0;
}

loraham_ch341_mon *loraham_ch341_mon_open(uint16_t vid, uint16_t pid)
{
    (void)vid;
    (void)pid;
    if (file_exists("nohotplug"))
        return NULL;
    loraham_ch341_mon *m = calloc(1, sizeof(*m));
    m->nlast = read_bus(m->last);
    m->fd_r = m->fd_w = -1;
    if (fdmode_is("pipe")) {                      /* like libusb: its fds exist from the context's start */
        int p[2];
        if (pipe2(p, O_CLOEXEC | O_NONBLOCK) == 0) {
            m->fd_r = p[0];
            m->fd_w = p[1];
            m->gen = 1;
        }
    }
    logline("mon_open %s", "-");
    return m;
}

void loraham_ch341_mon_close(loraham_ch341_mon *m)
{
    if (!m)
        return;
    logline("mon_close %s", "-");
    if (m->fd_r >= 0) {
        close(m->fd_r);
        close(m->fd_w);
    }
    free(m);
}

int loraham_ch341_mon_fds(loraham_ch341_mon *m, loraham_ch341_pollfd *out, int max)
{
    if (fdmode_is("pipe") && m->fd_r >= 0) {
        if (max < 1)
            return -1;
        if (write(m->fd_w, "x", 1) != 1) { }     /* readable: an event to handle */
        out[0].fd = m->fd_r;
        out[0].events = 0x001;
        return 1;
    }
    if (fdmode_is("fail") && !m->failed_mode) {
        /* The old fd closed and its number reused by a new, readable pipe
         * nobody drains: a stale registration would spin the main loop. */
        int old = m->fd_r;
        if (old >= 0) {
            close(m->fd_r);
            close(m->fd_w);
            int p[2];
            if (pipe2(p, O_CLOEXEC | O_NONBLOCK) == 0) {
                m->fd_r = p[0];
                m->fd_w = p[1];
                if (write(m->fd_w, "x", 1) != 1) { }
                logline(m->fd_r == old ? "fd_reused %s" : "fd_other %s", "-");
            }
        }
        m->failed_mode = 1;
        m->gen++;
    }
    if (m->failed_mode)
        return -1;
    return 0;
}

unsigned loraham_ch341_mon_fd_generation(loraham_ch341_mon *m)
{
    if (fdmode_is("fail") && !m->failed_mode)
        return m->gen + 1;                        /* the main loop re-reads mon_fds */
    return m->gen;
}

void loraham_ch341_mon_dispatch(loraham_ch341_mon *m)
{
    stick now[MAXS];
    int n = read_bus(now);

    if (!m->failed_mode && m->fd_r >= 0) {
        char b[64];
        while (read(m->fd_r, b, sizeof(b)) > 0) { }
    }
    for (int i = 0; i < m->nlast; i++) {
        int still = 0;
        for (int j = 0; j < n; j++)
            if (strcmp(now[j].port, m->last[i].port) == 0 && now[j].gen == m->last[i].gen)
                still = 1;
        if (!still && m->bound && strcmp(m->bound_port, m->last[i].port) == 0 &&
            m->bound_gen == m->last[i].gen)
            m->left_bound = 1;
        else if (!still)
            m->lefts++;
    }
    for (int j = 0; j < n; j++) {
        int was = 0;
        for (int i = 0; i < m->nlast; i++)
            if (strcmp(now[j].port, m->last[i].port) == 0 && now[j].gen == m->last[i].gen)
                was = 1;
        if (!was)
            m->arrivals++;
    }
    memcpy(m->last, now, sizeof(now));
    m->nlast = n;
    if (file_exists("overflow")) {
        unlink_ctl("overflow");
        logline("overflow %s", "-");
        m->arrivals++;
        if (m->bound && m->left_bound == 0)
            m->left_bound = 2;
    }
}

int loraham_ch341_mon_left_bound(loraham_ch341_mon *m) { return m->left_bound; }

int loraham_ch341_mon_take_arrival(loraham_ch341_mon *m)
{
    if (m->arrivals <= 0)
        return 0;
    m->arrivals--;
    return 1;
}

int loraham_ch341_mon_take_left(loraham_ch341_mon *m)
{
    if (m->lefts <= 0)
        return 0;
    m->lefts--;
    return 1;
}

int loraham_ch341_mon_bind(loraham_ch341_mon *m, loraham_ch341 *d)
{
    if (file_exists("binddelay"))                   /* the test changes the bus during the bind's list read */
        hold("binddelay", "bindwait %s", d->s.port);
    m->bound = 1;
    snprintf(m->bound_port, sizeof(m->bound_port), "%s", d->s.port);
    m->bound_gen = d->s.gen;
    m->left_bound = 0;
    return 0;
}

void loraham_ch341_mon_unbind(loraham_ch341_mon *m) { m->bound = 0; m->left_bound = 0; }

int loraham_ch341_mon_present_bound(loraham_ch341_mon *m)
{
    stick now[MAXS];
    int n = read_bus(now);
    for (int j = 0; j < n; j++)
        if (m->bound && strcmp(now[j].port, m->bound_port) == 0 && now[j].gen == m->bound_gen)
            return 1;
    return 0;
}

int loraham_ch341_mon_scan_begin(loraham_ch341_mon *m)
{
    m->nscan = read_bus(m->scan);
    return m->nscan;
}

void loraham_ch341_mon_scan_step(loraham_ch341_mon *m, int i, loraham_ch341_scan_item *o)
{
    if (file_exists("scandelay"))                   /* the test changes the bus during this step */
        hold("scandelay", "scanwait %s", "-");
    /* libusb_open + libusb_close on the monitor context: two pollfd
     * notifications, its fds otherwise unchanged. */
    if (!m->failed_mode)
        m->gen += 2;
    memset(o, 0, sizeof(*o));
    if (i < 0 || i >= m->nscan) {
        o->openable = -1;
        return;
    }
    snprintf(o->port, sizeof(o->port), "%s", m->scan[i].port);
    o->connection = (unsigned)m->scan[i].gen;       /* a replug (new gen) is a new connection */
    if (m->scan[i].access) {
        o->openable = 0;
        o->serial_state = -1;
        return;
    }
    o->openable = 1;
    o->serial_state = m->scan[i].unreadable ? -1 : (m->scan[i].serial[0] ? 1 : 0);
    if (o->serial_state == 1)
        snprintf(o->serial, sizeof(o->serial), "%.8s", m->scan[i].serial);
}

void loraham_ch341_mon_scan_end(loraham_ch341_mon *m) { m->nscan = 0; }

int loraham_ch341_identity(loraham_ch341 *d, loraham_ch341_scan_item *o)
{
    memset(o, 0, sizeof(*o));
    snprintf(o->port, sizeof(o->port), "%s", d->s.port);
    o->connection = (unsigned)d->s.gen;
    o->openable = 1;
    o->serial_state = d->s.unreadable ? -1 : (d->s.serial[0] ? 1 : 0);
    if (o->serial_state == 1)
        snprintf(o->serial, sizeof(o->serial), "%.8s", d->s.serial);
    return 0;
}

void loraham_ch341_seed_idle(void) { }
