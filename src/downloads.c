/*
 * $Id$
 *
 * Copyright (c) 2001-2002, Raphael Manfredi
 *
 * Handle downloads.
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

#include "gnutella.h"
#include "misc.h"
#include "downloads_gui.h"
#include "sockets.h"
#include "downloads.h"
#include "hosts.h"
#include "getline.h"
#include "header.h"
#include "routing.h"
#include "url.h"
#include "routing.h"
#include "gmsg.h"
#include "bsched.h"
#include "regex.h"
#include "getdate.h"
#include "atoms.h"
#include "huge.h"
#include "base32.h"
#include "dmesh.h"
#include "http.h"
#include "version.h"

#include "gnet_property_priv.h"
#include "settings.h"
#include "nodes.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>			/* For ctime() */

#define DOWNLOAD_RECV_BUFSIZE		114688		/* 112K */

static GSList *sl_downloads = NULL;	/* All downloads (queued + unqueued) */
GSList *sl_unqueued = NULL;			/* Unqueued downloads only */
static gchar dl_tmp[4096];
static gint queue_frozen = 0;

static GHashTable *pushed_downloads = 0;

static void download_add_to_list(struct download *d, enum dl_list idx);
static gboolean send_push_request(gchar *, guint32, guint16);
static void download_start_restart_timer(struct download *d);
static void download_read(gpointer data, gint source, GdkInputCondition cond);
static void download_request(struct download *d, header_t *header);
static void download_push_ready(struct download *d, getline_t *empty);
static void download_push_remove(struct download *d);
static void download_push(struct download *d, gboolean on_timeout);
static void download_store(void);
static void download_retrieve(void);

/*
 * Download structures.
 *
 * This `dl_key' is inserted in the `dl_by_host' hash table were we find a
 * `dl_server' structure describing all the downloads for the given host.
 * All `dl_server' structures are also inserted in the `dl_by_time' sorted list,
 * where hosts to try first are listed at the head.
 *
 * The `dl_count_by_name' and `dl_count_by_sha1' hash tables are indexed
 * resepectively by name and sha1, and count the amount of downloads scheduled.
 */

static GHashTable *dl_by_host = NULL;
static GList *dl_by_time = NULL;
static GHashTable *dl_count_by_name = NULL;
static GHashTable *dl_count_by_sha1 = NULL;

static gint dl_establishing = 0;		/* Establishing downloads */
static gint dl_active = 0;				/* Active downloads */

#define count_running_downloads()	(dl_establishing + dl_active)
#define count_running_on_server(s)	(s->count[DL_LIST_RUNNING])

/*
 * This structure is used to encapsulate the various arguments required
 * by the header parsing I/O callback.
 */
struct io_header {
	struct download *download;
	header_t *header;
	getline_t *getline;
	void (*process_header)(struct io_header *);
	gint flags;
};

#define IO_STATUS_LINE		0x00000002	/* First line is a status line */
#define IO_ONE_LINE			0x00000004	/* Get one line only, then process */

/* ----------------------------------------- */

/*
 * dl_key_hash
 *
 * Hashing of a `dl_key' structure.
 */
static guint dl_key_hash(gconstpointer key)
{
	struct dl_key *k = (struct dl_key *) key;
	guint hash;
	extern guint guid_hash(gconstpointer key);

	hash = guid_hash(k->guid);
	hash ^= k->ip;
	hash ^= (k->port << 16) | k->port;

	return hash;
}

/*
 * dl_key_eq
 *
 * Comparison of `dl_key' structures.
 */
static gint dl_key_eq(gconstpointer a, gconstpointer b)
{
	struct dl_key *ak = (struct dl_key *) a;
	struct dl_key *bk = (struct dl_key *) b;
	extern gint guid_eq(gconstpointer a, gconstpointer b);

	return ak->ip == bk->ip &&
		ak->port == bk->port &&
		guid_eq(ak->guid, bk->guid);
}

/*
 * dl_server_retry_cmp
 *
 * Compare two `dl_server' structures based on the `retry_after' field.
 * The smaller that time, the smaller the structure is.
 */
static gint dl_server_retry_cmp(gconstpointer a, gconstpointer b)
{
	struct dl_server *as = (struct dl_server *) a;
	struct dl_server *bs = (struct dl_server *) b;

	if (as->retry_after == bs->retry_after)
		return 0;

	return as->retry_after < bs->retry_after ? -1 : +1;
}

/* ----------------------------------------- */

/*
 * download_init
 *
 * Initialize downloading data structures.
 */
void download_init(void)
{
	pushed_downloads = g_hash_table_new(g_str_hash, g_str_equal);
	dl_by_host = g_hash_table_new(dl_key_hash, dl_key_eq);
	dl_count_by_name = g_hash_table_new(g_str_hash, g_str_equal);
	dl_count_by_sha1 = g_hash_table_new(g_str_hash, g_str_equal);
	file_info_retrieve();
	download_retrieve();
}

/* ----------------------------------------- */

/*
 * download_timer
 *
 * Download heartbeat timer.
 */
void download_timer(time_t now)
{
	GSList *l = sl_unqueued;		/* Only downloads not in the queue */

	if (queue_frozen > 0)
		return;

	while (l) {
		struct download *d = (struct download *) l->data;
		guint32 t;

		l = l->next;

		switch (d->status) {
		case GTA_DL_RECEIVING:
		case GTA_DL_HEADERS:
		case GTA_DL_PUSH_SENT:
		case GTA_DL_CONNECTING:
		case GTA_DL_REQ_SENT:
		case GTA_DL_FALLBACK:

			if (!is_inet_connected) {
				download_queue(d, "No longer connected");
				break;
			}

			switch (d->status) {
			case GTA_DL_PUSH_SENT:
			case GTA_DL_FALLBACK:
				t = download_push_sent_timeout;
				break;
			case GTA_DL_CONNECTING:
				t = download_connecting_timeout;
				break;
			default:
				t = download_connected_timeout;
				break;
			}

			if (now - d->last_update > t) {
				if (d->status == GTA_DL_CONNECTING)
					download_fallback_to_push(d, TRUE, FALSE);
				else {
					if (++d->retries <= download_max_retries)
						download_retry(d);
					else
						download_stop(d, GTA_DL_ERROR, "Timeout");
				}
			} else if (now != d->last_gui_update)
				gui_update_download(d, TRUE);
			break;
		case GTA_DL_TIMEOUT_WAIT:
			if (!is_inet_connected) {
				download_queue(d, "No longer connected");
				break;
			}

			if (now - d->last_update > d->timeout_delay)
				download_start(d, TRUE);
			else
				gui_update_download(d, FALSE);
			break;
		case GTA_DL_COMPLETED:
		case GTA_DL_ABORTED:
		case GTA_DL_ERROR:
		case GTA_DL_STOPPED:
			break;
		case GTA_DL_QUEUED:
			g_error("found queued download in sl_unqueued list: \"%s\"",
				d->file_name);
			break;
		default:
			g_warning("Hmm... new download state %d not handled for \"%s\"",
				d->status, d->file_name);
			break;
		}
	}

	if (clear_downloads)
		download_clear_stopped(FALSE, FALSE);

	/* Dequeuing */
	if (is_inet_connected)
		download_pickup_queued();
}

/*
 * io_free
 *
 * Free the opaque I/O data.
 */
static void io_free(gpointer opaque)
{
	struct io_header *ih = (struct io_header *) opaque;

	g_assert(ih);
	g_assert(ih->download->io_opaque == opaque);

	ih->download->io_opaque = NULL;

	if (ih->header)
		header_free(ih->header);
	if (ih->getline)
		getline_free(ih->getline);

	g_free(ih);
}

/* ----------------------------------------- */

/*
 * allocate_server
 *
 * Allocate new server structure.
 */
static struct dl_server *allocate_server(guchar *guid, guint32 ip, guint16 port)
{
	struct dl_key *key;
	struct dl_server *server;

	key = g_malloc(sizeof(*key));
	key->ip = ip;
	key->port = port;
	key->guid = atom_guid_get(guid);

	server = g_malloc0(sizeof(*server));
	server->key = key;

	g_hash_table_insert(dl_by_host, key, server);
	dl_by_time = g_list_insert_sorted(dl_by_time, server, dl_server_retry_cmp);

	return server;
}

/*
 * free_server
 *
 * Free server structure.
 */
static void free_server(struct dl_server *server)
{
	dl_by_time = g_list_remove(dl_by_time, server);
	g_hash_table_remove(dl_by_host, server->key);
	if (server->vendor)
		atom_str_free(server->vendor);
	atom_guid_free(server->key->guid);
	g_free(server->key);
	g_free(server);
}

/*
 * get_server
 *
 * Fetch server entry identified by GUID, IP:port.
 * Returns NULL if not found.
 */
static struct dl_server *get_server(guchar *guid, guint32 ip, guint16 port)
{
	struct dl_key key;

	g_assert(guid);

	key.guid = guid;
	key.ip = ip;
	key.port = port;

	return (struct dl_server *) g_hash_table_lookup(dl_by_host, &key);
}

/*
 * download_server_nopush
 *
 * Check whether we can safely ignore Push indication for this server,
 * identified by its GUID, IP and port.
 */
gboolean download_server_nopush(guchar *guid, guint32 ip, guint16 port)
{
	struct dl_server *server = get_server(guid, ip, port);

	if (server == NULL)
		return FALSE;

	/* 
	 * Returns true if we already made a direct connection to this server.
	 */

	return server->attrs & DLS_A_PUSH_IGN;
}

/*
 * count_running_downloads_with_name
 *
 * How many downloads with same filename are running (active or establishing)?
 */
static guint count_running_downloads_with_name(const char *name)
{
	return (guint) g_hash_table_lookup(dl_count_by_name, name);
}

/*
 * downloads_with_name_inc
 *
 * Add one to the amount of downloads running and bearing the filename.
 */
static void downloads_with_name_inc(const gchar *name)
{
	guint val;

	val = (guint) g_hash_table_lookup(dl_count_by_name, name);
	g_hash_table_insert(dl_count_by_name, (gchar *) name, (gpointer) (val + 1));
}

/*
 * downloads_with_name_dec
 *
 * Remove one from the amount of downloads running and bearing the filename.
 */
static void downloads_with_name_dec(const gchar *name)
{
	guint val;

	val = (guint) g_hash_table_lookup(dl_count_by_name, name);

	g_assert(val);		/* Cannot decrement something not present */

	if (val > 1)
		g_hash_table_insert(dl_count_by_name,
			(gchar *) name, (gpointer) (val - 1));
	else
		g_hash_table_remove(dl_count_by_name, name);
}

/*
 * count_running_downloads_with_sha1		-- XXX UNUSED for now
 *
 * How many downloads with same SHA1 are running (active or establishing)?
 */
static guint32 count_running_downloads_with_sha1(const guchar *sha1)
{
	return (guint) g_hash_table_lookup(dl_count_by_sha1, sha1);
}

/*
 * has_same_download
 *
 * Check whether we already have an identical (same file, same host)
 * running or queued download.
 *
 * Returns found active download, or NULL if we have no such download yet.
 */
static struct download *has_same_download(
	gchar *file, gchar *guid, guint32 ip, guint16 port)
{
	struct dl_server *server = get_server(guid, ip, port);
	GList *l;
	gint n;
	enum dl_list listnum[] = { DL_LIST_RUNNING, DL_LIST_WAITING };

	if (server == NULL)
		return NULL;

	for (n = 0; n < sizeof(listnum) / sizeof(listnum[0]); n++) {
		for (l = server->list[n]; l; l = l->next) {
			struct download *d = (struct download *) l->data;
			g_assert(!DOWNLOAD_IS_STOPPED(d));
			if (0 == strcmp(file, d->file_name))
				return d;
		}
	}

	return NULL;
}

/*
 * download_file_exists
 *
 * Returns whether the download file exists in the temporary directory.
 */
gboolean download_file_exists(struct download *d)
{
	gchar path[2048];
	struct stat buf;

	g_snprintf(path, sizeof(path), "%s/%s",
		d->file_info->path, d->file_info->file_name);

	if (-1 == stat(path, &buf))
		return FALSE;

	return TRUE;
}

/*
 * download_remove_file
 *
 * Remove temporary download file.
 */
void download_remove_file(struct download *d)
{
	gchar path[2048];

	g_snprintf(path, sizeof(path), "%s/%s",
		d->file_info->path, d->file_info->file_name);

	if (-1 == unlink(path))
		g_warning("cannot unlink \"%s\": %s", path, g_strerror(errno));
}

/*
 * queue_remove_downloads_with_file
 *
 * Removes all downloads that point to the file_info struct.
 * This replaces queue_remove_identical, since downloads can
 * have different file names but still point to the same file.
 *
 */
static void queue_remove_downloads_with_file(struct dl_file_info *fi)
{
	GSList *to_remove = NULL;
	GSList *l;

	for (l = sl_downloads; l; l = l->next) {
		struct download *d = (struct download *) l->data;

		if (d->file_info != fi) continue;
		
		if ((d->status == GTA_DL_QUEUED) || (d->status == GTA_DL_TIMEOUT_WAIT))
			to_remove = g_slist_prepend(to_remove, d);
	}
	
	for (l = to_remove; l; l = l->next)
		download_free((struct download *)l->data);

	g_slist_free(to_remove);
}


/*
 * queue_remove_identical
 *
 * Remove all queued downloads bearing given name or which have common SHA1.
 * During traversal, we also collect SHA1 of identically named files and will
 * also remove them.
 *
 * Only entries smaller or equal than `size' are removed.
 *
 * NB: we don't remove second-order aliases.  For instance, download "a" which
 * has a SHA1 of XA1 is removed.  There is also another "a" in the queue whose
 * SHA1 is XA2.  Then we'll remove all XA1 and XA2, but if we find a "b"
 * whose SHA1 is also XA2, we won't remove all "b".
 */
static void queue_remove_identical(
	const gchar *name, guchar *sha1, guint32 size)
{
	GSList *to_remove = NULL;
	GSList *l;
	gint bigger = 0;
	extern guint sha1_hash(gconstpointer key);
	extern gint sha1_eq(gconstpointer a, gconstpointer b);
	GHashTable *seen_sha1 = g_hash_table_new(sha1_hash, sha1_eq);

	g_assert(name);

	if (dbg > 3)
		printf("queue_remove_identical: for \"%s\" (%s) <= %d bytes\n",
			name, sha1 ? sha1_base32(sha1) : "", size);

	/*
	 * If SHA1 given, record it.
	 *
	 * Note that we don't need to clone the `sha1' atom since we'll dispose
	 * of the `seen_sha1' table before actually removing the downloads.
	 */

	if (sha1)
		g_hash_table_insert(seen_sha1, sha1, (gpointer) 1);

	/*
	 * First pass, by name: collect SHA1, mark identical downloads.
	 */

	for (l = sl_downloads; l; l = l->next) {
		struct download *d = (struct download *) l->data;

		d->flags &= ~DL_F_MARK;			/* Clear traversal mark */

		switch (d->status) {
		case GTA_DL_QUEUED:
		case GTA_DL_TIMEOUT_WAIT:
			if (0 == strcmp(name, d->file_info->file_name)) {
				d->flags |= DL_F_MARK;
				if (d->file_info->size <= size)
					to_remove = g_slist_prepend(to_remove, d);
				else
					bigger++;
				if (d->sha1 && !g_hash_table_lookup(seen_sha1, d->sha1))
					g_hash_table_insert(seen_sha1, d->sha1, (gpointer) 1);
			}
			break;
		default:
			break;
		}
	}

	/*
	 * Second pass, by collected SHA1.
	 * We only scan items not marked by first pass.
	 */

	for (l = sl_downloads; l; l = l->next) {
		struct download *d = (struct download *) l->data;

		if (d->flags & DL_F_MARK)		/* Already processed above */
			continue;

		switch (d->status) {
		case GTA_DL_QUEUED:
		case GTA_DL_TIMEOUT_WAIT:
			if (d->sha1 && g_hash_table_lookup(seen_sha1, d->sha1)) {
				if (d->file_info->size <= size)
					to_remove = g_slist_prepend(to_remove, d);
				else
					bigger++;
			}
			break;
		default:
			break;
		}
	}

	g_hash_table_destroy(seen_sha1);	/* No allocated key/value */

	for (l = to_remove; l; l = l->next) {
		struct download *d = (struct download *) l->data;

		if (dbg > 3)
			printf("queue_remove_identical: removing \"%s\" (%s) %d bytes\n",
				d->file_name, d->sha1 ? sha1_base32(d->sha1) : "",
				d->file_info->size);

		download_free(d);
	}

	g_slist_free(to_remove);

	if (bigger)
		g_warning("file \"%s\" is incomplete: %d bigger entr%s in queue",
			name, bigger, bigger == 1 ? "y" : "ies");
}

