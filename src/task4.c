/*
 * redirect.c — I/O redirection for child processes (Task 4)
 * Computing Lab (CS69201) · Term Project
 *
 * Handles setting up file-descriptor redirections (< for stdin,
 * > / >> for stdout) inside a forked child process before exec.
 * Also provides pre-fork validation of input files.
 */

#include "redirect.h"

/* ══════════════════════════════════════════════════════════════════
 *  redirect_setup — Configure stdin/stdout in a child process
 *
 *  Called after fork(), before exec(), inside the child.
 *
 *  - If cmd->input_file is set:
 *      open(input_file, O_RDONLY) and dup2 to STDIN_FILENO.
 *  - If cmd->output_file is set:
 *      open with O_WRONLY|O_CREAT, choosing O_TRUNC or O_APPEND
 *      based on cmd->append_output, and dup2 to STDOUT_FILENO.
 *
 *  Returns  0 on success.
 *  Returns -1 on error (with a message written to stderr).
 * ══════════════════════════════════════════════════════════════════ */
int redirect_setup(Command *cmd)
{
    if (!cmd)
        return -1;

    /* ── Input redirection: cmd < file ────────────────────────── */
    if (cmd->input_file) {
        int fd = open(cmd->input_file, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "terminator: %s: %s\n",
                    cmd->input_file, strerror(errno));
            return -1;
        }
        if (dup2(fd, STDIN_FILENO) < 0) {
            fprintf(stderr, "terminator: dup2 stdin: %s\n",
                    strerror(errno));
            close(fd);
            return -1;
        }
        close(fd);   /* original fd no longer needed */
    }

    /* ── Output redirection: cmd > file  or  cmd >> file ─────── */
    if (cmd->output_file) {
        int flags = O_WRONLY | O_CREAT;
        if (cmd->append_output)
            flags |= O_APPEND;
        else
            flags |= O_TRUNC;

        int fd = open(cmd->output_file, flags, 0644);
        if (fd < 0) {
            fprintf(stderr, "terminator: %s: %s\n",
                    cmd->output_file, strerror(errno));
            return -1;
        }
        if (dup2(fd, STDOUT_FILENO) < 0) {
            fprintf(stderr, "terminator: dup2 stdout: %s\n",
                    strerror(errno));
            close(fd);
            return -1;
        }
        close(fd);   /* original fd no longer needed */
    }

    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 *  redirect_validate — Pre-fork check that input files are readable
 *
 *  Walks the entire pipeline (cmd and all pipe_next commands).
 *  For each command that specifies an input_file, verifies:
 *    - The file exists (access with F_OK)
 *    - The file is readable (access with R_OK)
 *
 *  On failure, writes an error message to the tab's shell_to_gui
 *  pipe so the GUI can display it.
 *
 *  Returns  0 if all redirections are valid.
 *  Returns -1 if any input file is missing or unreadable.
 * ══════════════════════════════════════════════════════════════════ */
int redirect_validate(Terminal *term, Tab *tab, Command *cmd)
{
    (void)term;   /* reserved for future use */

    if (!cmd)
        return 0;   /* nothing to validate */

    Command *c = cmd;
    while (c) {
        if (c->input_file) {
            if (access(c->input_file, F_OK) != 0) {
                /* File does not exist */
                char errbuf[MAX_PATH_LEN + 128];
                snprintf(errbuf, sizeof(errbuf),
                         "terminator: %s: No such file or directory\n",
                         c->input_file);

                /* Write error to the tab's output pipe for the GUI */
                if (tab && tab->shell_to_gui[1] >= 0) {
                    ssize_t w = write(tab->shell_to_gui[1], errbuf, strlen(errbuf));
                    (void)w;
                }

                return -1;
            }
            if (access(c->input_file, R_OK) != 0) {
                /* File exists but is not readable */
                char errbuf[MAX_PATH_LEN + 128];
                snprintf(errbuf, sizeof(errbuf),
                         "terminator: %s: Permission denied\n",
                         c->input_file);

                if (tab && tab->shell_to_gui[1] >= 0) {
                    ssize_t w = write(tab->shell_to_gui[1], errbuf, strlen(errbuf));
                    (void)w;
                }

                return -1;
            }
        }

        /* Also validate that output directories exist (optional,
         * but a nice safeguard).  We check the parent directory. */
        if (c->output_file) {
            /* Extract parent directory */
            char path_copy[MAX_PATH_LEN];
            strncpy(path_copy, c->output_file, sizeof(path_copy) - 1);
            path_copy[sizeof(path_copy) - 1] = '\0';

            /* Find last slash */
            char *slash = strrchr(path_copy, '/');
            if (slash && slash != path_copy) {
                *slash = '\0';   /* terminate at parent dir */
                if (access(path_copy, W_OK) != 0) {
                    char errbuf[MAX_PATH_LEN + 128];
                    snprintf(errbuf, sizeof(errbuf),
                             "terminator: %s: Cannot create file "
                             "(directory not writable)\n",
                             c->output_file);

                    if (tab && tab->shell_to_gui[1] >= 0) {
                        ssize_t w = write(tab->shell_to_gui[1], errbuf, strlen(errbuf));
                        (void)w;
                    }

                    return -1;
                }
            }
        }

        c = c->pipe_next;
    }

    return 0;   /* all validations passed */
}
