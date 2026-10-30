/*
 * history.c — Command history management (Task 9)
 * Computing Lab (CS69201) · Term Project
 *
 * Implements a circular buffer of command history entries with thread-safe
 * and process-safe locking.  Supports file persistence, 3-tier search
 * (exact → longest common substring → no match), and display.
 */

#include "history.h"
#include "gui.h"

/* ── Internal helpers ─────────────────────────────────────────────── */

/*
 * lcs_length — Compute length of longest common substring between a and b
 * using dynamic programming with a rolling two-row table.
 * Returns the length of the longest common substring.
 */
static int lcs_length(const char *a, const char *b)
{
    int la = (int)strlen(a);
    int lb = (int)strlen(b);
    if (la == 0 || lb == 0)
        return 0;

    /*
     * To save memory we only keep two rows at a time.
     * dp[cur][j] = length of common substring ending at a[i-1], b[j-1].
     */
    int *prev = calloc((size_t)(lb + 1), sizeof(int));
    int *curr = calloc((size_t)(lb + 1), sizeof(int));
    if (!prev || !curr) {
        free(prev);
        free(curr);
        return 0;
    }

    int best = 0;
    for (int i = 1; i <= la; i++) {
        for (int j = 1; j <= lb; j++) {
            if (a[i - 1] == b[j - 1]) {
                curr[j] = prev[j - 1] + 1;
                if (curr[j] > best)
                    best = curr[j];
            } else {
                curr[j] = 0;
            }
        }
        /* Swap rows */
        int *tmp = prev;
        prev = curr;
        curr = tmp;
        memset(curr, 0, (size_t)(lb + 1) * sizeof(int));
    }

    free(prev);
    free(curr);
    return best;
}

/*
 * history_effective_count — Return the actual number of stored entries.
 * This is min(hist->count, MAX_HISTORY).
 */
static int history_effective_count(SharedHistory *hist)
{
    return (hist->count < MAX_HISTORY) ? hist->count : MAX_HISTORY;
}

/*
 * oldest_index — Return the index of the oldest entry in the circular buffer.
 */
static int oldest_index(SharedHistory *hist)
{
    int n = history_effective_count(hist);
    if (n < MAX_HISTORY)
        return 0;  /* Buffer hasn't wrapped yet */
    return hist->head;  /* head points to next-write, which is the oldest */
}

/* ── Public API ───────────────────────────────────────────────────── */

/*
 * history_init — Allocate and initialise SharedHistory.
 *
 * process_mode = 0: allocate with malloc, use pthread mutex.
 * process_mode = 1: allocate with mmap(MAP_SHARED|MAP_ANONYMOUS),
 *                   init mutex as PTHREAD_PROCESS_SHARED, init semaphore.
 */
