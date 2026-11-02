/*
 * jobs.h — Signal handling and job control (Task 8)
 */

#ifndef JOBS_H
#define JOBS_H

#include "terminator.h"

/* Initialise signal handlers for the shell */
void jobs_init_signals(void);

/* Add a job to the tab's job list. Returns job_id. */
int  jobs_add(Tab *tab, pid_t pid, pid_t pgid, const char *command, JobState state);

/* Remove a job from the tab's job list */
void jobs_remove(Tab *tab, int job_id);

/* Find a job by job_id. Returns pointer or NULL. */
Job *jobs_find(Tab *tab, int job_id);

/* Find a job by pid. Returns pointer or NULL. */
Job *jobs_find_by_pid(Tab *tab, pid_t pid);

/* Update job status (check for SIGCHLD, waitpid WNOHANG). */
void jobs_update(Tab *tab);

/* Print the jobs list to the tab */
void jobs_print(Terminal *term, Tab *tab);

/* Bring a job to the foreground */
int  jobs_fg(Terminal *term, Tab *tab, int job_id);

/* Send a job to the background */
int  jobs_bg(Terminal *term, Tab *tab, int job_id);

/* Send SIGINT to the foreground process group */
void jobs_interrupt(Tab *tab);

/* Send SIGTSTP to the foreground process group */
void jobs_suspend(Tab *tab);

/* Parse a job specifier (%1, %2, etc.). Returns job_id or -1. */
int  jobs_parse_spec(const char *spec);

/* Allocate the next available job id */
int  jobs_next_id(Tab *tab);

/* Notify about completed background jobs */
void jobs_notify_done(Terminal *term, Tab *tab);

#endif /* JOBS_H */
