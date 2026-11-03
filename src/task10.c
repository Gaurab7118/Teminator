/*
 * autocomplete.c — File-name auto-completion (Task 10)
 * Computing Lab (CS69201) · Term Project
 *
 * Implements tab-completion for file and directory names.  When the user
 * presses Tab the current word is extracted, matched against directory
 * entries, and either completed in-place (single match), extended to the
 * longest common prefix (ambiguous), or listed with numbered options.
 */

#include "autocomplete.h"
#include "gui.h"

/* ── Internal helpers ─────────────────────────────────────────────── */

/*
 * extract_word — Walk backwards from cursor_pos in input to find the
 * start of the current "word" (token delimited by spaces).
 * Sets *word_start to the index of the first character of the word.
 * Returns a pointer to the word (a NUL-terminated copy in 'buf').
 */
static int extract_word(const char *input, int cursor_pos, char *buf,
                        int bufsize, int *word_start)
{
    int start = cursor_pos;

    /* Walk backwards over non-space characters */
    while (start > 0 && input[start - 1] != ' ')
        start--;

    int len = cursor_pos - start;
    if (len >= bufsize)
        len = bufsize - 1;

    memcpy(buf, input + start, (size_t)len);
    buf[len] = '\0';
    *word_start = start;
    return len;
}

/*
 * split_dir_prefix — Given a word like "src/ma", split it into
 * directory part ("src/") and prefix part ("ma").
 * dir_out  receives the directory to scan (or "." if no slash).
 * pref_out receives the filename prefix.
 */
static void split_dir_prefix(const char *word, const char *cwd,
                             char *dir_out, int dir_size,
                             char *pref_out, int pref_size)
{
    /* Find the last path separator */
    const char *last_slash = strrchr(word, '/');

    if (last_slash) {
        /* Everything up to and including the slash is the directory */
        int dir_len = (int)(last_slash - word) + 1;
        if (dir_len >= dir_size)
            dir_len = dir_size - 1;
        memcpy(dir_out, word, (size_t)dir_len);
        dir_out[dir_len] = '\0';

        /* If the directory part is not absolute, prepend cwd */
        if (dir_out[0] != '/') {
            char tmp[MAX_PATH_LEN];
            snprintf(tmp, sizeof(tmp), "%s/%s", cwd, dir_out);
            strncpy(dir_out, tmp, (size_t)(dir_size - 1));
            dir_out[dir_size - 1] = '\0';
        }

        /* Prefix is everything after the last slash */
        const char *p = last_slash + 1;
        int plen = (int)strlen(p);
        if (plen >= pref_size)
            plen = pref_size - 1;
        memcpy(pref_out, p, (size_t)plen);
        pref_out[plen] = '\0';
    } else {
        /* No slash — scan cwd, whole word is the prefix */
        strncpy(dir_out, cwd, (size_t)(dir_size - 1));
        dir_out[dir_size - 1] = '\0';

        int plen = (int)strlen(word);
        if (plen >= pref_size)
            plen = pref_size - 1;
        memcpy(pref_out, word, (size_t)plen);
        pref_out[plen] = '\0';
    }
}

/*
 * is_directory — Return 1 if the given path is a directory.
 */
static int is_directory(const char *dir, const char *name)
{
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%.2048s/%.2040s", dir, name);

    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return 1;
    return 0;
}

/*
 * insert_completion — Replace the current word in the input buffer with
 * the completed text.  Adjusts cursor_pos and input_len.
 *
 * word_start: index in input[] where the word begins.
 * original_word: the raw word the user typed (may include dir prefix).
 * completion: the filename to insert (just the basename).
 * dir_in_word: the directory prefix the user typed (e.g. "src/"), or "".
 * scan_dir: the actual directory we scanned (used to check if completion is a dir).
 */
