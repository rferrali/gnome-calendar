/* gcal-edit-calendar-page.c
 *
 * Copyright 2019 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
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

#define G_LOG_DOMAIN "GcalEditCalendarPage"

/* Fork: GETTEXT_PACKAGE, and the dconf backend for the EDS calendar settings */
#include "config.h"
#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gsettingsbackend.h>

#include "gcal-context.h"
#include "gcal-calendar-management-page.h"
#include "gcal-debug.h"
#include "gcal-edit-calendar-page.h"
#include "gcal-utils.h"

#include <glib/gi18n.h>

struct _GcalEditCalendarPage
{
  AdwNavigationPage   parent;

  AdwActionRow       *account_row;
  GtkColorDialogButton *calendar_color_button;
  AdwSwitchRow       *calendar_visible_row;
  AdwActionRow       *calendar_url_row;
  AdwEntryRow        *name_entry;
  GtkWidget          *remove_button_row;

  /* Fork: default reminders */
  GtkWidget          *reminders_group;
  AdwSwitchRow       *remind_every_event_row;
  AdwComboRow        *reminder_time_row;
  GSettings          *eds_calendar_settings;
  GArray             *reminder_times;
  guint               applied_reminder_time;

  GcalCalendar       *calendar;
};

static void          gcal_calendar_management_page_iface_init    (GcalCalendarManagementPageInterface *iface);

G_DEFINE_TYPE_WITH_CODE (GcalEditCalendarPage, gcal_edit_calendar_page, ADW_TYPE_NAVIGATION_PAGE,
                         G_IMPLEMENT_INTERFACE (GCAL_TYPE_CALENDAR_MANAGEMENT_PAGE,
                                                gcal_calendar_management_page_iface_init))

static void          on_remind_every_event_changed_cb            (AdwSwitchRow         *row,
                                                                  GParamSpec           *pspec,
                                                                  GcalEditCalendarPage *self);

static void          on_reminder_time_changed_cb                 (AdwComboRow          *row,
                                                                  GParamSpec           *pspec,
                                                                  GcalEditCalendarPage *self);


/*
 * Fork: default reminders
 *
 * GNOME Shell reminds before every event of a calendar when both the global
 * defall-reminder-enabled key of the EDS calendar schema and the calendar's
 * ESourceAlarms:for-every-event are on. The reminder time is global too.
 */

#define EDS_CALENDAR_SCHEMA "org.gnome.evolution-data-server.calendar"

typedef enum
{
  REMINDER_UNIT_MINUTES,
  REMINDER_UNIT_HOURS,
  REMINDER_UNIT_DAYS,
} ReminderUnit;

typedef struct
{
  gint         interval;
  ReminderUnit unit;
} ReminderTime;

static const ReminderTime reminder_time_presets[] = {
  { 5, REMINDER_UNIT_MINUTES },
  { 10, REMINDER_UNIT_MINUTES },
  { 15, REMINDER_UNIT_MINUTES },
  { 30, REMINDER_UNIT_MINUTES },
  { 1, REMINDER_UNIT_HOURS },
  { 1, REMINDER_UNIT_DAYS },
};

/*
 * Other apps (GNOME Shell, Evolution) read the EDS keys from dconf. In the
 * Flatpak, the app's own settings live in a keyfile (GSETTINGS_BACKEND), so
 * this schema alone goes through the bundled dconf module.
 */
