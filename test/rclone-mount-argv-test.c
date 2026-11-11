/* -*- Mode: C; c-basic-offset: 4; tab-width: 4 -*-
 *
 * rclone-mount-argv-test.c: assert the mount argv only carries flags rclone
 * actually understands.
 *
 * Regression guard for the bug where build_mount_argv() shipped an
 * "--vfs-flags" argument that the installed rclone rejected, so every mount
 * that actually spawned rclone failed immediately with "unknown flag".
 *
 * The test recompiles nemo-rclone-service.c directly (see test/meson.build),
 * so it can call build_mount_argv() and inspect the real argv.  If rclone is
 * on PATH it validates every --flag against "rclone mount --help"; otherwise
 * it falls back to checking only the known-bogus flags, so it still runs in
 * CI without rclone installed.
 */

#include <config.h>

#include <gio/gio.h>
#include <glib.h>

#include "nemo-rclone-service.h"

static int g_failures;

static void
mark_fail (const char *msg)
{
	fprintf (stderr, "FAIL %s\n", msg);
	g_failures++;
}

static void
assert_flag_present (char **argv,
                     const char *want)
{
	for (int i = 0; argv[i] != NULL; i++)
		if (g_strcmp0 (argv[i], want) == 0)
			return;
	mark_fail (g_strdup_printf ("expected flag '%s' missing from argv", want));
}

static void
assert_flag_absent (char **argv,
                    const char *absent)
{
	for (int i = 0; argv[i] != NULL; i++)
		if (g_strcmp0 (argv[i], absent) == 0)
			mark_fail (g_strdup_printf ("bogus flag '%s' present in argv", absent));
}

/* Run command_line and return merged stdout+stderr in *out.  Returns FALSE
   (leaving *out NULL) if the command isn't available, e.g. rclone missing. */
static gboolean
run_capture (const char *command_line,
             gchar **out)
{
	gchar *stdout_buf = NULL, *stderr_buf = NULL;
	gint status = 0;
	GError *err = NULL;
	gboolean ok = g_spawn_command_line_sync (command_line, &stdout_buf,
	                                         &stderr_buf, &status, &err);
	g_clear_error (&err);
	if (!ok)
		return FALSE;

	*out = g_strconcat (stdout_buf ? stdout_buf : "",
	                    stderr_buf ? stderr_buf : "", NULL);
	g_free (stdout_buf);
	g_free (stderr_buf);
	return TRUE;
}

/* If rclone is available, validate that every "--flag" in argv is one
   "rclone mount --help" understands.  Skipped (best-effort) when rclone is
   missing so the regression guard still runs without it. */
static void
validate_against_rclone_help (char **argv)
{
	gchar *mount_help = NULL;

	if (!run_capture ("rclone mount --help", &mount_help)) {
		fprintf (stderr,
		         "SKIP: could not run rclone; checked known-bogus flags only\n");
		return;
	}

	/* `rclone mount --help` only lists mount-specific flags; the global ones
	   (--log-level, --stats, --allow-other, ...) live in `rclone help flags`,
	   so merge both sources before checking. */
	gchar *global = NULL;
	if (run_capture ("rclone help flags", &global)) {
		gchar *merged = g_strconcat (mount_help, "\n", global, NULL);
		g_free (mount_help);
		g_free (global);
		mount_help = merged;
	} else {
		g_free (global);
	}

	/* Every "--flag" in the argv must appear verbatim in rclone's own
	   usage text.  A substring test (rather than tokenizing the help) is
	   robust to rclone's exact whitespace/formatting and still catches any
	   bogus flag. */
	for (int i = 0; argv[i] != NULL; i++) {
		if (argv[i][0] != '-' || argv[i][1] != '-')
			continue;  /* value or short flag */
		if (!g_strstr_len (mount_help, -1, argv[i]))
			mark_fail (g_strdup_printf
				           ("rclone mount does not understand flag '%s'", argv[i]));
	}

	g_free (mount_help);
}

static void
check_one (const char *remote,
           const char *path)
{
	NemoRcloneFavorite fav = {
		.label = g_strdup (remote),
		.remote = g_strdup (remote),
		.path = g_strdup (path),
	};
	char *mp = g_build_filename (g_get_tmp_dir (), "rclone-argv-XXXXXX", NULL);
	char **argv = NULL;

	/* build_mount_argv ignores its self pointer, so NULL is fine here. */
	build_mount_argv (NULL, &fav, mp, &argv);

	/* The specific regression: a bogus rclone flag must never appear. */
	assert_flag_absent (argv, "--vfs-flags");
	assert_flag_absent (argv, "omit-mountpoint-in-errors");

	/* Sanity: the core VFS tuning flags we rely on are present. */
	assert_flag_present (argv, "--vfs-cache-mode");
	assert_flag_present (argv, "--allow-other");
	assert_flag_present (argv, "--allow-non-empty");

	validate_against_rclone_help (argv);

	g_free (argv);
	g_free (mp);
	g_free (fav.label);
	g_free (fav.remote);
	g_free (fav.path);
}

int
main (void)
{
	/* home root (empty path), absolute server path, and an s3 remote,
	   exercising several backends through the same argv builder. */
	check_one ("gt20", "");
	check_one ("m64", "/home/roychong");
	check_one ("r2", "/");

	if (g_failures == 0)
		fprintf (stderr, "OK: rclone mount argv is clean\n");
	else
		fprintf (stderr, "%d mount-argv problem(s) found\n", g_failures);

	return g_failures == 0 ? 0 : 1;
}
