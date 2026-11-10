/* gcal-contact-search.c
 *
 * Local fork addition (not upstream): guest suggestions.
 *
 * Suggests people from the same places as the Stamp mail client fork:
 *
 *  - every enabled EDS address book, including search-only directories such
 *    as the Exchange Global Address List (queried only from 3 characters on,
 *    since each query goes to the server);
 *  - the mail history kept in Stamp's Camel cache
 *    (~/.var/app/org.tabos.stamp/cache/stamp/<account>/folders.db): the
 *    recipients of sent mail and the senders of received mail, ranked by how
 *    often they appear. The index is built once, read-only, on first use.
 *
 * Everything runs in a worker thread.
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

#define G_LOG_DOMAIN "GcalContactSearch"

#include "gcal-contact-search.h"

#include <camel/camel.h>
#include <libebook/libebook.h>
#include <sqlite3.h>

#define DIRECTORY_MIN_CHARS 3

typedef struct
{
  gchar *name;
  guint  count;
} HistoryEntry;

typedef struct
{
  ESourceRegistry *registry;
  gchar           *text;
  guint            max_results;
} SearchData;

/* Shared between searches; guarded by the mutex */
static GMutex       cache_mutex;
static GHashTable  *book_clients = NULL;   /* source uid → EBookClient */
static GHashTable  *history = NULL;        /* lower address → HistoryEntry */

static void
history_entry_free (HistoryEntry *entry)
{
  g_free (entry->name);
  g_free (entry);
}

static void
search_data_free (SearchData *data)
{
  g_clear_object (&data->registry);
  g_free (data->text);
  g_free (data);
}

void
gcal_contact_match_free (GcalContactMatch *match)
{
  if (!match)
    return;

  g_free (match->name);
  g_free (match->address);
  g_free (match);
}


/*
 * Mail history
 */

static void
history_add_addresses (GHashTable  *index,
                       const gchar *header)
{
  g_autoptr (CamelInternetAddress) addresses = NULL;
  gint count;

  if (!header || !*header)
    return;

  addresses = camel_internet_address_new ();
  count = camel_address_decode (CAMEL_ADDRESS (addresses), header);

  for (gint i = 0; i < count; i++)
    {
      const gchar *name = NULL;
      const gchar *email = NULL;
      g_autofree gchar *key = NULL;
      HistoryEntry *entry;

      if (!camel_internet_address_get (addresses, i, &name, &email) || !email || !strchr (email, '@'))
        continue;

      key = g_ascii_strdown (email, -1);
      entry = g_hash_table_lookup (index, key);

      if (!entry)
        {
          entry = g_new0 (HistoryEntry, 1);
          g_hash_table_insert (index, g_steal_pointer (&key), entry);
        }

      entry->count++;

      if (name && *name && !entry->name)
        entry->name = g_strdup (name);
    }
}

static gboolean
folder_is_ignored (const gchar *folder_name)
{
  static const gchar *ignored[] = {
    "Deleted Items", "Trash", "Junk", "Junk Email", "Spam", "Drafts", "Outbox", "Sync Issues",
    "Conversation History", "Calendar", "Contacts", "Tasks", "Notes", "Journal",
  };

  for (gsize i = 0; i < G_N_ELEMENTS (ignored); i++)
    {
      gsize len = strlen (ignored[i]);

      if (g_ascii_strncasecmp (folder_name, ignored[i], len) == 0 &&
          (folder_name[len] == '\0' || folder_name[len] == '/'))
        return TRUE;
    }

  return FALSE;
}

static gboolean
folder_is_sent (const gchar *folder_name)
{
  return g_ascii_strncasecmp (folder_name, "Sent", 4) == 0;
}