static GSettings *
create_eds_calendar_settings (void)
{
  g_autoptr (GSettingsBackend) backend = NULL;
  g_autoptr (GSettingsSchema) schema = NULL;
  GSettingsSchemaSource *schema_source;
  GIOExtensionPoint *extension_point;
  GIOExtension *extension;

  schema_source = g_settings_schema_source_get_default ();
  if (!schema_source)
    return NULL;

  schema = g_settings_schema_source_lookup (schema_source, EDS_CALENDAR_SCHEMA, TRUE);
  if (!schema ||
      !g_settings_schema_has_key (schema, "defall-reminder-enabled") ||
      !g_settings_schema_has_key (schema, "defall-reminder-interval") ||
      !g_settings_schema_has_key (schema, "defall-reminder-units"))
    {
      g_debug ("No default reminder keys in the %s schema", EDS_CALENDAR_SCHEMA);
      return NULL;
    }

  /* Loads the GIO modules, so the dconf extension is registered */
  g_object_unref (g_settings_backend_get_default ());

  extension_point = g_io_extension_point_lookup (G_SETTINGS_BACKEND_EXTENSION_POINT_NAME);
  extension = extension_point ? g_io_extension_point_get_extension_by_name (extension_point, "dconf") : NULL;
  if (!extension)
    {
      g_debug ("No dconf settings backend, hiding the default reminder settings");
      return NULL;
    }

  backend = g_object_new (g_io_extension_get_type (extension), NULL);

  return g_settings_new_full (schema, backend, NULL);
}

static gchar *
format_reminder_time (const ReminderTime *time)
{
  switch (time->unit)
    {
    case REMINDER_UNIT_MINUTES:
      return g_strdup_printf (g_dngettext (GETTEXT_PACKAGE, "%d minute", "%d minutes", time->interval), time->interval);

    case REMINDER_UNIT_HOURS:
      return g_strdup_printf (g_dngettext (GETTEXT_PACKAGE, "%d hour", "%d hours", time->interval), time->interval);

    case REMINDER_UNIT_DAYS:
      return g_strdup_printf (g_dngettext (GETTEXT_PACKAGE, "%d day", "%d days", time->interval), time->interval);

    default:
      g_assert_not_reached ();
    }
}

/* The presets, plus the current time if it was set elsewhere (Evolution, gsettings) */
static void
setup_reminder_times (GcalEditCalendarPage *self)
{
  g_autoptr (GtkStringList) model = NULL;
  ReminderTime current;
  guint selected = GTK_INVALID_LIST_POSITION;

  current.interval = g_settings_get_int (self->eds_calendar_settings, "defall-reminder-interval");
  current.unit = g_settings_get_enum (self->eds_calendar_settings, "defall-reminder-units");

  g_array_set_size (self->reminder_times, 0);
  g_array_append_vals (self->reminder_times, reminder_time_presets, G_N_ELEMENTS (reminder_time_presets));

  for (guint i = 0; i < self->reminder_times->len; i++)
    {
      const ReminderTime *time = &g_array_index (self->reminder_times, ReminderTime, i);

      if (time->interval == current.interval && time->unit == current.unit)
        selected = i;
    }

  if (selected == GTK_INVALID_LIST_POSITION)
    {
      g_array_append_val (self->reminder_times, current);
      selected = self->reminder_times->len - 1;
    }

  model = gtk_string_list_new (NULL);
  for (guint i = 0; i < self->reminder_times->len; i++)
    {
      g_autofree gchar *label = format_reminder_time (&g_array_index (self->reminder_times, ReminderTime, i));

      gtk_string_list_append (model, label);
    }

  g_signal_handlers_block_by_func (self->reminder_time_row, on_reminder_time_changed_cb, self);
  adw_combo_row_set_model (self->reminder_time_row, G_LIST_MODEL (model));
  adw_combo_row_set_selected (self->reminder_time_row, selected);
  self->applied_reminder_time = selected;
  g_signal_handlers_unblock_by_func (self->reminder_time_row, on_reminder_time_changed_cb, self);
}

static gboolean
source_reminds_every_event (ESource *source)
{
  if (!e_source_has_extension (source, E_SOURCE_EXTENSION_ALARMS))
    return FALSE;

  return e_source_alarms_get_for_every_event (e_source_get_extension (source, E_SOURCE_EXTENSION_ALARMS));
}

