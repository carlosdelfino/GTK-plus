/* gtkshortcutswindow.c
 *
 * Copyright (C) 2015 Christian Hergert <christian@hergert.me>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public License as
 *  published by the Free Software Foundation; either version 2 of the
 *  License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public
 *  License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "gtkshortcutswindow.h"
#include "gtkscrolledwindow.h"
#include "gtkshortcutssection.h"
#include "gtkshortcutsgroup.h"
#include "gtkshortcutsgesture.h"
#include "gtkshortcutsshortcut.h"
#include "gtkprivate.h"
#include "gtkintl.h"

/**
 * SECTION:gtkshortcutswindow
 * @Title: GtkShortcutsWindow
 * @Short_description: Toplevel which shows help for shortcuts
 *
 * A GtkShortcutsWindow shows brief information about the keyboard shortcuts
 * and gestures of an application. The shortcuts can be grouped, and you can
 * have multiple sections in this window, corresponding to the major modes of
 * your application.
 *
 * Additionally, the shortcuts can be filtered by the current view, to avoid
 * showing information that is not relevant in the current application context.
 *
 * The recommended way to construct a GtkShortcutsWindow is with GtkBuilder,
 * by populating a #GtkShortcutsWindow with one or more #GtkShortcutsSection
 * objects, which contain #GtkShortcutsGroups that in turn contain objects of
 * class #GtkShortcutsShortcut or #GtkShortcutsGesture.
 *
 * # A simple example:
 *
 * ![](gedit-shortcuts.png)
 *
 * This example has as single section. As you can see, the shortcut groups
 * are arranged in columns, and spread across several pages if there are too
 * many to find on a single page.
 *
 * The .ui file for this example can be found [here](https://git.gnome.org/browse/gtk+/tree/demos/gtk-demo/shortcuts-gedit.ui).
 *
 * # An example with multiple views:
 *
 * ![](clocks-shortcuts.png)
 *
 * This example shows a #GtkShortcutsWindow that has been configured to show only
 * the shortcuts relevant to the "stopwatch" view.
 *
 * The .ui file for this example can be found [here](https://git.gnome.org/browse/gtk+/tree/demos/gtk-demo/shortcuts-clocks.ui).
 *
 * # An example with multiple sections:
 *
 * ![](builder-shortcuts.png)
 *
 * This example shows a #GtkShortcutsWindow with two sections, "Editor Shortcuts"
 * and "Terminal Shortcuts".
 *
 * The .ui file for this example can be found [here](https://git.gnome.org/browse/gtk+/tree/demos/gtk-demo/shortcuts-clocks.ui).
 */

typedef struct
{
  GHashTable     *keywords;
  gchar          *initial_section;
  gchar          *last_section_name;
  gchar          *view_name;
  GtkSizeGroup   *search_text_group;
  GtkSizeGroup   *search_image_group;
  GHashTable     *search_items_hash;

  GtkStack       *stack;
  GtkStack       *title_stack;
  GtkMenuButton  *menu_button;
  GtkLabel       *menu_label;
  GtkSearchBar   *search_bar;
  GtkHeaderBar   *header_bar;
  GtkPopover     *popover;
  GtkListBox     *list_box;
  GtkBox         *search_gestures;
  GtkBox         *search_shortcuts;
} GtkShortcutsWindowPrivate;

typedef struct
{
  GtkShortcutsWindow *self;
  GtkBuilder        *builder;
  GQueue            *stack;
  gchar             *property_name;
  guint              translatable : 1;
} ViewsParserData;


G_DEFINE_TYPE_WITH_PRIVATE (GtkShortcutsWindow, gtk_shortcuts_window, GTK_TYPE_WINDOW)


enum {
  CLOSE,
  LAST_SIGNAL
};

enum {
  PROP_0,
  PROP_SECTION_NAME,
  PROP_VIEW_NAME,
  LAST_PROP
};

static GParamSpec *properties[LAST_PROP];
static guint signals[LAST_SIGNAL];


static gint
number_of_children (GtkContainer *container)
{
  GList *children;
  gint n;

  children = gtk_container_get_children (container);
  n = g_list_length (children);
  g_list_free (children);

  return n;
}

