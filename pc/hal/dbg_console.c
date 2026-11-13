/* SPDX-License-Identifier: GPL-3.0-or-later */
/* dbg_console.c -- the debug toolkit's live console (DEBUG-ONLY, off by
 * default): PC_DBG_CONSOLE=stdin | PORT | unix:PATH.
 *
 * A line-based text protocol over stdin, a TCP socket on 127.0.0.1 only, or
 * a unix socket; one client at a time.  Each line is one command of
 * pc/hal/dbg_cmds.c (help, info, peek, poke, goto, pause, step, save, load,
 * shot, ...), answered with one line starting `ok` or `err` (on stdout for
 * the stdin console, prefixed `[DBG]`).  The console is polled only at the
 * main-loop frame boundary -- and continuously while the game is paused --
 * so a command never runs inside game code.  pc/tools/dbg_console.py is a
 * small client:
 *
 *   PC_DBG_CONSOLE=7777 ./pc/build/tokimeki-pc ./pc/assets &
 *   python3 pc/tools/dbg_console.py --port 7777 info
 *   python3 pc/tools/dbg_console.py --port 7777       # interactive
 *
 * The transport (sockets, a non-blocking stdin) is the host layer's
 * (pc/hal/dbg_platform.h): BSD sockets on Linux, Winsock and a stdin reader
 * thread on Windows.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dbg_platform.h"

extern void dbg_run_line(char *line, int out_fd);   /* pc/hal/dbg_cmds.c */
extern int dbg_console_ready(void);                  /* 0 while a `step` runs */

static int g_listen = -1, g_client = -1, g_stdin_console;
static char g_inbuf[4096];
static size_t g_inlen;
static int g_eof;

int dbg_console_open(const char *spec) {
    if (!strcmp(spec, "stdin")) {
        g_client = dbg_stdin_open();
        if (g_client < 0) {
            fprintf(stderr, "[DBG] console: cannot read stdin: %s\n", dbg_sock_error());
            return 0;
        }
        g_stdin_console = 1;
        printf("[DBG] console on stdin\n");
        return 1;
    }
    if (!strncmp(spec, "unix:", 5)) {
        g_listen = dbg_sock_listen_unix(spec + 5);
        if (g_listen < 0) {
            fprintf(stderr, "[DBG] console: cannot listen on %s: %s\n", spec, dbg_sock_error());
            return 0;
        }
    } else {
        g_listen = dbg_sock_listen_tcp(atoi(spec));
        if (g_listen < 0) {
            fprintf(stderr, "[DBG] console: cannot listen on 127.0.0.1:%s: %s\n", spec,
                    dbg_sock_error());
            return 0;
        }
    }
    printf("[DBG] console listening on %s\n", spec);
    fflush(stdout);
    return 1;
}

/* the reply of a console line (dbg_cmds.c reply()) */
void dbg_console_send(int h, const char *buf, unsigned long n) {
    dbg_sock_send(h, buf, n);
}

/* Lines run in order; while a `step N` is running (dbg_console_ready() is
 * 0) the following lines wait in the buffer, so `step 30` then `peek x`
 * peeks after the 30 frames.  End of input closes the console once every
 * buffered line has run. */
void dbg_console_poll(void) {
    if (g_listen >= 0 && g_client < 0) {
        int c = dbg_sock_accept(g_listen);
        if (c >= 0) {
            g_client = c;
            g_inlen = 0;
            g_eof = 0;
        }
    }
    if (g_client < 0) return;
    for (;;) {
        char *nl;
        long r;
        while (dbg_console_ready() && (nl = memchr(g_inbuf, '\n', g_inlen)) != NULL) {
            char line[sizeof g_inbuf];
            size_t len = (size_t)(nl - g_inbuf);
            memcpy(line, g_inbuf, len);
            line[len] = 0;
            if (len && line[len - 1] == '\r') line[len - 1] = 0;
            memmove(g_inbuf, nl + 1, g_inlen - len - 1);
            g_inlen -= len + 1;
            /* a web page can POST to 127.0.0.1:PORT and have its body
             * read as commands: an HTTP request line ends the connection
             * before anything after it runs */
            if (!g_stdin_console && strstr(line, "HTTP/")) {
                dbg_sock_close(g_client);
                g_client = -1;
                g_inlen = 0;
                return;
            }
            dbg_run_line(line, g_stdin_console ? -1 : g_client);
            if (g_client < 0) return;
        }
        if (!dbg_console_ready()) return;          /* a step is running */
        if (g_eof) {                               /* input ended, all lines run */
            if (!g_stdin_console) dbg_sock_close(g_client);
            g_client = -1;
            g_inlen = 0;
            return;
        }
        if (g_inlen >= sizeof g_inbuf - 1) {       /* overlong line */
            g_inlen = 0;
            /* (a socket client is dropped: a long HTTP request line must
             * not slip past the check above) */
            if (!g_stdin_console) {
                dbg_sock_close(g_client);
                g_client = -1;
                return;
            }
        }
        r = dbg_sock_recv(g_client, g_inbuf + g_inlen, sizeof g_inbuf - 1 - g_inlen);
        if (r == -1) return;                       /* nothing yet */
        if (r <= 0) { g_eof = 1; continue; }
        g_inlen += (size_t)r;
    }
}
