/*
 * hotkeys.c — Hotkey handling and configuration (Task 7)
 * Computing Lab (CS69201) · Term Project
 *
 * Manages keyboard binding configuration: loading defaults, parsing
 * the user's ~/.terminatorrc file, saving bindings, and dispatching
 * actions for matched key events.
 */

#include "hotkeys.h"
#include "gui.h"
#include "autocomplete.h"
#include "history.h"
#include "jobs.h"
#include "tab.h"

/* ── Action name table ────────────────────────────────────────────── */

static const char *action_names[HK_ACTION_COUNT] = {
    [HK_MOVE_LEFT]       = "MOVE_LEFT",
    [HK_MOVE_RIGHT]      = "MOVE_RIGHT",
    [HK_LINE_START]      = "LINE_START",
    [HK_LINE_END]        = "LINE_END",
    [HK_HISTORY_UP]      = "HISTORY_UP",
    [HK_HISTORY_DOWN]    = "HISTORY_DOWN",
    [HK_DELETE_TO_START]  = "DELETE_TO_START",
    [HK_DELETE_TO_END]    = "DELETE_TO_END",
    [HK_DELETE_WORD]      = "DELETE_WORD",
    [HK_CLEAR_SCREEN]    = "CLEAR_SCREEN",
    [HK_EXIT_EOF]        = "EXIT_EOF",
    [HK_COPY]            = "COPY",
    [HK_PASTE]           = "PASTE",
    [HK_SEARCH_HISTORY]  = "SEARCH_HISTORY",
    [HK_AUTOCOMPLETE]    = "AUTOCOMPLETE",
    [HK_INTERRUPT]       = "INTERRUPT",
    [HK_SUSPEND]         = "SUSPEND",
    [HK_NEW_TAB]         = "NEW_TAB",
    [HK_CLOSE_TAB]       = "CLOSE_TAB",
    [HK_NEXT_TAB]        = "NEXT_TAB",
    [HK_PREV_TAB]        = "PREV_TAB",
};

/* ── Internal helpers ─────────────────────────────────────────────── */

/*
 * set_binding — Set a binding in the config's binding table.
 */
static void set_binding(Config *config, HotkeyAction action,
                        unsigned int modifier, KeySym keysym)
{
    if ((int)action < 0 || (int)action >= HK_ACTION_COUNT)
        return;

    config->bindings[action].action   = action;
    config->bindings[action].modifier = modifier;
    config->bindings[action].keysym   = keysym;
}

/*
 * parse_action_name — Convert a string like "LINE_START" to HotkeyAction.
 * Returns -1 if not found.
 */
static int parse_action_name(const char *name)
{
    for (int i = 0; i < HK_ACTION_COUNT; i++) {
        if (action_names[i] && strcasecmp(action_names[i], name) == 0)
            return i;
    }
    return -1;
}

/*
 * parse_modifier_keysym — Parse a key spec like "Ctrl+Shift+C" into
 * modifier flags and keysym.
 * Returns 0 on success, -1 on failure.
 */
