/*
 * daemon_ch341_select: which Meshstick, at start and on reopen, on a fake bus.
 * The fake open behaves like pine64 at the pinned commit: it walks the sticks
 * in list order, skips one it cannot open or that is claimed, and with a serial
 * skips those whose first 8 characters differ.
 *
 *   T15  --usb-serial in either order; unknown possible duplicate -> refused;
 *        shared 8-char prefix -> refused; no selector + several with serials
 *        -> refused; post-open mismatch -> refused; a claimed stick
 *   T15b Rule B: free serial stick held, the no-serial one opened, every held
 *        stick released; cap N_snap; seeded before every attempt; the gap after
 *        a walk; two without a serial -> refused; reopen by port, other port
 *        refused; a single serial stick reopens by its serial on any port
 *   T16  ACCESS: retried until 2 s after the first observation, then refused
 *   B4   a BUSY start is refused with a reason beginning "BUSY:"
 */

#include "../daemon_ch341_select.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_ok = 0;
static int g_fail = 0;

static void expect(const std::string &name, bool cond)
{
    if (cond) {
        g_ok++;
        printf("[ OK ] %s\n", name.c_str());
    } else {
        g_fail++;
        printf("[FAIL] %s\n", name.c_str());
    }
}

/* ---- the fake bus ------------------------------------------------------------- */

struct Stick {
    const char *serial;    /* NULL = no serial */
    const char *port;
    int openable;          /* scan: 1, 0 ACCESS, -1 gone */
    bool claimed;          /* by another process or held */
    bool foreign;          /* claimed by another process: never freed by close */
};

static struct {
    Stick s[24];
    int n;
    uint64_t now;
    int seeds, opens, closes, bind_rc;
    bool wrong_identity;   /* identity reports a different serial */
    uint64_t scan_ms;      /* fake time one scan step takes */
    int steps;             /* scan steps run */
    uint64_t last_step_start;
    bool detach_fail[24];  /* scan opens it; pine64's claim fails (driver not detached) */
    bool unreadable[24];   /* a serial index, but the string cannot be read */
    unsigned addr[24];     /* the connection cookie: a replug on the same port gets a new one */
    uint64_t open_ms;      /* fake time one pine64 open takes (a stalling stick) */
    bool flap;             /* a stick ARRIVES during every scan step (a bad cable or hub) */
    int add_at_open;       /* this pine64 open (1 = the first) blocks while add_stick arrives */
    Stick add_stick;
    int unseen;            /* ARRIVEDs the monitor holds, not yet taken */
    int unseen_left;       /* unbound LEFTs the monitor holds, not yet taken */
    uint64_t flap_until;   /* a stick ARRIVES during every scan step before this time */
    int leave_after_step;  /* 1-based: that stick LEAVES right after its own scan step */
    bool add_at_bind;      /* add_stick arrives during the bind (its fresh device list) */
    bool left_at_bind;     /* another stick's LEFT arrives during the bind */
    bool bound_leaves_at_bind;  /* the stick being bound leaves during the bind */
    bool bound_left;       /* the monitor's bound-LEFT flag */
    int bound_idx, binds, unbinds;
    int identity_fails;    /* this many identity reads fail */
    bool lflap;            /* an unbound LEFT arrives during every scan step (a churning hub) */
} B;

static int g_list_fails;   /* scan_begin answers -1 while set */
/* One-shot transient failures for the transient sweep (each counts down). */
static struct { int list_fail, step_unreadable, open_null, bind_fail; } T1S;
static int f_scan_begin(void)
{
    if (T1S.list_fail > 0) {
        T1S.list_fail--;
        return -1;
    }
    return g_list_fails ? -1 : B.n;
}
static void fill_item(int i, loraham_ch341_scan_item *o);
static void f_scan_step(int i, loraham_ch341_scan_item *o)
{
    B.steps++;
    B.last_step_start = B.now;
    B.now += B.scan_ms;
    fill_item(i, o);
    if (T1S.step_unreadable > 0 && o->openable == 1) {
        T1S.step_unreadable--;
        o->serial_state = -1;
    }
    if (B.flap || B.now < B.flap_until)
        daemon_ch341_select_note_arrival();
    if (B.lflap)
        daemon_ch341_select_note_left();
    if (B.leave_after_step && i + 1 == B.leave_after_step) {   /* unplugged after its step */
        B.s[i].openable = -1;
        B.leave_after_step = 0;                     /* once */
        daemon_ch341_select_note_left();
    }
}
static void fill_item(int i, loraham_ch341_scan_item *o)
{
    memset(o, 0, sizeof(*o));
    o->connection = B.addr[i];
    o->openable = B.s[i].openable;
    snprintf(o->port, sizeof(o->port), "%s", B.s[i].port);
    if (o->openable != 1) {
        o->serial_state = -1;
        return;
    }
    if (B.unreadable[i]) {
        o->serial_state = -1;
    } else if (B.s[i].serial) {
        o->serial_state = 1;
        snprintf(o->serial, sizeof(o->serial), "%.8s", B.s[i].serial);
    }
}
static void f_scan_end(void) { }
static loraham_ch341 *f_open(const char *serial)
{
    if (T1S.open_null > 0) {
        T1S.open_null--;
        B.opens++;
        return nullptr;
    }
    B.opens++;
    B.now += B.open_ms;
    if (B.add_at_open && B.opens == B.add_at_open) {   /* arrives during pinedio_init */
        B.s[B.n++] = B.add_stick;
        B.unseen++;
    }
    for (int i = 0; i < B.n; i++) {
        if (B.s[i].openable != 1 || B.s[i].claimed || B.detach_fail[i])
            continue;
        if (serial && !(B.s[i].serial && strncmp(B.s[i].serial, serial, 8) == 0))
            continue;
        B.s[i].claimed = true;
        return reinterpret_cast<loraham_ch341 *>(static_cast<intptr_t>(i + 1));
    }
    return nullptr;
}
static int idx(loraham_ch341 *d) { return (int)reinterpret_cast<intptr_t>(d) - 1; }
static void f_close(loraham_ch341 *d) { B.closes++; B.s[idx(d)].claimed = false; }
static int f_identity(loraham_ch341 *d, loraham_ch341_scan_item *o)
{
    if (B.identity_fails > 0) {
        B.identity_fails--;
        return -1;
    }
    fill_item(idx(d), o);
    if (B.wrong_identity && o->serial_state == 1)
        snprintf(o->serial, sizeof(o->serial), "XXXXXXXX");
    return 0;
}
static void f_seed(void) { B.seeds++; }
static int f_bind(loraham_ch341 *d)
{
    B.binds++;
    B.bound_idx = idx(d);
    B.bound_left = false;
    if (B.add_at_bind) {                             /* arrives while the list is read */
        B.add_at_bind = false;
        B.s[B.n++] = B.add_stick;
        B.unseen++;
    }
    if (B.left_at_bind) {
        B.left_at_bind = false;
        B.unseen_left++;
    }
    if (B.bound_leaves_at_bind) {
        B.bound_leaves_at_bind = false;
        B.s[idx(d)].openable = -1;
        B.bound_left = true;
    }
    if (T1S.bind_fail > 0) {                         /* after the window's events: the list read failed */
        T1S.bind_fail--;
        return -1;
    }
    return B.bind_rc;
}
static void f_unbind(void) { B.unbinds++; B.bound_left = false; }
static bool f_bound_left(void) { return B.bound_left; }
static uint64_t f_now(void) { return B.now; }
static void f_take_events(void)
{
    for (; B.unseen > 0; B.unseen--)
        daemon_ch341_select_note_arrival();
    for (; B.unseen_left > 0; B.unseen_left--)
        daemon_ch341_select_note_left();
}

static void bus(std::initializer_list<Stick> sticks)
{
    memset(&B, 0, sizeof(B));
    B.now = 10000;
    for (const Stick &s : sticks)
        B.s[B.n++] = s;
}

static void init(const char *usb_serial)
{
    DaemonCh341SelectOps ops{};
    ops.scan_begin = f_scan_begin;
    ops.scan_step = f_scan_step;
    ops.scan_end = f_scan_end;
    ops.open = f_open;
    ops.close = f_close;
    ops.identity = f_identity;
    ops.seed_idle = f_seed;
    ops.bind = f_bind;
    ops.now_ms = f_now;
    ops.take_events = f_take_events;
    ops.unbind = f_unbind;
    ops.bound_left = f_bound_left;
    daemon_ch341_select_reset();
    daemon_ch341_select_init(&ops, usb_serial);
}

/* Run one selection to its end; returns the result, the opened stick and why. */
static DaemonCh341SelectResult run(bool at_start, int *opened_idx, std::string *why)
{
    char w[240] = "";
    loraham_ch341 *dev = nullptr;
    DaemonCh341SelectResult r;
    daemon_ch341_select_begin(at_start);
    int guard = 0;
    do {
        r = daemon_ch341_select_step(&dev, w, sizeof(w));
    } while (r == CH341_SEL_PENDING && ++guard < 100);
    *opened_idx = dev ? idx(dev) : -1;
    *why = w;
    return r;
}

/* ---- tests ------------------------------------------------------------------- */

static void test_serial_selector(void)
{
    int o;
    std::string why;

    for (int order = 0; order < 2; order++) {
        if (order == 0)
            bus({{"TESTSN01", "1-2", 1, false, false}, {"ABCDEFGH", "1-3", 1, false, false}});
        else
            bus({{"ABCDEFGH", "1-3", 1, false, false}, {"TESTSN01", "1-2", 1, false, false}});
        init("TESTSN01");
        expect("T15 --usb-serial picks its stick (order " + std::to_string(order) + ")",
               run(true, &o, &why) == CH341_SEL_OPENED && strcmp(B.s[o].serial, "TESTSN01") == 0);
    }

    bus({{"TESTSN01", "1-2", 1, false, false}, {"ZZZ", "1-3", 0, false, false}});
    init("TESTSN01");
    expect("T15 a readable match + an unreadable possible duplicate: inconclusive, waits",
           run(true, &o, &why) == CH341_SEL_WAIT && B.opens == 0);
    B.now += 2001;
    expect("T15 ... and refused after the 2 s ACCESS deadline",
           run(true, &o, &why) == CH341_SEL_REFUSED && why.find("no permission") != std::string::npos);

    bus({{"TESTSN01A", "1-2", 1, false, false}, {"TESTSN01B", "1-3", 1, false, false}});
    init("TESTSN01");
    expect("T15 two sticks sharing the 8-char prefix: refused, not unique",
           run(true, &o, &why) == CH341_SEL_REFUSED && why.find("not unique") != std::string::npos);

    bus({{"ABCDEFGH", "1-3", 1, false, false}});
    init("TESTSN01");
    expect("T15 the serial absent: refused at start",
           run(true, &o, &why) == CH341_SEL_REFUSED && why.find("no Meshstick with serial") != std::string::npos);

    bus({{"TESTSN01", "1-2", 1, false, false}});
    init("TESTSN01");
    B.wrong_identity = true;
    expect("T15 post-open mismatch: closed and refused",
           run(true, &o, &why) == CH341_SEL_REFUSED && B.closes == 1 && !B.s[0].claimed);

    bus({{"TESTSN01", "1-2", 1, true, true}});
    init("TESTSN01");
    expect("B4 a BUSY --usb-serial start: refused with a reason beginning BUSY:",
           run(true, &o, &why) == CH341_SEL_REFUSED && why.rfind("BUSY:", 0) == 0);
    init("TESTSN01");
    expect("BUSY on reopen: a wait with a retry, no exit",
           run(false, &o, &why) == CH341_SEL_WAIT && daemon_ch341_select_retry_at() == B.now + 200);
    B.s[0].claimed = false;
    expect("BUSY: the holder releases, the retry opens",
           run(false, &o, &why) == CH341_SEL_OPENED && o == 0);

    bus({{"TESTSN01", "1-2", 1, false, false}});
    init(nullptr);
    B.bind_rc = -1;
    expect("post-open check (e): no monitor match -> closed and refused",
           run(true, &o, &why) == CH341_SEL_REFUSED && !B.s[0].claimed);
}

static void test_no_selector(void)
{
    int o;
    std::string why;

    bus({{"TESTSN01", "1-2", 1, false, false}, {"ABCDEFGH", "1-3", 1, false, false}});
    init(nullptr);
    expect("T15 several sticks all with a serial, no selector: refused",
           run(true, &o, &why) == CH341_SEL_REFUSED && why.find("give --usb-serial") != std::string::npos);

    bus({{nullptr, "1-2", 1, false, false}, {nullptr, "1-3", 1, false, false}});
    init(nullptr);
    expect("T15b two without a serial: refused with the limit",
           run(true, &o, &why) == CH341_SEL_REFUSED && why.find("only one is supported") != std::string::npos);

    /* A single serial stick: plain open, its serial remembered, reopened on any port. */
    bus({{"TESTSN01", "1-2", 1, false, false}});
    init(nullptr);
    expect("single stick, no selector: plain open", run(true, &o, &why) == CH341_SEL_OPENED && o == 0);
    expect("its wait port is - (it has a serial)", strcmp(daemon_ch341_select_wait_port(), "-") == 0);
    B.s[0].claimed = false;
    B.s[0].port = "1-4";                         /* replugged elsewhere */
    expect("single serial stick reopens by its serial on another port",
           run(false, &o, &why) == CH341_SEL_OPENED && o == 0);
}

