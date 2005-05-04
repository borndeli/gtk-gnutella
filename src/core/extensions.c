/*
 * $Id$
 *
 * Copyright (c) 2002-2003, Raphael Manfredi
 *
 * Gnutella message extension handling.
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

#include "common.h"

RCSID("$Id$");

#include "extensions.h"
#include "ggep.h"

#include "lib/atoms.h"
#include "lib/misc.h"
#include "lib/walloc.h"
#include "lib/override.h"		/* Must be the last header included */

#include "if/gnet_property_priv.h"

#define HUGE_FS		'\x1c'		/* Field separator (HUGE) */

#define GGEP_MAXLEN	65535		/* Maximum decompressed length */
#define GGEP_GROW	512			/* Minimum chunk growth when resizing */

/**
 * @ingroup core
 * @file
 *
 * An extension descriptor.
 *
 * The extension block is structured thustly:
 *
 *    <.................len.......................>
 *    <..headlen.><..........paylen...............>
 *    +-----------+-------------------------------+
 *    |   header  |      extension payload        |
 *    +-----------+-------------------------------+
 *    ^           ^
 *    base        payload
 *
 * The <headlen> part is simply <len> - <paylen> so it is not stored.
 * Likewise, we store only the beginning of the payload, the base can be
 * computed if needed.
 *
 * All those pointers refer DIRECTLY to the message we received, so naturally
 * one MUST NOT alter the data we can read or we would corrupt the messages
 * before forwarding them.
 *
 * There is a slight complication introduced with GGEP extensions, since the
 * data there can be COBS encoded, and even deflated.  Therefore, reading
 * directly data from ext_phys_payload could yield compressed data, not
 * something really usable.
 *
 * Therefore, the extension structure is mostly private, and routines are
 * provided to access the data.  Decompression and decoding of COBS is lazily
 * performed when they wish to access the extension data.
 *
 * The ext_phys_xxx fields refer to the physical information about the
 * extension.  The ext_xxx() routines allow access to the virtual information
 * after decompression and COBS decoding.  Naturally, if the extension is
 * not compressed nor COBS-encoded, the ext_xxx() routine will return the
 * physical data.
 *
 * The structure here refers to the opaque data that is dynamically allocated
 * each time a new extension is found.
 */
typedef struct extdesc {
	gchar *ext_phys_payload;	/* Start of payload buffer */
	gchar *ext_payload;			/* "virtual" payload */
	guint16 ext_phys_len;		/* Extension length (header + payload) */
	guint16 ext_phys_paylen;	/* Extension payload length */
	guint16 ext_paylen;			/* "virtual" payload length */
	guint16 ext_rpaylen;		/* Length of buffer for "virtual" payload */

	union {
		struct {
			gboolean extu_cobs;			/* Payload is COBS-encoded */
			gboolean extu_deflate;		/* Payload is deflated */
			const gchar *extu_id;		/* Extension ID */
		} extu_ggep;
	} ext_u;

} extdesc_t;

#define ext_phys_headlen(d)	((d)->ext_phys_len - (d)->ext_phys_paylen)
#define ext_phys_base(d)	((d)->ext_phys_payload - ext_phys_headlen(d))

/*
 * Union access shortcuts.
 */

#define ext_ggep_cobs		ext_u.extu_ggep.extu_cobs
#define ext_ggep_deflate	ext_u.extu_ggep.extu_deflate
#define ext_ggep_id			ext_u.extu_ggep.extu_id

static const gchar * const extype[] = {
	"UNKNOWN",					/* EXT_UNKNOWN */
	"XML",						/* EXT_XML */
	"HUGE",						/* EXT_HUGE */
	"GGEP",						/* EXT_GGEP */
	"NONE",						/* EXT_NONE */
};

/***
 *** Extension name screener.
 ***/

struct rwtable {			/* Reserved word description */
	const gchar *rw_name;	/* Representation */
	ext_token_t rw_token;	/* Token value */
};

static const struct rwtable urntable[] =	/* URN name table (sorted) */
{
	{ "bitprint",		EXT_T_URN_BITPRINT },
	{ "sha1",			EXT_T_URN_SHA1 },
};

static const struct rwtable ggeptable[] =	/* GGEP extension table (sorted) */
{
#define GGEP_ID(x) { STRINGIFY(x), CAT2(EXT_T_GGEP_,x) }

	{ "<", EXT_T_GGEP_LIME_XML },	/* '<' is less that 'A' */
	GGEP_ID(ALT),					/* Alt-locs in qhits */
	GGEP_ID(BH),					/* Browseable host indication */
	GGEP_ID(CT),					/* Resource creation time */
	GGEP_ID(DU),					/* Average servent uptime */
	GGEP_ID(GTKGV1),				/* GTKG complete version number (binary) */
	GGEP_ID(GUE),					/* GUESS support */
	GGEP_ID(H),						/* Hashes in binary form */
	GGEP_ID(HNAME),					/* Hostname */
	GGEP_ID(IP),					/* Ip:Port in ping and pongs (F2F) */
	GGEP_ID(IPP),					/* IP:Port in pongs (UHC) */
	GGEP_ID(LF),					/* Large file size in qhits */
	GGEP_ID(LOC),					/* Locale preferences, for clustering  */
	GGEP_ID(PATH),					/* Shared file path, in query hits */
	GGEP_ID(PHC),					/* Packed host caches (UHC) in pongs */
	GGEP_ID(PUSH),					/* Push proxy info, in qhits */
	GGEP_ID(SCP),					/* Supports cached pongs, in pings (UHC) */
	GGEP_ID(T),						/* Textual information in qhits */
	GGEP_ID(UDPHC),					/* Is an UDP hostcache (UHC) , in pongs */
	GGEP_ID(UP),					/* Ultrapeer information about free slots */
	GGEP_ID(VC),					/* Vendor code, in pongs */
	GGEP_ID(u),						/* HUGE URN in ASCII */

#undef GGEP_ID
};

