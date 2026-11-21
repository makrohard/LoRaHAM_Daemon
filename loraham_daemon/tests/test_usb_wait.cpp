/*
 * daemon_usb_wait: the unplug-wait state machine on fakes (a fake monitor, a
 * fake clock, fake radio steps). The fatal hook records instead of exiting.
 *
 *   T1  loss + LEFT -> UNPLUGGED, torn down, no exit
 *   T2  loss, no LEFT, present at the deadline -> exit 5 (and absent -> gone,
 *       list failure -> exit 5, never "gone")
 *   T6  re-init failure -> RADIO=FAILED + exit 5; success -> READY
 *   T11 admission: on_loss sets RADIO=UNPLUGGED at once
 *   T12 an ARRIVED during a deferred teardown is kept and acted on after it
 *   T13 the grace deadline is set once (a second loss does not restart it);
 *       a tick before the deadline does not classify; early wakeups (socket
 *       traffic) do not act; a resolved loss's deadline does not carry over
 *   T12 LEFT + ARRIVED in one dispatch; B1/F1 a kept ARRIVED reaches the
 *       selection as an arrival (the Rule B settle is not bypassed)
 * --radio-error retry (1.5.0), fake clock:
 *   T1/T1b a present-path fault: torn down, no exit; the first selection
 *       waits for the 1 s step, its own due retry included; after a plain
 *       LEFT at once; a plain loss after a recovered fault at once (F1)
 *   T2/T2c the backoff 1, 2, 4 ... 1024 s, then 1800 s, 1800 s; a retry
 *       time of 0, 200 ms or past the step waits for max(step, retry)
 *   T2b the step carries over a READY under 10 min, resets after 10 min
 *   T2d an ARRIVED in a 30 min wait selects at once
 *   T3  failed re-inits after a plain unplug: first selection at once, then
 *       1, 2, 4 s; then READY
 *   T6  a refused close: no new selection ever, an attempt on each step
 * The existing tests run in the exit mode (O3: exit 5 as 1.4.1).
 * The real exit 5 and the sockets are proven on the real binary
 * (test_ch341_process).
 */

#include "../daemon_usb_wait.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <unistd.h>

static int g_ok = 0;
static int g_fail = 0;

static void expect(const char *name, bool ok)
{
    if (ok) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s\n", name);
    }
}

/* ---- fakes ------------------------------------------------------------- */

static struct {
    uint64_t now;
    bool left;
    int present;            /* 1, 0, -1 */
    int present_calls;
    int arrivals;           /* pending ARRIVED events */
    int lefts;              /* pending LEFT events of other sticks */
    int lefts_fed;          /* reopen_left calls */
    int dispatches;
    bool teardown_ok;
    int teardowns;
    bool reinit_ok;
    int reinit_busy;        /* this many re-inits answer busy first */
    int reinits;
    bool loss_in_reinit;    /* the new HAL reports a loss inside the re-init (its replay / RX re-arm) */
    uint64_t loss_in_reinit_at;
    RadioHealth health;
    int fatals;
    char fatal_why[160];
    int reopen_begins;
    bool last_arrival;      /* reopen_begin's argument */
    int arrivals_fed;       /* reopen_arrival calls */
    int reopen_steps;
    int reopen_results[8];   /* per step: 0 pending, 1 opened, 2 wait */
    int reopen_n;
    int step_default;       /* a step after the scripted ones */
    uint64_t retry_at;      /* the selection's retry time (0 = only an ARRIVED) */
    int reinit_fail_n;      /* this many re-inits fail first */
    int faults;             /* count_fault calls */
    bool kept;              /* handle_kept */
} F;

