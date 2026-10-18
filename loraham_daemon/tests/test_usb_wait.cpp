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
 * The real exit 5 and the sockets are proven on the real binary
 * (test_ch341_process).
 */

#include "../daemon_usb_wait.h"

#include <cstdio>
#include <cstring>

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
    RadioHealth health;
    int fatals;
    char fatal_why[160];
    int reopen_begins;
    bool last_arrival;      /* reopen_begin's argument */
    int arrivals_fed;       /* reopen_arrival calls */
    int reopen_steps;
    int reopen_results[8];   /* per step: 0 pending, 1 opened, 2 wait */
    int reopen_n;
    bool due;
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
    if (F.reinit_busy > 0) {
        F.reinit_busy--;
        return -1;
    }
    if (F.reinit_ok)
        F.health = RADIO_HEALTH_READY;
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
    return i < F.reopen_n ? F.reopen_results[i] : 1;
}
static bool f_reopen_due(void) { return F.due; }

static void setup(void)
{
    memset(&F, 0, sizeof(F));
    F.now = 1000;
    F.present = 1;
    F.teardown_ok = true;
    F.reinit_ok = true;
    F.health = RADIO_HEALTH_READY;

    DaemonUsbWaitOps ops{};
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
    ops.reopen_due = f_reopen_due;
    ops.set_health = f_set_health;
    ops.fatal = f_fatal;
    ops.now_ms = f_now;
    daemon_usb_wait_reset();
    daemon_usb_wait_init(&ops);
}

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
           strstr(F.fatal_why, "I/O error with the Meshstick present") != nullptr);

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
    F.due = true;                               /* the BUSY retry period passed */
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

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
