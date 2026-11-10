/* gcal-rsvp-box.h
 *
 * Local fork addition (not upstream): Accept / Maybe / Decline buttons.
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

#include <gtk/gtk.h>

#include "gcal-event.h"

G_BEGIN_DECLS

#define GCAL_TYPE_RSVP_BOX (gcal_rsvp_box_get_type())
G_DECLARE_FINAL_TYPE (GcalRsvpBox, gcal_rsvp_box, GCAL, RSVP_BOX, GtkBox)

GtkWidget*           gcal_rsvp_box_new                           (void);

void                 gcal_rsvp_box_set_event                     (GcalRsvpBox        *self,
                                                                  GcalEvent          *event);

gboolean             gcal_rsvp_box_get_can_respond               (GcalRsvpBox        *self);

const gchar*         gcal_rsvp_box_get_status_text               (GcalRsvpBox        *self);

G_END_DECLS
