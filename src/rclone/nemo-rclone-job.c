/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*-

   nemo-rclone-job.c: one rclone transfer job

   Spawns rclone with --use-json-log and reads the stream to report real
   progress (bytes / total / speed / ETA / current files).  Every job also
   gets its own Unix RC socket so it can be queried or stopped out of band.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.
*/

#include <config.h>

#include "nemo-rclone-job.h"

#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <signal.h>

/* Keep the in-memory log bounded; the full log file stays on disk. */
#define MAX_LOG_BYTES (512 * 1024)
/* Give rclone this long to exit after SIGINT before we SIGKILL it. */
#define CANCEL_GRACE_MS 4000

struct _NemoRcloneJob {
	GObject parent_instance;

	char *id;
	char *mode;
	char *src;
	char *dst;
	gboolean dry_run;

	char *socket_path;
	char *log_path;

	NemoRcloneJobState state;
	GSubprocess *process;
	GDataInputStream *stream;
	GCancellable *read_cancellable;
	gboolean read_done;
	gboolean wait_done;
	guint cancel_timeout_id;

	gint64 bytes;
	gint64 total_bytes;
	double speed;
	gint64 eta;
	guint transfers_done;
	guint transfers_total;

	char *status_text;
	char *error_text;
	GString *log;
	GPtrArray *transferring;

	gint64 started_at;
	gint64 finished_at;
};

