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

#include <pom-ng/ptype.h>
#include <pom-ng/proto.h>
#include <pom-ng/ptype_ipv4.h>
#include <pom-ng/ptype_uint8.h>
#include <pom-ng/ptype_uint32.h>

#include "proto_ipv4.h"

#include <string.h>
#include <arpa/inet.h>

#define IP_DONT_FRAG 0x4000
#define IP_MORE_FRAG 0x2000
#define IP_OFFSET_MASK 0x1fff

static struct proto_dependency *proto_icmp = NULL, *proto_tcp = NULL, *proto_udp = NULL, *proto_ipv6 = NULL, *proto_gre = NULL;

static struct ptype *ptype_uint8 = NULL, *ptype_ipv4 = NULL;

static struct ptype *param_frag_timeout = NULL, *param_conntrack_timeout = NULL;

struct mod_reg_info* proto_ipv4_reg_info() {
	static struct mod_reg_info reg_info;
	memset(&reg_info, 0, sizeof(struct mod_reg_info));
	reg_info.api_ver = MOD_API_VER;
	reg_info.register_func = proto_ipv4_mod_register;
	reg_info.unregister_func = proto_ipv4_mod_unregister;

	return &reg_info;
}


static int proto_ipv4_mod_register(struct mod_reg *mod) {

	ptype_uint8 = ptype_alloc("uint8");
	ptype_ipv4 = ptype_alloc("ipv4");

	if (!ptype_uint8 || !ptype_ipv4) {
		if (ptype_uint8) {
			ptype_cleanup(ptype_uint8);
			ptype_uint8 = NULL;
		}
		if (ptype_ipv4) {
			ptype_cleanup(ptype_ipv4);
			ptype_ipv4 = NULL;
		}
		return POM_ERR;
	}

	static struct proto_pkt_field fields[PROTO_IPV4_FIELD_NUM + 1];
	memset(fields, 0, sizeof(struct proto_pkt_field) * (PROTO_IPV4_FIELD_NUM + 1));
	fields[0].name = "src";
	fields[0].value_template = ptype_ipv4;
	fields[0].description = "Source address";
	fields[1].name = "dst";
	fields[1].value_template = ptype_ipv4;
	fields[1].description = "Destination address";
	fields[2].name = "tos";
	fields[2].value_template = ptype_uint8;
	fields[2].description = "Type of service";
	fields[3].name = "ttl";
	fields[3].value_template = ptype_uint8;
	fields[3].description = "Time to live";

	static struct proto_reg_info proto_ipv4;
	memset(&proto_ipv4, 0, sizeof(struct proto_reg_info));
	proto_ipv4.name = "ipv4";
	proto_ipv4.api_ver = PROTO_API_VER;
	proto_ipv4.mod = mod;

	proto_ipv4.pkt_fields = fields;
	proto_ipv4.ct_info.default_table_size = 20000;
	proto_ipv4.ct_info.fwd_pkt_field_id = proto_ipv4_field_src;
	proto_ipv4.ct_info.rev_pkt_field_id = proto_ipv4_field_dst;
	proto_ipv4.ct_info.cleanup_handler = proto_ipv4_conntrack_cleanup;
	
	proto_ipv4.init = proto_ipv4_init;
	proto_ipv4.parse = proto_ipv4_parse;
	proto_ipv4.process = proto_ipv4_process;
	proto_ipv4.cleanup = proto_ipv4_cleanup;

	if (proto_register(&proto_ipv4) == POM_OK)
		return POM_OK;

	return POM_ERR;
}


static int proto_ipv4_init(struct registry_instance *i) {

	param_frag_timeout = ptype_alloc_unit("uint32", "seconds");
	if (!param_frag_timeout)
		return POM_ERR;

	param_conntrack_timeout = ptype_alloc_unit("uint32", "seconds");
	if (!param_conntrack_timeout)
		return POM_ERR;

	struct registry_param *p = registry_new_param("fragment_timeout", "60", param_frag_timeout, "Timeout for incomplete ipv4 fragments", 0);
	if (registry_instance_add_param(i, p) != POM_OK)
		goto err;

	p = registry_new_param("conntrack_timeout", "7200", param_conntrack_timeout, "Timeout for ipv4 connections", 0);
	if (registry_instance_add_param(i, p) != POM_OK)
		goto err;

	proto_icmp = proto_add_dependency("icmp");
	proto_tcp = proto_add_dependency("tcp");
	proto_udp = proto_add_dependency("udp");
	proto_ipv6 = proto_add_dependency("ipv6");
	proto_gre = proto_add_dependency("gre");

	if (!proto_icmp || !proto_tcp || !proto_udp || !proto_ipv6 || !proto_gre) {
		proto_ipv4_cleanup();
		return POM_ERR;
	}

	return POM_OK;

err:
	if (param_frag_timeout) {
		ptype_cleanup(param_frag_timeout);
		param_frag_timeout = NULL;
	}
	if (param_conntrack_timeout) {
		ptype_cleanup(param_conntrack_timeout);
		param_conntrack_timeout = NULL;
	}
	return POM_ERR;
}

