/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 William Jon McCann <mccann@jhu.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib-object.h>

#define DBUS_API_SUBJECT_TO_CHANGE
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

/* Needed for signal handling */
#include "gdm-common.h"

#include "gdm-manager.h"
#include "gdm-log.h"
#include "gdm-master-config.h"
#include "gdm-daemon-config-entries.h"

#include "misc.h"

#define GDM_DBUS_NAME "org.gnome.DisplayManager"

static void bus_proxy_destroyed_cb (DBusGProxy *bus_proxy,
                                    GdmManager *manager);

extern char **environ;

static char           **stored_argv   = NULL;
static int              stored_argc   = 0;
static GList           *stored_env    = NULL;
static GdmManager      *manager       = NULL;
static GdmDaemonConfig *daemon_config = NULL;
static uid_t            gdm_uid       = -1;
static gid_t            gdm_gid       = -1;

static gboolean
timed_exit_cb (GMainLoop *loop)
{
	g_main_loop_quit (loop);
	return FALSE;
}

static DBusGProxy *
get_bus_proxy (DBusGConnection *connection)
{
	DBusGProxy *bus_proxy;

	bus_proxy = dbus_g_proxy_new_for_name (connection,
					       DBUS_SERVICE_DBUS,
					       DBUS_PATH_DBUS,
					       DBUS_INTERFACE_DBUS);
	return bus_proxy;
}

static gboolean
acquire_name_on_proxy (DBusGProxy *bus_proxy)
{
	GError	   *error;
	guint	    result;
	gboolean    res;
	gboolean    ret;

	ret = FALSE;

	if (bus_proxy == NULL) {
		goto out;
	}

	error = NULL;
	res = dbus_g_proxy_call (bus_proxy,
				 "RequestName",
				 &error,
				 G_TYPE_STRING, GDM_DBUS_NAME,
				 G_TYPE_UINT, 0,
				 G_TYPE_INVALID,
				 G_TYPE_UINT, &result,
				 G_TYPE_INVALID);
	if (! res) {
		if (error != NULL) {
			g_warning ("Failed to acquire %s: %s", GDM_DBUS_NAME, error->message);
			g_error_free (error);
		} else {
			g_warning ("Failed to acquire %s", GDM_DBUS_NAME);
		}
		goto out;
	}

	if (result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		if (error != NULL) {
			g_warning ("Failed to acquire %s: %s", GDM_DBUS_NAME, error->message);
			g_error_free (error);
		} else {
			g_warning ("Failed to acquire %s", GDM_DBUS_NAME);
		}
		goto out;
	}

	ret = TRUE;

 out:
	return ret;
}

static DBusGConnection *
get_system_bus (void)
{
	GError		*error;
	DBusGConnection *bus;
	DBusConnection	*connection;

	error = NULL;
	bus = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
	if (bus == NULL) {
		g_warning ("Couldn't connect to system bus: %s",
			   error->message);
		g_error_free (error);
		goto out;
	}

	connection = dbus_g_connection_get_connection (bus);
	dbus_connection_set_exit_on_disconnect (connection, FALSE);

 out:
	return bus;
}

static gboolean
bus_reconnect (GdmManager *manager)
{
	DBusGConnection *bus;
	DBusGProxy	*bus_proxy;
	gboolean	 ret;

	ret = TRUE;

	bus = get_system_bus ();
	if (bus == NULL) {
		goto out;
	}

	bus_proxy = get_bus_proxy (bus);
	if (bus_proxy == NULL) {
		g_warning ("Could not construct bus_proxy object; will retry");
		goto out;
	}

	if (! acquire_name_on_proxy (bus_proxy) ) {
		g_warning ("Could not acquire name; will retry");
		goto out;
	}

	manager = gdm_manager_new ();
	if (manager == NULL) {
		g_warning ("Could not construct manager object");
		exit (1);
	}

	g_signal_connect (bus_proxy,
			  "destroy",
			  G_CALLBACK (bus_proxy_destroyed_cb),
			  manager);

	gdm_debug ("Successfully reconnected to D-Bus");

	ret = FALSE;

 out:
	return ret;
}

static void
bus_proxy_destroyed_cb (DBusGProxy *bus_proxy,
			GdmManager  *manager)
{
	gdm_debug ("Disconnected from D-Bus");

	g_object_unref (manager);
	manager = NULL;

	g_timeout_add (3000, (GSourceFunc)bus_reconnect, manager);
}

static void
delete_pid (void)
{
	unlink (GDM_PID_FILE);
}

static void
write_pid (void)
{
	int	pf;
	ssize_t written;
	char	pid[9];

	errno = 0;
	pf = open (GDM_PID_FILE, O_WRONLY|O_CREAT|O_TRUNC|O_EXCL, 0644);
	if (pf < 0) {
		g_warning (_("Cannot write PID file %s: possibly out of diskspace: %s"),
			   GDM_PID_FILE,
			   g_strerror (errno));

		return;
	}

	snprintf (pid, sizeof (pid), "%lu\n", (long unsigned) getpid ());
	errno = 0;
	written = write (pf, pid, strlen (pid));
	close (pf);

	if (written < 0) {
		g_warning (_("Cannot write PID file %s: possibly out of diskspace: %s"),
			   GDM_PID_FILE,
			   g_strerror (errno));
		return;
	}

	g_atexit (delete_pid);
}

