/* gcal-rsvp.h
 *
 * Local fork addition (not upstream): reply to meeting invitations.
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

#pragma once

#include "gcal-enums.h"
#include "gcal-event.h"
#include "gcal-manager.h"

G_BEGIN_DECLS

GHashTable*          gcal_rsvp_dup_user_addresses                (GcalManager               *manager,
                                                                  GcalCalendar              *calendar);

gboolean             gcal_rsvp_is_user_address                   (GHashTable                *addresses,
                                                                  const gchar               *address);

gboolean             gcal_rsvp_get_user_status                   (GcalManager               *manager,
                                                                  GcalEvent                 *event,
                                                                  GcalEventAttendeePartStat *out_status,
                                                                  gboolean                  *out_rsvp_requested);

void                 gcal_rsvp_respond                           (GcalManager               *manager,
                                                                  GcalEvent                 *event,
                                                                  GcalEventAttendeePartStat  status,
                                                                  gboolean                   whole_series,
                                                                  gboolean                   notify_organizer,
                                                                  GCancellable              *cancellable,
                                                                  GAsyncReadyCallback        callback,
                                                                  gpointer                   user_data);

gboolean             gcal_rsvp_respond_finish                    (GAsyncResult              *result,
                                                                  GError                   **error);

GcalEvent*           gcal_rsvp_dup_event_with_user_status        (GcalManager               *manager,
                                                                  GcalEvent                 *event,
                                                                  GcalEventAttendeePartStat  status);

gboolean             gcal_rsvp_can_edit_guests                   (GcalManager               *manager,
                                                                  GcalEvent                 *event);

gboolean             gcal_rsvp_add_guest                         (GcalManager               *manager,
                                                                  GcalEvent                 *event,
                                                                  const gchar               *address,
                                                                  const gchar               *name);

void                 gcal_rsvp_remove_guest                      (GcalManager               *manager,
                                                                  GcalEvent                 *event,
                                                                  const gchar               *address);

gboolean             gcal_rsvp_guests_equal                      (GcalEvent                 *event1,
                                                                  GcalEvent                 *event2);

void                 gcal_rsvp_count_guest_changes               (GcalEvent                 *original,
                                                                  GcalEvent                 *edited,
                                                                  guint                     *out_added,
                                                                  guint                     *out_removed,
                                                                  guint                     *out_kept);

void                 gcal_rsvp_copy_guests                       (GcalEvent                 *from,
                                                                  GcalEvent                 *to);

G_END_DECLS