SharedHistory *history_init(int process_mode)
{
    SharedHistory *hist = NULL;

    if (process_mode) {
        /* Process mode — shared memory so forked children inherit it */
        hist = mmap(NULL, sizeof(SharedHistory),
                    PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (hist == MAP_FAILED) {
            perror("history_init: mmap");
            return NULL;
        }
    } else {
        /* Thread mode — normal heap allocation */
        hist = calloc(1, sizeof(SharedHistory));
        if (!hist) {
            perror("history_init: calloc");
            return NULL;
        }
    }

    /* Zero out the structure */
    memset(hist, 0, sizeof(SharedHistory));

    if (process_mode) {
        /* Process-shared mutex */
        pthread_mutexattr_t mattr;
        pthread_mutexattr_init(&mattr);
        pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
        if (pthread_mutex_init(&hist->mutex, &mattr) != 0) {
            perror("history_init: pthread_mutex_init");
            munmap(hist, sizeof(SharedHistory));
            pthread_mutexattr_destroy(&mattr);
            return NULL;
        }
        pthread_mutexattr_destroy(&mattr);

        /* Process-shared semaphore (binary, initial value 1) */
        if (sem_init(&hist->sem, 1, 1) != 0) {
            perror("history_init: sem_init");
            pthread_mutex_destroy(&hist->mutex);
            munmap(hist, sizeof(SharedHistory));
            return NULL;
        }
        hist->use_sem = 1;
    } else {
        /* Default mutex attributes (not process-shared) */
        if (pthread_mutex_init(&hist->mutex, NULL) != 0) {
            perror("history_init: pthread_mutex_init");
            free(hist);
            return NULL;
        }
        hist->use_sem = 0;
    }

    hist->count = 0;
    hist->head  = 0;

    return hist;
}

/*
 * history_cleanup — Destroy synchronisation primitives and free memory.
 */
void history_cleanup(SharedHistory *hist, int process_mode)
{
    if (!hist)
        return;

    pthread_mutex_destroy(&hist->mutex);

    if (process_mode) {
        sem_destroy(&hist->sem);
        munmap(hist, sizeof(SharedHistory));
    } else {
        free(hist);
    }
}

/*
 * history_lock — Acquire exclusive access to the history buffer.
 * Uses semaphore in process mode, mutex in thread mode.
 */
void history_lock(SharedHistory *hist)
{
    if (!hist)
        return;

    if (hist->use_sem) {
        while (sem_wait(&hist->sem) == -1) {
            if (errno != EINTR) {
                perror("history_lock: sem_wait");
                return;
            }
            /* Retry on EINTR */
        }
    } else {
        pthread_mutex_lock(&hist->mutex);
    }
}

/*
 * history_unlock — Release exclusive access.
 */
void history_unlock(SharedHistory *hist)
{
    if (!hist)
        return;

    if (hist->use_sem) {
        sem_post(&hist->sem);
    } else {
        pthread_mutex_unlock(&hist->mutex);
    }
}

/*
 * history_add — Append a command to the circular buffer.
 * Empty or whitespace-only strings are silently ignored.
 * Duplicate of the most recent entry is also skipped.
 */
void history_add(SharedHistory *hist, const char *command)
{
    if (!hist || !command)
        return;

    /* Skip empty or whitespace-only commands */
    const char *p = command;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p == '\0')
        return;

    history_lock(hist);

    /* Skip duplicate of the most recent entry */
    if (hist->count > 0) {
        int prev = (hist->head - 1 + MAX_HISTORY) % MAX_HISTORY;
        if (strcmp(hist->entries[prev], command) == 0) {
            history_unlock(hist);
            return;
        }
    }

    /* Write into the current head slot */
    strncpy(hist->entries[hist->head], command, MAX_LINE_LEN - 1);
    hist->entries[hist->head][MAX_LINE_LEN - 1] = '\0';

    /* Advance head (circular) */
    hist->head = (hist->head + 1) % MAX_HISTORY;
    hist->count++;

    history_unlock(hist);
}

/*
 * history_get — Retrieve the i-th most recent command (0 = most recent).
 * Returns a pointer into the buffer (valid until the next write wraps
 * around to that slot).  Returns NULL if index is out of range.
 */
const char *history_get(SharedHistory *hist, int index)
{
    if (!hist || index < 0)
        return NULL;

    history_lock(hist);

    int n = history_effective_count(hist);
    if (index >= n) {
        history_unlock(hist);
        return NULL;
    }

    /* head-1 is the most recent; head-1-index wraps backward */
    int pos = (hist->head - 1 - index + MAX_HISTORY * 2) % MAX_HISTORY;
    const char *result = hist->entries[pos];

    history_unlock(hist);
    return result;
}

/*
 * history_count — Return the number of stored entries.
 */
int history_count(SharedHistory *hist)
{
    if (!hist)
        return 0;
    return history_effective_count(hist);
}

/*
 * history_load — Read commands from a file, one per line.
 * Returns 0 on success, -1 on error (file not found is not an error:
 * we simply start with an empty history).
 */
int history_load(SharedHistory *hist, const char *path)
{
    if (!hist || !path)
        return -1;

    FILE *fp = fopen(path, "r");
    if (!fp) {
        /* Non-existent file is OK — first run */
        if (errno == ENOENT)
            return 0;
        perror("history_load: fopen");
        return -1;
    }

    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), fp)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (len > 0)
            history_add(hist, line);
    }

    fclose(fp);
    return 0;
}

/*
 * history_save — Write all stored entries to a file, oldest first.
 * Returns 0 on success, -1 on error.
 */
