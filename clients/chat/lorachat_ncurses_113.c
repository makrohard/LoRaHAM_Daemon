/******************************************************************************
 * Copyright (C) 2026  LoRaHAM / Alexander Walter
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <https://www.gnu.org/licenses/>.
 *****************************************************************************/

/*

/*
  sudo apt-get install libncurses5-dev libncursesw5-dev

  gcc lorachat_ncurses_113.c -o lorachat -lncurses -lpthread

 */

#include <errno.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <ctype.h>
#include <time.h>


/* --- Daemon-Socket-Pfadwahl -------------------------------------------------
 * systemd-Deployments servieren die Sockets unter /run/loraham, direkte/
 * Benutzer-Starts unter /tmp (LORAHAM_SOCKET_DIR). Ein Build funktioniert in
 * beiden Welten: nimm den Pfad, unter dem der Daemon-Socket tatsaechlich
 * existiert, sonst den /tmp-Fallback. */
#include <sys/stat.h>
static const char *loraham_sockpath(const char *runp, const char *tmpp)
{
    struct stat st;
    return (stat(runp, &st) == 0 && S_ISSOCK(st.st_mode)) ? runp : tmpp;
}

#define SOCKET_PATH loraham_sockpath("/run/loraham/lora433.sock", "/tmp/lora433.sock")
#define CONFIG_SOCKET_PATH loraham_sockpath("/run/loraham/loraconf433.sock", "/tmp/loraconf433.sock")

/* --data-socket / --conf-socket: a path given on the command line is used as
 * is for every open of that socket; without it each open evaluates the macro
 * above, exactly as before (so a daemon started later is still found). */
static const char *g_data_socket_opt = NULL;
static const char *g_conf_socket_opt = NULL;

static const char *data_path(void) { return g_data_socket_opt ? g_data_socket_opt : SOCKET_PATH; }
static const char *conf_path(void) { return g_conf_socket_opt ? g_conf_socket_opt : CONFIG_SOCKET_PATH; }

static void lorachat_usage(FILE *f)
{
    fprintf(f, "usage: loraham_chat [--data-socket PATH] [--conf-socket PATH]\n"
               "  --data-socket PATH  the daemon's raw data socket"
               " (default /run/loraham/lora433.sock, else /tmp/lora433.sock)\n"
               "  --conf-socket PATH  its CONF socket"
               " (default /run/loraham/loraconf433.sock, else /tmp/loraconf433.sock)\n"
               "  -h, --help          this help\n");
}

/* 0 = go on, 1 = refused (reason + usage on stderr: exit 1), 2 = help shown (exit 0). */
int lorachat_parse_args(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char **target = NULL;
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            lorachat_usage(stdout);
            return 2;
        }
        if (strcmp(a, "--data-socket") == 0)
            target = &g_data_socket_opt;
        else if (strcmp(a, "--conf-socket") == 0)
            target = &g_conf_socket_opt;
        if (!target) {
            fprintf(stderr, "unknown argument: %s\n", a);
            lorachat_usage(stderr);
            return 1;
        }
        if (i + 1 >= argc || argv[i + 1][0] == '\0') {
            fprintf(stderr, "%s needs a path\n", a);
            lorachat_usage(stderr);
            return 1;
        }
        if (strlen(argv[i + 1]) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
            fprintf(stderr, "%s: path longer than 107 bytes: %s\n", a, argv[i + 1]);
            lorachat_usage(stderr);
            return 1;
        }
        *target = argv[++i];
    }
    return 0;
}

