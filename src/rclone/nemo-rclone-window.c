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
#include "nemo-rclone-job-manager.h"
#include <libnemo-private/nemo-progress-info.h>
#include "nemo-window.h"
#include "nemo-window-slot.h"

#include <gio/gio.h>
#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdlib.h>

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

	GtkWidget *notebook;
	guint jobs_tab_index;
	GtkWidget *jobs_summary;

	NemoRcloneJobManager *job_manager;

	GtkWidget *jobs_view;
	GtkListStore *jobs_store;
	GtkWidget *jobs_detail_view;
	GtkTextBuffer *jobs_detail_buffer;
	GtkWidget *jobs_description;
	GtkWidget *jobs_state;
	GtkWidget *jobs_progress;
	GtkWidget *jobs_speed;
	guint jobs_selected;
	guint jobs_timer;

	/* Jobs-page settings (persisted, see jobs_settings_load/save). */
	int jobs_tick_rate;      /* progress refresh rate, 1..4 Hz (default 1) */
	gboolean jobs_show_state;
	gboolean jobs_show_progress;
	gboolean jobs_show_speed;
	gboolean jobs_show_job;
	gboolean jobs_auto_clear;
} RcloneWindow;

static RcloneWindow *rclone_window = NULL;

/* Kinds of rows in the Jobs list. */
enum {
	JOB_KIND_RCLONE,  /* a rclone transfer (cancellable via the job API) */
	JOB_KIND_NEMO     /* a Nemo file operation (local / mount copy) */
};

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
jobs_refresh (RcloneWindow *self,
              const char *select_id);

/* ------------------------------ Jobs page ------------------------------ */

/* Per-state foreground color (CSS color name) used to paint the State
   column of the Jobs list.  Gives each job a colored indicator without
   needing per-state icon resources. */
static const char *
jobs_state_color (NemoRcloneJobState state)
{
	switch (state) {
	case NEMO_RCLONE_JOB_RUNNING:
	case NEMO_RCLONE_JOB_QUEUED:
		return "#2f6fed";	/* blue */
	case NEMO_RCLONE_JOB_DONE:
		return "#2ecc71";	/* green */
	case NEMO_RCLONE_JOB_FAILED:
		return "#e74c3c";	/* red */
	case NEMO_RCLONE_JOB_CANCELLED:
		return "#e67e22";	/* orange */
	default:
		return "#7f8c8d";	/* grey */
	}
}

/* Unicode glyph used as an inline "icon" prefix for a state, so the
   Jobs list has a small visual indicator per row. */
static const char *
jobs_state_glyph (NemoRcloneJobState state)
{
	switch (state) {
	case NEMO_RCLONE_JOB_QUEUED:
		return "•";
	case NEMO_RCLONE_JOB_RUNNING:
		return "▶";
	case NEMO_RCLONE_JOB_DONE:
		return "✔";
	case NEMO_RCLONE_JOB_FAILED:
		return "✗";
	case NEMO_RCLONE_JOB_CANCELLED:
		return "⊘";
	default:
		return "•";
	}
}

/* Build a displayable State string: the state glyph followed by the
   human-readable label, e.g. "▶ Running".  Owned by the caller. */
static char *
jobs_state_display (NemoRcloneJobState state, const char *label)
{
	return g_strdup_printf ("%s %s",
	                         jobs_state_glyph (state),
	                         label != NULL ? label : "…");
}

/* The Nemo file-op progress indicator never maps to a JobState, so map
   the current fraction onto one of the known buckets for coloring. */
static const char *
jobs_nemo_state_color (double progress, gboolean running)
{
	if (progress >= 1.0)
		return "#2ecc71";	/* done -> green */
	if (!running)
		return "#7f8c8d";	/* not yet started -> grey */
	return "#2f6fed";		/* running -> blue */
}

/* Default jobs-page settings applied before any saved config is read. */
static void
jobs_settings_defaults (RcloneWindow *self)
{
	self->jobs_tick_rate = 1;
	self->jobs_show_state = TRUE;
	self->jobs_show_progress = TRUE;
	self->jobs_show_speed = TRUE;
	self->jobs_show_job = TRUE;
	self->jobs_auto_clear = FALSE;
}

static char *
jobs_settings_path (void)
{
	char *config_dir;
	char *path;

	config_dir = g_build_filename (g_get_user_config_dir (), "nemo", NULL);
	path = g_build_filename (config_dir, "rclone-jobs.json", NULL);
	g_free (config_dir);
	return path;
}

/* Load saved jobs settings; missing/malformed values fall back to
   defaults.  Kept tiny: a single JSON object with six keys. */
static void
jobs_settings_load (RcloneWindow *self)
{
	char *path;
	char *contents;
	JsonParser *parser;
	JsonNode *root;
	JsonObject *object;

	jobs_settings_defaults (self);

	path = jobs_settings_path ();
	if (!g_file_get_contents (path, &contents, NULL, NULL)) {
		g_free (path);
		return;
	}
	g_free (path);

	parser = json_parser_new ();
	if (!json_parser_load_from_data (parser, contents, -1, NULL)) {
		g_free (contents);
		g_object_unref (parser);
		return;
	}
	g_free (contents);

	root = json_parser_get_root (parser);
	if (root != NULL && JSON_NODE_HOLDS_OBJECT (root)) {
		object = json_node_get_object (root);

		if (json_object_has_member (object, "tick_rate_hz")) {
			gint v = json_object_get_int_member (object,
			                                     "tick_rate_hz");
			if (v >= 1 && v <= 4)
				self->jobs_tick_rate = v;
		}
		self->jobs_show_state =
			json_object_get_boolean_member_with_default (object,
			                                "show_state",
			                                TRUE);
		self->jobs_show_progress =
			json_object_get_boolean_member_with_default (object,
			                                "show_progress",
			                                TRUE);
		self->jobs_show_speed =
			json_object_get_boolean_member_with_default (object,
			                                "show_speed",
			                                TRUE);
		self->jobs_show_job =
			json_object_get_boolean_member_with_default (object,
			                                "show_job",
			                                TRUE);
		self->jobs_auto_clear =
			json_object_get_boolean_member_with_default (object,
			                                "auto_clear",
			                                FALSE);
	}

	g_object_unref (parser);
}

