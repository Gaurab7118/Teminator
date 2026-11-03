/*
 * multiwatch.h — multiWatch command (Task 6)
 */

#ifndef MULTIWATCH_H
#define MULTIWATCH_H

#include "terminator.h"

/* Execute the multiWatch built-in.
 * cmds is an array of command strings, count is the number of commands.
 * Runs all commands in parallel, polls with select/poll, re-runs every 2s.
 * Ctrl+C (signalled via the tab's interrupt flag) stops everything. */
int  multiwatch_run(Terminal *term, Tab *tab, char **cmds, int count);

/* Parse the multiWatch argument: multiWatch ["cmd1", "cmd2", ...]
 * Returns a malloc'd array of malloc'd strings, sets *count. */
char **multiwatch_parse_args(const char *arg_str, int *count);

/* Free the parsed argument array */
void multiwatch_free_args(char **cmds, int count);

#endif /* MULTIWATCH_H */