/* Open one UNIX socket; -1 with errno on failure. */
static int lorachat_connect_path(const char *path)
{
    struct sockaddr_un addr;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

/* How long to wait for a transmission to complete (see wait_for_tx_complete). */
#define TX_WAIT_POLL_US      100000   /* 100 ms between counter reads          */
#define TX_WAIT_MAX_POLLS    300      /* backstop: 30 s, then retune regardless */
#define TX_WAIT_FALLBACK_US  3000000  /* 3 s, only if the daemon cannot be asked */
#define CONFIG_FILE "lorachat.conf"
#define CHAT_LOG "lorachat.log"
#define MAX_MSG_LEN 256
#define MAX_CALLS 30
#define CHAT_BACKLOG 50

typedef struct {
    char sender[20];
    char text[MAX_MSG_LEN];
    char time_str[9]; // "HH:MM"
    int color_pair;
} ChatLine;

char TX_FREQ[10] = "433.775";
char RX_FREQ[10] = "433.900";

// --- Prototypen ---
void update_footer();
void update_call_list();
void setup_windows();
void to_uppercase(char *str);
void open_config_menu();
int add_call(const char *call);
void add_to_history(const char* sender, const char* text, int pair);
void redraw_chat();
void send_lora_config(const char* freq);
void load_config();
void save_config();
void save_chat_history();
void load_chat_history();

ChatLine chat_history[CHAT_BACKLOG];
int history_count = 0;

char CALL_SIGN[20] = "DL0XXX-10";
char CALL_SIGN_STOP[] = ">";
char APRS_PATH[30] = "APRS,WIDE1-1";
char SEPERATOR[] = "::";
char destination_callsign[10] = "ALL      ";

char heard_calls[MAX_CALLS][20] = {"Calls:", "", "ALL", "APRSPH", "DC2WA-15"};
int call_count = 5;

int sock_fd;
WINDOW *title_win, *call_win, *chat_win, *input_win, *footer_win;

/* --- The line queue: the only thing the receive thread shares ---
 * ncurses is not thread-safe and the history has no lock, so the receive
 * thread never touches either: it pushes each finished line here, and the
 * main thread, which does all drawing and all history writes, drains it.
 * Full: the oldest line is dropped and counted. The mutex is held only for
 * one push or one pop, never across an ncurses call, a send or a wait. */
#define LINE_QUEUE_LEN 64
typedef struct {
    char call[20];             /* a heard call for the call list, or "" */
    char sender[20];
    char text[MAX_MSG_LEN];
    int pair;
} QueuedLine;
static QueuedLine g_lq[LINE_QUEUE_LEN];
static int g_lq_head, g_lq_count;
static unsigned long g_lq_dropped;
static pthread_mutex_t g_lq_mu = PTHREAD_MUTEX_INITIALIZER;

static void line_queue_push(const char *call, const char *sender, const char *text, int pair) {
    pthread_mutex_lock(&g_lq_mu);
    if (g_lq_count == LINE_QUEUE_LEN) {
        g_lq_head = (g_lq_head + 1) % LINE_QUEUE_LEN;
        g_lq_count--;
        g_lq_dropped++;
    }
    QueuedLine *q = &g_lq[(g_lq_head + g_lq_count) % LINE_QUEUE_LEN];
    snprintf(q->call, sizeof(q->call), "%s", call);
    snprintf(q->sender, sizeof(q->sender), "%s", sender);
    snprintf(q->text, sizeof(q->text), "%s", text);
    q->pair = pair;
    g_lq_count++;
    pthread_mutex_unlock(&g_lq_mu);
}

/* One line, oldest first: 1, or 0 when the queue is empty. *dropped gets
 * the lines dropped since the last pop. */
static int line_queue_pop(QueuedLine *out, unsigned long *dropped) {
    int got;
    pthread_mutex_lock(&g_lq_mu);
    got = g_lq_count > 0;
    if (got) {
        *out = g_lq[g_lq_head];
        g_lq_head = (g_lq_head + 1) % LINE_QUEUE_LEN;
        g_lq_count--;
    }
    *dropped = g_lq_dropped;
    g_lq_dropped = 0;
    pthread_mutex_unlock(&g_lq_mu);
    return got;
}

// --- Chat-Sicherung mit Zeitstempel ---

void save_chat_history() {
    FILE *f = fopen(CHAT_LOG, "w");
    if (!f) return;
    for (int i = 0; i < history_count; i++) {
        char clean_text[MAX_MSG_LEN];
        strncpy(clean_text, chat_history[i].text, MAX_MSG_LEN-1);
        clean_text[MAX_MSG_LEN-1] = '\0';

        for (int j = 0; clean_text[j] != '\0'; j++) {
            if ((unsigned char)clean_text[j] < 32 || (unsigned char)clean_text[j] == 127) {
                clean_text[j] = '.';
            }
        }
        // Format: Farbe|Zeit|Absender|Text
        fprintf(f, "%d|%s|%s|%s\n", 
                chat_history[i].color_pair, 
                chat_history[i].time_str, 
                chat_history[i].sender, 
                clean_text);
    }
    fclose(f);
}

void load_chat_history() {
    FILE *f = fopen(CHAT_LOG, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f) && history_count < CHAT_BACKLOG) {
        char *pair_str = strtok(line, "|");
        char *time_str = strtok(NULL, "|");
        char *sender_str = strtok(NULL, "|");
        char *text_str = strtok(NULL, "\n");

        if (pair_str && time_str && sender_str && text_str) {
            chat_history[history_count].color_pair = atoi(pair_str);
            strncpy(chat_history[history_count].time_str, time_str, 8);
            strncpy(chat_history[history_count].sender, sender_str, 19);
            strncpy(chat_history[history_count].text, text_str, MAX_MSG_LEN - 1);
            history_count++;
        }
    }
    fclose(f);
}