/* Persist current jobs settings back to the config file. */
static void
jobs_settings_save (RcloneWindow *self)
{
	JsonBuilder *builder;
	JsonGenerator *gen;
	JsonNode *root;
	char *path;
	char *contents;

	builder = json_builder_new ();
	json_builder_begin_object (builder);
	json_builder_set_member_name (builder, "tick_rate_hz");
	json_builder_add_int_value (builder, self->jobs_tick_rate);
	json_builder_set_member_name (builder, "show_state");
	json_builder_add_boolean_value (builder, self->jobs_show_state);
	json_builder_set_member_name (builder, "show_progress");
	json_builder_add_boolean_value (builder, self->jobs_show_progress);
	json_builder_set_member_name (builder, "show_speed");
	json_builder_add_boolean_value (builder, self->jobs_show_speed);
	json_builder_set_member_name (builder, "show_job");
	json_builder_add_boolean_value (builder, self->jobs_show_job);
	json_builder_set_member_name (builder, "auto_clear");
	json_builder_add_boolean_value (builder, self->jobs_auto_clear);
	json_builder_end_object (builder);

	root = json_builder_get_root (builder);
	gen = json_generator_new ();
	json_generator_set_root (gen, root);
	contents = json_generator_to_data (gen, NULL);

	path = jobs_settings_path ();
	{
		char *config_dir = g_path_get_dirname (path);
		g_mkdir_with_parents (config_dir, 0700);
		g_free (config_dir);
	}
	g_file_set_contents (path, contents, -1, NULL);

	g_free (contents);
	g_free (path);
	g_object_unref (gen);
	g_object_unref (root);
	g_object_unref (builder);
}

/*  0: G_TYPE_STRING   row id ("r:<uuid>" or "n:<n>")
    1: G_TYPE_STRING   state label
    2: G_TYPE_STRING   formatted progress ("45%")
    3: G_TYPE_STRING   formatted speed ("1.2 MB/s")
    4: G_TYPE_STRING   "source → destination" description
    5: G_TYPE_BOOLEAN  whether this is a rclone job (else a Nemo op)
    6: G_TYPE_POINTER  rclone job pointer (NULL for Nemo ops)
    7: G_TYPE_POINTER  NemoProgressInfo pointer (NULL for rclone jobs)
    8: G_TYPE_STRING   foreground CSS color name (paints the state cell)
    9: G_TYPE_STRING   state glyph (inline visual indicator)
*/
static void
jobs_refresh (RcloneWindow *self,
              const char *select_id)
{
	NemoRcloneJobManager *manager;
	GtkListStore *store;
	GtkTreeIter iter;
	guint selected_row = 0;
	gboolean did_select = FALSE;
	GList *l;
	guint row = 0;
	guint nemo_counter = 0;

	manager = self->job_manager;
	store = self->jobs_store;

	if (store == NULL) {
		return;
	}

	gtk_list_store_clear (store);

	/* rclone jobs, newest first (get_jobs already returns newest first). */
	if (manager != NULL) {
		GPtrArray *jobs = nemo_rclone_job_manager_get_jobs (manager);
		guint i;

		for (i = jobs->len; i-- > 0; ) {
			NemoRcloneJob *job = g_ptr_array_index (jobs, i);
			const char *src = nemo_rclone_job_get_src (job);
			const char *dst = nemo_rclone_job_get_dst (job);
			double frac = nemo_rclone_job_get_fraction (job);
			double speed = nemo_rclone_job_get_speed (job);
			const char *state = nemo_rclone_job_get_state_label (job);
			NemoRcloneJobState jstate = nemo_rclone_job_get_state (job);
			char *desc = NULL;
			char *progress = NULL;
			char *spd = NULL;

			if (src != NULL && dst != NULL) {
				desc = g_strdup_printf ("%s → %s", src, dst);
			} else {
				desc = g_strdup (job != NULL ? nemo_rclone_job_get_id (job) : "?");
			}

			if (frac >= 0.0) {
				progress = g_strdup_printf ("%ld%%",
				                           (long) (frac * 100.0 + 0.5));
			} else {
				progress = g_strdup ("…");
			}

			if (speed > 0) {
				spd = g_strdup_printf ("%s/s", g_format_size ((guint64) speed));
			}

			gtk_list_store_append (store, &iter);
			gtk_list_store_set (store, &iter,
			                    0, nemo_rclone_job_get_id (job),
			                    1, jobs_state_display (jstate,
					                      state),
			                    2, progress,
			                    3, spd != NULL ? spd : "",
			                    4, desc,
			                    5, TRUE,
			                    6, job,
			                    7, NULL,
			                    8, jobs_state_color (jstate),
			                    9, jobs_state_glyph (jstate),
			                    -1);
			g_free (desc);
			g_free (progress);
			g_free (spd);

			if (select_id != NULL &&
			    g_strcmp0 (nemo_rclone_job_get_id (job), select_id) == 0) {
				selected_row = row;
				did_select = TRUE;
			}
			row++;
		}
	}

	/* Nemo's own file operations (local / FUSE copies), so the Jobs page
	   shows them alongside rclone jobs. */
	for (l = nemo_rclone_job_manager_get_nemo_infos (manager); l != NULL; l = l->next) {
		NemoProgressInfo *info = NEMO_PROGRESS_INFO (l->data);
		double progress = nemo_progress_info_get_progress (info);
		char *state = nemo_progress_info_get_status (info);
		char *desc = NULL;
		char *progress_txt = NULL;
		char *spd = NULL;

		if (progress < 0) {
			progress = 0.0;
		}

		desc = g_strdup_printf (_("File operation: %s"),
		                        state != NULL ? state : _("running"));

		if (progress >= 1.0) {
			progress_txt = g_strdup (_("Done"));
		} else {
			progress_txt = g_strdup_printf ("%ld%%",
			                               (long) (progress * 100.0 + 0.5));
		}

		gtk_list_store_append (store, &iter);
		gtk_list_store_set (store, &iter,
		                    0, g_strdup_printf ("n:%u", nemo_counter++),
		                    1, jobs_state_display (progress >= 1.0 ?
					                      NEMO_RCLONE_JOB_DONE :
					                      NEMO_RCLONE_JOB_RUNNING,
					                      state),
		                    2, progress_txt,
		                    3, spd != NULL ? spd : "",
		                    4, desc,
		                    5, FALSE,
		                    6, NULL,
		                    7, info,
		                    8, jobs_nemo_state_color (progress, TRUE),
		                    9, jobs_state_glyph (progress >= 1.0 ?
		                                         NEMO_RCLONE_JOB_DONE :
		                                         NEMO_RCLONE_JOB_RUNNING),
		                    -1);
		g_free (state);
		g_free (desc);
		g_free (progress_txt);
		g_free (spd);
		row++;
	}

	/* Select the row we were told to, or fall back to the first row. */
	if (did_select) {
		GtkTreeIter sel_iter;
		GtkTreeSelection *sel = gtk_tree_view_get_selection (
			GTK_TREE_VIEW (self->jobs_view));

		gtk_tree_model_iter_nth_child (GTK_TREE_MODEL (store), &sel_iter,
		                               NULL, selected_row);
		gtk_tree_selection_select_iter (sel, &sel_iter);
		{
			char *idx = g_strdup_printf ("%u", selected_row);
			GtkTreePath *path = gtk_tree_path_new_from_string (idx);

			gtk_tree_view_scroll_to_cell (GTK_TREE_VIEW (self->jobs_view),
			                              path, NULL, FALSE, 0.5, 0.0);
			gtk_tree_path_free (path);
			g_free (idx);
		}
	} else {
		GtkTreeIter first;

		if (gtk_tree_model_get_iter_first (GTK_TREE_MODEL (store), &first)) {
			GtkTreeSelection *sel = gtk_tree_view_get_selection (
				GTK_TREE_VIEW (self->jobs_view));
			gtk_tree_selection_select_iter (sel, &first);
		}
	}
}

