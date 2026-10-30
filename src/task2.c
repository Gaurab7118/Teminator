/*
 * task2.c — Task 2: Built-in and External Commands
 * Computing Lab (CS69201) · Term Project
 *
 * Implements shell command execution and all required built-in commands:
 *   - External commands executed via fork() + execvp() and waited for
 *   - Built-ins modifying shell state: cd, exit, history, multiWatch,
 *     jobs/fg/bg, source, and export
 *   - Error reporting for unknown commands without crashing the tab
 *   - Last exit status tracked in tab->last_exit_status ($?)
 *   - Direct interpretation of bash scripts without handing to /bin/bash
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "shell.h"
#include "builtins.h"
#include "tab.h"
#include "parser.h"
#include "pipes.h"
#include "redirect.h"
#include "jobs.h"
#include "gui.h"
#include "script.h"
#include "history.h"
#include "multiwatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <pwd.h>
#include <stdarg.h>

/* ── Helper: output a formatted message to the tab ────────────────── */
static void shell_output_msg(Tab *tab, const char *fmt, ...)
{
    char buf[MAX_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gui_append_text(tab, buf);
}

/* ── Check if a file is a script (ends in .sh or starts with #!) ─── */
static int is_script_file(const char *path, const char *cwd)
{
    if (!path || !*path) return 0;
    size_t len = strlen(path);
    if (len >= 3 && strcmp(path + len - 3, ".sh") == 0) return 1;

    char resolved[MAX_PATH_LEN];
    const char *target = path;
    if (path[0] != '/' && cwd && cwd[0]) {
        snprintf(resolved, sizeof(resolved), "%s/%s", cwd, path);
        target = resolved;
    }

    FILE *fp = fopen(target, "r");
    if (fp) {
        char magic[3] = {0};
        size_t r = fread(magic, 1, 2, fp);
        fclose(fp);
        if (r == 2 && magic[0] == '#' && magic[1] == '!') return 1;
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  SECTION 1: Shell Execution Engine
 * ══════════════════════════════════════════════════════════════════════ */

int shell_execute_line(Terminal *term, Tab *tab, const char *line)
{
    if (!term && g_terminal)
        term = g_terminal;

    if (!tab || !line)
        return -1;

    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '\0')
        return 0;

    char *subst_expanded = parser_expand_substitutions(line, term, tab);
    if (!subst_expanded) {
        shell_output_msg(tab, "terminator: substitution error");
        tab->last_exit_status = 1;
        return 1;
    }

    char *expanded = parser_expand_variables(subst_expanded, tab);
    free(subst_expanded);
    if (!expanded) {
        shell_output_msg(tab, "terminator: variable expansion error");
        tab->last_exit_status = 1;
        return 1;
    }

    Command *cmd = parser_parse(expanded);
    free(expanded);

    if (!cmd) {
        tab->last_exit_status = 2;
        return 2;
    }

    int status = 0;

    if (cmd->pipe_next != NULL) {
        status = pipes_execute_pipeline(term, tab, cmd);
    } else {
        if (cmd->argc == 0) {
            status = 0;
        } else if (builtin_is_builtin(cmd->args[0])) {
            status = builtin_execute(term, tab, cmd);
        } else if (strcmp(cmd->args[0], "bash") == 0 && cmd->argc >= 2) {
            const char *sp = cmd->args[1];
            char resolved[MAX_PATH_LEN * 2];
            if (sp[0] != '/' && tab && tab->cwd[0] != '\0') {
                snprintf(resolved, sizeof(resolved), "%s/%s", tab->cwd, sp);
                if (access(resolved, R_OK) == 0) sp = resolved;
            }
            status = script_execute(term, tab, sp, &cmd->args[2], cmd->argc - 2);
        } else if (is_script_file(cmd->args[0], tab->cwd)) {
            const char *sp = cmd->args[0];
            char resolved[MAX_PATH_LEN * 2];
            if (sp[0] != '/' && tab && tab->cwd[0] != '\0') {
                snprintf(resolved, sizeof(resolved), "%s/%s", tab->cwd, sp);
                if (access(resolved, R_OK) == 0) sp = resolved;
            }
            status = script_execute(term, tab, sp, &cmd->args[1], cmd->argc - 1);
        } else {
            status = shell_run_external(term, tab, cmd, -1, -1);
        }
    }

    tab->last_exit_status = status;
    parser_free_command(cmd);

    return status;
}

int shell_execute_command(Terminal *term, Tab *tab, Command *cmd)
{
    if (!term && g_terminal)
        term = g_terminal;

    if (!tab || !cmd)
        return -1;

    if (cmd->argc == 0)
        return 0;

    if (builtin_is_builtin(cmd->args[0])) {
        return builtin_execute(term, tab, cmd);
    }

    if (strcmp(cmd->args[0], "bash") == 0 && cmd->argc >= 2) {
        return script_execute(term, tab, cmd->args[1], &cmd->args[2], cmd->argc - 2);
    }
    if (is_script_file(cmd->args[0], tab->cwd)) {
        return script_execute(term, tab, cmd->args[0], &cmd->args[1], cmd->argc - 1);
    }

    return shell_run_external(term, tab, cmd, -1, -1);
}

int shell_execute_pipeline(Terminal *term, Tab *tab, Command *pipeline)
{
    if (!term && g_terminal)
        term = g_terminal;

    if (!tab || !pipeline)
        return -1;

    return pipes_execute_pipeline(term, tab, pipeline);
}

int shell_run_external(Terminal *term, Tab *tab, Command *cmd,
                       int input_fd, int output_fd)
{
    if (!term && g_terminal)
        term = g_terminal;

    if (!tab || !cmd || cmd->argc == 0)
        return -1;

    int capture_pipe[2] = { -1, -1 };
    int standalone = (input_fd < 0 && output_fd < 0);

    if (standalone) {
        if (pipe(capture_pipe) < 0) {
            shell_output_msg(tab, "terminator: pipe: %s", strerror(errno));
            return 1;
        }
    }

    if (redirect_validate(term, tab, cmd) < 0) {
        if (standalone) {
            close(capture_pipe[0]);
            close(capture_pipe[1]);
        }
        return 1;
    }

    pid_t pid = fork();

    if (pid < 0) {
        shell_output_msg(tab, "terminator: fork: %s", strerror(errno));
        if (standalone) {
            close(capture_pipe[0]);
            close(capture_pipe[1]);
        }
        return 1;
    }

    if (pid == 0) {
        setpgid(0, 0);

        if (input_fd >= 0) {
            dup2(input_fd, STDIN_FILENO);
            close(input_fd);
        }
        if (output_fd >= 0) {
            dup2(output_fd, STDOUT_FILENO);
            close(output_fd);
        }

        if (standalone) {
            close(capture_pipe[0]);
            dup2(capture_pipe[1], STDOUT_FILENO);
            dup2(capture_pipe[1], STDERR_FILENO);
            close(capture_pipe[1]);
        }

        if (redirect_setup(cmd) < 0) {
            fprintf(stderr, "terminator: redirect failed\n");
            _exit(126);
        }

        signal(SIGINT,  SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);

        if (tab->cwd[0] != '\0') {
            if (chdir(tab->cwd) != 0) {
            }
        }

        execvp(cmd->args[0], cmd->args);

        fprintf(stderr, "terminator: %s: command not found\n", cmd->args[0]);
        _exit(127);
    }

    setpgid(pid, pid);

    if (standalone) {
        close(capture_pipe[1]);
    }

    if (input_fd >= 0)
        close(input_fd);
    if (output_fd >= 0)
        close(output_fd);

    if (cmd->background) {
        char cmdline[MAX_LINE_LEN];
        cmdline[0] = '\0';
        for (int i = 0; i < cmd->argc; i++) {
            if (i > 0) strncat(cmdline, " ", sizeof(cmdline) - strlen(cmdline) - 1);
            strncat(cmdline, cmd->args[i], sizeof(cmdline) - strlen(cmdline) - 1);
        }

        int job_id = jobs_add(tab, pid, pid, cmdline, JOB_BACKGROUND);
        shell_output_msg(tab, "[%d] %d", job_id, (int)pid);

        if (standalone && capture_pipe[0] >= 0) {
            close(capture_pipe[0]);
        }

        return 0;
    }

    tab->fg_pid  = pid;
    tab->fg_pgid = pid;

    if (standalone && capture_pipe[0] >= 0) {
        shell_capture_output(term, tab, capture_pipe[0]);
        close(capture_pipe[0]);
    }

    int status = shell_wait_fg(tab, pid);
    return status;
}

int shell_wait_fg(Tab *tab, pid_t pid)
{
    if (!tab || pid <= 0)
        return -1;

    int wstatus;

    for (;;) {
        pid_t w = waitpid(pid, &wstatus, WUNTRACED);

        if (w < 0) {
            if (errno == EINTR)
                continue;
            return 1;
        }

        if (WIFSTOPPED(wstatus)) {
            char cmdname[MAX_LINE_LEN];
            snprintf(cmdname, sizeof(cmdname), "pid %d", (int)pid);

            for (int i = 0; i < tab->num_jobs; i++) {
                if (tab->jobs[i].active && tab->jobs[i].pid == pid) {
                    strncpy(cmdname, tab->jobs[i].command, sizeof(cmdname) - 1);
                    cmdname[sizeof(cmdname) - 1] = '\0';
                    tab->jobs[i].state = JOB_STOPPED;
                    break;
                }
            }

            int job_id = jobs_add(tab, pid, pid, cmdname, JOB_STOPPED);
            shell_output_msg(tab, "\n[%d]+  Stopped                 %s",
                             job_id, cmdname);
            (void)job_id;

            tab->fg_pid  = 0;
            tab->fg_pgid = 0;

            return 128 + WSTOPSIG(wstatus);
        }

        if (WIFEXITED(wstatus)) {
            tab->fg_pid  = 0;
            tab->fg_pgid = 0;
            return WEXITSTATUS(wstatus);
        }

        if (WIFSIGNALED(wstatus)) {
            tab->fg_pid  = 0;
            tab->fg_pgid = 0;
            return 128 + WTERMSIG(wstatus);
        }
    }
}

void shell_capture_output(Terminal *term, Tab *tab, int fd)
{
    (void)term;
    if (fd < 0)
        return;

    char buf[4096];
    char linebuf[MAX_LINE_LEN];
    int  line_pos = 0;
    ssize_t n;

    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';

        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                linebuf[line_pos] = '\0';
                gui_append_text(tab, linebuf);
                line_pos = 0;
            } else {
                if (line_pos < (int)(sizeof(linebuf) - 1)) {
                    linebuf[line_pos++] = buf[i];
                }
            }
        }
    }

    if (line_pos > 0) {
        linebuf[line_pos] = '\0';
        gui_append_text(tab, linebuf);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  SECTION 2: Shell Built-in Commands
 * ══════════════════════════════════════════════════════════════════════ */

static const char *builtin_names[] = {
    "cd", "exit", "history", "jobs", "fg", "bg",
    "source", "multiWatch", "multiwatch", "export", "clear", NULL
};

int builtin_is_builtin(const char *name)
{
    if (!name) return 0;
    for (int i = 0; builtin_names[i] != NULL; i++) {
        if (strcmp(name, builtin_names[i]) == 0)
            return 1;
    }
    return 0;
}

int builtin_execute(Terminal *term, Tab *tab, Command *cmd)
{
    if (!tab || !cmd || cmd->argc == 0)
        return 1;

    const char *name = cmd->args[0];

    if (strcmp(name, "cd") == 0)
        return builtin_cd(term, tab, cmd);
    if (strcmp(name, "exit") == 0)
        return builtin_exit_cmd(term, tab, cmd);
    if (strcmp(name, "history") == 0)
        return builtin_history(term, tab, cmd);
    if (strcmp(name, "jobs") == 0)
        return builtin_jobs(term, tab, cmd);
    if (strcmp(name, "fg") == 0)
        return builtin_fg(term, tab, cmd);
    if (strcmp(name, "bg") == 0)
        return builtin_bg(term, tab, cmd);
    if (strcmp(name, "source") == 0)
        return builtin_source(term, tab, cmd);
    if (strcmp(name, "multiWatch") == 0 || strcmp(name, "multiwatch") == 0)
        return builtin_multiwatch(term, tab, cmd);
    if (strcmp(name, "export") == 0)
        return builtin_export(term, tab, cmd);
    if (strcmp(name, "clear") == 0) {
        gui_clear_tab(tab);
        return 0;
    }

    shell_output_msg(tab, "terminator: %s: unknown builtin", name);
    return 1;
}

int builtin_cd(Terminal *term, Tab *tab, Command *cmd)
{
    (void)term;
    const char *target = NULL;
    char path_buf[MAX_PATH_LEN];

    if (cmd->argc < 2 || cmd->args[1] == NULL) {
        target = getenv("HOME");
        if (!target) {
            struct passwd *pw = getpwuid(getuid());
            if (pw) target = pw->pw_dir;
        }
        if (!target) target = "/";
    } else {
        const char *arg = cmd->args[1];
        if (strcmp(arg, "~") == 0) {
            target = getenv("HOME");
            if (!target) target = "/";
        } else if (strncmp(arg, "~/", 2) == 0) {
            const char *home = getenv("HOME");
            if (!home) home = "";
            snprintf(path_buf, sizeof(path_buf), "%s/%s", home, arg + 2);
            target = path_buf;
        } else if (strcmp(arg, "-") == 0) {
            target = getenv("OLDPWD");
            if (!target) {
                shell_output_msg(tab, "cd: OLDPWD not set");
                return 1;
            }
            shell_output_msg(tab, "%s", target);
        } else {
            target = arg;
        }
    }

    char old_cwd[MAX_PATH_LEN];
    if (getcwd(old_cwd, sizeof(old_cwd)) != NULL) {
        setenv("OLDPWD", old_cwd, 1);
    }

    if (chdir(target) != 0) {
        shell_output_msg(tab, "cd: %s: %s", target, strerror(errno));
        return 1;
    }

    if (getcwd(tab->cwd, sizeof(tab->cwd)) != NULL) {
        setenv("PWD", tab->cwd, 1);
    } else {
        strncpy(tab->cwd, target, sizeof(tab->cwd) - 1);
        tab->cwd[sizeof(tab->cwd) - 1] = '\0';
    }

    return 0;
}

int builtin_exit_cmd(Terminal *term, Tab *tab, Command *cmd)
{
    int code = 0;
    if (cmd->argc >= 2 && cmd->args[1] != NULL) {
        code = atoi(cmd->args[1]);
    } else {
        code = tab->last_exit_status;
    }

    tab->running = 0;
    tab->last_exit_status = code;

    if (term) {
        if (term->num_tabs <= 1) {
            term->running = 0;
        } else {
            tab_close(term, term->active_tab);
        }
    }
    return code;
}

int builtin_history(Terminal *term, Tab *tab, Command *cmd)
{
    (void)cmd;
    if (!term || !term->history) {
        shell_output_msg(tab, "history: not available");
        return 1;
    }
    history_print(term, tab);
    return 0;
}

int builtin_jobs(Terminal *term, Tab *tab, Command *cmd)
{
    (void)cmd;
    jobs_print(term, tab);
    return 0;
}

int builtin_fg(Terminal *term, Tab *tab, Command *cmd)
{
    int job_id = -1;
    if (cmd->argc >= 2 && cmd->args[1] != NULL) {
        job_id = jobs_parse_spec(cmd->args[1]);
    } else {
        for (int i = tab->num_jobs - 1; i >= 0; i--) {
            if (tab->jobs[i].active &&
                (tab->jobs[i].state == JOB_STOPPED ||
                 tab->jobs[i].state == JOB_BACKGROUND)) {
                job_id = tab->jobs[i].job_id;
                break;
            }
        }
    }

    if (job_id < 0) {
        shell_output_msg(tab, "fg: no current job");
        return 1;
    }

    return jobs_fg(term, tab, job_id);
}

int builtin_bg(Terminal *term, Tab *tab, Command *cmd)
{
    int job_id = -1;
    if (cmd->argc >= 2 && cmd->args[1] != NULL) {
        job_id = jobs_parse_spec(cmd->args[1]);
    } else {
        for (int i = tab->num_jobs - 1; i >= 0; i--) {
            if (tab->jobs[i].active && tab->jobs[i].state == JOB_STOPPED) {
                job_id = tab->jobs[i].job_id;
                break;
            }
        }
    }

    if (job_id < 0) {
        shell_output_msg(tab, "bg: no current job");
        return 1;
    }

    return jobs_bg(term, tab, job_id);
}

int builtin_source(Terminal *term, Tab *tab, Command *cmd)
{
    if (cmd->argc < 2 || cmd->args[1] == NULL) {
        shell_output_msg(tab, "source: filename argument required");
        return 1;
    }

    const char *path = cmd->args[1];
    char resolved[MAX_PATH_LEN * 2];
    if (path[0] != '/' && tab && tab->cwd[0] != '\0') {
        snprintf(resolved, sizeof(resolved), "%s/%s", tab->cwd, path);
        if (access(resolved, R_OK) == 0) {
            path = resolved;
        }
    }
    return script_source(term, tab, path);
}

int builtin_multiwatch(Terminal *term, Tab *tab, Command *cmd)
{
    if (cmd->argc < 2) {
        shell_output_msg(tab, "multiWatch: usage: multiWatch [\"cmd1\", \"cmd2\", ...]");
        return 1;
    }

    char arg_buf[MAX_LINE_LEN];
    arg_buf[0] = '\0';
    for (int i = 1; i < cmd->argc; i++) {
        if (i > 1) strncat(arg_buf, " ", sizeof(arg_buf) - strlen(arg_buf) - 1);
        strncat(arg_buf, cmd->args[i], sizeof(arg_buf) - strlen(arg_buf) - 1);
    }

    int count = 0;
    char **cmds = multiwatch_parse_args(arg_buf, &count);
    if (!cmds || count == 0) {
        shell_output_msg(tab, "multiWatch: invalid command list format");
        multiwatch_free_args(cmds, count);
        return 1;
    }

    int rc = multiwatch_run(term, tab, cmds, count);
    multiwatch_free_args(cmds, count);
    return rc;
}

int builtin_export(Terminal *term, Tab *tab, Command *cmd)
{
    (void)term;
    if (cmd->argc < 2) {
        for (int i = 0; i < tab->num_vars; i++) {
            shell_output_msg(tab, "declare -x %s=\"%s\"",
                             tab->vars[i].name, tab->vars[i].value);
        }
        return 0;
    }

    for (int i = 1; i < cmd->argc; i++) {
        char *eq = strchr(cmd->args[i], '=');
        if (eq) {
            *eq = '\0';
            const char *name  = cmd->args[i];
            const char *value = eq + 1;
            setenv(name, value, 1);

            int found = 0;
            for (int j = 0; j < tab->num_vars; j++) {
                if (strcmp(tab->vars[j].name, name) == 0) {
                    strncpy(tab->vars[j].value, value, MAX_VAR_VALUE - 1);
                    tab->vars[j].value[MAX_VAR_VALUE - 1] = '\0';
                    found = 1;
                    break;
                }
            }
            if (!found && tab->num_vars < MAX_VARS) {
                snprintf(tab->vars[tab->num_vars].name, sizeof(tab->vars[tab->num_vars].name), "%s", name);
                snprintf(tab->vars[tab->num_vars].value, sizeof(tab->vars[tab->num_vars].value), "%s", value);
                tab->num_vars++;
            }
        } else {
            const char *name = cmd->args[i];
            int found = 0;
            for (int j = 0; j < tab->num_vars; j++) {
                if (strcmp(tab->vars[j].name, name) == 0) {
                    setenv(name, tab->vars[j].value, 1);
                    found = 1;
                    break;
                }
            }
            if (!found) {
                const char *val = getenv(name);
                if (val && tab->num_vars < MAX_VARS) {
                    strncpy(tab->vars[tab->num_vars].name, name, MAX_VAR_NAME - 1);
                    tab->vars[tab->num_vars].name[MAX_VAR_NAME - 1] = '\0';
                    strncpy(tab->vars[tab->num_vars].value, val, MAX_VAR_VALUE - 1);
                    tab->vars[tab->num_vars].value[MAX_VAR_VALUE - 1] = '\0';
                    tab->num_vars++;
                }
            }
        }
    }

    return 0;
}
