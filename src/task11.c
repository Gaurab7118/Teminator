/*
 * script.c — Bash script interpreter (Task 11)
 * Computing Lab (CS69201) · Term Project
 *
 * Interprets bash script files directly inside Terminator without handing
 * them to /bin/bash. Supports comments, commands, variables (NAME=value, $NAME),
 * script arguments ($1..$9, $#, $?), command substitution, redirection,
 * pipes, if/elif/else/fi conditionals with [ ... ] tests, and exit [n].
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "script.h"
#include "shell.h"
#include "parser.h"
#include "gui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>

/* ── Forward declarations ────────────────────────────────────────── */

static int  execute_single_node(Terminal *term, Tab *tab, ScriptNode *node);
static void trim_whitespace(char *str);
static char *strip_quotes(const char *str);
static void set_shell_variable(Terminal *term, Tab *tab, const char *name, const char *val);
static ScriptNode *parse_node_from_line(const char *raw_line);

/* ══════════════════════════════════════════════════════════════════
 *  trim_whitespace — In-place whitespace trimmer
 * ══════════════════════════════════════════════════════════════════ */
static void trim_whitespace(char *str)
{
    if (!str || !*str) return;

    /* Leading */
    char *p = str;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != str) {
        memmove(str, p, strlen(p) + 1);
    }

    /* Trailing */
    size_t len = strlen(str);
    while (len > 0 && isspace((unsigned char)str[len - 1])) {
        str[--len] = '\0';
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  strip_quotes — Remove outer matching single or double quotes
 * ══════════════════════════════════════════════════════════════════ */
static char *strip_quotes(const char *str)
{
    if (!str) return strdup("");
    size_t len = strlen(str);
    if (len >= 2 && ((str[0] == '"' && str[len - 1] == '"') ||
                     (str[0] == '\'' && str[len - 1] == '\''))) {
        char *res = malloc(len - 1);
        if (res) {
            memcpy(res, str + 1, len - 2);
            res[len - 2] = '\0';
            return res;
        }
    }
    return strdup(str);
}

/* ══════════════════════════════════════════════════════════════════
 *  set_shell_variable — Set a variable in tab->vars and environment
 * ══════════════════════════════════════════════════════════════════ */
static void set_shell_variable(Terminal *term, Tab *tab, const char *name, const char *raw_val)
{
    (void)term;
    if (!tab || !name || !*name) return;

    /* Expand substitutions like $(...) or `...` */
    char *subst = parser_expand_substitutions(raw_val, term, tab);
    const char *to_expand = subst ? subst : raw_val;

    /* Expand variables */
    char *expanded = parser_expand_variables(to_expand, tab);
    if (subst) free(subst);

    const char *final_val = expanded ? expanded : raw_val;
    char *unquoted = strip_quotes(final_val);
    if (expanded) free(expanded);

    /* Update or add in tab->vars */
    int found = -1;
    for (int i = 0; i < tab->num_vars; i++) {
        if (strcmp(tab->vars[i].name, name) == 0) {
            found = i;
            break;
        }
    }

    if (found >= 0) {
        strncpy(tab->vars[found].value, unquoted, sizeof(tab->vars[found].value) - 1);
        tab->vars[found].value[sizeof(tab->vars[found].value) - 1] = '\0';
    } else if (tab->num_vars < MAX_VARS) {
        strncpy(tab->vars[tab->num_vars].name, name, sizeof(tab->vars[tab->num_vars].name) - 1);
        tab->vars[tab->num_vars].name[sizeof(tab->vars[tab->num_vars].name) - 1] = '\0';
        strncpy(tab->vars[tab->num_vars].value, unquoted, sizeof(tab->vars[tab->num_vars].value) - 1);
        tab->vars[tab->num_vars].value[sizeof(tab->vars[tab->num_vars].value) - 1] = '\0';
        tab->num_vars++;
    }

    /* Also export to environment */
    setenv(name, unquoted, 1);
    free(unquoted);
}

/* ══════════════════════════════════════════════════════════════════
 *  script_eval_test — Evaluate [ ... ] test condition
 *  Returns 0 on TRUE, 1 on FALSE (standard test convention).
 * ══════════════════════════════════════════════════════════════════ */
int script_eval_test(Terminal *term, Tab *tab, const char *condition)
{
    (void)term;
    if (!condition) return 1;

    char cond_copy[MAX_LINE_LEN];
    strncpy(cond_copy, condition, sizeof(cond_copy) - 1);
    cond_copy[sizeof(cond_copy) - 1] = '\0';
    trim_whitespace(cond_copy);

    /* Strip outer '[' and ']' if present */
    char *p = cond_copy;
    if (*p == '[') p++;
    size_t len = strlen(p);
    if (len > 0 && p[len - 1] == ']') p[len - 1] = '\0';
    trim_whitespace(p);

    /* Expand variables in the test condition */
    char *expanded = parser_expand_variables(p, tab);
    if (!expanded) return 1;

    /* Tokenize by whitespace */
    int count = 0;
    char **tokens = parser_tokenize(expanded, &count);
    free(expanded);

    if (!tokens || count == 0) {
        parser_free_tokens(tokens, count);
        return 1;
    }

    int result = 1; /* Default FALSE */

    if (count == 1) {
        /* [ string ]: non-empty string is true */
        result = (tokens[0] && tokens[0][0] != '\0') ? 0 : 1;
    } else if (count == 2) {
        const char *op  = tokens[0];
        const char *arg = tokens[1];

        /* Resolve path relative to tab->cwd if necessary */
        char path_buf[MAX_PATH_LEN];
        const char *test_path = arg;
        if (arg[0] != '/' && tab && tab->cwd[0] != '\0') {
            snprintf(path_buf, sizeof(path_buf), "%.2048s/%.2040s", tab->cwd, arg);
            test_path = path_buf;
        }

        struct stat st;
        if (strcmp(op, "-f") == 0) {
            /* File exists and is regular file */
            result = (stat(test_path, &st) == 0 && S_ISREG(st.st_mode)) ? 0 : 1;
        } else if (strcmp(op, "-d") == 0) {
            /* Directory exists */
            result = (stat(test_path, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : 1;
        } else if (strcmp(op, "-e") == 0) {
            /* Path exists */
            result = (access(test_path, F_OK) == 0) ? 0 : 1;
        } else if (strcmp(op, "-z") == 0) {
            /* String length is zero */
            result = (strlen(arg) == 0) ? 0 : 1;
        } else if (strcmp(op, "-n") == 0) {
            /* String length is non-zero */
            result = (strlen(arg) > 0) ? 0 : 1;
        } else if (strcmp(op, "!") == 0) {
            /* Invert non-empty test */
            result = (arg && *arg) ? 1 : 0;
        }
    } else if (count == 3) {
        const char *left = tokens[0];
        const char *op   = tokens[1];
        const char *right = tokens[2];

        if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) {
            result = (strcmp(left, right) == 0) ? 0 : 1;
        } else if (strcmp(op, "!=") == 0) {
            result = (strcmp(left, right) != 0) ? 0 : 1;
        } else if (strcmp(op, "-eq") == 0) {
            result = (atoi(left) == atoi(right)) ? 0 : 1;
        } else if (strcmp(op, "-ne") == 0) {
            result = (atoi(left) != atoi(right)) ? 0 : 1;
        } else if (strcmp(op, "-gt") == 0) {
            result = (atoi(left) > atoi(right)) ? 0 : 1;
        } else if (strcmp(op, "-ge") == 0) {
            result = (atoi(left) >= atoi(right)) ? 0 : 1;
        } else if (strcmp(op, "-lt") == 0) {
            result = (atoi(left) < atoi(right)) ? 0 : 1;
        } else if (strcmp(op, "-le") == 0) {
            result = (atoi(left) <= atoi(right)) ? 0 : 1;
        }
    } else if (count >= 4 && strcmp(tokens[0], "!") == 0) {
        /* Inverted test: ! -f file, etc. */
        char subcond[MAX_LINE_LEN] = "";
        for (int i = 1; i < count; i++) {
            if (i > 1) strncat(subcond, " ", sizeof(subcond) - strlen(subcond) - 1);
            strncat(subcond, tokens[i], sizeof(subcond) - strlen(subcond) - 1);
        }
        result = (script_eval_test(term, tab, subcond) == 0) ? 1 : 0;
    }

    parser_free_tokens(tokens, count);
    return result;
}

/* ══════════════════════════════════════════════════════════════════
 *  script_validate — Validate syntax of a script file before running
 * ══════════════════════════════════════════════════════════════════ */
int script_validate(const char *path, char *errbuf, int errbuf_size)
{
    if (!path) return -1;

    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (errbuf) snprintf(errbuf, errbuf_size, "cannot open script '%s': %s", path, strerror(errno));
        return -1;
    }

    char line[MAX_LINE_LEN];
    int line_num = 0;
    int if_depth = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        trim_whitespace(line);

        /* Skip empty lines and full-line comments */
        if (!line[0] || line[0] == '#') continue;

        if (strncmp(line, "if ", 3) == 0 || strcmp(line, "if") == 0) {
            if_depth++;
        } else if (strncmp(line, "elif ", 5) == 0 || strcmp(line, "elif") == 0) {
            if (if_depth <= 0) {
                if (errbuf) snprintf(errbuf, errbuf_size, "line %d: syntax error: unexpected 'elif'", line_num);
                fclose(fp);
                return -1;
            }
        } else if (strcmp(line, "else") == 0) {
            if (if_depth <= 0) {
                if (errbuf) snprintf(errbuf, errbuf_size, "line %d: syntax error: unexpected 'else'", line_num);
                fclose(fp);
                return -1;
            }
        } else if (strcmp(line, "fi") == 0) {
            if_depth--;
            if (if_depth < 0) {
                if (errbuf) snprintf(errbuf, errbuf_size, "line %d: syntax error: unexpected 'fi' without 'if'", line_num);
                fclose(fp);
                return -1;
            }
        }
    }

    fclose(fp);

    if (if_depth > 0) {
        if (errbuf) snprintf(errbuf, errbuf_size, "syntax error: unexpected end of file, expecting 'fi'");
        return -1;
    }

    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 *  parse_node_from_line — Identify command, assignment, exit, comment
 * ══════════════════════════════════════════════════════════════════ */
static ScriptNode *parse_node_from_line(const char *raw_line)
{
    char line[MAX_LINE_LEN];
    strncpy(line, raw_line, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    trim_whitespace(line);

    if (!line[0]) {
        ScriptNode *n = calloc(1, sizeof(ScriptNode));
        if (n) n->type = NODE_EMPTY;
        return n;
    }

    if (line[0] == '#') {
        ScriptNode *n = calloc(1, sizeof(ScriptNode));
        if (n) n->type = NODE_COMMENT;
        return n;
    }

    /* Check exit */
    if (strncmp(line, "exit", 4) == 0 && (line[4] == '\0' || isspace((unsigned char)line[4]))) {
        ScriptNode *n = calloc(1, sizeof(ScriptNode));
        if (n) {
            n->type = NODE_EXIT;
            n->command_line = strdup(line);
        }
        return n;
    }

    /* Check variable assignment: IDENT=value */
    char *eq = strchr(line, '=');
    if (eq && eq > line && !isspace((unsigned char)*(eq - 1))) {
        /* Check if left of '=' is a valid variable identifier (alphanumeric/underscore) */
        int valid = 1;
        for (char *c = line; c < eq; c++) {
            if (!isalnum((unsigned char)*c) && *c != '_') {
                valid = 0;
                break;
            }
        }

        if (valid) {
            ScriptNode *n = calloc(1, sizeof(ScriptNode));
            if (n) {
                n->type = NODE_ASSIGNMENT;
                *eq = '\0';
                n->var_name  = strdup(line);
                n->var_value = strdup(eq + 1);
            }
            return n;
        }
    }

    /* Regular command line */
    ScriptNode *n = calloc(1, sizeof(ScriptNode));
    if (n) {
        n->type = NODE_COMMAND;
        n->command_line = strdup(line);
    }
    return n;
}

/* ══════════════════════════════════════════════════════════════════
 *  Recursive parser for blocks and nested conditionals
 * ══════════════════════════════════════════════════════════════════ */
static ScriptNode *parse_block_lines(char **lines, int total, int *curr, const char *stop_mode);

static ScriptNode *parse_block_lines(char **lines, int total, int *curr, const char *stop_mode)
{
    ScriptNode *head = NULL;
    ScriptNode *tail = NULL;

    while (*curr < total) {
        char *line = lines[*curr];
        (*curr)++;

        trim_whitespace(line);
        if (!line[0] || line[0] == '#') continue;

        /* Stop keywords depending on stop_mode */
        if (stop_mode) {
            if (strcmp(stop_mode, "THEN") == 0) {
                if (strncmp(line, "elif ", 5) == 0 || strcmp(line, "elif") == 0 ||
                    strcmp(line, "else") == 0 || strcmp(line, "fi") == 0) {
                    (*curr)--; /* Rewind so caller sees it */
                    break;
                }
            } else if (strcmp(stop_mode, "ELIF") == 0) {
                if (strncmp(line, "elif ", 5) == 0 || strcmp(line, "elif") == 0 ||
                    strcmp(line, "else") == 0 || strcmp(line, "fi") == 0) {
                    (*curr)--;
                    break;
                }
            } else if (strcmp(stop_mode, "ELSE") == 0) {
                if (strcmp(line, "fi") == 0) {
                    (*curr)--;
                    break;
                }
            }
        }

        /* Check for IF statement */
        if (strncmp(line, "if ", 3) == 0 || strcmp(line, "if") == 0) {
            ScriptNode *if_node = calloc(1, sizeof(ScriptNode));
            if (!if_node) continue;
            if_node->type = NODE_IF;

            /* Extract condition */
            char *cond_start = line + 2;
            while (*cond_start && isspace((unsigned char)*cond_start)) cond_start++;

            char cond_buf[MAX_LINE_LEN];
            strncpy(cond_buf, cond_start, sizeof(cond_buf) - 1);
            cond_buf[sizeof(cond_buf) - 1] = '\0';

            /* Strip trailing "; then" or "then" */
            char *then_pt = strstr(cond_buf, "; then");
            if (!then_pt) then_pt = strstr(cond_buf, ";then");
            if (then_pt) {
                *then_pt = '\0';
            } else {
                /* Check if "then" is on the next line */
                if (*curr < total) {
                    char next_line[MAX_LINE_LEN];
                    strncpy(next_line, lines[*curr], sizeof(next_line) - 1);
                    next_line[sizeof(next_line) - 1] = '\0';
                    trim_whitespace(next_line);
                    if (strcmp(next_line, "then") == 0) {
                        (*curr)++;
                    }
                }
            }
            trim_whitespace(cond_buf);
            if_node->condition = strdup(cond_buf);

            /* Parse then_body */
            ScriptNode *then_list = parse_block_lines(lines, total, curr, "THEN");
            /* Convert linked list to then_body array */
            int tcount = 0;
            for (ScriptNode *k = then_list; k; k = k->next) tcount++;
            if (tcount > 0) {
                if_node->then_body = malloc(sizeof(ScriptNode *) * tcount);
                if_node->then_count = tcount;
                int idx = 0;
                for (ScriptNode *k = then_list; k; k = k->next) {
                    if_node->then_body[idx++] = k;
                }
            }

            /* Handle optional elif branches */
            while (*curr < total) {
                char next_l[MAX_LINE_LEN];
                strncpy(next_l, lines[*curr], sizeof(next_l) - 1);
                next_l[sizeof(next_l) - 1] = '\0';
                trim_whitespace(next_l);

                if (strncmp(next_l, "elif ", 5) != 0) break;
                (*curr)++;

                char *elif_cond_start = next_l + 4;
                while (*elif_cond_start && isspace((unsigned char)*elif_cond_start)) elif_cond_start++;
                char elif_cond_buf[MAX_LINE_LEN];
                strncpy(elif_cond_buf, elif_cond_start, sizeof(elif_cond_buf) - 1);
                elif_cond_buf[sizeof(elif_cond_buf) - 1] = '\0';

                char *e_then = strstr(elif_cond_buf, "; then");
                if (!e_then) e_then = strstr(elif_cond_buf, ";then");
                if (e_then) {
                    *e_then = '\0';
                } else if (*curr < total) {
                    char peek[MAX_LINE_LEN];
                    strncpy(peek, lines[*curr], sizeof(peek) - 1);
                    peek[sizeof(peek) - 1] = '\0';
                    trim_whitespace(peek);
                    if (strcmp(peek, "then") == 0) (*curr)++;
                }
                trim_whitespace(elif_cond_buf);

                int ec = if_node->elif_count;
                if_node->elif_conditions = realloc(if_node->elif_conditions, sizeof(char *) * (ec + 1));
                if_node->elif_bodies     = realloc(if_node->elif_bodies, sizeof(ScriptNode **) * (ec + 1));
                if_node->elif_body_counts= realloc(if_node->elif_body_counts, sizeof(int) * (ec + 1));

                if_node->elif_conditions[ec] = strdup(elif_cond_buf);

                ScriptNode *elif_list = parse_block_lines(lines, total, curr, "ELIF");
                int el_count = 0;
                for (ScriptNode *k = elif_list; k; k = k->next) el_count++;
                if_node->elif_body_counts[ec] = el_count;
                if_node->elif_bodies[ec] = malloc(sizeof(ScriptNode *) * (el_count > 0 ? el_count : 1));
                int eidx = 0;
                for (ScriptNode *k = elif_list; k; k = k->next) {
                    if_node->elif_bodies[ec][eidx++] = k;
                }
                if_node->elif_count++;
            }

            /* Handle optional else branch */
            if (*curr < total) {
                char next_l[MAX_LINE_LEN];
                strncpy(next_l, lines[*curr], sizeof(next_l) - 1);
                next_l[sizeof(next_l) - 1] = '\0';
                trim_whitespace(next_l);

                if (strcmp(next_l, "else") == 0) {
                    (*curr)++;
                    ScriptNode *else_list = parse_block_lines(lines, total, curr, "ELSE");
                    int ecount = 0;
                    for (ScriptNode *k = else_list; k; k = k->next) ecount++;
                    if (ecount > 0) {
                        if_node->else_body = malloc(sizeof(ScriptNode *) * ecount);
                        if_node->else_count = ecount;
                        int idx = 0;
                        for (ScriptNode *k = else_list; k; k = k->next) {
                            if_node->else_body[idx++] = k;
                        }
                    }
                }
            }

            /* Consume fi */
            if (*curr < total) {
                char next_l[MAX_LINE_LEN];
                strncpy(next_l, lines[*curr], sizeof(next_l) - 1);
                next_l[sizeof(next_l) - 1] = '\0';
                trim_whitespace(next_l);
                if (strcmp(next_l, "fi") == 0) {
                    (*curr)++;
                }
            }

            if (!head) head = if_node;
            else tail->next = if_node;
            tail = if_node;
            continue;
        }

        /* Normal statement */
        ScriptNode *node = parse_node_from_line(line);
        if (node) {
            if (!head) head = node;
            else tail->next = node;
            tail = node;
        }
    }

    return head;
}

/* ══════════════════════════════════════════════════════════════════
 *  script_parse_file — Parse full script into AST
 * ══════════════════════════════════════════════════════════════════ */
ScriptNode *script_parse_file(const char *path, char *errbuf, int errbuf_size)
{
    if (script_validate(path, errbuf, errbuf_size) != 0) {
        return NULL;
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (errbuf) snprintf(errbuf, errbuf_size, "cannot open '%s': %s", path, strerror(errno));
        return NULL;
    }

    /* Read all lines */
    char **lines = NULL;
    int count = 0;
    int cap = 64;
    lines = malloc(sizeof(char *) * cap);

    char buf[MAX_LINE_LEN];
    while (fgets(buf, sizeof(buf), fp)) {
        if (count >= cap) {
            cap *= 2;
            lines = realloc(lines, sizeof(char *) * cap);
        }
        lines[count++] = strdup(buf);
    }
    fclose(fp);

    /* Skip shebang line if first line */
    int start = 0;
    if (count > 0 && strncmp(lines[0], "#!", 2) == 0) {
        start = 1;
    }

    int curr = start;
    ScriptNode *ast = parse_block_lines(lines, count, &curr, NULL);

    for (int i = 0; i < count; i++) {
        free(lines[i]);
    }
    free(lines);

    return ast;
}

/* ══════════════════════════════════════════════════════════════════
 *  execute_single_node — Execute a single AST node
 * ══════════════════════════════════════════════════════════════════ */
static int execute_single_node(Terminal *term, Tab *tab, ScriptNode *node)
{
    if (!node) return 0;

    switch (node->type) {
    case NODE_COMMAND: {
        /* Expand variables like $1, $VAR, $?, $# */
        char *expanded = parser_expand_variables(node->command_line, tab);
        const char *cmd_to_run = expanded ? expanded : node->command_line;

        int status = shell_execute_line(term, tab, cmd_to_run);
        if (expanded) free(expanded);
        return status;
    }

    case NODE_ASSIGNMENT:
        set_shell_variable(term, tab, node->var_name, node->var_value);
        return 0;

    case NODE_EXIT: {
        int code = tab ? tab->last_exit_status : 0;
        if (node->command_line) {
            char *p = node->command_line + 4;
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p) {
                code = atoi(p);
            }
        }
        if (tab) tab->last_exit_status = code;
        return code;
    }

    case NODE_IF: {
        int cond = script_eval_test(term, tab, node->condition);
        if (cond == 0) {
            /* THEN branch */
            int st = 0;
            for (int i = 0; i < node->then_count; i++) {
                st = execute_single_node(term, tab, node->then_body[i]);
                if (node->then_body[i]->type == NODE_EXIT) return st;
            }
            return st;
        }

        /* ELIF branches */
        int matched = 0;
        for (int e = 0; e < node->elif_count; e++) {
            if (script_eval_test(term, tab, node->elif_conditions[e]) == 0) {
                matched = 1;
                int st = 0;
                for (int i = 0; i < node->elif_body_counts[e]; i++) {
                    st = execute_single_node(term, tab, node->elif_bodies[e][i]);
                    if (node->elif_bodies[e][i]->type == NODE_EXIT) return st;
                }
                return st;
            }
        }

        /* ELSE branch */
        if (!matched && node->else_count > 0) {
            int st = 0;
            for (int i = 0; i < node->else_count; i++) {
                st = execute_single_node(term, tab, node->else_body[i]);
                if (node->else_body[i]->type == NODE_EXIT) return st;
            }
            return st;
        }
        return 0;
    }

    case NODE_COMMENT:
    case NODE_EMPTY:
    default:
        return 0;
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  script_execute_nodes — Run a sequence of AST nodes
 * ══════════════════════════════════════════════════════════════════ */
int script_execute_nodes(Terminal *term, Tab *tab, ScriptNode *nodes)
{
    int last_status = 0;
    for (ScriptNode *cur = nodes; cur; cur = cur->next) {
        last_status = execute_single_node(term, tab, cur);
        if (cur->type == NODE_EXIT) {
            break;
        }
    }
    return last_status;
}

/* ══════════════════════════════════════════════════════════════════
 *  script_free_nodes — Free the entire AST recursively
 * ══════════════════════════════════════════════════════════════════ */
void script_free_nodes(ScriptNode *nodes)
{
    ScriptNode *cur = nodes;
    while (cur) {
        ScriptNode *next = cur->next;

        if (cur->command_line) free(cur->command_line);
        if (cur->var_name)     free(cur->var_name);
        if (cur->var_value)    free(cur->var_value);
        if (cur->condition)    free(cur->condition);

        if (cur->then_body) {
            for (int i = 0; i < cur->then_count; i++) {
                /* Free bodies that weren't in the linked list */
                cur->then_body[i]->next = NULL;
                script_free_nodes(cur->then_body[i]);
            }
            free(cur->then_body);
        }

        if (cur->elif_conditions) {
            for (int i = 0; i < cur->elif_count; i++) {
                if (cur->elif_conditions[i]) free(cur->elif_conditions[i]);
                if (cur->elif_bodies && cur->elif_bodies[i]) {
                    for (int j = 0; j < cur->elif_body_counts[i]; j++) {
                        cur->elif_bodies[i][j]->next = NULL;
                        script_free_nodes(cur->elif_bodies[i][j]);
                    }
                    free(cur->elif_bodies[i]);
                }
            }
            free(cur->elif_conditions);
            free(cur->elif_bodies);
            free(cur->elif_body_counts);
        }

        if (cur->else_body) {
            for (int i = 0; i < cur->else_count; i++) {
                cur->else_body[i]->next = NULL;
                script_free_nodes(cur->else_body[i]);
            }
            free(cur->else_body);
        }

        free(cur);
        cur = next;
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  script_execute — Interpret script with arguments
 * ══════════════════════════════════════════════════════════════════ */
int script_execute(Terminal *term, Tab *tab, const char *path,
                    char **args, int argc)
{
    if (!path) return -1;

    char errbuf[256];
    ScriptNode *ast = script_parse_file(path, errbuf, sizeof(errbuf));
    if (!ast) {
        gui_append_text(tab, errbuf);
        if (tab) tab->last_exit_status = 2;
        return 2;
    }

    /* Save previous script arguments */
    char **old_args = tab ? tab->script_args : NULL;
    int old_argc = tab ? tab->script_argc : 0;

    /* Set up new script arguments: $0 is path, $1.. are args */
    int new_argc = argc + 1;
    char **new_args = malloc(sizeof(char *) * (new_argc + 1));
    new_args[0] = strdup(path);
    for (int i = 0; i < argc; i++) {
        new_args[i + 1] = strdup(args[i]);
    }
    new_args[new_argc] = NULL;

    if (tab) {
        tab->script_args = new_args;
        tab->script_argc = new_argc;
    }

    int status = script_execute_nodes(term, tab, ast);

    /* Free new script arguments */
    for (int i = 0; i < new_argc; i++) {
        free(new_args[i]);
    }
    free(new_args);

    /* Restore previous arguments */
    if (tab) {
        tab->script_args = old_args;
        tab->script_argc = old_argc;
        tab->last_exit_status = status;
    }

    script_free_nodes(ast);
    return status;
}

/* ══════════════════════════════════════════════════════════════════
 *  script_source — Run script in current shell context
 * ══════════════════════════════════════════════════════════════════ */
int script_source(Terminal *term, Tab *tab, const char *path)
{
    if (!path) return -1;

    char errbuf[256];
    ScriptNode *ast = script_parse_file(path, errbuf, sizeof(errbuf));
    if (!ast) {
        gui_append_text(tab, errbuf);
        if (tab) tab->last_exit_status = 2;
        return 2;
    }

    int status = script_execute_nodes(term, tab, ast);
    if (tab) tab->last_exit_status = status;

    script_free_nodes(ast);
    return status;
}
