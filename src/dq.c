/*
 * $Id$
 *
 * Copyright (c) 2004, Raphael Manfredi
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

/**
 * @file
 *
 * Dynamic querying.
 */

#include "gnutella.h"

RCSID("$Id$");

#include <stdlib.h>			/* For qsort() */
#include <math.h>			/* For pow() */
#include <glib.h>

#include "dq.h"
#include "glib-missing.h"
#include "misc.h"
#include "atoms.h"
#include "mq.h"
#include "gmsg.h"
#include "walloc.h"
#include "pmsg.h"
#include "nodes.h"
#include "cq.h"
#include "gnet_search.h"
#include "gnet_stats.h"
#include "qrp.h"
#include "override.h"		/* Must be the last header included */

#define DQ_MAX_LIFETIME		300000	/* 5 minutes, in ms */
#define DQ_PROBE_TIMEOUT  	1000	/* 1 s extra per connection */
#define DQ_PENDING_TIMEOUT 	1000	/* 1 s extra per pending message */
#define DQ_QUERY_TIMEOUT	3400	/* 3.4 s */
#define DQ_TIMEOUT_ADJUST	100		/* 100 ms at each connection */
#define DQ_MIN_TIMEOUT		1500	/* 1.5 s at least between queries */
#define DQ_LINGER_TIMEOUT	120000	/* 2 minutes, in ms */

#define DQ_LEAF_RESULTS		50		/* # of results targetted for leaves */
#define DQ_LOCAL_RESULTS	150		/* # of results for local queries */
#define DQ_PROBE_UP			3		/* Amount of UPs for initial probe */
#define DQ_MAX_HORIZON		500000	/* Stop search after that many UP queried */
#define DQ_MIN_HORIZON		3000	/* Min horizon before timeout adjustment */
#define DQ_MAX_RESULTS		10		/* After DQ_MIN_HORIZON queried for adj. */

#define DQ_MAX_TTL			5		/* Max TTL we can use */

#define DQ_MQ_EPSILON		2048	/* Queues identical at +/- 2K */
#define DQ_FUZZY_FACTOR		0.80	/* Corrector for theoretical horizon */

/*
 * Compute start of search string (which is NUL terminated) in query.
 * The "+2" skips the "speed" field in the query.
 */
#define QUERY_TEXT(m)	((m) + sizeof(struct gnutella_header) + 2)

/*
 * The dynamic query.
 */
typedef struct dquery {
	guint32 qid;			/* Unique query ID, to detect ghosts */
	guint32 node_id;		/* ID of the node that originated the query */
	guint32 flags;			/* Operational flags */
	gnet_search_t sh;		/* Search handle, if node ID = NODE_ID_LOCAL */
	pmsg_t *mb;				/* The search messsage "template" */
	query_hashvec_t *qhv;	/* Query hash vector for QRP filtering */
	GHashTable *queried;	/* Contains node IDs that we queried so far */
	guint8 ttl;				/* Initial query TTL */
	guint32 horizon;		/* Theoretical horizon reached thus far */
	guint32 up_sent;		/* # of UPs to which we really sent our query */
	guint32 pending;		/* Pending query messages not ACK'ed yet by mq */
	guint32 max_results;	/* Max results we're targetting for */
	guint32 results;		/* Results we got so far for the query */
	guint32 linger_results;	/* Results we got whilst lingering */
	guint32 kept;			/* Results they say they kept after filtering */
	guint32 result_timeout;	/* The current timeout for getting results */
	gpointer expire_ev;		/* Callout queue global expiration event */
	gpointer results_ev;	/* Callout queue results expiration event */
	time_t start;			/* Time at which it started */
	time_t stop;			/* Time at which it was terminated */
	pmsg_t *by_ttl[DQ_MAX_TTL];	/* Copied mesages, one for each TTL */
} dquery_t;

#define DQ_F_ID_CLEANING	0x00000001	/* Cleaning the `by_node_id' table */
#define DQ_F_LINGER			0x00000002	/* Lingering to monitor extra results */

/*
 * This table keeps track of all the dynamic query objects that we have
 * created and which are alive.
 */
static GHashTable *dqueries = NULL;

/*
 * This table keeps track of all the dynamic query objects created
 * for a given node ID.  The key is an integer atom (the node ID) and
 * the value is a GSList containing all the queries for that node.
 */
static GHashTable *by_node_id = NULL;

/*
 * This table keeps track of the association between a MUID and the
 * dynamic query, so that when results come back, we may account them
 * for the relevant query.
 *
 * The keys are MUIDs (GUID atoms), the values are the dquery_t object.
 */
static GHashTable *by_muid = NULL;

/*
 * Information about query messages sent.
 *
 * We can't really add too many fields to the pmsg_t blocks we enqueue.
 * However, what we do is we extend the pmsg_t to enrich them with a free
 * routine, and we use that fact to be notified by the message queue when
 * a message is freed.  We can then probe into the flags to see whether
 * it was sent.
 *
 * But adding a free routine is about as much as we can do with a generic
 * message system.  To be able to keep track of more information about the
 * queries we send, we associate each message with a structure containing
 * meta-information about it.
 */
