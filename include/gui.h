/*
 * gui.h — X11 GUI rendering and event loop (Task 1)
 */

#ifndef GUI_H
#define GUI_H

#include "terminator.h"

/* Initialise X11 display, window, fonts, colours */
int  gui_init(Terminal *term);

/* Tear down X11 resources */
void gui_cleanup(Terminal *term);

/* Main event loop — blocks until terminal exits */
void gui_run(Terminal *term);

/* Redraw the entire window */
void gui_redraw(Terminal *term);

/* Redraw just the tab bar */
void gui_draw_tab_bar(Terminal *term);

/* Redraw the text area for the active tab */
void gui_draw_text_area(Terminal *term);

/* Append a line of text to a tab's display buffer */
void gui_append_text(Tab *tab, const char *text);

/* Append text without a trailing newline (for partial output) */
void gui_append_text_partial(Tab *tab, const char *text);

/* Clear a tab's display buffer */
void gui_clear_tab(Tab *tab);

/* Scroll up / down */
void gui_scroll_up(Terminal *term, int lines);
void gui_scroll_down(Terminal *term, int lines);

/* Handle X11 KeyPress event */
void gui_handle_keypress(Terminal *term, XKeyEvent *event);

/* Handle X11 ButtonPress event (tab clicks, +, scroll) */
void gui_handle_button(Terminal *term, XButtonEvent *event);
void gui_handle_button_release(Terminal *term, XButtonEvent *event);
void gui_handle_motion(Terminal *term, XMotionEvent *event);

/* Send a notification to the GUI from another thread (wakeup) */
void gui_notify(Terminal *term);

/* X11 selection (clipboard) helpers */
void gui_copy_selection(Terminal *term, const char *text, int len);
char *gui_paste_selection(Terminal *term);

#endif /* GUI_H */