/**
 * Perform a dichotomic search for keywords in the reserved-word table.
 * The `case_sensitive' parameter governs whether lookup is done with or
 * without paying attention to case.
 *
 * @return the keyword token value upon success, EXT_T_UNKNOWN if not found.
 * If keyword was found, its static shared string is returned in `retkw'.
 */
static ext_token_t
rw_screen(gboolean case_sensitive,
	const struct rwtable *table, size_t size,
	const gchar *word, const gchar **retkw)
{
	g_assert(retkw);

#define GET_KEY(i) (table[(i)].rw_name)
#define FOUND(i) \
	G_STMT_START { \
		*retkw = table[(i)].rw_name; \
	   	return table[(i)].rw_token; \
		/* NOTREACHED */ \
	} G_STMT_END

	if (case_sensitive)
		BINARY_SEARCH(const gchar *, word, size,
				strcmp, GET_KEY, FOUND);
	else
		BINARY_SEARCH(const gchar *, word, size,
				ascii_strcasecmp, GET_KEY, FOUND);
	
#undef FOUND
#undef GET_KEY
	
	*retkw = NULL;
	return EXT_T_UNKNOWN;
}

/**
 * Ensure the reserved-word table is lexically sorted.
 */
static void
rw_is_sorted(const gchar *name,
	const struct rwtable *table, size_t size)
{
	size_t i;

	/* Skip the first to have a previous element, tables with a single
	 * element are sorted anyway. */
	for (i = 1; i < size; i++) {
		const struct rwtable *prev = &table[i - 1], *e = &table[i];
		if (strcmp(prev->rw_name, e->rw_name) >= 0)
			g_error("reserved word table \"%s\" unsorted (near item \"%s\")",
				name, e->rw_name);
	}
}

/**
 * @return the GGEP token value upon success, EXT_T_UNKNOWN_GGEP if not found.
 * If keyword was found, its static shared string is returned in `retkw'.
 */
static ext_token_t
rw_ggep_screen(gchar *word, const gchar **retkw)
{
	ext_token_t t;

	t = rw_screen(TRUE, ggeptable, G_N_ELEMENTS(ggeptable), word, retkw);

	return (t == EXT_T_UNKNOWN) ? EXT_T_UNKNOWN_GGEP : t;
}

/**
 * @return the URN token value upon success, EXT_T_UNKNOWN if not found.
 * If keyword was found, its static shared string is returned in `retkw'.
 */
static ext_token_t
rw_urn_screen(gchar *word, const gchar **retkw)
{
	return rw_screen(FALSE, urntable, G_N_ELEMENTS(urntable), word, retkw);
}

/***
 *** Extension name atoms.
 ***/

static GHashTable *ext_names = NULL;

/**
 * Transform the name into a printable form, and return an atom string
 * of that printable form.
 */
static gchar *
ext_name_atom(const gchar *name)
{
	gchar *key;
	gchar *atom;

	/*
	 * Look whether we already known about this name.
	 */

	atom = g_hash_table_lookup(ext_names, name);

	if (atom != NULL)
		return atom;

	/*
	 * The key is always the raw name we're given.
	 *
	 * The value is always a printable form of the name, where non-printable
	 * chars are shown as hexadecimal escapes: \xhh.  However, if there is
	 * no escaping, then the name is also the key (same object).
	 */

	key = g_strdup(name);
	atom = hex_escape(key, TRUE); /* strict escaping */

	g_hash_table_insert(ext_names, key, atom);

	return atom;
}

/**
 * Callback for freeing entries in the `ext_names' hash table.
 */
static gboolean
ext_names_kv_free(gpointer key, gpointer value, gpointer unused_udata)
{
	(void) unused_udata;

	if (0 != strcmp((gchar *) key, (gchar *) value))
		G_FREE_NULL(value);

	G_FREE_NULL(key);

	return TRUE;
}

/***
 *** Extension parsing.
 ***
 *** All the ext_xxx_parse routines share the same signature and behaviour:
 ***
 *** They extract one extension, as guessed by the leading byte introducing
 *** those extensions and return the amount of entries they added to the
 *** supplied extension vector (this will be typically 1 but for GGEP which
 *** is structured and can therefore grab more than one extension in one call).
 ***
 *** Upon entry, `*retp' points to the start of the extension, and there are
 ** `len' bytes to parse.  There are `exvcnt' slots available in the extension
 *** vector, starting at `exv'.
 ***
 *** On exit, `p' is updated to the first byte following the last successfully
 *** parsed byte.  If the returned value is 0, then `p' is not updated.
 ***/

