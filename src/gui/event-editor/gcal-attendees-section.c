/* gcal-attendees-section.c
 *
 * Copyright (C) 2025 Alen Galinec <mind.on.warp@gmail.com>
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

#include "gcal-attendees-section.h"

#include <adwaita.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <libintl.h>

#include "gcal-attendee-summary-row.h"
#include "gcal-event-attendee.h"
#include "gcal-event-organizer.h"
#include "gcal-event.h"
#include "gcal-organizer-row.h"
#include "gcal-application.h"
#include "gcal-context.h"
#include "gcal-guest-list.h"
#include "gcal-rsvp.h"
#include "gcal-rsvp-box.h"
#include "gcal-utils.h"
#include "glib-object.h"

struct _GcalAttendeesSection
{
  GcalEventEditorSection  parent_instance;

  GcalEventOrganizer  *organizer;


  GListModel          *attendees;

  /* Fork: reply to the invitation */
  AdwActionRow        *response_row;
  GcalRsvpBox         *rsvp_box;
  GcalGuestList       *guest_list;
  gboolean             had_guests;
};

G_DEFINE_FINAL_TYPE (GcalAttendeesSection, gcal_attendees_section, GCAL_TYPE_EVENT_EDITOR_SECTION)

enum
{
  PROP_0,
  PROP_ATTENDEES,
  PROP_ORGANIZER,
  N_PROPS
};

static GParamSpec *properties[N_PROPS] = { NULL, };


/*
 * Fork: reply to the invitation
 */

static GcalManager *
get_manager (void)
{
  return gcal_context_get_manager (gcal_application_get_context (GCAL_DEFAULT_APPLICATION));
}

static void
refresh_guests (GcalAttendeesSection *self)
{
  GcalEvent *event;
  gboolean editable = FALSE;

  event = gcal_event_editor_section_get_event (GCAL_EVENT_EDITOR_SECTION (self));

  if (event)
    editable = gcal_rsvp_can_edit_guests (get_manager (), event);

  gcal_guest_list_set_editable (self->guest_list, editable);
  gcal_guest_list_set_keep_last_guest (self->guest_list, self->had_guests);
  gcal_guest_list_set_event (self->guest_list, event);

  gtk_widget_set_visible (GTK_WIDGET (self),
                          editable ||
                          gcal_guest_list_get_n_guests (self->guest_list) > 0 ||
                          gcal_rsvp_box_get_can_respond (self->rsvp_box));
}

static void
on_add_guest_cb (GcalGuestList        *guest_list,
                 const gchar          *address,
                 const gchar          *name,
                 GcalAttendeesSection *self)
{
  GcalEvent *event = gcal_event_editor_section_get_event (GCAL_EVENT_EDITOR_SECTION (self));

  if (!event)
    return;

  gcal_rsvp_add_guest (get_manager (), event, address, name);
  refresh_guests (self);
  gcal_guest_list_focus_add_row (self->guest_list);
}

static void
on_remove_guest_cb (GcalGuestList        *guest_list,
                    const gchar          *address,
                    GcalAttendeesSection *self)
{
  GcalEvent *event = gcal_event_editor_section_get_event (GCAL_EVENT_EDITOR_SECTION (self));

  if (!event)
    return;

  gcal_rsvp_remove_guest (get_manager (), event, address);
  refresh_guests (self);
}

static void
on_rsvp_responded_cb (GcalRsvpBox          *rsvp_box,
                      gint                  status,
                      GcalAttendeesSection *self)
{
  /* Exchange (and Outlook) remove a declined meeting from the calendar, so
   * there is nothing left to look at: close the editor like Outlook does. */
  if (status == GCAL_EVENT_ATTENDEE_PART_DECLINED)
    gtk_widget_activate_action (GTK_WIDGET (self), "event-editor.close", NULL);
}

/*
 * GcalEventEditorSection overrides
 */

static void
gcal_attendees_section_event_set_cb (GcalEventEditorSection *section,
                                     GcalEvent              *event)
{
  GcalAttendeesSection *self = GCAL_ATTENDEES_SECTION (section);

  if (event)
    {
      self->organizer = gcal_event_get_organizer (event);
      self->attendees = gcal_event_get_attendees (event);
    }
  else
    {
      self->organizer = NULL;
      self->attendees = NULL;
    }

  /* A meeting that already had guests is on the server: its last guest
   * cannot be removed through evolution-ews */
  self->had_guests = event && e_cal_component_has_attendees (gcal_event_get_component (event));

  gcal_rsvp_box_set_event (self->rsvp_box, event);
  refresh_guests (self);

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ATTENDEES]);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ORGANIZER]);
}