/* The global switch is on exactly when at least one calendar uses it */
static void
sync_default_reminder_enabled (GcalEditCalendarPage *self)
{
  g_autoptr (GList) calendars = NULL;
  GcalManager *manager;
  gboolean any = FALSE;

  manager = gcal_context_get_manager (gcal_application_get_context (GCAL_DEFAULT_APPLICATION));
  calendars = gcal_manager_get_calendars (manager);

  for (GList *l = calendars; l && !any; l = l->next)
    any = source_reminds_every_event (gcal_calendar_get_source (l->data));

  g_settings_set_boolean (self->eds_calendar_settings, "defall-reminder-enabled", any);
}

static void
setup_reminders (GcalEditCalendarPage *self,
                 ESource              *source)
{
  /* Hidden only when the source itself can't be saved; read-only calendars can still remind */
  gboolean available = self->eds_calendar_settings && e_source_get_writable (source);

  gtk_widget_set_visible (self->reminders_group, available);

  if (!available)
    return;

  g_signal_handlers_block_by_func (self->remind_every_event_row, on_remind_every_event_changed_cb, self);
  adw_switch_row_set_active (self->remind_every_event_row, source_reminds_every_event (source));
  g_signal_handlers_unblock_by_func (self->remind_every_event_row, on_remind_every_event_changed_cb, self);

  setup_reminder_times (self);
}

static void
on_remind_every_event_changed_cb (AdwSwitchRow         *row,
                                  GParamSpec           *pspec,
                                  GcalEditCalendarPage *self)
{
  ESourceAlarms *alarms;
  GcalManager *manager;
  ESource *source;

  if (!self->calendar || !self->eds_calendar_settings)
    return;

  source = gcal_calendar_get_source (self->calendar);
  alarms = e_source_get_extension (source, E_SOURCE_EXTENSION_ALARMS);
  e_source_alarms_set_for_every_event (alarms, adw_switch_row_get_active (row));

  manager = gcal_context_get_manager (gcal_application_get_context (GCAL_DEFAULT_APPLICATION));
  gcal_manager_save_source (manager, source);

  sync_default_reminder_enabled (self);
}

static void
select_reminder_time (GcalEditCalendarPage *self,
                      guint                 position)
{
  g_signal_handlers_block_by_func (self->reminder_time_row, on_reminder_time_changed_cb, self);
  adw_combo_row_set_selected (self->reminder_time_row, position);
  g_signal_handlers_unblock_by_func (self->reminder_time_row, on_reminder_time_changed_cb, self);
}

static void
on_reminder_time_dialog_response_cb (GObject      *object,
                                     GAsyncResult *result,
                                     gpointer      user_data)
{
  g_autoptr (GcalEditCalendarPage) self = user_data;
  const ReminderTime *time;
  const gchar *response;
  guint selected;

  response = adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (object), result);
  selected = adw_combo_row_get_selected (self->reminder_time_row);

  if (g_strcmp0 (response, "change") != 0 || selected >= self->reminder_times->len)
    {
      select_reminder_time (self, self->applied_reminder_time);
      return;
    }

  time = &g_array_index (self->reminder_times, ReminderTime, selected);

  g_settings_set_int (self->eds_calendar_settings, "defall-reminder-interval", time->interval);
  g_settings_set_enum (self->eds_calendar_settings, "defall-reminder-units", time->unit);

  self->applied_reminder_time = selected;
}

/* The time is global in EDS, so changing it from one calendar asks first */
static void
on_reminder_time_changed_cb (AdwComboRow          *row,
                             GParamSpec           *pspec,
                             GcalEditCalendarPage *self)
{
  g_autofree gchar *time_label = NULL;
  AdwDialog *dialog;
  guint selected;

  if (!self->eds_calendar_settings)
    return;

  selected = adw_combo_row_get_selected (row);
  if (selected >= self->reminder_times->len || selected == self->applied_reminder_time)
    return;

  time_label = format_reminder_time (&g_array_index (self->reminder_times, ReminderTime, selected));

  dialog = adw_alert_dialog_new (_("Change Reminder Time for All Calendars?"), NULL);
  adw_alert_dialog_format_body (ADW_ALERT_DIALOG (dialog),
                                _("Every calendar that reminds before every event will remind you %s before each event."),
                                time_label);

  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog),
                                  "cancel", _("_Cancel"),
                                  "change", _("C_hange"),
                                  NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "change", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "cancel");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");

  adw_alert_dialog_choose (ADW_ALERT_DIALOG (dialog),
                           GTK_WIDGET (self),
                           NULL,
                           on_reminder_time_dialog_response_cb,
                           g_object_ref (self));
}

