/*
 *  This file is part of pom-ng.
 *  Copyright (C) 2010 Guy Martin <gmsoft@tuxicoman.be>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */


#ifndef __POM_NG_PACKET_H__
#define __POM_NG_PACKET_H__

#include <pthread.h>

struct packet {
	// Used by input buffer
	int inpkt_prev_offset, inpkt_next_offset;
	int buff_offset;

	// Packet description
	struct timeval ts;
	size_t len;
	unsigned char *buff; // Only valid when input_client updates it in get_packet
	struct packet_info_list *info_head, *info_tail;
};

struct packet_info {
	struct ptype **fields_value;
	struct packet_info *pool_next, *pool_prev;
};


#endif
