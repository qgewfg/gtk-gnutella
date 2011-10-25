/*
 * Copyright (c) 2002-2003, Raphael Manfredi
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
 * @ingroup core
 * @file
 *
 * Version management.
 *
 * @author Raphael Manfredi
 * @date 2002-2003
 */

#ifndef _core_version_h_
#define _core_version_h_

#include "common.h"
#include "if/core/version.h"
#include "lib/host_addr.h"

/**
 * A decompiled version descriptor.
 * In our comments below, we are assuming a value of "0.90.3b2".
 */
typedef struct version {
	guint32 major;				/**< Major version number (0) */
	guint32 minor;				/**< Minor version number (90) */
	guint32 patchlevel;			/**< Patch level (3) */
	guchar tag;					/**< Code letter after version number (b) */
	guint32 taglevel;			/**< Value after code letter (2) */
	guint32 build;				/**< Build revision number (11723) */
	time_t timestamp;
} version_t;

/**
 * An extended decompiled version descriptor.
 */
typedef struct version_ext {
	version_t version;			/**< Basic version information */
	guint8 commit_len;			/**< Amount of valid nybbles in commit */
	sha1_t commit;				/**< Git's commit SHA1 (partial) */
	const char *osname;			/**< Static string */
	unsigned dirty:1;
} version_ext_t;

/*
 * Banning periods for our versions.
 */

#define VERSION_ANCIENT_WARN	(86400*365)		/**< 1 year */
#define VERSION_ANCIENT_BAN		(86400*365)		/**< 1 year */

#define VERSION_ANCIENT_REMIND	(86400*30)		/**< 30 days */
#define VERSION_ANCIENT_GRACE	(86400*20)		/**< 20 days */

/*
 * Public interface.
 */

void version_init(void);
void version_close(void);
void version_ancient_warn(void);
gboolean version_check(const char *str, const char *token, const host_addr_t);
int version_cmp(const version_t *a, const version_t *b);
int version_build_cmp(const version_t *a, const version_t *b);
gboolean version_fill(const char *version, version_t *vs);

const char *version_str(const version_t *ver);
const char *version_ext_str(const version_ext_t *vext, gboolean full);

extern const char *version_string;
extern const char *version_short_string;

const char *version_build_string(void);
guint8 version_get_code(void) G_GNUC_PURE;
const char *version_get_date(void) G_GNUC_CONST;
guint8 version_get_major(void) G_GNUC_CONST;
guint8 version_get_minor(void) G_GNUC_CONST;
guint8 version_get_revchar(void) G_GNUC_CONST;
guint8 version_get_patchlevel(void) G_GNUC_CONST;
gboolean version_is_dirty(void) G_GNUC_PURE;
const struct sha1 *version_get_commit(guint8 *len);

#endif	/* _core_version_h_ */

/* vi: set ts=4: */