struct pmsg_info {
	dquery_t *dq;			/* The dynamic query that sent the query */
	guint32 qid;			/* Query ID of the dynamic query */
	guint32 node_id;		/* The ID of the node we sent it to */
	guint16 degree;			/* The advertised degree of the destination node */
	guint8 ttl;				/* The TTL used for that query */
};

/*
 * Structure produced by dq_fill_next_up, representing the nodes to which
 * we could send the query, along with routing information to be able to favor
 * UPs that report a QRP match early in the querying process.
 */
struct next_up {
	gnutella_node_t *node;	/* Selected node */
	query_hashvec_t *qhv;	/* Query hash vector for the query */
	gint can_route;			/* -1 = unknown, otherwise TRUE / FALSE */
	gint queue_pending;		/* -1 = unknown, otherwise cached queue size */
};

/*
 * This table stores the pre-compution:
 *
 *  hosts(degree,ttl) = Sum[(degree-1)^i, 0 <= i <= ttl-1]
 *
 * For degree = 1 to 40 and ttl = 1 to 5.
 */

#define MAX_DEGREE		50
#define MAX_TTL			5

static guint32 hosts[MAX_DEGREE][MAX_TTL];	/* Pre-computed horizon */

static guint32 dyn_query_id = 0;

static void dq_send_next(dquery_t *dq);
static void dq_terminate(dquery_t *dq);

extern cqueue_t *callout_queue;

/**
 * Compute the hosts[] table so that:
 *
 *  hosts[i][j] = Sum[i^k, 0 <= k <= j]
 *
 * following the formula:
 *
 *  hosts(degree,ttl) = Sum[(degree-1)^i, 0 <= i <= ttl-1]
 */
static void
fill_hosts(void)
{
	gint i;
	gint j;

	for (i = 0; i < MAX_DEGREE; i++) {
		hosts[i][0] = 1;
		for (j = 1; j < MAX_TTL; j++) {
			hosts[i][j] = hosts[i][j-1] + pow(i, j);

			if (dq_debug > 19)
				printf("horizon(degree=%d, ttl=%d) = %d\n",
					i+1, j+1, hosts[i][j]);
		}
	}
}

/**
 * Computes theoretical horizon reached by a query sent to a host advertising
 * a given degree if it is going to travel ttl hops.
 *
 * We adjust the horizon by DQ_FUZZY_FACTOR, assuming that at each hop there
 * is deperdition due to flow-control, network cycles, etc...
 */
static guint32
dq_get_horizon(gint degree, gint ttl)
{
	gint i;
	gint j;

	g_assert(degree > 0);
	g_assert(ttl > 0);

	i = MIN(degree, MAX_DEGREE) - 1;
	j = MIN(ttl, MAX_TTL) - 1;

	return hosts[i][j] * pow(DQ_FUZZY_FACTOR, j);
}

/**
 * Select the proper TTL for the next query we're going to send to the
 * specified node, assuming hosts are equally split among the remaining
 * connections we have yet to query.
 */
static gint
dq_select_ttl(dquery_t *dq, gnutella_node_t *node, gint connections)
{
	guint32 needed = dq->max_results - dq->results;
	gdouble results_per_up;
	gdouble hosts_to_reach;
	gdouble hosts_to_reach_via_node;
	gint ttl;

	g_assert(connections > 0);
	g_assert(needed > 0);		/* Or query would have been stopped */

	results_per_up = dq->results / MAX(dq->horizon, 1);
	hosts_to_reach = (gdouble) needed / MAX(results_per_up, (gdouble) 0.000001);
	hosts_to_reach_via_node = hosts_to_reach / (gdouble) connections;

	/*
	 * Now iteratively find the TTL needed to reach the desired number
	 * of hosts, rounded to the lowest TTL to be conservative.
	 */

	for (ttl = MIN(node->max_ttl, dq->ttl); ttl > 0; ttl--) {
		if (dq_get_horizon(node->degree, ttl) <= hosts_to_reach_via_node)
			break;
	}

	if (ttl == 0)
		ttl = MIN(node->max_ttl, dq->ttl);

	g_assert(ttl > 0);

	return ttl;
}

/**
 * Create a pmsg_info structure, giving meta-information about the message
 * we're about to send.
 *
 * @param degree  the degree of the node to which the message is sent
 * @param ttl     the TTL at which the message is sent
 * @param node_id the ID of the node to which we send the message
 */
static struct pmsg_info *
dq_pmi_alloc(dquery_t *dq, guint16 degree, guint8 ttl, guint32 node_id)
{
	struct pmsg_info *pmi;

	pmi = walloc(sizeof(*pmi));

	pmi->dq = dq;
	pmi->qid = dq->qid;
	pmi->degree = degree;
	pmi->ttl = ttl;
	pmi->node_id = node_id;

	return pmi;
}

/**
 * Get rid of the pmsg_info structure.
 */
static void
dq_pmi_free(struct pmsg_info *pmi)
{
	wfree(pmi, sizeof(*pmi));
}