static void
history_index_database (GHashTable  *index,
                        const gchar *path)
{
  g_autofree gchar *uri = NULL;
  sqlite3_stmt *folders = NULL;
  sqlite3 *db = NULL;

  uri = g_strdup_printf ("file:%s?mode=ro", path);

  if (sqlite3_open_v2 (uri, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL) != SQLITE_OK)
    {
      g_debug ("Cannot open %s: %s", path, db ? sqlite3_errmsg (db) : "unknown error");
      sqlite3_close (db);
      return;
    }

  sqlite3_busy_timeout (db, 2000);

  if (sqlite3_prepare_v2 (db, "SELECT folder_id, folder_name FROM folders", -1, &folders, NULL) != SQLITE_OK)
    {
      g_debug ("Unexpected cache layout in %s: %s", path, sqlite3_errmsg (db));
      sqlite3_close (db);
      return;
    }

  while (sqlite3_step (folders) == SQLITE_ROW)
    {
      g_autofree gchar *query = NULL;
      const gchar *folder_name;
      sqlite3_stmt *messages = NULL;
      gboolean sent;
      gint64 folder_id;

      folder_id = sqlite3_column_int64 (folders, 0);
      folder_name = (const gchar *) sqlite3_column_text (folders, 1);

      if (!folder_name || folder_is_ignored (folder_name))
        continue;

      sent = folder_is_sent (folder_name);

      /* For sent mail the interesting people are the recipients, not the sender (the user) */
      query = g_strdup_printf ("SELECT %s FROM messages_%" G_GINT64_FORMAT,
                               sent ? "mail_to, mail_cc" : "mail_from",
                               folder_id);

      if (sqlite3_prepare_v2 (db, query, -1, &messages, NULL) != SQLITE_OK)
        continue;

      while (sqlite3_step (messages) == SQLITE_ROW)
        {
          history_add_addresses (index, (const gchar *) sqlite3_column_text (messages, 0));
          if (sent)
            history_add_addresses (index, (const gchar *) sqlite3_column_text (messages, 1));
        }

      sqlite3_finalize (messages);
    }

  sqlite3_finalize (folders);
  sqlite3_close (db);
}

/* Called with cache_mutex held */
static void
ensure_history (void)
{
  g_autofree gchar *base = NULL;
  g_autoptr (GDir) dir = NULL;
  const gchar *account;

  if (history)
    return;

  history = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) history_entry_free);

  base = g_build_filename (g_get_home_dir (), ".var", "app", "org.tabos.stamp", "cache", "stamp", NULL);
  dir = g_dir_open (base, 0, NULL);
  if (!dir)
    {
      g_debug ("No Stamp mail cache at %s", base);
      return;
    }

  while ((account = g_dir_read_name (dir)))
    {
      g_autofree gchar *path = g_build_filename (base, account, "folders.db", NULL);

      if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
        history_index_database (history, path);
    }

  g_debug ("Mail history index: %u addresses", g_hash_table_size (history));
}


/*
 * Address books
 */

/* Called with cache_mutex held; returns a new reference */
static EBookClient *
ensure_book_client (ESource      *source,
                    GCancellable *cancellable)
{
  g_autoptr (GError) error = NULL;
  EClient *client;

  if (!book_clients)
    book_clients = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

  client = g_hash_table_lookup (book_clients, e_source_get_uid (source));
  if (client)
    return g_object_ref (E_BOOK_CLIENT (client));

  client = e_book_client_connect_sync (source, 10, cancellable, &error);
  if (!client)
    {
      g_debug ("Cannot open address book '%s': %s", e_source_get_display_name (source), error->message);
      return NULL;
    }

  g_hash_table_insert (book_clients, g_strdup (e_source_get_uid (source)), client);

  return g_object_ref (E_BOOK_CLIENT (client));
}

static gboolean
book_is_directory (EBookClient *client)
{
  return !e_client_check_capability (E_CLIENT (client), "do-initial-query");
}

static gchar *
build_book_query (const gchar *text)
{
  g_auto (GStrv) words = NULL;
  g_autoptr (GPtrArray) conditions = NULL;
  g_autoptr (EBookQuery) query = NULL;

  words = g_strsplit_set (text, " \t", -1);
  conditions = g_ptr_array_new ();

  g_ptr_array_add (conditions, e_book_query_field_exists (E_CONTACT_EMAIL));

  for (guint i = 0; words[i]; i++)
    {
      EBookQuery *fields[2];

      if (!*words[i])
        continue;

      fields[0] = e_book_query_field_test (E_CONTACT_FULL_NAME, E_BOOK_QUERY_CONTAINS, words[i]);
      fields[1] = e_book_query_field_test (E_CONTACT_EMAIL, E_BOOK_QUERY_CONTAINS, words[i]);
      g_ptr_array_add (conditions, e_book_query_or (2, fields, TRUE));
    }

  query = e_book_query_and (conditions->len, (EBookQuery **) conditions->pdata, TRUE);

  return e_book_query_to_string (query);
}


