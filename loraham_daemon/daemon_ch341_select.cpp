#include "daemon_ch341_select.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

/* ---- state -------------------------------------------------------------------- */

typedef enum {
    PH_IDLE = 0,
    PH_SCAN,         /* one scan step per call */
    PH_OPEN,         /* one open (serial / plain) */
    PH_WALK,         /* Rule B: one attempt per call */
    PH_DONE
} Phase;

typedef enum { MODE_ANY = 0, MODE_SERIAL, MODE_PORT } Mode;

static DaemonCh341SelectOps g_ops;
static char g_selector[9];          /* --usb-serial, first 8 characters; "" = none */

static Mode g_mode = MODE_ANY;      /* the remembered identity */
static char g_serial[9];
static char g_port[32];
static char g_bound[48] = "-";     /* the last verified open's identity */

static bool g_at_start;
static Phase g_phase = PH_IDLE;
static int g_count, g_index;
/* Sized from each scan's count: the decision always sees every attached stick. */
static std::vector<loraham_ch341_scan_item> g_items;

/* One 2 s deadline per unresolved candidate connection (port + device
 * connection cookie: a replug on the same port is a new one), stamped by the scan step
 * that first sees it. A carried deadline counts only once the current scan
 * has seen that connection unresolved again; one it did not see is erased at
 * the scan's end, one that resolved at once. */
struct AccessDeadline {
    uint64_t at;
    bool seen;                      /* re-observed unresolved in the current scan */
};
static std::map<std::string, AccessDeadline> g_access;
static bool g_access_hold;          /* this decision keeps waiting on one */
static uint64_t g_retry_at;
static uint64_t g_walk_end_ms;      /* the last walk's release, for the gap */
static uint64_t g_busy_logged_ms;
static uint64_t g_list_logged_ms;   /* the transient-read-failure line, once a minute on a reopen */
static uint64_t g_burst_first, g_burst_last;  /* the ARRIVED burst a Rule B walk settles on */
static uint64_t g_cap_first;        /* the first change (ARRIVED or LEFT) since the last answer: the 2 s cap */
/* Topology changes noted (every ARRIVED and every LEFT of a stick other than
 * the bound one); the count when the snapshot began. Only ARRIVEDs make the
 * settle burst. */
static unsigned g_changes, g_scan_changes;
/* The 2 s cap: once it has passed, ONE more snapshot is taken and it is
 * final: ARRIVEDs during it no longer force another (a flapping stick cannot
 * postpone the walk past the cap). */
static bool g_final_pending, g_snapshot_final;

static int g_attempts, g_snap;
static std::vector<loraham_ch341 *> g_held;
static bool g_ruleb;                /* this selection opens by Rule B */
static char g_walk_port[32];        /* the scanned port of the stick a Rule B walk opens */
static char g_sole[32];             /* without a selector: the sole stick's serial, or its port (no serial) */

void daemon_ch341_select_init(const DaemonCh341SelectOps *ops, const char *usb_serial)
{
    g_ops = *ops;
    snprintf(g_selector, sizeof(g_selector), "%s", usb_serial ? usb_serial : "");
    if (g_selector[0]) {
        g_mode = MODE_SERIAL;
        snprintf(g_serial, sizeof(g_serial), "%s", g_selector);
    }
}

void daemon_ch341_select_reset(void)
{
    memset(&g_ops, 0, sizeof(g_ops));
    g_selector[0] = g_serial[0] = g_port[0] = g_sole[0] = '\0';
    snprintf(g_bound, sizeof(g_bound), "-");
    g_mode = MODE_ANY;
    g_phase = PH_IDLE;
    g_retry_at = g_walk_end_ms = g_busy_logged_ms = g_list_logged_ms = 0;
    g_access.clear();
    g_changes = g_scan_changes = 0;
    g_final_pending = g_snapshot_final = false;
    g_burst_first = g_burst_last = g_cap_first = 0;
    g_access_hold = false;
    g_items.clear();
    g_held.clear();
}

