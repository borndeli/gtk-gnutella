/*
 * $Id$
 *
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
 * @file
 *
 * PDU Messages.
 */

#include <string.h>		/* For memcpy() */

#include "gnutella.h"
#include "pmsg.h"
#include "zalloc.h"
#include "walloc.h"
#include "override.h"		/* Must be the last header included */

RCSID("$Id$");

#define implies(a,b)	(!(a) || (b))
#define valid_ptr(a)	(((gulong) (a)) > 100L)

#define EMBEDDED_OFFSET	G_STRUCT_OFFSET(pdata_t, d_embedded)

/*
 * An extended message block.
 *
 * Relies on C's structural equivalence for the first 4 fields.
 * An extended message block can be identified by its `m_prio' field
 * having the PMSG_PF_EXT flag set.
 */
typedef struct pmsg_ext {
	/* First four fields like "stuct pmsg" */
	gchar *m_rptr;					/* First unread byte in buffer */
	gchar *m_wptr;					/* First unwritten byte in buffer */
	pdata_t *m_data;				/* Data buffer */
	guint m_prio;					/* Message priority (0 = normal) */
	/* Additional fields */
	pmsg_free_t m_free;				/* Free routine */
	gpointer m_arg;					/* Argument to pass to free routine */
} pmsg_ext_t;

static zone_t *mb_zone = NULL;

/**
 * Allocate internal variables.
 */
void
pmsg_init(void)
{
	mb_zone = zget(sizeof(pmsg_t), 1024);
}

/**
 * Free internal variables
 */
void
pmsg_close(void)
{
	zdestroy(mb_zone);
}

/**
 * Compute message's size.
 */
int
pmsg_size(pmsg_t *mb)
{
	int msize = 0;

	g_assert(mb);

	/* In prevision of message block chaining */
	do {
		msize += mb->m_wptr - mb->m_rptr;
	} while (0);

	return msize;
}

/**
 * Fill newly created message block.
 *
 * @return the message block given as argument.
 */
static pmsg_t *
pmsg_fill(pmsg_t *mb, pdata_t *db, gint prio, void *buf, gint len)
{
	mb->m_data = db;
	mb->m_prio = prio;
	db->d_refcnt++;

	if (buf) {
		mb->m_rptr = db->d_arena;
		mb->m_wptr = db->d_arena + len;
		memcpy(db->d_arena, buf, len);
	} else
		mb->m_rptr = mb->m_wptr = db->d_arena;

	g_assert(implies(buf, len == pmsg_size(mb)));

	return mb;
}

/**
 * Create new message from user provided data, which are copied into the
 * allocated data block.  If no user buffer is provided, an empty message
 * is created and the length is used to size the data block.
 *
 * @return a message made of one message block referencing one new data block.
 */
pmsg_t *
pmsg_new(gint prio, void *buf, gint len)
{
	pmsg_t *mb;
	pdata_t *db;

	g_assert(len > 0);
	g_assert(implies(buf, valid_ptr(buf)));
	g_assert(0 == (prio & ~PMSG_PRIO_MASK));

	mb = (pmsg_t *) zalloc(mb_zone);
	db = pdata_new(len);

	return pmsg_fill(mb, db, prio, buf, len);
}

/**
 * Like pmsg_new() but returns an extended form with a free routine callback.
 */
pmsg_t *
pmsg_new_extend(gint prio, void *buf, gint len, pmsg_free_t free, gpointer arg)
{
	pmsg_ext_t *emb;
	pdata_t *db;

	g_assert(len > 0);
	g_assert(implies(buf, valid_ptr(buf)));
	g_assert(0 == (prio & ~PMSG_PRIO_MASK));

	emb = (pmsg_ext_t *) walloc(sizeof(*emb));
	db = pdata_new(len);

	emb->m_free = free;
	emb->m_arg = arg;

	(void) pmsg_fill((pmsg_t *) emb, db, prio, buf, len);

	emb->m_prio |= PMSG_PF_EXT;

	return (pmsg_t *) emb;
}

