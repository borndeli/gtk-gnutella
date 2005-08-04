/*
 * $Id$
 *
 * Copyright (c) 2005, Christian Biere
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
 * Network RX -- chunked-encoding.
 *
 * @author Christian Biere
 * @date 2005
 */

#include "common.h"

RCSID("$Id$");

#include "pmsg.h"
#include "hosts.h"				/* For host_ip() */
#include "rx.h"
#include "rx_chunk.h"
#include "rxbuf.h"

#include "lib/misc.h"
#include "lib/walloc.h"
#include "lib/override.h"		/* Must be the last header included */

enum chunk_state {
	CHUNK_STATE_ERROR = 0,
	CHUNK_STATE_SIZE,
	CHUNK_STATE_EXT,
	CHUNK_STATE_DATA,
	CHUNK_STATE_DATA_CRLF,
	CHUNK_STATE_TRAILER_START,
	CHUNK_STATE_TRAILER,
	CHUNK_STATE_END,

	NUM_CHUNK_STATES
};

/**
 * Private attributes for the dechunking layer.
 */
struct attr {
	struct rx_chunk_cb *cb;		/**< Layer-specific callbacks */
	guint64 data_remain;		/**< Amount of remaining chunk payload data */
	gchar hex_buf[16];			/**< Holds the hex digits of chunk-size */
	size_t hex_pos;				/**< Current position in hex_buf */
	enum chunk_state state;		/**< Current decoding state */
	gint flags;
};

#define IF_ENABLED	0x00000001		/**< Reception enabled */
#define IF_NO_CRLF	0x00000002		/**< Set when missing CRLF after data */

struct dechunk {
	const char *src;
	char *dst;
	size_t dst_len, src_len;
};

/**
 * Decodes "chunked" data.
 *
 * @param rx is the current RX driver.
 * @param ctx an initialized ``struct dechunk''.
 * @param error_str if not NULL and dechunk() fails, it will point to
 *        an informational error message.
 *
 * @return 0 on success, -1 on failure.
 */