/**
 * Check whether query bearing the specified ID is still alive and has
 * not been cancelled yet.
 */
static gboolean
dq_alive(dquery_t *dq, guint32 qid)
{
	if (!g_hash_table_lookup(dqueries, dq))
		return FALSE;

	return dq->qid == qid;		/* In case it reused the same address */
}

/**
 * Free routine for an extended message block.
 */
static void
dq_pmsg_free(pmsg_t *mb, gpointer arg)
{
	struct pmsg_info *pmi = (struct pmsg_info *) arg;
	dquery_t *dq = pmi->dq;

	g_assert(pmsg_is_extended(mb));

	/*
	 * It is possible that whilst the message was in the message queue,
	 * the dynamic query was cancelled.  Therefore, we need to ensure that
	 * the recorded query is still alive.  We use both the combination of
	 * a hash table and a unique ID in case the address of an old dquery_t
	 * object is reused later.
	 */

	if (!dq_alive(dq, pmi->qid))
		goto cleanup;

	g_assert(dq->pending > 0);

	dq->pending--;

	if (!pmsg_was_sent(mb)) {
		gpointer key;
		gpointer value;
		gboolean found;

		/*
		 * The message was not sent: we need to remove the entry for the
		 * node in the "dq->queried" structure, since the message did not
		 * make it through the network.
		 */

		found = g_hash_table_lookup_extended(dq->queried, &pmi->node_id,
			&key, &value);

		g_assert(found);		/* Or something is seriously corrupted */

		g_hash_table_remove(dq->queried, &pmi->node_id);
		atom_int_free(key);

		if (dq_debug > 19)
			printf("DQ[%d] node #%d degree=%d dropped message TTL=%d\n",
				dq->qid, pmi->node_id, pmi->degree, pmi->ttl);

		/*
		 * If we don't have any more pending message and we're waiting
		 * for results, chances are we're going to wait for nothing!
		 *
		 * We can't re-enter mq from here, so reschedule the event for
		 * immediate delivery (in 1 ms, since we can't say 0).
		 */

		if (0 == dq->pending && dq->results_ev)
			cq_resched(callout_queue, dq->results_ev, 1);
		
	} else {
		/*
		 * The message was sent.  Adjust the total horizon reached thus far.
		 * Record that this UP got the query.
		 */

		dq->horizon += dq_get_horizon(pmi->degree, pmi->ttl);
		dq->up_sent++;

		if (dq_debug > 19) {
			printf("DQ[%d] node #%d degree=%d sent message TTL=%d\n",
				dq->qid, pmi->node_id, pmi->degree, pmi->ttl);
			printf("DQ[%d] (%d secs) queried %d UP%s, horizon=%d, results=%d\n", 
				dq->qid, (gint) (time(NULL) - dq->start),
				dq->up_sent, dq->up_sent == 1 ? "" :"s",
				dq->horizon, dq->results);
		}
	}

cleanup:
	dq_pmi_free(pmi);
}

/**
 * Fetch message for a given TTL.
 * If no such message exists yet, create it from the "template" message.
 */
static pmsg_t *
dq_pmsg_by_ttl(dquery_t *dq, gint ttl)
{
	pmsg_t *mb;
	pmsg_t *t;
	pdata_t *db;
	gint len;

	g_assert(ttl > 0 && ttl <= DQ_MAX_TTL);

	mb = dq->by_ttl[ttl - 1];
	if (mb != NULL)
		return mb;

	/*
	 * Copy does not exist for this TTL.
	 *
	 * First, create the data buffer, and copy the data from the
	 * template to this new buffer.  We assume the original message
	 * is made of one data buffer only (no data block chaining yet).
	 */

	t = dq->mb;					/* Our "template" */
	len = pmsg_size(t);
	db = pdata_new(len);
	memcpy(pdata_start(db), pmsg_start(t), len);

	/*
	 * Patch the TTL in the new data buffer.
	 */

	((struct gnutella_header *) pdata_start(db))->ttl = ttl;

	/*
	 * Now create a message for this data buffer and save it for later perusal.
	 */

	mb = pmsg_alloc(pmsg_prio(t), db, 0, len);
	dq->by_ttl[ttl - 1] = mb;

	return mb;
}

/**
 * Fill node vector with UP hosts to which we could send our probe query.
 *
 * @param nv the pre-allocated node vector
 * @param ncount the size of the vector
 *
 * @return amount of nodes we found.
 */
static gint
dq_fill_probe_up(dquery_t *dq, gnutella_node_t **nv, gint ncount)
{
	const GSList *sl;
	gint i = 0;

	for (sl = node_all_nodes(); i < ncount && sl; sl = g_slist_next(sl)) {
		struct gnutella_node *n = (struct gnutella_node *) sl->data;

		if (!NODE_IS_ULTRA(n))
			continue;

		if (!qrp_node_can_route(n, dq->qhv))
			continue;

		g_assert(NODE_IS_WRITABLE(n));	/* Checked by qrp_node_can_route() */

		nv[i++] = n;		/* Node or one of its leaves could answer */
	}

	return i;
}

