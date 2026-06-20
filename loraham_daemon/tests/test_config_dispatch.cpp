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

struct FakeRadio {
    int callback_count;
    int start_receive_count;
    int scan_result;
    int scan_count;
    float rssi;
    void (*last_callback)(void);

    FakeRadio() : callback_count(0), start_receive_count(0),
                  scan_result(0), scan_count(0), rssi(-82.5f),
                  last_callback(NULL) {}

    void setPacketReceivedAction(void (*cb)(void))
    {
        last_callback = cb;
        callback_count++;
    }

    void startReceive()
    {
        start_receive_count++;
    }

    int scanChannel()
    {
        scan_count++;
        return scan_result;
    }

    float getRSSI()
    {
        return rssi;
    }
};

/* --- Test state --- */

typedef struct {
    int calls;
    char tag[32];
    char cmd[128];
} ApplyState;

static ApplyState g_apply_state;


/* --- Async TX runtime status stubs -------------------------------------- */

size_t daemon_tx_async_runtime_pending_for_band(int band)
{
    (void)band;
    return 0;
}

size_t daemon_tx_async_runtime_dropped_for_band(int band)
{
    (void)band;
    return 0;
}

size_t daemon_tx_async_runtime_processed_for_band(int band)
{
    (void)band;
    return 0;
}

size_t daemon_tx_async_runtime_completion_stale_for_band(int band)
{
    (void)band;
    return 0;
}


size_t daemon_tx_async_runtime_completion_dropped_for_band(int band)
{
    (void)band;
    return 0;
}

