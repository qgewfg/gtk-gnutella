/*
 * $Id$
 *
 * Copyright (c) 2002-2003, Raphael Manfredi
 *
 * Gnutella Messages.
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

#ifndef _core_gmsg_h_
#define _core_gmsg_h_

#include "gnutella.h"
#include "pmsg.h"

#include "if/core/search.h"

struct gnutella_node;
struct route_dest;
struct mqueue;

#define gmsg_function(p) (((struct gnutella_header *) p)->function)
#define gmsg_hops(p)     (((struct gnutella_header *) p)->hops)

/*
 * Public interface
 */

void gmsg_init(void);
const gchar *gmsg_name(guint function);

pmsg_t *gmsg_to_pmsg(gconstpointer msg, guint32 size);
pmsg_t *gmsg_to_ctrl_pmsg(gconstpointer msg, guint32 size);
pmsg_t * gmsg_to_ctrl_pmsg_extend(
	gconstpointer msg, guint32 size, pmsg_free_t free_cb, gpointer arg);
pmsg_t *gmsg_split_to_pmsg(gconstpointer head,
	gconstpointer data, guint32 size);
pmsg_t * gmsg_split_to_pmsg_extend(gconstpointer head, gconstpointer data,
	guint32 size, pmsg_free_t free_cb, gpointer arg);

void gmsg_mb_sendto_all(const GSList *sl, pmsg_t *mb);
void gmsg_mb_sendto_one(struct gnutella_node *n, pmsg_t *mb);

void gmsg_sendto_one(struct gnutella_node *n, gconstpointer msg, guint32 size);
void gmsg_ctrl_sendto_one(struct gnutella_node *n,
	gconstpointer msg, guint32 size);
void gmsg_split_sendto_one(struct gnutella_node *n,
	gconstpointer head, gconstpointer data, guint32 size);
void gmsg_sendto_all(const GSList *l, gconstpointer msg, guint32 size);
void gmsg_split_sendto_all(const GSList *l,
	gconstpointer head, gconstpointer data, guint32 size);
void gmsg_split_sendto_all_but_one(const GSList *l, struct gnutella_node *n,
	gconstpointer head, gconstpointer data, guint32 size);
void gmsg_sendto_route(struct gnutella_node *n, struct route_dest *rt);

gboolean gmsg_can_drop(gconstpointer pdu, gint size);
gboolean gmsg_is_oob_query(gconstpointer msg);
gboolean gmsg_split_is_oob_query(gconstpointer head, gconstpointer data);
gint gmsg_cmp(gconstpointer pdu1, gconstpointer pdu2);
gchar *gmsg_infostr(gconstpointer head);
gchar *gmsg_infostr_full(gconstpointer message);
gchar *gmsg_infostr_full_split(gconstpointer head, gconstpointer data);

void gmsg_install_presend(pmsg_t *mb);

void gmsg_log_dropped(gconstpointer head,
	const gchar *reason, ...) G_GNUC_PRINTF(2, 3);
void gmsg_log_bad(const struct gnutella_node *n,
	const gchar *reason, ...) G_GNUC_PRINTF(2, 3);

void gmsg_sendto_route_ggep(
	struct gnutella_node *n, struct route_dest *rt, gint regular_size);
void gmsg_sendto_one_ggep(struct gnutella_node *n,
	gconstpointer msg, guint32 size, guint32 regular_size);
void gmsg_ctrl_sendto_one_ggep(struct gnutella_node *n,
	gconstpointer msg, guint32 size, guint32 regular_size);
void gmsg_sendto_all_ggep(const GSList *sl,
	gconstpointer msg, guint32 size, guint32 regular_size);

void gmsg_search_sendto_one(struct gnutella_node *n, gnet_search_t sh,
	gconstpointer msg, guint32 size);
void gmsg_search_sendto_all(const GSList *l, gnet_search_t sh,
	gconstpointer msg, guint32 size);

#endif	/* _core_gmsg_h_ */

/* vi: set ts=4 sw=4 cindent: */