/*
 * Auxiliary methods
 */

static gboolean
is_goa_calendar (GcalEditCalendarPage *self,
                 GcalCalendar         *calendar)
{
  g_autoptr (ESource) parent = NULL;
  GcalContext *context;
  GcalManager *manager;
  ESource *source;

  g_assert (calendar && GCAL_IS_CALENDAR (calendar));

  context = gcal_application_get_context (GCAL_DEFAULT_APPLICATION);
  manager = gcal_context_get_manager (context);

  source = gcal_calendar_get_source (calendar);
  parent = gcal_manager_get_source (manager, e_source_get_parent (source));

  return e_source_has_extension (parent, E_SOURCE_EXTENSION_GOA);
}

static gboolean
is_remote_source (ESource *source)
{
  gboolean has_webdav, has_auth;

  g_assert (E_IS_SOURCE (source));

  has_webdav = e_source_has_extension (source, E_SOURCE_EXTENSION_WEBDAV_BACKEND);
  has_auth = e_source_has_extension (source, E_SOURCE_EXTENSION_AUTHENTICATION);

  if (!has_webdav || !has_auth)
    return FALSE;

  if (has_auth)
    {
      ESourceAuthentication *auth;

      auth = e_source_get_extension (source, E_SOURCE_EXTENSION_AUTHENTICATION);

      /* No host is set, it's not a remote source */
      if (!e_source_authentication_get_host (auth))
        return FALSE;
    }

  if (has_webdav)
    {
      ESourceWebdav *webdav;

      webdav = e_source_get_extension (source, E_SOURCE_EXTENSION_WEBDAV_BACKEND);

      /* No resource path specified, not a remote source */
      if (!e_source_webdav_get_resource_path (webdav))
        return FALSE;
    }

  return TRUE;
}