/*
 * download_remove_all_from_peer
 *
 * Remove all downloads to a given peer from the download queue
 * and abort all conenctions to peer in the active download list.
 * Return the number of removed downloads.
 */
gint download_remove_all_from_peer(const gchar *guid, guint32 ip, guint16 port)
{
	struct dl_server *server = get_server((gchar *) guid, ip, port);
	GSList *l;
	GSList *to_remove = NULL;
	gint n;
	enum dl_list listnum[] = { DL_LIST_RUNNING, DL_LIST_WAITING };

	for (n = 0; n < sizeof(listnum) / sizeof(listnum[0]); n++) {
		enum dl_list idx = listnum[n];
		GList *l;

		for (l = server->list[idx]; l; l = g_list_next(l)) {
			struct download *d = (struct download *) l->data;
			g_assert(d);
			to_remove = g_slist_prepend(to_remove, d);
		}
	}

	for (n = 0, l = to_remove; l; l = l->next) {
		struct download *d = (struct download *) l->data;
		n++;
		switch (d->status) {
		case GTA_DL_QUEUED:
		case GTA_DL_TIMEOUT_WAIT:
			download_free(d);
			break;
		default:
			download_abort(d);
			break;
		}
	}

	g_slist_free(to_remove);

	return n;
}

/*
 * download_remove_all_named
 *
 * remove all downloads with a given name from the download queue
 * and abort all conenctions to peer in the active download list.
 * Returns the number of removed downloads.
 */
gint download_remove_all_named(const gchar *name)
{
	GSList *l;
	GSList *to_remove = NULL;
	
	int n = 0, m = 0;

	g_return_val_if_fail(name, 0);
	
	g_snprintf(dl_tmp, sizeof(dl_tmp), "%s", name);

	for (l = sl_downloads; l; l = g_slist_next(l)) {
		struct download *d = (struct download *) l->data;

		n ++;

		if (!d) {
			g_warning("download_remove_all_named(): NULL download");
			continue;
		}

		if (!strcmp(dl_tmp, d->file_name)) 
			to_remove = g_slist_prepend(to_remove, d);
	}

	for (l = to_remove; l; l = l->next) {
		struct download *d = (struct download *) l->data;
		m ++;
		switch (d->status) {
		case GTA_DL_QUEUED:
		case GTA_DL_TIMEOUT_WAIT:
			download_free(d);
			break;
		default:
			download_abort(d);
			break;
		}
	}

	g_slist_free(to_remove);

	return m;
}

/*
 * download_remove_all_with_sha1
 *
 * remove all downloads with a given sha1 hash from the download queue
 * and abort all conenctions to peer in the active download list.
 * Returns the number of removed downloads.
 * Note: if sha1 is NULL, we do not clear all download with sha1==NULL
 *       but abort instead.
 */
gint download_remove_all_with_sha1(const guchar *sha1)
{
	GSList *l;
	GSList *to_remove = NULL;
	
	int n = 0, m = 0;

	g_return_val_if_fail(sha1 != NULL, 0);
	
	for (l = sl_downloads; l; l = g_slist_next(l)) {
		struct download *d = (struct download *) l->data;

		n ++;

		if (!d) {
			g_warning("download_remove_all_with_sha1(): NULL download");
			continue;
		}

		if ((d->sha1 != NULL) && (memcmp(sha1, d->sha1, SHA1_RAW_SIZE) == 0)) 
			to_remove = g_slist_prepend(to_remove, d);
	}

	for (l = to_remove; l; l = l->next) {
		struct download *d = (struct download *) l->data;
		m ++;
		switch (d->status) {
		case GTA_DL_QUEUED:
		case GTA_DL_TIMEOUT_WAIT:
			download_free(d);
			break;
		default:
			download_abort(d);
			break;
		}
	}

	g_slist_free(to_remove);

	return m;
}

/*
 * GUI operations
 */

/* Add a download to the GUI */

void download_gui_add(struct download *d)
{
	gchar *titles[5];
	gint row;
	GdkColor *color;
	GtkCList* clist_downloads;

	g_return_if_fail(d);

	if (DOWNLOAD_IS_VISIBLE(d)) {
		g_warning
			("download_gui_add() called on already visible download '%s' !",
			 d->file_name);
		return;
	}

	clist_downloads = GTK_CLIST
		(lookup_widget(main_window, "clist_downloads"));

	color = &(gtk_widget_get_style(GTK_WIDGET(clist_downloads))
				->fg[GTK_STATE_INSENSITIVE]);

	titles[c_dl_filename] = d->file_name;
	titles[c_dl_host] = ip_port_to_gchar(download_ip(d), download_port(d));
	titles[c_dl_server] = download_vendor_str(d);
	titles[c_dl_status] = "";

	if (DOWNLOAD_IS_QUEUED(d)) {		/* This is a queued download */
		GtkCList* clist_downloads_queue;

		titles[c_dl_size] = short_size(d->file_info->size);

		clist_downloads_queue = GTK_CLIST
			(lookup_widget(main_window, "clist_downloads_queue"));

		row = gtk_clist_append(clist_downloads_queue, titles);
		gtk_clist_set_row_data(clist_downloads_queue, row, (gpointer) d);
		if (d->always_push)
			 gtk_clist_set_foreground(clist_downloads_queue, row, color);
	} else {					/* This is an active download */
		gint rw;

		rw = g_snprintf(dl_tmp, sizeof(dl_tmp),
			"%s", short_size(d->file_info->size));
		rw += g_snprintf(&dl_tmp[rw], sizeof(dl_tmp)-rw,
			" (%s)", short_size(d->size));

		titles[c_dl_size] = dl_tmp;

		row = gtk_clist_append(clist_downloads, titles);
		gtk_clist_set_row_data(clist_downloads, row, (gpointer) d);
		if (DOWNLOAD_IS_IN_PUSH_MODE(d))
			 gtk_clist_set_foreground(clist_downloads, row, color);
	}

	d->visible = TRUE;
}

/**
 * download_gui_remove:
 *
 * Remove a download from the GUI.
 */
void download_gui_remove(struct download *d)
{
	gint row;

	g_return_if_fail(d);

	if (!DOWNLOAD_IS_VISIBLE(d)) {
		g_warning
			("download_gui_remove() called on invisible download '%s' !",
			 d->file_name);
		return;
	}

	if (DOWNLOAD_IS_QUEUED(d)) {
		GtkCList *clist_downloads_queue;

		clist_downloads_queue = GTK_CLIST
			(lookup_widget(main_window, "clist_downloads_queue"));

		row =
			gtk_clist_find_row_from_data(clist_downloads_queue, (gpointer) d);
		if (row != -1)
			gtk_clist_remove(clist_downloads_queue, row);
		else
			g_warning("download_gui_remove(): "
				"Queued download '%s' not found in clist !?", d->file_name);
	} else {
		GtkCList *clist_downloads;

		clist_downloads = GTK_CLIST
			(lookup_widget(main_window, "clist_downloads"));

		row = gtk_clist_find_row_from_data(clist_downloads, (gpointer) d);
		if (row != -1)
			gtk_clist_remove(clist_downloads, row);
		else
			g_warning("download_gui_remove(): "
				"Active download '%s' not found in clist !?", d->file_name);
	}

	d->visible = FALSE;

	gui_update_download_abort_resume();
	gui_update_download_clear();
}

/* Remove stopped downloads */

void download_clear_stopped(gboolean all, gboolean now)
{
	GSList *l = sl_unqueued;
	time_t current_time = 0;

	/*
	 * If all == TRUE: remove COMPLETED | ERROR | ABORTED,
	 * else remove only COMPLETED.
	 *
	 * If now == TRUE: remove immediately, else remove only downloads
	 * idle since at least 3 seconds
	 */

	if (l && !now)
		current_time = time(NULL);

	while (l) {
		struct download *d = (struct download *) l->data;
		l = l->next;

		if (!DOWNLOAD_IS_STOPPED(d))
			continue;

		if (all) {
			if (now || (current_time - d->last_update) > 3)
				download_free(d);
		} else if (d->status == GTA_DL_COMPLETED) {
			if (now || (current_time - d->last_update) > 3)
				download_free(d);
		}
	}

	gui_update_download_abort_resume();
	gui_update_download_clear();
}


/*
 * Downloads management
 */

/*
 * download_add_to_list
 */
static void download_add_to_list(struct download *d, enum dl_list idx)
{
	struct dl_server *server = d->server;

	g_assert(d->list_idx == -1);			/* Not in any list */

	d->list_idx = idx;

	server->list[idx] = g_list_prepend(server->list[idx], d);
	server->count[idx]++;
}

/*
 * download_move_to_list
 *
 * Move download from its current list to the `idx' one.
 */
static void download_move_to_list(struct download *d, enum dl_list idx)
{
	struct dl_server *server = d->server;
	enum dl_list old_idx = d->list_idx;

	g_assert(d->list_idx != -1);			/* In some list */
	g_assert(d->list_idx != idx);			/* Not in the target list */

	/*
	 * Global counters update.
	 */

	if (old_idx == DL_LIST_RUNNING) {
		if (DOWNLOAD_IS_ACTIVE(d))
			dl_active--;
		else {
			g_assert(DOWNLOAD_IS_ESTABLISHING(d));
			dl_establishing--;
		}
		downloads_with_name_dec(d->file_info->file_name);
	} else if (idx == DL_LIST_RUNNING) {
		dl_establishing++;
		downloads_with_name_inc(d->file_info->file_name);
	}

	g_assert(dl_active >= 0 && dl_establishing >= 0);

	/*
	 * Local counter and list update.
	 */

	g_assert(server->count[old_idx] > 0);

	server->list[old_idx] = g_list_remove(server->list[old_idx], d);
	server->count[old_idx]--;

	server->list[idx] = g_list_append(server->list[idx], d);
	server->count[idx]++;

	d->list_idx = idx;
}

/*
 * download_set_retry_after
 *
 * Change the `retry_after' field of the host where this download runs.
 */
static void download_set_retry_after(struct download *d, time_t after)
{
	struct dl_server *server = d->server;

	dl_by_time = g_list_remove(dl_by_time, server);
	server->retry_after = after;
	dl_by_time = g_list_insert_sorted(dl_by_time, server, dl_server_retry_cmp);
}

/*
 * download_remove_from_server
 *
 * Remove download from server.
 * Reclaim server if this was the last download held.
 */
static void download_remove_from_server(struct download *d)
{
	struct dl_server *server;
	enum dl_list idx;

	g_assert(d);
	g_assert(d->server);
	g_assert(d->list_idx != -1);

	idx = d->list_idx;
	server = d->server;

	server->list[idx] = g_list_remove(server->list[idx], d);
	server->count[idx]--;

	if (
		server->count[DL_LIST_RUNNING] == 0 &&
		server->count[DL_LIST_WAITING] == 0 &&
		server->count[DL_LIST_STOPPED] == 0
	)
		free_server(server);
}

/*
 * download_redirect_to_server
 *
 * Move download from a server to another when the IP:port changed due
 * to a Location: redirection.
 */
static void download_redirect_to_server(struct download *d,
	guint32 ip, guint16 port)
{
	struct dl_server *server;
	guchar old_guid[16];
	enum dl_list list_idx;
	
	g_assert(d);
	g_assert(d->server);

	/*
	 * If neither the IP nor the port changed, do nothing.
	 */

	server = d->server;
	if (server->key->ip == ip && server->key->port == port)
		return;

	/*
	 * We have no way to know the GUID of the new IP:port server, so we
	 * reuse the old one.  We must save it before removing the download
	 * from the old server.
	 */

	memcpy(old_guid, download_guid(d), 16);
	download_remove_from_server(d);

	/*
	 * Create new server.
	 */

	server = get_server(old_guid, ip, port);
	if (server == NULL)
		server = allocate_server(old_guid, ip, port);
	d->server = server;

	/*
	 * Insert download in new server, in the same list.
	 */

	list_idx = d->list_idx;
	d->list_idx = -1;			/* Pre-condition for download_add_to_list() */

	download_add_to_list(d, list_idx);
}

/*
 * download_stop
 *
 * Stop running download.
 */
void download_stop(struct download *d, guint32 new_status,
				   const gchar * reason, ...)
{
	gboolean store_queue = FALSE;		/* Shall we call download_store()? */
	enum dl_list list_target;

	/* Stop an active download, close its socket and its data file descriptor */

	g_return_if_fail(d);

	if (DOWNLOAD_IS_QUEUED(d)) {
		g_warning("download_stop() called on queued download '%s'!",
				  d->file_name);
		return;
	}

	if (DOWNLOAD_IS_STOPPED(d)) {
		g_warning("download_stop() called on stopped download '%s'!",
				  d->file_name);
		return;
	}

	if (d->status == new_status) {
		g_warning("download_stop(): download '%s' already in state %d",
				  d->file_name, new_status);
		return;
	}

	if (d->restart_timer_id) {
		if (d->status != GTA_DL_STOPPED)
			g_warning("download_stop: download \"%s\" has a restart_timer_id.",
				  d->file_name);
		g_source_remove(d->restart_timer_id);
		d->restart_timer_id = 0;
	}

	switch (new_status) {
	case GTA_DL_COMPLETED:
	case GTA_DL_ABORTED:
		list_target = DL_LIST_STOPPED;
		store_queue = TRUE;
		break;
	case GTA_DL_ERROR:
		list_target = DL_LIST_STOPPED;
		break;
	case GTA_DL_TIMEOUT_WAIT:
		list_target = DL_LIST_WAITING;
		break;
	case GTA_DL_STOPPED:
		list_target = DL_LIST_STOPPED;
		download_start_restart_timer(d);
		break;
	default:
		g_warning("download_stop(): unexpected new status %d !", new_status);
		return;
	}

	if (reason) {
		va_list args;
		va_start(args, reason);
		g_vsnprintf(d->error_str, sizeof(d->error_str), reason, args);
		d->error_str[sizeof(d->error_str) - 1] = '\0';	/* May be truncated */
		va_end(args);
		d->remove_msg = d->error_str;
	} else
		d->remove_msg = NULL;

	if (d->file_desc != -1) {		/* Close output file */
		close(d->file_desc);
		d->file_desc = -1;
	}
	if (d->socket) {				/* Close socket */
		socket_free(d->socket);
		d->socket = NULL;
	}
	if (d->io_opaque)				/* I/O data */
		io_free(d->io_opaque);
	if (d->bio) {
		bsched_source_remove(d->bio);
		d->bio = NULL;
	}

	if (d->list_idx != list_target)
		download_move_to_list(d, list_target);

	/* Register the new status, and update the GUI if needed */

	d->status = new_status;
	d->last_update = time((time_t *) NULL);

	if (d->status != GTA_DL_TIMEOUT_WAIT)
		d->retries = 0;		/* If they retry, go over whole cycle again */

	if (DOWNLOAD_IS_VISIBLE(d))
		gui_update_download(d, TRUE);

	if (!d->file_info->use_swarming) {
		if (new_status == GTA_DL_COMPLETED)
			queue_remove_downloads_with_file(d->file_info);
	} else if (d->file_info->done == d->file_info->size)
		queue_remove_downloads_with_file(d->file_info);

	if (store_queue) {
		download_store();			/* Refresh copy */
		file_info_store();
	}

	if (DOWNLOAD_IS_STOPPED(d) && DOWNLOAD_IS_IN_PUSH_MODE(d))
		download_push_remove(d);

	if (DOWNLOAD_IS_VISIBLE(d)) {
		gui_update_download_abort_resume();
		gui_update_download_clear();
	}

	file_info_clear_download(d);

	gui_update_c_downloads(dl_active, dl_establishing + dl_active);
}