static void test_rule_b(void)
{
    int o;
    std::string why;

    /* A free serial stick first, the stick without a serial second. */
    bus({{"TESTSN01", "1-2", 1, false, false}, {nullptr, "1-3", 1, false, false}});
    init(nullptr);
    expect("T15b Rule B opens the stick without a serial",
           run(true, &o, &why) == CH341_SEL_OPENED && o == 1);
    expect("T15b the free serial stick was held, then released", !B.s[0].claimed && B.closes == 1);
    expect("T15b seeded before every attempt", B.seeds == 2);
    expect("its port is remembered", strcmp(daemon_ch341_select_wait_port(), "1-3") == 0);

    /* Reopen: back on its port -> opens; on another port -> refused, waits. */
    B.s[1].claimed = false;
    B.now += 5000;
    expect("T15b reopen on its own port", run(false, &o, &why) == CH341_SEL_OPENED && o == 1);
    B.s[1].claimed = false;
    B.s[1].port = "1-9";
    B.now += 5000;
    expect("T15b reopen on another port: released, waits with the line",
           run(false, &o, &why) == CH341_SEL_WAIT && why.find("same port") != std::string::npos &&
           !B.s[1].claimed);

    /* Two free serial sticks first: both held, both released; the cap. */
    bus({{"AAAAAAAA", "1-2", 1, false, false}, {"BBBBBBBB", "1-3", 1, false, false},
         {nullptr, "1-4", 1, false, false}});
    init(nullptr);
    expect("T15b two serial sticks held on the way", run(true, &o, &why) == CH341_SEL_OPENED && o == 2);
    expect("T15b ... both released", !B.s[0].claimed && !B.s[1].claimed && B.closes == 2);
    expect("T15b at most N_snap attempts", B.opens <= 3);

    /* The stick without a serial held by a foreign process: BUSY wait, gap. */
    bus({{"AAAAAAAA", "1-2", 1, false, false}, {nullptr, "1-4", 1, true, true}});
    init(nullptr);
    expect("B4 a Rule B start whose target is held: refused with BUSY:",
           run(true, &o, &why) == CH341_SEL_REFUSED && why.rfind("BUSY:", 0) == 0 &&
           !B.s[0].claimed);
    init(nullptr);
    expect("Rule B reopen with the target held: waits, every held stick released",
           run(false, &o, &why) == CH341_SEL_WAIT && !B.s[0].claimed);
    uint64_t walk_end = B.now;
    expect("the next walk is due 1 s after the walk's end (the gap)",
           daemon_ch341_select_retry_at() == walk_end + 1000);
    B.now += 300;
    expect("inside the gap: no walk (nothing opened)",
           (B.opens = 0, run(false, &o, &why) == CH341_SEL_WAIT && B.opens == 0));
    B.s[1].claimed = false;                      /* the holder releases, no event */
    B.now = walk_end + 1000;
    expect("F8: after the gap the periodic retry opens it, no hotplug event needed",
           run(false, &o, &why) == CH341_SEL_OPENED && o == 1);
}

static void test_access(void)
{
    int o;
    std::string why;

    bus({{"TESTSN01", "1-2", 0, false, false}});
    init(nullptr);
    expect("T16 ACCESS on the only stick: waits", run(true, &o, &why) == CH341_SEL_WAIT);
    B.now += 1000;
    expect("T16 still inside 2 s from the FIRST observation: waits", run(true, &o, &why) == CH341_SEL_WAIT);
    B.s[0].openable = 1;                         /* udev applied the rule */
    expect("T16 cleared within 2 s: opens", run(true, &o, &why) == CH341_SEL_OPENED);

    bus({{"TESTSN01", "1-2", 0, false, false}});
    init(nullptr);
    run(true, &o, &why);
    B.now += 2000;
    expect("T16 expiry at start: refused, never D3 (no exit 5 path here)",
           run(true, &o, &why) == CH341_SEL_REFUSED && why.find("no permission") != std::string::npos);
}


/* ---- round 2 (gate-1 r1 findings) ---------------------------------------------- */

static bool has(const std::string &s, const char *part) { return s.find(part) != std::string::npos; }

static void test_r2_serial(void)
{
    int o;
    std::string why;

    /* T5: another identity arrives while UNPLUGGED: ignored with the line. */
    bus({{"ABCDEFGH", "1-3", 1, false, false}});
    init("TESTSN01");
    expect("T5 --usb-serial reopen, only another serial attached: waits with the line, opens nothing",
           run(false, &o, &why) == CH341_SEL_WAIT && has(why, "no Meshstick with serial TESTSN01") &&
           B.opens == 0);
    bus({{"TESTSN01", "1-2", 1, false, false}});
    init(nullptr);
    run(true, &o, &why);                         /* single serial stick: identity = its serial */
    B.s[0].claimed = false;
    B.s[0].serial = "ZZZZZZZZ";                  /* another stick on the same port */
    B.opens = 0;
    expect("T5 a different serial on the same port: ignored with the line, nothing opened",
           run(false, &o, &why) == CH341_SEL_WAIT && has(why, "no Meshstick with serial TESTSN01") &&
           B.opens == 0);

    /* T15: an unknown possible duplicate that reads different before the deadline. */
    bus({{"TESTSN01", "1-2", 1, false, false}, {"ABCDEFGH", "1-3", 0, false, false}});
    init("TESTSN01");
    expect("T15 unknown beside the match: waits", run(true, &o, &why) == CH341_SEL_WAIT);
    B.now += 500;
    B.s[1].openable = 1;
    expect("T15 ... the unknown reads different within 2 s: opens the match",
           run(true, &o, &why) == CH341_SEL_OPENED && o == 0);

    /* T15: a non-candidate gone during the scan is skipped. */
    bus({{"ABCDEFGH", "1-3", -1, false, false}, {"TESTSN01", "1-2", 1, false, false}});
    init("TESTSN01");
    expect("T15 a stick gone during the scan is skipped",
           run(true, &o, &why) == CH341_SEL_OPENED && o == 1);

    /* T15: another stick claimed by another daemon; ours free. */
    bus({{"ABCDEFGH", "1-3", 1, true, true}, {"TESTSN01", "1-2", 1, false, false}});
    init("TESTSN01");
    expect("T15 another stick claimed by another daemon: ours opens",
           run(true, &o, &why) == CH341_SEL_OPENED && o == 1 && B.s[0].claimed);

    /* T15: the single stick without a serial, no selector: plain open, no walk. */
    bus({{nullptr, "1-2", 1, false, false}});
    init(nullptr);
    expect("T15 single stick without a serial: plain open, no walk",
           run(true, &o, &why) == CH341_SEL_OPENED && o == 0 && B.seeds == 0);
    expect("T15 ... identity = its port", strcmp(daemon_ch341_select_wait_port(), "1-2") == 0);
    expect("BOUND: port:1-2 for the stick without a serial",
           strcmp(daemon_ch341_select_bound(), "port:1-2") == 0);
    /* Reopen with only serial sticks attached: refused with the line, waits. */
    B.s[0] = {"AAAAAAAA", "1-5", 1, false, false};
    B.s[1] = {"BBBBBBBB", "1-6", 1, false, false};
    B.n = 2;
    expect("T15 reopen, several sticks all with a serial: waits with the line",
           run(false, &o, &why) == CH341_SEL_WAIT && has(why, "give --usb-serial"));

    /* BOUND: serial kind; a serial stick on the stored port of a port daemon is never port:. */
    bus({{"TESTSN01", "1-2", 1, false, false}});
    init("TESTSN01");
    run(true, &o, &why);
    expect("BOUND: serial:TESTSN01 after a --usb-serial open",
           strcmp(daemon_ch341_select_bound(), "serial:TESTSN01") == 0);
    bus({{"A B%C", "1-2", 1, false, false}});
    init(nullptr);
    run(true, &o, &why);
    expect("BOUND: a space and a %% are written %20 / %25",
           strcmp(daemon_ch341_select_bound(), "serial:A%20B%25C") == 0);
    bus({{nullptr, "1-2", 1, false, false}});
    init(nullptr);
    run(true, &o, &why);                         /* port daemon on 1-2 */
    B.s[0] = {"TESTSN01", "1-2", 1, false, false};
    expect("a serial stick swapped into the stored port of the stick without a serial: refused, waits",
           run(false, &o, &why) == CH341_SEL_WAIT && has(why, "has serial TESTSN01") &&
           !B.s[0].claimed);

    /* DELTA-4-v3: a serial index whose string cannot be read is never the
     * stick without a serial and never BOUND=port:. */
    bus({{"TESTSN01", "1-2", 1, false, false}});
    B.unreadable[0] = true;
    init(nullptr);
    expect("unreadable serial, single stick at start: refused, not opened as the stick without a serial",
           run(true, &o, &why) == CH341_SEL_REFUSED && has(why, "unreadable serial") && !B.s[0].claimed);
    expect("unreadable serial: BOUND stays -", strcmp(daemon_ch341_select_bound(), "-") == 0);
    bus({{nullptr, "1-2", 1, false, false}});
    init(nullptr);
    run(true, &o, &why);                         /* port daemon on 1-2 */
    B.s[0] = {"TESTSN01", "1-2", 1, false, false};
    B.unreadable[0] = true;
    expect("unreadable serial swapped into the stored port: refused, waits, released",
           run(false, &o, &why) == CH341_SEL_WAIT && has(why, "unreadable serial") && !B.s[0].claimed);
    expect("... BOUND keeps the last verified open (port:1-2), never port: for the swapped stick",
           strcmp(daemon_ch341_select_bound(), "port:1-2") == 0);

    /* In a Rule B walk an unreadable serial is held like a serial: the walk goes on. */
    bus({{"AAAAAAAA", "1-1", 1, false, false}, {"TESTSN01", "1-2", 1, false, false},
         {nullptr, "1-3", 1, false, false}});
    B.unreadable[1] = true;
    init(nullptr);
    expect("Rule B with an unreadable serial among serial sticks: opens the stick without a serial",
           run(true, &o, &why) == CH341_SEL_OPENED && o == 2 && !B.s[1].claimed &&
           strcmp(daemon_ch341_select_bound(), "port:1-3") == 0);

    /* A --usb-serial daemon retries every 200 ms; no walk gap applies to it. */
    bus({{"TESTSN01", "1-2", 1, true, true}});
    init("TESTSN01");
    expect("a --usb-serial daemon's BUSY retry is due in 200 ms (no gap)",
           run(false, &o, &why) == CH341_SEL_WAIT && daemon_ch341_select_retry_at() == B.now + 200);

    /* T15b: a walk holds the serial daemon's stick for 30 s: it waits, never exits,
     * and opens on the first retry after the release. */
    bool refused = false;
    for (int i = 0; i < 150; i++) {              /* 30 s of 200 ms retries */
        B.now += 200;
        if (run(false, &o, &why) != CH341_SEL_WAIT)
            refused = true;
    }
    expect("T15b held 30 s by a walk: waits the whole time, no refusal", !refused);
    B.s[0].claimed = false;
    B.now += 200;
    expect("T15b ... opens on the first retry after the release", run(false, &o, &why) == CH341_SEL_OPENED);

    /* T15b: a permanent foreign holder: 10 minutes, never an exit. */
    bus({{"TESTSN01", "1-2", 1, true, true}});
    init("TESTSN01");
    refused = false;
    for (int i = 0; i < 3000; i++) {
        B.now += 200;
        if (run(false, &o, &why) != CH341_SEL_WAIT)
            refused = true;
    }
    expect("T15b a permanent holder: still waiting after 10 minutes, never refused", !refused);

    /* T15b: the kernel driver cannot be detached: the same waiting line; opens once detached. */
    bus({{"TESTSN01", "1-2", 1, false, false}});
    B.detach_fail[0] = true;
    init("TESTSN01");
    expect("T15b detach failure on reopen: the BUSY waiting line, no exit",
           run(false, &o, &why) == CH341_SEL_WAIT);
    B.detach_fail[0] = false;
    B.now += 200;
    expect("T15b ... opens once the driver is detached", run(false, &o, &why) == CH341_SEL_OPENED);

    /* T15b: a --usb-serial open blocked by ACCESS follows the ACCESS rule. */
    bus({{"TESTSN01", "1-2", 0, false, false}});
    init("TESTSN01");
    expect("T15b --usb-serial with ACCESS: waits (ACCESS rule)", run(true, &o, &why) == CH341_SEL_WAIT);
    B.now += 300;
    B.s[0].openable = 1;
    expect("T15b ... opens once udev applied the rule", run(true, &o, &why) == CH341_SEL_OPENED);
}

/* A port daemon (the stick without a serial on 1-3) after its first walk, now UNPLUGGED. */
static void port_daemon_unplugged(void)
{
    int o;
    std::string why;

    bus({{"AAAAAAAA", "1-2", 1, false, false}, {nullptr, "1-3", 1, false, false}});
    init(nullptr);
    run(true, &o, &why);
    B.s[1].claimed = false;
    B.now += 5000;                               /* past the gap */
    B.opens = B.seeds = 0;
}