static ssize_t proto_ipv4_parse(struct packet *p, struct proto_process_stack *stack, unsigned int stack_index) {


	struct proto_process_stack *s = &stack[stack_index];
	struct proto_process_stack *s_next = &stack[stack_index + 1];

	struct in_addr saddr, daddr;
	struct ip* hdr = s->pload;
	saddr.s_addr = hdr->ip_src.s_addr;
	daddr.s_addr = hdr->ip_dst.s_addr;

	unsigned int hdr_len = hdr->ip_hl * 4;

	if (s->plen < sizeof(struct ip) || // lenght smaller than header
		hdr->ip_hl < 5 || // ip header < 5 bytes
		ntohs(hdr->ip_len) < hdr_len || // datagram size < ip header length
		ntohs(hdr->ip_len) > s->plen) { // datagram size > given size
		return PROTO_INVALID;
	}


	PTYPE_IPV4_SETADDR(s->pkt_info->fields_value[proto_ipv4_field_src], hdr->ip_src);
	PTYPE_IPV4_SETADDR(s->pkt_info->fields_value[proto_ipv4_field_dst], hdr->ip_dst);
	PTYPE_UINT8_SETVAL(s->pkt_info->fields_value[proto_ipv4_field_tos], hdr->ip_tos);
	PTYPE_UINT8_SETVAL(s->pkt_info->fields_value[proto_ipv4_field_ttl], hdr->ip_ttl);

	// Handle conntrack stuff
	s->ct_field_fwd = s->pkt_info->fields_value[proto_ipv4_field_src];
	s->ct_field_rev = s->pkt_info->fields_value[proto_ipv4_field_dst];

	switch (hdr->ip_p) {
		case IPPROTO_ICMP: // 1
			s_next->proto = proto_icmp->proto;
			break;
		case IPPROTO_TCP: // 6
			s_next->proto = proto_tcp->proto;
			break;
		case IPPROTO_UDP: // 17
			s_next->proto = proto_udp->proto;
			break;
		case IPPROTO_IPV6: // 41
			s_next->proto = proto_ipv6->proto;
			break;
		case IPPROTO_GRE: // 47
			s_next->proto = proto_gre->proto;
			break;

		default:
			s_next->proto = NULL;
			break;

	}

	return hdr_len;

}


