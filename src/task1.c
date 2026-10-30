/*
 * task1.c — Task 1: X11 GUI and Tabs
 * Computing Lab (CS69201) · Term Project
 *
 * Implements the complete X11 terminal emulator GUI and tab management:
 *   - X11 window creation and rendering with Xlib
 *   - Non-busy-waiting select() event loop multiplexing X11 socket & tab pipes
 *   - KeyPress event processing via XLookupString & hotkey integration
 *   - Tab bar rendering with "[id: cwd]" labels and clickable "+" / close buttons
 *   - Per-tab circular text buffer (MAX_TEXT_LINES) with scrolling
 *   - Tab lifecycle: tab_init, tab_create, tab_close, tab_switch, tab_cleanup
 *   - Shell lifecycle in thread mode (pthread) or process mode (fork)
 *   - X11 selection clipboard copy and paste
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "gui.h"
#include "tab.h"
#include "shell.h"
#include "parser.h"
#include "hotkeys.h"
#include "history.h"
#include "autocomplete.h"
#include "jobs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <libgen.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <signal.h>

/* ══════════════════════════════════════════════════════════════════
 *  SECTION 1: Tab Lifecycle & Shell Process / Thread Management
 * ══════════════════════════════════════════════════════════════════ */

/* ── tab_init — Initialise a tab's state ─────────────────────────── */
void tab_init(Tab *tab, int id)
{
    if (!tab) return;

    memset(tab, 0, sizeof(Tab));
    tab->id = id;
    tab->active = 1;
    tab->running = 1;
    tab->history_index = -1;

    tab->gui_to_shell[0] = -1;
    tab->gui_to_shell[1] = -1;
    tab->shell_to_gui[0] = -1;
    tab->shell_to_gui[1] = -1;

    /* Initialise current working directory */
    if (getcwd(tab->cwd, sizeof(tab->cwd)) == NULL) {
        const char *home = getenv("HOME");
        if (home) {
            strncpy(tab->cwd, home, sizeof(tab->cwd) - 1);
        } else {
            strncpy(tab->cwd, "/", sizeof(tab->cwd) - 1);
        }
    }
}

/* ── tab_cleanup — Free all allocated memory inside a tab ────────── */
void tab_cleanup(Tab *tab)
{
    if (!tab) return;

    /* Free display text lines */
    for (int i = 0; i < tab->num_lines; i++) {
        if (tab->text_lines[i]) {
            free(tab->text_lines[i]);
            tab->text_lines[i] = NULL;
        }
    }
    tab->num_lines = 0;
    tab->scroll_offset = 0;

    /* Free completions if any */
    for (int i = 0; i < tab->num_completions; i++) {
        if (tab->completions[i]) {
            free(tab->completions[i]);
            tab->completions[i] = NULL;
        }
    }
    tab->num_completions = 0;
    tab->autocomplete_mode = 0;

    /* Free script arguments if any */
    if (tab->script_args) {
        for (int i = 0; i < tab->script_argc; i++) {
            if (tab->script_args[i]) {
                free(tab->script_args[i]);
            }
        }
        free(tab->script_args);
        tab->script_args = NULL;
        tab->script_argc = 0;
    }

    tab->active = 0;
}

/* ── tab_active — Return pointer to the currently active tab ─────── */
Tab *tab_active(Terminal *term)
{
    if (!term || term->active_tab < 0 || term->active_tab >= term->num_tabs)
        return NULL;
    return &term->tabs[term->active_tab];
}

/* ── tab_create — Allocate and initialise a new tab ──────────────── */
int tab_create(Terminal *term)
{
    if (!term || term->num_tabs >= MAX_TABS) {
        return -1;
    }

    int idx = term->num_tabs;
    Tab *tab = &term->tabs[idx];

    tab_init(tab, idx + 1);

    /* Create communication pipes: GUI <--> Tab Shell */
    if (pipe(tab->gui_to_shell) < 0) {
        perror("tab_create: pipe gui_to_shell");
        return -1;
    }
    if (pipe(tab->shell_to_gui) < 0) {
        perror("tab_create: pipe shell_to_gui");
        close(tab->gui_to_shell[0]);
        close(tab->gui_to_shell[1]);
        return -1;
    }

    /* Make shell_to_gui[0] non-blocking for GUI select() reading */
    int flags = fcntl(tab->shell_to_gui[0], F_GETFL, 0);
    fcntl(tab->shell_to_gui[0], F_SETFL, flags | O_NONBLOCK);

    /* Launch the shell for this tab */
    if (tab_start_shell(term, idx) < 0) {
        close(tab->gui_to_shell[0]);
        close(tab->gui_to_shell[1]);
        close(tab->shell_to_gui[0]);
        close(tab->shell_to_gui[1]);
        tab_cleanup(tab);
        return -1;
    }

    term->num_tabs++;
    return idx;
}

/* ── tab_close — Terminate a tab and reclaim its resources ───────── */
void tab_close(Terminal *term, int tab_index)
{
    if (!term || tab_index < 0 || tab_index >= term->num_tabs)
        return;

    Tab *tab = &term->tabs[tab_index];
    tab->running = 0;

    /* Close write end to GUI -> shell pipe to signal EOF */
    if (tab->gui_to_shell[1] >= 0) {
        close(tab->gui_to_shell[1]);
        tab->gui_to_shell[1] = -1;
    }

    if (term->use_process_mode) {
        /* Process mode: terminate child process */
        if (tab->process_id > 0) {
            kill(tab->process_id, SIGTERM);
            waitpid(tab->process_id, NULL, 0);
            tab->process_id = 0;
        }
    } else {
        /* Thread mode: cancel/join thread */
        if (tab->thread_id) {
            pthread_cancel(tab->thread_id);
            pthread_join(tab->thread_id, NULL);
            tab->thread_id = 0;
        }
    }

    /* Close remaining pipe ends */
    if (tab->gui_to_shell[0] >= 0) {
        close(tab->gui_to_shell[0]);
        tab->gui_to_shell[0] = -1;
    }
    if (tab->shell_to_gui[0] >= 0) {
        close(tab->shell_to_gui[0]);
        tab->shell_to_gui[0] = -1;
    }
    if (tab->shell_to_gui[1] >= 0) {
        close(tab->shell_to_gui[1]);
        tab->shell_to_gui[1] = -1;
    }

    tab_cleanup(tab);

    /* Shift remaining tabs to keep the array contiguous */
    for (int i = tab_index; i < term->num_tabs - 1; i++) {
        term->tabs[i] = term->tabs[i + 1];
    }
    memset(&term->tabs[term->num_tabs - 1], 0, sizeof(Tab));

    term->num_tabs--;

    /* Adjust active tab if needed */
    if (term->active_tab >= term->num_tabs) {
        term->active_tab = (term->num_tabs > 0) ? term->num_tabs - 1 : 0;
    }
}