/* Live-update progress / speed of rows without rebuilding the store, so we
   don't disturb the current selection or scroll position every tick. */
static void
jobs_update_live (RcloneWindow *self)
{
	GtkTreeModel *model = GTK_TREE_MODEL (self->jobs_store);
	GtkTreeIter iter;

	if (self->jobs_store == NULL) {
		return;
	}

	while (gtk_tree_model_iter_next (model, &iter)) {
		guint kind;
		gpointer job, info;
		char *progress = NULL;
		char *spd = NULL;
		char *color = NULL;
		char *glyph = NULL;

		gtk_tree_model_get (model, &iter, 5, &kind, 6, &job, 7, &info, -1);

		if (kind == JOB_KIND_RCLONE && job != NULL) {
			NemoRcloneJob *rjob = NEMO_RCLONE_JOB (job);
			double frac = nemo_rclone_job_get_fraction (rjob);
			double speed = nemo_rclone_job_get_speed (rjob);
			NemoRcloneJobState jstate = nemo_rclone_job_get_state (rjob);

			if (frac >= 0.0) {
				progress = g_strdup_printf ("%ld%%", (long) (frac * 100.0 + 0.5));
			} else {
				progress = g_strdup ("…");
			}
			if (speed > 0) {
				spd = g_strdup_printf ("%s/s", g_format_size ((guint64) speed));
			}
			color = g_strdup (jobs_state_color (jstate));
			glyph = g_strdup (jobs_state_glyph (jstate));
			{
				char *state_disp = jobs_state_display (jstate,
				                    nemo_rclone_job_get_state_label (rjob));
				gtk_list_store_set (self->jobs_store, &iter,
				                    1, state_disp,
				                    2, progress, 3, spd, 8, color, 9, glyph,
				                    -1);
				g_free (state_disp);
			}
		} else if (kind == JOB_KIND_NEMO && info != NULL) {
			double p = nemo_progress_info_get_progress (NEMO_PROGRESS_INFO (info));

			if (p >= 1.0) {
				progress = g_strdup (_("Done"));
				color = g_strdup (jobs_nemo_state_color (p, TRUE));
				glyph = g_strdup (jobs_state_glyph (NEMO_RCLONE_JOB_DONE));
			} else {
				progress = g_strdup_printf ("%ld%%", (long) (p * 100.0 + 0.5));
				color = g_strdup (jobs_nemo_state_color (p, TRUE));
				glyph = g_strdup (jobs_state_glyph (NEMO_RCLONE_JOB_RUNNING));
			}
			{
				char *status = nemo_progress_info_get_status (
				                    NEMO_PROGRESS_INFO (info));
				char *state_disp = jobs_state_display (
				                    p >= 1.0 ? NEMO_RCLONE_JOB_DONE :
				                             NEMO_RCLONE_JOB_RUNNING, status);
				gtk_list_store_set (self->jobs_store, &iter,
				                    1, state_disp,
				                    2, progress, 8, color, 9, glyph, -1);
				g_free (status);
				g_free (state_disp);
			}
		}

		g_free (progress);
		g_free (spd);
		g_free (color);
		g_free (glyph);
	}
}

static gboolean
jobs_live_tick (gpointer user_data)
{
	RcloneWindow *self = user_data;
	guint interval_ms;

	jobs_update_live (self);

	/* Honor the "auto-clear finished jobs" setting: once every job is
	   finished, remove them from the list so the page stays tidy. */
	if (self->jobs_auto_clear) {
		NemoRcloneJobManager *manager = self->job_manager;

		if (manager != NULL &&
		    nemo_rclone_job_manager_count_finished (manager) > 0 &&
			nemo_rclone_job_manager_count_running (manager) == 0) {
			nemo_rclone_job_manager_clear_finished (manager);
			jobs_refresh (self, NULL);
		}
	}

	/* Re-schedule using the current (possibly user-changed) rate so the
	   refresh interval always reflects the latest setting.  The rate is
	   clamped to 1..4 Hz, i.e. an interval of 250..1000 ms, which keeps
	   updates smooth (<= 4 Hz) and free of flicker. */
	interval_ms = self->jobs_tick_rate >= 1 ?
	              1000 / self->jobs_tick_rate : 1000;
	self->jobs_timer = g_timeout_add (interval_ms, jobs_live_tick, self);

	return G_SOURCE_REMOVE;
}

static void
jobs_update_detail (RcloneWindow *self,
                    gpointer user_data)
{
	GtkTreeModel *model = GTK_TREE_MODEL (self->jobs_store);
	GtkTreeIter iter;
	gboolean found;
	guint kind;
	gpointer job;
	gpointer info;

	GtkTreeSelection *sel = gtk_tree_view_get_selection (
		GTK_TREE_VIEW (self->jobs_view));
	found = gtk_tree_selection_get_selected (sel, &model, &iter);

	if (!found) {
		if (self->jobs_detail_buffer != NULL) {
			gtk_text_buffer_set_text (self->jobs_detail_buffer, "", -1);
		}
		if (self->jobs_state != NULL) {
			gtk_label_set_text (GTK_LABEL (self->jobs_state), "");
		}
		if (self->jobs_description != NULL) {
			gtk_label_set_text (GTK_LABEL (self->jobs_description), "");
		}
		return;
	}

	gtk_tree_model_get (model, &iter,
	                    5, &kind,
	                    6, &job,
	                    7, &info,
	                    -1);

	if (kind == JOB_KIND_RCLONE && job != NULL) {
		NemoRcloneJob *rjob = NEMO_RCLONE_JOB (job);

		if (self->jobs_description != NULL) {
			gtk_label_set_text (GTK_LABEL (self->jobs_description),
			                    nemo_rclone_job_get_status_text (rjob) != NULL ?
			                    nemo_rclone_job_get_status_text (rjob) : "");
		}
		if (self->jobs_state != NULL) {
			gtk_label_set_text (GTK_LABEL (self->jobs_state),
			                    nemo_rclone_job_get_state_label (rjob));
		}
		if (self->jobs_progress != NULL) {
			double frac = nemo_rclone_job_get_fraction (rjob);
			if (frac >= 0.0) {
				gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (self->jobs_progress), frac);
			} else {
				gtk_progress_bar_pulse (GTK_PROGRESS_BAR (self->jobs_progress));
			}
		}
		if (self->jobs_detail_buffer != NULL) {
			const char *log = nemo_rclone_job_get_log (rjob);
			gtk_text_buffer_set_text (self->jobs_detail_buffer,
			                          log != NULL ? log : "", -1);
		}
	} else if (kind == JOB_KIND_NEMO && info != NULL) {
		NemoProgressInfo *ninfo = NEMO_PROGRESS_INFO (info);
		char *details = nemo_progress_info_get_details (ninfo);

		if (self->jobs_description != NULL) {
			gtk_label_set_text (GTK_LABEL (self->jobs_description),
			                    details != NULL ? details : "");
		}
		if (self->jobs_state != NULL) {
			gtk_label_set_text (GTK_LABEL (self->jobs_state),
			                    nemo_progress_info_get_status (ninfo) != NULL ?
			                    nemo_progress_info_get_status (ninfo) : "");
		}
		if (self->jobs_progress != NULL) {
			double frac = nemo_progress_info_get_progress (ninfo);
			if (frac >= 0.0) {
				gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (self->jobs_progress), frac);
			} else {
				gtk_progress_bar_pulse (GTK_PROGRESS_BAR (self->jobs_progress));
			}
		}
		if (self->jobs_detail_buffer != NULL) {
			gtk_text_buffer_set_text (self->jobs_detail_buffer,
			                          details != NULL ? details : "", -1);
		}
		g_free (details);
	}

	if (self->jobs_summary != NULL) {
		double speed = nemo_rclone_job_manager_total_speed (self->job_manager);
		guint running = nemo_rclone_job_manager_count_running (self->job_manager);

		gtk_label_set_markup (GTK_LABEL (self->jobs_summary),
		                      g_strdup_printf (
			                      "<b>%d running</b> · <b>%s/s</b>",
			                      running, g_format_size ((guint64) speed)));
	}
}

