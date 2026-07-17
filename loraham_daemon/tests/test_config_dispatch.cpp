#include "../config_dispatch.h"

#include <stdio.h>
#include <atomic>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * CONFIG dispatch tests.
 *
 * These lock the socket-ready/read/apply/restart path before the main loop
 * is split further for the event backend.
 */

static int g_ok = 0;
static int g_fail = 0;

/* --- Fake radio --- */

// Fake-Treiber: überschreibt die virtuellen RadioDriver-Delegates und zählt
// Aufrufe wie der frühere Template-Fake (Zähler-Semantik unverändert).
struct FakeRadio : public RadioDriver {
    int callback_count;
    int start_receive_count;
    int scan_result;
    int scan_count;
    float rssi;
    void (*last_callback)(void);

    FakeRadio() : RadioDriver(NULL),
                  callback_count(0), start_receive_count(0),
                  scan_result(0), scan_count(0), rssi(-82.5f),
                  last_callback(NULL) {}

    void setPacketReceivedAction(void (*cb)(void)) override
    {
        last_callback = cb;
        callback_count++;
    }

    int16_t startReceive() override
    {
        start_receive_count++;
        return 0;
    }

    int16_t clearIrq(uint32_t) override
    {
        return 0;
    }

    int16_t scanChannel() override
    {
        scan_count++;
        return scan_result;
    }

    float getRSSI() override
    {
        return rssi;
    }

    float rssiProbe() override
    {
        return rssi;
    }

    int16_t begin(const RadioRfDefaults *) override { return 0; }
    int16_t switchMode(RadioMode_t,
                       const RadioRfDefaults *) override { return 0; }
    int16_t applyLoraParam(const char *, const std::string &,
                           const std::string &) override { return 0; }
    int16_t applyFskParam(const char *, const std::string &,
                          const std::string &) override { return 0; }
    // Kein Module hinter dem Fake: Live-RSSI unavailable wie zuvor (-200).
    float readLiveRssi(RadioMode_t, bool) override { return -200.0f; }
    const char *chipName() const override { return "FAKE"; }
    DaemonChipFamily chipFamily() const override
    {
        return DAEMON_CHIP_FAMILY_SX127X;
    }
};

static FakeRadio *fake(RadioController *ctrl)
{
    return static_cast<FakeRadio *>(ctrl->driver.get());
}

/* --- Test state --- */

typedef struct {
    int calls;
    char tag[32];
    char cmd[128];
    /* 0 (memset default) maps to CONFIG_APPLY_APPLIED so the existing
     * re-arm assertions keep exercising the radio-touched path. */
    int result_override; /* 0=APPLIED, else the ConfigApplyStatus to return */
} ApplyState;

static ApplyState g_apply_state;


/* --- Async TX runtime status stubs -------------------------------------- */

static size_t g_stub_pending = 0;
static int g_stub_job_active = 0;

size_t daemon_tx_async_runtime_pending()
{
    return g_stub_pending;
}

int daemon_tx_async_runtime_job_active()
{
    return g_stub_job_active;
}

size_t daemon_tx_async_runtime_dropped()
{
    return 0;
}

size_t daemon_tx_async_runtime_rejected()
{
    return 0;
}

size_t daemon_tx_async_runtime_processed()
{
    return 0;
}

size_t daemon_tx_async_runtime_completion_stale()
{
    return 0;
}


size_t daemon_tx_async_runtime_completion_dropped()
{
    return 0;
}

int daemon_tx_async_runtime_last_result(DaemonTxJobResult *out)
{
    (void)out;
    return 0;
}

static void fake_rx_callback(void)
{
}

/* --- Test helpers --- */

static void expect_int(const char *name, int actual, int expected)
{
    if (actual == expected) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected %d, got %d\n", name, expected, actual);
    }
}

static void expect_size(const char *name, size_t actual, size_t expected)
{
    if (actual == expected) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected %zu, got %zu\n", name, expected, actual);
    }
}

static void expect_str(const char *name, const char *actual, const char *expected)
{
    if (strcmp(actual, expected) == 0) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected '%s', got '%s'\n", name, expected, actual);
    }
}

static void expect_contains(const char *name, const char *actual, const char *needle)
{
    if (actual && needle && strstr(actual, needle)) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected substring '%s', got '%s'\n",
               name, needle ? needle : "", actual ? actual : "");
    }
}

static int make_socket_pair(int sv[2])
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        perror("socketpair");
        return 0;
    }

    return 1;
}

/* --- Apply callback used by tests --- */

static ConfigApplyStatus record_apply_config(RadioDriver& radio,
                                             const char *tag,
                                             const char *cmd,
                                             RadioMode_t& mode,
                                             std::atomic<bool>& getrssi_active)
{
    (void)radio;

    g_apply_state.calls++;
    snprintf(g_apply_state.tag, sizeof(g_apply_state.tag), "%s", tag);
    snprintf(g_apply_state.cmd, sizeof(g_apply_state.cmd), "%s", cmd);

    mode = RADIO_MODE_FSK;
    getrssi_active.store(true);

    if (g_apply_state.result_override != 0)
        return (ConfigApplyStatus)g_apply_state.result_override;

    return CONFIG_APPLY_APPLIED;
}

static void init_fake_controller(RadioController *ctrl,
                                 RadioHealth health)
{
    radio_controller_init(ctrl,
                          RADIO_BAND_433,
                          "TEST",
                          false,
                          fake_rx_callback,
                          13);
    ctrl->driver.reset(new FakeRadio());
    ctrl->health = health;
}

static ConfigDispatchContext make_context(ClientSlot *slots,
                                          RadioController *ctrl)
{
    ConfigDispatchLog log = {
        NULL,
        NULL,
        NULL
    };

    ConfigDispatchContext ctx = {
        slots,
        ctrl,
        "CONF TEST",
        record_apply_config,
        log
    };

    return ctx;
}

/* --- Dispatch behavior --- */

