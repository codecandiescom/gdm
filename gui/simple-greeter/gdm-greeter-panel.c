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
#include <unistd.h>
#include <string.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib-object.h>
#include <gtk/gtk.h>

#include "gdm-greeter-panel.h"

#include "na-tray.h"

#define GDM_GREETER_PANEL_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GDM_TYPE_GREETER_PANEL, GdmGreeterPanelPrivate))

struct GdmGreeterPanelPrivate
{
        int                     monitor;
        GdkRectangle            geometry;
        GtkWidget              *hbox;
};

enum {
        PROP_0,
};

static void     gdm_greeter_panel_class_init  (GdmGreeterPanelClass *klass);
static void     gdm_greeter_panel_init        (GdmGreeterPanel      *greeter_panel);
static void     gdm_greeter_panel_finalize    (GObject              *object);

G_DEFINE_TYPE (GdmGreeterPanel, gdm_greeter_panel, GTK_TYPE_WINDOW)

static void
gdm_greeter_panel_set_property (GObject        *object,
                                guint           prop_id,
                                const GValue   *value,
                                GParamSpec     *pspec)
{
        GdmGreeterPanel *self;

        self = GDM_GREETER_PANEL (object);

        switch (prop_id) {
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
gdm_greeter_panel_get_property (GObject        *object,
                                guint           prop_id,
                                GValue         *value,
                                GParamSpec     *pspec)
{
        GdmGreeterPanel *self;

        self = GDM_GREETER_PANEL (object);

        switch (prop_id) {
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static GObject *
gdm_greeter_panel_constructor (GType                  type,
                               guint                  n_construct_properties,
                               GObjectConstructParam *construct_properties)
{
        GdmGreeterPanel      *greeter_panel;
        GdmGreeterPanelClass *klass;

        klass = GDM_GREETER_PANEL_CLASS (g_type_class_peek (GDM_TYPE_GREETER_PANEL));

        greeter_panel = GDM_GREETER_PANEL (G_OBJECT_CLASS (gdm_greeter_panel_parent_class)->constructor (type,
                                                                                                         n_construct_properties,
                                                                                                         construct_properties));

        return G_OBJECT (greeter_panel);
}

static void
gdm_greeter_panel_dispose (GObject *object)
{
        GdmGreeterPanel *greeter_panel;

        greeter_panel = GDM_GREETER_PANEL (object);

        g_debug ("Disposing greeter_panel");

        G_OBJECT_CLASS (gdm_greeter_panel_parent_class)->dispose (object);
}

/* copied from panel-toplevel.c */
static void
gdm_greeter_panel_move_resize_window (GdmGreeterPanel *panel,
                                      gboolean         move,
                                      gboolean         resize)
{
        GtkWidget *widget;

        widget = GTK_WIDGET (panel);

        g_assert (GTK_WIDGET_REALIZED (widget));

        if (move && resize) {
                gdk_window_move_resize (widget->window,
                                        panel->priv->geometry.x,
                                        panel->priv->geometry.y,
                                        panel->priv->geometry.width,
                                        panel->priv->geometry.height);
        } else if (move) {
                gdk_window_move (widget->window,
                                 panel->priv->geometry.x,
                                 panel->priv->geometry.y);
        } else if (resize) {
                gdk_window_resize (widget->window,
                                   panel->priv->geometry.width,
                                   panel->priv->geometry.height);
        }
}

static void
on_screen_size_changed (GdkScreen       *screen,
                        GdmGreeterPanel *panel)
{
        gtk_widget_queue_resize (GTK_WIDGET (panel));
}

static void
gdm_greeter_panel_real_realize (GtkWidget *widget)
{
        if (GTK_WIDGET_CLASS (gdm_greeter_panel_parent_class)->realize) {
                GTK_WIDGET_CLASS (gdm_greeter_panel_parent_class)->realize (widget);
        }

        gdm_greeter_panel_move_resize_window (GDM_GREETER_PANEL (widget), TRUE, TRUE);

        g_signal_connect (gtk_window_get_screen (GTK_WINDOW (widget)),
                          "size_changed",
                          G_CALLBACK (on_screen_size_changed),
                          widget);
}

static void
gdm_greeter_panel_real_unrealize (GtkWidget *widget)
{
        g_signal_handlers_disconnect_by_func (gtk_window_get_screen (GTK_WINDOW (widget)),
                                              on_screen_size_changed,
                                              widget);

        if (GTK_WIDGET_CLASS (gdm_greeter_panel_parent_class)->unrealize) {
                GTK_WIDGET_CLASS (gdm_greeter_panel_parent_class)->unrealize (widget);
        }
}

static GdkRegion *
get_outside_region (GdmGreeterPanel *panel)
{
        int        i;
        GdkRegion *region;

        region = gdk_region_new ();
        for (i = 0; i < panel->priv->monitor; i++) {
                GdkRectangle geometry;

                gdk_screen_get_monitor_geometry (GTK_WINDOW (panel)->screen,
                                                 i,
                                                 &geometry);
                gdk_region_union_with_rect (region, &geometry);
        }

        return region;
}

static void
get_monitor_geometry (GdmGreeterPanel *panel,
                      GdkRectangle    *geometry)
{
        GdkRegion   *outside_region;
        GdkRegion   *monitor_region;
        GdkRectangle geom;

        outside_region = get_outside_region (panel);

        gdk_screen_get_monitor_geometry (GTK_WINDOW (panel)->screen,
                                         panel->priv->monitor,
                                         &geom);
        monitor_region = gdk_region_rectangle (&geom);
        gdk_region_subtract (monitor_region, outside_region);
        gdk_region_destroy (outside_region);

        gdk_region_get_clipbox (monitor_region, geometry);
        gdk_region_destroy (monitor_region);
}

static void
set_struts (GdmGreeterPanel *panel,
            int              x,
            int              y,
            int              width,
            int              height)
{
        gulong        data[12] = { 0, };
        int           screen_height;

        /* _NET_WM_STRUT_PARTIAL: CARDINAL[12]/32
         *
         * 0: left          1: right       2:  top             3:  bottom
         * 4: left_start_y  5: left_end_y  6:  right_start_y   7:  right_end_y
         * 8: top_start_x   9: top_end_x   10: bottom_start_x  11: bottom_end_x
         *
         * Note: In xinerama use struts relative to combined screen dimensions,
         *       not just the current monitor.
         */

        /* for bottom panel */
        screen_height = gdk_screen_get_height (gtk_window_get_screen (GTK_WINDOW (panel)));

        /* bottom */
        data[3] = screen_height - panel->priv->geometry.y - panel->priv->geometry.height + height;
        /* bottom_start_x */
        data[10] = x;
        /* bottom_end_x */
        data[11] = x + width;

        g_debug ("Setting strut: bottom=%lu bottom_start_x=%lu bottom_end_x=%lu", data[3], data[10], data[11]);

        gdk_error_trap_push ();

        gdk_property_change (GTK_WIDGET (panel)->window,
                             gdk_atom_intern ("_NET_WM_STRUT_PARTIAL", FALSE),
                             gdk_atom_intern ("CARDINAL", FALSE),
                             32,
                             GDK_PROP_MODE_REPLACE,
                             (guchar *) &data,
                             12);

        gdk_property_change (GTK_WIDGET (panel)->window,
                             gdk_atom_intern ("_NET_WM_STRUT", FALSE),
                             gdk_atom_intern ("CARDINAL", FALSE),
                             32,
                             GDK_PROP_MODE_REPLACE,
                             (guchar *) &data,
                             4);

        gdk_error_trap_pop ();
}

static void
update_struts (GdmGreeterPanel *panel)
{
        GdkRectangle geometry;

        get_monitor_geometry (panel, &geometry);

        /* FIXME: assumes only one panel */
        set_struts (panel,
                    panel->priv->geometry.x,
                    panel->priv->geometry.y,
                    panel->priv->geometry.width,
                    panel->priv->geometry.height);
}

static void
update_geometry (GdmGreeterPanel *panel,
                 GtkRequisition  *requisition)
{
        GdkRectangle geometry;
        int          height;

        get_monitor_geometry (panel, &geometry);

        height = requisition->height;
        panel->priv->geometry.width = geometry.width;
        panel->priv->geometry.height = height * 2;

        panel->priv->geometry.x = geometry.x;
        panel->priv->geometry.y = geometry.y + geometry.height - panel->priv->geometry.height;

        g_debug ("Setting geometry x:%d y:%d w:%d h:%d",
                 panel->priv->geometry.x,
                 panel->priv->geometry.y,
                 panel->priv->geometry.width,
                 panel->priv->geometry.height);

        update_struts (panel);
}

static void
gdm_greeter_panel_real_size_request (GtkWidget      *widget,
                                     GtkRequisition *requisition)
{
        GdmGreeterPanel *panel;
        GtkBin          *bin;
        GdkRectangle     old_geometry;
        int              position_changed = FALSE;
        int              size_changed = FALSE;

        panel = GDM_GREETER_PANEL (widget);
        bin = GTK_BIN (widget);

        if (bin->child && GTK_WIDGET_VISIBLE (bin->child)) {
                gtk_widget_size_request (bin->child, requisition);
        }

        old_geometry = panel->priv->geometry;

        update_geometry (panel, requisition);

        requisition->width  = panel->priv->geometry.width;
        requisition->height = panel->priv->geometry.height;

        if (! GTK_WIDGET_REALIZED (widget)) {
                return;
        }

        if (old_geometry.width  != panel->priv->geometry.width ||
            old_geometry.height != panel->priv->geometry.height) {
                size_changed = TRUE;
        }

        if (old_geometry.x != panel->priv->geometry.x ||
            old_geometry.y != panel->priv->geometry.y) {
                position_changed = TRUE;
        }

        gdm_greeter_panel_move_resize_window (panel, position_changed, size_changed);
}

static void
gdm_greeter_panel_class_init (GdmGreeterPanelClass *klass)
{
        GObjectClass   *object_class = G_OBJECT_CLASS (klass);
        GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

        object_class->get_property = gdm_greeter_panel_get_property;
        object_class->set_property = gdm_greeter_panel_set_property;
        object_class->constructor = gdm_greeter_panel_constructor;
        object_class->dispose = gdm_greeter_panel_dispose;
        object_class->finalize = gdm_greeter_panel_finalize;

        widget_class->realize = gdm_greeter_panel_real_realize;
        widget_class->unrealize = gdm_greeter_panel_real_unrealize;
        widget_class->size_request = gdm_greeter_panel_real_size_request;

        g_type_class_add_private (klass, sizeof (GdmGreeterPanelPrivate));
}

static void
gdm_greeter_panel_init (GdmGreeterPanel *panel)
{
        GtkWidget *label;
        GtkWidget *tray;

        panel->priv = GDM_GREETER_PANEL_GET_PRIVATE (panel);

        panel->priv->geometry.x      = -1;
        panel->priv->geometry.y      = -1;
        panel->priv->geometry.width  = -1;
        panel->priv->geometry.height = -1;

        gtk_window_set_decorated (GTK_WINDOW (panel), FALSE);

        gtk_window_set_skip_taskbar_hint (GTK_WINDOW (panel), TRUE);
        gtk_window_set_skip_pager_hint (GTK_WINDOW (panel), TRUE);

        gtk_window_set_keep_above (GTK_WINDOW (panel), TRUE);
        gtk_window_set_type_hint (GTK_WINDOW (panel), GDK_WINDOW_TYPE_HINT_DOCK);

        panel->priv->hbox = gtk_hbox_new (FALSE, 12);
        gtk_widget_show (panel->priv->hbox);
        gtk_container_add (GTK_CONTAINER (panel), panel->priv->hbox);

        label = gtk_label_new (g_get_host_name ());
        gtk_box_pack_start (GTK_BOX (panel->priv->hbox), label, FALSE, FALSE, 6);
        gtk_widget_show (label);

        tray = na_tray_new_for_screen (gtk_window_get_screen (GTK_WINDOW (panel)),
                                       GTK_ORIENTATION_HORIZONTAL);
        gtk_box_pack_end (GTK_BOX (panel->priv->hbox), tray, FALSE, FALSE, 6);
        gtk_widget_show (tray);

}

static void
gdm_greeter_panel_finalize (GObject *object)
{
        GdmGreeterPanel *greeter_panel;

        g_return_if_fail (object != NULL);
        g_return_if_fail (GDM_IS_GREETER_PANEL (object));

        greeter_panel = GDM_GREETER_PANEL (object);

        g_return_if_fail (greeter_panel->priv != NULL);

        G_OBJECT_CLASS (gdm_greeter_panel_parent_class)->finalize (object);
}

GtkWidget *
gdm_greeter_panel_new (void)
{
        GObject *object;

        object = g_object_new (GDM_TYPE_GREETER_PANEL,
                               NULL);

        return GTK_WIDGET (object);
}