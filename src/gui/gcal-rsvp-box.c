/* gcal-rsvp-box.c
 *
 * Local fork addition (not upstream): Accept / Maybe / Decline buttons.
 *
 * Linked toggle buttons that reply to a meeting invitation through
 * gcal_rsvp_respond(). Shared by the event popover and the event editor.
 *
 *  - #GcalRsvpBox:can-respond tells whether the user is an attendee (and not
 *    the organizer) of the event, i.e. whether the box should be shown;
 *  - #GcalRsvpBox:status-text is a one-line description of the current reply
 *    ("Accepted · The organizer will be notified"), or of an error;
 *  - #GcalRsvpBox::responded is emitted after a reply went through.
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

#define G_LOG_DOMAIN "GcalRsvpBox"

#include "gcal-rsvp-box.h"

#include <adwaita.h>
#include <glib/gi18n.h>

#include "gcal-application.h"
#include "gcal-context.h"
#include "gcal-rsvp.h"
#include "gcal-utils.h"

struct _GcalRsvpBox
{
  GtkBox                     parent_instance;

  GtkToggleButton           *accept_button;
  GtkToggleButton           *tentative_button;
  GtkToggleButton           *decline_button;

  GcalEvent                 *event;
  GcalEventAttendeePartStat  user_status;
  gboolean                   rsvp_requested;
  gboolean                   can_respond;
  gboolean                   updating_buttons;
  gchar                     *status_text;
  GCancellable              *cancellable;
};

G_DEFINE_FINAL_TYPE (GcalRsvpBox, gcal_rsvp_box, GTK_TYPE_BOX)

enum
{
  PROP_0,
  PROP_CAN_RESPOND,
  PROP_STATUS_TEXT,
  N_PROPS
};

enum
{
  RESPONDED,
  N_SIGNALS
};

static GParamSpec *properties[N_PROPS] = { NULL, };
static guint signals[N_SIGNALS] = { 0, };

typedef struct
{
  GcalRsvpBox               *self;
  GcalEventAttendeePartStat  status;
} PendingResponse;


/*
 * Auxiliary methods
 */

static GcalManager *
get_manager (void)
{
  return gcal_context_get_manager (gcal_application_get_context (GCAL_DEFAULT_APPLICATION));
}

static void
set_status_text (GcalRsvpBox *self,
                 const gchar *text)
{
  if (g_strcmp0 (self->status_text, text) == 0)
    return;

  g_free (self->status_text);
  self->status_text = g_strdup (text);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_STATUS_TEXT]);
}

static void
update_state (GcalRsvpBox *self,
              const gchar *error_message)
{
  g_autofree gchar *text = NULL;
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
    text = g_strdup_printf (_("Could not send the response: %s"), error_message);
  else
    text = g_strdup_printf ("%s · %s", status_text, notify_text);

  set_status_text (self, text);
}

static void
on_respond_finished_cb (GObject      *source_object,
                        GAsyncResult *result,
                        gpointer      user_data)
{
  PendingResponse *pending = user_data;
  g_autoptr (GcalRsvpBox) self = pending->self;
  GcalEventAttendeePartStat status = pending->status;
  g_autoptr (GError) error = NULL;

  g_free (pending);

  if (!gcal_rsvp_respond_finish (result, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      g_warning ("Error replying to invitation: %s", error->message);
      gtk_widget_set_sensitive (GTK_WIDGET (self), TRUE);
      update_state (self, error->message);
      return;
    }

  self->user_status = status;
  gtk_widget_set_sensitive (GTK_WIDGET (self), TRUE);
  update_state (self, NULL);

  g_signal_emit (self, signals[RESPONDED], 0, status);
}

static void
send_response (GcalRsvpBox               *self,
               GcalEventAttendeePartStat  status,
               gboolean                   whole_series)
{
  PendingResponse *pending;

  if (!self->event)
    return;

  pending = g_new0 (PendingResponse, 1);
  pending->self = g_object_ref (self);
  pending->status = status;

  gtk_widget_set_sensitive (GTK_WIDGET (self), FALSE);
  set_status_text (self, _("Sending response…"));

  gcal_rsvp_respond (get_manager (),
                     self->event,
                     status,
                     whole_series,
                     TRUE,
                     self->cancellable,
                     on_respond_finished_cb,
                     pending);
}

static void
on_recurrence_choice_response_cb (AdwAlertDialog  *dialog,
                                  const gchar     *response,
                                  PendingResponse *choice)
{
  g_autoptr (GcalRsvpBox) self = choice->self;
  GcalEventAttendeePartStat status = choice->status;

  g_free (choice);

  if (g_strcmp0 (response, "this-only") == 0)
    send_response (self, status, FALSE);
  else if (g_strcmp0 (response, "all-events") == 0)
    send_response (self, status, TRUE);
  else
    update_state (self, NULL); /* cancelled: restore the buttons */
}

static gboolean
event_is_recurring (GcalEvent *event)
{
  ECalComponent *component = gcal_event_get_component (event);

  return e_cal_component_has_recurrences (component) || e_cal_component_is_instance (component);
}

static void
on_button_toggled_cb (GtkToggleButton *button,
                      GcalRsvpBox     *self)
{
  GcalEventAttendeePartStat status;

  if (self->updating_buttons || !gtk_toggle_button_get_active (button) || !self->event)
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
      PendingResponse *choice;
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

      choice = g_new0 (PendingResponse, 1);
      choice->self = g_object_ref (self);
      choice->status = status;

      g_signal_connect (dialog, "response", G_CALLBACK (on_recurrence_choice_response_cb), choice);
      adw_dialog_present (dialog, GTK_WIDGET (self));
      return;
    }

  send_response (self, status, FALSE);
}

