/*
 * The REAL daemon binary on a scripted Meshstick library
 * (tests/fakes/ch341_process_fake.c, built here as lib/libloraham-ch341.so):
 * whole processes, their sockets, their exit codes.
 *
 *   T1/T3/T11  unplug: RADIO=UNPLUGGED, every client stays, a NEW client is
 *              accepted, framed TX answered RADIO_NOT_READY, no exit
 *   T12        a TX in flight at the unplug gets exactly one TX_RESULT
 *   T21/BOUND  USBWAIT= and BOUND= follow the runtime: -/port:1-2 when READY,
 *              1-2/- while waiting (gone, other port, a serial stick on the
 *              port, BUSY), back after the return
 *   T4/T6      the return: re-init + replay line + READY; a chip that fails
 *              re-init, or a replay that fails -> RADIO=FAILED path, exit 5
 *   T2         an I/O error with the stick present -> exit 5
 *   T16/T13    ACCESS at start -> exit 4 (never 5); ACCESS on a reopen ->
 *              waits with the line, a new ARRIVED succeeds
 *   T19        r3 / r3f / no id side by side: six+three distinct sockets, CONF
 *              answers ID= on each; a duplicate --id exits 3 before any USB
 *              open; T22 a second id on a claimed stick exits 4 with BUSY:,
 *              the first undisturbed
 *   T23        --usb-serial on a Pi preset, or malformed -> exit 1
 *   F4         SIGTERM: exit 0, the stick closed before the monitor
 *   fds        mon_fds() == -1 after a registered fd: the stale registration
 *              is dropped (a reused, readable fd number does not spin the loop)
 *   T7         no hotplug: the startup note; a lost stick exits 5 as before
 */

#include <cerrno>
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../framed_data.h"

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

static std::string g_tmp, g_bin, g_fake, g_sock, g_run;

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
    /* Changed guard (1.3.0): the applied FREQ= SF= POWER= now end the line. */
    expect("T21/BOUND READY: ... USBWAIT=- BOUND=port:1-2 ID=ms1, nothing applied yet",
           st.find(" USBWAIT=- BOUND=port:1-2 ID=ms1 FREQ=- SF=- POWER=-") != std::string::npos &&
           st.size() > 26 && st.substr(st.size() - 26) == "ID=ms1 FREQ=- SF=- POWER=-");
    expect("S1 SET SF=9 POWER=5 applied", conf(C, "SET SF=9 POWER=5", "") == "OK");
    expect("S1 STATUS reports the applied SF and POWER, FREQ unset",
           status(C).find(" ID=ms1 FREQ=- SF=9 POWER=5") != std::string::npos);
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
           wait_log(log, "[CH341] Meshstick gone (", 3000));
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
           status(C).find(" ID=ms1 FREQ=- SF=9 POWER=5") != std::string::npos);
    expect("T21/BOUND back: USBWAIT=- BOUND=port:1-2", status(C).find(" USBWAIT=- BOUND=port:1-2 ID=ms1") != std::string::npos);
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
    pid_t pid = start(mesh({}), log);
    expect("S2 READY", wait_status("loraconf868.sock", "RADIO=READY", 8000));
    bus("1-2 SERIAL01 1 ioerr\n");                    /* same connection, transfers fail */
    sleep_ms(200);                                    /* past the library's 20 ms bus cache */
    conf("loraconf868.sock", "SET SF=10", "");
    int rc = wait_exit(pid, 5000);
    expect("T2 an I/O error with the stick present: the real process exits 5", rc == 5);
    expect("T2 the log says present", log_has(log, "USB I/O error with the Meshstick present"));
    expect("F2(r5) the exit 5 closes the stick, then the monitor", closed_then_monitor(pid));
    if (rc == -2)
        stop(pid);

    /* r19 (core r14 F1): lost events (an unrelated overflow) while the stick
     * is present are not its LEFT: a later I/O fault still goes through the
     * presence check and exits 5, never "gone" and a reopen. */
    reset_dirs();
    log = g_tmp + "/s2b.log";
    bus("1-2 SERIAL01 1\n");
    pid = start(mesh({}), log);
    expect("r19 READY", wait_status("loraconf868.sock", "RADIO=READY", 8000));
    write_text(g_fake + "/overflow", "1\n");
    expect("r19 the fake reported lost events (marker)", wait_log(g_fake + "/log", " overflow", 3000));
    bus("1-2 SERIAL01 1 ioerr\n");
    sleep_ms(200);
    conf("loraconf868.sock", "SET SF=10", "");
    rc = wait_exit(pid, 5000);
    expect("r19 lost events, then an I/O fault with the stick present: the presence check, exit 5", rc == 5 &&
           log_has(log, "USB I/O error with the Meshstick present"));
    expect("r19 ... never classified gone by a LEFT", !log_has(log, "Meshstick gone (LEFT event"));
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
        pid_t pid = start(mesh({}), log);
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
           status("loraconf-r3.sock").find(" BOUND=serial:AAAA0001 ID=r3") != std::string::npos &&
           status("loraconf-r3f.sock").find(" BOUND=serial:BBBB0002 ID=r3f") != std::string::npos &&
           status("loraconf868.sock").find(" BOUND=serial:CCCC0003 ID=-") != std::string::npos);

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
           s3.find("RADIO=READY") != std::string::npos && s3.find(" BOUND=serial:AAAA0001 ") != std::string::npos);
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
           status("loraconf868.sock").find("BOUND=serial:SERIAL01") != std::string::npos);
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
    expect("T23 ... with the reason", log_has(g_tmp + "/s6a.log", "--usb-serial is only for --hw meshstick-1262"));
    if (rc == -2)
        stop(p);
    p = start(mesh({ "--usb-serial", "A B" }), g_tmp + "/s6b.log");
    rc = wait_exit(p, 8000);
    expect("T23 a malformed --usb-serial: exit 1", rc == 1);
    expect("T23 ... with the reason", log_has(g_tmp + "/s6b.log", "invalid --usb-serial"));
    if (rc == -2)
        stop(p);
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

static void scenario_no_hotplug(void)
{
    reset_dirs();
    std::string log = g_tmp + "/s8.log";
    write_text(g_fake + "/nohotplug", "1\n");
    bus("1-2 SERIAL01 1\n");
    pid_t pid = start(mesh({}), log);
    expect("T7 no hotplug: READY", wait_status("loraconf868.sock", "RADIO=READY", 8000));
    expect("T7 the startup note", log_has(log, "libusb offers no usable hotplug here"));
    bus("");
    int rc = wait_exit(pid, 5000);
    expect("T7 a lost stick exits 5 as before", rc == 5);
    if (rc == -2)
        stop(pid);

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

    char tmpl[] = "/tmp/loraham-ch341-process.XXXXXX";
    if (!mkdtemp(tmpl)) {
        printf("[FAIL] mkdtemp\n");
        return 1;
    }
    g_tmp = tmpl;
    g_bin = g_tmp + "/bundle/loraham_daemon";
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

    scenario_cli();
    scenario_unplug_and_return();
    scenario_io_fault();
    scenario_reinit_failures();
    scenario_access();
    scenario_instances();
    scenario_b4_classes();
    scenario_start_arrival();
    scenario_gap();
    scenario_fds();
    scenario_no_hotplug();

    if (g_fail)
        sh("for f in '" + g_tmp + "'/*.log '" + g_fake + "/log'; do echo \"== $f\"; tail -n 40 \"$f\"; done");
    sh("rm -rf '" + g_tmp + "'");
    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
