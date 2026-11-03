/*
 * pipes.h — Pipeline execution (Task 5)
 */

#ifndef PIPES_H
#define PIPES_H

#include "terminator.h"

/* Execute a pipeline of commands.
 * cmd is the head of the linked list (pipe_next chain).
 * Returns the exit status of the last command. */
int  pipes_execute_pipeline(Terminal *term, Tab *tab, Command *cmd);

/* Count the number of commands in a pipeline */
int  pipes_count(Command *cmd);

#endif /* PIPES_H */
