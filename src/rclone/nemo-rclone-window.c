/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*-

   nemo-rclone-window.c: rclone transfer window

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.
*/

#include <config.h>

#include "nemo-rclone-window.h"
#include "nemo-rclone-path.h"
#include "nemo-rclone-service.h"
#include "nemo-window.h"
#include "nemo-window-slot.h"

#include <gio/gio.h>
#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <string.h>

typedef struct {
	GtkWidget *window;
	GtkWindow *parent;

	GtkWidget *remote_a;
	GtkWidget *path_a;
	GtkWidget *remote_b;
	GtkWidget *path_b;
	GtkWidget *mode_combo;
	GtkWidget *dry_run_check;
	GtkWidget *start_button;
	GtkWidget *cancel_button;
	GtkWidget *progress;
	GtkWidget *status_label;
	GtkWidget *log_view;
	GtkTextBuffer *log_buffer;

	GtkWidget *favorites_view;
	GtkListStore *favorites_store;

	NemoRcloneService *service;

	GSubprocess *process;
	GDataInputStream *stream;
	GtkTextBuffer *job_log;
	GtkWidget *job_progress;
	GtkWidget *job_status;
	gboolean running;
	gboolean read_done;
	gboolean wait_done;

	guint status_timer;
} RcloneWindow;

static RcloneWindow *rclone_window = NULL;

static void
append_log (GtkTextBuffer *buffer,
            const char *text)
{
	GtkTextIter end;

	if (buffer == NULL || text == NULL) {
		return;
	}

	gtk_text_buffer_get_end_iter (buffer, &end);
	gtk_text_buffer_insert (buffer, &end, text, -1);
	gtk_text_buffer_insert (buffer, &end, "\n", -1);

	/* Keep the log bounded. */
	if (gtk_text_buffer_get_line_count (buffer) > 400) {
		GtkTextIter start;
		GtkTextIter cut;

		gtk_text_buffer_get_start_iter (buffer, &start);
		cut = start;
		gtk_text_iter_forward_lines (&cut, 100);
		gtk_text_buffer_delete (buffer, &start, &cut);
	}
}

static gint64
json_int_member (JsonObject *object,
                 const char *member)
{
	JsonNode *node;

	if (!json_object_has_member (object, member)) {
		return 0;
	}

	node = json_object_get_member (object, member);
	if (node == NULL || !JSON_NODE_HOLDS_VALUE (node)) {
		return 0;
	}

	if (json_node_get_value_type (node) == G_TYPE_DOUBLE) {
		return (gint64) json_node_get_double (node);
	}

	return json_node_get_int (node);
}

static double
json_double_member (JsonObject *object,
                    const char *member)
{
	JsonNode *node;

	if (!json_object_has_member (object, member)) {
		return 0.0;
	}

	node = json_object_get_member (object, member);
	if (node == NULL || !JSON_NODE_HOLDS_VALUE (node)) {
		return 0.0;
	}

	if (json_node_get_value_type (node) == G_TYPE_INT64) {
		return (double) json_node_get_int (node);
	}

	return json_node_get_double (node);
}

static void
handle_json_line (RcloneWindow *self,
                  const char *line)
{
	JsonParser *parser;
	JsonNode *root;
	JsonObject *object;

	parser = json_parser_new ();

	if (!json_parser_load_from_data (parser, line, -1, NULL)) {
		g_object_unref (parser);
		return;
	}

	root = json_parser_get_root (parser);

	if (!JSON_NODE_HOLDS_OBJECT (root)) {
		g_object_unref (parser);
		return;
	}

	object = json_node_get_object (root);

	if (json_object_has_member (object, "stats")) {
		JsonObject *stats = json_object_get_object_member (object, "stats");
		gint64 bytes = json_int_member (stats, "bytes");
		gint64 total = json_int_member (stats, "totalBytes");
		double speed = json_double_member (stats, "speed");
		gint64 eta = json_int_member (stats, "eta");
		char *done = g_format_size ((guint64) bytes);
		char *all = g_format_size ((guint64) total);
		char *rate = g_format_size ((guint64) speed);
		char *status;

		if (total > 0) {
			gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (self->job_progress),
			                               (double) bytes / (double) total);
		} else {
			gtk_progress_bar_pulse (GTK_PROGRESS_BAR (self->job_progress));
		}

		if (eta > 0) {
			status = g_strdup_printf (_("Transferred %s / %s (%s/s, ETA %" G_GINT64_FORMAT "s)"),
			                          done, all, rate, eta);
		} else {
			status = g_strdup_printf (_("Transferred %s / %s (%s/s)"),
			                          done, all, rate);
		}

		gtk_label_set_text (GTK_LABEL (self->job_status), status);

		g_free (status);
		g_free (done);
		g_free (all);
		g_free (rate);
	} else if (json_object_has_member (object, "msg")) {
		const char *msg = json_object_get_string_member (object, "msg");
		const char *level = json_object_has_member (object, "level") ?
		                    json_object_get_string_member (object, "level") : "info";

		if (g_strcmp0 (level, "debug") != 0) {
			append_log (self->job_log, msg);
		}
	}

	g_object_unref (parser);
}

static void
finish_job (RcloneWindow *self)
{
	self->running = FALSE;

	gtk_widget_set_sensitive (self->start_button, TRUE);
	gtk_widget_set_sensitive (self->cancel_button, FALSE);

	g_clear_object (&self->stream);
	g_clear_object (&self->process);
}

static void
job_read_line (RcloneWindow *self);

static void
job_read_line_cb (GObject *source_object,
                  GAsyncResult *result,
                  gpointer user_data)
{
	RcloneWindow *self = user_data;
	GError *error = NULL;
	char *line;

	line = g_data_input_stream_read_line_finish (G_DATA_INPUT_STREAM (source_object),
	                                             result, NULL, &error);

	if (line != NULL) {
		handle_json_line (self, line);
		g_free (line);
		job_read_line (self);
		return;
	}

	if (error != NULL) {
		g_error_free (error);
	}

	self->read_done = TRUE;

	if (self->wait_done && self->process != NULL) {
		finish_job (self);
	}
}

static void
job_read_line (RcloneWindow *self)
{
	if (self->stream == NULL) {
		self->read_done = TRUE;
		return;
	}

	g_data_input_stream_read_line_async (self->stream,
	                                     G_PRIORITY_DEFAULT,
	                                     NULL,
	                                     job_read_line_cb,
	                                     self);
}

