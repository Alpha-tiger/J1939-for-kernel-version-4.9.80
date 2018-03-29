// SPDX-License-Identifier: GPL-2.0
/* j1939-priv.h
 *
 * Copyright (c) 2010-2011 EIA Electronics
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef _J1939_PRIV_H_
#define _J1939_PRIV_H_

#include <linux/atomic.h>
#include <linux/if_arp.h>
#include <linux/interrupt.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/can/can-ml.h>
#include <linux/can/j1939.h>

#include <net/sock.h>

#include "../af_can.h"

/* TODO: return ENETRESET on busoff. */

#define J1939_PGN_REQUEST 0x0ea00
#define J1939_PGN_ADDRESS_CLAIMED 0x0ee00
#define J1939_PGN_MAX 0x3ffff

/* j1939 devices */
struct j1939_ecu {
	struct list_head list;
	name_t name;
	u8 addr;

	/* indicates that this ecu successfully claimed @sa as its address */
	struct hrtimer ac_timer;
	struct kref kref;
	struct j1939_priv *priv;

	/* count users, to help transport protocol decide for interaction */
	int nusers;
};

struct j1939_priv {
	struct list_head ecus;
	/* local list entry in priv
	 * These allow irq (& softirq) context lookups on j1939 devices
	 * This approach (separate lists) is done as the other 2 alternatives
	 * are not easier or even wrong
	 * 1) using the pure kobject methods involves mutexes, which are not
	 *    allowed in irq context.
	 * 2) duplicating data structures would require a lot of synchronization
	 *    code
	 * usage:
	 */

	/* segments need a lock to protect the above list */
	rwlock_t lock;

	struct net_device *ndev;

	/* list of 256 ecu ptrs, that cache the claimed addresses.
	 * also protected by the above lock
	 */
	struct j1939_addr_ent {
		struct j1939_ecu *ecu;
		/* count users, to help transport protocol */
		int nusers;
	} ents[256];

	struct kref kref;
};

void j1939_ecu_put(struct j1939_ecu *ecu);

/* keep the cache of what is local */
int j1939_local_ecu_get(struct j1939_priv *priv, name_t name, u8 sa);
void j1939_local_ecu_put(struct j1939_priv *priv, name_t name, u8 sa);

static inline bool j1939_address_is_unicast(u8 addr)
{
	return addr <= J1939_MAX_UNICAST_ADDR;
}

static inline bool j1939_address_is_idle(u8 addr)
{
	return addr == J1939_IDLE_ADDR;
}

static inline bool j1939_address_is_valid(u8 addr)
{
	return addr != J1939_NO_ADDR;
}

static inline bool j1939_pgn_is_pdu1(pgn_t pgn)
{
	/* ignore dp & res bits for this */
	return (pgn & 0xff00) < 0xf000;
}

/* utility to correctly unmap an ECU */
void j1939_ecu_unmap_locked(struct j1939_ecu *ecu);
void j1939_ecu_unmap(struct j1939_ecu *ecu);

u8 j1939_name_to_addr(struct j1939_priv *priv, name_t name);
struct j1939_ecu *j1939_ecu_find_by_addr_locked(struct j1939_priv *priv, u8 addr);
struct j1939_ecu *j1939_ecu_get_by_addr(struct j1939_priv *priv, u8 addr);
struct j1939_ecu *j1939_ecu_get_by_addr_locked(struct j1939_priv *priv, u8 addr);
struct j1939_ecu *j1939_ecu_get_by_name(struct j1939_priv *priv, name_t name);
struct j1939_ecu *j1939_ecu_get_by_name_locked(struct j1939_priv *priv, name_t name);

struct j1939_addr {
	name_t src_name;
	name_t dst_name;
	pgn_t pgn;

	u8 sa;
	u8 da;
};

/* control buffer of the sk_buff */
struct j1939_sk_buff_cb {
	struct j1939_addr addr;
	priority_t priority;

	/* Flags for quick lookups during skb processing
	 * These are set in the receive path only
	 */
#define J1939_ECU_LOCAL	BIT(0)
	u32 src_flags;
	u32 dst_flags;


	/*   Flags for modifying the transport protocol*/
	int tpflags;

#define BAM_NODELAY 1
	/* for tx, MSG_SYN will be used to sync on sockets */
	u32 msg_flags;

	/* j1939 clones incoming skb's.
	 * insock saves the incoming skb->sk
	 * to determine local generated packets
	 */
	struct sock *insock;
};

static inline struct j1939_sk_buff_cb *j1939_skb_to_cb(struct sk_buff *skb)
{
	BUILD_BUG_ON(sizeof(struct j1939_sk_buff_cb) > sizeof(skb->cb));

	return (struct j1939_sk_buff_cb *)skb->cb;
}

int j1939_send(struct net *net, struct sk_buff *skb);
void j1939_sk_recv(struct sk_buff *skb);

//Check if we want to disable the normal BAM 50 ms delay
//Return 0 if we want to disable the delay
//Return 1 if we want to keep the delay
static inline int j1939cb_use_bamdelay(const struct j1939_sk_buff_cb *skcb)
{
	//printk(KERN_ALERT "DEBUG: Passed %s %d \n",__FUNCTION__,__LINE__);
	//printk(KERN_ALERT "DEBUG: skcb->tpflags state: %d\n",skcb->tpflags);

	if(skcb->tpflags & BAM_NODELAY)
	{
		return 0;
	}

	return 1;
}

/* stack entries */
int j1939_tp_send(struct net *net, struct j1939_priv *priv, struct sk_buff *skb);
int j1939_tp_recv(struct net *net, struct sk_buff *skb);
int j1939_ac_fixup(struct j1939_priv *priv, struct sk_buff *skb);
void j1939_ac_recv(struct j1939_priv *priv, struct sk_buff *skb);

/* network management */
struct j1939_ecu *j1939_ecu_create_locked(struct j1939_priv *priv, name_t name);
struct j1939_ecu *j1939_ecu_find_by_name_locked(struct j1939_priv *priv,
						name_t name);

/* unregister must be called with lock held */
void j1939_ecu_unregister_locked(struct j1939_ecu *ecu);

int j1939_netdev_start(struct net *net, struct net_device *ndev);
void j1939_netdev_stop(struct net_device *ndev);

struct j1939_priv *j1939_priv_get(struct net_device *ndev);
void j1939_priv_put(struct j1939_priv *priv);

/* notify/alert all j1939 sockets bound to ifindex */
void j1939_sk_netdev_event(struct net_device *ndev, int error_code);
int j1939_tp_rmdev_notifier(struct net_device *ndev);

/* decrement pending skb for a j1939 socket */
void j1939_sock_pending_del(struct sock *sk);

/* separate module-init/modules-exit's */
__init int j1939_tp_module_init(void);

void j1939_tp_module_exit(void);

/* CAN protocol */
extern const struct can_proto j1939_can_proto;

#endif /* _J1939_PRIV_H_ */
