#include <stdio.h>
#include <malloc.h>
#define DONT_MAKE_WRAPPER
#include <_malloc.h>
#undef DONT_MAKE_WRAPPER
#include <string.h>
#include <byteswap.h>
#include <netinet/in.h>

#include <net/ip.h>
#include <net/udp.h>
#include <net/tcp.h>

#include "spd.h"

extern void* __gmalloc_pool;
SPD* spd_create() {
	SPD* spd = __malloc(sizeof(SPD), __gmalloc_pool);
	if(!spd)
		return NULL;

	memset(spd, 0, sizeof(SPD));
	spd->list = list_create(__gmalloc_pool);
	if(!spd->list)
		goto fail;

	rwlock_init(&spd->rwlock);

	return spd;

fail:
	if(spd->list)
		list_destroy(spd->list);

	__free(spd, __gmalloc_pool);

	return NULL;
}

bool spd_delete(SPD* spd) {
	rwlock_wlock(&spd->rwlock);
	//TODO gabage collection;

	list_destroy(spd->list);

	__free(spd, __gmalloc_pool);

	return true;
}

void spd_flush(SPD* spd) {
	rwlock_wlock(&spd->rwlock);
	ListIterator iter;
	list_iterator_init(&iter, spd->list);
	while(list_iterator_has_next(&iter)) {
		SP* sp = list_iterator_next(&iter);
#ifdef DEBUG
		sp_dump(sp);
#endif
		sp_free(sp);
		list_iterator_remove(&iter);
	}
	rwlock_wunlock(&spd->rwlock);
}

bool spd_add_sp(SPD* spd, SP* sp) {
	bool compare(void* new_data, void* data) {
		SP* new_sp = new_data;
		SP* sp = data;
		if(new_sp->policy->sadb_x_policy_priority >= sp->policy->sadb_x_policy_priority) {
			return true;
		}
		return false;
	}

	rwlock_wlock(&spd->rwlock);
	int idx = list_index_of(spd->list, sp, compare);

#ifdef DEBUG
	sp_dump(sp);
#endif

	bool result =  list_add_at(spd->list, idx, sp);
	rwlock_wunlock(&spd->rwlock);

	return result;
}

SP* spd_remove_sp(SPD* spd, uint8_t policy, uint32_t src_address, uint32_t dst_address) {
	rwlock_wlock(&spd->rwlock);

	ListIterator iter;
	list_iterator_init(&iter, spd->list);
	while(list_iterator_has_next(&iter)) {
		SP* sp = list_iterator_next(&iter);
		if(sp->policy->sadb_x_policy_type != policy)
			continue;
		//TODO fix here check mask
		struct sockaddr_in* src_sockaddr = (struct sockaddr_in*)((uint8_t*)sp->address_src + sizeof(*sp->address_src));
		if(src_address != src_sockaddr->sin_addr.s_addr)
			continue;

		struct sockaddr_in* dst_sockaddr = (struct sockaddr_in*)((uint8_t*)sp->address_dst + sizeof(*sp->address_dst));
		if(dst_address != dst_sockaddr->sin_addr.s_addr)
			continue;

#ifdef DEBUG
		sp_dump(sp);
#endif
		list_iterator_remove(&iter);
		rwlock_wunlock(&spd->rwlock);
		return sp;
	}

	rwlock_wunlock(&spd->rwlock);
	return NULL;
}

SP* spd_get_sp(SPD* spd, uint8_t policy, IP* ip) {
	rwlock_rlock(&spd->rwlock);
	uint32_t src_address = bswap_32(ip->source);
	uint32_t dst_address = bswap_32(ip->destination);

	ListIterator iter;
	list_iterator_init(&iter, spd->list);
	while(list_iterator_has_next(&iter)) {
		SP* sp = list_iterator_next(&iter);
		if(sp->policy->sadb_x_policy_type != policy)
			continue;

		//Check Source
		//1. Check Address
		struct sockaddr_in* src_sockaddr = (struct sockaddr_in*)((uint8_t*)sp->address_src + sizeof(*sp->address_src));
		uint32_t src_address0 = src_sockaddr->sin_addr.s_addr;
		uint32_t src_mask = 0xffffffff;
		src_mask <<= (32 - sp->address_src->sadb_address_prefixlen);
		if((src_address & src_mask) != (bswap_32(src_address0) & src_mask))
			continue;

		//2. Check Protocol
		if(sp->address_src->sadb_address_proto != 255 && sp->address_src->sadb_address_proto != ip->protocol)
			continue;

		//3. Check Port: 0 = ANY
		if(src_sockaddr->sin_port) {
			uint16_t src_port;
			if(ip->protocol == IP_PROTOCOL_UDP) {
				UDP* udp = (UDP*)((uint8_t*)ip + ip->ihl * 4);
				src_port = udp->source;
			} else if(ip->protocol == IP_PROTOCOL_TCP) {
				TCP* tcp= (TCP*)((uint8_t*)ip + ip->ihl * 4);
				src_port = tcp->source;
			} else
				continue;

			if(src_sockaddr->sin_port != src_port)
				continue;
		}

		//Check Destination
		//1. Check Address
		struct sockaddr_in* dst_sockaddr = (struct sockaddr_in*)((uint8_t*)sp->address_dst + sizeof(*sp->address_dst));
		uint32_t dst_address0 = dst_sockaddr->sin_addr.s_addr;
		uint32_t dst_mask = 0xffffffff;
		dst_mask <<= (32 - sp->address_dst->sadb_address_prefixlen);
		if((dst_address & dst_mask) != (bswap_32(dst_address0) & dst_mask))
			continue;
		//2. Check Protocol: No need
		//3. Check Port: 0 = ANY
		if(dst_sockaddr->sin_port) {
			uint16_t dst_port;
			if(ip->protocol == IP_PROTOCOL_UDP) {
				UDP* udp = (UDP*)((uint8_t*)ip + ip->ihl * 4);
				dst_port = udp->destination;
			} else if(ip->protocol == IP_PROTOCOL_TCP) {
				TCP* tcp= (TCP*)((uint8_t*)ip + ip->ihl * 4);
				dst_port = tcp->destination;
			} else
				continue;

			if(dst_sockaddr->sin_port != dst_port)
				continue;
		}

#ifdef DEBUG
		sp_dump(sp);
#endif
		rwlock_runlock(&spd->rwlock);
		return sp;
	}
	rwlock_runlock(&spd->rwlock);
	return NULL;
}