static void
job_wait_cb (GObject *source_object,
             GAsyncResult *result,
             gpointer user_data)
{
	RcloneWindow *self = user_data;
	GError *error = NULL;
	gboolean success;

	success = g_subprocess_wait_check_finish (G_SUBPROCESS (source_object),
	                                          result, &error);

	if (success) {
		gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (self->job_progress), 1.0);
		gtk_label_set_text (GTK_LABEL (self->job_status), _("Transfer finished."));
		append_log (self->job_log, _("Transfer finished."));
	} else {
		if (error != NULL) {
			char *msg = g_strdup_printf (_("Transfer failed: %s"), error->message);
			gtk_label_set_text (GTK_LABEL (self->job_status), msg);
			append_log (self->job_log, msg);
			g_free (msg);
			g_error_free (error);
		} else {
			gtk_label_set_text (GTK_LABEL (self->job_status), _("Transfer cancelled."));
			append_log (self->job_log, _("Transfer cancelled."));
		}
	}

	self->wait_done = TRUE;

	if (self->read_done && self->process != NULL) {
		finish_job (self);
	}
}

static char *
combo_active_text (GtkWidget *combo)
{
	if (gtk_combo_box_get_active (GTK_COMBO_BOX (combo)) < 0) {
		return NULL;
	}

	return gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (combo));
}

/* "~" in a path entry is a friendly spelling of the remote home (""). */
static const char *
entry_path_text (GtkWidget *entry)
{
	const char *text = gtk_entry_get_text (GTK_ENTRY (entry));

	return g_strcmp0 (text, "~") == 0 ? "" : text;
}

static void
set_combo_active_text (GtkWidget *combo,
                       const char *text)
{
	GtkTreeModel *model;
	GtkTreeIter iter;
	gboolean valid;

	if (text == NULL) {
		return;
	}

	model = gtk_combo_box_get_model (GTK_COMBO_BOX (combo));
	valid = gtk_tree_model_get_iter_first (model, &iter);

	while (valid) {
		char *value = NULL;

		gtk_tree_model_get (model, &iter, 0, &value, -1);
		if (g_strcmp0 (value, text) == 0) {
			gtk_combo_box_set_active_iter (GTK_COMBO_BOX (combo), &iter);
			g_free (value);
			return;
		}
		g_free (value);
		valid = gtk_tree_model_iter_next (model, &iter);
	}

	gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (combo), text);
	gtk_combo_box_set_active (GTK_COMBO_BOX (combo),
	                          gtk_tree_model_iter_n_children (model, NULL) - 1);
}

static void
on_start_clicked (GtkButton *button,
                  gpointer user_data)
{
	RcloneWindow *self = user_data;
	char *remote_a = combo_active_text (self->remote_a);
	char *remote_b = combo_active_text (self->remote_b);
	const char *path_a = entry_path_text (self->path_a);
	const char *path_b = entry_path_text (self->path_b);
	char *src = NULL;
	char *dst = NULL;
	char *mode;
	GPtrArray *args;
	GSubprocessLauncher *launcher;
	GError *error = NULL;

	(void) button;

	if (remote_a == NULL || remote_b == NULL) {
		gtk_label_set_text (GTK_LABEL (self->job_status),
		                    _("Choose a source and a destination first."));
		g_free (remote_a);
		g_free (remote_b);
		return;
	}

	src = nemo_rclone_service_format_path (remote_a, path_a);
	dst = nemo_rclone_service_format_path (remote_b, path_b);

	mode = gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (self->mode_combo));
	if (mode == NULL) {
		mode = g_strdup ("copy");
	}

	args = g_ptr_array_new_with_free_func (g_free);
	g_ptr_array_add (args, g_strdup ("rclone"));
	g_ptr_array_add (args, g_strdup (mode));
	g_ptr_array_add (args, g_strdup (src));
	g_ptr_array_add (args, g_strdup (dst));
	g_ptr_array_add (args, g_strdup ("--use-json-log"));
	g_ptr_array_add (args, g_strdup ("--stats"));
	g_ptr_array_add (args, g_strdup ("1s"));
	g_ptr_array_add (args, g_strdup ("--log-level"));
	g_ptr_array_add (args, g_strdup ("INFO"));
	g_ptr_array_add (args, g_strdup ("--transfers"));
	g_ptr_array_add (args, g_strdup ("4"));
	g_ptr_array_add (args, g_strdup ("--checkers"));
	g_ptr_array_add (args, g_strdup ("8"));
	g_ptr_array_add (args, g_strdup ("--ignore-checksum"));

	if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->dry_run_check))) {
		g_ptr_array_add (args, g_strdup ("--dry-run"));
	}

	g_ptr_array_add (args, NULL);

	gtk_text_buffer_set_text (self->job_log, "", -1);
	gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (self->job_progress), 0.0);
	gtk_label_set_text (GTK_LABEL (self->job_status), _("Starting…"));
	gtk_widget_set_sensitive (self->start_button, FALSE);
	gtk_widget_set_sensitive (self->cancel_button, TRUE);

	launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
	                                      G_SUBPROCESS_FLAGS_STDERR_MERGE);

	self->process = g_subprocess_launcher_spawnv (launcher,
	                                              (const char *const *) args->pdata,
	                                              &error);

	g_object_unref (launcher);
	g_ptr_array_free (args, TRUE);

	if (self->process == NULL) {
		char *msg = g_strdup_printf (_("Unable to start rclone: %s"),
		                             error != NULL ? error->message : _("unknown error"));

		gtk_label_set_text (GTK_LABEL (self->job_status), msg);
		append_log (self->job_log, msg);
		g_free (msg);
		if (error != NULL) {
			g_error_free (error);
		}
		gtk_widget_set_sensitive (self->start_button, TRUE);
		gtk_widget_set_sensitive (self->cancel_button, FALSE);
		g_free (remote_a);
		g_free (remote_b);
		g_free (src);
		g_free (dst);
		g_free (mode);
		return;
	}

	self->stream = g_data_input_stream_new (g_subprocess_get_stdout_pipe (self->process));
	g_data_input_stream_set_newline_type (self->stream, G_DATA_STREAM_NEWLINE_TYPE_ANY);

	self->running = TRUE;
	self->read_done = FALSE;
	self->wait_done = FALSE;

	g_subprocess_wait_check_async (self->process, NULL, job_wait_cb, self);
	job_read_line (self);

	append_log (self->job_log, src);
	append_log (self->job_log, dst);

	g_free (remote_a);
	g_free (remote_b);
	g_free (src);
	g_free (dst);
	g_free (mode);
}

static void
on_cancel_clicked (GtkButton *button,
                   gpointer user_data)
{
	RcloneWindow *self = user_data;

	(void) button;

	if (self->process != NULL) {
		g_subprocess_force_exit (self->process);
		gtk_label_set_text (GTK_LABEL (self->job_status), _("Cancelling…"));
	}
}

/* ----------------------------- browse dialog ----------------------------- */