/*
 * ext_ggep_parse
 *
 * Parses a GGEP block (can hold several extensions).
 */
static gint
ext_ggep_parse(gchar **retp, gint len, extvec_t *exv, gint exvcnt)
{
	gchar *p = *retp;
	gchar *end = p + len;
	gchar *lastp = p;				/* Last parsed point */
	gint count;

	for (count = 0; count < exvcnt && p < end; /* empty */) {
		guchar flags;
		gchar id[GGEP_F_IDLEN + 1];
		guint id_len, data_length, i;
		gboolean length_ended = FALSE;
		const gchar *name;
		extdesc_t *d;

		g_assert(exv->opaque == NULL);

		/*
		 * First byte is GGEP flags.
		 */

		flags = (guchar) *p++;

		if (flags & GGEP_F_MBZ)		/* A byte that Must Be Zero is set */
			goto abort;

		id_len = flags & GGEP_F_IDLEN;
		g_assert(id_len < sizeof id);

		if (id_len == 0)
			goto abort;

		if ((size_t) (end - p) < id_len) /* Not enough bytes to store the ID! */
			goto abort;

		/*
		 * Read ID, and NUL-terminate it.
		 *
		 * As a safety precaution, only allow ASCII IDs, and nothing in
		 * the control space.  It's not really in the GGEP specs, but it's
		 * safer that way, and should protect us if we parse garbage starting
		 * with 0xC3....
		 *		--RAM, 2004-11-12
		 */

		for (i = 0; i < id_len; i++) {
			gint c = *p++;
			if (c == '\0' || !isascii(c) || is_ascii_cntrl(c))
				goto abort;
			id[i] = c; 
		}
		id[i] = '\0';

		/*
		 * Read the payload length (maximum of 3 bytes).
		 */

		data_length = 0;
		for (i = 0; i < 3 && p < end; i++) {
			guchar b = *p++;

			/*
			 * Either GGEP_L_CONT or GGEP_L_LAST must be set, thereby
			 * ensuring that the byte cannot be NUL.
			 */

			if (((b & GGEP_L_XFLAGS) == GGEP_L_XFLAGS) || !(b & GGEP_L_XFLAGS))
				goto abort;

			data_length = (data_length << GGEP_L_VSHIFT) | (b & GGEP_L_VALUE);

			if (b & GGEP_L_LAST) {
				length_ended = TRUE;
				break;
			}
		}

		if (!length_ended)
			goto abort;

		/*
		 * Ensure we have enough bytes left for the payload.  If not, it
		 * means the length is garbage.
		 */

		/* Check whether there are enough bytes for the payload */
		if ((size_t) (end - p) < data_length)
			goto abort;

		/*
		 * Some sanity checks:
		 *
		 * A COBS-encoded buffer can be trivially validated.
		 * A deflated payload must be at least 6 bytes with a valid header.
		 */

		if (flags & (GGEP_F_COBS|GGEP_F_DEFLATE)) {
			guint d_len = data_length;

			if (flags & GGEP_F_COBS) {
				if (d_len == 0 || !cobs_is_valid(p, d_len))
					goto abort;
				d_len--;					/* One byte of overhead */
			}

			if (flags & GGEP_F_DEFLATE) {
				guint offset = 0;

				if (d_len < 6)
					goto abort;

				/*
				 * If COBS-ed, since neither the first byte nor the
				 * second byte of the raw deflated payload can be NUL,
				 * the leading COBS code will be at least 3.  Then
				 * the next 2 bytes are the raw deflated header.
				 *
				 * If not COBS-ed, check whether payload holds a valid
				 * deflated header.
				 */

				if (flags & GGEP_F_COBS) {
					if ((guchar) *p < 3)
						goto abort;
					offset = 1;			/* Skip leading byte */
				}

				if (!zlib_is_valid_header(p + offset, d_len))
					goto abort;
			}
		}

		/*
		 * OK, at this point we have validated the GGEP header.
		 */

		d = walloc(sizeof *d);

		d->ext_phys_payload = p;
		d->ext_phys_paylen = data_length;
		d->ext_phys_len = (p - lastp) + data_length;
		d->ext_ggep_cobs = flags & GGEP_F_COBS;
		d->ext_ggep_deflate = flags & GGEP_F_DEFLATE;

		if (0 == (flags & (GGEP_F_COBS|GGEP_F_DEFLATE))) {
			d->ext_payload = d->ext_phys_payload;
			d->ext_paylen = d->ext_phys_paylen;
		} else
			d->ext_payload = NULL;		/* Will lazily compute, if accessed */

		exv->opaque = d;

		g_assert(ext_phys_headlen(d) >= 0);

		/*
		 * Look whether we know about this extension.
		 *
		 * If we do, the name is the ID as well.  Otherwise, for tracing
		 * and debugging purposes, save the name away, once.
		 */

		exv->ext_type = EXT_GGEP;
		exv->ext_token = rw_ggep_screen(id, &name);
		exv->ext_name = name;

		if (name != NULL)
			d->ext_ggep_id = name;
		else
			d->ext_ggep_id = ext_name_atom(id);

		/*
		 * One more entry, prepare next iteration.
		 */

		exv++;
		count++;
		lastp = p + data_length;
		p = lastp;

		/*
		 * Was this the last extension?
		 */

		if (flags & GGEP_F_LAST)
			break;
	}

	*retp = lastp;	/* Points to first byte after what we parsed */

	return count;

abort:
	/*
	 * Cleanup any extension we already parsed.
	 */

	while (count--) {
		exv--;
		wfree(exv->opaque, sizeof(extdesc_t));
		exv->opaque = NULL;
	}

	return 0;		/* Cannot be a GGEP block: leave parsing pointer intact */
}

