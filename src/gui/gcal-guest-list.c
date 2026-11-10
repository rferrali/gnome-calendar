/* gcal-guest-list.c
 *
 * Local fork addition (not upstream): list of guests with their status.
 *
 * One row per guest: a status icon (accepted, tentative, declined, not
 * answered), the name and address, and tags (Organizer, You, Optional, Room,
 * Resource). The organizer comes first, then the user, then the other people,
 * then rooms and resources.
 *
 * #GcalGuestList:summary gives a count line such as
 * "2 accepted, 1 declined, 1 awaiting".
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
 */

#define G_LOG_DOMAIN "GcalGuestList"

#include "config.h"

#include "gcal-guest-list.h"

#include <adwaita.h>
#include <glib/gi18n.h>

#include "gcal-application.h"
#include "gcal-contact-search.h"
#include "gcal-context.h"
#include "gcal-event-attendee.h"
#include "gcal-event-organizer.h"
#include "gcal-rsvp.h"
#include "gcal-utils.h"

struct _GcalGuestList
{
  GtkBox              parent_instance;

  GtkListBox         *list;
  gboolean            editable;
  gboolean            keep_last_guest;
  guint               n_removable;
  GtkWidget          *add_row;
  guint               n_guests;

  /* Suggestions for the "Add Guest" row */
  GtkWidget          *popover;
  GtkListBox         *suggestions;
  GHashTable         *excluded;      /* lower addresses: current guests and the user */
  guint               search_timeout_id;
  GCancellable       *search_cancellable;
  gchar              *summary;
};

G_DEFINE_FINAL_TYPE (GcalGuestList, gcal_guest_list, GTK_TYPE_BOX)

enum
{
  PROP_0,
  PROP_N_GUESTS,
  PROP_SUMMARY,
  N_PROPS
};

enum
{
  ADD_GUEST,
  REMOVE_GUEST,
  N_SIGNALS
};

static GParamSpec *properties[N_PROPS] = { NULL, };
static guint signals[N_SIGNALS] = { 0, };

typedef enum
{
  GROUP_ORGANIZER,
  GROUP_USER,
  GROUP_REQUIRED,
  GROUP_OPTIONAL,
  GROUP_RESOURCE,
} GuestGroup;

typedef struct
{
  gchar                     *name;
  gchar                     *address;
  GcalEventAttendeePartStat  status;
  gboolean                   has_status;
  gboolean                   is_organizer;
  gboolean                   is_user;
  gboolean                   is_optional;
  GcalEventAttendeeType      type;
  guint                      index;
} Guest;

static void
guest_free (Guest *guest)
{
  g_free (guest->name);
  g_free (guest->address);
  g_free (guest);
}

static GuestGroup
guest_group (const Guest *guest)
{
  if (guest->is_organizer)
    return GROUP_ORGANIZER;
  if (guest->type == GCAL_EVENT_ATTENDEE_TYPE_RESOURCE || guest->type == GCAL_EVENT_ATTENDEE_TYPE_ROOM)
    return GROUP_RESOURCE;
  if (guest->is_user)
    return GROUP_USER;
  if (guest->is_optional)
    return GROUP_OPTIONAL;
  return GROUP_REQUIRED;
}

static gint
compare_guests (gconstpointer a,
                gconstpointer b)
{
  const Guest *ga = *(const Guest **) a;
  const Guest *gb = *(const Guest **) b;
  gint diff;

  diff = (gint) guest_group (ga) - (gint) guest_group (gb);
  if (diff != 0)
    return diff;

  return (gint) ga->index - (gint) gb->index;
}

static gchar *
dup_address (const gchar *uri)
{
  const gchar *address = e_cal_util_strip_mailto (uri);

  return (address && *address) ? g_strdup (address) : NULL;
}