// --- Hilfsfunktionen ---

void to_uppercase(char *str) {
    while (*str) { *str = toupper((unsigned char)*str); str++; }
}

void load_config() {
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *val = strchr(line, '=');
        if (!val) continue;
        *val = '\0'; val++;
        val[strcspn(val, "\r\n")] = 0;
        if (strcmp(line, "CALL") == 0) strncpy(CALL_SIGN, val, 19);
        else if (strcmp(line, "PATH") == 0) strncpy(APRS_PATH, val, 29);
        else if (strcmp(line, "DEST") == 0) snprintf(destination_callsign, 10, "%-9s", val);
        else if (strcmp(line, "TX") == 0) strncpy(TX_FREQ, val, 9);
        else if (strcmp(line, "RX") == 0) strncpy(RX_FREQ, val, 9);
    }
    fclose(f);
}

void save_config() {
    FILE *f = fopen(CONFIG_FILE, "w");
    if (!f) return;
    fprintf(f, "CALL=%s\n", CALL_SIGN);
    fprintf(f, "PATH=%s\n", APRS_PATH);
    char temp_dest[10];
    strncpy(temp_dest, destination_callsign, 9);
    temp_dest[9] = '\0';
    char *end = temp_dest + strlen(temp_dest) - 1;
    while(end > temp_dest && isspace((unsigned char)*end)) end--;
    *(end+1) = '\0';
    fprintf(f, "DEST=%s\n", temp_dest);
    fprintf(f, "TX=%s\n", TX_FREQ);
    fprintf(f, "RX=%s\n", RX_FREQ);
    fclose(f);
}

void send_lora_config(const char* freq) {
    int cfg_fd;
    struct sockaddr_un addr;
    char config_cmd[256];
    cfg_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (cfg_fd < 0) return;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, conf_path(), sizeof(addr.sun_path)-1);
    if (connect(cfg_fd, (struct sockaddr*)&addr, sizeof(addr)) != -1) {
        snprintf(config_cmd, sizeof(config_cmd),
                 "SET FREQ=%s SF=12 BW=125 CR=5 CRC=1 PREAMBLE=8 SYNC=0x12 LDRO=AUTO",
                 freq);
        send(cfg_fd, config_cmd, strlen(config_cmd), MSG_NOSIGNAL);
    }
    close(cfg_fd);
}

/* A line the daemon sends on its own, never a reply (docs/conf-protocol.md,
 * Unsolicited broadcasts). */
