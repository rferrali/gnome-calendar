/* gcal-suggestion-entry-row.c
 *
 * Copyright 2026 Romain Ferrali <rferrali@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * GcalSuggestionEntryRow is an AdwEntryRow with a list of suggestions under
 * it, for type-ahead.
 *
 * It owns the popover and the keyboard handling (Down and Up move through the
 * list, Return and Tab pick the selected row, Escape closes it), and asks for
 * suggestions with the #GcalSuggestionEntryRow::search signal once the text
 * has at least #GcalSuggestionEntryRow:min-chars characters (0 also asks when
 * an empty row gets the focus). The owner answers, possibly asynchronously,
 * with gcal_suggestion_entry_row_set_suggestions() and the rows to show, and
 * learns which one was picked from #GcalSuggestionEntryRow::suggestion-activated.
 * Rows are the owner's: it puts whatever data it needs on them.
 *
 * The popover never takes the focus, so typing goes on in the entry.
 */

#define G_LOG_DOMAIN "GcalSuggestionEntryRow"

#include "gcal-suggestion-entry-row.h"

#define MAX_CONTENT_HEIGHT 320

struct _GcalSuggestionEntryRow
{
  AdwEntryRow         parent_instance;

  GtkWidget          *popover;
  GtkListBox         *list;
  GtkEventController *focus_controller;

  guint               min_chars;
  guint               delay;

  guint               search_timeout_id;
  guint               focus_out_id;
  gboolean            picking;
};

G_DEFINE_FINAL_TYPE (GcalSuggestionEntryRow, gcal_suggestion_entry_row, ADW_TYPE_ENTRY_ROW)

enum
{
  PROP_0,
  PROP_MIN_CHARS,
  PROP_DELAY,
  N_PROPS
};

enum
{
  SEARCH,
  SUGGESTION_ACTIVATED,
  N_SIGNALS
};

static GParamSpec *properties[N_PROPS] = { NULL, };
static guint signals[N_SIGNALS] = { 0, };


/*
 * Auxiliary methods
 */

/* The root's focus is unset while the focus moves, so it can't be asked from
 * the focus controller's signals; the controller itself is already up to date */
static gboolean
entry_has_focus (GcalSuggestionEntryRow *self)
{
  return gtk_event_controller_focus_contains_focus (GTK_EVENT_CONTROLLER_FOCUS (self->focus_controller));
}

/* As wide as the row: with no natural width of their own, the rows would
 * otherwise shrink to their ellipsis */
static void
update_popover_width (GcalSuggestionEntryRow *self)
{
  gtk_widget_set_size_request (gtk_popover_get_child (GTK_POPOVER (self->popover)),
                               gtk_widget_get_width (GTK_WIDGET (self)),
                               -1);
}

static void
emit_search (GcalSuggestionEntryRow *self)
{
  g_signal_emit (self, signals[SEARCH], 0, gtk_editable_get_text (GTK_EDITABLE (self)));
}

static gboolean
search_timeout_cb (gpointer user_data)
{
  GcalSuggestionEntryRow *self = user_data;

  self->search_timeout_id = 0;
  emit_search (self);

  return G_SOURCE_REMOVE;
}

static void
request_search (GcalSuggestionEntryRow *self)
{
  const gchar *text = gtk_editable_get_text (GTK_EDITABLE (self));

  g_clear_handle_id (&self->search_timeout_id, g_source_remove);

  if ((guint) g_utf8_strlen (text, -1) < self->min_chars)
    {
      gcal_suggestion_entry_row_hide_suggestions (self);
      return;
    }

  if (self->delay == 0)
    emit_search (self);
  else
    self->search_timeout_id = g_timeout_add (self->delay, search_timeout_cb, self);
}

static void
pick (GcalSuggestionEntryRow *self,
      GtkListBoxRow          *row)
{
  /* The owner may destroy this row, or rebuild the list, from its handler */
  g_object_ref (self);
  g_object_ref (row);
  gcal_suggestion_entry_row_hide_suggestions (self);

  /* The owner usually sets the text here: that must not search again */
  self->picking = TRUE;
  g_signal_emit (self, signals[SUGGESTION_ACTIVATED], 0, row);
  self->picking = FALSE;

  g_object_unref (row);
  g_object_unref (self);
}

