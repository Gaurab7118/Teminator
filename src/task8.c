/*
 * jobs.c — Signal handling and job control (Task 8)
 * Computing Lab (CS69201) · Term Project
 *
 * Manages background/stopped processes per-tab.  Provides signal setup,
 * foreground/background switching, and bookkeeping for job notifications.
 */

#include "jobs.h"
#include "gui.h"

/* ── Signal handler ───────────────────────────────────────────────── */

/*
 * sigchld_handler — Reap zombie children.  We don't do per-tab bookkeeping
 * here because we can't safely access tab structures from a signal handler.
 * Instead we just reap; jobs_update() will discover state changes via
 * waitpid(WNOHANG) on each active job.
 */
static void sigchld_handler(int sig)
{
    (void)sig;
    int saved_errno = errno;

    /* Reap all available children */
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;

    errno = saved_errno;
}

/* ── Public API ───────────────────────────────────────────────────── */

/*
 * jobs_init_signals — Set up signal handlers for the shell.
 *
 * - SIGCHLD: handler with SA_RESTART | SA_NOCLDSTOP so we only get
 *   notified on child termination, not stops.
 * - SIGTTOU, SIGTTIN: ignored (so the shell can manage terminal without
 *   being stopped by its own tc* calls).
 */
void jobs_init_signals(void)
{
    struct sigaction sa;

    /* SIGCHLD */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) < 0)
        perror("jobs_init_signals: SIGCHLD");

    /* Ignore SIGTTOU and SIGTTIN so the shell can manage the terminal */
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
}

/*
 * jobs_next_id — Return the next available job ID for a tab.
 * Simply finds the maximum existing job_id and returns max + 1.
 * If no jobs exist, returns 1.
 */
int jobs_next_id(Tab *tab)
{
    if (!tab)
        return 1;

    int max_id = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (tab->jobs[i].active && tab->jobs[i].job_id > max_id)
            max_id = tab->jobs[i].job_id;
    }
    return max_id + 1;
}

/*
 * jobs_add — Add a new job to the tab's job table.
 * Finds the first empty slot, fills it in, returns the job_id.
 * Returns -1 if the table is full.
 */
int jobs_add(Tab *tab, pid_t pid, pid_t pgid, const char *command, JobState state)
{
    if (!tab)
        return -1;

    /* Find an empty slot */
    int slot = -1;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!tab->jobs[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        fprintf(stderr, "jobs_add: job table full\n");
        return -1;
    }

    int jid = jobs_next_id(tab);

    tab->jobs[slot].job_id = jid;
    tab->jobs[slot].pid    = pid;
    tab->jobs[slot].pgid   = (pgid > 0) ? pgid : pid;
    tab->jobs[slot].state  = state;
    tab->jobs[slot].active = 1;

    /* Copy command string safely */
    strncpy(tab->jobs[slot].command, command ? command : "", MAX_LINE_LEN - 1);
    tab->jobs[slot].command[MAX_LINE_LEN - 1] = '\0';

    tab->num_jobs++;
    return jid;
}

/*
 * jobs_remove — Mark a job as inactive.
 */
void jobs_remove(Tab *tab, int job_id)
{
    if (!tab)
        return;

    for (int i = 0; i < MAX_JOBS; i++) {
        if (tab->jobs[i].active && tab->jobs[i].job_id == job_id) {
            tab->jobs[i].active = 0;
            tab->jobs[i].job_id = 0;
            tab->jobs[i].pid    = 0;
            tab->jobs[i].pgid   = 0;
            tab->jobs[i].command[0] = '\0';
            tab->num_jobs--;
            if (tab->num_jobs < 0)
                tab->num_jobs = 0;
            return;
        }
    }
}

/*
 * jobs_find — Find a job by job_id.  Returns pointer or NULL.
 */
Job *jobs_find(Tab *tab, int job_id)
{
    if (!tab || job_id <= 0)
        return NULL;

    for (int i = 0; i < MAX_JOBS; i++) {
        if (tab->jobs[i].active && tab->jobs[i].job_id == job_id)
            return &tab->jobs[i];
    }
    return NULL;
}

/*
 * jobs_find_by_pid — Find a job by PID.  Returns pointer or NULL.
 */