uint64_t daemon_ch341_select_retry_at(void) { return g_retry_at; }

const char *daemon_ch341_select_wait_port(void)
{
    return (g_mode == MODE_PORT && g_port[0]) ? g_port : "-";
}

const char *daemon_ch341_select_bound(void)
{
    return g_bound;
}

void daemon_ch341_bound_text(const loraham_ch341_scan_item *id, char *out, size_t n)
{
    char serial[9];
    const char *value = id->port;
    size_t k;

    if (id->serial_state == 1) {
        snprintf(serial, sizeof(serial), "%.8s", id->serial);
        value = serial;
    }
    if (id->serial_state == -1) {           /* unreadable: no identity to report */
        snprintf(out, n, "-");
        return;
    }
    k = (size_t)snprintf(out, n, "%s:", id->serial_state == 1 ? "serial" : "port");
    for (const unsigned char *p = (const unsigned char *)value; *p && k + 4 < n; p++) {
        if (*p >= 0x21 && *p <= 0x7e && *p != '%')
            out[k++] = (char)*p;
        else
            k += (size_t)snprintf(out + k, n - k, "%%%02X", *p);
    }
    out[k] = '\0';
}

void daemon_ch341_select_begin(bool at_start)
{
    g_at_start = at_start;
    g_phase = PH_IDLE;
    g_retry_at = 0;
}

static void release_held(void);

void daemon_ch341_select_cancel(void)
{
    release_held();
    if (g_phase == PH_SCAN || g_phase == PH_OPEN || g_phase == PH_WALK)
        g_ops.scan_end();
    g_phase = PH_IDLE;
    g_access.clear();
}

void daemon_ch341_select_note_arrival(void)
{
    uint64_t now = g_ops.now_ms();

    if (g_burst_first == 0)
        g_burst_first = now;
    if (g_cap_first == 0)
        g_cap_first = now;
    g_burst_last = now;
    g_changes++;
}

void daemon_ch341_select_note_left(void)
{
    /* Every change starts the cap clock (a stick leaving again and again
     * must not keep the selection from concluding); only ARRIVEDs settle. */
    if (g_cap_first == 0)
        g_cap_first = g_ops.now_ms();
    g_changes++;
}

/* ---- helpers ----------------------------------------------------------------- */

static void release_held(void)
{
    for (loraham_ch341 *dev : g_held)
        g_ops.close(dev);
    g_held.clear();
}

static bool serial_eq(const char *a, const char *b)
{
    return strncmp(a, b, 8) == 0;
}

static DaemonCh341SelectResult finish_wait(uint64_t retry_at)
{
    g_ops.scan_end();
    g_phase = PH_IDLE;
    g_retry_at = retry_at;
    return CH341_SEL_WAIT;
}

static bool snapshot_stale(uint64_t now);

/* A refusal: final at start, a wait on reopen (the next change retries).
 * A change since the snapshot began (an ARRIVED or a LEFT, already taken from
 * the monitor, so no new selection would start for it) makes it stale, at
 * start as on a reopen: classify again now, until the settle cap makes the
 * snapshot final. */
static DaemonCh341SelectResult refuse(char *why, size_t n, const char *text)
{
    uint64_t now = g_ops.now_ms();
    if (snapshot_stale(now))
        return finish_wait(now);
    /* Past the cap with changes since the snapshot: they are taken already,
     * so on a reopen no further event may come (a cable that settled inside
     * this selection): classify once more after the quiet time. */
    bool arrived = g_changes != g_scan_changes;
    g_burst_first = g_burst_last = g_cap_first = 0;   /* this burst is answered */
    snprintf(why, n, "%s", text);
    if (g_at_start) {
        g_ops.scan_end();
        g_phase = PH_IDLE;
        return CH341_SEL_REFUSED;
    }
    printf("[CH341] %s; waiting\n", text);
    fflush(stdout);
    return finish_wait(arrived ? now + DAEMON_CH341_SETTLE_QUIET_MS : 0);
}