typedef struct {
	GtkWidget *dialog;
	GtkWidget *list_view;
	GtkListStore *store;
	GtkWidget *path_entry;
	GtkWidget *target_entry;
	NemoRcloneService *service;
	char *remote;
	char *path;
} BrowseData;

static void
browse_data_free (BrowseData *data)
{
	g_free (data->remote);
	g_free (data->path);
	g_free (data);
}

static char *
path_join (const char *base,
           const char *name)
{
	return nemo_rclone_path_join (base, name);
}

static void
browse_refresh (BrowseData *data)
{
	char *spec;
	const char *argv[9];
	GError *error = NULL;
	char *output;
	char **lines;
	int i;
	GtkTreeIter iter;

	spec = nemo_rclone_service_format_path (data->remote, data->path);
	argv[0] = "rclone";
	argv[1] = "lsf";
	argv[2] = spec;
	argv[3] = "--dirs-only";
	argv[4] = "--max-depth";
	argv[5] = "1";
	argv[6] = "--format";
	argv[7] = "p";
	argv[8] = NULL;

	gtk_list_store_clear (data->store);

	output = nemo_rclone_service_run_capture (data->service, argv, &error);

	if (output == NULL) {
		char *msg = g_strdup_printf (_("Unable to list %s: %s"), spec,
		                             error != NULL ? error->message : _("unknown error"));
		gtk_list_store_append (data->store, &iter);
		gtk_list_store_set (data->store, &iter, 0, msg, -1);
		g_free (msg);
		if (error != NULL) {
			g_error_free (error);
		}
	} else {
		lines = g_strsplit (output, "\n", -1);

		for (i = 0; lines[i] != NULL; i++) {
			char *line = g_strstrip (lines[i]);
			gsize len;

			if (line[0] == '\0') {
				continue;
			}

			len = strlen (line);
			if (line[len - 1] == '/') {
				line[len - 1] = '\0';
			}

			gtk_list_store_append (data->store, &iter);
			gtk_list_store_set (data->store, &iter, 0, line, -1);
		}

		g_strfreev (lines);
		g_free (output);
	}

	gtk_entry_set_text (GTK_ENTRY (data->path_entry),
	                    data->path != NULL ? data->path : "");
	g_free (spec);
}

static void
browse_row_activated (GtkTreeView *view,
                      GtkTreePath *tree_path,
                      GtkTreeViewColumn *column,
                      gpointer user_data)
{
	BrowseData *data = user_data;
	GtkTreeModel *model = gtk_tree_view_get_model (view);
	GtkTreeIter iter;
	char *name = NULL;
	char *next;

	(void) column;

	if (!gtk_tree_model_get_iter (model, &iter, tree_path)) {
		return;
	}

	gtk_tree_model_get (model, &iter, 0, &name, -1);
	if (name == NULL) {
		return;
	}

	next = path_join (data->path, name);
	g_free (data->path);
	data->path = next;
	g_free (name);

	browse_refresh (data);
}

static void
on_browse_up (GtkButton *button,
              gpointer user_data)
{
	BrowseData *data = user_data;
	char *parent;

	(void) button;

	parent = nemo_rclone_path_parent (data->path);
	g_free (data->path);
	data->path = parent;

	browse_refresh (data);
}

static void
on_browse_select (GtkButton *button,
                  gpointer user_data)
{
	BrowseData *data = user_data;

	(void) button;

	gtk_entry_set_text (GTK_ENTRY (data->target_entry),
	                    data->path != NULL ? data->path : "");
	gtk_widget_destroy (data->dialog);
}

static void
on_browse_home (GtkButton *button,
                gpointer user_data)
{
	BrowseData *data = user_data;

	(void) button;
	g_free (data->path);
	data->path = g_strdup ("");
	browse_refresh (data);
}

static void
open_browse_dialog (RcloneWindow *self,
                    GtkWidget *remote_combo,
                    GtkWidget *path_entry)
{
	BrowseData *data;
	GtkWidget *content;
	GtkWidget *scroll;
	GtkWidget *buttons;
	GtkWidget *home_button;
	GtkWidget *up_button;
	GtkWidget *select_button;
	GtkWidget *cancel_button;
	GtkWidget *vbox;
	char *remote;

	remote = combo_active_text (remote_combo);
	if (remote == NULL) {
		return;
	}

	data = g_new0 (BrowseData, 1);
	data->service = self->service;
	data->remote = remote;
	data->target_entry = path_entry;
	data->path = nemo_rclone_path_canonicalize (entry_path_text (path_entry));

	data->dialog = gtk_dialog_new ();
	gtk_window_set_title (GTK_WINDOW (data->dialog), _("Choose an rclone path"));
	gtk_window_set_transient_for (GTK_WINDOW (data->dialog), GTK_WINDOW (self->window));
	gtk_window_set_modal (GTK_WINDOW (data->dialog), TRUE);
	gtk_window_set_default_size (GTK_WINDOW (data->dialog), 560, 460);

	content = gtk_dialog_get_content_area (GTK_DIALOG (data->dialog));
	vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_set_border_width (GTK_CONTAINER (vbox), 8);
	gtk_box_pack_start (GTK_BOX (content), vbox, TRUE, TRUE, 0);

	data->path_entry = gtk_entry_new ();
	gtk_entry_set_placeholder_text (GTK_ENTRY (data->path_entry),
	                                _("~ = remote home (empty), or a relative/absolute path"));
	gtk_entry_set_text (GTK_ENTRY (data->path_entry),
	                    data->path != NULL ? data->path : "");
	gtk_box_pack_start (GTK_BOX (vbox), data->path_entry, FALSE, FALSE, 0);

	data->store = gtk_list_store_new (1, G_TYPE_STRING);
	data->list_view = gtk_tree_view_new_with_model (GTK_TREE_MODEL (data->store));
	gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (data->list_view), FALSE);
	{
		GtkCellRenderer *renderer = gtk_cell_renderer_text_new ();
		GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes (
			_("Name"), renderer, "text", 0, NULL);
		gtk_tree_view_append_column (GTK_TREE_VIEW (data->list_view), column);
	}
	g_signal_connect (data->list_view, "row-activated",
	                  G_CALLBACK (browse_row_activated), data);

	scroll = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scroll),
	                                     GTK_SHADOW_IN);
	gtk_container_add (GTK_CONTAINER (scroll), data->list_view);
	gtk_box_pack_start (GTK_BOX (vbox), scroll, TRUE, TRUE, 0);

	buttons = gtk_button_box_new (GTK_ORIENTATION_HORIZONTAL);
	gtk_button_box_set_layout (GTK_BUTTON_BOX (buttons), GTK_BUTTONBOX_END);
	gtk_box_pack_start (GTK_BOX (vbox), buttons, FALSE, FALSE, 0);

	up_button = gtk_button_new_with_label (_("Up"));
	g_signal_connect (up_button, "clicked", G_CALLBACK (on_browse_up), data);
	gtk_container_add (GTK_CONTAINER (buttons), up_button);

	home_button = gtk_button_new_with_label (_("Home"));
	gtk_widget_set_tooltip_text (home_button,
	                             _("Jump to the remote's login home (the safe root for Termux)."));
	g_signal_connect (home_button, "clicked", G_CALLBACK (on_browse_home), data);
	gtk_container_add (GTK_CONTAINER (buttons), home_button);

	select_button = gtk_button_new_with_label (_("Select"));
	g_signal_connect (select_button, "clicked", G_CALLBACK (on_browse_select), data);
	gtk_container_add (GTK_CONTAINER (buttons), select_button);

	cancel_button = gtk_button_new_with_label (_("Cancel"));
	g_signal_connect_swapped (cancel_button, "clicked",
	                          G_CALLBACK (gtk_widget_destroy), data->dialog);
	gtk_container_add (GTK_CONTAINER (buttons), cancel_button);

	g_signal_connect_swapped (data->dialog, "destroy",
	                          G_CALLBACK (browse_data_free), data);

	gtk_widget_show_all (data->dialog);
	browse_refresh (data);
}