static void f_dispatch(void *) { F.dispatches++; }
static bool f_left(void *) { return F.left; }
static int f_present(void *) { F.present_calls++; return F.present; }
static bool f_take_arrival(void *)
{
    if (F.arrivals <= 0)
        return false;
    F.arrivals--;
    return true;
}
static bool f_take_left(void *)
{
    if (F.lefts <= 0)
        return false;
    F.lefts--;
    return true;
}
static void f_reopen_left(void) { F.lefts_fed++; }
static bool f_teardown(void) { F.teardowns++; return F.teardown_ok; }
static int f_reinit(void)
{
    F.reinits++;
    if (F.reinit_fail_n > 0) {
        F.reinit_fail_n--;
        return 0;
    }
    if (F.reinit_busy > 0) {
        F.reinit_busy--;
        return -1;
    }
    if (F.reinit_ok)
        F.health = RADIO_HEALTH_READY;
    if (F.loss_in_reinit) {
        /* As the real re-init: lora_init() armed the new HAL and set READY;
         * the stick goes during the replay's RX re-arm, the HAL latches and
         * calls on_loss from inside reinit(), which still returns 1. */
        F.now = F.loss_in_reinit_at;
        daemon_usb_wait_on_loss();
    }
    return F.reinit_ok ? 1 : 0;
}
static void f_set_health(RadioHealth h) { F.health = h; }
static void f_fatal(const char *why)
{
    F.fatals++;
    snprintf(F.fatal_why, sizeof(F.fatal_why), "%s", why);
}
static uint64_t f_now(void) { return F.now; }
static void f_reopen_begin(bool arrival) { F.reopen_begins++; F.last_arrival = arrival; }
static void f_reopen_arrival(void) { F.arrivals_fed++; }
static int f_reopen_step(void)
{
    int i = F.reopen_steps++;
    return i < F.reopen_n ? F.reopen_results[i] : F.step_default;
}
static uint64_t f_reopen_retry_at(void) { return F.retry_at; }
static uint32_t f_count_fault(void) { return (uint32_t)++F.faults; }
static bool f_handle_kept(void) { return F.kept; }

static void setup_mode(bool retry)
{
    memset(&F, 0, sizeof(F));
    F.step_default = 1;
    F.now = 1000;
    F.present = 1;
    F.teardown_ok = true;
    F.reinit_ok = true;
    F.health = RADIO_HEALTH_READY;

    DaemonUsbWaitOps ops{};
    ops.stick = "TEST STICK";
    ops.mon = nullptr;
    ops.mon_dispatch = f_dispatch;
    ops.mon_left_bound = f_left;
    ops.mon_present_bound = f_present;
    ops.mon_take_arrival = f_take_arrival;
    ops.mon_take_left = f_take_left;
    ops.reopen_left = f_reopen_left;
    ops.teardown_try = f_teardown;
    ops.reinit = f_reinit;
    ops.reopen_begin = f_reopen_begin;
    ops.reopen_arrival = f_reopen_arrival;
    ops.reopen_step = f_reopen_step;
    ops.reopen_retry_at = f_reopen_retry_at;
    ops.set_health = f_set_health;
    ops.fatal = f_fatal;
    ops.now_ms = f_now;
    ops.retry = retry;
    ops.count_fault = f_count_fault;
    ops.handle_kept = f_handle_kept;
    daemon_usb_wait_reset();
    daemon_usb_wait_init(&ops);
}

static void setup(void) { setup_mode(false); }

/* ---- tests ------------------------------------------------------------- */

static void test_t1_left_unplugged(void)
{
    setup();
    daemon_usb_wait_tick();
    expect("T1: no loss: the events are handled, nothing is classified",
           daemon_usb_wait_state() == USB_WAIT_READY && F.dispatches == 1 && F.present_calls == 0);
    F.arrivals = 1;                             /* another stick plugged while READY */
    daemon_usb_wait_tick();
    expect("an arrival while READY is dropped, never acted on later", F.arrivals == 0 && F.reinits == 0);

    daemon_usb_wait_on_loss();
    expect("T11: on_loss sets RADIO=UNPLUGGED at once", F.health == RADIO_HEALTH_UNPLUGGED);
    F.left = true;
    daemon_usb_wait_tick();
    expect("T1: LEFT decides gone without the presence check", F.present_calls == 0);
    expect("T1: state TEARDOWN", daemon_usb_wait_state() == USB_WAIT_TEARDOWN);
    daemon_usb_wait_tick();
    expect("T1: torn down -> UNPLUGGED", daemon_usb_wait_state() == USB_WAIT_UNPLUGGED);
    expect("T1: no exit", F.fatals == 0);
    expect("T1: health stays UNPLUGGED", F.health == RADIO_HEALTH_UNPLUGGED);
    F.reopen_results[0] = 2;                    /* the forced selection finds nothing */
    F.reopen_n = 1;
    daemon_usb_wait_tick();
    expect("T1: one selection right after the teardown (B1), it finds nothing",
           F.reopen_begins == 1 && F.reinits == 0);
    daemon_usb_wait_tick();
    expect("T1: then waiting: no new selection without an arrival or a due retry",
           F.reopen_begins == 1 && F.reinits == 0);
}

