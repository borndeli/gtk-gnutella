/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi, Richard Eckart
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

#ifndef _uploads_gui_common_h_
#define _uploads_gui_common_h_

#include "config.h"

#include <glib.h> 
#include <time.h>           /* For time_t */
#include "uploads_gui.h"    /* For upload_row_data_t */

gfloat uploads_gui_progress(
	const gnet_upload_status_t *u, const upload_row_data_t *data);
const gchar *uploads_gui_status_str(
    const gnet_upload_status_t *u, const upload_row_data_t *data);
gboolean upload_should_remove(time_t now, const upload_row_data_t *ul);

#endif /* _uploads_gui_common_h_ */
