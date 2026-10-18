/*
 * ch341_monitor.c (the unplug-wait monitor in the Meshstick library), compiled
 * at run time like test_ch341_adapter: the REAL ch341_monitor.c against
 * tests/fakes/stub_libusb (no libusb in the lanes) and a stub pine64 header.
 * A C driver runs every case and prints "CHECK <name> <0|1>"; this file
 * asserts on those lines.
 *
 *   T7   no hotplug / pollfds that do not cover timeouts -> no monitor
 *   T9   pollfds with masks, -1 on a small max, generation on notifier;
 *        bind unique / none / several; the presence check opens nothing and
 *        sends no request; arrivals; device references balanced after close
 *   T14  a failed device list -> -1 (never "gone")
 *   T14b a new object at the same bus/address -> gone; a LEFT that becomes
 *        pending after look #1 -> drained -> gone; a removal after look #1
 *        whose LEFT is not queued yet -> look #2 -> gone
 *   T16  scan: serial / no serial (no request) / ACCESS / stall -> unknown
 *        within the budget; identity of an opened handle
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <limits.h>
#include <unistd.h>

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

static const char *PINEDIO_STUB_H =
    "#include <stdbool.h>\n#include <stdint.h>\n#include <libusb-1.0/libusb.h>\n"
    "struct pinedio_inst { libusb_device_handle *handle; bool in_error; char serial_number[9]; };\n";

static const char *DRIVER_C = R"C(
#include <stdio.h>
#include <string.h>
#include "ch341_adapter.h"
#include "ch341_adapter_priv.h"

extern int stub_opens, stub_controls, stub_has_hotplug, stub_handles_timeouts, stub_list_fails;
libusb_device *stub_plug(uint8_t bus, uint8_t addr, const uint8_t *ports, int nports, const char *serial);
void stub_unplug(libusb_device *d);
void stub_queue_event(libusb_device *d, int left);
void stub_after_list_event(libusb_device *d, int left);
void stub_set_access(libusb_device *d, int denied);
void stub_set_stall(libusb_device *d, int stall);
void stub_set_malformed(libusb_device *d, int m);
void stub_set_delay(libusb_device *d, int ms);
extern unsigned stub_timeouts[8];
extern int stub_ref_calls, stub_unref_calls, stub_refs_went_negative;
extern int stub_closes, stub_live_handles;
extern int stub_ntimeouts;
int stub_refs(libusb_device *d);
void stub_fd_added(int fd);

#define CHECK(name, cond) printf("CHECK %s %d\n", name, (cond) ? 1 : 0)

static loraham_ch341 opened(libusb_device *d)
{
    loraham_ch341 dev;
    memset(&dev, 0, sizeof(dev));
    libusb_open(d, &dev.inst.handle);
    return dev;
}

int main(void)
{
    const uint8_t p2[1] = { 2 }, p3[1] = { 3 }, p4[2] = { 1, 4 };

    /* T7 */
    stub_has_hotplug = 0;
    CHECK("no_hotplug_no_monitor", loraham_ch341_mon_open(0x1A86, 0x5512) == NULL);
    stub_has_hotplug = 1;
    stub_handles_timeouts = 0;
    CHECK("timeouts_not_covered_no_monitor", loraham_ch341_mon_open(0x1A86, 0x5512) == NULL);
    stub_handles_timeouts = 1;

    loraham_ch341_mon *m = loraham_ch341_mon_open(0x1A86, 0x5512);
    CHECK("monitor_opens", m != NULL);

    /* T9 pollfds */
    loraham_ch341_pollfd fds[4];
    int n = loraham_ch341_mon_fds(m, fds, 4);
    CHECK("fds_count_and_mask", n == 1 && fds[0].fd == 42 && fds[0].events == 1);
    CHECK("fds_small_max_is_minus1", loraham_ch341_mon_fds(m, fds, 0) == -1);
    unsigned g0 = loraham_ch341_mon_fd_generation(m);
    stub_fd_added(43);
    CHECK("fd_generation_changes", loraham_ch341_mon_fd_generation(m) != g0);

    /* T9 bind */
    libusb_device *a = stub_plug(1, 5, p2, 1, "TESTSN01");
    loraham_ch341 da = opened(a);
    CHECK("bind_unique", loraham_ch341_mon_bind(m, &da) == 0);
    loraham_ch341 none;
    memset(&none, 0, sizeof(none));
    CHECK("bind_none", loraham_ch341_mon_bind(m, &none) == -1);
    libusb_device *twin = stub_plug(1, 5, p2, 1, "x");
    CHECK("bind_several", loraham_ch341_mon_bind(m, &da) == -1);
    stub_unplug(twin);
    CHECK("bind_again", loraham_ch341_mon_bind(m, &da) == 0);

    /* T9: presence opens nothing, sends no request */
    int o0 = stub_opens, c0 = stub_controls;
    CHECK("present_when_attached", loraham_ch341_mon_present_bound(m) == 1);
    CHECK("present_no_open_no_request", stub_opens == o0 && stub_controls == c0);

    /* LEFT for the bound device */
    stub_unplug(a);
    stub_queue_event(a, 1);
    loraham_ch341_mon_dispatch(m);
    CHECK("left_bound_seen", loraham_ch341_mon_left_bound(m) == 1);
    CHECK("present_after_left_is_gone", loraham_ch341_mon_present_bound(m) == 0);

    /* T14b: a new object at the same bus/address/port */
    libusb_device *b = stub_plug(1, 6, p3, 1, "TESTSN01");
    loraham_ch341 db = opened(b);
    CHECK("bind_b", loraham_ch341_mon_bind(m, &db) == 0);
    CHECK("bind_clears_left", loraham_ch341_mon_left_bound(m) == 0);
    stub_unplug(b);
    libusb_device *b2 = stub_plug(1, 6, p3, 1, "TESTSN01");   /* reused number, new connection */
    CHECK("reuse_new_object_is_gone", loraham_ch341_mon_present_bound(m) == 0);

    /* T14b: LEFT pending only after look #1 -> the drain */
    loraham_ch341 db2 = opened(b2);
    CHECK("bind_b2", loraham_ch341_mon_bind(m, &db2) == 0);
    stub_after_list_event(b2, 1);
    CHECK("left_after_look1_drained_gone", loraham_ch341_mon_present_bound(m) == 0);

    /* T14b: removal after look #1, LEFT not queued yet -> look #2 */
    libusb_device *c = stub_plug(2, 7, p4, 2, NULL);
    loraham_ch341 dc = opened(c);
    CHECK("bind_c", loraham_ch341_mon_bind(m, &dc) == 0);
    stub_after_list_event(c, 2);
    CHECK("removal_after_look1_look2_gone", loraham_ch341_mon_present_bound(m) == 0);

    /* T14 */
    libusb_device *d = stub_plug(2, 8, p2, 1, NULL);
    loraham_ch341 dd = opened(d);
    int r_d0 = stub_refs(d);                         /* the baseline before the monitor holds it */
    CHECK("bind_d", loraham_ch341_mon_bind(m, &dd) == 0);
    CHECK("bind_takes_one_reference", stub_refs(d) == r_d0 + 1);
    stub_list_fails = 1;
    CHECK("list_failure_is_minus1", loraham_ch341_mon_present_bound(m) == -1);
    stub_list_fails = 0;

    /* arrivals */
    stub_queue_event(d, 0);
    loraham_ch341_mon_dispatch(m);
    CHECK("arrival_taken_once", loraham_ch341_mon_take_arrival(m) == 1 &&
                                loraham_ch341_mon_take_arrival(m) == 0);

    /* r12 (core r8 B2): a LEFT of a device other than the bound one is
     * counted for the selection (take_left), the bound one's is not */
    libusb_device *other = stub_plug(4, 9, p3, 1, "OTHER001");
    stub_queue_event(other, 1);
    loraham_ch341_mon_dispatch(m);
    CHECK("unbound_left_taken_once", loraham_ch341_mon_take_left(m) == 1 && loraham_ch341_mon_take_left(m) == 0 &&
                                     loraham_ch341_mon_left_bound(m) == 0);

    /* r16 (core r11 F1): a queue overflow loses events; a lost LEFT may have
     * been the bound stick's, so the bound stick counts as left (and the lost
     * events count as an arrival); the next bind clears it */
    for (int k = 0; k < 40; k++)
        stub_queue_event(other, 0);                  /* more than the queue holds */
    loraham_ch341_mon_dispatch(m);
    int overflow_arrivals = 0;
    while (loraham_ch341_mon_take_arrival(m))
        overflow_arrivals++;
    CHECK("overflow_marks_the_bound_stick_uncertain_not_left", loraham_ch341_mon_left_bound(m) == 2 && overflow_arrivals >= 1);
    CHECK("overflow_flag_cleared_by_the_next_bind", loraham_ch341_mon_bind(m, &dd) == 0 && loraham_ch341_mon_left_bound(m) == 0);

    /* r19 (core r14 F1): an overflow during the presence check's drain is
     * not a bound LEFT: list #2 decides (the stick is present) */
    for (int k = 0; k < 40; k++)
        stub_after_list_event(other, 0);             /* queued after list #1, more than the queue holds */
    CHECK("overflow_in_the_drain_goes_to_list2_present", loraham_ch341_mon_present_bound(m) == 1 &&
                                                        loraham_ch341_mon_left_bound(m) == 2);
    while (loraham_ch341_mon_take_arrival(m)) { }
    CHECK("overflow_flag_cleared_by_the_next_bind_again", loraham_ch341_mon_bind(m, &dd) == 0 && loraham_ch341_mon_left_bound(m) == 0);

    /* DELTA-6 N1c: a real unplug of the bound stick hidden in an overflow is
     * still "gone": absent at list #1 */
    libusb_device *e = stub_plug(5, 11, p3, 1, "LOSTLEFT");
    loraham_ch341 de = opened(e);
    bool e_bound = loraham_ch341_mon_bind(m, &de) == 0;
    for (int k = 0; k < 40; k++)
        stub_queue_event(other, 0);
    loraham_ch341_mon_dispatch(m);
    while (loraham_ch341_mon_take_arrival(m)) { }
    stub_unplug(e);                                  /* its LEFT lost in the overflow */
    CHECK("overflow_hiding_a_real_unplug_is_absent_at_list1", e_bound && loraham_ch341_mon_left_bound(m) == 2 &&
                                                             loraham_ch341_mon_present_bound(m) == 0);
    libusb_close(de.inst.handle);
    loraham_ch341_mon_bind(m, &dd);                  /* back to d for the checks below */
    stub_unplug(other);

    /* T16 scan: d (no serial, 2-8 p2) attached; add serial, ACCESS, stall */
    libusb_device *s1 = stub_plug(3, 2, p2, 1, "TESTSN01");
    libusb_device *s2 = stub_plug(3, 3, p3, 1, "ABCDEFGH");
    stub_set_access(s2, 1);
    libusb_device *s3 = stub_plug(3, 4, p4, 2, "STALLED1");
    stub_set_stall(s3, 1);
    int r_d1 = stub_refs(d);
    int cnt = loraham_ch341_mon_scan_begin(m);
    CHECK("scan_count", cnt == 4);
    loraham_ch341_scan_item it;
    int seen_serial = 0, seen_none = 0, seen_access = 0, seen_stall = 0, none_requests = 0;
    int none_opens = -1, none_closes = -1, live0 = stub_live_handles;
    for (int i = 0; i < cnt; i++) {
        int before = stub_controls, o_before = stub_opens, c_before = stub_closes;
        loraham_ch341_mon_scan_step(m, i, &it);
        if (it.openable == 1 && it.serial_state == 0) {
            none_opens = stub_opens - o_before;
            none_closes = stub_closes - c_before;
        }
        if (it.openable == 1 && it.serial_state == 1 && strcmp(it.serial, "TESTSN01") == 0 &&
            strcmp(it.port, "3-2") == 0)
            seen_serial = 1;
        if (it.openable == 1 && it.serial_state == 0) {
            seen_none = 1;
            none_requests = stub_controls - before;
        }
        if (it.openable == 0)
            seen_access = 1;
        if (it.openable == 1 && it.serial_state == -1 && strcmp(it.port, "3-1.4") == 0)
            seen_stall = 1;
    }
    loraham_ch341_mon_scan_end(m);
    CHECK("cookie_creation_takes_one_reference", stub_refs(d) == r_d1 + 1);
    CHECK("scan_serial_and_port", seen_serial);
    CHECK("scan_no_serial", seen_none);
    CHECK("scan_no_serial_sends_no_request", seen_none && none_requests == 0);
    CHECK("scan_index0_one_open_one_close", none_opens == 1 && none_closes == 1);
    CHECK("scan_leaves_no_handle_open", stub_live_handles == live0);
    CHECK("scan_access", seen_access);
    CHECK("scan_stall_unknown", seen_stall);

    /* identity of an opened handle */
    loraham_ch341 d1 = opened(s1);
    CHECK("identity_ok", loraham_ch341_identity(&d1, &it) == 0 && it.serial_state == 1 &&
                         strcmp(it.serial, "TESTSN01") == 0 && strcmp(it.port, "3-2") == 0);
    libusb_close(d1.inst.handle);

    /* W12 note: malformed string descriptors are no serial ("unknown") */
    libusb_device *mo = stub_plug(4, 2, p2, 1, "A");
    stub_set_malformed(mo, 1);
    loraham_ch341 dm = opened(mo);
    CHECK("odd_serial_length_is_unknown", loraham_ch341_identity(&dm, &it) == 0 && it.serial_state == -1);
    stub_set_malformed(mo, 2);
    CHECK("short_language_table_is_unknown", loraham_ch341_identity(&dm, &it) == 0 && it.serial_state == -1);
    stub_set_malformed(mo, 0);
    CHECK("one_char_serial_well_formed_is_read", loraham_ch341_identity(&dm, &it) == 0 &&
                                                 it.serial_state == 1 && strcmp(it.serial, "A") == 0);
    libusb_close(dm.inst.handle);

    /* T9: the two requests share ONE 200 ms budget; inst->serial_number is never read. */
    libusb_device *bd = stub_plug(5, 2, p2, 1, "BUDGET01");
    stub_set_delay(bd, 120);
    loraham_ch341 dbg = opened(bd);
    strcpy(dbg.inst.serial_number, "GARBAGE!");
    stub_ntimeouts = 0;
    int ir = loraham_ch341_identity(&dbg, &it);
    CHECK("serial_budget_first_request_gets_200", stub_ntimeouts >= 1 && stub_timeouts[0] == 200);
    CHECK("serial_budget_second_request_gets_the_rest",
          ir == 0 && it.serial_state == 1 && stub_ntimeouts == 2 && stub_timeouts[1] <= 80);
    CHECK("serial_number_field_ignored", strcmp(it.serial, "BUDGET01") == 0);
    stub_set_delay(bd, 250);
    stub_ntimeouts = 0;
    CHECK("serial_budget_exhausted_unknown_no_second_request",
          loraham_ch341_identity(&dbg, &it) == 0 && it.serial_state == -1 && stub_ntimeouts == 1);
    libusb_close(dbg.inst.handle);

    /* F3: more than 64 sticks: the scan counts every one. */
    int before_many = loraham_ch341_mon_scan_begin(m);
    loraham_ch341_mon_scan_end(m);
    for (int k = 0; k < 70; k++) {
        static const uint8_t pk[1] = { 9 };
        stub_plug(6, (uint8_t)(10 + k), pk, 1, NULL);
    }
    CHECK("scan_counts_more_than_64", loraham_ch341_mon_scan_begin(m) == before_many + 70);
    loraham_ch341_mon_scan_end(m);

    /* A connection cookie per libusb_device object: the same object keeps
     * its cookie; a new connection at the same bus/address/port gets a new
     * one; an object no scan lists any more loses the monitor's reference. */
    libusb_device *k1 = stub_plug(7, 20, p2, 1, "COOKIE01");
    int r_k0 = stub_refs(k1), r_k1 = -1;
    unsigned c1 = 0, c1b = 0, c2 = 0;
    for (int pass = 0; pass < 3; pass++) {
        if (pass == 2) {
            stub_unplug(k1);
            stub_plug(7, 20, p2, 1, "COOKIE01");     /* replugged: a new object, same address */
        }
        int nk = loraham_ch341_mon_scan_begin(m);
        for (int i = 0; i < nk; i++) {
            loraham_ch341_mon_scan_step(m, i, &it);
            if (strcmp(it.port, "7-2") == 0)
                *(pass == 0 ? &c1 : pass == 1 ? &c1b : &c2) = it.connection;
        }
        loraham_ch341_mon_scan_end(m);
        if (pass == 0)
            r_k1 = stub_refs(k1);
    }
    CHECK("cookie_created_with_one_reference", r_k1 == r_k0 + 1);
    CHECK("cookie_same_object_keeps_its_cookie", c1 != 0 && c1 == c1b);
    CHECK("cookie_new_connection_same_address_new_cookie", c2 != 0 && c2 != c1);
    CHECK("cookie_reference_dropped_when_unlisted", stub_refs(k1) == 1 && stub_refs(k1) == r_k0);

    /* references: the callback references an ARRIVED's device and the same
     * dispatch counts it and releases that reference, with no I/O; closing
     * the monitor releases the binding and the cookie table */
    while (loraham_ch341_mon_take_arrival(m)) { }
    int r_q0 = stub_refs(s1), o_q0 = stub_opens, c_q0 = stub_controls;
    int rc0 = stub_ref_calls, uc0 = stub_unref_calls;
    stub_queue_event(s1, 0);
    loraham_ch341_mon_dispatch(m);
    CHECK("arrival_counted_its_reference_released_no_io",
          loraham_ch341_mon_take_arrival(m) == 1 && stub_refs(s1) == r_q0 &&
          stub_opens == o_q0 && stub_controls == c_q0);
    CHECK("arrival_exactly_one_ref_and_one_unref", stub_ref_calls == rc0 + 1 && stub_unref_calls == uc0 + 1);
    stub_queue_event(s1, 0);
    loraham_ch341_mon_dispatch(m);
    int r_bound_before = stub_refs(d);
    loraham_ch341_mon_close(m);
    CHECK("bound_and_cookie_references_released", stub_refs(d) == r_bound_before - 2);
    CHECK("close_returns_to_the_original_baseline", stub_refs(d) == r_d0);
    CHECK("queued_references_released", stub_refs(s1) == 1);
    CHECK("no_reference_count_ever_negative", stub_refs_went_negative == 0);

    libusb_close(da.inst.handle);
    libusb_close(db.inst.handle);
    libusb_close(db2.inst.handle);
    libusb_close(dc.inst.handle);
    libusb_close(dd.inst.handle);
    (void)s2;
    return 0;
}
)C";

