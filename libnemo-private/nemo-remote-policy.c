/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*-

   nemo-remote-policy.c: policy helpers for slow remote filesystems

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public
   License along with this program; if not, write to the Free
   Software Foundation, Inc., 51 Franklin Street - Suite 500,
   Boston, MA 02110-1335, USA.
*/

#include <config.h>

#include "nemo-remote-policy.h"
#include "nemo-file-private.h"
#include "nemo-global-preferences.h"
#include "nemo-directory-private.h"

#include <gio/gio.h>
#include <gio/gunixmounts.h>
#include <string.h>

/* The mount table is re-read only after GLib's mount monitor says something
 * changed.  This is called from the main thread and from the directory async
 * state machine, but GUnixMountMonitor and the cached list are only touched
 * from the main loop; the async state machine runs in the main loop too. */
static GList *cached_mounts = NULL;
static guint64 cached_time_read = 0;
static GUnixMountMonitor *mount_monitor = NULL;

static GList *
get_mounts (void)
{
	if (mount_monitor == NULL) {
		mount_monitor = g_unix_mount_monitor_get ();
	}

	if (cached_mounts == NULL || g_unix_mounts_changed_since (cached_time_read)) {
		g_list_free_full (cached_mounts, (GDestroyNotify) g_unix_mount_free);
		cached_mounts = g_unix_mounts_get (&cached_time_read);
	}

	return cached_mounts;
}

static gboolean
mount_path_contains (const char *mount_path,
                     const char *path)
{
	gsize len;

	if (g_strcmp0 (mount_path, "/") == 0) {
		return TRUE;
	}

	len = strlen (mount_path);

	return g_str_has_prefix (path, mount_path) &&
	       (path[len] == '\0' || path[len] == '/');
}

static gboolean
fs_type_is_slow_remote (const char *fs_type)
{
	if (fs_type == NULL) {
		return FALSE;
	}

	if (g_str_has_prefix (fs_type, "fuse.rclone") ||
	    g_str_has_prefix (fs_type, "fuse.sshfs") ||
	    g_str_has_prefix (fs_type, "fuse.sftp") ||
	    g_str_has_prefix (fs_type, "fuse.smb") ||
	    g_str_has_prefix (fs_type, "fuse.davfs") ||
	    g_str_has_prefix (fs_type, "fuse.httpdir") ||
	    g_str_has_prefix (fs_type, "fuse.curlftpfs") ||
	    g_str_has_prefix (fs_type, "fuse.juicefs")) {
		return TRUE;
	}

	if (g_strcmp0 (fs_type, "cifs") == 0 ||
	    g_strcmp0 (fs_type, "smb3") == 0 ||
	    g_strcmp0 (fs_type, "smbfs") == 0 ||
	    g_strcmp0 (fs_type, "nfs") == 0 ||
	    g_strcmp0 (fs_type, "nfs4") == 0 ||
	    g_strcmp0 (fs_type, "davfs") == 0 ||
	    g_strcmp0 (fs_type, "afs") == 0 ||
	    g_strcmp0 (fs_type, "9p") == 0) {
		return TRUE;
	}

	/* Be conservative about other FUSE filesystems: overlayfs, gvfs,
	 * portals and squashfs are local and should keep the fast path. */
	if (g_str_has_prefix (fs_type, "fuse.")) {
		if (g_str_has_prefix (fs_type, "fuse.gvfsd-fuse") ||
		    g_str_has_prefix (fs_type, "fuse.portal") ||
		    g_str_has_prefix (fs_type, "fuse.fuse-overlayfs") ||
		    g_str_has_prefix (fs_type, "fuse.squashfuse") ||
		    g_str_has_prefix (fs_type, "fuse.archivemount")) {
			return FALSE;
		}
		return TRUE;
	}

	return FALSE;
}

