/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*-

   nemo-rclone-service.c: rclone integration service

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.
*/

#include <config.h>

#include "nemo-rclone-service.h"
#include "nemo-rclone-path.h"

#include <gio/gio.h>
#include <gio/gunixmounts.h>
#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#define NEMO_RCLONE_ERROR_DOMAIN g_quark_from_static_string ("nemo-rclone")

struct _NemoRcloneService {
	GObject parent_instance;

	GPtrArray *remotes;    /* char * */
	GPtrArray *favorites;  /* NemoRcloneFavorite * */
	char *favorites_path;
	gboolean remotes_loaded;
	gboolean available_checked;
	gboolean available;
};

G_DEFINE_TYPE (NemoRcloneService, nemo_rclone_service, G_TYPE_OBJECT)

static NemoRcloneService *default_service = NULL;

gboolean nemo_rclone_service_unmount_favorite (NemoRcloneService *self,
                                               const char        *id,
                                               GError           **error);

gboolean         nemo_rclone_service_is_mount_active (const char *mountpoint);

static void      save_favorites (NemoRcloneService *self);

static void
nemo_rclone_service_finalize (GObject *object)
{
	NemoRcloneService *self = NEMO_RCLONE_SERVICE (object);

	g_clear_pointer (&self->remotes, g_ptr_array_unref);
	g_clear_pointer (&self->favorites, g_ptr_array_unref);
	g_free (self->favorites_path);

	G_OBJECT_CLASS (nemo_rclone_service_parent_class)->finalize (object);
}

static void
nemo_rclone_service_class_init (NemoRcloneServiceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = nemo_rclone_service_finalize;
}

static void
nemo_rclone_service_init (NemoRcloneService *self)
{
	char *config_dir;

	self->remotes = g_ptr_array_new_with_free_func (g_free);
	self->favorites = g_ptr_array_new_with_free_func (
		(GDestroyNotify) nemo_rclone_service_favorite_free);

	config_dir = g_build_filename (g_get_user_config_dir (), "nemo", NULL);
	g_mkdir_with_parents (config_dir, 0700);
	self->favorites_path = g_build_filename (config_dir,
	                                         "rclone-favorites.json", NULL);
	g_free (config_dir);
}

NemoRcloneService *
nemo_rclone_service_get_default (void)
{
	if (default_service == NULL) {
		default_service = g_object_new (NEMO_TYPE_RCLONE_SERVICE, NULL);
	}

	return default_service;
}

void
nemo_rclone_service_favorite_free (NemoRcloneFavorite *favorite)
{
	if (favorite == NULL) {
		return;
	}

	g_free (favorite->id);
	g_free (favorite->label);
	g_free (favorite->remote);
	g_free (favorite->path);
	g_free (favorite->mountpoint);
	g_free (favorite);
}

char *
nemo_rclone_service_run_capture (NemoRcloneService *self,
                                 const char *const *argv,
                                 GError **error)
{
	char *standard_output = NULL;
	int wait_status = 0;

	if (!g_spawn_sync (NULL, (char **) argv, NULL,
	                   G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
	                   NULL, NULL, &standard_output, NULL,
	                   &wait_status, error)) {
		return NULL;
	}

	if (!g_spawn_check_wait_status (wait_status, error)) {
		g_free (standard_output);
		return NULL;
	}

	return standard_output;
}

gboolean
nemo_rclone_service_is_available (NemoRcloneService *self)
{
	const char *argv[] = { "rclone", "version", NULL };

	g_return_val_if_fail (NEMO_IS_RCLONE_SERVICE (self), FALSE);

	if (!self->available_checked) {
		char *out = nemo_rclone_service_run_capture (self, argv, NULL);
		self->available = (out != NULL);
		g_free (out);
		self->available_checked = TRUE;
	}

	return self->available;
}

static void
add_remote_name (NemoRcloneService *self,
                 const char *name,
                 gsize len)
{
	char *copy;

	if (name == NULL || len == 0) {
		return;
	}

	copy = g_strndup (name, len);
	g_ptr_array_add (self->remotes, copy);
}

static gint
compare_strings (gconstpointer a,
                 gconstpointer b)
{
	return g_strcmp0 (*(const char **) a, *(const char **) b);
}

static void
load_remotes_from_config (NemoRcloneService *self)
{
	char *path;
	char *contents = NULL;

	path = g_strdup (g_getenv ("RCLONE_CONFIG"));
	if (path == NULL) {
		path = g_build_filename (g_get_user_config_dir (), "rclone",
		                         "rclone.conf", NULL);
	}

	if (g_file_get_contents (path, &contents, NULL, NULL)) {
		char **lines = g_strsplit (contents, "\n", -1);
		int i;

		for (i = 0; lines[i] != NULL; i++) {
			char *line = g_strstrip (lines[i]);
			char *end;

			if (line[0] != '[') {
				continue;
			}

			end = strchr (line, ']');
			if (end != NULL && end > line + 1) {
				add_remote_name (self, line + 1, end - line - 1);
			}
		}

		g_strfreev (lines);
		g_free (contents);
	}

	g_free (path);
}

void
nemo_rclone_service_reload_remotes (NemoRcloneService *self)
{
	const char *argv[] = { "rclone", "listremotes", NULL };
	char *out;
	GError *error = NULL;

	g_return_if_fail (NEMO_IS_RCLONE_SERVICE (self));

	g_ptr_array_set_size (self->remotes, 0);

	out = nemo_rclone_service_run_capture (self, argv, &error);
	if (out != NULL) {
		char **lines = g_strsplit (out, "\n", -1);
		int i;

		for (i = 0; lines[i] != NULL; i++) {
			char *line = g_strstrip (lines[i]);
			gsize len;

			if (line[0] == '\0') {
				continue;
			}

			len = strlen (line);
			if (len > 0 && line[len - 1] == ':') {
				len--;
			}

			add_remote_name (self, line, len);
		}

		g_strfreev (lines);
		g_free (out);
	} else {
		if (error != NULL) {
			g_error_free (error);
		}
		load_remotes_from_config (self);
	}

	g_ptr_array_sort (self->remotes, compare_strings);
	self->remotes_loaded = TRUE;
}

GPtrArray *
nemo_rclone_service_get_remotes (NemoRcloneService *self)
{
	g_return_val_if_fail (NEMO_IS_RCLONE_SERVICE (self), NULL);

	if (!self->remotes_loaded) {
		nemo_rclone_service_reload_remotes (self);
	}

	return self->remotes;
}

static const char *
json_string_or_empty (JsonObject *object,
                      const char *member)
{
	JsonNode *node;

	if (!json_object_has_member (object, member)) {
		return "";
	}

	node = json_object_get_member (object, member);
	if (node == NULL || !JSON_NODE_HOLDS_VALUE (node) ||
	    json_node_get_value_type (node) != G_TYPE_STRING) {
		return "";
	}

	return json_object_get_string_member (object, member);
}