static void test_t2_deadline(void)
{
    setup();
    daemon_usb_wait_on_loss();
    daemon_usb_wait_tick();
    expect("T13: before the deadline nothing is classified", F.present_calls == 0 && F.fatals == 0);
    F.now += 300;
    daemon_usb_wait_on_loss();                  /* a second failure */
    daemon_usb_wait_tick();
    expect("T13: a second loss does not restart the grace", F.present_calls == 0);
    F.now += 200;                               /* 500 ms after the FIRST loss */
    daemon_usb_wait_tick();
    expect("T2: present at the deadline -> one presence check", F.present_calls == 1);
    expect("T2: present -> exit 5", F.fatals == 1 &&
           strstr(F.fatal_why, "I/O error with the TEST STICK present") != nullptr);

    setup();
    F.present = 0;
    daemon_usb_wait_on_loss();
    daemon_usb_wait_tick();
    F.now += DAEMON_USB_WAIT_GRACE_MS;
    daemon_usb_wait_tick();
    expect("T2: absent at the deadline -> gone, no exit",
           daemon_usb_wait_state() == USB_WAIT_TEARDOWN && F.fatals == 0);

    setup();
    F.present = -1;
    daemon_usb_wait_on_loss();
    daemon_usb_wait_tick();
    F.now += DAEMON_USB_WAIT_GRACE_MS;
    daemon_usb_wait_tick();
    expect("T14: list read failure -> exit 5, never gone",
           F.fatals == 1 && daemon_usb_wait_state() != USB_WAIT_TEARDOWN &&
           strstr(F.fatal_why, "could not be read") != nullptr);
}

static void test_t12_arrival_during_teardown(void)
{
    setup();
    F.teardown_ok = false;                      /* the TX worker holds radio_mutex */
    daemon_usb_wait_on_loss();
    F.left = true;
    daemon_usb_wait_tick();
    F.arrivals = 1;                             /* quick replug */
    daemon_usb_wait_tick();
    daemon_usb_wait_tick();
    expect("T12: teardown deferred while the mutex is busy",
           daemon_usb_wait_state() == USB_WAIT_TEARDOWN && F.teardowns == 2);
    expect("T12: the arrival is not consumed during teardown", F.arrivals == 1 && F.reinits == 0);
    F.teardown_ok = true;
    daemon_usb_wait_tick();
    expect("T12: teardown completes", daemon_usb_wait_state() == USB_WAIT_UNPLUGGED);
    daemon_usb_wait_tick();
    expect("T12: the kept arrival re-inits", F.reinits == 1);
    expect("T6: re-init ok -> READY", daemon_usb_wait_state() == USB_WAIT_READY &&
           F.health == RADIO_HEALTH_READY && F.fatals == 0);
    daemon_usb_wait_tick();
    expect("after READY: no new classification without a new loss", F.present_calls == 0);
}

static void test_t6_reinit_fails(void)
{
    setup();
    F.reinit_ok = false;
    daemon_usb_wait_on_loss();
    F.left = true;
    daemon_usb_wait_tick();
    daemon_usb_wait_tick();
    F.arrivals = 1;
    daemon_usb_wait_tick();
    expect("T6: re-init failure -> RADIO=FAILED", F.health == RADIO_HEALTH_FAILED);
    expect("T6: re-init failure -> exit 5", F.fatals == 1 &&
           strstr(F.fatal_why, "re-init") != nullptr);
}

static void test_reopen_steps(void)
{
    setup();
    daemon_usb_wait_on_loss();
    F.left = true;
    daemon_usb_wait_tick();
    daemon_usb_wait_tick();                     /* UNPLUGGED */
    F.reopen_results[0] = 0;                    /* scan step */
    F.reopen_results[1] = 2;                    /* BUSY: wait */
    F.reopen_n = 2;
    F.arrivals = 1;
    daemon_usb_wait_tick();
    expect("reopen: begun by the ARRIVED, one step per pass", F.reopen_begins == 1 && F.reopen_steps == 1);
    daemon_usb_wait_tick();
    expect("reopen: a wait result keeps UNPLUGGED, no re-init",
           daemon_usb_wait_state() == USB_WAIT_UNPLUGGED && F.reinits == 0);
    daemon_usb_wait_tick();
    expect("reopen: nothing due, no ARRIVED -> no new reopen", F.reopen_begins == 1);
    F.retry_at = F.now;                         /* the BUSY retry period passed */
    daemon_usb_wait_tick();
    expect("reopen: a due retry begins a reopen without an ARRIVED", F.reopen_begins == 2);
    expect("reopen: a due retry is not an arrival (no settle burst)", !F.last_arrival);
    expect("reopen: opened -> re-init -> READY", F.reinits == 1 && daemon_usb_wait_state() == USB_WAIT_READY);
}