static void
on_browse_a (GtkButton *button,
             gpointer user_data)
{
	RcloneWindow *self = user_data;

	(void) button;

	open_browse_dialog (self, self->remote_a, self->path_a);
}

static void
on_browse_b (GtkButton *button,
             gpointer user_data)
{
	RcloneWindow *self = user_data;

	(void) button;

	open_browse_dialog (self, self->remote_b, self->path_b);
}

/* ----------------------------- favorites tab ----------------------------- */

static void
reload_favorites (RcloneWindow *self)
{
	GPtrArray *favorites;
	GtkTreeIter iter;
	guint i;

	gtk_list_store_clear (self->favorites_store);

	favorites = nemo_rclone_service_get_favorites (self->service);

	for (i = 0; i < favorites->len; i++) {
		NemoRcloneFavorite *favorite = g_ptr_array_index (favorites, i);

		const char *status;

		status = nemo_rclone_service_favorite_is_mounted (self->service,
		                                                  favorite->id) ?
		                                _("█ Mounted") : _("◌ Offline");

		gtk_list_store_append (self->favorites_store, &iter);
		gtk_list_store_set (self->favorites_store, &iter,
		                    0, favorite->label,
		                    1, favorite->remote,
		                    2, favorite->path,
		                    3, favorite->mountpoint,
		                    4, favorite->id,
		                    5, status,
		                    -1);
	}
}

static gboolean
get_selected_favorite (RcloneWindow *self,
                       char **remote,
                       char **path,
                       char **id)
{
	GtkTreeSelection *selection;
	GtkTreeModel *model;
	GtkTreeIter iter;
	char *r = NULL;
	char *p = NULL;
	char *i = NULL;

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (self->favorites_view));
	if (!gtk_tree_selection_get_selected (selection, &model, &iter)) {
		return FALSE;
	}

	gtk_tree_model_get (model, &iter,
	                    1, &r,
	                    2, &p,
	                    4, &i,
	                    -1);

	if (r == NULL || p == NULL) {
		g_free (r);
		g_free (p);
		g_free (i);
		return FALSE;
	}

	if (remote != NULL) {
		*remote = r;
	} else {
		g_free (r);
	}
	if (path != NULL) {
		*path = p;
	} else {
		g_free (p);
	}
	if (id != NULL) {
		*id = i;
	} else {
		g_free (i);
	}

	return TRUE;
}

static void
use_favorite_for (RcloneWindow *self,
                  GtkWidget *remote_combo,
                  GtkWidget *path_entry)
{
	char *remote = NULL;
	char *path = NULL;

	if (!get_selected_favorite (self, &remote, &path, NULL)) {
		return;
	}

	set_combo_active_text (remote_combo, remote);
	gtk_entry_set_text (GTK_ENTRY (path_entry), path);

	g_free (remote);
	g_free (path);
}

static void
on_favorite_use_a (GtkButton *button,
                   gpointer user_data)
{
	RcloneWindow *self = user_data;

	(void) button;
	use_favorite_for (self, self->remote_a, self->path_a);
}

static void
on_favorite_use_b (GtkButton *button,
                   gpointer user_data)
{
	RcloneWindow *self = user_data;

	(void) button;
	use_favorite_for (self, self->remote_b, self->path_b);
}

static void
on_favorite_remove (GtkButton *button,
                    gpointer user_data)
{
	RcloneWindow *self = user_data;
	char *id = NULL;

	(void) button;

	if (get_selected_favorite (self, NULL, NULL, &id)) {
		nemo_rclone_service_remove_favorite (self->service, id);
		g_free (id);
		reload_favorites (self);
	}
}


/* --------------------------- rclone:// handling --------------------------- */

static void
show_mount_error (GtkWindow *parent,
                  const char *mountpoint,
                  const char *message)
{
	char *tail = mountpoint != NULL ?
		nemo_rclone_service_get_mount_log_tail (mountpoint, 3000) : NULL;
	char *detail = g_strdup_printf ("%s%s%s",
	                                message != NULL ? message : _("unknown error"),
	                                tail != NULL && tail[0] != '\0' ? "\n\n" : "",
	                                tail != NULL ? tail : "");
	GtkWidget *dialog;

	dialog = gtk_message_dialog_new (parent,
	                                 GTK_DIALOG_DESTROY_WITH_PARENT,
	                                 GTK_MESSAGE_ERROR,
	                                 GTK_BUTTONS_CLOSE,
	                                 "%s", _("Rclone mount failed"));
	gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
	                                          "%s", detail);
	gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (dialog);

	g_free (detail);
	g_free (tail);
}

static void
open_local_path (GtkWindow *parent,
                 const char *local)
{
	GFile *location;
	NemoWindowSlot *slot;

	if (!NEMO_IS_WINDOW (parent) || local == NULL) {
		return;
	}

	location = g_file_new_for_path (local);
	slot = nemo_window_get_active_slot (NEMO_WINDOW (parent));

	if (slot != NULL) {
		nemo_window_slot_open_location (slot, location, 0);
	}

	g_object_unref (location);
}

static void
open_local_mount (GtkWindow *parent,
                  const char *mountpoint,
                  const char *subpath)
{
	char *local;

	if (mountpoint == NULL) {
		return;
	}

	if (subpath != NULL && subpath[0] != '\0') {
		local = g_build_filename (mountpoint, subpath, NULL);
	} else {
		local = g_strdup (mountpoint);
	}

	open_local_path (parent, local);
	g_free (local);
}