static void
gdm_final_cleanup (void)
{
	g_object_unref (manager);
}

static void
main_saveenv (void)
{
	int i;

	g_list_foreach (stored_env, (GFunc)g_free, NULL);
	g_list_free (stored_env);
	stored_env = NULL;

	for (i = 0; environ[i] != NULL; i++) {
		char *env = environ[i];
		stored_env = g_list_prepend (stored_env, g_strdup (env));
	}
}

static void
main_restoreenv (void)
{
	GList *li;

	ve_clearenv ();

	/* FIXME: leaks */

	for (li = stored_env; li != NULL; li = li->next) {
		putenv (g_strdup (li->data));
	}
}

static void
gdm_restart_now (void)
{
	gdm_info (_("GDM restarting ..."));
	gdm_final_cleanup ();
	main_restoreenv ();
	VE_IGNORE_EINTR (execvp (stored_argv[0], stored_argv));
	g_warning (_("Failed to restart self"));
	_exit (1);
}

static void
store_argv (int	  argc,
	    char *argv[])
{
	int i;

	stored_argv = g_new0 (char *, argc + 1);
	for (i = 0; i < argc; i++) {
		stored_argv[i] = g_strdup (argv[i]);
	}
	stored_argv[i] = NULL;
	stored_argc = argc;
}

static void
check_logdir (void)
{
        struct stat     statbuf;
        int             r;
	char           *log_path;

	log_path = NULL;
	gdm_daemon_config_get_string_for_id (daemon_config, GDM_ID_LOG_DIR, &log_path);

        VE_IGNORE_EINTR (r = g_stat (log_path, &statbuf));
        if (r < 0 || ! S_ISDIR (statbuf.st_mode))  {
                gdm_fail (_("Logdir %s does not exist or isn't a directory."), log_path);
        }

	g_free (log_path);
}

static void
check_servauthdir (const char  *auth_path,
		   struct stat *statbuf)
{
	int r;

	/* Enter paranoia mode */
	VE_IGNORE_EINTR (r = g_stat (auth_path, statbuf));
	if G_UNLIKELY (r < 0) {
		gdm_fail (_("Authdir %s does not exist. Aborting."), auth_path);
	}

	if G_UNLIKELY (! S_ISDIR (statbuf->st_mode)) {
		gdm_fail (_("Authdir %s is not a directory. Aborting."), auth_path);
	}
}

static void
gdm_daemon_check_permissions (uid_t uid,
			      gid_t gid)
{
	struct stat statbuf;
	char       *auth_path;

	auth_path = NULL;
	gdm_daemon_config_get_string_for_id (daemon_config, GDM_ID_LOG_DIR, &auth_path);

	/* Enter paranoia mode */
	check_servauthdir (auth_path, &statbuf);

	NEVER_FAILS_root_set_euid_egid (0, 0);

	/* Now set things up for us as  */
	chown (auth_path, 0, gid);
	g_chmod (auth_path, (S_IRWXU|S_IRWXG|S_ISVTX));

	NEVER_FAILS_root_set_euid_egid (uid, gid);

	/* Again paranoid */
	check_servauthdir (auth_path, &statbuf);

	if G_UNLIKELY (statbuf.st_uid != 0 || statbuf.st_gid != gid)  {
		gdm_fail (_("Authdir %s is not owned by user %d, group %d. Aborting."),
			  auth_path,
			  (int)uid,
			  (int)gid);
	}

	if G_UNLIKELY (statbuf.st_mode != (S_IFDIR|S_IRWXU|S_IRWXG|S_ISVTX))  {
		gdm_fail (_("Authdir %s has wrong permissions %o. Should be %o. Aborting."),
			  auth_path,
			  statbuf.st_mode,
			  (S_IRWXU|S_IRWXG|S_ISVTX));
	}

	g_free (auth_path);
}

static void
gdm_daemon_change_user (uid_t *uidp,
			gid_t *gidp)
{
	char          *username;
	char          *groupname;
	uid_t          uid;
	gid_t          gid;
	struct passwd *pwent;
	struct group  *grent;

	username = NULL;
	groupname = NULL;
	uid = 0;
	gid = 0;

	gdm_daemon_config_get_string_for_id (daemon_config, GDM_ID_USER, &username);
	gdm_daemon_config_get_string_for_id (daemon_config, GDM_ID_GROUP, &groupname);

	/* Lookup user and groupid for the GDM user */
	pwent = getpwnam (username);

	/* Set uid and gid */
	if G_UNLIKELY (pwent == NULL) {
		gdm_fail (_("Can't find the GDM user '%s'. Aborting!"), username);
	} else {
		uid = pwent->pw_uid;
	}

	if G_UNLIKELY (uid == 0) {
		gdm_fail (_("The GDM user should not be root. Aborting!"));
	}

	grent = getgrnam (groupname);

	if G_UNLIKELY (grent == NULL) {
		gdm_fail (_("Can't find the GDM group '%s'. Aborting!"), groupname);
	} else  {
		gid = grent->gr_gid;
	}

	if G_UNLIKELY (gid == 0) {
		gdm_fail (_("The GDM group should not be root. Aborting!"));
	}

	/* gid remains `gdm' */
	NEVER_FAILS_root_set_euid_egid (uid, gid);

	if (uidp != NULL) {
		*uidp = uid;
	}

	if (gidp != NULL) {
		*gidp = gid;
	}

	g_free (username);
	g_free (groupname);
}