static int conf_is_broadcast(const char *line) {
    return strncmp(line, "TX=", 3) == 0 || strncmp(line, "CAD=", 4) == 0 ||
           strncmp(line, "RSSI=", 5) == 0 || strncmp(line, "RADIO=", 6) == 0;
}

/*
 * Ask the daemon one question and read its answer. `send_lora_config` above
 * deliberately stays write-only (a SET needs no answer);
 * this is the read path used to find out when a transmission has finished.
 * Broadcast lines before the answer are dropped (a partial line is kept for
 * the next recv); `out` starts at the first complete other line. EOF or a line
 * that does not fit: -1.
 */
static int config_query(const char *cmd, char *out, size_t out_len) {
    int fd;
    struct sockaddr_un addr;
    ssize_t n;
    size_t len = 0;
    char *nl;
    if (!out || out_len == 0) return -1;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, conf_path(), sizeof(addr.sun_path)-1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) { close(fd); return -1; }
    if (send(fd, cmd, strlen(cmd), MSG_NOSIGNAL) < 0) { close(fd); return -1; }
    while (len < out_len - 1) {
        n = recv(fd, out + len, out_len - 1 - len, 0);
        if (n <= 0) break;
        len += (size_t)n;
        out[len] = '\0';
        while ((nl = strchr(out, '\n')) != NULL) {
            if (!conf_is_broadcast(out)) { close(fd); return 0; }
            len -= (size_t)(nl + 1 - out);
            memmove(out, nl + 1, len + 1);
        }
    }
    close(fd);
    return -1;
}

/*
 * How many transmissions the daemon has FINISHED WITH, by any route: TXOK+TXERR
 * plus TXBUSY. TXBUSY counts a frame the daemon refused because the radio was
 * already busy -- it never becomes TXOK or TXERR, so leaving it out would mean a
 * refused frame moves no counter at all and we would sit out the whole backstop
 * before retuning. The frame is gone in that case too, so retuning at once is
 * both quicker and the honest answer.
 *
 * Returns -1 when the daemon cannot be asked. GET STATS is a counter read: it
 * never touches the radio, so polling it is free.
 */
static long tx_completed_count(void) {
    char buf[512];
    const char *ok, *err, *busy;
    if (config_query("GET STATS\n", buf, sizeof(buf)) != 0) return -1;
    ok   = strstr(buf, "TXOK=");
    err  = strstr(buf, "TXERR=");
    busy = strstr(buf, "TXBUSY=");
    if (!ok || !err || !busy) return -1;
    return strtol(ok + 5, NULL, 10)
         + strtol(err + 6, NULL, 10)
         + strtol(busy + 7, NULL, 10);
}

/*
 * Block until the frame we just handed over has actually left the radio.
 *
 * A fixed sleep cannot do this. Time-on-air alone is ~2.6 s for a short frame at
 * SF12/BW125, and in MANAGED mode the daemon additionally waits for a clear
 * channel (CAD/LBT) and may queue behind another stack — a delay with no upper
 * bound that any constant would eventually lose to. So wait on the daemon's own
 * completion counter instead, and treat the timeout purely as a backstop.
 */
static int tx_busy_state(void) {
    char buf[512];
    const char *b;
    /* NOSCAN: the passive form. The plain GET CHANNEL runs a CAD scan, which
       would fight the very transmission we are waiting on. */
    if (config_query("GET CHANNEL NOSCAN\n", buf, sizeof(buf)) != 0) return -1;
    b = strstr(buf, "BUSY=");
    if (!b) return -1;
    return strtol(b + 5, NULL, 10) != 0 ? 1 : 0;
}

