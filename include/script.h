/*
 * script.h — Bash script interpreter (Task 11)
 */

#ifndef SCRIPT_H
#define SCRIPT_H

#include "terminator.h"

/* Execute a bash script file. The script is interpreted by Terminator itself,
 * NOT handed to /bin/bash.
 * path: path to the script file
 * args: arguments ($1, $2, ...)
 * argc: number of arguments
 * Returns exit status. */
int  script_execute(Terminal *term, Tab *tab, const char *path,
                    char **args, int argc);

/* Execute the 'source' built-in: run a script in the current shell context */
int  script_source(Terminal *term, Tab *tab, const char *path);

/* Parse and validate a script before running it.
 * Returns 0 on success, -1 on syntax error (error message in errbuf). */
int  script_validate(const char *path, char *errbuf, int errbuf_size);

/* ── Script AST node types ─────────────────────────────────────── */

typedef enum {
    NODE_COMMAND,      /* a simple command line */
    NODE_IF,           /* if/elif/else/fi */
    NODE_EXIT,         /* exit [n] */
    NODE_ASSIGNMENT,   /* VAR=value */
    NODE_COMMENT,      /* # comment */
    NODE_EMPTY         /* blank line */
} ScriptNodeType;

typedef struct ScriptNode {
    ScriptNodeType type;

    /* NODE_COMMAND / NODE_EXIT */
    char *command_line;

    /* NODE_ASSIGNMENT */
    char *var_name;
    char *var_value;

    /* NODE_IF */
    char *condition;                   /* the [ ... ] test */
    struct ScriptNode **then_body;     /* commands in then block */
    int then_count;
    char **elif_conditions;            /* elif conditions */
    struct ScriptNode ***elif_bodies;    /* elif bodies */
    int *elif_body_counts;
    int elif_count;
    struct ScriptNode **else_body;     /* commands in else block */
    int else_count;

    /* Linked list for sequential execution */
    struct ScriptNode *next;
} ScriptNode;

/* Parse a script file into a list of ScriptNode.
 * Returns the head node or NULL on error. */
ScriptNode *script_parse_file(const char *path, char *errbuf, int errbuf_size);

/* Execute a list of ScriptNode */
int  script_execute_nodes(Terminal *term, Tab *tab, ScriptNode *nodes);

/* Free a list of ScriptNode */
void script_free_nodes(ScriptNode *nodes);

/* Evaluate a [ ... ] test condition. Returns 0 (true) or 1 (false). */
int  script_eval_test(Terminal *term, Tab *tab, const char *condition);

#endif /* SCRIPT_H */