enum {
	SIGNAL_CHANGED,
	SIGNAL_FINISHED,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

G_DEFINE_TYPE (NemoRcloneJob, nemo_rclone_job, G_TYPE_OBJECT)

static void
job_free_transferring (NemoRcloneJob *self)
{
	if (self->transferring != NULL) {
		g_ptr_array_set_size (self->transferring, 0);
	}
}

static void
job_flush_log_file (NemoRcloneJob *self)
{
	if (self->log_path != NULL) {
		g_file_set_contents (self->log_path, self->log->str, self->log->len, NULL);
	}
}

static void
job_append_log (NemoRcloneJob *self,
                const char *text)
{
	if (text == NULL || text[0] == '\0') {
		return;
	}

	g_string_append (self->log, text);
	g_string_append_c (self->log, '\n');

	if (self->log->len > MAX_LOG_BYTES) {
		gsize cut = self->log->len - MAX_LOG_BYTES;
		g_string_erase (self->log, 0, cut);
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

	if (node == NULL || json_node_get_value_type (node) == G_TYPE_NONE) {
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

	if (node == NULL || json_node_get_value_type (node) == G_TYPE_NONE) {
		return 0.0;
	}

	if (json_node_get_value_type (node) == G_TYPE_INT64) {
		return (double) json_node_get_int (node);
	}

	return json_node_get_double (node);
}

static void
job_update_transferring (NemoRcloneJob *self,
                         JsonArray *array)
{
	guint i;

	job_free_transferring (self);

	if (array == NULL) {
		return;
	}

	for (i = 0; i < json_array_get_length (array); i++) {
		JsonNode *node = json_array_get_element (array, i);

		if (JSON_NODE_HOLDS_OBJECT (node)) {
			JsonObject *entry = json_node_get_object (node);

			if (json_object_has_member (entry, "name")) {
				g_ptr_array_add (self->transferring,
				                 g_strdup (json_object_get_string_member (entry, "name")));
			}
		}
	}
}

static void
job_parse_stats (NemoRcloneJob *self,
                 JsonObject *stats)
{
	gint64 total = json_int_member (stats, "totalBytes");
	gint64 bytes = json_int_member (stats, "bytes");
	double speed = json_double_member (stats, "speed");
	gint64 eta = json_int_member (stats, "eta");

	self->bytes = bytes;
	self->total_bytes = total;
	self->speed = speed;
	self->eta = eta;
	self->transfers_done = (guint) json_int_member (stats, "transfers");
	self->transfers_total = (guint) json_int_member (stats, "totalTransfers");

	{
		char *done = g_format_size ((guint64) bytes);
		char *all = g_format_size ((guint64) total);
		char *rate = g_format_size ((guint64) speed);
		char *text;

		if (eta > 0) {
			text = g_strdup_printf (_("Transferred %s / %s (%s/s, ETA %" G_GINT64_FORMAT "s)"),
			                        done, all, rate, eta);
		} else {
			text = g_strdup_printf (_("Transferred %s / %s (%s/s)"),
			                        done, all, rate);
		}

		g_free (self->status_text);
		self->status_text = text;
		g_free (done);
		g_free (all);
		g_free (rate);
	}

	if (json_object_has_member (stats, "transferring")) {
		JsonNode *node = json_object_get_member (stats, "transferring");

		if (node != NULL && JSON_NODE_HOLDS_ARRAY (node)) {
			job_update_transferring (self, json_node_get_array (node));
		}
	} else if (total > 0 && bytes >= total) {
		/* No active files and nothing left: clear the list. */
		job_free_transferring (self);
	}
}

static void
job_parse_line (NemoRcloneJob *self,
                const char *line)
{
	JsonParser *parser;
	JsonNode *root;
	JsonObject *object;

	parser = json_parser_new ();

	if (!json_parser_load_from_data (parser, line, -1, NULL)) {
		/* Not JSON (very early output, or --dry-run notes). */
		job_append_log (self, line);
		g_object_unref (parser);
		return;
	}

	root = json_parser_get_root (parser);

	if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root)) {
		g_object_unref (parser);
		return;
	}

	object = json_node_get_object (root);

	if (json_object_has_member (object, "stats")) {
		JsonNode *node = json_object_get_member (object, "stats");

		if (node != NULL && JSON_NODE_HOLDS_OBJECT (node)) {
			job_parse_stats (self, json_node_get_object (node));
		}
	} else if (json_object_has_member (object, "level") &&
	           json_object_has_member (object, "msg")) {
		const char *level = json_object_get_string_member (object, "level");
		const char *msg = json_object_get_string_member (object, "msg");
		char *rendered;

		if (json_object_has_member (object, "object") &&
		    json_object_get_string_member (object, "object") != NULL) {
			const char *object_name =
				json_object_get_string_member (object, "object");
			rendered = g_strdup_printf ("%-5s %s: %s",
			                            level != NULL ? level : "info",
			                            object_name, msg != NULL ? msg : "");
		} else {
			rendered = g_strdup_printf ("%-5s %s",
			                            level != NULL ? level : "info",
			                            msg != NULL ? msg : "");
		}

		job_append_log (self, rendered);

		if (g_strcmp0 (level, "error") == 0 && self->error_text == NULL) {
			self->error_text = g_strdup (msg != NULL ? msg : _("rclone reported an error"));
		}

		g_free (rendered);
	} else if (json_object_has_member (object, "error")) {
		const char *error = json_object_get_string_member (object, "error");

		if (error != NULL && error[0] != '\0') {
			g_free (self->error_text);
			self->error_text = g_strdup (error);
			job_append_log (self, error);
		}
	}

	g_object_unref (parser);
}

static void
job_emit_changed (NemoRcloneJob *self)
{
	g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

static gboolean
job_cancel_timeout (gpointer user_data)
{
	NemoRcloneJob *self = user_data;

	self->cancel_timeout_id = 0;

	if (self->process != NULL) {
		g_subprocess_force_exit (self->process);
	}

	return G_SOURCE_REMOVE;
}

static void
job_read_line_cb (GObject *source,
                  GAsyncResult *result,
                  gpointer user_data)
{
	NemoRcloneJob *self = user_data;
	GError *error = NULL;
	char *line;

	line = g_data_input_stream_read_line_finish (self->stream, result, NULL, &error);

	if (line == NULL) {
		if (error != NULL) {
			g_error_free (error);
		}
		self->read_done = TRUE;
		/* The stream closing means rclone is finishing. */
		job_emit_changed (self);
		return;
	}

	job_parse_line (self, line);
	g_free (line);

	job_emit_changed (self);

	if (self->read_cancellable != NULL) {
		g_data_input_stream_read_line_async (self->stream,
		                                     G_PRIORITY_DEFAULT,
		                                     self->read_cancellable,
		                                     job_read_line_cb,
		                                     self);
	}
}

static void
job_wait_cb (GObject *source,
             GAsyncResult *result,
             gpointer user_data)
{
	NemoRcloneJob *self = user_data;
	GError *error = NULL;
	gboolean success;

	success = g_subprocess_wait_finish (self->process, result, &error);
	self->wait_done = TRUE;

	if (self->cancel_timeout_id != 0) {
		g_source_remove (self->cancel_timeout_id);
		self->cancel_timeout_id = 0;
	}

	if (self->state != NEMO_RCLONE_JOB_CANCELLED) {
		if (success) {
			self->state = NEMO_RCLONE_JOB_DONE;
			job_append_log (self, self->dry_run ?
			                _("Dry run finished.") : _("Transfer finished."));
		} else {
			self->state = NEMO_RCLONE_JOB_FAILED;

			if (self->error_text == NULL) {
				g_free (self->error_text);
				self->error_text = g_strdup (error != NULL ?
				                             error->message : _("rclone exited with an error"));
			}
			job_append_log (self, self->error_text);
		}
	}

	if (error != NULL) {
		g_error_free (error);
	}

	self->speed = 0.0;
	self->eta = 0;
	self->finished_at = g_get_real_time ();
	job_flush_log_file (self);

	job_emit_changed (self);
	g_signal_emit (self, signals[SIGNAL_FINISHED], 0);
}

static void
nemo_rclone_job_finalize (GObject *object)
{
	NemoRcloneJob *self = NEMO_RCLONE_JOB (object);

	if (self->cancel_timeout_id != 0) {
		g_source_remove (self->cancel_timeout_id);
		self->cancel_timeout_id = 0;
	}

	if (self->read_cancellable != NULL) {
		g_cancellable_cancel (self->read_cancellable);
		g_clear_object (&self->read_cancellable);
	}

	if (self->process != NULL) {
		g_subprocess_force_exit (self->process);
		g_clear_object (&self->process);
	}

	g_clear_object (&self->stream);

	g_free (self->id);
	g_free (self->mode);
	g_free (self->src);
	g_free (self->dst);
	g_free (self->socket_path);
	g_free (self->log_path);
	g_free (self->status_text);
	g_free (self->error_text);

	if (self->log != NULL) {
		g_string_free (self->log, TRUE);
	}

	if (self->transferring != NULL) {
		g_ptr_array_free (self->transferring, TRUE);
	}

	G_OBJECT_CLASS (nemo_rclone_job_parent_class)->finalize (object);
}

static void
nemo_rclone_job_class_init (NemoRcloneJobClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = nemo_rclone_job_finalize;

	signals[SIGNAL_CHANGED] =
		g_signal_new ("changed",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              0, NULL, NULL, NULL,
		              G_TYPE_NONE, 0);

	signals[SIGNAL_FINISHED] =
		g_signal_new ("finished",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              0, NULL, NULL, NULL,
		              G_TYPE_NONE, 0);
}

static void
nemo_rclone_job_init (NemoRcloneJob *self)
{
	self->state = NEMO_RCLONE_JOB_QUEUED;
	self->log = g_string_new (NULL);
	self->transferring = g_ptr_array_new_with_free_func (g_free);
	self->eta = -1;
	self->total_bytes = 0;
}

NemoRcloneJob *
nemo_rclone_job_new (const char *mode,
                     const char *src,
                     const char *dst,
                     gboolean dry_run)
{
	NemoRcloneJob *self;

	g_return_val_if_fail (mode != NULL, NULL);
	g_return_val_if_fail (src != NULL, NULL);
	g_return_val_if_fail (dst != NULL, NULL);

	self = g_object_new (NEMO_TYPE_RCLONE_JOB, NULL);

	self->id = g_uuid_string_random ();
	self->mode = g_strdup (mode);
	self->src = g_strdup (src);
	self->dst = g_strdup (dst);
	self->dry_run = dry_run;

	return self;
}

static char *
job_dir (const char *env,
         const char *fallback_base)
{
	const char *override = g_getenv (env);
	char *dir;

	if (override != NULL && override[0] != '\0') {
		dir = g_strdup (override);
	} else {
		dir = g_build_filename (fallback_base, "nemo-rclone", "jobs", NULL);
	}

	g_mkdir_with_parents (dir, 0700);
	return dir;
}

gboolean
nemo_rclone_job_start (NemoRcloneJob *self,
                       GError **error)
{
	GPtrArray *args;
	GSubprocessLauncher *launcher;
	char *runtime_dir;
	char *cache_dir;
	char *spec;
	char *socket_uri;
	char *log_name;

	g_return_val_if_fail (NEMO_IS_RCLONE_JOB (self), FALSE);

	if (self->process != NULL || self->state == NEMO_RCLONE_JOB_RUNNING) {
		return TRUE;
	}

	runtime_dir = job_dir ("NEMO_RCLONE_RUNTIME_DIR", g_get_user_runtime_dir ());
	cache_dir = job_dir ("NEMO_RCLONE_CACHE_DIR", g_get_user_cache_dir ());

	socket_uri = g_strdup_printf ("unix://%s/%s.sock", runtime_dir, self->id);
	g_free (self->socket_path);
	self->socket_path = g_strdup_printf ("%s/%s.sock", runtime_dir, self->id);

	log_name = g_strdup_printf ("%s.log", self->id);
	g_free (self->log_path);
	self->log_path = g_build_filename (cache_dir, log_name, NULL);
	g_free (log_name);

	/* rclone refuses to bind a stale socket path. */
	unlink (self->socket_path);

	args = g_ptr_array_new_with_free_func (g_free);
	g_ptr_array_add (args, g_strdup ("rclone"));
	g_ptr_array_add (args, g_strdup (self->mode));
	g_ptr_array_add (args, g_strdup (self->src));
	g_ptr_array_add (args, g_strdup (self->dst));
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
	g_ptr_array_add (args, g_strdup ("--rc"));
	g_ptr_array_add (args, g_strdup ("--rc-addr"));
	g_ptr_array_add (args, socket_uri);
	g_ptr_array_add (args, g_strdup ("--rc-no-auth"));

	if (self->dry_run) {
		g_ptr_array_add (args, g_strdup ("--dry-run"));
	}

	g_ptr_array_add (args, NULL);

	spec = g_strdup_printf ("%s → %s", self->src, self->dst);
	g_free (self->status_text);
	self->status_text = g_strdup_printf (_("Starting %s"), spec);
	g_free (spec);
	job_append_log (self, self->status_text);

	launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
	                                      G_SUBPROCESS_FLAGS_STDERR_MERGE);

	self->process = g_subprocess_launcher_spawnv (launcher,
	                                              (const char *const *) args->pdata,
	                                              error);

	g_object_unref (launcher);
	g_ptr_array_free (args, TRUE);

	g_free (runtime_dir);
	g_free (cache_dir);
	/* socket_uri was consumed by the args array. */

	if (self->process == NULL) {
		self->state = NEMO_RCLONE_JOB_FAILED;
		job_append_log (self, (*error)->message);
		job_emit_changed (self);
		return FALSE;
	}

	self->state = NEMO_RCLONE_JOB_RUNNING;
	self->started_at = g_get_real_time ();

	self->stream = g_data_input_stream_new (g_subprocess_get_stdout_pipe (self->process));
	g_data_input_stream_set_newline_type (self->stream, G_DATA_STREAM_NEWLINE_TYPE_ANY);

	self->read_cancellable = g_cancellable_new ();

	g_subprocess_wait_async (self->process, NULL, job_wait_cb, self);
	g_data_input_stream_read_line_async (self->stream,
	                                     G_PRIORITY_DEFAULT,
	                                     self->read_cancellable,
	                                     job_read_line_cb,
	                                     self);

	job_emit_changed (self);
	return TRUE;
}

void
nemo_rclone_job_cancel (NemoRcloneJob *self)
{
	g_return_if_fail (NEMO_IS_RCLONE_JOB (self));

	if (self->process == NULL || nemo_rclone_job_is_finished (self)) {
		return;
	}

	self->state = NEMO_RCLONE_JOB_CANCELLED;
	g_free (self->status_text);
	self->status_text = g_strdup (_("Cancelling…"));
	job_append_log (self, _("Cancelling…"));
	job_emit_changed (self);

	g_subprocess_send_signal (self->process, SIGINT);

	if (self->cancel_timeout_id == 0) {
		self->cancel_timeout_id = g_timeout_add (CANCEL_GRACE_MS,
		                                         job_cancel_timeout,
		                                         self);
	}
}

const char *
nemo_rclone_job_get_id (NemoRcloneJob *self)
{
	g_return_val_if_fail (NEMO_IS_RCLONE_JOB (self), NULL);
	return self->id;
}

const char *
nemo_rclone_job_get_mode (NemoRcloneJob *self)
{
	g_return_val_if_fail (NEMO_IS_RCLONE_JOB (self), NULL);
	return self->mode;
}

const char *
nemo_rclone_job_get_src (NemoRcloneJob *self)
{
	g_return_val_if_fail (NEMO_IS_RCLONE_JOB (self), NULL);
	return self->src;
}

const char *
nemo_rclone_job_get_dst (NemoRcloneJob *self)
{
	g_return_val_if_fail (NEMO_IS_RCLONE_JOB (self), NULL);
	return self->dst;
}

gboolean
nemo_rclone_job_is_dry_run (NemoRcloneJob *self)
{
	g_return_val_if_fail (NEMO_IS_RCLONE_JOB (self), FALSE);
	return self->dry_run;
}

gboolean
nemo_rclone_job_is_finished (NemoRcloneJob *self)
{
	g_return_val_if_fail (NEMO_IS_RCLONE_JOB (self), TRUE);

	return self->state == NEMO_RCLONE_JOB_DONE ||
	       self->state == NEMO_RCLONE_JOB_FAILED ||
	       self->state == NEMO_RCLONE_JOB_CANCELLED;
}

NemoRcloneJobState
nemo_rclone_job_get_state (NemoRcloneJob *self)
{
	g_return_val_if_fail (NEMO_IS_RCLONE_JOB (self), NEMO_RCLONE_JOB_QUEUED);
	return self->state;
}

const char *
nemo_rclone_job_get_state_label (NemoRcloneJob *self)
{
	g_return_val_if_fail (NEMO_IS_RCLONE_JOB (self), "");

	switch (self->state) {
	case NEMO_RCLONE_JOB_QUEUED:
		return _("Queued");
	case NEMO_RCLONE_JOB_RUNNING:
		return self->dry_run ? _("Dry run") : _("Running");
	case NEMO_RCLONE_JOB_DONE:
		return _("Done");
	case NEMO_RCLONE_JOB_FAILED:
		return _("Failed");
	case NEMO_RCLONE_JOB_CANCELLED:
		return _("Cancelled");
	}

	return "";
}

const char *
nemo_rclone_job_get_status_text (NemoRcloneJob *self)
{
	g_return_val_if_fail (NEMO_IS_RCLONE_JOB (self), NULL);
	return self->status_text;
}

const char *
nemo_rclone_job_get_error (NemoRcloneJob *self)
{
	g_return_val_if_fail (NEMO_IS_RCLONE_JOB (self), NULL);
	return self->error_text;
}

double
nemo_rclone_job_get_fraction (NemoRcloneJob *self)
{
	g_return_val_if_fail (NEMO_IS_RCLONE_JOB (self), -1.0);

	if (self->state == NEMO_RCLONE_JOB_DONE) {
		return 1.0;
	}

	if (self->total_bytes <= 0) {
		return -1.0;
	}

	return CLAMP ((double) self->bytes / (double) self->total_bytes, 0.0, 1.0);
}

gint64
nemo_rclone_job_get_bytes (NemoRcloneJob *self)
{
	g_return_val_if_fail (NEMO_IS_RCLONE_JOB (self), 0);
	return self->bytes;
}

gint64
nemo_rclone_job_get_total_bytes (NemoRcloneJob *self)
{
	g_return_val_if_fail (NEMO_IS_RCLONE_JOB (self), 0);
	return self->total_bytes;
}

double
nemo_rclone_job_get_speed (NemoRcloneJob *self)
{
	g_return_val_if_fail (NEMO_IS_RCLONE_JOB (self), 0.0);
	return self->speed;
}

gint64
nemo_rclone_job_get_eta (NemoRcloneJob *self)
{
	g_return_val_if_fail (NEMO_IS_RCLONE_JOB (self), -1);
	return self->eta;
}

gint64
nemo_rclone_job_get_elapsed (NemoRcloneJob *self)
{
	gint64 end;

	g_return_val_if_fail (NEMO_IS_RCLONE_JOB (self), 0);

	if (self->started_at == 0) {
		return 0;
	}

	end = self->finished_at != 0 ? self->finished_at : g_get_real_time ();
	return (end - self->started_at) / G_USEC_PER_SEC;
}

GPtrArray *
nemo_rclone_job_get_transferring (NemoRcloneJob *self)
{
	g_return_val_if_fail (NEMO_IS_RCLONE_JOB (self), NULL);
	return self->transferring;
}

const char *
nemo_rclone_job_get_log (NemoRcloneJob *self)
{
	g_return_val_if_fail (NEMO_IS_RCLONE_JOB (self), NULL);
	return self->log->str;
}

const char *
nemo_rclone_job_get_log_path (NemoRcloneJob *self)
{
	g_return_val_if_fail (NEMO_IS_RCLONE_JOB (self), NULL);
	return self->log_path;
}

const char *
nemo_rclone_job_get_socket_path (NemoRcloneJob *self)
{
	g_return_val_if_fail (NEMO_IS_RCLONE_JOB (self), NULL);
	return self->socket_path;
}