/**
 * Fill node vector with UP hosts to which we could send our next query.
 *
 * @param nv the pre-allocated node vector
 * @param ncount the size of the vector
 *
 * @return amount of nodes we found.
 */
static gint
dq_fill_next_up(dquery_t *dq, struct next_up *nv, gint ncount)
{
	const GSList *sl;
	gint i = 0;

	for (sl = node_all_nodes(); i < ncount && sl; sl = g_slist_next(sl)) {
		struct gnutella_node *n = (struct gnutella_node *) sl->data;
		struct next_up *nup;

		if (!NODE_IS_ULTRA(n) || !NODE_IS_WRITABLE(n))
			continue;

		if (g_hash_table_lookup(dq->queried, &n->id))
			continue;

		nup = &nv[i++];

		nup->node = n;
		nup->qhv = dq->qhv;
		nup->can_route = -1;		/* We don't know yet */
	}

	return i;
}

/**
 * Forward message to all the leaves but the one originating this query,
 * according to their QRP tables.
 *
 * NB: In order to avoid qrt_build_query_target() selecting neighbouring
 * ultra nodes that support last-hop QRP, we ensure the TTL is NOT 1.
 * This is why we somehow duplicate qrt_route_query() here.
 */
static void
dq_sendto_leaves(dquery_t *dq, gnutella_node_t *source)
{
	GSList *nodes;
	gchar *payload;
	struct gnutella_header *head;

	payload = pmsg_start(dq->mb);
	head = (struct gnutella_header *) payload;

	nodes = qrt_build_query_target(dq->qhv,
		head->hops, MAX(head->ttl, 2), source);

	if (dbg > 4)
		g_message("DQ QRP %s (%d word/hash) forwarded to %d/%d leaves",
			gmsg_infostr(head), dq->qhv->count, g_slist_length(nodes),
			node_leaf_count);

	gmsg_mb_sendto_all(nodes, dq->mb);

	g_slist_free(nodes);
}

/**
 * Hashtable iteration callback to free the atoms held in the keys.
 */
static void 
free_node_id(gpointer key, gpointer value, gpointer udata)
{
	atom_int_free(key);
}

/**
 * Release the dynamic query object.
 */
static void
dq_free(dquery_t *dq)
{
	gint i;
	gboolean found;
	gpointer key;
	gpointer value;
	struct gnutella_header *head;

	g_assert(dq != NULL);

	if (dq_debug > 19)
		printf("DQ[%d] (%d secs; +%d secs) node #%d ending: "
			"ttl=%d, queried=%d, horizon=%d, results=%d+%d\n",
			dq->qid, (gint) (time(NULL) - dq->start),
			(dq->flags & DQ_F_LINGER) ? (gint) (time(NULL) - dq->stop) : 0,
			dq->node_id, dq->ttl, dq->up_sent, dq->horizon, dq->results,
			dq->linger_results);

	if (dq->results_ev)
		cq_cancel(callout_queue, dq->results_ev);

	if (dq->expire_ev)
		cq_cancel(callout_queue, dq->expire_ev);

	if (dq->results >= dq->max_results)
		gnet_stats_count_general(NULL, GNR_DYN_QUERIES_COMPLETED_FULL, 1);
	else if (dq->results > 0)
		gnet_stats_count_general(NULL, GNR_DYN_QUERIES_COMPLETED_PARTIAL, 1);
	else
		gnet_stats_count_general(NULL, GNR_DYN_QUERIES_COMPLETED_ZERO, 1);

	if (dq->linger_results) {
		if (dq->results >= dq->max_results)
			gnet_stats_count_general(NULL, GNR_DYN_QUERIES_LINGER_EXTRA, 1);
		else if (dq->results + dq->linger_results >= dq->max_results)
			gnet_stats_count_general(NULL, GNR_DYN_QUERIES_LINGER_COMPLETED, 1);
		else
			gnet_stats_count_general(NULL, GNR_DYN_QUERIES_LINGER_RESULTS, 1);
	}

	g_hash_table_foreach(dq->queried, free_node_id, NULL);
	g_hash_table_destroy(dq->queried);

	qhvec_free(dq->qhv);

	for (i = 0; i < DQ_MAX_TTL; i++) {
		if (dq->by_ttl[i] != NULL)
			pmsg_free(dq->by_ttl[i]);
	}

	g_hash_table_remove(dqueries, dq);

	/*
	 * Remove query from the `by_node_id' table but only if the node ID
	 * is not the local node, since we don't store our own queries in
	 * there: if we disappear, everything else will!
	 *
	 * Also, if the DQ_F_ID_CLEANING flag is set, then someone is already
	 * cleaning up the `by_node_id' table for us, so we really must not
	 * mess with the table ourselves.
	 */

	if (
		dq->node_id != NODE_ID_LOCAL &&
		!(dq->flags & DQ_F_ID_CLEANING)
	) {
		GSList *list;

		found = g_hash_table_lookup_extended(by_node_id, &dq->node_id,
			&key, &value);

		g_assert(found);

		list = value;
		list = g_slist_remove(list, dq);

		if (list == NULL) {
			/* Last item removed, get rid of the entry */
			g_hash_table_remove(by_node_id, key);
			atom_int_free(key);
		} else if (list != value)
			g_hash_table_insert(by_node_id, key, list);
	}

	/*
	 * Remove query's MUID.
	 */

	head = (struct gnutella_header *) pmsg_start(dq->mb);
	found = g_hash_table_lookup_extended(by_muid, head->muid, &key, &value);

	if (found) {			/* Could be missing if a MUID conflict occurred */
		if (value == dq) {	/* Make sure it's for us in case of conflicts */
			g_hash_table_remove(by_muid, key);
			atom_guid_free(key);
		}
	}

	pmsg_free(dq->mb);			/* Now that we used the MUID */

	wfree(dq, sizeof(*dq));
}

