/*
 * The BUILT loraham_chat binary (path in argv[1]), as a separate process.
 *
 * Refusals: run with stdin/stdout/stderr on pipes and no controlling terminal
 * (setsid): empty path, a 108-byte path, an unknown argument, a missing
 * value, a data socket nobody listens on -> exit 1 with the reason on stderr,
 * before ncurses (nothing is drawn).
 * A real run: two listening sockets in a temporary directory; the binary
 * started on a pseudo-terminal (ncurses needs one) with --data-socket and
 * --conf-socket: it connects to the data listener, its first CONF request
 * ("SET FREQ=<RX> ...") arrives on the CONF listener, and a line typed on the
 * terminal arrives on the data socket as its frame (TX = RX in lorachat.conf,
 * so no retune wait). Then the data listener closes its end (the daemon
 * gone): chat shows "daemon gone" and stays idle (CPU over 1 s), and a line
 * typed then does not kill it (SIGPIPE).
 * One drawing thread (1.5.0): a second run against a CONF responder that
 * answers GET STATS / GET CHANNEL. 45 frames with distinct senders arrive
 * while a line is typed and sent (TX = RX), while the config menu waits for
 * its choice, while a field is typed with 300 ms pauses, and while a split
 * frequency send waits for its transmission; the history file then holds
 * all 45 senders and the two sent lines (47 entries), and the slow field
 * kept every character. 70 frames while a field is typed (nothing drains)
 * overflow the 64-line queue: the oldest 6 go, counted. After the data
 * socket closes, a typed line shows "not sent: daemon gone" within 1 s
 * (TX != RX: no wait) and chat retunes to RX; a CONF peer that closes at
 * once does not kill chat (MSG_NOSIGNAL).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_ok, g_fail;

static void expect(const char *name, int cond)
{
    if (cond) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s\n", name);
    }
    fflush(stdout);
}

static const char *g_bin;
static char g_dir[64];

/* Run the binary without a terminal; returns the exit code, stderr in err and
 * stdout in out. Both pipes are read together (poll), so a full one never
 * blocks the child. */
static int run_notty(char *const args[], char *err, size_t err_size, char *out, size_t out_size)
{
    int pe[2], po[2];
    if (pipe(pe) || pipe(po))
        return -1;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();                                 /* no controlling terminal */
        int nul = open("/dev/null", O_RDONLY);
        dup2(nul, 0);
        dup2(po[1], 1);
        dup2(pe[1], 2);
        if (chdir(g_dir) != 0)
            _exit(126);
        execv(g_bin, args);
        _exit(127);
    }
    close(pe[1]);
    close(po[1]);
    size_t ne = 0, no = 0;
    int open_fds = 2;
    struct pollfd p[2] = { { pe[0], POLLIN, 0 }, { po[0], POLLIN, 0 } };
    while (open_fds > 0 && poll(p, 2, 10000) > 0) {
        for (int k = 0; k < 2; k++) {
            if (p[k].fd < 0 || !(p[k].revents & (POLLIN | POLLHUP)))
                continue;
            char *buf = k == 0 ? err : out;
            size_t *n = k == 0 ? &ne : &no;
            size_t cap = k == 0 ? err_size : out_size;
            char sink[512];
            ssize_t r = *n + 1 < cap ? read(p[k].fd, buf + *n, cap - 1 - *n) : read(p[k].fd, sink, sizeof(sink));
            if (r <= 0) {
                close(p[k].fd);
                p[k].fd = -1;
                open_fds--;
            } else if (*n + 1 < cap) {
                *n += (size_t)r;
            }
        }
    }
    err[ne] = '\0';
    out[no] = '\0';
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int listen_at(const char *path)
{
    struct sockaddr_un a;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) || listen(fd, 8))
        return -1;
    return fd;
}