static void
on_jobs_clear_clicked (GtkButton *button,
                       gpointer user_data)
{
	RcloneWindow *self = user_data;

	(void) button;

	if (self->job_manager != NULL) {
		nemo_rclone_job_manager_clear_finished (self->job_manager);
	}

	if (self->jobs_view != NULL) {
		GtkTreeSelection *sel = gtk_tree_view_get_selection (
			GTK_TREE_VIEW (self->jobs_view));
		gtk_tree_selection_select_range (sel,
		                                 gtk_tree_path_new_from_string ("0"),
			                                 NULL);
	}
}

static void
on_jobs_cancel_clicked (GtkButton *button,
                        gpointer user_data)
{
	RcloneWindow *self = user_data;
	GtkTreeModel *model = GTK_TREE_MODEL (self->jobs_store);
	GtkTreeIter iter;
	gboolean found;
	guint kind;
	gpointer job;
	gpointer info;

	(void) button;

	GtkTreeSelection *sel = gtk_tree_view_get_selection (
		GTK_TREE_VIEW (self->jobs_view));
	found = gtk_tree_selection_get_selected (sel, &model, &iter);
	if (!found) {
		return;
	}

	gtk_tree_model_get (model, &iter,
	                    5, &kind,
	                    6, &job,
	                    7, &info,
	                    -1);

	if (kind == JOB_KIND_RCLONE && job != NULL) {
		nemo_rclone_job_cancel (NEMO_RCLONE_JOB (job));
	} else if (kind == JOB_KIND_NEMO && info != NULL) {
		nemo_progress_info_cancel (NEMO_PROGRESS_INFO (info));
	}
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
	NemoRcloneJob *job;
	NemoRcloneJobManager *manager;
	gboolean dry_run;
	GError *error = NULL;

	(void) button;

	if (remote_a == NULL || remote_b == NULL) {
		gtk_label_set_text (GTK_LABEL (self->status_label),
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

	dry_run = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->dry_run_check));

	manager = nemo_rclone_job_manager_get_default ();
	job = nemo_rclone_job_manager_start_job (manager, mode, src, dst,
	                                         dry_run, &error);

	if (job == NULL) {
		char *msg;

		if (error != NULL) {
			msg = g_strdup_printf (_("Unable to start transfer: %s"),
			                       error->message);
			g_error_free (error);
		} else {
			msg = g_strdup_printf (_("Unable to start %s"), mode);
		}

		gtk_label_set_text (GTK_LABEL (self->status_label), msg);
		g_free (msg);
	} else {
		char *msg = g_strdup_printf (_("Started %s to %s"), src, dst);
		gtk_label_set_text (GTK_LABEL (self->status_label), msg);
		g_free (msg);

		/* Switch to the Jobs page so the new job is visible. */
		if (self->notebook != NULL) {
			gtk_notebook_set_current_page (GTK_NOTEBOOK (self->notebook),
			                               self->jobs_tab_index);
		}

		jobs_refresh (self, nemo_rclone_job_get_id (job));
	}

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

	/* Cancel whatever is selected on the Jobs page (rclone job or Nemo
	   file operation).  Does nothing if the list is empty. */
	on_jobs_cancel_clicked (NULL, self);

	if (self->jobs_view != NULL) {
		jobs_refresh (self, NULL);
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

	if (self->job_manager != NULL &&
	    nemo_rclone_job_manager_count_running (self->job_manager) > 0) {
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

	if (self->jobs_timer != 0) {
		g_source_remove (self->jobs_timer);
		self->jobs_timer = 0;
	}

	if (self->job_manager != NULL) {
		g_clear_object (&self->job_manager);
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

typedef struct {
	GtkWidget *rate_spin;
	GtkWidget *auto_clear_check;
	GtkWidget *check_state;
	GtkWidget *check_progress;
	GtkWidget *check_speed;
	GtkWidget *check_job;
} JobsSettingsData;

/* Apply the column-visibility settings to the Jobs tree view. */
static void jobs_apply_column_visibility (RcloneWindow *self);

/* Tunable byte-size unit suffixes (K/M/G/T, plus plain bytes) and duration
   units (s/m/h/d) used to build compact spin+combo rows for the mount options. */
static const char *const byte_units[] = { "", "K", "M", "G", "T" };
static const char *const time_units[] = { "s", "m", "h", "d" };

/* Parse a suffixed mount value such as "128M", "10m" or "0" into a numeric
   value plus its unit string.  Caller frees *unit. */
static void
parse_mount_value (const char *text, double *value, char **unit)
{
	if (text == NULL) {
		*value = 0.0;
		*unit = g_strdup ("");
		return;
	}
	char *end = NULL;
	double v = strtod (text, &end);
	if (end == text) {
		*value = 0.0;
		*unit = g_strdup ("");
		return;
	}
	*value = v;
	*unit = g_strdup (end);
}

/* Index of a unit string in the given table, or 0 when absent. */
static int
unit_index (const char *const *units, int n_units, const char *unit)
{
	int i;
	if (unit == NULL)
		return 0;
	for (i = 0; i < n_units; i++)
		if (g_strcmp0 (units[i], unit) == 0)
			return i;
	return 0;
}

/* Record of the widgets for one mount-options field. */
typedef struct {
	GtkWidget *spin;
	GtkWidget *unit_combo;
} MountField;

/* Widgets for the whole Mount / FUSE section, so the response callback can
   read the chosen values back. */
typedef struct {
	GtkWidget *mode_combo;
	GtkWidget *chunk_spin;
	GtkWidget *max_size_spin, *max_size_unit;
	GtkWidget *read_ahead_spin, *read_ahead_unit;
	GtkWidget *max_age_spin, *max_age_unit;
	GtkWidget *dir_cache_spin, *dir_cache_unit;
	GtkWidget *attr_spin, *attr_unit;
	GtkWidget *poll_spin, *poll_unit;
} MountData;

/* Return the unit currently chosen in a field's unit combo, freshly allocated
   (caller frees).  Returns an empty string when the field has no unit combo
   (e.g. the read-chunk-streams spin). */
static char *
get_field_unit (GtkWidget *unit_combo)
{
	if (unit_combo == NULL)
		return g_strdup ("");
	return gtk_combo_box_text_get_active_text (
		GTK_COMBO_BOX_TEXT (unit_combo));
}

/* Set *dest to a freshly allocated "VALUEunit" string (freeing the old one).
   Used to turn a numeric spin + unit combo back into a rclone flag value. */
static void
free_suffixed_field (char **dest,
                     double value,
                     const char *unit)
{
	char *s;

	if (dest == NULL || unit == NULL)
		return;

	s = g_strdup_printf ("%.0f%s", value, unit);
	g_free (*dest);
	*dest = s;
}

/* Build one mount-options row: label, a numeric spin and (when n_units > 1)
   a unit dropdown.  The current value is split into spin value + unit index.
   Fills *out with the widgets so the caller can read them back. */
static void
mount_add_field (GtkGrid *grid, int y, const char *label,
                 double value, const char *unit,
                 const char *const *units, int n_units,
                 MountField *out)
{
	GtkWidget *spin, *combo;
	int unit_i;

	if (n_units > 1 && unit != NULL)
		unit_i = unit_index (units, n_units, unit);
	else
		unit_i = 0;

	spin = gtk_spin_button_new_with_range (0, 1e9, 1);
	gtk_spin_button_set_digits (GTK_SPIN_BUTTON (spin), 0);
	gtk_spin_button_set_value (GTK_SPIN_BUTTON (spin), value);

	combo = gtk_combo_box_text_new ();
	{
		int i;
		for (i = 0; i < n_units; i++)
			gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (combo),
			                                units[i]);
		if (n_units > 1)
			gtk_combo_box_set_active (GTK_COMBO_BOX (combo), unit_i);
	}

	if (out != NULL) {
		out->spin = spin;
		out->unit_combo = (n_units > 1) ? combo : NULL;
	}

	gtk_grid_attach (grid, gtk_label_new (label), 0, y, 1, 1);
	gtk_grid_attach (grid, spin, 1, y, 1, 1);
	if (n_units > 1)
		gtk_grid_attach (grid, combo, 2, y, 1, 1);
}

/* Called when the Jobs settings dialog is dismissed.  Reads the chosen
   values (only when OK was pressed), persists them, and restarts the
   live-update timer so the new refresh rate takes effect immediately. */
static void
jobs_settings_response_cb (GtkDialog *dialog,
                           GtkResponseType response_id,
                           gpointer user_data)
{
	RcloneWindow *self = user_data;
	JobsSettingsData *data;
	guint interval_ms;

	(void) dialog;

	if (response_id != GTK_RESPONSE_OK)
		return;

	data = g_object_get_data (G_OBJECT (dialog), "rclone-jobs-settings");
	if (data != NULL) {
		gint rate = gtk_spin_button_get_value_as_int (
			GTK_SPIN_BUTTON (data->rate_spin));
		self->jobs_tick_rate = (rate >= 1 && rate <= 4) ? rate : 1;
		self->jobs_auto_clear =
			gtk_toggle_button_get_active (
				GTK_TOGGLE_BUTTON (data->auto_clear_check));
		self->jobs_show_state =
			gtk_toggle_button_get_active (
				GTK_TOGGLE_BUTTON (data->check_state));
		self->jobs_show_progress =
			gtk_toggle_button_get_active (
				GTK_TOGGLE_BUTTON (data->check_progress));
		self->jobs_show_speed =
			gtk_toggle_button_get_active (
				GTK_TOGGLE_BUTTON (data->check_speed));
		self->jobs_show_job =
			gtk_toggle_button_get_active (
				GTK_TOGGLE_BUTTON (data->check_job));
	}

	/* Mount / FUSE options: read the edited widgets back and persist them,
	   so the "mount a remote" function has a settings option too. */
	{
		MountData *mdata = g_object_get_data (
			G_OBJECT (dialog), "rclone-mount-settings");
		RcloneMountOpts *opts;
		char *u;

		if (mdata != NULL) {
			opts = nemo_rclone_service_get_mount_opts ();

			opts->vfs_cache_mode =
				g_strdup (gtk_combo_box_text_get_active_text (
					GTK_COMBO_BOX_TEXT (mdata->mode_combo)));

			{
				gint64 chunk =
					gtk_spin_button_get_value_as_int (
						GTK_SPIN_BUTTON (mdata->chunk_spin));
				opts->read_chunk_streams =
					(chunk > 0) ? chunk : 8;
			}

			u = get_field_unit (mdata->max_size_unit);
			free_suffixed_field (&opts->vfs_cache_max_size,
				gtk_spin_button_get_value (
					GTK_SPIN_BUTTON (mdata->max_size_spin)), u);
			g_free (u);

			u = get_field_unit (mdata->read_ahead_unit);
			free_suffixed_field (&opts->read_ahead,
				gtk_spin_button_get_value (
					GTK_SPIN_BUTTON (mdata->read_ahead_spin)), u);
			g_free (u);

			u = get_field_unit (mdata->max_age_unit);
			free_suffixed_field (&opts->vfs_cache_max_age,
				gtk_spin_button_get_value (
					GTK_SPIN_BUTTON (mdata->max_age_spin)), u);
			g_free (u);

			u = get_field_unit (mdata->dir_cache_unit);
			free_suffixed_field (&opts->dir_cache_time,
				gtk_spin_button_get_value (
					GTK_SPIN_BUTTON (mdata->dir_cache_spin)), u);
			g_free (u);

			u = get_field_unit (mdata->attr_unit);
			free_suffixed_field (&opts->attr_timeout,
				gtk_spin_button_get_value (
					GTK_SPIN_BUTTON (mdata->attr_spin)), u);
			g_free (u);

			u = get_field_unit (mdata->poll_unit);
			free_suffixed_field (&opts->poll_interval,
				gtk_spin_button_get_value (
					GTK_SPIN_BUTTON (mdata->poll_spin)), u);
			g_free (u);

			nemo_rclone_service_save_mount_opts (opts);
			nemo_rclone_service_free_mount_opts (opts);
		}
	}

	jobs_apply_column_visibility (self);

	jobs_settings_save (self);

	/* Restart the live-update timer at the new rate. */
	if (self->jobs_timer != 0) {
		g_source_remove (self->jobs_timer);
		self->jobs_timer = 0;
	}
	jobs_update_live (self);
	interval_ms = self->jobs_tick_rate >= 1 ?
	              1000 / self->jobs_tick_rate : 1000;
	self->jobs_timer = g_timeout_add (interval_ms, jobs_live_tick, self);
}

/* Build and return the Jobs settings modal dialog.  The dialog is owned
   by the caller, who is responsible for destroying it. */
static GtkWidget *
build_jobs_settings_dialog (RcloneWindow *self)
{
	GtkWidget *dialog;
	GtkWidget *content;
	GtkWidget *grid;
	GtkWidget *rate_label;
	JobsSettingsData *data;

	dialog = gtk_dialog_new_with_buttons (_("Rclone Settings"),
	                                      GTK_WINDOW (self->window),
	                                      GTK_DIALOG_MODAL,
	                                      "_Cancel", GTK_RESPONSE_CANCEL,
	                                      "_OK", GTK_RESPONSE_OK,
	                                      NULL);

	content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));

	grid = gtk_grid_new ();
	gtk_container_set_border_width (GTK_CONTAINER (grid), 12);
	gtk_grid_set_row_spacing (GTK_GRID (grid), 10);
	gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
	gtk_box_pack_start (GTK_BOX (content), grid, FALSE, FALSE, 0);

	rate_label = gtk_label_new (_("Refresh rate (Hz):"));
	gtk_label_set_xalign (GTK_LABEL (rate_label), 1.0);
	gtk_grid_attach (GTK_GRID (grid), rate_label, 0, 0, 1, 1);

	data = g_new0 (JobsSettingsData, 1);
	data->rate_spin = gtk_spin_button_new_with_range (1, 4, 1);
	gtk_spin_button_set_digits (GTK_SPIN_BUTTON (data->rate_spin), 0);
	gtk_spin_button_set_value (GTK_SPIN_BUTTON (data->rate_spin),
	                           self->jobs_tick_rate);
	gtk_grid_attach (GTK_GRID (grid), data->rate_spin, 1, 0, 1, 1);

	data->auto_clear_check = gtk_check_button_new_with_label (
		_("Remove completed jobs from the list automatically"));
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (data->auto_clear_check),
	                              self->jobs_auto_clear);
	gtk_grid_attach (GTK_GRID (grid), data->auto_clear_check, 0, 1, 2, 1);

	/* Column-visibility toggles: let the user show/hide each Jobs column. */
	data->check_state = gtk_check_button_new_with_label (_("Show state"));
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (data->check_state),
	                              self->jobs_show_state);
	gtk_grid_attach (GTK_GRID (grid), data->check_state, 0, 2, 2, 1);

	data->check_progress = gtk_check_button_new_with_label (_("Show progress"));
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (data->check_progress),
	                              self->jobs_show_progress);
	gtk_grid_attach (GTK_GRID (grid), data->check_progress, 0, 3, 2, 1);

	data->check_speed = gtk_check_button_new_with_label (_("Show speed"));
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (data->check_speed),
	                              self->jobs_show_speed);
	gtk_grid_attach (GTK_GRID (grid), data->check_speed, 0, 4, 2, 1);

	data->check_job = gtk_check_button_new_with_label (_("Show job"));
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (data->check_job),
	                              self->jobs_show_job);
	gtk_grid_attach (GTK_GRID (grid), data->check_job, 0, 5, 2, 1);

	/* Mount / FUSE cache options: tunables that build_mount_argv() in the
	   service reads when starting a mount.  These give the user a settings
	   option for the "mount a remote" function. */
	{
		GtkWidget *sep;
		GtkWidget *mount_title;
		RcloneMountOpts *opts;
		MountData *mdata;
		GtkWidget *mode_combo;
		GtkWidget *chunk_spin;
		MountField f;

		sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
		gtk_grid_attach (GTK_GRID (grid), sep, 0, 6, 3, 1);

		mount_title = gtk_label_new (NULL);
		gtk_label_set_markup (GTK_LABEL (mount_title),
		                      "<b>Mount / FUSE cache</b>");
		gtk_label_set_xalign (GTK_LABEL (mount_title), 0.0);
		gtk_grid_attach (GTK_GRID (grid), mount_title, 0, 7, 3, 1);

		opts = nemo_rclone_service_get_mount_opts ();

		mdata = g_new0 (MountData, 1);

		/* vfs-cache-mode selector. */
		mode_combo = gtk_combo_box_text_new ();
		{
			const char *const modes[] = {
				"off", "minimal", "cheap", "full"
			};
			int i;
			for (i = 0; i < 4; i++)
				gtk_combo_box_text_append_text (
					GTK_COMBO_BOX_TEXT (mode_combo), modes[i]);
			gtk_combo_box_set_active (
				GTK_COMBO_BOX (mode_combo),
				unit_index (modes, 4, opts->vfs_cache_mode));
		}
		gtk_grid_attach (GTK_GRID (grid), mode_combo, 0, 8, 1, 1);
		gtk_grid_attach (GTK_GRID (grid),
		                 gtk_label_new ("Cache mode:"), 1, 8, 2, 1);
		mdata->mode_combo = mode_combo;

		/* read-chunk-streams (plain integer, no unit). */
		chunk_spin = gtk_spin_button_new_with_range (1, 64, 1);
		gtk_spin_button_set_digits (GTK_SPIN_BUTTON (chunk_spin), 0);
		gtk_spin_button_set_value (GTK_SPIN_BUTTON (chunk_spin),
		                           opts->read_chunk_streams);
		gtk_grid_attach (GTK_GRID (grid), chunk_spin, 1, 9, 2, 1);
		mdata->chunk_spin = chunk_spin;

		/* Byte-sized fields: vfs-cache-max-size, read-ahead.  The stored
	   value is a suffixed string ("2G", "128M") so split it into a
	   numeric spin value and a unit before handing it to the row. */
		{
			double v;
			char *u;
			parse_mount_value (opts->vfs_cache_max_size, &v, &u);
			mount_add_field (GTK_GRID (grid), 10, "Max cache size:",
			                 v, u, byte_units,
			                 G_N_ELEMENTS (byte_units), &f);
			g_free (u);
			mdata->max_size_spin = f.spin;
			mdata->max_size_unit = f.unit_combo;

			parse_mount_value (opts->read_ahead, &v, &u);
			mount_add_field (GTK_GRID (grid), 11, "Read-ahead:",
			                 v, u, byte_units,
			                 G_N_ELEMENTS (byte_units), &f);
			g_free (u);
			mdata->read_ahead_spin = f.spin;
			mdata->read_ahead_unit = f.unit_combo;
		}

		/* Time fields: vfs-cache-max-age, dir-cache-time, attr-timeout,
		   poll-interval.  Split the stored "10m"/"1h"/"0" values. */
		{
			double v;
			char *u;
			parse_mount_value (opts->vfs_cache_max_age, &v, &u);
			mount_add_field (GTK_GRID (grid), 12, "Cache age:",
			                 v, u, time_units,
			                 G_N_ELEMENTS (time_units), &f);
			g_free (u);
			mdata->max_age_spin = f.spin;
			mdata->max_age_unit = f.unit_combo;

			parse_mount_value (opts->dir_cache_time, &v, &u);
			mount_add_field (GTK_GRID (grid), 13, "Dir cache time:",
			                 v, u, time_units,
			                 G_N_ELEMENTS (time_units), &f);
			g_free (u);
			mdata->dir_cache_spin = f.spin;
			mdata->dir_cache_unit = f.unit_combo;

			parse_mount_value (opts->attr_timeout, &v, &u);
			mount_add_field (GTK_GRID (grid), 14, "Attr timeout:",
			                 v, u, time_units,
			                 G_N_ELEMENTS (time_units), &f);
			g_free (u);
			mdata->attr_spin = f.spin;
			mdata->attr_unit = f.unit_combo;

			parse_mount_value (opts->poll_interval, &v, &u);
			mount_add_field (GTK_GRID (grid), 15, "Poll interval:",
			                 v, u, time_units,
			                 G_N_ELEMENTS (time_units), &f);
			g_free (u);
			mdata->poll_spin = f.spin;
			mdata->poll_unit = f.unit_combo;
		}

		/* opts has served its purpose (initialising the widgets); free it
		   now.  The user's edited values are read from the widgets later,
		   in jobs_settings_response_cb(). */
		nemo_rclone_service_free_mount_opts (opts);

		g_object_set_data_full (G_OBJECT (dialog),
		                        "rclone-mount-settings", mdata,
		                        g_free);
	}

	g_object_set_data (G_OBJECT (dialog),
	                   "rclone-jobs-settings", data);

	gtk_widget_show_all (dialog);

	return dialog;
}