static void wait_for_tx_complete(long before) {
    int i;
    if (before < 0) {          /* stats unavailable: fall back to a generous wait */
        usleep(TX_WAIT_FALLBACK_US);
        return;
    }
    for (i = 0; i < TX_WAIT_MAX_POLLS; i++) {
        long now;
        usleep(TX_WAIT_POLL_US);
        now = tx_completed_count();
        if (now < 0 || now <= before) continue;
        /* The counter belongs to the BAND daemon, not to us: another client of the
           same band moves it too. The BUSY check below catches the case where such
           a frame finishes while OURS is in the air.

           It does not catch the other one -- a foreign frame finishing while ours
           is still queued behind the channel-free wait, because tx_busy is only
           set at acquire, after that wait, so BUSY reads 0 for a frame that has
           not started. What makes this safe is not the check but the product
           rule: one app stack per band daemon (chat is refused while another
           holds 433), so in a supported deployment this counter IS ours alone. A
           second client started by hand on the same band sits outside that rule.

           Closing it properly belongs in the daemon, which is the only party that
           knows when its TX worker released the radio -- a daemon behaviour
           change, not a client patch. So: rule is the proof, check is the belt. */
        if (tx_busy_state() != 1) return;
    }
}

void add_to_history(const char* sender, const char* text, int pair) {
    ChatLine new_line;
    strncpy(new_line.sender, sender, 19);
    strncpy(new_line.text, text, MAX_MSG_LEN - 1);
    new_line.color_pair = pair;

    // Aktuelle Uhrzeit holen
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(new_line.time_str, sizeof(new_line.time_str), "%H:%M", t);

    if (history_count < CHAT_BACKLOG) {
        chat_history[history_count++] = new_line;
    } else {
        for (int i = 1; i < CHAT_BACKLOG; i++) chat_history[i-1] = chat_history[i];
        chat_history[CHAT_BACKLOG-1] = new_line;
    }
    save_chat_history();
}

void redraw_chat() {
    wclear(chat_win);
    for (int i = 0; i < history_count; i++) {
        // 1. Uhrzeit (Dunkelblau / Pair 7)
        wattron(chat_win, COLOR_PAIR(7));
        wprintw(chat_win, "%s ", chat_history[i].time_str);
        wattroff(chat_win, COLOR_PAIR(7));

        // 2. Rufzeichen (Fett)
        wattron(chat_win, COLOR_PAIR(chat_history[i].color_pair) | A_BOLD);
        wprintw(chat_win, "%-10s: ", chat_history[i].sender);
        wattroff(chat_win, A_BOLD);

        // 3. Nachrichtentext
        if (chat_history[i].color_pair == 3) {
            wattroff(chat_win, COLOR_PAIR(3));
            wattron(chat_win, COLOR_PAIR(6));
            wprintw(chat_win, "%s\n", chat_history[i].text);
            wattroff(chat_win, COLOR_PAIR(6));
        } else {
            wprintw(chat_win, "%s\n", chat_history[i].text);
            wattroff(chat_win, COLOR_PAIR(2));
        }
    }
    wnoutrefresh(chat_win);
}

void update_call_list() {
    wclear(call_win);
    wbkgd(call_win, COLOR_PAIR(4));
    for (int i = 0; i < call_count; i++) {
        if (i == 0) wattron(call_win, A_BOLD);
        mvwprintw(call_win, i, 1, "%-10s", heard_calls[i]);
        if (i == 0) wattroff(call_win, A_BOLD);
    }
    wnoutrefresh(call_win);
}

void update_footer() {
    int h, w; getmaxyx(stdscr, h, w);
    wclear(footer_win);
    wbkgd(footer_win, COLOR_PAIR(1));
    wattron(footer_win, COLOR_PAIR(1));
    wprintw(footer_win, " Ctrl-K: Konf | TX: %s | RX: %s | Ziel: ", TX_FREQ, RX_FREQ);
    wattron(footer_win, COLOR_PAIR(5) | A_BOLD);
    waddstr(footer_win, destination_callsign);
    wattroff(footer_win, COLOR_PAIR(5) | A_BOLD);
    wnoutrefresh(footer_win);
}

