/*
 * The REAL daemon binary on a scripted Meshstick library
 * (tests/fakes/ch341_process_fake.c, built here as lib/libloraham-ch341.so):
 * whole processes, their sockets, their exit codes.
 *
 *   T1/T3/T11  unplug: RADIO=UNPLUGGED, every client stays, a NEW client is
 *              accepted, framed TX answered RADIO_NOT_READY, no exit
 *   T12        a TX in flight at the unplug gets exactly one TX_RESULT
 *   T21/BOUND  USBWAIT= and BOUND= follow the runtime: -/-@1-2 when READY,
 *              1-2/- while waiting (gone, other port, a serial stick on the
 *              port, BUSY), back after the return
 *   T4/T6      the return: re-init + replay line + READY; a chip that fails
 *              re-init, or a replay that fails -> RADIO=FAILED path, exit 5
 *              (--radio-error exit, O3)
 *   T2         an I/O error with the stick present -> exit 5 (exit mode, O3)
 *   RETRY      --radio-error retry (the USB default, 1.5.0): O1 the option,
 *              O2 RADIOERR=/USBFAULT=; T1/T1b/T7/T8 a present-path fault: the
 *              same pid, UNPLUGGED, USBFAULT=1, re-opens >= 1, 2, 4 s after
 *              each close, TX refused without reaching the chip, back READY
 *              with the replay; T3 a failed re-init after an unplug retried;
 *              T6 a refused close: no new open, the "restart the daemon" line
 *              on each attempt
 *   RADIO      GET RADIO: T1 the 868 defaults, T5 the values kept while
 *              UNPLUGGED and after the replay; T8b RX frames through the fake's
 *              RX poll thread, with and without GET RADIO polled at 100 Hz
 *   QUIT       (built as test_ch341_stop, -DCH341_PROCESS_STOP=1)
 *              Q1 QUIT/quit/EXIT/exit: OK, exit 0; Q2 QUIT x: ERR MALFORMED,
 *              running; Q3 a data client reads EOF; Q4 a never-reading CONF
 *              client's QUIT still exits 0 in the bound; T5 SIGTERM / QUIT in
 *              the backoff wait and inside a held re-open: exit 0 in the bound
 *   T16/T13    ACCESS at start -> exit 4 (never 5); ACCESS on a reopen ->
 *              waits with the line, a new ARRIVED succeeds
 *   T19        r3 / r3f / no id side by side: six+three distinct sockets, CONF
 *              answers ID= on each; a duplicate --id exits 3 before any USB
 *              open; T22 a second id on a claimed stick exits 4 with BUSY:,
 *              the first undisturbed
 *   T23        --usb-serial / --usb-port on a Pi preset, malformed, or both
 *              -> exit 1
 *   PORT       --usb-port: two sticks without a serial, the named port opened
 *              by port and READY (BOUND=-@<port>), the other never opened; a
 *              library without the open by port -> exit 4
 *   F4         SIGTERM: exit 0, the stick closed before the monitor
 *   fds        mon_fds() == -1 after a registered fd: the stale registration
 *              is dropped (a reused, readable fd number does not spin the loop)
 *   T7         no hotplug: the startup note; a lost stick exits 5 as before
 *   S9         preset POWER ceilings: PineDio 0..20, the Meshstick's 22
 *   S10        the real chat binary leaves the operator's POWER alone
 *   RL         1.6.0 RADIO=<state> on CONF: each health change once, in order,
 *              to every CONF client (unplug, a failed re-init in retry mode,
 *              the return: UNPLUGGED, READY only); no line without a change;
 *              a client connected after a change gets no old line
 */

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../framed_data.h"

/* 1: the build of test_ch341_stop (run_tests.sh), the stop scenarios only. */
#ifndef CH341_PROCESS_STOP
#define CH341_PROCESS_STOP 0
#endif

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
    fflush(stdout);
}

static int sh(const std::string &cmd)
{
    int rc = system(cmd.c_str());
    return (rc != -1 && WIFEXITED(rc)) ? WEXITSTATUS(rc) : -1;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, nullptr);
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static std::string g_tmp, g_bin, g_bin_noport, g_fake, g_sock, g_run, g_chat;

static void write_text(const std::string &path, const std::string &text)
{
    std::string tmp = path + ".new";
    FILE *f = fopen(tmp.c_str(), "w");
    fputs(text.c_str(), f);
    fclose(f);
    rename(tmp.c_str(), path.c_str());          /* the library never reads half a file */
}

static std::string read_text(const std::string &path)
{
    std::string s;
    FILE *f = fopen(path.c_str(), "r");
    char buf[4096];
    size_t n;
    while (f && (n = fread(buf, 1, sizeof(buf), f)) > 0)
        s.append(buf, n);
    if (f)
        fclose(f);
    return s;
}

static void bus(const std::string &lines) { write_text(g_fake + "/bus", lines); }

/* ---- processes --------------------------------------------------------------- */

static pid_t start(const std::vector<std::string> &args, const std::string &log)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
        setenv("LORAHAM_RUNTIME_DIR", g_run.c_str(), 1);
        setenv("LORAHAM_SOCKET_DIR", g_sock.c_str(), 1);
        setenv("LORAHAM_FAKE_CH341", g_fake.c_str(), 1);
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(g_bin.c_str()));
        for (const std::string &a : args)
            argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        execv(g_bin.c_str(), argv.data());
        _exit(127);
    }
    return pid;
}

/* Exit code, -2 still running after `ms`, -1 killed by a signal. */
static int wait_exit(pid_t pid, int ms)
{
    int status = 0;
    uint64_t end = now_ms() + (uint64_t)ms;
    do {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        sleep_ms(20);
    } while (now_ms() < end);
    return -2;
}

static void stop(pid_t pid)
{
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
}

static int term(pid_t pid)
{
    kill(pid, SIGTERM);
    int rc = wait_exit(pid, 10000);
    if (rc == -2)
        stop(pid);
    return rc;
}

/* ---- sockets ----------------------------------------------------------------- */

static int connect_to(const std::string &name)
{
    std::string path = g_sock + "/" + name;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path.c_str());
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static std::string read_line(int fd, int ms)
{
    std::string s;
    uint64_t end = now_ms() + (uint64_t)ms;
    while (now_ms() < end) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 50) <= 0)
            continue;
        char c;
        if (read(fd, &c, 1) != 1)
            break;
        if (c == '\n')
            return s;
        s += c;
    }
    return s;
}

/* One CONF command on its own connection; the first line answering it. */
static std::string conf(const std::string &sock, const std::string &cmd, const char *prefix)
{
    int fd = connect_to(sock);
    if (fd < 0)
        return "";
    std::string line = cmd + "\n";
    if (write(fd, line.data(), line.size()) < 0) { }
    std::string r;
    for (int i = 0; i < 20; i++) {
        r = read_line(fd, 1000);
        if (r.rfind(prefix, 0) == 0)
            break;
    }
    close(fd);
    return r;
}

static std::string status(const std::string &sock) { return conf(sock, "GET STATUS", "STATUS"); }

static bool wait_status(const std::string &sock, const std::string &part, int ms)
{
    uint64_t end = now_ms() + (uint64_t)ms;
    do {
        if (status(sock).find(part) != std::string::npos)
            return true;
        sleep_ms(100);
    } while (now_ms() < end);
    return false;
}

static bool log_has(const std::string &log, const std::string &part)
{
    return read_text(log).find(part) != std::string::npos;
}

static bool wait_log(const std::string &log, const std::string &part, int ms)
{
    uint64_t end = now_ms() + (uint64_t)ms;
    do {
        if (log_has(log, part))
            return true;
        sleep_ms(50);
    } while (now_ms() < end);
    return false;
}

/* Releases a step the fake holds (scandelay, opendelay, binddelay): the
 * fake waits from its marker until this removal, so the bus change made
 * before it always lands inside the step. */
static void release(const char *ctl) { unlink((g_fake + "/" + ctl).c_str()); }

static bool held_until_released(void) { return !log_has(g_fake + "/log", " holdtimeout "); }

/* Framed TX: one TX_PACKET; TX_RESULT frames counted. */
static void framed_tx(int fd, const char *payload)
{
    uint8_t buf[300];
    size_t n = strlen(payload);
    buf[0] = 0x02;
    buf[1] = (uint8_t)(n & 0xff);
    buf[2] = (uint8_t)(n >> 8);
    memcpy(buf + 3, payload, n);
    if (write(fd, buf, n + 3) < 0) { }
}

static std::vector<int> framed_results(int fd, int ms)
{
    std::vector<int> st;
    uint64_t end = now_ms() + (uint64_t)ms;
    std::string acc;
    while (now_ms() < end) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 50) <= 0)
            continue;
        char b[512];
        ssize_t n = read(fd, b, sizeof(b));
        if (n <= 0)
            break;
        acc.append(b, (size_t)n);
        while (acc.size() >= 3) {
            size_t len = (uint8_t)acc[1] | ((size_t)(uint8_t)acc[2] << 8);
            if (acc.size() < 3 + len)
                break;
            if ((uint8_t)acc[0] == 0x04 && len == 4)
                st.push_back((uint8_t)acc[3]);
            acc.erase(0, 3 + len);
        }
    }
    return st;
}

static bool connected(int fd)
{
    if (fd < 0)
        return false;
    struct pollfd p = { fd, POLLIN, 0 };
    if (poll(&p, 1, 0) == 1 && (p.revents & (POLLHUP | POLLERR)))
        return false;
    char c;
    if (p.revents & POLLIN)
        return recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT) != 0;
    return true;
}

static std::vector<std::string> mesh(const std::vector<std::string> &extra)
{
    std::vector<std::string> a = { "--radio", "868", "--hw", "meshstick-1262" };
    a.insert(a.end(), extra.begin(), extra.end());
    return a;
}

static void reset_dirs(void)
{
    sh("rm -rf '" + g_fake + "' '" + g_sock + "' '" + g_run + "' && mkdir -p '" + g_fake + "' '" + g_sock +
       "' '" + g_run + "' && chmod 700 '" + g_sock + "' '" + g_run + "'");
}

/* The fake library's log lines of one process: "<pid> <ms> <event> <arg>". */
struct Ev { uint64_t ms; std::string what, arg; };
static std::vector<Ev> events_of(pid_t pid)
{
    std::vector<Ev> out;
    std::string lg = read_text(g_fake + "/log");
    size_t pos = 0;
    while (pos < lg.size()) {
        size_t nl = lg.find('\n', pos);
        std::string line = lg.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? lg.size() : nl + 1;
        int p = 0;
        unsigned long long ms = 0;
        char what[32] = "", arg[64] = "";
        if (sscanf(line.c_str(), "%d %llu %31s %63s", &p, &ms, what, arg) >= 3 && p == (int)pid)
            out.push_back({ (uint64_t)ms, what, arg });
    }
    return out;
}

static bool has_event(pid_t pid, const char *what)
{
    for (const Ev &e : events_of(pid))
        if (e.what == what)
            return true;
    return false;
}

static bool socket_exists(const std::string &name)
{
    struct stat st;
    return stat((g_sock + "/" + name).c_str(), &st) == 0;
}

/* The process's last "close" of a stick comes before its "mon_close". */
static bool closed_then_monitor(pid_t pid)
{
    uint64_t close_ms = 0, mon_ms = 0;
    size_t close_at = 0, mon_at = 0, i = 0;
    for (const Ev &e : events_of(pid)) {
        i++;
        if (e.what == "close") { close_ms = e.ms; close_at = i; }
        if (e.what == "mon_close") { mon_ms = e.ms; mon_at = i; }
    }
    return mon_at != 0 && close_at != 0 && close_at < mon_at && close_ms <= mon_ms;
}