static void test_w12_fixes(void)
{
    /* B1: LEFT and ARRIVED before the HAL notices; then the loss. */
    setup();
    F.left = true;                              /* bound LEFT recorded while READY */
    F.arrivals = 1;                             /* the stick is already back */
    daemon_usb_wait_tick();
    expect("B1: arrivals are kept while a bound LEFT is recorded", F.arrivals == 1);
    daemon_usb_wait_on_loss();
    daemon_usb_wait_tick();                     /* LEFT -> gone */
    daemon_usb_wait_tick();                     /* teardown -> UNPLUGGED */
    F.arrivals = 0;                             /* even with no arrival left to take */
    daemon_usb_wait_tick();
    expect("B1: one selection right after the teardown, no new event needed",
           F.reopen_begins == 1 && F.reinits == 1 && daemon_usb_wait_state() == USB_WAIT_READY);

    /* B3: radio_mutex busy at re-init: retried, the stick stays selected, no exit. */
    setup();
    F.reinit_busy = 2;
    daemon_usb_wait_on_loss();
    F.left = true;
    daemon_usb_wait_tick();
    daemon_usb_wait_tick();
    daemon_usb_wait_tick();                     /* forced reopen -> opened -> re-init busy */
    expect("B3: busy re-init is not a failure", F.fatals == 0 && daemon_usb_wait_state() == USB_WAIT_UNPLUGGED);
    daemon_usb_wait_tick();
    expect("B3: retried on the next pass without a second selection", F.reopen_begins == 1 && F.reinits == 2);
    daemon_usb_wait_tick();
    expect("B3: ... and READY once the mutex is free", F.reinits == 3 && daemon_usb_wait_state() == USB_WAIT_READY);

    /* N1: the grace counts from the loss itself. */
    setup();
    daemon_usb_wait_on_loss();                  /* stamped at 1000 */
    F.now += 400;
    daemon_usb_wait_tick();                     /* first tick only 400 ms later */
    expect("N1: not yet at 400 ms", F.present_calls == 0);
    F.now += 100;
    daemon_usb_wait_tick();
    expect("N1: classified 500 ms after the loss, not after the first tick", F.present_calls == 1);
}

static void test_r2_cases(void)
{
    /* T12: LEFT and ARRIVED handled in the same dispatch. */
    setup();
    daemon_usb_wait_on_loss();
    F.left = true;
    F.arrivals = 1;
    daemon_usb_wait_tick();
    expect("T12: LEFT + ARRIVED in one dispatch: LEFT decides gone", daemon_usb_wait_state() == USB_WAIT_TEARDOWN &&
           F.present_calls == 0 && F.arrivals == 1);
    daemon_usb_wait_tick();
    daemon_usb_wait_tick();
    expect("T12: ... then the ARRIVED is acted on, as an arrival", F.arrivals == 0 && F.reopen_begins == 1 &&
           F.last_arrival && daemon_usb_wait_state() == USB_WAIT_READY);

    /* B1 with the arrival kept: the selection after the teardown is told it is
     * an arrival, so a Rule B walk settles on it (daemon_ch341_select). */
    setup();
    F.left = true;
    F.arrivals = 1;
    daemon_usb_wait_tick();
    daemon_usb_wait_on_loss();
    daemon_usb_wait_tick();
    daemon_usb_wait_tick();
    daemon_usb_wait_tick();
    expect("B1/F1: the kept ARRIVED reaches the selection as an arrival (settle not bypassed)",
           F.reopen_begins == 1 && F.last_arrival);

    /* T13: socket traffic before the deadline wakes the loop; nothing acts early. */
    setup();
    daemon_usb_wait_on_loss();                  /* stamped at 1000 */
    for (int i = 0; i < 10; i++) {
        F.now = 1000 + (uint64_t)i * 49;        /* ten wakeups inside the grace */
        daemon_usb_wait_tick();
    }
    expect("T13: ten early wakeups (socket traffic) inside the grace: nothing classified",
           F.present_calls == 0 && F.fatals == 0 && daemon_usb_wait_state() == USB_WAIT_PENDING);

    /* T13: a resolved loss cancels its deadline; the next loss has its own. */
    setup();
    daemon_usb_wait_on_loss();
    F.left = true;
    daemon_usb_wait_tick();
    daemon_usb_wait_tick();
    daemon_usb_wait_tick();                     /* forced reopen -> READY */
    expect("T13: resolved -> READY", daemon_usb_wait_state() == USB_WAIT_READY);
    F.left = false;
    F.now = 10000;
    daemon_usb_wait_on_loss();                  /* a new loss, stamped at 10000 */
    F.now = 10499;
    daemon_usb_wait_tick();
    expect("T13: the new loss is not classified on the old deadline", F.present_calls == 0);
    F.now = 10500;
    daemon_usb_wait_tick();
    expect("T13: ... but 500 ms after its own stamp", F.present_calls == 1);
}

