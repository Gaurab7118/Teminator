/*
 * parser.c — Command-line parsing, multiline input, quoting (Task 3)
 * Computing Lab (CS69201) · Term Project
 *
 * Implements tokenization, variable expansion, command substitution,
 * pipe/redirect parsing, and multiline continuation detection.
 */

#include "parser.h"
#include <sys/wait.h>

/* ── Forward declarations (internal helpers) ──────────────────── */

static __attribute__((unused)) char *expand_tilde(const char *token);
static int   is_varname_char(char c);
static char *capture_command_output(const char *cmd);

/* ══════════════════════════════════════════════════════════════════
 *  parser_is_special — Check if c is a shell-special character
 * ══════════════════════════════════════════════════════════════════ */
int parser_is_special(char c)
{
    return (c == '|' || c == '<' || c == '>' || c == '&' ||
            c == ';' || c == '(' || c == ')');
}

/* ══════════════════════════════════════════════════════════════════
 *  parser_is_incomplete — Does the input need a continuation line?
 *
 *  Returns 1 if the line is incomplete:
 *    - Trailing backslash (line continuation)
 *    - Unclosed single or double quotes
 *    - Unclosed $( ... )
 *    - Unclosed backticks
 * ══════════════════════════════════════════════════════════════════ */
int parser_is_incomplete(const char *line)
{
    if (!line || !*line)
        return 0;

    int    in_single   = 0;   /* inside single quotes */
    int    in_double   = 0;   /* inside double quotes */
    int    in_backtick = 0;   /* inside `...` */
    int    paren_depth = 0;   /* depth of $( ... ) nesting */
    size_t len         = strlen(line);

    for (size_t i = 0; i < len; i++) {
        char c = line[i];

        /* Handle escape: backslash outside single quotes */
        if (c == '\\' && !in_single) {
            /* Check if rest of line is just whitespace / newline or nothing */
            size_t j = i + 1;
            while (j < len && (line[j] == ' ' || line[j] == '\t' || line[j] == '\r' || line[j] == '\n'))
                j++;
            if (j == len)
                return 1;      /* trailing backslash → continuation */

            if (i + 1 < len)
                i++;           /* skip the escaped character */
            continue;
        }

        /* Single-quote toggle (not inside double quotes or backticks) */
        if (c == '\'' && !in_double) {
            in_single = !in_single;
            continue;
        }

        /* Everything inside single quotes is literal */
        if (in_single)
            continue;

        /* Double-quote toggle */
        if (c == '"' && !in_single) {
            in_double = !in_double;
            continue;
        }

        /* Backtick toggle (not inside single quotes) */
        if (c == '`') {
            in_backtick = !in_backtick;
            continue;
        }

        /* $( opens a subshell (even inside double quotes) */
        if (c == '$' && i + 1 < len && line[i + 1] == '(') {
            paren_depth++;
            i++;               /* skip the '(' */
            continue;
        }

        /* Closing paren for $( ... ) */
        if (c == ')' && paren_depth > 0) {
            paren_depth--;
            continue;
        }
    }

    /* Anything still open means the line is incomplete */
    return (in_single || in_double || in_backtick || paren_depth > 0);
}

/* ══════════════════════════════════════════════════════════════════
 *  parser_join_continuation — Merge lines ending with '\'
 *
 *  E.g.  "echo hello \\\nworld"  →  "echo hello world"
 *  Returns a new malloc'd string.
 * ══════════════════════════════════════════════════════════════════ */
char *parser_join_continuation(const char *input)
{
    if (!input)
        return NULL;

    size_t len = strlen(input);
    char  *out = malloc(len + 1);
    if (!out) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        /* Backslash immediately followed by newline → skip both */
        if (input[i] == '\\' && i + 1 < len && input[i + 1] == '\n') {
            i++;               /* skip the newline too */
            continue;
        }
        out[j++] = input[i];
    }
    out[j] = '\0';
    return out;
}

/* ══════════════════════════════════════════════════════════════════
 *  parser_expand_variables — Expand $VAR, $?, $#, $0..$9, $HOME
 *
 *  Single-quoted regions are treated as literal text.
 *  Returns a new malloc'd string with expansions applied.
 * ══════════════════════════════════════════════════════════════════ */