static void
update_title_stack (GtkShortcutsWindow *self)
{
  GtkShortcutsWindowPrivate *priv = gtk_shortcuts_window_get_instance_private (self);
  GtkWidget *visible_child;

  visible_child = gtk_stack_get_visible_child (priv->stack);

  if (GTK_IS_SHORTCUTS_SECTION (visible_child))
    {
      if (number_of_children (GTK_CONTAINER (priv->stack)) > 3)
        {
          gchar *title;

          gtk_stack_set_visible_child_name (priv->title_stack, "sections");
          g_object_get (visible_child, "title", &title, NULL);
          gtk_label_set_label (priv->menu_label, title);
          g_free (title);
        }
      else
        {
          gtk_stack_set_visible_child_name (priv->title_stack, "title");
        }
    }
  else if (visible_child != NULL)
    {
      gtk_stack_set_visible_child_name (priv->title_stack, "search");
    }
}

static void
gtk_shortcuts_window_add_search_item (GtkWidget *child, gpointer data)
{
  GtkShortcutsWindow *self = data;
  GtkShortcutsWindowPrivate *priv = gtk_shortcuts_window_get_instance_private (self);
  GtkWidget *item;
  gchar *subtitle = NULL;
  gchar *accelerator = NULL;
  gchar *title = NULL;
  gchar *hash_key = NULL;
  GIcon *icon = NULL;
  gchar *str;
  gchar *keywords;

  if (GTK_IS_SHORTCUTS_SHORTCUT (child))
    {
      g_object_get (child,
                    "accelerator", &accelerator,
                    "title", &title,
                    NULL);

      hash_key = g_strdup_printf ("%s-%s", title, accelerator);
      if (g_hash_table_contains (priv->search_items_hash, hash_key))
        {
          g_free (hash_key);
          g_free (title);
          g_free (accelerator);
          return;
        }

      g_hash_table_insert (priv->search_items_hash, hash_key, GINT_TO_POINTER (1));

      item = g_object_new (GTK_TYPE_SHORTCUTS_SHORTCUT,
                           "visible", TRUE,
                           "accelerator", accelerator,
                           "title", title,
                           "accel-size-group", priv->search_image_group,
                           "title-size-group", priv->search_text_group,
                           NULL);

      str = g_strdup_printf ("%s %s", accelerator, title);
      keywords = g_utf8_strdown (str, -1);

      g_hash_table_insert (priv->keywords, item, keywords);
      gtk_container_add (GTK_CONTAINER (priv->search_shortcuts), item);

      g_free (title);
      g_free (accelerator);
      g_free (str);
    }
  else if (GTK_IS_SHORTCUTS_GESTURE (child))
    {
      g_object_get (child,
                    "title", &title,
                    "subtitle", &subtitle,
                    "icon", &icon,
                    NULL);

      hash_key = g_strdup_printf ("%s-%s", title, subtitle);
      if (g_hash_table_contains (priv->search_items_hash, hash_key))
        {
          g_free (subtitle);
          g_free (title);
          g_free (hash_key);
          g_clear_object (&icon);
          return;
        }

      g_hash_table_insert (priv->search_items_hash, hash_key, GINT_TO_POINTER (1));

      item = g_object_new (GTK_TYPE_SHORTCUTS_GESTURE,
                           "visible", TRUE,
                           "title", title,
                           "subtitle", subtitle,
                           "icon", icon,
                           "icon-size-group", priv->search_image_group,
                           "title-size-group", priv->search_text_group,
                           NULL);

      str = g_strdup_printf ("%s %s", title, subtitle);
      keywords = g_utf8_strdown (str, -1);

      g_hash_table_insert (priv->keywords, item, keywords);
      gtk_container_add (GTK_CONTAINER (priv->search_gestures), item);

      g_free (subtitle);
      g_free (title);
      g_clear_object (&icon);
      g_free (str);
    }
  else if (GTK_IS_CONTAINER (child))
    {
      gtk_container_foreach (GTK_CONTAINER (child), gtk_shortcuts_window_add_search_item, self);
    }
}