/**
 * Callout queue callback invoked when the dynamic query has expired.
 */
static void
dq_expired(cqueue_t *cq, gpointer obj)
{
	dquery_t *dq = (dquery_t *) obj;

	if (dq_debug > 19)
		printf("DQ[%d] expired\n", dq->qid);

	dq->expire_ev = NULL;	/* Indicates callback fired */

	/*
	 * If query was lingering, free it.
	 */

	if (dq->flags & DQ_F_LINGER) {
		dq_free(dq);
		return;
	}

	/*
	 * Put query in lingering mode, to be able to monitor extra results
	 * that come back after we stopped querying.
	 */

	if (dq->results_ev) {
		cq_cancel(callout_queue, dq->results_ev);
		dq->results_ev = NULL;
	}

	dq_terminate(dq);
}

/**
 * Callout queue callback invoked when the result timer has expired.
 */
static void
dq_results_expired(cqueue_t *cq, gpointer obj)
{
	dquery_t *dq = (dquery_t *) obj;

	dq->results_ev = NULL;	/* Indicates callback fired */

	dq_send_next(dq);
}

/**
 * Terminate active querying.
 */
static void
dq_terminate(dquery_t *dq)
{
	g_assert(!(dq->flags & DQ_F_LINGER));
	g_assert(dq->results_ev == NULL);

	/*
	 * Put the query in lingering mode, so we can continue to monitor
	 * results for some time after we stopped the dynamic querying.
	 */

	if (dq->expire_ev != NULL)
		cq_resched(callout_queue, dq->expire_ev, DQ_LINGER_TIMEOUT);
	else
		dq->expire_ev = cq_insert(callout_queue, DQ_LINGER_TIMEOUT,
			dq_expired, dq);

	dq->flags |= DQ_F_LINGER;
	dq->stop = time(NULL);

	if (dq_debug > 19)
		printf("DQ[%d] (%d secs) node #%d lingering: "
			"ttl=%d, queried=%d, horizon=%d, results=%d\n",
			dq->qid, (gint) (time(NULL) - dq->start), dq->node_id,
			dq->ttl, dq->up_sent, dq->horizon, dq->results);
}

/**
 * qsort() callback for sorting nodes by increasing queue size.
 */
static gint
node_mq_cmp(const void *np1, const void *np2)
{
	gnutella_node_t *n1 = *(gnutella_node_t **) np1;
	gnutella_node_t *n2 = *(gnutella_node_t **) np2;
	gint qs1 = NODE_MQUEUE_PENDING(n1);
	gint qs2 = NODE_MQUEUE_PENDING(n2);

	/*
	 * We don't cache the results of NODE_MQUEUE_PENDING() like we do in
	 * node_mq_qrp_cmp() because this is done ONCE per each dynamic query,
	 * (for the probe query only, and on an array containing only UP with
	 * a matching QRP) whereas the other comparison routine is called for
	 * each subsequent UP selection...
	 */

	if (qs1 == qs2)
		return 0;

	return qs1 < qs2 ? -1 : +1;
}

/**
 * qsort() callback for sorting nodes by increasing queue size, with a
 * preference towards nodes that have a QRP match.
 */
static gint
node_mq_qrp_cmp(const void *np1, const void *np2)
{
	struct next_up *nu1 = (struct next_up *) np1;
	struct next_up *nu2 = (struct next_up *) np2;
	gnutella_node_t *n1 = nu1->node;
	gnutella_node_t *n2 = nu2->node;
	gint qs1 = nu1->queue_pending;
	gint qs2 = nu2->queue_pending;

	/*
	 * Cache the results of NODE_MQUEUE_PENDING() since it involves
	 * several function calls to go down to the link layer buffers.
	 */

	if (qs1 == -1)
		qs1 = nu1->queue_pending = NODE_MQUEUE_PENDING(n1);
	if (qs2 == -1)
		qs2 = nu2->queue_pending = NODE_MQUEUE_PENDING(n2);

	/*
	 * If queue sizes are rather identical, compare based on whether
	 * the node can route or not (i.e. whether it advertises a "match"
	 * in its QRP table).
	 *
	 * Since this determination is a rather costly operation, cache it.
	 */

	if (ABS(qs1 - qs2) < DQ_MQ_EPSILON) {
		if (nu1->can_route == -1)
			nu1->can_route = qrp_node_can_route(n1, nu1->qhv);
		if (nu2->can_route == -1)
			nu2->can_route = qrp_node_can_route(n2, nu2->qhv);

		if (!nu1->can_route == !nu2->can_route) {
			/* Both can equally route or not route */
			return qs1 == qs2 ? 0 :
				qs1 < qs2 ? -1 : +1;
		}

		return nu1->can_route ? -1 : +1;
	}

	return qs1 < qs2 ? -1 : +1;
}