static void
status_icon (const Guest  *guest,
             const gchar **out_icon,
             const gchar **out_css,
             const gchar **out_tooltip)
{
  if (!guest->has_status)
    {
      *out_icon = "avatar-default-symbolic";
      *out_css = "dimmed";
      *out_tooltip = _("Organizer");
      return;
    }

  switch (guest->status)
    {
    case GCAL_EVENT_ATTENDEE_PART_ACCEPTED:
      *out_icon = "object-select-symbolic";
      *out_css = "success";
      *out_tooltip = _("Accepted");
      break;

    case GCAL_EVENT_ATTENDEE_PART_TENTATIVE:
      *out_icon = "dialog-question-symbolic";
      *out_css = "warning";
      *out_tooltip = _("Tentative");
      break;

    case GCAL_EVENT_ATTENDEE_PART_DECLINED:
      *out_icon = "window-close-symbolic";
      *out_css = "error";
      *out_tooltip = _("Declined");
      break;

    case GCAL_EVENT_ATTENDEE_PART_DELEGATED:
      *out_icon = "radio-mixed-symbolic";
      *out_css = "dimmed";
      *out_tooltip = _("Delegated");
      break;

    case GCAL_EVENT_ATTENDEE_PART_NEEDS_ACTION:
    default:
      *out_icon = "user-status-pending-symbolic";
      *out_css = "dimmed";
      *out_tooltip = _("Not answered yet");
      break;
    }
}

static void
on_remove_button_clicked_cb (GtkButton     *button,
                             GcalGuestList *self)
{
  const gchar *address = g_object_get_data (G_OBJECT (button), "guest-address");

  g_signal_emit (self, signals[REMOVE_GUEST], 0, address);
}

#define MAX_SUGGESTIONS 8
#define SEARCH_DELAY_MS 150

static void
hide_suggestions (GcalGuestList *self)
{
  g_clear_handle_id (&self->search_timeout_id, g_source_remove);
  g_cancellable_cancel (self->search_cancellable);
  g_clear_object (&self->search_cancellable);

  if (self->popover)
    gtk_popover_popdown (GTK_POPOVER (self->popover));
}

static void
emit_add_guest (GcalGuestList *self,
                const gchar   *address,
                const gchar   *name)
{
  hide_suggestions (self);
  g_signal_emit (self, signals[ADD_GUEST], 0, address, name);
}

static void
on_add_guest_cb (AdwEntryRow   *entry_row,
                 GcalGuestList *self)
{
  g_autofree gchar *text = NULL;
  g_autofree gchar *address = NULL;
  g_autofree gchar *name = NULL;

  text = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (entry_row))));
  if (!text || !*text)
    return;

  /* Accept "Name <address>" as well as a bare address */
  if (strchr (text, '<') && strchr (text, '>') > strchr (text, '<'))
    {
      address = g_strndup (strchr (text, '<') + 1, strchr (text, '>') - strchr (text, '<') - 1);
      name = g_strndup (text, strchr (text, '<') - text);
      g_strstrip (address);
      g_strstrip (name);
      g_strdelimit (name, "\"", ' ');
      g_strstrip (name);
    }
  else
    {
      address = g_strdup (text);
    }

  if (!strchr (address, '@') || strchr (address, ' '))
    {
      gtk_widget_add_css_class (GTK_WIDGET (entry_row), "error");
      return;
    }

  gtk_widget_remove_css_class (GTK_WIDGET (entry_row), "error");
  emit_add_guest (self, address, (name && *name) ? name : NULL);
}

static void
pick_suggestion (GcalGuestList *self,
                 GtkListBoxRow *row)
{
  const gchar *address = g_object_get_data (G_OBJECT (row), "address");
  const gchar *name = g_object_get_data (G_OBJECT (row), "name");

  if (address)
    emit_add_guest (self, address, name);
}

static void
on_suggestion_activated_cb (GtkListBox    *list,
                            GtkListBoxRow *row,
                            GcalGuestList *self)
{
  pick_suggestion (self, row);
}

