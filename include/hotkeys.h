/*
 * hotkeys.h — Hotkey handling and configuration (Task 7)
 */

#ifndef HOTKEYS_H
#define HOTKEYS_H

#include "terminator.h"

/* Load default hotkey bindings */
void hotkeys_load_defaults(Config *config);

/* Load user bindings from ~/.terminatorrc (overrides defaults) */
int  hotkeys_load_config(Config *config, const char *path);

/* Save current bindings to config file */
int  hotkeys_save_config(Config *config, const char *path);

/* Look up a key event and return the matching action, or -1 */
int  hotkeys_lookup(Config *config, unsigned int modifier, KeySym keysym);

/* Execute a hotkey action on the active tab. Returns 1 if handled. */
int  hotkeys_execute(Terminal *term, HotkeyAction action);

/* Validate a binding: check for conflicts. Returns 0 if OK, -1 on conflict. */
int  hotkeys_validate_binding(Config *config, HotkeyBinding *binding);

/* Get a human-readable name for a hotkey action */
const char *hotkeys_action_name(HotkeyAction action);

#endif /* HOTKEYS_H */
