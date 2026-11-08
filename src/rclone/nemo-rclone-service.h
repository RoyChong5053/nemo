/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*-

   nemo-rclone-service.h: rclone integration service

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.
*/

#ifndef NEMO_RCLONE_SERVICE_H
#define NEMO_RCLONE_SERVICE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct {
	char *id;
	char *label;
	char *remote;
	char *path;
	char *mountpoint;
} NemoRcloneFavorite;

#define NEMO_TYPE_RCLONE_SERVICE (nemo_rclone_service_get_type ())
G_DECLARE_FINAL_TYPE (NemoRcloneService, nemo_rclone_service,
                      NEMO, RCLONE_SERVICE, GObject)

NemoRcloneService *nemo_rclone_service_get_default (void);

gboolean            nemo_rclone_service_is_available      (NemoRcloneService *self);

GPtrArray          *nemo_rclone_service_get_remotes       (NemoRcloneService *self);
void                nemo_rclone_service_reload_remotes    (NemoRcloneService *self);

GPtrArray          *nemo_rclone_service_get_favorites     (NemoRcloneService *self);
NemoRcloneFavorite *nemo_rclone_service_add_favorite      (NemoRcloneService *self,
                                                           const char        *label,
                                                           const char        *remote,
                                                           const char        *path,
                                                           const char        *mountpoint);
void                nemo_rclone_service_remove_favorite   (NemoRcloneService *self,
                                                           const char        *id);
void                nemo_rclone_service_favorite_free     (NemoRcloneFavorite *favorite);

/* Map a local path under an rclone mount to remote:path. */
gboolean            nemo_rclone_service_path_to_remote    (NemoRcloneService *self,
                                                           const char        *local_path,
                                                           char             **remote,
                                                           char             **remote_path,
                                                           char             **mountpoint);

/* Return a local path if the remote path is currently mounted. */
char               *nemo_rclone_service_remote_to_local   (NemoRcloneService *self,
                                                           const char        *remote,
                                                           const char        *path,
                                                           char             **mountpoint);

char               *nemo_rclone_service_format_path       (const char        *remote,
                                                           const char        *path);

/* Run a command synchronously and return its stdout (free with g_free). */
char               *nemo_rclone_service_run_capture       (NemoRcloneService *self,
                                                           const char *const *argv,
                                                           GError           **error);

void                nemo_rclone_service_prompt_add_favorite (NemoRcloneService *self,
                                                             GtkWindow         *parent,
                                                             const char        *local_path,
                                                             const char        *remote,
                                                             const char        *path);

G_END_DECLS

#endif /* NEMO_RCLONE_SERVICE_H */
