/*
 * main.c — Entry point for the Terminator terminal emulator
 * Computing Lab (CS69201) · Term Project
 *
 * Usage:
 *   ./terminator                     # thread mode (default)
 *   ./terminator useProcess=True     # process mode
 */

#include "../include/terminator.h"
#include "../include/gui.h"
#include "../include/tab.h"
#include "../include/history.h"
#include "../include/hotkeys.h"
#include "../include/jobs.h"

/* Global terminal pointer — accessible from all modules */
Terminal *g_terminal = NULL;

/* ── Signal handler for clean shutdown ────────────────────────── */

static void handle_sigterm(int sig)
{
    (void)sig;
    if (g_terminal) {
        g_terminal->running = 0;
    }
}

/* ── Parse command-line arguments ─────────────────────────────── */

static int parse_args(int argc, char **argv, int *use_process_mode)
{
    *use_process_mode = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "useProcess=True") == 0 ||
            strcmp(argv[i], "useProcess=true") == 0) {
            *use_process_mode = 1;
        } else {
            fprintf(stderr, "terminator: unknown argument: %s\n", argv[i]);
            fprintf(stderr, "Usage: %s [useProcess=True]\n", argv[0]);
            return -1;
        }
    }

    return 0;
}

/* ── Build path to a file in the user's home directory ────────── */

static void home_path(char *buf, int size, const char *filename)
{
    const char *home = getenv("HOME");
    if (!home) home = ".";
    snprintf(buf, size, "%s/%s", home, filename);
}

/* ── Main ─────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    int use_process_mode = 0;

    /* Parse arguments */
    if (parse_args(argc, argv, &use_process_mode) != 0) {
        return 1;
    }

    /* Allocate the terminal structure on the heap to avoid stack overflow */
    Terminal *term = calloc(1, sizeof(Terminal));
    if (!term) {
        fprintf(stderr, "terminator: out of memory\n");
        return 1;
    }

    term->use_process_mode = use_process_mode;
    term->running = 1;
    term->tab_bar_height = TAB_BAR_HEIGHT;
    term->active_tab = 0;
    term->num_tabs = 0;

    /* Set global pointer */
    g_terminal = term;

    /* Install signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigterm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* Ignore SIGPIPE (broken pipe writes should return errors, not kill us) */
    signal(SIGPIPE, SIG_IGN);

    /* Initialise job-control signals */
    jobs_init_signals();

    /* Load hotkey configuration */
    hotkeys_load_defaults(&term->config);
    {
        char cfg_path[MAX_PATH_LEN];
        home_path(cfg_path, sizeof(cfg_path), CONFIG_FILE_NAME);
        hotkeys_load_config(&term->config, cfg_path);
    }

    /* Initialise shared history */
    term->history = history_init(use_process_mode);
    if (!term->history) {
        fprintf(stderr, "terminator: failed to initialise history\n");
        free(term);
        return 1;
    }

    /* Load history from file */
    {
        char hist_path[MAX_PATH_LEN];
        home_path(hist_path, sizeof(hist_path), HISTORY_FILE_NAME);
        snprintf(term->config.history_file, sizeof(term->config.history_file),
                 "%s", hist_path);
        history_load(term->history, hist_path);
    }

    /* Initialise the X11 GUI */
    if (gui_init(term) != 0) {
        fprintf(stderr, "terminator: failed to initialise GUI\n");
        history_cleanup(term->history, use_process_mode);
        free(term);
        return 1;
    }

    /* Create the first tab */
    if (tab_create(term) < 0) {
        fprintf(stderr, "terminator: failed to create initial tab\n");
        gui_cleanup(term);
        history_cleanup(term->history, use_process_mode);
        free(term);
        return 1;
    }

    printf("Terminator started in %s mode.\n",
           use_process_mode ? "process" : "thread");

    /* Run the main event loop (blocks until exit) */
    gui_run(term);

    /* ── Cleanup ──────────────────────────────────────────────── */

    /* Close all tabs */
    for (int i = 0; i < term->num_tabs; i++) {
        if (term->tabs[i].active) {
            tab_close(term, i);
        }
    }

    /* Save history */
    history_save(term->history, term->config.history_file);

    /* Cleanup */
    history_cleanup(term->history, use_process_mode);
    gui_cleanup(term);
    free(term);

    printf("Terminator exited.\n");
    return 0;
}