static void
move_selection (GcalSuggestionEntryRow *self,
                gint                    delta)
{
  GtkListBoxRow *selected = gtk_list_box_get_selected_row (self->list);
  GtkListBoxRow *row;
  gint index;

  index = selected ? gtk_list_box_row_get_index (selected) + delta : (delta > 0 ? 0 : -1);
  if (index < 0)
    {
      gtk_list_box_unselect_all (self->list);
      return;
    }

  row = gtk_list_box_get_row_at_index (self->list, index);
  if (row)
    gtk_list_box_select_row (self->list, row);
}


/*
 * Callbacks
 */

static void
on_row_activated_cb (GtkListBox             *list,
                     GtkListBoxRow          *row,
                     GcalSuggestionEntryRow *self)
{
  pick (self, row);
}

static void
on_changed_cb (GtkEditable            *editable,
               GcalSuggestionEntryRow *self)
{
  if (self->picking)
    return;

  request_search (self);
}

static gboolean
on_key_pressed_cb (GtkEventControllerKey  *controller,
                   guint                   keyval,
                   guint                   keycode,
                   GdkModifierType         state,
                   GcalSuggestionEntryRow *self)
{
  GtkListBoxRow *selected;

  if (!gtk_widget_get_visible (self->popover))
    {
      /* Down opens the list, like a combo */
      if ((keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) &&
          (guint) g_utf8_strlen (gtk_editable_get_text (GTK_EDITABLE (self)), -1) >= self->min_chars)
        {
          g_clear_handle_id (&self->search_timeout_id, g_source_remove);
          emit_search (self);
          return GDK_EVENT_STOP;
        }

      return GDK_EVENT_PROPAGATE;
    }

  switch (keyval)
    {
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
      move_selection (self, 1);
      return GDK_EVENT_STOP;

    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
      move_selection (self, -1);
      return GDK_EVENT_STOP;

    case GDK_KEY_Escape:
      gcal_suggestion_entry_row_hide_suggestions (self);
      return GDK_EVENT_STOP;

    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_ISO_Enter:
    case GDK_KEY_Tab:
      selected = gtk_list_box_get_selected_row (self->list);
      if (selected)
        {
          pick (self, selected);
          return GDK_EVENT_STOP;
        }
      gcal_suggestion_entry_row_hide_suggestions (self);
      return GDK_EVENT_PROPAGATE;

    default:
      return GDK_EVENT_PROPAGATE;
    }
}

static void
on_focus_enter_cb (GtkEventControllerFocus *controller,
                   GcalSuggestionEntryRow  *self)
{
  g_clear_handle_id (&self->focus_out_id, g_source_remove);

  if (self->min_chars == 0 && gtk_editable_get_editable (GTK_EDITABLE (self)))
    request_search (self);
}

static gboolean
focus_out_cb (gpointer user_data)
{
  GcalSuggestionEntryRow *self = user_data;

  self->focus_out_id = 0;

  if (!entry_has_focus (self))
    gcal_suggestion_entry_row_hide_suggestions (self);

  return G_SOURCE_REMOVE;
}

static void
on_focus_leave_cb (GtkEventControllerFocus *controller,
                   GcalSuggestionEntryRow  *self)
{
  /* Checked a moment later: the focus may only be moving within the row */
  g_clear_handle_id (&self->focus_out_id, g_source_remove);
  self->focus_out_id = g_timeout_add (100, focus_out_cb, self);
}


/*
 * GtkWidget overrides
 */

static void
gcal_suggestion_entry_row_size_allocate (GtkWidget *widget,
                                         int        width,
                                         int        height,
                                         int        baseline)
{
  GcalSuggestionEntryRow *self = GCAL_SUGGESTION_ENTRY_ROW (widget);

  GTK_WIDGET_CLASS (gcal_suggestion_entry_row_parent_class)->size_allocate (widget, width, height, baseline);

  gtk_popover_present (GTK_POPOVER (self->popover));
}

static void
gcal_suggestion_entry_row_unmap (GtkWidget *widget)
{
  gcal_suggestion_entry_row_hide_suggestions (GCAL_SUGGESTION_ENTRY_ROW (widget));

  GTK_WIDGET_CLASS (gcal_suggestion_entry_row_parent_class)->unmap (widget);
}


/*
 * GObject overrides
 */

