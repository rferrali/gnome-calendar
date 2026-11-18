/* gcal-suggestion-entry-row.h
 *
 * Copyright 2026 Romain Ferrali <rferrali@gmail.com>
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
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define GCAL_TYPE_SUGGESTION_ENTRY_ROW (gcal_suggestion_entry_row_get_type ())
G_DECLARE_FINAL_TYPE (GcalSuggestionEntryRow, gcal_suggestion_entry_row, GCAL, SUGGESTION_ENTRY_ROW, AdwEntryRow)

GtkWidget*           gcal_suggestion_entry_row_new               (void);

void                 gcal_suggestion_entry_row_set_suggestions   (GcalSuggestionEntryRow *self,
                                                                  GPtrArray              *rows,
                                                                  gboolean                select_first);

void                 gcal_suggestion_entry_row_hide_suggestions  (GcalSuggestionEntryRow *self);

guint                gcal_suggestion_entry_row_get_min_chars     (GcalSuggestionEntryRow *self);

void                 gcal_suggestion_entry_row_set_min_chars     (GcalSuggestionEntryRow *self,
                                                                  guint                   min_chars);

guint                gcal_suggestion_entry_row_get_delay         (GcalSuggestionEntryRow *self);

void                 gcal_suggestion_entry_row_set_delay         (GcalSuggestionEntryRow *self,
                                                                  guint                   delay);

G_END_DECLS
