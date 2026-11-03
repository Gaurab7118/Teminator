/*
 * autocomplete.h — File-name auto-completion (Task 10)
 */

#ifndef AUTOCOMPLETE_H
#define AUTOCOMPLETE_H

#include "terminator.h"

/* Perform tab completion on the current input.
 * Modifies tab->input, tab->input_len, tab->cursor_pos as needed.
 * May set tab->autocomplete_mode and fill tab->completions.
 * Returns: 0 = completed, 1 = ambiguous (list shown), -1 = no match */
int  autocomplete_complete(Terminal *term, Tab *tab);

/* Handle a selection from the numbered completion list.
 * choice is 1-based index. */
void autocomplete_select(Terminal *term, Tab *tab, int choice);

/* Get completions for a prefix in a directory.
 * results: array to fill, max_results: max entries.
 * Returns the number of matches. */
int  autocomplete_find_matches(const char *dir, const char *prefix,
                               char **results, int max_results);

/* Find the longest common prefix among an array of strings.
 * Returns a malloc'd string. */
char *autocomplete_longest_prefix(char **strings, int count);

/* Free completion results */
void autocomplete_free_matches(char **results, int count);

#endif /* AUTOCOMPLETE_H */