typedef struct {
	GtkWindow *parent;        /* weak pointer */
	GtkLabel *status_label;   /* owned by parent window */
	char *mountpoint;
	char *subpath;            /* NULL: open the mount root */
	gboolean open_location;
	guint ticks;
} RcloneMountWait;

static void
mount_wait_free (RcloneMountWait *wait)
{
	if (wait->parent != NULL) {
		g_object_remove_weak_pointer (G_OBJECT (wait->parent),
		                              (gpointer *) &wait->parent);
	}
	g_free (wait->mountpoint);
	g_free (wait->subpath);
	g_free (wait);
}

static gboolean
mount_wait_tick (gpointer user_data)
{
	RcloneMountWait *wait = user_data;

	if (wait->parent == NULL) {
		mount_wait_free (wait);
		return G_SOURCE_REMOVE;
	}

	if (nemo_rclone_service_is_mount_active (wait->mountpoint)) {
		if (wait->status_label != NULL) {
			char *msg = g_strdup_printf (_("Mounted at %s"), wait->mountpoint);
			gtk_label_set_text (wait->status_label, msg);
			g_free (msg);
		}
		if (wait->open_location) {
			open_local_mount (wait->parent, wait->mountpoint, wait->subpath);
		}
		mount_wait_free (wait);
		return G_SOURCE_REMOVE;
	}

	if (++wait->ticks >= 100) {
		if (wait->status_label != NULL) {
			gtk_label_set_text (wait->status_label, _("Mount did not become ready."));
		}
		show_mount_error (wait->parent, wait->mountpoint,
		                  _("rclone did not finish mounting within 10 seconds."));
		mount_wait_free (wait);
		return G_SOURCE_REMOVE;
	}

	return G_SOURCE_CONTINUE;
}

static void
mount_wait_start (GtkWindow *parent,
                  GtkLabel *status_label,
                  const char *mountpoint,
                  const char *subpath,
                  gboolean open_location)
{
	RcloneMountWait *wait = g_new0 (RcloneMountWait, 1);

	wait->parent = parent;
	g_object_add_weak_pointer (G_OBJECT (parent), (gpointer *) &wait->parent);
	wait->status_label = status_label;
	wait->mountpoint = g_strdup (mountpoint);
	wait->subpath = subpath != NULL && subpath[0] != '\0' ? g_strdup (subpath) : NULL;
	wait->open_location = open_location;

	if (nemo_rclone_service_is_mount_active (mountpoint)) {
		mount_wait_tick (wait);
		return;
	}

	g_timeout_add (100, mount_wait_tick, wait);
}

/* Find the favorite whose remote path is the longest prefix of path.  The
   remainder is returned in relative_out (caller frees). */
static NemoRcloneFavorite *
find_favorite_for (NemoRcloneService *service,
                   const char *remote,
                   const char *path,
                   char **relative_out)
{
	GPtrArray *favorites = nemo_rclone_service_get_favorites (service);
	NemoRcloneFavorite *best = NULL;
	char *wanted = nemo_rclone_path_canonicalize (path);
	char *best_relative = NULL;
	gsize best_len = 0;
	guint i;

	for (i = 0; i < favorites->len; i++) {
		NemoRcloneFavorite *favorite = g_ptr_array_index (favorites, i);
		char *favorite_path;
		char *relative;
		gsize len;

		if (g_strcmp0 (favorite->remote, remote) != 0) {
			continue;
		}

		favorite_path = nemo_rclone_path_canonicalize (favorite->path);
		relative = nemo_rclone_path_relative (wanted, favorite_path);
		if (relative == NULL) {
			g_free (favorite_path);
			continue;
		}

		len = strlen (favorite_path);
		if (best == NULL || len > best_len) {
			best = favorite;
			best_len = len;
			g_free (best_relative);
			best_relative = relative;
		} else {
			g_free (relative);
		}

		g_free (favorite_path);
	}

	if (best != NULL && relative_out != NULL) {
		*relative_out = best_relative;
	} else {
		g_free (best_relative);
	}

	g_free (wanted);
	return best;
}

void
nemo_rclone_open_uri (GtkWindow *parent,
                      const char *uri)
{
	NemoRcloneService *service;
	NemoRcloneFavorite *favorite;
	char *rest;
	char *trimmed;
	char *remote = NULL;
	char *path = NULL;
	char *relative = NULL;
	char *mountpoint;

	if (uri == NULL || !g_str_has_prefix (uri, "rclone:")) {
		return;
	}

	rest = g_strdup (uri + strlen ("rclone:"));
	while (rest[0] == '/') {
		memmove (rest, rest + 1, strlen (rest));
	}

	trimmed = g_strdup (rest);
	while (trimmed[0] != '\0' && trimmed[strlen (trimmed) - 1] == '/') {
		trimmed[strlen (trimmed) - 1] = '\0';
	}
	g_free (rest);

	if (trimmed[0] == '\0' || g_strcmp0 (trimmed, "transfer") == 0 ||
	    g_strcmp0 (trimmed, "transfers") == 0) {
		g_free (trimmed);
		nemo_rclone_window_show (parent);
		return;
	}

	{
		char *slash = strchr (trimmed, '/');
		char *escaped_remote;
		char *escaped_path;

		if (slash == NULL) {
			escaped_remote = g_strdup (trimmed);
			escaped_path = g_strdup ("");
		} else {
			escaped_remote = g_strndup (trimmed, slash - trimmed);
			escaped_path = g_strdup (slash);
		}

		remote = g_uri_unescape_string (escaped_remote, NULL);
		path = g_uri_unescape_string (escaped_path, NULL);

		if (remote == NULL) {
			remote = g_strdup (escaped_remote);
		}
		if (path == NULL) {
			path = g_strdup (escaped_path);
		}

		g_free (escaped_remote);
		g_free (escaped_path);
	}
	g_free (trimmed);

	/* URI spelling:
	   rclone://remote          -> remote home (default root)
	   rclone://remote/rel      -> relative to home
	   rclone://remote//abs     -> absolute server path
	*/
	if (path[0] == '/') {
		if (path[1] == '/') {
			/* Keep the leading slash, drop the URI separator. */
			memmove (path, path + 1, strlen (path));
		} else {
			/* A single slash is the URI path separator, not part of
			   the remote path. */
			memmove (path, path + 1, strlen (path));
		}
	}

	{
		char *canonical = nemo_rclone_path_canonicalize (path);
		g_free (path);
		path = canonical;
	}

	if (remote[0] == '\0') {
		g_free (remote);
		g_free (path);
		nemo_rclone_window_show (parent);
		return;
	}

	service = nemo_rclone_service_get_default ();

	/* Already available through any active mount (including one started
	   outside Nemo, or a broader mount that contains this path). */
	{
		char *local = nemo_rclone_service_remote_to_local (service, remote, path, NULL);

		if (local != NULL) {
			open_local_path (parent, local);
			g_free (local);
			g_free (remote);
			g_free (path);
			return;
		}
	}

	favorite = find_favorite_for (service, remote, path, &relative);

	if (favorite == NULL) {
		/* No saved favorite: open the transfer window with this source
		   pre-filled instead of failing to open the location. */
		nemo_rclone_window_show_with_source (parent, remote, path);
		g_free (remote);
		g_free (path);
		return;
	}

	{
		GError *error = NULL;

		mountpoint = nemo_rclone_service_mount_favorite (service, favorite->id, &error);
		if (mountpoint == NULL) {
			show_mount_error (parent, favorite->mountpoint,
			                  error != NULL ? error->message : _("unknown error"));
			g_clear_error (&error);
		} else {
			mount_wait_start (parent, NULL, mountpoint, relative, TRUE);
			g_free (mountpoint);
		}
	}

	g_free (relative);
	g_free (remote);
	g_free (path);
}