static void test_r2_ruleb(void)
{
    int o;
    std::string why;

    /* F1/T15b: a hub burst of three ARRIVEDs, staggered: one walk after 500 ms quiet. */
    port_daemon_unplugged();
    uint64_t t0 = B.now;
    daemon_ch341_select_note_arrival();
    expect("F1 burst: first ARRIVED -> settle, no walk", run(false, &o, &why) == CH341_SEL_WAIT &&
           B.opens == 0 && daemon_ch341_select_retry_at() == t0 + 500);
    B.now = t0 + 300;
    daemon_ch341_select_note_arrival();
    expect("F1 burst: second ARRIVED moves the quiet deadline", run(false, &o, &why) == CH341_SEL_WAIT &&
           B.opens == 0 && daemon_ch341_select_retry_at() == t0 + 800);
    B.now = t0 + 600;
    daemon_ch341_select_note_arrival();
    run(false, &o, &why);
    B.now = t0 + 1100;
    expect("F1 burst: after 500 ms quiet exactly one walk opens the stick",
           run(false, &o, &why) == CH341_SEL_OPENED && o == 1 && B.seeds == 2);

    /* F1/T15b: a flapping stick (ARRIVED every 300 ms) cannot defer the walk past 2 s. */
    port_daemon_unplugged();
    t0 = B.now;
    bool early = false;
    for (uint64_t dt = 0; dt < 2000; dt += 300) {
        B.now = t0 + dt;
        daemon_ch341_select_note_arrival();
        if (run(false, &o, &why) != CH341_SEL_WAIT || B.opens != 0)
            early = true;
    }
    expect("F1 flapping: no walk before the 2 s cap", !early && daemon_ch341_select_retry_at() == t0 + 2000);
    B.now = t0 + 2000;
    expect("F1 flapping: the walk runs at the cap", run(false, &o, &why) == CH341_SEL_OPENED && o == 1);

    /* At start there is no burst: no settle. */
    bus({{"AAAAAAAA", "1-2", 1, false, false}, {nullptr, "1-3", 1, false, false}});
    init(nullptr);
    expect("no settle at start", run(true, &o, &why) == CH341_SEL_OPENED && o == 1);

    /* T15b (B3): a new serial stick arrives during a walk: never more than N_snap attempts. */
    bus({{"AAAAAAAA", "1-2", 1, false, false}, {"BBBBBBBB", "1-3", 1, false, false},
         {nullptr, "1-4", 1, true, true}});
    init(nullptr);
    {
        char w[240] = "";
        loraham_ch341 *dev = nullptr;
        DaemonCh341SelectResult r;
        daemon_ch341_select_begin(true);
        int guard = 0;
        do {
            r = daemon_ch341_select_step(&dev, w, sizeof(w));
            if (B.opens == 1 && B.n == 3)
                B.s[B.n++] = {"CCCCCCCC", "1-5", 1, false, false};   /* arrives mid-walk */
        } while (r == CH341_SEL_PENDING && ++guard < 100);
        expect("T15b a stick arriving mid-walk: at most N_snap (3) attempts", B.opens == 3);
        expect("T15b ... every held stick released, BUSY at start",
               r == CH341_SEL_REFUSED && strncmp(w, "BUSY:", 5) == 0 &&
               !B.s[0].claimed && !B.s[1].claimed && !B.s[3].claimed);
    }

    /* T15b: the stick without a serial vanishes after the scan: all released. */
    bus({{"AAAAAAAA", "1-2", 1, false, false}, {nullptr, "1-3", 1, false, false}});
    init(nullptr);
    {
        char w[240] = "";
        loraham_ch341 *dev = nullptr;
        DaemonCh341SelectResult r;
        daemon_ch341_select_begin(true);
        int guard = 0;
        do {
            r = daemon_ch341_select_step(&dev, w, sizeof(w));
            if (B.steps == 2)
                B.s[1].openable = -1;                /* gone before the walk */
        } while (r == CH341_SEL_PENDING && ++guard < 100);
        expect("T15b no target left in the walk: every held stick released, refused at start",
               r == CH341_SEL_REFUSED && !B.s[0].claimed && B.closes == 1);
    }

    /* F3: more than 16 sticks: the decision sees all of them. */
    bus({});
    for (int i = 0; i < 18; i++) {
        static char names[18][16];
        snprintf(names[i], sizeof(names[i]), "S%07d", i);
        B.s[B.n++] = {names[i], "1-1", 1, false, false};
    }
    B.s[B.n++] = {"TARGET01", "1-9", 1, false, false};
    init("TARGET01");
    expect("F3 --usb-serial target at position 19: found and opened",
           run(true, &o, &why) == CH341_SEL_OPENED && o == 18);
    B.s[18].claimed = false;
    B.s[B.n++] = {"TARGET01X", "1-8", 1, false, false};
    init("TARGET01");
    expect("F3 a duplicate at position 20: not unique",
           run(true, &o, &why) == CH341_SEL_REFUSED && has(why, "not unique"));
    B.s[19] = {nullptr, "1-8", 1, false, false};
    init(nullptr);
    expect("F3 the stick without a serial at position 20: Rule B finds it",
           run(true, &o, &why) == CH341_SEL_OPENED && o == 19);
}

static void test_r2_access(void)
{
    int o;
    std::string why;

    /* F2/T13: ACCESS early in a long scan: the deadline counts from that step,
     * and no step starts after it. */
    bus({{"TESTSN01", "1-2", 0, false, false}});
    for (int i = 1; i < 16; i++) {
        static char names[16][16];
        snprintf(names[i], sizeof(names[i]), "S%07d", i);
        B.s[B.n++] = {names[i], "1-1", 1, false, false};
    }
    B.scan_ms = 200;
    init("S0000015");
    uint64_t first = B.now + 200;                 /* the end of the step that saw ACCESS */
    expect("F2 expiry inside the scan: refused", run(true, &o, &why) == CH341_SEL_REFUSED &&
           has(why, "no permission"));
    expect("F2 ... measured from the step that saw ACCESS: no step started at or after it",
           B.last_step_start < first + 2000 && B.steps == 11);

    /* F2: a resolved ACCESS does not leave residual time for a later one. */
    bus({{"TESTSN01", "1-2", 0, false, false}});
    init(nullptr);
    expect("F2 ACCESS: waits", run(true, &o, &why) == CH341_SEL_WAIT);
    B.now += 1500;
    B.s[0].openable = 1;
    expect("F2 ... resolved: opens", run(true, &o, &why) == CH341_SEL_OPENED);
    B.s[0].claimed = false;
    B.s[0].openable = 0;
    B.now += 1000;                                /* the old deadline has passed */
    expect("F2 a later ACCESS on reopen gets a fresh 2 s (waits, no line)",
           run(false, &o, &why) == CH341_SEL_WAIT && why.empty());
    B.now += 1900;
    expect("F2 ... still inside its own 2 s", run(false, &o, &why) == CH341_SEL_WAIT && why.empty());
    B.now += 100;
    expect("T13 reopen ACCESS expiry: stays waiting with the line (never exit)",
           run(false, &o, &why) == CH341_SEL_WAIT && has(why, "no permission"));
    B.s[0].openable = 1;
    daemon_ch341_select_note_arrival();
    expect("T13 ... a new ARRIVED starts over and succeeds", run(false, &o, &why) == CH341_SEL_OPENED);
}

static void test_r4(void)
{
    int o;
    std::string why;

    /* F1 (r2): an ARRIVED during the scan makes its snapshot stale: a fresh
     * scan before the walk, even when the settle has already passed. */
    port_daemon_unplugged();
    uint64_t t0 = B.now;
    daemon_ch341_select_note_arrival();          /* the burst's first ARRIVED */
    B.now = t0 + 1900;
    B.scan_ms = 100;
    {
        char w[240] = "";
        loraham_ch341 *dev = nullptr;
        daemon_ch341_select_begin(false);
        daemon_ch341_select_step(&dev, w, sizeof(w));   /* scan begins at t0+1900 */
        daemon_ch341_select_step(&dev, w, sizeof(w));   /* step 0 -> t0+2000 */
        daemon_ch341_select_note_arrival();              /* ARRIVED during the scan */
        daemon_ch341_select_step(&dev, w, sizeof(w));   /* step 1 -> t0+2100 */
        DaemonCh341SelectResult r = daemon_ch341_select_step(&dev, w, sizeof(w));   /* decide */
        expect("F1 an ARRIVED during the scan: no walk on the stale snapshot (the cap has passed)",
               r == CH341_SEL_WAIT && B.opens == 0);
    }
    B.scan_ms = 0;
    expect("F1 ... the fresh scan then walks", run(false, &o, &why) == CH341_SEL_OPENED && o == 1);

    /* F2 (r2): one deadline per candidate: a candidate that resolves takes its
     * deadline with it; a new unresolved one gets its own 2 s. */
    bus({{"TESTSN01", "1-2", 1, false, false}, {"AAAAAAAA", "1-3", 0, false, false}});
    init("TESTSN01");
    expect("F2 match + ACCESS on 1-3: waits", run(true, &o, &why) == CH341_SEL_WAIT);
    B.now += 1500;
    B.s[1].openable = 1;                          /* 1-3 resolves ... */
    B.s[B.n++] = {"BBBBBBBB", "1-4", 0, false, false};   /* ... and 1-4 shows ACCESS */
    expect("F2 1-4 unresolved: waits", run(true, &o, &why) == CH341_SEL_WAIT);
    B.now += 600;                                 /* 2.1 s after 1-3 was first seen */
    expect("F2 1-4 does not inherit 1-3's time: still waiting", run(true, &o, &why) == CH341_SEL_WAIT);
    B.now += 1500;                                /* 2.1 s after 1-4 was first seen */
    expect("F2 ... refused on 1-4's own deadline", run(true, &o, &why) == CH341_SEL_REFUSED &&
           has(why, "no permission"));

    /* F2/DELTA-4-v3: without a selector an unreadable serial does not start a
     * deadline (it is not inconclusive). */
    bus({{"TESTSN01", "1-2", 1, false, false}, {"AAAAAAAA", "1-3", 0, false, false}});
    B.unreadable[0] = true;
    B.scan_ms = 1000;                             /* step 0 ends at +1000, step 1 at +2000 */
    init(nullptr);
    expect("unreadable + ACCESS, no selector: waits on the ACCESS stick", run(true, &o, &why) == CH341_SEL_WAIT);
    B.scan_ms = 0;
    B.now += 1500;                                /* 3.5 s after start: an unreadable-seeded deadline (+3000) would have expired */
    expect("... the deadline is the ACCESS stick's (+4000), not the unreadable one's: still waiting",
           run(true, &o, &why) == CH341_SEL_WAIT);

    /* N4 (r2): shutdown during a walk releases every held stick. */
    bus({{"AAAAAAAA", "1-2", 1, false, false}, {"BBBBBBBB", "1-3", 1, false, false},
         {nullptr, "1-4", 1, true, true}});
    init(nullptr);
    {
        char w[240] = "";
        loraham_ch341 *dev = nullptr;
        daemon_ch341_select_begin(true);
        int guard = 0;
        while (B.opens < 2 && ++guard < 50)
            daemon_ch341_select_step(&dev, w, sizeof(w));
        expect("N4 two sticks held mid-walk", B.s[0].claimed && B.s[1].claimed);
        daemon_ch341_select_cancel();
        expect("N4 cancel releases them", !B.s[0].claimed && !B.s[1].claimed && B.closes == 2);
    }
}

static void test_r5(void)
{
    int o;
    std::string why;

    /* F1 (r3): a candidate that left before its deadline never expires: the
     * next scan does not see it unresolved, so its deadline does not count. */
    bus({{"TESTSN01", "1-2", 1, false, false}, {"AAAAAAAA", "1-3", 0, false, false}});
    init("TESTSN01");
    expect("r5 F1 match + ACCESS on 1-3: waits", run(true, &o, &why) == CH341_SEL_WAIT);
    B.n = 1;                                      /* 1-3 is unplugged */
    B.now += 2100;                                /* past 1-3's deadline */
    expect("r5 F1 the departed candidate's deadline does not refuse: the match opens",
           run(true, &o, &why) == CH341_SEL_OPENED && o == 0);

    /* F1 (r3): a replacement on the same port is a new connection with its
     * own 2 s. */
    bus({{"TESTSN01", "1-2", 1, false, false}, {"AAAAAAAA", "1-3", 0, false, false}});
    B.addr[1] = 5;
    init("TESTSN01");
    expect("r5 F1 ACCESS on 1-3 (connection 5): waits", run(true, &o, &why) == CH341_SEL_WAIT);
    B.now += 1500;
    B.addr[1] = 6;                                /* replugged: a new connection, still ACCESS */
    expect("r5 F1 the replacement starts its own deadline", run(true, &o, &why) == CH341_SEL_WAIT);
    B.now += 600;                                 /* 2.1 s after the first one, 0.6 s after the replacement */
    expect("r5 F1 ... and does not inherit the old one's time: still waiting",
           run(true, &o, &why) == CH341_SEL_WAIT);

    /* F2 (r3): an ARRIVED between the classification and the open: classify
     * again (a duplicate serial may have come). */
    bus({{"TESTSN01", "1-2", 1, false, false}});
    init("TESTSN01");
    {
        char w[240] = "";
        loraham_ch341 *dev = nullptr;
        daemon_ch341_select_begin(false);
        DaemonCh341SelectResult r = CH341_SEL_PENDING;
        for (int i = 0; i < 3; i++)                /* scan begin, step, decide -> PH_OPEN */
            r = daemon_ch341_select_step(&dev, w, sizeof(w));
        B.s[B.n++] = {"TESTSN01", "1-3", 1, false, false};   /* a duplicate arrives */
        daemon_ch341_select_note_arrival();
        r = daemon_ch341_select_step(&dev, w, sizeof(w));
        expect("r5 F2 an ARRIVED before the open: no open on the stale classification",
               r == CH341_SEL_WAIT && B.opens == 0);
    }
    expect("r5 F2 ... the fresh classification sees the duplicate: not unique",
           run(false, &o, &why) == CH341_SEL_WAIT && has(why, "not unique") && B.opens == 0);

    /* Tests r3 F3: no refusal but BUSY begins with "BUSY:". */
    struct Refusal { const char *name; std::initializer_list<Stick> sticks; const char *sel; bool busy; };
    const Refusal cases[] = {
        { "serial absent", {{"ABCDEFGH", "1-3", 1, false, false}}, "TESTSN01", false },
        { "not unique", {{"TESTSN01A", "1-2", 1, false, false}, {"TESTSN01B", "1-3", 1, false, false}}, "TESTSN01", false },
        { "several with serials", {{"AAAAAAAA", "1-2", 1, false, false}, {"BBBBBBBB", "1-3", 1, false, false}}, nullptr, false },
        { "two without a serial", {{nullptr, "1-2", 1, false, false}, {nullptr, "1-3", 1, false, false}}, nullptr, false },
        { "no stick", {}, nullptr, false },
        { "BUSY --usb-serial", {{"TESTSN01", "1-2", 1, true, true}}, "TESTSN01", true },
        { "BUSY sole stick", {{nullptr, "1-2", 1, true, true}}, nullptr, true },
    };
    for (const Refusal &c : cases) {
        bus(c.sticks);
        init(c.sel);
        DaemonCh341SelectResult r = run(true, &o, &why);
        bool starts = why.rfind("BUSY:", 0) == 0;
        expect(std::string("refusal prefix: ") + c.name + (c.busy ? " begins with BUSY:" : " does not begin with BUSY:"),
               r == CH341_SEL_REFUSED && starts == c.busy);
    }
    /* ACCESS expiry and an unreadable serial are refusals too. */
    bus({{"TESTSN01", "1-2", 0, false, false}});
    init(nullptr);
    run(true, &o, &why);
    B.now += 2000;
    expect("refusal prefix: ACCESS expiry does not begin with BUSY:",
           run(true, &o, &why) == CH341_SEL_REFUSED && why.rfind("BUSY:", 0) != 0);
    bus({{"TESTSN01", "1-2", 1, false, false}});
    B.unreadable[0] = true;
    init(nullptr);
    expect("refusal prefix: an unreadable serial does not begin with BUSY:",
           run(true, &o, &why) == CH341_SEL_REFUSED && why.rfind("BUSY:", 0) != 0);
}