static int
dechunk(rxdrv_t *rx, struct dechunk *ctx, const gchar **p_error_str)
{
	struct attr *attr = rx->opaque;
	const gchar *error_str;

	g_assert(ctx);
	g_assert(attr);
	
	g_assert(ctx->dst);
	g_assert(ctx->src);
	g_assert(ctx->src_len > 0);
	g_assert(ctx->dst_len > 0);

	g_assert(attr->state < NUM_CHUNK_STATES);

	do {
		switch (attr->state) {
		case CHUNK_STATE_DATA:
			g_assert(attr->data_remain > 0);

			/* Just copy the chunk-data to destination buffer */
			{
				size_t n;

				n = MIN(ctx->src_len, attr->data_remain);
				n = MIN(n, ctx->dst_len);
				memcpy(ctx->dst, ctx->src, n);
				ctx->dst += n;
				ctx->src += n;
				ctx->dst_len -= n;
				ctx->src_len -= n;
				attr->data_remain -= n;
			}
			if (0 == attr->data_remain)
				attr->state = CHUNK_STATE_DATA_CRLF;
			break;

		case CHUNK_STATE_DATA_CRLF:
			/* The chunk-data must be followed by a CRLF */
			while (ctx->src_len > 0) {
				guchar c;

				ctx->src_len--;
				c = *ctx->src++;
				if ('\r' == c) {
				   /*
					* This allows more than one CR but we must consume
				 	* some data or keep state over this otherwise.
					*/
					continue;
				} else if ('\n' == c) {
					attr->state = CHUNK_STATE_SIZE;
					break;
				} else {
					/*
					 * Normally it is an error, there should be CRLF after
					 * the chunk data.  However, they might have forgotten
					 * to send the '\n' or the whole sequence.
					 *
					 * If what follows looks like a valid chunk size, then
					 * we should be able to resync properly: Unread the
					 * character and move on to the chunk size decoding.
					 */

					if (!(attr->flags & IF_NO_CRLF)) {
						attr->flags |= IF_NO_CRLF;
						g_warning("Host %s forgot CRLF after data",
							host_ip(&rx->host));
					}

					ctx->src_len++;
					ctx->src--;
					attr->state = CHUNK_STATE_SIZE;
					break;
				}
			}
			break;

		case CHUNK_STATE_SIZE:
			g_assert(attr->hex_pos < sizeof attr->hex_buf);
			while (ctx->src_len > 0) {
				guchar c;

				ctx->src_len--;
				c = *ctx->src++;
				if (is_ascii_xdigit(c)) {
					if (attr->hex_pos >= sizeof attr->hex_buf) {
						error_str = "Overflow in chunk-size";
						goto error;
					}
					/* Collect up to 16 hex characters */
					attr->hex_buf[attr->hex_pos++] = c;
				} else {
					/*
					 * There might be a chunk-extension after the
					 * hexadecimal chunk-size but there shouldn't
					 * anything else.
					 */

					if (!is_ascii_space(c) && ';' != c) {
						error_str = "Bad chunk-size";
						goto error;
					}

					/*
					 * Pick up the collected hex digits and
					 * calculate the chunk-size.
					 */

					{
						guint64 v = 0;
						guint i;

						for (i = 0; i < attr->hex_pos; i++)
							v = (v << 4) | hex2dec(attr->hex_buf[i]);

						attr->data_remain = v;
					}

					attr->hex_pos = 0;
					attr->state = CHUNK_STATE_EXT;
					break;
				}
			}
			break;

		case CHUNK_STATE_EXT:
			/* Just skip over the chunk-extension */
			while (ctx->src_len > 0) {
				ctx->src_len--;
				if ('\n' == *ctx->src++) {
					attr->state = 0 != attr->data_remain
						? CHUNK_STATE_DATA
						: CHUNK_STATE_TRAILER_START;
					break;
				}
			}
			break;

		case CHUNK_STATE_TRAILER_START:
			/* We've reached another trailer line */
			if (ctx->src_len < 1)
				break;
			if ('\r' == ctx->src[0]) {
				/*
				 * This allows more than one CR but we must consume
				 * some data or keep state over this otherwise.
				 */
				ctx->src++;
				ctx->src_len--;
			}
			if (ctx->src_len < 1)
				break;
			if ('\n' == ctx->src[0]) {
				/* An empty line means the end of all trailers was reached */
				ctx->src++;
				ctx->src_len--;
				attr->state = CHUNK_STATE_END;
				break;
			}
			attr->state = CHUNK_STATE_TRAILER;
			/* FALL THROUGH */

		case CHUNK_STATE_TRAILER:
			/* Just skip over the trailer line */
			while (ctx->src_len > 0) {
				ctx->src_len--;
				if ('\n' == *ctx->src++) {
					/*
					 * Now check whether there's another trailer
					 * line or whether we've reached the end
					 */

					attr->state = CHUNK_STATE_TRAILER_START;
					break;
				}
			}
			break;
			
		case CHUNK_STATE_END:
			/*
			 * We're not supposed to receive data after the chunk stream
			 * has been ended.  But if we do, it means either we
			 * misinterpreted the chunk end stream or the other end is just
			 * going berserk.
			 */

			error_str = "Remaining data after chunk end";
			goto error;
			
		case CHUNK_STATE_ERROR:
		case NUM_CHUNK_STATES:
			g_assert_not_reached();
			break;
		}

		/* NB: Some data from ``src'' must have been consumed or an
		 *     infinite loop may occur.
		 */

	} while (ctx->src_len > 0 && CHUNK_STATE_END != attr->state);

	if (p_error_str)
		*p_error_str = NULL;

	return 0;
	
error:
	
	if (p_error_str)
		*p_error_str = error_str;

	attr->state = CHUNK_STATE_ERROR;
	return -1;
}

/**
 * Dechunk more data from the input buffer `mb'.
 * @returns dechunked data in a new buffer, or NULL if no more data.
 */
