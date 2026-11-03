/*
 * terminator.h — Core types and constants for the Terminator terminal emulator
 * Computing Lab (CS69201) · Term Project
 */

#ifndef TERMINATOR_H
#define TERMINATOR_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>

/* ── Constants ─────────────────────────────────────────────────── */

#define MAX_TABS            16
#define MAX_INPUT_LEN       4096
#define MAX_LINE_LEN        4096
#define MAX_TEXT_LINES      10000
#define MAX_HISTORY         10000
#define HISTORY_DISPLAY     1000
#define MAX_JOBS            64
#define MAX_ARGS            256
#define MAX_PIPES           32
#define MAX_PATH_LEN        4096
#define MAX_VAR_NAME        256
#define MAX_VAR_VALUE       4096
#define MAX_VARS            256
#define MAX_COMPLETIONS     256

#define TAB_BAR_HEIGHT      34
#define TAB_WIDTH           190
#define TAB_PADDING         8
#define SCROLL_LINES        3

#define PROMPT_FORMAT       "user@terminator> "
#define CONT_PROMPT         "> "
#define SEARCH_PROMPT       "Enter search term: "

#define HISTORY_FILE_NAME   ".terminator_history"
#define CONFIG_FILE_NAME    ".terminatorrc"

#define MULTIWATCH_INTERVAL 2   /* seconds */

/* ── Hotkey Actions ────────────────────────────────────────────── */

typedef enum {
    HK_MOVE_LEFT = 0,
    HK_MOVE_RIGHT,
    HK_LINE_START,      /* Ctrl+A */
    HK_LINE_END,        /* Ctrl+E */
    HK_HISTORY_UP,
    HK_HISTORY_DOWN,
    HK_DELETE_TO_START,  /* Ctrl+U */
    HK_DELETE_TO_END,    /* Ctrl+K */
    HK_DELETE_WORD,      /* Ctrl+W */
    HK_CLEAR_SCREEN,    /* Ctrl+L */
    HK_EXIT_EOF,        /* Ctrl+D */
    HK_COPY,            /* Ctrl+Shift+C */
    HK_PASTE,           /* Ctrl+Shift+V */
    HK_SEARCH_HISTORY,  /* Ctrl+R */
    HK_AUTOCOMPLETE,    /* Tab */
    HK_INTERRUPT,       /* Ctrl+C */
    HK_SUSPEND,         /* Ctrl+Z */
    HK_NEW_TAB,         /* Ctrl+T */
    HK_CLOSE_TAB,       /* Ctrl+W with shift? or another */
    HK_NEXT_TAB,        /* Ctrl+PageDown */
    HK_PREV_TAB,        /* Ctrl+PageUp */
    HK_ACTION_COUNT
} HotkeyAction;

/* ── Hotkey Binding ────────────────────────────────────────────── */

typedef struct {
    unsigned int modifier;   /* ControlMask, ShiftMask, etc. */
    KeySym       keysym;
    HotkeyAction action;
} HotkeyBinding;

/* ── Configuration ─────────────────────────────────────────────── */

typedef struct {
    HotkeyBinding bindings[HK_ACTION_COUNT];
    int           num_bindings;
    char          history_file[MAX_PATH_LEN];
    int           history_size;
} Config;

/* ── Job Control ───────────────────────────────────────────────── */

typedef enum {
    JOB_RUNNING,
    JOB_STOPPED,
    JOB_DONE,
    JOB_BACKGROUND
} JobState;

typedef struct {
    int       job_id;
    pid_t     pid;
    pid_t     pgid;
    char      command[MAX_LINE_LEN];
    JobState  state;
    int       active;
} Job;

/* ── Shell Variable ────────────────────────────────────────────── */

typedef struct {
    char name[MAX_VAR_NAME];
    char value[MAX_VAR_VALUE];
} ShellVar;

/* ── Parsed Command ────────────────────────────────────────────── */

