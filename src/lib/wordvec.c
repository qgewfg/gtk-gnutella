/*
 * Copyright (c) 2001-2003, Raphael Manfredi
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
 * @ingroup lib
 * @file
 *
 * Word vector.
 *
 * @author Raphael Manfredi
 * @date 2001-2003
 */

#include "common.h"

#include "wordvec.h"
#include "utf8.h"
#include "halloc.h"
#include "htable.h"
#include "misc.h"
#include "walloc.h"
#include "zalloc.h"
#include "override.h"		/* Must be the last header included */

#define WOVEC_DFLT	10		/**< Default size of word-vectors */

static zone_t *wovec_zone = NULL;	/**< Word-vectors of WOVEC_DFLT entries */

/**
 * Initialize matching data structures.
 */
void
word_vec_init(void)
{
	/*
	 * We don't expect much word vectors to be created.  They are normally
	 * created and destroyed in the same routine, without any threading
	 * taking place.
	 *
	 * We only allocate word vectors of WOVEC_DFLT entries in the zone.
	 * If we need to expand that, it will be done through regular malloc().
	 */

	wovec_zone = zget(WOVEC_DFLT * sizeof(word_vec_t), 2, TRUE);
}

/**
 * Terminate matching data structures.
 */
void
word_vec_close(void)
{
	zdestroy(wovec_zone);
}

/*
 * Search query word splitting.
 *
 * When facing a query like "this file.jpg", we want to be able to
 * split that down to ("this", "file", "jpg"), and look for each word
 * at a time.
 *
 * However, with a query like "the file is the one", then the word
 * "the" must match twice, exactly.  We must not only collect the words,
 * but also their wanted frequency.
 */

/**
 * Reallocate a word-vector from the zone into heap memory, to hold `ncount'.
 */
static word_vec_t *
word_vec_zrealloc(word_vec_t *wv, int ncount)
{
	word_vec_t *nwv = halloc(ncount * sizeof(word_vec_t));

	g_assert(ncount > WOVEC_DFLT);

	memcpy(nwv, wv, WOVEC_DFLT * sizeof(word_vec_t));
	zfree(wovec_zone, wv);

	return nwv;
}

/**
 * Given a query string, return a dynamically built word vector, along
 * with the amount of items held into that vector.
 * Words are broken on non-alphanumeric boundaries.
 *
 * @returns the amount of valid items in the built vector, and fill `wovec'
 * with the pointer to the allocated vector.  If there are no items, there
 * is no vector returned.
 */
uint
word_vec_make(const char *query_str, word_vec_t **wovec)
{
	uint n = 0;
	htable_t *seen_word = NULL;
	uint nv = WOVEC_DFLT;
	word_vec_t *wv = zalloc(wovec_zone);
	const char *start = NULL;
	char * const query_dup = h_strdup(query_str);
	char *query;
	char first = TRUE;
	uchar c;

	g_assert(wovec != NULL);

	for (query = query_dup; /* empty */; query++) {
		bool is_separator;

		c = *(uchar *) query;
		/*
	 	 * We can't meet other separators than space, because the
	 	 * string is normalised.
	 	 */
		is_separator = c == ' ' || c == '\0';

		if (start == NULL) {				/* Not in a word yet */
			if (!is_separator)
				start = query;
		} else {
			uint np1;

			if (!is_separator)
				continue;

			*query = '\0';

			/* Only create a hash table if there is more than one word. */
			if (first)
				np1 = 0;
			else {
				if G_UNLIKELY(NULL == seen_word) {
					seen_word = htable_create(HASH_KEY_STRING, 0);
					htable_insert(seen_word, wv[0].word, uint_to_pointer(1));
				}

				/*
			 	 * If word already seen in query, it's in the seen_word table.
		 	 	 * The associated value is the index in the vector plus 1.
		 	 	 */

				np1 = pointer_to_uint(htable_lookup(seen_word, start));
			}

			if (np1--) {
				wv[np1].amount++;
				wv[np1].len = query - start;
			} else {
				word_vec_t *entry;
				if (n == nv) {				/* Filled all the slots */
					nv *= 2;
					if (n > WOVEC_DFLT)
						wv = hrealloc(wv, nv * sizeof(word_vec_t));
					else
						wv = word_vec_zrealloc(wv, nv);
				}
				entry = &wv[n++];
				entry->len = query - start;
				entry->word = walloc(entry->len + 1);	/* For trailing NUL */
				memcpy(entry->word, start, entry->len + 1); /* Includes NUL */

				entry->amount = 1;

				/*
				 * Delay insertion of first word until we find another one.
				 * The hash table storing duplicates is not created for
				 * the first word.  The word entry is saved into `first_word'
				 * for later insertion, if needed.
				 */

				if (first)
					first = FALSE;
				else {
					htable_insert(seen_word, entry->word, uint_to_pointer(n));
				}
			}
			start = NULL;
		}

		if (c == '\0') break;
	}

	htable_free_null(&seen_word);	/* Key pointers belong to vector */
	if (n)
		*wovec = wv;
	else
		zfree(wovec_zone, wv);
	hfree(query_dup);
	return n;
}

/**
 * Release a word vector, containing `n' items.
 */
void
word_vec_free(word_vec_t *wovec, uint n)
{
	uint i;

	for (i = 0; i < n; i++)
		wfree(wovec[i].word, wovec[i].len + 1);

	if (n > WOVEC_DFLT)
		HFREE_NULL(wovec);
	else
		zfree(wovec_zone, wovec);
}

/* vi: set ts=4 sw=4 cindent: */