/*
 * Merging and ranking
 */

static gboolean
text_matches (const gchar *lower_text,
              const gchar *name,
              const gchar *address,
              gboolean    *out_prefix)
{
  g_autofree gchar *lower_name = name ? g_utf8_strdown (name, -1) : NULL;
  g_autofree gchar *lower_address = address ? g_ascii_strdown (address, -1) : NULL;
  g_auto (GStrv) words = g_strsplit_set (lower_text, " \t", -1);
  gboolean prefix = FALSE;

  for (guint i = 0; words[i]; i++)
    {
      gboolean in_name, in_address;

      if (!*words[i])
        continue;

      in_name = lower_name && strstr (lower_name, words[i]);
      in_address = lower_address && strstr (lower_address, words[i]);

      if (!in_name && !in_address)
        return FALSE;
    }

  /* Prefix of the address, of the name, or of a word of the name */
  if (lower_address && g_str_has_prefix (lower_address, lower_text))
    prefix = TRUE;
  else if (lower_name && g_str_has_prefix (lower_name, lower_text))
    prefix = TRUE;
  else if (lower_name)
    {
      g_autofree gchar *needle = g_strconcat (" ", lower_text, NULL);

      prefix = strstr (lower_name, needle) != NULL;
    }

  if (out_prefix)
    *out_prefix = prefix;

  return TRUE;
}

static void
add_match (GHashTable        *matches,
           const gchar       *name,
           const gchar       *address,
           GcalContactOrigin  origin,
           gint               score)
{
  g_autofree gchar *key = NULL;
  GcalContactMatch *match;

  if (!address || !strchr (address, '@'))
    return;

  key = g_ascii_strdown (address, -1);
  match = g_hash_table_lookup (matches, key);

  if (match)
    {
      /* Keep the best score; prefer a real name from an address book */
      if (score > match->score)
        {
          match->score = score;
          match->origin = origin;
        }

      if (name && *name && (!match->name || origin == GCAL_CONTACT_ORIGIN_ADDRESS_BOOK))
        {
          g_free (match->name);
          match->name = g_strdup (name);
        }

      return;
    }

  match = g_new0 (GcalContactMatch, 1);
  match->name = (name && *name) ? g_strdup (name) : NULL;
  match->address = g_strdup (address);
  match->origin = origin;
  match->score = score;

  g_hash_table_insert (matches, g_steal_pointer (&key), match);
}

static gint
compare_matches (gconstpointer a,
                 gconstpointer b)
{
  const GcalContactMatch *ma = *(const GcalContactMatch **) a;
  const GcalContactMatch *mb = *(const GcalContactMatch **) b;

  if (ma->score != mb->score)
    return mb->score - ma->score;

  return g_ascii_strcasecmp (ma->address, mb->address);
}