static GtkWidget *
create_suggestion_row (GcalContactMatch *match)
{
  const gchar *icon_name;
  const gchar *tooltip;
  GtkWidget *image;
  GtkWidget *row;

  row = adw_action_row_new ();
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), match->name ? match->name : match->address);
  if (match->name)
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), match->address);

  switch (match->origin)
    {
    case GCAL_CONTACT_ORIGIN_ADDRESS_BOOK:
      icon_name = "x-office-address-book-symbolic";
      tooltip = _("From your contacts");
      break;

    case GCAL_CONTACT_ORIGIN_DIRECTORY:
      icon_name = "system-users-symbolic";
      tooltip = _("From the directory");
      break;

    case GCAL_CONTACT_ORIGIN_MAIL_HISTORY:
    default:
      icon_name = "mail-unread-symbolic";
      tooltip = _("From your mail");
      break;
    }

  image = gtk_image_new_from_icon_name (icon_name);
  gtk_widget_add_css_class (image, "dimmed");
  gtk_widget_set_tooltip_text (image, tooltip);
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), image);

  g_object_set_data_full (G_OBJECT (row), "address", g_strdup (match->address), g_free);
  g_object_set_data_full (G_OBJECT (row), "name", g_strdup (match->name), g_free);

  return row;
}

static void
show_suggestions (GcalGuestList *self)
{
  graphene_rect_t bounds;
  GdkRectangle rect;

  if (!self->add_row || !gtk_widget_get_mapped (self->add_row))
    return;

  if (!gtk_widget_compute_bounds (self->add_row, GTK_WIDGET (self), &bounds))
    return;

  rect.x = (gint) bounds.origin.x;
  rect.y = (gint) bounds.origin.y;
  rect.width = (gint) bounds.size.width;
  rect.height = (gint) bounds.size.height;

  gtk_popover_set_pointing_to (GTK_POPOVER (self->popover), &rect);
  gtk_widget_set_size_request (gtk_popover_get_child (GTK_POPOVER (self->popover)), rect.width, -1);
  gtk_popover_popup (GTK_POPOVER (self->popover));
}

static void
on_search_finished_cb (GObject      *source_object,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  g_autoptr (GcalGuestList) self = user_data;
  g_autoptr (GPtrArray) matches = NULL;
  g_autoptr (GError) error = NULL;
  const gchar *text;
  guint shown = 0;

  matches = gcal_contact_search_finish (result, &error);
  if (!matches)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Guest suggestions failed: %s", error ? error->message : "unknown error");
      return;
    }

  if (!self->add_row)
    return;

  gtk_list_box_remove_all (self->suggestions);

  for (guint i = 0; i < matches->len && shown < MAX_SUGGESTIONS; i++)
    {
      GcalContactMatch *match = g_ptr_array_index (matches, i);
      g_autofree gchar *key = g_ascii_strdown (match->address, -1);

      if (self->excluded && g_hash_table_contains (self->excluded, key))
        continue;

      gtk_list_box_append (self->suggestions, create_suggestion_row (match));
      shown++;
    }

  if (shown == 0)
    {
      gtk_popover_popdown (GTK_POPOVER (self->popover));
      return;
    }

  /* Preselect the best match, unless a full address is being typed */
  text = gtk_editable_get_text (GTK_EDITABLE (self->add_row));
  if (!strchr (text, '@'))
    gtk_list_box_select_row (self->suggestions, gtk_list_box_get_row_at_index (self->suggestions, 0));

  show_suggestions (self);
}

static gboolean
start_search_cb (gpointer user_data)
{
  GcalGuestList *self = user_data;
  GcalManager *manager;
  const gchar *text;

  self->search_timeout_id = 0;

  if (!self->add_row)
    return G_SOURCE_REMOVE;

  text = gtk_editable_get_text (GTK_EDITABLE (self->add_row));
  manager = gcal_context_get_manager (gcal_application_get_context (GCAL_DEFAULT_APPLICATION));

  g_cancellable_cancel (self->search_cancellable);
  g_clear_object (&self->search_cancellable);
  self->search_cancellable = g_cancellable_new ();

  gcal_contact_search (manager,
                       text,
                       MAX_SUGGESTIONS * 2,
                       self->search_cancellable,
                       on_search_finished_cb,
                       g_object_ref (self));

  return G_SOURCE_REMOVE;
}

