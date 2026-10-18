/* --- CH341 monitor: built into lib/libloraham-ch341.so beside the adapter --- */
/*
 * The adapter's own libusb context for unplug-wait (see ch341_adapter.h). It
 * never touches pine64's context or instance: pine64 opens and drives the
 * stick; this file only watches the bus (hotplug), answers "is the connection
 * I bound still attached" from the context's device list without any request
 * on the bus, and scans sticks one at a time for the selection rules.
 */

#define _GNU_SOURCE
#include "ch341_adapter.h"
#include "ch341_adapter_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MON_QUEUE 32
#define SCAN_BUDGET_MS 200u

typedef struct {
    int left;               /* 1 LEFT, 0 ARRIVED */
    libusb_device *dev;     /* referenced by the callback */
} mon_event;

struct loraham_ch341_mon {
    libusb_context *ctx;
    libusb_hotplug_callback_handle cb;
    uint16_t vid, pid;

    mon_event queue[MON_QUEUE];
    int queued;
    int overflow;

    libusb_device *bound;   /* referenced; NULL when unbound */
    int left_bound;
    int arrivals;
    int lefts;                  /* LEFTs of devices other than the bound one */
    unsigned fd_generation;

    libusb_device **scan_list;
    libusb_device **scan_devs;  /* the 1A86:5512 entries of scan_list, sized from it */
    int scan_count;

    /* A cookie per connection: each known 1A86:5512 object is referenced, so
     * its address cannot be reused by a new connection while it is known; a
     * new object gets a new cookie. Entries no scan lists any more are
     * dropped. (Device numbers repeat on Linux; objects do not.) */
    struct { libusb_device *dev; unsigned cookie; } *known;
    int nknown, cap_known;              /* cap_known: room reserved at scan_begin */
    unsigned next_cookie;
};

/* ---- time ------------------------------------------------------------------ */

#ifdef CH341_MONITOR_TEST_CLOCK
/* Tests only (test_ch341_monitor): the stub libusb's controlled clock, so the
 * serial budget is measured on time the test sets, not on scheduling. */
uint64_t stub_mono_ms(void);
static uint64_t mono_ms(void) { return stub_mono_ms(); }
#else
static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}
#endif

/* ---- hotplug: the callback only records ----------------------------------- */

static int LIBUSB_CALL mon_hotplug_cb(libusb_context *ctx, libusb_device *dev,
                                      libusb_hotplug_event event, void *user)
{
    loraham_ch341_mon *m = (loraham_ch341_mon *)user;
    (void)ctx;

    if (m->queued >= MON_QUEUE) {
        m->overflow = 1;
        return 0;
    }
    m->queue[m->queued].left = (event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT);
    m->queue[m->queued].dev = libusb_ref_device(dev);
    m->queued++;
    return 0;
}

static void LIBUSB_CALL mon_fd_added(int fd, short events, void *user)
{
    (void)fd;
    (void)events;
    ((loraham_ch341_mon *)user)->fd_generation++;
}

static void LIBUSB_CALL mon_fd_removed(int fd, void *user)
{
    (void)fd;
    ((loraham_ch341_mon *)user)->fd_generation++;
}

loraham_ch341_mon *loraham_ch341_mon_open(uint16_t vid, uint16_t pid)
{
    loraham_ch341_mon *m;

    if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG))
        return NULL;
    m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->vid = vid;
    m->pid = pid;
    if (libusb_init(&m->ctx) != 0) {
        free(m);
        return NULL;
    }
    /* Every libusb timeout must be visible through the pollfds, or a monitor
     * driven only by them could miss one. */
    if (!libusb_pollfds_handle_timeouts(m->ctx) ||
        libusb_hotplug_register_callback(m->ctx,
                LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
                LIBUSB_HOTPLUG_NO_FLAGS, vid, pid, LIBUSB_HOTPLUG_MATCH_ANY,
                mon_hotplug_cb, m, &m->cb) != LIBUSB_SUCCESS) {
        libusb_exit(m->ctx);
        free(m);
        return NULL;
    }
    libusb_set_pollfd_notifiers(m->ctx, mon_fd_added, mon_fd_removed, m);
    return m;
}

