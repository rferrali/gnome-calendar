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

G_END_DECLS
