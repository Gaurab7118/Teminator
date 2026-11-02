/*
 * parser.h — Command-line parsing, multiline, quoting (Task 3)
 */

#ifndef PARSER_H
#define PARSER_H

#include "terminator.h"

/* Parse a command line into a linked list of Command structures.
 * Handles pipes (|), redirection (<, >), background (&), quoting, etc.
 * Returns the head Command or NULL on error. */
Command *parser_parse(const char *line);

/* Free a linked list of Command structures */
void parser_free_command(Command *cmd);

/* Check if a line is incomplete (needs continuation).
 * Returns 1 if more input is needed, 0 if complete. */
int  parser_is_incomplete(const char *line);

/* Expand variables ($VAR, $?, $#, $1...) in a string.
 * Returns a new malloc'd string. */
char *parser_expand_variables(const char *str, Tab *tab);

/* Perform command substitution $(...) and backticks.
 * Returns a new malloc'd string with substitutions resolved. */
char *parser_expand_substitutions(const char *str, Terminal *term, Tab *tab);

/* Tokenize a string respecting quotes.
 * Returns a malloc'd array of malloc'd strings (NULL-terminated). */
char **parser_tokenize(const char *line, int *count);

/* Free a tokenized array */
void parser_free_tokens(char **tokens, int count);

/* Check if a character is a special shell character */
int  parser_is_special(char c);

/* Handle line continuation: join lines ending with backslash */
char *parser_join_continuation(const char *input);

#endif /* PARSER_H */
