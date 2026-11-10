/* -*- Mode: C; c-basic-offset: 4; tab-width: 4 -*-
 *
 * rclone-job-concurrency-test.c: two transfers must run independently.
 *
 * This is the regression guard for the "progress is single-process: start a
 * second copy and the first one disappears" bug.  It starts two real rclone
 * jobs at the same time and asserts that the job engine keeps them separate:
 * distinct ids, distinct RC sockets, both reach 100 % on their own, and the
 * manager observes both running concurrently (count_running == 2).
 *
 * Uses only local files, so no remote configuration is required.  Isolated
 * from the real user state via NEMO_RCLONE_RUNTIME_DIR / _CACHE_DIR.
 */

#include <config.h>

#include <gio/gio.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nemo-rclone-job.h"
#include "nemo-rclone-job-manager.h"

static int failures;

/* Test-owned state, shared with the polling callback. */
static NemoRcloneJob *g_j1;
static NemoRcloneJob *g_j2;
static NemoRcloneJobManager *g_mgr;
static gboolean g_saw_concurrency;
static gboolean g_100_1, g_100_2;

static void
fail (const char *what)
{
	fprintf (stderr, "FAIL %s\n", what);
	failures++;
}

static void
check (int cond,
       const char *what)
{
	if (cond)
		printf ("ok   %s\n", what);
	else
		fail (what);
}

static void
rm_rf (const char *path)
{
	if (g_file_test (path, G_FILE_TEST_IS_DIR)) {
		GError *err = NULL;
		GDir *dir = g_dir_open (path, 0, &err);
		const char *name;

		if (dir == NULL)
			return;
		while ((name = g_dir_read_name (dir)) != NULL) {
			char *child = g_build_filename (path, name, NULL);
			rm_rf (child);
			g_free (child);
		}
		g_dir_close (dir);
		remove (path);
	} else {
		remove (path);
	}
}

/* Write `size` bytes to `path` (created if needed). */
static void
make_file (const char *path,
           goffset size)
{
	FILE *fp = fopen (path, "wb");
	gsize chunk;
	goffset remaining = size;

	g_assert (fp != NULL);
	while (remaining > 0) {
		chunk = (remaining < (goffset) (64 * 1024)) ?
			(gsize) remaining : 64 * 1024;
		if (fwrite (NULL, 1, chunk, fp) != chunk)
			break;
		remaining -= chunk;
	}
	fclose (fp);
}

static gboolean
pump_cb (gpointer data)
{
	GMainLoop *loop = data;

	if (g_mgr != NULL &&
	    nemo_rclone_job_manager_count_running (g_mgr) == 2)
		g_saw_concurrency = TRUE;

	if (g_j1 != NULL && nemo_rclone_job_get_state (g_j1) ==
	                    NEMO_RCLONE_JOB_DONE &&
	    nemo_rclone_job_get_fraction (g_j1) >= 0.999)
		g_100_1 = TRUE;
	if (g_j2 != NULL && nemo_rclone_job_get_state (g_j2) ==
	                    NEMO_RCLONE_JOB_DONE &&
	    nemo_rclone_job_get_fraction (g_j2) >= 0.999)
		g_100_2 = TRUE;

	if (g_100_1 && g_100_2)
		g_main_loop_quit (loop);

	return TRUE;
}

static void
test_two_concurrent_jobs (void)
{
	char *tmp;
	char *src1, *src2, *dst1, *dst2;
	char *sock1 = NULL, *sock2 = NULL;
	GMainLoop *loop;

	failures = 0;

	tmp = g_dir_make_tmp ("nemo-rclone-job-XXXXXX", NULL);
	g_setenv ("NEMO_RCLONE_RUNTIME_DIR", tmp, TRUE);
	g_setenv ("NEMO_RCLONE_CACHE_DIR", tmp, TRUE);

	src1 = g_build_filename (tmp, "big1.bin", NULL);
	src2 = g_build_filename (tmp, "big2.bin", NULL);
	dst1 = g_build_filename (tmp, "out1", NULL);
	dst2 = g_build_filename (tmp, "out2", NULL);
	g_mkdir_with_parents (dst1, 0700);
	g_mkdir_with_parents (dst2, 0700);

	/* ~200 MB each: large enough that two concurrent copies visibly overlap
	   on the poll loop, small enough that the test stays fast. */
	make_file (src1, (goffset) 200 * 1024 * 1024);
	make_file (src2, (goffset) 200 * 1024 * 1024);

	g_mgr = nemo_rclone_job_manager_get_default ();

	/* Start both at once.  The old single-process code would let the second
	   overwrite the first; the engine must track both. */
	g_j1 = nemo_rclone_job_manager_start_job (g_mgr, "copy", src1, dst1,
	                                          FALSE, NULL);
	g_j2 = nemo_rclone_job_manager_start_job (g_mgr, "copy", src2, dst2,
	                                          FALSE, NULL);

	loop = g_main_loop_new (NULL, FALSE);
	/* Poll every 15 ms. */
	g_timeout_add (15, pump_cb, loop);
	g_main_loop_run (loop);

	check (g_j1 != NULL, "first job started");
	check (g_j2 != NULL, "second job started");
	if (g_j1 != NULL && g_j2 != NULL) {
		check (g_j1 != g_j2, "two distinct job objects");
		check (g_strcmp0 (nemo_rclone_job_get_id (g_j1),
		                  nemo_rclone_job_get_id (g_j2)) != 0,
		       "jobs have distinct ids");

		/* Independence: each job owns its own RC socket. */
		sock1 = g_strdup (nemo_rclone_job_get_socket_path (g_j1));
		sock2 = g_strdup (nemo_rclone_job_get_socket_path (g_j2));
		check (sock1 != NULL && sock2 != NULL &&
		       g_strcmp0 (sock1, sock2) != 0,
		       "jobs use distinct RC sockets");

		/* Concurrency: the manager must see BOTH running at once. */
		check (g_saw_concurrency,
		       "both jobs observed running simultaneously");

		/* Independent completion: each reaches its own 100 %. */
		check (g_100_1, "job 1 reached 100%");
		check (g_100_2, "job 2 reached 100%");
		check (nemo_rclone_job_get_state (g_j1) ==
	               NEMO_RCLONE_JOB_DONE, "job 1 finished (DONE)");
		check (nemo_rclone_job_get_state (g_j2) ==
	               NEMO_RCLONE_JOB_DONE, "job 2 finished (DONE)");
		check (nemo_rclone_job_manager_count_running (g_mgr) == 0,
		       "no jobs left running");
		check (nemo_rclone_job_manager_count_finished (g_mgr) == 2,
		       "both jobs counted finished");
	}

	g_free (sock1);
	g_free (sock2);
	rm_rf (tmp);
	g_free (tmp);

	if (failures == 0)
		printf ("job concurrency OK\n");
	else
		fprintf (stderr, "job concurrency FAILED (%d failures)\n",
		         failures);

	exit (failures == 0 ? 0 : 1);
}

int
main (void)
{
	test_two_concurrent_jobs ();
	return 1;
}