/* Start with stdout a PIPE (not a tty, not a file): the B4 contract. */
static int run_piped(const std::vector<std::string> &args, const std::string &log, pid_t *pid_out)
{
    std::string script = "export LORAHAM_RUNTIME_DIR='" + g_run + "' LORAHAM_SOCKET_DIR='" + g_sock +
                         "' LORAHAM_FAKE_CH341='" + g_fake + "'\n"
                         "bash -c 'echo $$ > \"$0.pid\"; exec \"$@\"' '" + log + "' '" + g_bin + "'";
    for (const std::string &a : args)
        script += " '" + a + "'";
    script += " < /dev/null 2>&1 | cat > '" + log + "'\nexit ${PIPESTATUS[0]}\n";
    write_text(log + ".sh", script);
    int rc = sh("bash '" + log + ".sh'");
    *pid_out = (pid_t)atoi(read_text(log + ".pid").c_str());
    return rc;
}

/* Lines on a kept CONF client up to the first one starting with `prefix`
 * (returned; "" on timeout). Every RADIO= line read on the way goes to *radio. */
static std::string read_until(int fd, const char *prefix, int ms, std::vector<std::string> *radio)
{
    uint64_t end = now_ms() + (uint64_t)ms;
    while (now_ms() < end) {
        std::string l = read_line(fd, (int)(end - now_ms()));
        if (l.rfind("RADIO=", 0) == 0)
            radio->push_back(l);
        if (!l.empty() && l.rfind(prefix, 0) == 0)
            return l;
    }
    return "";
}

/* One command on a kept CONF client; its reply line (prefix), RADIO= lines to *radio. */
static std::string ask(int fd, const std::string &cmd, const char *prefix, std::vector<std::string> *radio)
{
    std::string line = cmd + "\n";
    if (write(fd, line.data(), line.size()) != (ssize_t)line.size())
        return "";
    return read_until(fd, prefix, 2000, radio);
}

static size_t log_count(const std::string &log, const std::string &part)
{
    std::string s = read_text(log);
    size_t n = 0;
    for (size_t at = s.find(part); at != std::string::npos; at = s.find(part, at + part.size()))
        n++;
    return n;
}

/* ---- scenarios ---------------------------------------------------------------- */

static void scenario_unplug_and_return(void)
{
    reset_dirs();
    std::string log = g_tmp + "/s1.log";
    bus("1-2 - 1\n");
    pid_t pid = start(mesh({ "--id", "ms1" }), log);
    const std::string C = "loraconf-ms1.sock";
    expect("S1 the stick without a serial: READY", wait_status(C, "RADIO=READY", 8000));
    std::string st = status(C);
    /* Changed guard (1.3.0): the applied FREQ= SF= POWER= follow; since 1.6.2 the
     * CAD parameters end the line (SF11/BW250 band default: AUTO). */
    expect("T21/BOUND READY: ... USBWAIT=- BOUND=-@1-2 ID=ms1, nothing applied yet",
           st.find(" USBWAIT=- BOUND=-@1-2 ID=ms1 RADIOERR=retry USBFAULT=0 FREQ=- SF=- POWER=-") != std::string::npos &&
           st.size() > 89 && st.substr(st.size() - 89) == "ID=ms1 RADIOERR=retry USBFAULT=0 FREQ=- SF=- POWER=- CADSYM=AUTO CADPEAK=AUTO CADMIN=AUTO");
    expect("S1 SET SF=9 POWER=5 applied", conf(C, "SET SF=9 POWER=5", "") == "OK");
    expect("S1 STATUS reports the applied SF and POWER, FREQ unset",
           status(C).find(" ID=ms1 RADIOERR=retry USBFAULT=0 FREQ=- SF=9 POWER=5") != std::string::npos);
    int keep_conf = connect_to(C);
    int raw = connect_to("lora-ms1.sock");
    int fr = connect_to("loraf-ms1.sock");
    bool txr = write(keep_conf, "SET TXRESULT=1\n", 15) == 15;
    std::string ok;
    for (int i = 0; txr && i < 10 && ok != "OK"; i++)
        ok = read_line(keep_conf, 1000);
    expect("S1 TXRESULT=1 on the CONF client", ok == "OK");

    /* T12: a TX in flight when the stick goes. The fake holds the request at
     * its TX-path entry (SetCad/SetTx) until the test releases it after the
     * unplug, so the request can only complete after the loss. */
    write_text(g_fake + "/txdelay", "1\n");
    framed_tx(fr, "inflight");
    expect("T12 the TX reached the chip and is held there (marker)", wait_log(g_fake + "/log", " txpath 1-2", 5000));
    expect("T12 no TX_RESULT while the request is held", framed_results(fr, 300).empty());
    bus("");                                          /* unplug */
    release("txdelay");
    expect("T11 RADIO=UNPLUGGED", wait_status(C, "RADIO=UNPLUGGED", 3000));
    expect("T1 the loss is classified gone (log names the signal)",
           wait_log(log, "[CH341] ELECROW Meshstick gone (", 3000));
    expect("T21 waiting: USBWAIT=1-2 BOUND=-", wait_status(C, " USBWAIT=1-2 BOUND=- ID=ms1", 3000));
    std::vector<int> r1 = framed_results(fr, 3000);
    if (r1.size() == 1)
        printf("       T12 the in-flight TX's status: %d\n", r1[0]);
    /* A failed TX (plan §3.4: "exactly one failed-TX result"): RADIO_ERROR or
     * RADIO_NOT_READY, never OK for the request that met the loss. */
    expect("T12 the in-flight TX: exactly one TX_RESULT, after the loss, a failure (RADIO_ERROR or RADIO_NOT_READY)",
           r1.size() == 1 && (r1[0] == FRAMED_DATA_TX_STATUS_RADIO_ERROR ||
                              r1[0] == FRAMED_DATA_TX_STATUS_RADIO_NOT_READY));
    expect("T12 the request was held until released (no watchdog)", held_until_released());
    expect("T12 the held request saw the loss on its release", log_has(g_fake + "/log", " txrelease 1-2 gone"));
    framed_tx(fr, "while-unplugged");
    std::vector<int> r2 = framed_results(fr, 2000);
    expect("T3 framed TX while UNPLUGGED: one TX_RESULT RADIO_NOT_READY (3)", r2.size() == 1 && r2[0] == 3);
    expect("T3 SET while UNPLUGGED: refused", conf(C, "SET SF=10", "ERR").rfind("ERR RADIO_NOT_READY", 0) == 0);
    expect("T1 the existing CONF, raw and framed clients stay connected",
           connected(keep_conf) && connected(raw) && connected(fr));
    int fresh = connect_to(C);
    expect("T1 a NEW client is accepted while UNPLUGGED", fresh >= 0);
    if (fresh >= 0)
        close(fresh);
    expect("T1 no exit while UNPLUGGED", wait_exit(pid, 500) == -2);

    /* Another port, then a serial stick on the stored port: ignored. */
    bus("1-3 - 2\n");
    expect("T5/D2 the stick on another port: the same-port line", wait_log(log, "replug into the same port", 4000));
    bus("1-2 SER00001 3\n");
    expect("T5 a serial stick on the stored port: refused (has serial)", wait_log(log, "has serial SER00001", 4000));
    bus("1-2 SER00002 5 unreadable\n");
    expect("DELTA-4-v3 a serial stick with an unreadable serial on the stored port: refused",
           wait_log(log, "could not be read (unreadable serial)", 4000));
    expect("T21 still waiting on 1-2, BOUND=-", wait_status(C, " USBWAIT=1-2 BOUND=- ID=ms1", 2000) &&
           status(C).find("RADIO=UNPLUGGED") != std::string::npos);

    /* BUSY: back on its port but claimed by someone else. */
    int hold = open((g_fake + "/claim-1-2").c_str(), O_RDWR | O_CREAT, 0600);
    expect("S1 the test holds the stick's claim", hold >= 0 && flock(hold, LOCK_EX | LOCK_NB) == 0);
    bus("1-2 - 6\n");
    expect("T21 BUSY: the not-claimable line, USBWAIT=1-2",
           wait_log(log, "present but pine64 could not select or open it", 5000) &&
           status(C).find(" USBWAIT=1-2 BOUND=- ") != std::string::npos);
    close(hold);                                      /* released, no hotplug event */
    expect("T4 the return: READY", wait_status(C, "RADIO=READY", 8000));
    expect("T4 the replay line", log_has(log, "[CH341] configuration replayed: SET SF=9 POWER=5"));
    expect("T4 STATUS after the replay: the applied SF and POWER kept",
           status(C).find(" ID=ms1 RADIOERR=retry USBFAULT=0 FREQ=- SF=9 POWER=5") != std::string::npos);
    expect("T21/BOUND back: USBWAIT=- BOUND=-@1-2", status(C).find(" USBWAIT=- BOUND=-@1-2 ID=ms1") != std::string::npos);
    expect("S1 clients still connected after the return", connected(keep_conf) && connected(raw) && connected(fr));

    close(keep_conf);
    close(raw);
    close(fr);
    expect("F4 SIGTERM: exit 0", term(pid) == 0);
    std::string lg = read_text(g_fake + "/log");
    size_t c = lg.rfind(" close 1-2"), mc = lg.rfind(" mon_close");
    expect("F4 the monitor is closed at shutdown, after the stick", mc != std::string::npos &&
           c != std::string::npos && c < mc);
}

static void scenario_io_fault(void)
{
    reset_dirs();
    std::string log = g_tmp + "/s2.log";
    bus("1-2 SERIAL01 1\n");
    pid_t pid = start(mesh({ "--radio-error", "exit" }), log);
    expect("S2 READY", wait_status("loraconf868.sock", "RADIO=READY", 8000));
    expect("O1/O2 --radio-error exit on a USB stick: RADIOERR=exit USBFAULT=0",
           status("loraconf868.sock").find(" ID=- RADIOERR=exit USBFAULT=0 FREQ=") != std::string::npos);
    bus("1-2 SERIAL01 1 ioerr\n");                    /* same connection, transfers fail */
    sleep_ms(200);                                    /* past the library's 20 ms bus cache */
    conf("loraconf868.sock", "SET SF=10", "");
    int rc = wait_exit(pid, 5000);
    expect("T2 an I/O error with the stick present: the real process exits 5", rc == 5);
    expect("T2 the log says present", log_has(log, "USB I/O error with the ELECROW Meshstick present"));
    expect("F2(r5) the exit 5 closes the stick, then the monitor", closed_then_monitor(pid));
    if (rc == -2)
        stop(pid);

    /* r19 (core r14 F1): lost events (an unrelated overflow) while the stick
     * is present are not its LEFT: a later I/O fault still goes through the
     * presence check and exits 5, never "gone" and a reopen. */
    reset_dirs();
    log = g_tmp + "/s2b.log";
    bus("1-2 SERIAL01 1\n");
    pid = start(mesh({ "--radio-error", "exit" }), log);
    expect("r19 READY", wait_status("loraconf868.sock", "RADIO=READY", 8000));
    write_text(g_fake + "/overflow", "1\n");
    expect("r19 the fake reported lost events (marker)", wait_log(g_fake + "/log", " overflow", 3000));
    bus("1-2 SERIAL01 1 ioerr\n");
    sleep_ms(200);
    conf("loraconf868.sock", "SET SF=10", "");
    rc = wait_exit(pid, 5000);
    expect("r19 lost events, then an I/O fault with the stick present: the presence check, exit 5", rc == 5 &&
           log_has(log, "USB I/O error with the ELECROW Meshstick present"));
    expect("r19 ... never classified gone by a LEFT", !log_has(log, "ELECROW Meshstick gone (LEFT event"));
    if (rc == -2)
        stop(pid);

    /* DELTA-6 seam (the selection side of row 99 through the real binary):
     * events lost during the bind's window make the selection undo the bind
     * (no commit): the stick is closed and opened again, then READY. */
    reset_dirs();
    log = g_tmp + "/s2c.log";
    bus("1-2 SERIAL01 1\n");
    write_text(g_fake + "/binddelay", "1\n");
    pid = start(mesh({}), log);
    expect("r19 seam: the bind is paused (marker)", wait_log(g_fake + "/log", " bindwait", 5000));
    write_text(g_fake + "/overflow", "1\n");        /* events lost during the bind */
    release("binddelay");
    expect("r19 seam: then READY", wait_status("loraconf868.sock", "RADIO=READY", 8000));
    int opens = 0;
    for (const Ev &e : events_of(pid))
        if (e.what == "open")
            opens++;
    expect("r19 seam: lost events during the bind: not committed (closed, opened again)", opens >= 2 &&
           has_event(pid, "close") && has_event(pid, "overflow"));
    expect("r19 seam: the bind held until released (no watchdog)", held_until_released());
    term(pid);
}

