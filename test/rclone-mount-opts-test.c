/* -*- Mode: C; c-basic-offset: 4; tab-width: 4 -*-
 *
 * rclone-mount-opts-test.c: verify the FUSE/mount-options settings round-trip
 *
 * A value written through nemo_rclone_service_save_mount_opts() must be read
 * back unchanged through nemo_rclone_service_get_mount_opts().  Points
 * XDG_CONFIG_HOME at a scratch directory so the real user config is never
 * touched or required to exist.
 */

#include <config.h>

#include <gio/gio.h>
#include <glib.h>

#include "nemo-rclone-service.h"

static void
check_str (const char *got,
           const char *want,
           const char *field)
{
	if (g_strcmp0 (got, want) != 0) {
		fprintf (stderr,
		         "MISMATCH %s: got '%s' want '%s'\n",
		         field, got ? got : "(null)", want);
		exit (1);
	}
	fprintf (stderr, "OK %s = '%s'\n", field, got);
}

static void
test_roundtrip (void)
{
	RcloneMountOpts *set;
	RcloneMountOpts *loaded;

	/* Start from the defaults, then deliberately change every field. */
	set = nemo_rclone_service_get_mount_opts ();
	g_free (set->vfs_cache_mode);
	set->vfs_cache_mode = g_strdup ("minimal");
	g_free (set->vfs_cache_max_size);
	set->vfs_cache_max_size = g_strdup ("512M");
	g_free (set->vfs_cache_max_age);
	set->vfs_cache_max_age = g_strdup ("2h");
	g_free (set->read_ahead);
	set->read_ahead = g_strdup ("64M");
	set->read_chunk_streams = 16;
	g_free (set->dir_cache_time);
	set->dir_cache_time = g_strdup ("30m");
	g_free (set->attr_timeout);
	set->attr_timeout = g_strdup ("30s");
	g_free (set->poll_interval);
	set->poll_interval = g_strdup ("1s");

	nemo_rclone_service_save_mount_opts (set);
	nemo_rclone_service_free_mount_opts (set);

	loaded = nemo_rclone_service_get_mount_opts ();

	check_str (loaded->vfs_cache_mode, "minimal", "vfs_cache_mode");
	check_str (loaded->vfs_cache_max_size, "512M",
	           "vfs_cache_max_size");
	check_str (loaded->vfs_cache_max_age, "2h",
	           "vfs_cache_max_age");
	check_str (loaded->read_ahead, "64M", "read_ahead");
	if (loaded->read_chunk_streams != 16) {
		fprintf (stderr, "MISMATCH read_chunk_streams\n");
		exit (1);
	}
	check_str (loaded->dir_cache_time, "30m", "dir_cache_time");
	check_str (loaded->attr_timeout, "30s", "attr_timeout");
	check_str (loaded->poll_interval, "1s", "poll_interval");

	nemo_rclone_service_free_mount_opts (loaded);

	fprintf (stderr, "mount opts round trip OK\n");
}

int
main (void)
{
	char *tmp;

	tmp = g_dir_make_tmp ("nemo-rclone-mount-opts.XXXXXX", NULL);
	g_setenv ("XDG_CONFIG_HOME", tmp, TRUE);

	test_roundtrip ();

	g_free (tmp);
	return 0;
}