/**
 * Parses a URN block (one URN only).
 */
static gint
ext_huge_parse(gchar **retp, gint len, extvec_t *exv, gint exvcnt)
{
	gchar *p = *retp;
	gchar *end = p + len;
	gchar *lastp = p;				/* Last parsed point */
	gchar *name_start;
	ext_token_t token;
	gchar *payload_start = NULL;
	gint data_length = 0;
	const gchar *name = NULL;
	extdesc_t *d;

	g_assert(exvcnt > 0);
	g_assert(exv->opaque == NULL);

	/*
	 * Make sure we can at least read "urn:", i.e. that we have 4 chars.
	 */

	if (len < 4)
		return 0;

	/*
	 * Recognize "urn:".
	 */

	p = is_strcaseprefix(p, "urn:");
	if (!p)
		return 0;

	/*
	 * Maybe it's simply a "urn:" empty specification?
	 */

	if (p == end || *p == '\0' || *p == HUGE_FS) {
		token = EXT_T_URN_EMPTY;
		payload_start = p;
		g_assert(data_length == 0);
		goto found;
	}

	/*
	 * Look for the end of the name, identified by ':'.
	 */

	name_start = p;

	while (p < end) {
		if (*p == ':')
			break;
		p++;
	}

	if (p == end || p == name_start)	/* Not found, or empty name */
		return 0;

	g_assert(*p == ':');

	/*
	 * Lookup the token.
	 */

	*p = '\0';
	token = rw_urn_screen(name_start, &name);
	*p++ = ':';

	/*
	 * Now extract the payload (must be made of alphanum chars),
	 * until we reach a delimiter (NUL byte, GGEP header, GEM separator).
	 * NB: of those, only GGEP_MAGIC could be "alnum" under some locales.
	 */

	payload_start = p;

	while (p < end) {
		guchar c = *p++;
		if (!is_ascii_alnum(c) || c == (guchar) GGEP_MAGIC) {
			p--;
			break;
		}
		data_length++;
	}

	g_assert(data_length == p - payload_start);

found:
	g_assert(payload_start);

	d = walloc(sizeof(*d));

	d->ext_phys_payload = payload_start;
	d->ext_phys_paylen = data_length;
	d->ext_phys_len = (payload_start - lastp) + data_length;
	d->ext_payload = d->ext_phys_payload;
	d->ext_paylen = d->ext_phys_paylen;

	exv->opaque = d;
	exv->ext_type = EXT_HUGE;
	exv->ext_name = name;
	exv->ext_token = token;

	g_assert(ext_phys_headlen(d) >= 0);
	g_assert(p - lastp == d->ext_phys_len);

	*retp = p;	/* Points to first byte after what we parsed */

	return 1;
}

/**
 * Parses a XML block (grabs the whole xml up to the first NUL or separator).
 */
static gint
ext_xml_parse(gchar **retp, gint len, extvec_t *exv, gint exvcnt)
{
	gchar *p = *retp;
	gchar *end = p + len;
	gchar *lastp = p;				/* Last parsed point */
	extdesc_t *d;

	g_assert(exvcnt > 0);
	g_assert(exv->opaque == NULL);

	while (p < end) {
		guchar c = *p++;
		if (c == '\0' || c == (guchar) HUGE_FS) {
			p--;
			break;
		}
	}

	/*
	 * We don't analyze the XML, encapsulate as one big opaque chunk.
	 */

	d = walloc(sizeof(*d));

	d->ext_phys_payload = lastp;
	d->ext_phys_len = d->ext_phys_paylen = p - lastp;
	d->ext_payload = d->ext_phys_payload;
	d->ext_paylen = d->ext_phys_paylen;

	exv->opaque = d;
	exv->ext_type = EXT_XML;
	exv->ext_name = NULL;
	exv->ext_token = EXT_T_XML;

	g_assert(p - lastp == d->ext_phys_len);

	*retp = p;			/* Points to first byte after what we parsed */

	return 1;
}

/**
 * Parses an unknown block, attempting to resynchronize on a known separator.
 * Everything up to the resync point is wrapped as an "unknown" extension.
 *
 * If `skip' is TRUE, we don't resync on the first resync point.
 */