/* One step with an optional ARRIVED before it; time moves by ms. */
static DaemonCh341SelectResult step_with(bool arrival, uint64_t ms, loraham_ch341 **dev, char *w)
{
    B.now += ms;
    if (arrival)
        daemon_ch341_select_note_arrival();
    return daemon_ch341_select_step(dev, w, 240);
}

static void test_r6(void)
{
    int o;
    std::string why;
    char w[240];
    loraham_ch341 *dev = nullptr;

    /* F2 (r4): an ARRIVED after the decision to walk, before its first step:
     * no walk step on the stale snapshot. */
    port_daemon_unplugged();
    B.now += 3000;                                /* past any gap and settle */
    daemon_ch341_select_begin(false);
    DaemonCh341SelectResult r = CH341_SEL_PENDING;
    for (int i = 0; i < 4; i++)                   /* scan begin, 2 steps, decide -> PH_WALK */
        r = step_with(false, 0, &dev, w);
    r = step_with(true, 0, &dev, w);
    expect("r6 F2 an ARRIVED before the first walk step: released, no walk, rescan",
           r == CH341_SEL_WAIT && B.opens == 0 && B.seeds == 0);

    /* F2 (r4): an ARRIVED during the walk: every held stick released, rescan. */
    bus({{"AAAAAAAA", "1-2", 1, false, false}, {"BBBBBBBB", "1-3", 1, false, false}, {nullptr, "1-4", 1, false, false}});
    init(nullptr);
    run(true, &o, &why);                           /* the port daemon on 1-4 */
    B.s[2].claimed = false;
    B.now += 5000;
    B.opens = B.seeds = B.closes = 0;
    daemon_ch341_select_begin(false);
    for (int i = 0; i < 5; i++)                   /* scan begin, 3 steps, decide -> PH_WALK */
        r = step_with(false, 0, &dev, w);
    r = step_with(false, 0, &dev, w);             /* walk attempt 1: holds 1-2 */
    expect("r6 F2 mid-walk: one stick held", B.s[0].claimed && B.opens == 1);
    r = step_with(true, 0, &dev, w);              /* an ARRIVED during the walk */
    expect("r6 F2 an ARRIVED during the walk: the held stick released, no further attempt",
           r == CH341_SEL_WAIT && !B.s[0].claimed && B.opens == 1);
    B.now += 1000;                                /* the gap after the aborted walk */
    expect("r6 F2 ... the fresh scan walks to the stick without a serial",
           run(false, &o, &why) == CH341_SEL_OPENED && o == 2);

    /* F3 (r4): a stick flapping on EVERY pass (an ARRIVED before every step):
     * after the 2 s cap one final snapshot is taken and the walk completes. */
    port_daemon_unplugged();
    uint64_t t0 = B.now;
    daemon_ch341_select_note_arrival();
    bool opened = false;
    for (int pass = 0; pass < 200 && !opened; pass++) {
        daemon_ch341_select_begin(false);
        for (int s = 0; s < 20; s++) {
            r = step_with(true, 50, &dev, w);
            if (r == CH341_SEL_OPENED) {
                opened = true;
                break;
            }
            if (r != CH341_SEL_PENDING)
                break;
        }
        B.now = std::max(B.now, daemon_ch341_select_retry_at());
    }
    expect("r6 F3 flapping on every pass: the walk still completes after the cap",
           opened && B.now >= t0 + 2000 && B.now < t0 + 4000);
}

/* A reopen selection whose scan sees `before`, with an ARRIVED (the stick of
 * `after`) during the scan; returns the first result after the scan. */
static DaemonCh341SelectResult reopen_with_arrival_during_scan(std::initializer_list<Stick> after, std::string *why)
{
    char w[240] = "";
    loraham_ch341 *dev = nullptr;
    daemon_ch341_select_begin(false);
    DaemonCh341SelectResult r = daemon_ch341_select_step(&dev, w, sizeof(w));   /* scan begin */
    daemon_ch341_select_note_arrival();                                         /* the ARRIVED, taken now */
    int n_before = B.n;
    while (r == CH341_SEL_PENDING && B.steps < 1000) {
        r = daemon_ch341_select_step(&dev, w, sizeof(w));
        if (n_before >= 0 && B.n == n_before) {                                 /* the stick shows up after the snapshot */
            B.n = 0;
            for (const Stick &s : after)
                B.s[B.n++] = s;
            n_before = -1;
        }
    }
    *why = w;
    return r;
}

static void test_r7(void)
{
    int o;
    std::string why;

    /* Core r5 F1: an ARRIVED taken during a reopen scan must not be lost
     * behind a stale terminal result: classify again, then the stick opens. */
    struct Case { const char *name; const char *sel; std::initializer_list<Stick> before, after; };
    const Case cases[] = {
        { "no Meshstick attached", "TESTSN01", {}, {{"TESTSN01", "1-2", 1, false, false}} },
        { "serial not found", "TESTSN01", {{"ABCDEFGH", "1-3", 1, false, false}},
          {{"ABCDEFGH", "1-3", 1, false, false}, {"TESTSN01", "1-2", 1, false, false}} },
        { "not unique", "TESTSN01", {{"TESTSN01", "1-2", 1, false, false}, {"TESTSN01", "1-3", 1, false, false}},
          {{"TESTSN01", "1-2", 1, false, false}} },
    };
    for (const Case &c : cases) {
        bus(c.before);
        init(c.sel);
        B.opens = 0;
        DaemonCh341SelectResult r = reopen_with_arrival_during_scan(c.after, &why);
        expect(std::string("r7 F1 ") + c.name + " + an ARRIVED during the scan: no stale wait, classify again",
               r == CH341_SEL_WAIT && daemon_ch341_select_retry_at() != 0 && why.empty());
        expect(std::string("r7 F1 ") + c.name + " ... the next selection opens the arrived stick",
               run(false, &o, &why) == CH341_SEL_OPENED);
    }

    /* "several Meshsticks: give --usb-serial" for the daemon of the stick
     * without a serial, while that stick arrives during the scan. */
    bus({{nullptr, "1-4", 1, false, false}});
    init(nullptr);
    run(true, &o, &why);                             /* the port daemon on 1-4 */
    bus({{"AAAAAAAA", "1-2", 1, false, false}, {"BBBBBBBB", "1-3", 1, false, false}});
    B.now += 5000;
    {
        DaemonCh341SelectResult r = reopen_with_arrival_during_scan(
            {{"AAAAAAAA", "1-2", 1, false, false}, {"BBBBBBBB", "1-3", 1, false, false}, {nullptr, "1-4", 1, false, false}}, &why);
        expect("r7 F1 several sticks + its stick arriving during the scan: no stale wait, classify again",
               r == CH341_SEL_WAIT && daemon_ch341_select_retry_at() != 0 && why.empty());
        B.now += 600;                                /* the settle after that ARRIVED */
        expect("r7 F1 several sticks ... the next selection walks to it", run(false, &o, &why) == CH341_SEL_OPENED && o == 2);
    }

    /* Past the cap: the final snapshot of a serial daemon still reclassifies on
     * an ARRIVED before PH_OPEN (the cap exemption is Rule B's walk only). */
    bus({{"TESTSN01", "1-2", 1, false, false}});
    init("TESTSN01");
    daemon_ch341_select_note_arrival();
    B.now += 3000;                                   /* past the 2 s cap of that burst */
    {
        char w[240] = "";
        loraham_ch341 *dev = nullptr;
        for (int round = 0; round < 2; round++) {    /* round 0 makes the next snapshot the final one */
            daemon_ch341_select_begin(false);
            DaemonCh341SelectResult r = CH341_SEL_PENDING;
            r = daemon_ch341_select_step(&dev, w, sizeof(w));       /* scan begin */
            if (round == 0)
                daemon_ch341_select_note_arrival();                 /* during the scan, past the cap */
            r = daemon_ch341_select_step(&dev, w, sizeof(w));       /* step */
            r = daemon_ch341_select_step(&dev, w, sizeof(w));       /* decide -> PH_OPEN */
            if (round == 1)
                daemon_ch341_select_note_arrival();                 /* before PH_OPEN of the final snapshot */
            r = daemon_ch341_select_step(&dev, w, sizeof(w));
            if (round == 1)
                expect("r7 F1 past the cap, the final snapshot + an ARRIVED before PH_OPEN: no open",
                       r == CH341_SEL_WAIT && B.opens == 0);
        }
    }
}

static void test_r8(void)
{
    int o;
    std::string why;

    /* Second-reader note on r7, corrected by the r11 invariant sweep (C1): a
     * failing device-list read on a reopen is retried after 200 ms (never on
     * every pass, never only on an ARRIVED: a transient failure must not leave
     * a present stick unopened until a replug). */
    bus({{"TESTSN01", "1-2", 1, false, false}});
    init("TESTSN01");
    daemon_ch341_select_note_arrival();
    g_list_fails = 1;
    uint64_t t_fail = B.now;
    expect("r8/r12 list read failure on a reopen: waits with the reason",
           run(false, &o, &why) == CH341_SEL_WAIT && has(why, "could not be read"));
    expect("r12 C1 ... a bounded retry: due 200 ms later, never 0, never at once",
           daemon_ch341_select_retry_at() == t_fail + DAEMON_CH341_BUSY_RETRY_MS);
    g_list_fails = 0;
    B.now = daemon_ch341_select_retry_at();
    expect("r12 C1 ... the list readable again: the retry opens the present stick without a new ARRIVED",
           run(false, &o, &why) == CH341_SEL_OPENED);

    /* Core r6 B1: at START an ARRIVED during the selection invalidates it too
     * (immediate rescan, no settle). The start loop feeds every ARRIVED. */
    auto start_until_open_step = [&](int steps) {
        char w[240] = "";
        loraham_ch341 *dev = nullptr;
        daemon_ch341_select_begin(true);
        for (int i = 0; i < steps; i++)
            daemon_ch341_select_step(&dev, w, sizeof(w));
    };
    /* (1) a duplicate --usb-serial stick arrives before PH_OPEN */
    bus({{"TESTSN01", "1-2", 1, false, false}});
    init("TESTSN01");
    start_until_open_step(3);                        /* scan begin, step, decide -> PH_OPEN */
    B.s[B.n++] = {"TESTSN01", "1-3", 1, false, false};
    daemon_ch341_select_note_arrival();
    {
        char w[240] = "";
        loraham_ch341 *dev = nullptr;
        DaemonCh341SelectResult r = daemon_ch341_select_step(&dev, w, sizeof(w));
        expect("r8 B1 start: a duplicate serial arriving before the open: no open, classify again",
               r == CH341_SEL_WAIT && B.opens == 0 && daemon_ch341_select_retry_at() != 0);
    }
    expect("r8 B1 start ... refused: not unique", run(true, &o, &why) == CH341_SEL_REFUSED && has(why, "not unique"));
    /* (2) one stick seen, a second (with a serial) arrives before the plain open */
    bus({{"AAAAAAAA", "1-2", 1, false, false}});
    init(nullptr);
    start_until_open_step(3);
    B.s[B.n++] = {"BBBBBBBB", "1-3", 1, false, false};
    daemon_ch341_select_note_arrival();
    {
        char w[240] = "";
        loraham_ch341 *dev = nullptr;
        DaemonCh341SelectResult r = daemon_ch341_select_step(&dev, w, sizeof(w));
        expect("r8 B1 start: one stick became several before the plain open: no open, classify again",
               r == CH341_SEL_WAIT && B.opens == 0);
    }
    expect("r8 B1 start ... refused: give --usb-serial", run(true, &o, &why) == CH341_SEL_REFUSED &&
           has(why, "give --usb-serial"));
    /* (3) a second stick without a serial arrives during the Rule B walk */
    bus({{"AAAAAAAA", "1-2", 1, false, false}, {nullptr, "1-3", 1, false, false}});
    init(nullptr);
    start_until_open_step(4);                        /* scan begin, 2 steps, decide -> PH_WALK */
    B.s[B.n++] = {nullptr, "1-4", 1, false, false};
    daemon_ch341_select_note_arrival();
    {
        char w[240] = "";
        loraham_ch341 *dev = nullptr;
        DaemonCh341SelectResult r = daemon_ch341_select_step(&dev, w, sizeof(w));
        expect("r8 B1 start: a second stick without a serial arriving before the walk opens: released, classify again",
               r == CH341_SEL_WAIT && B.opens == 0);
    }
    expect("r8 B1 start ... refused: only one without a serial", run(true, &o, &why) == CH341_SEL_REFUSED &&
           has(why, "only one is supported"));

    /* T15b (tests r6): a deliberately stalled walk (3 s per pine64 open) and a
     * new serial stick arriving during it: no more than N_snap attempts, every
     * held stick released, a rescan after the settle, then the walk. */
    bus({{"AAAAAAAA", "1-2", 1, false, false}, {"BBBBBBBB", "1-3", 1, false, false}, {nullptr, "1-4", 1, false, false}});
    init(nullptr);
    run(true, &o, &why);                             /* the port daemon on 1-4 */
    B.s[2].claimed = false;
    B.now += 5000;
    B.opens = B.closes = B.seeds = 0;
    B.open_ms = 3000;
    {
        char w[240] = "";
        loraham_ch341 *dev = nullptr;
        uint64_t t0 = B.now;
        daemon_ch341_select_begin(false);
        DaemonCh341SelectResult r = CH341_SEL_PENDING;
        for (int i = 0; i < 6; i++)                  /* scan begin, 3 steps, decide -> PH_WALK, attempt 1 (stalls 3 s) */
            r = daemon_ch341_select_step(&dev, w, sizeof(w));
        B.s[B.n++] = {"CCCCCCCC", "1-5", 1, false, false};   /* arrives during the stalled walk */
        daemon_ch341_select_note_arrival();
        r = daemon_ch341_select_step(&dev, w, sizeof(w));
        expect("T15b stalled walk + an ARRIVED: stops (no more than N_snap attempts, here 1 of 3)",
               r == CH341_SEL_WAIT && B.opens == 1 && B.now >= t0 + 3000);
        expect("T15b stalled walk: every held stick released", !B.s[0].claimed && !B.s[1].claimed && B.closes == 1);
    }
    {
        uint64_t walk_end = B.now;
        B.now += 500;                                /* inside the gap: no attempt */
        int opens = B.opens;
        expect("T15b stalled walk: the >= 1 s gap after the walk holds (no attempt, retry at the gap's end)",
               run(false, &o, &why) == CH341_SEL_WAIT && B.opens == opens &&
               daemon_ch341_select_retry_at() >= walk_end + 1000);
        B.now = walk_end + 1000;
    }
    expect("T15b stalled walk: the rescan and walk then open the stick without a serial",
           run(false, &o, &why) == CH341_SEL_OPENED && o == 2);
    B.open_ms = 0;
}