/* Accept within ms; -1 on timeout. */
static int accept_within(int lfd, int ms)
{
    struct pollfd p = { lfd, POLLIN, 0 };
    if (poll(&p, 1, ms) != 1)
        return -1;
    return accept(lfd, NULL, NULL);
}

/* Read until `want` is seen or ms passed (also drains a pty). */
static int read_until(int fd, const char *want, char *buf, size_t size, int ms)
{
    size_t n = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    buf[0] = '\0';
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        int left = ms - (int)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
        if (left <= 0)
            return 0;
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, left) != 1)
            return 0;
        ssize_t r = read(fd, buf + n, size - 1 - n);
        if (r <= 0)
            return 0;
        n += (size_t)r;
        buf[n] = '\0';
        if (memmem(buf, n, want, strlen(want)))
            return 1;
        if (n + 1 >= size)
            n = 0;                                /* keep looking in the next chunk */
    }
}

/* utime + stime of pid in clock ticks (/proc/<pid>/stat fields 14 and 15);
 * -1 when the process is gone. */
static long proc_cpu_ticks(pid_t pid)
{
    char path[64], line[1024];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    size_t n = fread(line, 1, sizeof(line) - 1, f);
    fclose(f);
    line[n] = '\0';
    char *p = strrchr(line, ')');                 /* the comm field may hold spaces */
    unsigned long ut, st;
    char state;
    if (!p || sscanf(p + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &state, &ut, &st) != 3)
        return -1;
    if (state == 'Z')
        return -1;
    return (long)(ut + st);
}

/* Drains the pseudo-terminal for ms (chat never blocks on a full one). */
static void drain_ms(int master, int ms)
{
    char sink[8192];
    read_until(master, "\001never\001", sink, sizeof(sink), ms);
}

/* One received frame, as the daemon hands it to a data client. */
static void send_frame(int dfd, const char *sender)
{
    char f[160];
    int n = snprintf(f, sizeof(f), "\x3c\xff\x01%s>APRS,WIDE1-1::ALL      :msg from %s", sender, sender);
    if (write(dfd, f, (size_t)n) != n) { }
}

/* The CONF responder (a child process): GET STATS answers a TX counter
 * that moves only on the 11th poll after a SET FREQ=<tx> (a transmission of
 * about 1.1 s); GET CHANNEL answers BUSY=0; each SET is appended to `sets`
 * as one line. */
static void conf_responder(int lfd, const char *tx, const char *sets)
{
    long done = 0;
    int polls = -1;
    char want[64];
    snprintf(want, sizeof(want), "SET FREQ=%s ", tx);
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0)
            continue;
        char b[512];
        struct pollfd p = { fd, POLLIN, 0 };
        ssize_t n = poll(&p, 1, 2000) == 1 ? read(fd, b, sizeof(b) - 1) : 0;
        if (n > 0) {
            b[n] = '\0';
            if (strncmp(b, want, strlen(want)) == 0)
                polls = 0;
            if (strncmp(b, "SET ", 4) == 0) {
                FILE *f = fopen(sets, "a");
                if (f) {
                    fprintf(f, "%.*s\n", (int)strcspn(b, "\n"), b);
                    fclose(f);
                }
            }
            if (strncmp(b, "GET STATS", 9) == 0) {
                if (polls >= 0 && ++polls == 11) {
                    done++;
                    polls = -1;
                }
                char r[96];
                int m = snprintf(r, sizeof(r), "STATS TXOK=%ld TXERR=0 TXBUSY=0\n", done);
                if (write(fd, r, (size_t)m) != m) { }
            } else if (strncmp(b, "GET CHANNEL", 11) == 0) {
                if (write(fd, "CHANNEL BUSY=0\n", 15) != 15) { }
            }
        }
        close(fd);
    }
}

static int file_has(const char *path, const char *part)
{
    char b[16384];
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    size_t n = fread(b, 1, sizeof(b) - 1, f);
    fclose(f);
    b[n] = '\0';
    return strstr(b, part) != NULL;
}