/*
 * download_queue_v
 *
 * The vectorized (message-wise) version of download_queue().
 */
static void download_queue_v(struct download *d, const gchar *fmt, va_list ap)
{
	/*
	 * Put a download in the queue :
	 * - it's a new download, but we have reached the max number of
	 *   running downloads
	 * - the user requested it with the popup menu "Move back to the queue"
	 */

	g_assert(d);

	if (DOWNLOAD_IS_QUEUED(d)) {
		g_warning("download_queue(): Download is already queued ?!");
		return;
	}

	if (fmt) {
		g_vsnprintf(d->error_str, sizeof(d->error_str), fmt, ap);
		d->error_str[sizeof(d->error_str) - 1] = '\0';	/* May be truncated */
		/* d->remove_msg updated below */
	}

	if (DOWNLOAD_IS_VISIBLE(d))
		download_gui_remove(d);

	if (DOWNLOAD_IS_RUNNING(d))
		download_stop(d, GTA_DL_TIMEOUT_WAIT, NULL);

	/*
	 * Since download stop can change "d->remove_msg", update it now.
	 */

	d->remove_msg = fmt ? d->error_str: NULL;
	d->status = GTA_DL_QUEUED;

	if (d->list_idx != DL_LIST_WAITING)		/* Timeout wait is in "waiting" */
		download_move_to_list(d, DL_LIST_WAITING);

	sl_unqueued = g_slist_remove(sl_unqueued, d);

	download_gui_add(d);
	gui_update_download(d, TRUE);

	if (d->restart_timer_id) {
		g_source_remove(d->restart_timer_id);
		d->restart_timer_id = 0;
	}

	file_info_clear_download(d);
}

/*
 * download_queue
 *
 * Put download into queue.
 */
void download_queue(struct download *d, const gchar *fmt, ...)
{
	va_list args;

	g_assert(d);

	va_start(args, fmt);
	download_queue_v(d, fmt, args);
	va_end(args);
}

/*
 * download_freeze_queue
 *
 * Freeze the scheduling queue. Multiple freezing requires
 * multiple thawing.
 */
void download_freeze_queue()
{
	queue_frozen++;
	gui_update_queue_frozen();
}

/*
 * download_thaw_queue
 *
 * Thaw the scheduling queue. Multiple freezing requires
 * multiple thawing.
 */
void download_thaw_queue(void)
{
	g_return_if_fail(queue_frozen > 0);

	queue_frozen--;
	gui_update_queue_frozen();
}

/*
 * download_queue_is_frozen
 *
 * Test whether download queue is frozen.
 */
gint download_queue_is_frozen(void)
{
	return queue_frozen;
}

/*
 * download_queue_delay
 *
 * Put download back to queue, but don't reconsider it for starting
 * before the next `delay' seconds. -- RAM, 03/09/2001
 */
static void download_queue_delay(struct download *d, guint32 delay,
	const gchar *fmt, ...)
{
	struct dl_server *server = d->server;
	time_t now = time((time_t *) NULL);
	va_list args;

	g_assert(d);

	va_start(args, fmt);
	download_queue_v(d, fmt, args);
	va_end(args);

	/*
	 * Always consider the farthest time in the future when updating the
	 * `retry_after' field of the server.
	 */

	d->last_update = now;
	if (server->retry_after < (now + delay))
		download_set_retry_after(d, now + delay);
}

/*
 * download_retry_no_urires
 *
 * If we sent a "GET /uri-res/N2R?" and we don't know the remote
 * server does not support it, then mark it here and retry as if we
 * got a 503 busy.
 *
 * `delay' is the Retry-After delay we got, 0 if none.
 * `ack_code' is the HTTP status code we got, 0 if none.
 *
 * Returns TRUE if we marked the download for retry.
 */
static gboolean download_retry_no_urires(struct download *d,
	gint delay, gint ack_code)
{
	/*
	 * Gtk-gnutella servers understand /uri-res.  Therefore, if we get an
	 * HTTP error after sending such a request, trust it (i.e. don't retry).
	 */

	if (0 == strncmp(download_vendor_str(d), "gtk-gnutella/", 13))
		return FALSE;

	if (!(d->server->attrs & DLS_A_NO_URIRES) && (d->flags & DL_F_URIRES)) {
		/*
		 * We sent /uri-res, and never marked server as not supporting it.
		 */

		d->server->attrs |= DLS_A_NO_URIRES;

		if (dbg > 3)
			printf("Server %s (%s) does not support /uri-res/N2R?\n",
				ip_port_to_gchar(download_ip(d), download_port(d)),
				download_vendor_str(d));

		if (ack_code)
			download_queue_delay(d,
				delay ? delay : download_retry_busy_delay,
				"Server cannot handle /uri-res (%d)", ack_code);
		else
			download_queue_delay(d, download_retry_busy_delay,
				"Server cannot handle /uri-res (EOF)");

		return TRUE;
	}

	return FALSE;
}

/*
 * download_push_insert
 *
 * Record that we sent a push request for this download.
 */
static void download_push_insert(struct download *d)
{
	gchar *key;
	GSList *list;
	gboolean found;

	g_assert(!d->push);

	g_snprintf(dl_tmp, sizeof(dl_tmp), "%u:%s",
		d->record_index, guid_hex_str(download_guid(d)));

	/*
	 * We should not have the download already in the table, since we take care
	 * when starting a download that there is no (active) duplicate.  We also
	 * perform the same check on resuming a stopped download, so the following
	 * warning should not happen.  It will indicate a bug. --RAM, 01/01/2002
	 *
	 * However, it is possible that a servent updates its library, and that
	 * we get another query hit from that servent with a different file name
	 * but with the same index of a file we already recorded in the hash table.
	 * That is possible because we check for duplicate downloads based on
	 * the (name, GUID) tuple only.
	 *
	 * To overcome this, we have to store a list of downloads with the same
	 * key, and prepend newest ones, as being the ones with the "most accurate"
	 * index, supposedly. --RAM, 13/03/2002
	 */

	found = g_hash_table_lookup_extended(pushed_downloads, (gpointer) dl_tmp,
		(gpointer *) &key, (gpointer *) &list);

	if (!found) {
		list = g_slist_append(NULL, d);
		key = atom_str_get(dl_tmp);
		g_hash_table_insert(pushed_downloads, key, list);
	} else {
		GSList *l;

		if ((l = g_slist_find(list, d))) {
			struct download *ad = (struct download *) l->data;
			g_warning("BUG: duplicate push ignored for \"%s\"", ad->file_name);
			g_warning("BUG: argument is 0x%lx, \"%s\", key = %s, state = %d",
				(gulong) d, d->file_name, key, d->status);
			g_snprintf(dl_tmp, sizeof(dl_tmp), "%u:%s",
				ad->record_index, guid_hex_str(download_guid(ad)));
			g_warning("BUG: in table has 0x%lx \"%s\", key = %s, state = %d",
				(gulong) ad, ad->file_name, dl_tmp, ad->status);
		} else {
			list = g_slist_prepend(list, d);
			g_hash_table_insert(pushed_downloads, key, list);
		}
	}

	d->push = TRUE;
}

/*
 * download_push_remove
 *
 * Forget that we sent a push request for this download.
 */
static void download_push_remove(struct download *d)
{
	gpointer key;
	GSList *list;

	g_assert(d->push);

	g_snprintf(dl_tmp, sizeof(dl_tmp), "%u:%s",
		d->record_index, guid_hex_str(download_guid(d)));

	if (
		g_hash_table_lookup_extended(pushed_downloads, (gpointer) dl_tmp,
			&key, (gpointer *) &list)
	) {
		GSList *l = g_slist_find(list, d);

		/*
		 * Value `list' is a list of downloads that share the same key.
		 * We need to remove the entry in the hash table only when the
		 * last downlaod is removed from that list.
		 */

		if (l == NULL) {
			g_warning("BUG: push 0x%lx \"%s\" not found, key = %s, state = %d",
				(gulong) d, d->file_name, dl_tmp, d->status);
		} else {
			g_assert(l->data == (gpointer) d);
			list = g_slist_remove(list, d);
			if (list == NULL) {
				g_hash_table_remove(pushed_downloads, key);
				atom_str_free(key);
			} else
				g_hash_table_insert(pushed_downloads, key, list);
		}
	} else
		g_warning("BUG: tried to remove missing push %s", dl_tmp);

	d->push = FALSE;
}

/*
 * download_start_prepare
 *
 * Setup the download structure with proper range offset, and check that the
 * download is not otherwise completed.
 *
 * Returns TRUE if we may continue with the download, FALSE if it has been
 * stopped due to a problem.
 */
static gboolean download_start_prepare(struct download *d)
{
	struct stat st;

	g_assert(d->list_idx != DL_LIST_RUNNING);

	/*
	 * Updata global accounting data.
	 */

	download_move_to_list(d, DL_LIST_RUNNING);

	/*
	 * If the download is in the queue, we remove it from there.
	 */

	if (DOWNLOAD_IS_QUEUED(d)) {
		if (DOWNLOAD_IS_VISIBLE(d))
			download_gui_remove(d);
		sl_unqueued = g_slist_prepend(sl_unqueued, d);
	}

	d->status = GTA_DL_CONNECTING;	/* Most common state if we succeed */

	/*
	 * If the output file already exists, we have to send a partial request
	 * This is done here so multiple downloads of existing files drop out when
	 * they are smaller than the existing file.
	 *
	 * If the file already exists, and has less than `download_overlap_range'
	 * bytes, we restart the download from scratch.  Otherwise, we request
	 * that amount before the resuming point.
	 * Later on, in download_write_data(), and as soon as we have read more
	 * than `download_overlap_range' bytes, we'll check for a match.
	 *		--RAM, 12/01/2002
	 *
	 * Now that we have overlapping range checking, we can restore older code
	 * to move back the file from the "done" directory back to the working
	 * directory if the to-be-downloaded file is bigger than what we have.
	 * This means that we started to download s shorter version (incomplete?)
	 * of that file.
	 *		--RAM, 13/03/2002
	 */

	if (!d->file_info->use_swarming) {

		g_snprintf(dl_tmp, sizeof(dl_tmp), "%s/%s",
			d->file_info->path, d->file_info->file_name);

		d->skip = 0;

		if (stat(dl_tmp, &st) != -1) {
			if (st.st_size > download_overlap_range)
				d->skip = st.st_size;
		} else if (!d->file_info->use_swarming) {
			gchar dl_dest[4096];

			g_snprintf(dl_dest, sizeof(dl_dest), "%s/%s",
						move_file_path, d->file_info->file_name);

			if (stat(dl_dest, &st) != -1) {		/* File exists in "done" dir */
				if (st.st_size < d->size) {		/* And is smaller */
					if (-1 == rename(dl_dest, dl_tmp))
						g_warning("cannot move incomplete \"%s\" back to "
							"download dir: %s", dl_dest, g_strerror(errno));
					else {
						if (st.st_size > download_overlap_range)
							d->skip = st.st_size;
						g_warning("moved incomplete \"%s\" back to download dir",
									d->file_info->file_name);
					}
				} else
					d->skip = st.st_size;		/* "done" file is larger */
			}
		}

		d->pos = d->skip;
		d->last_update = time((time_t *) NULL);
		d->overlap_size = (d->skip == 0 || d->size <= d->pos) ?
			0 : download_overlap_range;

	} else { /* using swarming */
	
		enum dl_chunk_status status;
		guint32 from, to;

		d->overlap_size = 0;
		d->last_update = time((time_t *) NULL);

		status = file_info_find_hole(d, &from, &to);

		if (status == DL_CHUNK_EMPTY) {

			d->skip = d->pos = from;
			d->size = to - from;

			if (
				from > download_overlap_range &&
				file_info_chunk_status(d->file_info, 
					from - download_overlap_range, from) == DL_CHUNK_DONE
			)
				d->overlap_size = download_overlap_range;

		} else if (status == DL_CHUNK_BUSY) {

			download_queue_delay(d, 10, "Waiting for a free slot");
			return FALSE;

		} else if (status == DL_CHUNK_DONE) {

			if (!DOWNLOAD_IS_VISIBLE(d))
				download_gui_add(d);

			download_stop(d, GTA_DL_ERROR, "No more gaps to fill");
			queue_remove_downloads_with_file(d->file_info);
			return FALSE;
		}
	}

	g_assert(d->overlap_size == 0 || d->skip > d->overlap_size);

	/*
	 * Is there anything to get at all?
	 */

	if (d->file_info->done == d->file_info->size) {
		if (!DOWNLOAD_IS_VISIBLE(d))
			download_gui_add(d);
		download_stop(d, GTA_DL_ERROR, "Nothing more to get");
		//queue_remove_identical(d->file_info->file_name, d->sha1, d->file_info->size);
		queue_remove_downloads_with_file(d->file_info);
		return FALSE;
	}

	return TRUE;
}

/* (Re)start a stopped or queued download */

void download_start(struct download *d, gboolean check_allowed)
{
	guint32 ip = download_ip(d);
	guint16 port = download_port(d);

	g_assert(d);
	g_assert(d->list_idx != DL_LIST_RUNNING);	/* Waiting or stopped */

	/*
	 * If caller did not check whether we were allowed to start downloading
	 * this file, do it now. --RAM, 03/09/2001
	 */

	if (check_allowed && (
		count_running_downloads() >= max_downloads ||
		count_running_on_server(d->server) >= max_host_downloads ||
		(!d->file_info->use_swarming &&
			count_running_downloads_with_name(d->file_info->file_name) != 0))
	) {
		if (!DOWNLOAD_IS_QUEUED(d))
			download_queue(d, NULL);
		return;
	}

	if (!download_start_prepare(d))
		return;

	g_assert(d->list_idx == DL_LIST_RUNNING);	/* Moved to "running" list */

	if (!send_pushes && d->push)
		download_push_remove(d);

	/*
	 * If server is known to be reachable without pushes, reset the flag.
	 */

	if (d->always_push && (d->server->attrs & DLS_A_PUSH_IGN)) {
		g_assert(check_valid_host(ip, port));	/* Or would not have set flag */
		if (d->push)
			download_push_remove(d);
		d->always_push = FALSE;
	}

	if (!DOWNLOAD_IS_IN_PUSH_MODE(d) && check_valid_host(ip, port)) {
		/* Direct download */
		d->status = GTA_DL_CONNECTING;
		d->socket = socket_connect(ip, port, GTA_TYPE_DOWNLOAD);

		if (!DOWNLOAD_IS_VISIBLE(d))
			download_gui_add(d);

		if (!d->socket) {
			download_stop(d, GTA_DL_ERROR, "Connection failed");
			return;
		}

		d->socket->resource.download = d;
		d->socket->pos = 0;
	} else {					/* We have to send a push request */
		d->status = GTA_DL_PUSH_SENT;

		if (!DOWNLOAD_IS_VISIBLE(d))
			download_gui_add(d);

		download_push(d, FALSE);
	}

	gui_update_download(d, TRUE);
	gui_update_c_downloads(dl_active, dl_establishing + dl_active);
}