static gint
ext_unknown_parse(gchar **retp, gint len, extvec_t *exv,
	gint exvcnt, gboolean skip)
{
	gchar *p = *retp;
	gchar *end = p + len;
	gchar *lastp = p;				/* Last parsed point */
	extdesc_t *d;

	g_assert(exvcnt > 0);
	g_assert(exv->opaque == NULL);

	/*
	 * Try to resync on a NUL byte, the HUGE_FS separator, "urn:" or what
	 * could appear to be the start of a GGEP block or XML.
	 */

	while (p < end) {
		guchar c = *p++;
		if (
			(c == '\0' || c == (guchar) HUGE_FS || c == (guchar) GGEP_MAGIC) ||
			(
				(c == 'u' || c == 'U') &&
				(end - p) >= 3 &&
				is_strcaseprefix(p, "rn:")
			) ||
			(c == '<' && (p < end) && is_ascii_alpha((guchar) *p))
		) {
			if (skip) {
				skip = FALSE;
				continue;
			}
			p--;
			break;
		}
	}

	/*
	 * Encapsulate as one big opaque chunk.
	 */

	d = walloc(sizeof(*d));

	d->ext_phys_payload = lastp;
	d->ext_phys_len = d->ext_phys_paylen = p - lastp;
	d->ext_payload = d->ext_phys_payload;
	d->ext_paylen = d->ext_phys_paylen;

	exv->opaque = d;
	exv->ext_type = EXT_UNKNOWN;
	exv->ext_name = NULL;
	exv->ext_token = EXT_T_UNKNOWN;

	g_assert(p - lastp == d->ext_phys_len);

	*retp = p;			/* Points to first byte after what we parsed */

	return 1;
}

/**
 * Parses a "no extension" block, made of NUL bytes or HUGE field separators
 * exclusively.  Obviously, this is unneeded stuff that simply accounts
 * for overhead!
 *
 * If more that one separator in a row is found, they are all wrapped as a
 * "none" extension.
 */
static gint
ext_none_parse(gchar **retp, gint len, extvec_t *exv, gint exvcnt)
{
	gchar *p = *retp;
	gchar *end = p + len;
	gchar *lastp = p;				/* Last parsed point */
	extdesc_t *d;

	g_assert(exvcnt > 0);
	g_assert(exv->opaque == NULL);

	while (p < end) {
		guchar c = *p++;
		if (c == '\0' || c == (guchar) HUGE_FS)
			continue;
		p--;						/* Point back to the non-NULL char */
		break;
	}

	/*
	 * If we're still at the beginning, it means there was no separator
	 * at all, so we did not find any "null" extension.
	 */

	if (p == lastp)
		return 0;

	/*
	 * Encapsulate as one big opaque chunk.
	 */

	d = walloc(sizeof(*d));

	d->ext_phys_payload = lastp;
	d->ext_phys_len = d->ext_phys_paylen = p - lastp;
	d->ext_payload = d->ext_phys_payload;
	d->ext_paylen = d->ext_phys_paylen;

	exv->opaque = d;
	exv->ext_type = EXT_NONE;
	exv->ext_name = NULL;
	exv->ext_token = EXT_T_OVERHEAD;

	g_assert(p - lastp == d->ext_phys_len);

	*retp = p;			/* Points to first byte after what we parsed */

	return 1;
}

/**
 * Merge two consecutive extensions `exv' and `next' into one big happy
 * extension, in `exv'.   The resulting extension type is that of `exv'.
 */
static void
ext_merge_adjacent(extvec_t *exv, extvec_t *next)
{
	gchar *end;
	gchar *nend;
	gchar *nbase;
	guint16 added;
	extdesc_t *d = exv->opaque;
	extdesc_t *nd = next->opaque;

	g_assert(exv->opaque != NULL);
	g_assert(next->opaque != NULL);

	end = d->ext_phys_payload + d->ext_phys_paylen;
	nbase = ext_phys_base(nd);
	nend = nd->ext_phys_payload + nd->ext_phys_paylen;

	g_assert(nbase + nd->ext_phys_len == nend);
	g_assert(nend > end);

	/*
	 * Extensions are adjacent, but can be separated by a single NUL or other
	 * one byte separator.
	 */

	g_assert(nbase == end || nbase == (end + 1));

	added = nend - end;			/* Includes any separator between the two */

	/*
	 * By incrementing the total length and the payload length of `exv',
	 * we catenate `next' at the tail of `exv'.
	 */

	d->ext_phys_len += added;
	d->ext_phys_paylen += added;

	if (d->ext_payload != NULL) {
		g_assert(d->ext_payload == d->ext_phys_payload);

		d->ext_paylen += added;
	}

	/*
	 * Get rid of the `next' opaque descriptor.
	 * We should not have computed any "virtual" payload at this point.
	 */

	g_assert(
		nd->ext_payload == NULL || nd->ext_payload == nd->ext_phys_payload);

	wfree(nd, sizeof(*nd));
	next->opaque = NULL;
}

/**
 * Parse extension block of `len' bytes starting at `buf' and fill the
 * supplied extension vector `exv', whose size is `exvcnt' entries.
 *
 * @return the number of filled entries.
 */