static NemoRcloneFavorite *
favorite_from_json (JsonObject *object)
{
	NemoRcloneFavorite *favorite = g_new0 (NemoRcloneFavorite, 1);

	favorite->id = g_strdup (json_string_or_empty (object, "id"));
	favorite->label = g_strdup (json_string_or_empty (object, "label"));
	favorite->remote = g_strdup (json_string_or_empty (object, "remote"));
	favorite->path = g_strdup (json_string_or_empty (object, "path"));
	favorite->mountpoint = g_strdup (json_string_or_empty (object, "mountpoint"));

	if (favorite->id[0] == '\0') {
		char *id = g_uuid_string_random ();
		g_free (favorite->id);
		favorite->id = id;
	}

	if (favorite->label[0] == '\0') {
		char *label = nemo_rclone_service_format_path (favorite->remote,
		                                               favorite->path);
		g_free (favorite->label);
		favorite->label = label;
	}

	return favorite;
}

/* Read one key from a section of rclone.conf.  Returns a newly allocated
   string, or NULL when the section/key is missing.  This only reads
   non-secret keys (type/port/user), and never logs values. */
static char *
remote_config_value (const char *remote,
                     const char *key)
{
	char *path;
	char *contents = NULL;
	char *section;
	char *result = NULL;
	char **lines;
	gboolean in_section = FALSE;
	guint i;

	if (remote == NULL || remote[0] == '\0') {
		return NULL;
	}

	path = g_strdup (g_getenv ("RCLONE_CONFIG"));
	if (path == NULL) {
		path = g_build_filename (g_get_user_config_dir (), "rclone",
		                         "rclone.conf", NULL);
	}

	if (!g_file_get_contents (path, &contents, NULL, NULL)) {
		g_free (path);
		return NULL;
	}

	section = g_strdup_printf ("[%s]", remote);
	lines = g_strsplit (contents, "\n", -1);

	for (i = 0; lines[i] != NULL; i++) {
		char *line = g_strstrip (lines[i]);
		char *equals;
		char *name;

		if (line[0] == '[') {
			in_section = g_strcmp0 (line, section) == 0;
			continue;
		}

		if (!in_section || line[0] == '\0' || line[0] == '#' ||
		    line[0] == ';') {
			continue;
		}

		/* rclone.conf writes "key = value" (spaces around '='), so
		   compare the trimmed key instead of assuming "key=". */
		equals = strchr (line, '=');
		if (equals == NULL) {
			continue;
		}

		name = g_strndup (line, equals - line);
		g_strstrip (name);

		if (g_strcmp0 (name, key) == 0) {
			result = g_strdup (g_strstrip (equals + 1));
			g_free (name);
			break;
		}

		g_free (name);
	}

	g_strfreev (lines);
	g_free (section);
	g_free (contents);
	g_free (path);

	return result;
}

/* Termux's OpenSSH SFTP server exposes Android's real filesystem.  Its
   server root "/" is not listable; the login home is the useful root.
   Detect the common Termux signature (port 8022 or a u0_a* Android user)
   so we can repair path semantics created by older builds. */
static gboolean
remote_is_termux (const char *remote)
{
	char *type;
	char *port;
	char *user;
	gboolean termux = FALSE;

	type = remote_config_value (remote, "type");
	port = remote_config_value (remote, "port");
	user = remote_config_value (remote, "user");

	if (g_strcmp0 (type, "sftp") == 0) {
		if (g_strcmp0 (port, "8022") == 0) {
			termux = TRUE;
		} else if (user != NULL && g_str_has_prefix (user, "u0_a")) {
			termux = TRUE;
		}
	}

	g_free (type);
	g_free (port);
	g_free (user);
	return termux;
}

/* Old builds stored "/" for the remote root and "/home" for a guessed
   Termux home.  Both are absolute Android paths, so the mount came up empty
   with "permission denied".  Rewrite those to "", and turn the explicit
   Termux home prefix into a relative path. */
static gboolean
repair_favorite_path (NemoRcloneFavorite *favorite)
{
	char *new_path = NULL;

	if (!remote_is_termux (favorite->remote)) {
		return FALSE;
	}

	if (favorite->path == NULL || favorite->path[0] == '\0') {
		return FALSE;
	}

	if (g_strcmp0 (favorite->path, "/") == 0 ||
	    g_strcmp0 (favorite->path, "/home") == 0) {
		new_path = g_strdup ("");
	} else if (g_strcmp0 (favorite->path,
	                      "/data/data/com.termux/files/home") == 0) {
		new_path = g_strdup ("");
	} else if (g_str_has_prefix (favorite->path,
	                             "/data/data/com.termux/files/home/")) {
		new_path = g_strdup (favorite->path +
		                     strlen ("/data/data/com.termux/files/home/"));
	}

	if (new_path == NULL) {
		return FALSE;
	}

	g_message ("nemo-rclone: repairing Termux favorite %s: \"%s\" -> \"%s\"",
	           favorite->remote, favorite->path,
	           new_path[0] != '\0' ? new_path : "(home)");

	/* If the label was auto-generated from the old path, regenerate it so
	   the sidebar does not keep showing "gt20:/". */
	{
		char *old_spec = nemo_rclone_service_format_path (favorite->remote,
		                                                 favorite->path);

		if (g_strcmp0 (favorite->label, old_spec) == 0) {
			char *new_label = nemo_rclone_service_format_path (favorite->remote,
			                                                  new_path);
			g_free (favorite->label);
			favorite->label = new_label;
		}

		g_free (old_spec);
	}

	g_free (favorite->path);
	favorite->path = new_path;
	return TRUE;
}

/* True when both favorites point at the same remote path, ignoring trailing
   slashes.  Used to collapse duplicates created by older builds. */
static gboolean
favorite_same_target (NemoRcloneFavorite *a,
                      NemoRcloneFavorite *b)
{
	char *a_path;
	char *b_path;
	gboolean same;

	if (g_strcmp0 (a->remote, b->remote) != 0) {
		return FALSE;
	}

	a_path = nemo_rclone_path_canonicalize (a->path);
	b_path = nemo_rclone_path_canonicalize (b->path);
	same = g_strcmp0 (a_path, b_path) == 0;
	g_free (a_path);
	g_free (b_path);

	return same;
}

static void
load_favorites (NemoRcloneService *self)
{
	char *contents = NULL;
	JsonParser *parser;
	gboolean repaired = FALSE;

	g_ptr_array_set_size (self->favorites, 0);

	if (!g_file_get_contents (self->favorites_path, &contents, NULL, NULL)) {
		return;
	}

	parser = json_parser_new ();
	if (json_parser_load_from_data (parser, contents, -1, NULL)) {
		JsonNode *root = json_parser_get_root (parser);

		if (JSON_NODE_HOLDS_OBJECT (root)) {
			JsonObject *object = json_node_get_object (root);

			if (json_object_has_member (object, "favorites")) {
				JsonArray *array = json_object_get_array_member (object, "favorites");
				guint i;

				for (i = 0; i < json_array_get_length (array); i++) {
					JsonNode *node = json_array_get_element (array, i);

					if (JSON_NODE_HOLDS_OBJECT (node)) {
						NemoRcloneFavorite *favorite =
							favorite_from_json (json_node_get_object (node));

						if (favorite->remote[0] != '\0') {
							gboolean duplicate = FALSE;

							if (repair_favorite_path (favorite)) {
								repaired = TRUE;
							}

							for (guint j = 0; j < self->favorites->len; j++) {
								NemoRcloneFavorite *existing =
									g_ptr_array_index (self->favorites, j);

								if (favorite_same_target (existing, favorite)) {
									duplicate = TRUE;
									break;
								}
							}

							if (duplicate) {
								g_message ("nemo-rclone: dropping duplicate favorite %s: \"%s\"",
								           favorite->remote, favorite->path);
								repaired = TRUE;
								nemo_rclone_service_favorite_free (favorite);
							} else {
								g_ptr_array_add (self->favorites, favorite);
							}
						} else {
							nemo_rclone_service_favorite_free (favorite);
						}
					}
				}
			}
		}
	}

	g_object_unref (parser);
	g_free (contents);

	if (repaired) {
		save_favorites (self);
	}
}