static void mon_drop_queue(loraham_ch341_mon *m)
{
    for (int i = 0; i < m->queued; i++)
        libusb_unref_device(m->queue[i].dev);
    m->queued = 0;
}

static unsigned mon_cookie(loraham_ch341_mon *m, libusb_device *d)
{
    for (int i = 0; i < m->nknown; i++)
        if (m->known[i].dev == d)
            return m->known[i].cookie;
    /* scan_begin reserved a slot for every listed device. */
    m->known[m->nknown].dev = libusb_ref_device(d);
    m->known[m->nknown].cookie = ++m->next_cookie;
    return m->known[m->nknown++].cookie;
}

static void mon_forget_unlisted(loraham_ch341_mon *m)
{
    int k = 0;
    for (int i = 0; i < m->nknown; i++) {
        int listed = 0;
        for (int j = 0; j < m->scan_count; j++)
            listed = listed || m->scan_devs[j] == m->known[i].dev;
        if (listed)
            m->known[k++] = m->known[i];
        else
            libusb_unref_device(m->known[i].dev);
    }
    m->nknown = k;
}

void loraham_ch341_mon_close(loraham_ch341_mon *m)
{
    if (!m)
        return;
    loraham_ch341_mon_scan_end(m);
    for (int i = 0; i < m->nknown; i++)
        libusb_unref_device(m->known[i].dev);
    free(m->known);
    loraham_ch341_mon_unbind(m);
    libusb_hotplug_deregister_callback(m->ctx, m->cb);
    libusb_set_pollfd_notifiers(m->ctx, NULL, NULL, NULL);
    mon_drop_queue(m);
    libusb_exit(m->ctx);
    free(m);
}

int loraham_ch341_mon_fds(loraham_ch341_mon *m, loraham_ch341_pollfd *out, int max)
{
    const struct libusb_pollfd **fds = libusb_get_pollfds(m->ctx);
    int n = 0;

    if (!fds)
        return -1;
    while (fds[n])
        n++;
    if (n > max) {
        libusb_free_pollfds(fds);
        return -1;
    }
    for (int i = 0; i < n; i++) {
        out[i].fd = fds[i]->fd;
        out[i].events = fds[i]->events;
    }
    libusb_free_pollfds(fds);
    return n;
}

unsigned loraham_ch341_mon_fd_generation(loraham_ch341_mon *m)
{
    return m->fd_generation;
}

void loraham_ch341_mon_dispatch(loraham_ch341_mon *m)
{
    struct timeval zero = { 0, 0 };

    libusb_handle_events_timeout_completed(m->ctx, &zero, NULL);
    for (int i = 0; i < m->queued; i++) {
        if (m->queue[i].left) {
            if (m->bound && m->queue[i].dev == m->bound)
                m->left_bound = 1;
            else
                m->lefts++;
        } else {
            m->arrivals++;
        }
    }
    mon_drop_queue(m);
    if (m->overflow) {
        /* Events were lost: an arrival may be among them, so look again; a
         * lost LEFT may have been the bound stick's: UNCERTAIN (2), never a
         * LEFT (1). A selection does not commit on it; the runtime and the
         * presence check treat it as no LEFT (the two looks decide). */
        m->overflow = 0;
        m->arrivals++;
        if (m->bound && m->left_bound == 0)
            m->left_bound = 2;
    }
}

int loraham_ch341_mon_left_bound(loraham_ch341_mon *m)
{
    return m->left_bound;
}

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

/* ---- the bound connection ------------------------------------------------- */

static int same_location(libusb_device *a, libusb_device *b)
{
    uint8_t pa[8], pb[8];
    int na, nb;

    if (libusb_get_bus_number(a) != libusb_get_bus_number(b) ||
        libusb_get_device_address(a) != libusb_get_device_address(b))
        return 0;
    na = libusb_get_port_numbers(a, pa, sizeof(pa));
    nb = libusb_get_port_numbers(b, pb, sizeof(pb));
    return na > 0 && na == nb && memcmp(pa, pb, (size_t)na) == 0;
}

