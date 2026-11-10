/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*-

   nemo-rclone-path.h: rclone remote path helpers

   rclone paths are not all absolute.  For SFTP (and Termux in particular)
   an empty path means "the remote's default root" (usually the login home),
   a path without a leading slash is relative to that root, and a path with
   a leading slash is an absolute server path.  Treating "/" as "the remote
   root" breaks Termux, where the server root is Android's unreadable "/".

   These helpers keep that distinction throughout the fork.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.
*/

#ifndef NEMO_RCLONE_PATH_H
#define NEMO_RCLONE_PATH_H

#include <glib.h>

G_BEGIN_DECLS

/* TRUE for NULL / "" (the remote's default root, often the login home). */
gboolean  nemo_rclone_path_is_home       (const char *path);

/* Trim trailing slashes, preserving the difference between "" and "/".
   Caller frees. */
char     *nemo_rclone_path_canonicalize  (const char *path);

/* "~" for the remote home, otherwise the path itself.  Caller frees. */
char     *nemo_rclone_path_display       (const char *path);

/* Join a directory-relative name onto base.  Absolute names replace base.
   An empty base means the remote home, so the result stays relative.
   Caller frees. */
char     *nemo_rclone_path_join          (const char *base,
                                          const char *name);

/* Parent directory.  The parent of "" is "" and the parent of "/" is "/".
   Caller frees. */
char     *nemo_rclone_path_parent        (const char *path);

/* Whether path lives under prefix in the same coordinate space:
   - an empty prefix matches relative paths (remote home)
   - "/" matches absolute paths (server root)
   - relative/absolute prefixes only match their own kind
*/
gboolean  nemo_rclone_path_is_under      (const char *path,
                                          const char *prefix);

/* The part of path below prefix, without a leading slash, or NULL when path
   is not under prefix.  Caller frees. */
char     *nemo_rclone_path_relative      (const char *path,
                                          const char *prefix);

G_END_DECLS

#endif /* NEMO_RCLONE_PATH_H */