static void
save_favorites (NemoRcloneService *self)
{
	JsonBuilder *builder;
	JsonGenerator *generator;
	JsonNode *root;
	char *data;
	guint i;

	builder = json_builder_new ();
	json_builder_begin_object (builder);

	json_builder_set_member_name (builder, "version");
	json_builder_add_int_value (builder, 1);

	json_builder_set_member_name (builder, "favorites");
	json_builder_begin_array (builder);

	for (i = 0; i < self->favorites->len; i++) {
		NemoRcloneFavorite *favorite = g_ptr_array_index (self->favorites, i);

		json_builder_begin_object (builder);
		json_builder_set_member_name (builder, "id");
		json_builder_add_string_value (builder, favorite->id);
		json_builder_set_member_name (builder, "label");
		json_builder_add_string_value (builder, favorite->label);
		json_builder_set_member_name (builder, "remote");
		json_builder_add_string_value (builder, favorite->remote);
		json_builder_set_member_name (builder, "path");
		json_builder_add_string_value (builder, favorite->path);
		json_builder_set_member_name (builder, "mountpoint");
		json_builder_add_string_value (builder, favorite->mountpoint != NULL ?
		                               favorite->mountpoint : "");
		json_builder_end_object (builder);
	}

	json_builder_end_array (builder);
	json_builder_end_object (builder);

	generator = json_generator_new ();
	root = json_builder_get_root (builder);
	json_generator_set_root (generator, root);
	json_generator_set_pretty (generator, TRUE);
	data = json_generator_to_data (generator, NULL);

	if (data != NULL) {
		g_file_set_contents (self->favorites_path, data, -1, NULL);
	}

	g_free (data);
	g_object_unref (generator);
	g_object_unref (builder);
}

GPtrArray *
nemo_rclone_service_get_favorites (NemoRcloneService *self)
{
	g_return_val_if_fail (NEMO_IS_RCLONE_SERVICE (self), NULL);

	if (self->favorites->len == 0) {
		load_favorites (self);
	}

	return self->favorites;
}

NemoRcloneFavorite *
nemo_rclone_service_add_favorite (NemoRcloneService *self,
                                  const char *label,
                                  const char *remote,
                                  const char *path,
                                  const char *mountpoint)
{
	NemoRcloneFavorite *favorite;

	g_return_val_if_fail (NEMO_IS_RCLONE_SERVICE (self), NULL);
	g_return_val_if_fail (remote != NULL, NULL);
	g_return_val_if_fail (path != NULL, NULL);

	nemo_rclone_service_get_favorites (self);

	/* Replace an existing entry for the same remote path. */
	for (guint i = 0; i < self->favorites->len; i++) {
		NemoRcloneFavorite *existing = g_ptr_array_index (self->favorites, i);

		if (g_strcmp0 (existing->remote, remote) == 0 &&
		    g_strcmp0 (existing->path, path) == 0) {
			g_free (existing->label);
			existing->label = g_strdup (label != NULL ? label : "");
			g_free (existing->mountpoint);
			existing->mountpoint = g_strdup (mountpoint != NULL ? mountpoint : "");
			save_favorites (self);
			return existing;
		}
	}

	favorite = g_new0 (NemoRcloneFavorite, 1);
	favorite->id = g_uuid_string_random ();
	favorite->label = g_strdup (label != NULL ? label : "");
	favorite->remote = g_strdup (remote);
	favorite->path = g_strdup (path);
	favorite->mountpoint = g_strdup (mountpoint != NULL ? mountpoint : "");

	if (favorite->label[0] == '\0') {
		g_free (favorite->label);
		favorite->label = nemo_rclone_service_format_path (remote, path);
	}

	g_ptr_array_add (self->favorites, favorite);
	save_favorites (self);

	return favorite;
}

void
nemo_rclone_service_remove_favorite (NemoRcloneService *self,
                                     const char *id)
{
	guint i;

	g_return_if_fail (NEMO_IS_RCLONE_SERVICE (self));
	g_return_if_fail (id != NULL);

	nemo_rclone_service_get_favorites (self);

	for (i = 0; i < self->favorites->len; i++) {
		NemoRcloneFavorite *favorite = g_ptr_array_index (self->favorites, i);

		if (g_strcmp0 (favorite->id, id) == 0) {
			/* Best-effort unmount before forgetting the favorite. */
			nemo_rclone_service_unmount_favorite (self, id, NULL);
			g_ptr_array_remove_index (self->favorites, i);
			save_favorites (self);
			return;
		}
	}
}

char *
nemo_rclone_service_format_path (const char *remote,
                                 const char *path)
{
	char *canonical;
	char *result;

	if (remote == NULL) {
		return g_strdup (path != NULL ? path : "");
	}

	canonical = nemo_rclone_path_canonicalize (path);

	if (canonical[0] == '\0') {
		result = g_strdup_printf ("%s:", remote);
	} else {
		/* Do not add a leading slash: rclone treats "/foo" as an absolute
		   server path and "foo" as relative to the remote's default root
		   (the login home for SFTP/Termux). */
		result = g_strdup_printf ("%s:%s", remote, canonical);
	}

	g_free (canonical);
	return result;
}

/* Find the longest mount point containing path with an rclone FUSE type. */
static gboolean
find_rclone_mount (const char *path,
                   char **mount_path_out,
                   char **remote_out,
                   char **base_path_out)
{
	GList *mounts;
	GList *l;
	GUnixMountEntry *best = NULL;
	const char *best_path = NULL;
	gsize best_len = 0;
	const char *source;
	const char *colon;

	mounts = g_unix_mounts_get (NULL);

	for (l = mounts; l != NULL; l = l->next) {
		GUnixMountEntry *entry = l->data;
		const char *mount_path = g_unix_mount_get_mount_path (entry);
		const char *fs_type = g_unix_mount_get_fs_type (entry);
		gsize len;

		if (fs_type == NULL || !g_str_has_prefix (fs_type, "fuse.rclone")) {
			continue;
		}

		if (g_strcmp0 (mount_path, "/") != 0) {
			len = strlen (mount_path);
			if (!g_str_has_prefix (path, mount_path) ||
			    (path[len] != '\0' && path[len] != '/')) {
				continue;
			}
		} else {
			len = 1;
		}

		if (best == NULL || len > best_len) {
			best = entry;
			best_path = mount_path;
			best_len = len;
		}
	}

	if (best == NULL) {
		g_list_free_full (mounts, (GDestroyNotify) g_unix_mount_free);
		return FALSE;
	}

	source = g_unix_mount_get_device_path (best);
	if (source == NULL || (colon = strchr (source, ':')) == NULL) {
		g_list_free_full (mounts, (GDestroyNotify) g_unix_mount_free);
		return FALSE;
	}

	if (mount_path_out != NULL) {
		*mount_path_out = g_strdup (best_path);
	}
	if (remote_out != NULL) {
		*remote_out = g_strndup (source, colon - source);
	}
	if (base_path_out != NULL) {
		*base_path_out = g_strdup (colon + 1);
	}

	g_list_free_full (mounts, (GDestroyNotify) g_unix_mount_free);

	return TRUE;
}