static void insert_completion(Tab *tab, int word_start,
                              const char *original_word,
                              const char *completion,
                              const char *dir_in_word,
                              const char *scan_dir)
{
    (void)dir_in_word;
    /* Build the full replacement: dir_in_word + completion [+ '/'] */
    char full[MAX_INPUT_LEN];
    const char *slash_in_word = strrchr(original_word, '/');
    if (slash_in_word) {
        /* Keep the user's directory prefix exactly as typed */
        int dlen = (int)(slash_in_word - original_word) + 1;
        if (dlen >= (int)sizeof(full) - 1)
            dlen = (int)sizeof(full) - 2;
        memcpy(full, original_word, (size_t)dlen);
        full[dlen] = '\0';
        strncat(full, completion, sizeof(full) - strlen(full) - 1);
    } else {
        strncpy(full, completion, sizeof(full) - 1);
        full[sizeof(full) - 1] = '\0';
    }

    /* Append trailing '/' if the completion is a directory */
    if (is_directory(scan_dir, completion)) {
        size_t flen = strlen(full);
        if (flen < sizeof(full) - 1) {
            full[flen] = '/';
            full[flen + 1] = '\0';
        }
    } else {
        /* Append a space after a completed file for convenience */
        size_t flen = strlen(full);
        if (flen < sizeof(full) - 1) {
            full[flen] = ' ';
            full[flen + 1] = '\0';
        }
    }

    /* Replace [word_start .. cursor_pos) with full[] */
    (void)original_word;
    int new_word_len = (int)strlen(full);
    int tail_len = tab->input_len - tab->cursor_pos;

    /* Check bounds */
    if (word_start + new_word_len + tail_len >= MAX_INPUT_LEN)
        return;

    /* Shift the tail */
    memmove(tab->input + word_start + new_word_len,
            tab->input + tab->cursor_pos,
            (size_t)tail_len);

    /* Copy in the completion */
    memcpy(tab->input + word_start, full, (size_t)new_word_len);

    tab->input_len = word_start + new_word_len + tail_len;
    tab->input[tab->input_len] = '\0';
    tab->cursor_pos = word_start + new_word_len;
}

/* ── Public API ───────────────────────────────────────────────────── */

/*
 * autocomplete_find_matches — Scan directory 'dir' and return filenames
 * that start with 'prefix'.  Entries "." and ".." are skipped unless
 * the prefix starts with '.'.
 * Returns the number of matches placed in results[].
 */
int autocomplete_find_matches(const char *dir, const char *prefix,
                              char **results, int max_results)
{
    if (!dir || !prefix || !results || max_results <= 0)
        return 0;

    DIR *dp = opendir(dir);
    if (!dp)
        return 0;

    int prefix_len = (int)strlen(prefix);
    int count = 0;
    struct dirent *ent;

    while ((ent = readdir(dp)) != NULL && count < max_results) {
        const char *name = ent->d_name;

        /* Skip "." and ".." unless user explicitly typed a dot prefix */
        if (name[0] == '.' && prefix_len == 0)
            continue;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;

        /* Check prefix match */
        if (strncmp(name, prefix, (size_t)prefix_len) == 0) {
            results[count] = strdup(name);
            if (!results[count])
                break;  /* OOM */
            count++;
        }
    }

    closedir(dp);
    return count;
}

/*
 * autocomplete_longest_prefix — Find the longest common prefix among
 * an array of strings.  Returns a malloc'd string (caller frees).
 */
char *autocomplete_longest_prefix(char **strings, int count)
{
    if (!strings || count <= 0)
        return strdup("");

    if (count == 1)
        return strdup(strings[0]);

    /* Start with the full first string, shorten as needed */
    int prefix_len = (int)strlen(strings[0]);

    for (int i = 1; i < count; i++) {
        int j = 0;
        while (j < prefix_len && strings[i][j] != '\0' &&
               strings[0][j] == strings[i][j]) {
            j++;
        }
        prefix_len = j;
        if (prefix_len == 0)
            break;
    }

    char *result = malloc((size_t)(prefix_len + 1));
    if (!result)
        return strdup("");
    memcpy(result, strings[0], (size_t)prefix_len);
    result[prefix_len] = '\0';
    return result;
}

/*
 * autocomplete_free_matches — Free an array of strdup'd match strings.
 */
void autocomplete_free_matches(char **results, int count)
{
    if (!results)
        return;
    for (int i = 0; i < count; i++) {
        free(results[i]);
        results[i] = NULL;
    }
}

/*
 * autocomplete_complete — Main tab-completion entry point.
 *
 * 1. Extract the word under the cursor.
 * 2. Determine the directory to scan and the filename prefix.
 * 3. Find matching entries.
 * 4. If 1 match: complete in-place.
 *    If multiple: extend to longest common prefix; if still ambiguous,
 *    display a numbered list and enter autocomplete mode.
 *    If 0 matches: do nothing.
 *
 * Returns:  0 = completed,  1 = ambiguous (list shown),  -1 = no match.
 */
