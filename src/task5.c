/*
 * pipes.c — Pipeline execution (Task 5)
 * Computing Lab (CS69201) · Term Project
 *
 * Executes a pipeline of N commands connected by Unix pipes.
 * For N commands, creates N-1 pipe pairs, forks N child processes,
 * connects them via dup2, handles I/O redirection on the first
 * and last stages, and waits (or backgrounds) as appropriate.
 */

#include "pipes.h"
#include "redirect.h"
#include "jobs.h"

/* ══════════════════════════════════════════════════════════════════
 *  pipes_count — Count commands in a pipeline linked list
 * ══════════════════════════════════════════════════════════════════ */
int pipes_count(Command *cmd)
{
    int n = 0;
    while (cmd) {
        n++;
        cmd = cmd->pipe_next;
    }
    return n;
}

/* ══════════════════════════════════════════════════════════════════
 *  close_all_pipes — Close every fd in the pipe-pair array
 *
 *  pipefds is an array of 2*(num_pipes) ints.
 * ══════════════════════════════════════════════════════════════════ */
static void close_all_pipes(int *pipefds, int num_pipes)
{
    for (int i = 0; i < 2 * num_pipes; i++) {
        if (pipefds[i] >= 0) {
            close(pipefds[i]);
            pipefds[i] = -1;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  pipes_execute_pipeline — Fork & exec a pipeline
 *
 *  Parameters
 *  ----------
 *  term : Terminal pointer (for writing output to GUI)
 *  tab  : Current tab (job list, exit status, pipes, cwd)
 *  cmd  : Head of the Command linked list
 *
 *  Pipeline layout (N commands, N-1 pipes):
 *
 *    cmd[0] --pipe0--> cmd[1] --pipe1--> ... --pipeN-2--> cmd[N-1]
 *
 *  - cmd[0]   reads from cmd->input_file (or inherits stdin)
 *  - cmd[N-1] writes to cmd->output_file (or shell_to_gui pipe)
 *  - Each intermediate command reads from the previous pipe and
 *    writes to the next pipe.
 *
 *  Returns the exit status of the last command in the pipeline,
 *  or -1 on fatal setup error.
 * ══════════════════════════════════════════════════════════════════ */
int pipes_execute_pipeline(Terminal *term, Tab *tab, Command *cmd)
{
    (void)term;
    if (!cmd || !cmd->args || !cmd->args[0])
        return -1;

    int n_cmds = pipes_count(cmd);

    /* Guard against absurd pipeline lengths */
    if (n_cmds > MAX_PIPES) {
        const char *msg = "terminator: pipeline too long\n";
        if (tab && tab->shell_to_gui[1] >= 0) {
            ssize_t w = write(tab->shell_to_gui[1], msg, strlen(msg));
            (void)w;
        }
        return -1;
    }

    int n_pipes = n_cmds - 1;

    /* ── Create all pipes ─────────────────────────────────────── */
    int pipefds[MAX_PIPES * 2];   /* pairs: [read0, write0, read1, write1, ...] */
    for (int i = 0; i < n_pipes * 2; i++)
        pipefds[i] = -1;

    for (int i = 0; i < n_pipes; i++) {
        if (pipe(pipefds + i * 2) < 0) {
            perror("terminator: pipe");
            close_all_pipes(pipefds, i);
            return -1;
        }
    }

    /* ── Collect child pids ───────────────────────────────────── */
    pid_t pids[MAX_PIPES];
    memset(pids, 0, sizeof(pids));

    /* Build the pipeline command string for job display */
    char pipeline_str[MAX_LINE_LEN];
    pipeline_str[0] = '\0';
    {
        Command *c = cmd;
        int first = 1;
        for (int idx = 0; c && idx < n_cmds; idx++, c = c->pipe_next) {
            if (!first)
                strncat(pipeline_str, " | ",
                        sizeof(pipeline_str) - strlen(pipeline_str) - 1);
            first = 0;
            for (int a = 0; a < c->argc; a++) {
                if (a > 0)
                    strncat(pipeline_str, " ",
                            sizeof(pipeline_str) - strlen(pipeline_str) - 1);
                strncat(pipeline_str, c->args[a],
                        sizeof(pipeline_str) - strlen(pipeline_str) - 1);
            }
        }
    }

    /* ── Fork each child ──────────────────────────────────────── */
    Command *cur = cmd;
    pid_t pgid = 0;   /* process group id (set from first child) */

    for (int i = 0; i < n_cmds; i++) {
        pid_t pid = fork();

        if (pid < 0) {
            perror("terminator: fork");
            close_all_pipes(pipefds, n_pipes);
            /* Kill already-forked children */
            for (int k = 0; k < i; k++) {
                kill(pids[k], SIGKILL);
                waitpid(pids[k], NULL, 0);
            }
            return -1;
        }

        if (pid == 0) {
            /* ═══ Child process ════════════════════════════════ */

            /* Set the process group for job control.
             * First child creates a new group; others join it. */
            if (pgid == 0)
                setpgid(0, 0);      /* new group = own pid */
            else
                setpgid(0, pgid);   /* join first child's group */

            /* Reset signal dispositions to default for the child */
            signal(SIGINT,  SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);

            /* ── Wire up pipe fds ─────────────────────────────── */

            /* stdin: read from the previous pipe (except first cmd) */
            if (i > 0) {
                dup2(pipefds[(i - 1) * 2], STDIN_FILENO);
            }

            /* stdout: write to the next pipe (except last cmd) */
            if (i < n_cmds - 1) {
                dup2(pipefds[i * 2 + 1], STDOUT_FILENO);
            } else {
                /* Last command: if no output_file redirection,
                 * send output to the shell_to_gui pipe for the GUI. */
                if (!cur->output_file && tab && tab->shell_to_gui[1] >= 0) {
                    dup2(tab->shell_to_gui[1], STDOUT_FILENO);
                    /* Also redirect stderr to the GUI pipe */
                    dup2(tab->shell_to_gui[1], STDERR_FILENO);
                }
            }

            /* Close ALL pipe fds in the child — they've been dup2'd */
            close_all_pipes(pipefds, n_pipes);

            /* Close the shell_to_gui write end if we dup'd it */
            if (tab) {
                if (tab->shell_to_gui[1] >= 0 &&
                    tab->shell_to_gui[1] != STDOUT_FILENO &&
                    tab->shell_to_gui[1] != STDERR_FILENO)
                    close(tab->shell_to_gui[1]);
                /* Close the read end of shell_to_gui in the child */
                if (tab->shell_to_gui[0] >= 0)
                    close(tab->shell_to_gui[0]);
                /* Close both ends of gui_to_shell in the child */
                if (tab->gui_to_shell[0] >= 0)
                    close(tab->gui_to_shell[0]);
                if (tab->gui_to_shell[1] >= 0)
                    close(tab->gui_to_shell[1]);
            }

            /* ── Apply file redirections (< and >) ────────────── */
            if (redirect_setup(cur) < 0)
                _exit(1);

            /* ── Exec ─────────────────────────────────────────── */
            execvp(cur->args[0], cur->args);

            /* If exec fails, report and exit */
            fprintf(stderr, "terminator: %s: %s\n",
                    cur->args[0], strerror(errno));
            _exit(127);
        }

        /* ═══ Parent process continues ═════════════════════════ */
        pids[i] = pid;

        /* First child defines the process group */
        if (i == 0) {
            pgid = pid;
            setpgid(pid, pid);    /* ensure pgid is set even if child is slow */
        } else {
            setpgid(pid, pgid);
        }

        cur = cur->pipe_next;
    }

    /* ── Parent: close all pipe fds ───────────────────────────── */
    close_all_pipes(pipefds, n_pipes);

    /* ── Background vs foreground handling ────────────────────── */
    if (cmd->background) {
        /* Background pipeline: add a job and don't wait */
        if (tab) {
            jobs_add(tab, pids[n_cmds - 1], pgid,
                     pipeline_str, JOB_BACKGROUND);

            /* Inform the user */
            char msg[MAX_LINE_LEN + 64];
            snprintf(msg, sizeof(msg), "[%d] %d\n",
                     tab->num_jobs, (int)pids[n_cmds - 1]);
            if (tab->shell_to_gui[1] >= 0) {
                ssize_t w = write(tab->shell_to_gui[1], msg, strlen(msg));
                (void)w;
            }
        }
        return 0;
    }

    /* ── Foreground: wait for all children ────────────────────── */
    if (tab) {
        tab->fg_pid  = pids[n_cmds - 1];
        tab->fg_pgid = pgid;
    }

    int last_status = 0;

    for (int i = 0; i < n_cmds; i++) {
        int wstatus = 0;
        pid_t w;

        do {
            w = waitpid(pids[i], &wstatus, 0);
        } while (w < 0 && errno == EINTR);

        if (w < 0) {
            /* waitpid error — child may have already been reaped */
            continue;
        }

        /* We care about the last command's status */
        if (i == n_cmds - 1) {
            if (WIFEXITED(wstatus))
                last_status = WEXITSTATUS(wstatus);
            else if (WIFSIGNALED(wstatus))
                last_status = 128 + WTERMSIG(wstatus);
            else
                last_status = 1;
        }

        /* If a child was stopped (Ctrl+Z), add it as a job */
        if (WIFSTOPPED(wstatus) && tab) {
            jobs_add(tab, pids[i], pgid, pipeline_str, JOB_STOPPED);
        }
    }

    /* Clear foreground tracking */
    if (tab) {
        tab->fg_pid  = 0;
        tab->fg_pgid = 0;
        tab->last_exit_status = last_status;
    }

    return last_status;
}