static pmsg_t *
dechunk_data(rxdrv_t *rx, pmsg_t *mb)
{
	struct attr *attr = rx->opaque;
	pdata_t *db;					/**< Dechunked buffer */
	size_t old_size, old_avail;
	const gchar *error_str;
	struct dechunk ctx;

	/*
	 * Prepare call to dechunk().
	 */

	ctx.src = pmsg_read_base(mb);
	ctx.src_len = old_size = pmsg_size(mb);

	if (old_size == 0)
		return NULL;				/* No more data */

	db = rxbuf_new();

	ctx.dst = pdata_start(db);
	ctx.dst_len = old_avail = pdata_len(db);

	g_assert(ctx.dst_len > 0);
	g_assert(ctx.src_len > 0);

	/*
	 * Dechunk data.
	 */

	g_assert(attr->state != CHUNK_STATE_ERROR);

	if (0 != dechunk(rx, &ctx, &error_str)) {
		/*
		 * We can't continue if we meet a dechunking error.  Signal
		 * our user so that the connection is terminated.
		 */

		attr->cb->chunk_error(rx->owner, "dechunk() failed: %s", error_str);
		goto cleanup;
	}

	mb->m_rptr += old_size - ctx.src_len;		/* Read that far */

	/*
	 * Check whether some data was produced.
	 */

	if (ctx.dst_len != old_avail) {
		size_t n;

		/*
		 * Build message block with dechunked data.
		 */

		n = old_avail - ctx.dst_len;

		return pmsg_alloc(PMSG_P_DATA, db, 0, n);
	}

cleanup:
	rxbuf_free(db, NULL);
	return NULL;
}

/***
 *** Polymorphic routines.
 ***/

/**
 * Initialize the driver.
 */
static gpointer
rx_chunk_init(rxdrv_t *rx, gpointer args)
{
	struct attr *attr;
	struct rx_chunk_args *rargs = args;

	g_assert(rx);
	g_assert(rargs->cb != NULL);

	attr = walloc(sizeof *attr);

	attr->cb = rargs->cb;
	attr->flags = 0;
	attr->data_remain = 0;
	attr->hex_pos = 0;
	attr->state = CHUNK_STATE_SIZE;

	rx->opaque = attr;

	return rx;		/* OK */
}

/**
 * Get rid of the driver's private data.
 */
static void
rx_chunk_destroy(rxdrv_t *rx)
{
	struct attr *attr = rx->opaque;

	wfree(attr, sizeof *attr);
}

/**
 * Got data from lower layer.
 */
static void
rx_chunk_recv(rxdrv_t *rx, pmsg_t *mb)
{
	struct attr *attr = rx->opaque;
	pmsg_t *imb;		/**< Dechunked message */

	g_assert(rx);
	g_assert(mb);

	/*
	 * Dechunk the stream, forwarding dechunked data to the upper layer.
	 * At any time, a packet we forward can cause the reception to be
	 * disabled, in which case we must stop.
	 */

	while ((attr->flags & IF_ENABLED) && (imb = dechunk_data(rx, mb)))
		(*rx->data_ind)(rx, imb);

	pmsg_free(mb);
}

/**
 * Enable reception of data.
 */
static void
rx_chunk_enable(rxdrv_t *rx)
{
	struct attr *attr = (struct attr *) rx->opaque;

	attr->flags |= IF_ENABLED;
}

/**
 * Disable reception of data.
 */
static void
rx_chunk_disable(rxdrv_t *rx)
{
	struct attr *attr = rx->opaque;

	attr->flags &= ~IF_ENABLED;
}

static const struct rxdrv_ops rx_chunk_ops = {
	rx_chunk_init,		/**< init */
	rx_chunk_destroy,	/**< destroy */
	rx_chunk_recv,		/**< recv */
	rx_chunk_enable,	/**< enable */
	rx_chunk_disable,	/**< disable */
	rx_no_source,		/**< bio_source */
};

const struct rxdrv_ops *
rx_chunk_get_ops(void)
{
	return &rx_chunk_ops;
}

/* vi: set ts=4 sw=4 cindent: */
