/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*-

   nemo-rclone-job-manager.h: registry of running rclone transfer jobs

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.
*/

#ifndef NEMO_RCLONE_JOB_MANAGER_H
#define NEMO_RCLONE_JOB_MANAGER_H

#include <glib-object.h>

#include "nemo-rclone-job.h"

G_BEGIN_DECLS

#define NEMO_TYPE_RCLONE_JOB_MANAGER (nemo_rclone_job_manager_get_type ())
G_DECLARE_FINAL_TYPE (NemoRcloneJobManager, nemo_rclone_job_manager,
                      NEMO, RCLONE_JOB_MANAGER, GObject)

NemoRcloneJobManager *nemo_rclone_job_manager_get_default (void);

/* Start a transfer and return the (owned-by-manager) job. */
NemoRcloneJob *nemo_rclone_job_manager_start_job (NemoRcloneJobManager *self,
                                                  const char           *mode,
                                                  const char           *src,
                                                  const char           *dst,
                                                  gboolean              dry_run,
                                                  GError              **error);

/* Newest first.  Do not modify; the array is owned by the manager. */
GPtrArray     *nemo_rclone_job_manager_get_jobs    (NemoRcloneJobManager *self);
NemoRcloneJob *nemo_rclone_job_manager_find_job    (NemoRcloneJobManager *self,
                                                    const char           *id);
void           nemo_rclone_job_manager_cancel_job  (NemoRcloneJobManager *self,
                                                    const char           *id);
void           nemo_rclone_job_manager_remove_job  (NemoRcloneJobManager *self,
                                                    const char           *id);
void           nemo_rclone_job_manager_clear_finished (NemoRcloneJobManager *self);

guint          nemo_rclone_job_manager_count_running (NemoRcloneJobManager *self);
guint          nemo_rclone_job_manager_count_finished (NemoRcloneJobManager *self);
double         nemo_rclone_job_manager_total_speed   (NemoRcloneJobManager *self);

/* Nemo's own file operations (local / FUSE copies), so the Jobs page can
   show them next to rclone jobs.  Borrowed list of NemoProgressInfo*. */
GList         *nemo_rclone_job_manager_get_nemo_infos (NemoRcloneJobManager *self);

G_END_DECLS

#endif /* NEMO_RCLONE_JOB_MANAGER_H */