static int parse_modifier_keysym(const char *spec, unsigned int *mod_out,
                                 KeySym *key_out)
{
    unsigned int modifier = 0;
    char buf[256];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Trim leading/trailing whitespace */
    char *p = buf;
    while (*p && isspace((unsigned char)*p))
        p++;
    size_t len = strlen(p);
    while (len > 0 && isspace((unsigned char)p[len - 1]))
        p[--len] = '\0';

    /* Tokenise on '+' */
    char *saveptr = NULL;
    char *token = strtok_r(p, "+", &saveptr);
    char *last_token = NULL;

    while (token) {
        /* Trim whitespace around the token */
        while (*token && isspace((unsigned char)*token))
            token++;
        char *end = token + strlen(token) - 1;
        while (end > token && isspace((unsigned char)*end))
            *end-- = '\0';

        char *next = strtok_r(NULL, "+", &saveptr);
        if (next == NULL) {
            /* Last token is the key name */
            last_token = token;
            break;
        }

        /* This token is a modifier */
        if (strcasecmp(token, "Ctrl") == 0 || strcasecmp(token, "Control") == 0) {
            modifier |= ControlMask;
        } else if (strcasecmp(token, "Shift") == 0) {
            modifier |= ShiftMask;
        } else if (strcasecmp(token, "Alt") == 0 || strcasecmp(token, "Mod1") == 0) {
            modifier |= Mod1Mask;
        } else if (strcasecmp(token, "Super") == 0 || strcasecmp(token, "Mod4") == 0) {
            modifier |= Mod4Mask;
        } else {
            /* Unknown modifier — treat as error or ignore */
            return -1;
        }

        token = next;
    }

    if (!last_token)
        return -1;

    /* Convert the key name to a keysym */
    KeySym ks = NoSymbol;

    /* Handle special key names */
    if (strcasecmp(last_token, "Left") == 0)
        ks = XK_Left;
    else if (strcasecmp(last_token, "Right") == 0)
        ks = XK_Right;
    else if (strcasecmp(last_token, "Up") == 0)
        ks = XK_Up;
    else if (strcasecmp(last_token, "Down") == 0)
        ks = XK_Down;
    else if (strcasecmp(last_token, "Tab") == 0)
        ks = XK_Tab;
    else if (strcasecmp(last_token, "Return") == 0 || strcasecmp(last_token, "Enter") == 0)
        ks = XK_Return;
    else if (strcasecmp(last_token, "Escape") == 0 || strcasecmp(last_token, "Esc") == 0)
        ks = XK_Escape;
    else if (strcasecmp(last_token, "BackSpace") == 0 || strcasecmp(last_token, "Backspace") == 0)
        ks = XK_BackSpace;
    else if (strcasecmp(last_token, "Delete") == 0)
        ks = XK_Delete;
    else if (strcasecmp(last_token, "Home") == 0)
        ks = XK_Home;
    else if (strcasecmp(last_token, "End") == 0)
        ks = XK_End;
    else if (strcasecmp(last_token, "PageUp") == 0)
        ks = XK_Page_Up;
    else if (strcasecmp(last_token, "PageDown") == 0)
        ks = XK_Page_Down;
    else if (strcasecmp(last_token, "Space") == 0)
        ks = XK_space;
    else if (strlen(last_token) == 1) {
        /* Single character — use XStringToKeysym on the uppercase version
         * for letters, or as-is for others */
        char symname[2] = { last_token[0], '\0' };

        /* For letters, XK_a through XK_z and XK_A through XK_Z */
        if (islower((unsigned char)last_token[0]))
            ks = XK_a + (last_token[0] - 'a');
        else if (isupper((unsigned char)last_token[0]))
            ks = XK_A + (last_token[0] - 'A');
        else
            ks = XStringToKeysym(symname);
    } else {
        /* Try XStringToKeysym as a last resort */
        ks = XStringToKeysym(last_token);
    }

    if (ks == NoSymbol)
        return -1;

    *mod_out = modifier;
    *key_out = ks;
    return 0;
}

/*
 * keysym_name — Return a human-readable name for a keysym.
 * Uses a static buffer (not thread-safe, but fine for config I/O).
 */
static const char *keysym_name(KeySym ks)
{
    static char buf[64];

    switch (ks) {
    case XK_Left:      return "Left";
    case XK_Right:     return "Right";
    case XK_Up:        return "Up";
    case XK_Down:      return "Down";
    case XK_Tab:       return "Tab";
    case XK_Return:    return "Return";
    case XK_Escape:    return "Escape";
    case XK_BackSpace: return "BackSpace";
    case XK_Delete:    return "Delete";
    case XK_Home:      return "Home";
    case XK_End:       return "End";
    case XK_Page_Up:   return "PageUp";
    case XK_Page_Down: return "PageDown";
    case XK_space:     return "Space";
    default:
        break;
    }

    /* Single letter keys */
    if (ks >= XK_a && ks <= XK_z) {
        buf[0] = (char)('A' + (ks - XK_a));  /* Always output uppercase */
        buf[1] = '\0';
        return buf;
    }
    if (ks >= XK_A && ks <= XK_Z) {
        buf[0] = (char)('A' + (ks - XK_A));
        buf[1] = '\0';
        return buf;
    }

    /* Fallback to Xlib's name */
    const char *name = XKeysymToString(ks);
    if (name)
        return name;

    snprintf(buf, sizeof(buf), "0x%lx", (unsigned long)ks);
    return buf;
}

/*
 * binding_to_string — Format a binding as "Ctrl+Shift+A".
 * Writes into buf (size buflen).
 */