static void
on_add_row_text_changed_cb (GtkEditable   *editable,
                            GcalGuestList *self)
{
  const gchar *text = gtk_editable_get_text (editable);

  gtk_widget_remove_css_class (GTK_WIDGET (editable), "error");
  g_clear_handle_id (&self->search_timeout_id, g_source_remove);

  if (g_utf8_strlen (text, -1) < 2)
    {
      hide_suggestions (self);
      return;
    }

  self->search_timeout_id = g_timeout_add (SEARCH_DELAY_MS, start_search_cb, self);
}

static void
move_selection (GcalGuestList *self,
                gint           delta)
{
  GtkListBoxRow *selected = gtk_list_box_get_selected_row (self->suggestions);
  GtkListBoxRow *row;
  gint index;

  index = selected ? gtk_list_box_row_get_index (selected) + delta : (delta > 0 ? 0 : -1);
  if (index < 0)
    {
      gtk_list_box_unselect_all (self->suggestions);
      return;
    }

  row = gtk_list_box_get_row_at_index (self->suggestions, index);
  if (row)
    gtk_list_box_select_row (self->suggestions, row);
}

static gboolean
on_add_row_key_pressed_cb (GtkEventControllerKey *controller,
                           guint                  keyval,
                           guint                  keycode,
                           GdkModifierType        state,
                           GcalGuestList         *self)
{
  GtkListBoxRow *selected;

  if (!gtk_widget_get_visible (self->popover))
    return GDK_EVENT_PROPAGATE;

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
      hide_suggestions (self);
      return GDK_EVENT_STOP;

    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_ISO_Enter:
    case GDK_KEY_Tab:
      selected = gtk_list_box_get_selected_row (self->suggestions);
      if (selected)
        {
          pick_suggestion (self, selected);
          return GDK_EVENT_STOP;
        }
      return GDK_EVENT_PROPAGATE;

    default:
      return GDK_EVENT_PROPAGATE;
    }
}

static GtkWidget *
create_add_row (GcalGuestList *self)
{
  GtkEventController *controller;
  GtkWidget *row;

  row = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), _("Add Guest"));
  adw_entry_row_set_show_apply_button (ADW_ENTRY_ROW (row), TRUE);
  adw_entry_row_set_input_purpose (ADW_ENTRY_ROW (row), GTK_INPUT_PURPOSE_EMAIL);
  gtk_widget_set_tooltip_text (row, _("Type a name or an email address"));

  g_signal_connect (row, "apply", G_CALLBACK (on_add_guest_cb), self);
  g_signal_connect (row, "entry-activated", G_CALLBACK (on_add_guest_cb), self);
  g_signal_connect (row, "changed", G_CALLBACK (on_add_row_text_changed_cb), self);

  controller = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
  g_signal_connect (controller, "key-pressed", G_CALLBACK (on_add_row_key_pressed_cb), self);
  gtk_widget_add_controller (row, controller);

  return row;
}