/* A transient read failure (the device list for the scan or the bind, the
 * opened stick's identity): at start a refusal; on a reopen a present stick
 * must not wait for an event it never sends: retry after 200 ms (never per
 * tick), the line once a minute. */
static DaemonCh341SelectResult transient(char *why, size_t n, const char *text)
{
    if (g_at_start)
        return refuse(why, n, text);
    uint64_t now = g_ops.now_ms();
    snprintf(why, n, "%s", text);
    if (g_list_logged_ms == 0 || now - g_list_logged_ms >= 60000u) {
        printf("[CH341] %s; retrying\n", text);
        fflush(stdout);
        g_list_logged_ms = now;
    }
    return finish_wait(now + DAEMON_CH341_BUSY_RETRY_MS);
}

static DaemonCh341SelectResult busy(char *why, size_t n, const char *who, uint64_t retry_ms)
{
    char text[200];
    uint64_t now = g_ops.now_ms();

    snprintf(text, sizeof(text), "stick %s present but pine64 could not select or open it "
             "(another process, a bound kernel driver, or an unreadable serial)", who);
    if (g_at_start) {
        snprintf(why, n, "BUSY: %s", text);
        g_ops.scan_end();
        g_phase = PH_IDLE;
        return CH341_SEL_REFUSED;
    }
    if (g_busy_logged_ms == 0 || now - g_busy_logged_ms >= 60000u) {
        printf("[CH341] %s; waiting\n", text);
        fflush(stdout);
        g_busy_logged_ms = now;
    }
    return finish_wait(now + retry_ms);
}

static DaemonCh341SelectResult access_expired(char *why, size_t n, const char *who)
{
    char text[160];

    /* A change since the snapshot: classify again first, and keep every
     * candidate's deadline (each connection gets its 2 s once, not anew). */
    if (snapshot_stale(g_ops.now_ms())) {
        g_access_hold = true;
        return finish_wait(g_ops.now_ms());
    }
    snprintf(text, sizeof(text), "no permission to open stick %s, or it cannot be told apart "
             "(udev rule?)", who);
    g_access.clear();                   /* the expiry stands */
    return refuse(why, n, text);
}

/* ACCESS or an inconclusive scan: retry until 2 s after the first observation
 * (stamped by the scan step that saw it). */
static uint64_t access_first_deadline(bool seen_only)
{
    uint64_t first = 0;
    for (const auto &kv : g_access)
        if ((!seen_only || kv.second.seen) && (first == 0 || kv.second.at < first))
            first = kv.second.at;
    return first;
}

static std::string access_key(const loraham_ch341_scan_item *it)
{
    return std::string(it->port) + "@" + std::to_string(it->connection);
}

static DaemonCh341SelectResult access_wait(char *why, size_t n, const char *who)
{
    uint64_t now = g_ops.now_ms();
    /* A change after the last scan step: classify again now (the deadlines
     * are kept by snapshot_stale), not a wait on the stale snapshot. */
    if (snapshot_stale(now))
        return finish_wait(now);
    uint64_t first = access_first_deadline(false);

    if (first == 0)                     /* every unknown is stamped by its scan step */
        first = now + DAEMON_CH341_ACCESS_MS;
    if (now >= first)
        return access_expired(why, n, who);
    g_access_hold = true;
    return finish_wait(std::min<uint64_t>(now + DAEMON_CH341_BUSY_RETRY_MS, first));
}

/* A change since the snapshot began: an ARRIVED may be a duplicate serial, a
 * second stick or the stick a walk needs; a LEFT may make a duplicate unique
 * or several sticks one: classify again (refusals, the open, the walk and
 * every walk step). After the settle cap one final snapshot is used
 * regardless (one hub burst = one selection), so a stick that keeps arriving
 * cannot keep the selection from concluding. */