static void binding_to_string(const HotkeyBinding *b, char *buf, int buflen)
{
    buf[0] = '\0';
    if (b->modifier & ControlMask)
        strncat(buf, "Ctrl+", (size_t)(buflen - (int)strlen(buf) - 1));
    if (b->modifier & ShiftMask)
        strncat(buf, "Shift+", (size_t)(buflen - (int)strlen(buf) - 1));
    if (b->modifier & Mod1Mask)
        strncat(buf, "Alt+", (size_t)(buflen - (int)strlen(buf) - 1));
    if (b->modifier & Mod4Mask)
        strncat(buf, "Super+", (size_t)(buflen - (int)strlen(buf) - 1));
    strncat(buf, keysym_name(b->keysym), (size_t)(buflen - (int)strlen(buf) - 1));
}

/* ── Public API ───────────────────────────────────────────────────── */

/*
 * hotkeys_load_defaults — Set up the built-in default bindings.
 */
void hotkeys_load_defaults(Config *config)
{
    if (!config)
        return;

    memset(config->bindings, 0, sizeof(config->bindings));
    config->num_bindings = HK_ACTION_COUNT;

    /* Navigation */
    set_binding(config, HK_MOVE_LEFT,     0,                     XK_Left);
    set_binding(config, HK_MOVE_RIGHT,    0,                     XK_Right);
    set_binding(config, HK_LINE_START,    ControlMask,           XK_a);
    set_binding(config, HK_LINE_END,      ControlMask,           XK_e);

    /* History browsing */
    set_binding(config, HK_HISTORY_UP,    0,                     XK_Up);
    set_binding(config, HK_HISTORY_DOWN,  0,                     XK_Down);

    /* Editing */
    set_binding(config, HK_DELETE_TO_START, ControlMask,         XK_u);
    set_binding(config, HK_DELETE_TO_END,   ControlMask,         XK_k);
    set_binding(config, HK_DELETE_WORD,     ControlMask,         XK_w);

    /* Screen */
    set_binding(config, HK_CLEAR_SCREEN,  ControlMask,           XK_l);
    set_binding(config, HK_EXIT_EOF,      ControlMask,           XK_d);

    /* Clipboard */
    set_binding(config, HK_COPY,          ControlMask | ShiftMask, XK_C);
    set_binding(config, HK_PASTE,         ControlMask | ShiftMask, XK_V);

    /* Search */
    set_binding(config, HK_SEARCH_HISTORY, ControlMask,          XK_r);

    /* Tab completion */
    set_binding(config, HK_AUTOCOMPLETE,  0,                     XK_Tab);

    /* Signal control */
    set_binding(config, HK_INTERRUPT,     ControlMask,           XK_c);
    set_binding(config, HK_SUSPEND,       ControlMask,           XK_z);

    /* Tab management */
    set_binding(config, HK_NEW_TAB,       ControlMask,           XK_t);
    set_binding(config, HK_CLOSE_TAB,     ControlMask | ShiftMask, XK_W);
    set_binding(config, HK_NEXT_TAB,      ControlMask,           XK_Page_Down);
    set_binding(config, HK_PREV_TAB,      ControlMask,           XK_Page_Up);
}

/*
 * hotkeys_load_config — Parse a config file and override default bindings.
 *
 * File format (one binding per line):
 *   Ctrl+A = LINE_START
 *   Ctrl+Shift+C = COPY
 *   # comment
 *
 * Invalid or conflicting bindings are reported to stderr and the default
 * is preserved for that action.
 *
 * Returns 0 on success, -1 if the file could not be opened (which is
 * not fatal — defaults remain).
 */
