#include <stdio.h>
#include <thread.h>
#include <stdint.h>
#include <malloc.h>
#define DONT_MAKE_WRAPPER
#include <_malloc.h>
#undef DONT_MAKE_WRAPPER
#include <net/ether.h>
#include <net/ip.h>
#include <net/tcp.h>
#include <net/udp.h>
#include <util/list.h>

#include "spd.h"
#include "sp.h"
#include "rwlock.h"

bool spd_ginit() {
	int id = thread_id();
	if(id != 0)
		return false;

	int count = nic_count();

	for(int i = 0; i < count; i++) {
		NIC* nic = nic_get(i);
		SPD* spd = __malloc(sizeof(SPD), nic->pool);
		if(!spd) {
			printf("Can't create SPD\n");
			goto fail;
		}

		spd->out_database = list_create(nic->pool);
		if(!spd->out_database) {
			__free(spd, nic->pool);
			printf("Can't create SPD Outbound Database\n");
			goto fail;
		}
		rwlock_init(&spd->out_rwlock);

		spd->in_database = list_create(nic->pool);
		if(!spd->in_database) {
			list_destroy(spd->out_database);
			__free(spd, nic->pool);
			printf("Can't create SPD Outbound Database\n");
			goto fail;
		}
		rwlock_init(&spd->in_rwlock);

		if(!nic_config_put(nic, IPSEC_SPD, spd)) {
			list_destroy(spd->in_database);
			list_destroy(spd->out_database);
			__free(spd, nic->pool);
			printf("Can't add SPD Outbound\n");
			goto fail;
		}
	}

	return true;

fail:
	for(int i = 0; i < count; i++) {
		NIC* nic = nic_get(i);
		SPD* spd = nic_config_get(nic, IPSEC_SPD);
		if(!spd)
			continue;

		if(spd->out_database)
			list_destroy(spd->out_database);

		if(spd->in_database)
			list_destroy(spd->in_database);

		nic_config_remove(nic, IPSEC_SPD);

		__free(spd, nic->pool);
	}

	return false;
}

void spd_gdestroy() {
	int id = thread_id();
	if(id != 0)
		return;

	int count = nic_count();

	for(int i = 0; i < count; i++) {
		NIC* nic = nic_get(i);
		SPD* spd = nic_config_get(nic, IPSEC_SPD);
		if(!spd)
			continue;

		if(spd->out_database)
			list_destroy(spd->out_database);

		if(spd->in_database)
			list_destroy(spd->in_database);

		nic_config_remove(nic, IPSEC_SPD);

		__free(spd, nic->pool);
	}
}

SPD* spd_get(NIC* nic) {
	return nic_config_get(nic, IPSEC_SPD);
}

SP* spd_get_sp_index(NIC* nic, uint8_t direction, uint16_t index) {
	SPD* spd = nic_config_get(nic, IPSEC_SPD);
	SP* sp = NULL;
	switch(direction) {
		case DIRECTION_OUT:
			sp = list_get(spd->out_database, index);

			break;
		case DIRECTION_IN:
			sp = list_get(spd->in_database, index);

			break;
	}

	return sp;
}

SP* spd_get_sp(NIC* nic, uint8_t direction, IP* ip) {
	SPD* spd = nic_config_get(nic, IPSEC_SPD);
	List* database = NULL;
	switch(direction) {
		case DIRECTION_OUT:
			database = spd->out_database;
			break;
		case DIRECTION_IN:
			database = spd->in_database;
			break;
	}

	ListIterator iter;

	list_iterator_init(&iter, database);
	while(list_iterator_has_next(&iter)) {
		SP* sp = list_iterator_next(&iter);
		if(sp->protocol && (ip->protocol != sp->protocol))
			continue;

		if(sp->src_ip && ((endian32(ip->source) & sp->src_mask) != (sp->src_ip & sp->src_mask)))
			continue;

		if(sp->dest_ip && ((endian32(ip->destination) & sp->dest_mask) != (sp->dest_ip & sp->dest_mask)))
			continue;

		switch(ip->protocol) {
			case IP_PROTOCOL_TCP:
				;
				TCP* tcp = (TCP*)ip->body;
				if(sp->src_port && (endian16(tcp->source) != sp->src_port))
					continue;

				if(sp->dest_port && (endian16(tcp->destination) != sp->dest_port))
					continue;

				return sp;
			case IP_PROTOCOL_UDP:
				;
				UDP* udp = (UDP*)ip->body;
				if(sp->src_port && (endian16(udp->source) != sp->src_port))
					continue;

				if(sp->dest_port && (endian16(udp->destination) != sp->dest_port))
					continue;

				return sp;

			default:
				return sp;
		}
	}

	return NULL;
}