/* pick up new downloads from the queue as needed */

void download_pickup_queued(void)
{
	GList *l;
	time_t now = time((time_t *) NULL);
	gint running = count_running_downloads();

	/*
	 * To select downloads, we iterate over the sorted `dl_by_time' list and
	 * look for something we could schedule.
	 *
	 * Note that we jump from one host to the other, even if we have multiple
	 * things to schedule on the same host: It's better to spread load among
	 * all hosts first.
	 */

	for (l = dl_by_time; l && running < max_downloads; l = g_list_next(l)) {
		struct dl_server *server = (struct dl_server *) l->data;
		GList *w;

		/*
		 * List is sorted, so as soon as we go beyond the current time, we
		 * can stop.
		 */

		if (server->retry_after > now)
			break;

		if (
			server->count[DL_LIST_WAITING] == 0 ||
			count_running_on_server(server) >= max_host_downloads
		)
			continue;

		/*
		 * OK, pick the download at the start of the waiting list, but
		 * do not remove it yet.  This will be done by download_start().
		 */

		g_assert(server->list[DL_LIST_WAITING]);	/* Since count != 0 */

		for (w = server->list[DL_LIST_WAITING]; w; w = g_list_next(w)) {
			struct download *d = (struct download *) w->data;

			if (
				!d->file_info->use_swarming &&
			  	count_running_downloads_with_name(download_outname(d)) != 0
			)
				continue;

			if ((now - d->last_update) <= d->timeout_delay)
				continue;

			download_start(d, FALSE);

			if (DOWNLOAD_IS_RUNNING(d))
				running++;

			break;			/* Don't schedule all files on same host at once */
		}
	}

	/*
	 * Enable "Start now" only if we would not exceed limits.
	 */

	gtk_widget_set_sensitive(
		lookup_widget(popup_queue, "popup_queue_start_now"), 
		(running < max_downloads) &&
		GTK_CLIST(
			lookup_widget(main_window, "clist_downloads_queue"))->selection); 
}

static void download_push(struct download *d, gboolean on_timeout)
{
	gboolean ignore_push = FALSE;

	g_assert(d);

	if (d->flags & DL_F_PUSH_IGN)
		ignore_push = TRUE;

	if (!send_pushes || ignore_push) {
		if (d->push)
			download_push_remove(d);
		goto attempt_retry;
	}

	/*
	 * The push request is sent with the listening port set to our Gnet port.
	 *
	 * To be able to later distinguish which download is referred to by each
	 * GIV we'll receive back, we record the association file_index/guid of
	 * the to-be-downloaded file with this download into a hash table.
	 * When stopping a download for which d->push is true, we'll have to
	 * remove the mapping.
	 *
	 *		--RAM, 30/12/2001
	 */

	if (!d->push)
		download_push_insert(d);

	g_assert(d->push);
	if (!send_push_request(download_guid(d), d->record_index, listen_port)) {
		if (!d->always_push) {
			download_push_remove(d);
			goto attempt_retry;
		} else {
			/*
			 * If the address is not a private IP, it is possible that the
			 * servent set the "Push" flag incorrectly.
			 *		-- RAM, 18/08/2002.
			 */

			if (!check_valid_host(download_ip(d), download_port(d)))
				download_stop(d, GTA_DL_ERROR, "Push route lost");
			else {
				/*
				 * Later on, if we manage to connect to the server, we'll
				 * make sure to mark it so that we ignore pushes to it, and
				 * we will clear the `always_push' indication.
				 * (see download_send_request() for more information)
				 */

				download_push_remove(d);

				if (dbg > 2)
					printf("PUSH trying to ignore them for %s\n",
						ip_port_to_gchar(download_ip(d), download_port(d)));

				d->flags |= DL_F_PUSH_IGN;
				download_queue(d, "Ignoring Push flag");
			}
		}
	}

	return;

attempt_retry:
	/*
	 * If we're aboring a download flagged with "Push ignore" due to a
	 * timeout reason, chances are great that this host is indeed firewalled!
	 * Tell them so. -- RAM, 18/08/2002.
	 */

	if (on_timeout && d->always_push && (d->flags & DL_F_PUSH_IGN))
		download_stop(d, GTA_DL_ERROR, "Can't reach host (Push or Direct)");
	else if (++d->retries <= download_max_retries) {
		if (on_timeout)
			download_queue_delay(d, download_retry_timeout_delay,
				"Timeout (%d retr%s)",
				d->retries, d->retries == 1 ? "y" : "ies");
		else
			download_queue_delay(d, download_retry_refused_delay,
				"Connection refused %s(%d retr%s)",
				ignore_push ? "[No Push] " : "",
				d->retries, d->retries == 1 ? "y" : "ies");
	} else
		download_stop(d, GTA_DL_ERROR, "Timeout (%d retr%s)",
				d->retries, d->retries == 1 ? "y" : "ies");

	/*
	 * Remove this source from mesh, since we don't seem to be able to
	 * connect to it properly.
	 */

	if (!d->always_push && d->sha1)
		dmesh_remove(d->sha1, download_ip(d), download_port(d),
			d->record_index, d->file_name);
}

/* Direct download failed, let's try it with a push request */

void download_fallback_to_push(struct download *d,
	gboolean on_timeout, gboolean user_request)
{
	g_return_if_fail(d);

	if (DOWNLOAD_IS_QUEUED(d)) {
		g_warning
			("download_fallback_to_push() called on a queued download !?!");
		return;
	}

	if (DOWNLOAD_IS_STOPPED(d))
		return;

	if (!d->socket)
		g_warning("download_fallback_to_push(): no socket for '%s'",
				  d->file_name);
	else {
		d->socket->resource.download = NULL;
		socket_destroy(d->socket);
		d->socket = NULL;
	}

	if (d->file_desc != -1) {
		close(d->file_desc);
		d->file_desc = -1;
	}

	if (user_request)
		d->status = GTA_DL_PUSH_SENT;
	else
		d->status = GTA_DL_FALLBACK;

	download_push(d, on_timeout);

	gui_update_download(d, TRUE);
}

/*
 * escape_filename
 *
 * Lazily replace all '/' if filename with '_': if a substitution needs to
 * be done, a copy of the original argument is made first.  Otherwise,
 * no change nor allocation occur.
 *
 * Returns the pointer to the escaped filename, or the original argument if
 * no escaping needed to be performed.
 */
static gchar *escape_filename(gchar *file)
{
	gchar *escaped = NULL;
	gchar *s;
	gchar c;

	s = file;
	while ((c = *s)) {
		if (c == '/') {
			if (escaped == NULL) {
				escaped = g_strdup(file);
				s = escaped + (s - file);	/* s now refers to escaped string */
				g_assert(*s == '/');
			}
			*s = '_';
		}
		s++;
	}

	return escaped == NULL ? file : escaped;
}

/*
 * Downloads creation and destruction
 */

/*
 * create_download
 * 
 * Create a new download
 *
 * When `interactive' is false, we assume that `file' was already duped,
 * and take ownership of the pointer.
 * If `output' is not NULL, we also take ownership.
 */
static void create_download(
	gchar *file, gchar *output, guint32 size, guint32 record_index,
	guint32 ip, guint16 port, gchar *guid, gchar *sha1, time_t stamp,
	gboolean push, gboolean interactive)
{
	struct dl_server *server;
	struct download *d;
	gchar *file_name = interactive ? atom_str_get(file) : file;
	struct dl_file_info *file_info;

	/*
	 * Refuse to queue the same download twice. --RAM, 04/11/2001
	 */

	if ((d = has_same_download(file_name, guid, ip, port))) {
		if (interactive)
			g_warning("rejecting duplicate download for %s", file_name);

#if 0		// XXX cannot do that anymore
		if (ip != d->ip || port != d->port) {
			d->ip = ip;
			d->port = port;
			g_warning("updated IP:port for %s to %s",
				file_name, ip_port_to_gchar(ip, port));
		}
#endif

		atom_str_free(file_name);
		return;
	}

	/*
	 * Replace all slashes by underscores in the file name, if not
	 * arealdy done by caller.	--RAM, 12/01/2002
	 */

	if (output == NULL)
		output = escape_filename(file_name);

	file_info = file_info_get(output, save_file_path, size, sha1);

	if (output != file_name)
		g_free(output);
	output = NULL;				/* No longer used */

	/*
	 * Initialize download, creating new server if needed.
	 */

	d = (struct download *) g_malloc0(sizeof(struct download));

	server = get_server(guid, ip, port);
	if (server == NULL)
		server = allocate_server(guid, ip, port);

	d->server = server;
	d->list_idx = -1;

	/*
	 * If we know that this server can be directly connected to, ignore
	 * the push flag. --RAM, 18/08/2002.
	 */

	if (d->server->attrs & DLS_A_PUSH_IGN)
		push = FALSE;

	d->file_name = file_name;
	/* Note: size and skip will be filled by file_info_find_hole() later */
	d->size = size;
	d->record_index = record_index;
	d->file_desc = -1;
	d->restart_timer_id = 0;
	d->always_push = push;
	if (sha1)
		d->sha1 = atom_sha1_get(sha1);
	if (push)
		download_push_insert(d);
	else
		d->push = FALSE;
	d->record_stamp = stamp;

	d->file_info = file_info;

	download_add_to_list(d, DL_LIST_WAITING);
	sl_downloads = g_slist_prepend(sl_downloads, d);
	sl_unqueued = g_slist_prepend(sl_unqueued, d);

	download_store();			/* Refresh list, in case we crash */

	/*
	 * Insert in download mesh if it does not require a push and has a SHA1.
	 */

	if (!d->always_push && d->sha1)
		dmesh_add(d->sha1, ip, port, record_index, file_name, stamp);

	if (
		count_running_downloads() < max_downloads &&
		count_running_on_server(d->server) < max_host_downloads &&
		count_running_downloads_with_name(d->file_info->file_name) == 0
	) {
		download_start(d, FALSE);		/* Starts the download immediately */
	} else {
		/* Max number of downloads reached, we have to queue it */
		download_queue(d, NULL);
	}
}


/* Automatic download request */

void download_auto_new(gchar *file, guint32 size, guint32 record_index,
					   guint32 ip, guint16 port, gchar *guid, gchar *sha1,
					   time_t stamp, gboolean push)
{
	gchar dl_tmp[4096];
	gchar *output_name = escape_filename(file);
	gchar *file_name;
	struct stat buf;
	char *reason;
	int tmplen;

	/*
	 * Make sure we have not got a bigger file in the "download dir".
	 *
	 * Because of swarming, we could have a trailer in the file, hence
	 * we cannot blindly stat() it.  Call a specialized routine that will
	 * figure this out.
	 *		--RAM, 18/08/2002
	 */

	g_snprintf(dl_tmp, sizeof(dl_tmp), "%s/%s", save_file_path, output_name);
	dl_tmp[sizeof(dl_tmp)-1] = '\0';

	if (file_info_filesize(dl_tmp) >= size) {
		reason = "downloaded file bigger";
		goto abort_download;
	}

	/*
	 * Make sure we have not got a bigger file in the "completed dir".
	 *
	 * We must also check for bigger files bearing our renaming exts,
	 * i.e. .01, .02, etc... and keep going while files exist.
	 */

	g_snprintf(dl_tmp, sizeof(dl_tmp), "%s/%s", move_file_path, output_name);
	dl_tmp[sizeof(dl_tmp)-1] = '\0';

	if (-1 != stat(dl_tmp, &buf) && buf.st_size >= size) {
		reason = "complete file bigger";
		goto abort_download;
	}

	tmplen = strlen(dl_tmp);
	if (tmplen >= sizeof(dl_tmp) - 4) {
		g_warning("'%s' in completed dir is too long for further checks",
			output_name);
	} else {
		int i;
		for (i = 1; i < 100; i++) {
			gchar ext[4];

			g_snprintf(ext, 4, ".%02d", i);
			dl_tmp[tmplen] = '\0';				/* Ignore prior attempt */
			strncat(dl_tmp+tmplen, ext, 3);		/* Append .01, .02, ...*/

			if (-1 == stat(dl_tmp, &buf))
				break;							/* No file, stop scanning */

			if (buf.st_size >= size) {
				g_snprintf(dl_tmp, sizeof(dl_tmp),
					"alternate complete file #%d bigger", i);
				reason = dl_tmp;
				goto abort_download;
			}
		}
	}

	file_name = atom_str_get(file);
	if (output_name == file)		/* Not duplicated, has no '/' inside */
		output_name = file_name;	/* So must reuse file_name */

	create_download(file_name, output_name,
		size, record_index, ip, port, guid, sha1, stamp, push, FALSE);
	return;

abort_download:
	if (dbg > 4)
		printf("ignoring auto download for '%s': %s\n", file, reason);
	if (output_name != file)		/* Was allocated by escape_filename() */
		g_free(output_name);
	return;
}

/* search has detected index change in queued download --RAM, 18/12/2001 */

void download_index_changed(guint32 ip, guint16 port, guchar *guid,
	guint32 from, guint32 to)
{
	struct dl_server *server = get_server(guid, ip, port);
	GList *l;
	gint nfound = 0;
	GSList *to_stop = NULL;
	GSList *sl;
	gint n;
	enum dl_list listnum[] = { DL_LIST_RUNNING, DL_LIST_WAITING };

	if (!server)
		return;

	for (n = 0; n < sizeof(listnum) / sizeof(listnum[0]); n++) {
		for (l = server->list[n]; l; l = l->next) {
			struct download *d = (struct download *) l->data;
			gboolean push_mode;

			if (d->record_index != from)
				continue;

			push_mode = d->push;

			/*
			 * When in push mode, we've recorded the index in a hash table,
			 * associating the GIV string to the download structure.
			 * If that index changes, we need to remove the old mapping before
			 * operating the change, and re-install the new mapping after
			 * then change took place.
			 */

			if (push_mode)
				download_push_remove(d);

			d->record_index = to;
			nfound++;

			if (push_mode)
				download_push_insert(d);

			switch (d->status) {
			case GTA_DL_REQ_SENT:
			case GTA_DL_HEADERS:
			case GTA_DL_PUSH_SENT:
				/*
				 * We've sent a request with possibly the wrong index.
				 * We can't know for sure, but it's safer to stop it, and
				 * restart it in a while.  Sure, we might loose the download
				 * slot, but we might as well have gotten a wrong file.
				 *
				 * NB: this can't happen when the remote peer is gtk-gnutella
				 * since we check the matching between the index and the file
				 * name, but some peers might not bother.
				 */
				g_warning("Stopping request for '%s': index changed",
					d->file_name);
				to_stop = g_slist_prepend(to_stop, d);
				break;
			case GTA_DL_RECEIVING:
				/*
				 * Ouch.  Pray and hope that the change occurred after we
				 * requested the file.  There's nothing we can do now.
				 */
				g_warning("Index of '%s' changed during reception",
					d->file_name);
				break;
			default:
				/*
				 * Queued or other state not needing special notice
				 */
				if (dbg > 3)
					printf("Noted index change from %u to %u at %s for %s",
						from, to, guid_hex_str(guid), d->file_name);
				break;
			}
		}
	}

	for (sl = to_stop; sl; sl = sl->next) {
		struct download *d = (struct download *) sl->data;
		download_stop(d, GTA_DL_STOPPED, "Stopped (Index changed)");
	}

	/*
	 * This is a sanity check: we should not have any duplicate request
	 * in our download list.
	 */

	if (nfound > 1)
		g_warning("Found %d requests for index %d (now %d) at %s",
			nfound, from, to, ip_port_to_gchar(ip, port));
}