static void
gtk_shortcuts_window_add_section (GtkShortcutsWindow  *self,
                                  GtkShortcutsSection *section)
{
  GtkShortcutsWindowPrivate *priv = gtk_shortcuts_window_get_instance_private (self);
  GtkListBoxRow *row;
  gchar *title;
  gchar *name;
  const gchar *visible_section;
  GtkWidget *label;

  gtk_container_foreach (GTK_CONTAINER (section), gtk_shortcuts_window_add_search_item, self);

  g_object_get (section,
                "section-name", &name,
                "title", &title,
                NULL);

  gtk_stack_add_titled (priv->stack, GTK_WIDGET (section), name, title);

  visible_section = gtk_stack_get_visible_child_name (priv->stack);
  if (strcmp (visible_section, "internal-search") == 0 ||
      (priv->initial_section && strcmp (priv->initial_section, visible_section) == 0))
    gtk_stack_set_visible_child (priv->stack, GTK_WIDGET (section));

  row = g_object_new (GTK_TYPE_LIST_BOX_ROW,
                      "visible", TRUE,
                      NULL);
  g_object_set_data_full (G_OBJECT (row), "GTK_SHORTCUTS_SECTION_NAME", g_strdup (name), g_free);
  label = g_object_new (GTK_TYPE_LABEL,
                        "margin", 6,
                        "label", title,
                        "xalign", 0.5f,
                        "visible", TRUE,
                        NULL);
  gtk_container_add (GTK_CONTAINER (row), GTK_WIDGET (label));
  gtk_container_add (GTK_CONTAINER (priv->list_box), GTK_WIDGET (row));

  update_title_stack (self);

  g_free (name);
  g_free (title);
}

static void
gtk_shortcuts_window_add (GtkContainer *container,
                          GtkWidget    *widget)
{
  GtkShortcutsWindow *self = (GtkShortcutsWindow *)container;

  if (GTK_IS_SHORTCUTS_SECTION (widget))
    gtk_shortcuts_window_add_section (self, GTK_SHORTCUTS_SECTION (widget));
  else
    g_warning ("Can't add children of type %s to %s",
               G_OBJECT_TYPE_NAME (widget),
               G_OBJECT_TYPE_NAME (container));
}

static void
gtk_shortcuts_window_set_view_name (GtkShortcutsWindow *self,
                                    const gchar        *view_name)
{
  GtkShortcutsWindowPrivate *priv = gtk_shortcuts_window_get_instance_private (self);
  GList *sections, *l;

  g_free (priv->view_name);
  priv->view_name = g_strdup (view_name);

  sections = gtk_container_get_children (GTK_CONTAINER (priv->stack));
  for (l = sections; l; l = l->next)
    {
      GtkShortcutsSection *section = l->data;

      if (GTK_IS_SHORTCUTS_SECTION (section))
        g_object_set (section, "view-name", priv->view_name, NULL);
    }
  g_list_free (sections);
}

static void
gtk_shortcuts_window_set_section_name (GtkShortcutsWindow *self,
                                       const gchar        *section_name)
{
  GtkShortcutsWindowPrivate *priv = gtk_shortcuts_window_get_instance_private (self);
  GtkWidget *section;

  g_free (priv->initial_section);
  priv->initial_section = g_strdup (section_name);

  section = gtk_stack_get_child_by_name (priv->stack, section_name);
  if (section)
    gtk_stack_set_visible_child (priv->stack, section);
}

static void
gtk_shortcuts_window__list_box__row_activated (GtkShortcutsWindow *self,
                                               GtkListBoxRow      *row,
                                               GtkListBox         *list_box)
{
  GtkShortcutsWindowPrivate *priv = gtk_shortcuts_window_get_instance_private (self);
  const gchar *name;

  name = g_object_get_data (G_OBJECT (row), "GTK_SHORTCUTS_SECTION_NAME");
  gtk_stack_set_visible_child_name (priv->stack, name);
  gtk_widget_hide (GTK_WIDGET (priv->popover));
}