static void test_dispatch_ready_client(void)
{
    int sv[2];
    ClientSlot slots[2];
    uint8_t buf[buf_SIZE];
    EventLoopSet set;
    EventLoopReadySet readfds;
    RadioController ctrl;
    init_fake_controller(&ctrl, RADIO_HEALTH_READY);

    memset(&g_apply_state, 0, sizeof(g_apply_state));
    client_slot_init_all(slots, 2);
    memset(buf, 0, sizeof(buf));

    if (!make_socket_pair(sv)) {
        g_fail++;
        return;
    }

    client_slot_set_fd(&slots[0], sv[1]);

    if (event_loop_init(&set) != 0) {
        close(sv[0]);
        client_slot_close(&slots[0]);
        g_fail++;
        printf("[FAIL] config dispatch init\n");
        return;
    }

    const char *cmd = "SET GETRSSI=1\n";
    write(sv[0], cmd, strlen(cmd));

    event_loop_reset(&set);
    event_loop_add_fd(&set, sv[1]);
    expect_int("ready wait", event_loop_wait(&set, &readfds, 100000), 1);

    ConfigDispatchContext ctx =
        make_context(slots, &ctrl);

    config_dispatch_context(&ctx, 2, &readfds, buf);

    expect_int("apply called once", g_apply_state.calls, 1);
    expect_str("apply tag", g_apply_state.tag, "CONF TEST");
    expect_str("apply cmd", g_apply_state.cmd, "SET GETRSSI=1");
    expect_int("mode updated by apply", ctrl.mode == RADIO_MODE_FSK, 1);
    expect_int("getrssi updated by apply", ctrl.getrssi_active.load() == true, 1);
    expect_int("callback restored", fake(&ctrl)->callback_count, 1);
    expect_int("startReceive called", fake(&ctrl)->start_receive_count, 1);
    expect_int("client still open", client_slot_has_client(&slots[0]), 1);

    event_loop_close(&set);
    close(sv[0]);
    client_slot_close(&slots[0]);
}


static void test_dispatch_ready_client_epoll(void)
{
    int sv[2];
    ClientSlot slots[2];
    uint8_t buf[buf_SIZE];
    EventLoopSet set;
    EventLoopReadySet readfds;
    RadioController ctrl;
    init_fake_controller(&ctrl, RADIO_HEALTH_READY);

    memset(&g_apply_state, 0, sizeof(g_apply_state));
    client_slot_init_all(slots, 2);
    memset(buf, 0, sizeof(buf));

    if (!make_socket_pair(sv)) {
        g_fail++;
        return;
    }

    client_slot_set_fd(&slots[0], sv[1]);

    if (event_loop_init(&set) != 0) {
        close(sv[0]);
        client_slot_close(&slots[0]);
        g_fail++;
        printf("[FAIL] config epoll init\n");
        return;
    }

    const char *cmd = "SET GETRSSI=1\n";
    write(sv[0], cmd, strlen(cmd));

    event_loop_add_fd(&set, sv[1]);
    expect_int("ready epoll wait", event_loop_wait(&set, &readfds, 100000), 1);

    ConfigDispatchContext ctx =
        make_context(slots, &ctrl);

    config_dispatch_context(&ctx, 2, &readfds, buf);

    expect_int("epoll apply called once", g_apply_state.calls, 1);
    expect_str("epoll apply cmd", g_apply_state.cmd, "SET GETRSSI=1");
    expect_int("epoll callback restored", fake(&ctrl)->callback_count, 1);
    expect_int("epoll startReceive called", fake(&ctrl)->start_receive_count, 1);

    event_loop_close(&set);
    close(sv[0]);
    client_slot_close(&slots[0]);
}

static void test_dispatch_ignores_not_ready_client(void)
{
    int sv[2];
    ClientSlot slots[2];
    uint8_t buf[buf_SIZE];
    EventLoopSet set;
    EventLoopReadySet readfds;
    RadioController ctrl;
    init_fake_controller(&ctrl, RADIO_HEALTH_FAILED);

    memset(&g_apply_state, 0, sizeof(g_apply_state));
    client_slot_init_all(slots, 2);
    memset(buf, 0, sizeof(buf));

    if (!make_socket_pair(sv)) {
        g_fail++;
        return;
    }

    client_slot_set_fd(&slots[0], sv[1]);

    if (event_loop_init(&set) != 0) {
        close(sv[0]);
        client_slot_close(&slots[0]);
        g_fail++;
        printf("[FAIL] config dispatch not-ready init\n");
        return;
    }

    const char *cmd = "SET GETRSSI=1\n";
    write(sv[0], cmd, strlen(cmd));

    event_loop_reset(&set);
    event_loop_add_fd(&set, sv[1]);
    expect_int("not ready wait", event_loop_wait(&set, &readfds, 100000), 1);

    ConfigDispatchContext ctx =
        make_context(slots, &ctrl);

    config_dispatch_context(&ctx, 2, &readfds, buf);

    expect_int("not ready apply count", g_apply_state.calls, 0);
    expect_int("not ready callback count", fake(&ctrl)->callback_count, 0);
    expect_int("not ready startReceive count", fake(&ctrl)->start_receive_count, 0);
    expect_int("not ready client open", client_slot_has_client(&slots[0]), 1);

    event_loop_close(&set);
    close(sv[0]);
    client_slot_close(&slots[0]);
}



static void test_status_uses_cad_broadcast_latch(void)
{
    RadioController ctrl;
    char status[384];

    init_fake_controller(&ctrl, RADIO_HEALTH_READY);

    ctrl.cad_active.store(true);
    ctrl.cad_broadcast_active.store(false);
    config_status_format(status, sizeof(status), &ctrl);
    expect_contains("status ignores transient CAD", status, " CAD=0 ");

    ctrl.cad_active.store(false);
    ctrl.cad_broadcast_active.store(true);
    config_status_format(status, sizeof(status), &ctrl);
    expect_contains("status reports broadcast CAD", status, " CAD=1 ");
    expect_contains("status reports queue rejects", status, " TXQREJECT=0 ");
}