static void scenario_reinit_failures(void)
{
    const char *flags[2] = { "badchip", "failafterrx" };
    const char *line[2] = { "re-init after the replug failed", "replay of the applied configuration failed" };
    for (int k = 0; k < 2; k++) {
        reset_dirs();
        std::string log = g_tmp + "/s3-" + std::to_string(k) + ".log";
        bus("1-2 SERIAL01 1\n");
        pid_t pid = start(mesh({ "--radio-error", "exit" }), log);
        wait_status("loraconf868.sock", "RADIO=READY", 8000);
        conf("loraconf868.sock", "SET SF=9", "");
        bus("");
        wait_status("loraconf868.sock", "RADIO=UNPLUGGED", 3000);
        bus(std::string("1-2 SERIAL01 2 ") + flags[k] + "\n");
        int rc = wait_exit(pid, 8000);
        expect(std::string("T6 ") + flags[k] + " on the return: exit 5", rc == 5);
        expect(std::string("T6 ") + flags[k] + ": the log line", log_has(log, line[k]));
        expect(std::string("F2(r5) ") + flags[k] + ": the exit 5 closes the stick, then the monitor",
               closed_then_monitor(pid));
        if (rc == -2)
            stop(pid);
    }
}

static void scenario_access(void)
{
    reset_dirs();
    std::string log = g_tmp + "/s4a.log";
    bus("1-2 SERIAL01 1 access\n");
    pid_t pid = start(mesh({}), log);
    int rc = wait_exit(pid, 8000);
    expect("T16 ACCESS at start: exit 4 (never 5)", rc == 4);
    expect("T16 ... with the no-permission line", log_has(log, "[CH341] refused: no permission"));
    expect("F4 an ACCESS refusal at start closes the monitor and removes the sockets",
           has_event(pid, "mon_close") && !socket_exists("loraconf868.sock"));
    if (rc == -2)
        stop(pid);

    reset_dirs();
    log = g_tmp + "/s4b.log";
    bus("1-2 SERIAL01 1\n");
    pid = start(mesh({}), log);
    wait_status("loraconf868.sock", "RADIO=READY", 8000);
    bus("");
    wait_status("loraconf868.sock", "RADIO=UNPLUGGED", 3000);
    bus("1-2 SERIAL01 2 access\n");
    expect("T13 ACCESS on a reopen: the line after its 2 s, still running",
           wait_log(log, "no permission to open stick SERIAL01", 6000) && wait_exit(pid, 300) == -2);
    bus("1-2 SERIAL01 3\n");                          /* a new ARRIVED, udev done */
    expect("T13 ... a new ARRIVED starts over and succeeds", wait_status("loraconf868.sock", "RADIO=READY", 6000));
    expect("S4 SIGTERM exit 0", term(pid) == 0);
}