char *parser_expand_variables(const char *str, Tab *tab)
{
    if (!str)
        return NULL;

    size_t cap = strlen(str) * 2 + 256;
    char  *out = malloc(cap);
    if (!out) return NULL;

    size_t j       = 0;
    int    in_sq   = 0;  /* inside single quotes (literal) */

    for (size_t i = 0; str[i]; i++) {
        /* Ensure room in output buffer */
        if (j + MAX_VAR_VALUE + 16 > cap) {
            cap *= 2;
            char *tmp = realloc(out, cap);
            if (!tmp) { free(out); return NULL; }
            out = tmp;
        }

        /* Backslash escape (outside single quotes) */
        if (str[i] == '\\' && !in_sq && str[i + 1]) {
            out[j++] = str[i + 1];
            i++;
            continue;
        }

        /* Toggle single-quote mode */
        if (str[i] == '\'' && !in_sq) {
            in_sq = 1;
            /* Don't copy the quote itself into the expanded result
             * — we're expanding, not tokenizing. Actually we should
             *   preserve the quote so that tokenize can strip it. */
            out[j++] = str[i];
            continue;
        }
        if (str[i] == '\'' && in_sq) {
            in_sq = 0;
            out[j++] = str[i];
            continue;
        }

        /* Inside single quotes: literal copy */
        if (in_sq) {
            out[j++] = str[i];
            continue;
        }

        /* Variable expansion */
        if (str[i] == '$') {
            const char *value = NULL;
            size_t skip = 0;       /* extra chars consumed after '$' */

            /* $? — last exit status */
            if (str[i + 1] == '?') {
                char buf[32];
                snprintf(buf, sizeof(buf), "%d",
                         tab ? tab->last_exit_status : 0);
                value = buf;
                skip = 1;
                size_t vlen = strlen(value);
                memcpy(out + j, value, vlen);
                j += vlen;
                i += skip;
                continue;
            }

            /* $# — script argument count (excluding $0) */
            if (str[i + 1] == '#') {
                char buf[32];
                snprintf(buf, sizeof(buf), "%d",
                         tab ? (tab->script_argc > 0 ? tab->script_argc - 1 : 0) : 0);
                value = buf;
                skip = 1;
                size_t vlen = strlen(value);
                memcpy(out + j, value, vlen);
                j += vlen;
                i += skip;
                continue;
            }

            /* $0..$9 — positional script args */
            if (str[i + 1] >= '0' && str[i + 1] <= '9') {
                int idx = str[i + 1] - '0';
                if (tab && tab->script_args && idx < tab->script_argc)
                    value = tab->script_args[idx];
                else
                    value = "";
                skip = 1;
                size_t vlen = strlen(value);
                memcpy(out + j, value, vlen);
                j += vlen;
                i += skip;
                continue;
            }

            /* ${ VAR } or $VAR — named variable */
            if (str[i + 1] == '{') {
                /* Find closing brace */
                const char *start = str + i + 2;
                const char *end   = strchr(start, '}');
                if (end) {
                    char varname[MAX_VAR_NAME];
                    size_t nlen = (size_t)(end - start);
                    if (nlen >= MAX_VAR_NAME) nlen = MAX_VAR_NAME - 1;
                    memcpy(varname, start, nlen);
                    varname[nlen] = '\0';
                    skip = nlen + 2; /* '{' + name + '}' */

                    /* Look up in tab vars first */
                    value = NULL;
                    if (tab) {
                        for (int v = 0; v < tab->num_vars; v++) {
                            if (strcmp(tab->vars[v].name, varname) == 0) {
                                value = tab->vars[v].value;
                                break;
                            }
                        }
                    }
                    /* Fall back to environment */
                    if (!value)
                        value = getenv(varname);
                    if (!value)
                        value = "";

                    size_t vlen = strlen(value);
                    if (j + vlen + 16 > cap) {
                        cap = cap * 2 + vlen;
                        char *tmp = realloc(out, cap);
                        if (!tmp) { free(out); return NULL; }
                        out = tmp;
                    }
                    memcpy(out + j, value, vlen);
                    j += vlen;
                    i += skip;
                    continue;
                }
                /* No closing brace → treat '$' literally */
            }

            if (is_varname_char(str[i + 1])) {
                /* Collect variable name */
                char varname[MAX_VAR_NAME];
                size_t k = 0;
                size_t vi = i + 1;
                while (str[vi] && is_varname_char(str[vi]) &&
                       k < MAX_VAR_NAME - 1) {
                    varname[k++] = str[vi++];
                }
                varname[k] = '\0';
                skip = k; /* chars after '$' */

                /* Look up in tab vars first */
                value = NULL;
                if (tab) {
                    for (int v = 0; v < tab->num_vars; v++) {
                        if (strcmp(tab->vars[v].name, varname) == 0) {
                            value = tab->vars[v].value;
                            break;
                        }
                    }
                }
                /* Fall back to environment */
                if (!value)
                    value = getenv(varname);
                if (!value)
                    value = "";

                size_t vlen = strlen(value);
                if (j + vlen + 16 > cap) {
                    cap = cap * 2 + vlen;
                    char *tmp = realloc(out, cap);
                    if (!tmp) { free(out); return NULL; }
                    out = tmp;
                }
                memcpy(out + j, value, vlen);
                j += vlen;
                i += skip;
                continue;
            }

            /* '$' followed by something non-variable → literal */
            out[j++] = str[i];
            continue;
        }

        /* Tilde expansion at the start of a word is handled in tokenize,
         * but we pass through here. */
        out[j++] = str[i];
    }

    out[j] = '\0';
    return out;
}