int hotkeys_load_config(Config *config, const char *path)
{
    if (!config || !path)
        return -1;

    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (errno == ENOENT)
            return 0;  /* No config file is fine */
        perror("hotkeys_load_config: fopen");
        return -1;
    }

    char line[512];
    int lineno = 0;

    while (fgets(line, sizeof(line), fp)) {
        lineno++;

        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        /* Skip empty lines and comments */
        char *p = line;
        while (*p && isspace((unsigned char)*p))
            p++;
        if (*p == '\0' || *p == '#')
            continue;

        /* Find '=' separator */
        char *eq = strchr(p, '=');
        if (!eq) {
            fprintf(stderr, "%s:%d: missing '=' separator\n", path, lineno);
            continue;
        }

        /* Split into key_spec and action_name */
        *eq = '\0';
        char *key_spec = p;
        char *act_name = eq + 1;

        /* Trim whitespace from key_spec */
        while (*key_spec && isspace((unsigned char)*key_spec))
            key_spec++;
        char *end = key_spec + strlen(key_spec) - 1;
        while (end > key_spec && isspace((unsigned char)*end))
            *end-- = '\0';

        /* Trim whitespace from action name */
        while (*act_name && isspace((unsigned char)*act_name))
            act_name++;
        end = act_name + strlen(act_name) - 1;
        while (end > act_name && isspace((unsigned char)*end))
            *end-- = '\0';

        /* Parse action */
        int action = parse_action_name(act_name);
        if (action < 0) {
            fprintf(stderr, "%s:%d: unknown action '%s'\n",
                    path, lineno, act_name);
            continue;
        }

        /* Parse key spec */
        unsigned int modifier;
        KeySym keysym;
        if (parse_modifier_keysym(key_spec, &modifier, &keysym) < 0) {
            fprintf(stderr, "%s:%d: invalid key spec '%s'\n",
                    path, lineno, key_spec);
            continue;
        }

        /* Validate: check for conflicts with other bindings */
        HotkeyBinding trial = {
            .modifier = modifier,
            .keysym   = keysym,
            .action   = (HotkeyAction)action
        };
        if (hotkeys_validate_binding(config, &trial) < 0) {
            char kbuf[128];
            binding_to_string(&trial, kbuf, sizeof(kbuf));
            fprintf(stderr, "%s:%d: conflicting binding %s for %s, keeping default\n",
                    path, lineno, kbuf, act_name);
            continue;
        }

        /* Apply the override */
        set_binding(config, (HotkeyAction)action, modifier, keysym);
    }

    fclose(fp);
    return 0;
}

/*
 * hotkeys_save_config — Write all current bindings to a config file.
 * Returns 0 on success, -1 on error.
 */
int hotkeys_save_config(Config *config, const char *path)
{
    if (!config || !path)
        return -1;

    FILE *fp = fopen(path, "w");
    if (!fp) {
        perror("hotkeys_save_config: fopen");
        return -1;
    }

    fprintf(fp, "# Terminator key bindings\n");
    fprintf(fp, "# Format: KeySpec = ACTION\n\n");

    for (int i = 0; i < HK_ACTION_COUNT; i++) {
        HotkeyBinding *b = &config->bindings[i];
        if (b->keysym == NoSymbol)
            continue;

        char kbuf[128];
        binding_to_string(b, kbuf, sizeof(kbuf));
        fprintf(fp, "%s = %s\n", kbuf, action_names[i]);
    }

    fclose(fp);
    return 0;
}

/*
 * hotkeys_lookup — Given a modifier mask and keysym, find a matching
 * binding.  Returns the HotkeyAction (>= 0) or -1 if no match.
 *
 * We mask out NumLock (Mod2Mask) and CapsLock (LockMask) from the
 * comparison so that those don't interfere with hotkey matching.
 */
int hotkeys_lookup(Config *config, unsigned int modifier, KeySym keysym)
{
    if (!config)
        return -1;

    /* Strip NumLock and CapsLock from the event modifier */
    unsigned int clean_mod = modifier & ~(Mod2Mask | LockMask);

    for (int i = 0; i < HK_ACTION_COUNT; i++) {
        HotkeyBinding *b = &config->bindings[i];
        if (b->keysym == NoSymbol)
            continue;

        /* Compare keysym.  For letter keys, compare case-insensitively
         * by checking both lowercase and uppercase variants. */
        KeySym ks = keysym;
        KeySym bks = b->keysym;

        int keymatch = 0;
        if (ks == bks) {
            keymatch = 1;
        } else {
            /* Check case-insensitive match for letters */
            KeySym ks_lower = ks, bks_lower = bks;
            if (ks >= XK_A && ks <= XK_Z)
                ks_lower = ks - XK_A + XK_a;
            if (bks >= XK_A && bks <= XK_Z)
                bks_lower = bks - XK_A + XK_a;
            if (ks_lower == bks_lower)
                keymatch = 1;
        }

        if (keymatch && clean_mod == b->modifier)
            return (int)b->action;
    }

    return -1;
}

/*
 * hotkeys_execute — Execute a hotkey action on the active tab.
 * Returns 1 if the action was handled, 0 if not.
 */