gint
ext_parse(gchar *buf, gint len, extvec_t *exv, gint exvcnt)
{
	gchar *p = buf;
	gchar *end = buf + len;
	gint cnt = 0;

	g_assert(buf);
	g_assert(len > 0);
	g_assert(exv);
	g_assert(exvcnt > 0);
	g_assert(exv->opaque == NULL);

	while (p < end && exvcnt > 0) {
		gint found = 0;
		gchar *old_p = p;

		g_assert(len > 0);

		/*
		 * From now on, all new Gnutella extensions will be done via GGEP.
		 * However, we have to be backward compatible with legacy extensions
		 * that predate GGEP (HUGE and XML) and were not properly encapsulated.
		 */

		switch (*p) {
		case GGEP_MAGIC:
			p++;
			if (p == end)
				goto out;
			found = ext_ggep_parse(&p, len-1, exv, exvcnt);
			break;
		case 'u':
		case 'U':
			found = ext_huge_parse(&p, len, exv, exvcnt);
			break;
		case '<':
			found = ext_xml_parse(&p, len, exv, exvcnt);
			break;
		case HUGE_FS:
		case '\0':
			p++;
			if (p == end)
				goto out;
			found = ext_none_parse(&p, len-1, exv, exvcnt);
			if (!found) {
				len--;
				continue;			/* Single separator, no bloat then */
			}
			break;
		default:
			found = ext_unknown_parse(&p, len, exv, exvcnt, FALSE);
			break;
		}

		/*
		 * If parsing did not advance one bit, grab as much as we can as
		 * an "unknown" extension.
		 */

		g_assert(found == 0 || p != old_p);

		if (found == 0) {
			g_assert(*old_p == GGEP_MAGIC || p == old_p);

			/*
			 * If we were initially on a GGEP magic byte, and since we did
			 * not find any valid GGEP extension, go back one byte.  We're
			 * about to skip the first synchronization point...
			 */

			if (*old_p == GGEP_MAGIC) {
				p--;
				g_assert(p == old_p);
			}

			found = ext_unknown_parse(&p, len, exv, exvcnt, TRUE);
		}

		g_assert(found > 0);
		g_assert(found <= exvcnt);
		g_assert(p != old_p);

		len -= p - old_p;

		/*
		 * If we found an "unknown" or "none" extension, and the previous
		 * extension was "unknown", merge them.  The result will be "unknown".
		 */

		if (
			found == 1 && cnt > 0 &&
			(exv->ext_type == EXT_UNKNOWN || exv->ext_type == EXT_NONE)
		) {
			extvec_t *prev = exv - 1;
			if (prev->ext_type == EXT_UNKNOWN) {
				ext_merge_adjacent(prev, exv);
				continue;					/* Don't move `exv' */
			}
		}

		exv += found;
		exvcnt -= found;
		cnt += found;
	}

out:
	return cnt;
}

/**
 * Inflate `len' bytes starting at `buf', up to GGEP_MAXLEN bytes.
 * The payload `name' is given only in case there is an error to report.
 *
 * Returns the allocated inflated buffer, and its inflated length in `retlen'.
 * Returns NULL on error.
 */
static gchar *
ext_ggep_inflate(gchar *buf, gint len, guint16 *retlen, const gchar *name)
{
	gchar *result;					/* Inflated buffer */
	gint rsize;						/* Result's buffer size */
	z_streamp inz;
	gint ret;
	gint inflated;					/* Amount of inflated data so far */
	gboolean failed = FALSE;

	g_assert(buf);
	g_assert(len > 0);
	g_assert(retlen);

	/*
	 * Allocate decompressor.
	 */

	inz = walloc(sizeof(*inz));

	inz->zalloc = NULL;
	inz->zfree = NULL;
	inz->opaque = NULL;

	ret = inflateInit(inz);

	if (ret != Z_OK) {
		wfree(inz, sizeof(*inz));
		g_warning("unable to setup decompressor for GGEP payload \"%s\": %s",
			name, zlib_strerror(ret));
		return NULL;
	}

	rsize = len * 2;				/* Assume a 50% compression ratio */
	rsize = MIN(rsize, GGEP_MAXLEN);
	result = g_malloc(rsize);

	/*
	 * Prepare call to inflate().
	 */

	inz->next_in = (gpointer) buf;
	inz->avail_in = len;

	inflated = 0;

	for (;;) {
		/*
		 * Resize output buffer if needed.
		 * Never grow the result buffer to more than MAX_PAYLOAD_LEN bytes.
		 */

		if (rsize == inflated) {
			rsize += MAX(len, GGEP_GROW);
			rsize = MIN(rsize, GGEP_MAXLEN);

			if (rsize == inflated) {		/* Reached maximum size! */
				g_warning("GGEP payload \"%s\" would be larger than %d bytes",
					name, GGEP_MAXLEN);
				failed = TRUE;
				break;
			}

			g_assert(rsize > inflated);

			result = g_realloc(result, rsize);
		}

		inz->next_out = (guchar *) result + inflated;
		inz->avail_out = rsize - inflated;

		/*
		 * Decompress data.
		 */

		ret = inflate(inz, Z_SYNC_FLUSH);
		inflated += rsize - inflated - inz->avail_out;

		g_assert(inflated <= rsize);

		if (ret == Z_STREAM_END)				/* All done! */
			break;

		if (ret != Z_OK) {
			g_warning("decompression of GGEP payload \"%s\" failed: %s",
				name, zlib_strerror(ret));
			failed = TRUE;
			break;
		}
	}

	/*
	 * Dispose of decompressor.
	 */

	ret = inflateEnd(inz);
	if (ret != Z_OK)
		g_warning("while freeing decompressor for GGEP payload \"%s\": %s",
			name, zlib_strerror(ret));

	wfree(inz, sizeof(*inz));

	/*
	 * Return NULL on error, fill `retlen' if OK.
	 */

	if (failed) {
		G_FREE_NULL(result);
		return NULL;
	}

	*retlen = inflated;

	g_assert(*retlen == inflated);	/* Make sure it was not truncated */

	return result;				/* OK, successfully inflated */
}