/**
 * Allocate new message using existing data block `db'.
 * The `roff' and `woff' are used to delimit the start and the end (first
 * unwritten byte) of the message within the data buffer.
 *
 * @return new message.
 */
pmsg_t *
pmsg_alloc(gint prio, pdata_t *db, gint roff, gint woff)
{
	pmsg_t *mb;

	g_assert(valid_ptr(db));
	g_assert(roff >= 0 && (size_t) roff <= pdata_len(db));
	g_assert(woff >= 0 && (size_t) woff <= pdata_len(db));
	g_assert(woff >= roff);
	g_assert(0 == (prio & ~PMSG_PRIO_MASK));

	mb = (pmsg_t *) zalloc(mb_zone);

	mb->m_data = db;
	mb->m_prio = prio;
	db->d_refcnt++;

	mb->m_rptr = db->d_arena + roff;
	mb->m_wptr = db->d_arena + woff;

	return mb;
}

/**
 * Extended cloning of message, adds a free routine callback.
 */
pmsg_t *
pmsg_clone_extend(pmsg_t *mb, pmsg_free_t free, gpointer arg)
{
	pmsg_ext_t *nmb;

	nmb = (pmsg_ext_t *) walloc(sizeof(*nmb));

	nmb->m_rptr = mb->m_rptr;
	nmb->m_wptr = mb->m_wptr;
	nmb->m_data = mb->m_data;
	nmb->m_prio = mb->m_prio;
	pdata_addref(nmb->m_data);

	nmb->m_prio |= PMSG_PF_EXT;
	nmb->m_free = free;
	nmb->m_arg = arg;

	return (pmsg_t *) nmb;
}

/**
 * Shallow cloning of extended message, result is referencing the same data.
 */
static pmsg_t *
pmsg_clone_ext(pmsg_ext_t *mb)
{
	pmsg_ext_t *nmb;

	g_assert(pmsg_is_extended(mb));

	nmb = (pmsg_ext_t *) walloc(sizeof(*nmb));
	*nmb = *mb;					/* Struct copy */
	pdata_addref(nmb->m_data);

	return (pmsg_t *) nmb;
}

/**
 * Shallow cloning of message, result is referencing the same data.
 */
pmsg_t *
pmsg_clone(pmsg_t *mb)
{
	pmsg_t *nmb;

	if (pmsg_is_extended(mb))
		return pmsg_clone_ext((pmsg_ext_t *) mb);

	nmb = (pmsg_t *) zalloc(mb_zone);
	*nmb = *mb;					/* Struct copy */
	pdata_addref(nmb->m_data);

	return nmb;
}

/**
 * Free all message blocks, and decrease ref count on all data buffers.
 */
void
pmsg_free(pmsg_t *mb)
{
	/* In provision for messsage chaining */
	do {
		pdata_t *db = mb->m_data;

		g_assert(valid_ptr(mb));

		/*
		 * Invoke free routine on extended message block.
		 */

		if (pmsg_is_extended(mb)) {
			pmsg_ext_t *emb = (pmsg_ext_t *) mb;
			(*emb->m_free)(mb, emb->m_arg);
			wfree(emb, sizeof(*emb));
		} else
			zfree(mb_zone, mb);

		/*
		 * Unref buffer data only after possible free routine was
		 * invoked, since it may cause a free, preventing access to
		 * memory from within the free routine.
		 */

		pdata_unref(db);
	} while (0);
}

/**
 * Write data at the end of the message.
 * The message must be the only reference to the underlying data.
 *
 * @returns amount of written data.
 */
gint
pmsg_write(pmsg_t *mb, gpointer data, gint len)
{
	pdata_t *arena = mb->m_data;
	gint available = arena->d_end - mb->m_wptr;
	gint written = len >= available ? available : len;

	g_assert(pmsg_is_writable(mb));	/* Not shared, or would corrupt data */
	g_assert(available >= 0);		/* Data cannot go beyond end of arena */

	if (written == 0)
		return 0;

	memcpy(mb->m_wptr, data, written);
	mb->m_wptr += written;

	return written;
}