Job *jobs_find_by_pid(Tab *tab, pid_t pid)
{
    if (!tab || pid <= 0)
        return NULL;

    for (int i = 0; i < MAX_JOBS; i++) {
        if (tab->jobs[i].active && tab->jobs[i].pid == pid)
            return &tab->jobs[i];
    }
    return NULL;
}

/*
 * jobs_update — Poll all active jobs with waitpid(WNOHANG) and update
 * their states.  Does not print notifications (use jobs_notify_done
 * for that).
 */
void jobs_update(Tab *tab)
{
    if (!tab)
        return;

    for (int i = 0; i < MAX_JOBS; i++) {
        if (!tab->jobs[i].active)
            continue;

        int status;
        pid_t ret = waitpid(tab->jobs[i].pid, &status, WNOHANG | WUNTRACED | WCONTINUED);

        if (ret == 0) {
            /* No change */
            continue;
        }

        if (ret < 0) {
            if (errno == ECHILD) {
                /* Process already reaped — mark done */
                tab->jobs[i].state = JOB_DONE;
            }
            continue;
        }

        /* ret == tab->jobs[i].pid: state changed */
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            tab->jobs[i].state = JOB_DONE;
        } else if (WIFSTOPPED(status)) {
            tab->jobs[i].state = JOB_STOPPED;
        }
#ifdef WCONTINUED
        else if (WIFCONTINUED(status)) {
            /* If it was stopped, now it's running/background again */
            if (tab->jobs[i].state == JOB_STOPPED)
                tab->jobs[i].state = JOB_BACKGROUND;
        }
#endif
    }
}

/*
 * job_state_string — Return a printable string for a job state.
 */
static const char *job_state_string(JobState state)
{
    switch (state) {
    case JOB_RUNNING:    return "Running";
    case JOB_STOPPED:    return "Stopped";
    case JOB_DONE:       return "Done";
    case JOB_BACKGROUND: return "Running";
    default:             return "Unknown";
    }
}

/*
 * jobs_print — Print the job table in a format similar to bash:
 *   [N]+  Running/Stopped   command
 *
 * The most recently created active job gets '+', the second-most gets '-'.
 */
void jobs_print(Terminal *term, Tab *tab)
{
    if (!term || !tab)
        return;

    /* Find the two highest job_ids for +/- annotation */
    int highest = 0, second = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!tab->jobs[i].active)
            continue;
        if (tab->jobs[i].job_id > highest) {
            second = highest;
            highest = tab->jobs[i].job_id;
        } else if (tab->jobs[i].job_id > second) {
            second = tab->jobs[i].job_id;
        }
    }

    /* Print in job_id order */
    for (int jid = 1; jid <= highest; jid++) {
        Job *j = jobs_find(tab, jid);
        if (!j)
            continue;

        char marker = ' ';
        if (jid == highest)
            marker = '+';
        else if (jid == second)
            marker = '-';

        char buf[MAX_LINE_LEN + 64];
        snprintf(buf, sizeof(buf), "[%d]%c  %-12s %s",
                 j->job_id, marker, job_state_string(j->state), j->command);
        gui_append_text(tab, buf);
    }
}

/*
 * jobs_fg — Bring a job to the foreground.
 *
 * 1. If the job is stopped, send SIGCONT.
 * 2. Set tab->fg_pid and tab->fg_pgid.
 * 3. Wait for the process to finish or be stopped.
 * 4. Update the job state accordingly.
 *
 * Returns 0 on success, -1 on error.
 */
int jobs_fg(Terminal *term, Tab *tab, int job_id)
{
    if (!term || !tab)
        return -1;

    Job *job = jobs_find(tab, job_id);
    if (!job) {
        gui_append_text(tab, "fg: no such job");
        return -1;
    }

    /* Print the command being foregrounded */
    gui_append_text(tab, job->command);

    /* If stopped, continue it */
    if (job->state == JOB_STOPPED) {
        if (kill(-job->pgid, SIGCONT) < 0 && errno != ESRCH) {
            perror("jobs_fg: SIGCONT");
            return -1;
        }
    }

    /* Set as foreground process */
    tab->fg_pid  = job->pid;
    tab->fg_pgid = job->pgid;
    job->state   = JOB_RUNNING;

    /* Wait for the process */
    int status;
    pid_t ret;
    do {
        ret = waitpid(job->pid, &status, WUNTRACED);
    } while (ret < 0 && errno == EINTR);

    /* Clear foreground */
    tab->fg_pid  = 0;
    tab->fg_pgid = 0;

    if (ret > 0) {
        if (WIFEXITED(status)) {
            tab->last_exit_status = WEXITSTATUS(status);
            job->state = JOB_DONE;
            jobs_remove(tab, job_id);
        } else if (WIFSIGNALED(status)) {
            tab->last_exit_status = 128 + WTERMSIG(status);
            job->state = JOB_DONE;
            jobs_remove(tab, job_id);
        } else if (WIFSTOPPED(status)) {
            job->state = JOB_STOPPED;
            char buf[MAX_LINE_LEN + 64];
            snprintf(buf, sizeof(buf), "[%d]+  Stopped   %s",
                     job->job_id, job->command);
            gui_append_text(tab, buf);
        }
    }

    return 0;
}