static GtkToggleButton *
add_button (GcalRsvpBox     *self,
            const gchar     *label,
            GtkToggleButton *group)
{
  GtkWidget *button;

  button = gtk_toggle_button_new_with_mnemonic (label);
  gtk_widget_set_hexpand (button, TRUE);

  if (group)
    gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (button), group);

  g_signal_connect (button, "toggled", G_CALLBACK (on_button_toggled_cb), self);
  gtk_box_append (GTK_BOX (self), button);

  return GTK_TOGGLE_BUTTON (button);
}


/*
 * GObject overrides
 */

static void
gcal_rsvp_box_dispose (GObject *object)
{
  GcalRsvpBox *self = (GcalRsvpBox *) object;

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->event);

  G_OBJECT_CLASS (gcal_rsvp_box_parent_class)->dispose (object);
}

static void
gcal_rsvp_box_finalize (GObject *object)
{
  GcalRsvpBox *self = (GcalRsvpBox *) object;

  g_clear_pointer (&self->status_text, g_free);

  G_OBJECT_CLASS (gcal_rsvp_box_parent_class)->finalize (object);
}

static void
gcal_rsvp_box_get_property (GObject    *object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  GcalRsvpBox *self = GCAL_RSVP_BOX (object);

  switch (prop_id)
    {
    case PROP_CAN_RESPOND:
      g_value_set_boolean (value, self->can_respond);
      break;

    case PROP_STATUS_TEXT:
      g_value_set_string (value, self->status_text);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gcal_rsvp_box_class_init (GcalRsvpBoxClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gcal_rsvp_box_dispose;
  object_class->finalize = gcal_rsvp_box_finalize;
  object_class->get_property = gcal_rsvp_box_get_property;

  properties[PROP_CAN_RESPOND] = g_param_spec_boolean ("can-respond", NULL, NULL,
                                                       FALSE,
                                                       G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_STATUS_TEXT] = g_param_spec_string ("status-text", NULL, NULL,
                                                      NULL,
                                                      G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);

  /**
   * GcalRsvpBox::responded:
   * @status: the #GcalEventAttendeePartStat that was sent
   *
   * Emitted after a reply was successfully sent.
   */
  signals[RESPONDED] = g_signal_new ("responded",
                                     GCAL_TYPE_RSVP_BOX,
                                     G_SIGNAL_RUN_LAST,
                                     0, NULL, NULL, NULL,
                                     G_TYPE_NONE,
                                     1,
                                     G_TYPE_INT);
}

static void
gcal_rsvp_box_init (GcalRsvpBox *self)
{
  gtk_widget_add_css_class (GTK_WIDGET (self), "linked");
  gtk_widget_set_valign (GTK_WIDGET (self), GTK_ALIGN_CENTER);
  gtk_box_set_homogeneous (GTK_BOX (self), TRUE);

  self->accept_button = add_button (self, _("_Accept"), NULL);
  self->tentative_button = add_button (self, _("_Maybe"), self->accept_button);
  self->decline_button = add_button (self, _("_Decline"), self->accept_button);
}

GtkWidget*
gcal_rsvp_box_new (void)
{
  return g_object_new (GCAL_TYPE_RSVP_BOX, NULL);
}

/**
 * gcal_rsvp_box_set_event:
 * @self: a #GcalRsvpBox
 * @event: (nullable): a #GcalEvent
 *
 * Sets the event to reply to, and updates #GcalRsvpBox:can-respond.
 */
void
gcal_rsvp_box_set_event (GcalRsvpBox *self,
                         GcalEvent   *event)
{
  gboolean can_respond = FALSE;

  g_return_if_fail (GCAL_IS_RSVP_BOX (self));

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

  gtk_widget_set_sensitive (GTK_WIDGET (self), TRUE);

  if (can_respond)
    update_state (self, NULL);
  else
    set_status_text (self, NULL);

  if (self->can_respond != can_respond)
    {
      self->can_respond = can_respond;
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_CAN_RESPOND]);
    }
}

gboolean
gcal_rsvp_box_get_can_respond (GcalRsvpBox *self)
{
  g_return_val_if_fail (GCAL_IS_RSVP_BOX (self), FALSE);

  return self->can_respond;
}

const gchar*
gcal_rsvp_box_get_status_text (GcalRsvpBox *self)
{
  g_return_val_if_fail (GCAL_IS_RSVP_BOX (self), NULL);

  return self->status_text;
}