static void test_r4_arrivals_during_selection(void)
{
    /* F1 (r2): an ARRIVED during a running (stepwise) selection is fed to it. */
    setup();
    daemon_usb_wait_on_loss();
    F.left = true;
    daemon_usb_wait_tick();
    daemon_usb_wait_tick();                     /* UNPLUGGED */
    F.reopen_results[0] = 0;
    F.reopen_results[1] = 0;
    F.reopen_results[2] = 2;
    F.reopen_n = 3;
    F.arrivals = 1;
    daemon_usb_wait_tick();                     /* begins, step 1 pending */
    expect("F1: the first ARRIVED is fed and begins the selection", F.arrivals_fed == 1 && F.reopen_begins == 1);
    F.arrivals = 2;                             /* two more during the scan */
    daemon_usb_wait_tick();
    expect("F1: ARRIVEDs during the running selection are fed to it, no second begin",
           F.arrivals_fed == 3 && F.reopen_begins == 1 && F.arrivals == 0);
}

/* r23 (gate-2 r1 F1): a loss latched by the NEW HAL inside the re-init is
 * classified like any loss, with its own grace, never erased into READY (the
 * latched HAL never reports again, so READY would wait for nothing). */
static void test_r23_loss_during_reinit(void)
{
    setup();
    F.now = 1000;
    daemon_usb_wait_on_loss();                  /* the first loss, stamped 1000 */
    F.left = true;
    daemon_usb_wait_tick();
    daemon_usb_wait_tick();
    expect("r23: gone and torn down", daemon_usb_wait_state() == USB_WAIT_UNPLUGGED);
    F.left = false;
    F.reopen_results[0] = 1;                    /* the stick is back, selected */
    F.reopen_n = 1;
    F.loss_in_reinit = true;
    F.loss_in_reinit_at = 60000;                /* lost again during its re-init */
    daemon_usb_wait_tick();
    expect("r23: the re-init ran once", F.reinits == 1);
    expect("r23: health UNPLUGGED, not READY (the new loss)", F.health == RADIO_HEALTH_UNPLUGGED);
    F.now = 60000 + DAEMON_USB_WAIT_GRACE_MS - 1;
    daemon_usb_wait_tick();
    expect("r23: the new loss is being classified (PENDING), not READY",
           daemon_usb_wait_state() == USB_WAIT_PENDING);
    expect("r23: its grace counts from the new loss, not the first (no presence check yet)",
           F.present_calls == 0 && F.fatals == 0);
    F.left = true;                              /* its LEFT */
    daemon_usb_wait_tick();
    expect("r23: the LEFT decides gone -> TEARDOWN", daemon_usb_wait_state() == USB_WAIT_TEARDOWN);
    daemon_usb_wait_tick();
    expect("r23: torn down again, waiting", daemon_usb_wait_state() == USB_WAIT_UNPLUGGED && F.fatals == 0);
}

static void test_r12_left(void)
{
    /* Core r8 B2: a LEFT of another stick while waiting (a duplicate gone, the
     * wanted stick now unique) begins a reopen like an ARRIVED and is fed to
     * the selection; while READY such LEFTs are drained, nothing begins. */
    setup();
    F.lefts = 3;
    daemon_usb_wait_tick();
    expect("r12 B2 READY: LEFTs of other sticks drained, nothing begun", F.lefts == 0 && F.reopen_begins == 0 &&
           daemon_usb_wait_state() == USB_WAIT_READY);
    setup();
    daemon_usb_wait_on_loss();
    F.left = true;
    daemon_usb_wait_tick();
    daemon_usb_wait_tick();                     /* UNPLUGGED */
    F.reopen_results[0] = 2;                    /* the first selection waits (e.g. "not unique") */
    F.reopen_n = 1;
    daemon_usb_wait_tick();                     /* the forced selection, waits */
    daemon_usb_wait_tick();
    expect("r12 B2 waiting: nothing due, no event -> no new reopen", F.reopen_begins == 1);
    F.lefts = 1;                                /* the duplicate is unplugged */
    daemon_usb_wait_tick();
    expect("r12 B2 a LEFT of another stick while waiting begins a reopen and is fed to it",
           F.reopen_begins == 2 && F.lefts_fed == 1 && F.lefts == 0);
    expect("r12 B2 ... the reopen opens -> READY", daemon_usb_wait_state() == USB_WAIT_READY);
}

