/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
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
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <glib-object.h>

#include <libgnomevfs/gnome-vfs-ops.h>

#include "gdm-user-manager.h"
#include "gdm-user-private.h"

#define GDM_USER_MANAGER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GDM_TYPE_USER_MANAGER, GdmUserManagerPrivate))

/* Prefs Defaults */
#define DEFAULT_ALLOW_ROOT      TRUE
#define DEFAULT_MAX_ICON_SIZE   128
#define DEFAULT_USER_MAX_FILE   65536
#define DEFAULT_MINIMAL_UID     500
#define DEFAULT_GLOBAL_FACE_DIR DATADIR "/faces"
#define DEFAULT_USER_ICON       "stock_person"
#define DEFAULT_EXCLUDE         { "bin",        \
                                  "daemon",     \
                                  "adm",        \
                                  "lp",         \
                                  "sync",       \
                                  "shutdown",   \
                                  "halt",       \
                                  "mail",       \
                                  "news",       \
                                  "uucp",       \
                                  "operator",   \
                                  "nobody",     \
                                  "gdm",        \
                                  "postgres",   \
                                  "pvm",        \
                                  "rpm",        \
                                  "nfsnobody",  \
                                  "pcap",       \
                                  NULL }

struct GdmUserManagerPrivate
{
        GHashTable            *users;
        GHashTable            *shells;
        GHashTable            *exclusions;
        GnomeVFSMonitorHandle *passwd_monitor;
        GnomeVFSMonitorHandle *shells_monitor;

        guint                  reload_id;
        uid_t                  minimal_uid;

        guint8                 users_dirty : 1;
};

enum {
        USER_ADDED,
        USER_REMOVED,
        LAST_SIGNAL
};

static guint signals [LAST_SIGNAL] = { 0, };

static void     gdm_user_manager_class_init (GdmUserManagerClass *klass);
static void     gdm_user_manager_init       (GdmUserManager      *user_manager);
static void     gdm_user_manager_finalize   (GObject             *object);

static gpointer user_manager_object = NULL;

G_DEFINE_TYPE (GdmUserManager, gdm_user_manager, G_TYPE_OBJECT)

GQuark
gdm_user_manager_error_quark (void)
{
        static GQuark ret = 0;
        if (ret == 0) {
                ret = g_quark_from_static_string ("gdm_user_manager_error");
        }

        return ret;
}

/**
 * gdm_manager_get_user:
 * @manager: the manager to query.
 * @username: the login name of the user to get.
 *
 * Retrieves a pointer to the #GdmUser object for the login named @username
 * from @manager. This pointer is not a reference, and should not be released.
 *
 * Returns: a pointer to a #GdmUser object.
 **/
GdmUser *
gdm_user_manager_get_user (GdmUserManager *manager,
                           const char     *username)
{
        GdmUser *user;

        g_return_val_if_fail (GDM_IS_USER_MANAGER (manager), NULL);
        g_return_val_if_fail (username != NULL && username[0] != '\0', NULL);

        user = g_hash_table_lookup (manager->priv->users, username);

        if (user == NULL) {
                struct passwd *pwent;

                pwent = getpwnam (username);

                if (pwent != NULL) {
                        user = g_object_new (GDM_TYPE_USER, "manager", manager, NULL);
                        _gdm_user_update (user, pwent);
                        g_hash_table_insert (manager->priv->users,
                                             g_strdup (pwent->pw_name),
                                             user);
                        g_signal_emit (manager, signals[USER_ADDED], 0, user);
                }
        }

        return user;
}

static void
listify_hash_values_hfunc (gpointer key,
                           gpointer value,
                           gpointer user_data)
{
        GSList **list = user_data;

        *list = g_slist_prepend (*list, value);
}

GSList *
gdm_user_manager_list_users (GdmUserManager *manager)
{
        GSList *retval;

        g_return_val_if_fail (GDM_IS_USER_MANAGER (manager), NULL);

        retval = NULL;
        g_hash_table_foreach (manager->priv->users, listify_hash_values_hfunc, &retval);

        return g_slist_sort (retval, (GCompareFunc) gdm_user_collate);
}