/* Drives the selection like daemon_ch341_select_at_start's loop (WAIT -> a new
 * begin at the retry time) until it concludes or 2000 steps have run. */
static DaemonCh341SelectResult run_until_concluded(bool at_start, int *opened_idx, std::string *why, uint64_t *ms)
{
    char w[240] = "";
    loraham_ch341 *dev = nullptr;
    DaemonCh341SelectResult r = CH341_SEL_WAIT;
    uint64_t t0 = B.now;
    daemon_ch341_select_begin(at_start);
    for (int i = 0; i < 2000; i++) {
        r = daemon_ch341_select_step(&dev, w, sizeof(w));
        if (r == CH341_SEL_OPENED || r == CH341_SEL_REFUSED)
            break;
        if (r == CH341_SEL_WAIT) {
            if (!at_start)
                break;
            B.now = std::max(B.now + 5, daemon_ch341_select_retry_at());
            daemon_ch341_select_begin(true);
        }
    }
    *opened_idx = dev ? idx(dev) : -1;
    *why = w;
    *ms = B.now - t0;
    return r;
}

/* r9 (second reader, r8 F1): a stick ARRIVING during every start scan must not
 * keep the start selection from concluding: the 2 s settle cap holds for the
 * refusal and PH_OPEN as for Rule B; past it the snapshot is final. */
static void test_r9(void)
{
    int o;
    std::string why;
    uint64_t ms;

    bus({{"AAAAAAAA", "1-2", 1, false, false}, {"AAAAAAAA", "1-3", 1, false, false}});
    B.scan_ms = 5;
    B.flap = true;
    init("AAAAAAAA");
    DaemonCh341SelectResult r = run_until_concluded(true, &o, &why, &ms);
    expect("r9 F1 a duplicate serial with a stick arriving in every start scan: refused (not unique)",
           r == CH341_SEL_REFUSED && why.find("not unique") != std::string::npos);
    expect("r9 F1 ... within the 2 s cap plus one scan", ms <= 2000 + 2 * 5 + 10);
    expect("r9 F1 ... nothing opened", B.opens == 0);

    bus({{"AAAAAAAA", "1-2", 1, false, false}});
    B.scan_ms = 5;
    B.flap = true;
    init("AAAAAAAA");
    r = run_until_concluded(true, &o, &why, &ms);
    expect("r9 F1 one stick, an ARRIVED in every start scan: the selection concludes within the cap",
           (r == CH341_SEL_REFUSED || r == CH341_SEL_OPENED) && ms <= 2000 + 2 * 5 + 10);
    expect("r9 F1 ... never opened on a stale snapshot (an ARRIVED after the final one): refused, nothing opened",
           r == CH341_SEL_REFUSED && B.opens == 0 && why.find("kept arriving") != std::string::npos);

    /* A single ARRIVED before the open still rescans and opens (row 65 kept). */
    bus({{"AAAAAAAA", "1-2", 1, false, false}});
    init("AAAAAAAA");
    {
        char w[240] = "";
        loraham_ch341 *dev = nullptr;
        daemon_ch341_select_begin(true);
        DaemonCh341SelectResult s = CH341_SEL_PENDING;
        for (int i = 0; i < 3; i++)                  /* scan begin, the step, decide -> PH_OPEN */
            s = daemon_ch341_select_step(&dev, w, sizeof(w));
        daemon_ch341_select_note_arrival();
        s = daemon_ch341_select_step(&dev, w, sizeof(w));
        expect("r9 one ARRIVED before the open (inside the cap): classified again, not opened", s == CH341_SEL_WAIT && B.opens == 0);
    }
    expect("r9 ... the rescan then opens it", run(true, &o, &why) == CH341_SEL_OPENED && o == 0);
}

/* r10 (core r7 B1): an ARRIVED during the blocking open (pinedio_init) or its
 * identity read is taken before the result is committed. */
static void test_r10(void)
{
    int o;
    std::string why;
    uint64_t ms;

    /* PH_OPEN: --usb-serial, a duplicate serial arrives during the open. */
    bus({{"AAAAAAAA", "1-2", 1, false, false}});
    B.add_at_open = 1;
    B.add_stick = {"AAAAAAAA", "1-3", 1, false, false};
    init("AAAAAAAA");
    DaemonCh341SelectResult r = run_until_concluded(true, &o, &why, &ms);
    expect("r10 B1 PH_OPEN: a duplicate serial arriving during the open: not opened, refused not unique",
           r == CH341_SEL_REFUSED && why.find("not unique") != std::string::npos && o == -1);
    expect("r10 B1 PH_OPEN: ... the stick opened in that window closed again", B.opens == 1 && B.closes == 1 &&
           !B.s[0].claimed);
    expect("r10 B1 PH_OPEN: ... nothing bound", daemon_ch341_select_bound()[0] == '\0' ||
           strcmp(daemon_ch341_select_bound(), "-") == 0);

    /* The Rule B target attempt: a second stick without a serial arrives
     * during the target's open (the second attempt). */
    bus({{"BBBBBBBB", "1-2", 1, false, false}, {nullptr, "1-3", 1, false, false}});
    B.add_at_open = 2;
    B.add_stick = {nullptr, "1-4", 1, false, false};
    init(nullptr);
    r = run_until_concluded(true, &o, &why, &ms);
    expect("r10 B1 Rule B target: a second stick without a serial arriving during the target open: refused",
           r == CH341_SEL_REFUSED && why.find("more than one Meshstick without a serial") != std::string::npos);
    expect("r10 B1 Rule B target: ... every stick released", !B.s[0].claimed && !B.s[1].claimed && !B.s[2].claimed);

    /* START, no selector: a second serial stick arrives during the plain open. */
    bus({{"AAAAAAAA", "1-2", 1, false, false}});
    B.add_at_open = 1;
    B.add_stick = {"CCCCCCCC", "1-3", 1, false, false};
    init(nullptr);
    r = run_until_concluded(true, &o, &why, &ms);
    expect("r10 B1 start, no selector: a second stick arriving during the open: refused (give --usb-serial)",
           r == CH341_SEL_REFUSED && why.find("give --usb-serial") != std::string::npos && !B.s[0].claimed);

    /* Past the cap the same window refuses rather than open on a stale snapshot. */
    bus({{"AAAAAAAA", "1-2", 1, false, false}});
    B.add_at_open = 1;
    B.add_stick = {"AAAAAAAA", "1-3", 1, false, false};
    B.open_ms = 2500;                              /* the open itself outlasts the 2 s cap */
    init("AAAAAAAA");
    daemon_ch341_select_note_arrival();            /* a burst began before this selection */
    r = run_until_concluded(true, &o, &why, &ms);
    expect("r10 B1 past the cap: refused, nothing kept open", r == CH341_SEL_REFUSED && o == -1 && !B.s[0].claimed);
    B.open_ms = 0;
}

/* r11 (second reader, r10 F1): on a reopen, a refusal caused by the ARRIVEDs
 * themselves must not wait for a further ARRIVED (those are consumed): a cable
 * that settles inside the final selection would leave the stick unopened.
 * Driven like the reopen loop: WAIT with a retry time -> advance and begin
 * again; WAIT with retry time 0 -> only an ARRIVED would retry: stop. */
static void test_r11(void)
{
    int stuck = 0, runs = 0;
    std::string stuck_at;
    for (uint64_t end = 1990; end <= 2060; end++) {
        int o;
        std::string why;
        bus({{"AAAAAAAA", "1-2", 1, false, false}});
        init(nullptr);
        run(true, &o, &why);                         /* the daemon bound serial AAAAAAAA */
        B.s[0].claimed = false;                      /* unplugged, replugged, flapping */
        B.now += 5000;
        B.scan_ms = 5;
        uint64_t t0 = B.now;
        B.flap_until = t0 + end;
        daemon_ch341_select_note_arrival();          /* the ARRIVED that starts the reopen */
        char w[240] = "";
        loraham_ch341 *dev = nullptr;
        DaemonCh341SelectResult s = CH341_SEL_WAIT;
        daemon_ch341_select_begin(false);
        for (int i = 0; i < 4000; i++) {
            s = daemon_ch341_select_step(&dev, w, sizeof(w));
            if (s == CH341_SEL_OPENED)
                break;
            if (s == CH341_SEL_WAIT) {
                uint64_t at = daemon_ch341_select_retry_at();
                if (at == 0)
                    break;
                B.now = std::max(B.now + 5, at);
                daemon_ch341_select_begin(false);
            }
        }
        runs++;
        if (!(s == CH341_SEL_OPENED && idx(dev) == 0)) {
            stuck++;
            stuck_at += " " + std::to_string(end);
        }
        B.flap_until = 0;
        B.scan_ms = 0;
    }
    expect("r11 F1 reopen, the flapping ending anywhere 1990..2060 ms in (around the final selection): every run "
           "opens the settled stick (" + std::to_string(runs - stuck) + "/" + std::to_string(runs) +
           (stuck ? "; stuck at" + stuck_at : "") + ")", stuck == 0 && runs == 71);
}

/* First non-PENDING result of a selection begun at start. */
static DaemonCh341SelectResult first_result(bool at_start, std::string *why)
{
    char w[240] = "";
    loraham_ch341 *dev = nullptr;
    DaemonCh341SelectResult s = CH341_SEL_PENDING;
    daemon_ch341_select_begin(at_start);
    for (int i = 0; i < 100 && s == CH341_SEL_PENDING; i++)
        s = daemon_ch341_select_step(&dev, w, sizeof(w));
    *why = w;
    return s;
}

/* r12 (core r8 B1, B2): the events of a FAILED open's window and of the
 * Rule B preliminary identity read are taken before BUSY, a hold or the cap;
 * an unbound LEFT invalidates a snapshot like an ARRIVED (no settle burst). */