static bool snapshot_stale(uint64_t now)
{
    if (g_changes == g_scan_changes || g_snapshot_final)
        return false;
    if (g_cap_first && now >= g_cap_first + DAEMON_CH341_SETTLE_CAP_MS)
        g_final_pending = true;         /* the next snapshot is the final one */
    /* Classifying again keeps every ACCESS deadline: a deadline ends only
     * when its candidate resolves or leaves, never because the snapshot
     * went stale (one 2 s per connection). */
    g_access_hold = true;
    return true;
}

/* The port of the unresolved candidate whose 2 s run out first (seen_only:
 * among those this scan saw unresolved), or NULL. */
static const char *access_first_port(bool seen_only)
{
    static char port[32];
    uint64_t first = access_first_deadline(seen_only);
    for (const auto &kv : g_access) {
        if (first && kv.second.at == first && (!seen_only || kv.second.seen)) {
            snprintf(port, sizeof(port), "%s", kv.first.substr(0, kv.first.find('@')).c_str());
            return port;
        }
    }
    return NULL;
}

/* The stick a line names: the serial or port asked for; without one, the
 * sole stick's serial, or its port when it has none (plan §3.2 F6); with
 * several sticks, the unresolved candidate that governs the 2 s rule. */
static const char *who_text(void)
{
    if (g_mode == MODE_SERIAL)
        return g_serial;
    if (g_mode == MODE_PORT)
        return g_port;
    if (g_sole[0])
        return g_sole;
    const char *p = access_first_port(false);
    return p ? p : "1A86:5512";
}

/* The stick an expiry found during the scan names: the asked-for one, else
 * exactly the seen candidate whose deadline triggered it. */
static const char *who_scan_expiry(void)
{
    if (g_mode != MODE_ANY)
        return who_text();
    const char *p = access_first_port(true);
    return p ? p : "1A86:5512";
}

/* A scan item that can keep the decision open: not openable on the monitor
 * context (ACCESS, another open error) in every mode; an unreadable serial
 * only when a serial is looked for (it could be the target; without a
 * selector it is not "no serial", DELTA-4-v3). */
static bool scan_item_unknown(const loraham_ch341_scan_item *it)
{
    if (it->openable == 0 || it->openable == -2)
        return true;
    return g_mode == MODE_SERIAL && it->openable == 1 && it->serial_state == -1;
}

/* Post-open checks (a) .. (e): the stick handed out is the one asked for. */
static bool post_open_ok(loraham_ch341 *dev, loraham_ch341_scan_item *out_id, bool *read_failed,
                         char *why, size_t n)
{
    loraham_ch341_scan_item id;

    *read_failed = false;
    if (g_ops.identity(dev, &id) != 0) {
        snprintf(why, n, "the opened stick's identity could not be read");
        *read_failed = true;
        return false;
    }
    /* Only iSerialNumber 0 makes "the stick without a serial": a serial that
     * is there but cannot be read is neither identity. */
    if (id.serial_state == -1) {
        snprintf(why, n, "stick on port %s has a serial that could not be read (unreadable serial)",
                 id.port);
        return false;
    }
    if (g_mode == MODE_SERIAL && !(id.serial_state == 1 && serial_eq(id.serial, g_serial))) {
        snprintf(why, n, "the opened stick is not serial %s", g_serial);
        return false;
    }
    if (g_ruleb && id.serial_state != 0) {
        snprintf(why, n, "the opened stick has a serial");
        return false;
    }
    if (g_mode == MODE_PORT && strcmp(id.port, g_port) != 0) {
        snprintf(why, n, "stick on port %s, waiting for port %s: replug into the same port, or "
                 "restart", id.port, g_port);
        return false;
    }
    /* The stick without a serial is identified by its port; a stick WITH a
     * serial on that port is another stick. */
    if (g_mode == MODE_PORT && id.serial_state == 1) {
        snprintf(why, n, "stick on port %s has serial %.8s, waiting for the stick without a "
                 "serial: replug it, or restart", id.port, id.serial);
        return false;
    }
    *out_id = id;
    return true;
}

/* After the checks and the bind: fix the identity (BOUND text, and the mode
 * the first open sets for every reopen). */