static int file_lines(const char *path)
{
    FILE *f = fopen(path, "r");
    int c, n = 0;
    if (!f)
        return -1;
    while ((c = fgetc(f)) != EOF)
        n += c == '\n';
    fclose(f);
    return n;
}

/* Waits up to ms for `part` in the file at path. */
static int wait_file(const char *path, const char *part, int ms)
{
    for (int t = 0; t < ms; t += 50) {
        if (file_has(path, part))
            return 1;
        usleep(50000);
    }
    return file_has(path, part);
}

static void scenario_one_drawing_thread(void)
{
    char dir[160], dpath[200], cpath[200], conf[200], hist[200], sets[200], name[16], buf[8192], t[200], cmd2[300];
    snprintf(dir, sizeof(dir), "%s/ui", g_dir);
    mkdir(dir, 0700);
    snprintf(dpath, sizeof(dpath), "%s/lora-u.sock", dir);
    snprintf(cpath, sizeof(cpath), "%s/loraconf-u.sock", dir);
    snprintf(conf, sizeof(conf), "%s/lorachat.conf", dir);
    snprintf(hist, sizeof(hist), "%s/lorachat.log", dir);
    snprintf(sets, sizeof(sets), "%s/sets.log", dir);
    FILE *cf = fopen(conf, "w");
    if (cf) {
        fputs("TX=433.900\nRX=433.900\n", cf);
        fclose(cf);
    }
    int dl = listen_at(dpath), cl = listen_at(cpath);
    expect("ui: two listeners", dl >= 0 && cl >= 0);
    pid_t resp = fork();
    if (resp == 0) {
        conf_responder(cl, "433.775", sets);
        _exit(0);
    }
    int master = -1;
    pid_t pid = forkpty(&master, NULL, NULL, NULL);
    if (pid == 0) {
        setenv("TERM", "xterm", 1);
        setenv("ESCDELAY", "25", 1);
        if (chdir(dir) != 0)
            _exit(126);
        execl(g_bin, "loraham_chat", "--data-socket", dpath, "--conf-socket", cpath, (char *)NULL);
        _exit(127);
    }
    int dfd = accept_within(dl, 5000);
    expect("ui: chat connects", dfd >= 0);
    drain_ms(master, 500);                        /* the first screen */

    /* 15 frames while a line is typed and sent, TX = RX. */
    const char *la = "typed-line-AAAA";
    for (int i = 0; i < 15; i++) {
        snprintf(name, sizeof(name), "TA%02d", i);
        send_frame(dfd, name);
        if (write(master, la + i, 1) != 1) { }
        drain_ms(master, 40);
    }
    if (write(master, "\r", 1) != 1) { }
    expect("ui: the typed line arrives on the data socket",
           read_until(dfd, la, buf, sizeof(buf), 3000));

    /* 10 frames while the menu waits for its choice. */
    if (write(master, "\013", 1) != 1) { }
    expect("ui: Ctrl-K opens the menu", read_until(master, "KONFIGURATION", buf, sizeof(buf), 3000));
    for (int i = 0; i < 10; i++) {
        snprintf(name, sizeof(name), "TC%02d", i);
        send_frame(dfd, name);
        drain_ms(master, 40);
    }
    expect("ui: lines received with the menu open reach the history while it is open",
           wait_file(hist, "|TC09|", 2000));

    /* 5 frames while the TX field is typed with 300 ms pauses. */
    if (write(master, "4", 1) != 1) { }
    drain_ms(master, 200);
    const char *tx = "433.775";
    for (int i = 0; i < 7; i++) {
        if (write(master, tx + i, 1) != 1) { }
        if (i < 5) {
            snprintf(name, sizeof(name), "TD%02d", i);
            send_frame(dfd, name);
        }
        drain_ms(master, 300);
    }
    if (write(master, "\r", 1) != 1) { }
    drain_ms(master, 200);
    if (write(master, "\033", 1) != 1) { }
    expect("ui: the menu closes and the footer shows the new TX",
           read_until(master, "TX: 433.775", buf, sizeof(buf), 3000));
    expect("ui: the slow field kept every character (lorachat.conf TX=433.775)",
           wait_file(conf, "TX=433.775\n", 2000));

    /* 15 frames while a split frequency send waits for its transmission. */
    const char *lb = "split-line-BBBB";
    if (write(master, lb, strlen(lb)) != (ssize_t)strlen(lb) || write(master, "\r", 1) != 1) { }
    for (int i = 0; i < 15; i++) {
        snprintf(name, sizeof(name), "TB%02d", i);
        send_frame(dfd, name);
        drain_ms(master, 50);
    }
    expect("ui: the split line arrives on the data socket", read_until(dfd, lb, buf, sizeof(buf), 3000));
    expect("ui: the split send ends and the last frame is shown", wait_file(hist, "|TB14|", 5000));

    /* The history: 45 senders and the two sent lines, nothing else. */
    int all = 1;
    const char *groups[] = { "TA", "TC", "TD", "TB" };
    const int counts[] = { 15, 10, 5, 15 };
    for (int g = 0; g < 4; g++)
        for (int i = 0; i < counts[g]; i++) {
            snprintf(name, sizeof(name), "|%s%02d|", groups[g], i);
            if (!file_has(hist, name)) {
                printf("       missing in the history: %s\n", name);
                all = 0;
            }
        }
    expect("ui: the history holds all 45 senders, each by name", all);
    expect("ui: and both sent lines", file_has(hist, "|Ich|typed-line-AAAA") && file_has(hist, "|Ich|split-line-BBBB"));
    snprintf(t, sizeof(t), "ui: 47 entries in the history (got %d)", file_lines(hist));
    expect(t, file_lines(hist) == 47);
    if (file_lines(hist) != 47) {
        snprintf(cmd2, sizeof(cmd2), "sed 's/^/       history: /' '%s' | cut -c1-110", hist);
        if (system(cmd2) != 0) { }
    }

    /* 70 frames while the call field is typed: nothing drains, the queue
     * keeps the newest 64 and counts the 6 it dropped. */
    if (write(master, "\013", 1) != 1) { }
    expect("ui: Ctrl-K opens the menu again", read_until(master, "KONFIGURATION", buf, sizeof(buf), 3000));
    if (write(master, "1", 1) != 1) { }
    drain_ms(master, 200);
    for (int i = 0; i < 70; i++) {
        snprintf(name, sizeof(name), "TE%02d", i);
        send_frame(dfd, name);
        drain_ms(master, 20);
    }
    if (write(master, "DL1TST\r\033", 8) != 8) { }
    expect("ui: the overflow: the dropped count is shown", wait_file(hist, "|System|(6 lines dropped)", 3000));
    int kept = 1;
    for (int i = 21; i < 70; i++) {               /* the 50-line backlog: these 49 and the note */
        snprintf(name, sizeof(name), "|TE%02d|", i);
        kept = kept && file_has(hist, name);
    }
    expect("ui: the overflow: the newest lines are kept (TE21..TE69 and the note: the 50-line history)", kept);

    /* A failed send: the data socket gone, TX != RX. */
    close(dfd);
    expect("ui: the daemon gone is shown", read_until(master, "daemon gone", buf, sizeof(buf), 3000));
    drain_ms(master, 200);
    if (write(master, "gone-line\r", 10) != 10) { }
    FILE *sf = fopen(sets, "a");                  /* a marker: only the SETs after it count */
    if (sf) {
        fputs("MARK\n", sf);
        fclose(sf);
    }
    expect("ui: a line typed then shows \"not sent: daemon gone\" within 1 s",
           read_until(master, "not sent: daemon gone", buf, sizeof(buf), 1000));
    char lastset[512] = "", line[512];
    sf = fopen(sets, "r");
    while (sf && fgets(line, sizeof(line), sf))
        snprintf(lastset, sizeof(lastset), "%s", line);
    if (sf)
        fclose(sf);
    snprintf(t, sizeof(t), "ui: ... and chat retuned to RX after it (last SET: %.60s)", lastset);
    expect(t, strncmp(lastset, "SET FREQ=433.900 ", 17) == 0);
    drain_ms(master, 1500);
    expect("ui: ... and is not shown as sent", !file_has(hist, "|Ich|gone-line"));

    /* A CONF peer that accepts and closes at once, then a retune. */
    kill(resp, SIGKILL);
    waitpid(resp, NULL, 0);
    if (write(master, "n1-line\r", 8) != 8) { }
    int closed = 0;
    for (int k = 0; k < 15; k++) {
        int fd = accept_within(cl, 100);
        if (fd >= 0) {
            close(fd);
            closed++;
        }
        drain_ms(master, 10);
    }
    expect("ui: the retune reached the CONF peer that closes at once", closed > 0);
    expect("ui: chat is still running (MSG_NOSIGNAL)", waitpid(pid, NULL, WNOHANG) == 0);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    close(master);
    close(dl);
    close(cl);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("[FAIL] usage: %s <loraham_chat>\n", argv[0]);
        return 1;
    }
    g_bin = argv[1];
    snprintf(g_dir, sizeof(g_dir), "/tmp/lorachat-bin.XXXXXX");
    if (!mkdtemp(g_dir))
        return 1;
    signal(SIGPIPE, SIG_IGN);

    char err[4096], longp[120];
    memset(longp, 'x', 108);
    longp[0] = '/';
    longp[108] = '\0';
    char dead[160];
    snprintf(dead, sizeof(dead), "%s/nobody.sock", g_dir);

    struct { const char *name; char *args[6]; const char *reason; } cases[] = {
        { "empty --data-socket", { (char *)"loraham_chat", (char *)"--data-socket", (char *)"", NULL },
          "--data-socket needs a path" },
        { "108-byte --conf-socket", { (char *)"loraham_chat", (char *)"--conf-socket", longp, NULL },
          "path longer than 107 bytes" },
        { "unknown argument", { (char *)"loraham_chat", (char *)"--bogus", NULL }, "unknown argument: --bogus" },
        { "missing value", { (char *)"loraham_chat", (char *)"--conf-socket", NULL }, "--conf-socket needs a path" },
        { "a data socket nobody listens on", { (char *)"loraham_chat", (char *)"--data-socket", dead, NULL },
          "cannot connect to" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char out[4096];
        int rc = run_notty(cases[i].args, err, sizeof(err), out, sizeof(out));
        char t[160];
        snprintf(t, sizeof(t), "no tty, %s: exit 1", cases[i].name);
        expect(t, rc == 1);
        snprintf(t, sizeof(t), "no tty, %s: the reason on stderr", cases[i].name);
        expect(t, strstr(err, cases[i].reason) != NULL);
        snprintf(t, sizeof(t), "no tty, %s: nothing drawn (stdout empty, no escape sequence anywhere)", cases[i].name);
        expect(t, out[0] == '\0' && strstr(err, "\033[") == NULL);
    }

    /* The real run on a pseudo-terminal against two listeners. */
    char dpath[160], cpath[160];
    snprintf(dpath, sizeof(dpath), "%s/lora-t1.sock", g_dir);
    snprintf(cpath, sizeof(cpath), "%s/loraconf-t1.sock", g_dir);
    int dl = listen_at(dpath), cl = listen_at(cpath);
    expect("two listeners", dl >= 0 && cl >= 0);
    /* TX = RX: a sent line needs no retune, so no GET STATS wait on the
     * (silent) test listener. */
    char conf[200];
    snprintf(conf, sizeof(conf), "%s/lorachat.conf", g_dir);
    FILE *cf = fopen(conf, "w");
    if (cf) {
        fputs("TX=433.900\nRX=433.900\n", cf);
        fclose(cf);
    }
    int master = -1;
    pid_t pid = forkpty(&master, NULL, NULL, NULL);
    if (pid == 0) {
        setenv("TERM", "xterm", 1);
        if (chdir(g_dir) != 0)
            _exit(126);
        signal(SIGPIPE, SIG_DFL);                 /* ours is ignored, and exec keeps that */
        execl(g_bin, "loraham_chat", "--data-socket", dpath, "--conf-socket", cpath, (char *)NULL);
        _exit(127);
    }
    char buf[8192];
    int dfd = accept_within(dl, 5000);
    expect("the binary connects to the given data socket", dfd >= 0);
    int c1 = accept_within(cl, 5000);           /* lorachat_check_conf() */
    int c2 = accept_within(cl, 5000);           /* the receive thread's first CONF request */
    expect("the binary opens the given CONF socket (start check + its first request)", c1 >= 0 && c2 >= 0);
    expect("its first CONF request arrives there: SET FREQ=433.900 ...",
           c2 >= 0 && read_until(c2, "SET FREQ=433.900", buf, sizeof(buf), 3000));
    /* 1.4.2: chat leaves the power alone (what LHPC applied stays). */
    read_until(c2, "\001never\001", buf + strlen(buf), sizeof(buf) - strlen(buf), 300);
    expect("... and it sets no POWER", c2 >= 0 && strstr(buf, "SET FREQ=433.900") && !strstr(buf, "POWER="));
    read_until(master, "> ", buf, sizeof(buf), 2000);   /* the input line is drawn */
    if (write(master, "hello-bin\r", 10) != 10) { }
    expect("a typed line arrives on the data socket as its frame",
           dfd >= 0 && read_until(dfd, "hello-bin", buf, sizeof(buf), 3000));

    /* The daemon gone: the data listener closes its end. First let chat
     * finish drawing the sent line: chat's two threads both draw with
     * ncurses, which is not thread-safe, and overlapping draws garble the
     * screen. */
    read_until(master, "\001never\001", buf, sizeof(buf), 500);
    if (dfd >= 0)
        close(dfd);
    expect("the daemon closes the data socket: chat shows \"daemon gone\"",
           read_until(master, "daemon gone", buf, sizeof(buf), 3000));
    long t0 = proc_cpu_ticks(pid);
    read_until(master, "\001never\001", buf, sizeof(buf), 1000);   /* 1 s, drains the pty */
    long t1 = proc_cpu_ticks(pid);
    long hz = sysconf(_SC_CLK_TCK);
    printf("       chat CPU in the 1 s after the close: %ld ticks (%ld per second)\n", t1 - t0, hz);
    expect("chat is still running after the close (no crash)", t0 >= 0 && t1 >= 0);
    expect("and idle: under 10 % of one CPU in that second (no busy loop on recv() == 0)",
           t0 >= 0 && t1 >= 0 && (t1 - t0) * 10 < hz);

    /* A line typed after "daemon gone": its send hits the closed data
     * socket. Chat must survive it (no SIGPIPE death). */
    if (write(master, "after-gone\r", 11) != 11) { }
    int wst = 0;
    pid_t wr = 0;
    for (int k = 0; k < 20 && wr == 0; k++) {     /* up to 2 s for it to die */
        read_until(master, "\001never\001", buf, sizeof(buf), 100);
        wr = waitpid(pid, &wst, WNOHANG);
    }
    expect("a line typed after \"daemon gone\" does not kill chat (no SIGPIPE)", wr == 0);
    if (wr == pid && WIFSIGNALED(wst))
        printf("       chat ended by signal %d\n", WTERMSIG(wst));
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);

    scenario_one_drawing_thread();

    char cmd[200];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) { }
    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