static GtkWidget *
create_row (GcalGuestList *self,
            const Guest   *guest)
{
  g_autoptr (GPtrArray) tags = NULL;
  const gchar *icon_name, *css_class, *tooltip;
  const gchar *title;
  GtkWidget *image;
  GtkWidget *row;

  row = adw_action_row_new ();
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);

  title = (guest->name && *guest->name) ? guest->name : guest->address;
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title ? title : _("Unknown"));

  if (guest->address && g_strcmp0 (title, guest->address) != 0)
    {
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), guest->address);
      adw_action_row_set_subtitle_selectable (ADW_ACTION_ROW (row), TRUE);
    }

  status_icon (guest, &icon_name, &css_class, &tooltip);
  image = gtk_image_new_from_icon_name (icon_name);
  gtk_widget_add_css_class (image, css_class);
  gtk_widget_set_tooltip_text (image, tooltip);
  gtk_accessible_update_property (GTK_ACCESSIBLE (image),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, tooltip,
                                  -1);
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), image);

  tags = g_ptr_array_new ();
  if (guest->is_organizer)
    g_ptr_array_add (tags, (gpointer) _("Organizer"));
  if (guest->is_user)
    g_ptr_array_add (tags, (gpointer) _("You"));
  if (guest->is_optional)
    g_ptr_array_add (tags, (gpointer) _("Optional"));
  if (guest->type == GCAL_EVENT_ATTENDEE_TYPE_ROOM)
    g_ptr_array_add (tags, (gpointer) _("Room"));
  else if (guest->type == GCAL_EVENT_ATTENDEE_TYPE_RESOURCE)
    g_ptr_array_add (tags, (gpointer) _("Resource"));

  if (tags->len > 0)
    {
      g_autofree gchar *text = NULL;
      GtkWidget *label;

      g_ptr_array_add (tags, NULL);
      text = g_strjoinv (" · ", (gchar **) tags->pdata);

      label = gtk_label_new (text);
      gtk_widget_add_css_class (label, "dimmed");
      gtk_widget_add_css_class (label, "caption");
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), label);
    }

  /* evolution-ews never sends an empty guest list to Exchange, so the last
   * guest of a saved meeting cannot be removed (delete the event instead) */
  if (self->editable && !guest->is_organizer && guest->address &&
      !(self->keep_last_guest && self->n_removable <= 1))
    {
      GtkWidget *button;

      button = gtk_button_new_from_icon_name ("window-close-symbolic");
      gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
      gtk_widget_set_tooltip_text (button, _("Remove Guest"));
      gtk_widget_add_css_class (button, "flat");
      gtk_widget_add_css_class (button, "circular");
      g_object_set_data_full (G_OBJECT (button), "guest-address", g_strdup (guest->address), g_free);
      g_signal_connect (button, "clicked", G_CALLBACK (on_remove_button_clicked_cb), self);
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), button);
    }

  return row;
}

static void
set_counts (GcalGuestList *self,
            guint          n_guests,
            const gchar   *summary)
{
  if (self->n_guests != n_guests)
    {
      self->n_guests = n_guests;
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_N_GUESTS]);
    }

  if (g_strcmp0 (self->summary, summary) != 0)
    {
      g_free (self->summary);
      self->summary = g_strdup (summary);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_SUMMARY]);
    }
}

static gchar *
build_summary (GPtrArray *guests)
{
  g_autoptr (GPtrArray) parts = NULL;
  guint accepted = 0, tentative = 0, declined = 0, awaiting = 0;

  for (guint i = 0; i < guests->len; i++)
    {
      Guest *guest = g_ptr_array_index (guests, i);

      if (!guest->has_status)
        continue;

      switch (guest->status)
        {
        case GCAL_EVENT_ATTENDEE_PART_ACCEPTED:
          accepted++;
          break;

        case GCAL_EVENT_ATTENDEE_PART_TENTATIVE:
          tentative++;
          break;

        case GCAL_EVENT_ATTENDEE_PART_DECLINED:
          declined++;
          break;

        case GCAL_EVENT_ATTENDEE_PART_NEEDS_ACTION:
          awaiting++;
          break;

        default:
          break;
        }
    }

  parts = g_ptr_array_new_with_free_func (g_free);

  if (accepted > 0)
    g_ptr_array_add (parts, g_strdup_printf (g_dngettext (GETTEXT_PACKAGE, "%u accepted", "%u accepted", accepted), accepted));
  if (tentative > 0)
    g_ptr_array_add (parts, g_strdup_printf (g_dngettext (GETTEXT_PACKAGE, "%u maybe", "%u maybe", tentative), tentative));
  if (declined > 0)
    g_ptr_array_add (parts, g_strdup_printf (g_dngettext (GETTEXT_PACKAGE, "%u declined", "%u declined", declined), declined));
  if (awaiting > 0)
    g_ptr_array_add (parts, g_strdup_printf (g_dngettext (GETTEXT_PACKAGE, "%u awaiting", "%u awaiting", awaiting), awaiting));

  if (parts->len == 0)
    return NULL;

  g_ptr_array_add (parts, NULL);
  return g_strjoinv (", ", (gchar **) parts->pdata);
}


