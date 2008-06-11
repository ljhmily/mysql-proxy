/* Copyright (C) 2008 MySQL AB
 
 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; version 2 of the License.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */ 

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "chassis-mainloop.h"

#if GLIB_CHECK_VERSION(2, 16, 0)
#define C(x) x, sizeof(x) - 1

#define START_TEST(x) void (x)(void)

/**
 * Tests for the chassis basedir handling
 * @ingroup path
 */

/*@{*/

/**
 * load 
 */
START_TEST(test_path_basedir) {
	gchar *filename;
	chassis *chas;

	chas = chassis_init();
	chas->base_dir = g_strdup("/tmp");
	
	filename = g_strdup("some/relative/path/file");
	
	/* resolving this path must lead to changing the filename */
	g_assert_cmpint(chassis_resolve_path(chas, &filename), ==, 1);
	
	g_assert_cmpint(g_strcmp0("/tmp/some/relative/path/file", filename), ==, 0);
	
	g_free(filename);
	chassis_free(chas);
}

START_TEST(test_no_basedir) {
	gchar *filename;
	chassis *chas;
	
	chas = chassis_init();
	chas->base_dir = NULL;
	
	filename = g_strdup("some/relative/path/file");
	
	/* resolving this path must lead to changing the filename */
	g_assert_cmpint(chassis_resolve_path(chas, &filename), ==, 0);
	
	g_assert_cmpint(g_strcmp0("some/relative/path/file", filename), ==, 0);
	
	g_free(filename);
	chassis_free(chas);
}

START_TEST(test_abspath_basedir) {
	gchar *filename;
	chassis *chas;
	
	chas = chassis_init();
	chas->base_dir = g_strdup("/tmp");
	
	filename = g_strdup("/some/relative/path/file");
	
	/* resolving this path must lead to no change in the filename */
	g_assert_cmpint(chassis_resolve_path(chas, &filename), ==, 0);
	
	g_assert_cmpint(g_strcmp0("/some/relative/path/file", filename), ==, 0);
	
	g_free(filename);
	chassis_free(chas);
}
/*@}*/

int main(int argc, char **argv) {
	g_test_init(&argc, &argv, NULL);
	g_test_bug_base("http://bugs.mysql.com/");
	
	g_test_add_func("/core/basedir/relpath", test_path_basedir);
	g_test_add_func("/core/basedir/nobasedir", test_no_basedir);
	g_test_add_func("/core/basedir/abspath", test_abspath_basedir);
	
	return g_test_run();
}
#else
int main() {
	return 77;
}
#endif