static void
gtk_shortcuts_window__entry__changed (GtkShortcutsWindow *self,
                                     GtkSearchEntry      *search_entry)
{
  GtkShortcutsWindowPrivate *priv = gtk_shortcuts_window_get_instance_private (self);
  gchar *downcase = NULL;
  GHashTableIter iter;
  const gchar *text;
  const gchar *last_section_name;
  gpointer key;
  gpointer value;
  gboolean has_result;

  text = gtk_entry_get_text (GTK_ENTRY (search_entry));

  if (!text || !*text)
    {
      if (priv->last_section_name != NULL)
        {
          gtk_stack_set_visible_child_name (priv->stack, priv->last_section_name);
          return;
        }
    }

  last_section_name = gtk_stack_get_visible_child_name (priv->stack);

  if (g_strcmp0 (last_section_name, "internal-search") != 0 &&
      g_strcmp0 (last_section_name, "no-search-results") != 0)
    {
      g_free (priv->last_section_name);
      priv->last_section_name = g_strdup (last_section_name);
    }

  downcase = g_utf8_strdown (text, -1);

  g_hash_table_iter_init (&iter, priv->keywords);

  has_result = FALSE;
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      GtkWidget *widget = key;
      const gchar *keywords = value;
      gboolean match;

      match = strstr (keywords, downcase) != NULL;
      gtk_widget_set_visible (widget, match);
      has_result |= match;
    }

  g_free (downcase);

  if (has_result)
    gtk_stack_set_visible_child_name (priv->stack, "internal-search");
  else
    gtk_stack_set_visible_child_name (priv->stack, "no-search-results");
}

static void
gtk_shortcuts_window__search_mode__changed (GtkShortcutsWindow *self)
{
  GtkShortcutsWindowPrivate *priv = gtk_shortcuts_window_get_instance_private (self);

  if (!gtk_search_bar_get_search_mode (priv->search_bar))
    {
      if (priv->last_section_name != NULL)
        gtk_stack_set_visible_child_name (priv->stack, priv->last_section_name);
    }
}

static void
gtk_shortcuts_window_real_close (GtkShortcutsWindow *self)
{
  gtk_window_close (GTK_WINDOW (self));
}

static void
gtk_shortcuts_window_constructed (GObject *object)
{
  GtkShortcutsWindow *self = (GtkShortcutsWindow *)object;
  GtkShortcutsWindowPrivate *priv = gtk_shortcuts_window_get_instance_private (self);

  G_OBJECT_CLASS (gtk_shortcuts_window_parent_class)->constructed (object);

  if (priv->initial_section != NULL)
    gtk_stack_set_visible_child_name (priv->stack, priv->initial_section);
}

static void
gtk_shortcuts_window_finalize (GObject *object)
{
  GtkShortcutsWindow *self = (GtkShortcutsWindow *)object;
  GtkShortcutsWindowPrivate *priv = gtk_shortcuts_window_get_instance_private (self);

  g_clear_pointer (&priv->keywords, g_hash_table_unref);
  g_clear_pointer (&priv->initial_section, g_free);
  g_clear_pointer (&priv->view_name, g_free);
  g_clear_pointer (&priv->last_section_name, g_free);
  g_clear_pointer (&priv->search_items_hash, g_hash_table_unref);

  g_clear_object (&priv->search_image_group);
  g_clear_object (&priv->search_text_group);

  G_OBJECT_CLASS (gtk_shortcuts_window_parent_class)->finalize (object);
}