gboolean
nemo_rclone_service_path_to_remote (NemoRcloneService *self,
                                    const char *local_path,
                                    char **remote,
                                    char **remote_path,
                                    char **mountpoint)
{
	char *mount_path = NULL;
	char *found_remote = NULL;
	char *base_path = NULL;
	char *relative;
	char *result;
	guint i;
	gboolean found;

	g_return_val_if_fail (NEMO_IS_RCLONE_SERVICE (self), FALSE);

	nemo_rclone_service_get_favorites (self);

	if (local_path == NULL || local_path[0] != '/') {
		return FALSE;
	}

	found = find_rclone_mount (local_path, &mount_path, &found_remote, &base_path);

	if (found && found_remote != NULL) {
		relative = (char *) local_path + strlen (mount_path);
		while (relative[0] == '/') {
			relative++;
		}

		result = nemo_rclone_path_join (base_path, relative);

		if (remote != NULL) {
			*remote = found_remote;
			found_remote = NULL;
		}
		if (remote_path != NULL) {
			*remote_path = result;
			result = NULL;
		}
		if (mountpoint != NULL) {
			*mountpoint = mount_path;
			mount_path = NULL;
		}

		g_free (found_remote);
		g_free (base_path);
		g_free (result);
		g_free (mount_path);

		return TRUE;
	}

	g_free (found_remote);
	g_free (base_path);
	g_free (mount_path);

	/* Fall back to a favorite mountpoint even when the mount is currently
	 * unavailable; this lets us label paths while offline. */
	for (i = 0; i < self->favorites->len; i++) {
		NemoRcloneFavorite *favorite = g_ptr_array_index (self->favorites, i);
		gsize len;

		if (favorite->mountpoint == NULL || favorite->mountpoint[0] == '\0') {
			continue;
		}

		len = strlen (favorite->mountpoint);
		if (!g_str_has_prefix (local_path, favorite->mountpoint) ||
		    (local_path[len] != '\0' && local_path[len] != '/')) {
			continue;
		}

		relative = (char *) local_path + len;
		while (relative[0] == '/') {
			relative++;
		}

		if (remote != NULL) {
			*remote = g_strdup (favorite->remote);
		}
		if (remote_path != NULL) {
			*remote_path = nemo_rclone_path_join (favorite->path,
			                                     relative);
		}
		if (mountpoint != NULL) {
			*mountpoint = g_strdup (favorite->mountpoint);
		}

		return TRUE;
	}

	return FALSE;
}

char *
nemo_rclone_service_remote_to_local (NemoRcloneService *self,
                                     const char *remote,
                                     const char *path,
                                     char **mountpoint)
{
	GList *mounts;
	GList *l;
	char *wanted;
	char *best_result = NULL;
	char *best_mountpoint = NULL;
	gsize best_prefix_len = 0;
	gboolean have_match = FALSE;

	g_return_val_if_fail (NEMO_IS_RCLONE_SERVICE (self), NULL);

	nemo_rclone_service_get_favorites (self);

	wanted = nemo_rclone_path_canonicalize (path);

	/* Prefer an active rclone mount whose source path is the longest prefix
	   of the requested path.  This makes multiple favorites on the same
	   remote resolve to the right mount. */
	mounts = g_unix_mounts_get (NULL);

	for (l = mounts; l != NULL; l = l->next) {
		GUnixMountEntry *entry = l->data;
		const char *fs_type = g_unix_mount_get_fs_type (entry);
		const char *source = g_unix_mount_get_device_path (entry);
		const char *colon;
		const char *mount_path;
		char *source_path;
		char *relative;
		char *result;
		gsize source_len;

		if (fs_type == NULL || !g_str_has_prefix (fs_type, "fuse.rclone") ||
		    source == NULL || (colon = strchr (source, ':')) == NULL) {
			continue;
		}

		if (strncmp (source, remote, colon - source) != 0 ||
		    (gsize) (colon - source) != strlen (remote)) {
			continue;
		}

		source_path = nemo_rclone_path_canonicalize (colon + 1);
		source_len = strlen (source_path);

		relative = nemo_rclone_path_relative (wanted, source_path);
		if (relative == NULL ||
		    (have_match && source_len <= best_prefix_len)) {
			g_free (relative);
			g_free (source_path);
			continue;
		}

		mount_path = g_unix_mount_get_mount_path (entry);
		result = relative[0] != '\0' ?
			g_build_filename (mount_path, relative, NULL) :
			g_strdup (mount_path);

		g_free (best_result);
		g_free (best_mountpoint);
		best_result = result;
		best_mountpoint = g_strdup (mount_path);
		best_prefix_len = source_len;
		have_match = TRUE;

		g_free (relative);
		g_free (source_path);
	}

	g_list_free_full (mounts, (GDestroyNotify) g_unix_mount_free);

	if (have_match) {
		if (mountpoint != NULL) {
			*mountpoint = best_mountpoint;
		} else {
			g_free (best_mountpoint);
		}
		g_free (wanted);
		return best_result;
	}

	g_free (best_mountpoint);
	g_free (wanted);
	return NULL;
}
/* --------------------------- mount management --------------------------- */

/* Slugify a label into a filesystem-safe token (lowercase, alnum / _ / -). */
char *
nemo_rclone_service_slugify (const char *label)
{
	const char *p;
	gsize len;
	char *out, *start;
	gboolean last_dash = FALSE;

	if (label == NULL || label[0] == '\0') {
		return g_strdup ("rclone");
	}

	len = strlen (label);
	out = g_malloc (len + 1);
	start = out;

	for (p = label; *p != '\0'; p++) {
		unsigned char c = (unsigned char) *p;
		int ch;

		if (c >= 'A' && c <= 'Z') {
			c = (unsigned char) (c - 'A' + 'a');
		}

		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		    c == '-' || c == '_') {
			ch = (int) c;
		} else {
			ch = '-';
		}

		if (ch == '-' && last_dash) {
			continue;
		}
		last_dash = (ch == '-');
		*out++ = (char) ch;
	}

	while (out > start && out[-1] == '-') {
		--out;
	}

	*out = '\0';

	if (out == start) {
		g_free (start);
		return g_strdup ("rclone");
	}

	return start;
}