int daemon_tx_async_runtime_last_result_for_band(int band,
                                                     DaemonTxJobResult *out)
{
    (void)band;
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

static void record_apply_config(FakeRadio& radio,
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
}

static void init_fake_controller(RadioController<FakeRadio> *ctrl,
                                 RadioHealth health)
{
    radio_controller_init(ctrl,
                          RADIO_BAND_433,
                          "TEST",
                          false,
                          fake_rx_callback,
                          13);
    ctrl->radio.reset(new FakeRadio());
    ctrl->health = health;
}

static ConfigDispatchContext<FakeRadio> make_context(ClientSlot *slots,
                                                     RadioController<FakeRadio> *ctrl)
{
    ConfigDispatchLog log = {
        NULL,
        NULL,
        NULL
    };

    ConfigDispatchContext<FakeRadio> ctx = {
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
    RadioController<FakeRadio> ctrl;
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

    ConfigDispatchContext<FakeRadio> ctx =
        make_context(slots, &ctrl);

    config_dispatch_context<FakeRadio>(&ctx, 2, &readfds, buf);

    expect_int("apply called once", g_apply_state.calls, 1);
    expect_str("apply tag", g_apply_state.tag, "CONF TEST");
    expect_str("apply cmd", g_apply_state.cmd, "SET GETRSSI=1");
    expect_int("mode updated by apply", ctrl.mode == RADIO_MODE_FSK, 1);
    expect_int("getrssi updated by apply", ctrl.getrssi_active.load() == true, 1);
    expect_int("callback restored", ctrl.radio->callback_count, 1);
    expect_int("startReceive called", ctrl.radio->start_receive_count, 1);
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
    RadioController<FakeRadio> ctrl;
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

    ConfigDispatchContext<FakeRadio> ctx =
        make_context(slots, &ctrl);

    config_dispatch_context<FakeRadio>(&ctx, 2, &readfds, buf);

    expect_int("epoll apply called once", g_apply_state.calls, 1);
    expect_str("epoll apply cmd", g_apply_state.cmd, "SET GETRSSI=1");
    expect_int("epoll callback restored", ctrl.radio->callback_count, 1);
    expect_int("epoll startReceive called", ctrl.radio->start_receive_count, 1);

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
    RadioController<FakeRadio> ctrl;
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

    ConfigDispatchContext<FakeRadio> ctx =
        make_context(slots, &ctrl);

    config_dispatch_context<FakeRadio>(&ctx, 2, &readfds, buf);

    expect_int("not ready apply count", g_apply_state.calls, 0);
    expect_int("not ready callback count", ctrl.radio->callback_count, 0);
    expect_int("not ready startReceive count", ctrl.radio->start_receive_count, 0);
    expect_int("not ready client open", client_slot_has_client(&slots[0]), 1);

    event_loop_close(&set);
    close(sv[0]);
    client_slot_close(&slots[0]);
}


static void test_dispatch_get_channel_restores_rx(void)
{
    int sv[2];
    ClientSlot slots[2];
    uint8_t buf[buf_SIZE];
    EventLoopSet set;
    EventLoopReadySet readfds;
    RadioController<FakeRadio> ctrl;
    char out[256];
    ssize_t n;

    init_fake_controller(&ctrl, RADIO_HEALTH_READY);
    ctrl.radio->scan_result = 1;
    ctrl.radio->rssi = -77.25f;

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

    ConfigDispatchContext<FakeRadio> ctx =
        make_context(slots, &ctrl);

    config_dispatch_context<FakeRadio>(&ctx, 2, &readfds, buf);

    n = read(sv[0], out, sizeof(out) - 1);
    if (n < 0)
        n = 0;
    out[n] = '\0';

    expect_int("get channel apply count", g_apply_state.calls, 0);
    expect_int("get channel scan count", ctrl.radio->scan_count, 1);
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
    expect_int("get channel callback restored", ctrl.radio->callback_count, 2);
    expect_int("get channel startReceive called", ctrl.radio->start_receive_count, 2);
    expect_int("get channel client open", client_slot_has_client(&slots[0]), 1);

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
    RadioController<FakeRadio> ctrl;
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

    ConfigDispatchContext<FakeRadio> ctx =
        make_context(slots, &ctrl);

    config_dispatch_context<FakeRadio>(&ctx, 2, &readfds, buf);

    expect_int("txqueue apply not called", g_apply_state.calls, 0);
    expect_int("txqueue enabled", ctrl.tx_queue_active.load() == true, 1);
    expect_int("txqueue callback not restored", ctrl.radio->callback_count, 0);
    expect_int("txqueue startReceive not called", ctrl.radio->start_receive_count, 0);
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
    RadioController<FakeRadio> ctrl;
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

    ConfigDispatchContext<FakeRadio> ctx =
        make_context(slots, &ctrl);

    config_dispatch_context<FakeRadio>(&ctx, 2, &readfds, buf);

    expect_int("txresult apply not called", g_apply_state.calls, 0);
    expect_int("txresult enabled", ctrl.tx_result_active.load() == true, 1);
    expect_int("txresult callback not restored", ctrl.radio->callback_count, 0);
    expect_int("txresult startReceive not called", ctrl.radio->start_receive_count, 0);
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
    RadioController<FakeRadio> ctrl;
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

    const char *cmd = "SET TXMODE=RAW\n";
    write(sv[0], cmd, strlen(cmd));

    event_loop_reset(&set);
    event_loop_add_fd(&set, sv[1]);
    expect_int("txmode wait", event_loop_wait(&set, &readfds, 100000), 1);

    ConfigDispatchContext<FakeRadio> ctx =
        make_context(slots, &ctrl);

    config_dispatch_context<FakeRadio>(&ctx, 2, &readfds, buf);

    expect_int("txmode apply count", g_apply_state.calls, 0);
    expect_int("txmode raw", ctrl.tx_mode == RADIO_TX_MODE_RAW, 1);
    expect_int("txmode client open", client_slot_has_client(&slots[0]), 1);

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
    RadioController<FakeRadio> ctrl;
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

    ConfigDispatchContext<FakeRadio> ctx =
        make_context(slots, &ctrl);

    config_dispatch_context<FakeRadio>(&ctx, 2, &readfds, buf);

    expect_int("eof apply count", g_apply_state.calls, 0);
    expect_int("eof callback count", ctrl.radio->callback_count, 0);
    expect_int("eof startReceive count", ctrl.radio->start_receive_count, 0);
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
    RadioController<FakeRadio> ctrl;
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

    ConfigDispatchContext<FakeRadio> ctx = make_context(slots, &ctrl);
    config_dispatch_context<FakeRadio>(&ctx, 2, &readfds, buf);

    expect_int("cadwait apply not called",  g_apply_state.calls, 0);
    expect_int("cadwait stored", (int)ctrl.cad_wait_timeout_ms.load(), 300);
    expect_int("cadwait callback not restored",  ctrl.radio->callback_count, 0);
    expect_int("cadwait startReceive not called", ctrl.radio->start_receive_count, 0);
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
    RadioController<FakeRadio> ctrl;
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

    ConfigDispatchContext<FakeRadio> ctx = make_context(slots, &ctrl);
    config_dispatch_context<FakeRadio>(&ctx, 2, &readfds, buf);

    expect_int("cadidle apply not called",  g_apply_state.calls, 0);
    expect_int("cadidle stored", (int)ctrl.cad_idle_stable_ms.load(), 100);
    expect_int("cadidle callback not restored",  ctrl.radio->callback_count, 0);
    expect_int("cadidle startReceive not called", ctrl.radio->start_receive_count, 0);
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
    RadioController<FakeRadio> ctrl;
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

    ConfigDispatchContext<FakeRadio> ctx = make_context(slots, &ctrl);
    config_dispatch_context<FakeRadio>(&ctx, 2, &readfds, buf);

    expect_int("cadpoll apply not called",  g_apply_state.calls, 0);
    expect_int("cadpoll stored", (int)ctrl.cad_poll_interval_ms.load(), 50);
    expect_int("cadpoll callback not restored",  ctrl.radio->callback_count, 0);
    expect_int("cadpoll startReceive not called", ctrl.radio->start_receive_count, 0);
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
    RadioController<FakeRadio> ctrl;
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

    ConfigDispatchContext<FakeRadio> ctx = make_context(slots, &ctrl);
    config_dispatch_context<FakeRadio>(&ctx, 2, &readfds, buf);

    expect_int("cadtx apply not called",  g_apply_state.calls, 0);
    expect_int("cadtx stored", ctrl.cad_send_after_timeout.load() ? 1 : 0, 1);
    expect_int("cadtx callback not restored",  ctrl.radio->callback_count, 0);
    expect_int("cadtx startReceive not called", ctrl.radio->start_receive_count, 0);
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
    RadioController<FakeRadio> ctrl;
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

    ConfigDispatchContext<FakeRadio> ctx = make_context(slots, &ctrl);
    config_dispatch_context<FakeRadio>(&ctx, 2, &readfds, buf);

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
    test_dispatch_ready_client_epoll();
    test_dispatch_ignores_not_ready_client();
    test_dispatch_sets_txmode_without_radio();
    test_dispatch_get_channel_restores_rx();
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
