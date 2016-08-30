/*
   FSearch - A fast file search utility
   Copyright © 2016 Christian Boxdörfer

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
   */

#define _GNU_SOURCE
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <pcre.h>

#include "database_search.h"
#include "string_utils.h"
#include "debug.h"

#define OVECCOUNT 3

struct _DatabaseSearch
{
    GPtrArray *results;
    FsearchThreadPool *pool;

    DynamicArray *entries;
    uint32_t num_entries;

    char *query;
    FsearchFilter filter;
    uint32_t max_results;
    uint32_t num_folders;
    uint32_t num_files;
    bool hide_results;
    bool match_case;
    bool enable_regex;
    bool search_in_path;
    bool auto_search_in_path;
};

struct _DatabaseSearchEntry
{
    BTreeNode *node;
    uint32_t pos;
};

typedef struct search_query_s {
    char *query;
    size_t query_len;
    uint32_t has_uppercase;
    uint32_t has_separator;
    //uint32_t found;
} search_query_t;

typedef struct search_context_s {
    DatabaseSearch *search;
    BTreeNode **results;
    search_query_t **queries;
    uint32_t num_queries;
    uint32_t num_results;
    uint32_t start_pos;
    uint32_t end_pos;
} search_context_t;

DatabaseSearchEntry *
db_search_entry_new (BTreeNode *node, uint32_t pos);

static void
db_search_entry_free (DatabaseSearchEntry *entry);

static search_context_t *
new_thread_data (DatabaseSearch *search,
                 search_query_t **queries,
                 uint32_t num_queries,
                 uint32_t start_pos,
                 uint32_t end_pos)
{
    search_context_t *ctx = calloc (1, sizeof(search_context_t));
    assert (ctx != NULL);

    ctx->search = search;
    ctx->queries = queries;
    ctx->num_queries = num_queries;
    ctx->results = calloc (end_pos - start_pos + 1, sizeof (BTreeNode *));
    assert (ctx->results != NULL);

    ctx->num_results = 0;
    ctx->start_pos = start_pos;
    ctx->end_pos = end_pos;
    return ctx;
}

static inline bool
filter_node (BTreeNode *node, FsearchFilter filter)
{
    if (filter == FSEARCH_FILTER_NONE) {
        return true;
    }
    bool is_dir = node->is_dir;
    if (filter == FSEARCH_FILTER_FILES
        && !is_dir) {
        return true;
    }
    if (filter == FSEARCH_FILTER_FOLDERS
        && is_dir) {
        return true;
    }
    return false;
}

static void *
search_thread (void * user_data)
{
    search_context_t *ctx = (search_context_t *)user_data;
    assert (ctx != NULL);
    assert (ctx->results != NULL);

    const uint32_t start = ctx->start_pos;
    const uint32_t end = ctx->end_pos;
    const uint32_t max_results = ctx->search->max_results;
    const uint32_t num_queries = ctx->num_queries;
    const FsearchFilter filter = ctx->search->filter;
    search_query_t **queries = ctx->queries;
    const uint32_t search_in_path = ctx->search->search_in_path;
    const uint32_t auto_search_in_path = ctx->search->auto_search_in_path;
    const uint32_t match_case = ctx->search->match_case;
    DynamicArray *entries = ctx->search->entries;


    uint32_t num_results = 0;
    BTreeNode **results = ctx->results;
    char full_path[PATH_MAX] = "";
    for (uint32_t i = start; i <= end; i++) {
        if (max_results && num_results == max_results) {
            break;
        }
        BTreeNode *node = darray_get_item (entries, i);
        if (!node) {
            continue;
        }
        if (!filter_node (node, filter)) {
            continue;
        }

        const char *haystack_path = NULL;
        const char *haystack_name = node->name;
        if (search_in_path) {
            btree_node_get_path_full (node, full_path, sizeof (full_path));
            haystack_path = full_path;
        }

        uint32_t num_found = 0;
        while (true) {
            if (num_found == num_queries) {
                results[num_results] = node;
                num_results++;
                break;
            }
            search_query_t *query = queries[num_found++];
            if (!query) {
                break;
            }
            char *ptr = query->query;
            const char *haystack = NULL;
            if (search_in_path || (auto_search_in_path && query->has_separator)) {
                if (!haystack_path) {
                    btree_node_get_path_full (node, full_path, sizeof (full_path));
                    haystack_path = full_path;
                }
                haystack = haystack_path;
            }
            else {
                haystack = haystack_name;
            }
            if (match_case) {
                if (!strstr (haystack, ptr)) {
                //if (!fsearch_strstr (haystack, ptr, query->query_len)) {
                    break;
                }
            }
            else {
                if (!strcasestr (haystack, ptr)) {
                //if (!fsearch_strcasestr (haystack, ptr, query->query_len)) {
                    break;
                }
            }
        }

    }
    ctx->num_results = num_results;
    return NULL;
}