/*
 * jobs_bg — Send a job to the background.
 * If the job is stopped, sends SIGCONT and sets state to JOB_BACKGROUND.
 * Returns 0 on success, -1 on error.
 */
int jobs_bg(Terminal *term, Tab *tab, int job_id)
{
    if (!term || !tab)
        return -1;

    Job *job = jobs_find(tab, job_id);
    if (!job) {
        gui_append_text(tab, "bg: no such job");
        return -1;
    }

    if (job->state == JOB_STOPPED || job->state == JOB_RUNNING) {
        if (kill(-job->pgid, SIGCONT) < 0 && errno != ESRCH) {
            perror("jobs_bg: SIGCONT");
            return -1;
        }
        job->state = JOB_BACKGROUND;

        char buf[MAX_LINE_LEN + 64];
        snprintf(buf, sizeof(buf), "[%d]+  %s &", job->job_id, job->command);
        gui_append_text(tab, buf);
    }

    return 0;
}

/*
 * jobs_interrupt — Send SIGINT to the foreground process group.
 */
void jobs_interrupt(Tab *tab)
{
    if (!tab)
        return;

    if (tab->fg_pgid > 0) {
        kill(-tab->fg_pgid, SIGINT);
    } else if (tab->fg_pid > 0) {
        kill(tab->fg_pid, SIGINT);
    }
}

/*
 * jobs_suspend — Send SIGTSTP to the foreground process group, then
 * add the process as a stopped job.
 */
void jobs_suspend(Tab *tab)
{
    if (!tab)
        return;

    if (tab->fg_pgid > 0) {
        kill(-tab->fg_pgid, SIGTSTP);
    } else if (tab->fg_pid > 0) {
        kill(tab->fg_pid, SIGTSTP);
    }

    /* The actual job addition happens after waitpid detects WIFSTOPPED
     * in the shell's main loop.  We just send the signal here. */
}

/*
 * jobs_parse_spec — Parse a job specifier string.
 * Supported formats: %N, %%, %+, %-
 *
 *  %N → job N
 *  %%, %+ → most recent job (not implemented here; caller can handle)
 *  %- → second most recent
 *
 * Returns the job_id (>= 1), or -1 on parse error.
 */
int jobs_parse_spec(const char *spec)
{
    if (!spec || spec[0] != '%')
        return -1;

    if (spec[1] == '\0')
        return -1;

    /* %N */
    if (isdigit((unsigned char)spec[1])) {
        int n = atoi(spec + 1);
        return (n > 0) ? n : -1;
    }

    /* %% or %+ means "current job" — return 0 as sentinel for "current" */
    if (spec[1] == '%' || spec[1] == '+')
        return 0;  /* Caller should map 0 → highest job_id */

    /* %- means "previous job" — return -2 as sentinel */
    if (spec[1] == '-')
        return -2;  /* Caller should map to second-highest */

    return -1;
}

/*
 * jobs_notify_done — Scan for background jobs that have completed
 * (state == JOB_DONE).  Print a notification for each and remove them.
 */
void jobs_notify_done(Terminal *term, Tab *tab)
{
    if (!term || !tab)
        return;

    /* First, update statuses */
    jobs_update(tab);

    /* Then notify and remove done jobs */
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!tab->jobs[i].active)
            continue;

        if (tab->jobs[i].state == JOB_DONE) {
            char buf[MAX_LINE_LEN + 64];
            snprintf(buf, sizeof(buf), "[%d]+  Done   %s",
                     tab->jobs[i].job_id, tab->jobs[i].command);
            gui_append_text(tab, buf);
            jobs_remove(tab, tab->jobs[i].job_id);
        }
    }
}