static void
setup_calendar (GcalEditCalendarPage *self,
                GcalCalendar         *calendar)
{
  ESource *source;
  gboolean is_remote;
  gboolean is_file;
  gboolean is_goa;

  self->calendar = g_object_ref (calendar);

  is_goa = is_goa_calendar (self, calendar);
  source = gcal_calendar_get_source (calendar);
  is_file = e_source_has_extension (source, E_SOURCE_EXTENSION_LOCAL_BACKEND);
  is_remote = is_remote_source (source);

  GCAL_TRACE_MSG ("Calendar '%s' is GOA: %d; is file: %d; is remote: %d",
                  gcal_calendar_get_name (calendar),
                  is_goa,
                  is_file,
                  is_remote);

  gtk_widget_set_visible (GTK_WIDGET (self->account_row), is_goa);
  gtk_widget_set_visible (GTK_WIDGET (self->calendar_url_row), !is_goa);

  /* If it's a file, set the file path */
  if (is_file)
    {
      g_autofree gchar *basename = NULL;
      g_autofree gchar *text = NULL;
      g_autofree gchar *uri = NULL;
      ESourceLocal *local;
      GFile *file;

      local = e_source_get_extension (source, E_SOURCE_EXTENSION_LOCAL_BACKEND);
      file = e_source_local_get_custom_file (local);
      basename = g_file_get_basename (file);
      uri = g_file_get_uri (file);

      text = g_strdup_printf ("<a title=\"%1$s\" href=\"%2$s\">%1$s</a>", basename, uri);
      adw_action_row_set_subtitle (self->calendar_url_row, text);

      gtk_accessible_update_property (GTK_ACCESSIBLE (self->calendar_url_row),
                                      GTK_ACCESSIBLE_PROPERTY_LABEL, basename,
                                      -1);
    }

  /* If it's remote, build the uri */
  if (is_remote)
    {
      g_autoptr (GUri) guri = NULL;
      g_autofree gchar *text = NULL;
      g_autofree gchar *uri = NULL;
      ESourceWebdav *webdav;

      webdav = e_source_get_extension (source, E_SOURCE_EXTENSION_WEBDAV_BACKEND);
      guri = e_source_webdav_dup_uri (webdav);
      uri = g_uri_to_string_partial (guri, G_URI_HIDE_PASSWORD);
      text = g_strdup_printf ("<a title=\"%1$s\" href=\"%1$s\">%1$s</a>", uri);

      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->calendar_url_row), _("Calendar URL"));
      adw_action_row_set_subtitle (self->calendar_url_row, text);

      gtk_accessible_update_property (GTK_ACCESSIBLE (self->calendar_url_row),
                                      GTK_ACCESSIBLE_PROPERTY_LABEL, uri,
                                      -1);
    }

  if (is_goa)
    {
      const gchar *parent_name = e_source_get_display_name (gcal_calendar_get_parent_source (calendar));

      adw_action_row_set_subtitle (self->account_row, parent_name);
    }

  if (!is_goa && !is_remote && !is_file)
    {
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->calendar_url_row), _("Location"));
      adw_action_row_set_subtitle (self->calendar_url_row, _("On This Computer"));
    }

  gtk_color_dialog_button_set_rgba (self->calendar_color_button, gcal_calendar_get_color (calendar));
  gtk_editable_set_text (GTK_EDITABLE (self->name_entry), gcal_calendar_get_name (calendar));
  adw_switch_row_set_active (self->calendar_visible_row, gcal_calendar_get_visible (calendar));

  gtk_widget_set_visible (self->remove_button_row, e_source_get_removable (source));

  setup_reminders (self, source);
}

/*
 * Callbacks
 */

static void
on_calendar_color_changed_cb (GtkColorDialogButton *button,
                              GParamSpec           *pspec,
                              GcalEditCalendarPage *self)
{
  gcal_calendar_set_color (self->calendar, gtk_color_dialog_button_get_rgba (button));
}

static void
on_calendar_visibility_changed_cb (AdwSwitchRow         *row,
                                   GParamSpec           *pspec,
                                   GcalEditCalendarPage *self)
{
  gcal_calendar_set_visible (self->calendar, adw_switch_row_get_active (row));
}

typedef struct
{
  GHashTable *zones;
  ECalClient *client;
} TimeZoneData;

static void
insert_tz_comps (ICalParameter *param,
                 gpointer       data)
{
  g_autoptr (GError) error = NULL;
  TimeZoneData *tdata = data;
  ICalTimezone *zone = NULL;
  ICalComponent *tzcomp;
  const gchar *tzid;

  tzid = i_cal_parameter_get_tzid (param);

  if (g_hash_table_lookup (tdata->zones, tzid))
    return;

  if (!e_cal_client_get_timezone_sync (tdata->client, tzid, &zone, NULL, &error))
    zone = NULL;

  if (error != NULL)
    {
      g_warning ("Could not get the timezone information for %s: %s", tzid, error->message);
      return;
    }

  tzcomp = i_cal_component_clone (i_cal_timezone_get_component (zone));
  g_hash_table_insert (tdata->zones, (gpointer) tzid, (gpointer) tzcomp);
}

static void
append_tz_to_comp (gpointer       key,
                   gpointer       value,
                   ICalComponent *toplevel)
{
  i_cal_component_add_component (toplevel, (ICalComponent *) value);
}