/*
 * GObject overrides
 */

static void
gcal_attendees_section_get_property (GObject *object,
                                     guint property_id,
                                     GValue *value,
                                     GParamSpec *pspec)
{
  GcalAttendeesSection *self = GCAL_ATTENDEES_SECTION (object);

  switch (property_id)
    {
    case PROP_ATTENDEES:
      g_value_set_object (value, self->attendees);
      break;

    case PROP_ORGANIZER:
      g_value_set_object (value, self->organizer);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
gcal_attendees_section_set_property (GObject *object,
                                     guint property_id,
                                     const GValue *value,
                                     GParamSpec *pspec)
{
  GcalAttendeesSection *self = GCAL_ATTENDEES_SECTION (object);

  switch (property_id)
    {
    case PROP_ORGANIZER:
      self->organizer = g_value_get_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
gcal_attendees_section_finalize (GObject *object)
{
  GcalAttendeesSection *self = (GcalAttendeesSection *) object;

  self->attendees = NULL; /* not owned */

  G_OBJECT_CLASS (gcal_attendees_section_parent_class)->finalize (object);
}

static void
gcal_attendees_section_init (GcalAttendeesSection *instance)
{
  gtk_widget_init_template (GTK_WIDGET (instance));

  /* Fork: reply to the invitation */
  g_object_bind_property (instance->rsvp_box, "can-respond", instance->response_row, "visible", G_BINDING_SYNC_CREATE);
  g_object_bind_property (instance->rsvp_box, "status-text", instance->response_row, "subtitle", G_BINDING_SYNC_CREATE);
}

static void
gcal_attendees_section_class_init (GcalAttendeesSectionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GcalEventEditorSectionClass *section_class = GCAL_EVENT_EDITOR_SECTION_CLASS (klass);

  g_type_ensure (GCAL_TYPE_ORGANIZER_ROW);
  g_type_ensure (GCAL_TYPE_ATTENDEE_SUMMARY_ROW);

  object_class->finalize = gcal_attendees_section_finalize;
  object_class->get_property = gcal_attendees_section_get_property;
  object_class->set_property = gcal_attendees_section_set_property;

  section_class->event_set_cb = gcal_attendees_section_event_set_cb;

  /**
   * GcalAttendeesSection:attendees:
   *
   * ListStore of #GcalEventAttendee.
   * Passed to the summary rows #GcalAttendeeSummaryRow.
   */
  properties[PROP_ATTENDEES] =
    g_param_spec_object ("attendees", NULL, NULL,
                         G_TYPE_LIST_MODEL,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * GcalAttendeesSection:organizer:
   *
   * Organizer of the event.
   * #GcalOrganizerRow binds to this property.
   */
  properties[PROP_ORGANIZER] =
    g_param_spec_object ("organizer", NULL, NULL,
                         GCAL_TYPE_EVENT_ORGANIZER,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/calendar/ui/event-editor/gcal-attendees-section.ui");

  g_type_ensure (GCAL_TYPE_RSVP_BOX);
  g_type_ensure (GCAL_TYPE_GUEST_LIST);

  gtk_widget_class_bind_template_child (widget_class, GcalAttendeesSection, response_row);
  gtk_widget_class_bind_template_child (widget_class, GcalAttendeesSection, rsvp_box);
  gtk_widget_class_bind_template_child (widget_class, GcalAttendeesSection, guest_list);

  gtk_widget_class_bind_template_callback (widget_class, on_rsvp_responded_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_add_guest_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_remove_guest_cb);
}

/**
 * gcal_attendees_section_refresh:
 * @self: a #GcalAttendeesSection
 *
 * Fork addition. Re-evaluates whether guests can be edited, e.g. after the
 * calendar of a new event changed.
 */
void
gcal_attendees_section_refresh (GcalAttendeesSection *self)
{
  g_return_if_fail (GCAL_IS_ATTENDEES_SECTION (self));

  refresh_guests (self);
}