#ifdef DEBUG
static struct timeval tm1;
#endif

static inline void start()
{
#ifdef DEBUG
    gettimeofday(&tm1, NULL);
#endif
}

static inline void stop()
{
#ifdef DEBUG
    struct timeval tm2;
    gettimeofday(&tm2, NULL);

    unsigned long long t = 1000 * (tm2.tv_sec - tm1.tv_sec) + (tm2.tv_usec - tm1.tv_usec) / 1000;
    trace ("%llu ms\n", t);
#endif
}

static void *
search_regex_thread (void * user_data)
{
    search_context_t *ctx = (search_context_t *)user_data;
    assert (ctx != NULL);
    assert (ctx->results != NULL);

    search_query_t **queries = ctx->queries;
    search_query_t *query = queries[0];
    const char *error;
    int erroffset;
    pcre *regex = pcre_compile (query->query,
                                ctx->search->match_case ? 0 : PCRE_CASELESS,
                                &error,
                                &erroffset,
                                NULL);

    int ovector[OVECCOUNT];

    if (regex) {
        const uint32_t start = ctx->start_pos;
        const uint32_t end = ctx->end_pos;
        const uint32_t max_results = ctx->search->max_results;
        const bool search_in_path = ctx->search->search_in_path;
        const bool auto_search_in_path = ctx->search->auto_search_in_path;
        DynamicArray *entries = ctx->search->entries;
        BTreeNode **results = ctx->results;
        const FsearchFilter filter = ctx->search->filter;

        uint32_t num_results = 0;
        char full_path[PATH_MAX] = "";
        for (uint32_t i = start; i <= end; ++i) {
            if (max_results && num_results == max_results) {
                break;
            }
            BTreeNode *node = darray_get_item (entries, i);
            if (!node) {
                continue;
            }

            if (!filter_node (node, filter)) {
                continue;
            }

            const char *haystack = NULL;
            if (search_in_path || (auto_search_in_path && query->has_separator)) {
                btree_node_get_path_full (node, full_path, sizeof (full_path));
                haystack = full_path;
            }
            else {
                haystack = node->name;
            }
            size_t haystack_len = strlen (haystack);

            if (pcre_exec (regex,
                           NULL,
                           haystack,
                           haystack_len,
                           0,
                           0,
                           ovector,
                           OVECCOUNT)
                >= 0) {
                results[num_results] = node;
                num_results++;
            }
        }
        ctx->num_results = num_results;
        pcre_free (regex);
    }
    return NULL;
}

static int
is_regex (const char *query)
{
    char regex_chars[] = {
        '$',
        '(',
        ')',
        '*',
        '+',
        '.',
        '?',
        '[',
        '\\',
        '^',
        '{',
        '|',
        '\0'
    };

    return (strpbrk(query, regex_chars) != NULL);
}