static void test_dispatch_get_channel_restores_rx(void)
{
    int sv[2];
    ClientSlot slots[2];
    uint8_t buf[buf_SIZE];
    EventLoopSet set;
    EventLoopReadySet readfds;
    RadioController ctrl;
    char out[256];
    ssize_t n;

    init_fake_controller(&ctrl, RADIO_HEALTH_READY);
    fake(&ctrl)->scan_result = 1;
    fake(&ctrl)->rssi = -77.25f;

    memset(&g_apply_state, 0, sizeof(g_apply_state));
    client_slot_init_all(slots, 2);
    memset(buf, 0, sizeof(buf));
    memset(out, 0, sizeof(out));

    if (!make_socket_pair(sv)) {
        g_fail++;
        return;
    }

    client_slot_set_fd(&slots[0], sv[1]);

    if (event_loop_init(&set) != 0) {
        close(sv[0]);
        client_slot_close(&slots[0]);
        g_fail++;
        printf("[FAIL] config GET CHANNEL init\n");
        return;
    }

    const char *cmd = "GET CHANNEL\n";
    write(sv[0], cmd, strlen(cmd));

    event_loop_reset(&set);
    event_loop_add_fd(&set, sv[1]);
    expect_int("get channel wait", event_loop_wait(&set, &readfds, 100000), 1);

    ConfigDispatchContext ctx =
        make_context(slots, &ctrl);

    config_dispatch_context(&ctx, 2, &readfds, buf);

    n = read(sv[0], out, sizeof(out) - 1);
    if (n < 0)
        n = 0;
    out[n] = '\0';

    expect_int("get channel apply count", g_apply_state.calls, 0);
    expect_int("get channel scan count", fake(&ctrl)->scan_count, 1);
    expect_contains("get channel prefix", out, "CHANNEL RADIO=READY");
    expect_contains("get channel busy", out, "BUSY=1");
    expect_contains("get channel cad", out, "CAD=1");
    expect_contains("get channel cadscan", out, "CADSCAN=1");
    expect_contains("get channel cadstate", out, "CADSTATE=BUSY");
    expect_contains("get channel rssi", out, "RSSI=-77.25");
    expect_contains("get channel packet rssi", out, "PACKETRSSI=-77.25");
    expect_contains("get channel live rssi", out, "LIVERSSI=-200.00");
    expect_contains("get channel mode", out, "MODE=LORA");
    expect_contains("get channel txmode", out, "TXMODE=MANAGED");
    expect_int("get channel callback restored", fake(&ctrl)->callback_count, 1);
    expect_int("get channel startReceive called", fake(&ctrl)->start_receive_count, 1);
    expect_int("get channel client open", client_slot_has_client(&slots[0]), 1);

    event_loop_close(&set);
    close(sv[0]);
    client_slot_close(&slots[0]);
}


static void test_dispatch_get_channel_during_tx_skips_scan(void)
{
    int sv[2];
    ClientSlot slots[2];
    uint8_t buf[buf_SIZE];
    EventLoopSet set;
    EventLoopReadySet readfds;
    RadioController ctrl;
    char out[256];
    ssize_t n;

    init_fake_controller(&ctrl, RADIO_HEALTH_READY);
    ctrl.tx_busy.store(true);

    memset(&g_apply_state, 0, sizeof(g_apply_state));
    client_slot_init_all(slots, 2);
    memset(buf, 0, sizeof(buf));
    memset(out, 0, sizeof(out));

    if (!make_socket_pair(sv)) {
        g_fail++;
        return;
    }

    client_slot_set_fd(&slots[0], sv[1]);

    if (event_loop_init(&set) != 0) {
        close(sv[0]);
        client_slot_close(&slots[0]);
        g_fail++;
        printf("[FAIL] config GET CHANNEL busy init\n");
        return;
    }

    write(sv[0], "GET CHANNEL\n", strlen("GET CHANNEL\n"));

    event_loop_reset(&set);
    event_loop_add_fd(&set, sv[1]);
    expect_int("get channel busy wait",
               event_loop_wait(&set, &readfds, 100000),
               1);

    ConfigDispatchContext ctx =
        make_context(slots, &ctrl);

    config_dispatch_context(&ctx, 2, &readfds, buf);

    n = read(sv[0], out, sizeof(out) - 1);
    if (n < 0)
        n = 0;
    out[n] = '\0';

    expect_int("get channel busy apply count", g_apply_state.calls, 0);
    expect_int("get channel busy no scan", fake(&ctrl)->scan_count, 0);
    expect_contains("get channel busy marker", out, "BUSY=1");
    expect_contains("get channel busy CAD marker", out, "CAD=0");
    expect_contains("get channel busy scan marker", out, "CADSCAN=0");
    expect_contains("get channel busy state", out, "CADSTATE=UNAVAILABLE");
    expect_int("get channel busy callback untouched",
               fake(&ctrl)->callback_count,
               0);
    expect_int("get channel busy RX untouched",
               fake(&ctrl)->start_receive_count,
               0);
    expect_int("get channel busy client open",
               client_slot_has_client(&slots[0]),
               1);

    event_loop_close(&set);
    close(sv[0]);
    client_slot_close(&slots[0]);
}

static void test_dispatch_set_txqueue(void)
{
    int sv[2];
    ClientSlot slots[2];
    uint8_t buf[buf_SIZE];
    EventLoopSet set;
    EventLoopReadySet readfds;
    RadioController ctrl;
    init_fake_controller(&ctrl, RADIO_HEALTH_READY);

    memset(&g_apply_state, 0, sizeof(g_apply_state));
    client_slot_init_all(slots, 2);
    memset(buf, 0, sizeof(buf));

    if (!make_socket_pair(sv)) {
        g_fail++;
        return;
    }

    client_slot_set_fd(&slots[0], sv[1]);

    if (event_loop_init(&set) != 0) {
        close(sv[0]);
        client_slot_close(&slots[0]);
        g_fail++;
        printf("[FAIL] config TXQUEUE init\n");
        return;
    }

    const char *cmd = "SET TXQUEUE=1\n";
    write(sv[0], cmd, strlen(cmd));

    event_loop_reset(&set);
    event_loop_add_fd(&set, sv[1]);
    expect_int("txqueue wait", event_loop_wait(&set, &readfds, 100000), 1);

    ConfigDispatchContext ctx =
        make_context(slots, &ctrl);

    config_dispatch_context(&ctx, 2, &readfds, buf);

    expect_int("txqueue apply not called", g_apply_state.calls, 0);
    expect_int("txqueue enabled", ctrl.tx_queue_active.load() == true, 1);
    expect_int("txqueue callback not restored", fake(&ctrl)->callback_count, 0);
    expect_int("txqueue startReceive not called", fake(&ctrl)->start_receive_count, 0);
    expect_int("txqueue client still open", client_slot_has_client(&slots[0]), 1);

    event_loop_close(&set);
    close(sv[0]);
    client_slot_close(&slots[0]);
}

