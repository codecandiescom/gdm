/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 Ray Strode <rstrode@redhat.com>
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
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>

#include "gdm-chooser-widget.h"

#define GDM_CHOOSER_WIDGET_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GDM_TYPE_CHOOSER_WIDGET, GdmChooserWidgetPrivate))

#ifndef GDM_CHOOSER_WIDGET_DEFAULT_ICON_SIZE
#define GDM_CHOOSER_WIDGET_DEFAULT_ICON_SIZE 64
#endif

typedef enum {
        GDM_CHOOSER_WIDGET_STATE_GROWN = 0,
        GDM_CHOOSER_WIDGET_STATE_GROWING,
        GDM_CHOOSER_WIDGET_STATE_SHRINKING,
        GDM_CHOOSER_WIDGET_STATE_SHRUNK,
} GdmChooserWidgetState;

struct GdmChooserWidgetPrivate
{
        GtkWidget                *frame;
        GtkWidget                *frame_alignment;
        GtkWidget                *scrolled_window;

        GtkWidget                *items_view;
        GtkListStore             *list_store;

        GtkTreeModelFilter       *model_filter;
        GtkTreeModelSort         *model_sorter;

        GdkPixbuf                *is_in_use_pixbuf;

        GtkTreeRowReference      *active_row;
        GtkTreeRowReference      *separator_row;
        GtkTreeRowReference      *top_edge_row;    /* Only around for shrink */
        GtkTreeRowReference      *bottom_edge_row; /* animations */

        GtkTreeViewColumn        *is_in_use_column;
        GtkTreeViewColumn        *image_column;

        char                     *inactive_text;
        char                     *active_text;
        char                     *in_use_message;

        gint                     number_of_normal_rows;
        gint                     number_of_separated_rows;
        gint                     number_of_in_use_rows;
        gint                     number_of_rows_with_images;

        guint                    update_idle_id;
        guint                    animation_timeout_id;

        guint32                  should_hide_inactive_items : 1;

        GdmChooserWidgetPosition separator_position;
        GdmChooserWidgetState    state;

};

enum {
        PROP_0,
        PROP_INACTIVE_TEXT,
        PROP_ACTIVE_TEXT
};

enum {
        ACTIVATED = 0,
        DEACTIVATED,
        NUMBER_OF_SIGNALS
};

static guint    signals[NUMBER_OF_SIGNALS];

static void     gdm_chooser_widget_class_init  (GdmChooserWidgetClass *klass);
static void     gdm_chooser_widget_init        (GdmChooserWidget      *chooser_widget);
static void     gdm_chooser_widget_finalize    (GObject                   *object);

G_DEFINE_TYPE (GdmChooserWidget, gdm_chooser_widget, GTK_TYPE_ALIGNMENT)
enum {
        CHOOSER_IMAGE_COLUMN = 0,
        CHOOSER_NAME_COLUMN,
        CHOOSER_COMMENT_COLUMN,
        CHOOSER_ITEM_IS_IN_USE_COLUMN,
        CHOOSER_ITEM_IS_SEPARATED_COLUMN,
        CHOOSER_ITEM_IS_VISIBLE_COLUMN,
        CHOOSER_ID_COLUMN,
        NUMBER_OF_CHOOSER_COLUMNS
};

static gboolean
find_item (GdmChooserWidget *widget,
           const char       *id,
           GtkTreeIter      *iter)
{
        GtkTreeModel *model;
        gboolean      found_item;

        g_assert (GDM_IS_CHOOSER_WIDGET (widget));
        g_assert (id != NULL);

        found_item = FALSE;
        model = GTK_TREE_MODEL (widget->priv->list_store);

        if (!gtk_tree_model_get_iter_first (model, iter)) {
                return FALSE;
        }

        do {
                char *item_id;


                gtk_tree_model_get (model, iter,
                                    CHOOSER_ID_COLUMN, &item_id, -1);

                g_assert (item_id != NULL);

                if (strcmp (id, item_id) == 0) {
                        found_item = TRUE;
                }
                g_free (item_id);

        } while (!found_item && gtk_tree_model_iter_next (model, iter));

        return found_item;
}

static char *
get_active_item_id (GdmChooserWidget *widget,
                    GtkTreeIter      *iter)
{
        char *item_id;
        GtkTreeModel *model;
        GtkTreePath *path;

        g_return_val_if_fail (GDM_IS_CHOOSER_WIDGET (widget), NULL);

        model = GTK_TREE_MODEL (widget->priv->list_store);
        item_id = NULL;

        if (widget->priv->active_row == NULL) {
                return NULL;
        }

        path = gtk_tree_row_reference_get_path (widget->priv->active_row);
        if (gtk_tree_model_get_iter (model, iter, path)) {
                gtk_tree_model_get (model, iter,
                                    CHOOSER_ID_COLUMN, &item_id, -1);
        };
        gtk_tree_path_free (path);

        return item_id;
}

char *
gdm_chooser_widget_get_active_item (GdmChooserWidget *widget)
{
        GtkTreeIter iter;

        return get_active_item_id (widget, &iter);
}

static void
activate_from_item_id (GdmChooserWidget *widget,
                       const char       *item_id)
{
        GtkTreeModel *model;
        GtkTreePath  *path;
        GtkTreeIter   iter;

        model = GTK_TREE_MODEL (widget->priv->list_store);
        path = NULL;

        if (find_item (widget, item_id, &iter)) {
                GtkTreePath  *child_path;

                child_path = gtk_tree_model_get_path (model, &iter);
                path = gtk_tree_model_sort_convert_child_path_to_path (widget->priv->model_sorter, child_path);
                gtk_tree_path_free (child_path);
        }

        if (path == NULL) {
                return;
        }

        gtk_tree_view_scroll_to_cell (GTK_TREE_VIEW (widget->priv->items_view),
                                      path,
                                      NULL,
                                      TRUE,
                                      0.5,
                                      0.0);

        gtk_tree_view_set_cursor (GTK_TREE_VIEW (widget->priv->items_view),
                                  path,
                                  NULL,
                                  FALSE);

        gtk_tree_view_row_activated (GTK_TREE_VIEW (widget->priv->items_view),
                                     path,
                                     NULL);
        gtk_tree_path_free (path);
}