/* Directory under which per-favorite mounts live. */
static char *
mount_base_dir (void)
{
	const char *override = g_getenv ("NEMO_RCLONE_MOUNT_BASE");

	if (override != NULL && override[0] != '\0') {
		return g_strdup (override);
	}

	/* If the administrator made /mnt writable for this user, put the mounts
	   in a visible place.  Otherwise fall back to the user data directory;
	   creating directories in /mnt normally requires root. */
	if (g_file_test ("/mnt", G_FILE_TEST_IS_DIR) && access ("/mnt", W_OK) == 0) {
		return g_build_filename ("/mnt", "nemo-rclone", NULL);
	}

	return g_build_filename (g_get_user_data_dir (),
	                         "nemo-rclone", "mounts", NULL);
}

/* Whether another favorite already claims candidate as its mountpoint. */
static gboolean
mountpoint_claimed_by_other (NemoRcloneService *self,
                             const char *candidate,
                             const char *remote,
                             const char *path)
{
	GPtrArray *favorites;
	guint i;

	favorites = nemo_rclone_service_get_favorites (self);

	for (i = 0; i < favorites->len; i++) {
		NemoRcloneFavorite *fav = g_ptr_array_index (favorites, i);
		gboolean same_target = FALSE;

		if (fav->mountpoint == NULL ||
		    g_strcmp0 (fav->mountpoint, candidate) != 0) {
			continue;
		}

		if (g_strcmp0 (fav->remote, remote) == 0) {
			char *fav_path = nemo_rclone_path_canonicalize (fav->path);
			char *wanted_path = nemo_rclone_path_canonicalize (path);

			same_target = g_strcmp0 (fav_path, wanted_path) == 0;
			g_free (fav_path);
			g_free (wanted_path);
		}

		if (!same_target) {
			return TRUE;
		}
	}

	return FALSE;
}

/* Whether candidate is already an active rclone mount of a different remote
   path.  (An active mount of this exact favorite is not a collision.) */
static gboolean
mountpoint_active_by_other (const char *candidate,
                            const char *remote,
                            const char *path)
{
	GList *mounts;
	GList *l;
	gboolean other = FALSE;
	char *wanted = nemo_rclone_path_canonicalize (path);

	mounts = g_unix_mounts_get (NULL);

	for (l = mounts; l != NULL; l = l->next) {
		GUnixMountEntry *entry = l->data;
		const char *mp = g_unix_mount_get_mount_path (entry);
		const char *fs_type = g_unix_mount_get_fs_type (entry);
		const char *source = g_unix_mount_get_device_path (entry);
		const char *colon;

		if (fs_type == NULL || !g_str_has_prefix (fs_type, "fuse.rclone") ||
		    g_strcmp0 (mp, candidate) != 0 ||
		    source == NULL || (colon = strchr (source, ':')) == NULL) {
			continue;
		}

		if (strncmp (source, remote, colon - source) == 0 &&
		    (gsize) (colon - source) == strlen (remote)) {
			char *source_path = nemo_rclone_path_canonicalize (colon + 1);

			if (g_strcmp0 (source_path, wanted) != 0) {
				other = TRUE;
			}
			g_free (source_path);
		} else {
			other = TRUE;
		}
		break;
	}

	g_list_free_full (mounts, (GDestroyNotify) g_unix_mount_free);
	g_free (wanted);
	return other;
}

/* Mountpoint for a favorite: reuse an existing one, otherwise derive a
   deterministic path under the mount base.  Collisions are disambiguated with
   a token of the remote.  Caller frees the result. */
char *
nemo_rclone_service_resolve_mountpoint (NemoRcloneService *self,
                                        const char *remote,
                                        const char *path,
                                        const char *label)
{
	char *slug, *base, *candidate;
	char *remote_slug;
	int suffix;

	g_return_val_if_fail (NEMO_IS_RCLONE_SERVICE (self), NULL);
	g_return_val_if_fail (label != NULL, NULL);

	slug = nemo_rclone_service_slugify (label);
	base = mount_base_dir ();
	candidate = g_build_filename (base, slug, NULL);

	if (mountpoint_claimed_by_other (self, candidate, remote, path) ||
	    mountpoint_active_by_other (candidate, remote, path)) {
		g_free (candidate);
		remote_slug = (remote != NULL && remote[0] != '\0') ?
			nemo_rclone_service_slugify (remote) : g_strdup ("mount");
		candidate = g_build_filename (base, slug, remote_slug, NULL);
		g_free (remote_slug);

		for (suffix = 2;
		     mountpoint_claimed_by_other (self, candidate, remote, path) ||
		     mountpoint_active_by_other (candidate, remote, path);
		     suffix++) {
			char *leaf = g_strdup_printf ("%d", suffix);

			g_free (candidate);
			candidate = g_build_filename (base, slug, leaf, NULL);
			g_free (leaf);
		}
	}

	g_free (base);
	g_free (slug);
	return candidate;
}

char *
nemo_rclone_service_format_uri (const char *remote,
                                const char *path)
{
	char *canonical;
	char *escaped_remote;
	char *escaped_path;
	char *uri;

	if (remote == NULL || remote[0] == '\0') {
		return g_strdup ("rclone://transfer");
	}

	canonical = nemo_rclone_path_canonicalize (path);
	escaped_remote = g_uri_escape_string (remote, NULL, TRUE);

	if (canonical[0] == '\0') {
		/* rclone://remote means the remote's default root (home). */
		uri = g_strdup_printf ("rclone://%s", escaped_remote);
	} else if (canonical[0] == '/') {
		/* rclone://remote//abs/path: the second slash marks an absolute
		   server path, distinct from a path relative to the home. */
		escaped_path = g_uri_escape_string (canonical, "/", TRUE);
		uri = g_strdup_printf ("rclone://%s/%s", escaped_remote, escaped_path);
		g_free (escaped_path);
	} else {
		escaped_path = g_uri_escape_string (canonical, "/", TRUE);
		uri = g_strdup_printf ("rclone://%s/%s", escaped_remote, escaped_path);
		g_free (escaped_path);
	}

	g_free (escaped_remote);
	g_free (canonical);

	return uri;
}

char *
nemo_rclone_service_get_mount_log_path (const char *mountpoint)
{
	char *dir;
	char *base;
	char *leaf;
	char *log_path;

	if (mountpoint == NULL || mountpoint[0] == '\0') {
		return NULL;
	}

	dir = g_build_filename (g_get_user_cache_dir (), "nemo-rclone", NULL);
	base = g_path_get_basename (mountpoint);
	leaf = g_strconcat (base, ".log", NULL);
	log_path = g_build_filename (dir, leaf, NULL);

	g_free (dir);
	g_free (base);
	g_free (leaf);

	return log_path;
}

char *
nemo_rclone_service_get_mount_log_tail (const char *mountpoint,
                                        gsize max_bytes)
{
	char *path;
	char *contents = NULL;
	gsize len;
	char *tail;

	path = nemo_rclone_service_get_mount_log_path (mountpoint);
	if (path == NULL) {
		return NULL;
	}

	if (!g_file_get_contents (path, &contents, &len, NULL)) {
		g_free (path);
		return NULL;
	}

	if (max_bytes == 0) {
		max_bytes = 4096;
	}

	if (len > max_bytes) {
		tail = g_strdup (contents + len - max_bytes);
	} else {
		tail = g_strdup (contents);
	}

	g_free (contents);
	g_free (path);
	return tail;
}