static void
setup_signal_handlers (void)
{
	/* FIXME */
}

int
main (int    argc,
      char **argv)
{
	GMainLoop	*loop;
	GOptionContext	*context;
	DBusGProxy	*bus_proxy;
	DBusGConnection *connection;
	int		 ret;
	int		 i;
	gboolean         debug;
	static char	   *config_file	     = NULL;
	static gboolean	    no_daemon	     = FALSE;
	static gboolean	    no_console	     = FALSE;
	static gboolean	    do_timed_exit    = FALSE;
	static gboolean	    print_version    = FALSE;
	static GOptionEntry entries []	 = {
		{ "config", 0, 0, G_OPTION_ARG_STRING, &config_file, N_("Alternative GDM System Defaults configuration file"), N_("CONFIGFILE") },

		{ "nodaemon", 0, 0, G_OPTION_ARG_NONE, &no_daemon, N_("Don't become a daemon"), NULL },
		{ "no-console", 0, 0, G_OPTION_ARG_NONE, &no_console, N_("No console (static) servers to be run"), NULL },

		{ "timed-exit", 0, 0, G_OPTION_ARG_NONE, &do_timed_exit, N_("Exit after a time - for debugging"), NULL },
		{ "version", 0, 0, G_OPTION_ARG_NONE, &print_version, N_("Print GDM version"), NULL },

		{ NULL }
	};

	bindtextdomain (GETTEXT_PACKAGE, GNOMELOCALEDIR);
	textdomain (GETTEXT_PACKAGE);
	setlocale (LC_ALL, "");

	ret = 1;

	g_type_init ();

	store_argv (argc, argv);
	main_saveenv ();

	context = g_option_context_new (_("GNOME Display Manager"));
	g_option_context_add_main_entries (context, entries, NULL);

	/* preprocess the arguments to support the xdm style -nodaemon
	 * option
	 */
	for (i = 0; i < argc; i++) {
		if (strcmp (argv[i], "-nodaemon") == 0)
			argv[i] = (char *) "--nodaemon";
	}

	g_option_context_parse (context, &argc, &argv, NULL);
	g_option_context_free (context);

	if (! no_daemon && daemon (0, 0)) {
		g_error ("Could not daemonize: %s", g_strerror (errno));
	}

	connection = get_system_bus ();
	if (connection == NULL) {
		goto out;
	}

	bus_proxy = get_bus_proxy (connection);
	if (bus_proxy == NULL) {
		g_warning ("Could not construct bus_proxy object; bailing out");
		goto out;
	}

	if (! acquire_name_on_proxy (bus_proxy) ) {
		g_warning ("Could not acquire name; bailing out");
		goto out;
	}

	gdm_log_init ();

	daemon_config = gdm_daemon_config_new ();
	gdm_daemon_config_load (daemon_config);

	debug = FALSE;
	gdm_daemon_config_get_bool_for_id (daemon_config, GDM_ID_DEBUG, &debug);
	gdm_log_set_debug (debug);

	gdm_daemon_change_user (&gdm_uid, &gdm_gid);
	gdm_daemon_check_permissions (gdm_uid, gdm_gid);
	NEVER_FAILS_root_set_euid_egid (0, 0);
	check_logdir ();

	/* XDM compliant error message */
	if (getuid () != 0) {
		/* make sure the pid file doesn't get wiped */
		g_warning (_("Only root wants to run GDM"));
		exit (-1);
	}

	/* pid file */
	delete_pid ();
	write_pid ();

	g_chdir (AUTHDIR);

#ifdef __sun
	g_unlink (SDTLOGIN_DIR);
	g_mkdir (SDTLOGIN_DIR, 0700);
#endif

	setup_signal_handlers ();

	manager = gdm_manager_new ();

	if (manager == NULL) {
		goto out;
	}

	g_signal_connect (bus_proxy,
			  "destroy",
			  G_CALLBACK (bus_proxy_destroyed_cb),
			  manager);

	loop = g_main_loop_new (NULL, FALSE);

	if (do_timed_exit) {
		g_timeout_add (1000 * 30, (GSourceFunc) timed_exit_cb, loop);
	}

	gdm_manager_start (manager);

	g_main_loop_run (loop);

	if (manager != NULL) {
		g_object_unref (manager);
	}
	if (daemon_config != NULL) {
		g_object_unref (daemon_config);
	}

	g_main_loop_unref (loop);

	ret = 0;

 out:

	return ret;
}