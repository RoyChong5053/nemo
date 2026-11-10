/* Standalone unit test for src/rclone/nemo-rclone-path.c helpers. */
#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "nemo-rclone-path.h"

static int failures = 0;

static void
check_str (const char *label, const char *got, const char *want)
{
	if (g_strcmp0 (got, want) != 0) {
		fprintf (stderr, "FAIL %s: got \"%s\" want \"%s\"\n",
		         label, got ? got : "(null)", want ? want : "(null)");
		failures++;
	} else {
		printf ("ok   %s = \"%s\"\n", label, got ? got : "(null)");
	}
}

static void
check_bool (const char *label, gboolean got, gboolean want)
{
	if (got != want) {
		fprintf (stderr, "FAIL %s: got %d want %d\n", label, got, want);
		failures++;
	} else {
		printf ("ok   %s = %d\n", label, got);
	}
}

int
main (void)
{
	char *s;

	/* is_home */
	check_bool ("is_home(NULL)", nemo_rclone_path_is_home (NULL), TRUE);
	check_bool ("is_home(\"\")", nemo_rclone_path_is_home (""), TRUE);
	check_bool ("is_home(\"/\")", nemo_rclone_path_is_home ("/"), FALSE);

	/* canonicalize keeps "" vs "/" */
	s = nemo_rclone_path_canonicalize (NULL); check_str ("canonicalize(NULL)", s, ""); g_free (s);
	s = nemo_rclone_path_canonicalize (""); check_str ("canonicalize(\"\")", s, ""); g_free (s);
	s = nemo_rclone_path_canonicalize ("/"); check_str ("canonicalize(\"/\")", s, "/"); g_free (s);
	s = nemo_rclone_path_canonicalize ("a/b//"); check_str ("canonicalize(a/b//)", s, "a/b"); g_free (s);
	s = nemo_rclone_path_canonicalize ("/a/b/"); check_str ("canonicalize(/a/b/)", s, "/a/b"); g_free (s);

	/* display */
	s = nemo_rclone_path_display (""); check_str ("display(\"\")", s, "~"); g_free (s);
	s = nemo_rclone_path_display ("a"); check_str ("display(a)", s, "a"); g_free (s);

	/* join: empty base keeps relative (this is the Termux fix) */
	s = nemo_rclone_path_join ("", "Documents"); check_str ("join(\"\",Documents)", s, "Documents"); g_free (s);
	s = nemo_rclone_path_join ("", ""); check_str ("join(\"\",\"\")", s, ""); g_free (s);
	s = nemo_rclone_path_join ("/home/roychong", "x"); check_str ("join(abs,x)", s, "/home/roychong/x"); g_free (s);
	s = nemo_rclone_path_join ("a", "b"); check_str ("join(a,b)", s, "a/b"); g_free (s);
	s = nemo_rclone_path_join ("a", "/abs"); check_str ("join(a,/abs)", s, "/abs"); g_free (s);
	s = nemo_rclone_path_join ("/", "x"); check_str ("join(/,x)", s, "/x"); g_free (s);

	/* parent */
	s = nemo_rclone_path_parent (""); check_str ("parent(\"\")", s, ""); g_free (s);
	s = nemo_rclone_path_parent ("a"); check_str ("parent(a)", s, ""); g_free (s);
	s = nemo_rclone_path_parent ("a/b"); check_str ("parent(a/b)", s, "a"); g_free (s);
	s = nemo_rclone_path_parent ("/a"); check_str ("parent(/a)", s, "/"); g_free (s);
	s = nemo_rclone_path_parent ("/"); check_str ("parent(/)", s, "/"); g_free (s);

	/* is_under namespaces */
	check_bool ("under(\"\",\"\")", nemo_rclone_path_is_under ("", ""), TRUE);
	check_bool ("under(a,\"\")", nemo_rclone_path_is_under ("a", ""), TRUE);
	check_bool ("under(/a,\"\")", nemo_rclone_path_is_under ("/a", ""), FALSE);
	check_bool ("under(/a,/)", nemo_rclone_path_is_under ("/a", "/"), TRUE);
	check_bool ("under(a,/)", nemo_rclone_path_is_under ("a", "/"), FALSE);
	check_bool ("under(a/b,a)", nemo_rclone_path_is_under ("a/b", "a"), TRUE);
	check_bool ("under(ab,a)", nemo_rclone_path_is_under ("ab", "a"), FALSE);
	check_bool ("under(a/b,/a)", nemo_rclone_path_is_under ("a/b", "/a"), FALSE);

	/* relative */
	s = nemo_rclone_path_relative ("a/b", ""); check_str ("rel(a/b,\"\")", s, "a/b"); g_free (s);
	s = nemo_rclone_path_relative ("/a/b", "/"); check_str ("rel(/a/b,/)", s, "a/b"); g_free (s);
	s = nemo_rclone_path_relative ("/a/b", "/a"); check_str ("rel(/a/b,/a)", s, "b"); g_free (s);
	s = nemo_rclone_path_relative ("a/b", "a"); check_str ("rel(a/b,a)", s, "b"); g_free (s);
	s = nemo_rclone_path_relative ("a/b", "a/b"); check_str ("rel(a/b,a/b)", s, ""); g_free (s);
	s = nemo_rclone_path_relative ("x", "a"); check_str ("rel(x,a)", s, NULL); g_free (s);

	if (failures != 0) {
		fprintf (stderr, "\n%d FAILURES\n", failures);
		return 1;
	}

	printf ("\nall path helper tests passed\n");
	return 0;
}