/*
 * GObject overrides
 */

static void
gcal_guest_list_size_allocate (GtkWidget *widget,
                               int        width,
                               int        height,
                               int        baseline)
{
  GcalGuestList *self = GCAL_GUEST_LIST (widget);

  GTK_WIDGET_CLASS (gcal_guest_list_parent_class)->size_allocate (widget, width, height, baseline);

  if (self->popover)
    gtk_popover_present (GTK_POPOVER (self->popover));
}

static void
gcal_guest_list_dispose (GObject *object)
{
  GcalGuestList *self = (GcalGuestList *) object;

  g_clear_handle_id (&self->search_timeout_id, g_source_remove);
  g_cancellable_cancel (self->search_cancellable);
  g_clear_object (&self->search_cancellable);
  g_clear_pointer (&self->popover, gtk_widget_unparent);
  self->suggestions = NULL;
  self->add_row = NULL;

  G_OBJECT_CLASS (gcal_guest_list_parent_class)->dispose (object);
}

static void
gcal_guest_list_finalize (GObject *object)
{
  GcalGuestList *self = (GcalGuestList *) object;

  g_clear_pointer (&self->excluded, g_hash_table_unref);

  g_clear_pointer (&self->summary, g_free);

  G_OBJECT_CLASS (gcal_guest_list_parent_class)->finalize (object);
}

static void
gcal_guest_list_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  GcalGuestList *self = GCAL_GUEST_LIST (object);

  switch (prop_id)
    {
    case PROP_N_GUESTS:
      g_value_set_uint (value, self->n_guests);
      break;

    case PROP_SUMMARY:
      g_value_set_string (value, self->summary);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gcal_guest_list_class_init (GcalGuestListClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gcal_guest_list_dispose;
  object_class->finalize = gcal_guest_list_finalize;

  GTK_WIDGET_CLASS (klass)->size_allocate = gcal_guest_list_size_allocate;
  object_class->get_property = gcal_guest_list_get_property;

  properties[PROP_N_GUESTS] = g_param_spec_uint ("n-guests", NULL, NULL,
                                                 0, G_MAXUINT, 0,
                                                 G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_SUMMARY] = g_param_spec_string ("summary", NULL, NULL,
                                                  NULL,
                                                  G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);

  /**
   * GcalGuestList::remove-guest:
   * @address: the guest's email address
   *
   * Emitted when the remove button of a guest is clicked (editable lists only).
   */
  /**
   * GcalGuestList::add-guest:
   * @address: the email address typed or picked in the "Add Guest" row
   * @name: (nullable): the guest's name, if known
   *
   * Emitted when a guest is added (editable lists only).
   */
  signals[ADD_GUEST] = g_signal_new ("add-guest",
                                     GCAL_TYPE_GUEST_LIST,
                                     G_SIGNAL_RUN_LAST,
                                     0, NULL, NULL, NULL,
                                     G_TYPE_NONE,
                                     2,
                                     G_TYPE_STRING,
                                     G_TYPE_STRING);

  signals[REMOVE_GUEST] = g_signal_new ("remove-guest",
                                        GCAL_TYPE_GUEST_LIST,
                                        G_SIGNAL_RUN_LAST,
                                        0, NULL, NULL, NULL,
                                        G_TYPE_NONE,
                                        1,
                                        G_TYPE_STRING);
}

static void
gcal_guest_list_init (GcalGuestList *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);

  self->list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (self->list, GTK_SELECTION_NONE);
  gtk_widget_add_css_class (GTK_WIDGET (self->list), "boxed-list");
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->list));

  /* Suggestions popover; does not take the focus away from the entry */
  {
    GtkWidget *scrolled;

    self->suggestions = GTK_LIST_BOX (gtk_list_box_new ());
    gtk_list_box_set_selection_mode (self->suggestions, GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click (self->suggestions, TRUE);
    gtk_widget_add_css_class (GTK_WIDGET (self->suggestions), "navigation-sidebar");
    g_signal_connect (self->suggestions, "row-activated", G_CALLBACK (on_suggestion_activated_cb), self);

    scrolled = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scrolled), TRUE);
    gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scrolled), 320);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), GTK_WIDGET (self->suggestions));

    self->popover = gtk_popover_new ();
    gtk_popover_set_child (GTK_POPOVER (self->popover), scrolled);
    gtk_popover_set_autohide (GTK_POPOVER (self->popover), FALSE);
    gtk_popover_set_has_arrow (GTK_POPOVER (self->popover), FALSE);
    gtk_popover_set_position (GTK_POPOVER (self->popover), GTK_POS_BOTTOM);
    gtk_widget_set_can_focus (self->popover, FALSE);
    gtk_widget_set_halign (self->popover, GTK_ALIGN_START);
    gtk_widget_set_parent (self->popover, GTK_WIDGET (self));
  }
}