static void
search_thread (GTask        *task,
               gpointer      source_object,
               gpointer      task_data,
               GCancellable *cancellable)
{
  g_autoptr (GHashTable) matches = NULL;
  g_autolist (ESource) books = NULL;
  g_autofree gchar *lower_text = NULL;
  g_autofree gchar *sexp = NULL;
  SearchData *data = task_data;
  GPtrArray *results;
  GHashTableIter iter;
  gpointer key, value;

  matches = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) gcal_contact_match_free);
  lower_text = g_utf8_strdown (data->text, -1);
  g_strstrip (lower_text);

  if (!*lower_text)
    {
      g_task_return_pointer (task, g_ptr_array_new (), (GDestroyNotify) g_ptr_array_unref);
      return;
    }

  /* Mail history */
  g_mutex_lock (&cache_mutex);
  ensure_history ();

  g_hash_table_iter_init (&iter, history);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      HistoryEntry *entry = value;
      gboolean prefix = FALSE;

      if (!text_matches (lower_text, entry->name, key, &prefix))
        continue;

      /* Frequent correspondents first, capped so a prefix match still wins */
      add_match (matches, entry->name, key, GCAL_CONTACT_ORIGIN_MAIL_HISTORY,
                 200 + MIN (entry->count, 500) + (prefix ? 1000 : 0));
    }
  g_mutex_unlock (&cache_mutex);

  if (g_task_return_error_if_cancelled (task))
    return;

  /* Address books */
  sexp = build_book_query (lower_text);
  books = e_source_registry_list_enabled (data->registry, E_SOURCE_EXTENSION_ADDRESS_BOOK);

  for (GList *l = books; l; l = l->next)
    {
      g_autoptr (EBookClient) client = NULL;
      g_autoslist (EContact) contacts = NULL;
      g_autoptr (GError) error = NULL;
      gboolean directory;

      g_mutex_lock (&cache_mutex);
      client = ensure_book_client (l->data, cancellable);
      g_mutex_unlock (&cache_mutex);

      if (!client)
        continue;

      directory = book_is_directory (client);
      if (directory && g_utf8_strlen (lower_text, -1) < DIRECTORY_MIN_CHARS)
        continue;

      if (!e_book_client_get_contacts_sync (client, sexp, &contacts, cancellable, &error))
        {
          if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_debug ("Search in '%s' failed: %s", e_source_get_display_name (l->data), error->message);
          continue;
        }

      for (GSList *c = contacts; c; c = c->next)
        {
          GList *emails = e_contact_get (c->data, E_CONTACT_EMAIL);
          const gchar *name = e_contact_get_const (c->data, E_CONTACT_FULL_NAME);

          for (GList *e = emails; e; e = e->next)
            {
              gboolean prefix = FALSE;

              if (!text_matches (lower_text, name, e->data, &prefix))
                continue;

              add_match (matches, name, e->data,
                         directory ? GCAL_CONTACT_ORIGIN_DIRECTORY : GCAL_CONTACT_ORIGIN_ADDRESS_BOOK,
                         (directory ? 100 : 800) + (prefix ? 1000 : 0));
            }

          g_list_free_full (emails, g_free);
        }

      if (g_task_return_error_if_cancelled (task))
        return;
    }

  results = g_ptr_array_new_with_free_func ((GDestroyNotify) gcal_contact_match_free);

  g_hash_table_iter_init (&iter, matches);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      g_hash_table_iter_steal (&iter);
      g_ptr_array_add (results, value);
    }

  g_ptr_array_sort (results, compare_matches);

  if (data->max_results > 0 && results->len > data->max_results)
    g_ptr_array_set_size (results, data->max_results);

  g_task_return_pointer (task, results, (GDestroyNotify) g_ptr_array_unref);
}


/*
 * Public API
 */

/**
 * gcal_contact_search:
 * @manager: a #GcalManager
 * @text: what the user typed
 * @max_results: maximum number of results, 0 for no limit
 * @cancellable: (nullable): a #GCancellable
 * @callback: callback
 * @user_data: data for @callback
 *
 * Looks for people matching @text in the address books and the mail history.
 */
void
gcal_contact_search (GcalManager         *manager,
                     const gchar         *text,
                     guint                max_results,
                     GCancellable        *cancellable,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
  g_autoptr (GTask) task = NULL;
  SearchData *data;

  g_return_if_fail (GCAL_IS_MANAGER (manager));

  data = g_new0 (SearchData, 1);
  data->registry = g_object_ref (gcal_manager_get_source_registry (manager));
  data->text = g_strdup (text ? text : "");
  data->max_results = max_results;

  task = g_task_new (manager, cancellable, callback, user_data);
  g_task_set_source_tag (task, gcal_contact_search);
  g_task_set_task_data (task, data, (GDestroyNotify) search_data_free);
  g_task_set_return_on_cancel (task, TRUE);
  g_task_run_in_thread (task, search_thread);
}

/**
 * gcal_contact_search_finish:
 * @result: a #GAsyncResult
 * @error: return location for a #GError
 *
 * Returns: (transfer full) (element-type GcalContactMatch): the matches, best first
 */
GPtrArray*
gcal_contact_search_finish (GAsyncResult  *result,
                            GError       **error)
{
  g_return_val_if_fail (g_task_get_source_tag (G_TASK (result)) == gcal_contact_search, NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