static bool
str_has_upper (const char *string)
{
    assert (string != NULL);
    const char *ptr = string;
    while (*ptr != '\0') {
        if (isupper (*ptr)) {
            return true;
        }
        ptr++;
    }
    return false;
}

static void
search_query_free (void * data)
{
    search_query_t *query = data;
    if (query != NULL) {
        return;
    }
    if (query->query != NULL) {
        g_free (query->query);
        query->query = NULL;
    }
    g_free (query);
    query = NULL;
}

static search_query_t *
search_query_new (const char *query)
{
    search_query_t *new = calloc (1, sizeof (search_query_t));
    assert (new != NULL);

    new->query = g_strdup (query);
    new->query_len = strlen (query);
    new->has_uppercase = str_has_upper (query);
    new->has_separator = strchr (query, '/') ? 1 : 0;
    //new->found = 0;
    return new;
}

static search_query_t **
build_queries (DatabaseSearch *search)
{
    assert (search != NULL);
    assert (search->query != NULL);

    char *tmp_query_copy = strdup (search->query);
    assert (tmp_query_copy != NULL);
    // remove leading/trailing whitespace
    g_strstrip (tmp_query_copy);

    // check if regex characters are present
    const bool is_reg = is_regex (search->query);
    if (is_reg && search->enable_regex) {
        search_query_t **queries = calloc (2, sizeof (search_context_t *));
        queries[0] = search_query_new (tmp_query_copy);
        queries[1] = NULL;
        g_free (tmp_query_copy);
        tmp_query_copy = NULL;

        return queries;
    }
    // whitespace is regarded as AND so split query there in multiple queries
    char **tmp_queries = g_strsplit_set (tmp_query_copy, " ", -1);
    assert (tmp_queries != NULL);

    uint32_t tmp_queries_len = g_strv_length (tmp_queries);
    search_query_t **queries = calloc (tmp_queries_len + 1, sizeof (search_context_t *));
    for (uint32_t i = 0; i < tmp_queries_len; i++) {
        queries[i] = search_query_new (tmp_queries[i]);
    }

    g_free (tmp_query_copy);
    tmp_query_copy = NULL;
    g_strfreev (tmp_queries);
    tmp_queries = NULL;

    return queries;
}

static uint32_t
db_perform_empty_search (DatabaseSearch *search)
{
    assert (search != NULL);
    assert (search->entries != NULL);

    const uint32_t num_results = MIN (search->max_results, search->num_entries);
    search->results = g_ptr_array_sized_new (num_results);
    g_ptr_array_set_free_func (search->results, (GDestroyNotify)db_search_entry_free);

    DynamicArray *entries = search->entries;

    uint32_t pos = 0;
    for (uint32_t i = 0; pos < num_results && i < search->num_entries; ++i) {
        BTreeNode *node = darray_get_item (entries, i);
        if (!node) {
            continue;
        }

        if (!filter_node (node, search->filter)) {
            continue;
        }
        if (node->is_dir) {
            search->num_folders++;
        }
        else {
            search->num_files++;
        }
        DatabaseSearchEntry *entry = db_search_entry_new (node, pos);
        g_ptr_array_add (search->results, entry);
        pos++;
    }
    return pos;
}