/* Whether an rclone FUSE mount is currently active at mountpoint. */
gboolean
nemo_rclone_service_is_mount_active (const char *mountpoint)
{
	GList *mounts, *l;

	if (mountpoint == NULL || mountpoint[0] == '\0') {
		return FALSE;
	}

	mounts = g_unix_mounts_get (NULL);

	for (l = mounts; l != NULL; l = l->next) {
		GUnixMountEntry *entry = l->data;
		const char *mp = g_unix_mount_get_mount_path (entry);
		const char *fs_type = g_unix_mount_get_fs_type (entry);

		if (fs_type != NULL &&
		    g_str_has_prefix (fs_type, "fuse.rclone") &&
		    g_strcmp0 (mp, mountpoint) == 0) {
			g_list_free_full (mounts, (GDestroyNotify) g_unix_mount_free);
			return TRUE;
		}
	}

	g_list_free_full (mounts, (GDestroyNotify) g_unix_mount_free);
	return FALSE;
}

/* Whether the favorite identified by id is currently mounted. */
gboolean
nemo_rclone_service_favorite_is_mounted (NemoRcloneService *self,
                                         const char *id)
{
	GPtrArray *favorites;
	guint i;

	g_return_val_if_fail (NEMO_IS_RCLONE_SERVICE (self), FALSE);

	favorites = nemo_rclone_service_get_favorites (self);

	for (i = 0; i < favorites->len; i++) {
		NemoRcloneFavorite *fav = g_ptr_array_index (favorites, i);

		if (g_strcmp0 (fav->id, id) == 0) {
			char *local;
			gboolean mounted = nemo_rclone_service_is_mount_active (fav->mountpoint);

			if (mounted) {
				return TRUE;
			}

			/* Also recognise a mount that was started outside Nemo. */
			local = nemo_rclone_service_remote_to_local (self, fav->remote,
			                                             fav->path, NULL);
			if (local != NULL) {
				g_free (local);
				return TRUE;
			}
			return FALSE;
		}
	}

	return FALSE;
}

/* Build argv for "rclone mount <spec> <mountpoint> --vfs-flags".  The returned
   argv is NULL-terminated and owned by the caller.  spec is taken into argv,
   so this frees it (no caller return needed). */
static void
build_mount_argv (NemoRcloneService *self,
                  NemoRcloneFavorite *favorite,
                  const char *mountpoint,
                  char ***argv_out)
{
	char *spec, *cache_dir, *log_path, *log_dir;
	char **argv;
	int n = 0;

	(void) self;

	spec = nemo_rclone_service_format_path (favorite->remote, favorite->path);
	log_path = nemo_rclone_service_get_mount_log_path (mountpoint);
	log_dir = g_path_get_dirname (log_path);
	g_mkdir_with_parents (log_dir, 0700);
	g_free (log_dir);

	argv = g_malloc (sizeof (char *) * 36);
	argv[n++] = g_strdup ("rclone");
	argv[n++] = g_strdup ("mount");
	argv[n++] = spec;
	argv[n++] = g_strdup (mountpoint);
	argv[n++] = g_strdup ("--vfs-cache-mode");
	argv[n++] = g_strdup ("full");
	argv[n++] = g_strdup ("--vfs-read-ahead");
	argv[n++] = g_strdup ("128M");
	argv[n++] = g_strdup ("--vfs-read-chunk-streams");
	argv[n++] = g_strdup ("8");
	argv[n++] = g_strdup ("--dir-cache-time");
	argv[n++] = g_strdup ("10m");
	argv[n++] = g_strdup ("--attr-timeout");
	argv[n++] = g_strdup ("1m");
	argv[n++] = g_strdup ("--poll-interval");
	argv[n++] = g_strdup ("0");
	cache_dir = g_build_filename (g_get_user_cache_dir (),
	                              "rclone", "vfs", NULL);
	argv[n++] = g_strdup ("--cache-dir");
	argv[n++] = g_strdup (cache_dir);
	argv[n++] = g_strdup ("--allow-other");
	argv[n++] = g_strdup ("--allow-non-empty");
	argv[n++] = g_strdup ("--log-level");
	argv[n++] = g_strdup ("INFO");
	argv[n++] = g_strdup ("--log-file");
	argv[n++] = log_path;
	argv[n++] = g_strdup ("--log-format");
	argv[n++] = g_strdup ("date,time");
	argv[n++] = g_strdup ("--stats");
	argv[n++] = g_strdup ("1s");
	argv[n++] = NULL;

	g_free (cache_dir);

	*argv_out = argv;
}

/* Fork/exec rclone fully detached (setsid + stdio to /dev/null) so the mount
   outlives the nemo process that started it.  Returns the child pid, or -1
   with error set. */
static GPid
spawn_mount_detached (char **argv, GError **error)
{
	GPid pid;
	int null_fd;

	pid = fork ();
	if (pid < 0) {
		if (error != NULL)
			*error = g_error_new (NEMO_RCLONE_ERROR_DOMAIN, 0,
			                      "%s", g_strerror (errno));
		return -1;
	}
	if (pid == 0) {
		setsid ();
		null_fd = open ("/dev/null", O_RDWR);
		if (null_fd >= 0) {
			dup2 (null_fd, STDIN_FILENO);
			dup2 (null_fd, STDOUT_FILENO);
			dup2 (null_fd, STDERR_FILENO);
			if (null_fd > 2)
				close (null_fd);
		}
		execvp (argv[0], argv);
		_exit (127);
	}

	return pid;
}

/* Reap the detached rclone process when it eventually exits, so it does not
   stay around as a zombie for the lifetime of Nemo. */
static void
mount_child_exited (GPid pid,
                    gint status,
                    gpointer user_data)
{
	(void) pid;
	(void) status;
	(void) user_data;
}

/* Ensure the favorite identified by id is mounted.  On success returns the
   mountpoint (caller frees); on failure returns NULL and sets error. */