static GUnixMountEntry *
find_mount_for_path (const char *path,
                     char **mount_path_out)
{
	GList *l;
	GUnixMountEntry *best = NULL;
	const char *best_path = NULL;
	gsize best_len = 0;

	for (l = get_mounts (); l != NULL; l = l->next) {
		GUnixMountEntry *entry = l->data;
		const char *mount_path = g_unix_mount_get_mount_path (entry);
		gsize len;

		if (!mount_path_contains (mount_path, path)) {
			continue;
		}

		len = strlen (mount_path);
		if (best == NULL || len > best_len) {
			best = entry;
			best_path = mount_path;
			best_len = len;
		}
	}

	if (best != NULL && mount_path_out != NULL) {
		*mount_path_out = g_strdup (best_path);
	}

	return best;
}

gboolean
nemo_remote_policy_path_is_slow (const char *path)
{
	GUnixMountEntry *entry;

	if (path == NULL || path[0] == '\0') {
		return FALSE;
	}

	/* Anything that is not a native POSIX path (gvfs, sftp://, ...) is
	 * treated as remote. */
	if (path[0] != '/') {
		return TRUE;
	}

	entry = find_mount_for_path (path, NULL);

	if (entry == NULL) {
		return FALSE;
	}

	return fs_type_is_slow_remote (g_unix_mount_get_fs_type (entry));
}

gboolean
nemo_remote_policy_file_is_slow (NemoFile *file)
{
	GFile *location;
	char *path;
	gboolean slow;

	if (file == NULL) {
		return FALSE;
	}

	location = nemo_file_get_location (file);
	if (location == NULL) {
		return FALSE;
	}

	path = g_file_get_path (location);
	g_object_unref (location);

	if (path == NULL) {
		/* Virtual locations (search://, recent://, ...) keep the full
		 * attribute set so extensions and metadata still work. */
		return FALSE;
	}

	slow = nemo_remote_policy_path_is_slow (path);
	g_free (path);

	return slow;
}

gboolean
nemo_remote_policy_directory_is_slow (NemoDirectory *directory)
{
	GFile *location;
	char *path;
	gboolean slow;

	if (directory == NULL) {
		return FALSE;
	}

	location = nemo_directory_get_location (directory);
	if (location == NULL) {
		return FALSE;
	}

	path = g_file_get_path (location);
	g_object_unref (location);

	if (path == NULL) {
		return FALSE;
	}

	slow = nemo_remote_policy_path_is_slow (path);
	g_free (path);

	return slow;
}

gboolean
nemo_remote_policy_should_skip_item_count (NemoFile *file)
{
	NemoSpeedTradeoffValue setting;

	setting = g_settings_get_enum (nemo_preferences,
	                               NEMO_PREFERENCES_SHOW_DIRECTORY_ITEM_COUNTS);

	if (setting == NEMO_SPEED_TRADEOFF_NEVER) {
		return TRUE;
	}

	if (setting == NEMO_SPEED_TRADEOFF_ALWAYS) {
		return FALSE;
	}

	/* local-only: do not enumerate every subdirectory on a slow remote. */
	return nemo_remote_policy_file_is_slow (file);
}

const char *
nemo_remote_policy_get_attributes (NemoFile *file)
{
	if (nemo_remote_policy_file_is_slow (file)) {
		return NEMO_FILE_REMOTE_ATTRIBUTES;
	}

	return NEMO_FILE_DEFAULT_ATTRIBUTES;
}

char *
nemo_remote_policy_describe (const char *path)
{
	GUnixMountEntry *entry;
	char *mount_path = NULL;
	const char *fs_type;
	char *result;

	if (path == NULL) {
		return g_strdup ("unknown");
	}

	if (path[0] != '/') {
		return g_strdup ("non-native");
	}

	entry = find_mount_for_path (path, &mount_path);
	if (entry == NULL) {
		return g_strdup ("local (no mount entry)");
	}

	fs_type = g_unix_mount_get_fs_type (entry);
	result = g_strdup_printf ("%s at %s", fs_type, mount_path);

	g_free (mount_path);

	return result;
}