static void
on_file_dialog_save_cb (GObject      *object,
                        GAsyncResult *res,
                        gpointer      user_data)
{
  GcalEditCalendarPage *self = user_data;
  g_autofree gchar *ics_str = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GSList) events = NULL;
  g_autoptr (GFile) file = NULL;
  ICalComponent *toplevel_component = NULL;
  ECalClient *client = NULL;
  TimeZoneData tz_data;

  file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (object), res, &error);
  if (error)
    {
      if (!g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
        g_warning ("Error saving file: %s", error->message);
      return;
    }

  client = gcal_calendar_get_client (self->calendar);
  e_cal_client_get_object_list_sync (client, "#t", &events, NULL, NULL);

  /* Avoid exporting empty calendars */
  if (!events)
    return;

  toplevel_component = e_cal_util_new_top_level ();
  tz_data.zones = g_hash_table_new (g_str_hash, g_str_equal);
  tz_data.client = client;

  for (GSList *l = events; l != NULL; l = l->next)
   {
      ICalComponent *component = i_cal_component_clone (l->data);

      i_cal_component_foreach_tzid (component, insert_tz_comps, &tz_data);
      i_cal_component_take_component (toplevel_component, g_steal_pointer (&component));
    }

  g_hash_table_foreach (tz_data.zones, (GHFunc) append_tz_to_comp, toplevel_component);
  g_clear_pointer (&tz_data.zones, g_hash_table_destroy);

  i_cal_component_strip_errors (toplevel_component);
  ics_str = i_cal_component_as_ical_string (toplevel_component);

  g_clear_pointer (&toplevel_component, i_cal_component_free);

  g_file_replace_contents (file,
                           ics_str,
                           strlen (ics_str),
                           NULL,
                           FALSE,
                           G_FILE_CREATE_REPLACE_DESTINATION,
                           NULL,
                           NULL,
                           NULL);
}

static void
on_export_button_row_activated_cb (GtkButton            *button,
                                   GcalEditCalendarPage *self)
{
  g_autofree gchar *filename = NULL;
  GtkFileDialog *file_dialog = NULL;
  GtkWindow *window = NULL;

  GCAL_ENTRY;

  window = GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self)));
  file_dialog = gtk_file_dialog_new ();
  filename = g_strconcat (gcal_calendar_get_name (self->calendar), ".ics", NULL);

  gtk_file_dialog_set_title (file_dialog, _("Export Calendar"));
  gtk_file_dialog_set_initial_name (file_dialog, filename);

  gtk_file_dialog_save (file_dialog,
                        window,
                        NULL,
                        on_file_dialog_save_cb,
                        self);


  GCAL_EXIT;
}

static void
on_remove_button_row_activated_cb (GtkButton            *button,
                                   GcalEditCalendarPage *self)
{
  GcalCalendarManagementPage *page;

  GCAL_ENTRY;

  page = GCAL_CALENDAR_MANAGEMENT_PAGE (self);
  gcal_calendar_management_page_switch_page (page, "calendars", self->calendar);

  GCAL_EXIT;
}

static void
on_settings_button_clicked_cb (GtkWidget            *button,
                               GcalEditCalendarPage *self)
{
  g_autoptr (ESource) parent = NULL;
  GApplication *app;
  GcalContext *context;
  GcalManager *manager;
  ESourceGoa *goa;
  ESource *source;

  GCAL_ENTRY;

  context = gcal_application_get_context (GCAL_DEFAULT_APPLICATION);
  manager = gcal_context_get_manager (context);
  source = gcal_calendar_get_source (self->calendar);
  parent = gcal_manager_get_source (manager, e_source_get_parent (source));

  g_assert (e_source_has_extension (parent, E_SOURCE_EXTENSION_GOA));
  goa = e_source_get_extension (parent, E_SOURCE_EXTENSION_GOA);

  app = g_application_get_default ();
  gcal_utils_launch_gnome_settings (g_application_get_dbus_connection (app),
                                    "online-accounts",
                                    e_source_goa_get_account_id (goa));

  GCAL_EXIT;
}


/*
 * GcalCalendarManagementPage iface
 */