char *
nemo_rclone_service_mount_favorite (NemoRcloneService *self,
                                    const char *id,
                                    GError **error)
{
	GPtrArray *favorites;
	guint i;
	NemoRcloneFavorite *favorite = NULL;
	char *mountpoint = NULL;
	char **argv = NULL;

	g_return_val_if_fail (NEMO_IS_RCLONE_SERVICE (self), NULL);

	favorites = nemo_rclone_service_get_favorites (self);

	for (i = 0; i < favorites->len; i++) {
		NemoRcloneFavorite *fav = g_ptr_array_index (favorites, i);

		if (g_strcmp0 (fav->id, id) == 0) {
			favorite = fav;
			break;
		}
	}

	if (favorite == NULL) {
		if (error != NULL) {
			*error = g_error_new (NEMO_RCLONE_ERROR_DOMAIN, 0,
			                      _("No rclone favorite with id %s."), id);
		}
		return NULL;
	}

	if (!nemo_rclone_service_is_available (self)) {
		if (error != NULL) {
			*error = g_error_new (NEMO_RCLONE_ERROR_DOMAIN, 0,
			                      _("rclone is not installed or not in PATH."));
		}
		return NULL;
	}

	/* Reuse an existing mountpoint, or derive one. */
	if (favorite->mountpoint != NULL && favorite->mountpoint[0] != '\0') {
		mountpoint = g_strdup (favorite->mountpoint);
	} else {
		mountpoint = nemo_rclone_service_resolve_mountpoint (self,
		                                                     favorite->remote,
		                                                     favorite->path,
		                                                     favorite->label);
	}

	/* Already active (perhaps mounted manually) — just persist it. */
	if (nemo_rclone_service_is_mount_active (mountpoint)) {
		if (g_strcmp0 (favorite->mountpoint, mountpoint) != 0) {
			g_free (favorite->mountpoint);
			favorite->mountpoint = g_strdup (mountpoint);
			save_favorites (self);
		}
		g_free (mountpoint);
		return g_strdup (favorite->mountpoint);
	}

	/* Dry-run mode: build and log the command without spawning. */
	if (g_getenv ("NEMO_RCLONE_MOUNT_DRY_RUN") != NULL) {
		char **argvv = NULL;
		build_mount_argv (self, favorite, mountpoint, &argvv);
		g_message ("rclone mount would run:");
		for (guint j = 0; argvv[j] != NULL; j++) {
			g_message ("  %s", argvv[j]);
			g_free (argvv[j]);
		}
		g_free (argvv);
		g_free (mountpoint);
		return NULL;
	}

	/* Ensure the mountpoint directory exists and is user-writable. */
	if (g_mkdir_with_parents (mountpoint, 0755) != 0 && errno != EEXIST) {
		if (error != NULL) {
			*error = g_error_new (NEMO_RCLONE_ERROR_DOMAIN, 0,
			                      _("Unable to create mount point %s."),
			                      mountpoint);
		}
		g_free (mountpoint);
		return NULL;
	}

	build_mount_argv (self, favorite, mountpoint, &argv);

	{
		GPid pid = spawn_mount_detached (argv, error);

		g_strfreev (argv);

		if (pid == -1) {
			g_free (mountpoint);
			return NULL;
		}

		/* Give rclone a moment to fail early (bad remote, busy mountpoint,
		   missing FUSE permissions) so the caller gets a useful error
		   instead of a silent "mounted" message. */
		g_usleep (200 * 1000);

		{
			int wait_status = 0;
			pid_t reaped = waitpid (pid, &wait_status, WNOHANG);

			if (reaped == pid) {
				char *tail = nemo_rclone_service_get_mount_log_tail (mountpoint, 2000);
				GError *child_error = NULL;

				g_spawn_check_wait_status (wait_status, &child_error);

				if (error != NULL) {
					*error = g_error_new (NEMO_RCLONE_ERROR_DOMAIN, 0,
					                      _("rclone mount exited: %s%s%s"),
					                      child_error != NULL ? child_error->message : _("unknown error"),
					                      tail != NULL && tail[0] != '\0' ? "\n" : "",
					                      tail != NULL ? tail : "");
				}

				g_clear_error (&child_error);
				g_free (tail);
				g_free (mountpoint);
				return NULL;
			}

			if (reaped == 0) {
				g_child_watch_add (pid, mount_child_exited, NULL);
			}
		}
	}

	/* Persist the mountpoint so it is known even while offline. */
	if (g_strcmp0 (favorite->mountpoint, mountpoint) != 0) {
		g_free (favorite->mountpoint);
		favorite->mountpoint = g_strdup (mountpoint);
		save_favorites (self);
	}
	g_free (mountpoint);

	return g_strdup (favorite->mountpoint);
}

/* Unmount the favorite identified by id, if mounted.  Returns TRUE on success
   (or if there was nothing to unmount). */
gboolean
nemo_rclone_service_unmount_favorite (NemoRcloneService *self,
                                      const char *id,
                                      GError **error)
{
	GPtrArray *favorites;
	guint i;
	NemoRcloneFavorite *favorite = NULL;
	char *mountpoint = NULL;
	gint wait_status = 0;
	gboolean spawned = FALSE;

	g_return_val_if_fail (NEMO_IS_RCLONE_SERVICE (self), FALSE);

	favorites = nemo_rclone_service_get_favorites (self);

	for (i = 0; i < favorites->len; i++) {
		NemoRcloneFavorite *fav = g_ptr_array_index (favorites, i);

		if (g_strcmp0 (fav->id, id) == 0) {
			favorite = fav;
			break;
		}
	}

	if (favorite == NULL) {
		if (error != NULL) {
			*error = g_error_new (NEMO_RCLONE_ERROR_DOMAIN, 0,
			                      _("No rclone favorite with id %s."), id);
		}
		return FALSE;
	}

	if (favorite->mountpoint == NULL || favorite->mountpoint[0] == '\0' ||
	    !nemo_rclone_service_is_mount_active (favorite->mountpoint)) {
		/* Nothing mounted — clear any stale metadata and succeed. */
		if (favorite->mountpoint != NULL && favorite->mountpoint[0] != '\0') {
			g_free (favorite->mountpoint);
			favorite->mountpoint = NULL;
			save_favorites (self);
		}
		return TRUE;
	}

	mountpoint = g_strdup (favorite->mountpoint);

	if (g_getenv ("NEMO_RCLONE_MOUNT_DRY_RUN") != NULL) {
		g_message ("would run: fusermount -u %s", mountpoint);
		g_free (mountpoint);
		return TRUE;
	}

	{
		char *fusermount = g_find_program_in_path ("fusermount3");

		if (fusermount == NULL) {
			fusermount = g_find_program_in_path ("fusermount");
		}

		if (fusermount != NULL) {
			const char *argvv[] = { fusermount, "-u", mountpoint, NULL };

			if (g_spawn_sync (NULL, (char **) argvv, NULL,
			                  G_SPAWN_STDERR_TO_DEV_NULL,
			                  NULL, NULL, NULL, NULL, &wait_status, error)) {
				spawned = g_spawn_check_wait_status (wait_status, error);
			}

			g_free (fusermount);
		} else if (error != NULL) {
			*error = g_error_new (NEMO_RCLONE_ERROR_DOMAIN, 0,
			                      _("fusermount is not installed."));
		}
	}

	if (!spawned && error != NULL && *error == NULL) {
		*error = g_error_new (NEMO_RCLONE_ERROR_DOMAIN, 0,
		                      _("Failed to unmount %s."), mountpoint);
	}

	g_free (mountpoint);

	if (spawned && !nemo_rclone_service_is_mount_active (favorite->mountpoint)) {
		g_free (favorite->mountpoint);
		favorite->mountpoint = NULL;
		save_favorites (self);
	} else if (spawned) {
		/* fusermount reported success but the mount is still visible. */
		spawned = FALSE;
		if (error != NULL && *error == NULL) {
			*error = g_error_new (NEMO_RCLONE_ERROR_DOMAIN, 0,
			                      _("%s is still mounted."), favorite->mountpoint);
		}
	}

	return spawned;
}
static void
on_favorite_home_clicked (GtkButton *button,
                          gpointer user_data)
{
	GtkWidget *entry = user_data;

	(void) button;
	gtk_entry_set_text (GTK_ENTRY (entry), "");
}

