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

#ifndef __GDM_HOST_CHOOSER_WIDGET_H
#define __GDM_HOST_CHOOSER_WIDGET_H

#include <glib-object.h>
#include <gtk/gtkvbox.h>

G_BEGIN_DECLS

#define GDM_TYPE_HOST_CHOOSER_WIDGET         (gdm_host_chooser_widget_get_type ())
#define GDM_HOST_CHOOSER_WIDGET(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), GDM_TYPE_HOST_CHOOSER_WIDGET, GdmHostChooserWidget))
#define GDM_HOST_CHOOSER_WIDGET_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), GDM_TYPE_HOST_CHOOSER_WIDGET, GdmHostChooserWidgetClass))
#define GDM_IS_HOST_CHOOSER_WIDGET(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), GDM_TYPE_HOST_CHOOSER_WIDGET))
#define GDM_IS_HOST_CHOOSER_WIDGET_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), GDM_TYPE_HOST_CHOOSER_WIDGET))
#define GDM_HOST_CHOOSER_WIDGET_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), GDM_TYPE_HOST_CHOOSER_WIDGET, GdmHostChooserWidgetClass))

typedef struct GdmHostChooserWidgetPrivate GdmHostChooserWidgetPrivate;

typedef struct
{
        GtkVBox                      parent;
        GdmHostChooserWidgetPrivate *priv;
} GdmHostChooserWidget;

typedef struct
{
        GtkVBoxClass   parent_class;

        /* signals */
        void (* hostname_activated)        (GdmHostChooserWidget *widget,
                                            const char           *hostname);
} GdmHostChooserWidgetClass;

GType                  gdm_host_chooser_widget_get_type                       (void);
GtkWidget *            gdm_host_chooser_widget_new                            (void);

void                   gdm_host_chooser_widget_refresh                        (GdmHostChooserWidget *widget);

char *                 gdm_host_chooser_widget_get_current_hostname           (GdmHostChooserWidget *widget);

G_END_DECLS

#endif /* __GDM_HOST_CHOOSER_WIDGET_H */