/* ---- --radio-error retry ------------------------------------------------ */

/* The state machine's log lines (stdout) between out_begin() and out_end(). */
static int g_saved_stdout = -1;
static char g_out_path[512];
static void out_begin(void)
{
    const char *t = getenv("TMPDIR");
    snprintf(g_out_path, sizeof(g_out_path), "%s/usbwait-out.XXXXXX", t && t[0] ? t : "/tmp");
    int fd = mkstemp(g_out_path);
    fflush(stdout);
    g_saved_stdout = dup(STDOUT_FILENO);
    dup2(fd, STDOUT_FILENO);
    close(fd);
}
static std::string out_end(void)
{
    fflush(stdout);
    dup2(g_saved_stdout, STDOUT_FILENO);
    close(g_saved_stdout);
    std::string out;
    FILE *f = fopen(g_out_path, "r");
    char b[512];
    size_t n;
    while (f && (n = fread(b, 1, sizeof(b), f)) > 0)
        out.append(b, n);
    if (f)
        fclose(f);
    unlink(g_out_path);
    /* The checks made meanwhile wrote their result lines into the capture too:
     * print them again, so a [FAIL] is never hidden. */
    for (size_t at = 0; at < out.size();) {
        size_t nl = out.find('\n', at);
        std::string line = out.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        if (line.rfind("[ OK ]", 0) == 0 || line.rfind("[FAIL]", 0) == 0)
            printf("%s\n", line.c_str());
        at = nl == std::string::npos ? out.size() : nl + 1;
    }
    return out;
}
static bool has(const std::string &out, const char *line) { return out.find(line) != std::string::npos; }

/* A present-path fault from READY: the grace, the presence check, the
 * teardown; ends UNPLUGGED at F.now (the backoff counts from here). */
static void fault_to_unplugged(void)
{
    daemon_usb_wait_on_loss();
    F.health = RADIO_HEALTH_FAILED;             /* a failed CONF apply's fail-closed, as on the binary */
    daemon_usb_wait_tick();                     /* PENDING, before the deadline */
    F.now += DAEMON_USB_WAIT_GRACE_MS;
    daemon_usb_wait_tick();                     /* present: the fault */
    daemon_usb_wait_tick();                     /* the teardown */
}

/* From F.now: no selection at +d-1 ms, one at +d ms (F.now ends at +d). */
static bool begins_after(uint64_t d)
{
    int b = F.reopen_begins;
    uint64_t t0 = F.now;
    F.now = t0 + d - 1;
    daemon_usb_wait_tick();
    if (F.reopen_begins != b)
        return false;
    F.now = t0 + d;
    daemon_usb_wait_tick();
    return F.reopen_begins == b + 1;
}

