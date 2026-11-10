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
#include "gcal-rsvp.h"
#include "gcal-utils.h"
#include "glib-object.h"

struct _GcalAttendeesSection
{
  GcalEventEditorSection  parent_instance;

  GcalEventOrganizer  *organizer;

  AdwActionRow        *summary_row;

  GListModel          *attendees;

  /* Fork: reply to the invitation */
  AdwActionRow              *response_row;
  GtkWidget                 *response_buttons;
  GtkToggleButton           *accept_button;
  GtkToggleButton           *tentative_button;
  GtkToggleButton           *decline_button;
  GcalEvent                 *event;
  GcalEventAttendeePartStat  user_status;
  gboolean                   rsvp_requested;
  gboolean                   updating_buttons;
  GCancellable              *cancellable;
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
update_response_row (GcalAttendeesSection *self,
                     const gchar          *error_message)
{
  g_autofree gchar *subtitle = NULL;
  const gchar *status_text;
  const gchar *notify_text;

  self->updating_buttons = TRUE;

  gtk_toggle_button_set_active (self->accept_button, self->user_status == GCAL_EVENT_ATTENDEE_PART_ACCEPTED);
  gtk_toggle_button_set_active (self->tentative_button, self->user_status == GCAL_EVENT_ATTENDEE_PART_TENTATIVE);
  gtk_toggle_button_set_active (self->decline_button, self->user_status == GCAL_EVENT_ATTENDEE_PART_DECLINED);

  self->updating_buttons = FALSE;

  switch (self->user_status)
    {
    case GCAL_EVENT_ATTENDEE_PART_ACCEPTED:
      status_text = _("Accepted");
      break;

    case GCAL_EVENT_ATTENDEE_PART_TENTATIVE:
      status_text = _("Tentatively accepted");
      break;

    case GCAL_EVENT_ATTENDEE_PART_DECLINED:
      status_text = _("Declined");
      break;

    case GCAL_EVENT_ATTENDEE_PART_DELEGATED:
      status_text = _("Delegated");
      break;

    case GCAL_EVENT_ATTENDEE_PART_NEEDS_ACTION:
    default:
      status_text = _("Not answered yet");
      break;
    }

  notify_text = self->rsvp_requested ? _("The organizer will be notified") : _("No reply was requested");

  if (error_message)
    subtitle = g_strdup_printf (_("Could not send the response: %s"), error_message);
  else
    subtitle = g_strdup_printf ("%s · %s", status_text, notify_text);

  adw_action_row_set_subtitle (self->response_row, subtitle);
}

typedef struct
{
  GcalAttendeesSection      *self;
  GcalEventAttendeePartStat  status;
} ResponseCall;

static void
on_rsvp_respond_finished_cb (GObject      *source_object,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  ResponseCall *call = user_data;
  g_autoptr (GcalAttendeesSection) self = call->self;
  GcalEventAttendeePartStat status = call->status;
  g_autoptr (GError) error = NULL;

  g_free (call);

  if (!gcal_rsvp_respond_finish (result, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      g_warning ("Error replying to invitation: %s", error->message);
      gtk_widget_set_sensitive (self->response_buttons, TRUE);
      update_response_row (self, error->message);
      return;
    }

  self->user_status = status;
  gtk_widget_set_sensitive (self->response_buttons, TRUE);
  update_response_row (self, NULL);

  /* Exchange (and Outlook) remove a declined meeting from the calendar, so
   * there is nothing left to look at: close the editor like Outlook does. */
  if (status == GCAL_EVENT_ATTENDEE_PART_DECLINED)
    gtk_widget_activate_action (GTK_WIDGET (self), "event-editor.close", NULL);
}

static void
send_response (GcalAttendeesSection      *self,
               GcalEventAttendeePartStat  status,
               gboolean                   whole_series)
{
  ResponseCall *call;

  if (!self->event)
    return;

  call = g_new0 (ResponseCall, 1);
  call->self = g_object_ref (self);
  call->status = status;

  gtk_widget_set_sensitive (self->response_buttons, FALSE);
  adw_action_row_set_subtitle (self->response_row, _("Sending response…"));

  gcal_rsvp_respond (get_manager (),
                     self->event,
                     status,
                     whole_series,
                     TRUE,
                     self->cancellable,
                     on_rsvp_respond_finished_cb,
                     call);
}

typedef struct
{
  GcalAttendeesSection      *self;
  GcalEventAttendeePartStat  status;
} RecurrenceChoice;

static void
on_recurrence_choice_response_cb (AdwAlertDialog   *dialog,
                                  const gchar      *response,
                                  RecurrenceChoice *choice)
{
  g_autoptr (GcalAttendeesSection) self = choice->self;

  if (g_strcmp0 (response, "this-only") == 0)
    send_response (self, choice->status, FALSE);
  else if (g_strcmp0 (response, "all-events") == 0)
    send_response (self, choice->status, TRUE);
  else
    update_response_row (self, NULL); /* cancelled: restore the buttons */

  g_free (choice);
}

static gboolean
event_is_recurring (GcalEvent *event)
{
  ECalComponent *component = gcal_event_get_component (event);

  return e_cal_component_has_recurrences (component) || e_cal_component_is_instance (component);
}

static void
on_response_button_toggled_cb (GtkToggleButton      *button,
                               GcalAttendeesSection *self)
{
  GcalEventAttendeePartStat status;

  if (self->updating_buttons || !gtk_toggle_button_get_active (button))
    return;

  if (button == self->accept_button)
    status = GCAL_EVENT_ATTENDEE_PART_ACCEPTED;
  else if (button == self->tentative_button)
    status = GCAL_EVENT_ATTENDEE_PART_TENTATIVE;
  else
    status = GCAL_EVENT_ATTENDEE_PART_DECLINED;

  if (status == self->user_status)
    return;

  if (event_is_recurring (self->event))
    {
      RecurrenceChoice *choice;
      AdwDialog *dialog;

      dialog = adw_alert_dialog_new (_("Respond to Recurring Event?"),
                                     _("This event is part of a series. Your response should apply to:"));
      adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog),
                                      "cancel", _("_Cancel"),
                                      "this-only", _("_Only This Event"),
                                      "all-events", _("_All Events"),
                                      NULL);
      adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "this-only");
      adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");