/* Create a new download */

void download_new(gchar *file, guint32 size, guint32 record_index,
				  guint32 ip, guint16 port, gchar *guid, gchar *sha1,
				  time_t stamp, gboolean push)
{
	create_download(file, NULL, size, record_index, ip, port, guid, sha1,
		stamp, push, TRUE);
}

/* Free a download. */

void download_free(struct download *d)
{
	g_assert(d);

	if (DOWNLOAD_IS_VISIBLE(d))
		download_gui_remove(d);

	if (DOWNLOAD_IS_RUNNING(d))
		download_stop(d, GTA_DL_ABORTED, NULL);

	g_assert(d->io_opaque == NULL);

	sl_downloads = g_slist_remove(sl_downloads, d);
	sl_unqueued = g_slist_remove(sl_unqueued, d);

	if (d->restart_timer_id)
		g_source_remove(d->restart_timer_id);

	if (d->push)
		download_push_remove(d);

	if (d->sha1)
		atom_sha1_free(d->sha1);

	download_remove_from_server(d);

	atom_str_free(d->file_name);
	file_info_free(d->file_info, FALSE);

	g_free(d);
}

/* ----------------------------------------- */

void download_abort(struct download *d)
{
	g_return_if_fail(d);

	if (DOWNLOAD_IS_QUEUED(d)) {
		g_warning("download_abort() called on queued download '%s'!",
				  d->file_name);
		return;
	}

	if (DOWNLOAD_IS_STOPPED(d))
		return;

	download_stop(d, GTA_DL_ABORTED, NULL);

	/* 
	 * Fixme! The refcount isn't decreased until "Clear completed", so
	 * we may very well have a file with a high refcount and no active
	 * or queued downloads. ;-/
	 *
	 * So, the following doesn't really work;
	 */
	if (d->file_info->refcount == 1)
	if (download_delete_aborted)
		download_remove_file(d);
}

void download_resume(struct download *d)
{
	g_return_if_fail(d);

	if (DOWNLOAD_IS_QUEUED(d)) {
		g_warning("download_resume() called on queued download '%s'!",
				  d->file_name);
		return;
	}

	if (DOWNLOAD_IS_RUNNING(d))
		return;

	if (
		NULL != has_same_download(d->file_name, download_guid(d),
			download_ip(d), download_port(d))
	) {
		d->status = GTA_DL_CONNECTING;		/* So we may call download_stop */
		download_move_to_list(d, DL_LIST_RUNNING);
		download_stop(d, GTA_DL_ERROR, "Duplicate");
		return;
	}

	download_start(d, TRUE);
}

/*
 * IO functions
 */

/* Based on patch from Myers W. Carpenter <myers@fil.org> */