static void test_r12(void)
{
    int o;
    std::string why;
    uint64_t ms;

    /* B1 PH_OPEN at start: the open fails (the stick is held elsewhere) while a
     * duplicate arrives: classified again, "not unique", never BUSY on the old
     * topology. */
    bus({{"AAAAAAAA", "1-2", 1, true, true}});
    B.add_at_open = 1;
    B.add_stick = {"AAAAAAAA", "1-3", 1, true, true};   /* held elsewhere too: the open returns NULL */
    init("AAAAAAAA");
    DaemonCh341SelectResult r = run_until_concluded(true, &o, &why, &ms);
    expect("r12 B1 a failed start open with a duplicate arriving in its window: refused not unique, not BUSY",
           r == CH341_SEL_REFUSED && why.find("not unique") != std::string::npos && why.find("BUSY") == std::string::npos);

    /* B1 Rule B, the last attempt fails while a second stick without a serial
     * (held elsewhere) arrives: classified again, not BUSY. */
    bus({{"BBBBBBBB", "1-2", 1, false, false}, {nullptr, "1-3", 1, true, true}});
    B.add_at_open = 2;
    B.add_stick = {nullptr, "1-4", 1, true, true};
    init(nullptr);
    r = run_until_concluded(true, &o, &why, &ms);
    expect("r12 B1 the last Rule B attempt fails with a second serial-less stick arriving: refused (more than one), not BUSY",
           r == CH341_SEL_REFUSED && why.find("more than one Meshstick without a serial") != std::string::npos &&
           !B.s[0].claimed);

    /* B1 Rule B, the last attempt opens a serial stick that arrived during it
     * (the preliminary identity read): no BUSY from the cap first. */
    bus({{"BBBBBBBB", "1-2", 1, false, false}, {nullptr, "1-3", 1, true, true}});
    B.add_at_open = 2;
    B.add_stick = {"DDDDDDDD", "1-4", 1, false, false};
    init(nullptr);
    r = first_result(true, &why);
    expect("r12 B1 the last Rule B attempt holds a stick that arrived during it: classified again (WAIT), not the cap's BUSY",
           r == CH341_SEL_WAIT && !B.s[0].claimed && !B.s[2].claimed);

    /* B2 reopen, two copies of the bound serial, one leaves after its scan
     * step: classified again, opens the remaining one (was: "not unique"
     * waiting for an ARRIVED that never comes). */
    bus({{"AAAAAAAA", "1-2", 1, false, false}});
    init(nullptr);
    run(true, &o, &why);                             /* bound serial AAAAAAAA */
    B.s[0].claimed = false;
    B.s[B.n++] = {"AAAAAAAA", "1-3", 1, false, false};
    B.now += 5000;
    B.leave_after_step = 2;                          /* the copy on 1-3 */
    r = run_until_concluded(false, &o, &why, &ms);
    if (r == CH341_SEL_WAIT && daemon_ch341_select_retry_at() == 0) {
        B.leave_after_step = 0;
        r = CH341_SEL_WAIT;                          /* stuck: nothing would retry */
    } else {
        B.leave_after_step = 0;
        if (r == CH341_SEL_WAIT) {
            B.now = std::max(B.now, daemon_ch341_select_retry_at());
            r = run(false, &o, &why);
        }
    }
    expect("r12 B2 reopen: a duplicate leaving after its scan step: the remaining stick opens (not stuck on not unique)",
           r == CH341_SEL_OPENED && o == 0);

    /* B2 start, no selector, two serial sticks, one leaves after its step. */
    bus({{"AAAAAAAA", "1-2", 1, false, false}, {"BBBBBBBB", "1-3", 1, false, false}});
    B.leave_after_step = 2;
    init(nullptr);
    r = run_until_concluded(true, &o, &why, &ms);
    B.leave_after_step = 0;
    expect("r12 B2 start: several becoming one by a LEFT: opens it (not \"give --usb-serial\")",
           r == CH341_SEL_OPENED && o == 0);

    /* B2 start, two without a serial, one leaves after its step. */
    bus({{nullptr, "1-2", 1, false, false}, {nullptr, "1-3", 1, false, false}});
    B.leave_after_step = 2;
    init(nullptr);
    r = run_until_concluded(true, &o, &why, &ms);
    B.leave_after_step = 0;
    expect("r12 B2 start: two without a serial becoming one by a LEFT: opens it (not \"more than one\")",
           r == CH341_SEL_OPENED && o == 0);

    /* B2 a LEFT starts no settle burst: a reopen Rule B walk right after a
     * LEFT does not wait the 500 ms quiet time. */
    port_daemon_unplugged();
    uint64_t t0 = B.now;
    daemon_ch341_select_note_left();
    r = run(false, &o, &why);
    expect("r12 B2 a LEFT alone starts no settle burst: the walk runs at once", r == CH341_SEL_OPENED && B.now - t0 < 500);
}

/* The invariant sweep on r11 (SELECTION-INVARIANTS-daemon-v1): the cells not
 * already covered by test_r12 (A1-A3 = the B1 cases, B1/B2 = the B2 cases,
 * C1 = test_r8). I1 freshness, I3 reopen liveness, I6 a start's reason from
 * the current topology. */
static void test_sweep(void)
{
    int o;
    std::string why;
    uint64_t ms;

    /* B3 REOPEN: two without a serial, one LEAVES after its scan step: opens
     * the remaining one (was: "more than one", retry 0, stuck). */
    bus({{nullptr, "1-2", 1, false, false}});
    init(nullptr);
    run(true, &o, &why);                             /* the port daemon on 1-2 */
    B.s[0].claimed = false;
    B.s[B.n++] = {nullptr, "1-3", 1, false, false};
    B.now += 5000;
    B.leave_after_step = 2;
    DaemonCh341SelectResult r = run_until_concluded(false, &o, &why, &ms);
    for (int k = 0; k < 5 && r == CH341_SEL_WAIT && daemon_ch341_select_retry_at() != 0; k++) {
        B.now = std::max(B.now + 5, daemon_ch341_select_retry_at());
        r = run(false, &o, &why);
    }
    expect("sweep B3 reopen: two without a serial becoming one by a LEFT: opens it (I3)", r == CH341_SEL_OPENED && o == 0);

    /* B4 REOPEN: the target itself LEAVES after its scan step: a wait that
     * its ARRIVED ends, nothing opened. */
    bus({{"SERIAL01", "1-2", 1, false, false}});
    init("SERIAL01");
    run(true, &o, &why);
    B.s[0].claimed = false;
    B.now += 5000;
    B.leave_after_step = 1;
    int opens = B.opens;
    r = run_until_concluded(false, &o, &why, &ms);
    for (int k = 0; k < 5 && r == CH341_SEL_WAIT && daemon_ch341_select_retry_at() != 0; k++) {
        B.now = std::max(B.now + 5, daemon_ch341_select_retry_at());
        r = run(false, &o, &why);
    }
    expect("sweep B4 reopen: the target leaving after its scan: waits, nothing opened", r == CH341_SEL_WAIT &&
           B.opens == opens && has(why, "no Meshstick with serial"));

    /* D1 START: open() NULL with no event: the BUSY refusal (exit 4). */
    bus({{"SERIAL01", "1-2", 1, true, true}});
    init("SERIAL01");
    r = run_until_concluded(true, &o, &why, &ms);
    expect("sweep D1 start: a failed open with no event: BUSY (designed)", r == CH341_SEL_REFUSED && has(why, "BUSY:"));

    /* D2 REOPEN: open() NULL with no event: a BUSY retry is scheduled. */
    bus({{"SERIAL01", "1-2", 1, false, false}});
    init("SERIAL01");
    run(true, &o, &why);
    B.now += 5000;                                   /* still claimed: held elsewhere now */
    B.s[0].foreign = true;
    r = first_result(false, &why);
    expect("sweep D2 reopen: a failed open with no event: a BUSY retry is due (I3)", r == CH341_SEL_WAIT &&
           daemon_ch341_select_retry_at() > B.now);

    /* E1 REOPEN Rule B: an ARRIVED during a failed walk open: classified again
     * at once (retry now), not a BUSY wait on the stale snapshot. */
    bus({{"BBBBBBBB", "1-2", 1, false, false}, {nullptr, "1-3", 1, true, true}});
    init(nullptr);
    B.add_at_open = 2;
    B.add_stick = {nullptr, "1-4", 1, true, true};
    daemon_ch341_select_begin(false);
    char w[240] = "";
    loraham_ch341 *dev = nullptr;
    r = CH341_SEL_PENDING;
    for (int k = 0; k < 50 && r == CH341_SEL_PENDING; k++)
        r = daemon_ch341_select_step(&dev, w, sizeof(w));
    expect("sweep E1 reopen Rule B: an ARRIVED during a failed walk open: classified again at once",
           r == CH341_SEL_WAIT && daemon_ch341_select_retry_at() == B.now && !B.s[0].claimed);
}

/* r13 (core r9 B1): the bind reads a fresh device list after the last drain;
 * an ARRIVED, an unbound LEFT or the bound stick's own LEFT in that window is
 * taken before anything is committed: unbind, close, classify again. */
static void test_r13(void)
{
    int o;
    std::string why;
    uint64_t ms;

    /* (a) start, --usb-serial: a duplicate arrives during the bind. */
    bus({{"AAAAAAAA", "1-2", 1, false, false}});
    B.add_at_bind = true;
    B.add_stick = {"AAAAAAAA", "1-3", 1, false, false};
    init("AAAAAAAA");
    DaemonCh341SelectResult r = run_until_concluded(true, &o, &why, &ms);
    expect("r13 B1 (a) a duplicate arriving during the bind: refused not unique, nothing kept",
           r == CH341_SEL_REFUSED && has(why, "not unique") && o == -1 && B.unbinds >= 1 && !B.s[0].claimed);

    /* (b) start, no selector: a second stick without a serial arrives during the bind. */
    bus({{nullptr, "1-2", 1, false, false}});
    B.add_at_bind = true;
    B.add_stick = {nullptr, "1-3", 1, false, false};
    init(nullptr);
    r = run_until_concluded(true, &o, &why, &ms);
    expect("r13 B1 (b) a second serial-less stick arriving during the bind: refused (more than one), nothing kept",
           r == CH341_SEL_REFUSED && has(why, "more than one Meshstick without a serial") && B.unbinds >= 1 &&
           !B.s[0].claimed && strcmp(daemon_ch341_select_bound(), "-") == 0);

    /* (c) reopen: the stick being bound leaves during the bind. */
    bus({{"AAAAAAAA", "1-2", 1, false, false}});
    init(nullptr);
    run(true, &o, &why);                             /* bound serial AAAAAAAA */
    B.s[0].claimed = false;
    B.now += 5000;
    B.unbinds = 0;
    B.bound_leaves_at_bind = true;
    r = first_result(false, &why);
    expect("r13 B1 (c) the bound stick leaving during the bind: not opened, unbound, classified again",
           r == CH341_SEL_WAIT && B.unbinds == 1 && !B.s[0].claimed);
    r = run(false, &o, &why);
    expect("r13 B1 (c) ... the rescan finds it gone: waits for it", r == CH341_SEL_WAIT && has(why, "no Meshstick with serial"));

    /* (d) start: an unbound LEFT during the bind: classified again, then opens. */
    bus({{"AAAAAAAA", "1-2", 1, false, false}});
    B.left_at_bind = true;
    init(nullptr);
    r = run_until_concluded(true, &o, &why, &ms);
    expect("r13 B1 (d) an unbound LEFT during the bind: unbound, classified again, then opened",
           r == CH341_SEL_OPENED && o == 0 && B.unbinds == 1 && B.binds == 2);
}

/* r14 (second reader, r13 F1): a failed bind (its own device-list read) on a
 * reopen is retried after 200 ms like a failed scan list (row 81), never left
 * waiting for an ARRIVED a present stick never sends; at start a refusal. */
static void test_r14(void)
{
    int o;
    std::string why;
    uint64_t ms;

    bus({{"AAAAAAAA", "1-2", 1, false, false}});
    init(nullptr);
    run(true, &o, &why);                             /* bound serial AAAAAAAA */
    B.s[0].claimed = false;
    B.now += 5000;
    B.bind_rc = -1;                                  /* the bind's list read fails once */
    uint64_t t0 = B.now;
    DaemonCh341SelectResult r = first_result(false, &why);
    expect("r14 F1 reopen: a failed bind: nothing kept, a bounded retry (200 ms), not retry 0",
           r == CH341_SEL_WAIT && !B.s[0].claimed && daemon_ch341_select_retry_at() == t0 + DAEMON_CH341_BUSY_RETRY_MS);
    B.bind_rc = 0;
    B.now = daemon_ch341_select_retry_at();
    expect("r14 F1 ... the retry binds and opens the present stick", run(false, &o, &why) == CH341_SEL_OPENED && o == 0);

    bus({{"AAAAAAAA", "1-2", 1, false, false}});
    B.bind_rc = -1;
    init(nullptr);
    r = first_result(true, &why);
    expect("r14 F1 start: a failed bind is the refusal (exit 4)", r == CH341_SEL_REFUSED &&
           has(why, "could not be matched") && !B.s[0].claimed);

    /* The same class for the opened stick's identity read (every read x a
     * transient failure x reopen): a bounded retry, then it opens. */
    bus({{"AAAAAAAA", "1-2", 1, false, false}});
    init(nullptr);
    run(true, &o, &why);
    B.s[0].claimed = false;
    B.now += 5000;
    B.identity_fails = 1;
    t0 = B.now;
    r = first_result(false, &why);
    expect("r14 reopen: a failed identity read: nothing kept, a bounded retry (200 ms), not retry 0",
           r == CH341_SEL_WAIT && !B.s[0].claimed && daemon_ch341_select_retry_at() == t0 + DAEMON_CH341_BUSY_RETRY_MS);
    B.now = daemon_ch341_select_retry_at();
    expect("r14 ... the retry opens the present stick", run(false, &o, &why) == CH341_SEL_OPENED && o == 0);

    /* Sweep DELTA-1 T2: the Rule B walk's preliminary identity read fails
     * once on a serial stick: held (not taken for the target), the walk goes
     * on and opens the stick without a serial (was: "the opened stick has a
     * serial", a wrong-cause exit 4 at start). */
    bus({{"BBBBBBBB", "1-2", 1, false, false}, {nullptr, "1-3", 1, false, false}});
    B.identity_fails = 1;
    init(nullptr);
    r = run_until_concluded(true, &o, &why, &ms);
    expect("r14 T2 start Rule B: a failed preliminary identity read: held, the walk opens the stick without a serial",
           r == CH341_SEL_OPENED && o == 1 && !B.s[0].claimed);
}

/* The second reader's transient sweep (SELECTION-INVARIANTS DELTA-1): every
 * read in the selection path x one transient failure x START/REOPEN x
 * plain/Rule B, 20 cells. START: a refusal names the failing read itself (no
 * wrong cause) or the stick opens; REOPEN: the stick opens through bounded
 * retries, never a wait on retry 0. */