static void test_dispatch_set_txresult(void)
{
    int sv[2];
    ClientSlot slots[2];
    uint8_t buf[buf_SIZE];
    EventLoopSet set;
    EventLoopReadySet readfds;
    RadioController ctrl;
    init_fake_controller(&ctrl, RADIO_HEALTH_READY);

    memset(&g_apply_state, 0, sizeof(g_apply_state));
    client_slot_init_all(slots, 2);
    memset(buf, 0, sizeof(buf));

    if (!make_socket_pair(sv)) {
        g_fail++;
        return;
    }

    client_slot_set_fd(&slots[0], sv[1]);

    if (event_loop_init(&set) != 0) {
        close(sv[0]);
        client_slot_close(&slots[0]);
        g_fail++;
        printf("[FAIL] config TXRESULT init\n");
        return;
    }

    const char *cmd = "SET TXRESULT=1\n";
    write(sv[0], cmd, strlen(cmd));

    event_loop_reset(&set);
    event_loop_add_fd(&set, sv[1]);
    expect_int("txresult wait", event_loop_wait(&set, &readfds, 100000), 1);

    ConfigDispatchContext ctx =
        make_context(slots, &ctrl);

    config_dispatch_context(&ctx, 2, &readfds, buf);

    expect_int("txresult apply not called", g_apply_state.calls, 0);
    expect_int("txresult enabled", ctrl.tx_result_active.load() == true, 1);
    expect_int("txresult callback not restored", fake(&ctrl)->callback_count, 0);
    expect_int("txresult startReceive not called", fake(&ctrl)->start_receive_count, 0);
    expect_int("txresult client still open", client_slot_has_client(&slots[0]), 1);

    event_loop_close(&set);
    close(sv[0]);
    client_slot_close(&slots[0]);
}


static void test_dispatch_sets_txmode_without_radio(void)
{
    int sv[2];
    ClientSlot slots[2];
    uint8_t buf[buf_SIZE];
    EventLoopSet set;
    EventLoopReadySet readfds;
    RadioController ctrl;
    init_fake_controller(&ctrl, RADIO_HEALTH_FAILED);

    memset(&g_apply_state, 0, sizeof(g_apply_state));
    client_slot_init_all(slots, 2);
    memset(buf, 0, sizeof(buf));

    if (!make_socket_pair(sv)) {
        g_fail++;
        return;
    }

    client_slot_set_fd(&slots[0], sv[1]);

    if (event_loop_init(&set) != 0) {
        close(sv[0]);
        client_slot_close(&slots[0]);
        g_fail++;
        printf("[FAIL] config dispatch txmode init\n");
        return;
    }

    const char *cmd = "SET TXMODE=DIRECT\n";
    write(sv[0], cmd, strlen(cmd));

    event_loop_reset(&set);
    event_loop_add_fd(&set, sv[1]);
    expect_int("txmode wait", event_loop_wait(&set, &readfds, 100000), 1);

    ConfigDispatchContext ctx =
        make_context(slots, &ctrl);

    config_dispatch_context(&ctx, 2, &readfds, buf);

    expect_int("txmode apply count", g_apply_state.calls, 0);
    expect_int("txmode direct", ctrl.tx_mode == RADIO_TX_MODE_DIRECT, 1);
    expect_int("txmode client open", client_slot_has_client(&slots[0]), 1);

    event_loop_close(&set);
    close(sv[0]);
    client_slot_close(&slots[0]);
}

static void test_get_channel_pending_rx_guard(void)
{
    RadioController ctrl;
    char ch[256];

    init_fake_controller(&ctrl, RADIO_HEALTH_READY);

    // Idle: GET CHANNEL runs the active scan.
    ctrl.received.store(false);
    config_status_format_channel(ch, sizeof(ch), &ctrl);
    expect_int("get channel scans when idle", fake(&ctrl)->scan_count >= 1 ? 1 : 0, 1);

    // Pending RX: must NOT scan (would discard the packet) and report PENDING.
    int before = fake(&ctrl)->scan_count;
    ctrl.received.store(true);
    config_status_format_channel(ch, sizeof(ch), &ctrl);
    expect_int("get channel pending no scan", fake(&ctrl)->scan_count, before);
    expect_int("get channel pending state",
               strstr(ch, "CADSTATE=PENDING") != NULL ? 1 : 0, 1);
    expect_int("get channel pending cadscan 0",
               strstr(ch, "CADSCAN=0") != NULL ? 1 : 0, 1);
}

static void test_set_cadrssi_parser(void)
{
    int dbm = 0;

    expect_int("cadrssi parse -85", config_status_is_set_cadrssi("SET CADRSSI=-85", &dbm), 1);
    expect_int("cadrssi value -85", dbm, -85);
    expect_int("cadrssi parse 0", config_status_is_set_cadrssi("SET CADRSSI=0", &dbm), 1);
    expect_int("cadrssi reject too low", config_status_is_set_cadrssi("SET CADRSSI=-131", &dbm), 0);
    expect_int("cadrssi reject positive", config_status_is_set_cadrssi("SET CADRSSI=5", &dbm), 0);
    expect_int("cadrssi reject junk", config_status_is_set_cadrssi("SET CADRSSI=-90x", &dbm), 0);
}