int hotkeys_execute(Terminal *term, HotkeyAction action)
{
    if (!term)
        return 0;

    Tab *tab = tab_active(term);
    if (!tab && action != HK_NEW_TAB)
        return 0;

    switch (action) {

    /* ── Cursor movement ──────────────────────────────────────── */
    case HK_MOVE_LEFT:
        if (tab->cursor_pos > 0)
            tab->cursor_pos--;
        gui_redraw(term);
        return 1;

    case HK_MOVE_RIGHT:
        if (tab->cursor_pos < tab->input_len)
            tab->cursor_pos++;
        gui_redraw(term);
        return 1;

    case HK_LINE_START:
        tab->cursor_pos = 0;
        gui_redraw(term);
        return 1;

    case HK_LINE_END:
        tab->cursor_pos = tab->input_len;
        gui_redraw(term);
        return 1;

    /* ── History navigation ───────────────────────────────────── */
    case HK_HISTORY_UP:
        if (!term->history)
            return 1;
        if (tab->history_index < 0) {
            /* First press — save current input */
            memcpy(tab->saved_input, tab->input, (size_t)(tab->input_len + 1));
            tab->history_index = 0;
        } else {
            tab->history_index++;
        }
        {
            const char *cmd = history_get(term->history, tab->history_index);
            if (cmd) {
                strncpy(tab->input, cmd, MAX_INPUT_LEN - 1);
                tab->input[MAX_INPUT_LEN - 1] = '\0';
                tab->input_len  = (int)strlen(tab->input);
                tab->cursor_pos = tab->input_len;
            } else {
                /* Beyond the oldest entry — clamp */
                tab->history_index--;
            }
        }
        gui_redraw(term);
        return 1;

    case HK_HISTORY_DOWN:
        if (!term->history || tab->history_index < 0)
            return 1;
        tab->history_index--;
        if (tab->history_index < 0) {
            /* Restore saved input */
            memcpy(tab->input, tab->saved_input,
                   strlen(tab->saved_input) + 1);
            tab->input_len  = (int)strlen(tab->input);
            tab->cursor_pos = tab->input_len;
            tab->history_index = -1;
        } else {
            const char *cmd = history_get(term->history, tab->history_index);
            if (cmd) {
                strncpy(tab->input, cmd, MAX_INPUT_LEN - 1);
                tab->input[MAX_INPUT_LEN - 1] = '\0';
                tab->input_len  = (int)strlen(tab->input);
                tab->cursor_pos = tab->input_len;
            }
        }
        gui_redraw(term);
        return 1;

    /* ── Editing ──────────────────────────────────────────────── */
    case HK_DELETE_TO_START:
        /* Kill text from start to cursor (Ctrl+U) */
        if (tab->cursor_pos > 0) {
            memmove(tab->input, tab->input + tab->cursor_pos,
                    (size_t)(tab->input_len - tab->cursor_pos + 1));
            tab->input_len -= tab->cursor_pos;
            tab->cursor_pos = 0;
        }
        gui_redraw(term);
        return 1;

    case HK_DELETE_TO_END:
        /* Kill text from cursor to end (Ctrl+K) */
        tab->input[tab->cursor_pos] = '\0';
        tab->input_len = tab->cursor_pos;
        gui_redraw(term);
        return 1;

    case HK_DELETE_WORD:
        /* Delete the word before cursor (Ctrl+W) */
        if (tab->cursor_pos > 0) {
            int end = tab->cursor_pos;

            /* Skip trailing spaces */
            while (tab->cursor_pos > 0 &&
                   tab->input[tab->cursor_pos - 1] == ' ')
                tab->cursor_pos--;

            /* Skip non-space characters (the word) */
            while (tab->cursor_pos > 0 &&
                   tab->input[tab->cursor_pos - 1] != ' ')
                tab->cursor_pos--;

            int deleted = end - tab->cursor_pos;
            memmove(tab->input + tab->cursor_pos,
                    tab->input + end,
                    (size_t)(tab->input_len - end + 1));
            tab->input_len -= deleted;
        }
        gui_redraw(term);
        return 1;

    /* ── Screen management ────────────────────────────────────── */
    case HK_CLEAR_SCREEN:
        gui_clear_tab(tab);
        gui_redraw(term);
        return 1;

    case HK_EXIT_EOF:
        /* Ctrl+D — if input is empty, signal EOF / close tab */
        if (tab->input_len == 0) {
            /* Close the tab or exit */
            if (term->num_tabs <= 1) {
                term->running = 0;  /* Exit the terminal */
            } else {
                tab_close(term, term->active_tab);
                gui_redraw(term);
            }
        }
        return 1;

    /* ── Clipboard ────────────────────────────────────────────── */
    case HK_COPY:
        /* Copy current input to clipboard */
        if (tab->input_len > 0)
            gui_copy_selection(term, tab->input, tab->input_len);
        return 1;

    case HK_PASTE:
        {
            char *clip = gui_paste_selection(term);
            if (clip) {
                int clen = (int)strlen(clip);
                if (tab->input_len + clen < MAX_INPUT_LEN) {
                    /* Insert at cursor */
                    memmove(tab->input + tab->cursor_pos + clen,
                            tab->input + tab->cursor_pos,
                            (size_t)(tab->input_len - tab->cursor_pos + 1));
                    memcpy(tab->input + tab->cursor_pos, clip, (size_t)clen);
                    tab->input_len += clen;
                    tab->cursor_pos += clen;
                }
                free(clip);
                gui_redraw(term);
            }
        }
        return 1;

    /* ── Search ───────────────────────────────────────────────── */
    case HK_SEARCH_HISTORY:
        tab->search_mode = 1;
        tab->search_input[0] = '\0';
        tab->search_input_len = 0;
        gui_redraw(term);
        return 1;

    /* ── Autocomplete ─────────────────────────────────────────── */
    case HK_AUTOCOMPLETE:
        autocomplete_complete(term, tab);
        gui_redraw(term);
        return 1;

    /* ── Signals ──────────────────────────────────────────────── */
    case HK_INTERRUPT:
        if (tab->fg_pid > 0) {
            jobs_interrupt(tab);
        } else {
            /* Clear current input line (like bash) */
            tab->input[0] = '\0';
            tab->input_len = 0;
            tab->cursor_pos = 0;
            gui_append_text(tab, "^C");
        }
        gui_redraw(term);
        return 1;

    case HK_SUSPEND:
        if (tab->fg_pid > 0) {
            jobs_suspend(tab);
        }
        gui_redraw(term);
        return 1;

    /* ── Tab management ───────────────────────────────────────── */
    case HK_NEW_TAB:
        if (term->num_tabs < MAX_TABS) {
            int idx = tab_create(term);
            if (idx >= 0) {
                tab_start_shell(term, idx);
                tab_switch(term, idx);
                gui_redraw(term);
            }
        }
        return 1;

    case HK_CLOSE_TAB:
        if (term->num_tabs > 1) {
            tab_close(term, term->active_tab);
            gui_redraw(term);
        } else {
            term->running = 0;
        }
        return 1;

    case HK_NEXT_TAB:
        if (term->num_tabs > 1) {
            int next = (term->active_tab + 1) % term->num_tabs;
            tab_switch(term, next);
            gui_redraw(term);
        }
        return 1;

    case HK_PREV_TAB:
        if (term->num_tabs > 1) {
            int prev = (term->active_tab - 1 + term->num_tabs) % term->num_tabs;
            tab_switch(term, prev);
            gui_redraw(term);
        }
        return 1;

    default:
        return 0;
    }
}

/*
 * hotkeys_validate_binding — Check whether a proposed binding conflicts
 * with an existing binding (same modifier + keysym assigned to a
 * different action).
 *
 * Returns 0 if OK, -1 if there's a conflict.
 */
int hotkeys_validate_binding(Config *config, HotkeyBinding *binding)
{
    if (!config || !binding)
        return -1;

    for (int i = 0; i < HK_ACTION_COUNT; i++) {
        HotkeyBinding *b = &config->bindings[i];
        if (b->keysym == NoSymbol)
            continue;
        if ((int)b->action == (int)binding->action)
            continue;  /* Same action — not a conflict */

        if (b->modifier == binding->modifier && b->keysym == binding->keysym)
            return -1;  /* Conflict! */
    }

    return 0;
}

/*
 * hotkeys_action_name — Return a human-readable string for an action.
 */
const char *hotkeys_action_name(HotkeyAction action)
{
    if ((int)action < 0 || (int)action >= HK_ACTION_COUNT)
        return "UNKNOWN";
    return action_names[action] ? action_names[action] : "UNKNOWN";
}