static void write_file(const std::string &path, const char *text)
{
    FILE *f = fopen(path.c_str(), "w");
    fputs(text, f);
    fclose(f);
}

int main(void)
{
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0)
        return 1;
    exe[n] = '\0';
    std::string src_dir = std::string(exe, strrchr(exe, '/') - exe) + "/..";
    std::string stub_dir = src_dir + "/tests/fakes/stub_libusb";

    char tmpl[] = "/tmp/loraham-ch341-monitor.XXXXXX";
    if (!mkdtemp(tmpl))
        return 1;
    std::string tmp = tmpl;
    write_file(tmp + "/libpinedio-usb.h", PINEDIO_STUB_H);
    write_file(tmp + "/driver.c", DRIVER_C);

    std::string cc = "cc -std=gnu11 -Wall -Wextra -Werror -I'" + tmp + "' -I'" + stub_dir +
                     "' -I'" + src_dir + "' -o '" + tmp + "/driver' '" + tmp + "/driver.c' '" +
                     stub_dir + "/libusb_stub.c' '" + src_dir + "/ch341_monitor.c'";
    expect("the real ch341_monitor.c builds against the stub libusb", system(cc.c_str()) == 0);

    std::string out = tmp + "/out.txt";
    expect("the driver runs", system(("'" + tmp + "/driver' > '" + out + "' 2>&1").c_str()) == 0);

    FILE *f = fopen(out.c_str(), "r");
    char line[256];
    int checks = 0;
    std::string all;
    while (f && fgets(line, sizeof(line), f)) {
        all += line;
        char name[128];
        int v;
        if (sscanf(line, "CHECK %127s %d", name, &v) == 2) {
            expect(name, v == 1);
            checks++;
        } else {
            printf("       %s", line);
        }
    }
    if (f)
        fclose(f);
    expect("every case reported", checks == 59);
    /* The presence check names the look that decided (live proof P1 reads it). */
    expect("presence log: absent at list #1", all.find("presence check: absent at list #1 (drain and list #2 not called)") != std::string::npos);
    expect("presence log: LEFT in the drain", all.find("present at list #1, LEFT in the drain (list #2 not called)") != std::string::npos);
    expect("presence log: absent at list #2", all.find("no LEFT in the drain, absent at list #2") != std::string::npos);
    expect("presence log: present at list #2", all.find("no LEFT in the drain, present at list #2") != std::string::npos);

    int rc = system(("rm -rf '" + tmp + "'").c_str());
    (void)rc;
    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
