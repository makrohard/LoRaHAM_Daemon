/*
 * Chat's --data-socket / --conf-socket (clients/chat/README.md). The chat
 * source is compiled in with LORACHAT_NO_MAIN, so every check runs without a
 * terminal:
 *   - no options: each open evaluates loraham_sockpath() as before, with and
 *     without a socket under a temporary /run-style directory;
 *   - each option alone and both, verbatim; -h; an empty path, a 108-byte
 *     path and an unknown argument are refused;
 *   - against two listening sockets named like a daemon started with --id t1:
 *     the data socket connects, and both CONF opens (send_lora_config and
 *     config_query) reach the given CONF socket.
 */
#define LORACHAT_NO_MAIN
#include "../lorachat_ncurses_113.c"

#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int g_ok = 0, g_fail = 0;

static void expect(const char *name, int cond)
{
    if (cond) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s\n", name);
    }
}

static int listen_at(const char *path)
{
    struct sockaddr_un a;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 4) != 0)
        return -1;
    return fd;
}

/* Accept one client and read what it sent (non-blocking after accept). */
static int accept_read(int lfd, char *buf, size_t n)
{
    int c = accept(lfd, NULL, NULL);
    if (c < 0)
        return -1;
    /* The client may connect before it writes: wait for its bytes (2 s),
     * never read at once (a race seen in CI 39102076524). */
    struct pollfd p = { c, POLLIN, 0 };
    (void)poll(&p, 1, 2000);
    ssize_t r = recv(c, buf, n - 1, MSG_DONTWAIT);
    buf[r > 0 ? r : 0] = '\0';
    close(c);
    return 0;
}

static void reset_opts(void)
{
    g_data_socket_opt = NULL;
    g_conf_socket_opt = NULL;
}

static int parse(int argc, char **argv)
{
    reset_opts();
    return lorachat_parse_args(argc, argv);
}

int main(void)
{
    char tmpl[] = "/tmp/loraham-chat-test.XXXXXX";
    char path[160], other[160];
    if (!mkdtemp(tmpl))
        return 1;

    /* Defaults: the same function, the same choice as before. */
    reset_opts();
    snprintf(path, sizeof(path), "%s/run-lora433.sock", tmpl);
    expect("no socket under the /run-style dir: the /tmp fallback",
           strcmp(loraham_sockpath(path, "/tmp/lora433.sock"), "/tmp/lora433.sock") == 0);
    int l0 = listen_at(path);
    expect("a socket under the /run-style dir: that path",
           l0 >= 0 && strcmp(loraham_sockpath(path, "/tmp/lora433.sock"), path) == 0);
    close(l0);
    expect("no options: data_path() is the macro's choice", strcmp(data_path(), SOCKET_PATH) == 0);
    expect("no options: conf_path() is the macro's choice", strcmp(conf_path(), CONFIG_SOCKET_PATH) == 0);

    /* Options. */
    char *a1[] = { "chat", "--data-socket", "/x/lora-t1.sock", NULL };
    expect("--data-socket alone parses", parse(3, a1) == 0);
    expect("--data-socket verbatim", strcmp(data_path(), "/x/lora-t1.sock") == 0);
    expect("--data-socket alone leaves the CONF default", strcmp(conf_path(), CONFIG_SOCKET_PATH) == 0);
    char *a2[] = { "chat", "--conf-socket", "/x/loraconf-t1.sock", NULL };
    expect("--conf-socket alone", parse(3, a2) == 0 && strcmp(conf_path(), "/x/loraconf-t1.sock") == 0 &&
                                  strcmp(data_path(), SOCKET_PATH) == 0);
    char *a3[] = { "chat", "--data-socket", "/x/d", "--conf-socket", "/x/c", NULL };
    expect("both", parse(5, a3) == 0 && strcmp(data_path(), "/x/d") == 0 && strcmp(conf_path(), "/x/c") == 0);
    char *a4[] = { "chat", "-h", NULL };
    expect("-h shows the help (2)", parse(2, a4) == 2);
    char *a5[] = { "chat", "--data-socket", "", NULL };
    expect("an empty path is refused", parse(3, a5) == 1);
    char longp[120];
    memset(longp, 'p', 108);
    longp[0] = '/';
    longp[108] = '\0';
    char *a6[] = { "chat", "--conf-socket", longp, NULL };
    expect("a 108-byte path is refused", parse(3, a6) == 1);
    char *a7[] = { "chat", "--bogus", NULL };
    expect("an unknown argument is refused", parse(2, a7) == 1);
    char *a8[] = { "chat", "--data-socket", NULL };
    expect("an option without its path is refused", parse(2, a8) == 1);

    /* Against sockets named like a daemon with --id t1. */
    snprintf(path, sizeof(path), "%s/lora-t1.sock", tmpl);
    snprintf(other, sizeof(other), "%s/loraconf-t1.sock", tmpl);
    int ld = listen_at(path), lc = listen_at(other);
    expect("test listeners up", ld >= 0 && lc >= 0);
    char *a9[] = { "chat", "--data-socket", path, "--conf-socket", other, NULL };
    parse(5, a9);
    expect("the data socket connects", lorachat_connect_data() == 0);
    char buf[300];
    expect("the data connection reaches lora-t1.sock", accept_read(ld, buf, sizeof(buf)) == 0);
    close(sock_fd);
    expect("the CONF path check at start passes", lorachat_check_conf() == 0);
    accept_read(lc, buf, sizeof(buf));
    send_lora_config("868.1");
    expect("send_lora_config reaches loraconf-t1.sock",
           accept_read(lc, buf, sizeof(buf)) == 0 && strncmp(buf, "SET FREQ=868.1", 14) == 0);
    /* config_query: a second CONF open; the listener answers nothing, the
     * query times out on its own read, the connect is what counts here. */
    char q[64];
    int qfd = fork();
    if (qfd == 0) {
        config_query("GET STATUS\n", q, sizeof(q));
        _exit(0);
    }
    expect("config_query reaches loraconf-t1.sock",
           accept_read(lc, buf, sizeof(buf)) == 0 && strncmp(buf, "GET STATUS", 10) == 0);
    kill(qfd, SIGKILL);
    waitpid(qfd, NULL, 0);

    char *a10[] = { "chat", "--data-socket", "/nonexistent/lora-x.sock", NULL };
    parse(3, a10);
    expect("a given data path that cannot connect: refused (1)", lorachat_connect_data() == 1);

    close(ld);
    close(lc);
    char cmd[200];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpl);
    if (system(cmd) != 0)
        printf("note: could not remove %s\n", tmpl);
    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