/**
 * Decode the GGEP payload pointed at by `e', allocating a new buffer capable
 * of holding the decoded data.
 *
 * This is performed only when the GGEP payload is either COBS-encoded or
 * deflated.
 */
static void
ext_ggep_decode(const extvec_t *e)
{
	gchar *pbase;					/* Current payload base */
	gint plen;						/* Curernt payload length */
	gchar *uncobs = NULL;			/* COBS-decoded buffer */
	gint uncobs_len = 0;			/* Length of walloc()'ed buffer */
	gint result;					/* Decoded length */
	extdesc_t *d;

	g_assert(e);
	g_assert(e->ext_type == EXT_GGEP);
	g_assert(e->opaque != NULL);

	d = e->opaque;

	g_assert(d->ext_ggep_cobs || d->ext_ggep_deflate);
	g_assert(d->ext_payload == NULL);

	pbase = d->ext_phys_payload;
	plen = d->ext_phys_paylen;

	if (plen == 0)
		goto out;

	/*
	 * COBS decoding must be performed before inflation, if any.
	 */

	if (d->ext_ggep_cobs) {
		uncobs = walloc(plen);		/* At worse slightly oversized */
		uncobs_len = plen;

		if (!d->ext_ggep_deflate) {
			if (!cobs_decode_into(pbase, plen, uncobs, plen, &result)) {
				if (ggep_debug)
					g_warning("unable to decode COBS buffer");
				goto out;
			}

			g_assert(result <= plen);

			d->ext_payload = uncobs;
			d->ext_paylen = result;
			d->ext_rpaylen = plen;		/* Signals it was walloc()'ed */

			return;
		} else {
			if (!cobs_decode_into(pbase, plen, uncobs, plen, &result)) {
				if (ggep_debug)
					g_warning("unable to decode COBS buffer");
				goto out;
			}

			g_assert(result <= plen);

			/*
			 * Replace current payload base/length with the COBS buffer.
			 */

			pbase = uncobs;
			plen = result;
		}

		if (plen == 0)		/* 0 bytes cannot be a valid deflated payload */
			goto out;

		/* FALL THROUGH */
	}

	/*
	 * Payload is deflated, inflate it.
	 */

	g_assert(d->ext_ggep_deflate);

	d->ext_rpaylen = 0;			/* Signals it was malloc()'ed */
	d->ext_payload =
		ext_ggep_inflate(pbase, plen, &d->ext_paylen, d->ext_ggep_id);

	/* FALL THROUGH */
out:
	if (uncobs != NULL)
		wfree(uncobs, uncobs_len);

	/*
	 * If something went wrong, setup a zero-length payload so that we
	 * don't go through this whole decoding again.
	 */

	if (d->ext_payload == NULL) {
		if (dbg || ggep_debug)
			g_warning("unable to get GGEP \"%s\" %d-byte payload (%s)",
				d->ext_ggep_id, d->ext_phys_paylen,
				(d->ext_ggep_deflate && d->ext_ggep_cobs) ? "COBS + deflated" :
				d->ext_ggep_cobs ? "COBS" : "deflated");

		d->ext_paylen = 0;
		d->ext_payload = d->ext_phys_payload;
	}
}

/**
 * Returns a pointer to the extension's payload.
 */
const gchar *
ext_payload(const extvec_t *e)
{
	extdesc_t *d = e->opaque;

	g_assert(e->opaque != NULL);

	if (d->ext_payload != NULL)
		return d->ext_payload;

	/*
	 * GGEP payload is COBS-ed and/or deflated.
	 */

	ext_ggep_decode(e);

	return d->ext_payload;
}

/**
 * Returns a pointer to the extension's payload length.
 */
guint16
ext_paylen(const extvec_t *e)
{
	extdesc_t *d = e->opaque;

	g_assert(e->opaque != NULL);

	if (d->ext_payload != NULL)
		return d->ext_paylen;

	/*
	 * GGEP payload is COBS-ed and/or deflated.
	 */

	ext_ggep_decode(e);

	return d->ext_paylen;
}

/**
 * Returns a pointer to the extension's header.
 *
 * WARNING: the actual "virtual" payload may not be contiguous to the end
 * of the header: don't read past the ext_headlen() first bytes of the
 * header.
 */
const gchar *
ext_base(const extvec_t *e)
{
	extdesc_t *d = e->opaque;

	g_assert(e->opaque != NULL);

	return ext_phys_base(d);
}

/**
 * Returns the length of the extensions's header.
 */
guint16
ext_headlen(const extvec_t *e)
{
	extdesc_t *d = e->opaque;

	g_assert(e->opaque != NULL);

	return ext_phys_headlen(d);
}

/**
 * Returns the total length of the extension (payload + extension header).
 */
guint16
ext_len(const extvec_t *e)
{
	extdesc_t *d = e->opaque;
	gint headlen;

	g_assert(e->opaque != NULL);

	headlen = ext_phys_headlen(d);

	if (d->ext_payload != NULL)
		return headlen + d->ext_paylen;

	return headlen + ext_paylen(e);		/* Will decompress / COBS decode */
}

/**
 * Returns extension's GGEP ID, or "" if not a GGEP one.
 */