static void commit_open(const loraham_ch341_scan_item &id)
{
    daemon_ch341_bound_text(&id, g_bound, sizeof(g_bound));
    /* The first open fixes the identity for every reopen. */
    if (g_mode == MODE_ANY) {
        if (id.serial_state == 1) {
            g_mode = MODE_SERIAL;
            snprintf(g_serial, sizeof(g_serial), "%s", id.serial);
        } else {
            g_mode = MODE_PORT;
            snprintf(g_port, sizeof(g_port), "%s", id.port);
        }
    }
}

/* After a blocking open (also a failed one) or identity read: pine64's init
 * takes up to seconds, and an ARRIVED or LEFT in that window may change the
 * answer. Take those events before any result (opened, BUSY, hold, the cap)
 * is committed; true = classify again (inside the cap). */
static bool window_stale(void)
{
    if (g_ops.take_events)
        g_ops.take_events();
    return snapshot_stale(g_ops.now_ms());
}

static DaemonCh341SelectResult opened(loraham_ch341 *dev, loraham_ch341 **out, char *why, size_t n)
{
    char reason[200];
    loraham_ch341_scan_item id;

    bool read_failed;
    bool ok = post_open_ok(dev, &id, &read_failed, reason, sizeof(reason));
    /* Past the cap a Rule B walk keeps its final snapshot (its target is
     * checked to have no serial); every other open refuses rather than open
     * on a stale one. */
    if (window_stale()) {
        g_ops.close(dev);
        return finish_wait(g_ops.now_ms());
    }
    if (!g_ruleb && g_changes != g_scan_changes) {
        g_ops.close(dev);
        return refuse(why, n, "Meshsticks kept arriving during the selection");
    }
    if (!ok) {
        g_ops.close(dev);
        return read_failed ? transient(why, n, reason) : refuse(why, n, reason);
    }
    if (g_ops.bind) {
        if (g_ops.bind(dev) != 0) {                 /* its own list read failed, or no unique match */
            if (g_ops.unbind)
                g_ops.unbind();
            g_ops.close(dev);
            /* The failed bind blocked too: a change in its window decides
             * first (classify again), only then the transient failure. */
            if (window_stale())
                return finish_wait(g_ops.now_ms());
            return transient(why, n, "the opened stick could not be matched in the monitor's device list");
        }
        /* The bind reads a fresh device list (it blocks too): the events of
         * that window count as well, and so does our own stick leaving in
         * it. Nothing is committed on a stale or lost result. */
        bool stale = window_stale();
        bool gone = g_ops.bound_left && g_ops.bound_left();
        if (stale || gone || (!g_ruleb && g_changes != g_scan_changes)) {
            if (g_ops.unbind)
                g_ops.unbind();
            g_ops.close(dev);
            if (stale || gone)
                return finish_wait(g_ops.now_ms());
            return refuse(why, n, "Meshsticks kept arriving during the selection");
        }
    }
    commit_open(id);
    g_ops.scan_end();
    g_phase = PH_DONE;
    g_access.clear();
    g_busy_logged_ms = 0;
    g_burst_first = g_burst_last = g_cap_first = 0;
    *out = dev;
    return CH341_SEL_OPENED;
}

/* ---- the decision after a scan ----------------------------------------------- */