/* ── tab_switch — Switch the active tab and request redraw ───────── */
void tab_switch(Terminal *term, int tab_index)
{
    if (!term || tab_index < 0 || tab_index >= term->num_tabs)
        return;

    term->active_tab = tab_index;
    gui_redraw(term);
}

/* ── tab_shell_loop — Execution loop for each tab shell ──────────── */
void *tab_shell_loop(void *arg)
{
    Tab *tab = (Tab *)arg;
    Terminal *term = g_terminal;

    if (!tab) return NULL;

    /* Open stream from GUI pipe */
    FILE *in = fdopen(tab->gui_to_shell[0], "r");
    if (!in) {
        perror("tab_shell_loop: fdopen");
        return NULL;
    }

    char line[MAX_INPUT_LEN];

    while (tab->running && (!term || term->running)) {
        if (!fgets(line, sizeof(line), in)) {
            /* EOF or read error */
            break;
        }

        /* Strip trailing newlines and carriage returns */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        /* Execute command line */
        shell_execute_line(term, tab, line);

        /* Wake up the GUI thread in thread mode */
        if (term && !term->use_process_mode) {
            gui_notify(term);
        }
    }

    fclose(in);
    tab->running = 0;
    return NULL;
}

/* ── tab_start_shell — Start the tab's shell in thread/process mode ─ */
int tab_start_shell(Terminal *term, int tab_index)
{
    if (!term || tab_index < 0 || tab_index >= MAX_TABS)
        return -1;

    Tab *tab = &term->tabs[tab_index];

    if (term->use_process_mode) {
        /* ── Process Mode (useProcess=True) ─────────────────────── */
        pid_t pid = fork();
        if (pid < 0) {
            perror("tab_start_shell: fork");
            return -1;
        }

        if (pid == 0) {
            /* Child process: this is the tab shell */
            tab->process_id = getpid();

            /* Close GUI-side pipe ends */
            close(tab->gui_to_shell[1]);
            close(tab->shell_to_gui[0]);

            /* Redirect stdout and stderr to the shell_to_gui write end */
            dup2(tab->shell_to_gui[1], STDOUT_FILENO);
            dup2(tab->shell_to_gui[1], STDERR_FILENO);

            /* Child process does not use X11 */
            term->display = NULL;

            /* Run shell loop (exits on EOF / tab closure) */
            tab_shell_loop((void *)tab);
            _exit(0);
        }

        /* Parent (GUI) process */
        tab->process_id = pid;

        /* Close shell-side pipe ends in parent */
        close(tab->gui_to_shell[0]);
        close(tab->shell_to_gui[1]);
        tab->gui_to_shell[0] = -1;
        tab->shell_to_gui[1] = -1;

        return 0;
    } else {
        /* ── Thread Mode (default) ──────────────────────────────── */
        if (pthread_create(&tab->thread_id, NULL, tab_shell_loop, (void *)tab) != 0) {
            perror("tab_start_shell: pthread_create");
            return -1;
        }
        return 0;
    }
}

/* ── tab_send_input — Send a command line to the tab's shell ─────── */
void tab_send_input(Terminal *term, int tab_index, const char *line)
{
    if (!term || tab_index < 0 || tab_index >= term->num_tabs || !line)
        return;

    Tab *tab = &term->tabs[tab_index];
    int write_fd = tab->gui_to_shell[1];

    if (write_fd >= 0) {
        size_t len = strlen(line);
        ssize_t w1 = write(write_fd, line, len);
        ssize_t w2 = write(write_fd, "\n", 1);
        (void)w1;
        (void)w2;
    }
}

/* ── tab_read_output — Read available output from a tab's shell ──── */
int tab_read_output(Terminal *term, int tab_index, char *buf, int bufsize)
{
    if (!term || tab_index < 0 || tab_index >= term->num_tabs || !buf || bufsize <= 0)
        return -1;

    Tab *tab = &term->tabs[tab_index];
    int read_fd = tab->shell_to_gui[0];

    if (read_fd < 0) return -1;
    return (int)read(read_fd, buf, (size_t)bufsize);
}

/* ══════════════════════════════════════════════════════════════════
 *  SECTION 2: X11 GUI Rendering, Event Handling & Selections
 * ══════════════════════════════════════════════════════════════════ */

/* Internal self-pipe for cross-thread wakeup */
static int notify_pipe[2] = { -1, -1 };

/* Colour definitions: Pure black background and crisp white letters */
#define COLOR_BG          "#000000"
#define COLOR_FG          "#ffffff"
#define COLOR_TAB_BG      "#1a1a1a"
#define COLOR_TAB_ACTIVE  "#383838"
#define COLOR_TAB_TEXT    "#ffffff"
#define COLOR_HIGHLIGHT   "#ffff00"
#define COLOR_CURSOR      "#ffffff"

static unsigned long alloc_color(Terminal *term, const char *hex);
static void          handle_expose(Terminal *term);
static void          handle_configure(Terminal *term, XConfigureEvent *ev);
static void          handle_client_message(Terminal *term, XClientMessageEvent *ev);
static void          handle_selection_request(Terminal *term, XSelectionRequestEvent *ev);
static void          handle_selection_notify(Terminal *term, XSelectionEvent *ev);
static void          draw_prompt_and_input(Terminal *term, Tab *tab, int y);
static const char   *tab_label(Tab *tab, char *buf, int buflen);

static char  *clipboard_data = NULL;
static int    clipboard_len  = 0;
static char  *paste_buf      = NULL;
static int    paste_ready    = 0;

static unsigned long alloc_color(Terminal *term, const char *hex)
{
    XColor col;
    if (!XParseColor(term->display, term->colormap, hex, &col)) {
        return WhitePixel(term->display, term->screen);
    }
    if (!XAllocColor(term->display, term->colormap, &col)) {
        return WhitePixel(term->display, term->screen);
    }
    return col.pixel;
}