const gchar *
ext_ggep_id_str(const extvec_t *e)
{
	extdesc_t *d = e->opaque;

	g_assert(e->opaque != NULL);

	if (e->ext_type != EXT_GGEP)
		return "";

	return d->ext_ggep_id;
}

/**
 * @return TRUE if extension is printable.
 */
gboolean
ext_is_printable(const extvec_t *e)
{
	const gchar *p = ext_payload(e);
	gint len = ext_paylen(e);

	g_assert(len >= 0);
	while (len--) {
		guchar c = *p++;
		if (!isprint(c))
			return FALSE;
	}

	return TRUE;
}

/**
 * @return TRUE if extension is ASCII.
 */
gboolean
ext_is_ascii(const extvec_t *e)
{
	const gchar *p = ext_payload(e);
	gint len = ext_paylen(e);

	g_assert(len >= 0);
	while (len--) {
		guchar c = *p++;
		if (!isascii(c))
			return FALSE;
	}

	return TRUE;
}

/**
 * @return TRUE if extension is ASCII and contains at least a character.
 */
gboolean
ext_has_ascii_word(const extvec_t *e)
{
	const gchar *p = ext_payload(e);
	gint len = ext_paylen(e);
	gboolean has_alnum = FALSE;

	g_assert(len >= 0);
	while (len--) {
		guchar c = *p++;
		if (!isascii(c))
			return FALSE;
		if (!has_alnum && is_ascii_alnum(c))
			has_alnum = TRUE;
	}

	return has_alnum;
}

/**
 * Dump an extension to specified stdio stream.
 */
static void
ext_dump_one(FILE *f, const extvec_t *e, const gchar *prefix,
	const gchar *postfix, gboolean payload)
{
	guint16 paylen;

	g_assert(e->ext_type < EXT_TYPE_COUNT);
	g_assert(e->opaque != NULL);

	if (prefix)
		fputs(prefix, f);

	fputs(extype[e->ext_type], f);
	fprintf(f, " (token=%d) ", e->ext_token);

	if (e->ext_name)
		fprintf(f, "\"%s\" ", e->ext_name);

	paylen = ext_paylen(e);

	fprintf(f, "%d byte%s", paylen, paylen == 1 ? "" : "s");

	if (e->ext_type == EXT_GGEP) {
		extdesc_t *d = e->opaque;
		fprintf(f, " (ID=\"%s\", COBS: %s, deflate: %s)",
			d->ext_ggep_id,
			d->ext_ggep_cobs ? "yes" : "no",
			d->ext_ggep_deflate ? "yes" : "no");
	}

	if (postfix)
		fputs(postfix, f);

	if (payload && paylen > 0) {
		if (ext_is_printable(e)) {
			if (prefix)
				fputs(prefix, f);

			fputs("Payload: ", f);
			fwrite(ext_payload(e), paylen, 1, f);

			if (postfix)
				fputs(postfix, f);
		} else
			dump_hex(f, "Payload", ext_payload(e), paylen);
	}

	fflush(f);
}

/**
 * Dump all extensions in vector to specified stdio stream.
 *
 * The `prefix' and `postfix' strings, if non-NULL, are emitted before and
 * after the extension summary.
 *
 * If `payload' is true, the payload is dumped in hexadecimal if it contains
 * non-printable characters, as text otherwise.
 */
void
ext_dump(FILE *fd, const extvec_t *exv, gint exvcnt,
	const gchar *prefix, const gchar *postfix, gboolean payload)
{
	while (exvcnt--)
		ext_dump_one(fd, exv++, prefix, postfix, payload);
}

/**
 * Prepare the vector for parsing, by ensuring the `opaque' pointers are
 * all set to NULL.
 */
void
ext_prepare(extvec_t *exv, gint exvcnt)
{
	while (exvcnt--)
		(exv++)->opaque = NULL;
}

/**
 * Reset an extension vector by disposing of the opaque structures
 * and of any allocated "virtual" payload.
 */
void
ext_reset(extvec_t *exv, gint exvcnt)
{
	while (exvcnt--) {
		extvec_t *e = exv++;
		extdesc_t *d;

		if (e->opaque == NULL)		/* No more allocated extensions */
			break;

		d = e->opaque;

		if (d->ext_payload != NULL && d->ext_payload != d->ext_phys_payload) {
			if (d->ext_rpaylen == 0)
				g_free(d->ext_payload);
			else
				wfree(d->ext_payload, d->ext_rpaylen);
		}

		wfree(d, sizeof(*d));
		e->opaque = NULL;
	}
}

/***
 *** Init & Shutdown
 ***/

/**
 * Initialize the extension subsystem.
 */
void
ext_init(void)
{
	ext_names = g_hash_table_new(g_str_hash, g_str_equal);

	rw_is_sorted("ggeptable", ggeptable, G_N_ELEMENTS(ggeptable));
	rw_is_sorted("urntable", urntable, G_N_ELEMENTS(urntable));
}

/**
 * Free resources used by the extension subsystem.
 */
void
ext_close(void)
{
	g_hash_table_foreach_remove(ext_names, ext_names_kv_free, NULL);
	g_hash_table_destroy(ext_names);
}

/* vi: set ts=4 sw=4 cindent: */