static void
gcal_suggestion_entry_row_dispose (GObject *object)
{
  GcalSuggestionEntryRow *self = GCAL_SUGGESTION_ENTRY_ROW (object);

  g_clear_handle_id (&self->search_timeout_id, g_source_remove);
  g_clear_handle_id (&self->focus_out_id, g_source_remove);
  g_clear_pointer (&self->popover, gtk_widget_unparent);
  self->list = NULL;

  G_OBJECT_CLASS (gcal_suggestion_entry_row_parent_class)->dispose (object);
}

static void
gcal_suggestion_entry_row_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  GcalSuggestionEntryRow *self = GCAL_SUGGESTION_ENTRY_ROW (object);

  switch (prop_id)
    {
    case PROP_MIN_CHARS:
      g_value_set_uint (value, self->min_chars);
      break;

    case PROP_DELAY:
      g_value_set_uint (value, self->delay);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gcal_suggestion_entry_row_set_property (GObject      *object,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  GcalSuggestionEntryRow *self = GCAL_SUGGESTION_ENTRY_ROW (object);

  switch (prop_id)
    {
    case PROP_MIN_CHARS:
      gcal_suggestion_entry_row_set_min_chars (self, g_value_get_uint (value));
      break;

    case PROP_DELAY:
      gcal_suggestion_entry_row_set_delay (self, g_value_get_uint (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gcal_suggestion_entry_row_class_init (GcalSuggestionEntryRowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = gcal_suggestion_entry_row_dispose;
  object_class->get_property = gcal_suggestion_entry_row_get_property;
  object_class->set_property = gcal_suggestion_entry_row_set_property;

  widget_class->size_allocate = gcal_suggestion_entry_row_size_allocate;
  widget_class->unmap = gcal_suggestion_entry_row_unmap;

  /**
   * GcalSuggestionEntryRow:min-chars:
   *
   * The number of characters to type before suggestions are asked for. With
   * 0, they are also asked for when the row gets the focus while empty.
   */
  properties[PROP_MIN_CHARS] = g_param_spec_uint ("min-chars", NULL, NULL,
                                                  0, G_MAXUINT, 1,
                                                  G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  /**
   * GcalSuggestionEntryRow:delay:
   *
   * How long to wait after the last keystroke before asking for suggestions,
   * in milliseconds. 0 asks at once.
   */
  properties[PROP_DELAY] = g_param_spec_uint ("delay", NULL, NULL,
                                              0, G_MAXUINT, 0,
                                              G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);

  /**
   * GcalSuggestionEntryRow::search:
   * @text: the current text
   *
   * Asks for suggestions matching @text. Answer with
   * gcal_suggestion_entry_row_set_suggestions(), now or later.
   */
  signals[SEARCH] = g_signal_new ("search",
                                  GCAL_TYPE_SUGGESTION_ENTRY_ROW,
                                  G_SIGNAL_RUN_LAST,
                                  0, NULL, NULL, NULL,
                                  G_TYPE_NONE,
                                  1,
                                  G_TYPE_STRING);

  /**
   * GcalSuggestionEntryRow::suggestion-activated:
   * @row: the #GtkListBoxRow that was picked
   *
   * Emitted when a suggestion is clicked, or picked with Return or Tab.
   * Changing the text from the handler does not ask for new suggestions.
   */
  signals[SUGGESTION_ACTIVATED] = g_signal_new ("suggestion-activated",
                                                GCAL_TYPE_SUGGESTION_ENTRY_ROW,
                                                G_SIGNAL_RUN_LAST,
                                                0, NULL, NULL, NULL,
                                                G_TYPE_NONE,
                                                1,
                                                GTK_TYPE_LIST_BOX_ROW);
}

static void
gcal_suggestion_entry_row_init (GcalSuggestionEntryRow *self)
{
  GtkEventController *controller;
  GtkWidget *scrolled;

  self->min_chars = 1;

  g_signal_connect (self, "changed", G_CALLBACK (on_changed_cb), self);

  controller = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
  g_signal_connect (controller, "key-pressed", G_CALLBACK (on_key_pressed_cb), self);
  gtk_widget_add_controller (GTK_WIDGET (self), controller);

  self->focus_controller = gtk_event_controller_focus_new ();
  g_signal_connect (self->focus_controller, "enter", G_CALLBACK (on_focus_enter_cb), self);
  g_signal_connect (self->focus_controller, "leave", G_CALLBACK (on_focus_leave_cb), self);
  gtk_widget_add_controller (GTK_WIDGET (self), self->focus_controller);

  self->list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (self->list, GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click (self->list, TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (self->list), "navigation-sidebar");
  g_signal_connect (self->list, "row-activated", G_CALLBACK (on_row_activated_cb), self);

  scrolled = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scrolled), TRUE);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scrolled), MAX_CONTENT_HEIGHT);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), GTK_WIDGET (self->list));

  self->popover = gtk_popover_new ();
  gtk_popover_set_child (GTK_POPOVER (self->popover), scrolled);
  gtk_popover_set_autohide (GTK_POPOVER (self->popover), FALSE);
  gtk_popover_set_has_arrow (GTK_POPOVER (self->popover), FALSE);
  gtk_popover_set_position (GTK_POPOVER (self->popover), GTK_POS_BOTTOM);
  gtk_widget_set_can_focus (self->popover, FALSE);
  gtk_widget_set_halign (self->popover, GTK_ALIGN_START);
  gtk_widget_set_parent (self->popover, GTK_WIDGET (self));
}

GtkWidget*
gcal_suggestion_entry_row_new (void)
{
  return g_object_new (GCAL_TYPE_SUGGESTION_ENTRY_ROW, NULL);
}

/**
 * gcal_suggestion_entry_row_set_suggestions:
 * @self: a #GcalSuggestionEntryRow
 * @rows: (nullable)(element-type GtkListBoxRow): the rows to show, in order
 * @select_first: whether the first row starts selected, so Return picks it
 *
 * Replaces the suggestions. The list opens if there is at least one row and
 * the row still has the focus, and closes otherwise.
 */
void
gcal_suggestion_entry_row_set_suggestions (GcalSuggestionEntryRow *self,
                                           GPtrArray              *rows,
                                           gboolean                select_first)
{
  g_return_if_fail (GCAL_IS_SUGGESTION_ENTRY_ROW (self));

  gtk_list_box_remove_all (self->list);

  for (guint i = 0; rows && i < rows->len; i++)
    gtk_list_box_append (self->list, g_ptr_array_index (rows, i));

  if (!rows || rows->len == 0 || !gtk_widget_get_mapped (GTK_WIDGET (self)) || !entry_has_focus (self))
    {
      gtk_popover_popdown (GTK_POPOVER (self->popover));
      return;
    }

  if (select_first)
    gtk_list_box_select_row (self->list, gtk_list_box_get_row_at_index (self->list, 0));

  update_popover_width (self);
  gtk_popover_popup (GTK_POPOVER (self->popover));
}

/**
 * gcal_suggestion_entry_row_hide_suggestions:
 * @self: a #GcalSuggestionEntryRow
 *
 * Closes the list and drops any search waiting to start.
 */
void
gcal_suggestion_entry_row_hide_suggestions (GcalSuggestionEntryRow *self)
{
  g_return_if_fail (GCAL_IS_SUGGESTION_ENTRY_ROW (self));

  g_clear_handle_id (&self->search_timeout_id, g_source_remove);

  if (self->popover)
    gtk_popover_popdown (GTK_POPOVER (self->popover));
}

guint
gcal_suggestion_entry_row_get_min_chars (GcalSuggestionEntryRow *self)
{
  g_return_val_if_fail (GCAL_IS_SUGGESTION_ENTRY_ROW (self), 0);

  return self->min_chars;
}

void
gcal_suggestion_entry_row_set_min_chars (GcalSuggestionEntryRow *self,
                                         guint                   min_chars)
{
  g_return_if_fail (GCAL_IS_SUGGESTION_ENTRY_ROW (self));

  if (self->min_chars == min_chars)
    return;

  self->min_chars = min_chars;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_MIN_CHARS]);
}

guint
gcal_suggestion_entry_row_get_delay (GcalSuggestionEntryRow *self)
{
  g_return_val_if_fail (GCAL_IS_SUGGESTION_ENTRY_ROW (self), 0);

  return self->delay;
}

void
gcal_suggestion_entry_row_set_delay (GcalSuggestionEntryRow *self,
                                     guint                   delay)
{
  g_return_if_fail (GCAL_IS_SUGGESTION_ENTRY_ROW (self));

  if (self->delay == delay)
    return;

  self->delay = delay;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_DELAY]);
}