static void
set_frame_text (GdmChooserWidget *widget,
                const char       *text)
{
        GtkWidget *label;

        label = gtk_frame_get_label_widget (GTK_FRAME (widget->priv->frame));

        if (text == NULL && label != NULL) {
                gtk_frame_set_label_widget (GTK_FRAME (widget->priv->frame),
                                            NULL);
                gtk_alignment_set_padding (GTK_ALIGNMENT (widget->priv->frame_alignment),
                                           0, 0, 0, 0);
        } else if (text != NULL && label == NULL) {
                label = gtk_label_new ("");
                gtk_label_set_mnemonic_widget (GTK_LABEL (label),
                                               widget->priv->items_view);
                gtk_widget_show (label);
                gtk_frame_set_label_widget (GTK_FRAME (widget->priv->frame),
                                            label);
                gtk_alignment_set_padding (GTK_ALIGNMENT (widget->priv->frame_alignment),
                                           6, 0, 12, 0);
        }

        if (label != NULL && text != NULL) {
                char *markup;
                markup = g_strdup_printf ("<b>%s</b>", text);
                gtk_label_set_markup_with_mnemonic (GTK_LABEL (label), markup);
                g_free (markup);
        }
}

static void
translate_base_path_to_sorted_path (GdmChooserWidget  *widget,
                                    GtkTreePath      **path)
{
        GtkTreePath *filtered_path;
        GtkTreePath *sorted_path;

        filtered_path =
            gtk_tree_model_filter_convert_child_path_to_path (widget->priv->model_filter, *path);
        sorted_path = gtk_tree_model_sort_convert_child_path_to_path (widget->priv->model_sorter,
                                                                      filtered_path);
        gtk_tree_path_free (filtered_path);

        gtk_tree_path_free (*path);
        *path = sorted_path;
}

static gboolean
shrink_edge_toward_active_row (GdmChooserWidget     *widget,
                               GtkTreeRowReference **edge_row)
{
        GtkTreeModel *model;
        GtkTreePath  *active_path;
        GtkTreePath  *edge_path;
        GtkTreeIter   edge_iter;
        gboolean      edge_is_hidden;
        int           relative_position;

        model = GTK_TREE_MODEL (widget->priv->list_store);

        active_path = gtk_tree_row_reference_get_path (widget->priv->active_row);
        translate_base_path_to_sorted_path (widget, &active_path);

        g_assert (*edge_row != NULL);
        edge_path = gtk_tree_row_reference_get_path (*edge_row);
        g_assert (edge_path != NULL);
        relative_position = gtk_tree_path_compare (edge_path, active_path);
        if (relative_position != 0 &&
            gtk_tree_model_get_iter (GTK_TREE_MODEL (widget->priv->model_sorter),
                                     &edge_iter, edge_path)) {
                GtkTreeIter filtered_iter;
                GtkTreeIter iter;

                if (relative_position < 0) {
                        gtk_tree_path_next (edge_path);
                } else {
                        gtk_tree_path_prev (edge_path);
                }
                gtk_tree_row_reference_free (*edge_row);

                *edge_row = gtk_tree_row_reference_new (GTK_TREE_MODEL (widget->priv->model_sorter),
                                                        edge_path);

                gtk_tree_model_sort_convert_iter_to_child_iter (widget->priv->model_sorter,
                                                                &filtered_iter, &edge_iter);
                gtk_tree_model_filter_convert_iter_to_child_iter (widget->priv->model_filter,
                                                                  &iter, &filtered_iter);
                gtk_list_store_set (GTK_LIST_STORE (widget->priv->list_store),
                                    &iter, CHOOSER_ITEM_IS_VISIBLE_COLUMN, FALSE, -1);

                edge_is_hidden = FALSE;
        } else {
                edge_is_hidden = TRUE;
        }
        gtk_tree_path_free (active_path);
        gtk_tree_path_free (edge_path);

        return edge_is_hidden;
}

static gboolean
iterate_animation (GdmChooserWidget *widget)
{
        gboolean is_done;

        is_done = FALSE;

        if (widget->priv->state == GDM_CHOOSER_WIDGET_STATE_SHRINKING) {
                if (widget->priv->top_edge_row != NULL) {
                        is_done = shrink_edge_toward_active_row (widget,
                                                   &widget->priv->top_edge_row);
                }

                if (widget->priv->bottom_edge_row != NULL) {
                        is_done = is_done &&
                                shrink_edge_toward_active_row (widget,
                                                 &widget->priv->bottom_edge_row);
                }
        } else {
                GtkTreePath *path;
                GtkTreeIter  iter;
                gboolean     is_visible;

                path = gtk_tree_path_new_first ();

                do {
                        if (!gtk_tree_model_get_iter (GTK_TREE_MODEL (widget->priv->list_store),
                                                 &iter, path)) {
                                is_done = TRUE;
                                break;
                        }

                        gtk_tree_model_get (GTK_TREE_MODEL (widget->priv->list_store),
                                                 &iter, CHOOSER_ITEM_IS_VISIBLE_COLUMN,
                                                 &is_visible, -1);

                        if (is_visible) {
                                gtk_tree_path_next (path);
                        } else {
                                gtk_list_store_set (GTK_LIST_STORE (widget->priv->list_store),
                                                    &iter, CHOOSER_ITEM_IS_VISIBLE_COLUMN,
                                                    TRUE, -1);
                        }
                } while (is_visible);

                gtk_tree_path_free (path);
        }

        return is_done != TRUE;
}

static void
stop_animation (GdmChooserWidget *widget)
{
        if (widget->priv->animation_timeout_id == 0) {
                return;
        }

        gtk_tree_row_reference_free (widget->priv->top_edge_row);
        widget->priv->top_edge_row = NULL;

        gtk_tree_row_reference_free (widget->priv->bottom_edge_row);
        widget->priv->bottom_edge_row = NULL;

        widget->priv->animation_timeout_id = 0;
        gtk_widget_set_sensitive (GTK_WIDGET (widget), TRUE);
}