static void test_dispatch_sets_cadmonitor_optin(void)
{
    int sv[2];
    ClientSlot slots[2];
    uint8_t buf[buf_SIZE];
    EventLoopSet set;
    EventLoopReadySet readfds;
    RadioController ctrl;
    init_fake_controller(&ctrl, RADIO_HEALTH_FAILED);

    memset(&g_apply_state, 0, sizeof(g_apply_state));
    client_slot_init_all(slots, 2);
    memset(buf, 0, sizeof(buf));

    if (!make_socket_pair(sv)) {
        g_fail++;
        return;
    }

    client_slot_set_fd(&slots[0], sv[1]);

    if (event_loop_init(&set) != 0) {
        close(sv[0]);
        client_slot_close(&slots[0]);
        g_fail++;
        printf("[FAIL] config dispatch cadmonitor init\n");
        return;
    }

    ConfigDispatchContext ctx = make_context(slots, &ctrl);

    // Default: monitoring off.
    expect_int("cadmonitor default off", ctrl.cad_monitor_active.load() ? 1 : 0, 0);

    // SET CADMONITOR=1 enables the opt-in.
    const char *on = "SET CADMONITOR=1\n";
    write(sv[0], on, strlen(on));
    event_loop_reset(&set);
    event_loop_add_fd(&set, sv[1]);
    expect_int("cadmonitor on wait", event_loop_wait(&set, &readfds, 100000), 1);
    config_dispatch_context(&ctx, 2, &readfds, buf);
    expect_int("cadmonitor enabled", ctrl.cad_monitor_active.load() ? 1 : 0, 1);

    // SET CADMONITOR=0 disables it and resets the broadcast latch and the
    // free-confirmation streak.
    ctrl.cad_broadcast_active.store(true);
    ctrl.cad_monitor_free_streak.store(1);
    const char *off = "SET CADMONITOR=0\n";
    write(sv[0], off, strlen(off));
    event_loop_reset(&set);
    event_loop_add_fd(&set, sv[1]);
    expect_int("cadmonitor off wait", event_loop_wait(&set, &readfds, 100000), 1);
    config_dispatch_context(&ctx, 2, &readfds, buf);
    expect_int("cadmonitor disabled", ctrl.cad_monitor_active.load() ? 1 : 0, 0);
    expect_int("cadmonitor reset broadcast latch",
               ctrl.cad_broadcast_active.load() ? 1 : 0, 0);
    expect_int("cadmonitor reset free streak",
               ctrl.cad_monitor_free_streak.load(), 0);

    event_loop_close(&set);
    close(sv[0]);
    client_slot_close(&slots[0]);
}

static void test_dispatch_closes_eof_client(void)
{
    int sv[2];
    ClientSlot slots[2];
    uint8_t buf[buf_SIZE];
    EventLoopSet set;
    EventLoopReadySet readfds;
    RadioController ctrl;
    init_fake_controller(&ctrl, RADIO_HEALTH_READY);

    memset(&g_apply_state, 0, sizeof(g_apply_state));
    client_slot_init_all(slots, 2);
    memset(buf, 0, sizeof(buf));

    if (!make_socket_pair(sv)) {
        g_fail++;
        return;
    }

    client_slot_set_fd(&slots[0], sv[1]);
    client_output_queue_append(&slots[0].output, (const uint8_t *)"pending", 7);

    if (event_loop_init(&set) != 0) {
        close(sv[0]);
        client_slot_close(&slots[0]);
        g_fail++;
        printf("[FAIL] config dispatch eof init\n");
        return;
    }

    close(sv[0]);

    event_loop_reset(&set);
    event_loop_add_fd(&set, sv[1]);
    expect_int("eof wait", event_loop_wait(&set, &readfds, 100000), 1);

    ConfigDispatchContext ctx =
        make_context(slots, &ctrl);

    config_dispatch_context(&ctx, 2, &readfds, buf);

    expect_int("eof apply count", g_apply_state.calls, 0);
    expect_int("eof callback count", fake(&ctrl)->callback_count, 0);
    expect_int("eof startReceive count", fake(&ctrl)->start_receive_count, 0);
    expect_int("eof client closed", slots[0].fd, -1);
    expect_size("eof output reset", client_output_queue_pending(&slots[0].output), 0);
    expect_size("eof stream reset", slots[0].stream.len, 0);

    event_loop_close(&set);
}

static void test_dispatch_set_cadwait(void)
{
    int sv[2];
    ClientSlot slots[2];
    uint8_t buf[buf_SIZE];
    EventLoopSet set;
    EventLoopReadySet readfds;
    RadioController ctrl;
    init_fake_controller(&ctrl, RADIO_HEALTH_READY);

    memset(&g_apply_state, 0, sizeof(g_apply_state));
    client_slot_init_all(slots, 2);
    memset(buf, 0, sizeof(buf));

    if (!make_socket_pair(sv)) { g_fail++; return; }
    client_slot_set_fd(&slots[0], sv[1]);
    if (event_loop_init(&set) != 0) {
        close(sv[0]); client_slot_close(&slots[0]); g_fail++;
        printf("[FAIL] cadwait init\n"); return;
    }

    const char *cmd = "SET CADWAIT=300\n";
    write(sv[0], cmd, strlen(cmd));
    event_loop_reset(&set);
    event_loop_add_fd(&set, sv[1]);
    expect_int("cadwait wait", event_loop_wait(&set, &readfds, 100000), 1);

    ConfigDispatchContext ctx = make_context(slots, &ctrl);
    config_dispatch_context(&ctx, 2, &readfds, buf);

    expect_int("cadwait apply not called",  g_apply_state.calls, 0);
    expect_int("cadwait stored", (int)ctrl.cad_wait_timeout_ms.load(), 300);
    expect_int("cadwait callback not restored",  fake(&ctrl)->callback_count, 0);
    expect_int("cadwait startReceive not called", fake(&ctrl)->start_receive_count, 0);
    expect_int("cadwait client still open", client_slot_has_client(&slots[0]), 1);

    event_loop_close(&set);
    close(sv[0]);
    client_slot_close(&slots[0]);
}