/* 1 when the call is new to the list (the caller redraws it). */
int add_call(const char *call) {
    if (!call || strlen(call) < 3) return 0;
    char up_call[20];
    strncpy(up_call, call, 19);
    up_call[19] = '\0';
    to_uppercase(up_call);
    for (int i = 0; i < call_count; i++) {
        if (strcmp(heard_calls[i], up_call) == 0) return 0;
    }
    if (call_count < MAX_CALLS) {
        strncpy(heard_calls[call_count++], up_call, 19);
        return 1;
    }
    return 0;
}

/* Main thread only: the queued lines into the call list and the history;
 * with `draw` also onto the screen (not while the config menu covers it:
 * closing the menu redraws everything). */
static void drain_lines(int draw) {
    QueuedLine l;
    unsigned long dropped, lost = 0;
    int any = 0, calls = 0;
    while (line_queue_pop(&l, &dropped)) {
        lost += dropped;
        if (l.call[0] && add_call(l.call))
            calls = 1;
        add_to_history(l.sender, l.text, l.pair);
        any = 1;
    }
    if (lost) {
        char note[48];
        snprintf(note, sizeof(note), "(%lu lines dropped)", lost);
        add_to_history("System", note, 4);
    }
    if (!draw)
        return;
    if (calls)
        update_call_list();
    if (any) {
        redraw_chat();
        doupdate();
    }
}

void setup_windows() {
    int h, w; getmaxyx(stdscr, h, w);
    if(title_win) delwin(title_win);
    if(call_win) delwin(call_win);
    if(chat_win) delwin(chat_win);
    if(input_win) delwin(input_win);
    if(footer_win) delwin(footer_win);
    clear(); refresh();
    title_win = newwin(1, w, 0, 0);
    call_win = newwin(h - 3, 11, 1, 0);
    chat_win = newwin(h - 3, w - 11, 1, 11);
    input_win = newwin(1, w - 2, h - 2, 1);
    /* One thread draws: the input read returns every 100 ms so the main
       loop drains the received lines (a new window starts blocking). */
    wtimeout(input_win, 100);
    footer_win = newwin(1, w, h - 1, 0);
    wbkgd(title_win, COLOR_PAIR(1));
    wbkgd(chat_win, COLOR_PAIR(4));
    scrollok(chat_win, TRUE);
    mvwprintw(title_win, 0, (w - 20) / 2, "LoRaHAM_Pi Chat (C)");
    wnoutrefresh(title_win);
    redraw_chat();
}

void open_config_menu() {
    int h, w; getmaxyx(stdscr, h, w);
    int win_h = 14, win_w = 50;
    WINDOW *cfg_win = newwin(win_h, win_w, (h - win_h) / 2, (w - win_w) / 2);
    keypad(cfg_win, TRUE);
    wbkgd(cfg_win, COLOR_PAIR(1));
    while (1) {
        wclear(cfg_win); box(cfg_win, 0, 0);
        mvwprintw(cfg_win, 1, (win_w - 15) / 2, " KONFIGURATION ");
        mvwprintw(cfg_win, 3, 2, "1. Eigenes Call: %s", CALL_SIGN);
        mvwprintw(cfg_win, 4, 2, "2. APRS-Pfad   : %s", APRS_PATH);
        mvwprintw(cfg_win, 5, 2, "3. Ziel-Call   : %s", destination_callsign);
        mvwprintw(cfg_win, 6, 2, "4. TX Frequenz : %s", TX_FREQ);
        mvwprintw(cfg_win, 7, 2, "5. RX Frequenz : %s", RX_FREQ);
        mvwprintw(cfg_win, 10, 2, "Wähle 1-5 oder ESC zum Beenden");
        wrefresh(cfg_win);
        /* The choice: a 100 ms read, so received lines reach the history
           while the menu is open. */
        wtimeout(cfg_win, 100);
        int ch;
        while ((ch = wgetch(cfg_win)) == ERR)
            drain_lines(0);
        if (ch == 27) break;
        /* A field: a blocking line read, so a typing pause never ends or
           cuts it; lines queue meanwhile and drain after it. */
        wtimeout(cfg_win, -1);
        echo(); curs_set(1);
        if (ch == '1') {
            mvwgetnstr(cfg_win, 8, 14, CALL_SIGN, 15); to_uppercase(CALL_SIGN);
        } else if (ch == '2') {
            mvwgetnstr(cfg_win, 8, 14, APRS_PATH, 25);
        } else if (ch == '3') {
            char b[10]; mvwgetnstr(cfg_win, 8, 14, b, 9); to_uppercase(b);
            snprintf(destination_callsign, 10, "%-9s", b);
        } else if (ch == '4') {
            mvwgetnstr(cfg_win, 8, 14, TX_FREQ, 9);
        } else if (ch == '5') {
            mvwgetnstr(cfg_win, 8, 14, RX_FREQ, 9);
            send_lora_config(RX_FREQ);
        }
        noecho(); curs_set(0);
    }
    save_config();
    delwin(cfg_win);
    setup_windows();
    update_call_list();
    update_footer();
    doupdate();
}

