/*
 * tab.h — Tab management (Task 1 support)
 */

#ifndef TAB_H
#define TAB_H

#include "terminator.h"

/* Create a new tab; returns tab index or -1 on failure */
int  tab_create(Terminal *term);

/* Get the lowest unused positive integer ID for a new tab */
int  tab_get_next_id(Terminal *term);

/* Close a tab by index */
void tab_close(Terminal *term, int tab_index);

/* Switch to a tab by index */
void tab_switch(Terminal *term, int tab_index);

/* Initialise a tab's state */
void tab_init(Tab *tab, int id);

/* Free a tab's resources */
void tab_cleanup(Tab *tab);

/* Get the active tab pointer */
Tab *tab_active(Terminal *term);

/* Start the shell for a tab (thread or process mode) */
int  tab_start_shell(Terminal *term, int tab_index);

/* Shell loop — runs in each tab's thread / process */
void *tab_shell_loop(void *arg);

/* Send a line of text from the GUI to the tab's shell */
void tab_send_input(Terminal *term, int tab_index, const char *line);

/* Read available output from a tab's shell (non-blocking) */
int  tab_read_output(Terminal *term, int tab_index, char *buf, int bufsize);

#endif /* TAB_H */