static void
start_animation (GdmChooserWidget *widget)
{
       GtkTreePath *edge_path;
       int          number_of_visible_rows;
       int          number_of_rows;

       if (widget->priv->animation_timeout_id != 0) {
                g_source_remove (widget->priv->animation_timeout_id);
       }

       number_of_visible_rows = gtk_tree_model_iter_n_children (GTK_TREE_MODEL (widget->priv->model_sorter), NULL);
       number_of_rows = gtk_tree_model_iter_n_children (GTK_TREE_MODEL (widget->priv->list_store), NULL);

       if (widget->priv->state == GDM_CHOOSER_WIDGET_STATE_SHRINKING) {
               if (number_of_visible_rows <= 1) {
                       widget->priv->state = GDM_CHOOSER_WIDGET_STATE_SHRUNK;
                       return;
               }

               edge_path = gtk_tree_path_new_first ();
               widget->priv->top_edge_row = gtk_tree_row_reference_new (GTK_TREE_MODEL (widget->priv->model_sorter),
                                                                        edge_path);
               gtk_tree_path_free (edge_path);

               edge_path = gtk_tree_path_new_from_indices (number_of_visible_rows - 1, -1);

               widget->priv->bottom_edge_row = gtk_tree_row_reference_new (GTK_TREE_MODEL (widget->priv->model_sorter),
                                                                           edge_path);
               gtk_tree_path_free (edge_path);

               g_assert (widget->priv->top_edge_row != NULL && widget->priv->bottom_edge_row != NULL);
       }

       if (widget->priv->state == GDM_CHOOSER_WIDGET_STATE_GROWING) {
               if (number_of_visible_rows >= number_of_rows) {
                       widget->priv->state = GDM_CHOOSER_WIDGET_STATE_GROWN;
                       return;
               }
       }

       gtk_widget_set_sensitive (GTK_WIDGET (widget), FALSE);

       /* FIXME: The 4 here is abitrary.  We should really keep track of the time we start and
        * hide enough rows to catch up to where we should be each time through
        */
       widget->priv->animation_timeout_id =
               g_timeout_add_full (G_PRIORITY_DEFAULT_IDLE,
                                   1000 / (4 * number_of_rows),
                                   (GSourceFunc) iterate_animation,
                                   widget, (GDestroyNotify) stop_animation);
}

static void
gdm_chooser_widget_grow (GdmChooserWidget *widget)
{
        gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (widget->priv->scrolled_window),
                                             GTK_SHADOW_ETCHED_IN);
        gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (widget->priv->scrolled_window),
                                        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_alignment_set (GTK_ALIGNMENT (widget->priv->frame_alignment),
                           0.0, 0.0, 1.0, 1.0);

        set_frame_text (widget, widget->priv->inactive_text);

        widget->priv->state = GDM_CHOOSER_WIDGET_STATE_GROWING;
        start_animation (widget);
}

static void
move_cursor_to_top (GdmChooserWidget *widget)
{
        GtkTreeModel *model;
        GtkTreePath  *path;
        GtkTreeIter   iter;

        model = gtk_tree_view_get_model (GTK_TREE_VIEW (widget->priv->items_view));
        path = gtk_tree_path_new_first ();
        if (gtk_tree_model_get_iter (model, &iter, path)) {
                gtk_tree_view_set_cursor (GTK_TREE_VIEW (widget->priv->items_view),
                                          path,
                                          NULL,
                                          FALSE);
        }
        gtk_tree_path_free (path);
}

static gboolean
clear_selection (GdmChooserWidget *widget)
{
        GtkTreeSelection *selection;

        selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (widget->priv->items_view));
        gtk_tree_selection_unselect_all (selection);

        return FALSE;
}

static void
gdm_chooser_widget_shrink (GdmChooserWidget *widget)
{
        g_assert (widget->priv->should_hide_inactive_items == TRUE);

        set_frame_text (widget, widget->priv->active_text);

        gtk_alignment_set (GTK_ALIGNMENT (widget->priv->frame_alignment),
                           0.0, 0.0, 1.0, 0.0);

        gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (widget->priv->scrolled_window),
                                        GTK_POLICY_NEVER, GTK_POLICY_NEVER);

        gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (widget->priv->scrolled_window),
                                             GTK_SHADOW_ETCHED_OUT);

        clear_selection (widget);
        widget->priv->state = GDM_CHOOSER_WIDGET_STATE_SHRINKING;
        start_animation (widget);
}

static void
activate_from_row (GdmChooserWidget    *widget,
                   GtkTreeRowReference *row)
{
        g_assert (row != NULL);
        g_assert (gtk_tree_row_reference_valid (row));

        if (widget->priv->active_row != NULL) {
                gtk_tree_row_reference_free (widget->priv->active_row);
                widget->priv->active_row = NULL;
        }

        widget->priv->active_row = gtk_tree_row_reference_copy (row);
        g_signal_emit (widget, signals[ACTIVATED], 0);

        if (widget->priv->should_hide_inactive_items) {
                gdm_chooser_widget_shrink (widget);
        }
}

static void
deactivate (GdmChooserWidget *widget)
{
        if (widget->priv->active_row == NULL) {
                return;
        }

        gtk_tree_row_reference_free (widget->priv->active_row);
        widget->priv->active_row = NULL;

        g_signal_emit (widget, signals[DEACTIVATED], 0, NULL, NULL);

        if (widget->priv->state != GDM_CHOOSER_WIDGET_STATE_GROWN) {
                gdm_chooser_widget_grow (widget);
        }
}

static void
activate_selected_item (GdmChooserWidget *widget)
{
        GtkTreeRowReference *row;
        GtkTreeSelection    *selection;
        GtkTreeModel        *model;
        GtkTreeModel        *sort_model;
        GtkTreeIter          sorted_iter;
        gboolean             is_already_active;

        row = NULL;
        model = GTK_TREE_MODEL (widget->priv->list_store);
        is_already_active = FALSE;

        selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (widget->priv->items_view));
        if (gtk_tree_selection_get_selected (selection, &sort_model, &sorted_iter)) {
                GtkTreePath *sorted_path;
                GtkTreePath *filtered_path;
                GtkTreePath *base_path;

                g_assert (sort_model == GTK_TREE_MODEL (widget->priv->model_sorter));

                sorted_path = gtk_tree_model_get_path (sort_model, &sorted_iter);
                filtered_path =
                    gtk_tree_model_sort_convert_path_to_child_path (widget->priv->model_sorter,
                                                                    sorted_path);
                gtk_tree_path_free (sorted_path);
                base_path =
                    gtk_tree_model_filter_convert_path_to_child_path (widget->priv->model_filter,
                                                                      filtered_path);

                if (widget->priv->active_row != NULL) {
                        GtkTreePath *active_path;

                        active_path = gtk_tree_row_reference_get_path (widget->priv->active_row);

                        if (gtk_tree_path_compare  (base_path, active_path) == 0) {
                                is_already_active = TRUE;
                        }
                        gtk_tree_path_free (active_path);
                }
                g_assert (base_path != NULL);
                row = gtk_tree_row_reference_new (model, base_path);
                gtk_tree_path_free (base_path);
        }

        if (!is_already_active) {
                activate_from_row (widget, row);
        } else {
                deactivate (widget);
        }
        gtk_tree_row_reference_free (row);
}

void
gdm_chooser_widget_set_active_item (GdmChooserWidget *widget,
                                    const char       *id)
{
        g_return_if_fail (GDM_IS_CHOOSER_WIDGET (widget));

        if (id != NULL) {
                activate_from_item_id (widget, id);
        } else {
                deactivate (widget);
        }
}

