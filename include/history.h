/*
 * history.h — Command history management (Task 9)
 */

#ifndef HISTORY_H
#define HISTORY_H

#include "terminator.h"

/* Initialise shared history (allocate, init mutex/semaphore).
 * process_mode: 0 = thread mode, 1 = process mode (shared memory). */
SharedHistory *history_init(int process_mode);

/* Cleanup shared history */
void history_cleanup(SharedHistory *hist, int process_mode);

/* Add a command to history */
void history_add(SharedHistory *hist, const char *command);

/* Get the i-th most recent command (0 = most recent).
 * Returns pointer to the entry or NULL. */
const char *history_get(SharedHistory *hist, int index);

/* Get total number of entries */
int  history_count(SharedHistory *hist);

/* Load history from file */
int  history_load(SharedHistory *hist, const char *path);

/* Save history to file */
int  history_save(SharedHistory *hist, const char *path);

/* Search history:
 *  (a) exact match → returns most recent match
 *  (b) longest common substring > 2 chars → returns matches
 *  (c) no match
 * results: array to fill with matching command strings
 * max_results: max entries to return
 * Returns the number of matches found. */
int  history_search(SharedHistory *hist, const char *term,
                    const char **results, int max_results);

/* Print the most recent HISTORY_DISPLAY commands */
void history_print(Terminal *term, Tab *tab);

/* Lock history for thread-safe access */
void history_lock(SharedHistory *hist);

/* Unlock history */
void history_unlock(SharedHistory *hist);

#endif /* HISTORY_H */
