/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*-

   nemo-rclone-thumbs.h: persistent per-directory thumbnail opt-in for
   rclone / remote mounts.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.
*/

#ifndef NEMO_RCLONE_THUMBS_H
#define NEMO_RCLONE_THUMBS_H

#include <glib.h>

G_BEGIN_DECLS

/* Load the whitelist from disk (idempotent; safe to call once at startup). */
void nemo_rclone_thumbs_init (void);

/* Return TRUE if thumbnails are enabled for <path> (or any ancestor
   directory that was explicitly enabled).  This is checked before the
   default local-only policy in nemo_file_should_show_thumbnail().
   The list only ever contains remote mount paths, so this is a cheap
   string-prefix match and never affects local files. */
gboolean nemo_rclone_thumbs_is_enabled (const char *path);

/* Record that the directory <dir_path> should always show thumbnails
   (add if not present, then persist).  No-op if already present. */
void nemo_rclone_thumbs_add (const char *dir_path);

/* Remove <dir_path> from the whitelist (and persist). */
void nemo_rclone_thumbs_remove (const char *dir_path);

G_END_DECLS

#endif /* NEMO_RCLONE_THUMBS_H */