GtkWidget*
gcal_guest_list_new (void)
{
  return g_object_new (GCAL_TYPE_GUEST_LIST, NULL);
}

/**
 * gcal_guest_list_set_event:
 * @self: a #GcalGuestList
 * @event: (nullable): a #GcalEvent
 *
 * Fills the list with the organizer and attendees of @event.
 */
void
gcal_guest_list_set_event (GcalGuestList *self,
                           GcalEvent     *event)
{
  g_autoptr (GHashTable) user_addresses = NULL;
  g_autoptr (GHashTable) seen = NULL;
  g_autoptr (GPtrArray) guests = NULL;
  g_autofree gchar *organizer_address = NULL;
  g_autofree gchar *summary = NULL;
  GcalEventOrganizer *organizer;
  GListModel *attendees;
  gboolean organizer_listed = FALSE;

  g_return_if_fail (GCAL_IS_GUEST_LIST (self));

  hide_suggestions (self);
  gtk_list_box_remove_all (self->list);
  self->add_row = NULL;
  g_clear_pointer (&self->excluded, g_hash_table_unref);

  if (!event)
    {
      set_counts (self, 0, NULL);
      return;
    }

  if (gcal_event_get_calendar (event))
    {
      GcalManager *manager = gcal_context_get_manager (gcal_application_get_context (GCAL_DEFAULT_APPLICATION));

      user_addresses = gcal_rsvp_dup_user_addresses (manager, gcal_event_get_calendar (event));
    }

  guests = g_ptr_array_new_with_free_func ((GDestroyNotify) guest_free);
  seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  organizer = gcal_event_get_organizer (event);
  if (organizer)
    organizer_address = dup_address (gcal_event_organizer_get_uri (organizer));

  attendees = gcal_event_get_attendees (event);
  for (guint i = 0; attendees && i < g_list_model_get_n_items (attendees); i++)
    {
      g_autoptr (GcalEventAttendee) attendee = g_list_model_get_item (attendees, i);
      g_autofree gchar *address = NULL;
      g_autofree gchar *key = NULL;
      Guest *guest;

      address = dup_address (gcal_event_attendee_get_uri (attendee));
      key = address ? g_ascii_strdown (address, -1) : g_strdup_printf ("#%u", i);

      if (g_hash_table_contains (seen, key))
        continue;

      guest = g_new0 (Guest, 1);
      guest->name = g_strdup (gcal_event_attendee_get_name (attendee));
      guest->address = g_steal_pointer (&address);
      guest->status = gcal_event_attendee_get_part_status (attendee);
      guest->has_status = TRUE;
      guest->type = gcal_event_attendee_get_attendee_type (attendee);
      guest->is_optional = gcal_event_attendee_get_role (attendee) == GCAL_EVENT_ATTENDEE_ROLE_OPTIONAL;
      guest->is_user = user_addresses && gcal_rsvp_is_user_address (user_addresses, guest->address);
      guest->is_organizer = organizer_address && guest->address &&
                            g_ascii_strcasecmp (organizer_address, guest->address) == 0;
      guest->index = i;

      organizer_listed |= guest->is_organizer;

      g_hash_table_add (seen, g_steal_pointer (&key));
      g_ptr_array_add (guests, guest);
    }

  /* The organizer is not always repeated among the attendees */
  if (organizer && !organizer_listed && (organizer_address || gcal_event_organizer_get_name (organizer)))
    {
      Guest *guest = g_new0 (Guest, 1);

      guest->name = g_strdup (gcal_event_organizer_get_name (organizer));
      guest->address = g_strdup (organizer_address);
      guest->is_organizer = TRUE;
      guest->is_user = user_addresses && gcal_rsvp_is_user_address (user_addresses, organizer_address);
      guest->type = GCAL_EVENT_ATTENDEE_TYPE_INDIVIDUAL;

      g_ptr_array_add (guests, guest);
    }

  g_ptr_array_sort (guests, compare_guests);

  self->n_removable = 0;
  for (guint i = 0; i < guests->len; i++)
    {
      Guest *guest = g_ptr_array_index (guests, i);

      if (!guest->is_organizer && guest->address)
        self->n_removable++;
    }

  /* Never suggest current guests or the user */
  self->excluded = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < guests->len; i++)
    {
      Guest *guest = g_ptr_array_index (guests, i);

      if (guest->address)
        g_hash_table_add (self->excluded, g_ascii_strdown (guest->address, -1));
    }
  if (user_addresses)
    {
      GHashTableIter iter;
      gpointer key;

      g_hash_table_iter_init (&iter, user_addresses);
      while (g_hash_table_iter_next (&iter, &key, NULL))
        g_hash_table_add (self->excluded, g_strdup (key));
    }

  for (guint i = 0; i < guests->len; i++)
    gtk_list_box_append (self->list, create_row (self, g_ptr_array_index (guests, i)));

  if (self->editable)
    {
      self->add_row = create_add_row (self);
      gtk_list_box_append (self->list, self->add_row);
    }

  summary = build_summary (guests);
  set_counts (self, guests->len, summary);
}

