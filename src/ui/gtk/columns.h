/*
 * $Id$
 *
 * Copyright (c) 2001-2004, Raphael Manfredi & Richard Eckart
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

#ifndef _gtk_columns_h_
#define _gtk_columns_h_

/*
 * Gnet table columns.
 */

enum {
	c_gnet_host = 0,
	c_gnet_flags,
	c_gnet_user_agent,
	c_gnet_version,
	c_gnet_connected,
	c_gnet_uptime,
	c_gnet_info,
#define NODES_VISIBLE_COLUMNS ((guint) c_gnet_info + 1)
#ifdef USE_GTK2
	c_gnet_handle,
	c_gnet_fg,
#endif
	c_gnet_num
};

/*
 * Uploads table columns
 */
enum {
    c_ul_filename = 0,
    c_ul_host,
    c_ul_size,
    c_ul_range,
    c_ul_agent,
#ifdef USE_GTK2
	c_ul_progress,
#endif /* USE_GTK2 */	
    c_ul_status,
#ifdef USE_GTK2
#define UPLOADS_GUI_VISIBLE_COLUMNS ((guint) c_ul_status + 1)
    c_ul_fg,
    c_ul_data,
#endif /* USE_GTK2 */

	c_ul_num
};

/*
 * Upload stats columns
 */
enum {
    c_us_filename = 0,
    c_us_size,
    c_us_attempts,
    c_us_complete,
    c_us_norm,
#define UPLOAD_STATS_GUI_VISIBLE_COLUMNS ((guint) c_us_norm + 1)
	c_us_stat,

	c_us_num
};

/*
 * Downloads table columns
 */

#ifdef USE_GTK1
enum {
    c_dl_filename = 0,
    c_dl_host,
    c_dl_size,
    c_dl_range,
    c_dl_server,
    c_dl_status,
	c_dl_num
};
#endif

#ifdef USE_GTK2
enum {
    c_dl_filename = 0,
    c_dl_size,
    c_dl_host,
    c_dl_range,
    c_dl_server,
    c_dl_progress,
    c_dl_status,
#define DOWNLOADS_VISIBLE_COLUMNS ((guint) c_dl_status + 1)
    c_dl_fg, /* invisible, holds the foreground color for the row */
    c_dl_bg, /* invisible, holds the background color for the row */
    c_dl_record, /* invisible, pointer to the record_t of this entry */
    c_dl_num
};
#endif

/*
 * Queue table columns
 */

#ifdef USE_GTK1
enum {
    c_queue_filename = 0,
    c_queue_host,
    c_queue_size,
    c_queue_server,
    c_queue_status,
	c_queue_num
};
#endif

#ifdef USE_GTK2
enum {
    c_queue_filename = 0,
    c_queue_size,
    c_queue_host,
    c_queue_server,
    c_queue_status,
#define DOWNLOAD_QUEUE_VISIBLE_COLUMNS ((guint) c_queue_status + 1)
	c_queue_fg, /* invisible, holds the foreground color for the row */
	c_queue_bg, /* invisible, holds the background color for the row */
	c_queue_record, /* invisible, pointer to the record_t of this entry */
	c_queue_num
};
#endif


/*
 * Fileinfo table columns.
 */

enum {
	c_fi_filename = 0,
	c_fi_size,
	c_fi_done,
	c_fi_sources,
	c_fi_status,
#ifdef USE_GTK2
#define FILEINFO_VISIBLE_COLUMNS ((guint) c_fi_status + 1)
	c_fi_handle,
	c_fi_isize,
	c_fi_idone,
	c_fi_isources,
#endif
	c_fi_num
};

/*
 * Searches table columns
 */
enum {
    c_sr_filename = 0,
	c_sr_ext,
    c_sr_size,
	c_sr_count,
#ifdef USE_GTK1
    c_sr_speed,
    c_sr_host,
    c_sr_sha1,	
#endif
    c_sr_info,
#ifdef USE_GTK2
#define SEARCH_RESULTS_VISIBLE_COLUMNS ((guint) c_sr_info + 1)

	c_sr_fg, /* invisible, holds the foreground color for the row */
	c_sr_bg, /* invisible, holds the background color for the row */
	c_sr_record, /* invisible, pointer to the record_t of this entry */
#endif
	c_sr_num
};

/*
 * Gnet stats table columns
 */
typedef enum {
    c_gs_type = 0,
    c_gs_received,
    c_gs_expired,
    c_gs_dropped,
    c_gs_relayed,
    c_gs_generated,

	num_c_gs
} c_gs_t;

typedef enum {
    c_horizon_hops = 0,
    c_horizon_nodes,
    c_horizon_files,
    c_horizon_size,

    num_c_horizon
} c_horizon_t;

/*
 * Hostcache stats table columns
 */
enum {
    c_hcs_name = 0,
    c_hcs_host_count,
    c_hcs_hits,
    c_hcs_misses
#define HCACHE_STATS_VISIBLE_COLUMNS ((guint) c_hcs_misses + 1)
};


/*
 * Searches overview table columns
 */
enum {
    c_sl_name = 0,
    c_sl_hit,
    c_sl_new,
#define SEARCH_LIST_VISIBLE_COLUMNS ((guint) c_sl_new + 1)
#ifdef USE_GTK2
    c_sl_fg,
    c_sl_bg,
	c_sl_sch, /* invisible, pointer to the search_t for this entry */
#endif
	c_sl_num
};

/*
 * Search stats table columns
 */
enum {
    c_st_term = 0,
    c_st_period,
    c_st_total
};

#endif /* _gtk_columns_h_ */

/* vi: set ts=4: */
