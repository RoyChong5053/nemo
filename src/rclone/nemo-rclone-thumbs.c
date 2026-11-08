/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*-

   nemo-rclone-thumbs.c: persistent per-directory thumbnail opt-in for
   rclone / remote mounts.

   The built-in Nemo per-directory "show thumbnails" toggle is persisted via
   GVFS metadata:: xattr.  rclone FUSE mounts (notably SFTP) do not support
   xattr ("Operation not supported"), so that metadata is lost on every
   navigation and "bind the path" never sticks.  This module keeps an
   equivalent opt-in list in its own JSON file, keyed by directory path, so
   that a remote mount the user opted into keeps generating thumbnails
   across sessions.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.
*/

#include <config.h>

#include "nemo-rclone-thumbs.h"

#include <string.h>

#include <json-glib/json-glib.h>

/* Path to the whitelist JSON under ~/.config/nemo/. */
static char *thumbs_path = NULL;

/* Ordered list of directory paths the user opted into.  Owned, g_free. */
static GPtrArray *enabled_dirs = NULL;

/* Guards enabled_dirs.  The list is tiny, so a mutex is plenty. */
static GMutex lock;

static void
load_thumbs (void)
{
	char *contents = NULL;
	JsonParser *parser;

	g_ptr_array_set_size (enabled_dirs, 0);

	if (!g_file_get_contents (thumbs_path, &contents, NULL, NULL)) {
		g_free (contents);
		return;
	}

	parser = json_parser_new ();
	if (json_parser_load_from_data (parser, contents, -1, NULL)) {
		JsonNode *root = json_parser_get_root (parser);

		if (JSON_NODE_HOLDS_OBJECT (root)) {
			JsonObject *object = json_node_get_object (root);
			JsonNode *array_node = json_object_get_member (object, "dirs");

			if (array_node && JSON_NODE_HOLDS_ARRAY (array_node)) {
				JsonArray *array = json_node_get_array (array_node);
				guint i;

				for (i = 0; i < json_array_get_length (array); i++) {
					JsonNode *el = json_array_get_element (array, i);

					if (el && JSON_NODE_HOLDS_VALUE (el) &&
					    json_node_get_value_type (el) == G_TYPE_STRING) {
						char *dir = g_strdup (json_array_get_string_element (array, i));

						if (dir[0] != '\0') {
							g_ptr_array_add (enabled_dirs, dir);
						} else {
							g_free (dir);
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
save_thumbs (void)
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

	json_builder_set_member_name (builder, "dirs");
	json_builder_begin_array (builder);
	for (i = 0; i < enabled_dirs->len; i++) {
		char *dir = g_ptr_array_index (enabled_dirs, i);

		json_builder_add_string_value (builder, dir);
	}
	json_builder_end_array (builder);
	json_builder_end_object (builder);

	generator = json_generator_new ();
	root = json_builder_get_root (builder);
	json_generator_set_root (generator, root);
	json_generator_set_pretty (generator, TRUE);
	data = json_generator_to_data (generator, NULL);

	if (data != NULL) {
		g_file_set_contents (thumbs_path, data, -1, NULL);
		g_free (data);
	}

	g_object_unref (generator);
	g_object_unref (builder);
}

void
nemo_rclone_thumbs_init (void)
{
	char *config_dir;

	if (thumbs_path != NULL) {
		return;
	}

	g_mutex_init (&lock);
	g_mutex_lock (&lock);

	config_dir = g_build_filename (g_get_user_config_dir (), "nemo", NULL);
	g_mkdir_with_parents (config_dir, 0700);
	thumbs_path = g_build_filename (config_dir, "rclone-thumbs.json", NULL);
	g_free (config_dir);

	enabled_dirs = g_ptr_array_new_with_free_func (g_free);
	load_thumbs ();

	g_mutex_unlock (&lock);
}

static gboolean
find_enabled (const char *path, guint *out_index)
{
	guint i;

	for (i = 0; i < enabled_dirs->len; i++) {
		char *dir = g_ptr_array_index (enabled_dirs, i);

		if (g_strcmp0 (path, dir) == 0) {
			if (out_index) {
				*out_index = i;
			}
			return TRUE;
		}
	}

	return FALSE;
}

gboolean
nemo_rclone_thumbs_is_enabled (const char *path)
{
	guint i;

	if (thumbs_path == NULL || path == NULL || enabled_dirs == NULL) {
		return FALSE;
	}

	/* No whitelist populated yet -> fall back to Nemo's default policy. */
	if (enabled_dirs->len == 0) {
		return FALSE;
	}

	for (i = 0; i < enabled_dirs->len; i++) {
		char *dir = g_ptr_array_index (enabled_dirs, i);
		gsize dir_len = strlen (dir);

		/* Exact match, or <path> is a descendant of an enabled dir
		   (enables the whole subtree, mirroring the old metadata
		   inheritance behaviour). */
		if (g_str_has_prefix (path, dir) &&
		    (path[dir_len] == '\0' || path[dir_len] == '/')) {
			return TRUE;
		}
	}

	return FALSE;
}

void
nemo_rclone_thumbs_add (const char *dir_path)
{
	guint index;

	if (thumbs_path == NULL) {
		nemo_rclone_thumbs_init ();
	}

	if (dir_path == NULL || dir_path[0] == '\0') {
		return;
	}

	g_mutex_lock (&lock);

	if (find_enabled (dir_path, &index)) {
		g_mutex_unlock (&lock);
		return;
	}

	g_ptr_array_add (enabled_dirs, g_strdup (dir_path));
	save_thumbs ();
	g_mutex_unlock (&lock);
}

void
nemo_rclone_thumbs_remove (const char *dir_path)
{
	guint index;

	if (thumbs_path == NULL) {
		return;
	}

	g_mutex_lock (&lock);

	if (find_enabled (dir_path, &index)) {
		g_ptr_array_remove_index (enabled_dirs, index);
		save_thumbs ();
	}

	g_mutex_unlock (&lock);
}