/**
 * Read data from the message, returning the amount of bytes transferred.
 */
gint
pmsg_read(pmsg_t *mb, gpointer data, gint len)
{
	gint available = mb->m_wptr - mb->m_rptr;
	gint readable = len >= available ? available : len;

	g_assert(available >= 0);		/* Data cannot go beyond end of arena */

	if (readable == 0)
		return 0;

	memcpy(data, mb->m_rptr, readable);
	mb->m_rptr += readable;

	return readable;
}

/**
 * Allocate a new data block of given size.
 * The block header is at the start of the allocated block.
 */
pdata_t *
pdata_new(int len)
{
	pdata_t *db;
	gchar *arena;

	g_assert(len > 0);

	arena = g_malloc(len + EMBEDDED_OFFSET);

	db = pdata_allocb(arena, len + EMBEDDED_OFFSET, NULL, 0);

	g_assert((size_t) len == pdata_len(db));

	return db;
}

/**
 * Create an embedded data buffer out of existing arena.
 *
 * The optional `freecb' structure supplies the free routine callback to be
 * used to free the arena, with freearg as additional argument.
 */
pdata_t *
pdata_allocb(void *buf, gint len, pdata_free_t freecb, gpointer freearg)
{
	pdata_t *db;

	g_assert(valid_ptr(buf));
	g_assert(len >= (gint) EMBEDDED_OFFSET);
	g_assert(implies(freecb, valid_ptr(freecb)));

	db = (pdata_t *) buf;

	db->d_arena = db->d_embedded;
	db->d_end = db->d_arena + (len - EMBEDDED_OFFSET);
	db->d_refcnt = 0;
	db->d_free = freecb;
	db->d_arg = freearg;

	g_assert((size_t) len - EMBEDDED_OFFSET == pdata_len(db));

	return db;
}

/**
 * Create an external (arena not embedded) data buffer out of existing arena.
 *
 * The optional `freecb' structure supplies the free routine callback to be
 * used to free the arena, with freearg as additional argument.
 */
pdata_t *
pdata_allocb_ext(void *buf, gint len, pdata_free_t freecb, gpointer freearg)
{
	pdata_t *db;

	g_assert(valid_ptr(buf));
	g_assert(implies(freecb, valid_ptr(freecb)));

	db = g_malloc(sizeof(*db));

	db->d_arena = buf;
	db->d_end = (gchar *) buf + len;
	db->d_refcnt = 0;
	db->d_free = freecb;
	db->d_arg = freearg;

	g_assert((size_t) len == pdata_len(db));

	return db;
}

/**
 * This free routine can be used when there is nothing to be freed for
 * the buffer, probably because it was made out of a static buffer.
 */
void
pdata_free_nop(gpointer p, gpointer arg)
{
}

/**
 * Free data block when its reference count has reached 0.
 */
static void
pdata_free(pdata_t *db)
{
	gboolean is_embedded = (db->d_arena == db->d_embedded);

	g_assert(db->d_refcnt == 0);

	/*
	 * If user supplied a free routine for the buffer, invoke it.
	 */

	if (db->d_free) {
		gpointer p = is_embedded ? (gpointer) db : (gpointer) db->d_arena;
		(*db->d_free)(p, db->d_arg);
		if (!is_embedded)
			G_FREE_NULL(db);
	} else {
		if (!is_embedded)
			G_FREE_NULL(db->d_arena);
		G_FREE_NULL(db);
	}
}

/**
 * Decrease reference count on buffer, and free it when it reaches 0.
 */
void
pdata_unref(pdata_t *db)
{
	g_assert(valid_ptr(db));
	g_assert(db->d_refcnt > 0);

	if (db->d_refcnt-- == 1)
		pdata_free(db);
}

/* vi: set ts=4: */