static void test_dispatch_set_cadidle(void)
{
    int sv[2];
    ClientSlot slots[2];
    uint8_t buf[buf_SIZE];
    EventLoopSet set;
    EventLoopReadySet readfds;
    RadioController ctrl;
    init_fake_controller(&ctrl, RADIO_HEALTH_READY);

    memset(&g_apply_state, 0, sizeof(g_apply_state));
    client_slot_init_all(slots, 2);
    memset(buf, 0, sizeof(buf));

    if (!make_socket_pair(sv)) { g_fail++; return; }
    client_slot_set_fd(&slots[0], sv[1]);
    if (event_loop_init(&set) != 0) {
        close(sv[0]); client_slot_close(&slots[0]); g_fail++;
        printf("[FAIL] cadidle init\n"); return;
    }

    const char *cmd = "SET CADIDLE=100\n";
    write(sv[0], cmd, strlen(cmd));
    event_loop_reset(&set);
    event_loop_add_fd(&set, sv[1]);
    expect_int("cadidle wait", event_loop_wait(&set, &readfds, 100000), 1);

    ConfigDispatchContext ctx = make_context(slots, &ctrl);
    config_dispatch_context(&ctx, 2, &readfds, buf);

    expect_int("cadidle apply not called",  g_apply_state.calls, 0);
    expect_int("cadidle stored", (int)ctrl.cad_idle_stable_ms.load(), 100);
    expect_int("cadidle callback not restored",  fake(&ctrl)->callback_count, 0);
    expect_int("cadidle startReceive not called", fake(&ctrl)->start_receive_count, 0);
    expect_int("cadidle client still open", client_slot_has_client(&slots[0]), 1);

    event_loop_close(&set);
    close(sv[0]);
    client_slot_close(&slots[0]);
}

static void test_dispatch_set_cadpoll(void)
{
    int sv[2];
    ClientSlot slots[2];
    uint8_t buf[buf_SIZE];
    EventLoopSet set;
    EventLoopReadySet readfds;
    RadioController ctrl;
    init_fake_controller(&ctrl, RADIO_HEALTH_READY);

    memset(&g_apply_state, 0, sizeof(g_apply_state));
    client_slot_init_all(slots, 2);
    memset(buf, 0, sizeof(buf));

    if (!make_socket_pair(sv)) { g_fail++; return; }
    client_slot_set_fd(&slots[0], sv[1]);
    if (event_loop_init(&set) != 0) {
        close(sv[0]); client_slot_close(&slots[0]); g_fail++;
        printf("[FAIL] cadpoll init\n"); return;
    }

    const char *cmd = "SET CADPOLL=50\n";
    write(sv[0], cmd, strlen(cmd));
    event_loop_reset(&set);
    event_loop_add_fd(&set, sv[1]);
    expect_int("cadpoll wait", event_loop_wait(&set, &readfds, 100000), 1);

    ConfigDispatchContext ctx = make_context(slots, &ctrl);
    config_dispatch_context(&ctx, 2, &readfds, buf);

    expect_int("cadpoll apply not called",  g_apply_state.calls, 0);
    expect_int("cadpoll stored", (int)ctrl.cad_poll_interval_ms.load(), 50);
    expect_int("cadpoll callback not restored",  fake(&ctrl)->callback_count, 0);
    expect_int("cadpoll startReceive not called", fake(&ctrl)->start_receive_count, 0);
    expect_int("cadpoll client still open", client_slot_has_client(&slots[0]), 1);

    event_loop_close(&set);
    close(sv[0]);
    client_slot_close(&slots[0]);
}

static void test_dispatch_set_cadtxaftertimeout(void)
{
    int sv[2];
    ClientSlot slots[2];
    uint8_t buf[buf_SIZE];
    EventLoopSet set;
    EventLoopReadySet readfds;
    RadioController ctrl;
    init_fake_controller(&ctrl, RADIO_HEALTH_READY);

    memset(&g_apply_state, 0, sizeof(g_apply_state));
    client_slot_init_all(slots, 2);
    memset(buf, 0, sizeof(buf));

    if (!make_socket_pair(sv)) { g_fail++; return; }
    client_slot_set_fd(&slots[0], sv[1]);
    if (event_loop_init(&set) != 0) {
        close(sv[0]); client_slot_close(&slots[0]); g_fail++;
        printf("[FAIL] cadtx init\n"); return;
    }

    const char *cmd = "SET CADTXAFTERTIMEOUT=1\n";
    write(sv[0], cmd, strlen(cmd));
    event_loop_reset(&set);
    event_loop_add_fd(&set, sv[1]);
    expect_int("cadtx wait", event_loop_wait(&set, &readfds, 100000), 1);

    ConfigDispatchContext ctx = make_context(slots, &ctrl);
    config_dispatch_context(&ctx, 2, &readfds, buf);

    expect_int("cadtx apply not called",  g_apply_state.calls, 0);
    expect_int("cadtx stored", ctrl.cad_send_after_timeout.load() ? 1 : 0, 1);
    expect_int("cadtx callback not restored",  fake(&ctrl)->callback_count, 0);
    expect_int("cadtx startReceive not called", fake(&ctrl)->start_receive_count, 0);
    expect_int("cadtx client still open", client_slot_has_client(&slots[0]), 1);

    event_loop_close(&set);
    close(sv[0]);
    client_slot_close(&slots[0]);
}