int loraham_ch341_mon_bind(loraham_ch341_mon *m, loraham_ch341 *dev)
{
    libusb_device *opened, **list, *match = NULL;
    ssize_t n;
    int found = 0;

    loraham_ch341_mon_unbind(m);
    if (!dev || !dev->inst.handle)
        return -1;
    opened = libusb_get_device(dev->inst.handle);
    n = libusb_get_device_list(m->ctx, &list);
    if (n < 0)
        return -1;
    for (ssize_t i = 0; i < n; i++) {
        if (same_location(list[i], opened)) {
            match = list[i];
            found++;
        }
    }
    if (found == 1)
        m->bound = libusb_ref_device(match);
    libusb_free_device_list(list, 1);
    m->left_bound = 0;
    return found == 1 ? 0 : -1;
}

void loraham_ch341_mon_unbind(loraham_ch341_mon *m)
{
    if (m->bound) {
        libusb_unref_device(m->bound);
        m->bound = NULL;
    }
    m->left_bound = 0;
}

/* 1 the bound object is in a fresh list, 0 not, -1 the list failed. */
static int bound_in_list(loraham_ch341_mon *m)
{
    libusb_device **list;
    ssize_t n = libusb_get_device_list(m->ctx, &list);
    int in = 0;

    if (n < 0)
        return -1;
    for (ssize_t i = 0; i < n; i++)
        if (list[i] == m->bound)
            in = 1;
    libusb_free_device_list(list, 1);
    return in;
}

int loraham_ch341_mon_present_bound(loraham_ch341_mon *m)
{
    int r;

    if (!m->bound)
        return 0;
    /* One line names the look that decided and the ones not called (the
     * live proof reads it). */
    r = bound_in_list(m);                    /* look #1 */
    if (r <= 0) {
        printf("[CH341] presence check: %s at list #1 (drain and list #2 not called)\n",
               r == 0 ? "absent" : "list read failed");
        fflush(stdout);
        return r;
    }
    /* libusb_get_device_list() runs the backend's hotplug poll and then
     * snapshots the list; a disconnect removes the object and queues LEFT
     * afterwards, so an unplug can land after look #1. Drain, then look
     * once more. */
    loraham_ch341_mon_dispatch(m);
    if (m->left_bound == 1) {                /* a real LEFT; an overflow (2) is not one */
        printf("[CH341] presence check: present at list #1, LEFT in the drain (list #2 not called)\n");
        fflush(stdout);
        return 0;
    }
    r = bound_in_list(m);                    /* look #2 */
    printf("[CH341] presence check: present at list #1, no LEFT in the drain, %s at list #2\n",
           r == 0 ? "absent" : (r < 0 ? "list read failed" : "present"));
    fflush(stdout);
    return r;
}

/* ---- serial and port path ------------------------------------------------- */

int loraham_ch341_port_path(libusb_device *d, char *out, size_t size)
{
    uint8_t ports[8];
    int n = libusb_get_port_numbers(d, ports, sizeof(ports));
    size_t used;

    if (n <= 0 || size < 4)
        return -1;
    used = (size_t)snprintf(out, size, "%u-%u", libusb_get_bus_number(d), ports[0]);
    for (int i = 1; i < n && used < size; i++)
        used += (size_t)snprintf(out + used, size - used, ".%u", ports[i]);
    return used < size ? 0 : -1;
}

int loraham_ch341_read_serial(libusb_device_handle *h, uint8_t index, char out[9],
                              unsigned budget_ms)
{
    uint64_t deadline = mono_ms() + budget_ms;
    unsigned char langs[4], buf[255];
    uint64_t now;
    int r, k = 0;
    uint16_t lang;

    out[0] = '\0';
    if (index == 0)
        return 0;

    /* GET_DESCRIPTOR(STRING, 0): the language table. */
    r = libusb_control_transfer(h, LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR,
                                (uint16_t)(LIBUSB_DT_STRING << 8), 0, langs, sizeof(langs),
                                budget_ms);
    /* The language table: at least one 2-byte id, an even advertised length.
     * Only the first id is read (the request asks for 4 bytes). */
    if (r < 4 || langs[1] != LIBUSB_DT_STRING || langs[0] < 4 || (langs[0] & 1))
        return -1;
    lang = (uint16_t)(langs[2] | (langs[3] << 8));

    now = mono_ms();
    if (now >= deadline)
        return -1;
    r = libusb_control_transfer(h, LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR,
                                (uint16_t)((LIBUSB_DT_STRING << 8) | index), lang, buf,
                                sizeof(buf), (unsigned)(deadline - now));
    /* The string: an even advertised length of at least one character, all of
     * it returned. A dangling byte or a short read is no serial. */
    if (r < 4 || buf[1] != LIBUSB_DT_STRING || buf[0] < 4 || (buf[0] & 1) || buf[0] > r)
        return -1;
    /* UTF-16LE to ASCII, first 8 characters (what pine64 compares). */
    for (int i = 2; i + 1 < buf[0] && k < 8; i += 2) {
        unsigned c = buf[i] | (buf[i + 1] << 8);
        if (c < 0x20 || c > 0x7e)
            return -1;
        out[k++] = (char)c;
    }
    out[k] = '\0';
    return k > 0 ? 1 : -1;
}