void
nemo_rclone_service_prompt_add_favorite (NemoRcloneService *self,
                                         GtkWindow *parent,
                                         const char *local_path,
                                         const char *remote_in,
                                         const char *path_in)
{
	GtkWidget *dialog;
	GtkWidget *grid;
	GtkWidget *label_entry;
	GtkWidget *remote_combo;
	GtkWidget *path_entry;
	GtkWidget *home_button;
	GtkWidget *path_box;
	GtkWidget *hint_label;
	char *remote = NULL;
	char *remote_path = NULL;
	char *mountpoint = NULL;
	char *default_label = NULL;
	gint response;
	guint i;

	g_return_if_fail (NEMO_IS_RCLONE_SERVICE (self));

	if (remote_in != NULL && path_in != NULL) {
		remote = g_strdup (remote_in);
		remote_path = g_strdup (path_in);
	} else if (local_path != NULL) {
		nemo_rclone_service_path_to_remote (self, local_path,
		                                    &remote, &remote_path, &mountpoint);
	}

	dialog = gtk_dialog_new_with_buttons (_("Rclone Favorite"),
	                                      parent,
	                                      GTK_DIALOG_MODAL |
	                                      GTK_DIALOG_DESTROY_WITH_PARENT,
	                                      _("_Cancel"), GTK_RESPONSE_CANCEL,
	                                      _("_Add"), GTK_RESPONSE_ACCEPT,
	                                      NULL);

	gtk_window_set_default_size (GTK_WINDOW (dialog), 520, 260);
	gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);

	grid = gtk_grid_new ();
	gtk_grid_set_row_spacing (GTK_GRID (grid), 8);
	gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
	gtk_container_set_border_width (GTK_CONTAINER (grid), 12);
	gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (dialog))),
	                    grid, TRUE, TRUE, 0);

	gtk_grid_attach (GTK_GRID (grid), gtk_label_new (_("Label:")), 0, 0, 1, 1);
	label_entry = gtk_entry_new ();
	gtk_entry_set_placeholder_text (GTK_ENTRY (label_entry),
	                                _("e.g. m64_backup_x_app"));
	gtk_widget_set_hexpand (label_entry, TRUE);
	gtk_grid_attach (GTK_GRID (grid), label_entry, 1, 0, 2, 1);

	gtk_grid_attach (GTK_GRID (grid), gtk_label_new (_("Remote:")), 0, 1, 1, 1);
	remote_combo = gtk_combo_box_text_new ();
	{
		GPtrArray *remotes = nemo_rclone_service_get_remotes (self);

		for (i = 0; i < remotes->len; i++) {
			const char *name = g_ptr_array_index (remotes, i);
			gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (remote_combo), name);
		}
	}
	gtk_widget_set_hexpand (remote_combo, TRUE);
	gtk_grid_attach (GTK_GRID (grid), remote_combo, 1, 1, 2, 1);

	gtk_grid_attach (GTK_GRID (grid), gtk_label_new (_("Path:")), 0, 2, 1, 1);
	path_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	path_entry = gtk_entry_new ();
	gtk_entry_set_placeholder_text (GTK_ENTRY (path_entry),
	                                _("empty = remote home (recommended for Termux)"));
	gtk_widget_set_hexpand (path_entry, TRUE);
	gtk_box_pack_start (GTK_BOX (path_box), path_entry, TRUE, TRUE, 0);

	home_button = gtk_button_new_with_label (_("Home"));
	gtk_widget_set_tooltip_text (home_button,
	                             _("Use the remote's login home instead of an absolute path. "
	                               "Termux can only list its home, not Android's / root."));
	gtk_box_pack_start (GTK_BOX (path_box), home_button, FALSE, FALSE, 0);
	gtk_grid_attach (GTK_GRID (grid), path_box, 1, 2, 2, 1);
	g_signal_connect (home_button, "clicked",
	                  G_CALLBACK (on_favorite_home_clicked), path_entry);

	hint_label = gtk_label_new (NULL);
	gtk_label_set_xalign (GTK_LABEL (hint_label), 0.0);
	gtk_label_set_line_wrap (GTK_LABEL (hint_label), TRUE);
	gtk_widget_set_sensitive (hint_label, FALSE);
	gtk_grid_attach (GTK_GRID (grid), hint_label, 0, 3, 3, 1);

	if (remote != NULL) {
		char *display = nemo_rclone_service_format_path (remote, remote_path);

		gtk_entry_set_text (GTK_ENTRY (path_entry), remote_path != NULL ? remote_path : "");
		gtk_label_set_text (GTK_LABEL (hint_label), display);
		gtk_combo_box_text_remove_all (GTK_COMBO_BOX_TEXT (remote_combo));

		{
			GPtrArray *remotes = nemo_rclone_service_get_remotes (self);
			gint active = -1;

			for (i = 0; i < remotes->len; i++) {
				const char *name = g_ptr_array_index (remotes, i);

				gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (remote_combo), name);
				if (g_strcmp0 (name, remote) == 0) {
					active = i;
				}
			}

			if (active >= 0) {
				gtk_combo_box_set_active (GTK_COMBO_BOX (remote_combo), active);
			}
		}

		g_free (display);
	} else if (local_path != NULL) {
		gtk_label_set_text (GTK_LABEL (hint_label),
		                    _("This path is not under an rclone mount; choose a remote and path manually."));
	}

	if (local_path != NULL) {
		default_label = g_path_get_basename (local_path);
		gtk_entry_set_text (GTK_ENTRY (label_entry), default_label);
	}

	gtk_widget_show_all (dialog);

	response = gtk_dialog_run (GTK_DIALOG (dialog));

	if (response == GTK_RESPONSE_ACCEPT) {
		const char *label = gtk_entry_get_text (GTK_ENTRY (label_entry));
		const char *path = gtk_entry_get_text (GTK_ENTRY (path_entry));
		gint active = gtk_combo_box_get_active (GTK_COMBO_BOX (remote_combo));
		const char *selected_remote = NULL;

		if (active >= 0) {
			selected_remote = gtk_combo_box_text_get_active_text (
				GTK_COMBO_BOX_TEXT (remote_combo));
		}

		if (selected_remote == NULL) {
			GtkWidget *error = gtk_message_dialog_new (
				parent, GTK_DIALOG_MODAL,
				GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
				_("A remote is required."));
			gtk_dialog_run (GTK_DIALOG (error));
			gtk_widget_destroy (error);
		} else {
			nemo_rclone_service_add_favorite (self, label,
			                                  selected_remote, path,
			                                  mountpoint);
		}

		g_free ((char *) selected_remote);
	}

	g_free (remote);
	g_free (remote_path);
	g_free (mountpoint);
	g_free (default_label);
	gtk_widget_destroy (dialog);
}