static void
on_jobs_settings_clicked (GtkButton *button,
                          gpointer user_data)
{
	RcloneWindow *self = user_data;
	GtkWidget *dialog;
	GtkResponseType result;

	(void) button;

	dialog = build_jobs_settings_dialog (self);
	result = gtk_dialog_run (GTK_DIALOG (dialog));
	jobs_settings_response_cb (GTK_DIALOG (dialog), result, self);
	gtk_widget_destroy (dialog);
}

/* Apply the column-visibility settings to the Jobs tree view.  Columns are
   matched by their header text so this works even outside build_jobs_tab
   (e.g. after the user toggles a checkbox in the settings dialog). */
static void
jobs_apply_column_visibility (RcloneWindow *self)
{
	GList *cols, *l;

	if (self->jobs_view == NULL)
		return;

	cols = gtk_tree_view_get_columns (GTK_TREE_VIEW (self->jobs_view));
	for (l = cols; l != NULL; l = l->next) {
		GtkTreeViewColumn *col = GTK_TREE_VIEW_COLUMN (l->data);
		char *title = NULL;

		g_object_get (col, "title", &title, NULL);

		if (title != NULL) {
			if (g_strcmp0 (title, _("State")) == 0) {
				gtk_tree_view_column_set_visible (col,
				                          self->jobs_show_state);
			} else if (g_strcmp0 (title, _("Progress")) == 0) {
				gtk_tree_view_column_set_visible (col,
				                          self->jobs_show_progress);
			} else if (g_strcmp0 (title, _("Speed")) == 0) {
				gtk_tree_view_column_set_visible (col,
				                          self->jobs_show_speed);
			} else if (g_strcmp0 (title, _("Job")) == 0) {
				gtk_tree_view_column_set_visible (col,
				                          self->jobs_show_job);
			}
		}
		g_free (title);
	}
	g_list_free (cols);
}