/**
 * Send individual query to selected node at the supplied TTL.
 * If the node advertises a lower maximum TTL, the supplied TTL is
 * adjusted down accordingly.
 */
static void
dq_send_query(dquery_t *dq, gnutella_node_t *n, gint ttl)
{
	gint *id_atom;
	struct pmsg_info *pmi;
	pmsg_t *mb;

	g_assert(!g_hash_table_lookup(dq->queried, &n->id));
	g_assert(NODE_IS_WRITABLE(n));

	id_atom = atom_int_get(&n->id);
	g_hash_table_insert(dq->queried, id_atom, GINT_TO_POINTER(1));

	pmi = dq_pmi_alloc(dq, n->degree, MIN(n->max_ttl, ttl), n->id);

	/*
	 * Now for the magic...
	 *
	 * We're going to clone the messsage template into an extended one,
	 * which will be associated with a free routine.  That way, we'll know
	 * when the message is freed, and we'll get back the meta data (pmsg_info)
	 * as an argument to the free routine.
	 *
	 * Then, in the cloned message, adjust the TTL before sending.
	 */

	mb = dq_pmsg_by_ttl(dq, pmi->ttl);
	mb = pmsg_clone_extend(mb, dq_pmsg_free, pmi);

	if (dq_debug > 19)
		printf("DQ[%d] (%d secs) queuing ttl=%d to #%d %s <%s> Q=%d bytes\n",
			dq->qid, (gint) (time(NULL) - dq->start),
			pmi->ttl, n->id, node_ip(n), node_vendor(n),
			NODE_MQUEUE_PENDING(n));

	dq->pending++;
	gmsg_mb_sendto_one(n, mb);
}

/**
 * Iterate over the UPs which have not seen our query yet, select one and
 * send it the query.
 *
 * If no more UP remain, terminate this query.
 */
static void
dq_send_next(dquery_t *dq)
{
	struct next_up *nv;
	gint ncount = max_connections;
	gint found;
	gnutella_node_t *node;
	gint ttl;
	gint timeout;

	g_assert(dq->results_ev == NULL);

	/*
	 * Terminate query immediately if we're no longer an UP.
	 */

	if (current_peermode != NODE_P_ULTRA) {
		dq_terminate(dq);
		return;
	}

	/*
	 * Terminate query if we reached the amount of results we wanted or
	 * if we reached the maximum theoretical horizon.
	 */

	if (dq->results >= dq->max_results || dq->horizon >= DQ_MAX_HORIZON) {
		dq_terminate(dq);
		return;
	}

	/*
	 * If we already queried as many UPs as the maximum we configured,
	 * stop the query.
	 */

	if (dq->up_sent >= max_connections - normal_connections) {
		dq_terminate(dq);
		return;
	}

	nv = walloc(ncount * sizeof(struct next_up));
	found = dq_fill_next_up(dq, nv, ncount);

	if (dq_debug > 19)
		printf("DQ[%d] still %d UP%s to query\n",
			dq->qid, found, found == 1 ? "" : "s");

	if (found == 0) {
		dq_terminate(dq);	/* Terminate query: no more UP to send it to */
		goto cleanup;
	}

	/*
	 * Sort the array by increasing queue size, so that the nodes with
	 * the less pending data are listed first, with a preference to nodes
	 * with a QRP match.
	 */

	qsort(nv, found, sizeof(struct next_up), node_mq_qrp_cmp);

	/*
	 * Select the first node, and compute the proper TTL for the query.
	 */

	node = nv[0].node;
	ttl = dq_select_ttl(dq, node, found);

	dq_send_query(dq, node, ttl);

	/*
	 * Adjust waiting period if we don't get enough results, indicating
	 * that the query might be for rare content.
	 */

	if (
		dq->horizon > DQ_MIN_HORIZON &&
		dq->results < (DQ_MAX_RESULTS * dq->horizon / DQ_MIN_HORIZON)
	) {
		dq->result_timeout -= DQ_TIMEOUT_ADJUST;
		dq->result_timeout = MAX(DQ_MIN_TIMEOUT, dq->result_timeout);
	}

	/*
	 * Install a watchdog for the query, to go on if we don't get
	 * all the results we want by then.
	 */

	timeout = dq->result_timeout;
	if (dq->pending > 1)
		timeout += (dq->pending - 1) * DQ_PENDING_TIMEOUT;

	if (dq_debug > 1)
		printf("DQ[%d] (%d secs) timeout set to %d ms (pending=%d)\n",
			dq->qid, (gint) (time(NULL) - dq->start), timeout, dq->pending);

	dq->results_ev = cq_insert(callout_queue, timeout, dq_results_expired, dq);

cleanup:
	wfree(nv, ncount * sizeof(struct next_up));
}