static void
gdm_chooser_widget_set_property (GObject        *object,
                                 guint           prop_id,
                                 const GValue   *value,
                                 GParamSpec     *pspec)
{
        GdmChooserWidget *self;

        self = GDM_CHOOSER_WIDGET (object);

        switch (prop_id) {

        case PROP_INACTIVE_TEXT:
                g_free (self->priv->inactive_text);
                self->priv->inactive_text = g_value_dup_string (value);

                if (self->priv->active_row == NULL) {
                        set_frame_text (self, self->priv->inactive_text);
                }
                break;

        case PROP_ACTIVE_TEXT:
                g_free (self->priv->active_text);
                self->priv->active_text = g_value_dup_string (value);

                if (self->priv->active_row != NULL) {
                        set_frame_text (self, self->priv->active_text);
                }
                break;

        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
gdm_chooser_widget_get_property (GObject        *object,
                                 guint           prop_id,
                                 GValue         *value,
                                 GParamSpec     *pspec)
{
        GdmChooserWidget *self;

        self = GDM_CHOOSER_WIDGET (object);

        switch (prop_id) {
        case PROP_INACTIVE_TEXT:
                g_value_set_string (value, self->priv->inactive_text);
                break;

        case PROP_ACTIVE_TEXT:
                g_value_set_string (value, self->priv->active_text);
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static GObject *
gdm_chooser_widget_constructor (GType                  type,
                                guint                  n_construct_properties,
                                GObjectConstructParam *construct_properties)
{
        GdmChooserWidget      *chooser_widget;
        GdmChooserWidgetClass *klass;

        klass = GDM_CHOOSER_WIDGET_CLASS (g_type_class_peek (GDM_TYPE_CHOOSER_WIDGET));

        chooser_widget = GDM_CHOOSER_WIDGET (G_OBJECT_CLASS (gdm_chooser_widget_parent_class)->constructor (type,
                                                                                                            n_construct_properties,
                                                                                                            construct_properties));

        return G_OBJECT (chooser_widget);
}

static void
gdm_chooser_widget_dispose (GObject *object)
{
        GdmChooserWidget *widget;

        widget = GDM_CHOOSER_WIDGET (object);

        if (widget->priv->separator_row != NULL) {
                gtk_tree_row_reference_free (widget->priv->separator_row);
                widget->priv->separator_row = NULL;
        }

        if (widget->priv->active_row != NULL) {
                gtk_tree_row_reference_free (widget->priv->active_row);
                widget->priv->active_row = NULL;
        }

        if (widget->priv->inactive_text != NULL) {
                g_free (widget->priv->inactive_text);
                widget->priv->inactive_text = NULL;
        }

        if (widget->priv->active_text != NULL) {
                g_free (widget->priv->active_text);
                widget->priv->active_text = NULL;
        }

        if (widget->priv->in_use_message != NULL) {
                g_free (widget->priv->in_use_message);
                widget->priv->in_use_message = NULL;
        }

        G_OBJECT_CLASS (gdm_chooser_widget_parent_class)->dispose (object);
}

static gboolean
gdm_chooser_widget_focus_in (GtkWidget     *widget,
                             GdkEventFocus *event)
{
        GdmChooserWidget *chooser;
        chooser = GDM_CHOOSER_WIDGET (widget);

        gtk_widget_grab_focus (chooser->priv->items_view);

        return FALSE;
}

static void
gdm_chooser_widget_size_request (GtkWidget      *widget,
                                 GtkRequisition *requisition)
{
        GdmChooserWidget *chooser;

        chooser = GDM_CHOOSER_WIDGET (widget);

        GTK_WIDGET_CLASS (gdm_chooser_widget_parent_class)->size_request (widget, requisition);

        /* XXX: this hack makes the scrolled window behave the way we want in
         * the login window.  The login window is special because it always
         * tries to hug the widgets as tightly as possible.  Normally, this
         * "tight hug" makes the scrolled_window get squeezed into nothing (if
         * POLICY_AUTOMATIC) If we use POLICY_NEVER then the scrolled window
         * gets the right size but can't have a scrollbar (which sort of
         * defeats the point I guess)
         */
        requisition->height -= chooser->priv->scrolled_window->requisition.height;
        chooser->priv->scrolled_window->requisition.height = chooser->priv->items_view->requisition.height;
        chooser->priv->scrolled_window->requisition.height += chooser->priv->scrolled_window->style->ythickness * 2;
        chooser->priv->scrolled_window->requisition.height += GTK_CONTAINER (chooser->priv->scrolled_window)->border_width * 2;
        requisition->height += chooser->priv->scrolled_window->requisition.height;
}

#ifdef BUILD_ALLOCATION_HACK
static gint
compare_allocation_height (GdmChooserWidget *widget_a,
                           GdmChooserWidget *widget_b)
{
        return GTK_WIDGET (widget_a)->allocation.height - GTK_WIDGET (widget_b)->allocation.height;
}

static void
renegotiate_allocation (GtkContainer          *container,
                        GdmChooserWidgetClass *klass)
{
        GList *children;
        GList *choosers;
        GList *tmp;
        int    total_allocation;
        int    number_of_choosers;

        if (klass->size_negotiation_handler == 0) {
                return;
        }
        klass->size_negotiation_handler = 0;
        g_signal_handlers_disconnect_by_func (container, renegotiate_allocation, klass);

        children = gtk_container_get_children (container);

        total_allocation = 0;
        number_of_choosers = 0;
        choosers = NULL;
        for (tmp = children; tmp != NULL; tmp = tmp->next) {
                GdmChooserWidget *widget;

                if (!GDM_IS_CHOOSER_WIDGET (tmp->data)) {
                        continue;
                }

                widget = GDM_CHOOSER_WIDGET (tmp->data);

                total_allocation += GTK_WIDGET (widget)->allocation.height;
                choosers = g_list_insert_sorted (choosers, widget, (GCompareFunc) compare_allocation_height);
                number_of_choosers++;
        }
        total_allocation = MIN (total_allocation, GTK_WIDGET (container)->allocation.height);

        for (tmp = choosers; tmp != NULL; tmp = tmp->next) {
                GdmChooserWidget *widget;
                GtkAllocation  allocation;

                g_assert (GDM_IS_CHOOSER_WIDGET (tmp->data));

                widget = GDM_CHOOSER_WIDGET (tmp->data);

                allocation = GTK_WIDGET (widget)->allocation;

                GTK_WIDGET (widget)->allocation.height = MIN (GTK_WIDGET (widget)->requisition.height,
                                                              total_allocation / number_of_choosers);

                total_allocation -= allocation.height;

                number_of_choosers--;
        }
        g_list_free (children);
}

static void
gdm_chooser_widget_size_allocate (GtkWidget        *widget,
                                  GtkAllocation    *allocation)
{
        GdmChooserWidgetClass *klass;

        klass = GDM_CHOOSER_WIDGET_GET_CLASS (widget);

        GTK_WIDGET_CLASS (gdm_chooser_widget_parent_class)->size_allocate (widget, allocation);

        /* XXX: Vbox isn't too smart about divving up allocations when there isn't enough room to go around.
         * Since we may have more than one chooser widget in a vbox, we redistribute space between the choosers
         * (if one chooser gets lots of space and another gets no space, give some up)
         */
        if (allocation->height == 1 && klass->size_negotiation_handler == 0) {
                GtkWidget *parent;

                parent = gtk_widget_get_parent (widget);
                klass->size_negotiation_handler = g_signal_connect (parent, "size-allocate",
                                                                    G_CALLBACK (renegotiate_allocation),
                                                                    klass);
        }
}
#endif

static void
gdm_chooser_widget_class_init (GdmChooserWidgetClass *klass)
{
        GObjectClass   *object_class = G_OBJECT_CLASS (klass);
        GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

        object_class->get_property = gdm_chooser_widget_get_property;
        object_class->set_property = gdm_chooser_widget_set_property;
        object_class->constructor = gdm_chooser_widget_constructor;
        object_class->dispose = gdm_chooser_widget_dispose;
        object_class->finalize = gdm_chooser_widget_finalize;
        widget_class->focus_in_event = gdm_chooser_widget_focus_in;
        widget_class->size_request = gdm_chooser_widget_size_request;
#ifdef BUILD_ALLOCATION_HACK
        widget_class->size_allocate = gdm_chooser_widget_size_allocate;
#endif

        signals [ACTIVATED] = g_signal_new ("activated",
                                            G_TYPE_FROM_CLASS (object_class),
                                            G_SIGNAL_RUN_LAST,
                                            G_STRUCT_OFFSET (GdmChooserWidgetClass, activated),
                                            NULL,
                                            NULL,
                                            g_cclosure_marshal_VOID__VOID,
                                            G_TYPE_NONE,
                                            0);

        signals [DEACTIVATED] = g_signal_new ("deactivated",
                                              G_TYPE_FROM_CLASS (object_class),
                                              G_SIGNAL_RUN_LAST,
                                              G_STRUCT_OFFSET (GdmChooserWidgetClass, deactivated),
                                              NULL,
                                              NULL,
                                              g_cclosure_marshal_VOID__VOID,
                                              G_TYPE_NONE,
                                              0);

        g_object_class_install_property (object_class,
                                         PROP_INACTIVE_TEXT,
                                         g_param_spec_string ("inactive-text",
                                                              _("Inactive Text"),
                                                              _("The text to use in the label if the "
                                                                "user hasn't picked an item yet"),
                                                              NULL,
                                                              (G_PARAM_READWRITE |
                                                               G_PARAM_CONSTRUCT)));
        g_object_class_install_property (object_class,
                                         PROP_ACTIVE_TEXT,
                                         g_param_spec_string ("active-text",
                                                              _("Active Text"),
                                                              _("The text to use in the label if the "
                                                                "user has picked an item"),
                                                              NULL,
                                                              (G_PARAM_READWRITE |
                                                               G_PARAM_CONSTRUCT)));

        g_type_class_add_private (klass, sizeof (GdmChooserWidgetPrivate));
}

static void
on_row_activated (GtkTreeView          *tree_view,
                  GtkTreePath          *tree_path,
                  GtkTreeViewColumn    *tree_column,
                  GdmChooserWidget     *widget)
{
        activate_selected_item (widget);
}

static gboolean
path_is_separator (GdmChooserWidget *widget,
                   GtkTreeModel     *model,
                   GtkTreePath      *path)
{
        GtkTreePath      *base_path;
        GtkTreePath      *filtered_path;
        GtkTreePath      *sorted_path;
        GtkTreePath      *separator_path;
        gboolean          is_separator;

        if (widget->priv->separator_row == NULL) {
                return FALSE;
        }

        base_path = gtk_tree_row_reference_get_path (widget->priv->separator_row);
        separator_path = base_path;
        filtered_path = NULL;
        sorted_path = NULL;

        if (model != GTK_TREE_MODEL (widget->priv->list_store)) {
                filtered_path = gtk_tree_model_filter_convert_child_path_to_path (widget->priv->model_filter, base_path);
                separator_path = filtered_path;

                gtk_tree_path_free (base_path);
                base_path = NULL;
        }

        if (filtered_path != NULL && model != GTK_TREE_MODEL (widget->priv->model_filter)) {
                sorted_path = gtk_tree_model_sort_convert_child_path_to_path (widget->priv->model_sorter, filtered_path);
                separator_path = sorted_path;

                gtk_tree_path_free (filtered_path);
                filtered_path = NULL;
        }

        if ((separator_path != NULL) &&
            gtk_tree_path_compare (path, separator_path) == 0) {
                is_separator = TRUE;
        } else {
                is_separator = FALSE;
        }
        gtk_tree_path_free (separator_path);

        return is_separator;
}

static int
compare_item  (GtkTreeModel *model,
               GtkTreeIter  *a,
               GtkTreeIter  *b,
               gpointer      data)
{
        GdmChooserWidget *widget;
        char             *name_a;
        char             *name_b;
        gboolean          is_separate_a;
        gboolean          is_separate_b;
        int               result;
        int               direction;
        GtkTreeIter      *separator_iter;

        g_assert (GDM_IS_CHOOSER_WIDGET (data));

        widget = GDM_CHOOSER_WIDGET (data);

        separator_iter = NULL;
        if (widget->priv->separator_row != NULL) {

                GtkTreePath      *path_a;
                GtkTreePath      *path_b;

                path_a = gtk_tree_model_get_path (model, a);
                path_b = gtk_tree_model_get_path (model, b);

                if (path_is_separator (widget, model, path_a)) {
                        separator_iter = a;
                } else if (path_is_separator (widget, model, path_b)) {
                        separator_iter = b;
                }

                gtk_tree_path_free (path_a);
                gtk_tree_path_free (path_b);
        }

        name_a = NULL;
        is_separate_a = FALSE;
        if (separator_iter != a) {
                gtk_tree_model_get (model, a,
                                    CHOOSER_NAME_COLUMN, &name_a,
                                    CHOOSER_ITEM_IS_SEPARATED_COLUMN, &is_separate_a,
                                    -1);
        }

        char *id;
        name_b = NULL;
        is_separate_b = FALSE;
        if (separator_iter != b) {
                gtk_tree_model_get (model, b,
                                    CHOOSER_NAME_COLUMN, &name_b,
                                    CHOOSER_ID_COLUMN, &id,
                                    CHOOSER_ITEM_IS_SEPARATED_COLUMN, &is_separate_b,
                                    -1);
        }

        if (widget->priv->separator_position == GDM_CHOOSER_WIDGET_POSITION_TOP) {
                direction = -1;
        } else {
                direction = 1;
        }

        if (separator_iter == b) {
                result = is_separate_a? 1 : -1;
                result *= direction;
        } else if (separator_iter == a) {
                result = is_separate_b? -1 : 1;
                result *= direction;
        } else if (is_separate_b == is_separate_a) {
                result = g_utf8_collate (name_a, name_b);
        } else {
                result = is_separate_a - is_separate_b;
                result *= direction;
        }

        g_free (name_a);
        g_free (name_b);

        return result;
}

static void
name_cell_data_func (GtkTreeViewColumn  *tree_column,
                     GtkCellRenderer    *cell,
                     GtkTreeModel       *model,
                     GtkTreeIter        *iter,
                     GdmChooserWidget   *widget)
{
        gboolean is_in_use;
        char    *name;
        char    *markup;

        name = NULL;
        gtk_tree_model_get (model,
                            iter,
                            CHOOSER_ITEM_IS_IN_USE_COLUMN, &is_in_use,
                            CHOOSER_NAME_COLUMN, &name,
                            -1);

        if (is_in_use) {
                markup = g_strdup_printf ("<b>%s</b>\n"
                                          "<i><span size=\"x-small\">%s</span></i>",
                                          name, widget->priv->in_use_message);
        } else {
                markup = g_strdup_printf ("<b>%s</b>", name);
        }
        g_free (name);

        g_object_set (cell, "markup", markup, NULL);
        g_free (markup);
}

static void
check_cell_data_func (GtkTreeViewColumn    *tree_column,
                      GtkCellRenderer      *cell,
                      GtkTreeModel         *model,
                      GtkTreeIter          *iter,
                      GdmChooserWidget     *widget)
{
        gboolean   is_in_use;
        GdkPixbuf *pixbuf;

        gtk_tree_model_get (model,
                            iter,
                            CHOOSER_ITEM_IS_IN_USE_COLUMN, &is_in_use,
                            -1);

        if (is_in_use) {
                pixbuf = widget->priv->is_in_use_pixbuf;
        } else {
                pixbuf = NULL;
        }

        g_object_set (cell, "pixbuf", pixbuf, NULL);
}

static GdkPixbuf *
get_is_in_use_pixbuf (GdmChooserWidget *widget)
{
        GtkIconTheme *theme;
        GdkPixbuf    *pixbuf;

        theme = gtk_icon_theme_get_default ();
        pixbuf = gtk_icon_theme_load_icon (theme,
                                           "emblem-default",
                                           GDM_CHOOSER_WIDGET_DEFAULT_ICON_SIZE / 3,
                                           0,
                                           NULL);

        return pixbuf;
}

static gboolean
separator_func (GtkTreeModel *model,
                GtkTreeIter  *iter,
                gpointer      data)
{
        GdmChooserWidget *widget;
        GtkTreePath      *path;
        gboolean          is_separator;

        g_assert (GDM_IS_CHOOSER_WIDGET (data));

        widget = GDM_CHOOSER_WIDGET (data);

        g_assert (widget->priv->separator_row != NULL);

        path = gtk_tree_model_get_path (model, iter);

        is_separator = path_is_separator (widget, model, path);

        gtk_tree_path_free (path);

        return is_separator;
}

static void
add_separator (GdmChooserWidget *widget)
{
        GtkTreeIter   iter;
        GtkTreeModel *model;
        GtkTreePath  *path;

        g_assert (widget->priv->separator_row == NULL);

        model = GTK_TREE_MODEL (widget->priv->list_store);

        gtk_list_store_insert_with_values (widget->priv->list_store,
                                           &iter, 0,
                                           CHOOSER_ID_COLUMN, "-", -1);
        path = gtk_tree_model_get_path (model, &iter);
        widget->priv->separator_row =
            gtk_tree_row_reference_new (model, path);
        gtk_tree_path_free (path);
}

static gboolean
update_column_visibility (GdmChooserWidget *widget)
{
        if (widget->priv->number_of_rows_with_images > 0) {
                gtk_tree_view_column_set_visible (widget->priv->image_column,
                                                  TRUE);
        } else {
                gtk_tree_view_column_set_visible (widget->priv->image_column,
                                                  FALSE);
        }
        if (widget->priv->number_of_in_use_rows > 0) {
                gtk_tree_view_column_set_visible (widget->priv->is_in_use_column,
                                                  TRUE);
        } else {
                gtk_tree_view_column_set_visible (widget->priv->is_in_use_column,
                                                  FALSE);
        }

        return FALSE;
}

static void
clear_canceled_visibility_update (GdmChooserWidget *widget)
{
        widget->priv->update_idle_id = 0;
}

static void
queue_column_visibility_update (GdmChooserWidget *widget)
{
        if (widget->priv->update_idle_id == 0) {
                widget->priv->update_idle_id =
                        g_idle_add_full (G_PRIORITY_DEFAULT_IDLE,
                                         (GSourceFunc)
                                         update_column_visibility, widget,
                                         (GDestroyNotify)
                                         clear_canceled_visibility_update);
        }
}

static void
on_row_changed (GtkTreeModel     *model,
                GtkTreePath      *path,
                GtkTreeIter      *iter,
                GdmChooserWidget *widget)
{
        queue_column_visibility_update (widget);
}

static void
add_frame (GdmChooserWidget *widget)
{
        widget->priv->frame = gtk_frame_new (NULL);
        gtk_frame_set_shadow_type (GTK_FRAME (widget->priv->frame),
                                   GTK_SHADOW_NONE);
        gtk_widget_show (widget->priv->frame);
        gtk_container_add (GTK_CONTAINER (widget), widget->priv->frame);

        widget->priv->frame_alignment = gtk_alignment_new (0.5, 0.5, 1.0, 1.0);
        gtk_widget_show (widget->priv->frame_alignment);
        gtk_container_add (GTK_CONTAINER (widget->priv->frame),
                           widget->priv->frame_alignment);
}

static gboolean
on_button_release (GtkTreeView      *items_view,
                   GdkEventButton   *event,
                   GdmChooserWidget *widget)
{
        GtkTreeModel     *model;
        GtkTreeIter       iter;
        GtkTreeSelection *selection;

        selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (widget->priv->items_view));
        if (gtk_tree_selection_get_selected (selection, &model, &iter)) {
                GtkTreePath *path;

                path = gtk_tree_model_get_path (model, &iter);
                gtk_tree_view_row_activated (GTK_TREE_VIEW (items_view),
                                             path, NULL);
                gtk_tree_path_free (path);
        }

        return FALSE;
}

static void
gdm_chooser_widget_init (GdmChooserWidget *widget)
{
        GtkTreeViewColumn *column;
        GtkTreeSelection  *selection;
        GtkCellRenderer   *renderer;

        widget->priv = GDM_CHOOSER_WIDGET_GET_PRIVATE (widget);

        gtk_alignment_set_padding (GTK_ALIGNMENT (widget), 6, 0, 12, 0);

        add_frame (widget);

        widget->priv->scrolled_window = gtk_scrolled_window_new (NULL, NULL);

        gtk_scrolled_window_set_vadjustment (GTK_SCROLLED_WINDOW (widget->priv->scrolled_window),
                                             NULL);
        gtk_scrolled_window_set_hadjustment (GTK_SCROLLED_WINDOW (widget->priv->scrolled_window),
                                             NULL);
        gtk_widget_show (widget->priv->scrolled_window);
        gtk_container_add (GTK_CONTAINER (widget->priv->frame_alignment),
                           widget->priv->scrolled_window);

        widget->priv->items_view = gtk_tree_view_new ();
        gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (widget->priv->items_view),
                                           FALSE);
        g_signal_connect (widget->priv->items_view,
                          "row-activated",
                          G_CALLBACK (on_row_activated),
                          widget);

        /* hack to make single-click activate work
         */
        g_signal_connect_after (widget->priv->items_view,
                               "button-release-event",
                               G_CALLBACK (on_button_release),
                               widget);

        gtk_widget_show (widget->priv->items_view);
        gtk_container_add (GTK_CONTAINER (widget->priv->scrolled_window),
                           widget->priv->items_view);

        selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (widget->priv->items_view));
        gtk_tree_selection_set_mode (selection, GTK_SELECTION_BROWSE);

        g_assert (NUMBER_OF_CHOOSER_COLUMNS == 7);
        widget->priv->list_store = gtk_list_store_new (NUMBER_OF_CHOOSER_COLUMNS,
                                                       GDK_TYPE_PIXBUF,
                                                       G_TYPE_STRING,
                                                       G_TYPE_STRING,
                                                       G_TYPE_BOOLEAN,
                                                       G_TYPE_BOOLEAN,
                                                       G_TYPE_BOOLEAN,
                                                       G_TYPE_STRING);

        widget->priv->model_filter = GTK_TREE_MODEL_FILTER (gtk_tree_model_filter_new (GTK_TREE_MODEL (widget->priv->list_store), NULL));

        gtk_tree_model_filter_set_visible_column (widget->priv->model_filter,
                                                  CHOOSER_ITEM_IS_VISIBLE_COLUMN);
        g_signal_connect (G_OBJECT (widget->priv->model_filter), "row-changed",
                          G_CALLBACK (on_row_changed), widget);

        widget->priv->model_sorter = GTK_TREE_MODEL_SORT (gtk_tree_model_sort_new_with_model (GTK_TREE_MODEL (widget->priv->model_filter)));

        gtk_tree_sortable_set_sort_func (GTK_TREE_SORTABLE (widget->priv->model_sorter),
                                         CHOOSER_ID_COLUMN,
                                         compare_item,
                                         widget, NULL);

        gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (widget->priv->model_sorter),
                                              CHOOSER_ID_COLUMN,
                                              GTK_SORT_ASCENDING);
        gtk_tree_view_set_model (GTK_TREE_VIEW (widget->priv->items_view),
                                 GTK_TREE_MODEL (widget->priv->model_sorter));
        gtk_tree_view_set_row_separator_func (GTK_TREE_VIEW (widget->priv->items_view),
                                              separator_func,
                                              widget, NULL);

        /* IMAGE COLUMN */
        renderer = gtk_cell_renderer_pixbuf_new ();
        column = gtk_tree_view_column_new ();
        gtk_tree_view_column_pack_start (column, renderer, FALSE);
        gtk_tree_view_append_column (GTK_TREE_VIEW (widget->priv->items_view), column);
        widget->priv->image_column = column;

        gtk_tree_view_column_set_attributes (column,
                                             renderer,
                                             "pixbuf", CHOOSER_IMAGE_COLUMN,
                                             NULL);

        g_object_set (renderer,
                      "width", GDM_CHOOSER_WIDGET_DEFAULT_ICON_SIZE,
                      "xalign", 1.0,
                      NULL);

        /* NAME COLUMN */
        renderer = gtk_cell_renderer_text_new ();
        column = gtk_tree_view_column_new ();
        gtk_tree_view_column_pack_start (column, renderer, FALSE);
        gtk_tree_view_append_column (GTK_TREE_VIEW (widget->priv->items_view), column);
        gtk_tree_view_column_set_cell_data_func (column,
                                                 renderer,
                                                 (GtkTreeCellDataFunc) name_cell_data_func,
                                                 widget,
                                                 NULL);

        gtk_tree_view_set_tooltip_column (GTK_TREE_VIEW (widget->priv->items_view),
                                          CHOOSER_COMMENT_COLUMN);

        /* IN USE COLUMN */
        renderer = gtk_cell_renderer_pixbuf_new ();
        column = gtk_tree_view_column_new ();
        gtk_tree_view_column_pack_start (column, renderer, FALSE);
        gtk_tree_view_append_column (GTK_TREE_VIEW (widget->priv->items_view), column);
        widget->priv->is_in_use_column = column;

        gtk_tree_view_column_set_cell_data_func (column,
                                                 renderer,
                                                 (GtkTreeCellDataFunc) check_cell_data_func,
                                                 widget,
                                                 NULL);
        widget->priv->is_in_use_pixbuf = get_is_in_use_pixbuf (widget);

        g_object_set (renderer,
                      "width", GDM_CHOOSER_WIDGET_DEFAULT_ICON_SIZE,
                      NULL);

        add_separator (widget);

        queue_column_visibility_update (widget);
        gdm_chooser_widget_grow (widget);
}