static GtkWidget *
build_jobs_tab (RcloneWindow *self)
{
	GtkWidget *vbox;
	GtkWidget *scroll;
	GtkWidget *buttons;
	GtkWidget *cancel_button;
	GtkWidget *clear_button;
	GtkWidget *settings_button;
	GtkWidget *detail_scroll;
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *column;

	self->job_manager = nemo_rclone_job_manager_get_default ();

	/* Apply persisted Jobs-page settings (tick rate, column visibility). */
	jobs_settings_load (self);

	self->jobs_store = gtk_list_store_new (10,
	                                       G_TYPE_STRING,
	                                       G_TYPE_STRING,
	                                       G_TYPE_STRING,
	                                       G_TYPE_STRING,
	                                       G_TYPE_STRING,
	                                       G_TYPE_BOOLEAN,
	                                       G_TYPE_POINTER,
	                                       G_TYPE_POINTER,
	                                       G_TYPE_STRING,
	                                       G_TYPE_STRING);

	self->jobs_view = gtk_tree_view_new_with_model (
		GTK_TREE_MODEL (self->jobs_store));
	gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (self->jobs_view), TRUE);
	gtk_tree_view_set_enable_search (GTK_TREE_VIEW (self->jobs_view), FALSE);

	/* State (colored by state; column 8 carries the CSS color name) */
	renderer = gtk_cell_renderer_text_new ();
	column = gtk_tree_view_column_new_with_attributes (_("State"), renderer,
	                                                   "text", 1, NULL);
	gtk_tree_view_column_set_attributes (column, renderer,
	                                     "foreground", 8, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (self->jobs_view), column);

	/* Progress as a percentage */
	renderer = gtk_cell_renderer_text_new ();
	column = gtk_tree_view_column_new_with_attributes (_("Progress"), renderer,
	                                                   "text", 2, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (self->jobs_view), column);

	/* Speed */
	renderer = gtk_cell_renderer_text_new ();
	column = gtk_tree_view_column_new_with_attributes (_("Speed"), renderer,
	                                                   "text", 3, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (self->jobs_view), column);

	/* Description */
	renderer = gtk_cell_renderer_text_new ();
	column = gtk_tree_view_column_new_with_attributes (_("Job"), renderer,
	                                                   "text", 4, NULL);
	gtk_tree_view_column_set_expand (column, TRUE);
	gtk_tree_view_append_column (GTK_TREE_VIEW (self->jobs_view), column);

	/* Honor the column-visibility settings: hide the ones the user
	   disabled, and drop their headers so the column width isn't kept. */
	jobs_apply_column_visibility (self);

	g_signal_connect (GTK_TREE_VIEW (self->jobs_view), "selection-changed",
	                  G_CALLBACK (jobs_update_detail), self);

	scroll = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scroll), GTK_SHADOW_IN);
	gtk_container_add (GTK_CONTAINER (scroll), self->jobs_view);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
	                                GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

	self->jobs_summary = gtk_label_new (_("No transfers."));
	gtk_label_set_xalign (GTK_LABEL (self->jobs_summary), 0.0);

	/* Detail area */
	self->jobs_description = gtk_label_new (NULL);
	gtk_label_set_xalign (GTK_LABEL (self->jobs_description), 0.0);
	gtk_label_set_line_wrap (GTK_LABEL (self->jobs_description), TRUE);
	gtk_label_set_ellipsize (GTK_LABEL (self->jobs_description), PANGO_ELLIPSIZE_END);
	gtk_label_set_markup (GTK_LABEL (self->jobs_description),
	                      "<b></b>");

	self->jobs_progress = gtk_progress_bar_new ();
	gtk_progress_bar_set_show_text (GTK_PROGRESS_BAR (self->jobs_progress), FALSE);

	self->jobs_state = gtk_label_new (NULL);
	gtk_label_set_xalign (GTK_LABEL (self->jobs_state), 0.0);

	detail_scroll = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (detail_scroll), GTK_SHADOW_IN);
	self->jobs_detail_view = gtk_text_view_new ();
	gtk_text_view_set_editable (GTK_TEXT_VIEW (self->jobs_detail_view), FALSE);
	gtk_text_view_set_monospace (GTK_TEXT_VIEW (self->jobs_detail_view), TRUE);
	gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (self->jobs_detail_view), GTK_WRAP_WORD_CHAR);
	self->jobs_detail_buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->jobs_detail_view));
	gtk_container_add (GTK_CONTAINER (detail_scroll), self->jobs_detail_view);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (detail_scroll),
	                                GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

	vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_set_border_width (GTK_CONTAINER (vbox), 10);

	gtk_box_pack_start (GTK_BOX (vbox), self->jobs_summary, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (vbox), scroll, TRUE, TRUE, 0);

	/* Separator between list and detail */
	gtk_box_pack_start (GTK_BOX (vbox), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL),
	                    FALSE, FALSE, 2);

	{
		GtkWidget *detail_grid;
		GtkWidget *plabel;

		detail_grid = gtk_grid_new ();
		gtk_grid_set_row_spacing (GTK_GRID (detail_grid), 4);
		gtk_grid_set_column_spacing (GTK_GRID (detail_grid), 6);
		gtk_widget_set_margin_start (detail_grid, 0);
		gtk_widget_set_margin_end (detail_grid, 0);

		plabel = gtk_label_new (_("Details:"));
		gtk_label_set_markup (GTK_LABEL (plabel), "<b>Details</b>");
		gtk_grid_attach (GTK_GRID (detail_grid), plabel, 0, 0, 1, 1);

		gtk_grid_attach (GTK_GRID (detail_grid), self->jobs_description, 0, 1, 1, 1);
		gtk_grid_attach (GTK_GRID (detail_grid), self->jobs_progress, 0, 2, 1, 1);
		gtk_grid_attach (GTK_GRID (detail_grid), self->jobs_state, 0, 3, 1, 1);
		gtk_grid_attach (GTK_GRID (detail_grid), detail_scroll, 0, 4, 1, 3);

		gtk_box_pack_start (GTK_BOX (vbox), detail_grid, TRUE, TRUE, 0);
	}

	buttons = gtk_button_box_new (GTK_ORIENTATION_HORIZONTAL);
	gtk_button_box_set_layout (GTK_BUTTON_BOX (buttons), GTK_BUTTONBOX_START);
	gtk_box_pack_start (GTK_BOX (vbox), buttons, FALSE, FALSE, 0);

	cancel_button = gtk_button_new_with_label (_("Cancel"));
	gtk_container_add (GTK_CONTAINER (buttons), cancel_button);

	clear_button = gtk_button_new_with_label (_("Clear finished"));
	gtk_container_add (GTK_CONTAINER (buttons), clear_button);

	settings_button = gtk_button_new_with_label (_("Settings"));
	gtk_container_add (GTK_CONTAINER (buttons), settings_button);

	g_signal_connect (cancel_button, "clicked",
	                  G_CALLBACK (on_jobs_cancel_clicked), self);
	g_signal_connect (clear_button, "clicked",
	                  G_CALLBACK (on_jobs_clear_clicked), self);
	g_signal_connect (settings_button, "clicked",
	                  G_CALLBACK (on_jobs_settings_clicked), self);

	jobs_refresh (self, NULL);

	/* Kick off the live-update loop.  jobs_live_tick re-schedules itself
	   at self->jobs_tick_rate, so the initial 1 s tick is just a
	   warm-up. */
	self->jobs_timer = g_timeout_add (1000, jobs_live_tick, self);

	return vbox;
}