void send_aprs(const char *text) {
    long tx_before;
    int split_freq;
    if (strlen(text) == 0) return;
    /* With TX == RX there is nothing to retune, so there is nothing to wait for:
       skip the whole dance and keep that configuration as fast as it ever was. */
    split_freq = (strcmp(TX_FREQ, RX_FREQ) != 0);
    /* Snapshot BEFORE retuning: we wait for this number to move, so a stale
       read here would be indistinguishable from a completed transmit. */
    tx_before = split_freq ? tx_completed_count() : -2;
    send_lora_config(TX_FREQ);
    usleep(50000);
    unsigned char packet[1024];
    int len = 0;
    packet[len++] = 0x3c; packet[len++] = 0xff; packet[len++] = 0x01;
    len += sprintf((char*)&packet[len], "%s%s%s%s%s:%s",
                   CALL_SIGN, CALL_SIGN_STOP, APRS_PATH, SEPERATOR, destination_callsign, text);
    /* MSG_NOSIGNAL: a closed daemon socket must not kill chat (SIGPIPE). */
    if (send(sock_fd, packet, len, MSG_NOSIGNAL) != len) {
        /* Not sent (an error or a short write): nothing to wait for. */
        if (split_freq)
            send_lora_config(RX_FREQ);
        add_to_history("System", "not sent: daemon gone", 4);
        redraw_chat();
        return;
    }
    if (split_freq) {
        /* The frame is only QUEUED at this point. Retuning now would move the
           radio out from under a transmission still in progress -- that is the
           bug this waits out. */
        wait_for_tx_complete(tx_before);
        send_lora_config(RX_FREQ);
    }
    add_to_history("Ich", text, 2);
    redraw_chat();
}

/* Receives and parses frames; never draws and never writes the history
   (the line queue above). */
void *receive_thread(void *arg) {
    unsigned char buf[1024];
    while (1) {
        int n = recv(sock_fd, buf, sizeof(buf) - 1, 0);
        if (n == 0 || (n < 0 && errno != EINTR)) {
            /* The daemon closed the data socket (stopped, restarted or gone):
               say so once and end this thread; chat does not reconnect.
               Looping on would get 0 again at once: a busy loop. */
            line_queue_push("", "System", "daemon gone: restart chat", 4);
            break;
        }
        if (n > 0) {
            buf[n] = '\0';
            char sender[20] = "UNK";
            char call[20] = "";
            char *msg_content = "---";
            char *tag_open = strchr((char*)buf, '<');
            char *tag_close = strchr((char*)buf, '>');
            if (tag_open && tag_close && (tag_close > tag_open)) {
                char *call_start = tag_open + 3;
                int s_len = tag_close - call_start;
                if (s_len > 0 && s_len < 20) {
                    strncpy(sender, call_start, s_len);
                    sender[s_len] = '\0';
                    snprintf(call, sizeof(call), "%s", sender);
                }
            }
            char *first_colon = strchr((char*)buf, ':');
            if (first_colon) {
                if (*(first_colon + 1) == ':') msg_content = first_colon + 2;
                else msg_content = first_colon + 1;
            }
            for (int i = 0; msg_content[i] != '\0'; i++) {
                if ((unsigned char)msg_content[i] < 32 && msg_content[i] != '\n') {
                    msg_content[i] = ' ';
                }
            }
            line_queue_push(call, sender, msg_content, 3);
        }
    }
    return NULL;
}