static DaemonCh341SelectResult decide_inner(char *why, size_t n)
{
    /* unknown: could be anything (not openable on the monitor context, or a
     * serial that cannot be read: possibly the --usb-serial target);
     * unopened: of those, the ones that could even be a stick without a
     * serial (a readable index says it has one). */
    int match = 0, unknown = 0, unopened = 0, no_serial = 0, access = 0, present = 0;

    g_sole[0] = '\0';
    for (int i = 0; i < g_count; i++) {
        const loraham_ch341_scan_item *it = &g_items[i];
        if (it->openable == -1)
            continue;                                   /* gone during the scan */
        present++;
        /* The name of the sole stick, if it stays the only one. */
        snprintf(g_sole, sizeof(g_sole), "%s", it->serial_state == 1 ? it->serial : it->port);
        if (it->openable == 0)
            access++;
        if (it->openable != 1 || it->serial_state == -1) {
            unknown++;
            if (it->openable != 1)
                unopened++;
            continue;
        }
        if (it->serial_state == 0) {
            no_serial++;
            snprintf(g_walk_port, sizeof(g_walk_port), "%s", it->port);
        }
        else if (g_mode == MODE_SERIAL && serial_eq(it->serial, g_serial))
            match++;
    }

    if (present != 1)
        g_sole[0] = '\0';
    const char *who = who_text();

    g_ruleb = false;
    if (g_mode == MODE_SERIAL) {
        if (match > 1) {
            char t[96];
            snprintf(t, sizeof(t), "serial %s is not unique", g_serial);
            return refuse(why, n, t);
        }
        if (match == 1 && unknown == 0) {
            g_phase = PH_OPEN;
            return CH341_SEL_PENDING;
        }
        if (unknown > 0)
            return access_wait(why, n, who);
        char t[96];
        snprintf(t, sizeof(t), "no Meshstick with serial %s", g_serial);
        return refuse(why, n, t);
    }

    if (present == 0)
        return refuse(why, n, "no Meshstick attached");
    if (present == 1) {
        if (unopened > 0)                               /* ACCESS or another open error: the 2 s rule */
            return access_wait(why, n, who);
        g_phase = PH_OPEN;                              /* plain open of the only stick */
        return CH341_SEL_PENDING;
    }
    if (no_serial > 1)
        return refuse(why, n, "more than one Meshstick without a serial: only one is supported "
                              "(no open by port)");
    if (unopened > 0)
        return access_wait(why, n, who);
    if (no_serial == 0)
        return refuse(why, n, "several Meshsticks: give --usb-serial");

    /* Exactly one without a serial: Rule B. On a reopen the walk settles on
     * the ARRIVED burst first (500 ms without a further ARRIVED, at most 2 s
     * after its first one: one hub burst = one walk), and keeps the gap since
     * the last walk. */
    uint64_t now = g_ops.now_ms();
    uint64_t due = 0;
    if (g_walk_end_ms)
        due = g_walk_end_ms + DAEMON_CH341_WALK_GAP_MS;
    if (!g_at_start && g_burst_first)
        due = std::max(due, std::min(g_burst_last + DAEMON_CH341_SETTLE_QUIET_MS,
                                     g_burst_first + DAEMON_CH341_SETTLE_CAP_MS));
    if (now < due)
        return finish_wait(due);
    /* A change during this scan makes its snapshot stale: scan again
     * (now, the settle having passed) before walking. */
    if (snapshot_stale(now))
        return finish_wait(now);
    g_burst_first = g_burst_last = g_cap_first = 0;
    g_ruleb = true;
    g_snap = present;
    g_attempts = 0;
    g_held.clear();
    g_phase = PH_WALK;
    return CH341_SEL_PENDING;
}

/* An ACCESS deadline lives only while the decision keeps waiting on it; any
 * other outcome resolves it, so a later ARRIVED gets a fresh 2 s. */
static DaemonCh341SelectResult decide(char *why, size_t n)
{
    g_access_hold = false;
    /* A candidate this scan did not see unresolved has left. */
    for (auto it = g_access.begin(); it != g_access.end();)
        it = it->second.seen ? std::next(it) : g_access.erase(it);
    DaemonCh341SelectResult r = decide_inner(why, n);
    if (!g_access_hold)
        g_access.clear();
    return r;
}

/* ---- one step ------------------------------------------------------------------ */