static void test_transient_sweep(void)
{
    const char *what[] = {"scan list", "scan step", "open", "identity", "bind"};
    /* START expectations: the refusal's reason, or nullptr = opens. */
    const char *start_plain[] = {"device list could not be read", nullptr, "BUSY:", "identity could not be read",
                                 "could not be matched"};
    const char *start_ruleb[] = {"device list could not be read", nullptr, "BUSY:", nullptr, "could not be matched"};
    for (int walk = 0; walk < 2; walk++) {
        for (int start = 1; start >= 0; start--) {
            for (int k = 0; k < 5; k++) {
                if (walk)
                    bus({{"OTHER001", "1-6", 1, false, false}, {nullptr, "1-4", 1, false, false}});
                else
                    bus({{"SERIAL01", "1-2", 1, false, false}});
                init(walk ? nullptr : "SERIAL01");
                T1S = {};
                if (!start) {
                    int o;
                    std::string w0;
                    run(true, &o, &w0);
                    for (int s = 0; s < B.n; s++)
                        B.s[s].claimed = false;
                    B.now += 5000;
                }
                if (k == 0) T1S.list_fail = 1;
                if (k == 1) T1S.step_unreadable = 1;
                if (k == 2) T1S.open_null = 1;
                if (k == 3) B.identity_fails = 1;
                if (k == 4) T1S.bind_fail = 1;
                char w[240] = "";
                loraham_ch341 *dev = nullptr;
                DaemonCh341SelectResult r = CH341_SEL_PENDING;
                daemon_ch341_select_begin(start);
                for (int s = 0; s < 2000; s++) {
                    r = daemon_ch341_select_step(&dev, w, sizeof(w));
                    if (r == CH341_SEL_OPENED || r == CH341_SEL_REFUSED)
                        break;
                    if (r == CH341_SEL_WAIT) {
                        uint64_t at = daemon_ch341_select_retry_at();
                        if (at == 0)
                            break;
                        B.now = std::max(B.now, at);
                        daemon_ch341_select_begin(start);
                    }
                }
                bool ok;
                if (start) {
                    const char *want = walk ? start_ruleb[k] : start_plain[k];
                    ok = want ? (r == CH341_SEL_REFUSED && strstr(w, want) != nullptr) : r == CH341_SEL_OPENED;
                } else {
                    ok = r == CH341_SEL_OPENED;
                }
                char name[160];
                snprintf(name, sizeof(name), "transient T%d%d%d %s %s, one failed %s: %s", walk, 1 - start, k,
                         start ? "START" : "REOPEN", walk ? "Rule B" : "plain", what[k],
                         start ? "the read's own refusal or an open" : "opens through bounded retries");
                expect(name, ok);
            }
        }
    }
}

/* r15 (core r10 B1, B2): a FAILED bind's window counts too (a change there
 * classifies again before the transient retry or refusal); a line without a
 * selector names the sole stick (serial, or port without one), never the
 * bare USB id. */
static void test_r15(void)
{
    int o;
    std::string why;
    uint64_t ms;

    /* B1 START: the bind fails while a duplicate arrives: "not unique", not
     * the bind's own refusal on the old topology. */
    bus({{"AAAAAAAA", "1-2", 1, false, false}});
    T1S = {};
    T1S.bind_fail = 1;
    B.add_at_bind = true;
    B.add_stick = {"AAAAAAAA", "1-3", 1, false, false};
    init("AAAAAAAA");
    DaemonCh341SelectResult r = run_until_concluded(true, &o, &why, &ms);
    expect("r15 B1 start: a failed bind with a duplicate arriving in it: refused not unique (not \"could not be matched\")",
           r == CH341_SEL_REFUSED && has(why, "not unique") && !B.s[0].claimed);

    /* B1 START: the bind fails while another stick leaves: classified again,
     * then opens (sweep DELTA-2 G1). */
    bus({{"AAAAAAAA", "1-2", 1, false, false}});
    T1S = {};
    T1S.bind_fail = 1;
    B.left_at_bind = true;
    init(nullptr);
    r = run_until_concluded(true, &o, &why, &ms);
    expect("r15 B1 start: a failed bind with a LEFT in it: classified again, then opened (not the bind's refusal)",
           r == CH341_SEL_OPENED && o == 0);

    /* B1 REOPEN: the bind fails while another stick leaves: classified again
     * at once (retry now), not the 200 ms transient retry on the old snapshot. */
    bus({{"AAAAAAAA", "1-2", 1, false, false}});
    init(nullptr);
    run(true, &o, &why);
    B.s[0].claimed = false;
    B.now += 5000;
    T1S = {};
    T1S.bind_fail = 1;
    B.left_at_bind = true;
    r = first_result(false, &why);
    expect("r15 B1 reopen: a failed bind with a LEFT in it: classified again at once, nothing kept",
           r == CH341_SEL_WAIT && daemon_ch341_select_retry_at() == B.now && !B.s[0].claimed);

    /* B2: the sole stick named in the BUSY line, exact. */
    bus({{"AAAAAAAA", "1-2", 1, true, true}});
    init(nullptr);
    r = run_until_concluded(true, &o, &why, &ms);
    expect("r15 B2 start, no selector, sole stick with a serial held elsewhere: BUSY names its serial (exact)",
           r == CH341_SEL_REFUSED && why == "BUSY: stick AAAAAAAA present but pine64 could not select or open it "
                                            "(another process, a bound kernel driver, or an unreadable serial)");
    bus({{nullptr, "1-2", 1, true, true}});
    init(nullptr);
    r = run_until_concluded(true, &o, &why, &ms);
    expect("r15 B2 start, no selector, sole stick without a serial held elsewhere: BUSY names its port (exact)",
           r == CH341_SEL_REFUSED && why == "BUSY: stick 1-2 present but pine64 could not select or open it "
                                            "(another process, a bound kernel driver, or an unreadable serial)");
    /* ... and the ACCESS refusal likewise (the sole stick's port: no serial readable). */
    bus({{"AAAAAAAA", "1-2", 0, false, false}});
    init(nullptr);
    r = run_until_concluded(true, &o, &why, &ms);
    expect("r15 B2 start, no selector, sole stick in ACCESS: the refusal names its port (exact)",
           r == CH341_SEL_REFUSED && why == "no permission to open stick 1-2, or it cannot be told apart (udev rule?)");
}

/* r16 (core r11 F1/N1; sweep DELTA-3 H1/H2): LEFT-only churn starts the 2 s
 * cap like ARRIVED churn, so the selection concludes; the bound stick lost
 * during a bind on a FINAL Rule B snapshot (the monitor reports a queue
 * overflow as the bound stick's LEFT) is never committed. */
static void test_r16(void)
{
    int o;
    std::string why;
    uint64_t ms;

    /* H2 START: an unbound LEFT in every scan step. */
    bus({{"AAAAAAAA", "1-2", 1, false, false}});
    B.scan_ms = 5;
    B.lflap = true;
    init(nullptr);
    DaemonCh341SelectResult r = run_until_concluded(true, &o, &why, &ms);
    expect("r16 H2 start: LEFT-only churn concludes within the cap plus one scan (the final snapshot's refusal)",
           r == CH341_SEL_REFUSED && has(why, "kept arriving") && ms <= 2000 + 2 * 5 + 10);

    /* H2 REOPEN: the same, driven like the reopen loop. */
    bus({{"AAAAAAAA", "1-2", 1, false, false}});
    init(nullptr);
    run(true, &o, &why);
    B.s[0].claimed = false;
    B.now += 5000;
    B.scan_ms = 5;
    B.lflap = true;
    uint64_t t0 = B.now;
    r = CH341_SEL_WAIT;
    for (int k = 0; k < 2000; k++) {
        r = run(false, &o, &why);
        if (r != CH341_SEL_WAIT || has(why, "kept arriving"))
            break;
        B.now = std::max(B.now + 5, daemon_ch341_select_retry_at());
    }
    expect("r16 H2 reopen: LEFT-only churn concludes within the cap (the refusal, a 500 ms retry)",
           r == CH341_SEL_WAIT && has(why, "kept arriving") && B.now - t0 <= 2000 + 2 * 5 + 10 &&
           daemon_ch341_select_retry_at() == B.now + DAEMON_CH341_SETTLE_QUIET_MS);
    B.lflap = false;

    /* H1: the bound stick lost during the bind of a FINAL Rule B snapshot
     * (an ARRIVED in every scan step until past the cap, then the target's
     * LEFT lost in an overflow = the monitor's bound-LEFT flag): not
     * committed; unbound, closed, classified again. */
    bus({{"BBBBBBBB", "1-2", 1, false, false}, {nullptr, "1-3", 1, false, false}});
    init(nullptr);
    B.scan_ms = 5;
    B.flap_until = B.now + 2100;                     /* past the cap: the next snapshot is final */
    daemon_ch341_select_note_arrival();
    B.bound_leaves_at_bind = true;
    r = run_until_concluded(true, &o, &why, &ms);
    expect("r16 H1 start Rule B, final snapshot: the bound stick lost during the bind is not committed; unbound, and "
           "the fresh classification opens the remaining stick",
           r == CH341_SEL_OPENED && o == 0 && B.unbinds >= 1 && !B.s[1].claimed);

    /* H1 REOPEN (tests r12 F1): the same on a reopen: no commit, a fresh
     * classification, a wait for the stick (UNPLUGGED), never an exit. */
    bus({{nullptr, "1-3", 1, false, false}});
    init(nullptr);
    run(true, &o, &why);                             /* the port daemon on 1-3 */
    B.s[0].claimed = false;
    B.s[B.n++] = {"BBBBBBBB", "1-2", 1, false, false};
    B.now += 5000;
    B.scan_ms = 5;
    B.unbinds = 0;
    B.flap_until = B.now + 2100;                     /* ARRIVEDs past the cap: the next snapshot is final */
    daemon_ch341_select_note_arrival();
    B.bound_leaves_at_bind = true;
    r = CH341_SEL_WAIT;
    for (int k = 0; k < 2000; k++) {
        r = run(false, &o, &why);
        if (r != CH341_SEL_WAIT || B.unbinds >= 1)
            break;
        B.now = std::max(B.now + 5, daemon_ch341_select_retry_at());
    }
    expect("r16 H1 reopen Rule B, final snapshot: the bound stick lost during the bind: not committed, unbound, waiting",
           r == CH341_SEL_WAIT && B.unbinds == 1 && !B.s[0].claimed && strcmp(daemon_ch341_select_bound(), "-") != 0);
    B.flap_until = 0;
    B.scan_ms = 0;
    r = run(false, &o, &why);
    expect("r16 H1 reopen ... the fresh classification finds its port gone: waits for it (no exit)",
           r == CH341_SEL_WAIT);
}

/* r17 (core r12 F1, N): a sole stick without a selector whose open fails
 * with another error (openable -2, not ACCESS) gets the same 2 s rule as
 * ACCESS, not an immediate BUSY; with several sticks the ACCESS expiry line
 * names the governing candidate's port. */