static void
on_favorite_mount (GtkButton *button,
                   gpointer user_data)
{
	RcloneWindow *self = user_data;
	char *id = NULL, *mountpoint = NULL;
	GError *error = NULL;

	(void) button;

	if (!get_selected_favorite (self, NULL, NULL, &id)) {
		return;
	}

	if (nemo_rclone_service_favorite_is_mounted (self->service, id)) {
		gtk_label_set_text (GTK_LABEL (self->status_label), _("Already mounted."));
		g_free (id);
		return;
	}

	mountpoint = nemo_rclone_service_mount_favorite (self->service, id, &error);
	if (mountpoint == NULL) {
		char *msg = g_strdup_printf (_("Mount failed: %s"),
		                             error != NULL ? error->message : _("unknown error"));
		gtk_label_set_text (GTK_LABEL (self->status_label), msg);
		g_free (msg);
		if (error != NULL) {
			g_error_free (error);
		}
	} else {
		char *msg = g_strdup_printf (_("Mounting at %s…"), mountpoint);
		gtk_label_set_text (GTK_LABEL (self->status_label), msg);
		g_free (msg);
		mount_wait_start (GTK_WINDOW (self->window), GTK_LABEL (self->status_label),
		                  mountpoint, NULL, FALSE);
		g_free (mountpoint);
		reload_favorites (self);
	}

	g_free (id);
}

static void
on_favorite_unmount (GtkButton *button,
                     gpointer user_data)
{
	RcloneWindow *self = user_data;
	char *id = NULL;
	GError *error = NULL;

	(void) button;

	if (!get_selected_favorite (self, NULL, NULL, &id)) {
		return;
	}

	if (!nemo_rclone_service_unmount_favorite (self->service, id, &error)) {
		char *msg = g_strdup_printf (_("Unmount failed: %s"),
		                             error != NULL ? error->message : _("unknown error"));
		gtk_label_set_text (GTK_LABEL (self->status_label), msg);
		g_free (msg);
		if (error != NULL) {
			g_error_free (error);
		}
	} else {
		gtk_label_set_text (GTK_LABEL (self->status_label), _("Unmounted."));
		reload_favorites (self);
	}

	g_free (id);
}

static void
on_favorite_open (GtkButton *button,
                  gpointer user_data)
{
	RcloneWindow *self = user_data;
	char *remote = NULL, *path = NULL, *id = NULL;
	char *uri;

	(void) button;

	if (!get_selected_favorite (self, &remote, &path, &id)) {
		return;
	}

	uri = nemo_rclone_service_format_uri (remote, path);
	nemo_rclone_open_uri (GTK_WINDOW (self->window), uri);

	g_free (uri);
	g_free (remote);
	g_free (path);
	g_free (id);
}

static void
on_favorite_add (GtkButton *button,
                 gpointer user_data)
{
	RcloneWindow *self = user_data;

	(void) button;

	nemo_rclone_service_prompt_add_favorite (self->service,
	                                         GTK_WINDOW (self->window),
	                                         NULL, NULL, NULL);
	reload_favorites (self);
}

static void
on_favorite_row_activated (GtkTreeView *view,
                           GtkTreePath *path,
                           GtkTreeViewColumn *column,
                           gpointer user_data)
{
	RcloneWindow *self = user_data;

	(void) view;
	(void) path;
	(void) column;

	use_favorite_for (self, self->remote_a, self->path_a);
}

static void
on_mark_a_favorite (GtkButton *button,
                    gpointer user_data)
{
	RcloneWindow *self = user_data;
	char *remote = combo_active_text (self->remote_a);
	const char *path = gtk_entry_get_text (GTK_ENTRY (self->path_a));

	(void) button;

	if (remote != NULL && path[0] != '\0') {
		nemo_rclone_service_prompt_add_favorite (self->service,
		                                         GTK_WINDOW (self->window),
		                                         NULL, remote, path);
		reload_favorites (self);
	}

	g_free (remote);
}

static void
on_mark_b_favorite (GtkButton *button,
                    gpointer user_data)
{
	RcloneWindow *self = user_data;
	char *remote = combo_active_text (self->remote_b);
	const char *path = gtk_entry_get_text (GTK_ENTRY (self->path_b));

	(void) button;

	if (remote != NULL && path[0] != '\0') {
		nemo_rclone_service_prompt_add_favorite (self->service,
		                                         GTK_WINDOW (self->window),
		                                         NULL, remote, path);
		reload_favorites (self);
	}

	g_free (remote);
}

static gboolean
on_delete_event (GtkWidget *widget,
                 GdkEvent *event,
                 gpointer user_data)
{
	RcloneWindow *self = user_data;

	(void) widget;
	(void) event;

	if (self->running) {
		GtkWidget *dialog = gtk_message_dialog_new (
			GTK_WINDOW (self->window), GTK_DIALOG_MODAL,
			GTK_MESSAGE_WARNING, GTK_BUTTONS_CLOSE,
			_("An rclone transfer is still running. Cancel it before closing."));

		gtk_dialog_run (GTK_DIALOG (dialog));
		gtk_widget_destroy (dialog);
		return TRUE;
	}

	return FALSE;
}