DaemonCh341SelectResult daemon_ch341_select_step(loraham_ch341 **out, char *why, size_t why_size)
{
    *out = NULL;
    switch (g_phase) {
    case PH_IDLE:
    case PH_DONE:
        g_count = g_ops.scan_begin();
        if (g_count < 0) {
            /* The failed read has seen the changes so far. */
            g_scan_changes = g_changes;
            return transient(why, why_size, "the USB device list could not be read");
        }
        g_items.assign((size_t)g_count, loraham_ch341_scan_item{});
        g_index = 0;
        g_scan_changes = g_changes;
        g_snapshot_final = g_final_pending;
        g_final_pending = false;
        for (auto &kv : g_access)
            kv.second.seen = false;
        g_phase = PH_SCAN;
        return CH341_SEL_PENDING;

    case PH_SCAN:
        if (g_index < g_count) {
            /* The remaining ACCESS deadlines are checked before every step. */
            uint64_t first = access_first_deadline(true);
            if (first && g_ops.now_ms() >= first)
                return access_expired(why, why_size, who_scan_expiry());
            loraham_ch341_scan_item *item = &g_items[(size_t)g_index];
            g_ops.scan_step(g_index, item);
            std::string key = access_key(item);
            if (scan_item_unknown(item)) {
                auto found = g_access.find(key);
                if (found != g_access.end())
                    found->second.seen = true;    /* still unresolved: its time runs on */
                else
                    g_access[key] = { g_ops.now_ms() + DAEMON_CH341_ACCESS_MS, true };
            } else {
                g_access.erase(key);              /* resolved */
            }
            g_index++;
            return CH341_SEL_PENDING;
        }
        return decide(why, why_size);

    case PH_OPEN: {
        /* A change since the snapshot was taken (a duplicate serial or a
         * second stick arrived, one left): classify again before opening. */
        if (snapshot_stale(g_ops.now_ms()))
            return finish_wait(g_ops.now_ms());
        if (g_changes != g_scan_changes)        /* past the cap: never open on a stale snapshot */
            return refuse(why, why_size, "Meshsticks kept arriving during the selection");
        loraham_ch341 *dev = g_ops.open(g_mode == MODE_SERIAL ? g_serial : NULL);
        if (!dev) {
            if (window_stale())
                return finish_wait(g_ops.now_ms());
            return busy(why, why_size, who_text(), DAEMON_CH341_BUSY_RETRY_MS);
        }
        return opened(dev, out, why, why_size);
    }

    case PH_WALK: {
        /* A change first: on the last attempt the cap must not answer for a
         * stale snapshot. */
        if (snapshot_stale(g_ops.now_ms())) {     /* a change during the walk */
            release_held();
            g_walk_end_ms = g_ops.now_ms();
            return finish_wait(g_ops.now_ms());
        }
        if (g_attempts >= g_snap) {                    /* the cap: never more than N_snap */
            release_held();
            g_walk_end_ms = g_ops.now_ms();
            return busy(why, why_size, g_mode == MODE_PORT ? g_port : g_walk_port,
                        DAEMON_CH341_WALK_GAP_MS);
        }
        g_attempts++;
        g_ops.seed_idle();
        loraham_ch341 *dev = g_ops.open(NULL);
        if (!dev) {                                     /* nothing claimable left */
            release_held();
            g_walk_end_ms = g_ops.now_ms();
            if (window_stale())
                return finish_wait(g_ops.now_ms());
            return busy(why, why_size, g_mode == MODE_PORT ? g_port : g_walk_port,
                        DAEMON_CH341_WALK_GAP_MS);
        }
        loraham_ch341_scan_item id;
        int idr = g_ops.identity(dev, &id);
        if (window_stale()) {                           /* a change during that open */
            g_ops.close(dev);
            release_held();
            g_walk_end_ms = g_ops.now_ms();
            return finish_wait(g_ops.now_ms());
        }
        if (idr != 0 || id.serial_state != 0) {
            /* A serial, one that cannot be read, or an identity read that
             * failed: not known to be ours; hold it so pine64 skips it next
             * time (N_snap still bounds the walk). */
            g_held.push_back(dev);
            return CH341_SEL_PENDING;
        }
        release_held();
        g_walk_end_ms = g_ops.now_ms();
        return opened(dev, out, why, why_size);
    }
    }
    return CH341_SEL_PENDING;
}