static void
gtk_shortcuts_window_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  GtkShortcutsWindow *self = (GtkShortcutsWindow *)object;
  GtkShortcutsWindowPrivate *priv = gtk_shortcuts_window_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_SECTION_NAME:
      {
        GtkWidget *child = gtk_stack_get_visible_child (priv->stack);

        if (child != NULL)
          {
            gchar *name = NULL;

            gtk_container_child_get (GTK_CONTAINER (priv->stack), child,
                                     "name", &name,
                                     NULL);
            g_value_take_string (value, name);
          }
      }
      break;

    case PROP_VIEW_NAME:
      g_value_set_string (value, priv->view_name);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gtk_shortcuts_window_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  GtkShortcutsWindow *self = (GtkShortcutsWindow *)object;

  switch (prop_id)
    {
    case PROP_SECTION_NAME:
      gtk_shortcuts_window_set_section_name (self, g_value_get_string (value));
      break;

    case PROP_VIEW_NAME:
      gtk_shortcuts_window_set_view_name (self, g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static GType
gtk_shortcuts_window_child_type (GtkContainer *container)
{
  return GTK_TYPE_SHORTCUTS_SECTION;
}

static void
gtk_shortcuts_window_class_init (GtkShortcutsWindowClass *klass)
{
  GtkContainerClass *container_class = GTK_CONTAINER_CLASS (klass);
  GtkBindingSet *binding_set = gtk_binding_set_by_class (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = gtk_shortcuts_window_constructed;
  object_class->finalize = gtk_shortcuts_window_finalize;
  object_class->get_property = gtk_shortcuts_window_get_property;
  object_class->set_property = gtk_shortcuts_window_set_property;

  container_class->add = gtk_shortcuts_window_add;
  container_class->child_type = gtk_shortcuts_window_child_type;

  klass->close = gtk_shortcuts_window_real_close;

  /**
   * GtkShortcutsWindow:section-name:
   *
   * The name of the section to show.
   *
   * This should be the section-name of one of the #GtkShortcutsSection
   * objects that are in this shortcuts window.
   */
  properties[PROP_SECTION_NAME] =
    g_param_spec_string ("section-name", P_("Section Name"), P_("Section Name"),
                         NULL,
                         (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GtkShortcutsWindow:view-name:
   *
   * The view name by which to filter the contents.
   *
   * This should correspond to the #GtkShortcutsGroup:view property of some of
   * the #GtkShortcutsGroup objects that are inside this shortcuts window.
   *
   * Set this to %NULL to show all groups.
   */
  properties[PROP_VIEW_NAME] =
    g_param_spec_string ("view-name", P_("View Name"), P_("View Name"),
                         NULL,
                         (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, LAST_PROP, properties);

  /**
   * GtkShortcutsWindow::close:
   *
   * The ::close signal is a
   * [keybinding signal][GtkBindingSignal]
   * which gets emitted when the user uses a keybinding to close
   * the window.
   *
   * The default binding for this signal is the Escape key.
   */
  signals[CLOSE] = g_signal_new (I_("close"),
                                 G_TYPE_FROM_CLASS (klass),
                                 G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
                                 G_STRUCT_OFFSET (GtkShortcutsWindowClass, close),
                                 NULL, NULL, NULL,
                                 G_TYPE_NONE,
                                 0);
  gtk_binding_entry_add_signal (binding_set, GDK_KEY_Escape, 0, "close", 0);

  g_type_ensure (GTK_TYPE_SHORTCUTS_GROUP);
  g_type_ensure (GTK_TYPE_SHORTCUTS_GESTURE);
  g_type_ensure (GTK_TYPE_SHORTCUTS_SHORTCUT);
}

static gboolean
window_key_press_event_cb (GtkWidget *window,
                           GdkEvent  *event,
                           gpointer   data)
{
  GtkShortcutsWindow *self = GTK_SHORTCUTS_WINDOW (window);
  GtkShortcutsWindowPrivate *priv = gtk_shortcuts_window_get_instance_private (self);

  return gtk_search_bar_handle_event (priv->search_bar, event);
}

static void
gtk_shortcuts_window_init (GtkShortcutsWindow *self)
{
  GtkShortcutsWindowPrivate *priv = gtk_shortcuts_window_get_instance_private (self);
  GtkToggleButton *search_button;
  GtkBox *main_box;
  GtkBox *menu_box;
  GtkBox *box;
  GtkArrow *arrow;
  GtkWidget *entry;
  GtkWidget *scroller;
  GtkWidget *label;
  GtkWidget *empty;
  PangoAttrList *attributes;

  gtk_window_set_resizable (GTK_WINDOW (self), FALSE);
  gtk_window_set_type_hint (GTK_WINDOW (self), GDK_WINDOW_TYPE_HINT_DIALOG);

  g_signal_connect (self, "key-press-event",
                    G_CALLBACK (window_key_press_event_cb), NULL);

  priv->keywords = g_hash_table_new_full (NULL, NULL, NULL, g_free);
  priv->search_items_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  priv->search_text_group = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);
  priv->search_image_group = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);

  priv->header_bar = g_object_new (GTK_TYPE_HEADER_BAR,
                                   "show-close-button", TRUE,
                                   "visible", TRUE,
                                   NULL);
  gtk_window_set_titlebar (GTK_WINDOW (self), GTK_WIDGET (priv->header_bar));

  search_button = g_object_new (GTK_TYPE_TOGGLE_BUTTON,
                                "child", g_object_new (GTK_TYPE_IMAGE,
                                                       "visible", TRUE,
                                                       "icon-name", "edit-find-symbolic",
                                                       NULL),
                                "visible", TRUE,
                                NULL);
  gtk_container_add (GTK_CONTAINER (priv->header_bar), GTK_WIDGET (search_button));

  main_box = g_object_new (GTK_TYPE_BOX,
                           "orientation", GTK_ORIENTATION_VERTICAL,
                           "visible", TRUE,
                           NULL);
  GTK_CONTAINER_CLASS (gtk_shortcuts_window_parent_class)->add (GTK_CONTAINER (self), GTK_WIDGET (main_box));

  priv->search_bar = g_object_new (GTK_TYPE_SEARCH_BAR,
                                   "visible", TRUE,
                                   NULL);
  g_object_bind_property (priv->search_bar, "search-mode-enabled",
                          search_button, "active",
                          G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);
  gtk_container_add (GTK_CONTAINER (main_box), GTK_WIDGET (priv->search_bar));

  priv->stack = g_object_new (GTK_TYPE_STACK,
                              "expand", TRUE,
                              "homogeneous", TRUE,
                              "transition-type", GTK_STACK_TRANSITION_TYPE_CROSSFADE,
                              "visible", TRUE,
                              NULL);
  gtk_container_add (GTK_CONTAINER (main_box), GTK_WIDGET (priv->stack));

  priv->title_stack = g_object_new (GTK_TYPE_STACK,
                                    "visible", TRUE,
                                    NULL);
  gtk_header_bar_set_custom_title (priv->header_bar, GTK_WIDGET (priv->title_stack));

  label = gtk_label_new (_("Shortcuts"));
  gtk_widget_show (label);
  gtk_style_context_add_class (gtk_widget_get_style_context (label), "title");
  gtk_stack_add_named (priv->title_stack, label, "title");

  label = gtk_label_new (_("Search Results"));
  gtk_widget_show (label);
  gtk_style_context_add_class (gtk_widget_get_style_context (label), "title");
  gtk_stack_add_named (priv->title_stack, label, "search");

  priv->menu_button = g_object_new (GTK_TYPE_MENU_BUTTON,
                                    "focus-on-click", FALSE,
                                    "visible", TRUE,
                                    NULL);
  gtk_style_context_add_class (gtk_widget_get_style_context (GTK_WIDGET (priv->menu_button)), "flat");
  gtk_stack_add_named (priv->title_stack, GTK_WIDGET (priv->menu_button), "sections");

  menu_box = g_object_new (GTK_TYPE_BOX,
                           "orientation", GTK_ORIENTATION_HORIZONTAL,
                           "spacing", 6,
                           "visible", TRUE,
                           NULL);
  gtk_container_add (GTK_CONTAINER (priv->menu_button), GTK_WIDGET (menu_box));

  priv->menu_label = g_object_new (GTK_TYPE_LABEL,
                                   "visible", TRUE,
                                   NULL);
  gtk_container_add (GTK_CONTAINER (menu_box), GTK_WIDGET (priv->menu_label));

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
  arrow = g_object_new (GTK_TYPE_ARROW,
                        "arrow-type", GTK_ARROW_DOWN,
                        "visible", TRUE,
                        NULL);
  gtk_container_add (GTK_CONTAINER (menu_box), GTK_WIDGET (arrow));
  G_GNUC_END_IGNORE_DEPRECATIONS;

  priv->popover = g_object_new (GTK_TYPE_POPOVER,
                                "border-width", 6,
                                "relative-to", priv->menu_button,
                                "position", GTK_POS_BOTTOM,
                                NULL);
  gtk_menu_button_set_popover (priv->menu_button, GTK_WIDGET (priv->popover));

  priv->list_box = g_object_new (GTK_TYPE_LIST_BOX,
                                 "selection-mode", GTK_SELECTION_NONE,
                                 "visible", TRUE,
                                 NULL);
  g_signal_connect_object (priv->list_box,
                           "row-activated",
                           G_CALLBACK (gtk_shortcuts_window__list_box__row_activated),
                           self,
                           G_CONNECT_SWAPPED);
  gtk_container_add (GTK_CONTAINER (priv->popover), GTK_WIDGET (priv->list_box));

  entry = gtk_search_entry_new ();
  gtk_widget_show (entry);
  gtk_container_add (GTK_CONTAINER (priv->search_bar), entry);
  g_object_set (entry,
                "placeholder-text", _("Search Shortcuts"),
                "width-chars", 40,
                NULL);
  g_signal_connect_object (entry,
                           "search-changed",
                           G_CALLBACK (gtk_shortcuts_window__entry__changed),
                           self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (priv->search_bar,
                           "notify::search-mode-enabled",
                           G_CALLBACK (gtk_shortcuts_window__search_mode__changed),
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (priv->stack, "notify::visible-child",
                           G_CALLBACK (update_title_stack), self, G_CONNECT_SWAPPED);

  scroller = g_object_new (GTK_TYPE_SCROLLED_WINDOW,
                           "visible", TRUE,
                           NULL);
  box = g_object_new (GTK_TYPE_BOX,
                      "border-width", 24,
                      "halign", GTK_ALIGN_CENTER,
                      "spacing", 24,
                      "orientation", GTK_ORIENTATION_VERTICAL,
                      "visible", TRUE,
                      NULL);
  gtk_container_add (GTK_CONTAINER (scroller), GTK_WIDGET (box));
  gtk_stack_add_named (priv->stack, scroller, "internal-search");

  priv->search_shortcuts = g_object_new (GTK_TYPE_BOX,
                                         "halign", GTK_ALIGN_CENTER,
                                         "spacing", 6,
                                         "orientation", GTK_ORIENTATION_VERTICAL,
                                         "visible", TRUE,
                                         NULL);
  gtk_container_add (GTK_CONTAINER (box), GTK_WIDGET (priv->search_shortcuts));

  priv->search_gestures = g_object_new (GTK_TYPE_BOX,
                                        "halign", GTK_ALIGN_CENTER,
                                        "spacing", 6,
                                        "orientation", GTK_ORIENTATION_VERTICAL,
                                        "visible", TRUE,
                                        NULL);
  gtk_container_add (GTK_CONTAINER (box), GTK_WIDGET (priv->search_gestures));

  empty = g_object_new (GTK_TYPE_GRID,
                        "visible", TRUE,
                        "row-spacing", 12,
                        "margin", 12,
                        "hexpand", TRUE,
                        "vexpand", TRUE,
                        "halign", GTK_ALIGN_CENTER,
                        "valign", GTK_ALIGN_CENTER,
                        NULL);
  gtk_style_context_add_class (gtk_widget_get_style_context (empty), "dim-label");
  gtk_grid_attach (GTK_GRID (empty),
                   g_object_new (GTK_TYPE_IMAGE,
                                 "visible", TRUE,
                                 "icon-name", "edit-find-symbolic",
                                 "pixel-size", 72,
                                 NULL),
                   0, 0, 1, 1);
  attributes = pango_attr_list_new ();
  pango_attr_list_insert (attributes, pango_attr_weight_new (PANGO_WEIGHT_BOLD));
  pango_attr_list_insert (attributes, pango_attr_scale_new (1.44));
  label = g_object_new (GTK_TYPE_LABEL,
                        "visible", TRUE,
                        "label", _("No Results Found"),
                        "attributes", attributes,
                        NULL);
  pango_attr_list_unref (attributes);
  gtk_grid_attach (GTK_GRID (empty), label, 0, 1, 1, 1);
  label = g_object_new (GTK_TYPE_LABEL,
                        "visible", TRUE,
                        "label", _("Try a different search"),
                        NULL);
  gtk_grid_attach (GTK_GRID (empty), label, 0, 2, 1, 1);

  gtk_stack_add_named (priv->stack, empty, "no-search-results");
}