static void
reload_passwd (GdmUserManager *manager)
{
        struct passwd *pwent;
        GSList        *old_users;
        GSList        *new_users;
        GSList        *list;

        old_users = NULL;
        new_users = NULL;

        g_hash_table_foreach (manager->priv->users, listify_hash_values_hfunc, &old_users);
        g_slist_foreach (old_users, (GFunc) g_object_ref, NULL);

        /* Make sure we keep users who are logged in no matter what. */
        for (list = old_users; list; list = list->next) {
                if (gdm_user_get_n_sessions (list->data)) {
                        g_object_freeze_notify (G_OBJECT (list->data));
                        new_users = g_slist_prepend (new_users, g_object_ref (list->data));
                }
        }

        setpwent ();

        for (pwent = getpwent (); pwent; pwent = getpwent ()) {
                GdmUser *user;

                user = NULL;

                /* Skip users below MinimalUID... */
                if (pwent->pw_uid < manager->priv->minimal_uid) {
                        continue;
                }

                /* ...And users w/ invalid shells... */
                if (!pwent->pw_shell ||
                    !g_hash_table_lookup (manager->priv->shells, pwent->pw_shell)) {
                        continue;
                }

                /* ...And explicitly excluded users */
                if (g_hash_table_lookup (manager->priv->exclusions, pwent->pw_name)) {
                        continue;
                }

                user = g_hash_table_lookup (manager->priv->users, pwent->pw_name);

                /* Update users already in the *new* list */
                if (g_slist_find (new_users, user)) {
                        _gdm_user_update (user, pwent);
                        continue;
                }

                if (user == NULL) {
                        user = g_object_new (GDM_TYPE_USER,
                                             "manager", manager,
                                             NULL);
                } else {
                        g_object_ref (user);
                }

                /* Freeze & update users not already in the new list */
                g_object_freeze_notify (G_OBJECT (user));
                _gdm_user_update (user, pwent);

                new_users = g_slist_prepend (new_users, user);
        }

        endpwent ();

        /* Go through and handle added users */
        for (list = new_users; list; list = list->next) {
                if (! g_slist_find (old_users, list->data)) {
                        g_hash_table_insert (manager->priv->users,
                                             g_strdup (gdm_user_get_user_name (list->data)),
                                             g_object_ref (list->data));
                        g_signal_emit (manager, signals[USER_ADDED], 0, list->data);
                }
        }

        /* Go through and handle removed users */
        for (list = old_users; list; list = list->next) {
                if (! g_slist_find (new_users, list->data)) {
                        g_signal_emit (manager, signals[USER_REMOVED], 0, list->data);
                        g_hash_table_remove (manager->priv->users,
                                             gdm_user_get_user_name (list->data));
                }
        }

        /* Cleanup */
        g_slist_foreach (new_users, (GFunc) g_object_thaw_notify, NULL);
        g_slist_foreach (new_users, (GFunc) g_object_unref, NULL);
        g_slist_free (new_users);

        g_slist_foreach (old_users, (GFunc) g_object_unref, NULL);
        g_slist_free (old_users);
}

static void
reload_shells (GdmUserManager *manager)
{
        char *shell;

        setusershell ();

        g_hash_table_remove_all (manager->priv->shells);
        for (shell = getusershell (); shell; shell = getusershell ()) {
                g_hash_table_insert (manager->priv->shells,
                                     g_strdup (shell),
                                     GUINT_TO_POINTER (TRUE));
        }

        endusershell ();
}

static void
shells_monitor_cb (GnomeVFSMonitorHandle    *handle,
                   const gchar              *text_uri,
                   const gchar              *info_uri,
                   GnomeVFSMonitorEventType  event_type,
                   GdmUserManager           *manager)
{
        if (event_type != GNOME_VFS_MONITOR_EVENT_CHANGED &&
            event_type != GNOME_VFS_MONITOR_EVENT_CREATED)
                return;

        reload_shells (manager);
        reload_passwd (manager);
}

static void
passwd_monitor_cb (GnomeVFSMonitorHandle    *handle,
                   const gchar              *text_uri,
                   const gchar              *info_uri,
                   GnomeVFSMonitorEventType  event_type,
                   GdmUserManager           *manager)
{
        if (event_type != GNOME_VFS_MONITOR_EVENT_CHANGED &&
            event_type != GNOME_VFS_MONITOR_EVENT_CREATED)
                return;

        reload_passwd (manager);
}

static void
gdm_user_manager_class_init (GdmUserManagerClass *klass)
{
        GObjectClass   *object_class = G_OBJECT_CLASS (klass);

        object_class->finalize = gdm_user_manager_finalize;

        signals [USER_ADDED] =
                g_signal_new ("user-added",
                              G_TYPE_FROM_CLASS (klass),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GdmUserManagerClass, user_added),
                              NULL, NULL,
                              g_cclosure_marshal_VOID__OBJECT,
                              G_TYPE_NONE, 1, GDM_TYPE_USER);
        signals [USER_REMOVED] =
                g_signal_new ("user-removed",
                              G_TYPE_FROM_CLASS (klass),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GdmUserManagerClass, user_removed),
                              NULL, NULL,
                              g_cclosure_marshal_VOID__OBJECT,
                              G_TYPE_NONE, 1, GDM_TYPE_USER);

        g_type_class_add_private (klass, sizeof (GdmUserManagerPrivate));
}

