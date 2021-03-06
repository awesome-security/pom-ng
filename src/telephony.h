/*
 *  This file is part of pom-ng.
 *  Copyright (C) 2014-2015 Guy Martin <gmsoft@tuxicoman.be>
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


#ifndef __TELEPHONY_H__
#define __TELEPHONY_H__


#include <pom-ng/telephony.h>
#include <pom-ng/mime.h>

#define TELEPHONY_SDP_MAX_LINE_LEN	512
#define TELEPHONY_EXPECTATION_TIMEOUT	60

struct telephony_codec_reg {

	enum telephony_codec_type type;
	char *name;
	int default_rtp_pload_type;
	char *pom_pload_type;

};

// Call definitions

struct telephony_call {

	struct telephony_sdp_dialog *dialogs;
	struct proto *sess_proto;
	char *call_id;
};

struct telephony_sdp_dialog {

	struct telephony_call *call;
	struct telephony_stream *streams;
	struct telephony_stream_conntrack *sc;

	pthread_rwlock_t lock;

	struct telephony_sdp_dialog *prev, *next;
};

// Media stream definitions

enum telephony_stream_type {
	telephony_stream_type_unknown = 0,
	telephony_stream_type_rtp_avp,
	telephony_stream_type_rtp_savp,
};

enum telephony_stream_direction {
	telephony_stream_direction_unknown = 0,
	telephony_stream_direction_inactive,
	telephony_stream_direction_sendonly,
	telephony_stream_direction_recvonly,
	telephony_stream_direction_sendrecv,
};

struct telephony_stream {

	enum telephony_stream_type type;
	struct telephony_stream_address *addrs;
	struct telephony_stream_payload *ploads;
	enum telephony_stream_direction dir;
	enum telephony_codec_type pload_type;
	struct proto *l4proto;

	struct proto_expectation *expt;

	struct telephony_stream *prev, *next;

	uint16_t port, port_num;

};

struct telephony_stream_conntrack {

	struct telephony_sdp_dialog *dialog;
	struct proto_expectation *expt;
	struct conntrack_entry *ce;
	struct telephony_stream_conntrack *prev, *next;

};

struct telephony_stream_address {

	struct proto *proto;
	struct ptype *addr;
	struct telephony_stream_address *next;
};

struct telephony_stream_payload {

	struct telephony_codec_reg *codec;
	struct telephony_stream_payload *next;
	unsigned int clock_rate;
	uint8_t pload_type, chan_num;
};

// SDP definitions

enum telephony_sdp_attrib_type {
	telephony_sdp_attrib_direction,
	telephony_sdp_attrib_rtpmap,
};

struct telephony_sdp_attrib_rtpmap {
	struct telephony_codec_reg *codec;
	uint8_t pload_type, chan_num;
};

struct telephony_sdp_attrib {

	enum telephony_sdp_attrib_type type;
	union {
		enum telephony_stream_direction direction;
		struct telephony_sdp_attrib_rtpmap rtpmap;
	};

	struct telephony_sdp_attrib *next;
};

struct telephony_sdp {

	struct packet_stream_parser *parser;
	struct telephony_stream_address *addr;

	struct telephony_sdp_attrib *attribs;

	struct telephony_stream *streams;

	struct telephony_sdp_dialog *dialog;

	ptime ts;

};


int telephony_init();
int telephony_cleanup();
void telephony_sdp_expectation_callback(struct proto_expectation *e, void *priv, struct conntrack_entry *ce);
int telephony_cleanup_stream_conntrack(void *obj, void *priv);
struct telephony_stream *telephony_stream_find(struct telephony_stream *streams, struct ptype *addr, struct proto *l4proto, uint16_t port);

#endif
