/*
 * shell.h — Shell command execution (Task 2)
 */

#ifndef SHELL_H
#define SHELL_H

#include "terminator.h"

/* Execute a full command line (may contain pipes, redirection, etc.)
 * Returns exit status of the last command. */
int  shell_execute_line(Terminal *term, Tab *tab, const char *line);

/* Execute a single parsed command (fork + execvp) */
int  shell_execute_command(Terminal *term, Tab *tab, Command *cmd);

/* Execute a pipeline of commands */
int  shell_execute_pipeline(Terminal *term, Tab *tab, Command *pipeline);

/* Run an external command in a child process */
int  shell_run_external(Terminal *term, Tab *tab, Command *cmd,
                        int input_fd, int output_fd);

/* Wait for a foreground process */
int  shell_wait_fg(Tab *tab, pid_t pid);

/* Process output from a child and append to tab */
void shell_capture_output(Terminal *term, Tab *tab, int fd);

#endif /* SHELL_H */