      choice = g_new0 (RecurrenceChoice, 1);
      choice->self = g_object_ref (self);
      choice->status = status;

      g_signal_connect (dialog, "response", G_CALLBACK (on_recurrence_choice_response_cb), choice);
      adw_dialog_present (dialog, GTK_WIDGET (self));
      return;
    }

  send_response (self, status, FALSE);
}

static void
setup_response_row (GcalAttendeesSection *self,
                    GcalEvent            *event)
{
  gboolean can_respond = FALSE;

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_set_object (&self->event, event);

  if (event)
    {
      self->cancellable = g_cancellable_new ();
      can_respond = gcal_rsvp_get_user_status (get_manager (),
                                               event,
                                               &self->user_status,
                                               &self->rsvp_requested);
    }

  gtk_widget_set_visible (GTK_WIDGET (self->response_row), can_respond);
  gtk_widget_set_sensitive (self->response_buttons, TRUE);

  if (can_respond)
    update_response_row (self, NULL);
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

  setup_response_row (self, event);

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

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->event);

  G_OBJECT_CLASS (gcal_attendees_section_parent_class)->finalize (object);
}

static void
gcal_attendees_section_init (GcalAttendeesSection *instance)
{
  gtk_widget_init_template (GTK_WIDGET (instance));
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

  gtk_widget_class_bind_template_child (widget_class, GcalAttendeesSection, summary_row);
  gtk_widget_class_bind_template_child (widget_class, GcalAttendeesSection, response_row);
  gtk_widget_class_bind_template_child (widget_class, GcalAttendeesSection, response_buttons);
  gtk_widget_class_bind_template_child (widget_class, GcalAttendeesSection, accept_button);
  gtk_widget_class_bind_template_child (widget_class, GcalAttendeesSection, tentative_button);
  gtk_widget_class_bind_template_child (widget_class, GcalAttendeesSection, decline_button);

  gtk_widget_class_bind_template_callback (widget_class, on_response_button_toggled_cb);
}