static void
gdm_chooser_widget_finalize (GObject *object)
{
        GdmChooserWidget *widget;

        g_return_if_fail (object != NULL);
        g_return_if_fail (GDM_IS_CHOOSER_WIDGET (object));

        widget = GDM_CHOOSER_WIDGET (object);

        g_return_if_fail (widget->priv != NULL);

        G_OBJECT_CLASS (gdm_chooser_widget_parent_class)->finalize (object);
}

GtkWidget *
gdm_chooser_widget_new (const char *inactive_text,
                        const char *active_text)
{
        GObject *object;

        object = g_object_new (GDM_TYPE_CHOOSER_WIDGET,
                               "inactive-text", inactive_text,
                               "active-text", active_text, NULL);

        return GTK_WIDGET (object);
}

void
gdm_chooser_widget_add_item (GdmChooserWidget *widget,
                             const char       *id,
                             GdkPixbuf        *image,
                             const char       *name,
                             const char       *comment,
                             gboolean          in_use,
                             gboolean          keep_separate)
{
        gboolean is_visible;

        g_return_if_fail (GDM_IS_CHOOSER_WIDGET (widget));

        if (keep_separate) {
                widget->priv->number_of_separated_rows++;
        } else {
                widget->priv->number_of_normal_rows++;
        }

        if (in_use) {
                widget->priv->number_of_in_use_rows++;
        }

        if (image != NULL) {
                widget->priv->number_of_rows_with_images++;
        }

        is_visible = widget->priv->active_row == NULL;

        gtk_list_store_insert_with_values (widget->priv->list_store,
                                           NULL, 0,
                                           CHOOSER_IMAGE_COLUMN, image,
                                           CHOOSER_NAME_COLUMN, name,
                                           CHOOSER_COMMENT_COLUMN, comment,
                                           CHOOSER_ITEM_IS_IN_USE_COLUMN, in_use,
                                           CHOOSER_ITEM_IS_SEPARATED_COLUMN, keep_separate,
                                           CHOOSER_ITEM_IS_VISIBLE_COLUMN, is_visible,
                                           CHOOSER_ID_COLUMN, id,
                                           -1);

        move_cursor_to_top (widget);
}

