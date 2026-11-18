/* gcal-guest-list.h
 *
 * Local fork addition (not upstream): list of guests with their status.
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

#define GCAL_TYPE_GUEST_LIST (gcal_guest_list_get_type())
G_DECLARE_FINAL_TYPE (GcalGuestList, gcal_guest_list, GCAL, GUEST_LIST, GtkBox)

GtkWidget*           gcal_guest_list_new                         (void);

void                 gcal_guest_list_set_event                   (GcalGuestList      *self,
                                                                  GcalEvent          *event);

guint                gcal_guest_list_get_n_guests                (GcalGuestList      *self);

const gchar*         gcal_guest_list_get_summary                 (GcalGuestList      *self);

void                 gcal_guest_list_set_editable                (GcalGuestList      *self,
                                                                  gboolean            editable);

void                 gcal_guest_list_focus_add_row               (GcalGuestList      *self);

void                 gcal_guest_list_set_keep_last_guest         (GcalGuestList      *self,
                                                                  gboolean            keep_last_guest);

G_END_DECLS