int autocomplete_complete(Terminal *term, Tab *tab)
{
    if (!term || !tab)
        return -1;

    /* Free any previous completions */
    if (tab->num_completions > 0) {
        autocomplete_free_matches(tab->completions, tab->num_completions);
        tab->num_completions = 0;
    }
    tab->autocomplete_mode = 0;

    /* Extract the current word */
    char word[MAX_INPUT_LEN];
    int word_start = 0;
    int wlen = extract_word(tab->input, tab->cursor_pos, word,
                            sizeof(word), &word_start);
    (void)wlen;

    /* Determine directory and prefix */
    char scan_dir[MAX_PATH_LEN];
    char prefix[MAX_INPUT_LEN];
    split_dir_prefix(word, tab->cwd, scan_dir, sizeof(scan_dir),
                     prefix, sizeof(prefix));

    /* Find matches */
    char *matches[MAX_COMPLETIONS];
    int nmatches = autocomplete_find_matches(scan_dir, prefix,
                                             matches, MAX_COMPLETIONS);

    if (nmatches == 0) {
        return -1;  /* No matches */
    }

    if (nmatches == 1) {
        /* Single match — complete in-place */
        insert_completion(tab, word_start, word, matches[0], "", scan_dir);
        autocomplete_free_matches(matches, nmatches);
        return 0;
    }

    /* Multiple matches — try to extend to longest common prefix */
    char *lcp = autocomplete_longest_prefix(matches, nmatches);

    if (lcp && (int)strlen(lcp) > (int)strlen(prefix)) {
        /* We can extend the prefix further */
        insert_completion(tab, word_start, word, lcp, "", scan_dir);

        /* Check if this fully resolves it by re-scanning */
        /* (lcp might equal one of the matches exactly) */
        int still_ambiguous = 0;
        if (nmatches > 1) {
            /* If lcp matches one entry exactly, it's still ambiguous
             * because there are other entries starting with lcp too.
             * Show the list. */
            still_ambiguous = 1;
        }

        if (!still_ambiguous) {
            free(lcp);
            autocomplete_free_matches(matches, nmatches);
            return 0;
        }
    }
    free(lcp);

    /* Still ambiguous — show numbered list and enter autocomplete mode */
    gui_append_text(tab, "");  /* blank line separator */

    for (int i = 0; i < nmatches; i++) {
        char line[MAX_LINE_LEN];
        /* Mark directories with trailing / */
        const char *suffix = is_directory(scan_dir, matches[i]) ? "/" : "";
        snprintf(line, sizeof(line), "  [%d] %s%s", i + 1, matches[i], suffix);
        gui_append_text(tab, line);

        /* Store in tab for later selection */
        if (i < MAX_COMPLETIONS) {
            tab->completions[i] = matches[i];  /* Transfer ownership */
            matches[i] = NULL;  /* Don't double-free */
        }
    }
    tab->num_completions = (nmatches < MAX_COMPLETIONS) ? nmatches : MAX_COMPLETIONS;
    tab->autocomplete_mode = 1;

    /* Free any matches that didn't fit */
    autocomplete_free_matches(matches, nmatches);

    return 1;  /* Ambiguous */
}

/*
 * autocomplete_select — Handle the user pressing a number key to pick
 * a completion from the numbered list.
 * choice is 1-based.
 */
void autocomplete_select(Terminal *term, Tab *tab, int choice)
{
    if (!term || !tab)
        return;

    if (choice < 1 || choice > tab->num_completions) {
        gui_append_text(tab, "Invalid completion number.");
        tab->autocomplete_mode = 0;
        autocomplete_free_matches(tab->completions, tab->num_completions);
        tab->num_completions = 0;
        return;
    }

    /* Extract current word again so we can replace it */
    char word[MAX_INPUT_LEN];
    int word_start = 0;
    extract_word(tab->input, tab->cursor_pos, word,
                 sizeof(word), &word_start);

    /* Determine scan directory again for is_directory check */
    char scan_dir[MAX_PATH_LEN];
    char prefix[MAX_INPUT_LEN];
    split_dir_prefix(word, tab->cwd, scan_dir, sizeof(scan_dir),
                     prefix, sizeof(prefix));

    /* Insert the chosen completion */
    const char *chosen = tab->completions[choice - 1];
    if (chosen) {
        insert_completion(tab, word_start, word, chosen, "", scan_dir);
    }

    /* Clean up */
    tab->autocomplete_mode = 0;
    autocomplete_free_matches(tab->completions, tab->num_completions);
    tab->num_completions = 0;
}