void download_move_to_completed_dir(struct download *d)
{
	/* Move a complete file to move_file_path */

	gchar dl_src[4096];
	gchar dl_dest[4096];
	gint return_tmp, return_tmp2;
	struct stat buf;

	file_info_strip_binary(d->file_info);
	
	if (0 == strcmp(d->file_info->path, move_file_path))
		return;			/* Already in "completed dir" */

	g_snprintf(dl_src, sizeof(dl_src), "%s/%s",
		d->file_info->path, d->file_info->file_name);
	g_snprintf(dl_dest, sizeof(dl_dest), "%s/%s",
		move_file_path, d->file_info->file_name);

	dl_src[sizeof(dl_src)-1] = '\0';
	dl_dest[sizeof(dl_dest)-1] = '\0';

	/*
	 * If, by extraordinary, there is already a file in the "completed dir"
	 * with the same name, don't overwrite the existing file.
	 *
	 * NB: we assume either there is only one gnutella servent running, or if
	 * several ones are running, that they are configured to use different
	 * download and completed dirs.
	 *
	 *		--RAM, 03/11/2001
	 */

	if (-1 != stat(dl_dest, &buf)) {
		gchar dl_tmp[4096];
		int destlen = strlen(dl_dest);
		int i;

		/*
		 * There must be enough room for us to append the ".xx" extensions.
		 * That's 3 chars, plus the trailing NUL.
		 */

		if (destlen >= sizeof(dl_dest) - 4) {
			g_warning("Found '%s' in completed dir, and path already too long",
				d->file_info->file_name);
			return;
		}

		strncpy(dl_tmp, dl_dest, destlen);

		for (i = 1; i < 100; i++) {
			gchar ext[4];

			g_snprintf(ext, 4, ".%02d", i);
			dl_tmp[destlen] = '\0';				/* Ignore prior attempt */
			strncat(dl_tmp+destlen, ext, 3);	/* Append .01, .02, ...*/
			if (-1 == stat(dl_tmp, &buf))
				break;
		}

		if (i == 100) {
			g_warning("Found '%s' in completed dir, "
				"and was unable to find another unique name",
				d->file_info->file_name);
			return;
		}

		strncat(dl_dest+destlen, dl_tmp+destlen, 3);

		g_warning("Moving completed file as '%s'", dl_dest);
	}

	/* First try and link it to the new locatation */

	return_tmp = rename(dl_src, dl_dest);

	if (return_tmp == -1 && (errno == EXDEV || errno == EPERM)) {
		/* link failed becase either the two paths aren't on the */
		/* same filesystem or the filesystem doesn't support hard */
		/* links, so we have to do a copy. */

		gint tmp_src, tmp_dest;
		gboolean ok = FALSE;

		if ((tmp_src = open(dl_src, O_RDONLY)) < 0) {
			g_warning("Unable to open() file '%s' (%s) !", dl_src,
					  g_strerror(errno));
			return;
		}

		if ((tmp_dest =
			 open(dl_dest, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
			close(tmp_src);
			g_warning("Unable to create file '%s' (%s) !", dl_src,
					  g_strerror(errno));
			return;
		}

		for (;;) {
			return_tmp = read(tmp_src, dl_tmp, sizeof(dl_tmp));

			if (!return_tmp) {
				ok = TRUE;
				break;
			}

			if (return_tmp < 0) {
				g_warning("download_move_to_completed_dir(): "
					"error reading while moving file to save directory (%s)",
					 g_strerror(errno));
				break;
			}

			return_tmp2 = write(tmp_dest, dl_tmp, return_tmp);

			if (return_tmp2 < 0) {
				g_warning("download_move_to_completed_dir(): "
					"error writing while moving file to save directory (%s)",
					 g_strerror(errno));
				break;
			}

			if (return_tmp < sizeof(dl_tmp)) {
				ok = TRUE;
				break;
			}
		}

		close(tmp_dest);
		close(tmp_src);
		if (ok)
			unlink(dl_src);
	}

	return;
}

/*
 * send_push_request
 *
 * Send a push request to the target GUID, in order to request the push of
 * the file whose index is `file_id' there onto our local port `port'.
 *
 * Returns TRUE if the request could be sent, FALSE if we don't have the route.
 */
static gboolean send_push_request(gchar *guid, guint32 file_id, guint16 port)
{
	struct gnutella_msg_push_request m;
	struct gnutella_node *n;

	n = route_towards_guid(guid);
	if (!n)
		return FALSE;

	if (!NODE_IS_WRITABLE(n))
		return FALSE;

	message_set_muid(&(m.header), FALSE);

	m.header.function = GTA_MSG_PUSH_REQUEST;
	m.header.ttl = my_ttl;
	m.header.hops = 0;

	WRITE_GUINT32_LE(sizeof(struct gnutella_push_request), m.header.size);

	memcpy(&(m.request.guid), guid, 16);

	WRITE_GUINT32_LE(file_id, m.request.file_id);
	WRITE_GUINT32_BE(listen_ip(), m.request.host_ip);
	WRITE_GUINT16_LE(port, m.request.host_port);

	message_add(m.header.muid, GTA_MSG_PUSH_REQUEST, NULL);
	gmsg_sendto_one(n, (guchar *) &m, sizeof(struct gnutella_msg_push_request));

	return TRUE;
}

static gboolean download_queue_w(gpointer dp)
{
	struct download *d = (struct download *) dp;
	gchar error_str[256];	/* Used to sprintf() error strings with vars */

	if (d->remove_msg)
		strncpy(error_str, d->remove_msg, sizeof(error_str) - 1);

	d->restart_timer_id = 0;
	download_queue(d, d->remove_msg ? error_str : NULL);

	return FALSE;			/* Called only once per download */
}

static void download_start_restart_timer(struct download *d)
{
	/* download_retry_stopped: seconds to wait after EOF or ECONNRESET */

	d->restart_timer_id = g_timeout_add(download_retry_stopped_delay * 1000,
		download_queue_w, d);
}

/***
 *** Header parsing callbacks
 ***
 *** We could call those directly, but I'm thinking about factoring all
 *** that processing into a generic set of functions, and the processing
 *** callbacks will all have the same signature.  --RAM, 30/12/2001
 ***/

static void call_download_request(struct io_header *ih)
{
	download_request(ih->download, ih->header);
}

static void call_download_push_ready(struct io_header *ih)
{
	download_push_ready(ih->download, ih->getline);
}

/***
 *** Read data on a download socket
 ***/

/*
 * download_header_parse
 *
 * This routine is called to parse the input buffer, a line at a time,
 * until EOH is reached.
 */
static void download_header_parse(struct io_header *ih)
{
	struct download *d = ih->download;
	struct gnutella_socket *s = d->socket;
	getline_t *getline = ih->getline;
	header_t *header = ih->header;
	guint parsed;
	gint error;

	/*
	 * Read header a line at a time.  We have exacly s->pos chars to handle.
	 * NB: we're using a goto label to loop over.
	 */

nextline:
	switch (getline_read(getline, s->buffer, s->pos, &parsed)) {
	case READ_OVERFLOW:
		g_warning("download_header_parse: line too long, disconnecting from %s",
			ip_to_gchar(s->ip));
		dump_hex(stderr, "Leading Data", s->buffer, MIN(s->pos, 256));
		download_stop(d, GTA_DL_ERROR, "Failed (Header too large)");
		return;
		/* NOTREACHED */
	case READ_DONE:
		if (s->pos != parsed)
			memmove(s->buffer, s->buffer + parsed, s->pos - parsed);
		s->pos -= parsed;
		break;
	case READ_MORE:		/* ok, but needs more data */
	default:
		g_assert(parsed == s->pos);
		s->pos = 0;
		return;
	}

	/*
	 * We come here everytime we get a full header line.
	 */

	if (ih->flags & IO_STATUS_LINE) {
		/*
		 * Save status line away in socket's "getline" object, then clear
		 * the fact that we're expecting a status line and continue to get
		 * the following header lines.
		 */

		g_assert(s->getline == 0);
		s->getline = getline_make();

		getline_copy(getline, s->getline);
		getline_reset(getline);
		ih->flags &= ~IO_STATUS_LINE;
		goto nextline;
	}

	if (ih->flags & IO_ONE_LINE) {
		/*
		 * Call processing routine immediately, then terminate processing.
		 * It is up to the callback to cleanup the I/O structure.
		 */

		gdk_input_remove(s->gdk_tag);
		s->gdk_tag = 0;

		ih->process_header(ih);
		return;
	}

	error = header_append(header,
		getline_str(getline), getline_length(getline));

	switch (error) {
	case HEAD_OK:
		getline_reset(getline);
		goto nextline;			/* Go process other lines we may have read */
		/* NOTREACHED */
	case HEAD_EOH:				/* We reached the end of the header */
		break;
	case HEAD_TOO_LARGE:
	case HEAD_MANY_LINES:
	case HEAD_EOH_REACHED:
		g_warning("download_header_parse: %s, disconnecting from %s",
			header_strerror(error),  ip_to_gchar(s->ip));
		fprintf(stderr, "------ Header Dump:\n");
		header_dump(header, stderr);
		fprintf(stderr, "------\n");
		dump_hex(stderr, "Header Line", getline_str(getline),
			MIN(getline_length(getline), 128));
		download_stop(d, GTA_DL_ERROR, "Failed (%s)", header_strerror(error));
		return;
		/* NOTREACHED */
	default:					/* Error, but try to continue */
		g_warning("download_header_parse: %s, from %s",
			header_strerror(error), ip_to_gchar(s->ip));
		dump_hex(stderr, "Header Line",
			getline_str(getline), getline_length(getline));
		getline_reset(getline);
		goto nextline;			/* Go process other lines we may have read */
	}

	/*
	 * We reached the end of headers.  Downloaded data should follow.
	 * Remove the I/O callback input before invoking the processing callback.
	 */

	gdk_input_remove(s->gdk_tag);
	s->gdk_tag = 0;

	ih->process_header(ih);
}

/*
 * download_header_read
 *
 * This routine is installed as an input callback to read the HTTP headers
 * of the request.
 */
static void download_header_read(
	gpointer data, gint source, GdkInputCondition cond)
{
	struct io_header *ih = (struct io_header *) data;
	struct download *d = ih->download;
	struct gnutella_socket *s = d->socket;
	guint count;
	gint r;

	if (cond & GDK_INPUT_EXCEPTION) {
		download_stop(d, GTA_DL_ERROR, "Failed (Input Exception)");
		return;
	}

	/*
	 * Update status and GUI.
	 */

	if (d->status != GTA_DL_HEADERS) {
		d->status = GTA_DL_HEADERS;
		gui_update_download(d, TRUE);
	}

	count = sizeof(s->buffer) - s->pos - 1;		/* -1 to allow trailing NUL */
	if (count <= 0) {
		g_warning("download_header_read: incoming buffer full, "
			"disconnecting from %s", ip_to_gchar(s->ip));
		dump_hex(stderr, "Leading Data", s->buffer, MIN(s->pos, 256));
		download_stop(d, GTA_DL_ERROR, "Failed (Input buffer full)");
		return;
	}

	r = bws_read(bws.in, s->file_desc, s->buffer + s->pos, count);
	if (r == 0) {
		/*
		 * If we did not read anything in the header at that point, and
		 * we sent a /uri-res request, maybe the remote server does not
		 * support it and closed the connection abruptly.
		 *		--RAM, 20/06/2002
		 */

		if (
			header_lines(ih->header) == 0 &&
			download_retry_no_urires(d, 0, 0)
		)
			return;
		download_stop(d, GTA_DL_STOPPED, "Stopped (EOF)");
		return;
	} else if (r < 0 && errno == EAGAIN)
		return;
	else if (r < 0) {
		if (errno == ECONNRESET)
			download_stop(d, GTA_DL_STOPPED, "Stopped (%s)",
				g_strerror(errno));
		else
			download_stop(d, GTA_DL_ERROR, "Failed (Read error: %s)",
				g_strerror(errno));
		return;
	}

	/*
	 * During the header reading phase, we do update "d->last_update".
	 */

	s->pos += r;
	d->last_update = time((time_t *) 0);
	d->retries = 0;		/* successful read means our retry was successful */

	download_header_parse(ih);
	return;
}

/*
 * download_overlap_check
 *
 * Check that the leading overlapping data in the socket buffer match with
 * the last ones in the downloaded file.  Then remove them.
 *
 * Returns TRUE if the data match, FALSE if they don't, in which case the
 * download is stopped.
 */
static gboolean download_overlap_check(struct download *d)
{
	struct gnutella_socket *s = d->socket;
	gint fd = -1;
	struct stat buf;
	gchar *data = NULL;
	gint r;

	g_snprintf(dl_tmp, sizeof(dl_tmp), "%s/%s", d->file_info->path,
		d->file_info->file_name);

	fd = open(dl_tmp, O_RDONLY);

	if (fd == -1) {
		const gchar * error = g_strerror(errno);
		g_warning("cannot check resuming for \"%s\": %s",
			d->file_info->file_name, error);
		download_stop(d, GTA_DL_ERROR, "Can't check resume data: %s", error);
		goto out;
	}

	if (-1 == fstat(fd, &buf)) {			/* Should never happen */
		const gchar *error = g_strerror(errno);
		g_warning("cannot stat opened \"%s\": %s",
			d->file_info->file_name, error);
		download_stop(d, GTA_DL_ERROR, "Can't stat opened file: %s", error);
		goto out;
	}

	/*
	 * Sanity check: if the file is bigger than when we started, abort
	 * immediately.
	 */

	if (!d->file_info->use_swarming && d->skip != buf.st_size) {
		g_warning("File '%s' changed size (now %lu, but was %u)",
			d->file_info->file_name, (gulong) buf.st_size, d->skip);
		download_stop(d, GTA_DL_STOPPED, "Stopped (Output file size changed)");
		goto out;
	}

	if (-1 == lseek(fd, d->skip - d->overlap_size, SEEK_SET)) {
		download_stop(d, GTA_DL_ERROR, "Unable to seek: %s",
			g_strerror(errno));
		goto out;
	}

	/*
	 * We're now at the overlapping start.  Read the data.
	 */

	data = g_malloc(d->overlap_size);
	r = read(fd, data, d->overlap_size);

	if (r < 0) {
		const gchar *error = g_strerror(errno);
		g_warning("cannot read resuming data for \"%s\": %s",
			d->file_info->file_name, error);
		download_stop(d, GTA_DL_ERROR, "Can't read resume data: %s", error);
		goto out;
	}

	if (r != d->overlap_size) {
		if (r == 0)
			download_stop(d, GTA_DL_STOPPED, "Stopped (EOF)");
		else {
			g_warning("Short read (%d instead of %d bytes) on resuming data "
				"for \"%s\"", r, d->overlap_size, d->file_info->file_name);
			download_stop(d, GTA_DL_ERROR, "Short read on resume data");
		}
		goto out;
	}

	if (0 != memcmp(s->buffer, data, d->overlap_size)) {
		download_stop(d, GTA_DL_ERROR, "Resuming data mismatch");
		if (dbg > 3)
			printf("%d overlapping bytes UNMATCHED at offset %d for \"%s\"\n",
				d->overlap_size, d->skip - d->overlap_size, d->file_name);
		goto out;
	}

	/*
	 * Remove the overlapping data from the socket buffer.
	 */

	if (s->pos > d->overlap_size)
		memmove(s->buffer, &s->buffer[d->overlap_size],
			s->pos - d->overlap_size);
	s->pos -= d->overlap_size;

	g_free(data);
	close(fd);

	if (dbg > 3)
		printf("%d overlapping bytes MATCHED at offset %d for \"%s\"\n",
			d->overlap_size, d->skip - d->overlap_size, d->file_name);

	return TRUE;

out:
	if (fd != -1)
		close(fd);
	if (data)
		g_free(data);

	return FALSE;
}

/*
 * download_write_data
 *
 * Write data in socket buffer to file.
 */
static void download_write_data(struct download *d)
{
	struct gnutella_socket *s = d->socket;
	gint written;

	g_assert(s->pos > 0);

	/*
	 * If we have d->pos == d->skip and a non-zero overlapping window, then
	 * the leading data we have in the buffer are overlapping data.
	 *		--RAM, 12/01/2002
	 */

	if (d->overlap_size && d->pos == d->skip) {
		if (s->pos < d->overlap_size)		/* Not enough bytes yet */
			return;							/* Don't even write anything */
		if (!download_overlap_check(d))		/* Mismatch on overlapped bytes? */
			return;							/* Download was stopped */
		if (s->pos == 0)					/* No bytes left to write */
			return;
		/* FALL THROUGH */
	}

	if (
		d->file_info->use_swarming &&
		-1 == lseek(d->file_desc, d->pos, SEEK_SET)
	) {
		const char *error = g_strerror(errno);
		g_warning("download_read(): failed to seek at offset %u (%s)",
			d->pos, error);
		download_stop(d, GTA_DL_ERROR, "Can't seek to offset %u: %s",
			d->pos, error);
	}

	if (-1 == (written = write(d->file_desc, s->buffer, s->pos))) {
		const char *error = g_strerror(errno);
		g_warning("download_read(): write to file failed (%s) !", error);
		g_warning("download_read: tried to write(%d, %p, %d)",
			  d->file_desc, s->buffer, s->pos);
		download_stop(d, GTA_DL_ERROR, "Can't save data: %s", error);
		return;
	} else if (written < s->pos) {
		g_warning("download_read(): "
			"partial write of %d out of %d bytes to file '%s'",
			written, s->pos, d->file_info->file_name);
		download_stop(d, GTA_DL_ERROR, "Partial write to file");
		return;
	}
	file_info_update(d, d->pos, d->pos + s->pos, DL_CHUNK_DONE);

	d->pos += s->pos;
	s->pos = 0;

	/*
	 * End download if we have completed it.
	 */

	if (d->file_info->use_swarming) {
 
		enum dl_chunk_status s = file_info_pos_status(d->file_info, d->pos);

		if (s == DL_CHUNK_DONE) {
			download_stop(d, GTA_DL_COMPLETED, NULL);
			if (d->file_info->done < d->file_info->size) {
				download_queue_delay(d, 2, "Requeued by competing download.");
			} else {
				guint32 val = total_downloads + 1;

				queue_remove_downloads_with_file(d->file_info);
				download_move_to_completed_dir(d);
				gnet_prop_set_guint32(PROP_TOTAL_DOWNLOADS, &val, 0, 1);
			}
		} else if (s == DL_CHUNK_EMPTY) {
			/*
			 * We have completed our busy-chunk, so we should extend it
			 * to avoid new downloads starting from here. (FIXME -- XXX)
			 */
		} else if (d->pos >= (d->skip + d->size)) {
			/*
			 * Growing the chunk size dynamically would be nice, but since
			 * since the Size column isn't updated regularly, let's
			 * just progress beyond 100% for now.
			 */
			/* d->size = d->pos - d->skip; */
		}

	} else if (d->file_info->done == d->file_info->size) {
		guint32 val = total_downloads+1;

		download_stop(d, GTA_DL_COMPLETED, NULL);
		download_move_to_completed_dir(d);
		gnet_prop_set_guint32(PROP_TOTAL_DOWNLOADS, &val, 0, 1);
	} else
		gui_update_download(d, FALSE);
}

/*
 * download_moved_permanently
 *
 * Refresh IP:port, download index and name, by looking at the new location
 * in the header ("Location:").
 *
 * Returns TRUE if we managed to parse the new location.
 */
static gboolean download_moved_permanently(struct download *d, header_t *header)
{
	gchar *buf;
	dmesh_urlinfo_t info;
	guint32 ip = download_ip(d);
	guint16 port = download_port(d);

	buf = header_get(header, "Location");
	if (buf == NULL)
		return FALSE;

	if (!dmesh_url_parse(buf, &info)) {
		g_warning("could not parse HTTP Location: %s", buf);
		return FALSE;
	}

	/*
	 * If ip/port changed, accept the new ones but warn.
	 */

	if (info.ip != ip || info.port != port)
		g_warning("server %s (file \"%s\") redirecting us to alien %s",
			ip_port_to_gchar(ip, port), d->file_name, buf);

	/*
	 * Check filename.
	 *
	 * If it changed, we don't change the output_name, so we'll continue
	 * to write to the same file we previously started with.
	 *
	 * NB: idx = 0 is used to indicate a /uri-res/N2R? URL, which we don't
	 * really want here (if we have the SHA1, we already asked for it).
	 */

	if (info.idx == 0) {
		g_warning("server %s (file \"%s\") would redirect us to %s",
			ip_port_to_gchar(ip, port), d->file_name, buf);
		atom_str_free(info.name);
		return FALSE;
	}

	if (0 != strcmp(info.name, d->file_name)) {
		g_warning("file \"%s\" was renamed \"%s\" on %s",
			d->file_name, info.name, ip_port_to_gchar(info.ip, info.port));

		/*
		 * If name changed, we must update the global hash counting downloads.
		 * We ensure the current download is in the running list, since only
		 * those can be stored in the hash.
		 */

		g_assert(d->list_idx == DL_LIST_RUNNING);

		//downloads_with_name_dec(d->file_name);
		//downloads_with_name_inc(info.name);

		/*
		 * About to free the d->file_name atom, but maybe the output_name
		 * was also the same.  In that case, we have to duplicate it, as if
		 * we had escaped '/' in the output file name.
		 *
		 * NB: This means the download will still be written to the previous
		 * filename, and therefore the actual filename displayed in the GUI
		 * will not be the targetted file on the disk.
		 */

		//if (d->output_name == d->file_name)
		//	d->output_name = g_strdup(d->file_name);

		atom_str_free(d->file_name);
		d->file_name = info.name;			/* Already an atom */
	} else
		atom_str_free(info.name);

	/*
	 * Update download structure.
	 */

	d->record_index = info.idx;
	download_redirect_to_server(d, info.ip, info.port);

	return TRUE;
}

/*
 * download_request
 *
 * Called to initiate the download once all the HTTP headers have been read.
 * Validate the reply, and begin saving the incoming data if OK.
 * Otherwise, stop the download.
 */
static void download_request(struct download *d, header_t *header)
{
	struct gnutella_socket *s = d->socket;
	gchar *status = getline_str(s->getline);
	gint ack_code;
	gchar *ack_message = "";
	gchar *buf;
	struct stat st;
	gboolean got_content_length = FALSE;
	gboolean got_new_server = FALSE;
	guint32 ip;
	guint16 port;

	d->last_update = time(NULL);	/* Done reading headers */

	if (dbg > 2) {
		printf("----Got reply from %s:\n", ip_to_gchar(s->ip));
		printf("%s\n", status);
		header_dump(header, stdout);
		printf("----\n");
		fflush(stdout);
	}

	ack_code = http_status_parse(status, "HTTP", &ack_message, NULL, NULL);

	/*
	 * Extract Server: header string, if present, and store it unless
	 * we already have it.
	 */

	buf = header_get(header, "Server");			/* Mandatory */
	if (!buf)
		buf = header_get(header, "User-Agent");	/* Maybe they're confused */

	if (buf) {
		struct dl_server *server = d->server;
		version_check(buf);
		if (server->vendor == NULL) {
			server->vendor = atom_str_get(buf);
			got_new_server = TRUE;
		} else if (0 != strcmp(server->vendor, buf)) {	/* Name changed? */
			atom_str_free(server->vendor);
			server->vendor = atom_str_get(buf);
			got_new_server = TRUE;
		}
	}

	/*
	 * Check status.
	 */

	if (ack_code == -1) {
		g_warning("weird HTTP acknowledgment status line from %s",
			ip_to_gchar(s->ip));
		dump_hex(stderr, "Status Line", status,
			MIN(getline_length(s->getline), 80));
		download_stop(d, GTA_DL_ERROR, "Weird HTTP status");
		return;
	}

	ip = download_ip(d);
	port = download_port(d);

	/*
	 * Check for X-Gnutella-Content-URN.
	 */
	
	if ((buf = header_get(header, "X-Gnutella-Content-Urn"))) {
		gchar *sha1 = strcasestr(buf, "urn:sha1:");	/* Case-insensitive */
		guchar digest[SHA1_RAW_SIZE];
		
		if (sha1) {
			sha1 += 9;		/* Skip "urn:sha1:" */
			if (!huge_http_sha1_extract32(sha1, digest))
				sha1 = NULL;
		}

		if (sha1) {
			if (d->sha1 && 0 != memcmp(digest, d->sha1, SHA1_RAW_SIZE)) {
				download_stop(d, GTA_DL_ERROR, "URN mismatch detected");
				return;
			}

			/*
			 * Record SHA1 if we did not know it yet.
			 */

			if (d->sha1 == NULL) {
				d->sha1 = atom_sha1_get(digest);
				download_store();		/* Save SHA1 */
				file_info_store();

				/*
				 * Insert record in download mesh if it does not require
				 * a push.  Since we just got a connection, we use "now"
				 * as the mesh timestamp.
				 */

				if (!d->always_push)
					dmesh_add(d->sha1, ip, port, d->record_index,
						d->file_name, 0);
			}

			/*
			 * Check for possible download mesh headers.
			 */

			huge_collect_locations(d->sha1, header);
		}
	}

	if (ack_code >= 200 && ack_code <= 299) {
		/* OK -- Update mesh */
		if (!d->always_push && d->sha1)
			dmesh_add(d->sha1, ip, port, d->record_index, d->file_name, 0);
	} else {
		guint delay = 0;

		/*
		 * Check for Retry-After.
		 *
		 * A Retry-After header is either a full HTTP date, such as
		 * "Fri, 31 Dec 1999 23:59:59 GMT", or an amount of seconds.
		 */

		buf = header_get(header, "Retry-After");
		if (buf) {
			if (!sscanf(buf, "%u", &delay)) {
				time_t now = time((time_t *) NULL);
				time_t retry = date2time(buf, &now);

				if (retry == -1)
					g_warning("cannot parse Retry-After: %s", buf);
				else
					delay = retry > now ? retry - now : 0;
			}
		}

		switch (ack_code) {
		case 301:				/* Moved permanently */
			if (!download_moved_permanently(d, header))
				break;
			download_queue_delay(d,
				delay ? delay : download_retry_busy_delay,
				"HTTP %d %s", ack_code, ack_message);
			return;
		case 400:				/* Bad request */
		case 404:				/* Could be sent if /uri-res not understood */
		case 401:				/* Idem, /uri-res is "unauthorized" */
		case 403:				/* Idem, /uri-res is "forbidden" */
		case 410:				/* Idem, /uri-res is "gone" */
		case 500:				/* Server error */
		case 501:				/* Not implemented */
			/*
			 * If we sent a "GET /uri-res/N2R?" and the remote
			 * server does not support it, then retry without it.
			 */
			if (download_retry_no_urires(d, delay, ack_code))
				return;
			break;
		case 408:				/* Request timeout */
		case 503:				/* Busy */
			/* Update mesh */
			if (!d->always_push && d->sha1)
				dmesh_add(d->sha1, ip, port, d->record_index, d->file_name, 0);

			/* No hammering */
			download_queue_delay(d,
				delay ? delay : download_retry_busy_delay,
				"HTTP %d %s", ack_code, ack_message);
			return;
		case 550:				/* Banned */
			download_queue_delay(d,
				delay ? delay : download_retry_refused_delay,
				"HTTP %d %s", ack_code, ack_message);
			return;
		default:
			break;
		}
		if (!d->always_push && d->sha1)
			dmesh_remove(d->sha1, ip, port, d->record_index, d->file_name);
		if (got_new_server)
			gui_update_download_server(d);
		download_stop(d, GTA_DL_ERROR, "HTTP %d %s", ack_code, ack_message);
		return;
	}

	/*
	 * We got a success status from the remote servent.  Parse header.
	 *
	 * Normally, a Content-Length: header is mandatory.  However, if we
	 * get a valid Content-Range, relax that constraint a bit.
	 *		--RAM, 08/01/2002
	 */

	if (got_new_server)
		gui_update_download_server(d);

	buf = header_get(header, "Content-Length");		/* Mandatory */
	if (buf) {
		guint32 z = atol(buf);
		guint32 server_size = z + d->skip - d->overlap_size;
		if (z == 0) {
			download_stop(d, GTA_DL_ERROR, "Bad length !?");
			return;
		} else if (server_size != d->file_info->size) {
			if (z == d->file_info->size) {
				g_warning("File '%s': server seems to have "
					"ignored our range request of %u.",
					d->file_name, d->file_info->size - d->overlap_size);
				download_stop(d, GTA_DL_ERROR,
					"Server can't handle resume request");
				return;
			} else {
				/*
				 * If the file on the server is greater than what we thought
				 * it would be, then probably they are sharing a file that
				 * they still receive.  Because now sevents start doing
				 * swarming, we cannot be sure that the file is complete
				 * without holes within the range we request.
				 *
				 * If the file is smaller than expected, then I don't know
				 * what's happening but in any case, don't proceed!
				 *
				 *		--RAM, 15/05/2002
				 */
				g_warning("File '%s': expected size %u but server said %u",
					d->file_name, d->file_info->size, server_size);
				download_stop(d, GTA_DL_ERROR, "File size mismatch");
				return;
			}
		}
		got_content_length = TRUE;
	}

	buf = header_get(header, "Content-Range");		/* Optional */
	if (buf) {
		guint32 start, end, total;
		if (
			sscanf(buf, "bytes %d-%d/%d", &start, &end, &total) ||	/* Good */
			sscanf(buf, "bytes=%d-%d/%d", &start, &end, &total)		/* Bad! */
		) {
			if (start != d->skip - d->overlap_size) {
				g_warning("File '%s': start byte mismatch: wanted %u, got %u",
					d->file_name, d->skip - d->overlap_size, start);
				download_stop(d, GTA_DL_ERROR, "Range start mismatch");
				return;
			}
			if (total != d->file_info->size) {
				g_warning("File '%s': file size mismatch: expected %u, got %u",
					d->file_name, d->file_info->size, total);
				download_stop(d, GTA_DL_ERROR, "File size mismatch");
				return;
			}
			got_content_length = TRUE;
		} else {
			g_warning("File '%s': malformed Content-Range: %s",
				d->file_name, buf);
		}
	}

	/*
	 * If neither Content-Length nor Content-Range was seen, abort!
	 *
	 * If we were talking to an official web-server, we'd assume the length
	 * to be correct and would be reading until EOF, but we're talking to
	 * an unknown party, that we cannot trust too much.
	 *		--RAM, 09/01/2002
	 */

	if (!got_content_length) {
		char *ua = header_get(header, "Server");
		ua = ua ? ua : header_get(header, "User-Agent");
		if (ua)
			g_warning("server \"%s\" did not send any length indication", ua);
		download_stop(d, GTA_DL_ERROR, "No Content-Length header");
		return;
	}

	/*
	 * Open output file.
	 */

	g_assert(d->file_desc == -1);

	g_snprintf(dl_tmp, sizeof(dl_tmp), "%s/%s", d->file_info->path,
			d->file_info->file_name);

	if (stat(dl_tmp, &st) != -1) {
		/* File exists, we'll append the data to it */
		if (!d->file_info->use_swarming && (st.st_size != d->skip)) {
			g_warning("File '%s' changed size (now %lu, but was %u)",
					d->file_info->file_name, (gulong) st.st_size, d->skip);
			download_stop(d, GTA_DL_STOPPED,
				"Stopped (Output file size changed)");
			return;
		}

		d->file_desc = open(dl_tmp, O_WRONLY);
	} else {
		if (!d->file_info->use_swarming && d->skip) {
			download_stop(d, GTA_DL_ERROR, "Cannot resume: file gone");
			return;
		}
		d->file_desc = open(dl_tmp, O_WRONLY | O_CREAT, 0644);
	}

	if (d->file_desc == -1) {
		const gchar *error = g_strerror(errno);
		g_warning("Unable to open file '%s' for writing! (%s)",
			dl_tmp, error);
		download_stop(d, GTA_DL_ERROR, "Cannot write into file: %s", error);
		return;
	}

	if (d->skip && -1 == lseek(d->file_desc, d->skip, SEEK_SET)) {
		download_stop(d, GTA_DL_ERROR, "Unable to seek: %s",
			g_strerror(errno));
		return;
	}

	/*
	 * We're ready to receive.
	 */

	io_free(d->io_opaque);

	d->start_date = time((time_t *) NULL);
	d->status = GTA_DL_RECEIVING;

	dl_establishing--;
	dl_active++;
	g_assert(dl_establishing >= 0);

	gui_update_download(d, TRUE);
	gui_update_c_downloads(dl_active, dl_establishing + dl_active);

	g_assert(s->gdk_tag == 0);
	g_assert(d->bio == NULL);

	d->bio = bsched_source_add(bws.in, s->file_desc,
		BIO_F_READ, download_read, (gpointer) d);

	/*
	 * Increase our reception window to maximize throughput.
	 */

	sock_recv_buf(s, DOWNLOAD_RECV_BUFSIZE, FALSE);

	/*
	 * If we have something in the input buffer, write the data to the
	 * file immediately.  Note that this may close the download immediately
	 * if the whole file was already read in the socket buffer.
	 */

	if (s->pos > 0)
		download_write_data(d);
}

/*
 * download_read
 *
 * Read callback for file data.
 */
static void download_read(gpointer data, gint source, GdkInputCondition cond)
{
	struct download *d = (struct download *) data;
	struct gnutella_socket *s;
	gint32 r;
	gint32 to_read, remains;

	g_return_if_fail(d);
	s = d->socket;
	g_return_if_fail(s);

	if (cond & GDK_INPUT_EXCEPTION) {
		download_stop(d, GTA_DL_ERROR, "Failed (Input Exception)");
		return;
	}

	g_assert(s->pos >= 0 && s->pos <= sizeof(s->buffer));

	if (s->pos == sizeof(s->buffer)) {
		download_stop(d, GTA_DL_STOPPED, "Stopped (Read buffer full)");
		return;
	}

	g_assert(d->pos <= d->file_info->size);

	if (d->pos == d->file_info->size) {
		download_stop(d, GTA_DL_ERROR, "Failed (Completed?)");
		return;
	}

	remains = sizeof(s->buffer) - s->pos;
	to_read = d->file_info->size - d->pos;
	if (remains < to_read)
		to_read = remains;			/* Only read to fill buffer */

	r = bio_read(d->bio, s->buffer + s->pos, to_read);

	if (r <= 0) {
		if (r == 0) {
			download_stop(d, GTA_DL_STOPPED, "Stopped (EOF)");
		} else if (errno != EAGAIN) {
			if (errno == ECONNRESET)
				download_stop(d, GTA_DL_STOPPED,
					"Stopped (%s)", g_strerror(errno));
			else
				download_stop(d, GTA_DL_ERROR,
					"Failed (Read error: %s)", g_strerror(errno));
		}
		return;
	}

	s->pos += r;
	d->last_update = time((time_t *) 0);

	g_assert(s->pos > 0);

	download_write_data(d);
}

/*
 * Send the HTTP request for a download, then prepare I/O reading callbacks
 * to read the incoming status line and following headers.
 */
gboolean download_send_request(struct download *d)
{
	struct gnutella_socket *s = d->socket;
	struct io_header *ih;
	gint rw;
	gint sent;
	gboolean n2r = FALSE;

	g_return_val_if_fail(d, FALSE);

	if (!s) {
		g_warning("download_send_request(): No socket for '%s'", d->file_name);
		download_stop(d, GTA_DL_ERROR, "Internal Error");
		return FALSE;
	}

	/*
	 * If we have d->always_push set, yet we did not use a Push, it means we
	 * finally tried to connect directly to this server.  And we succeeded!
	 *		-- RAM, 18/08/2002.
	 */

	if (d->always_push && !DOWNLOAD_IS_IN_PUSH_MODE(d)) {
		if (dbg > 2)
			printf("PUSH not necessary to reach %s\n",
				ip_port_to_gchar(download_ip(d), download_port(d)));
		d->server->attrs |= DLS_A_PUSH_IGN;
		d->always_push = FALSE;
	}

	g_assert(d->overlap_size <= sizeof(s->buffer));

	/*
	 * When we have a SHA1, the remote host normally supports HUGE, and
	 * therefore should understand our "GET /uri-res/N2R?" query.
	 * However, I'm suspicious, so we track our attempts and don't send
	 * the /uri-res when we have evidence the remote host does not support it.
	 *
	 * When we got a GIV request, don't take the chance that /uri-res be
	 * not understood and request the file.
	 *
	 *		--RAM, 14/06/2002
	 */

	if (d->sha1 && !d->push && !(d->server->attrs & DLS_A_NO_URIRES)) {
		d->flags |= DL_F_URIRES;
		n2r = TRUE;
	}

	/*
	 * Build the HTTP request.
	 */

	if (n2r)
		rw = g_snprintf(dl_tmp, sizeof(dl_tmp),
			"GET /uri-res/N2R?urn:sha1:%s HTTP/1.0\r\n",
			sha1_base32(d->sha1));
	else
		rw = g_snprintf(dl_tmp, sizeof(dl_tmp),
			"GET /get/%u/%s HTTP/1.0\r\n",
			d->record_index, d->file_name);

	rw += g_snprintf(&dl_tmp[rw], sizeof(dl_tmp)-rw,
		"User-Agent: %s\r\n"
		"Connection: close\r\n",
		version_string);

	if (d->skip)
		rw += g_snprintf(&dl_tmp[rw], sizeof(dl_tmp)-rw,
			"Range: bytes=%u-\r\n",
			d->skip - d->overlap_size);

	g_assert(rw + 3 < sizeof(dl_tmp));		/* Should not have filled yet! */

	if (d->sha1) {
		gint wmesh;
		gint sha1_room;

		/*
		 * Leave room for the urn:sha1: possibly, plus final 2 * "\r\n".
		 */

		sha1_room = 33 + SHA1_BASE32_SIZE + 4;

		/*
		 * Send to the server any new alternate locations we may have
		 * learned about since the last time.
		 */

		wmesh = dmesh_alternate_location(d->sha1,
			&dl_tmp[rw], sizeof(dl_tmp)-(rw+sha1_room),
			download_ip(d), d->last_dmesh);
		rw += wmesh;

		d->last_dmesh = (guint32) time(NULL);

		/*
		 * HUGE specs says that the alternate locations are only defined
		 * when there is an X-Gnutella-Content-URN present.  When we use
		 * the N2R form to retrieve a resource by SHA1, that line is
		 * redundant.  We only send it if we sent mesh information.
		 */

		if (!n2r || wmesh)
			rw += g_snprintf(&dl_tmp[rw], sizeof(dl_tmp)-rw,
				"X-Gnutella-Content-URN: urn:sha1:%s\r\n",
				sha1_base32(d->sha1));
	}

	rw += g_snprintf(&dl_tmp[rw], sizeof(dl_tmp)-rw, "\r\n");

	/*
	 * Send the HTTP Request
	 */

	if (-1 == (sent = bws_write(bws.out, d->socket->file_desc, dl_tmp, rw))) {
		download_stop(d, GTA_DL_ERROR, "Write failed: %s", g_strerror(errno));
		return FALSE;
	} else if (sent < rw) {
		download_stop(d, GTA_DL_ERROR, "Partial write: wrote %d of %d bytes",
			sent, rw);
		return FALSE;
	} else if (dbg > 2) {
		printf("----Sent Request to %s:\n%.*s----\n",
			ip_port_to_gchar(download_ip(d), download_port(d)),
				(int) rw, dl_tmp);
		fflush(stdout);
	}

	/*
	 * Update status and GUI.
	 */

	d->last_update = time((time_t *) 0);
	d->status = GTA_DL_REQ_SENT;
	gui_update_download(d, TRUE);

	/*
	 * Now prepare to read the status line and the headers.
	 * XXX separate this to swallow 100 continuations?
	 */

	g_assert(d->io_opaque == NULL);

	ih = (struct io_header *) g_malloc(sizeof(struct io_header));
	ih->download = d;
	ih->header = header_make();
	ih->getline = getline_make();
	ih->flags = IO_STATUS_LINE;		/* First line will be a status line */
	ih->process_header = call_download_request;
	d->io_opaque = (gpointer) ih;

	g_assert(s->gdk_tag == 0);

	s->gdk_tag = gdk_input_add(s->file_desc,
		GDK_INPUT_READ | GDK_INPUT_EXCEPTION,
		download_header_read, (gpointer) ih);

	return TRUE;
}

/*
 * download_push_ready
 *
 * Send download request on the opened connection.
 *
 * Header processing callback, invoked when we have read the second "\n" at
 * the end of the GIV string.
 */
static void download_push_ready(struct download *d, getline_t *empty)
{
	gint len = getline_length(empty);

	if (len != 0) {
		g_warning("File '%s': push reply was not followed by an empty line",
			d->file_name);
		dump_hex(stderr, "Extra GIV data", getline_str(empty), MIN(len, 80));
		download_stop(d, GTA_DL_ERROR, "Malformed push reply");
		return;
	}

	/*
	 * Free up the s->getline structure which holds the GIV line.
	 */

	g_assert(d->socket->getline);
	getline_free(d->socket->getline);
	d->socket->getline = NULL;

	io_free(d->io_opaque);
	download_send_request(d);		/* Will install new I/O data */
}

/*
 * select_push_download
 *
 * On reception of a "GIV index:GUID" string, select the appropriate download
 * to request.
 *
 * Returns the selected download, or NULL if we could not find one.
 */
static struct download *select_push_download(guint file_index, gchar *hex_guid)
{
	struct download *d = NULL;
	GSList *list;
	guchar rguid[16];		/* Remote GUID */
	GList *l;

	g_strdown(hex_guid);
	g_snprintf(dl_tmp, sizeof(dl_tmp), "%u:%s", file_index, hex_guid);

	list = (GSList *) g_hash_table_lookup(pushed_downloads, (gpointer) dl_tmp);
	if (list) {
		d = (struct download *) list->data;			/* Take first entry */
		g_assert(d->record_index == file_index);
	} else if (dbg > 3)
		printf("got unexpected GIV: nothing pending currently\n");

	/*
	 * We might get another GIV for the same download: we send two pushes
	 * in a row, and with the propagation delay, the first gets handled
	 * after we sent the second push.  We'll get a GIV for an already
	 * connected download.
	 *
	 * We check two things: that we're not already connected (has a socket)
	 * and that we're in a state where we can expect a GIV string.  Doing
	 * the two tests add robustness, since they are overlapping, but not
	 * completely equivalent (if we're in the queued state, for instance).
	 */

	if (d) {
		if (d->socket) {
			if (dbg > 3)
				printf("got concurrent GIV: download is connected, state %d\n",
					d->status);
			d = NULL;
		} else if (!DOWNLOAD_IS_EXPECTING_GIV(d)) {
			if (dbg > 3)
				printf("got GIV string for download in state %d\n",
					d->status);
			d = NULL;
		}
	}

	if (d)
		return d;

	/*
	 * Whilst we are connected to that servent, find a suitable download
	 * we could request.
	 */

	hex_to_guid(hex_guid, rguid);

#if 0		/* XXX do not limit by download slot */
	/*
	 * If we have already reached our maximum amount of concurrent downloads,
	 * or if we have reached our maximum amount of downloads for this host,
	 * then abort: this connection is wasted.
	 *
	 * XXX As a future enhancement, we could instead "kill" a non-active
	 * XXX download for which no push is done, to give priority to the
	 * XXX push, which is precious since we need a route to the remote
	 * XXX servent to initiate the requests. --RAM, 20/01/2002
	 */

	if (count_running_downloads() >= max_downloads) {
		g_warning("discarding GIV from %s: no more download slot",
			guid_hex_str(rguid));
		return NULL;
	}
#endif

	/*
	 * Look for a queued download on this host that we could request.
	 */

	for (l = dl_by_time; l; l = l->next) {
		struct dl_server *server = (struct dl_server *) l->data;
		extern gint guid_eq(gconstpointer a, gconstpointer b);
		GList *w;

		/*
		 * There might be several hosts with the same GUID (Mallory nodes).
		 */

		if (!guid_eq(rguid, server->key->guid))
			continue;

		if (
			server->count[DL_LIST_WAITING] == 0 ||
			count_running_on_server(server) >= max_host_downloads
		)
			continue;

		for (w = server->list[DL_LIST_WAITING]; w; w = g_list_next(w)) {
			struct download *d = (struct download *) w->data;

			g_assert(!DOWNLOAD_IS_RUNNING(d));

			if (count_running_downloads_with_name(d->file_info->file_name) != 0)
				continue;

			if (dbg > 4)
				printf("GIV: trying alternate download '%s' from %s at %s\n",
					d->file_name, guid_hex_str(rguid),
					ip_port_to_gchar(download_ip(d), download_port(d)));

			/*
			 * Only prepare the download, don't call download_start(): we
			 * already have the connection, and simply need to prepare the
			 * range offset.
			 */

			g_assert(d->socket == NULL);

			if (download_start_prepare(d)) {
				d->status = GTA_DL_CONNECTING;
				if (!DOWNLOAD_IS_VISIBLE(d))
					download_gui_add(d);

				gui_update_download(d, TRUE);
				gui_update_c_downloads(dl_active, dl_establishing + dl_active);

				if (dbg > 4)
					printf("GIV: selected alternate download '%s' from %s\n",
						d->file_name, guid_hex_str(rguid));

				return d;
			}
		}
	}

	g_warning("discarding GIV from %s: no suitable alternate found",
		guid_hex_str(rguid));

	return NULL;
}

/*
 * download_push_ack
 *
 * Initiate download on the remotely initiated connection.
 *
 * This is called when an incoming "GIV" request is received in answer to
 * some of our pushes.
 */
void download_push_ack(struct gnutella_socket *s)
{
	struct download *d = NULL;
	gchar *giv;
	guint file_index;		/* The requested file index */
	gchar hex_guid[33];		/* The hexadecimal GUID */
	struct io_header *ih;

	g_assert(s->getline);

	giv = getline_str(s->getline);

	if (dbg > 4) {
		printf("----Got GIV from %s:\n", ip_to_gchar(s->ip));
		printf("%s\n", giv);
		printf("----\n");
		fflush(stdout);
	}

	/*
	 * To find out which download this is, we have to parse the incoming
	 * GIV request, which is stored in "s->getline".
	 */

	if (!sscanf(giv, "GIV %u:%32c/", &file_index, hex_guid)) {
		g_warning("malformed GIV string: %s", giv);
		socket_destroy(s);
		return;
	}

	/*
	 * Look for a recorded download.
	 */

	hex_guid[32] = '\0';
	d = select_push_download(file_index, hex_guid);
	if (!d) {
		g_warning("discarded GIV string: %s", giv);
		socket_destroy(s);
		return;
	}

	/*
	 * Install socket for the download.
	 */

	g_assert(d->socket == NULL);

	d->last_update = time((time_t *) NULL);
	d->socket = s;
	s->resource.download = d;

	/*
	 * Now we have to read that trailing "\n" which comes right afterwards.
	 */

	ih = (struct io_header *) g_malloc(sizeof(struct io_header));
	ih->download = d;
	ih->header = NULL;				/* Won't be needed, we read one line */
	ih->getline = getline_make();
	ih->flags = IO_ONE_LINE;		/* Process one line (will be empty) */
	ih->process_header = call_download_push_ready;
	d->io_opaque = ih;

	g_assert(s->gdk_tag == 0);

	s->gdk_tag = gdk_input_add(s->file_desc,
		GDK_INPUT_READ | GDK_INPUT_EXCEPTION,
		download_header_read, (gpointer) ih);

	download_header_parse(ih);		/* Data might already be there */
}

void download_retry(struct download *d)
{
	g_assert(d != NULL);

	/* download_stop() sets the time, so all we need to do is set the delay */

	if (d->timeout_delay == 0)
		d->timeout_delay = download_retry_timeout_min;
	else {
		d->timeout_delay *= 2;
		if (d->start_date) {
			/* We forgive a little while the download is working */
			d->timeout_delay -=
				(time((time_t *) NULL) - d->start_date) / 10;
		}
	}

	if (d->timeout_delay < download_retry_timeout_min)
		d->timeout_delay = download_retry_timeout_min;
	if (d->timeout_delay > download_retry_timeout_max)
		d->timeout_delay = download_retry_timeout_max;

	download_stop(d, GTA_DL_TIMEOUT_WAIT, NULL);
}

/***
 *** Queue persistency routines
 ***/

static gchar *download_file = "downloads";
static gboolean retrieving = FALSE;

/*
 * download_store
 *
 * Store all pending downloads that are not in PUSH mode (since we'll loose
 * routing information when we quit).
 *
 * The downloads are normally stored in ~/.gtk-gnutella/downloads.
 */
static void download_store(void)
{
	FILE *out;
	GSList *l;
	time_t now = time((time_t *) NULL);
	gchar filename[1024];

	if (retrieving)
		return;

	g_snprintf(dl_tmp, sizeof(dl_tmp), "%s/%s.new", config_dir, download_file);
	out = fopen(dl_tmp, "w");

	if (!out) {
		g_warning("Unable to create %s to persist downloads: %s",
			dl_tmp, g_strerror(errno));
		return;
	}

	fputs("# THIS FILE IS AUTOMATICALLY GENERATED -- DO NOT EDIT\n", out);
	fprintf(out, "#\n# Downloads saved on %s#\n\n", ctime(&now));
	fputs("#\n# Format is:\n", out);
	fputs("#   File name\n", out);
	fputs("#   size, index:GUID, IP:port\n", out);
	fputs("#   SHA1 or * if none\n", out);
	fputs("#   <blank line>\n", out);
	fputs("#\n\n", out);

	fputs("RECLINES=3\n\n", out);

	for (l = sl_downloads; l; l = l->next) {
		struct download *d = (struct download *) l->data;
		gchar *escaped;

		if (d->status == GTA_DL_COMPLETED)
			continue;
		if (d->always_push)
			continue;

		escaped = url_escape_cntrl(d->file_name);	/* Protect against "\n" */

		fprintf(out, "%s\n", escaped);
		fprintf(out, "%u, %u:%s, %s\n",
			d->file_info->size,
			d->record_index, guid_hex_str(download_guid(d)),
			ip_port_to_gchar(download_ip(d), download_port(d)));
		fprintf(out, "%s\n\n",
			d->sha1 ? sha1_base32(d->sha1) : "*");

		if (escaped != d->file_name)				/* Lazily dup'ed */
			g_free(escaped);
	}

	if (0 == fclose(out)) {
		g_snprintf(filename, sizeof(filename), "%s/%s",
			config_dir, download_file);

		if (-1 == rename(dl_tmp, filename))
			g_warning("could not rename %s as %s: %s",
				dl_tmp, filename, g_strerror(errno));
	} else
		g_warning("could not flush %s: %s", dl_tmp, g_strerror(errno));
}

/*
 * download_retrieve
 *
 * Retrieve download list and requeue each download.
 * The downloads are normally retrieved from ~/.gtk-gnutella/downloads.
 */
static void download_retrieve(void)
{
	FILE *in;
	guchar d_guid[16];		/* The d_ vars are what we deserialize */
	guint32 d_size;
	gchar *d_name;
	guint32 d_ip;
	guint16 d_port;
	guint32 d_index;
	gchar d_hexguid[33];
	gchar d_ipport[23];
	gint recline;			/* Record line number */
	gint line;				/* File line number */
	gchar filename[1024];
	guchar sha1_digest[SHA1_RAW_SIZE];
	gboolean has_sha1 = FALSE;
	gint maxlines = -1;

	g_snprintf(dl_tmp, sizeof(dl_tmp), "%s/%s", config_dir, download_file);

	in = fopen(dl_tmp, "r");

	/*
	 * Rename "downloads" as "downloads.orig", so that the original file is
	 * kept around some time for recovery purposes.
	 *
	 * Also if we can't open "downloads" but there is a "downloads.orig", then
	 * open it instead: it probably means that we started but were suddenly
	 * killed in the middle of this routine, before we were able to persist
	 * it again.  (The alternative being that we could not persist the
	 * downloads back for some reason, later on during processing, before
	 * quitting, or whatever).
	 */

	g_snprintf(filename, sizeof(filename), "%s/%s.orig",
		config_dir, download_file);

	if (in) {
		if (-1 == rename(dl_tmp, filename))
			g_warning("could not rename %s as %s: %s",
				dl_tmp, filename, g_strerror(errno));
	} else {
		const gchar *error = g_strerror(errno);
		struct stat buf;
		gchar *instead = " instead";

		if (-1 == stat(dl_tmp, &buf))
			instead = "";				/* OK, we can't open a missing file */
		else
			g_warning("unable to open \"%s\" to retrieve downloads: %s",
				dl_tmp, error);

		in = fopen(filename, "r");
		if (!in)
			return;

		g_warning("retrieving downloads from \"%s\"%s", filename, instead);
	}

	/*
	 * Retrieval algorithm:
	 *
	 * Lines starting with a # are skipped.
	 *
	 * We read the ines that make up each serialized record, and
	 * recreate the download.  We stop as soon as we encounter an
	 * error.
	 */

	retrieving = TRUE;			/* Prevent download_store() runs */

	line = recline = 0;
	d_name = NULL;

	while (fgets(dl_tmp, sizeof(dl_tmp) - 1, in)) {	/* Room for trailing NUL */
		line++;

		if (dl_tmp[0] == '#')
			continue;				/* Skip comments */

		/*
		 * We emitted a "RECLINES=x" at store time to indicate the amount of
		 * lines each record takes.
		 */

		if (maxlines < 0 && dl_tmp[0] == 'R') {
			if (1 == sscanf(dl_tmp, "RECLINES=%d", &maxlines))
				continue;
		}

		if (dl_tmp[0] == '\n') {
			if (recline == 0)
				continue;			/* Allow arbitrary blank lines */

			g_warning("download_retrieve: "
				"Unexpected empty line #%d, aborting", line);
			goto out;
		}

		recline++;					/* We're in a record */

		switch (recline) {
		case 1:						/* The file name */
			(void) str_chomp(dl_tmp, 0);
			(void) url_unescape(dl_tmp, TRUE);	/* Un-escape in place */
			d_name = atom_str_get(dl_tmp);

			/*
			 * Backward compatibility with 0.85, which did not have the
			 * "RECLINE=x" line.  If we reached the first record line, then
			 * either we saw that line in recent versions, or we did not and
			 * we know we had only 2 lines per record.
			 */

			if (maxlines < 0)
				maxlines = 2;

			continue;
		case 2:						/* Other information */
			g_assert(d_name);

			if (
				sscanf(dl_tmp, "%u, %u:%32c, %22s",
					&d_size, &d_index, d_hexguid, d_ipport) < 4
			) {
				(void) str_chomp(dl_tmp, 0);
				g_warning("download_retrieve: "
					"cannot parse line #%d: %s", line, dl_tmp);
				goto out;
			}

			d_ipport[22] = '\0';
			if (!gchar_to_ip_port(d_ipport, &d_ip, &d_port)) {
				g_warning("download_retrieve: "
					"bad IP:port '%s' at line #%d, aborting", d_ipport, line);
				goto out;
			}
			if (maxlines == 2)
				break;
			continue;
		case 3:
			if (maxlines != 3) {
				g_warning("download_retrieve: "
					"can't handle %d lines in records, aborting", maxlines);
				goto out;
			}

			if (dl_tmp[0] == '*')
				break;
			if (
				strlen(dl_tmp) != (1+SHA1_BASE32_SIZE) ||	/* Final "\n" */
				!base32_decode_into(dl_tmp, SHA1_BASE32_SIZE,
					sha1_digest, sizeof(sha1_digest))
			) {
				g_warning("download_retrieve: "
					"bad base32 SHA1 '%32s' at line #%d, ignoring",
					dl_tmp, line);
			} else
				has_sha1 = TRUE;
			break;
		default:
			g_warning("download_retrieve: "
				"Too many lines for record at line #%d, aborting", line);
			goto out;
		}

		/*
		 * At the last line of the record.
		 */

		hex_to_guid(d_hexguid, d_guid);

		/*
		 * Download is created with a timestamp of `1' so that it is very
		 * old and the entry does not get added to the download mesh yet.
		 */

		create_download(d_name, NULL, d_size, d_index, d_ip, d_port, d_guid,
			has_sha1 ? sha1_digest : NULL, 1, FALSE, FALSE);

		/*
		 * Don't free `d_name', we gave it to create_download()!
		 */

		d_name = NULL;
		recline = 0;				/* Mark the end */
		has_sha1 = FALSE;

	}

out:
	retrieving = FALSE;			/* Re-enable download_store() runs */

	if (d_name)
		atom_str_free(d_name);

	fclose(in);
	download_store();			/* Persist what we have retrieved */
}

void download_close(void)
{
	GSList *l;

	download_store();			/* Save latest copy */
	download_freeze_queue(TRUE);

	for (l = sl_downloads; l; l = l->next) {
		struct download *d = (struct download *) l->data;
		if (d->socket)
			socket_free(d->socket);
		if (d->push)
			download_push_remove(d);
		if (d->io_opaque)
			io_free(d->io_opaque);
		if (d->bio)
			bsched_source_remove(d->bio);
		if (d->sha1)
			atom_sha1_free(d->sha1);
		if (d->restart_timer_id)
			g_source_remove(d->restart_timer_id);
		file_info_free(d->file_info, FALSE);
		download_remove_from_server(d);
		atom_str_free(d->file_name);

		g_free(d);
	}

	g_slist_free(sl_downloads);
	g_slist_free(sl_unqueued);
	g_hash_table_destroy(pushed_downloads);

	file_info_close();

	// XXX free & check other hash tables as well.
}

/* vi: set ts=4: */
