/*
 * multiwatch.c — multiWatch command (Task 6)
 *
 * Implements the multiWatch built-in that runs multiple commands in parallel,
 * re-executing each one every MULTIWATCH_INTERVAL seconds.  Uses poll() to
 * multiplex output from all children.  Continues until the user presses
 * Ctrl+C (which sets tab->in_multiwatch = 0).
 *
 * Output format per command:
 *
 *   "cmd", <unix_timestamp>:
 *   ----------------------------------------------------
 *   <output>
 *   ----------------------------------------------------
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "multiwatch.h"
#include "gui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <ctype.h>
#include <stdarg.h>

/* ── Constants ─────────────────────────────────────────────────────── */

#define MW_SEPARATOR "----------------------------------------------------"
#define MW_POLL_TIMEOUT_MS 200  /* poll timeout in milliseconds */
#define MW_READ_BUF 4096

/* ── Helper: formatted output to tab ──────────────────────────────── */

static void mw_msg(Tab *tab, const char *fmt, ...)
{
    char buf[MAX_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gui_append_text(tab, buf);
    if (g_terminal && !g_terminal->use_process_mode) {
        gui_notify(g_terminal);
    }
}

/* ── Internal: spawn a single command via /bin/sh -c ──────────────── */
/*
 * We use /bin/sh -c so that each "command" can contain pipes, redirections
 * etc.  Returns the child PID, sets *read_fd to the read end of the pipe
 * carrying the child's stdout+stderr.  Returns -1 on error.
 */

static pid_t mw_spawn(const char *cmd_str, int *read_fd)
{
    int pipefd[2];
    if (pipe(pipefd) < 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        /* Restore default signals */
        signal(SIGINT,  SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);

        execlp("/bin/sh", "sh", "-c", cmd_str, (char *)NULL);
        _exit(127);
    }

    /* Parent */
    close(pipefd[1]);
    *read_fd = pipefd[0];
    return pid;
}

/* ══════════════════════════════════════════════════════════════════════
 * multiwatch_run
 *
 * Main entry point.  Runs all commands in parallel, collecting output
 * via poll().  Each command is restarted MULTIWATCH_INTERVAL seconds
 * after it finishes.  Continues until tab->in_multiwatch is cleared.
 * ══════════════════════════════════════════════════════════════════════ */

int multiwatch_run(Terminal *term, Tab *tab, char **cmds, int count)
{
    (void)term;

    if (!tab || !cmds || count <= 0)
        return 1;

    /* Mark multiwatch as active */
    tab->in_multiwatch = 1;
    tab->multiwatch_count = 0;

    /* Per-command state */
    typedef struct {
        pid_t  pid;           /* -1 if not running */
        int    read_fd;       /* pipe read end, -1 if closed */
        int    finished;      /* 1 when child exited and output drained */
        time_t finish_time;   /* time when child finished (for restart delay) */
        char  *output;        /* accumulated output buffer */
        int    output_len;
        int    output_cap;
    } CmdState;

    CmdState *states = calloc((size_t)count, sizeof(CmdState));
    if (!states) {
        mw_msg(tab, "multiWatch: out of memory");
        tab->in_multiwatch = 0;
        return 1;
    }

    /* Initial spawn of all commands */
    for (int i = 0; i < count; i++) {
        states[i].output_cap = MW_READ_BUF;
        states[i].output = malloc((size_t)states[i].output_cap);
        states[i].output_len = 0;
        if (states[i].output)
            states[i].output[0] = '\0';

        states[i].pid = mw_spawn(cmds[i], &states[i].read_fd);
        states[i].finished = 0;
        states[i].finish_time = 0;

        if (states[i].pid > 0 && tab->multiwatch_count < MAX_ARGS) {
            tab->multiwatch_pids[tab->multiwatch_count++] = states[i].pid;
        }
    }

    /* ── Main poll loop ──────────────────────────────────────────── */
    while (tab->in_multiwatch) {
        /* Build the pollfd array for all active read_fds */
        struct pollfd pfds[MAX_ARGS];
        int pfd_map[MAX_ARGS];  /* pfds[j] corresponds to states[pfd_map[j]] */
        int nfds = 0;

        for (int i = 0; i < count; i++) {
            if (states[i].read_fd >= 0) {
                pfds[nfds].fd = states[i].read_fd;
                pfds[nfds].events = POLLIN;
                pfds[nfds].revents = 0;
                pfd_map[nfds] = i;
                nfds++;
            }
        }

        /* Poll with a short timeout so we can check tab->in_multiwatch */
        int ret = 0;
        if (nfds > 0) {
            ret = poll(pfds, (nfds_t)nfds, MW_POLL_TIMEOUT_MS);
        } else {
            /* No active fds — just sleep briefly */
            usleep(MW_POLL_TIMEOUT_MS * 1000);
        }

        if (ret < 0 && errno != EINTR)
            break;

        /* Read available data from any ready fds */
        for (int j = 0; j < nfds; j++) {
            if (pfds[j].revents & (POLLIN | POLLHUP)) {
                int idx = pfd_map[j];
                char buf[MW_READ_BUF];
                ssize_t n = read(states[idx].read_fd, buf, sizeof(buf) - 1);

                if (n > 0) {
                    buf[n] = '\0';
                    /* Append to the command's output buffer */
                    int needed = states[idx].output_len + (int)n + 1;
                    if (needed > states[idx].output_cap) {
                        int newcap = needed * 2;
                        char *newbuf = realloc(states[idx].output, (size_t)newcap);
                        if (newbuf) {
                            states[idx].output = newbuf;
                            states[idx].output_cap = newcap;
                        }
                    }
                    if (states[idx].output) {
                        memcpy(states[idx].output + states[idx].output_len,
                               buf, (size_t)n);
                        states[idx].output_len += (int)n;
                        states[idx].output[states[idx].output_len] = '\0';
                    }
                } else if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
                    /* EOF or error — pipe closed */
                    close(states[idx].read_fd);
                    states[idx].read_fd = -1;
                }
            }
        }

        /* Check for finished children */
        for (int i = 0; i < count; i++) {
            if (states[i].pid > 0 && !states[i].finished) {
                int wstatus;
                pid_t w = waitpid(states[i].pid, &wstatus, WNOHANG);
                if (w > 0 || (w < 0 && errno == ECHILD) || states[i].read_fd < 0) {
                    /* Child has exited — drain remaining output */
                    if (states[i].read_fd >= 0) {
                        char buf[MW_READ_BUF];
                        ssize_t n;
                        while ((n = read(states[i].read_fd, buf,
                                         sizeof(buf) - 1)) > 0) {
                            buf[n] = '\0';
                            int needed = states[i].output_len + (int)n + 1;
                            if (needed > states[i].output_cap) {
                                int newcap = needed * 2;
                                char *nw = realloc(states[i].output,
                                                   (size_t)newcap);
                                if (nw) {
                                    states[i].output = nw;
                                    states[i].output_cap = newcap;
                                }
                            }
                            if (states[i].output) {
                                memcpy(states[i].output + states[i].output_len,
                                       buf, (size_t)n);
                                states[i].output_len += (int)n;
                                states[i].output[states[i].output_len] = '\0';
                            }
                        }
                        close(states[i].read_fd);
                        states[i].read_fd = -1;
                    }

                    /* Print the output block */
                    time_t now = time(NULL);
                    char header[MAX_LINE_LEN];
                    snprintf(header, sizeof(header),
                             "\"%s\", %ld:", cmds[i], (long)now);
                    mw_msg(tab, "%s", header);
                    mw_msg(tab, MW_SEPARATOR);

                    /* Print each line of the output */
                    if (states[i].output && states[i].output_len > 0) {
                        char *line = states[i].output;
                        char *nl;
                        while ((nl = strchr(line, '\n')) != NULL) {
                            *nl = '\0';
                            mw_msg(tab, "%s", line);
                            line = nl + 1;
                        }
                        /* Any trailing partial line */
                        if (*line != '\0') {
                            mw_msg(tab, "%s", line);
                        }
                    }
                    mw_msg(tab, MW_SEPARATOR);

                    states[i].finished = 1;
                    states[i].finish_time = time(NULL);
                    states[i].pid = -1;

                    /* Clear the output buffer for next run */
                    states[i].output_len = 0;
                    if (states[i].output)
                        states[i].output[0] = '\0';
                }
            }
        }

        /* Restart finished commands after MULTIWATCH_INTERVAL seconds */
        time_t now = time(NULL);
        for (int i = 0; i < count; i++) {
            if (states[i].finished &&
                (now - states[i].finish_time) >= MULTIWATCH_INTERVAL) {
                /* Respawn */
                states[i].pid = mw_spawn(cmds[i], &states[i].read_fd);
                states[i].finished = 0;
                states[i].finish_time = 0;

                /* Update the pid tracking array */
                if (states[i].pid > 0) {
                    /* Try to find an empty slot or append */
                    int found_slot = 0;
                    for (int k = 0; k < tab->multiwatch_count; k++) {
                        if (tab->multiwatch_pids[k] <= 0) {
                            tab->multiwatch_pids[k] = states[i].pid;
                            found_slot = 1;
                            break;
                        }
                    }
                    if (!found_slot && tab->multiwatch_count < MAX_ARGS) {
                        tab->multiwatch_pids[tab->multiwatch_count++] =
                            states[i].pid;
                    }
                }
            }
        }
    }

    /* ── Cleanup: kill all running children ──────────────────────── */
    for (int i = 0; i < count; i++) {
        if (states[i].pid > 0) {
            kill(states[i].pid, SIGKILL);
            waitpid(states[i].pid, NULL, 0);
        }
        if (states[i].read_fd >= 0) {
            close(states[i].read_fd);
        }
        free(states[i].output);
    }

    /* Clear the PID tracking */
    for (int i = 0; i < tab->multiwatch_count; i++) {
        tab->multiwatch_pids[i] = 0;
    }
    tab->multiwatch_count = 0;
    tab->in_multiwatch = 0;

    free(states);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 * multiwatch_parse_args
 *
 * Parse a multiWatch argument string of the form:
 *   ["cmd1", "cmd2", "cmd3"]
 *
 * Returns a malloc'd array of malloc'd strings.  Sets *count.
 * Returns NULL on parse error.
 *
 * The parser handles:
 *   - Leading/trailing whitespace
 *   - Double-quoted strings (required)
 *   - Escaped quotes inside strings (\")
 *   - Commas between elements
 * ══════════════════════════════════════════════════════════════════════ */

char **multiwatch_parse_args(const char *arg_str, int *count)
{
    if (!arg_str || !count)
        return NULL;

    *count = 0;

    /* Skip leading whitespace */
    const char *p = arg_str;
    while (*p && isspace((unsigned char)*p)) p++;

    /* Expect opening '[' */
    if (*p != '[')
        return NULL;
    p++;

    /* Allocate result array — start with room for 16 commands */
    int cap = 16;
    char **result = malloc(sizeof(char *) * (size_t)cap);
    if (!result)
        return NULL;

    while (*p) {
        /* Skip whitespace and commas */
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;

        /* Check for closing ']' */
        if (*p == ']')
            break;

        /* Check if element is single-quoted */
        if (*p == '\'') {
            char quote_char = *p++;
            char cmd_buf[MAX_LINE_LEN];
            int ci = 0;
            while (*p && *p != quote_char) {
                if (*p == '\\' && *(p + 1)) p++;
                if (ci < (int)(sizeof(cmd_buf) - 1))
                    cmd_buf[ci++] = *p;
                p++;
            }
            if (*p == quote_char) p++;
            cmd_buf[ci] = '\0';

            if (*count >= cap) {
                cap *= 2;
                char **newres = realloc(result, sizeof(char *) * (size_t)cap);
                if (!newres) { multiwatch_free_args(result, *count); *count = 0; return NULL; }
                result = newres;
            }
            result[*count] = strdup(cmd_buf);
            if (!result[*count]) { multiwatch_free_args(result, *count); *count = 0; return NULL; }
            (*count)++;
            continue;
        }

        /* Check if element is double-quoted */
        if (*p == '"') {
            p++;
            char cmd_buf[MAX_LINE_LEN];
            int ci = 0;
            while (*p && *p != '"') {
                if (*p == '\\' && *(p + 1) == '"') p++;
                if (ci < (int)(sizeof(cmd_buf) - 1))
                    cmd_buf[ci++] = *p;
                p++;
            }
            if (*p == '"') p++;
            cmd_buf[ci] = '\0';

            if (*count >= cap) {
                cap *= 2;
                char **newres = realloc(result, sizeof(char *) * (size_t)cap);
                if (!newres) { multiwatch_free_args(result, *count); *count = 0; return NULL; }
                result = newres;
            }
            result[*count] = strdup(cmd_buf);
            if (!result[*count]) { multiwatch_free_args(result, *count); *count = 0; return NULL; }
            (*count)++;
            continue;
        }

        /* Otherwise, unquoted element (e.g. tokenizer stripped quotes) — read until ',' or ']' */
        char cmd_buf[MAX_LINE_LEN];
        int ci = 0;
        while (*p && *p != ',' && *p != ']') {
            if (ci < (int)(sizeof(cmd_buf) - 1))
                cmd_buf[ci++] = *p;
            p++;
        }
        cmd_buf[ci] = '\0';
        while (ci > 0 && isspace((unsigned char)cmd_buf[ci - 1]))
            cmd_buf[--ci] = '\0';

        if (ci > 0) {
            if (*count >= cap) {
                cap *= 2;
                char **newres = realloc(result, sizeof(char *) * (size_t)cap);
                if (!newres) { multiwatch_free_args(result, *count); *count = 0; return NULL; }
                result = newres;
            }
            result[*count] = strdup(cmd_buf);
            if (!result[*count]) { multiwatch_free_args(result, *count); *count = 0; return NULL; }
            (*count)++;
        }
    }

    if (*count == 0) {
        free(result);
        return NULL;
    }

    return result;
}

/* ══════════════════════════════════════════════════════════════════════
 * multiwatch_free_args
 *
 * Free the array returned by multiwatch_parse_args.
 * ══════════════════════════════════════════════════════════════════════ */

void multiwatch_free_args(char **cmds, int count)
{
    if (!cmds)
        return;

    for (int i = 0; i < count; i++) {
        free(cmds[i]);
    }
    free(cmds);
}