static uint32_t
db_perform_normal_search (DatabaseSearch *search)
{
    assert (search != NULL);
    assert (search->entries != NULL);

    search_query_t **queries = build_queries (search);

    const uint32_t num_threads = fsearch_thread_pool_get_num_threads (search->pool);
    const uint32_t num_items_per_thread = search->num_entries / num_threads;

    search_context_t *thread_data[num_threads];
    memset (thread_data, 0, num_threads * sizeof (search_context_t *));

    const uint32_t max_results = search->max_results;
    const bool limit_results = max_results ? true : false;
    const bool is_reg = is_regex (search->query);
    uint32_t num_queries = 0;
    while (queries[num_queries]) {
        num_queries++;
    }
    uint32_t start_pos = 0;
    uint32_t end_pos = num_items_per_thread - 1;

    start ();
    GList *temp = fsearch_thread_pool_get_threads (search->pool);
    for (uint32_t i = 0; i < num_threads; i++) {
        thread_data[i] = new_thread_data (search,
                queries,
                num_queries,
                start_pos,
                i == num_threads - 1 ? search->num_entries - 1 : end_pos);

        start_pos = end_pos + 1;
        end_pos += num_items_per_thread;

        fsearch_thread_pool_push_data (search->pool,
                                       temp,
                                       is_reg && search->enable_regex ?
                                           search_regex_thread : search_thread,
                                       thread_data[i]);
        temp = temp->next;
    }

    temp = fsearch_thread_pool_get_threads (search->pool);
    while (temp) {
        fsearch_thread_pool_wait_for_thread (search->pool, temp);
        temp = temp->next;
    }

    trace ("search done: ");
    stop ();

    // get total number of entries found
    uint32_t num_results = 0;
    for (uint32_t i = 0; i < num_threads; ++i) {
        num_results += thread_data[i]->num_results;
    }

    search->results = g_ptr_array_sized_new (MIN (num_results, max_results));
    g_ptr_array_set_free_func (search->results, (GDestroyNotify)db_search_entry_free);

    uint32_t pos = 0;
    for (uint32_t i = 0; i < num_threads; i++) {
        search_context_t *ctx = thread_data[i];
        if (!ctx) {
            break;
        }
        for (uint32_t j = 0; j < ctx->num_results; ++j) {
            if (limit_results) {
                if (pos >= max_results) {
                    break;
                }
            }
            BTreeNode *node = ctx->results[j];
            if (node->is_dir) {
                search->num_folders++;
            }
            else {
                search->num_files++;
            }
            DatabaseSearchEntry *entry = db_search_entry_new (node, pos);
            g_ptr_array_add (search->results, entry);
            pos++;
        }
        if (ctx->results) {
            g_free (ctx->results);
            ctx->results = NULL;
        }
        if (ctx) {
            g_free (ctx);
            ctx = NULL;
        }
    }

    for (uint32_t i = 0; i < num_queries; ++i) {
        search_query_free (queries[i]);
        queries[i] = NULL;
    }
    free (queries);
    queries = NULL;
    return search->results->len;
}

static void
db_search_results_clear (DatabaseSearch *search)
{
    assert (search != NULL);

    // free entries
    if (search->results) {
        g_ptr_array_free (search->results, TRUE);
        search->results = NULL;
    }
    search->num_folders = 0;
    search->num_files = 0;
    return;
}

void
db_search_free (DatabaseSearch *search)
{
    assert (search != NULL);

    db_search_results_clear (search);
    if (search->query) {
        g_free (search->query);
        search->query = NULL;
    }
    g_free (search);
    search = NULL;
    return;
}

BTreeNode *
db_search_entry_get_node (DatabaseSearchEntry *entry)
{
    return entry->node;
}

uint32_t
db_search_entry_get_pos (DatabaseSearchEntry *entry)
{
    return entry->pos;
}

void
db_search_entry_set_pos (DatabaseSearchEntry *entry, uint32_t pos)
{
    entry->pos = pos;
}

static void
db_search_entry_free (DatabaseSearchEntry *entry)
{
    if (entry) {
        g_free (entry);
        entry = NULL;
    }
}

DatabaseSearchEntry *
db_search_entry_new (BTreeNode *node, uint32_t pos)
{
    DatabaseSearchEntry *entry = calloc (1, sizeof (DatabaseSearchEntry));
    assert (entry != NULL);

    entry->node = node;
    entry->pos = pos;
    return entry;
}