static void scenario_instances(void)
{
    reset_dirs();
    bus("1-2 AAAA0001 1\n1-3 BBBB0002 1\n1-4 CCCC0003 1\n");
    pid_t a = start(mesh({ "--id", "r3", "--usb-serial", "AAAA0001" }), g_tmp + "/s5a.log");
    expect("T19 r3 READY", wait_status("loraconf-r3.sock", "RADIO=READY", 8000));
    pid_t b = start(mesh({ "--id", "r3f", "--usb-serial", "BBBB0002" }), g_tmp + "/s5b.log");
    expect("T19 r3f READY", wait_status("loraconf-r3f.sock", "RADIO=READY", 8000));
    pid_t c = start(mesh({ "--usb-serial", "CCCC0003" }), g_tmp + "/s5c.log");
    expect("T19 no id READY", wait_status("loraconf868.sock", "RADIO=READY", 8000));
    const char *names[] = { "lora-r3.sock", "loraf-r3.sock", "loraconf-r3.sock", "lora-r3f.sock",
                            "loraf-r3f.sock", "loraconf-r3f.sock", "lora868.sock", "lora868f.sock",
                            "loraconf868.sock" };
    bool all = true;
    for (const char *n : names) {
        struct stat st;
        all = all && stat((g_sock + "/" + n).c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
    }
    expect("T19 nine distinct sockets", all);
    expect("T19 CONF answers on each: ID=r3 / ID=r3f / ID=-",
           status("loraconf-r3.sock").find(" BOUND=AAAA0001@1-2 ID=r3") != std::string::npos &&
           status("loraconf-r3f.sock").find(" BOUND=BBBB0002@1-3 ID=r3f") != std::string::npos &&
           status("loraconf868.sock").find(" BOUND=CCCC0003@1-4 ID=-") != std::string::npos);

    pid_t d = start(mesh({ "--id", "r3", "--usb-serial", "BBBB0002" }), g_tmp + "/s5d.log");
    int rc = wait_exit(d, 8000);
    expect("T19 a duplicate --id: exit 3", rc == 3);
    std::string lg = read_text(g_fake + "/log");
    std::string mine = std::to_string(d) + " ";
    expect("T19 ... before any USB open (no monitor, no open by that process)",
           lg.find("\n" + mine) == std::string::npos && lg.rfind(mine, 0) != 0);
    if (rc == -2)
        stop(d);

    pid_t e = start(mesh({ "--id", "ms2", "--usb-serial", "AAAA0001" }), g_tmp + "/s5e.log");
    rc = wait_exit(e, 8000);
    expect("T22 a second id on a claimed stick: exit 4", rc == 4);
    expect("T22 ... with the BUSY: line", log_has(g_tmp + "/s5e.log", "[CH341] refused: BUSY: stick AAAA0001"));
    if (rc == -2)
        stop(e);
    std::string s3 = status("loraconf-r3.sock");
    expect("T22 the first undisturbed: READY, still bound",
           s3.find("RADIO=READY") != std::string::npos && s3.find(" BOUND=AAAA0001@1-2 ") != std::string::npos);
    expect("T22 the first still running", wait_exit(a, 200) == -2);
    term(a);
    term(b);
    term(c);
}

static int hold_claim(const char *port)
{
    int fd = open((g_fake + "/claim-" + port).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        fd = -1;
    }
    return fd;
}

/* B4 / §13: every start class that ends in the BUSY branch exits 4 at once with
 * the BUSY: line through a PIPE; F4: the refusal runs the startup cleanup
 * (monitor closed, sockets removed). */
static void scenario_b4_classes(void)
{
    struct Case { const char *name, *bus, *held; std::vector<std::string> args; const char *who; };
    const Case cases[] = {
        { "a --usb-serial start", "1-2 SERB0001 1\n", "1-2", { "--usb-serial", "SERB0001" }, "SERB0001" },
        { "a no-selector start on its sole stick", "1-2 - 1\n", "1-2", {}, "1-2" },
        { "a Rule B start (several sticks, the one without a serial held)", "1-2 SERB0001 1\n1-3 - 1\n", "1-3", {},
          "1-3" },
    };
    for (const Case &c : cases) {
        reset_dirs();
        bus(c.bus);
        int hold = hold_claim(c.held);
        std::vector<std::string> args = { "--radio", "868", "--hw", "meshstick-1262" };
        args.insert(args.end(), c.args.begin(), c.args.end());
        std::string log = g_tmp + "/b4.log";
        pid_t pid = 0;
        int rc = run_piped(args, log, &pid);
        std::string want = std::string("[CH341] refused: BUSY: stick ") + c.who +
                           " present but pine64 could not select or open it (another process, a bound kernel "
                           "driver, or an unreadable serial) (exit 4).";
        expect(std::string("B4 ") + c.name + ": exit 4", rc == 4);
        expect(std::string("B4 ") + c.name + ": the exact BUSY: line through the pipe", log_has(log, want));
        expect(std::string("F4 ") + c.name + ": the monitor closed before the exit", has_event(pid, "mon_close"));
        expect(std::string("F4 ") + c.name + ": the sockets removed", !socket_exists("loraconf868.sock") &&
               !socket_exists("lora868.sock") && !socket_exists("lora868f.sock"));
        if (hold >= 0)
            close(hold);
    }
}

/* T15b: a --usb-serial daemon reopening while another daemon's Rule B walks
 * keep holding its stick opens it inside a walk gap (the gap is back-off,
 * R22), while the walking daemon is still waiting. */
static void scenario_gap(void)
{
    reset_dirs();
    bus("1-3 - 1\n");
    pid_t a = start(mesh({ "--id", "ga" }), g_tmp + "/ga.log");
    expect("gap: daemon A (the stick without a serial) READY", wait_status("loraconf-ga.sock", "RADIO=READY", 8000));
    bus("");                                          /* A: UNPLUGGED */
    wait_status("loraconf-ga.sock", "RADIO=UNPLUGGED", 3000);
    bus("1-2 SERB0001 1\n");
    pid_t b = start(mesh({ "--id", "gb", "--usb-serial", "SERB0001" }), g_tmp + "/gb.log");
    expect("gap: daemon B (--usb-serial) READY", wait_status("loraconf-gb.sock", "RADIO=READY", 8000));
    int hold = hold_claim("1-3");                     /* A's own stick held elsewhere: A walks, BUSY */
    bus("1-2 SERB0001 1\n1-3 - 2\n");
    expect("gap: A walks and waits (BUSY)", wait_log(g_tmp + "/ga.log", "present but pine64 could not select", 6000));
    uint64_t t_replug = now_ms();
    bus("1-3 - 2\n");                                /* B loses its stick ... */
    wait_status("loraconf-gb.sock", "RADIO=UNPLUGGED", 3000);
    bus("1-2 SERB0001 2\n1-3 - 2\n");               /* ... and it comes back while A walks */
    expect("gap: B READY again while A keeps walking", wait_status("loraconf-gb.sock", "RADIO=READY", 10000) &&
           status("loraconf-ga.sock").find("RADIO=UNPLUGGED") != std::string::npos);
    /* B's open of 1-2 lies inside one of A's gaps: A's walks end with pine64
     * finding nothing ("open-none"); B opened less than the 1 s gap after the
     * last of them, and A was still waiting. */
    uint64_t b_open = 0;
    for (const Ev &e : events_of(b))
        if (e.what == "open" && e.arg == "1-2" && e.ms >= t_replug)
            b_open = e.ms;
    uint64_t walk_end = 0;
    int walks = 0;
    for (const Ev &e : events_of(a))
        if (e.what == "open-none") {
            walks++;
            if (e.ms <= b_open)
                walk_end = e.ms;
        }
    expect("gap: A walked (" + std::to_string(walks) + " walks ended with nothing claimable)", walks >= 1);
    expect("gap: B opened inside A's gap (" + std::to_string(b_open - walk_end) + " ms after A's walk ended)",
           b_open && walk_end && b_open >= walk_end && b_open - walk_end < 1000);
    if (hold >= 0)
        close(hold);
    term(a);
    term(b);
}

/* Core r6 B1 on the real binary: a duplicate --usb-serial stick arriving
 * while the start selection runs makes it classify again: not unique. */
static void scenario_start_arrival(void)
{
    reset_dirs();
    std::string log = g_tmp + "/s9.log";
    bus("1-2 SERIAL01 1\n");
    write_text(g_fake + "/scandelay", "1\n");
    pid_t pid = start(mesh({ "--usb-serial", "SERIAL01" }), log);
    expect("B1 start: the first scan step is paused (marker)", wait_log(g_fake + "/log", " scanwait", 5000));
    bus("1-2 SERIAL01 1\n1-3 SERIAL01 1\n");        /* a duplicate arrives during the start selection */
    release("scandelay");
    int rc = wait_exit(pid, 8000);
    expect("B1 start: a duplicate serial arriving during the start selection: exit 4", rc == 4);
    expect("B1 start: ... refused not unique", log_has(log, "[CH341] refused: serial SERIAL01 is not unique"));
    expect("B1 start: the scan step held until released (no watchdog)", held_until_released());
    if (rc == -2)
        stop(pid);

    /* r10 (core r7 B1): a duplicate arriving during the blocking pine64 open
     * (pinedio_init) is taken before the open is committed: never READY. */
    reset_dirs();
    log = g_tmp + "/s9b.log";
    bus("1-2 SERIAL01 1\n");
    write_text(g_fake + "/opendelay", "1\n");
    pid = start(mesh({ "--usb-serial", "SERIAL01" }), log);
    expect("r10 start: the pine64 open is paused after its claim (marker)", wait_log(g_fake + "/log", " openwait", 5000));
    bus("1-2 SERIAL01 1\n1-3 SERIAL01 1\n");        /* a duplicate arrives during pinedio_init */
    release("opendelay");
    rc = wait_exit(pid, 8000);
    expect("r10 start: a duplicate serial arriving during the open: exit 4 (a start refusal: never READY)", rc == 4);
    expect("r10 start: ... refused not unique", log_has(log, "[CH341] refused: serial SERIAL01 is not unique"));
    expect("r10 start: ... the stick opened during the window closed again", has_event(pid, "close"));
    expect("r10 start: the open held until released (no watchdog)", held_until_released());
    if (rc == -2)
        stop(pid);

    /* r12 (core r8 B2): a duplicate LEAVING during the start selection makes
     * the wanted stick unique: classified again, READY (was: exit 4 "not
     * unique" on the old topology). */
    reset_dirs();
    log = g_tmp + "/s9c.log";
    bus("1-2 SERIAL01 1\n1-3 SERIAL01 1\n");
    write_text(g_fake + "/scandelay", "1\n");
    pid = start(mesh({ "--usb-serial", "SERIAL01" }), log);
    expect("r12 start: the first scan step is paused (marker)", wait_log(g_fake + "/log", " scanwait", 5000));
    bus("1-2 SERIAL01 1\n");                        /* the duplicate on 1-3 is unplugged */
    release("scandelay");
    expect("r12 start: a duplicate leaving during the start selection: READY on the remaining stick",
           wait_status("loraconf868.sock", "RADIO=READY", 8000) &&
           status("loraconf868.sock").find("BOUND=SERIAL01@1-2") != std::string::npos);
    expect("r12 start: ... no refusal", !log_has(log, "refused"));
    expect("r12 start: the scan step held until released (no watchdog)", held_until_released());
    term(pid);

    /* r13 (core r9 B1): a duplicate arriving during the bind's own device-list
     * read is taken before anything is committed: never READY. */
    reset_dirs();
    log = g_tmp + "/s9d.log";
    bus("1-2 SERIAL01 1\n");
    write_text(g_fake + "/binddelay", "1\n");
    pid = start(mesh({ "--usb-serial", "SERIAL01" }), log);
    expect("r13 start: the bind is paused (marker)", wait_log(g_fake + "/log", " bindwait", 5000));
    bus("1-2 SERIAL01 1\n1-3 SERIAL01 1\n");        /* a duplicate arrives during the bind */
    release("binddelay");
    rc = wait_exit(pid, 8000);
    expect("r13 start: a duplicate arriving during the bind: exit 4 not unique (never READY)", rc == 4 &&
           log_has(log, "[CH341] refused: serial SERIAL01 is not unique"));
    expect("r13 start: the bind held until released (no watchdog)", held_until_released());
    if (rc == -2)
        stop(pid);
}

static void scenario_cli(void)
{
    reset_dirs();
    pid_t p = start({ "--radio", "868", "--hw", "waveshare-sx1262", "--usb-serial", "AAAA0001" }, g_tmp + "/s6a.log");
    int rc = wait_exit(p, 8000);
    expect("T23 --usb-serial on a Pi preset: exit 1", rc == 1);
    expect("T23 ... with the reason, naming every USB stick preset",
           log_has(g_tmp + "/s6a.log", "--usb-serial / --usb-port are only for a USB stick preset "
                                       "(meshstick-1262, pinedio-usb-1262, ch341-rfm98pw-433)"));
    if (rc == -2)
        stop(p);
    p = start(mesh({ "--usb-serial", "A B" }), g_tmp + "/s6b.log");
    rc = wait_exit(p, 8000);
    expect("T23 a malformed --usb-serial: exit 1", rc == 1);
    expect("T23 ... with the reason", log_has(g_tmp + "/s6b.log", "invalid --usb-serial"));
    if (rc == -2)
        stop(p);
    p = start({ "--radio", "868", "--hw", "waveshare-sx1262", "--usb-port", "1-2" }, g_tmp + "/s6c.log");
    rc = wait_exit(p, 8000);
    expect("T23 --usb-port on a Pi preset: exit 1", rc == 1);
    expect("T23 ... with the reason", log_has(g_tmp + "/s6c.log", "are only for a USB stick preset"));
    if (rc == -2)
        stop(p);
    p = start(mesh({ "--usb-port", "1-x" }), g_tmp + "/s6d.log");
    rc = wait_exit(p, 8000);
    expect("T23 a malformed --usb-port: exit 1", rc == 1);
    expect("T23 ... with the reason", log_has(g_tmp + "/s6d.log", "invalid --usb-port"));
    if (rc == -2)
        stop(p);
    p = start(mesh({ "--usb-port", "1-2", "--usb-serial", "AAAA0001" }), g_tmp + "/s6e.log");
    rc = wait_exit(p, 8000);
    expect("T23 --usb-port with --usb-serial: exit 1", rc == 1);
    expect("T23 ... with the reason", log_has(g_tmp + "/s6e.log", "--usb-serial and --usb-port exclude each other"));
    if (rc == -2)
        stop(p);
}

/* The process's events of one kind, in order. */
static std::vector<uint64_t> event_times(pid_t pid, const char *what)
{
    std::vector<uint64_t> t;
    for (const Ev &e : events_of(pid))
        if (e.what == what)
            t.push_back(e.ms);
    return t;
}

/* O1: --radio-error on the real binary. */
static void scenario_radio_error_cli(void)
{
    reset_dirs();
    pid_t p = start({ "--help" }, g_tmp + "/o1h.log");
    expect("O1 --help: exit 0 and the option row", wait_exit(p, 8000) == 0 &&
           log_has(g_tmp + "/o1h.log", "--radio-error MODE  on a radio I/O error after start: retry, exit"));
    p = start(mesh({ "--radio-error", "foo" }), g_tmp + "/o1a.log");
    int rc = wait_exit(p, 8000);
    expect("O1 an unknown value: the usage error (exit 1) with the reason and the usage text", rc == 1 &&
           log_has(g_tmp + "/o1a.log", "invalid --radio-error: foo") &&
           log_has(g_tmp + "/o1a.log", "allowed: retry, exit") && log_has(g_tmp + "/o1a.log", "Usage:"));
    if (rc == -2)
        stop(p);
    p = start(mesh({ "--radio-error" }), g_tmp + "/o1b.log");
    rc = wait_exit(p, 8000);
    expect("O1 a missing value: the usage error (exit 1)", rc == 1 && log_has(g_tmp + "/o1b.log", "Usage:"));
    if (rc == -2)
        stop(p);
    p = start({ "--radio", "868", "--hw", "waveshare-sx1262", "--radio-error", "retry" }, g_tmp + "/o1c.log");
    rc = wait_exit(p, 8000);
    expect("O1 explicit retry on an SPI preset: refused at parse (exit 1), before any I/O", rc == 1 &&
           log_has(g_tmp + "/o1c.log", "--radio-error retry is only for a USB stick preset") &&
           !log_has(g_tmp + "/o1c.log", "[SPI]") && !socket_exists("loraconf868.sock"));
    if (rc == -2)
        stop(p);
    /* An SPI board cannot come up here (no gpiochip: exit 4, as test_conf_status
     * skips): "accepted" = past the parse, to the instance lock, no usage error.
     * Its RADIOERR=exit USBFAULT=- is the controller default (test_config_dispatch). */
    p = start({ "--radio", "433", "--radio-error", "EXIT" }, g_tmp + "/o1d.log");
    rc = wait_exit(p, 8000);
    expect("O1 explicit EXIT (any case) on an SPI preset: accepted (past the parse, no usage error)",
           (rc == -2 || rc == 4) && wait_log(g_tmp + "/o1d.log", "instance lock acquired", 1000) &&
           !log_has(g_tmp + "/o1d.log", "Usage:"));
    if (rc == -2)
        term(p);
    p = start({ "--radio", "433" }, g_tmp + "/o1e.log");
    rc = wait_exit(p, 8000);
    expect("O1 no option on an SPI preset (B1): accepted (past the parse, no usage error)",
           (rc == -2 || rc == 4) && wait_log(g_tmp + "/o1e.log", "instance lock acquired", 1000) &&
           !log_has(g_tmp + "/o1e.log", "Usage:"));
    if (rc == -2)
        term(p);
    bus("1-2 SERIAL01 1\n");
    p = start(mesh({ "--radio-error", "RETRY" }), g_tmp + "/o1f.log");
    expect("O1 RETRY (any case) on a USB stick: RADIOERR=retry",
           wait_status("loraconf868.sock", " ID=- RADIOERR=retry USBFAULT=0 FREQ=", 8000));
    term(p);
}

/* T1/T1b/T7/T8: a present-path fault in the default mode (retry). */
static void scenario_retry_fault(void)
{
    reset_dirs();
    std::string log = g_tmp + "/r1.log";
    const std::string C = "loraconf868.sock";
    bus("1-2 SERIAL01 1\n");
    pid_t pid = start(mesh({}), log);
    expect("O1/O2 no option on a USB stick: READY, RADIOERR=retry USBFAULT=0",
           wait_status(C, "RADIO=READY", 8000) &&
           status(C).find(" ID=- RADIOERR=retry USBFAULT=0 FREQ=") != std::string::npos);
    expect("T1 SET SF=9 applied", conf(C, "SET SF=9", "") == "OK");
    int fr = connect_to("lora868f.sock");
    int keep_conf = connect_to(C);                    /* TX_RESULT frames while this client has TXRESULT=1 */
    std::string ok;
    bool txr = write(keep_conf, "SET TXRESULT=1\n", 15) == 15;
    for (int i = 0; txr && i < 10 && ok != "OK"; i++)
        ok = read_line(keep_conf, 1000);
    expect("T8 TXRESULT=1 on a kept CONF client", ok == "OK");
    bus("1-2 SERIAL01 1 ioerr\n");                    /* same connection, transfers fail */
    sleep_ms(200);                                    /* past the library's 20 ms bus cache */
    conf(C, "SET SF=10", "");
    expect("T1 the fault line: closing it, retry in 1 s (fault 1)",
           wait_log(log, "[CH341] ELECROW Meshstick: USB I/O error with the stick present: closing it, "
                         "retry in 1 s (fault 1)", 3000));
    expect("T7 during the retry: RADIO=UNPLUGGED USBFAULT=1", wait_status(C, "RADIO=UNPLUGGED", 2000) &&
           status(C).find(" RADIOERR=retry USBFAULT=1 ") != std::string::npos);
    write_text(g_fake + "/txdelay", "1\n");          /* a TX reaching the chip would log txpath */
    framed_tx(fr, "during-retry");
    std::vector<int> r = framed_results(fr, 2000);
    expect("T8 framed TX during the retry: RADIO_NOT_READY (3)", r.size() == 1 && r[0] == 3);
    if (!(r.size() == 1 && r[0] == 3))
        printf("       T8 results: %zu, first %d\n", r.size(), r.empty() ? -1 : r[0]);
    expect("T8 ... no radio I/O for it reached the fake", !log_has(g_fake + "/log", " txpath "));
    release("txdelay");
    expect("T8 SET during the retry: refused", conf(C, "SET SF=11", "ERR").rfind("ERR RADIO_NOT_READY", 0) == 0);
    expect("T1 attempt 1 (a re-init on the failing stick): closed again, next attempt in 2 s",
           wait_log(log, "ELECROW Meshstick: attempt 1: re-init failed: closing it, next attempt in 2 s", 4000));
    bus("1-2 SERIAL01 1\n");                          /* the I/O works again, no USB event */
    expect("T1 back after 2 attempts: READY, the same pid",
           wait_log(log, "ELECROW Meshstick back after 2 attempts: RADIO=READY", 5000) &&
           wait_status(C, "RADIO=READY", 2000) && wait_exit(pid, 0) == -2);
    expect("T7 after: USBFAULT stays 1, the replay applied (SF=9)",
           status(C).find(" RADIOERR=retry USBFAULT=1 FREQ=- SF=9 ") != std::string::npos &&
           log_has(log, "[CH341] configuration replayed: SET SF=9"));
    /* opens: the start, then attempts 1-2; closes: the fault teardown, then
     * the one after the failed re-init. Each re-open waits its step after the
     * close before it. */
    std::vector<uint64_t> cl = event_times(pid, "close"), op = event_times(pid, "open");
    bool gaps = cl.size() >= 2 && op.size() >= 3 && op[1] >= cl[0] + 1000 && op[2] >= cl[1] + 2000;
    expect("T1b/T2 re-open >= 1 s after the fault's close, then >= 2 s after the next close", gaps);
    if (gaps)
        printf("       re-open after close: %llu, %llu ms\n", (unsigned long long)(op[1] - cl[0]),
               (unsigned long long)(op[2] - cl[1]));
    close(fr);
    close(keep_conf);
    expect("T1 SIGTERM: exit 0", term(pid) == 0);
}

/* T3: a failed re-init after a plain unplug is retried (the default). */
static void scenario_retry_reinit(void)
{
    reset_dirs();
    std::string log = g_tmp + "/r3.log";
    const std::string C = "loraconf868.sock";
    bus("1-2 SERIAL01 1\n");
    pid_t pid = start(mesh({}), log);
    wait_status(C, "RADIO=READY", 8000);
    bus("");
    expect("T3 unplugged; the immediate selection (attempt 1) finds no stick",
           wait_status(C, "RADIO=UNPLUGGED", 3000) &&
           wait_log(log, "no ELECROW Meshstick with serial SERIAL01; waiting", 3000));
    bus("1-2 SERIAL01 2 badchip\n");
    expect("T3 the re-init on the return (attempt 2) fails: closing it, next attempt in 1 s, no exit",
           wait_log(log, "ELECROW Meshstick: attempt 2: re-init failed: closing it, next attempt in 1 s", 5000) &&
           wait_exit(pid, 0) == -2);
    expect("T3 during: UNPLUGGED, not a present-path fault (USBFAULT=0)", wait_status(C, "RADIO=UNPLUGGED", 2000) &&
           status(C).find(" USBFAULT=0 ") != std::string::npos);
    bus("1-2 SERIAL01 2\n");                          /* the same connection, the chip answers now */
    expect("T3 the next timed attempt: back after 3 attempts, READY",
           wait_log(log, "ELECROW Meshstick back after 3 attempts: RADIO=READY", 5000) &&
           wait_status(C, "RADIO=READY", 2000));
    term(pid);
}

/* 1.6.0: RADIO=<state> on CONF, through the real unplug, re-init and replay. */
static void scenario_radio_lines(void)
{
    reset_dirs();
    std::string log = g_tmp + "/rl.log";
    const std::string C = "loraconf868.sock";
    const std::vector<std::string> both = { "RADIO=UNPLUGGED", "RADIO=READY" };
    bus("1-2 SERIAL01 1\n");
    pid_t pid = start(mesh({}), log);
    expect("RL READY", wait_status(C, "RADIO=READY", 8000));
    int a = connect_to(C), b = connect_to(C);
    std::vector<std::string> ra, rb, rc, rd;
    expect("RL two CONF clients: no line at connect, GET STATUS says READY",
           ask(a, "GET STATUS", "STATUS", &ra).rfind("STATUS RADIO=READY ", 0) == 0 &&
           ask(b, "GET STATUS", "STATUS", &rb).rfind("STATUS RADIO=READY ", 0) == 0 && ra.empty() && rb.empty());
    bus("");                                          /* unplug */
    expect("RL unplug: RADIO=UNPLUGGED to both clients",
           read_until(a, "RADIO=", 3000, &ra) == "RADIO=UNPLUGGED" &&
           read_until(b, "RADIO=", 3000, &rb) == "RADIO=UNPLUGGED");
    expect("RL the line's value equals GET STATUS's RADIO=",
           ask(a, "GET STATUS", "STATUS", &ra).rfind("STATUS RADIO=UNPLUGGED ", 0) == 0);
    int c = connect_to(C);                            /* after the change was sent */
    expect("RL a client connected after the change: no old line, GET STATUS says UNPLUGGED",
           ask(c, "GET STATUS", "STATUS", &rc).rfind("STATUS RADIO=UNPLUGGED ", 0) == 0 && rc.empty());
    bus("1-2 SERIAL01 2 badchip\n");
    expect("RL the re-init on the return fails (retry mode: FAILED, then UNPLUGGED in the pass)",
           wait_log(log, "attempt 2: re-init failed: closing it, next attempt in 1 s", 5000));
    bus("1-2 SERIAL01 2\n");
    expect("RL the next attempt (re-init + replay): RADIO=READY to all three clients",
           read_until(a, "RADIO=READY", 6000, &ra) != "" && read_until(b, "RADIO=READY", 6000, &rb) != "" &&
           read_until(c, "RADIO=READY", 6000, &rc) != "");
    std::string st;
    for (int i = 0; i < 20; i++)                      /* 20 round trips: 20+ loop passes, READY throughout */
        st = ask(a, "GET STATUS", "STATUS", &ra);
    ask(b, "GET STATUS", "STATUS", &rb);
    ask(c, "GET STATUS", "STATUS", &rc);
    expect("RL READY held: GET STATUS says READY", st.rfind("STATUS RADIO=READY ", 0) == 0);
    expect("RL exactly UNPLUGGED, READY on each client, in order: no FAILED/UNINITIALIZED line, none without a change",
           ra == both && rb == both && rc == std::vector<std::string>{ "RADIO=READY" });
    if (ra != both || rb != both)
        for (const std::string &l : ra)
            printf("       RL client a: %s\n", l.c_str());

    /* A change with no CONF client connected: stored all the same. The gone
     * line is printed in the pass whose tick sees UNPLUGGED (on_loss stores
     * it first); a client connected after that line gets no line. */
    close(a);
    close(b);
    close(c);
    bus("");
    expect("RL unplug with no CONF client: the gone line",
           [&] { uint64_t end = now_ms() + 3000;
                 while (log_count(log, "ELECROW Meshstick gone (") < 2 && now_ms() < end)
                     sleep_ms(50);
                 return log_count(log, "ELECROW Meshstick gone (") >= 2; }());
    int d = connect_to(C);
    expect("RL a client connected after a change made with no client: no old line",
           ask(d, "GET STATUS", "STATUS", &rd).rfind("STATUS RADIO=UNPLUGGED ", 0) == 0 && rd.empty());
    close(d);
    term(pid);
}

/* T6: the library refuses the close: the handle is kept, nothing new opened. */
static void scenario_retry_kept(void)
{
    reset_dirs();
    std::string log = g_tmp + "/r6.log";
    const std::string C = "loraconf868.sock";
    bus("1-2 SERIAL01 1\n");
    pid_t pid = start(mesh({}), log);
    wait_status(C, "RADIO=READY", 8000);
    bus("1-2 SERIAL01 1 ioerr closefail\n");
    sleep_ms(200);
    conf(C, "SET SF=10", "");
    expect("T6 the close is refused (fake marker)", wait_log(g_fake + "/log", " closefail 1-2", 3000));
    expect("T6 attempts 1 and 2: the library kept the old handle: restart the daemon",
           wait_log(log, "attempt 1: the library kept the old handle: restart the daemon (next attempt in 2 s)", 3000) &&
           wait_log(log, "attempt 2: the library kept the old handle: restart the daemon (next attempt in 4 s)", 4000));
    expect("T6 no new handle opened after the refused close (the start's open only)",
           event_times(pid, "open").size() == 1);
    expect("T6 the same pid, UNPLUGGED", wait_exit(pid, 0) == -2 &&
           status(C).find("RADIO=UNPLUGGED") != std::string::npos);
    term(pid);
}

/* Q1-Q4 and T5 (1.5.0, the binary test_ch341_stop): QUIT / EXIT on the CONF
 * socket and the stop during a USB retry. The stop is seen between main-loop
 * steps, so "within the bound" = the exit follows the end of the step that
 * runs when the stop comes (a re-init with its replay, an open) within
 * 500 ms; outside a step (the backoff wait) the exit comes before the next
 * attempt. The test and the fake share CLOCK_MONOTONIC; the times are printed
 * (docs/limits.md). */

/* The fake's events of `what` of `pid` after `after_ms`. */
static std::vector<uint64_t> events_after(pid_t pid, const char *what, uint64_t after_ms)
{
    std::vector<uint64_t> t;
    for (uint64_t ms : event_times(pid, what))
        if (ms > after_ms)
            t.push_back(ms);
    return t;
}

static void scenario_quit(void)
{
    const char *verbs[4] = { "QUIT", "quit", "EXIT", "exit" };
    for (int i = 0; i < 4; i++) {
        reset_dirs();
        bus("1-2 SERIAL01 1\n");
        pid_t pid = start(mesh({}), g_tmp + "/q1.log");
        wait_status("loraconf868.sock", "RADIO=READY", 8000);
        int raw = connect_to("lora868.sock");
        int c = connect_to("loraconf868.sock");
        std::string line = std::string(verbs[i]) + "\n";
        std::string reply = write(c, line.c_str(), line.size()) == (ssize_t)line.size() ? read_line(c, 2000) : "";
        uint64_t t0 = now_ms();
        int rc = wait_exit(pid, 10000);
        uint64_t ms = now_ms() - t0;
        expect(std::string("Q1 ") + verbs[i] + ": OK, then exit 0 (within 1 s)", reply == "OK" && rc == 0 && ms < 1000);
        if (i == 0) {
            printf("       Q1 QUIT -> exit: %llu ms\n", (unsigned long long)ms);
            char b;
            struct pollfd p = { raw, POLLIN, 0 };          /* bounded: a daemon that keeps running fails, not hangs */
            expect("Q3 a data client reads EOF at the QUIT",
                   raw >= 0 && poll(&p, 1, 1000) == 1 && read(raw, &b, 1) == 0);
            expect("Q1 the log names the stop", log_has(g_tmp + "/q1.log", "QUIT: stopping (as SIGTERM)"));
        }
        close(raw);
        close(c);
        if (rc == -2)
            stop(pid);
    }

    reset_dirs();
    bus("1-2 SERIAL01 1\n");
    pid_t pid = start(mesh({}), g_tmp + "/q2.log");
    wait_status("loraconf868.sock", "RADIO=READY", 8000);
    expect("Q2 QUIT x: ERR MALFORMED", conf("loraconf868.sock", "QUIT x", "ERR") == "ERR MALFORMED");
    conf("loraconf868.sock", "QUITX", "");
    conf("loraconf868.sock", "Exit\tnow", "");
    expect("Q2 ... and after QUITX and \"Exit<TAB>now\" the daemon keeps running", wait_exit(pid, 500) == -2 &&
           status("loraconf868.sock").find("RADIO=READY") != std::string::npos);

    /* Q4: a CONF client that never reads: 250 GET STATUS and a QUIT in one
     * write. Its replies (about 93 KB) fill its socket (62 KB here) and wait
     * in the daemon's output queue (64 KB); the QUIT still stops the daemon. */
    int st_len = (int)status("loraconf868.sock").size() + 1;
    int c = connect_to("loraconf868.sock");
    std::string flood;
    for (int i = 0; i < 250; i++)
        flood += "GET STATUS\n";
    flood += "QUIT\n";
    bool w = send(c, flood.data(), flood.size(), MSG_NOSIGNAL) == (ssize_t)flood.size();
    uint64_t t0 = now_ms();
    int rc = wait_exit(pid, 10000);
    uint64_t ms = now_ms() - t0;
    int unread = 0;
    ioctl(c, FIONREAD, &unread);
    printf("       Q4 250 replies of %d bytes, %d readable at the exit; QUIT -> exit: %llu ms\n", st_len, unread,
           (unsigned long long)ms);
    expect("Q4 the never-reading client's socket was full (fewer bytes readable than replied)",
           unread > 0 && unread < 250 * st_len);
    expect("Q4 QUIT from the never-reading client: exit 0 within 1 s", w && rc == 0 && ms < 1000);
    close(c);
    if (rc == -2)
        stop(pid);
}

/* A stop by SIGTERM (k 0) or QUIT (k 1); for QUIT the client and its OK. */
static int stop_send(pid_t pid, int k)
{
    if (k == 0) {
        kill(pid, SIGTERM);
        return -1;
    }
    int c = connect_to("loraconf868.sock");           /* read by the daemon after the running step */
    if (write(c, "QUIT\n", 5) != 5) { }
    return c;
}

static void scenario_stop_in_retry(void)
{
    const char *how[2] = { "SIGTERM", "QUIT" };

    /* (a) In the backoff wait: a fast failing re-init (badchip), then the
     * stop in its 1 s wait: the exit comes before the next attempt. */
    for (int k = 0; k < 2; k++) {
        reset_dirs();
        std::string log = g_tmp + "/t5a.log";
        bus("1-2 SERIAL01 1\n");
        pid_t pid = start(mesh({}), log);
        wait_status("loraconf868.sock", "RADIO=READY", 8000);
        bus("");
        wait_log(log, "no ELECROW Meshstick with serial SERIAL01; waiting", 3000);
        bus("1-2 SERIAL01 2 badchip\n");
        bool in_wait = wait_log(log, "attempt 2: re-init failed: closing it, next attempt in 1 s", 5000);
        int c = stop_send(pid, k);
        std::string reply = k ? read_line(c, 2000) : "OK";
        uint64_t t0 = now_ms();
        int rc = wait_exit(pid, 10000);
        uint64_t ms = now_ms() - t0;
        expect(std::string("T5 ") + how[k] + " in the backoff wait: exit 0 before the next attempt",
               in_wait && reply == "OK" && rc == 0 && ms < 1000 && !log_has(log, "attempt 3:"));
        printf("       T5 %s in the backoff wait -> exit: %llu ms\n", how[k], (unsigned long long)ms);
        if (c >= 0)
            close(c);
        if (rc == -2)
            stop(pid);
    }

    /* (b) Inside a re-init step: a present-path fault (ioerr), the stop once
     * the attempt's open is in the fake's log (the re-init runs on the failing
     * stick, ~10 s on the fake: RadioLib's chip search against failing
     * transfers): the exit follows that step's close. */
    for (int k = 0; k < 2; k++) {
        reset_dirs();
        std::string log = g_tmp + "/t5b.log";
        bus("1-2 SERIAL01 1\n");
        pid_t pid = start(mesh({}), log);
        wait_status("loraconf868.sock", "RADIO=READY", 8000);
        bus("1-2 SERIAL01 1 ioerr\n");
        sleep_ms(200);                                /* past the library's 20 ms bus cache */
        conf("loraconf868.sock", "SET SF=10", "");
        wait_log(log, "USB I/O error with the stick present: closing it, retry in 1 s (fault 1)", 3000);
        uint64_t fault_close = 0;
        for (int i = 0; i < 100 && !fault_close; i++) {
            std::vector<uint64_t> cl = event_times(pid, "close");
            fault_close = cl.empty() ? 0 : cl.back();
            if (!fault_close)
                sleep_ms(20);
        }
        bool opened = false;
        for (int i = 0; i < 300 && !opened; i++) {    /* the attempt's open, 1 s after the close */
            opened = !events_after(pid, "open", fault_close).empty();
            if (!opened)
                sleep_ms(10);
        }
        uint64_t t_stop = now_ms();
        int c = stop_send(pid, k);
        std::string reply = k ? read_line(c, 30000) : "OK";
        int rc = wait_exit(pid, 30000);
        uint64_t t_exit = now_ms();
        std::vector<uint64_t> op = events_after(pid, "open", fault_close), cl = events_after(pid, "close", fault_close);
        bool step = opened && !op.empty() && !cl.empty();
        uint64_t step_end = step ? cl.front() : 0;
        expect(std::string("T5 ") + how[k] + " inside a re-init step: exit 0, within 500 ms after the step ends",
               step && reply == "OK" && rc == 0 && t_exit >= step_end && t_exit - step_end < 500 && t_stop < step_end);
        if (step)
            printf("       T5 %s inside a re-init step: step %llu ms, stop -> exit %llu ms, step end -> exit %llu ms\n",
                   how[k], (unsigned long long)(step_end - op.front()), (unsigned long long)(t_exit - t_stop),
                   (unsigned long long)(t_exit - step_end));
        if (c >= 0)
            close(c);
        if (rc == -2)
            stop(pid);
    }

    /* (c) Inside a held open (pinedio_init at its timeout): the stop while it
     * is held, the stick healthy again, then the release: the exit follows the
     * open's return within 1 s (the re-init that follows is short). */
    for (int k = 0; k < 2; k++) {
        reset_dirs();
        std::string log = g_tmp + "/t5c.log";
        bus("1-2 SERIAL01 1\n");
        pid_t pid = start(mesh({}), log);
        wait_status("loraconf868.sock", "RADIO=READY", 8000);
        bus("1-2 SERIAL01 1 ioerr\n");
        sleep_ms(200);
        write_text(g_fake + "/opendelay", "1\n");
        conf("loraconf868.sock", "SET SF=10", "");
        bool held = wait_log(g_fake + "/log", " openwait 1-2", 5000);
        int c = stop_send(pid, k);
        bus("1-2 SERIAL01 1\n");                      /* the stick works again */
        uint64_t t0 = now_ms();
        release("opendelay");                         /* the library call returns */
        std::string reply = k ? read_line(c, 5000) : "OK";
        int rc = wait_exit(pid, 10000);
        uint64_t ms = now_ms() - t0;
        expect(std::string("T5 ") + how[k] + " inside a held open: exit 0 within 1 s after the call returns",
               held && reply == "OK" && rc == 0 && ms < 1000);
        printf("       T5 %s inside a held open -> exit after its return: %llu ms\n", how[k], (unsigned long long)ms);
        expect("T5 the open was held until released (no watchdog)", held_until_released());
        if (c >= 0)
            close(c);
        if (rc == -2)
            stop(pid);
    }
}

/* GET RADIO on the real binary (1.5.0, plan v17 §13 T5, T8b). */

/* T5: the values are kept while UNPLUGGED and the same after the replay. */
static void scenario_radio_across_unplug(void)
{
    reset_dirs();
    std::string log = g_tmp + "/rad5.log";
    const std::string C = "loraconf868.sock";
    bus("1-2 SERIAL01 1\n");
    pid_t pid = start(mesh({}), log);
    expect("T5 READY", wait_status(C, "RADIO=READY", 8000));
    const std::string dflt =
        "RADIO RADIO=READY MODE=LORA FREQ=869.525 SF=11 BW=250.0 CR=5 PREAMBLE=16 SYNC=0x2B CRC=1 LDRO=AUTO POWER=10";
    expect("T1 the real binary: the 868 defaults", conf(C, "GET RADIO", "RADIO") == dflt);
    expect("T5 SET FREQ=869.4 SF=9 applied", conf(C, "SET FREQ=869.4 SF=9", "") == "OK");
    const std::string set =
        "RADIO MODE=LORA FREQ=869.4 SF=9 BW=250.0 CR=5 PREAMBLE=16 SYNC=0x2B CRC=1 LDRO=AUTO POWER=10";
    auto with = [&set](const char *health) {
        return std::string("RADIO RADIO=") + health + set.substr(5);
    };
    expect("T5 GET RADIO after the SET", conf(C, "GET RADIO", "RADIO") == with("READY"));
    bus("");
    expect("T5 unplugged", wait_status(C, "RADIO=UNPLUGGED", 3000));
    expect("T5/T6 while UNPLUGGED: the same values, RADIO=UNPLUGGED (no ERR RADIO_NOT_READY)",
           conf(C, "GET RADIO", "RADIO") == with("UNPLUGGED"));
    bus("1-2 SERIAL01 2\n");
    expect("T5 back READY with the replay", wait_status(C, "RADIO=READY", 8000) &&
           log_has(log, "[CH341] configuration replayed: SET FREQ=869.4 SF=9"));
    expect("T5 after the replay: the same values", conf(C, "GET RADIO", "RADIO") == with("READY"));
    term(pid);
}

/* T8b: RX while GET RADIO is polled. Identifiable frames (a sequence number
 * each), one at a time (the next only after the previous reached the raw
 * client, so the received flag never coalesces two), through the fake's RX
 * poll thread, the interrupt, the main loop's drain and the broadcast: once
 * without and once with a client polling GET RADIO at 100 Hz. The same frames
 * in the same order, all delivered; the latency compared between the two runs.
 * Margin: the polled run's 95th percentile within 15 ms of the unpolled run's
 * (one 10 ms main-loop tick plus the fake's 2 ms poll, rounded up). */
static void scenario_rx_with_radio_polling(void)
{
    reset_dirs();
    std::string log = g_tmp + "/t8b.log";
    const std::string C = "loraconf868.sock";
    write_text(g_fake + "/rxpoll", "1\n");
    bus("1-2 SERIAL01 1\n");
    pid_t pid = start(mesh({}), log);
    expect("T8b READY", wait_status(C, "RADIO=READY", 8000));
    int raw = connect_to("lora868.sock");
    const int N = 30;
    std::vector<uint64_t> lat[2];
    bool in_order[2] = { true, true };
    int polls = 0, polls_ok = 0;
    for (int run = 0; run < 2; run++) {
        std::atomic<bool> stop_poll{false};
        std::atomic<int> n_poll{0}, n_ok{0};
        std::thread poller;
        if (run == 1) {
            poller = std::thread([&] {
                int c = connect_to(C);
                while (c >= 0 && !stop_poll) {
                    if (write(c, "GET RADIO\n", 10) != 10)
                        break;
                    n_ok += read_line(c, 1000).rfind("RADIO RADIO=READY MODE=LORA ", 0) == 0;
                    n_poll++;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if (c >= 0)
                    close(c);
            });
            while (n_poll.load() < 3)                 /* the polling runs before the first frame */
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::string acc;
        for (int k = 0; k < N; k++) {
            char payload[32];
            snprintf(payload, sizeof(payload), "T8B-%d-%03d", run, k);
            write_text(g_fake + "/rx.tmp", payload);
            uint64_t t0 = now_ms();
            rename((g_fake + "/rx.tmp").c_str(), (g_fake + "/rx").c_str());   /* one whole frame */
            bool seen = false;
            while (!seen && now_ms() < t0 + 2000) {
                struct pollfd p = { raw, POLLIN, 0 };
                if (poll(&p, 1, 20) == 1) {
                    char b[256];
                    ssize_t n = read(raw, b, sizeof(b));
                    if (n <= 0)
                        break;
                    acc.append(b, (size_t)n);
                }
                seen = acc.find(payload) != std::string::npos;
            }
            lat[run].push_back(now_ms() - t0);
            /* in order: exactly this frame is what arrived since the last one */
            in_order[run] = in_order[run] && seen && acc == payload;
            acc.clear();
        }
        if (run == 1) {
            stop_poll = true;
            poller.join();
            polls = n_poll.load();
            polls_ok = n_ok.load();
        }
    }
    close(raw);
    expect("T8b without polling: all frames, each alone, in order", in_order[0] && lat[0].size() == N);
    expect("T8b with GET RADIO polled at 100 Hz: the same frames, each alone, in order",
           in_order[1] && lat[1].size() == N);
    expect("T8b the polling ran through the second run, every reply a RADIO line", polls >= 10 && polls_ok == polls);
    std::vector<uint64_t> a = lat[0], b = lat[1];
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    uint64_t p95a = a[(N * 95) / 100], p95b = b[(N * 95) / 100];
    printf("       T8b latency median/p95 without polling %llu/%llu ms, with %llu/%llu ms (%d polls)\n",
           (unsigned long long)a[N / 2], (unsigned long long)p95a, (unsigned long long)b[N / 2],
           (unsigned long long)p95b, polls);
    expect("T8b the polled run's p95 latency within 15 ms of the unpolled run's", p95b <= p95a + 15);
    term(pid);
}

/* --usb-port on the real binary: two sticks without a serial (Rule B refuses
 * those), the port names one; a library without the open by port refuses. */
static void scenario_usb_port(void)
{
    reset_dirs();
    std::string log = g_tmp + "/up1.log";
    bus("1-2 - 1\n1-3 - 1\n");
    pid_t pid = start(mesh({ "--id", "up", "--usb-port", "1-3" }), log);
    const std::string C = "loraconf-up.sock";
    expect("PORT --usb-port 1-3 beside another stick without a serial: READY",
           wait_status(C, "RADIO=READY", 8000));
    expect("PORT BOUND=-@1-3", status(C).find(" BOUND=-@1-3 ID=up") != std::string::npos);
    bool by_port = false, other = false;
    for (const Ev &e : events_of(pid)) {
        if (e.what == "openport" && e.arg == "1-3")
            by_port = true;
        if ((e.what == "open" || e.what == "openport") && e.arg == "1-2")
            other = true;
    }
    expect("PORT opened by port", by_port);
    expect("PORT the stick on 1-2 never opened", !other);
    stop(pid);

    /* B4b (DEC-W35): the port names the stick, whatever its serial says. */
    reset_dirs();
    bus("1-2 AAAA0001 1\n1-3 TESTSN01 1\n");
    pid = start(mesh({ "--id", "up2", "--usb-port", "1-3" }), g_tmp + "/up1b.log");
    expect("PORT a stick with a serial on the port: READY", wait_status("loraconf-up2.sock", "RADIO=READY", 8000));
    expect("PORT ... BOUND=TESTSN01@1-3",
           status("loraconf-up2.sock").find(" BOUND=TESTSN01@1-3 ID=up2") != std::string::npos);
    stop(pid);

    reset_dirs();
    bus("1-2 - 1\n1-3 - 1\n");
    std::string keep = g_bin;
    g_bin = g_bin_noport;
    pid = start(mesh({ "--usb-port", "1-3" }), g_tmp + "/up2.log");
    g_bin = keep;
    int rc = wait_exit(pid, 8000);
    expect("PORT a library without the open by port: exit 4", rc == 4);
    expect("PORT ... with the reason", log_has(g_tmp + "/up2.log",
           "[CH341] refused: --usb-port needs the libch341 build with port selection"));
    if (rc == -2)
        stop(pid);
}

static double cpu_s(pid_t pid)
{
    std::string st = read_text("/proc/" + std::to_string(pid) + "/stat");
    size_t rp = st.rfind(')');
    if (rp == std::string::npos)
        return -1;
    unsigned long ut = 0, stt = 0;
    /* fields after ")": state(3) ... utime(14) stime(15) */
    const char *p = st.c_str() + rp + 2;
    int field = 3;
    while (*p && field < 14) {
        if (*p == ' ')
            field++;
        p++;
    }
    sscanf(p, "%lu %lu", &ut, &stt);
    return (double)(ut + stt) / (double)sysconf(_SC_CLK_TCK);
}

static void scenario_fds(void)
{
    reset_dirs();
    std::string log = g_tmp + "/s7.log";
    write_text(g_fake + "/fdmode", "pipe\n");
    bus("1-2 SERIAL01 1\n");
    pid_t pid = start(mesh({}), log);
    expect("fds READY with a registered monitor fd", wait_status("loraconf868.sock", "RADIO=READY", 8000));
    /* A reopen while the loop runs: every scan step bumps the notifier
     * generation while the monitor's fd stays registered. */
    bus("");
    wait_status("loraconf868.sock", "RADIO=UNPLUGGED", 3000);
    bus("1-2 SERIAL01 2\n");
    expect("fds a reopen scan with the monitor fd registered: READY again, no loop error",
           wait_status("loraconf868.sock", "RADIO=READY", 6000) && !log_has(log, "event_loop_wait"));
    write_text(g_fake + "/fdmode", "fail\n");
    expect("fds mon_fds() -1: the diagnostic", wait_log(log, "the monitor's pollfds could not be read", 3000));
    expect("fds the old fd number was reused by a readable fd", wait_log(g_fake + "/log", " fd_reused", 1000));
    double c0 = cpu_s(pid);
    sleep_ms(3000);
    double c1 = cpu_s(pid);
    expect("fds no spin on the reused fd: < 0.6 s CPU in 3 s (was " + std::to_string(c1 - c0) + ")",
           c0 >= 0 && c1 - c0 < 0.6);
    expect("fds the daemon still answers", status("loraconf868.sock").find("RADIO=READY") != std::string::npos);
    term(pid);
}

/* Preset POWER ceilings through the public path (the daemon binary, the CONF
 * socket, STATUS): the PineDio takes the SX1262 range 0..20 (1.4.2: its
 * placeholder 14 is gone); the Meshstick's raised 22 reaches the process,
 * and from 1.5.0 its 21 and 22 need --high-power: refused with the named
 * reason without it; with it admitted by the CONF layer AND applied by the
 * driver (its above-20 warning line is printed only after an accepted
 * setOutputPower). */
static void scenario_pinedio_power(void)
{
    reset_dirs();
    std::string log = g_tmp + "/s9.log";
    bus("1-2 - 1\n");
    pid_t pid = start({ "--radio", "868", "--hw", "pinedio-usb-1262", "--id", "pd1" }, log);
    const std::string C = "loraconf-pd1.sock";
    expect("S9 PineDio: READY", wait_status(C, "RADIO=READY", 8000));
    expect("S9 PineDio SET POWER=17 applied", conf(C, "SET POWER=17", "") == "OK");
    expect("S9 STATUS reports POWER=17", status(C).find(" ID=pd1 RADIOERR=retry USBFAULT=0 FREQ=- SF=- POWER=17") != std::string::npos);
    expect("S9 PineDio SET POWER=20 applied", conf(C, "SET POWER=20", "") == "OK");
    expect("S9 PineDio SET POWER=21 refused", conf(C, "SET POWER=21", "").rfind("ERR", 0) == 0);
    expect("S9 STATUS still POWER=20", status(C).find(" ID=pd1 RADIOERR=retry USBFAULT=0 FREQ=- SF=- POWER=20") != std::string::npos);
    stop(pid);

    /* The Meshstick without --high-power: 0..20 only; 21 and 22 refused
     * with the switch named, STATUS keeps the applied 20. */
    reset_dirs();
    std::string log2 = g_tmp + "/s9b.log";
    bus("1-2 - 1\n");
    pid = start(mesh({ "--id", "ms9" }), log2);
    const std::string M = "loraconf-ms9.sock";
    const std::string why = "(high-power mode not enabled (start with --high-power))";
    expect("S9 Meshstick: READY", wait_status(M, "RADIO=READY", 8000));
    expect("S9 Meshstick SET POWER=20 applied without the flag", conf(M, "SET POWER=20", "") == "OK");
    std::string r21 = conf(M, "SET POWER=21", ""), r22 = conf(M, "SET POWER=22", "");
    expect("S9 Meshstick SET POWER=21 refused without the flag (" + r21 + ")", r21.rfind("ERR", 0) == 0);
    expect("S9 Meshstick SET POWER=22 refused without the flag (" + r22 + ")", r22.rfind("ERR", 0) == 0);
    expect("S9 Meshstick the 21 refusal names the switch", wait_log(log2, "CONFIG rejected: POWER=21 " + why, 3000));
    expect("S9 Meshstick the 22 refusal names the switch", wait_log(log2, "CONFIG rejected: POWER=22 " + why, 3000));
    expect("S9 Meshstick STATUS still POWER=20", status(M).find(" SF=- POWER=20") != std::string::npos);
    expect("S9 Meshstick no above-20 apply without the flag", !log_has(log2, "above +20 dBm"));
    stop(pid);

    /* With --high-power: the boot line, then 22 admitted and applied. */
    reset_dirs();
    std::string log3 = g_tmp + "/s9c.log";
    bus("1-2 - 1\n");
    pid = start(mesh({ "--id", "ms9", "--high-power" }), log3);
    expect("S9 Meshstick --high-power: READY", wait_status(M, "RADIO=READY", 8000));
    expect("S9 Meshstick --high-power: the boot line names 21..22",
           log_has(log3, "WARNING SX1262 POWER 21..22 permitted (--high-power)"));
    expect("S9 Meshstick SET POWER=22 applied with the flag", conf(M, "SET POWER=22", "") == "OK");
    expect("S9 Meshstick STATUS POWER=22", status(M).find(" SF=- POWER=22") != std::string::npos);
    expect("S9 Meshstick the driver applied 22 (its warning line)",
           wait_log(log3, "WARNING POWER=22 above +20 dBm", 3000));
    /* The FSK apply path has its own POWER branch in the driver. */
    expect("S9 Meshstick SET MODE=FSK POWER=21 applied with the flag", conf(M, "SET MODE=FSK POWER=21", "") == "OK");
    expect("S9 Meshstick the driver applied 21 in FSK (its warning line)",
           wait_log(log3, "WARNING POWER=21 above +20 dBm", 3000));
    expect("S9 Meshstick back to LoRa", conf(M, "SET MODE=LORA", "") == "OK");
    std::string r23 = conf(M, "SET POWER=23", "");
    expect("S9 Meshstick SET POWER=23 refused with the flag (" + r23 + ")", r23.rfind("ERR", 0) == 0);
    expect("S9 Meshstick the 23 refusal is generic",
           wait_log(log3, "CONFIG rejected: POWER=23 (invalid LoRa value)", 3000));
    stop(pid);

    /* The PineDio (ceiling 20) with --high-power: inert, 21 still refused
     * with the generic reason. */
    reset_dirs();
    std::string log4 = g_tmp + "/s9d.log";
    bus("1-2 - 1\n");
    pid = start({ "--radio", "868", "--hw", "pinedio-usb-1262", "--id", "pd1", "--high-power" }, log4);
    expect("S9 PineDio --high-power: READY", wait_status(C, "RADIO=READY", 8000));
    expect("S9 PineDio --high-power: the inert line", log_has(log4, "--high-power: not applicable on"));
    std::string p21 = conf(C, "SET POWER=21", "");
    expect("S9 PineDio --high-power SET POWER=21 refused (" + p21 + ")", p21.rfind("ERR", 0) == 0);
    expect("S9 PineDio --high-power the 21 refusal is generic",
           wait_log(log4, "CONFIG rejected: POWER=21 (invalid LoRa value)", 3000));
    stop(pid);
}

/* Reads and drops a pseudo-terminal's output for `ms` (the chat child must
 * not block on a full terminal buffer). */
static void drain(int fd, int ms)
{
    char b[4096];
    uint64_t end = now_ms() + (uint64_t)ms;
    while (now_ms() < end) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 50) > 0 && read(fd, b, sizeof(b)) <= 0)
            break;
    }
}

