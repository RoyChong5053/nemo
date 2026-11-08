/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*-

   nemo-remote-policy.h: policy helpers for slow remote filesystems

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public
   License along with this program; if not, write to the Free
   Software Foundation, Inc., 51 Franklin Street - Suite 500,
   Boston, MA 02110-1335, USA.
*/

#ifndef NEMO_REMOTE_POLICY_H
#define NEMO_REMOTE_POLICY_H

#include <glib.h>
#include <libnemo-private/nemo-file.h>
#include <libnemo-private/nemo-directory.h>

G_BEGIN_DECLS

/* Returns TRUE for network/FUSE filesystems where every metadata round trip
 * is expensive (rclone, sshfs, cifs, nfs, ...).  The path must be a native
 * filesystem path. */
gboolean nemo_remote_policy_path_is_slow (const char *path);

/* Convenience wrappers. */
gboolean nemo_remote_policy_file_is_slow (NemoFile *file);
gboolean nemo_remote_policy_directory_is_slow (NemoDirectory *directory);

/* TRUE when a directory item count should not be computed automatically. */
gboolean nemo_remote_policy_should_skip_item_count (NemoFile *file);

/* The attribute list to use for enumeration/queries.  Slow remotes get a
 * lean set so that we do not trigger access()/getxattr() for every file. */
const char *nemo_remote_policy_get_attributes (NemoFile *file);

/* Description of the mount for diagnostics (free with g_free). */
char *nemo_remote_policy_describe (const char *path);

G_END_DECLS

#endif /* NEMO_REMOTE_POLICY_H */