/* ── is_varname_char — Characters valid in a variable name ─────── */
static int is_varname_char(char c)
{
    return (c == '_' || (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'));
}

/* ══════════════════════════════════════════════════════════════════
 *  capture_command_output — Fork/exec a command, return its stdout
 *
 *  The result is a malloc'd string with trailing newlines stripped.
 *  Returns "" on error (never NULL).
 * ══════════════════════════════════════════════════════════════════ */
static char *capture_command_output(const char *cmd)
{
    if (!cmd || !*cmd) {
        return strdup("");
    }

    int pipefd[2];
    if (pipe(pipefd) < 0)
        return strdup("");

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return strdup("");
    }

    if (pid == 0) {
        /* ── Child process ────────────────────────────────────── */
        close(pipefd[0]);                /* close read end */
        dup2(pipefd[1], STDOUT_FILENO);  /* redirect stdout to pipe */
        close(pipefd[1]);

        /* Execute via /bin/sh -c for full shell parsing */
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    /* ── Parent process ──────────────────────────────────────── */
    close(pipefd[1]);                    /* close write end */

    /* Read all output from child */
    size_t cap  = 1024;
    size_t len  = 0;
    char  *buf  = malloc(cap);
    if (!buf) {
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        return strdup("");
    }

    ssize_t n;
    while ((n = read(pipefd[0], buf + len, cap - len - 1)) > 0) {
        len += (size_t)n;
        if (len + 1 >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) { free(buf); close(pipefd[0]); waitpid(pid, NULL, 0); return strdup(""); }
            buf = tmp;
        }
    }
    close(pipefd[0]);
    waitpid(pid, NULL, 0);

    buf[len] = '\0';

    /* Strip trailing newlines */
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';

    return buf;
}

/* ══════════════════════════════════════════════════════════════════
 *  parser_expand_substitutions — Expand $(...) and `...`
 *
 *  Handles nested substitutions (processes innermost first for $()).
 *  Returns a new malloc'd string.
 * ══════════════════════════════════════════════════════════════════ */
char *parser_expand_substitutions(const char *str, Terminal *term, Tab *tab)
{
    (void)term;
    (void)tab;

    if (!str)
        return NULL;

    /* We'll iteratively resolve substitutions from the input.
     * Strategy: scan for the *innermost* $( ... ) or earliest `...`,
     *           execute it, replace, and repeat until none remain.    */

    char *work = strdup(str);
    if (!work)
        return NULL;

    int changed = 1;
    int iterations = 0;
    const int MAX_ITERATIONS = 64;   /* safety guard against infinite loops */

    while (changed && iterations < MAX_ITERATIONS) {
        changed = 0;
        iterations++;

        /* ── Phase 1: Find innermost $( ... ) ─────────────────── */
        int    in_single = 0;
        int    best_open = -1;       /* position of last '$(' without closing ')' */

        for (size_t i = 0; work[i]; i++) {
            if (work[i] == '\\' && work[i + 1]) { i++; continue; }
            if (work[i] == '\'') { in_single = !in_single; continue; }
            if (in_single) continue;

            if (work[i] == '$' && work[i + 1] == '(') {
                best_open = (int)i;
                i++;  /* skip '(' */
                continue;
            }

            if (work[i] == ')' && best_open >= 0) {
                /* Found innermost $( ... ) from best_open to i */
                size_t cmd_start = (size_t)best_open + 2; /* after "$(" */
                size_t cmd_len   = i - cmd_start;

                char *inner_cmd = malloc(cmd_len + 1);
                if (!inner_cmd) break;
                memcpy(inner_cmd, work + cmd_start, cmd_len);
                inner_cmd[cmd_len] = '\0';

                char *output = capture_command_output(inner_cmd);
                free(inner_cmd);

                /* Build new string: prefix + output + suffix */
                size_t prefix_len = (size_t)best_open;
                size_t suffix_len = strlen(work + i + 1);
                size_t out_len    = strlen(output);
                char  *new_work   = malloc(prefix_len + out_len + suffix_len + 1);
                if (!new_work) { free(output); break; }

                memcpy(new_work, work, prefix_len);
                memcpy(new_work + prefix_len, output, out_len);
                memcpy(new_work + prefix_len + out_len, work + i + 1, suffix_len);
                new_work[prefix_len + out_len + suffix_len] = '\0';

                free(output);
                free(work);
                work = new_work;
                changed = 1;
                break;   /* restart scan from the beginning */
            }
        }
        if (changed)
            continue;

        /* ── Phase 2: Find first `...` (backtick substitution) ── */
        in_single = 0;
        int bt_open = -1;

        for (size_t i = 0; work[i]; i++) {
            if (work[i] == '\\' && work[i + 1]) { i++; continue; }
            if (work[i] == '\'') { in_single = !in_single; continue; }
            if (in_single) continue;

            if (work[i] == '`') {
                if (bt_open < 0) {
                    bt_open = (int)i;
                } else {
                    /* Found matching pair from bt_open to i */
                    size_t cmd_start = (size_t)bt_open + 1;
                    size_t cmd_len   = i - cmd_start;

                    char *inner_cmd = malloc(cmd_len + 1);
                    if (!inner_cmd) break;
                    memcpy(inner_cmd, work + cmd_start, cmd_len);
                    inner_cmd[cmd_len] = '\0';

                    char *output = capture_command_output(inner_cmd);
                    free(inner_cmd);

                    size_t prefix_len = (size_t)bt_open;
                    size_t suffix_len = strlen(work + i + 1);
                    size_t out_len    = strlen(output);
                    char  *new_work   = malloc(prefix_len + out_len + suffix_len + 1);
                    if (!new_work) { free(output); break; }

                    memcpy(new_work, work, prefix_len);
                    memcpy(new_work + prefix_len, output, out_len);
                    memcpy(new_work + prefix_len + out_len, work + i + 1, suffix_len);
                    new_work[prefix_len + out_len + suffix_len] = '\0';

                    free(output);
                    free(work);
                    work = new_work;
                    changed = 1;
                    break;   /* restart scan */
                }
            }
        }
    }

    return work;
}

/* ══════════════════════════════════════════════════════════════════
 *  parser_tokenize — Split a command line into tokens
 *
 *  Respects single-quotes (literal), double-quotes (grouping),
 *  and backslash escapes.  Returns NULL-terminated malloc'd array.
 *  *count is set to the number of tokens.
 * ══════════════════════════════════════════════════════════════════ */
char **parser_tokenize(const char *line, int *count)
{
    if (!line) {
        if (count) *count = 0;
        return NULL;
    }

    /* Allocate a generous initial array */
    int    cap    = 64;
    int    n      = 0;
    char **tokens = calloc((size_t)cap, sizeof(char *));
    if (!tokens) {
        if (count) *count = 0;
        return NULL;
    }

    size_t len = strlen(line);
    size_t i   = 0;

    while (i < len) {
        /* Skip whitespace between tokens */
        while (i < len && isspace((unsigned char)line[i]))
            i++;
        if (i >= len)
            break;

        /* Start of a new token */
        size_t tok_cap = 256;
        size_t tok_len = 0;
        char  *tok     = malloc(tok_cap);
        if (!tok) break;

        while (i < len && !isspace((unsigned char)line[i])) {
            /* Grow token buffer if needed */
            if (tok_len + 4 > tok_cap) {
                tok_cap *= 2;
                char *tmp = realloc(tok, tok_cap);
                if (!tmp) { free(tok); tok = NULL; break; }
                tok = tmp;
            }

            char c = line[i];

            /* Handle single quotes: everything inside is literal */
            if (c == '\'') {
                i++; /* skip opening quote */
                while (i < len && line[i] != '\'') {
                    if (tok_len + 2 > tok_cap) {
                        tok_cap *= 2;
                        char *tmp = realloc(tok, tok_cap);
                        if (!tmp) { free(tok); tok = NULL; break; }
                        tok = tmp;
                    }
                    tok[tok_len++] = line[i++];
                }
                if (!tok) break;
                if (i < len) i++; /* skip closing quote */
                continue;
            }

            /* Handle double quotes: grouping, backslash escapes inside */
            if (c == '"') {
                i++; /* skip opening quote */
                while (i < len && line[i] != '"') {
                    if (tok_len + 2 > tok_cap) {
                        tok_cap *= 2;
                        char *tmp = realloc(tok, tok_cap);
                        if (!tmp) { free(tok); tok = NULL; break; }
                        tok = tmp;
                    }
                    if (line[i] == '\\' && i + 1 < len) {
                        char next = line[i + 1];
                        /* Inside double quotes, only certain chars are escaped */
                        if (next == '"' || next == '\\' || next == '$' || next == '`') {
                            tok[tok_len++] = next;
                            i += 2;
                        } else {
                            tok[tok_len++] = line[i++];
                        }
                    } else {
                        tok[tok_len++] = line[i++];
                    }
                }
                if (!tok) break;
                if (i < len) i++; /* skip closing quote */
                continue;
            }

            /* Backslash escape outside quotes */
            if (c == '\\' && i + 1 < len) {
                tok[tok_len++] = line[i + 1];
                i += 2;
                continue;
            }

            /* Shell special characters are their own tokens */
            if (parser_is_special(c)) {
                if (tok_len > 0)
                    break;  /* finish current token, will pick up special next time */

                tok[tok_len++] = c;
                i++;

                /* Handle multi-character operators: >>, || (future), && (future) */
                if (c == '>' && i < len && line[i] == '>') {
                    tok[tok_len++] = '>';
                    i++;
                }
                break;  /* special token complete */
            }

            /* Tilde expansion at token start */
            if (c == '~' && tok_len == 0) {
                const char *home = getenv("HOME");
                if (home) {
                    size_t hlen = strlen(home);
                    if (tok_len + hlen + 2 > tok_cap) {
                        tok_cap = tok_cap * 2 + hlen;
                        char *tmp = realloc(tok, tok_cap);
                        if (!tmp) { free(tok); tok = NULL; break; }
                        tok = tmp;
                    }
                    memcpy(tok + tok_len, home, hlen);
                    tok_len += hlen;
                    i++;
                    continue;
                }
            }

            /* Regular character */
            tok[tok_len++] = c;
            i++;
        }

        if (!tok)
            break;

        tok[tok_len] = '\0';

        /* Add token to array */
        if (n + 2 > cap) {
            cap *= 2;
            char **tmp = realloc(tokens, (size_t)cap * sizeof(char *));
            if (!tmp) { free(tok); break; }
            tokens = tmp;
        }
        tokens[n++] = tok;
    }

    tokens[n] = NULL;   /* NULL-terminated */
    if (count) *count = n;
    return tokens;
}

/* ══════════════════════════════════════════════════════════════════
 *  parser_free_tokens — Free a token array from parser_tokenize
 * ══════════════════════════════════════════════════════════════════ */
void parser_free_tokens(char **tokens, int count)
{
    if (!tokens) return;
    for (int i = 0; i < count; i++)
        free(tokens[i]);
    free(tokens);
}

/* ══════════════════════════════════════════════════════════════════
 *  parser_parse — Parse a complete command line into Command list
 *
 *  Steps:
 *    1. Tokenize the line.
 *    2. Walk through tokens, building Command structs.
 *    3. '|' creates a new Command linked via pipe_next.
 *    4. '<', '>', '>>' set input_file / output_file.
 *    5. '&' sets background flag.
 *
 *  Returns the head Command, or NULL on empty / error.
 * ══════════════════════════════════════════════════════════════════ */
Command *parser_parse(const char *line)
{
    if (!line || !*line)
        return NULL;

    int    token_count = 0;
    char **tokens      = parser_tokenize(line, &token_count);
    if (!tokens || token_count == 0) {
        parser_free_tokens(tokens, token_count);
        return NULL;
    }

    /* Allocate the first Command */
    Command *head    = calloc(1, sizeof(Command));
    Command *current = head;
    if (!head) {
        parser_free_tokens(tokens, token_count);
        return NULL;
    }

    /* Allocate args array for current command */
    current->args = calloc(MAX_ARGS + 1, sizeof(char *));
    if (!current->args) {
        free(current);
        parser_free_tokens(tokens, token_count);
        return NULL;
    }
    current->argc = 0;

    for (int i = 0; i < token_count; i++) {
        const char *tok = tokens[i];

        /* ── Pipe operator ─────────────────────────────────────── */
        if (strcmp(tok, "|") == 0) {
            /* Finish current command, start a new one */
            current->args[current->argc] = NULL;

            Command *next = calloc(1, sizeof(Command));
            if (!next) break;
            next->args = calloc(MAX_ARGS + 1, sizeof(char *));
            if (!next->args) { free(next); break; }
            next->argc = 0;

            current->pipe_next = next;
            current = next;
            continue;
        }

        /* ── Background operator ───────────────────────────────── */
        if (strcmp(tok, "&") == 0) {
            /* Set background flag on ALL commands in the pipeline */
            Command *c = head;
            while (c) {
                c->background = 1;
                c = c->pipe_next;
            }
            continue;
        }

        /* ── Input redirection ─────────────────────────────────── */
        if (strcmp(tok, "<") == 0) {
            if (i + 1 < token_count) {
                free(current->input_file);
                current->input_file = strdup(tokens[++i]);
            }
            continue;
        }

        /* ── Output redirection (append) ───────────────────────── */
        if (strcmp(tok, ">>") == 0) {
            if (i + 1 < token_count) {
                free(current->output_file);
                current->output_file  = strdup(tokens[++i]);
                current->append_output = 1;
            }
            continue;
        }

        /* ── Output redirection (truncate) ─────────────────────── */
        if (strcmp(tok, ">") == 0) {
            if (i + 1 < token_count) {
                free(current->output_file);
                current->output_file  = strdup(tokens[++i]);
                current->append_output = 0;
            }
            continue;
        }

        /* ── Regular argument ──────────────────────────────────── */
        if (current->argc < MAX_ARGS) {
            current->args[current->argc++] = strdup(tok);
        }
    }

    /* NULL-terminate the last command's args */
    if (current->args)
        current->args[current->argc] = NULL;

    /* If the head command has no arguments, treat as empty */
    if (head->argc == 0 && !head->pipe_next) {
        parser_free_command(head);
        parser_free_tokens(tokens, token_count);
        return NULL;
    }

    parser_free_tokens(tokens, token_count);
    return head;
}

/* ══════════════════════════════════════════════════════════════════
 *  parser_free_command — Free an entire pipeline of Command structs
 * ══════════════════════════════════════════════════════════════════ */
void parser_free_command(Command *cmd)
{
    while (cmd) {
        Command *next = cmd->pipe_next;

        /* Free argument array */
        if (cmd->args) {
            for (int i = 0; i < cmd->argc; i++)
                free(cmd->args[i]);
            free(cmd->args);
        }

        /* Free redirection file names */
        free(cmd->input_file);
        free(cmd->output_file);

        free(cmd);
        cmd = next;
    }
}

/* ── expand_tilde (unused externally, kept for potential future use) ─ */
static __attribute__((unused)) char *expand_tilde(const char *token)
{
    if (!token || token[0] != '~')
        return strdup(token ? token : "");

    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    size_t hlen = strlen(home);
    size_t tlen = strlen(token + 1);
    char  *out  = malloc(hlen + tlen + 1);
    if (!out) return strdup(token);

    memcpy(out, home, hlen);
    memcpy(out + hlen, token + 1, tlen);
    out[hlen + tlen] = '\0';
    return out;
}
