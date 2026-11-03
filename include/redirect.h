/*
 * redirect.h — I/O redirection (Task 4)
 */

#ifndef REDIRECT_H
#define REDIRECT_H

#include "terminator.h"

/* Set up redirections in the child process (after fork, before exec).
 * Returns 0 on success, -1 on error. */
int  redirect_setup(Command *cmd);

/* Validate that input files exist before executing.
 * Returns 0 on success, -1 if a file is missing (error message written to tab). */
int  redirect_validate(Terminal *term, Tab *tab, Command *cmd);

#endif /* REDIRECT_H */