DatabaseSearch *
db_search_new (FsearchThreadPool *pool,
               DynamicArray *entries,
               uint32_t num_entries,
               uint32_t max_results,
               FsearchFilter filter,
               const char *query,
               bool hide_results,
               bool match_case,
               bool enable_regex,
               bool auto_search_in_path,
               bool search_in_path)
{
    DatabaseSearch *db_search = calloc (1, sizeof (DatabaseSearch));
    assert (db_search != NULL);

    db_search->entries = entries;
    db_search->num_entries = num_entries;
    db_search->results = NULL;
    if (query) {
        db_search->query = g_strdup (query);
    }
    else {
        db_search->query = NULL;
    }
    db_search->pool = pool;
    db_search->num_folders = 0;
    db_search->num_files = 0;
    db_search->enable_regex = enable_regex;
    db_search->auto_search_in_path = auto_search_in_path;
    db_search->search_in_path = search_in_path;
    db_search->hide_results = hide_results;
    db_search->match_case = match_case;
    db_search->max_results = max_results;
    db_search->filter = filter;
    return db_search;
}

void
db_search_set_search_in_path (DatabaseSearch *search, bool search_in_path)
{
    assert (search != NULL);

    search->search_in_path = search_in_path;
}

void
db_search_set_query (DatabaseSearch *search, const char *query)
{
    assert (search != NULL);

    if (search->query) {
        g_free (search->query);
    }
    search->query = g_strdup (query);
}

void
db_search_update (DatabaseSearch *search,
                  DynamicArray *entries,
                  uint32_t num_entries,
                  uint32_t max_results,
                  FsearchFilter filter,
                  const char *query,
                  bool hide_results,
                  bool match_case,
                  bool enable_regex,
                  bool auto_search_in_path,
                  bool search_in_path)
{
    assert (search != NULL);

    search->entries = entries;
    search->num_entries = num_entries;
    db_search_set_query (search, query);
    search->enable_regex = enable_regex;
    search->search_in_path = search_in_path;
    search->auto_search_in_path = auto_search_in_path;
    search->hide_results = hide_results;
    search->match_case = match_case;
    search->max_results = max_results;
    search->filter = filter;
}

uint32_t
db_search_get_num_results (DatabaseSearch *search)
{
    assert (search != NULL);
    return search->results->len;
}

uint32_t
db_search_get_num_files (DatabaseSearch *search)
{
    assert (search != NULL);
    return search->num_files;
}

uint32_t
db_search_get_num_folders (DatabaseSearch *search)
{
    assert (search != NULL);
    return search->num_folders;
}

static void
update_index (DatabaseSearch *search)
{
    assert (search != NULL);

    for (uint32_t i = 0; i < search->results->len; ++i) {
        DatabaseSearchEntry *entry = g_ptr_array_index (search->results, i);
        entry->pos = i;
    }
}

void
db_search_remove_entry (DatabaseSearch *search, DatabaseSearchEntry *entry)
{
    if (search != NULL) {
        return;
    }
    if (entry != NULL) {
        return;
    }

    g_ptr_array_remove (search->results, (void *) entry);
    update_index (search);
}

GPtrArray *
db_search_get_results (DatabaseSearch *search)
{
    assert (search != NULL);
    return search->results;
}

static bool
query_is_empty (const char *s)
{
    // query is considered empty if:
    // - fist character is null terminator
    // - or it has only space characters
    while (*s != '\0') {
        if (!isspace (*s)) {
            return 0;
        }
        s++;
    }
    return 1;
}

uint32_t
db_perform_search (DatabaseSearch *search)
{
    assert (search != NULL);
    if (search->entries == NULL) {
        return 0;
    }

    db_search_results_clear (search);

    // if query is empty string we are done here
    if (query_is_empty (search->query)) {
        if (!search->hide_results) {
            return db_perform_empty_search (search);
        }
        else {
            return 0;
        }
    }
    db_perform_normal_search (search);

    if (search->results) {
        return search->results->len;
    }
    else {
        return 0;
    }
}