typedef struct Command {
    char  **args;            /* NULL-terminated argument array */
    int     argc;
    char   *input_file;      /* < redirection */
    char   *output_file;     /* > redirection */
    int     append_output;   /* >> */
    int     background;      /* & at end */
    struct Command *pipe_next; /* next command in pipeline */
} Command;

/* ── Tab ───────────────────────────────────────────────────────── */

typedef struct {
    int    id;
    int    active;

    /* Current working directory */
    char   cwd[MAX_PATH_LEN];

    /* Display text buffer */
    char  *text_lines[MAX_TEXT_LINES];
    int    num_lines;
    int    scroll_offset;

    /* Input buffer */
    char   input[MAX_INPUT_LEN];
    int    input_len;
    int    cursor_pos;

    /* Multiline input accumulator */
    char   multiline_buf[MAX_INPUT_LEN * 8];
    int    multiline_len;
    int    in_multiline;

    /* History browsing */
    int    history_index;       /* -1 = not browsing */
    char   saved_input[MAX_INPUT_LEN]; /* save current input when browsing */

    /* Job control */
    Job    jobs[MAX_JOBS];
    int    num_jobs;
    int    last_exit_status;

    /* Foreground process */
    pid_t  fg_pid;
    pid_t  fg_pgid;

    /* Communication pipes  GUI <--> shell */
    int    gui_to_shell[2];    /* GUI writes commands, shell reads */
    int    shell_to_gui[2];    /* Shell writes output, GUI reads */

    /* Thread / process id */
    pthread_t thread_id;
    pid_t     process_id;

    /* Shell variables */
    ShellVar vars[MAX_VARS];
    int      num_vars;

    /* Script arguments ($1, $2, ...) */
    char   **script_args;
    int      script_argc;

    /* Running flag */
    volatile int running;

    /* Search mode (Ctrl+R) */
    int    search_mode;
    char   search_input[MAX_INPUT_LEN];
    int    search_input_len;

    /* Autocomplete mode */
    int    autocomplete_mode;
    char  *completions[MAX_COMPLETIONS];
    int    num_completions;

    /* multiWatch active flag */
    int    in_multiwatch;
    pid_t  multiwatch_pids[MAX_ARGS];
    int    multiwatch_count;
} Tab;

/* ── Shared History (used in both modes) ───────────────────────── */

typedef struct {
    char entries[MAX_HISTORY][MAX_LINE_LEN];
    int  count;          /* total entries added */
    int  head;           /* next write position (circular) */
    /* Thread mode: pthread mutex; Process mode: process-shared semaphore */
    pthread_mutex_t mutex;
    sem_t           sem;
    int             use_sem;   /* 1 = process mode (use sem), 0 = thread mode (use mutex) */
} SharedHistory;

/* ── Terminal (top-level state) ────────────────────────────────── */

typedef struct {
    /* X11 */
    Display       *display;
    Window         window;
    GC             gc;
    XFontStruct   *font;
    int            screen;
    Colormap       colormap;
    unsigned long  fg_color;
    unsigned long  bg_color;
    unsigned long  tab_bg_color;
    unsigned long  tab_active_color;
    unsigned long  tab_text_color;
    unsigned long  highlight_color;
    unsigned long  cursor_color;
    int            win_width;
    int            win_height;
    int            font_width;
    int            font_height;
    int            cols;
    int            rows;
    Atom           wm_delete;
    Atom           clipboard;
    Atom           utf8_string;

    /* Tabs */
    Tab            tabs[MAX_TABS];
    int            num_tabs;
    int            active_tab;

    /* Execution mode */
    int            use_process_mode;

    /* History */
    SharedHistory *history;

    /* Config */
    Config         config;

    /* Running */
    volatile int   running;

    /* Tab bar */
    int            tab_bar_height;
} Terminal;

/* ── Global terminal pointer (set in main.c) ──────────────────── */

extern Terminal *g_terminal;

#endif /* TERMINATOR_H */