static void test_retry_fault_backoff(void)
{
    setup_mode(true);
    F.step_default = 2;                         /* every selection: not opened (wait) */
    out_begin();
    fault_to_unplugged();
    expect("T1 retry: a present-path fault: no exit, torn down, UNPLUGGED",
           F.fatals == 0 && F.teardowns == 1 && daemon_usb_wait_state() == USB_WAIT_UNPLUGGED &&
           F.health == RADIO_HEALTH_UNPLUGGED);
    expect("T1 retry: the fault is counted once", F.faults == 1);
    daemon_usb_wait_tick();
    expect("T1b(a) no forced selection after a fault teardown", F.reopen_begins == 0);
    F.retry_at = F.now;                         /* the selection's own retry is due */
    F.now += 999;
    daemon_usb_wait_tick();
    expect("T1b(a)/T2c a due selection retry still waits for the 1 s step", F.reopen_begins == 0);
    F.now -= 999;
    F.retry_at = 0;
    expect("T2/T2c retry time 0: the first selection at the 1 s step", begins_after(1000));
    /* 2 ... 1024 s, the selection's retry at 0 or its 200 ms BUSY period. */
    bool steps = true;
    for (uint64_t d = 2000; d <= 1024000; d *= 2) {
        F.retry_at = (d / 1000) % 2 ? 0 : F.now + 200;
        steps = steps && begins_after(d);
    }
    expect("T2/T2c then 2, 4 ... 1024 s, a 200 ms BUSY retry on the step too", steps);
    F.retry_at = 0;
    expect("T2 then 1800 s", begins_after(1800000));
    expect("T2 then 1800 s again (forever)", begins_after(1800000));
    F.retry_at = F.now + 1800000 + 500;         /* the selection's retry after the step */
    expect("T2c a retry time past the step: the later of the two", begins_after(1800000 + 500));
    F.retry_at = 0;
    F.now += 60000;
    F.arrivals = 1;
    int b = F.reopen_begins;
    daemon_usb_wait_tick();
    expect("T2d an ARRIVED in the 30 min wait: a selection at once, as an arrival",
           F.reopen_begins == b + 1 && F.last_arrival);
    expect("T2 no exit through all of it", F.fatals == 0 && F.faults == 1);
    F.step_default = 1;
    expect("T1 then opened: re-init, READY", begins_after(1800000) &&
           daemon_usb_wait_state() == USB_WAIT_READY && F.health == RADIO_HEALTH_READY);
    std::string out = out_end();
    expect("T1 log: the fault line", has(out, "[CH341] TEST STICK: USB I/O error with the stick present: closing it, "
                                              "retry in 1 s (fault 1)\n"));
    expect("T1 log: attempt 1 not opened, next in 2 s; attempt 11 next in 1800 s",
           has(out, "[CH341] TEST STICK: attempt 1: not opened, next attempt in 2 s\n") &&
           has(out, "[CH341] TEST STICK: attempt 11: not opened, next attempt in 1800 s\n"));
    expect("T1 log: back after 16 attempts", has(out, "[CH341] TEST STICK back after 16 attempts: RADIO=READY\n"));
}

static void test_retry_plain_and_f1(void)
{
    /* (b) a plain LEFT in retry mode: the selection at once. */
    setup_mode(true);
    daemon_usb_wait_on_loss();
    F.left = true;
    daemon_usb_wait_tick();
    daemon_usb_wait_tick();
    F.left = false;
    daemon_usb_wait_tick();
    expect("T1b(b) retry mode, a plain LEFT: the first selection at once", F.reopen_begins == 1 &&
           daemon_usb_wait_state() == USB_WAIT_READY && F.faults == 0);

    /* (c) a fault, recovered with no USB event; then a plain loss (absent at
     * the deadline, no LEFT): forced, at once. */
    setup_mode(true);
    fault_to_unplugged();
    expect("T1b(c) fault recovered on the timer", begins_after(1000) &&
           daemon_usb_wait_state() == USB_WAIT_READY);
    F.now += 1000;
    F.present = 0;
    daemon_usb_wait_on_loss();
    daemon_usb_wait_tick();
    F.now += DAEMON_USB_WAIT_GRACE_MS;
    daemon_usb_wait_tick();                     /* absent: gone */
    daemon_usb_wait_tick();                     /* the teardown */
    int b = F.reopen_begins;
    daemon_usb_wait_tick();
    expect("T1b(c) the later plain loss: the forced selection at once (no fault carried over)",
           F.reopen_begins == b + 1 && F.faults == 1);
}

static void test_retry_step_carry_and_reset(void)
{
    setup_mode(true);
    fault_to_unplugged();
    expect("T2b fault 1: recovered at 1 s", begins_after(1000) && daemon_usb_wait_state() == USB_WAIT_READY);
    F.now += DAEMON_USB_WAIT_STABLE_MS - 1;     /* READY one ms short of 10 min */
    daemon_usb_wait_tick();
    out_begin();
    fault_to_unplugged();
    expect("T2b fault 2 inside 10 min: the step carries on (2 s)", begins_after(2000));
    std::string out = out_end();
    expect("T2b log: fault 2, retry in 2 s; its attempts counted from 1 again",
           has(out, "retry in 2 s (fault 2)\n") && has(out, "[CH341] TEST STICK back after 1 attempts: RADIO=READY\n"));
    F.now += DAEMON_USB_WAIT_STABLE_MS;         /* READY 10 min */
    daemon_usb_wait_tick();
    fault_to_unplugged();
    expect("T2b fault 3 after 10 min of READY: back to 1 s", begins_after(1000) && F.faults == 3);
}