static GtkWidget *
build_transfer_tab (RcloneWindow *self)
{
	GtkWidget *grid;
	GtkWidget *label;
	GtkWidget *browse_a;
	GtkWidget *browse_b;
	GtkWidget *mark_a;
	GtkWidget *mark_b;
	GtkWidget *scroll;
	GtkWidget *log_view;

	grid = gtk_grid_new ();
	gtk_grid_set_row_spacing (GTK_GRID (grid), 8);
	gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
	gtk_container_set_border_width (GTK_CONTAINER (grid), 10);

	label = gtk_label_new (_("Source (A):"));
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 1, 1);

	self->remote_a = gtk_combo_box_text_new ();
	gtk_widget_set_hexpand (self->remote_a, TRUE);
	gtk_grid_attach (GTK_GRID (grid), self->remote_a, 1, 0, 1, 1);

	self->path_a = gtk_entry_new ();
	gtk_entry_set_placeholder_text (GTK_ENTRY (self->path_a),
	                                _("~ or path (empty = remote home)"));
	gtk_widget_set_hexpand (self->path_a, TRUE);
	gtk_grid_attach (GTK_GRID (grid), self->path_a, 2, 0, 1, 1);

	browse_a = gtk_button_new_with_label (_("Browse…"));
	gtk_grid_attach (GTK_GRID (grid), browse_a, 3, 0, 1, 1);

	mark_a = gtk_button_new_with_label ("★");
	gtk_grid_attach (GTK_GRID (grid), mark_a, 4, 0, 1, 1);

	label = gtk_label_new (_("Destination (B):"));
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_grid_attach (GTK_GRID (grid), label, 0, 1, 1, 1);

	self->remote_b = gtk_combo_box_text_new ();
	gtk_widget_set_hexpand (self->remote_b, TRUE);
	gtk_grid_attach (GTK_GRID (grid), self->remote_b, 1, 1, 1, 1);

	self->path_b = gtk_entry_new ();
	gtk_entry_set_placeholder_text (GTK_ENTRY (self->path_b),
	                                _("~ or path (empty = remote home)"));
	gtk_widget_set_hexpand (self->path_b, TRUE);
	gtk_grid_attach (GTK_GRID (grid), self->path_b, 2, 1, 1, 1);

	browse_b = gtk_button_new_with_label (_("Browse…"));
	gtk_grid_attach (GTK_GRID (grid), browse_b, 3, 1, 1, 1);

	mark_b = gtk_button_new_with_label ("★");
	gtk_grid_attach (GTK_GRID (grid), mark_b, 4, 1, 1, 1);

	label = gtk_label_new (_("Operation:"));
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_grid_attach (GTK_GRID (grid), label, 0, 2, 1, 1);

	self->mode_combo = gtk_combo_box_text_new ();
	gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (self->mode_combo), "copy");
	gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (self->mode_combo), "copyto");
	gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (self->mode_combo), "move");
	gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (self->mode_combo), "sync");
	gtk_combo_box_set_active (GTK_COMBO_BOX (self->mode_combo), 0);
	gtk_grid_attach (GTK_GRID (grid), self->mode_combo, 1, 2, 1, 1);

	self->dry_run_check = gtk_check_button_new_with_label (_("Dry run"));
	gtk_grid_attach (GTK_GRID (grid), self->dry_run_check, 2, 2, 1, 1);

	self->start_button = gtk_button_new_with_label (_("Start"));
	self->cancel_button = gtk_button_new_with_label (_("Cancel"));
	gtk_widget_set_sensitive (self->cancel_button, FALSE);
	gtk_grid_attach (GTK_GRID (grid), self->start_button, 3, 2, 1, 1);
	gtk_grid_attach (GTK_GRID (grid), self->cancel_button, 4, 2, 1, 1);

	self->progress = gtk_progress_bar_new ();
	gtk_progress_bar_set_show_text (GTK_PROGRESS_BAR (self->progress), FALSE);
	gtk_grid_attach (GTK_GRID (grid), self->progress, 0, 3, 5, 1);

	self->status_label = gtk_label_new (_("Choose a source and destination."));
	gtk_label_set_xalign (GTK_LABEL (self->status_label), 0.0);
	gtk_label_set_ellipsize (GTK_LABEL (self->status_label), PANGO_ELLIPSIZE_END);
	gtk_grid_attach (GTK_GRID (grid), self->status_label, 0, 4, 5, 1);

	log_view = gtk_text_view_new ();
	gtk_text_view_set_editable (GTK_TEXT_VIEW (log_view), FALSE);
	gtk_text_view_set_monospace (GTK_TEXT_VIEW (log_view), TRUE);
	gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (log_view), GTK_WRAP_WORD_CHAR);
	self->log_buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (log_view));

	scroll = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scroll), GTK_SHADOW_IN);
	gtk_container_add (GTK_CONTAINER (scroll), log_view);
	gtk_widget_set_size_request (scroll, -1, 220);
	gtk_grid_attach (GTK_GRID (grid), scroll, 0, 5, 5, 1);

	g_signal_connect (browse_a, "clicked", G_CALLBACK (on_browse_a), self);
	g_signal_connect (browse_b, "clicked", G_CALLBACK (on_browse_b), self);

	g_signal_connect (mark_a, "clicked", G_CALLBACK (on_mark_a_favorite), self);
	g_signal_connect (mark_b, "clicked", G_CALLBACK (on_mark_b_favorite), self);
	g_signal_connect (self->start_button, "clicked", G_CALLBACK (on_start_clicked), self);
	g_signal_connect (self->cancel_button, "clicked", G_CALLBACK (on_cancel_clicked), self);

	self->job_progress = self->progress;
	self->job_status = self->status_label;
	self->job_log = self->log_buffer;

	return grid;
}

