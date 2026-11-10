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
#include "gcal-context.h"
#include "gcal-event-attendee.h"
#include "gcal-event-organizer.h"
#include "gcal-rsvp.h"
#include "gcal-utils.h"

struct _GcalGuestList
{
  GtkBox              parent_instance;

  GtkListBox         *list;
  guint               n_guests;
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

static GParamSpec *properties[N_PROPS] = { NULL, };

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

static GtkWidget *
create_row (const Guest *guest)
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
gcal_guest_list_finalize (GObject *object)
{
  GcalGuestList *self = (GcalGuestList *) object;

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

  object_class->finalize = gcal_guest_list_finalize;
  object_class->get_property = gcal_guest_list_get_property;

  properties[PROP_N_GUESTS] = g_param_spec_uint ("n-guests", NULL, NULL,
                                                 0, G_MAXUINT, 0,
                                                 G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_SUMMARY] = g_param_spec_string ("summary", NULL, NULL,
                                                  NULL,
                                                  G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
gcal_guest_list_init (GcalGuestList *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);

  self->list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (self->list, GTK_SELECTION_NONE);
  gtk_widget_add_css_class (GTK_WIDGET (self->list), "boxed-list");
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->list));
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

  gtk_list_box_remove_all (self->list);

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

  for (guint i = 0; i < guests->len; i++)
    gtk_list_box_append (self->list, create_row (g_ptr_array_index (guests, i)));

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