/* The chat binary on a new pseudo-terminal in `dir`, against radio `id`'s
 * data and CONF sockets; *master gets the terminal's master side. */
static pid_t start_chat(const std::string &dir, const std::string &id, int *master)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    const char *name = m >= 0 && grantpt(m) == 0 && unlockpt(m) == 0 ? ptsname(m) : nullptr;
    if (!name) {
        if (m >= 0)
            close(m);
        return -1;
    }
    std::string slave = name, data = g_sock + "/lora-" + id + ".sock", cf = g_sock + "/loraconf-" + id + ".sock";
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int s = open(slave.c_str(), O_RDWR);        /* the controlling terminal */
        if (s < 0 || chdir(dir.c_str()) != 0)
            _exit(126);
        dup2(s, STDIN_FILENO);
        dup2(s, STDOUT_FILENO);
        dup2(s, STDERR_FILENO);
        close(m);
        setenv("TERM", "xterm", 1);
        signal(SIGPIPE, SIG_DFL);                   /* ours is ignored, and exec keeps that */
        execl(g_chat.c_str(), "loraham_chat", "--data-socket", data.c_str(), "--conf-socket", cf.c_str(),
              (char *)nullptr);
        _exit(127);
    }
    *master = m;
    return pid;
}

/* 1.4.2 (gate-2 NOTE): chat leaves the power alone, through the public path.
 * A PineDio daemon is set to POWER=12, then the real chat binary starts
 * against it on a pseudo-terminal. Once chat's SET line is applied (STATUS
 * shows chat's FREQ; the line is applied whole or not at all), STATUS still
 * reports POWER=12. A chat that sends a POWER, or a daemon that resets the
 * power when it applies chat's other keys, fails here. */
