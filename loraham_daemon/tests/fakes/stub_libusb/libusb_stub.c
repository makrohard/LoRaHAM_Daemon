/* The stub libusb behind tests/fakes/stub_libusb/libusb-1.0/libusb.h.
 *
 * One monitor context and the default context share one bus of up to 96
 * devices. A device has a bus, an address, a port path, a serial (or none) and
 * flags for ACCESS / stalls. Removing a device makes its object leave the
 * list; plugging it again makes a NEW object (a new connection), possibly at
 * the same bus/address, as Linux does. Hotplug events are queued by the test
 * and delivered by handle_events; a test can also have an event queued only
 * AFTER a device-list snapshot (stub_after_list_*). Every reference the
 * default context gets through libusb_init(NULL) is counted, and so are the
 * opens and control transfers, so a test can assert "no I/O". */
#include "libusb-1.0/libusb.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

struct libusb_context { int dummy; };

struct libusb_device {
    int refs;
    int attached;
    uint8_t bus, addr;
    uint8_t ports[8];
    int nports;
    uint16_t vid, pid;
    const char *serial;   /* NULL = no serial (index 0) */
    int access_denied;
    int stall;            /* control transfers time out */
    int malformed;        /* 1 odd serial length, 2 a 2-byte language table */
    int delay_ms;         /* the language-table request takes this long */
};

struct libusb_device_handle { libusb_device *dev; };

/* ---- state the tests read and drive ---------------------------------------- */

int stub_default_refs;      /* libusb_init(NULL) minus libusb_exit(NULL) */
int stub_opens;             /* libusb_open calls */
int stub_controls;          /* control transfers */
int stub_has_hotplug = 1;
int stub_handles_timeouts = 1;
int stub_list_fails;
int stub_fd_changes;
unsigned stub_timeouts[8]; /* the timeout of each control transfer (the test resets the count) */
int stub_ntimeouts;

static libusb_device *g_devs[96];
static int g_ndevs;
static libusb_hotplug_callback_fn g_cb;
static void *g_cb_user;
static libusb_pollfd_added_cb g_added;
static void *g_fd_user;
static struct { libusb_device *dev; int left; } g_pending[64];   /* more than the monitor's queue: overflow tests */
static int g_npending;
static struct { libusb_device *dev; int left; } g_after_list[64];   /* more than the monitor's queue */
static int g_nafter;

libusb_device *stub_plug(uint8_t bus, uint8_t addr, const uint8_t *ports, int nports,
                         const char *serial)
{
    libusb_device *d = calloc(1, sizeof(*d));
    d->refs = 1;
    d->attached = 1;
    d->bus = bus;
    d->addr = addr;
    memcpy(d->ports, ports, (size_t)nports);
    d->nports = nports;
    d->vid = 0x1A86;
    d->pid = 0x5512;
    d->serial = serial;
    g_devs[g_ndevs++] = d;
    return d;
}

void stub_unplug(libusb_device *d)
{
    d->attached = 0;
}

void stub_queue_event(libusb_device *d, int left)
{
    if (g_npending >= (int)(sizeof(g_pending) / sizeof(g_pending[0])))
        return;
    g_pending[g_npending].dev = d;
    g_pending[g_npending].left = left;
    g_npending++;
}

/* Queue the event only after the next device-list snapshot (removal follows). */
void stub_after_list_event(libusb_device *d, int left)
{
    if (g_nafter >= (int)(sizeof(g_after_list) / sizeof(g_after_list[0])))
        return;
    g_after_list[g_nafter].dev = d;
    g_after_list[g_nafter].left = left;
    g_nafter++;
}

void stub_set_access(libusb_device *d, int denied) { d->access_denied = denied; }
void stub_set_stall(libusb_device *d, int stall) { d->stall = stall; }
void stub_set_malformed(libusb_device *d, int m) { d->malformed = m; }
void stub_set_delay(libusb_device *d, int ms) { d->delay_ms = ms; }
int stub_refs(libusb_device *d) { return d->refs; }

/* ---- the API ------------------------------------------------------------------ */

static libusb_context g_monitor_ctx;

int libusb_init(libusb_context **ctx)
{
    if (ctx)
        *ctx = &g_monitor_ctx;
    else
        stub_default_refs++;
    return 0;
}

void libusb_exit(libusb_context *ctx)
{
    if (!ctx)
        stub_default_refs--;
}

int libusb_has_capability(uint32_t c) { return c == LIBUSB_CAP_HAS_HOTPLUG && stub_has_hotplug; }
int libusb_pollfds_handle_timeouts(libusb_context *ctx) { (void)ctx; return stub_handles_timeouts; }

int libusb_hotplug_register_callback(libusb_context *ctx, int events, int flags, int vid, int pid,
                                     int dev_class, libusb_hotplug_callback_fn cb, void *user,
                                     libusb_hotplug_callback_handle *h)
{
    (void)ctx; (void)events; (void)flags; (void)vid; (void)pid; (void)dev_class;
    g_cb = cb;
    g_cb_user = user;
    *h = 1;
    return LIBUSB_SUCCESS;
}

void libusb_hotplug_deregister_callback(libusb_context *ctx, libusb_hotplug_callback_handle h)
{
    (void)ctx; (void)h;
    g_cb = NULL;
}

void libusb_set_pollfd_notifiers(libusb_context *ctx, libusb_pollfd_added_cb a,
                                 libusb_pollfd_removed_cb r, void *user)
{
    (void)ctx; (void)r;
    g_added = a;
    g_fd_user = user;
}