bool spd_add_sp(NIC* nic, uint8_t direction, SP* sp, int priority) {
	SPD* spd = nic_config_get(nic, IPSEC_SPD);
	List* database = NULL;
	switch(direction) {
		case DIRECTION_OUT:
			database = spd->out_database;
			break;
		case DIRECTION_IN:
			database = spd->in_database;
			break;
	}

	bool result = list_add_at(database, priority, sp);

	return result;
}

bool spd_remove_sp(NIC* nic, uint8_t direction, int index) {
	SPD* spd = nic_config_get(nic, IPSEC_SPD);
	List* database = NULL;
	switch(direction) {
		case DIRECTION_OUT:
			database = spd->out_database;
			break;
		case DIRECTION_IN:
			database = spd->in_database;
			break;
		default:
			return false;
	}

	SP* sp = list_remove(database, index);
	if(!sp)
		return false;
	
	return 	sp_free(sp);
}

void spd_delete_all(NIC* nic, uint8_t direction) {
	SPD* spd = nic_config_get(nic, IPSEC_SPD);
	List* database = NULL;
	switch(direction) {
		case DIRECTION_OUT:
			database = spd->out_database;
			break;
		case DIRECTION_IN:
			database = spd->in_database;
			break;
	}

	ListIterator iter;
	list_iterator_init(&iter, database);
	while((list_iterator_has_next(&iter))) {
		SP* sp = list_iterator_next(&iter);
		list_iterator_remove(&iter);
		sp_free(sp);
	}
}

/* SPD Read & Write Lock */
inline void spd_inbound_rlock(NIC* nic) {
	SPD* spd = nic_config_get(nic, IPSEC_SPD);

	rwlock_read_lock(&spd->in_rwlock);
}

inline void spd_inbound_un_rlock(NIC* nic) {
	SPD* spd = nic_config_get(nic, IPSEC_SPD);

	rwlock_read_unlock(&spd->in_rwlock);
}

inline void spd_inbound_wlock(NIC* nic) {
	SPD* spd = nic_config_get(nic, IPSEC_SPD);

	rwlock_write_lock(&spd->in_rwlock);
}

inline void spd_inbound_un_wlock(NIC* nic) {
	SPD* spd = nic_config_get(nic, IPSEC_SPD);

	rwlock_write_unlock(&spd->in_rwlock);
}

inline void spd_outbound_rlock(NIC* nic) {
	SPD* spd = nic_config_get(nic, IPSEC_SPD);

	rwlock_read_lock(&spd->out_rwlock);
}

inline void spd_outbound_un_rlock(NIC* nic) {
	SPD* spd = nic_config_get(nic, IPSEC_SPD);

	rwlock_read_unlock(&spd->out_rwlock);
}

inline void spd_outbound_wlock(NIC* nic) {
	SPD* spd = nic_config_get(nic, IPSEC_SPD);

	rwlock_write_lock(&spd->out_rwlock);
}

inline void spd_outbound_un_wlock(NIC* nic) {
	SPD* spd = nic_config_get(nic, IPSEC_SPD);

	rwlock_write_unlock(&spd->out_rwlock);
}