int gui_init(Terminal *term)
{
    if (pipe(notify_pipe) < 0) {
        perror("gui_init: pipe");
        return -1;
    }
    fcntl(notify_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(notify_pipe[1], F_SETFL, O_NONBLOCK);

    term->display = XOpenDisplay(NULL);
    if (!term->display) {
        fprintf(stderr, "gui_init: cannot open X display\n");
        return -1;
    }
    term->screen   = DefaultScreen(term->display);
    term->colormap = DefaultColormap(term->display, term->screen);

    const char *font_names[] = {
        "-misc-fixed-medium-r-normal--13-120-75-75-c-80-iso8859-1",
        "-misc-fixed-medium-r-normal--14-*-*-*-*-*-*-*",
        "-misc-fixed-medium-r-normal--13-*-*-*-*-*-*-*",
        "fixed",
        "9x15",
        "8x13",
        NULL
    };

    term->font = NULL;
    for (int i = 0; font_names[i] != NULL; i++) {
        term->font = XLoadQueryFont(term->display, font_names[i]);
        if (term->font) break;
    }

    if (!term->font) {
        fprintf(stderr, "gui_init: warning: falling back to server default font\n");
        XGCValues dummy_gcv;
        GC dummy_gc = XCreateGC(term->display, RootWindow(term->display, term->screen), 0, &dummy_gcv);
        term->font_width  = 8;
        term->font_height = 14;
        XFreeGC(term->display, dummy_gc);
    } else {
        term->font_width  = term->font->max_bounds.width;
        term->font_height = term->font->ascent + term->font->descent;
        if (term->font_height <= 0) term->font_height = 14;
        if (term->font_width  <= 0) term->font_width  = 8;
    }

    term->bg_color         = alloc_color(term, COLOR_BG);
    term->fg_color         = alloc_color(term, COLOR_FG);
    term->tab_bg_color     = alloc_color(term, COLOR_TAB_BG);
    term->tab_active_color = alloc_color(term, COLOR_TAB_ACTIVE);
    term->tab_text_color   = alloc_color(term, COLOR_TAB_TEXT);
    term->highlight_color  = alloc_color(term, COLOR_HIGHLIGHT);
    term->cursor_color     = alloc_color(term, COLOR_CURSOR);

    term->win_width  = 1024;
    term->win_height = 768;
    term->tab_bar_height = TAB_BAR_HEIGHT;

    term->window = XCreateSimpleWindow(
        term->display,
        RootWindow(term->display, term->screen),
        100, 100,
        (unsigned)term->win_width, (unsigned)term->win_height,
        1,
        term->fg_color,
        term->bg_color
    );

    XStoreName(term->display, term->window, "Terminator");

    XSelectInput(term->display, term->window,
                 ExposureMask | KeyPressMask | ButtonPressMask |
                 StructureNotifyMask | FocusChangeMask);

    term->wm_delete = XInternAtom(term->display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(term->display, term->window, &term->wm_delete, 1);

    term->clipboard   = XInternAtom(term->display, "CLIPBOARD", False);
    term->utf8_string = XInternAtom(term->display, "UTF8_STRING", False);

    XGCValues gcv;
    memset(&gcv, 0, sizeof(gcv));
    gcv.foreground = term->fg_color;
    gcv.background = term->bg_color;
    if (term->font) gcv.font = term->font->fid;

    unsigned long gc_mask = GCForeground | GCBackground;
    if (term->font) gc_mask |= GCFont;
    term->gc = XCreateGC(term->display, term->window, gc_mask, &gcv);

    term->cols = term->win_width  / term->font_width;
    term->rows = (term->win_height - term->tab_bar_height) / term->font_height;

    XMapWindow(term->display, term->window);
    XFlush(term->display);

    return 0;
}

void gui_cleanup(Terminal *term)
{
    if (notify_pipe[0] >= 0) { close(notify_pipe[0]); notify_pipe[0] = -1; }
    if (notify_pipe[1] >= 0) { close(notify_pipe[1]); notify_pipe[1] = -1; }

    if (term && term->display) {
        if (term->gc) {
            XFreeGC(term->display, term->gc);
            term->gc = NULL;
        }
        if (term->font) {
            XFreeFont(term->display, term->font);
            term->font = NULL;
        }
        if (term->window) {
            XDestroyWindow(term->display, term->window);
            term->window = 0;
        }
        XCloseDisplay(term->display);
        term->display = NULL;
    }

    free(clipboard_data);
    clipboard_data = NULL;
    clipboard_len  = 0;

    free(paste_buf);
    paste_buf   = NULL;
    paste_ready = 0;
}

void gui_run(Terminal *term)
{
    int x11_fd = ConnectionNumber(term->display);

    while (term->running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(x11_fd, &rfds);
        int max_fd = x11_fd;

        if (notify_pipe[0] >= 0) {
            FD_SET(notify_pipe[0], &rfds);
            if (notify_pipe[0] > max_fd) max_fd = notify_pipe[0];
        }

        for (int i = 0; i < term->num_tabs; i++) {
            int fd = term->tabs[i].shell_to_gui[0];
            if (fd >= 0 && term->tabs[i].running) {
                FD_SET(fd, &rfds);
                if (fd > max_fd) max_fd = fd;
            }
        }

        struct timeval tv;
        tv.tv_sec  = 1;
        tv.tv_usec = 0;
        int ready = select(max_fd + 1, &rfds, NULL, NULL, &tv);

        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("gui_run: select");
            break;
        }

        if (notify_pipe[0] >= 0 && FD_ISSET(notify_pipe[0], &rfds)) {
            char drain[64];
            while (read(notify_pipe[0], drain, sizeof(drain)) > 0)
                ;
            for (int i = 0; i < term->num_tabs; i++) {
                Tab *tab = &term->tabs[i];
                int visible = term->rows - 1;
                if (tab->scroll_offset <= 0 ||
                    tab->scroll_offset >= tab->num_lines - visible) {
                    tab->scroll_offset = tab->num_lines - visible;
                    if (tab->scroll_offset < 0) tab->scroll_offset = 0;
                }
            }
        }

        for (int i = 0; i < term->num_tabs; i++) {
            int fd = term->tabs[i].shell_to_gui[0];
            if (fd >= 0 && FD_ISSET(fd, &rfds)) {
                char buf[4096];
                ssize_t n = read(fd, buf, sizeof(buf) - 1);
                if (n > 0) {
                    buf[n] = '\0';
                    char *start = buf;
                    char *nl;
                    while ((nl = strchr(start, '\n')) != NULL) {
                        *nl = '\0';
                        gui_append_text(&term->tabs[i], start);
                        start = nl + 1;
                    }
                    if (*start) {
                        gui_append_text_partial(&term->tabs[i], start);
                    }

                    Tab *tab = &term->tabs[i];
                    int visible = term->rows - 1;
                    if (tab->scroll_offset <= 0 ||
                        tab->scroll_offset >= tab->num_lines - visible) {
                        tab->scroll_offset = tab->num_lines - visible;
                        if (tab->scroll_offset < 0) tab->scroll_offset = 0;
                    }
                }
            }
        }

        while (XPending(term->display)) {
            XEvent ev;
            XNextEvent(term->display, &ev);

            switch (ev.type) {
            case Expose:
                handle_expose(term);
                break;
            case ConfigureNotify:
                handle_configure(term, &ev.xconfigure);
                break;
            case KeyPress:
                gui_handle_keypress(term, &ev.xkey);
                break;
            case ButtonPress:
                gui_handle_button(term, &ev.xbutton);
                break;
            case ClientMessage:
                handle_client_message(term, &ev.xclient);
                break;
            case SelectionRequest:
                handle_selection_request(term, &ev.xselectionrequest);
                break;
            case SelectionNotify:
                handle_selection_notify(term, &ev.xselection);
                break;
            default:
                break;
            }
        }

        for (int i = 0; i < term->num_tabs; i++) {
            jobs_update(&term->tabs[i]);
        }

        gui_redraw(term);
    }
}

void gui_redraw(Terminal *term)
{
    if (!term || !term->display || !term->window) return;

    XClearWindow(term->display, term->window);
    gui_draw_tab_bar(term);
    gui_draw_text_area(term);
    XFlush(term->display);
}

void gui_draw_tab_bar(Terminal *term)
{
    int y = 0;
    XSetForeground(term->display, term->gc, term->tab_bg_color);
    XFillRectangle(term->display, term->window, term->gc,
                   0, y, (unsigned)term->win_width, (unsigned)term->tab_bar_height);

    for (int i = 0; i < term->num_tabs; i++) {
        int x = i * TAB_WIDTH;
        if (x + TAB_WIDTH > term->win_width) break;

        if (i == term->active_tab) {
            XSetForeground(term->display, term->gc, term->tab_active_color);
        } else {
            XSetForeground(term->display, term->gc, term->tab_bg_color);
        }
        XFillRectangle(term->display, term->window, term->gc,
                       x, y, TAB_WIDTH - 2, (unsigned)term->tab_bar_height);

        char label[64];
        tab_label(&term->tabs[i], label, sizeof(label));
        XSetForeground(term->display, term->gc, term->tab_text_color);
        int text_y = y + (term->tab_bar_height + term->font->ascent) / 2;
        XDrawString(term->display, term->window, term->gc,
                    x + TAB_PADDING, text_y, label, (int)strlen(label));

        int close_x = x + TAB_WIDTH - TAB_PADDING - term->font_width;
        XDrawString(term->display, term->window, term->gc,
                    close_x, text_y, "x", 1);
    }

    int plus_x = term->num_tabs * TAB_WIDTH;
    if (plus_x + TAB_PADDING + term->font_width * 3 < term->win_width) {
        XSetForeground(term->display, term->gc, term->tab_text_color);
        int text_y = y + (term->tab_bar_height + term->font->ascent) / 2;
        const char *plus = "[+]";
        XDrawString(term->display, term->window, term->gc,
                    plus_x + TAB_PADDING, text_y, plus, 3);
    }

    XSetForeground(term->display, term->gc, term->highlight_color);
    XDrawLine(term->display, term->window, term->gc,
              0, term->tab_bar_height - 1,
              term->win_width, term->tab_bar_height - 1);
}

static const char *tab_label(Tab *tab, char *buf, int buflen)
{
    char cwd_copy[MAX_PATH_LEN];
    strncpy(cwd_copy, tab->cwd, sizeof(cwd_copy) - 1);
    cwd_copy[sizeof(cwd_copy) - 1] = '\0';
    const char *base = basename(cwd_copy);
    if (!base || !*base) base = "/";

    snprintf(buf, buflen, "[%d: %s]", tab->id, base);
    return buf;
}

void gui_draw_text_area(Terminal *term)
{
    Tab *tab = tab_active(term);
    if (!tab) return;

    int text_area_y = term->tab_bar_height;
    int line_height = term->font_height;
    if (line_height <= 0) line_height = 14;

    int max_visible = (term->win_height - text_area_y - line_height) / line_height;
    if (max_visible < 0) max_visible = 0;

    int start_line = tab->scroll_offset;
    if (start_line < 0) start_line = 0;
    int end_line = start_line + max_visible;
    if (end_line > tab->num_lines) end_line = tab->num_lines;

    XSetForeground(term->display, term->gc, term->fg_color);

    int row = 0;
    for (int i = start_line; i < end_line; i++) {
        int y = text_area_y + (row + 1) * line_height;
        const char *line = tab->text_lines[i] ? tab->text_lines[i] : "";
        XDrawString(term->display, term->window, term->gc,
                    TAB_PADDING, y, line, (int)strlen(line));
        row++;
    }

    if (tab->scroll_offset > 0) {
        char scroll_info[32];
        snprintf(scroll_info, sizeof(scroll_info), "-- SCROLL %d/%d --",
                 tab->scroll_offset, tab->num_lines);
        XSetForeground(term->display, term->gc, term->highlight_color);
        int info_x = term->win_width - (int)strlen(scroll_info) * term->font_width - TAB_PADDING;
        XDrawString(term->display, term->window, term->gc,
                    info_x, text_area_y + line_height,
                    scroll_info, (int)strlen(scroll_info));
        XSetForeground(term->display, term->gc, term->fg_color);
    }

    /* Ordinary terminal prompt placement:
     * Prompt appears immediately beneath the last visible line (top to bottom).
     * If the screen fills with output, the prompt stays on the bottom visible line. */
    int prompt_row = tab->num_lines - tab->scroll_offset;
    if (prompt_row < 0) prompt_row = 0;
    if (prompt_row > max_visible) prompt_row = max_visible;

    int input_y = text_area_y + (prompt_row + 1) * line_height;
    draw_prompt_and_input(term, tab, input_y);
}

static void draw_prompt_and_input(Terminal *term, Tab *tab, int y)
{
    if (tab->in_multiwatch) {
        const char *mw_status = "[multiWatch active -- Press Ctrl+C to stop]";
        XSetForeground(term->display, term->gc, term->highlight_color);
        XDrawString(term->display, term->window, term->gc,
                    TAB_PADDING, y, mw_status, (int)strlen(mw_status));
        XSetForeground(term->display, term->gc, term->fg_color);
        return;
    }

    const char *prompt = tab->in_multiline ? CONT_PROMPT : PROMPT_FORMAT;
    if (tab->search_mode) prompt = SEARCH_PROMPT;

    int prompt_len = (int)strlen(prompt);
    XSetForeground(term->display, term->gc,
                   tab->search_mode ? term->highlight_color : term->fg_color);
    XDrawString(term->display, term->window, term->gc,
                TAB_PADDING, y, prompt, prompt_len);

    const char *input_text = tab->search_mode ? tab->search_input : tab->input;
    int         input_len  = tab->search_mode ? tab->search_input_len : tab->input_len;
    int         cursor     = tab->search_mode ? tab->search_input_len : tab->cursor_pos;

    int input_x = TAB_PADDING + prompt_len * term->font_width;
    XSetForeground(term->display, term->gc, term->fg_color);
    if (input_len > 0) {
        XDrawString(term->display, term->window, term->gc,
                    input_x, y, input_text, input_len);
    }

    int ascent   = term->font ? term->font->ascent : (term->font_height * 3 / 4);
    int cursor_x = input_x + cursor * term->font_width;
    int cursor_y = y - ascent;
    int cursor_w = term->font_width > 0 ? term->font_width : 8;
    int cursor_h = term->font_height > 0 ? term->font_height : 14;

    XSetForeground(term->display, term->gc, term->cursor_color);
    XFillRectangle(term->display, term->window, term->gc,
                   cursor_x, cursor_y, (unsigned)cursor_w, (unsigned)cursor_h);

    if (cursor < input_len) {
        XSetForeground(term->display, term->gc, term->bg_color);
        XDrawString(term->display, term->window, term->gc,
                    cursor_x, y, &input_text[cursor], 1);
        XSetForeground(term->display, term->gc, term->fg_color);
    }

    if (tab->autocomplete_mode && tab->num_completions > 0) {
        int comp_y = (y + term->font_height <= term->win_height - 4)
                   ? (y + term->font_height)
                   : (y - term->font_height - 4);
        char comp_line[MAX_LINE_LEN];
        comp_line[0] = '\0';
        for (int i = 0; i < tab->num_completions && i < 8; i++) {
            char item[128];
            snprintf(item, sizeof(item), "%d. %s   ", i + 1, tab->completions[i]);
            strncat(comp_line, item, sizeof(comp_line) - strlen(comp_line) - 1);
        }
        XSetForeground(term->display, term->gc, term->tab_active_color);
        XFillRectangle(term->display, term->window, term->gc,
                       0, comp_y - ascent - 2,
                       (unsigned)term->win_width, (unsigned)term->font_height + 4);
        XSetForeground(term->display, term->gc, term->fg_color);
        XDrawString(term->display, term->window, term->gc,
                    TAB_PADDING, comp_y,
                    comp_line, (int)strlen(comp_line));
    }
}

void gui_handle_keypress(Terminal *term, XKeyEvent *event)
{
    Tab *tab = tab_active(term);
    if (!tab) return;

    char   buf[32];
    KeySym keysym = NoSymbol;
    Status status = 0;
    (void)status;

    int len = XLookupString(event, buf, sizeof(buf) - 1, &keysym, NULL);
    buf[len] = '\0';

    unsigned int mod = event->state & (ControlMask | ShiftMask | Mod1Mask);

    /* Direct tab management shortcuts: Ctrl+PageDown, Ctrl+PageUp, Ctrl+Tab, Ctrl+T, Alt+1..9 */
    if (mod & ControlMask) {
        if (keysym == XK_Next || keysym == XK_Page_Down || keysym == 0xff9b /* XK_KP_Page_Down */ ||
            ((keysym == XK_Tab || keysym == 0xfe20) && !(mod & ShiftMask))) {
            if (term->num_tabs > 1) {
                tab_switch(term, (term->active_tab + 1) % term->num_tabs);
            }
            return;
        }
        if (keysym == XK_Prior || keysym == XK_Page_Up || keysym == 0xff9a /* XK_KP_Page_Up */ ||
            ((keysym == XK_Tab || keysym == 0xfe20) && (mod & ShiftMask))) {
            if (term->num_tabs > 1) {
                tab_switch(term, (term->active_tab - 1 + term->num_tabs) % term->num_tabs);
            }
            return;
        }
        if (keysym == XK_t || keysym == XK_T) {
            if (term->num_tabs < MAX_TABS) {
                int idx = tab_create(term);
                if (idx >= 0) tab_switch(term, idx);
            }
            return;
        }
        if ((keysym == XK_w || keysym == XK_W) && (mod & ShiftMask)) {
            if (term->num_tabs > 1) {
                int cur = term->active_tab;
                tab_close(term, cur);
                if (cur >= term->num_tabs) cur = term->num_tabs - 1;
                tab_switch(term, cur);
            } else {
                term->running = 0;
            }
            return;
        }
    }

    if (mod & Mod1Mask) {
        if (keysym >= XK_1 && keysym <= XK_9) {
            int t_idx = keysym - XK_1;
            if (t_idx < term->num_tabs) {
                tab_switch(term, t_idx);
                return;
            }
        }
    }

    int action = hotkeys_lookup(&term->config, mod, keysym);
    if (action >= 0) {
        switch ((HotkeyAction)action) {
        case HK_LINE_START:
            tab->cursor_pos = 0;
            return;
        case HK_LINE_END:
            tab->cursor_pos = tab->input_len;
            return;
        case HK_MOVE_LEFT:
            if (tab->cursor_pos > 0) tab->cursor_pos--;
            return;
        case HK_MOVE_RIGHT:
            if (tab->cursor_pos < tab->input_len) tab->cursor_pos++;
            return;
        case HK_DELETE_TO_START:
            if (tab->cursor_pos > 0) {
                memmove(tab->input,
                        &tab->input[tab->cursor_pos],
                        (size_t)(tab->input_len - tab->cursor_pos + 1));
                tab->input_len -= tab->cursor_pos;
                tab->cursor_pos = 0;
            }
            return;
        case HK_DELETE_TO_END:
            tab->input[tab->cursor_pos] = '\0';
            tab->input_len = tab->cursor_pos;
            return;
        case HK_DELETE_WORD:
            if (tab->cursor_pos > 0) {
                int p = tab->cursor_pos;
                while (p > 0 && isspace((unsigned char)tab->input[p - 1])) p--;
                while (p > 0 && !isspace((unsigned char)tab->input[p - 1])) p--;
                int del_len = tab->cursor_pos - p;
                memmove(&tab->input[p],
                        &tab->input[tab->cursor_pos],
                        (size_t)(tab->input_len - tab->cursor_pos + 1));
                tab->input_len -= del_len;
                tab->cursor_pos = p;
            }
            return;
        case HK_CLEAR_SCREEN:
            gui_clear_tab(tab);
            return;
        case HK_HISTORY_UP:
            if (term->history) {
                int total = history_count(term->history);
                if (total > 0) {
                    if (tab->history_index == -1) {
                        strncpy(tab->saved_input, tab->input, sizeof(tab->saved_input) - 1);
                        tab->history_index = 0;
                    } else if (tab->history_index < total - 1) {
                        tab->history_index++;
                    }
                    const char *h = history_get(term->history, tab->history_index);
                    if (h) {
                        strncpy(tab->input, h, sizeof(tab->input) - 1);
                        tab->input_len  = (int)strlen(tab->input);
                        tab->cursor_pos = tab->input_len;
                    }
                }
            }
            return;
        case HK_HISTORY_DOWN:
            if (term->history && tab->history_index >= 0) {
                tab->history_index--;
                if (tab->history_index < 0) {
                    strncpy(tab->input, tab->saved_input, sizeof(tab->input) - 1);
                    tab->input_len  = (int)strlen(tab->input);
                    tab->cursor_pos = tab->input_len;
                } else {
                    const char *h = history_get(term->history, tab->history_index);
                    if (h) {
                        strncpy(tab->input, h, sizeof(tab->input) - 1);
                        tab->input_len  = (int)strlen(tab->input);
                        tab->cursor_pos = tab->input_len;
                    }
                }
            }
            return;
        case HK_AUTOCOMPLETE:
            autocomplete_complete(term, tab);
            return;
        case HK_COPY:
            if (tab->input_len > 0) {
                gui_copy_selection(term, tab->input, tab->input_len);
            }
            return;
        case HK_PASTE: {
            char *pasted = gui_paste_selection(term);
            if (pasted) {
                size_t plen = strlen(pasted);
                if (tab->input_len + plen < sizeof(tab->input) - 1) {
                    memmove(&tab->input[tab->cursor_pos + plen],
                            &tab->input[tab->cursor_pos],
                            (size_t)(tab->input_len - tab->cursor_pos + 1));
                    memcpy(&tab->input[tab->cursor_pos], pasted, plen);
                    tab->input_len  += (int)plen;
                    tab->cursor_pos += (int)plen;
                }
                free(pasted);
            }
            return;
        }
        case HK_NEW_TAB:
            if (term->num_tabs < MAX_TABS) {
                int idx = tab_create(term);
                if (idx >= 0) tab_switch(term, idx);
            }
            return;
        case HK_CLOSE_TAB:
            if (term->num_tabs > 1) {
                int cur = term->active_tab;
                tab_close(term, cur);
                if (cur >= term->num_tabs) cur = term->num_tabs - 1;
                tab_switch(term, cur);
            } else {
                term->running = 0;
            }
            return;
        case HK_NEXT_TAB:
            if (term->num_tabs > 1)
                tab_switch(term, (term->active_tab + 1) % term->num_tabs);
            return;
        case HK_PREV_TAB:
            if (term->num_tabs > 1)
                tab_switch(term, (term->active_tab - 1 + term->num_tabs) % term->num_tabs);
            return;
        case HK_SEARCH_HISTORY:
            tab->search_mode = !tab->search_mode;
            if (tab->search_mode) {
                tab->search_input[0]  = '\0';
                tab->search_input_len = 0;
            }
            return;
        case HK_INTERRUPT:
            if (tab->in_multiwatch) {
                tab->in_multiwatch = 0;
                for (int i = 0; i < tab->multiwatch_count; i++) {
                    if (tab->multiwatch_pids[i] > 0) {
                        kill(-tab->multiwatch_pids[i], SIGINT);
                        kill(tab->multiwatch_pids[i], SIGINT);
                    }
                }
            } else if (tab->fg_pid > 0) {
                jobs_interrupt(tab);
            }
            tab->input[0]      = '\0';
            tab->input_len     = 0;
            tab->cursor_pos    = 0;
            tab->in_multiline  = 0;
            tab->multiline_len = 0;
            tab->multiline_buf[0] = '\0';
            gui_append_text(tab, "^C");
            return;
        case HK_SUSPEND:
            jobs_suspend(tab);
            return;
        default:
            if (hotkeys_execute(term, (HotkeyAction)action))
                return;
            break;
        }
    }

    if (tab->in_multiwatch) {
        return;
    }

    if (tab->search_mode) {
        if (keysym == XK_Return || keysym == XK_KP_Enter) {
            if (tab->search_input_len > 0 && term->history) {
                const char *results[MAX_HISTORY];
                int nres = history_search(term->history, tab->search_input,
                                          results, MAX_HISTORY);

                char echo[MAX_INPUT_LEN + 32];
                snprintf(echo, sizeof(echo), "(Ctrl+R) %s", tab->search_input);
                gui_append_text(tab, echo);

                if (nres > 0) {
                    for (int i = 0; i < nres; i++) {
                        gui_append_text(tab, results[i]);
                    }
                    strncpy(tab->input, results[0], sizeof(tab->input) - 1);
                    tab->input_len  = (int)strlen(tab->input);
                    tab->cursor_pos = tab->input_len;
                } else {
                    gui_append_text(tab, "No match for search term in history");
                }
            }
            tab->search_mode      = 0;
            tab->search_input[0]  = '\0';
            tab->search_input_len = 0;
            return;
        }
        if (keysym == XK_BackSpace) {
            if (tab->search_input_len > 0) {
                tab->search_input[--tab->search_input_len] = '\0';
            }
            return;
        }
        if (keysym == XK_Escape) {
            tab->search_mode      = 0;
            tab->search_input[0]  = '\0';
            tab->search_input_len = 0;
            return;
        }
        if (len > 0 && isprint((unsigned char)buf[0])) {
            if (tab->search_input_len < (int)sizeof(tab->search_input) - 1) {
                tab->search_input[tab->search_input_len++] = buf[0];
                tab->search_input[tab->search_input_len]   = '\0';
            }
            return;
        }
        return;
    }

    if (tab->autocomplete_mode) {
        if (len == 1 && buf[0] >= '1' && buf[0] <= '9') {
            int choice = buf[0] - '0';
            autocomplete_select(term, tab, choice);
            return;
        }
        autocomplete_select(term, tab, 0);
    }

    if (keysym == XK_Return || keysym == XK_KP_Enter) {
        int incomplete = parser_is_incomplete(
            tab->in_multiline ? tab->multiline_buf : tab->input);

        if (incomplete) {
            if (!tab->in_multiline) {
                size_t cpy = tab->input_len < (int)sizeof(tab->multiline_buf) - 1 ? (size_t)tab->input_len : sizeof(tab->multiline_buf) - 1;
                memcpy(tab->multiline_buf, tab->input, cpy);
                tab->multiline_buf[cpy] = '\0';
                tab->multiline_len = (int)cpy;
                tab->in_multiline  = 1;
            } else {
                if (tab->multiline_len + 1 < (int)sizeof(tab->multiline_buf) - 1) {
                    tab->multiline_buf[tab->multiline_len++] = '\n';
                    size_t rem = (size_t)(sizeof(tab->multiline_buf) - tab->multiline_len - 1);
                    size_t cpy = (size_t)tab->input_len < rem ? (size_t)tab->input_len : rem;
                    memcpy(tab->multiline_buf + tab->multiline_len, tab->input, cpy);
                    tab->multiline_len += (int)cpy;
                    tab->multiline_buf[tab->multiline_len] = '\0';
                }
            }

            char echo[MAX_INPUT_LEN + 32];
            snprintf(echo, sizeof(echo), "%s%s",
                     tab->in_multiline ? CONT_PROMPT : PROMPT_FORMAT,
                     tab->input);
            gui_append_text(tab, echo);

            int max_vis = term->rows - 1;
            if (max_vis < 0) max_vis = 0;
            if (tab->num_lines > max_vis)
                tab->scroll_offset = tab->num_lines - max_vis;
            else
                tab->scroll_offset = 0;

            tab->input[0]   = '\0';
            tab->input_len  = 0;
            tab->cursor_pos = 0;
            return;
        }

        char final_line[MAX_INPUT_LEN * 8];
        if (tab->in_multiline) {
            if (tab->multiline_len + tab->input_len < (int)sizeof(tab->multiline_buf) - 1) {
                memcpy(&tab->multiline_buf[tab->multiline_len],
                       tab->input, (size_t)tab->input_len);
                tab->multiline_len += tab->input_len;
                tab->multiline_buf[tab->multiline_len] = '\0';
            }
            strncpy(final_line, tab->multiline_buf, sizeof(final_line) - 1);
            tab->in_multiline     = 0;
            tab->multiline_len    = 0;
            tab->multiline_buf[0] = '\0';
        } else {
            strncpy(final_line, tab->input, sizeof(final_line) - 1);
        }
        final_line[sizeof(final_line) - 1] = '\0';

        char echo[MAX_INPUT_LEN + 32];
        snprintf(echo, sizeof(echo), "%s%s", PROMPT_FORMAT, tab->input);
        gui_append_text(tab, echo);

        int max_vis = term->rows - 1;
        if (max_vis < 0) max_vis = 0;
        if (tab->num_lines > max_vis)
            tab->scroll_offset = tab->num_lines - max_vis;
        else
            tab->scroll_offset = 0;

        if (term->history && final_line[0] != '\0') {
            int all_space = 1;
            for (size_t i = 0; final_line[i]; i++) {
                if (!isspace((unsigned char)final_line[i])) { all_space = 0; break; }
            }
            if (!all_space)
                history_add(term->history, final_line);
        }

        tab_send_input(term, term->active_tab, final_line);

        tab->input[0]      = '\0';
        tab->input_len     = 0;
        tab->cursor_pos    = 0;
        tab->history_index = -1;
        return;
    }

    if (keysym == XK_BackSpace) {
        if (tab->cursor_pos > 0) {
            memmove(&tab->input[tab->cursor_pos - 1],
                    &tab->input[tab->cursor_pos],
                    (size_t)(tab->input_len - tab->cursor_pos + 1));
            tab->input_len--;
            tab->cursor_pos--;
        }
        return;
    }

    if (keysym == XK_Delete) {
        if (tab->cursor_pos < tab->input_len) {
            memmove(&tab->input[tab->cursor_pos],
                    &tab->input[tab->cursor_pos + 1],
                    (size_t)(tab->input_len - tab->cursor_pos));
            tab->input_len--;
        }
        return;
    }

    if (keysym == XK_Prior) {
        gui_scroll_up(term, SCROLL_LINES);
        return;
    }
    if (keysym == XK_Next) {
        gui_scroll_down(term, SCROLL_LINES);
        return;
    }

    if (len > 0) {
        for (int b = 0; b < len; b++) {
            unsigned char c = (unsigned char)buf[b];
            if (!iscntrl(c) || c == ' ') {
                if (tab->input_len < MAX_INPUT_LEN - 1) {
                    memmove(&tab->input[tab->cursor_pos + 1],
                            &tab->input[tab->cursor_pos],
                            (size_t)(tab->input_len - tab->cursor_pos + 1));
                    tab->input[tab->cursor_pos] = (char)c;
                    tab->input_len++;
                    tab->cursor_pos++;
                }
            }
        }
    }
}

void gui_handle_button(Terminal *term, XButtonEvent *event)
{
    int x = event->x;
    int y = event->y;

    if (y < term->tab_bar_height && event->button == Button1) {
        /* Check click on [+] button: wide hitbox (up to 70 pixels after last tab) */
        int plus_x_start = term->num_tabs * TAB_WIDTH;
        if (x >= plus_x_start && x < plus_x_start + 70) {
            if (term->num_tabs < MAX_TABS) {
                int idx = tab_create(term);
                if (idx >= 0) tab_switch(term, idx);
            }
            return;
        }

        /* Check click on an existing tab */
        int tab_idx = x / TAB_WIDTH;
        if (tab_idx >= 0 && tab_idx < term->num_tabs) {
            int tab_start = tab_idx * TAB_WIDTH;
            int close_x_start = tab_start + TAB_WIDTH - 28;
            /* If clicking near the right edge, close the tab; otherwise switch to it */
            if (x >= close_x_start && x <= tab_start + TAB_WIDTH) {
                if (term->num_tabs > 1) {
                    tab_close(term, tab_idx);
                    if (term->active_tab >= term->num_tabs)
                        term->active_tab = term->num_tabs - 1;
                    tab_switch(term, term->active_tab);
                } else {
                    term->running = 0;
                }
            } else {
                tab_switch(term, tab_idx);
            }
        }
        return;
    }

    if (event->button == Button4) {
        gui_scroll_up(term, SCROLL_LINES);
    } else if (event->button == Button5) {
        gui_scroll_down(term, SCROLL_LINES);
    }
}

void gui_append_text(Tab *tab, const char *text)
{
    if (!tab || !text) return;

    if (g_terminal && g_terminal->use_process_mode && tab->shell_to_gui[1] >= 0) {
        size_t tlen = strlen(text);
        ssize_t w1 = write(tab->shell_to_gui[1], text, tlen);
        ssize_t w2 = write(tab->shell_to_gui[1], "\n", 1);
        (void)w1;
        (void)w2;
        return;
    }

    if (tab->num_lines >= MAX_TEXT_LINES) {
        free(tab->text_lines[0]);
        memmove(&tab->text_lines[0],
                &tab->text_lines[1],
                (size_t)(MAX_TEXT_LINES - 1) * sizeof(char *));
        tab->num_lines = MAX_TEXT_LINES - 1;
        if (tab->scroll_offset > 0) tab->scroll_offset--;
    }

    tab->text_lines[tab->num_lines] = strdup(text);
    if (!tab->text_lines[tab->num_lines]) {
        fprintf(stderr, "gui_append_text: out of memory\n");
        return;
    }
    tab->num_lines++;

    if (g_terminal && !g_terminal->use_process_mode) {
        gui_notify(g_terminal);
    }
}

void gui_append_text_partial(Tab *tab, const char *text)
{
    if (!tab || !text || !*text) return;

    if (g_terminal && g_terminal->use_process_mode && tab->shell_to_gui[1] >= 0) {
        ssize_t w = write(tab->shell_to_gui[1], text, strlen(text));
        (void)w;
        return;
    }

    if (tab->num_lines > 0 && tab->text_lines[tab->num_lines - 1]) {
        char *old = tab->text_lines[tab->num_lines - 1];
        size_t old_len = strlen(old);
        size_t add_len = strlen(text);
        char *combined = realloc(old, old_len + add_len + 1);
        if (!combined) {
            fprintf(stderr, "gui_append_text_partial: out of memory\n");
            return;
        }
        memcpy(combined + old_len, text, add_len + 1);
        tab->text_lines[tab->num_lines - 1] = combined;
        if (g_terminal && !g_terminal->use_process_mode) {
            gui_notify(g_terminal);
        }
    } else {
        gui_append_text(tab, text);
    }
}

void gui_clear_tab(Tab *tab)
{
    if (!tab) return;

    for (int i = 0; i < tab->num_lines; i++) {
        free(tab->text_lines[i]);
        tab->text_lines[i] = NULL;
    }
    tab->num_lines     = 0;
    tab->scroll_offset = 0;
}

void gui_scroll_up(Terminal *term, int lines)
{
    Tab *tab = tab_active(term);
    if (!tab) return;
    tab->scroll_offset -= lines;
    if (tab->scroll_offset < 0) tab->scroll_offset = 0;
}

void gui_scroll_down(Terminal *term, int lines)
{
    Tab *tab = tab_active(term);
    if (!tab) return;
    int max_scroll = tab->num_lines - (term->rows - 1);
    if (max_scroll < 0) max_scroll = 0;
    tab->scroll_offset += lines;
    if (tab->scroll_offset > max_scroll) tab->scroll_offset = max_scroll;
}

void gui_copy_selection(Terminal *term, const char *text, int len)
{
    if (!text || len <= 0) return;

    free(clipboard_data);
    clipboard_data = malloc((size_t)len + 1);
    if (!clipboard_data) return;
    memcpy(clipboard_data, text, (size_t)len);
    clipboard_data[len] = '\0';
    clipboard_len = len;

    XSetSelectionOwner(term->display, XA_PRIMARY,   term->window, CurrentTime);
    XSetSelectionOwner(term->display, term->clipboard, term->window, CurrentTime);
    XFlush(term->display);
}

char *gui_paste_selection(Terminal *term)
{
    Atom sel = term->clipboard;
    Window owner = XGetSelectionOwner(term->display, sel);
    if (owner == None) {
        sel   = XA_PRIMARY;
        owner = XGetSelectionOwner(term->display, sel);
        if (owner == None) return NULL;
    }

    Atom target = term->utf8_string;
    Atom prop   = XInternAtom(term->display, "TERMINATOR_PASTE", False);

    paste_ready = 0;
    free(paste_buf);
    paste_buf = NULL;

    XConvertSelection(term->display, sel, target, prop, term->window, CurrentTime);
    XFlush(term->display);

    int elapsed_ms = 0;
    while (!paste_ready && term->running) {
        while (XPending(term->display)) {
            XEvent ev;
            XNextEvent(term->display, &ev);
            if (ev.type == SelectionNotify) {
                handle_selection_notify(term, &ev.xselection);
                break;
            }
        }
        if (paste_ready) break;
        usleep(10000);
        elapsed_ms += 10;
        if (elapsed_ms > 500) break;
    }

    if (paste_buf) {
        char *result = paste_buf;
        paste_buf = NULL;
        return result;
    }
    return NULL;
}

void gui_notify(Terminal *term)
{
    (void)term;
    if (notify_pipe[1] >= 0) {
        char byte = 1;
        ssize_t r = write(notify_pipe[1], &byte, 1);
        (void)r;
    }
}

static void handle_expose(Terminal *term)
{
    gui_redraw(term);
}

static void handle_configure(Terminal *term, XConfigureEvent *ev)
{
    if (ev->width != term->win_width || ev->height != term->win_height) {
        term->win_width  = ev->width;
        term->win_height = ev->height;
        term->cols = term->win_width  / term->font_width;
        term->rows = (term->win_height - term->tab_bar_height) / term->font_height;
    }
}

static void handle_client_message(Terminal *term, XClientMessageEvent *ev)
{
    if ((Atom)ev->data.l[0] == term->wm_delete) {
        term->running = 0;
    }
}

static void handle_selection_request(Terminal *term, XSelectionRequestEvent *ev)
{
    XSelectionEvent response;
    memset(&response, 0, sizeof(response));
    response.type      = SelectionNotify;
    response.requestor = ev->requestor;
    response.selection = ev->selection;
    response.target    = ev->target;
    response.time      = ev->time;
    response.property  = None;

    Atom targets_atom = XInternAtom(term->display, "TARGETS", False);

    if (ev->target == targets_atom) {
        Atom supported[] = { targets_atom, XA_STRING, term->utf8_string };
        XChangeProperty(term->display, ev->requestor,
                        ev->property, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)supported,
                        (int)(sizeof(supported) / sizeof(Atom)));
        response.property = ev->property;
    } else if ((ev->target == XA_STRING || ev->target == term->utf8_string)
               && clipboard_data) {
        XChangeProperty(term->display, ev->requestor,
                        ev->property, ev->target, 8, PropModeReplace,
                        (unsigned char *)clipboard_data,
                        clipboard_len);
        response.property = ev->property;
    }

    XSendEvent(term->display, ev->requestor, False, 0, (XEvent *)&response);
    XFlush(term->display);
}

static void handle_selection_notify(Terminal *term, XSelectionEvent *ev)
{
    if (ev->property == None) {
        paste_ready = 1;
        return;
    }

    Atom actual_type;
    int  actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(
        term->display, term->window,
        ev->property,
        0, 65536,
        True,
        AnyPropertyType,
        &actual_type, &actual_format,
        &nitems, &bytes_after,
        &data
    );

    if (status == Success && data && nitems > 0) {
        free(paste_buf);
        paste_buf = malloc(nitems + 1);
        if (paste_buf) {
            memcpy(paste_buf, data, nitems);
            paste_buf[nitems] = '\0';
        }
    }

    if (data) XFree(data);
    paste_ready = 1;
}
