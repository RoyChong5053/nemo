/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*-

   nemo-rclone-path.c: rclone remote path helpers

   See nemo-rclone-path.h for the path model.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.
*/

#include <config.h>

#include "nemo-rclone-path.h"

#include <string.h>

gboolean
nemo_rclone_path_is_home (const char *path)
{
	return path == NULL || path[0] == '\0';
}

char *
nemo_rclone_path_canonicalize (const char *path)
{
	char *out;
	gsize len;

	if (path == NULL) {
		return g_strdup ("");
	}

	out = g_strdup (path);
	len = strlen (out);

	while (len > 1 && out[len - 1] == '/') {
		out[--len] = '\0';
	}

	return out;
}

char *
nemo_rclone_path_display (const char *path)
{
	if (nemo_rclone_path_is_home (path)) {
		return g_strdup ("~");
	}

	return g_strdup (path);
}

char *
nemo_rclone_path_join (const char *base,
                       const char *name)
{
	char *clean_base;
	char *clean_name;
	char *result;

	if (name == NULL || name[0] == '\0') {
		return nemo_rclone_path_canonicalize (base);
	}

	/* An absolute name is not relative to base. */
	if (name[0] == '/') {
		return nemo_rclone_path_canonicalize (name);
	}

	if (nemo_rclone_path_is_home (base)) {
		return nemo_rclone_path_canonicalize (name);
	}

	clean_base = nemo_rclone_path_canonicalize (base);
	clean_name = nemo_rclone_path_canonicalize (name);

	if (g_strcmp0 (clean_base, "/") == 0) {
		result = g_strconcat ("/", clean_name, NULL);
	} else {
		result = g_strconcat (clean_base, "/", clean_name, NULL);
	}

	g_free (clean_base);
	g_free (clean_name);
	return result;
}

char *
nemo_rclone_path_parent (const char *path)
{
	char *clean;
	char *slash;

	if (nemo_rclone_path_is_home (path)) {
		return g_strdup ("");
	}

	clean = nemo_rclone_path_canonicalize (path);

	if (g_strcmp0 (clean, "/") == 0) {
		return clean;
	}

	slash = strrchr (clean, '/');
	if (slash == NULL) {
		/* A relative top-level entry: its parent is the remote home. */
		g_free (clean);
		return g_strdup ("");
	}

	if (slash == clean) {
		/* "/foo" -> "/" */
		clean[1] = '\0';
		return clean;
	}

	*slash = '\0';
	return clean;
}

gboolean
nemo_rclone_path_is_under (const char *path,
                           const char *prefix)
{
	char *clean_path;
	char *clean_prefix;
	gsize len;
	gboolean result;

	if (path == NULL) {
		return FALSE;
	}

	clean_path = nemo_rclone_path_canonicalize (path);
	clean_prefix = nemo_rclone_path_canonicalize (prefix);

	/* Empty prefix is the remote home: it only covers relative paths. */
	if (clean_prefix[0] == '\0') {
		result = clean_path[0] != '/';
		goto out;
	}

	/* "/" is the absolute server root: it only covers absolute paths. */
	if (g_strcmp0 (clean_prefix, "/") == 0) {
		result = clean_path[0] == '/';
		goto out;
	}

	/* Relative and absolute paths are separate namespaces. */
	if ((clean_prefix[0] == '/') != (clean_path[0] == '/')) {
		result = FALSE;
		goto out;
	}

	len = strlen (clean_prefix);
	result = strncmp (clean_path, clean_prefix, len) == 0 &&
	         (clean_path[len] == '\0' || clean_path[len] == '/');

out:
	g_free (clean_path);
	g_free (clean_prefix);
	return result;
}

char *
nemo_rclone_path_relative (const char *path,
                           const char *prefix)
{
	char *clean_path;
	char *clean_prefix;
	char *relative;

	if (!nemo_rclone_path_is_under (path, prefix)) {
		return NULL;
	}

	clean_path = nemo_rclone_path_canonicalize (path);
	clean_prefix = nemo_rclone_path_canonicalize (prefix);

	if (clean_prefix[0] == '\0') {
		relative = g_strdup (clean_path);
	} else if (g_strcmp0 (clean_prefix, "/") == 0) {
		relative = g_strdup (clean_path + 1);
	} else {
		relative = g_strdup (clean_path + strlen (clean_prefix));
		while (relative[0] == '/') {
			memmove (relative, relative + 1, strlen (relative));
		}
	}

	g_free (clean_path);
	g_free (clean_prefix);
	return relative;
}
