/*
 * Copyright (c) 2008, Raphael Manfredi
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
 * Entropy collection.
 *
 * @author Raphael Manfredi
 * @date 2008
 */

#ifndef _entropy_h_
#define _entropy_h_

struct sha1;

void entropy_collect(struct sha1 *digest);
void entropy_minimal_collect(struct sha1 *digest);
unsigned entropy_random(void);
void entropy_fill(void *buffer, size_t len);
void entropy_delay(void);

#endif /* _entropy_h_ */

/* vi: set ts=4 sw=4 cindent: */