static GtkWidget *
build_favorites_tab (RcloneWindow *self)
{
	GtkWidget *vbox;
	GtkWidget *scroll;
	GtkWidget *buttons;
	GtkWidget *add_button;
	GtkWidget *remove_button;
	GtkWidget *use_a_button;
	GtkWidget *use_b_button;
	GtkWidget *mount_button;
	GtkWidget *unmount_button;
	GtkWidget *open_button;
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *column;

	vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_set_border_width (GTK_CONTAINER (vbox), 10);

	self->favorites_store = gtk_list_store_new (6,
	                                            G_TYPE_STRING,
	                                            G_TYPE_STRING,
	                                            G_TYPE_STRING,
	                                            G_TYPE_STRING,
	                                            G_TYPE_STRING,
	                                            G_TYPE_STRING);

	self->favorites_view = gtk_tree_view_new_with_model (
		GTK_TREE_MODEL (self->favorites_store));
	gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (self->favorites_view), TRUE);

	renderer = gtk_cell_renderer_text_new ();
	column = gtk_tree_view_column_new_with_attributes (_("Label"), renderer,
	                                                   "text", 0, NULL);
	gtk_tree_view_column_set_expand (column, TRUE);
	gtk_tree_view_append_column (GTK_TREE_VIEW (self->favorites_view), column);

	renderer = gtk_cell_renderer_text_new ();
	column = gtk_tree_view_column_new_with_attributes (_("Remote"), renderer,
	                                                   "text", 1, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (self->favorites_view), column);

	renderer = gtk_cell_renderer_text_new ();
	column = gtk_tree_view_column_new_with_attributes (_("Path"), renderer,
	                                                   "text", 2, NULL);
	gtk_tree_view_column_set_expand (column, TRUE);
	gtk_tree_view_append_column (GTK_TREE_VIEW (self->favorites_view), column);

	renderer = gtk_cell_renderer_text_new ();
	column = gtk_tree_view_column_new_with_attributes (_("Status"), renderer,
	                                                   "text", 5, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (self->favorites_view), column);

	g_signal_connect (self->favorites_view, "row-activated",
	                  G_CALLBACK (on_favorite_row_activated), self);

	scroll = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scroll), GTK_SHADOW_IN);
	gtk_container_add (GTK_CONTAINER (scroll), self->favorites_view);
	gtk_box_pack_start (GTK_BOX (vbox), scroll, TRUE, TRUE, 0);

	buttons = gtk_button_box_new (GTK_ORIENTATION_HORIZONTAL);
	gtk_button_box_set_layout (GTK_BUTTON_BOX (buttons), GTK_BUTTONBOX_START);
	gtk_box_pack_start (GTK_BOX (vbox), buttons, FALSE, FALSE, 0);

	add_button = gtk_button_new_with_label (_("Add…"));
	gtk_container_add (GTK_CONTAINER (buttons), add_button);

	remove_button = gtk_button_new_with_label (_("Remove"));
	gtk_container_add (GTK_CONTAINER (buttons), remove_button);

	use_a_button = gtk_button_new_with_label (_("Use as A"));
	gtk_container_add (GTK_CONTAINER (buttons), use_a_button);

	use_b_button = gtk_button_new_with_label (_("Use as B"));
	gtk_container_add (GTK_CONTAINER (buttons), use_b_button);

	mount_button = gtk_button_new_with_label (_("Mount"));
	gtk_container_add (GTK_CONTAINER (buttons), mount_button);
	unmount_button = gtk_button_new_with_label (_("Unmount"));
	gtk_container_add (GTK_CONTAINER (buttons), unmount_button);
	open_button = gtk_button_new_with_label (_("Open"));
	gtk_container_add (GTK_CONTAINER (buttons), open_button);

	g_signal_connect (add_button, "clicked", G_CALLBACK (on_favorite_add), self);
	g_signal_connect (remove_button, "clicked", G_CALLBACK (on_favorite_remove), self);
	g_signal_connect (use_a_button, "clicked", G_CALLBACK (on_favorite_use_a), self);
	g_signal_connect (use_b_button, "clicked", G_CALLBACK (on_favorite_use_b), self);
	g_signal_connect (mount_button, "clicked", G_CALLBACK (on_favorite_mount), self);
	g_signal_connect (unmount_button, "clicked", G_CALLBACK (on_favorite_unmount), self);
	g_signal_connect (open_button, "clicked", G_CALLBACK (on_favorite_open), self);

	return vbox;
}

static void
on_window_destroy (GtkWidget *widget,
                   gpointer user_data)
{
	RcloneWindow *self = user_data;

	(void) widget;

	if (self->status_timer != 0) {
		g_source_remove (self->status_timer);
		self->status_timer = 0;
	}

	if (rclone_window == self) {
		rclone_window = NULL;
	}

	g_free (self);
}

static gboolean
favorites_status_tick (gpointer user_data)
{
	RcloneWindow *self = user_data;

	if (self->window != NULL && gtk_widget_get_visible (self->window)) {
		reload_favorites (self);
	}

	return G_SOURCE_CONTINUE;
}

static RcloneWindow *
rclone_window_new (GtkWindow *parent)
{
	RcloneWindow *self;
	GtkWidget *notebook;
	GtkWidget *transfer_tab;
	GtkWidget *favorites_tab;
	GPtrArray *remotes;
	guint i;

	self = g_new0 (RcloneWindow, 1);
	self->service = nemo_rclone_service_get_default ();
	self->parent = parent;

	self->window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title (GTK_WINDOW (self->window), _("Rclone"));
	gtk_window_set_default_size (GTK_WINDOW (self->window), 820, 600);
	gtk_window_set_transient_for (GTK_WINDOW (self->window), parent);
	gtk_window_set_destroy_with_parent (GTK_WINDOW (self->window), FALSE);
	gtk_window_set_type_hint (GTK_WINDOW (self->window), GDK_WINDOW_TYPE_HINT_DIALOG);

	notebook = gtk_notebook_new ();
	gtk_container_add (GTK_CONTAINER (self->window), notebook);

	transfer_tab = build_transfer_tab (self);
	gtk_notebook_append_page (GTK_NOTEBOOK (notebook), transfer_tab,
	                          gtk_label_new (_("Transfer")));

	favorites_tab = build_favorites_tab (self);
	gtk_notebook_append_page (GTK_NOTEBOOK (notebook), favorites_tab,
	                          gtk_label_new (_("Favorites")));

	remotes = nemo_rclone_service_get_remotes (self->service);

	for (i = 0; i < remotes->len; i++) {
		const char *name = g_ptr_array_index (remotes, i);

		gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (self->remote_a), name);
		gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (self->remote_b), name);
	}

	if (remotes->len > 0) {
		gtk_combo_box_set_active (GTK_COMBO_BOX (self->remote_a), 0);
		gtk_combo_box_set_active (GTK_COMBO_BOX (self->remote_b), 0);
	}

	reload_favorites (self);
	self->status_timer = g_timeout_add_seconds (2, favorites_status_tick, self);

	g_signal_connect (self->window, "delete-event",
	                  G_CALLBACK (on_delete_event), self);
	g_signal_connect (self->window, "destroy",
	                  G_CALLBACK (on_window_destroy), self);

	gtk_widget_show_all (self->window);

	return self;
}

static void
set_source (RcloneWindow *self,
            const char *remote,
            const char *path)
{
	if (remote != NULL) {
		set_combo_active_text (self->remote_a, remote);
	}

	if (path != NULL) {
		gtk_entry_set_text (GTK_ENTRY (self->path_a), path);
	}
}

void
nemo_rclone_window_show (GtkWindow *parent)
{
	if (rclone_window == NULL) {
		rclone_window = rclone_window_new (parent);
	}

	gtk_window_present (GTK_WINDOW (rclone_window->window));
}

void
nemo_rclone_window_show_with_source (GtkWindow *parent,
                                     const char *remote,
                                     const char *path)
{
	nemo_rclone_window_show (parent);

	if (rclone_window != NULL) {
		set_source (rclone_window, remote, path);
	}
}

void
nemo_rclone_window_show_with_selection (GtkWindow *parent,
                                        const char *remote,
                                        const char *path)
{
	nemo_rclone_window_show_with_source (parent, remote, path);
}