static void test_dispatch_set_cadwait_invalid_rejected(void)
{
    int sv[2];
    ClientSlot slots[2];
    uint8_t buf[buf_SIZE];
    EventLoopSet set;
    EventLoopReadySet readfds;
    RadioController ctrl;
    init_fake_controller(&ctrl, RADIO_HEALTH_FAILED);

    memset(&g_apply_state, 0, sizeof(g_apply_state));
    client_slot_init_all(slots, 2);
    memset(buf, 0, sizeof(buf));

    if (!make_socket_pair(sv)) { g_fail++; return; }
    client_slot_set_fd(&slots[0], sv[1]);
    if (event_loop_init(&set) != 0) {
        close(sv[0]); client_slot_close(&slots[0]); g_fail++;
        printf("[FAIL] cadwait-invalid init\n"); return;
    }

    const char *cmd = "SET CADWAIT=1\n";
    write(sv[0], cmd, strlen(cmd));
    event_loop_reset(&set);
    event_loop_add_fd(&set, sv[1]);
    expect_int("cadwait-invalid wait", event_loop_wait(&set, &readfds, 100000), 1);

    ConfigDispatchContext ctx = make_context(slots, &ctrl);
    config_dispatch_context(&ctx, 2, &readfds, buf);

    expect_int("cadwait-invalid apply not called",  g_apply_state.calls, 0);
    expect_int("cadwait-invalid value unchanged",
               (int)ctrl.cad_wait_timeout_ms.load(),
               (int)DAEMON_TX_POLICY_CAD_WAIT_TIMEOUT_MS);
    expect_int("cadwait-invalid client still open", client_slot_has_client(&slots[0]), 1);

    event_loop_close(&set);
    close(sv[0]);
    client_slot_close(&slots[0]);
}

static void test_cad_policy_value_bounds(void)
{
    uint32_t ms = 0;
    int v = 0;

    /* CADWAIT 50..5000 ms */
    expect_int("cadwait lo accept",
               config_status_is_set_cadwait("SET CADWAIT=50", &ms), 1);
    expect_int("cadwait lo value", (int)ms, 50);
    expect_int("cadwait hi accept",
               config_status_is_set_cadwait("SET CADWAIT=5000", &ms), 1);
    expect_int("cadwait hi value", (int)ms, 5000);
    expect_int("cadwait below reject",
               config_status_is_set_cadwait("SET CADWAIT=49", &ms), 0);
    expect_int("cadwait above reject",
               config_status_is_set_cadwait("SET CADWAIT=5001", &ms), 0);
    expect_int("cadwait empty reject",
               config_status_is_set_cadwait("SET CADWAIT=", &ms), 0);

    ms = 123u;
    expect_int("cadwait blank reject",
               config_status_is_set_cadwait("", &ms), 0);
    expect_int("cadwait blank unchanged", (int)ms, 123);
    expect_int("cadwait null reject",
               config_status_is_set_cadwait(NULL, &ms), 0);
    expect_int("cadwait null unchanged", (int)ms, 123);

    expect_int("cadwait suffix reject",
               config_status_is_set_cadwait("SET CADWAIT=300x", &ms), 0);

    /* CADIDLE 0..2000 ms */
    expect_int("cadidle lo accept",
               config_status_is_set_cadidle("SET CADIDLE=0", &ms), 1);
    expect_int("cadidle lo value", (int)ms, 0);
    expect_int("cadidle hi accept",
               config_status_is_set_cadidle("SET CADIDLE=2000", &ms), 1);
    expect_int("cadidle above reject",
               config_status_is_set_cadidle("SET CADIDLE=2001", &ms), 0);

    /* CADPOLL 10..500 ms */
    expect_int("cadpoll lo accept",
               config_status_is_set_cadpoll("SET CADPOLL=10", &ms), 1);
    expect_int("cadpoll hi accept",
               config_status_is_set_cadpoll("SET CADPOLL=500", &ms), 1);
    expect_int("cadpoll below reject",
               config_status_is_set_cadpoll("SET CADPOLL=9", &ms), 0);
    expect_int("cadpoll above reject",
               config_status_is_set_cadpoll("SET CADPOLL=501", &ms), 0);

    /* CADTXAFTERTIMEOUT 0|1 */
    expect_int("cadtx 1 accept",
               config_status_is_set_cadtxaftertimeout("SET CADTXAFTERTIMEOUT=1", &v), 1);
    expect_int("cadtx 1 value", v, 1);
    expect_int("cadtx 0 accept",
               config_status_is_set_cadtxaftertimeout("SET CADTXAFTERTIMEOUT=0", &v), 1);
    expect_int("cadtx 0 value", v, 0);
    expect_int("cadtx 2 reject",
               config_status_is_set_cadtxaftertimeout("SET CADTXAFTERTIMEOUT=2", &v), 0);
}

/* --- CLI parsing and test sequence --- */

/* Audit P1-2: rejected/no-op commands must not re-arm RX (the blind
 * callback+startReceive destroyed a received, undrained packet), and a
 * hardware error mid-apply fails the radio closed. */
static void test_dispatch_rejected_no_rearm(void)
{
    int sv[2];
    ClientSlot slots[2];
    uint8_t buf[buf_SIZE];
    EventLoopSet set;
    EventLoopReadySet readfds;
    RadioController ctrl;

    memset(&g_apply_state, 0, sizeof(g_apply_state));
    g_apply_state.result_override = (int)CONFIG_APPLY_REJECTED;
    init_fake_controller(&ctrl, RADIO_HEALTH_READY);
    ctrl.received.store(true);

    client_slot_init_all(slots, 2);
    memset(buf, 0, sizeof(buf));

    if (!make_socket_pair(sv)) {
        g_fail++;
        return;
    }

    client_slot_set_fd(&slots[0], sv[1]);

    if (event_loop_init(&set) != 0) {
        close(sv[0]);
        client_slot_close(&slots[0]);
        g_fail++;
        printf("[FAIL] rejected-no-rearm setup\n");
        return;
    }

    const char *cmd = "SET SF=99\n";
    write(sv[0], cmd, strlen(cmd));

    event_loop_reset(&set);
    event_loop_add_fd(&set, sv[1]);
    expect_int("rejected-no-rearm ready wait", event_loop_wait(&set, &readfds, 100000), 1);

    ConfigDispatchContext ctx = make_context(slots, &ctrl);

    config_dispatch_context(&ctx, 2, &readfds, buf);

    expect_int("rejected: apply ran", g_apply_state.calls, 1);
    expect_int("rejected: callback untouched", fake(&ctrl)->callback_count, 0);
    expect_int("rejected: no startReceive",
               fake(&ctrl)->start_receive_count, 0);
    expect_int("rejected: pending RX survives",
               ctrl.received.load() ? 1 : 0, 1);
    expect_int("rejected: health stays READY",
               ctrl.health == RADIO_HEALTH_READY ? 1 : 0, 1);

    g_apply_state.result_override = 0;
    event_loop_close(&set);
    close(sv[0]);
    client_slot_close(&slots[0]);
}