int history_save(SharedHistory *hist, const char *path)
{
    if (!hist || !path)
        return -1;

    FILE *fp = fopen(path, "w");
    if (!fp) {
        perror("history_save: fopen");
        return -1;
    }

    history_lock(hist);

    int n = history_effective_count(hist);
    int start = oldest_index(hist);

    for (int i = 0; i < n; i++) {
        int idx = (start + i) % MAX_HISTORY;
        fprintf(fp, "%s\n", hist->entries[idx]);
    }

    history_unlock(hist);
    fclose(fp);
    return 0;
}

/*
 * history_search — 3-tier search:
 *
 *  (a) Exact match: find the most recent entry that equals term exactly.
 *      If found, return 1 result.
 *  (b) Longest common substring > 2 chars: find all entries that share
 *      the longest common substring with term (using DP).
 *      Return those entries (up to max_results).
 *  (c) No match at all: return 0.
 *
 * results[]: filled with pointers into the history buffer.
 * Returns the number of matches placed in results[].
 */
int history_search(SharedHistory *hist, const char *term,
                   const char **results, int max_results)
{
    if (!hist || !term || !results || max_results <= 0)
        return 0;

    history_lock(hist);

    int n = history_effective_count(hist);
    if (n <= 0 || n > MAX_HISTORY) {
        history_unlock(hist);
        return 0;
    }

    /* ── (a) Exact match — most recent first ────────────────────── */
    for (int i = 0; i < n; i++) {
        int pos = (hist->head - 1 - i + MAX_HISTORY * 2) % MAX_HISTORY;
        if (strcmp(hist->entries[pos], term) == 0) {
            results[0] = hist->entries[pos];
            history_unlock(hist);
            return 1;
        }
    }

    /* ── (b) Longest common substring > 2 chars ─────────────────── */

    /* First pass: compute the maximum LCS length across all entries */
    int best_lcs = 0;

    /*
     * We need per-entry LCS lengths for the second pass.
     * Use a stack-allocated array if possible (MAX_HISTORY can be large,
     * so heap-allocate).
     */
    int *lcs_vals = calloc((size_t)n, sizeof(int));
    if (!lcs_vals) {
        history_unlock(hist);
        return 0;
    }

    for (int i = 0; i < n; i++) {
        int pos = (hist->head - 1 - i + MAX_HISTORY * 2) % MAX_HISTORY;
        lcs_vals[i] = lcs_length(hist->entries[pos], term);
        if (lcs_vals[i] > best_lcs)
            best_lcs = lcs_vals[i];
    }

    if (best_lcs <= 2) {
        /* No meaningful common substring */
        free(lcs_vals);
        history_unlock(hist);
        return 0;  /* tier (c) */
    }

    /* Second pass: collect entries whose LCS == best_lcs (most recent first) */
    int count = 0;
    for (int i = 0; i < n && count < max_results; i++) {
        if (lcs_vals[i] == best_lcs) {
            int pos = (hist->head - 1 - i + MAX_HISTORY * 2) % MAX_HISTORY;
            results[count++] = hist->entries[pos];
        }
    }

    free(lcs_vals);
    history_unlock(hist);
    return count;
}

/*
 * history_print — Print the most recent HISTORY_DISPLAY entries to the
 * active tab, each prefixed with a line number (oldest first).
 */
void history_print(Terminal *term, Tab *tab)
{
    if (!term || !tab || !term->history)
        return;

    SharedHistory *hist = term->history;
    history_lock(hist);

    int n = history_effective_count(hist);
    int display = (n < HISTORY_DISPLAY) ? n : HISTORY_DISPLAY;

    /* We want to print oldest-first.  The most recent entry is at
     * index 0 (via history_get semantics), so oldest of the display
     * batch is at index display-1. */
    for (int i = display - 1; i >= 0; i--) {
        int pos = (hist->head - 1 - i + MAX_HISTORY * 2) % MAX_HISTORY;
        char buf[MAX_LINE_LEN + 32];
        snprintf(buf, sizeof(buf), " %5d  %s", display - i, hist->entries[pos]);
        gui_append_text(tab, buf);
    }

    history_unlock(hist);
}