int loraham_ch341_identity(loraham_ch341 *dev, loraham_ch341_scan_item *out)
{
    struct libusb_device_descriptor desc;
    libusb_device *d;

    memset(out, 0, sizeof(*out));
    if (!dev || !dev->inst.handle)
        return -1;
    d = libusb_get_device(dev->inst.handle);
    if (libusb_get_device_descriptor(d, &desc) != 0 ||
        loraham_ch341_port_path(d, out->port, sizeof(out->port)) != 0)
        return -1;
    out->openable = 1;
    out->serial_state = loraham_ch341_read_serial(dev->inst.handle, desc.iSerialNumber,
                                                  out->serial, SCAN_BUDGET_MS);
    return 0;
}

/* ---- scan: one stick per step ---------------------------------------------- */

int loraham_ch341_mon_scan_begin(loraham_ch341_mon *m)
{
    ssize_t n;

    loraham_ch341_mon_scan_end(m);
    n = libusb_get_device_list(m->ctx, &m->scan_list);
    if (n < 0) {
        m->scan_list = NULL;
        return -1;
    }
    /* Every attached stick, never a truncated prefix: no room = no scan. */
    m->scan_devs = calloc((size_t)n + 1, sizeof(*m->scan_devs));
    if (!m->scan_devs) {
        loraham_ch341_mon_scan_end(m);
        return -1;
    }
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(m->scan_list[i], &desc) == 0 &&
            desc.idVendor == m->vid && desc.idProduct == m->pid)
            m->scan_devs[m->scan_count++] = m->scan_list[i];
    }
    mon_forget_unlisted(m);
    /* Room for a cookie per listed device, or no scan (fail closed: never a
     * scan whose candidates share a cookie). */
    if (m->nknown + m->scan_count > m->cap_known) {
        void *grown = realloc(m->known, (size_t)(m->nknown + m->scan_count) * sizeof(*m->known));
        if (!grown) {
            loraham_ch341_mon_scan_end(m);
            return -1;
        }
        m->known = grown;
        m->cap_known = m->nknown + m->scan_count;
    }
    return m->scan_count;
}

void loraham_ch341_mon_scan_step(loraham_ch341_mon *m, int i, loraham_ch341_scan_item *out)
{
    struct libusb_device_descriptor desc;
    libusb_device_handle *h;
    libusb_device *d;
    int r;

    memset(out, 0, sizeof(*out));
    out->serial_state = -1;
    if (i < 0 || i >= m->scan_count) {
        out->openable = -1;
        return;
    }
    d = m->scan_devs[i];
    loraham_ch341_port_path(d, out->port, sizeof(out->port));
    out->connection = mon_cookie(m, d);
    r = libusb_open(d, &h);
    if (r == LIBUSB_ERROR_ACCESS) {
        out->openable = 0;
        return;
    }
    if (r == LIBUSB_ERROR_NO_DEVICE) {
        out->openable = -1;
        return;
    }
    if (r != 0) {
        out->openable = -2;
        return;
    }
    out->openable = 1;
    if (libusb_get_device_descriptor(d, &desc) == 0)
        out->serial_state = loraham_ch341_read_serial(h, desc.iSerialNumber, out->serial,
                                                      SCAN_BUDGET_MS);
    libusb_close(h);
}

void loraham_ch341_mon_scan_end(loraham_ch341_mon *m)
{
    if (m->scan_list)
        libusb_free_device_list(m->scan_list, 1);
    free(m->scan_devs);
    m->scan_list = NULL;
    m->scan_devs = NULL;
    m->scan_count = 0;
}