static void scenario_chat_keeps_power(void)
{
    reset_dirs();
    std::string log = g_tmp + "/s10.log";
    bus("1-2 - 1\n");
    pid_t pid = start({ "--radio", "868", "--hw", "pinedio-usb-1262", "--id", "pd2" }, log);
    const std::string C = "loraconf-pd2.sock";
    expect("S10 PineDio: READY", wait_status(C, "RADIO=READY", 8000));
    expect("S10 the operator's SET POWER=12 applied", conf(C, "SET POWER=12", "") == "OK");
    expect("S10 STATUS reports POWER=12, FREQ not set yet", status(C).find(" ID=pd2 RADIOERR=retry USBFAULT=0 FREQ=- SF=- POWER=12") != std::string::npos);
    std::string dir = g_tmp + "/chat-run";
    sh("rm -rf '" + dir + "' && mkdir -p '" + dir + "'");
    write_text(dir + "/lorachat.conf", "TX=868.100\nRX=868.100\n");   /* TX = RX: no retune */
    int master = -1;
    pid_t chat = start_chat(dir, "pd2", &master);
    expect("S10 chat starts on a pseudo-terminal", chat > 0);
    bool applied = false;
    for (uint64_t end = now_ms() + 8000; chat > 0 && !applied && now_ms() < end;) {
        drain(master, 100);
        applied = status(C).find(" ID=pd2 RADIOERR=retry USBFAULT=0 FREQ=868.1") != std::string::npos;
    }
    expect("S10 chat's SET line is applied (STATUS shows its FREQ=868.1)", applied);
    if (chat > 0)
        drain(master, 500);                         /* nothing later changes the power */
    std::string st = status(C);
    size_t at = st.rfind(" POWER=");
    /* The POWER value only: the CAD parameters follow it since 1.6.2. */
    std::string pw = at == std::string::npos ? "?" : st.substr(at + 7, st.find(' ', at + 7) - (at + 7));
    while (!pw.empty() && (pw.back() == '\n' || pw.back() == '\r' || pw.back() == ' '))
        pw.pop_back();
    expect("S10 after chat's SET, STATUS still POWER=12 (was POWER=" + pw + ")", pw == "12");
    expect("S10 chat is still running", chat > 0 && waitpid(chat, nullptr, WNOHANG) == 0);
    if (chat > 0) {
        kill(chat, SIGKILL);
        waitpid(chat, nullptr, 0);
        close(master);
    }
    stop(pid);
}