/* The data socket: 0, or 1 after printing why (before ncurses starts). */
int lorachat_connect_data(void)
{
    sock_fd = lorachat_connect_path(data_path());
    if (sock_fd >= 0)
        return 0;
    if (g_data_socket_opt)
        fprintf(stderr, "cannot connect to %s: %s\n", g_data_socket_opt, strerror(errno));
    else
        printf("Socket Fehler!\n");
    return 1;
}

/* A --conf-socket path is checked once at start (the default is not: CONF
 * opens stay per command, as before). */
int lorachat_check_conf(void)
{
    if (!g_conf_socket_opt)
        return 0;
    int fd = lorachat_connect_path(g_conf_socket_opt);
    if (fd < 0) {
        fprintf(stderr, "cannot connect to %s: %s\n", g_conf_socket_opt, strerror(errno));
        return 1;
    }
    close(fd);
    return 0;
}

#ifndef LORACHAT_NO_MAIN
int main(int argc, char **argv) {
    int pa = lorachat_parse_args(argc, argv);
    if (pa == 2) return 0;
    if (pa == 1) return 1;
    load_config();
    load_chat_history();

    if (lorachat_connect_data() != 0) return 1;
    if (lorachat_check_conf() != 0) return 1;
    initscr(); start_color(); cbreak(); noecho(); curs_set(0);
    init_pair(1, COLOR_WHITE,  COLOR_BLUE);
    init_pair(2, COLOR_CYAN,   COLOR_BLACK);
    init_pair(3, COLOR_GREEN,  COLOR_BLACK);
    init_pair(4, COLOR_YELLOW, COLOR_BLACK);
    init_pair(5, COLOR_RED,    COLOR_BLUE);
    init_pair(6, 11,           COLOR_BLACK);
    init_pair(7, COLOR_BLUE,   COLOR_BLACK); // FARBE FÜR DIE UHRZEIT
    
    setup_windows(); update_footer(); update_call_list(); doupdate();
    send_lora_config(RX_FREQ);
    pthread_t tid;
    pthread_create(&tid, NULL, receive_thread, NULL);
    char input_buf[256];
    int pos = 0;
    while (1) {
        int ch = wgetch(input_win);
        drain_lines(1);
        if (ch == ERR) continue;
        if (ch == KEY_RESIZE) {
            setup_windows(); update_footer(); update_call_list(); doupdate();
            mvwaddstr(input_win, 0, 0, "> "); waddnstr(input_win, input_buf, pos);
        } else if (ch == 11 || ch == 18) {
            open_config_menu();
        } else if (ch == 10 || ch == 13) {
            input_buf[pos] = '\0'; send_aprs(input_buf);
            pos = 0; wclear(input_win); mvwaddstr(input_win, 0, 0, "> ");
        } else if (ch >= 32 && ch <= 126 && pos < 255) {
            input_buf[pos++] = ch; waddch(input_win, ch);
        } else if ((ch == KEY_BACKSPACE || ch == 127 || ch == 8) && pos > 0) {
            pos--; mvwaddch(input_win, 0, pos + 2, ' '); wmove(input_win, 0, pos + 2);
        }
        wrefresh(input_win);
    }
    endwin(); return 0;
}
#endif /* LORACHAT_NO_MAIN */