static void test_dispatch_hw_error_fails_closed(void)
{
    int sv[2];
    ClientSlot slots[2];
    uint8_t buf[buf_SIZE];
    EventLoopSet set;
    EventLoopReadySet readfds;
    RadioController ctrl;

    memset(&g_apply_state, 0, sizeof(g_apply_state));
    g_apply_state.result_override = (int)CONFIG_APPLY_HW_ERROR;
    init_fake_controller(&ctrl, RADIO_HEALTH_READY);

    client_slot_init_all(slots, 2);
    memset(buf, 0, sizeof(buf));

    if (!make_socket_pair(sv)) {
        g_fail++;
        return;
    }

    client_slot_set_fd(&slots[0], sv[1]);

    if (event_loop_init(&set) != 0) {
        close(sv[0]);
        client_slot_close(&slots[0]);
        g_fail++;
        printf("[FAIL] hw-error setup\n");
        return;
    }

    const char *cmd = "SET SF=7\n";
    write(sv[0], cmd, strlen(cmd));

    event_loop_reset(&set);
    event_loop_add_fd(&set, sv[1]);
    expect_int("hw-error ready wait", event_loop_wait(&set, &readfds, 100000), 1);

    ConfigDispatchContext ctx = make_context(slots, &ctrl);

    config_dispatch_context(&ctx, 2, &readfds, buf);

    expect_int("hw error: apply ran", g_apply_state.calls, 1);
    expect_int("hw error: health FAILED",
               ctrl.health == RADIO_HEALTH_FAILED ? 1 : 0, 1);
    expect_int("hw error: no re-arm", fake(&ctrl)->start_receive_count, 0);

    g_apply_state.result_override = 0;
    event_loop_close(&set);
    close(sv[0]);
    client_slot_close(&slots[0]);
}

/* Audit P1-5: RF CONFIG is rejected while queued TX jobs exist (a job
 * transmits with execution-time configuration; retuning pending jobs is the
 * hazard). Status/GET queries and the drained case pass through. */
static void test_dispatch_defers_config_while_queue_busy(void)
{
    int sv[2];
    ClientSlot slots[2];
    uint8_t buf[buf_SIZE];
    EventLoopSet set;
    EventLoopReadySet readfds;
    RadioController ctrl;

    memset(&g_apply_state, 0, sizeof(g_apply_state));
    init_fake_controller(&ctrl, RADIO_HEALTH_READY);
    g_stub_pending = 2;

    client_slot_init_all(slots, 2);
    memset(buf, 0, sizeof(buf));

    if (!make_socket_pair(sv)) {
        g_fail++;
        return;
    }

    client_slot_set_fd(&slots[0], sv[1]);

    if (event_loop_init(&set) != 0) {
        close(sv[0]);
        client_slot_close(&slots[0]);
        g_fail++;
        printf("[FAIL] queue-busy setup\n");
        return;
    }

    const char *cmd = "SET SF=7\n";
    write(sv[0], cmd, strlen(cmd));

    event_loop_reset(&set);
    event_loop_add_fd(&set, sv[1]);
    expect_int("queue-busy ready wait", event_loop_wait(&set, &readfds, 100000), 1);

    ConfigDispatchContext ctx = make_context(slots, &ctrl);

    config_dispatch_context(&ctx, 2, &readfds, buf);
    expect_int("queue busy: apply not called", g_apply_state.calls, 0);
    expect_int("queue busy: no re-arm", fake(&ctrl)->start_receive_count, 0);

    /* Executing-only (pop-to-transmit window) also defers. */
    g_stub_pending = 0;
    g_stub_job_active = 1;
    write(sv[0], cmd, strlen(cmd));
    event_loop_reset(&set);
    event_loop_add_fd(&set, sv[1]);
    expect_int("executing ready wait", event_loop_wait(&set, &readfds, 100000), 1);
    config_dispatch_context(&ctx, 2, &readfds, buf);
    expect_int("job executing: apply not called", g_apply_state.calls, 0);

    /* Drained queue: the same command applies. */
    g_stub_job_active = 0;
    write(sv[0], cmd, strlen(cmd));
    event_loop_reset(&set);
    event_loop_add_fd(&set, sv[1]);
    expect_int("drained ready wait", event_loop_wait(&set, &readfds, 100000), 1);
    config_dispatch_context(&ctx, 2, &readfds, buf);
    expect_int("queue drained: apply runs", g_apply_state.calls, 1);

    event_loop_close(&set);
    close(sv[0]);
    client_slot_close(&slots[0]);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bin") == 0) {
            if (i + 1 >= argc) {
                printf("Usage: %s [--bin ignored]\n", argv[0]);
                return 2;
            }
            i++;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--bin ignored]\n", argv[0]);
            return 0;
        } else {
            printf("Usage: %s [--bin ignored]\n", argv[0]);
            return 2;
        }
    }

    test_dispatch_ready_client();
    test_dispatch_rejected_no_rearm();
    test_dispatch_hw_error_fails_closed();
    test_dispatch_defers_config_while_queue_busy();
    test_dispatch_ready_client_epoll();
    test_dispatch_ignores_not_ready_client();
    test_dispatch_sets_txmode_without_radio();
    test_get_channel_pending_rx_guard();
    test_set_cadrssi_parser();
    test_dispatch_sets_cadmonitor_optin();
    test_status_uses_cad_broadcast_latch();
    test_dispatch_get_channel_restores_rx();
    test_dispatch_get_channel_during_tx_skips_scan();
    test_dispatch_set_txresult();
    test_dispatch_set_txqueue();
    test_dispatch_closes_eof_client();
    test_dispatch_set_cadwait();
    test_dispatch_set_cadidle();
    test_dispatch_set_cadpoll();
    test_dispatch_set_cadtxaftertimeout();
    test_dispatch_set_cadwait_invalid_rejected();
    test_cad_policy_value_bounds();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);

    return g_fail ? 1 : 0;
}
