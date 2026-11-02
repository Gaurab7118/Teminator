/*
 * builtins.h — Built-in commands (Task 2)
 */

#ifndef BUILTINS_H
#define BUILTINS_H

#include "terminator.h"

/* Returns 1 if the command is a built-in, 0 otherwise */
int  builtin_is_builtin(const char *name);

/* Execute a built-in command. Returns exit status. */
int  builtin_execute(Terminal *term, Tab *tab, Command *cmd);

/* Individual built-ins */
int  builtin_cd(Terminal *term, Tab *tab, Command *cmd);
int  builtin_exit_cmd(Terminal *term, Tab *tab, Command *cmd);
int  builtin_history(Terminal *term, Tab *tab, Command *cmd);
int  builtin_jobs(Terminal *term, Tab *tab, Command *cmd);
int  builtin_fg(Terminal *term, Tab *tab, Command *cmd);
int  builtin_bg(Terminal *term, Tab *tab, Command *cmd);
int  builtin_source(Terminal *term, Tab *tab, Command *cmd);
int  builtin_multiwatch(Terminal *term, Tab *tab, Command *cmd);
int  builtin_export(Terminal *term, Tab *tab, Command *cmd);

#endif /* BUILTINS_H */
