/* gcal-rsvp.c
 *
 * Local fork addition (not upstream): reply to meeting invitations.
 *
 * A reply is sent the way Evolution does it: the event, with the user's own
 * ATTENDEE PARTSTAT changed, is wrapped in a METHOD:REQUEST VCALENDAR and
 * handed to e_cal_client_receive_objects(). Backends that schedule on the
 * server turn that into a real response; evolution-ews, for instance, issues
 * Exchange AcceptItem / TentativelyAcceptItem / DeclineItem, which emails the
 * organizer when the invitation asked for replies (RSVP=TRUE) unless
 * E_CAL_OPERATION_FLAG_DISABLE_ITIP_MESSAGE is passed.
 *
 * A plain e_cal_client_modify_object() is NOT enough: on EWS, an attendee's
 * modification is saved with SendToNone and the organizer never hears of it.
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

#define G_LOG_DOMAIN "GcalRsvp"

#include "gcal-rsvp.h"

#include "gcal-calendar.h"
#include "gcal-debug.h"

/* libical-glib ships no autoptr definitions */
G_DEFINE_AUTOPTR_CLEANUP_FUNC (ICalComponent, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (ICalProperty, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (ICalParameter, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (ECalComponent, g_object_unref)

typedef struct
{
  GcalCalendar          *calendar;
  ICalComponent         *icomp;
  GHashTable            *user_addresses;
  ICalParameterPartstat  partstat;
  gboolean               whole_series;
  gboolean               notify_organizer;
} RespondData;

static void
respond_data_free (RespondData *data)
{
  g_clear_object (&data->calendar);
  g_clear_object (&data->icomp);
  g_clear_pointer (&data->user_addresses, g_hash_table_unref);
  g_free (data);
}

static void
add_address (GHashTable  *addresses,
             const gchar *address)
{
  address = e_cal_util_strip_mailto (address);

  if (address && *address)
    g_hash_table_add (addresses, g_ascii_strdown (address, -1));
}

/*
 * The user's addresses: the one the calendar backend reports for this calendar
 * (cal-email-address, e.g. the Exchange account address), plus every mail
 * identity address and alias.
 */
GHashTable *
gcal_rsvp_dup_user_addresses (GcalManager  *manager,
                              GcalCalendar *calendar)
{
  g_autolist (ESource) identities = NULL;
  ESourceRegistry *registry;
  GHashTable *addresses;

  addresses = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  add_address (addresses, gcal_calendar_get_user_email (calendar));

  registry = gcal_manager_get_source_registry (manager);
  if (!registry)
    return addresses;

  identities = e_source_registry_list_enabled (registry, E_SOURCE_EXTENSION_MAIL_IDENTITY);

  for (GList *l = identities; l; l = l->next)
    {
      g_autoptr (GHashTable) aliases = NULL;
      ESourceMailIdentity *identity;
      GHashTableIter iter;
      gpointer key;

      identity = e_source_get_extension (l->data, E_SOURCE_EXTENSION_MAIL_IDENTITY);
      add_address (addresses, e_source_mail_identity_get_address (identity));

      aliases = e_source_mail_identity_get_aliases_as_hash_table (identity);
      if (!aliases)
        continue;

      g_hash_table_iter_init (&iter, aliases);
      while (g_hash_table_iter_next (&iter, &key, NULL))
        add_address (addresses, key);
    }

  return addresses;
}

gboolean
gcal_rsvp_is_user_address (GHashTable  *addresses,
                           const gchar *address)
{
  g_autofree gchar *lower = NULL;

  address = e_cal_util_strip_mailto (address);
  if (!address || !*address)
    return FALSE;

  lower = g_ascii_strdown (address, -1);

  return g_hash_table_contains (addresses, lower);
}

static gboolean
user_is_organizer (ICalComponent *icomp,
                   GHashTable    *addresses)
{
  g_autoptr (ICalProperty) organizer = NULL;

  organizer = i_cal_component_get_first_property (icomp, I_CAL_ORGANIZER_PROPERTY);

  return organizer && gcal_rsvp_is_user_address (addresses, i_cal_property_get_organizer (organizer));
}

/* Returns: (transfer full) (nullable): the user's ATTENDEE property of @icomp */
static ICalProperty *
find_user_attendee (ICalComponent *icomp,
                    GHashTable    *addresses)
{
  ICalProperty *prop;

  for (prop = i_cal_component_get_first_property (icomp, I_CAL_ATTENDEE_PROPERTY);
       prop;
       g_object_unref (prop), prop = i_cal_component_get_next_property (icomp, I_CAL_ATTENDEE_PROPERTY))
    {
      if (gcal_rsvp_is_user_address (addresses, i_cal_property_get_attendee (prop)))
        return prop;
    }

  return NULL;
}

static GcalEventAttendeePartStat
partstat_from_ical (ICalProperty *attendee)
{
  g_autoptr (ICalParameter) param = NULL;

  param = i_cal_property_get_first_parameter (attendee, I_CAL_PARTSTAT_PARAMETER);
  if (!param)
    return GCAL_EVENT_ATTENDEE_PART_NEEDS_ACTION;

  switch (i_cal_parameter_get_partstat (param))
    {
    case I_CAL_PARTSTAT_ACCEPTED:
      return GCAL_EVENT_ATTENDEE_PART_ACCEPTED;

    case I_CAL_PARTSTAT_DECLINED:
      return GCAL_EVENT_ATTENDEE_PART_DECLINED;

    case I_CAL_PARTSTAT_TENTATIVE:
      return GCAL_EVENT_ATTENDEE_PART_TENTATIVE;

    case I_CAL_PARTSTAT_DELEGATED:
      return GCAL_EVENT_ATTENDEE_PART_DELEGATED;

    case I_CAL_PARTSTAT_NEEDSACTION:
    default:
      return GCAL_EVENT_ATTENDEE_PART_NEEDS_ACTION;
    }
}

static gboolean
rsvp_requested (ICalProperty *attendee)
{
  g_autoptr (ICalParameter) param = NULL;

  param = i_cal_property_get_first_parameter (attendee, I_CAL_RSVP_PARAMETER);

  return param && i_cal_parameter_get_rsvp (param) == I_CAL_RSVP_TRUE;
}

static void
add_timezones (ECalClient    *client,
               ICalComponent *vcalendar,
               ICalComponent *icomp,
               GCancellable  *cancellable)
{
  static const ICalPropertyKind kinds[] = {
    I_CAL_DTSTART_PROPERTY,
    I_CAL_DTEND_PROPERTY,
    I_CAL_RECURRENCEID_PROPERTY,
  };
  g_autoptr (GHashTable) seen = NULL;

  seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  for (gsize i = 0; i < G_N_ELEMENTS (kinds); i++)
    {
      g_autoptr (ICalProperty) prop = NULL;
      g_autoptr (ICalComponent) tz_comp = NULL;
      g_autoptr (GError) error = NULL;
      g_autofree gchar *tzid = NULL;
      ICalTimezone *zone = NULL;

      prop = i_cal_component_get_first_property (icomp, kinds[i]);
      if (!prop)
        continue;

      tzid = i_cal_property_get_parameter_as_string (prop, "TZID");
      if (!tzid || g_hash_table_contains (seen, tzid))
        continue;

      g_hash_table_add (seen, g_strdup (tzid));

      if (!e_cal_client_get_timezone_sync (client, tzid, &zone, cancellable, &error) || !zone)
        {
          g_debug ("No VTIMEZONE for '%s': %s", tzid, error ? error->message : "not found");
          continue;
        }

      tz_comp = i_cal_timezone_get_component (zone);
      if (tz_comp)
        i_cal_component_take_component (vcalendar, i_cal_component_clone (tz_comp));
    }
}

static void
strip_recurrence (ICalComponent *icomp)
{
  e_cal_util_component_remove_property_by_kind (icomp, I_CAL_RRULE_PROPERTY, TRUE);
  e_cal_util_component_remove_property_by_kind (icomp, I_CAL_RDATE_PROPERTY, TRUE);
  e_cal_util_component_remove_property_by_kind (icomp, I_CAL_EXRULE_PROPERTY, TRUE);
  e_cal_util_component_remove_property_by_kind (icomp, I_CAL_EXDATE_PROPERTY, TRUE);
}

static void
respond_thread (GTask        *task,
                gpointer      source_object,
                gpointer      task_data,
                GCancellable *cancellable)
{
  g_autoptr (ICalComponent) vcalendar = NULL;
  g_autoptr (ICalComponent) target = NULL;
  g_autoptr (ICalProperty) attendee = NULL;
  g_autoptr (GError) error = NULL;
  RespondData *data = task_data;
  ECalClient *client;
  guint32 flags;

  client = gcal_calendar_get_client (data->calendar);

  if (data->whole_series && e_cal_util_component_has_property (data->icomp, I_CAL_RECURRENCEID_PROPERTY))
    {
      /* Respond on the series: fetch the master component */
      if (!e_cal_client_get_object_sync (client,
                                         i_cal_component_get_uid (data->icomp),
                                         NULL,
                                         &target,
                                         cancellable,
                                         &error))
        {
          g_task_return_error (task, g_steal_pointer (&error));
          return;
        }
    }
  else
    {
      target = i_cal_component_clone (data->icomp);

      /* A single occurrence: keep RECURRENCE-ID, drop the series rules */
      if (e_cal_util_component_has_property (target, I_CAL_RECURRENCEID_PROPERTY))
        strip_recurrence (target);
    }

  attendee = find_user_attendee (target, data->user_addresses);
  if (!attendee)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                               "You are not among the attendees of this event");
      return;
    }

  i_cal_property_remove_parameter_by_kind (attendee, I_CAL_PARTSTAT_PARAMETER);
  i_cal_property_take_parameter (attendee, i_cal_parameter_new_partstat (data->partstat));

  vcalendar = e_cal_util_new_top_level ();
  i_cal_component_set_method (vcalendar, I_CAL_METHOD_REQUEST);
  add_timezones (client, vcalendar, target, cancellable);
  i_cal_component_take_component (vcalendar, g_steal_pointer (&target));

  flags = E_CAL_OPERATION_FLAG_NONE;
  if (!data->notify_organizer)
    flags |= E_CAL_OPERATION_FLAG_DISABLE_ITIP_MESSAGE;

  g_debug ("Sending reply: %s", i_cal_component_as_ical_string (vcalendar));

  if (!e_cal_client_receive_objects_sync (client, vcalendar, flags, cancellable, &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  g_task_return_boolean (task, TRUE);
}


/*
 * Public API
 */

/**
 * gcal_rsvp_get_user_status:
 * @manager: a #GcalManager
 * @event: a #GcalEvent
 * @out_status: (out) (optional): the user's current participation status
 * @out_rsvp_requested: (out) (optional): whether the organizer asked for a reply
 *
 * Returns: %TRUE if the user is an attendee of @event but not its organizer,
 * i.e. if the user can reply to it.
 */
gboolean
gcal_rsvp_get_user_status (GcalManager               *manager,
                           GcalEvent                 *event,
                           GcalEventAttendeePartStat *out_status,
                           gboolean                  *out_rsvp_requested)
{
  g_autoptr (GHashTable) addresses = NULL;
  g_autoptr (ICalProperty) attendee = NULL;
  ICalComponent *icomp;

  g_return_val_if_fail (GCAL_IS_MANAGER (manager), FALSE);
  g_return_val_if_fail (GCAL_IS_EVENT (event), FALSE);

  if (!gcal_event_get_calendar (event))
    return FALSE;

  icomp = e_cal_component_get_icalcomponent (gcal_event_get_component (event));
  addresses = gcal_rsvp_dup_user_addresses (manager, gcal_event_get_calendar (event));

  if (user_is_organizer (icomp, addresses))
    return FALSE;

  attendee = find_user_attendee (icomp, addresses);
  if (!attendee)
    return FALSE;

  if (out_status)
    *out_status = partstat_from_ical (attendee);
  if (out_rsvp_requested)
    *out_rsvp_requested = rsvp_requested (attendee);

  return TRUE;
}

/**
 * gcal_rsvp_respond:
 * @manager: a #GcalManager
 * @event: a #GcalEvent the user is invited to
 * @status: %GCAL_EVENT_ATTENDEE_PART_ACCEPTED, _TENTATIVE or _DECLINED
 * @whole_series: for an occurrence of a recurring event, respond to the
 *   whole series instead of this occurrence only
 * @notify_organizer: whether the backend may send the reply to the organizer
 * @cancellable: (nullable): a #GCancellable
 * @callback: callback to call when done
 * @user_data: data for @callback
 *
 * Replies to a meeting invitation through the calendar backend.
 */
void
gcal_rsvp_respond (GcalManager               *manager,
                   GcalEvent                 *event,
                   GcalEventAttendeePartStat  status,
                   gboolean                   whole_series,
                   gboolean                   notify_organizer,
                   GCancellable              *cancellable,
                   GAsyncReadyCallback        callback,
                   gpointer                   user_data)
{
  g_autoptr (GTask) task = NULL;
  RespondData *data;

  g_return_if_fail (GCAL_IS_MANAGER (manager));
  g_return_if_fail (GCAL_IS_EVENT (event));
  g_return_if_fail (status == GCAL_EVENT_ATTENDEE_PART_ACCEPTED ||
                    status == GCAL_EVENT_ATTENDEE_PART_TENTATIVE ||
                    status == GCAL_EVENT_ATTENDEE_PART_DECLINED);

  data = g_new0 (RespondData, 1);
  data->calendar = g_object_ref (gcal_event_get_calendar (event));
  data->icomp = i_cal_component_clone (e_cal_component_get_icalcomponent (gcal_event_get_component (event)));
  data->user_addresses = gcal_rsvp_dup_user_addresses (manager, data->calendar);
  data->whole_series = whole_series;
  data->notify_organizer = notify_organizer;

  switch (status)
    {
    case GCAL_EVENT_ATTENDEE_PART_ACCEPTED:
      data->partstat = I_CAL_PARTSTAT_ACCEPTED;
      break;

    case GCAL_EVENT_ATTENDEE_PART_TENTATIVE:
      data->partstat = I_CAL_PARTSTAT_TENTATIVE;
      break;

    case GCAL_EVENT_ATTENDEE_PART_DECLINED:
    default:
      data->partstat = I_CAL_PARTSTAT_DECLINED;
      break;
    }

  task = g_task_new (manager, cancellable, callback, user_data);
  g_task_set_source_tag (task, gcal_rsvp_respond);
  g_task_set_task_data (task, data, (GDestroyNotify) respond_data_free);
  g_task_run_in_thread (task, respond_thread);
}

/**
 * gcal_rsvp_dup_event_with_user_status:
 * @manager: a #GcalManager
 * @event: a #GcalEvent
 * @status: the user's new participation status
 *
 * Builds a copy of @event where the user's ATTENDEE carries @status, so the UI
 * can show a reply before the backend sends the updated event back.
 *
 * Returns: (transfer full) (nullable): a new #GcalEvent
 */
GcalEvent *
gcal_rsvp_dup_event_with_user_status (GcalManager               *manager,
                                      GcalEvent                 *event,
                                      GcalEventAttendeePartStat  status)
{
  g_autoptr (GHashTable) addresses = NULL;
  g_autoptr (ICalProperty) attendee = NULL;
  g_autoptr (ECalComponent) component = NULL;
  g_autoptr (GError) error = NULL;
  ICalParameterPartstat partstat;
  ICalComponent *icomp;
  GcalEvent *copy;

  g_return_val_if_fail (GCAL_IS_MANAGER (manager), NULL);
  g_return_val_if_fail (GCAL_IS_EVENT (event), NULL);

  switch (status)
    {
    case GCAL_EVENT_ATTENDEE_PART_ACCEPTED:
      partstat = I_CAL_PARTSTAT_ACCEPTED;
      break;

    case GCAL_EVENT_ATTENDEE_PART_TENTATIVE:
      partstat = I_CAL_PARTSTAT_TENTATIVE;
      break;

    case GCAL_EVENT_ATTENDEE_PART_DECLINED:
      partstat = I_CAL_PARTSTAT_DECLINED;
      break;

    default:
      partstat = I_CAL_PARTSTAT_NEEDSACTION;
      break;
    }

  component = e_cal_component_clone (gcal_event_get_component (event));
  icomp = e_cal_component_get_icalcomponent (component);
  addresses = gcal_rsvp_dup_user_addresses (manager, gcal_event_get_calendar (event));

  attendee = find_user_attendee (icomp, addresses);
  if (!attendee)
    return NULL;

  i_cal_property_remove_parameter_by_kind (attendee, I_CAL_PARTSTAT_PARAMETER);
  i_cal_property_take_parameter (attendee, i_cal_parameter_new_partstat (partstat));

  copy = gcal_event_new (gcal_event_get_calendar (event), component, &error);
  if (error)
    g_warning ("Could not copy event: %s", error->message);

  return copy;
}

gboolean
gcal_rsvp_respond_finish (GAsyncResult  *result,
                          GError       **error)
{
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (g_task_get_source_tag (G_TASK (result)) == gcal_rsvp_respond, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}