static RcloneWindow *
rclone_window_new (GtkWindow *parent)
{
	RcloneWindow *self;
	GtkWidget *notebook;
	GtkWidget *transfer_tab;
	GtkWidget *favorites_tab;
	GtkWidget *jobs_tab;
	GPtrArray *remotes;
	guint i;

	self = g_new0 (RcloneWindow, 1);
	self->service = nemo_rclone_service_get_default ();
	self->parent = parent;

	self->window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title (GTK_WINDOW (self->window), _("Rclone"));
	gtk_window_set_default_size (GTK_WINDOW (self->window), 860, 640);
	gtk_window_set_transient_for (GTK_WINDOW (self->window), parent);
	gtk_window_set_destroy_with_parent (GTK_WINDOW (self->window), FALSE);
	gtk_window_set_type_hint (GTK_WINDOW (self->window), GDK_WINDOW_TYPE_HINT_DIALOG);

	notebook = gtk_notebook_new ();
	gtk_container_add (GTK_CONTAINER (self->window), notebook);

	transfer_tab = build_transfer_tab (self);
	gtk_notebook_append_page (GTK_NOTEBOOK (notebook), transfer_tab,
	                          gtk_label_new (_("Transfer")));

	jobs_tab = build_jobs_tab (self);
	self->jobs_tab_index = gtk_notebook_get_n_pages (GTK_NOTEBOOK (notebook));
	gtk_notebook_append_page (GTK_NOTEBOOK (notebook), jobs_tab,
	                          gtk_label_new (_("Jobs")));

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

	g_signal_connect (self->job_manager, "changed",
	                  G_CALLBACK (jobs_refresh), self);

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