static void test_r17(void)
{
    int o;
    std::string why;
    uint64_t ms;

    /* F1 START, permanent: refused after the 2 s, not BUSY at once. */
    bus({{"AAAAAAAA", "1-2", -2, false, false}});
    B.scan_ms = 5;
    init(nullptr);
    DaemonCh341SelectResult r = run_until_concluded(true, &o, &why, &ms);
    expect("r17 F1 start: a sole stick with another open error: the 2 s rule, then refused (exact), not BUSY at once",
           r == CH341_SEL_REFUSED && ms >= 2000 &&
           why == "no permission to open stick 1-2, or it cannot be told apart (udev rule?)");

    /* F1 START, transient: opens once the open works. */
    bus({{"AAAAAAAA", "1-2", -2, false, false}});
    B.scan_ms = 5;
    init(nullptr);
    char w[240] = "";
    loraham_ch341 *dev = nullptr;
    daemon_ch341_select_begin(true);
    r = CH341_SEL_PENDING;
    for (int k = 0; k < 50 && r == CH341_SEL_PENDING; k++)
        r = daemon_ch341_select_step(&dev, w, sizeof(w));
    expect("r17 F1 start, transient: the first pass waits (no BUSY)", r == CH341_SEL_WAIT && B.opens == 0);
    B.s[0].openable = 1;
    B.now = std::max(B.now, daemon_ch341_select_retry_at());
    expect("r17 F1 start, transient: the retry opens it", run(true, &o, &why) == CH341_SEL_OPENED && o == 0);

    /* F1 REOPEN in port mode (the stick without a serial; a serial-mode
     * reopen already counted every open error): waits within the 2 s (the
     * deadline kept across passes), then the line and a wait. */
    bus({{nullptr, "1-2", 1, false, false}});
    init(nullptr);
    run(true, &o, &why);
    B.s[0].claimed = false;
    B.s[0].openable = -2;
    B.now += 5000;
    uint64_t t0 = B.now;
    int opens0 = B.opens;
    r = first_result(false, &why);
    expect("r17 F1 reopen (port mode): another open error: waits within the 2 s (a retry due), no open tried",
           r == CH341_SEL_WAIT && daemon_ch341_select_retry_at() > B.now && B.opens == opens0);
    for (int k = 0; k < 50; k++) {
        B.now = std::max(B.now, daemon_ch341_select_retry_at());
        r = first_result(false, &why);
        if (has(why, "no permission"))
            break;
    }
    expect("r17 F1 reopen ... the 2 s run out (the deadline kept across passes): the refusal line, waiting",
           r == CH341_SEL_WAIT && has(why, "no permission to open stick 1-2") && B.now - t0 >= 2000 &&
           B.now - t0 <= 2000 + DAEMON_CH341_BUSY_RETRY_MS + 10);

    /* N: several sticks, one in ACCESS: the expiry line names its port, exact. */
    bus({{"AAAAAAAA", "1-2", 1, false, false}, {"BBBBBBBB", "1-3", 0, false, false}});
    B.scan_ms = 5;
    init(nullptr);
    r = run_until_concluded(true, &o, &why, &ms);
    expect("r17 N start, several sticks, one in ACCESS: the expiry line names its port (exact)",
           r == CH341_SEL_REFUSED && why == "no permission to open stick 1-3, or it cannot be told apart (udev rule?)");

    /* Sweep DELTA-4 K2: another open error (-2) on the serial-less stick beside a serial stick. */
    bus({{"AAAAAAAA", "1-2", 1, false, false}, {nullptr, "1-4", -2, false, false}});
    B.scan_ms = 5;
    init(nullptr);
    r = run_until_concluded(true, &o, &why, &ms);
    expect("r17 K2 start: -2 beside a serial stick: refused after the 2 s, naming 1-4 (exact)",
           r == CH341_SEL_REFUSED && ms >= 2000 &&
           why == "no permission to open stick 1-4, or it cannot be told apart (udev rule?)");

    /* K3: two sticks, both in ACCESS: the earliest candidate named, at start and on a reopen. */
    bus({{nullptr, "1-2", 0, false, false}, {nullptr, "1-3", 0, false, false}});
    B.scan_ms = 5;
    init(nullptr);
    r = run_until_concluded(true, &o, &why, &ms);
    expect("r17 K3 start: two sticks in ACCESS: the expiry line names the first candidate's port (exact)",
           r == CH341_SEL_REFUSED && why == "no permission to open stick 1-2, or it cannot be told apart (udev rule?)");
    bus({{nullptr, "1-2", 1, false, false}});
    init(nullptr);
    run(true, &o, &why);                             /* the port daemon on 1-2 */
    B.s[0].claimed = false;
    B.s[0].openable = 0;
    B.s[B.n++] = {nullptr, "1-3", 0, false, false};
    B.now += 5000;
    r = CH341_SEL_WAIT;
    for (int k = 0; k < 50; k++) {
        r = first_result(false, &why);
        if (has(why, "no permission"))
            break;
        B.now = std::max(B.now, daemon_ch341_select_retry_at());
    }
    /* On a reopen the mode is fixed (here port mode, the stored port 1-2):
     * the line names the port asked for, unchanged since r16 (a guard). */
    expect("r17 K3 reopen (port mode): two sticks in ACCESS: the line names the stored port 1-2 (exact)",
           r == CH341_SEL_WAIT && why == "no permission to open stick 1-2, or it cannot be told apart (udev rule?)");
}

/* r18 (core r13 F1, F2): an ACCESS expiry checks staleness first and keeps
 * every deadline across the rescan (one 2 s per connection); an expiry found
 * during the scan names exactly the seen candidate that triggered it. */
static DaemonCh341SelectResult drive_with_change_at(bool at_start, uint64_t when, bool left, uint64_t *ms, std::string *why)
{
    char w[240] = "";
    loraham_ch341 *dev = nullptr;
    DaemonCh341SelectResult r = CH341_SEL_PENDING;
    uint64_t t0 = B.now;
    bool fired = false;
    daemon_ch341_select_begin(at_start);
    for (int i = 0; i < 4000; i++) {
        /* an unrelated change at the expiry, DURING a scan (after its
         * snapshot began), so that scan's decision is stale */
        if (!fired && B.now >= t0 + when && r == CH341_SEL_PENDING) {
            fired = true;
            if (left)
                daemon_ch341_select_note_left();
            else
                daemon_ch341_select_note_arrival();
        }
        r = daemon_ch341_select_step(&dev, w, sizeof(w));
        if (r == CH341_SEL_OPENED || r == CH341_SEL_REFUSED)
            break;
        if (r == CH341_SEL_WAIT) {
            if (!at_start && has(w, "no permission"))
                break;
            B.now = std::max(B.now + 5, daemon_ch341_select_retry_at());
            daemon_ch341_select_begin(at_start);
        }
    }
    *ms = B.now - t0;
    *why = w;
    return r;
}

static void test_r18(void)
{
    int o;
    std::string why;
    uint64_t ms;

    /* F1 START: a sole stick in ACCESS for good, an unrelated ARRIVED at its
     * 2 s expiry: refused on the ORIGINAL deadline (one rescan), not after a
     * fresh 2 s. */
    bus({{"AAAAAAAA", "1-2", 0, false, false}});
    B.scan_ms = 5;
    init(nullptr);
    DaemonCh341SelectResult r = drive_with_change_at(true, 2000, false, &ms, &why);
    expect("r18 F1 start: ACCESS for good + an ARRIVED at the expiry: refused on the original deadline (not a fresh 2 s)",
           r == CH341_SEL_REFUSED && has(why, "no permission to open stick 1-2") && ms >= 2000 && ms < 2000 + 500);

    /* F1 START with -2 and an unrelated LEFT at the expiry. */
    bus({{"AAAAAAAA", "1-2", -2, false, false}});
    B.scan_ms = 5;
    init(nullptr);
    r = drive_with_change_at(true, 2000, true, &ms, &why);
    expect("r18 F1 start: -2 for good + a LEFT at the expiry: refused on the original deadline",
           r == CH341_SEL_REFUSED && has(why, "no permission to open stick 1-2") && ms >= 2000 && ms < 2000 + 500);

    /* F1 REOPEN (port mode): ACCESS for good + an unrelated ARRIVED at the expiry. */
    bus({{nullptr, "1-2", 1, false, false}});
    init(nullptr);
    run(true, &o, &why);
    B.s[0].claimed = false;
    B.s[0].openable = 0;
    B.now += 5000;
    B.scan_ms = 5;
    r = drive_with_change_at(false, 2000, false, &ms, &why);
    expect("r18 F1 reopen: ACCESS for good + an ARRIVED at the expiry: the line on the original deadline",
           r == CH341_SEL_WAIT && has(why, "no permission to open stick 1-2") && ms >= 2000 && ms < 2000 + 500);

    /* Sweep DELTA-5 L1: --usb-serial, the stick in ACCESS for good; one
     * unbound LEFT 5 ms before its expiry: refused on the original deadline. */
    bus({{"AAAAAAAA", "1-2", 0, false, false}});
    B.scan_ms = 5;
    init("AAAAAAAA");
    r = drive_with_change_at(true, 1995, true, &ms, &why);
    expect("r18 L1 start, --usb-serial in ACCESS + a LEFT 5 ms before the expiry: refused on the original deadline",
           r == CH341_SEL_REFUSED && has(why, "no permission to open stick AAAAAAAA") && ms >= 2000 && ms < 2000 + 500);

    /* L1 REOPEN (serial mode): the same LEFT 5 ms before the expiry. */
    bus({{"AAAAAAAA", "1-2", 1, false, false}});
    init(nullptr);
    run(true, &o, &why);                             /* bound serial AAAAAAAA */
    B.s[0].claimed = false;
    B.s[0].openable = 0;
    B.now += 5000;
    B.scan_ms = 5;
    r = drive_with_change_at(false, 1995, true, &ms, &why);
    expect("r18 L1 reopen, serial mode in ACCESS + a LEFT 5 ms before the expiry: the line on the original deadline",
           r == CH341_SEL_WAIT && has(why, "no permission to open stick AAAAAAAA") && ms >= 2000 && ms < 2000 + 500);

    /* L1 churn: an unbound LEFT every 1990 ms: still the original deadline. */
    bus({{"AAAAAAAA", "1-2", 0, false, false}});
    B.scan_ms = 5;
    init("AAAAAAAA");
    {
        char w[240] = "";
        loraham_ch341 *dev = nullptr;
        uint64_t t0 = B.now, next = t0 + 1990;
        r = CH341_SEL_PENDING;
        daemon_ch341_select_begin(true);
        for (int i = 0; i < 20000; i++) {
            if (B.now >= next && r == CH341_SEL_PENDING) {   /* mid-scan */
                daemon_ch341_select_note_left();
                next += 1990;
            }
            r = daemon_ch341_select_step(&dev, w, sizeof(w));
            if (r == CH341_SEL_OPENED || r == CH341_SEL_REFUSED)
                break;
            if (r == CH341_SEL_WAIT) {
                B.now = std::max(B.now + 5, daemon_ch341_select_retry_at());
                daemon_ch341_select_begin(true);
            }
        }
        ms = B.now - t0;
        why = w;
    }
    expect("r18 L1 churn every 1990 ms: concludes on the original deadline (refused), never a fresh 2 s",
           r == CH341_SEL_REFUSED && has(why, "no permission to open stick AAAAAAAA") && ms < 2000 + 500);

    /* F2: three candidates in ACCESS stamped at distinct times (1-2 first);
     * 1-2 then leaves; in the next scan the expiry is triggered by the SEEN
     * 1-3 and must name 1-3, not the gone 1-2 still in the table. */
    bus({{nullptr, "1-2", 0, false, false}, {nullptr, "1-3", 0, false, false}, {nullptr, "1-4", 0, false, false}});
    B.scan_ms = 5;
    init(nullptr);
    std::string w0;
    r = first_result(true, &w0);                     /* the first pass: stamps 1-2, 1-3, 1-4 and waits */
    B.s[0] = B.s[1];
    B.s[1] = B.s[2];
    B.n = 2;                                         /* 1-2 has left: the list is 1-3, 1-4 */
    B.now += 3000;                                   /* every deadline has passed */
    r = first_result(true, &why);
    expect("r18 F2 the scan's expiry names exactly the seen candidate that triggered it (1-3), not a gone one (exact)",
           r == CH341_SEL_REFUSED && why == "no permission to open stick 1-3, or it cannot be told apart (udev rule?)");
}

/* r20 (core r15 F1): a change seen after the last scan step, while a
 * candidate is unresolved, makes access_wait classify again at once (retry
 * now), keeping the deadline; the fresh scan sees the change. */
static void test_r20(void)
{
    int cells = 0, ok = 0;
    std::string bad;
    for (int open_err = 0; open_err < 2; open_err++)              /* openable 0 (ACCESS) or -2 */
    for (int sel = 0; sel < 2; sel++)                             /* --usb-serial or no selector */
    for (int start = 1; start >= 0; start--)
    for (int left = 0; left < 2; left++) {
        int o;
        std::string why;
        bus({{"AAAAAAAA", "1-2", 1, false, false}});
        B.scan_ms = 5;
        init(sel ? "AAAAAAAA" : nullptr);
        if (!start) {
            run(true, &o, &why);
            B.s[0].claimed = false;
            B.now += 5000;
        }
        B.s[0].openable = open_err ? -2 : 0;                     /* A unresolved */
        char w[240] = "";
        loraham_ch341 *dev = nullptr;
        uint64_t t0 = B.now;
        daemon_ch341_select_begin(start);
        daemon_ch341_select_step(&dev, w, sizeof(w));             /* the scan begins */
        daemon_ch341_select_step(&dev, w, sizeof(w));             /* its last step: A unresolved */
        uint64_t deadline = t0 + 5 + DAEMON_CH341_ACCESS_MS;
        if (left) {
            daemon_ch341_select_note_left();                      /* seen by the caller's drain */
        } else {
            B.s[B.n++] = {"BBBBBBBB", "1-3", 1, false, false};    /* a second stick joins */
            daemon_ch341_select_note_arrival();
        }
        uint64_t event = B.now;
        DaemonCh341SelectResult r = daemon_ch341_select_step(&dev, w, sizeof(w));   /* decide */
        bool at_once = r == CH341_SEL_WAIT && daemon_ch341_select_retry_at() == event;
        /* then drive like the start / reopen loops to the conclusion */
        for (int k = 0; k < 4000; k++) {
            B.now = std::max(B.now, daemon_ch341_select_retry_at());
            daemon_ch341_select_begin(start);
            r = CH341_SEL_PENDING;
            for (int s = 0; s < 50 && r == CH341_SEL_PENDING; s++)
                r = daemon_ch341_select_step(&dev, w, sizeof(w));
            if (r == CH341_SEL_REFUSED || r == CH341_SEL_OPENED || (r == CH341_SEL_WAIT && has(w, "no permission")))
                break;
        }
        bool concluded = has(w, "no permission") && B.now >= deadline && B.now <= deadline + 30;
        cells++;
        if (at_once && concluded)
            ok++;
        else {
            char d[160];
            snprintf(d, sizeof(d), " [open %d sel %d %s %s: at_once %d concluded %d at +%llu]", open_err ? -2 : 0, sel,
                     start ? "START" : "REOPEN", left ? "LEFT" : "ARRIVED", at_once, concluded,
                     (unsigned long long)(B.now - t0));
            bad += d;
        }
    }
    expect("r20 F1 (sweep DELTA-7, 16 cells) a change after the last scan step with A unresolved: classified again at once "
           "(retry at the event), concluded at A's deadline plus one scan (" + std::to_string(ok) + "/" +
           std::to_string(cells) + bad + ")", ok == 16 && cells == 16);
}

int main(void)
{
    test_serial_selector();
    test_no_selector();
    test_rule_b();
    test_access();
    test_r2_serial();
    test_r2_ruleb();
    test_r2_access();
    test_r4();
    test_r5();
    test_r6();
    test_r7();
    test_r8();
    test_r9();
    test_r10();
    test_r11();
    test_r12();
    test_sweep();
    test_r13();
    test_r14();
    test_transient_sweep();
    test_r15();
    test_r16();
    test_r17();
    test_r18();
    test_r20();
    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