static gboolean
reload_passwd_timeout (GdmUserManager *manager)
{
        reload_passwd (manager);
        manager->priv->reload_id = 0;
        return FALSE;
}

static void
queue_reload_passwd (GdmUserManager *manager)
{
        if (manager->priv->reload_id > 0) {
                return;
        }

        manager->priv->reload_id = g_idle_add ((GSourceFunc)reload_passwd_timeout, manager);
}

static void
gdm_user_manager_init (GdmUserManager *manager)
{
        GError        *error;
        char          *uri;
        GnomeVFSResult result;
        int            i;
        const char    *exclude_default[] = DEFAULT_EXCLUDE;

        manager->priv = GDM_USER_MANAGER_GET_PRIVATE (manager);

        manager->priv->minimal_uid = DEFAULT_MINIMAL_UID;

        /* exclusions */
        manager->priv->exclusions = g_hash_table_new_full (g_str_hash,
                                                           g_str_equal,
                                                           g_free,
                                                           NULL);
        for (i = 0; exclude_default[i] != NULL; i++) {
                g_hash_table_insert (manager->priv->exclusions,
                                     g_strdup (exclude_default [i]),
                                     GUINT_TO_POINTER (TRUE));
        }

        /* /etc/shells */
        manager->priv->shells = g_hash_table_new_full (g_str_hash,
                                                       g_str_equal,
                                                       g_free,
                                                       NULL);
        reload_shells (manager);
        error = NULL;
        uri = g_filename_to_uri ("/etc/shells", NULL, &error);
        if (uri == NULL) {
                g_critical ("Could not create URI for shells file `/etc/shells': %s",
                            error->message);
                g_error_free (error);
        } else {
                result = gnome_vfs_monitor_add (&(manager->priv->shells_monitor),
                                                uri,
                                                GNOME_VFS_MONITOR_FILE,
                                                (GnomeVFSMonitorCallback)shells_monitor_cb,
                                                manager);
                g_free (uri);

                if (result != GNOME_VFS_OK)
                        g_critical ("Could not install monitor for shells file `/etc/shells': %s",
                                    gnome_vfs_result_to_string (result));
        }

        /* /etc/passwd */
        manager->priv->users = g_hash_table_new_full (g_str_hash,
                                                      g_str_equal,
                                                      g_free,
                                                      (GDestroyNotify) g_object_run_dispose);
        error = NULL;
        uri = g_filename_to_uri ("/etc/passwd", NULL, &error);
        if (uri == NULL) {
                g_critical ("Could not create URI for password file `/etc/passwd': %s",
                            error->message);
                g_error_free (error);
        } else {
                result = gnome_vfs_monitor_add (&(manager->priv->passwd_monitor),
                                                uri,
                                                GNOME_VFS_MONITOR_FILE,
                                                (GnomeVFSMonitorCallback)passwd_monitor_cb,
                                                manager);
                g_free (uri);

                if (result != GNOME_VFS_OK)
                        g_critical ("Could not install monitor for password file `/etc/passwd: %s",
                                    gnome_vfs_result_to_string (result));
        }

        /* FIXME: add ConsoleKit seat monitoring */

        queue_reload_passwd (manager);

        manager->priv->users_dirty = FALSE;

}

static void
gdm_user_manager_finalize (GObject *object)
{
        GdmUserManager *manager;

        g_return_if_fail (object != NULL);
        g_return_if_fail (GDM_IS_USER_MANAGER (object));

        manager = GDM_USER_MANAGER (object);

        g_return_if_fail (manager->priv != NULL);

        if (manager->priv->reload_id > 0) {
                g_source_remove (manager->priv->reload_id);
                manager->priv->reload_id = 0;
        }

        gnome_vfs_monitor_cancel (manager->priv->shells_monitor);
        g_hash_table_destroy (manager->priv->shells);

        gnome_vfs_monitor_cancel (manager->priv->passwd_monitor);
        g_hash_table_destroy (manager->priv->users);

        G_OBJECT_CLASS (gdm_user_manager_parent_class)->finalize (object);
}

GdmUserManager *
gdm_user_manager_ref_default (void)
{
        if (user_manager_object != NULL) {
                g_object_ref (user_manager_object);
        } else {
                user_manager_object = g_object_new (GDM_TYPE_USER_MANAGER, NULL);
                g_object_add_weak_pointer (user_manager_object,
                                           (gpointer *) &user_manager_object);
        }

        return GDM_USER_MANAGER (user_manager_object);
}