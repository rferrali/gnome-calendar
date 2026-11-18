/* gcal-contact-search.h
 *
 * Local fork addition (not upstream): guest suggestions.
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

#include <gio/gio.h>

#include "gcal-manager.h"

G_BEGIN_DECLS

typedef enum
{
  GCAL_CONTACT_ORIGIN_ADDRESS_BOOK,
  GCAL_CONTACT_ORIGIN_MAIL_HISTORY,
  GCAL_CONTACT_ORIGIN_DIRECTORY,
} GcalContactOrigin;

typedef struct
{
  gchar             *name;
  gchar             *address;
  GcalContactOrigin  origin;
  gint               score;
} GcalContactMatch;

void                 gcal_contact_match_free                     (GcalContactMatch    *match);

void                 gcal_contact_search                         (GcalManager         *manager,
                                                                  const gchar         *text,
                                                                  guint                max_results,
                                                                  GCancellable        *cancellable,
                                                                  GAsyncReadyCallback  callback,
                                                                  gpointer             user_data);

GPtrArray*           gcal_contact_search_finish                  (GAsyncResult        *result,
                                                                  GError             **error);

G_END_DECLS