static void
gcal_edit_calendar_page_activate (GcalCalendarManagementPage *page,
                                  GcalCalendar               *calendar)
{
  GCAL_ENTRY;

  setup_calendar (GCAL_EDIT_CALENDAR_PAGE (page), calendar);

  GCAL_EXIT;
}

static void
gcal_edit_calendar_page_deactivate (GcalCalendarManagementPage *page)
{
  GcalEditCalendarPage *self;
  GcalContext *context;
  GcalManager *manager;

  GCAL_ENTRY;

  self = GCAL_EDIT_CALENDAR_PAGE (page);

  gcal_calendar_set_name (self->calendar, gtk_editable_get_text (GTK_EDITABLE (self->name_entry)));

  context = gcal_application_get_context (GCAL_DEFAULT_APPLICATION);
  manager = gcal_context_get_manager (context);
  gcal_manager_save_source (manager, gcal_calendar_get_source (self->calendar));

  g_clear_object (&self->calendar);

  GCAL_EXIT;
}

static void
gcal_calendar_management_page_iface_init (GcalCalendarManagementPageInterface *iface)
{
  iface->activate = gcal_edit_calendar_page_activate;
  iface->deactivate = gcal_edit_calendar_page_deactivate;
}


/*
 * GObject overrides
 */

static void
gcal_edit_calendar_page_dispose (GObject *object)
{
  GcalEditCalendarPage *self = GCAL_EDIT_CALENDAR_PAGE (object);

  if (self->calendar)
    gcal_calendar_set_name (self->calendar, gtk_editable_get_text (GTK_EDITABLE (self->name_entry)));

  G_OBJECT_CLASS (gcal_edit_calendar_page_parent_class)->dispose (object);
}

static void
gcal_edit_calendar_page_finalize (GObject *object)
{
  GcalEditCalendarPage *self = (GcalEditCalendarPage *)object;

  g_clear_object (&self->calendar);
  g_clear_object (&self->eds_calendar_settings);
  g_clear_pointer (&self->reminder_times, g_array_unref);

  G_OBJECT_CLASS (gcal_edit_calendar_page_parent_class)->finalize (object);
}

static void
gcal_edit_calendar_page_class_init (GcalEditCalendarPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = gcal_edit_calendar_page_dispose;
  object_class->finalize = gcal_edit_calendar_page_finalize;

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/calendar/ui/gui/calendar-management/gcal-edit-calendar-page.ui");

  gtk_widget_class_bind_template_child (widget_class, GcalEditCalendarPage, account_row);
  gtk_widget_class_bind_template_child (widget_class, GcalEditCalendarPage, calendar_color_button);
  gtk_widget_class_bind_template_child (widget_class, GcalEditCalendarPage, calendar_url_row);
  gtk_widget_class_bind_template_child (widget_class, GcalEditCalendarPage, calendar_visible_row);
  gtk_widget_class_bind_template_child (widget_class, GcalEditCalendarPage, name_entry);
  gtk_widget_class_bind_template_child (widget_class, GcalEditCalendarPage, remove_button_row);
  gtk_widget_class_bind_template_child (widget_class, GcalEditCalendarPage, reminders_group);
  gtk_widget_class_bind_template_child (widget_class, GcalEditCalendarPage, remind_every_event_row);
  gtk_widget_class_bind_template_child (widget_class, GcalEditCalendarPage, reminder_time_row);

  gtk_widget_class_bind_template_callback (widget_class, on_calendar_color_changed_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_calendar_visibility_changed_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_remove_button_row_activated_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_export_button_row_activated_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_settings_button_clicked_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_remind_every_event_changed_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_reminder_time_changed_cb);
}

static void
gcal_edit_calendar_page_init (GcalEditCalendarPage *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  /* Fork: default reminders */
  self->reminder_times = g_array_new (FALSE, FALSE, sizeof (ReminderTime));
  self->eds_calendar_settings = create_eds_calendar_settings ();
}