void
gdm_chooser_widget_remove_item (GdmChooserWidget *widget,
                                const char       *id)
{
        GtkTreeModel *model;
        GtkTreeIter   iter;
        GdkPixbuf    *image;
        gboolean      is_separate;
        gboolean      is_in_use;

        g_return_if_fail (GDM_IS_CHOOSER_WIDGET (widget));

        model = GTK_TREE_MODEL (widget->priv->list_store);

        if (!find_item (widget, id, &iter)) {
                g_critical ("Tried to remove non-existing item from chooser");
                return;
        }

        is_separate = FALSE;
        gtk_tree_model_get (model, &iter,
                            CHOOSER_IMAGE_COLUMN, &image,
                            CHOOSER_ITEM_IS_IN_USE_COLUMN, &is_in_use,
                            CHOOSER_ITEM_IS_SEPARATED_COLUMN, &is_separate,
                            -1);

        if (image != NULL) {
                widget->priv->number_of_rows_with_images--;
                g_object_unref (image);
        }

        if (is_in_use) {
                widget->priv->number_of_in_use_rows--;
        }

        if (is_separate) {
                widget->priv->number_of_separated_rows--;
        } else {
                widget->priv->number_of_normal_rows--;
        }

        gtk_list_store_remove (widget->priv->list_store, &iter);

        move_cursor_to_top (widget);
}