guint
gcal_guest_list_get_n_guests (GcalGuestList *self)
{
  g_return_val_if_fail (GCAL_IS_GUEST_LIST (self), 0);

  return self->n_guests;
}

const gchar*
gcal_guest_list_get_summary (GcalGuestList *self)
{
  g_return_val_if_fail (GCAL_IS_GUEST_LIST (self), NULL);

  return self->summary;
}

/**
 * gcal_guest_list_set_editable:
 * @self: a #GcalGuestList
 * @editable: whether guests can be removed
 *
 * Takes effect on the next gcal_guest_list_set_event().
 */
void
gcal_guest_list_set_editable (GcalGuestList *self,
                              gboolean       editable)
{
  g_return_if_fail (GCAL_IS_GUEST_LIST (self));

  self->editable = editable;
}

/**
 * gcal_guest_list_focus_add_row:
 * @self: a #GcalGuestList
 *
 * Moves the keyboard focus to the "Add Guest" row, if any.
 */
void
gcal_guest_list_focus_add_row (GcalGuestList *self)
{
  g_return_if_fail (GCAL_IS_GUEST_LIST (self));

  if (self->add_row)
    gtk_widget_grab_focus (self->add_row);
}

/**
 * gcal_guest_list_set_keep_last_guest:
 * @self: a #GcalGuestList
 * @keep_last_guest: whether the last guest must stay
 *
 * When set, the remove button is not offered on the only remaining guest.
 * Takes effect on the next gcal_guest_list_set_event().
 */
void
gcal_guest_list_set_keep_last_guest (GcalGuestList *self,
                                     gboolean       keep_last_guest)
{
  g_return_if_fail (GCAL_IS_GUEST_LIST (self));

  self->keep_last_guest = keep_last_guest;
}
