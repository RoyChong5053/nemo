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

/* Tunable FUSE/mount options for a single rclone mount.  All string fields
   are newly-allocated and owned by the caller; free a whole set with
   nemo_rclone_service_free_mount_opts().  "" means "use rclone's default". */
typedef struct {
	char *vfs_cache_mode;
	char *vfs_cache_max_size;
	char *vfs_cache_max_age;
	char *read_ahead;
	gint read_chunk_streams;
	char *dir_cache_time;
	char *attr_timeout;
	char *poll_interval;
} RcloneMountOpts;

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

/* Mount management (on-demand FUSE mounts under a predictable base path). */

/* Slugify a label into a filesystem-safe token suitable for a mountpoint. */
char               *nemo_rclone_service_slugify           (const char        *label);

/* Resolve the mountpoint for a favorite: keep an existing one, otherwise
   derive a deterministic path under the mount base (env NEMO_RCLONE_MOUNT_BASE,
   default ~/.local/share/nemo-rclone/mounts).  Caller frees the result. */
char               *nemo_rclone_service_resolve_mountpoint (NemoRcloneService *self,
                                                           const char        *remote,
                                                           const char        *path,
                                                           const char        *label);

/* Format a rclone location URI understood by Nemo ("rclone://remote/path").
   Caller frees the result. */
char               *nemo_rclone_service_format_uri        (const char        *remote,
                                                           const char        *path);

/* Path of the log file used by the mount for mountpoint.  Caller frees. */
char               *nemo_rclone_service_get_mount_log_path (const char        *mountpoint);

/* Return the tail of a mount log (or NULL when there is no log).  Caller
   frees the result. */
char               *nemo_rclone_service_get_mount_log_tail (const char        *mountpoint,
                                                            gsize              max_bytes);

/* Whether an rclone FUSE mount is currently active at mountpoint. */
gboolean            nemo_rclone_service_is_mount_active   (const char        *mountpoint);

/* Whether the favorite identified by id is currently mounted. */
gboolean            nemo_rclone_service_favorite_is_mounted (NemoRcloneService *self,
                                                             const char        *id);

/* Ensure the favorite identified by id is mounted.  On success returns a newly
   allocated string with the mountpoint (caller frees), and persists it.  On
   failure returns NULL and sets error. */
char               *nemo_rclone_service_mount_favorite    (NemoRcloneService *self,
                                                           const char        *id,
                                                           GError           **error);

/* Unmount the favorite identified by id (if mounted) and clear its persisted
   mountpoint.  Returns TRUE on success (or if nothing to do). */
/* Free a set previously returned by nemo_rclone_service_get_mount_opts(). */
void                nemo_rclone_service_free_mount_opts     (RcloneMountOpts   *opts);

/* Load the persisted FUSE/mount options (see ~/.config/nemo/rclone-mount.json,
   falling back to sensible defaults).  Returns a fresh set the caller owns. */
RcloneMountOpts    *nemo_rclone_service_get_mount_opts      (void);

/* Persist the given FUSE/mount options to disk.  The set keeps being owned by
   the caller. */
void                nemo_rclone_service_save_mount_opts     (RcloneMountOpts   *opts);
gboolean            nemo_rclone_service_unmount_favorite  (NemoRcloneService *self,
                                                           const char        *id,
                                                           GError           **error);

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