static int proto_ipv4_process(struct packet *p, struct proto_process_stack *stack, unsigned int stack_index, int hdr_len) {

	struct proto_process_stack *s = &stack[stack_index];

	if (!s->ce)
		return PROTO_ERR;

	struct ip* hdr = s->pload;

	uint16_t frag_off = ntohs(hdr->ip_off);

	// Check if packet is fragmented and need more handling

	if (frag_off & IP_DONT_FRAG)
		return PROTO_OK; // Nothing to do

	if (!(frag_off & IP_MORE_FRAG) && !(frag_off & IP_OFFSET_MASK))
		return PROTO_OK; // Nothing to do, full packet

	uint16_t offset = (frag_off & IP_OFFSET_MASK) << 3;
	size_t frag_size = ntohs(hdr->ip_len) - (hdr->ip_hl * 4);

	// Ignore invalid fragments
	if (frag_size > 0xFFFF) 
		return PROTO_INVALID;

	if (frag_size > s->plen + hdr_len)
		return PROTO_INVALID;


	pom_mutex_lock(&s->ce->lock);

	struct proto_ipv4_fragment *tmp = s->ce->priv;

	// Let's find the right buffer
	for (; tmp && tmp->id != hdr->ip_id; tmp = tmp->next);

	if (!tmp) {
		// Buffer not found, create it
		tmp = malloc(sizeof(struct proto_ipv4_fragment));
		if (!tmp) {
			pom_oom(sizeof(struct proto_ipv4_fragment));
			pom_mutex_unlock(&s->ce->lock);
			return PROTO_ERR;
		}
		memset(tmp, 0, sizeof(struct proto_ipv4_fragment));

		tmp->t = timer_alloc(tmp, proto_ipv4_fragment_cleanup);
		if (!tmp->t) {
			pom_mutex_unlock(&s->ce->lock);
			free(tmp);
			return PROTO_ERR;
		}
		
		tmp->ce = s->ce;
		tmp->id = hdr->ip_id;

		struct proto_dependency *proto = NULL;
		switch (hdr->ip_p) {
			case IPPROTO_ICMP: // 1
				proto = proto_icmp;
				break;
			case IPPROTO_TCP: // 6
				proto = proto_tcp;
				break;
			case IPPROTO_UDP: // 17
				proto = proto_udp;
				break;
			case IPPROTO_IPV6: // 41
				proto = proto_ipv6;
				break;
			case IPPROTO_GRE: // 47
				proto = proto_gre;
				break;
		}

		if (!proto || !proto->proto) {
			// Set processed flag so no attempt to process this will be done
			tmp->flags |= PROTO_IPV4_FLAG_PROCESSED;
			pom_mutex_unlock(&s->ce->lock);
			timer_cleanup(tmp->t);
			free(tmp);
			return PROTO_STOP;
		}

		tmp->multipart = packet_multipart_alloc(proto, 0);
		if (!tmp->multipart) {
			pom_mutex_unlock(&s->ce->lock);
			timer_cleanup(tmp->t);
			free(tmp);
			return PROTO_ERR;
		}

		tmp->next = s->ce->priv;
		if (tmp->next)
			tmp->next->prev = tmp;
		s->ce->priv = tmp;
	} else {
		timer_dequeue(tmp->t);
	}

	// Fragment was already handled
	if (tmp->flags & PROTO_IPV4_FLAG_PROCESSED) {
		pom_mutex_unlock(&s->ce->lock);
		return PROTO_STOP;
	}
	
	// Add the fragment
	if (packet_multipart_add_packet(tmp->multipart, p, offset, frag_size, (s->pload - (void*)p->buff) + (hdr->ip_hl * 4)) != POM_OK) {
		pom_mutex_unlock(&s->ce->lock);
		packet_multipart_cleanup(tmp->multipart);
		timer_cleanup(tmp->t);
		free(tmp);
		return PROTO_ERR;
	}

	// Schedule the timeout for the fragment
	uint32_t *frag_timeout; PTYPE_UINT32_GETVAL(param_frag_timeout, frag_timeout);
	timer_queue(tmp->t, *frag_timeout);


	if (!(frag_off & IP_MORE_FRAG))
		tmp->flags |= PROTO_IPV4_FLAG_GOT_LAST;

	if ((tmp->flags & PROTO_IPV4_FLAG_GOT_LAST) && !tmp->multipart->gaps)
		tmp->flags |= PROTO_IPV4_FLAG_PROCESSED;

	// We can process the packet unlocked
	pom_mutex_unlock(&s->ce->lock);

	if ((tmp->flags & PROTO_IPV4_FLAG_PROCESSED)) {
		int res = packet_multipart_process(tmp->multipart, stack, stack_index + 1);
		tmp->multipart = NULL; // Multipart will be cleared automatically
		if (res == PROTO_ERR)
			return PROTO_ERR;
	}
	
	

	return PROTO_STOP; // Stop processing the packet

}

static int proto_ipv4_fragment_cleanup(void *priv) {

	struct proto_ipv4_fragment *f = priv;

	// Remove the frag from the conntrack
	pom_mutex_lock(&f->ce->lock);
	if (f->prev)
		f->prev->next = f->next;
	else
		f->ce->priv = f->next;

	if (f->next)
		f->next->prev = f->prev;

	pom_mutex_unlock(&f->ce->lock);

	if (!(f->flags & PROTO_IPV4_FLAG_PROCESSED))
		pomlog(POMLOG_DEBUG "Cleaning up unprocessed fragment");

	if (f->multipart)
		packet_multipart_cleanup(f->multipart);
	
	if (f->t)
		timer_cleanup(f->t);
	
	free(f);

	return POM_OK;

}

static int proto_ipv4_conntrack_cleanup(struct conntrack_entry *ce) {

	while (ce->priv) {
		struct proto_ipv4_fragment *f = ce->priv;
		ce->priv = f->next;

		if (!(f->flags & PROTO_IPV4_FLAG_PROCESSED))
			pomlog(POMLOG_DEBUG "Cleaning up unprocessed fragment");

		if (f->multipart)
			packet_multipart_cleanup(f->multipart);
		
		if (f->t)
			timer_cleanup(f->t);
		
		free(f);

	}

	return POM_OK;
}

static int proto_ipv4_cleanup() {

	int res = POM_OK;

	res += ptype_cleanup(param_frag_timeout);
	res += ptype_cleanup(param_conntrack_timeout);

	res += proto_remove_dependency(proto_icmp);
	res += proto_remove_dependency(proto_udp);
	res += proto_remove_dependency(proto_tcp);
	res += proto_remove_dependency(proto_ipv6);
	res += proto_remove_dependency(proto_gre);

	return res;
}

static int proto_ipv4_mod_unregister() {

	int res = proto_unregister("ipv4");

	ptype_cleanup(ptype_uint8);
	ptype_uint8 = NULL;
	ptype_cleanup(ptype_ipv4);
	ptype_ipv4 = NULL;


	return res;
}