/**
 * Send probe query (initial querying).
 *
 * This can generate up to DQ_PROBE_UP individual queries.
 */
static void
dq_send_probe(dquery_t *dq)
{
	gnutella_node_t **nv;
	gint ncount = max_connections;
	gint found;
	gint ttl = dq->ttl;
	gint i;

	g_assert(dq->results_ev == NULL);

	nv = walloc(ncount * sizeof(gnutella_node_t *));
	found = dq_fill_probe_up(dq, nv, ncount);

	if (dq_debug > 19)
		printf("DQ[%d] found %d UP%s to probe\n",
			dq->qid, found, found == 1 ? "" : "s");

	/*
	 * If we don't find any suitable UP holding that content, then
	 * the query might be for something that is rare enough.  Start
	 * the sequential probing.
	 */

	if (found == 0) {
		dq_send_next(dq);
		goto cleanup;
	}

	/*
	 * If we have 3 times the amount of UPs necessary for the probe,
	 * then content must be common, so reduce TTL by 1.  If we have 6 times
	 * the default amount, further reduce by 1.
	 */

	if (found > 6 * DQ_PROBE_UP)
		ttl--;
	if (found > 3 * DQ_PROBE_UP)
		ttl--;

	ttl = MAX(ttl, 1);

	/*
	 * Sort the array by increasing queue size, so that the nodes with
	 * the less pending data are listed first.
	 */

	qsort(nv, found, sizeof(gnutella_node_t *), node_mq_cmp);

	/*
	 * Send the probe query to the first DQ_PROBE_UP nodes.
	 */

	for (i = 0; i < DQ_PROBE_UP && i < found; i++)
		dq_send_query(dq, nv[i], ttl);

	/*
	 * Install a watchdog for the query, to go on if we don't get
	 * all the results we want by then.  We wait the specified amount
	 * of time per connection plus an extra DQ_PROBE_TIMEOUT because
	 * this is the first queries we send and their results will help us
	 * assse how popular the query is.
	 */

	dq->results_ev = cq_insert(callout_queue,
		MIN(found, DQ_PROBE_UP) * (DQ_PROBE_TIMEOUT + dq->result_timeout),
		dq_results_expired, dq);

cleanup:
	wfree(nv, ncount * sizeof(gnutella_node_t *));
}

/**
 * Common initialization code for a dynamic query.
 */
static void
dq_common_init(dquery_t *dq, query_hashvec_t *qhv)
{
	struct gnutella_header *head;

	dq->qid = dyn_query_id++;
	dq->qhv = qhvec_clone(qhv);
	dq->queried = g_hash_table_new(g_int_hash, g_int_equal);
	dq->result_timeout = DQ_QUERY_TIMEOUT;
	dq->start = time(NULL);

	/*
	 * Make sure the dynamic query structure is cleaned up in at most
	 * DQ_MAX_LIFETIME ms, whatever happens.
	 */

	dq->expire_ev = cq_insert(callout_queue, DQ_MAX_LIFETIME,
		dq_expired, dq);

	/*
	 * Record the query as being "alive".
	 */

	g_hash_table_insert(dqueries, dq, GINT_TO_POINTER(1));

	/*
	 * If query is not for the local node, insert it in `by_node_id'.
	 */

	if (dq->node_id != NODE_ID_LOCAL) {
		gboolean found;
		gpointer key;
		gpointer value;
		GSList *list;

		found = g_hash_table_lookup_extended(by_node_id, &dq->node_id,
			&key, &value);

		if (found) {
			list = value;
			list = gm_slist_insert_after(list, list, dq);
			g_assert(list == value);		/* Head not changed */
		} else {
			list = g_slist_prepend(NULL, dq);
			key = atom_int_get(&dq->node_id);
			g_hash_table_insert(by_node_id, key, list);
		}
	}

	/*
	 * Record the MUID of this query, warning if a conflict occurs.
	 */

	head = (struct gnutella_header *) pmsg_start(dq->mb);

	if (g_hash_table_lookup(by_muid, head->muid))
		g_warning("conflicting MUID \"%s\" for dynamic query, ignoring.",
			guid_hex_str(head->muid));
	else {
		gchar *muid = atom_guid_get(head->muid);
		g_hash_table_insert(by_muid, muid, dq);
	}

	if (dq_debug > 19)
		printf("DQ[%d] created for node #%d: TTL=%d max_results=%d "
			"MUID=%s q=\"%s\"\n",
			dq->qid, dq->node_id, dq->ttl, dq->max_results,
			guid_hex_str(head->muid), QUERY_TEXT(pmsg_start(dq->mb)));
}

/**
 * Start new dynamic query out of a message we got from one of our leaves.
 */