void stub_fd_added(int fd)
{
    if (g_added)
        g_added(fd, 1, g_fd_user);
}

static struct libusb_pollfd g_pfd = { 42, 1 };
static const struct libusb_pollfd *g_pfds[2] = { &g_pfd, NULL };

const struct libusb_pollfd **libusb_get_pollfds(libusb_context *ctx)
{
    (void)ctx;
    const struct libusb_pollfd **copy = malloc(sizeof(g_pfds));
    memcpy(copy, g_pfds, sizeof(g_pfds));
    return copy;
}

void libusb_free_pollfds(const struct libusb_pollfd **fds) { free((void *)fds); }

int libusb_handle_events_timeout_completed(libusb_context *ctx, struct timeval *tv, int *done)
{
    (void)tv; (void)done;
    for (int i = 0; i < g_npending; i++)
        if (g_cb)
            g_cb(ctx, g_pending[i].dev,
                 g_pending[i].left ? LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT
                                   : LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED, g_cb_user);
    g_npending = 0;
    return 0;
}

ssize_t libusb_get_device_list(libusb_context *ctx, libusb_device ***list)
{
    (void)ctx;
    if (stub_list_fails)
        return LIBUSB_ERROR_IO;
    libusb_device **l = calloc(97, sizeof(*l));
    ssize_t n = 0;
    for (int i = 0; i < g_ndevs; i++)
        if (g_devs[i]->attached)
            l[n++] = libusb_ref_device(g_devs[i]);
    *list = l;
    /* Events that "land after the snapshot": the removal happens now, the LEFT
     * is queued for the next handle_events. */
    /* left: 1 removal + LEFT queued, 2 removal only (its LEFT not queued yet). */
    for (int i = 0; i < g_nafter; i++) {
        if (g_after_list[i].left)
            g_after_list[i].dev->attached = 0;
        if (g_after_list[i].left != 2)
            stub_queue_event(g_after_list[i].dev, g_after_list[i].left);
    }
    g_nafter = 0;
    return n;
}

void libusb_free_device_list(libusb_device **list, int unref)
{
    if (!list)
        return;
    for (int i = 0; unref && list[i]; i++)
        libusb_unref_device(list[i]);
    free(list);
}

int stub_ref_calls, stub_unref_calls;   /* every libusb_ref_device / libusb_unref_device */
int stub_refs_went_negative;            /* set once any device's count drops below 0 */
libusb_device *libusb_ref_device(libusb_device *d) { stub_ref_calls++; d->refs++; return d; }
void libusb_unref_device(libusb_device *d)
{
    stub_unref_calls++;
    if (--d->refs < 0)
        stub_refs_went_negative = 1;
}
libusb_device *libusb_get_device(libusb_device_handle *h) { return h->dev; }
uint8_t libusb_get_bus_number(libusb_device *d) { return d->bus; }
uint8_t libusb_get_device_address(libusb_device *d) { return d->addr; }

int libusb_get_port_numbers(libusb_device *d, uint8_t *ports, int len)
{
    if (d->nports > len)
        return -1;
    memcpy(ports, d->ports, (size_t)d->nports);
    return d->nports;
}

int libusb_get_device_descriptor(libusb_device *d, struct libusb_device_descriptor *desc)
{
    desc->idVendor = d->vid;
    desc->idProduct = d->pid;
    desc->iSerialNumber = d->serial ? 3 : 0;
    return 0;
}

int stub_closes;            /* libusb_close calls */
int stub_live_handles;      /* opened minus closed */
int libusb_open(libusb_device *d, libusb_device_handle **h)
{
    stub_opens++;
    if (!d->attached)
        return LIBUSB_ERROR_NO_DEVICE;
    if (d->access_denied)
        return LIBUSB_ERROR_ACCESS;
    *h = calloc(1, sizeof(**h));
    (*h)->dev = d;
    stub_live_handles++;
    return 0;
}

void libusb_close(libusb_device_handle *h)
{
    stub_closes++;
    stub_live_handles--;
    free(h);
}

int libusb_control_transfer(libusb_device_handle *h, uint8_t type, uint8_t req, uint16_t value,
                            uint16_t index, unsigned char *data, uint16_t len, unsigned timeout)
{
    (void)type; (void)req; (void)index;
    stub_controls++;
    if (stub_ntimeouts < 8)
        stub_timeouts[stub_ntimeouts++] = timeout;
    if (h->dev->stall)
        return LIBUSB_ERROR_TIMEOUT;
    if ((value & 0xff) == 0) {           /* language table: en-US */
        if (h->dev->delay_ms) {
            struct timespec ts = { h->dev->delay_ms / 1000, (long)(h->dev->delay_ms % 1000) * 1000000L };
            nanosleep(&ts, NULL);
        }
        unsigned char t[4] = { 4, LIBUSB_DT_STRING, 0x09, 0x04 };
        if (h->dev->malformed == 2)
            t[0] = 2;                    /* advertises no language id */
        memcpy(data, t, len < 4 ? len : 4);
        return 4;
    }
    const char *s = h->dev->serial;
    int n = 2;
    for (int i = 0; s && s[i] && n + 1 < len; i++) {
        data[n++] = (unsigned char)s[i];
        data[n++] = 0;
    }
    if (h->dev->malformed == 1 && n + 1 <= len) {
        data[n] = 'Z';                   /* a dangling byte: odd length */
        n++;
    }
    data[0] = (unsigned char)n;
    data[1] = LIBUSB_DT_STRING;
    return n;
}
