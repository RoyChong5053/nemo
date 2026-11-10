/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*-

   nemo-rclone-job-manager.c: registry of running rclone transfer jobs

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.
*/

#include <config.h>

#include "nemo-rclone-job-manager.h"

#include <libnemo-private/nemo-progress-info-manager.h>

struct _NemoRcloneJobManager {
	GObject parent_instance;

	GPtrArray *jobs;              /* NemoRcloneJob *, newest last */
	NemoProgressInfoManager *nemo_manager;  /* borrowed singleton */
};

enum {
	SIGNAL_CHANGED,
	SIGNAL_JOB_ADDED,
	SIGNAL_JOB_FINISHED,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

G_DEFINE_TYPE (NemoRcloneJobManager, nemo_rclone_job_manager, G_TYPE_OBJECT)

static NemoRcloneJobManager *default_manager = NULL;

static void
on_job_changed (NemoRcloneJob *job,
                gpointer user_data)
{
	NemoRcloneJobManager *self = user_data;

	(void) job;
	g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

static void
on_job_finished (NemoRcloneJob *job,
                 gpointer user_data)
{
	NemoRcloneJobManager *self = user_data;

	g_signal_emit (self, signals[SIGNAL_JOB_FINISHED], 0, job);
	g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

static void
on_nemo_info_changed (NemoProgressInfo *info,
                      gpointer user_data)
{
	NemoRcloneJobManager *self = user_data;

	(void) info;
	g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

static void
on_nemo_info_new (NemoProgressInfoManager *manager,
                  NemoProgressInfo *info,
                  gpointer user_data)
{
	NemoRcloneJobManager *self = user_data;

	(void) manager;
	g_signal_connect (info, "changed", G_CALLBACK (on_nemo_info_changed), self);
	g_signal_connect (info, "progress-changed", G_CALLBACK (on_nemo_info_changed), self);
	g_signal_connect (info, "finished", G_CALLBACK (on_nemo_info_changed), self);
	g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

static void
nemo_rclone_job_manager_finalize (GObject *object)
{
	NemoRcloneJobManager *self = NEMO_RCLONE_JOB_MANAGER (object);

	if (self->jobs != NULL) {
		g_ptr_array_free (self->jobs, TRUE);
	}

	g_clear_object (&self->nemo_manager);

	G_OBJECT_CLASS (nemo_rclone_job_manager_parent_class)->finalize (object);
}

static void
nemo_rclone_job_manager_class_init (NemoRcloneJobManagerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = nemo_rclone_job_manager_finalize;

	signals[SIGNAL_CHANGED] =
		g_signal_new ("changed",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              0, NULL, NULL, NULL,
		              G_TYPE_NONE, 0);

	signals[SIGNAL_JOB_ADDED] =
		g_signal_new ("job-added",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              0, NULL, NULL, NULL,
		              G_TYPE_NONE, 1, NEMO_TYPE_RCLONE_JOB);

	signals[SIGNAL_JOB_FINISHED] =
		g_signal_new ("job-finished",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              0, NULL, NULL, NULL,
		              G_TYPE_NONE, 1, NEMO_TYPE_RCLONE_JOB);
}

static void
nemo_rclone_job_manager_init (NemoRcloneJobManager *self)
{
	GList *infos;
	GList *l;

	self->jobs = g_ptr_array_new_with_free_func (g_object_unref);

	/* The progress-info manager is a process-wide singleton. */
	self->nemo_manager = nemo_progress_info_manager_new ();

	g_signal_connect (self->nemo_manager, "new-progress-info",
	                  G_CALLBACK (on_nemo_info_new), self);

	infos = nemo_progress_info_manager_get_all_infos (self->nemo_manager);
	for (l = infos; l != NULL; l = l->next) {
		g_signal_connect (l->data, "changed",
		                  G_CALLBACK (on_nemo_info_changed), self);
		g_signal_connect (l->data, "progress-changed",
		                  G_CALLBACK (on_nemo_info_changed), self);
		g_signal_connect (l->data, "finished",
		                  G_CALLBACK (on_nemo_info_changed), self);
	}
}

NemoRcloneJobManager *
nemo_rclone_job_manager_get_default (void)
{
	if (default_manager == NULL) {
		default_manager = g_object_new (NEMO_TYPE_RCLONE_JOB_MANAGER, NULL);
		g_object_add_weak_pointer (G_OBJECT (default_manager),
		                           (gpointer *) &default_manager);
	}

	return default_manager;
}

NemoRcloneJob *
nemo_rclone_job_manager_start_job (NemoRcloneJobManager *self,
                                   const char *mode,
                                   const char *src,
                                   const char *dst,
                                   gboolean dry_run,
                                   GError **error)
{
	NemoRcloneJob *job;

	g_return_val_if_fail (NEMO_IS_RCLONE_JOB_MANAGER (self), NULL);

	job = nemo_rclone_job_new (mode, src, dst, dry_run);

	g_signal_connect (job, "changed", G_CALLBACK (on_job_changed), self);
	g_signal_connect (job, "finished", G_CALLBACK (on_job_finished), self);

	g_ptr_array_add (self->jobs, job);

	if (!nemo_rclone_job_start (job, error)) {
		g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
		return job;
	}

	g_signal_emit (self, signals[SIGNAL_JOB_ADDED], 0, job);
	g_signal_emit (self, signals[SIGNAL_CHANGED], 0);

	return job;
}

GPtrArray *
nemo_rclone_job_manager_get_jobs (NemoRcloneJobManager *self)
{
	g_return_val_if_fail (NEMO_IS_RCLONE_JOB_MANAGER (self), NULL);
	return self->jobs;
}

NemoRcloneJob *
nemo_rclone_job_manager_find_job (NemoRcloneJobManager *self,
                                  const char *id)
{
	guint i;

	g_return_val_if_fail (NEMO_IS_RCLONE_JOB_MANAGER (self), NULL);

	if (id == NULL) {
		return NULL;
	}

	for (i = 0; i < self->jobs->len; i++) {
		NemoRcloneJob *job = g_ptr_array_index (self->jobs, i);

		if (g_strcmp0 (nemo_rclone_job_get_id (job), id) == 0) {
			return job;
		}
	}

	return NULL;
}

void
nemo_rclone_job_manager_cancel_job (NemoRcloneJobManager *self,
                                    const char *id)
{
	NemoRcloneJob *job;

	g_return_if_fail (NEMO_IS_RCLONE_JOB_MANAGER (self));

	job = nemo_rclone_job_manager_find_job (self, id);
	if (job != NULL) {
		nemo_rclone_job_cancel (job);
	}
}

void
nemo_rclone_job_manager_remove_job (NemoRcloneJobManager *self,
                                    const char *id)
{
	guint i;

	g_return_if_fail (NEMO_IS_RCLONE_JOB_MANAGER (self));

	for (i = 0; i < self->jobs->len; i++) {
		NemoRcloneJob *job = g_ptr_array_index (self->jobs, i);

		if (g_strcmp0 (nemo_rclone_job_get_id (job), id) == 0) {
			if (!nemo_rclone_job_is_finished (job)) {
				nemo_rclone_job_cancel (job);
			}
			g_ptr_array_remove_index (self->jobs, i);
			g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
			return;
		}
	}
}

void
nemo_rclone_job_manager_clear_finished (NemoRcloneJobManager *self)
{
	guint i = 0;

	g_return_if_fail (NEMO_IS_RCLONE_JOB_MANAGER (self));

	while (i < self->jobs->len) {
		NemoRcloneJob *job = g_ptr_array_index (self->jobs, i);

		if (nemo_rclone_job_is_finished (job)) {
			g_ptr_array_remove_index (self->jobs, i);
		} else {
			i++;
		}
	}

	g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

guint
nemo_rclone_job_manager_count_running (NemoRcloneJobManager *self)
{
	guint count = 0;
	guint i;

	g_return_val_if_fail (NEMO_IS_RCLONE_JOB_MANAGER (self), 0);

	for (i = 0; i < self->jobs->len; i++) {
		NemoRcloneJob *job = g_ptr_array_index (self->jobs, i);

		if (!nemo_rclone_job_is_finished (job)) {
			count++;
		}
	}

	return count;
}

double
nemo_rclone_job_manager_total_speed (NemoRcloneJobManager *self)
{
	double speed = 0.0;
	guint i;

	g_return_val_if_fail (NEMO_IS_RCLONE_JOB_MANAGER (self), 0.0);

	for (i = 0; i < self->jobs->len; i++) {
		NemoRcloneJob *job = g_ptr_array_index (self->jobs, i);

		if (!nemo_rclone_job_is_finished (job)) {
			speed += nemo_rclone_job_get_speed (job);
		}
	}

	return speed;
}

GList *
nemo_rclone_job_manager_get_nemo_infos (NemoRcloneJobManager *self)
{
	g_return_val_if_fail (NEMO_IS_RCLONE_JOB_MANAGER (self), NULL);
	return nemo_progress_info_manager_get_all_infos (self->nemo_manager);
}