void
dq_launch_net(gnutella_node_t *n, query_hashvec_t *qhv)
{
	dquery_t *dq;

	g_assert(NODE_IS_LEAF(n));

	dq = walloc0(sizeof(*dq));

	dq->node_id = n->id;
	dq->mb = gmsg_split_to_pmsg(
		(guchar *) &n->header, n->data,
		n->size + sizeof(struct gnutella_header));
	dq->max_results = DQ_LEAF_RESULTS;
	dq->ttl = MIN(n->header.ttl, DQ_MAX_TTL);

	if (dq_debug > 19)
		printf("DQ node #%d %s <%s> queries \"%s\"\n",
			n->id, node_ip(n), node_vendor(n), QUERY_TEXT(pmsg_start(dq->mb)));

	gnet_stats_count_general(NULL, GNR_LEAF_DYN_QUERIES, 1);

	dq_common_init(dq, qhv);
	dq_sendto_leaves(dq, n);
	dq_send_probe(dq);
}

/**
 * Tells us a node ID has been removed.
 * Get rid of all the queries registered for that node.
 */
void
dq_node_removed(guint32 node_id)
{
	gboolean found;
	gpointer key;
	gpointer value;
	GSList *sl;

	found = g_hash_table_lookup_extended(by_node_id, &node_id, &key, &value);

	if (!found)
		return;		/* No dynamic query for this node */

	for (sl = value; sl; sl = g_slist_next(sl)) {
		dquery_t *dq = (dquery_t *) sl->data;

		if (dq_debug > 19)
			printf("DQ[%d] terminated by node #%d removal\n",
				dq->qid, dq->node_id);

		/* Don't remove query from the table in dq_free() */
		dq->flags |= DQ_F_ID_CLEANING;
		dq_free(dq);
	}

	g_hash_table_remove(by_node_id, key);
	g_slist_free(value);
	atom_int_free(key);
}

/**
 * Called every time we successfully parsed a query hit from the network.
 * If we have a dynamic query registered for the MUID, increase the result
 * count.
 */
void
dq_got_results(gchar *muid, gint count)
{
	dquery_t *dq;

	g_assert(count >= 0);

	if (count == 0)
		return;

	dq = g_hash_table_lookup(by_muid, muid);

	if (dq == NULL)
		return;

	if (dq->flags & DQ_F_LINGER)
		dq->linger_results += count;
	else
		dq->results += count;

	if (dq_debug > 19) {
		if (dq->flags & DQ_F_LINGER)
			printf("DQ[%d] (%d secs; +%d secs) +%d linger_results=%d\n",
				dq->qid, (gint) (time(NULL) - dq->start),
				(gint) (time(NULL) - dq->stop),
				count, dq->linger_results);
		else
			printf("DQ[%d] (%d secs) +%d results=%d\n",
				dq->qid, (gint) (time(NULL) - dq->start),
				count, dq->results);
	}
}

/**
 * Initialize dynamic querying.
 */
void
dq_init(void)
{
	extern guint guid_hash(gconstpointer key);
	extern gint guid_eq(gconstpointer a, gconstpointer b);

	dqueries = g_hash_table_new(g_direct_hash, 0);
	by_node_id = g_hash_table_new(g_int_hash, g_int_equal);
	by_muid = g_hash_table_new(guid_hash, guid_eq);
	fill_hosts();
}

/**
 * Hashtable iteration callback to free the dquery_t object held as the key.
 */
static void
free_query(gpointer key, gpointer value, gpointer udata)
{
	dq_free((dquery_t *) key);
}

/**
 * Hashtable iteration callback to free the items remaining in the
 * by_node_id table.  Normally, after having freed the dqueries table,
 * there should not be anything remaining, hence warn!
 */
static void
free_query_list(gpointer key, gpointer value, gpointer udata)
{
	gint *atom = (gint *) key;
	GSList *list = (GSList *) value;
	gint count = g_slist_length(list);
	GSList *sl;

	g_warning("remained %d un-freed dynamic quer%s for node #%d",
		count, count == 1 ? "y" : "ies", *atom);

	for (sl = list; sl; sl = g_slist_next(sl)) {
		dquery_t *dq = (dquery_t *) sl->data;

		/* Don't remove query from the table we're traversing in dq_free() */
		dq->flags |= DQ_F_ID_CLEANING;
		dq_free(dq);
	}

	g_slist_free(list);
	atom_int_free(atom);
}

/**
 * Hashtable iteration callback to free the MUIDs in the `by_muid' table.
 * Normally, after having freed the dqueries table, there should not be
 * anything remaining, hence warn!
 */
static void
free_muid(gpointer key, gpointer value, gpointer udata)
{
	g_warning("remained un-freed MUID \"%s\" in dynamic queries",
		guid_hex_str(key));

	atom_guid_free(key);
}

/**
 * Cleanup data structures used by dynamic querying.
 */
void
dq_close(void)
{
	g_hash_table_foreach(dqueries, free_query, NULL);
	g_hash_table_destroy(dqueries);

	g_hash_table_foreach(by_node_id, free_query_list, NULL);
	g_hash_table_destroy(by_node_id);

	g_hash_table_foreach(by_muid, free_muid, NULL);
	g_hash_table_destroy(by_muid);
}

