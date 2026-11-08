/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*-

   nemo-rclone-service.c: rclone integration service

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.
*/

#include <config.h>

#include "nemo-rclone-service.h"

#include <gio/gio.h>
#include <gio/gunixmounts.h>
#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <string.h>

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

static void
load_favorites (NemoRcloneService *self)
{
	char *contents = NULL;
	JsonParser *parser;

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
							g_ptr_array_add (self->favorites, favorite);
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
	if (remote == NULL) {
		return g_strdup (path != NULL ? path : "");
	}

	if (path == NULL || path[0] == '\0') {
		return g_strdup_printf ("%s:", remote);
	}

	if (path[0] == '/') {
		return g_strdup_printf ("%s:%s", remote, path);
	}

	return g_strdup_printf ("%s:/%s", remote, path);
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
		if (relative[0] == '\0') {
			relative = (char *) "/";
		}

		if (base_path != NULL && base_path[0] != '\0') {
			result = g_strconcat (base_path, relative, NULL);
		} else {
			result = g_strdup (relative);
		}

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

		if (remote != NULL) {
			*remote = g_strdup (favorite->remote);
		}
		if (remote_path != NULL) {
			*remote_path = g_strconcat (favorite->path,
			                            relative[0] != '\0' ? relative : "/",
			                            NULL);
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
	guint i;

	g_return_val_if_fail (NEMO_IS_RCLONE_SERVICE (self), NULL);

	nemo_rclone_service_get_favorites (self);

	/* Prefer an active rclone mount. */
	mounts = g_unix_mounts_get (NULL);

	for (l = mounts; l != NULL; l = l->next) {
		GUnixMountEntry *entry = l->data;
		const char *fs_type = g_unix_mount_get_fs_type (entry);
		const char *source = g_unix_mount_get_device_path (entry);
		const char *colon;
		const char *mount_path;
		char *result;

		if (fs_type == NULL || !g_str_has_prefix (fs_type, "fuse.rclone") ||
		    source == NULL || (colon = strchr (source, ':')) == NULL) {
			continue;
		}

		if (strncmp (source, remote, colon - source) != 0 ||
		    (gsize) (colon - source) != strlen (remote)) {
			continue;
		}

		mount_path = g_unix_mount_get_mount_path (entry);
		if (colon[1] != '\0' && g_str_has_prefix (path, colon + 1)) {
			path += strlen (colon + 1);
		}

		while (path[0] == '/') {
			path++;
		}

		result = g_build_filename (mount_path, path, NULL);

		if (mountpoint != NULL) {
			*mountpoint = g_strdup (mount_path);
		}

		g_list_free_full (mounts, (GDestroyNotify) g_unix_mount_free);
		return result;
	}

	g_list_free_full (mounts, (GDestroyNotify) g_unix_mount_free);

	/* Fall back to a favorite's cached mountpoint. */
	for (i = 0; i < self->favorites->len; i++) {
		NemoRcloneFavorite *favorite = g_ptr_array_index (self->favorites, i);

		if (g_strcmp0 (favorite->remote, remote) != 0 ||
		    favorite->mountpoint == NULL || favorite->mountpoint[0] == '\0') {
			continue;
		}

		while (path[0] == '/') {
			path++;
		}

		if (mountpoint != NULL) {
			*mountpoint = g_strdup (favorite->mountpoint);
		}

		return g_build_filename (favorite->mountpoint, path, NULL);
	}

	return NULL;
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
	path_entry = gtk_entry_new ();
	gtk_entry_set_placeholder_text (GTK_ENTRY (path_entry), _("/data/backup"));
	gtk_widget_set_hexpand (path_entry, TRUE);
	gtk_grid_attach (GTK_GRID (grid), path_entry, 1, 2, 2, 1);

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

		if (selected_remote == NULL || path[0] == '\0') {
			GtkWidget *error = gtk_message_dialog_new (
				parent, GTK_DIALOG_MODAL,
				GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
				_("A remote and a path are required."));
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
