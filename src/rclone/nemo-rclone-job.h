/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*-

   nemo-rclone-job.h: one rclone transfer job

   Each job owns a single rclone process (copy/move/sync) and reads its
   JSON log stream, so several transfers can run at the same time without
   overwriting each other's progress.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.
*/

#ifndef NEMO_RCLONE_JOB_H
#define NEMO_RCLONE_JOB_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum {
	NEMO_RCLONE_JOB_QUEUED,
	NEMO_RCLONE_JOB_RUNNING,
	NEMO_RCLONE_JOB_DONE,
	NEMO_RCLONE_JOB_FAILED,
	NEMO_RCLONE_JOB_CANCELLED
} NemoRcloneJobState;

#define NEMO_TYPE_RCLONE_JOB (nemo_rclone_job_get_type ())
G_DECLARE_FINAL_TYPE (NemoRcloneJob, nemo_rclone_job, NEMO, RCLONE_JOB, GObject)

NemoRcloneJob *nemo_rclone_job_new (const char *mode,
                                    const char *src,
                                    const char *dst,
                                    gboolean    dry_run);

gboolean nemo_rclone_job_start  (NemoRcloneJob *self, GError **error);
void     nemo_rclone_job_cancel (NemoRcloneJob *self);

const char        *nemo_rclone_job_get_id            (NemoRcloneJob *self);
const char        *nemo_rclone_job_get_mode          (NemoRcloneJob *self);
const char        *nemo_rclone_job_get_src           (NemoRcloneJob *self);
const char        *nemo_rclone_job_get_dst           (NemoRcloneJob *self);
gboolean           nemo_rclone_job_is_dry_run        (NemoRcloneJob *self);
gboolean           nemo_rclone_job_is_finished       (NemoRcloneJob *self);
NemoRcloneJobState nemo_rclone_job_get_state         (NemoRcloneJob *self);
const char        *nemo_rclone_job_get_state_label   (NemoRcloneJob *self);
const char        *nemo_rclone_job_get_status_text   (NemoRcloneJob *self);
const char        *nemo_rclone_job_get_error         (NemoRcloneJob *self);

/* 0..1, or -1 when the total size is not known yet. */
double             nemo_rclone_job_get_fraction      (NemoRcloneJob *self);
gint64             nemo_rclone_job_get_bytes         (NemoRcloneJob *self);
gint64             nemo_rclone_job_get_total_bytes   (NemoRcloneJob *self);
double             nemo_rclone_job_get_speed         (NemoRcloneJob *self);
gint64             nemo_rclone_job_get_eta           (NemoRcloneJob *self);
gint64             nemo_rclone_job_get_elapsed       (NemoRcloneJob *self);

/* Names of files currently being transferred (const char *). */
GPtrArray         *nemo_rclone_job_get_transferring  (NemoRcloneJob *self);
const char        *nemo_rclone_job_get_log           (NemoRcloneJob *self);
const char        *nemo_rclone_job_get_log_path      (NemoRcloneJob *self);
const char        *nemo_rclone_job_get_socket_path   (NemoRcloneJob *self);

G_END_DECLS

#endif /* NEMO_RCLONE_JOB_H */