gboolean
gdm_chooser_widget_lookup_item (GdmChooserWidget *widget,
                                const char       *id,
                                GdkPixbuf       **image,
                                char            **name,
                                char            **comment,
                                gboolean         *is_in_use,
                                gboolean         *is_separate)
{
        GtkTreeIter   iter;
        char         *active_item_id;

        g_return_val_if_fail (GDM_IS_CHOOSER_WIDGET (widget), FALSE);
        g_return_val_if_fail (id != NULL, FALSE);

        active_item_id = get_active_item_id (widget, &iter);

        if (active_item_id == NULL || strcmp (active_item_id, id) != 0) {
                g_free (active_item_id);

                if (!find_item (widget, id, &iter)) {
                        return FALSE;
                }
        }
        g_free (active_item_id);

        gtk_tree_model_get (GTK_TREE_MODEL (widget->priv->list_store), &iter,
                            CHOOSER_IMAGE_COLUMN, image,
                            CHOOSER_NAME_COLUMN, name,
                            CHOOSER_ITEM_IS_IN_USE_COLUMN, is_in_use,
                            CHOOSER_ITEM_IS_SEPARATED_COLUMN, is_separate,
                            -1);

        return TRUE;
}

void
gdm_chooser_widget_set_item_in_use (GdmChooserWidget *widget,
                                    const char       *id,
                                    gboolean          is_in_use)
{
        GtkTreeIter   iter;

        g_return_if_fail (GDM_IS_CHOOSER_WIDGET (widget));

        if (!find_item (widget, id, &iter)) {
                return;
        }

        gtk_list_store_set (widget->priv->list_store, &iter,
                            CHOOSER_ITEM_IS_IN_USE_COLUMN, is_in_use, -1);
}