static void scenario_no_hotplug(void)
{
    reset_dirs();
    std::string log = g_tmp + "/s8.log";
    write_text(g_fake + "/nohotplug", "1\n");
    bus("1-2 SERIAL01 1\n");
    pid_t pid = start(mesh({}), log);
    expect("T7 no hotplug: READY", wait_status("loraconf868.sock", "RADIO=READY", 8000));
    expect("T7 the startup note", log_has(log, "libusb offers no usable hotplug here"));
    expect("N1 no option (the default retry): the note does not name --radio-error",
           !log_has(log, "--radio-error retry has no effect here"));
    expect("O2 no hotplug: the running mode is exit (a loss exits 5), USBFAULT=0",
           status("loraconf868.sock").find(" RADIOERR=exit USBFAULT=0 ") != std::string::npos);
    bus("");
    int rc = wait_exit(pid, 5000);
    expect("T7 a lost stick exits 5 as before", rc == 5);
    if (rc == -2)
        stop(pid);

    /* N1 (a25 on C1): an explicit --radio-error retry without hotplug is named
     * inert, never silent (OPEN-A's rule). */
    reset_dirs();
    write_text(g_fake + "/nohotplug", "1\n");
    bus("1-2 SERIAL01 1\n");
    std::string logr = g_tmp + "/s8r.log";
    pid = start(mesh({ "--radio-error", "retry" }), logr);
    expect("N1 explicit retry, no hotplug: READY, RADIOERR=exit", wait_status("loraconf868.sock", "RADIO=READY", 8000) &&
           status("loraconf868.sock").find(" RADIOERR=exit USBFAULT=0 ") != std::string::npos);
    expect("N1 ... the note says the option has no effect here",
           log_has(logr, "[CH341] note: libusb offers no usable hotplug here; a lost stick exits 5 as before "
                         "(--radio-error retry has no effect here)"));
    term(pid);

    /* Core r4 F4: without a monitor the start still refuses an unreadable
     * serial (DELTA-4-v3: never "no serial", in every mode). */
    reset_dirs();
    write_text(g_fake + "/nohotplug", "1\n");
    bus("1-2 SERIAL01 1 unreadable\n");
    std::string log2 = g_tmp + "/s8b.log";
    pid = start(mesh({}), log2);
    rc = wait_exit(pid, 8000);
    expect("no hotplug + an unreadable serial: exit 4", rc == 4);
    expect("... with the reason", log_has(log2, "[CH341] refused: the opened stick's serial could not be read (unreadable serial)"));
    expect("... the sockets removed", !socket_exists("loraconf868.sock"));
    if (rc == -2)
        stop(pid);
}

