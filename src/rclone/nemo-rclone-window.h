/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*-

   nemo-rclone-window.h: rclone transfer window

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.
*/

#ifndef NEMO_RCLONE_WINDOW_H
#define NEMO_RCLONE_WINDOW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

void nemo_rclone_window_show (GtkWindow *parent);
void nemo_rclone_window_show_with_source (GtkWindow *parent,
                                          const char *remote,
                                          const char *path);

/* Open a "rclone://transfer" or "rclone://remote/path" location.  Paths that
   belong to a favorite are mounted on demand and opened locally; unknown
   paths open the transfer window with the source pre-filled. */
void nemo_rclone_open_uri (GtkWindow *parent,
                           const char *uri);
void nemo_rclone_window_show_with_selection (GtkWindow *parent,
                                             const char *remote,
                                             const char *path);

G_END_DECLS

#endif /* NEMO_RCLONE_WINDOW_H */