static void test_retry_reinit_fails(void)
{
    setup_mode(true);
    F.reinit_fail_n = 3;
    out_begin();
    daemon_usb_wait_on_loss();
    F.left = true;
    daemon_usb_wait_tick();
    daemon_usb_wait_tick();                     /* torn down */
    F.left = false;
    daemon_usb_wait_tick();                     /* the forced selection: opened, the re-init fails */
    expect("T3 a plain unplug: the first selection at once; the failed re-init: no exit, UNPLUGGED, closed again",
           F.reopen_begins == 1 && F.reinits == 1 && F.fatals == 0 && F.health == RADIO_HEALTH_UNPLUGGED &&
           daemon_usb_wait_state() == USB_WAIT_TEARDOWN);
    daemon_usb_wait_tick();                     /* the teardown */
    expect("T3 torn down again, no immediate selection", F.teardowns == 2 &&
           (daemon_usb_wait_tick(), F.reopen_begins == 1));
    expect("T3 the re-opens follow 1 s", begins_after(1000) && F.reinits == 2);
    daemon_usb_wait_tick();
    expect("T3 ... 2 s", begins_after(2000) && F.reinits == 3);
    daemon_usb_wait_tick();
    expect("T3 ... 4 s, then READY", begins_after(4000) && F.reinits == 4 &&
           daemon_usb_wait_state() == USB_WAIT_READY && F.health == RADIO_HEALTH_READY && F.fatals == 0);
    expect("T3 a re-init failure is not a present-path fault (USBFAULT)", F.faults == 0);
    std::string out = out_end();
    expect("T3 log: attempt 1 re-init failed, next in 1 s ... attempt 3, next in 4 s; back after 4 attempts",
           has(out, "[CH341] TEST STICK: attempt 1: re-init failed: closing it, next attempt in 1 s\n") &&
           has(out, "[CH341] TEST STICK: attempt 3: re-init failed: closing it, next attempt in 4 s\n") &&
           has(out, "[CH341] TEST STICK back after 4 attempts: RADIO=READY\n"));
}

static void test_retry_kept_handle(void)
{
    setup_mode(true);
    F.kept = true;
    out_begin();
    fault_to_unplugged();
    F.now += 1000;
    daemon_usb_wait_tick();
    F.now += 2000;
    daemon_usb_wait_tick();
    F.arrivals = 1;
    daemon_usb_wait_tick();
    F.now += 1800000;
    daemon_usb_wait_tick();
    std::string out = out_end();
    expect("T6 a refused close: never a new selection (steps, an ARRIVED), no exit",
           F.reopen_begins == 0 && F.fatals == 0 && daemon_usb_wait_state() == USB_WAIT_UNPLUGGED);
    expect("T6 log: each due attempt names the kept handle (1, 2, 3)",
           has(out, "[CH341] TEST STICK: attempt 1: the library kept the old handle: restart the daemon "
                    "(next attempt in 2 s)\n") &&
           has(out, "attempt 3: the library kept the old handle: restart the daemon (next attempt in 8 s)\n"));

    /* A refused close after a plain LEFT waits the same way in retry mode. */
    setup_mode(true);
    F.kept = true;
    daemon_usb_wait_on_loss();
    F.left = true;
    daemon_usb_wait_tick();
    daemon_usb_wait_tick();
    F.now += 1800000;
    daemon_usb_wait_tick();
    expect("T6 a refused close after a plain LEFT: no selection either", F.reopen_begins == 0);

    /* The exit mode does not look at it (1.4.1). */
    setup_mode(false);
    F.kept = true;
    daemon_usb_wait_on_loss();
    F.left = true;
    daemon_usb_wait_tick();
    daemon_usb_wait_tick();
    daemon_usb_wait_tick();
    expect("O3 exit mode: the forced selection as 1.4.1", F.reopen_begins == 1);
}

int main(void)
{
    test_t1_left_unplugged();
    test_t2_deadline();
    test_t12_arrival_during_teardown();
    test_t6_reinit_fails();
    test_reopen_steps();
    test_w12_fixes();
    test_r2_cases();
    test_r4_arrivals_during_selection();
    test_r12_left();
    test_r23_loss_during_reinit();
    test_retry_fault_backoff();
    test_retry_plain_and_f1();
    test_retry_step_carry_and_reset();
    test_retry_reinit_fails();
    test_retry_kept_handle();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