int main(int argc, char *argv[])
{
    const char *daemon = nullptr;
    for (int i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], "--bin") == 0)
            daemon = argv[i + 1];
    if (!daemon) {
        printf("[FAIL] usage: %s --bin <loraham_daemon>\n", argv[0]);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    /* Under TMPDIR (run_tests.sh's own directory): its daemons are in the run's scope. */
    const char *tmpdir = getenv("TMPDIR");
    std::string tmpl_s = std::string(tmpdir && tmpdir[0] ? tmpdir : "/tmp") + "/loraham-ch341-process.XXXXXX";
    std::vector<char> tmpl(tmpl_s.begin(), tmpl_s.end());
    tmpl.push_back('\0');
    if (!mkdtemp(tmpl.data())) {
        printf("[FAIL] mkdtemp\n");
        return 1;
    }
    g_tmp = tmpl.data();
    /* The copies keep the given file name: a run under a private name (not
     * loraham_daemon) stays out of name-based lab tools (pgrep/pkill -x). */
    std::string name(daemon);
    name = name.substr(name.rfind('/') + 1);
    g_bin = g_tmp + "/bundle/" + name;
    g_fake = g_tmp + "/fake";
    g_sock = g_tmp + "/sock";
    g_run = g_tmp + "/run";
    sh("mkdir -p '" + g_tmp + "/bundle/lib' && cp '" + std::string(daemon) + "' '" + g_bin + "'");

    std::string d(daemon);
    std::string exe_dir = d.substr(0, d.rfind('/'));
    std::string pin = read_text(exe_dir + "/../.github/ci/libpinedio.lock");
    size_t at = pin.find("PINEDIO_COMMIT=");
    pin = at == std::string::npos ? "" : pin.substr(at + 15, 40);
    std::string src = exe_dir + "/tests/fakes/ch341_process_fake.c";
    expect("the fake Meshstick library builds",
           sh("cc -std=gnu11 -Wall -Wextra -Werror -shared -fPIC -DFAKE_PIN='\"" + pin + "\"' -o '" + g_tmp +
              "/bundle/lib/libloraham-ch341.so' '" + src + "'") == 0);
    g_bin_noport = g_tmp + "/bundle-noport/" + name;
    sh("mkdir -p '" + g_tmp + "/bundle-noport/lib' && cp '" + std::string(daemon) + "' '" + g_bin_noport + "'");
    expect("the fake library without the open by port builds",
           sh("cc -std=gnu11 -Wall -Wextra -Werror -shared -fPIC -DFAKE_NO_OPEN_PORT -DFAKE_PIN='\"" + pin +
              "\"' -o '" + g_tmp + "/bundle-noport/lib/libloraham-ch341.so' '" + src + "'") == 0);
    /* The chat client, built as clients/chat/run_tests.sh builds it. */
    g_chat = g_tmp + "/loraham_chat";
    expect("the chat client builds",
           sh("cc -std=gnu11 -Wall -o '" + g_chat + "' '" + exe_dir + "/../clients/chat/lorachat_ncurses_113.c' -lncurses -lpthread") == 0);

    if (CH341_PROCESS_STOP) {
        /* test_ch341_stop: the stop scenarios (their re-init steps take ~10 s
         * on the fake), apart, so each binary stays inside the suite's time limit. */
        scenario_quit();
        scenario_stop_in_retry();
    } else {
    scenario_cli();
    scenario_usb_port();
    scenario_unplug_and_return();
    scenario_io_fault();
    scenario_reinit_failures();
    scenario_radio_error_cli();
    scenario_retry_fault();
    scenario_retry_reinit();
    scenario_radio_lines();
    scenario_retry_kept();
    scenario_radio_across_unplug();
    scenario_rx_with_radio_polling();
    scenario_access();
    scenario_instances();
    scenario_b4_classes();
    scenario_start_arrival();
    scenario_gap();
    scenario_fds();
    scenario_no_hotplug();
    scenario_pinedio_power();
    scenario_chat_keeps_power();
    }

    if (g_fail)
        sh("for f in '" + g_tmp + "'/*.log '" + g_fake + "/log'; do echo \"== $f\"; tail -n 40 \"$f\"; done");
    sh("rm -rf '" + g_tmp + "'");
    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
