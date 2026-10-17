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
 * so no retune wait).
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
    read_until(master, "> ", buf, sizeof(buf), 2000);   /* the input line is drawn */
    if (write(master, "hello-bin\r", 10) != 10) { }
    expect("a typed line arrives on the data socket as its frame",
           dfd >= 0 && read_until(dfd, "hello-bin", buf, sizeof(buf), 3000));
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);

    char cmd[200];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) { }
    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