void
gdm_chooser_widget_set_in_use_message (GdmChooserWidget *widget,
                                       const char       *message)
{
        g_return_if_fail (GDM_IS_CHOOSER_WIDGET (widget));

        g_free (widget->priv->in_use_message);
        widget->priv->in_use_message = g_strdup (message);
}

void
gdm_chooser_widget_set_separator_position (GdmChooserWidget         *widget,
                                           GdmChooserWidgetPosition  position)
{
        g_return_if_fail (GDM_IS_CHOOSER_WIDGET (widget));

        if (widget->priv->separator_position != position) {
                widget->priv->separator_position = position;
        }

        gtk_tree_model_filter_refilter (widget->priv->model_filter);
}

void
gdm_chooser_widget_set_hide_inactive_items (GdmChooserWidget  *widget,
                                            gboolean           should_hide)
{
        widget->priv->should_hide_inactive_items = should_hide;

        if (should_hide &&
            (widget->priv->state != GDM_CHOOSER_WIDGET_STATE_SHRUNK
             || widget->priv->state != GDM_CHOOSER_WIDGET_STATE_SHRINKING) &&
            widget->priv->active_row != NULL) {
                gdm_chooser_widget_shrink (widget);
        } else if (!should_hide &&
              (widget->priv->state != GDM_CHOOSER_WIDGET_STATE_GROWN
               || widget->priv->state != GDM_CHOOSER_WIDGET_STATE_GROWING)) {
                gdm_chooser_widget_grow (widget);
        }
}