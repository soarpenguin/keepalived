/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        NETLINK IPv4 routes manipulation.
 *
 * Author:      Alexandre Cassen, <acassen@linux-vs.org>
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Copyright (C) 2001-2012 Alexandre Cassen, <acassen@gmail.com>
 */

/* local include */
#include "vrrp_ipaddress.h"
#include "vrrp_iproute.h"
#include "vrrp_netlink.h"
#include "vrrp_if.h"
#include "vrrp_data.h"
#include "logger.h"
#include "memory.h"
#include "utils.h"
#include "rttables.h"
#include "vrrp_ip_rule_route_parser.h"

#include <linux/icmpv6.h>
#include <inttypes.h>
#ifdef _HAVE_RTA_ENCAP_
#include <linux/lwtunnel.h>
#include <linux/mpls_iptunnel.h>
#include <linux/ila.h>
#endif

/* Buffer sizes for netlink messages. Increase if needed. */
#define	RTM_SIZE		1024
#define	RTA_SIZE		1024
#define	ENCAP_RTA_SIZE		 128
#define NEXTHOP_RTA_SIZE	1024

/* Utility functions */
static int
add_addr2req(struct nlmsghdr *n, int maxlen, int type, ip_address_t *ip_address)
{
	void *addr;
	int alen;

	if (!ip_address)
		return -1;

	if (IP_IS6(ip_address)) {
		addr = (void *) &ip_address->u.sin6_addr;
		alen = sizeof(ip_address->u.sin6_addr);
	}
	else
	{
	     addr = (void *) &ip_address->u.sin.sin_addr;
	     alen = sizeof(ip_address->u.sin.sin_addr);
	}

	return addattr_l(n, maxlen, type, addr, alen);
}

#ifdef _HAVE_RTA_VIA_
static int
add_addr_fam2req(struct nlmsghdr *n, int maxlen, int type, ip_address_t *ip_address)
{
	void *addr;
	int alen;
	uint16_t family;

	if (!ip_address)
		return -1;

	if (IP_IS6(ip_address)) {
		addr = (void *)&ip_address->u.sin6_addr;
		alen = sizeof(ip_address->u.sin6_addr);
	}
	else {
		addr = (void *)&ip_address->u.sin.sin_addr;
		alen = sizeof(ip_address->u.sin.sin_addr);
	}
	family = ip_address->ifa.ifa_family;

	return addattr_l2(n, maxlen, type, &family, sizeof(family), addr, alen);
}
#endif

static int
add_addr2rta(struct rtattr *rta, int maxlen, int type, ip_address_t *ip_address)
{
	void *addr;
	int alen;

	if (!ip_address)
		return -1;

	if (IP_IS6(ip_address)) {
		addr = (void *)&ip_address->u.sin6_addr;
		alen = sizeof(ip_address->u.sin6_addr);
	}
	else {
		addr = (void *)&ip_address->u.sin.sin_addr;
		alen = sizeof(ip_address->u.sin.sin_addr);
	}

	return rta_addattr_l(rta, maxlen, type, addr, alen);
}

#ifdef _HAVE_RTA_VIA_
static int
add_addrfam2rta(struct rtattr *rta, int maxlen, int type, ip_address_t *ip_address)
{
	void *addr;
	int alen;
	uint16_t family;

	if (!ip_address)
		return -1;

	if (IP_IS6(ip_address)) {
		addr = (void *)&ip_address->u.sin6_addr;
		alen = sizeof(ip_address->u.sin6_addr);
	}
	else {
		addr = (void *)&ip_address->u.sin.sin_addr;
		alen = sizeof(ip_address->u.sin.sin_addr);
	}
	family = ip_address->ifa.ifa_family;

	return rta_addattr_l2(rta, maxlen, type, &family, sizeof(family), addr, alen);
}
#endif

#ifdef _HAVE_RTA_ENCAP_
static void
add_encap_mpls(struct rtattr *rta, size_t len, const encap_t *encap)
{
	rta_addattr_l(rta, len, MPLS_IPTUNNEL_DST, &encap->mpls.addr, encap->mpls.num_labels * sizeof(encap->mpls.addr[0]));
}

static void
add_encap_ip(struct rtattr *rta, size_t len, const encap_t *encap)
{
	if (encap->flags & IPROUTE_BIT_ENCAP_ID)
		rta_addattr64(rta, len, LWTUNNEL_IP_ID, htonll(encap->ip.id));
	if (encap->ip.dst)
		rta_addattr_l(rta, len, LWTUNNEL_IP_DST, &encap->ip.dst->u.sin.sin_addr.s_addr, sizeof(encap->ip.dst->u.sin.sin_addr.s_addr));
	if (encap->ip.src)
		rta_addattr_l(rta, len, LWTUNNEL_IP_SRC, &encap->ip.src->u.sin.sin_addr.s_addr, sizeof(encap->ip.src->u.sin.sin_addr.s_addr));
	if (encap->flags & IPROUTE_BIT_ENCAP_DSFIELD)
		rta_addattr8(rta, len, LWTUNNEL_IP_TOS, encap->ip.tos);
	if (encap->flags & IPROUTE_BIT_ENCAP_HOPLIMIT)
		rta_addattr8(rta, len, LWTUNNEL_IP_TTL, encap->ip.ttl);
	if (encap->flags & IPROUTE_BIT_ENCAP_FLAGS)
		rta_addattr16(rta, len, LWTUNNEL_IP_FLAGS, encap->ip.flags);
}

static void
add_encap_ila(struct rtattr *rta, size_t len, const encap_t *encap)
{
	rta_addattr64(rta, len, ILA_ATTR_LOCATOR, encap->ila.locator);
}

static void
add_encap_ip6(struct rtattr *rta, size_t len, const encap_t *encap)
{
	if (encap->flags & IPROUTE_BIT_ENCAP_ID)
		rta_addattr64(rta, len, LWTUNNEL_IP6_ID, htonll(encap->ip6.id));
	if (encap->ip6.dst)
		rta_addattr_l(rta, len, LWTUNNEL_IP6_DST, &encap->ip6.dst->u.sin6_addr, sizeof(encap->ip6.dst->u.sin6_addr));
	if (encap->ip6.src)
		rta_addattr_l(rta, len, LWTUNNEL_IP6_SRC, &encap->ip6.src->u.sin6_addr, sizeof(encap->ip6.src->u.sin6_addr));
	if (encap->flags & IPROUTE_BIT_ENCAP_DSFIELD)
		rta_addattr8(rta, len, LWTUNNEL_IP6_TC, encap->ip6.tc);
	if (encap->flags & IPROUTE_BIT_ENCAP_HOPLIMIT)
		rta_addattr8(rta, len, LWTUNNEL_IP6_HOPLIMIT, encap->ip6.hoplimit);
	if (encap->flags & IPROUTE_BIT_ENCAP_FLAGS)
		rta_addattr16(rta, len, LWTUNNEL_IP6_FLAGS, encap->ip6.flags);
}

static bool
add_encap(struct rtattr *rta, size_t len, encap_t *encap)
{
	struct rtattr *nest;

	nest = rta_nest(rta, len, RTA_ENCAP);
	switch (encap->type) {
	case LWTUNNEL_ENCAP_MPLS:
		add_encap_mpls(rta, len, encap);
		break;
	case LWTUNNEL_ENCAP_IP:
		add_encap_ip(rta, len, encap);
		break;
	case LWTUNNEL_ENCAP_ILA:
		add_encap_ila(rta, len, encap);
		break;
	case LWTUNNEL_ENCAP_IP6:
		add_encap_ip6(rta, len, encap);
		break;
	default:
		log_message(LOG_INFO, "unknown encap type %d", encap->type);
		break;
	}
	rta_nest_end(rta, nest);

	rta_addattr16(rta, len, RTA_ENCAP_TYPE, encap->type);

	return true;
}
#endif

static void
add_nexthop(nexthop_t *nh, struct nlmsghdr *nlh, struct rtmsg *rtm, struct rtattr *rta, size_t len, struct rtnexthop *rtnh)
{
	if (nh->addr) {
		if (rtm->rtm_family == nh->addr->ifa.ifa_family)
			rtnh->rtnh_len += add_addr2rta(rta, len, RTA_GATEWAY, nh->addr);
#ifdef _HAVE_RTA_VIA_
		else
			rtnh->rtnh_len += add_addrfam2rta(rta, len, RTA_VIA, nh->addr);
#endif
	}
	if (nh->ifp)
		rtnh->rtnh_ifindex = nh->ifp->ifindex;

	if (nh->mask |= IPROUTE_BIT_WEIGHT)
		rtnh->rtnh_hops = nh->weight;

	rtnh->rtnh_flags = nh->flags;

	if (nh->realms)
		rtnh->rtnh_len += rta_addattr32(rta, len, RTA_FLOW, nh->realms);

#ifdef _HAVE_RTA_ENCAP_
	if (nh->encap.type != LWTUNNEL_ENCAP_NONE) {
		int len = rta->rta_len;
		add_encap(rta, len, &nh->encap);
		rtnh->rtnh_len += rta->rta_len - len;
	}
#endif
}

static void
add_nexthops(ip_route_t *route, struct nlmsghdr *nlh, struct rtmsg *rtm)
{
	char buf[ENCAP_RTA_SIZE];
	struct rtattr *rta = (void *)buf;
	struct rtnexthop *rtnh;
	nexthop_t *nh;
	element e;

	rta->rta_type = RTA_MULTIPATH;
	rta->rta_len = RTA_LENGTH(0);
	rtnh = RTA_DATA(rta);

	for (e = LIST_HEAD(route->nhs); e; ELEMENT_NEXT(e)) {
		nh = ELEMENT_DATA(e);

		memset(rtnh, 0, sizeof(*rtnh));
		rtnh->rtnh_len = sizeof(*rtnh);
		rta->rta_len += rtnh->rtnh_len;
		add_nexthop(nh, nlh, rtm, rta, sizeof(buf), rtnh);
		rtnh = RTNH_NEXT(rtnh);
	}

	if (rta->rta_len > RTA_LENGTH(0))
		addattr_l(nlh, sizeof(buf), RTA_MULTIPATH, RTA_DATA(rta), RTA_PAYLOAD(rta));
}

/* Add/Delete IP route to/from a specific interface */
static int
netlink_route(ip_route_t *iproute, int cmd)
{
	int status = 1;
	struct {
		struct nlmsghdr n;
		struct rtmsg r;
		char buf[RTM_SIZE];
	} req;
	char buf[RTA_SIZE];
	struct rtattr *rta = (void*)buf;

	memset(&req, 0, sizeof (req));

	req.n.nlmsg_len   = NLMSG_LENGTH(sizeof(struct rtmsg));
	if (cmd == IPROUTE_DEL) {
		req.n.nlmsg_flags = NLM_F_REQUEST;
		req.n.nlmsg_type  = RTM_DELROUTE;
	}
	else {
		req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE;
		if (cmd == IPROUTE_REPLACE)
			req.n.nlmsg_flags |= NLM_F_REPLACE;
		req.n.nlmsg_type  = RTM_NEWROUTE;
	}

	rta->rta_type = RTA_METRICS;
	rta->rta_len = RTA_LENGTH(0);

	req.r.rtm_family = iproute->family;
	if (iproute->table < 256)
		req.r.rtm_table = iproute->table;
	else {
		req.r.rtm_table = RT_TABLE_UNSPEC;
		addattr32(&req.n, sizeof(req), RTA_TABLE, iproute->table);
	}

	if (cmd == IPROUTE_DEL) {
		req.r.rtm_scope = RT_SCOPE_NOWHERE;
		if (iproute->mask & IPROUTE_BIT_TYPE)
			req.r.rtm_type = iproute->type;
	}
	else {
		req.r.rtm_protocol = RTPROT_BOOT;
		req.r.rtm_scope = RT_SCOPE_UNIVERSE;
		req.r.rtm_type = iproute->type;
	}

	if (iproute->mask & IPROUTE_BIT_PROTOCOL)
		req.r.rtm_protocol = iproute->protocol;

	if (iproute->mask & IPROUTE_BIT_SCOPE)
		req.r.rtm_scope = iproute->scope;

	if (iproute->dst) {
		req.r.rtm_dst_len = iproute->dst->ifa.ifa_prefixlen;
		add_addr2req(&req.n, sizeof(req), RTA_DST, iproute->dst);
	}

	if (iproute->src) {
		req.r.rtm_src_len = iproute->src->ifa.ifa_prefixlen;
		add_addr2req(&req.n, sizeof(req), RTA_SRC, iproute->src);
	}

	if (iproute->pref_src)
		add_addr2req(&req.n, sizeof(req), RTA_PREFSRC, iproute->pref_src);

//#ifdef _HAVE_RTA_NEWDST_
//	if (iproute->as_to)
//		add_addr2req(&req.n, sizeof(req), RTA_NEWDST, iproute->as_to);
//#endif

	if (iproute->via) {
		if (iproute->via->ifa.ifa_family == iproute->family)
			add_addr2req(&req.n, sizeof(req), RTA_GATEWAY, iproute->via);
#ifdef _HAVE_RTA_VIA_
		else
			add_addr_fam2req(&req.n, sizeof(req), RTA_VIA, iproute->via);
#endif
	}

#ifdef _HAVE_RTA_ENCAP_
	if (iproute->encap.type != LWTUNNEL_ENCAP_NONE) {
		char encap_buf[ENCAP_RTA_SIZE];
		struct rtattr *encap_rta = (void *)encap_buf;

		encap_rta->rta_type = RTA_ENCAP;
		encap_rta->rta_len = RTA_LENGTH(0);
		add_encap(encap_rta, sizeof(encap_buf), &iproute->encap);

		if (encap_rta->rta_len > RTA_LENGTH(0))
			addraw_l(&req.n, sizeof(encap_buf), RTA_DATA(encap_rta), RTA_PAYLOAD(encap_rta));
	}
#endif

	if (iproute->mask & IPROUTE_BIT_DSFIELD)
		req.r.rtm_tos = iproute->tos;
	
	if (iproute->oif)
		addattr32(&req.n, sizeof(req), RTA_OIF, iproute->oif->ifindex);

	if (iproute->mask & IPROUTE_BIT_METRIC)
		addattr32(&req.n, sizeof(req), RTA_PRIORITY, iproute->metric);

	req.r.rtm_flags = iproute->flags;

	if (iproute->realms)
		addattr32(&req.n, sizeof(req), RTA_FLOW, iproute->realms);

#ifdef _HAVE_RTA_EXPIRES_
	if (iproute->mask & IPROUTE_BIT_EXPIRES)
		addattr32(&req.n, sizeof(req), RTA_EXPIRES, iproute->expires);
#endif

#ifdef RTAX_CC_ALGO
	if (iproute->congctl)
		rta_addattr_l(rta, sizeof(buf), RTAX_CC_ALGO, iproute->congctl, strlen(iproute->congctl));
#endif

	if (iproute->mask & IPROUTE_BIT_RTT)
		rta_addattr32(rta, sizeof(buf), RTAX_RTT, iproute->rtt);

	if (iproute->mask & IPROUTE_BIT_RTTVAR)
		rta_addattr32(rta, sizeof(buf), RTAX_RTTVAR, iproute->rttvar);

	if (iproute->mask & IPROUTE_BIT_RTO_MIN)
		rta_addattr32(rta, sizeof(buf), RTAX_RTO_MIN, iproute->rto_min);

#ifdef RTAX_FEATURES
	if (iproute->features)
		rta_addattr32(rta, sizeof(buf), RTAX_FEATURES, iproute->features);
#endif

	if (iproute->mask & IPROUTE_BIT_MTU)
		rta_addattr32(rta, sizeof(buf), RTAX_MTU, iproute->mtu);

	if (iproute->mask & IPROUTE_BIT_WINDOW)
		rta_addattr32(rta, sizeof(buf), RTAX_WINDOW, iproute->window);

	if (iproute->mask & IPROUTE_BIT_SSTHRESH)
		rta_addattr32(rta, sizeof(buf), RTAX_SSTHRESH, iproute->ssthresh);

	if (iproute->mask & IPROUTE_BIT_CWND)
		rta_addattr32(rta, sizeof(buf), RTAX_CWND, iproute->cwnd);

	if (iproute->mask & IPROUTE_BIT_ADVMSS)
		rta_addattr32(rta, sizeof(buf), RTAX_ADVMSS, iproute->advmss);

	if (iproute->mask & IPROUTE_BIT_REORDERING)
		rta_addattr32(rta, sizeof(buf), RTAX_REORDERING, iproute->reordering);

	if (iproute->mask & IPROUTE_BIT_HOPLIMIT)
		rta_addattr32(rta, sizeof(buf), RTAX_HOPLIMIT, iproute->hoplimit);

	if (iproute->mask & IPROUTE_BIT_INITCWND)
		rta_addattr32(rta, sizeof(buf), RTAX_INITCWND, iproute->initcwnd);

#ifdef RTAX_INITRWND
	if (iproute->mask & IPROUTE_BIT_INITRWND)
		rta_addattr32(rta, sizeof(buf), RTAX_INITRWND, iproute->initrwnd);
#endif

#ifdef RTAX_QUICKACK
	if (iproute->mask & IPROUTE_BIT_QUICKACK)
		rta_addattr32(rta, sizeof(buf), RTAX_QUICKACK, iproute->quickack);
#endif

#ifdef _HAVE_RTA_PREF_
	if (iproute->mask & IPROUTE_BIT_PREF)
		addattr8(&req.n, sizeof(req), RTA_PREF, iproute->pref);
#endif

	if (rta->rta_len > RTA_LENGTH(0)) {
		if (iproute->lock)
			rta_addattr32(rta, sizeof(buf), RTAX_LOCK, iproute->lock);
		addattr_l(&req.n, sizeof(req), RTA_METRICS, RTA_DATA(rta), RTA_PAYLOAD(rta));
	}

	if (!LIST_ISEMPTY(iproute->nhs))
		add_nexthops(iproute, &req.n, &req.r);

#ifdef DEBUG_NETLINK_MSG
	size_t i, j;
	uint8_t *p;
	char lbuf[3072];
	char *op = lbuf;

	log_message(LOG_INFO, "rtmsg buffer used %lu, rtattr buffer used %d", req.n.nlmsg_len - NLMSG_LENGTH(sizeof(struct rtmsg)), rta->rta_len);

	op += snprintf(op, sizeof(lbuf) - (op - lbuf), "nlmsghdr %p(%u):", &req.n, req.n.nlmsg_len);
	for (i = 0, p = (uint8_t*)&req.n; i < sizeof(struct nlmsghdr); i++)
		op += snprintf(op, sizeof(lbuf) - (op - lbuf), " %2.2hhx", *(p++));
	log_message(LOG_INFO, "%s\n", lbuf);

	op = lbuf;
	op += snprintf(op, sizeof(lbuf) - (op - lbuf), "rtmsg %p(%lu):", &req.r, req.n.nlmsg_len - sizeof(struct nlmsghdr));
	for (i = 0, p = (uint8_t*)&req.r; i < + req.n.nlmsg_len - sizeof(struct nlmsghdr); i++)
		op += snprintf(op, sizeof(lbuf) - (op - lbuf), " %2.2hhx", *(p++));

	for (j = 0; lbuf + j < op; j+= MAX_LOG_MSG)
		log_message(LOG_INFO, "%.*\n", MAX_LOG_MSG, lbuf+j);
#endif

	/* This returns ESRCH if the address of via address doesn't exist */
	/* ENETDOWN if dev p33p1.40 for example is down */
	if (netlink_talk(&nl_cmd, &req.n) < 0) {
#ifdef _HAVE_RTA_EXPIRES_
		/* If an expiry was set on the route, it may have disappeared already */
		if (cmd != IPADDRESS_DEL || !(iproute->mask & IPROUTE_BIT_EXPIRES))
#endif
			status = -1;
	}

	return status;
}

/* Add/Delete a list of IP routes */
void
netlink_rtlist(list rt_list, int cmd)
{
	ip_route_t *iproute;
	element e;

	/* No routes to add */
	if (LIST_ISEMPTY(rt_list))
		return;

	for (e = LIST_HEAD(rt_list); e; ELEMENT_NEXT(e)) {
		iproute = ELEMENT_DATA(e);
		if ((cmd == IPROUTE_DEL) == iproute->set) {
			if (netlink_route(iproute, cmd) > 0)
				iproute->set = (cmd == IPROUTE_ADD);
			else
				iproute->set = false;
		}
	}
}

/* Route dump/allocation */
#ifdef _HAVE_RTA_ENCAP_
void
free_encap(void *rt_data)
{
	encap_t *encap = rt_data;

	if (encap->type == LWTUNNEL_ENCAP_IP) {
		FREE_PTR(encap->ip.dst);
		FREE_PTR(encap->ip.src);
	}
	else if (encap->type == LWTUNNEL_ENCAP_IP6) {
		FREE_PTR(encap->ip6.dst);
		FREE_PTR(encap->ip6.src);
	}

	FREE(rt_data);
}
#endif

void
free_nh(void *rt_data)
{
	nexthop_t *nh = rt_data;

	FREE_PTR(nh->addr);
//#ifdef _HAVE_RTA_NEWDST_
//	FREE_PTR(nh->as_to);
//#endif
	FREE(rt_data);
}

void
free_iproute(void *rt_data)
{
	ip_route_t *route = rt_data;

	FREE_PTR(route->dst);
	FREE_PTR(route->src);
	FREE_PTR(route->pref_src);
	FREE_PTR(route->via);
	free_list(&route->nhs);
#ifdef RTAX_CC_ALGO
	FREE_PTR(route->congctl);
#endif
	FREE(rt_data);
}

#ifdef _HAVE_RTA_ENCAP_
static size_t
print_encap_mpls(char *op, size_t len, const encap_t* encap)
{
	int i;
	char *opn = op;

	opn += snprintf(opn, len - (opn - op), " encap mpls");
	for (i = 0; i < encap->mpls.num_labels; i++)
		opn += snprintf(opn, len - (opn - op), "%s%x", i ? "/" : " ", ntohl(encap->mpls.addr[i].entry));

	return opn - op;
}

static size_t
print_encap_ip(char *op, size_t len, const encap_t* encap)
{
	char *opn = op;

	opn += snprintf(opn, len - (opn - op), " encap ip");

	if (encap->flags & IPROUTE_BIT_ENCAP_ID)
		opn += snprintf(opn, len - (opn - op), " id %" PRIu64, encap->ip.id);
	if (encap->ip.dst)
		opn += snprintf(opn, len - (opn - op), " dst %s", ipaddresstos(NULL, encap->ip.dst));
	if (encap->ip.src)
		opn += snprintf(opn, len - (opn - op), " src %s", ipaddresstos(NULL, encap->ip.src));
	if (encap->flags & IPROUTE_BIT_ENCAP_DSFIELD)
		opn += snprintf(opn, len - (opn - op), " tos %d", encap->ip.tos);
	if (encap->flags & IPROUTE_BIT_ENCAP_TTL)
		opn += snprintf(opn, len - (opn - op), " ttl %d", encap->ip.ttl);
	if (encap->flags & IPROUTE_BIT_ENCAP_FLAGS)
		opn += snprintf(opn, len - (opn - op), " flags 0x%x", encap->ip.flags);

	return opn - op;
}

static size_t
print_encap_ila(char *op, size_t len, const encap_t* encap)
{
	return snprintf(op, len, " encap ila %" PRIu64, encap->ila.locator);
}

static size_t
print_encap_ip6(char *op, size_t len, const encap_t* encap)
{
	char *opn = op;

	opn += snprintf(opn, len - (opn - op), " encap ip6");

	if (encap->flags & IPROUTE_BIT_ENCAP_ID)
		opn += snprintf(opn, len - (opn - op), " id %" PRIu64, encap->ip6.id);
	if (encap->ip.dst)
		opn += snprintf(opn, len - (opn - op), " dst %s", ipaddresstos(NULL, encap->ip6.dst));
	if (encap->ip.src)
		opn += snprintf(opn, len - (opn - op), " src %s", ipaddresstos(NULL, encap->ip6.src));
	if (encap->flags & IPROUTE_BIT_ENCAP_DSFIELD)
		opn += snprintf(opn, len - (opn - op), " tc %d", encap->ip6.tc);
	if (encap->flags & IPROUTE_BIT_ENCAP_HOPLIMIT)
		opn += snprintf(opn, len - (opn - op), " hoplimit %d", encap->ip6.hoplimit);
	if (encap->flags & IPROUTE_BIT_ENCAP_FLAGS)
		opn += snprintf(opn, len - (opn - op), " flags 0x%x", encap->ip6.flags);

	return opn - op;
}

static size_t
print_encap(char *op, size_t len, const encap_t* encap)
{
	switch (encap->type) {
	case LWTUNNEL_ENCAP_MPLS:
		return print_encap_mpls(op, len, encap);
	case LWTUNNEL_ENCAP_IP:
		return print_encap_ip(op, len, encap);
	case LWTUNNEL_ENCAP_ILA:
		return print_encap_ila(op, len, encap);
	case LWTUNNEL_ENCAP_IP6:
		return print_encap_ip6(op, len, encap);
	}

	return snprintf(op, len, "unknown encap type %d", encap->type);
}
#endif

void
format_iproute(ip_route_t *route, char *buf, size_t buf_len)
{
	char *op = buf;
	char *buf_end = buf + buf_len;
	nexthop_t *nh;
	element e;

	if (route->type != RTN_UNICAST)
		op += snprintf(op, buf_end - op, " %s", get_rttables_rtntype(route->type));
	if (route->dst) {
		op += snprintf(op, buf_end - op, " %s", ipaddresstos(NULL, route->dst));
		if ((route->dst->ifa.ifa_family == AF_INET && route->dst->ifa.ifa_prefixlen != 32 ) ||
		    (route->dst->ifa.ifa_family == AF_INET6 && route->dst->ifa.ifa_prefixlen != 128 ))
			op += snprintf(op, buf_end - op, "/%u", route->dst->ifa.ifa_prefixlen);
	}
	else
		op += snprintf(op, buf_end - op, " %s", "default");

	if (route->src) {
		op += snprintf(op, buf_end - op, " from %s", ipaddresstos(NULL, route->src));
		if ((route->src->ifa.ifa_family == AF_INET && route->src->ifa.ifa_prefixlen != 32 ) ||
		    (route->src->ifa.ifa_family == AF_INET6 && route->src->ifa.ifa_prefixlen != 128 ))
			op += snprintf(op, buf_end - op, "/%u", route->src->ifa.ifa_prefixlen);
	}

//#ifdef _HAVE_RTA_NEWDST_
//	/* MPLS only */
//	if (route->as_to)
//		op += snprintf(op, buf_end - op, " as to %s", ipaddresstos(NULL, route->as_to));
//#endif

	if (route->pref_src)
		op += snprintf(op, buf_end - op, " src %s", ipaddresstos(NULL, route->pref_src));

	if (route->mask & IPROUTE_BIT_DSFIELD)
		op += snprintf(op, buf_end - op, " tos %u", route->tos);

#ifdef _HAVE_RTA_ENCAP_
	if (route->encap.type != LWTUNNEL_ENCAP_NONE)
		op += print_encap(op, buf_end - op, &route->encap);
#endif

	if (route->via)
		op += snprintf(op, buf_end - op, " via %s %s", route->via->ifa.ifa_family == AF_INET6 ? "inet6" : "inet", ipaddresstos(NULL, route->via));

	if (route->oif)
		op += snprintf(op, buf_end - op, " dev %s", route->oif->ifname);

	if (route->table != RT_TABLE_MAIN)
		op += snprintf(op, buf_end - op, " table %u", route->table);

	if (route->mask & IPROUTE_BIT_PROTOCOL)
		op += snprintf(op, buf_end - op, " proto %u", route->protocol);

	if (route->mask & IPROUTE_BIT_SCOPE)
		op += snprintf(op, buf_end - op, " scope %u", route->scope);

	if (route->mask & IPROUTE_BIT_METRIC)
		op += snprintf(op, buf_end - op, " metric %u", route->metric);

	if (route->family == AF_INET && route->flags & RTNH_F_ONLINK)
		op += snprintf(op, buf_end - op, " %s", "onlink");

	if (route->realms) {
		if (route->realms & 0xFFFF0000)
			op += snprintf(op, buf_end - op, " realms %d/", route->realms >> 16);
		else
			op += snprintf(op, buf_end - op, " realm ");
		op += snprintf(op, buf_end - op, "%d", route->realms & 0xFFFF);
	}

#ifdef _HAVE_RTA_EXPIRES_
	if (route->mask & IPROUTE_BIT_EXPIRES)
		op += snprintf(op, buf_end - op, " expires %dsec", route->expires);
#endif

#ifdef RTAX_CC_ALGO
	if (route->congctl)
		op += snprintf(op, buf_end - op, " congctl %s%s", route->congctl, route->lock & (1<<RTAX_CC_ALGO) ? "lock " : "");
#endif

	if (route->mask & IPROUTE_BIT_RTT) {
		op += snprintf(op, buf_end - op, " %s%s ", "rtt", route->lock & (1<<RTAX_RTT) ? " lock" : "");
		if (route->rtt >= 8000)
			op += snprintf(op, buf_end - op, "%gs", route->rtt / 8000.0);
		else
			op += snprintf(op, buf_end - op, "%ums", route->rtt / 8);
	}

	if (route->mask & IPROUTE_BIT_RTTVAR) {
		op += snprintf(op, buf_end - op, " %s%s ", "rttvar", route->lock & (1<<RTAX_RTTVAR) ? " lock" : "");
		if (route->rttvar >= 4000)
			op += snprintf(op, buf_end - op, "%gs", route->rttvar / 4000.0);
		else
			op += snprintf(op, buf_end - op, "%ums", route->rttvar / 4);
	}

	if (route->mask & IPROUTE_BIT_RTO_MIN) {
		op += snprintf(op, buf_end - op, " %s%s ", "rto_min", route->lock & (1<<RTAX_RTO_MIN) ? " lock" : "");
		if (route->rto_min >= 1000)
			op += snprintf(op, buf_end - op, "%gs", route->rto_min / 1000.0);
		else
			op += snprintf(op, buf_end - op, "%ums", route->rto_min);
	}

#ifdef RTAX_FEATURES
	if (route->features) {
		if (route->features & RTAX_FEATURE_ECN)
			op += snprintf(op, buf_end - op, " %s", "features ecn");
	}
#endif

	if (route->mask & IPROUTE_BIT_MTU) {
		op += snprintf(op, buf_end - op, " mtu %s%u",
			route->lock & (1<<RTAX_MTU) ? "lock " : "",
			route->mtu);
	}

	if (route->mask & IPROUTE_BIT_WINDOW)
		op += snprintf(op, buf_end - op, " window %u", route->window);

	if (route->mask & IPROUTE_BIT_SSTHRESH) {
		op += snprintf(op, buf_end - op, " ssthresh %s%u",
			route->lock & (1<<RTAX_SSTHRESH) ? "lock " : "",
			route->ssthresh);
	}

	if (route->mask & IPROUTE_BIT_CWND) {
		op += snprintf(op, buf_end - op, " cwnd %s%u",
			route->lock & (1<<RTAX_CWND) ? "lock " : "",
			route->cwnd);
	}

	if (route->mask & IPROUTE_BIT_ADVMSS) {
		op += snprintf(op, buf_end - op, " advmss %s%u",
			route->lock & (1<<RTAX_ADVMSS) ? "lock " : "",
			route->advmss);
	}

	if (route->mask & IPROUTE_BIT_REORDERING) {
		op += snprintf(op, buf_end - op, " reordering %s%u",
			route->lock & (1<<RTAX_REORDERING) ? "lock " : "",
			route->reordering);
	}

	if (route->mask & IPROUTE_BIT_HOPLIMIT)
		op += snprintf(op, buf_end - op, " hoplimit %u", route->hoplimit);

	if (route->mask & IPROUTE_BIT_INITCWND)
		op += snprintf(op, buf_end - op, " initcwnd %u", route->initcwnd);

#ifdef RTAX_INITRWND
	if (route->mask & IPROUTE_BIT_INITRWND)
		op += snprintf(op, buf_end - op, " initrwnd %u", route->initrwnd);
#endif

#ifdef RTAX_QUICKACK
	if (route->mask & IPROUTE_BIT_QUICKACK)
		op += snprintf(op, buf_end - op, " quickack %u", route->quickack);
#endif

#ifdef _HAVE_RTA_PREF_
	if (route->mask & IPROUTE_BIT_PREF)
		op += snprintf(op, buf_end - op, " %s %s", "pref",
			route->pref == ICMPV6_ROUTER_PREF_LOW ? "low" :
			route->pref == ICMPV6_ROUTER_PREF_MEDIUM ? "medium" :
			route->pref == ICMPV6_ROUTER_PREF_HIGH ? "high" :
			"unknown");
#endif

	if (!LIST_ISEMPTY(route->nhs)) {
		for (e = LIST_HEAD(route->nhs); e; ELEMENT_NEXT(e)) {
			nh = ELEMENT_DATA(e);

			op += snprintf(op, buf_end - op, " nexthop");

			if (nh->addr)
				op += snprintf(op, buf_end - op, " via inet%s %s",
					nh->addr->ifa.ifa_family == AF_INET ? "" : "6",
					ipaddresstos(NULL,nh->addr));
			if (nh->ifp)
				op += snprintf(op, buf_end - op, " dev %s", nh->ifp->ifname);

			if (nh->mask & IPROUTE_BIT_WEIGHT)
				op += snprintf(op, buf_end - op, " weight %d", nh->weight + 1);

			if (nh->flags & RTNH_F_ONLINK)
				op += snprintf(op, buf_end - op, " onlink");

			if (nh->realms) {
				if (route->realms & 0xFFFF0000)
					op += snprintf(op, buf_end - op, " realms %d/", nh->realms >> 16);
				else
					op += snprintf(op, buf_end - op, " realm ");
				op += snprintf(op, buf_end - op, "%d", nh->realms & 0xFFFF);
			}
#ifdef _HAVE_RTA_ENCAP_
			if (nh->encap.type != LWTUNNEL_ENCAP_NONE)
				op += print_encap(op, buf_end - op, &nh->encap);
#endif
		}
	}
}

void
dump_iproute(void *rt_data)
{
	ip_route_t *route = rt_data;
	char *buf = MALLOC(ROUTE_BUF_SIZE);
	int len;
	int i;

	format_iproute(route, buf, ROUTE_BUF_SIZE);

	for (i = 0, len = strlen(buf); i < len; i += i ? MAX_LOG_MSG - 7 : MAX_LOG_MSG - 5)
		log_message(LOG_INFO, "%*s%s", i ? 7 : 5, "", buf + i);

	FREE(buf);
}

#ifdef _HAVE_RTA_ENCAP_
static int parse_encap_mpls(vector_t *strvec, unsigned int *i_ptr, encap_t *encap)
{
	char *str;

	encap->type = LWTUNNEL_ENCAP_MPLS;

	if (*i_ptr >= vector_size(strvec)) {
		log_message(LOG_INFO, "missing address for MPLS encapsulation");
		return true;
	}

	str = vector_slot(strvec, (*i_ptr)++);
	if (parse_mpls_address(str, &encap->mpls)) {
		log_message(LOG_INFO, "invalid mpls address %s for encapsulation", str);
		return true;
	}

	return false;
}

static int parse_encap_ip(vector_t *strvec, unsigned int *i_ptr, encap_t *encap)
{
	unsigned int i = *i_ptr;
	char *str, *str1;

	encap->type = LWTUNNEL_ENCAP_IP;

	while (i + 1 < vector_size(strvec)) {
		str = vector_slot(strvec, i);
		str1 = vector_slot(strvec, i + 1);

		if (!strcmp(str, "id")) {
			if (get_u64(&encap->ip.id, str1, UINT64_MAX, "encap id %s value is invalid"))
				goto err;
			encap->flags |= IPROUTE_BIT_ENCAP_ID;
		} else if (!strcmp(str, "dst")) {
			if (encap->ip.dst)
				FREE_PTR(encap->ip.dst);
			encap->ip.dst = parse_ipaddress(NULL, str1, false);
			if (!encap->ip.dst) {
				log_message(LOG_INFO, "Invalid encap ip dst %s", str1);
				goto err;
			}
			if (encap->ip.dst->ifa.ifa_family != AF_INET) {
				log_message(LOG_INFO, "IPv6 address %s not valid for ip encapsulation", str1);
				goto err;
			}
		} else if (!strcmp(str, "src")) {
			if (encap->ip.src)
				FREE_PTR(encap->ip.src);
			encap->ip.src = parse_ipaddress(NULL, str1, false);
			if (!encap->ip.src) {
				log_message(LOG_INFO, "Invalid encap ip src %s", str1);
				goto err;
			}
			if (encap->ip.src->ifa.ifa_family != AF_INET) {
				log_message(LOG_INFO, "IPv6 address %s not valid for ip encapsulation", str1);
				goto err;
			}
		} else if (!strcmp(str, "tos")) {
			if (!find_rttables_dsfield(str1, &encap->ip.tos)) {
				log_message(LOG_INFO, "dsfield %s not valid for ip encapsulation", str1);
				goto err;
			}
			encap->flags |= IPROUTE_BIT_ENCAP_DSFIELD;
		} else if (!strcmp(str, "ttl")) {
			if (get_u8(&encap->ip.ttl, str1, UINT8_MAX, "ttl %s is not valid for ip encapsulation"))
				goto err;
			encap->flags |= IPROUTE_BIT_ENCAP_TTL;
		} else if (!strcmp(str, "flags")) {
			if (get_u16(&encap->ip.flags, str1, UINT16_MAX, "flags %s is not valid for ip encapsulation"))
				goto err;
			encap->flags |= IPROUTE_BIT_ENCAP_FLAGS;
		} else
			break;

		i += 2;
	}

	if (!encap->ip.dst && !(encap->flags | IPROUTE_BIT_ENCAP_ID)) {
		log_message(LOG_INFO, "address or id missing for ip encapsulation");
		goto err;
	}

	*i_ptr = i;

	return false;

err:
	*i_ptr = i;

	FREE_PTR(encap->ip.dst);
	FREE_PTR(encap->ip.src);
	return true;
}

static
int parse_encap_ila(vector_t *strvec, unsigned int *i_ptr, encap_t *encap)
{
	char *str;

	encap->type = LWTUNNEL_ENCAP_ILA;

	if (*i_ptr >= vector_size(strvec)) {
		log_message(LOG_INFO, "missing locator for ILA encapsulation");
		return true;
	}

	str = vector_slot(strvec, (*i_ptr)++);

	if (get_addr64(&encap->ila.locator, str)) {
		log_message(LOG_INFO, "invalid locator %s for ila encapsulation", str);
		return true;
	}

	return false;
}

static
int parse_encap_ip6(vector_t *strvec, unsigned int *i_ptr, encap_t *encap)
{
	unsigned int i = *i_ptr;
	char *str, *str1;

	encap->type = LWTUNNEL_ENCAP_IP6;

	while (i + 1 < vector_size(strvec)) {
		str = vector_slot(strvec, i);
		str1 = vector_slot(strvec, i + 1);

		if (!strcmp(str, "id")) {
			if (get_u64(&encap->ip6.id, str1, UINT64_MAX, "id %s value invalid for IPv6 encapsulation\n"))
				goto err;
			encap->flags |= IPROUTE_BIT_ENCAP_ID;
		} else if (!strcmp(str, "dst")) {
			if (encap->ip6.dst)
				FREE_PTR(encap->ip6.dst);
			encap->ip6.dst = parse_ipaddress(NULL, str1, false);
			if (!encap->ip6.dst) {
				log_message(LOG_INFO, "Invalid encap ip6 dst %s", str1);
				goto err;
			}
			if (encap->ip6.dst->ifa.ifa_family != AF_INET6) {
				log_message(LOG_INFO, "IPv4 address %s not valid for ip6 encapsulation", str1);
				goto err;
			}
		} else if (!strcmp(str, "src")) {
			if (encap->ip6.src)
				FREE_PTR(encap->ip6.src);
			encap->ip6.src = parse_ipaddress(NULL, str1, false);
			if (!encap->ip6.src) {
				log_message(LOG_INFO, "Invalid encap ip6 src %s", str1);
				goto err;
			}
			if (encap->ip6.src->ifa.ifa_family != AF_INET6) {
				log_message(LOG_INFO, "IPv4 address %s not valid for ip6 encapsulation", str1);
				goto err;
			}
		} else if (!strcmp(str, "tc")) {
			if (!find_rttables_dsfield(str1, &encap->ip6.tc)) {
				log_message(LOG_INFO, "tc value %s is invalid for ip6 encapsulation", str);
				goto err;
			}
			encap->flags |= IPROUTE_BIT_ENCAP_DSFIELD;
		} else if (!strcmp(str, "hoplimit")) {
			if (get_u8(&encap->ip6.hoplimit, str1, UINT8_MAX, "Invalid hoplimit %s specified for ip6 encapsulation"))
				goto err;
			encap->flags |= IPROUTE_BIT_ENCAP_HOPLIMIT;
		} else if (!strcmp(str, "flags")) {
			if (get_u16(&encap->ip6.flags, str1, UINT16_MAX, "flags %s is not valid for ip6 encapsulation"))
				goto err;
			encap->flags |= IPROUTE_BIT_ENCAP_FLAGS;
		} else
			break;

		i += 2;
	}

	if (!encap->ip.dst && !(encap->flags | IPROUTE_BIT_ENCAP_ID)) {
		log_message(LOG_INFO, "address or id missing for ip6 encapsulation");
		goto err;
	}

	*i_ptr = i;
	return false;

err:
	*i_ptr = i;
	FREE_PTR(encap->ip6.dst);
	FREE_PTR(encap->ip6.src);
	return true;
}

static bool
parse_encap(vector_t *strvec, unsigned int *i, encap_t *encap)
{
	char *str;

	if (vector_size(strvec) <= ++*i) {
		log_message(LOG_INFO, "Missing encap type");
		return false;
	}

	str = vector_slot(strvec, (*i)++);

	if (!strcmp(str, "mpls"))
		parse_encap_mpls(strvec, i, encap);
	else if (!strcmp(str, "ip"))
		parse_encap_ip(strvec, i, encap);
	else if (!strcmp(str, "ip6"))
		parse_encap_ip6(strvec, i, encap);
	else if (!strcmp(str, "ila"))
		parse_encap_ila(strvec, i, encap);
	else {
		log_message(LOG_INFO, "Unknown encap type - %s", str);
		return false;
	}

	--*i;
	return true;
}
#endif

static void
parse_nexthops(vector_t *strvec, unsigned int i, ip_route_t *route)
{
	int family = AF_UNSPEC;
	nexthop_t *new;
	char *str;
	uint32_t val;

	if (!LIST_EXISTS(route->nhs))
		route->nhs = alloc_list(free_nh, NULL);

	while (i < vector_size(strvec) && !strcmp("nexthop", vector_slot(strvec, i))) {
		i++;
		new = MALLOC(sizeof(nexthop_t));

		while (i < vector_size(strvec)) {
			str = vector_slot(strvec, i);

			if (!strcmp(str, "via")) {
				str = vector_slot(strvec, ++i);
				if (!strcmp(str, "inet")) {
					family = AF_INET;
					str = vector_slot(strvec, ++i);
				}
				else if (!strcmp(str, "inet6")) {
					family = AF_INET6;
					str = vector_slot(strvec, ++i);
				}

				if (family != AF_UNSPEC) {
					if (route->family == AF_UNSPEC)
						route->family = family;
					else if (route->family != family) {
						log_message(LOG_INFO, "IPv4/6 mismatch for nexthop");
						goto err;
					}
				}

				new->addr = parse_ipaddress(NULL, str, false);
				if (!new->addr) {
					log_message(LOG_INFO, "invalid nexthop address %s", str);
					goto err;
				}
				if (route->family != AF_UNSPEC && new->addr->ifa.ifa_family != route->family) {
					log_message(LOG_INFO, "Address family mismatch for next hop");
					goto err;
				}
				if (route->family == AF_UNSPEC)
					route->family = new->addr->ifa.ifa_family;
			}
			else if (!strcmp(str, "dev")) {
				new->ifp = if_get_by_ifname(vector_slot(strvec, ++i));
				if (!new->ifp) {
					log_message(LOG_INFO, "VRRP is trying to assign VROUTE to unknown "
					       "%s interface !!! go out and fix your conf !!!",
					       FMT_STR_VSLOT(strvec, i));
					goto err;
				}
			}
			else if (!strcmp(str, "weight")) {
				if (get_u32(&val, vector_slot(strvec, ++i), 256, "Invalid weight %s specified for route"))
					goto err;
				if (!val) {
					log_message(LOG_INFO, "Invalid weight 0 specified for route");
					goto err;
				}
				new->weight = val - 1;
				new->mask |= IPROUTE_BIT_WEIGHT;
			}
			else if (!strcmp(str, "onlink")) {
				/* Note: IPv4 only */
				new->flags |= RTNH_F_ONLINK;
			}
			else if (!strcmp(str, "encap")) {	// New in 4.4
#ifdef _HAVE_RTA_ENCAP_
				parse_encap(strvec, &i, &new->encap);
#else
				log_message(LOG_INFO, "encap not supported by kernel - please remove configuration");
#endif
			}
			else if (!strcmp(str, "realms")) {
				/* Note: IPv4 only */
				if (get_realms(&new->realms, vector_slot(strvec, ++i))) {
					log_message(LOG_INFO, "Invalid realms %s for route", FMT_STR_VSLOT(strvec,i));
					goto err;
				}
				if (route->family == AF_UNSPEC)
					route->family = AF_INET;
				else if (route->family != AF_INET) {
					log_message(LOG_INFO, "realms are only supported for IPv4");
					goto err;
				}
			}
			else if (!strcmp(str, "as")) {
				if (!strcmp("to", vector_slot(strvec, ++i)))
					i++;
				log_message(LOG_INFO, "'as [to]' (nat) not supported");
				goto err;
			}
			else
				break;

			i++;
		}
		list_add(route->nhs, new);
		new = NULL;
	}

	if (i < vector_size(strvec)) {
		log_message(LOG_INFO, "Route has trailing nonsense after nexthops - %s", FMT_STR_VSLOT(strvec, i));
		goto err;
	}

	return;

err:
	FREE_PTR(new);
}

void
alloc_route(list rt_list, vector_t *strvec)
{
	ip_route_t *new;
	interface_t *ifp;
	char *str;
	uint32_t val;
	uint8_t val8;
	unsigned int i = 0;
	bool do_nexthop = false;
	bool raw;
	ip_address_t *dst;

	new = (ip_route_t *) MALLOC(sizeof(ip_route_t));

	new->table = RT_TABLE_MAIN;
	new->scope = RT_SCOPE_UNIVERSE;
	new->type = RTN_UNICAST;
	new->family = AF_UNSPEC;

	/* FMT parse */
	while (i < vector_size(strvec)) {
		str = vector_slot(strvec, i);

		/* cmd parsing */
		if (!strcmp(str, "src")) {
			if (new->pref_src)
				FREE(new->pref_src);
			new->pref_src = parse_ipaddress(NULL, vector_slot(strvec, ++i), false);
			if (!new->pref_src) {
				log_message(LOG_INFO, "invalid route src address %s", FMT_STR_VSLOT(strvec, i));
				goto err;
			}
			if (new->family == AF_UNSPEC)
				new->family = new->pref_src->ifa.ifa_family;
			else if (new->family != new->pref_src->ifa.ifa_family) {
				log_message(LOG_INFO, "Cannot mix IPv4 and IPv6 addresses for route");
				goto err;
			}
		}
		else if (!strcmp(str, "as")) {
			if (!strcmp("to", vector_slot(strvec, ++i)))
				i++;
#ifdef _HAVE_RTA_NEWDST_
			log_message(LOG_INFO, "\"as to\" for MPLS only - ignoring");
#else
			log_message(LOG_INFO, "'as [to]' not supported by kernel");
#endif
		}
		else if (!strcmp(str, "via") || !strcmp(str, "gw")) {
			int family;

			/* "gw" maintained for backward keepalived compatibility */
			if (str[0] == 'g')	/* "gw" */
				log_message(LOG_INFO, "\"gw\" for routes is deprecated. Please use \"via\"");

			str = vector_slot(strvec, ++i);
			if (!strcmp(str, "inet")) {
				family = AF_INET;
				str = vector_slot(strvec, ++i);
			}
			if (!strcmp(str, "inet6")) {
				family = AF_INET6;
				str = vector_slot(strvec, ++i);
			}
			else
				family = new->family;

			if (new->family == AF_UNSPEC)
				new->family = family;
			else if (new->family != family) {
				log_message(LOG_INFO, "Cannot mix IPv4 and IPv6 addresses for route");
				goto err;
			}

			if (new->via)
				FREE(new->via);
			new->via = parse_ipaddress(NULL, str, false);
			if (!new->via) {
				log_message(LOG_INFO, "invalid route via address %s", FMT_STR_VSLOT(strvec, i));
				goto err;
			}
			if (new->family == AF_UNSPEC)
				new->family = new->via->ifa.ifa_family;
			else if (new->family != new->via->ifa.ifa_family) {
				log_message(LOG_INFO, "Cannot mix IPv4 and IPv6 addresses for route");
				goto err;
			}
		}
		else if (!strcmp(str, "from")) {
			if (new->src)
				FREE(new->src);
			new->src = parse_ipaddress(NULL, vector_slot(strvec, ++i), false);
			if (!new->src) {
				log_message(LOG_INFO, "invalid route from address %s", FMT_STR_VSLOT(strvec, i));
				goto err;
			}
			if (new->src->ifa.ifa_family != AF_INET6) {
				log_message(LOG_INFO, "route from address only supported with IPv6 (%s)", FMT_STR_VSLOT(strvec, i));
				goto err;
			}
			if (new->family == AF_UNSPEC)
				new->family = new->src->ifa.ifa_family;
			else if (new->family != new->src->ifa.ifa_family) {
				log_message(LOG_INFO, "Cannot mix IPv4 and IPv6 addresses for route");
				goto err;
			}
		}
		else if (!strcmp(str, "tos") || !strcmp(str,"dsfield")) {
			/* Note: IPv4 only */
			if (!find_rttables_dsfield(vector_slot(strvec, ++i), &val)) {
				log_message(LOG_INFO, "TOS value %s is invalid", FMT_STR_VSLOT(strvec, i));
				goto err;
			}

			new->tos = val;
			new->mask |= IPROUTE_BIT_DSFIELD;
		}
		else if (!strcmp(str, "table")) {
			if (!find_rttables_table(vector_slot(strvec, ++i), &val)) {
				log_message(LOG_INFO, "Routing table %s not found for route", FMT_STR_VSLOT(strvec, i));
				goto err;
			}
			new->table = val;
		}
		else if (!strcmp(str, "protocol")) {
			if (!find_rttables_proto(vector_slot(strvec, ++i), &val)) {
				log_message(LOG_INFO, "Protocol %s not found or invalid for route", FMT_STR_VSLOT(strvec, i));
				goto err;
			}
			new->protocol = val;
			new->mask |= IPROUTE_BIT_PROTOCOL;
		}
		else if (!strcmp(str, "scope")) {
			/* Note: IPv4 only */
			if (!find_rttables_scope(vector_slot(strvec, ++i), &val)) {
				log_message(LOG_INFO, "Scope %s not found or invalid for route", FMT_STR_VSLOT(strvec, i));
				goto err;
			}
			new->scope = val;
			new->mask |= IPROUTE_BIT_SCOPE;
		}
		else if (!strcmp(str, "metric") ||
			 !strcmp(str, "priority") ||
			 !strcmp(str, "preference")) {
			if (get_u32(&new->metric, vector_slot(strvec, ++i), UINT32_MAX, "Invalid MTU %s specified for route"))
				goto err;
			new->mask |= IPROUTE_BIT_METRIC;
		}
		else if (!strcmp(str, "dev") || !strcmp(str, "oif")) {
			ifp = if_get_by_ifname(vector_slot(strvec, ++i));
			if (!ifp) {
				log_message(LOG_INFO, "VRRP is trying to assign VROUTE to unknown "
				       "%s interface !!! go out and fix your conf !!!",
				       FMT_STR_VSLOT(strvec, i));
				goto err;
			}
			new->oif = ifp;
		}
		else if (!strcmp(str, "onlink")) {
			/* Note: IPv4 only */
			new->flags |= RTNH_F_ONLINK;
		}
		else if (!strcmp(str, "encap")) {	// New in 4.4
#ifdef _HAVE_RTA_ENCAP_
			parse_encap(strvec, &i, &new->encap);
#else
			log_message(LOG_INFO, "encap not supported by kernel - please remove configuration");
#endif
		}
		else if (!strcmp(str, "expires")) {	// New in 4.4
			i++;
#ifdef _HAVE_RTA_EXPIRES_
			if (new->family == AF_INET) {
				log_message(LOG_INFO, "expires is only valid for IPv6");
				goto err;
			}
			new->family = AF_INET6;
			if (get_u32(&new->expires, vector_slot(strvec, i), UINT32_MAX, "Invalid expires time %s specified for route"))
				goto err;
			new->mask |= IPROUTE_BIT_EXPIRES;
#else
			log_message(LOG_INFO, "expires not supported by kernel");
#endif
		}
		else if (!strcmp(str, "mtu")) {
			if (!strcmp(vector_slot(strvec, ++i), "lock")) {
				new->lock |= 1 << RTAX_MTU;
				i++;
			}
			if (get_u32(&new->mtu, vector_slot(strvec, i), UINT32_MAX, "Invalid MTU %s specified for route"))
				goto err;
			new->mask |= IPROUTE_BIT_MTU;
		}
		else if (!strcmp(str, "hoplimit")) {
			if (get_u32(&val, vector_slot(strvec, ++i), 255, "Invalid hoplimit %s specified for route"))
				goto err;
			new->hoplimit = val;
			new->mask |= IPROUTE_BIT_HOPLIMIT;
		}
		else if (!strcmp(str, "advmss")) {
			if (!strcmp(vector_slot(strvec, ++i), "lock")) {
				new->lock |= 1 << RTAX_ADVMSS;
				i++;
			}
			if (get_u32(&new->advmss, vector_slot(strvec, i), UINT32_MAX, "Invalid advmss %s specified for route"))
				goto err;
			new->mask |= IPROUTE_BIT_ADVMSS;
		}
		else if (!strcmp(str, "rtt")) {
			if (!strcmp(vector_slot(strvec, ++i), "lock")) {
				new->lock |= 1 << RTAX_RTT;
				i++;
			}
			if (get_time_rtt(&new->rtt, vector_slot(strvec, i), &raw) ||
			    (!raw && new->rtt >= UINT32_MAX / 8)) {
				log_message(LOG_INFO, "Invalid rtt %s for route", FMT_STR_VSLOT(strvec,i));
				goto err;
			}
			if (raw)
				new->rtt *= 8;
			new->mask |= IPROUTE_BIT_RTT;
		}
		else if (!strcmp(str, "rttvar")) {
			if (!strcmp(vector_slot(strvec, ++i), "lock")) {
				new->lock |= 1 << RTAX_RTTVAR;
				i++;
			}
			if (get_time_rtt(&new->rttvar, vector_slot(strvec, i), &raw) ||
			    (!raw && new->rtt >= UINT32_MAX / 4)) {
				log_message(LOG_INFO, "Invalid rttvar %s for route", FMT_STR_VSLOT(strvec,i));
				goto err;
			}
			if (raw)
				new->rttvar *= 4;
			new->mask |= IPROUTE_BIT_RTTVAR;
		}
		else if (!strcmp(str, "reordering")) {
			if (!strcmp(vector_slot(strvec, ++i), "lock")) {
				new->lock |= 1 << RTAX_REORDERING;
				i++;
			}
			if (get_u32(&new->reordering, vector_slot(strvec, i), UINT32_MAX, "Invalid reordering value %s specified for route"))
				goto err;
			new->mask |= IPROUTE_BIT_REORDERING;
		}
		else if (!strcmp(str, "window")) {
			if (get_u32(&new->window, vector_slot(strvec, ++i), UINT32_MAX, "Invalid window value %s specified for route"))
				goto err;
			new->mask |= IPROUTE_BIT_WINDOW;
		}
		else if (!strcmp(str, "cwnd")) {
			if (!strcmp(vector_slot(strvec, ++i), "lock")) {
				new->lock |= 1 << RTAX_CWND;
				i++;
			}
			if (get_u32(&new->cwnd, vector_slot(strvec, i), UINT32_MAX, "Invalid cwnd value %s specified for route"))
				goto err;
			new->mask |= IPROUTE_BIT_CWND;
		}
		else if (!strcmp(str, "ssthresh")) {
			if (!strcmp(vector_slot(strvec, ++i), "lock")) {
				new->lock |= 1 << RTAX_SSTHRESH;
				i++;
			}
			if (get_u32(&new->ssthresh, vector_slot(strvec, i), UINT32_MAX, "Invalid ssthresh value %s specified for route"))
				goto err;
			new->mask |= IPROUTE_BIT_SSTHRESH;
		}
		else if (!strcmp(str, "realms")) {
			if (get_realms(&new->realms, vector_slot(strvec, ++i))) {
				log_message(LOG_INFO, "Invalid realms %s for route", FMT_STR_VSLOT(strvec,i));
				goto err;
			}
			if (new->family == AF_INET6) {
				log_message(LOG_INFO, "realms are only valid for IPv4");
				goto err;
			}
			new->family = AF_INET;
		}
		else if (!strcmp(str, "rto_min")) {
			if (!strcmp(vector_slot(strvec, ++i), "lock")) {
				new->lock |= 1 << RTAX_RTO_MIN;
				i++;
			}
			if (get_time_rtt(&new->rto_min, vector_slot(strvec, i), &raw)) {
				log_message(LOG_INFO, "Invalid rto_min value %s specified for route", FMT_STR_VSLOT(strvec, i));
				goto err;
			}
			new->mask |= IPROUTE_BIT_RTO_MIN;
		}
		else if (!strcmp(str, "initcwnd")) {
			if (get_u32(&new->initcwnd, vector_slot(strvec, ++i), UINT32_MAX, "Invalid initcwnd value %s specified for route"))
				goto err;
			new->mask |= IPROUTE_BIT_INITCWND;
		}
		else if (!strcmp(str, "initrwnd")) {
			i++;
#ifdef RTAX_INITRWND
			if (get_u32(&new->initrwnd, vector_slot(strvec, i), UINT32_MAX, "Invalid initrwnd value %s specified for route"))
				goto err;
			new->mask |= IPROUTE_BIT_INITRWND;
#else
			log_message(LOG_INFO, "initrwnd for route not supported by kernel");
#endif
		}
		else if (!strcmp(str, "features")) {
			i++;
#ifdef RTAX_FEATURES
			if (!strcmp("ecn", vector_slot(strvec, i)))
				new->features |= RTAX_FEATURE_ECN;
			else
				log_message(LOG_INFO, "feature %s not supported", FMT_STR_VSLOT(strvec,i));
#else
			log_message(LOG_INFO, "features for route not supported by kernel");
#endif
		}
		else if (!strcmp(str, "quickack")) {
			i++;
#ifdef RTAX_QUICKACK
			if (get_u32(&val, vector_slot(strvec, i), 1, "Invalid quickack value %s specified for route"))
				goto err;
			new->quickack = val;
			new->mask |= IPROUTE_BIT_QUICKACK;
#else
			log_message(LOG_INFO, "quickack for route not supported by kernel");
#endif
		}
		else if (!strcmp(str, "congctl")) {
			i++;
#ifdef RTAX_CC_ALGO
			if (!strcmp(vector_slot(strvec, i), "lock")) {
				new->lock |= 1 << RTAX_CC_ALGO;
				i++;
			}
			str = vector_slot(strvec, i);
			new->congctl = malloc(strlen(str) + 1);
			strcpy(new->congctl, str); 
#else
			log_message(LOG_INFO, "congctl for route not supported by kernel");
#endif
		}
		else if (!strcmp(str, "pref")) {
			i++;
#ifdef _HAVE_RTA_PREF_
			if (new->family == AF_INET) {
				log_message(LOG_INFO, "pref is only valid for IPv6");
				goto err;
			}
			new->family = AF_INET6;
			str = vector_slot(strvec, i);
			if (!strcmp(str, "low"))
				new->pref = ICMPV6_ROUTER_PREF_LOW;
			else if (!strcmp(str, "medium"))
				new->pref = ICMPV6_ROUTER_PREF_MEDIUM;
			else if (!strcmp(str, "high"))
				new->pref = ICMPV6_ROUTER_PREF_HIGH;
			else if (!get_u32(&val, str, UINT8_MAX, "Invalid pref value %s specified for route"))
				new->pref = val;
			else
				goto err;
			new->mask |= IPROUTE_BIT_PREF;
#else
			log_message(LOG_INFO, "pref not supported by kernel");
#endif
		}
		/* Maintained for backward compatibility */
		else if (!strcmp(str, "or")) {
			log_message(LOG_INFO, "\"or\" for routes is deprecated. Please use \"nexthop\"");

			if (new->nhs) {
				log_message(LOG_INFO, "\"or\" route already specified - ignoring subsequent");
				i += 2;
				continue;
			}

			new->nhs = alloc_list(free_nh, NULL);

			/* Transfer the via address to the first nexthop */
			nexthop_t *nh = MALLOC(sizeof(nexthop_t));
			nh->addr = new->via;
			new->via = NULL;
			list_add(new->nhs, nh);

			/* Now handle the "or" address */
			nh = MALLOC(sizeof(nexthop_t));
			nh->addr = parse_ipaddress(NULL, vector_slot(strvec, ++i), false);
			if (!nh->addr) {
				log_message(LOG_INFO, "Invalid \"or\" address %s", FMT_STR_VSLOT(strvec, i));
				FREE(nh);
				goto err;
			}
			list_add(new->nhs, nh);
		}
		else if (!strcmp(str, "nexthop")) {
			if (new->nhs)
				log_message(LOG_INFO, "Cannot specify nexthops with \"or\" route");
			else
				do_nexthop = true;
			break;
		}
		else {
			if (!strcmp(str, "to"))
				i++;

			if (find_rttables_rtntype(str, &val8)) {
				new->type = val8;
				new->mask |= IPROUTE_BIT_TYPE;
				i++;
			}
			if (new->dst)
				FREE(new->dst);
			dst = parse_ipaddress(NULL, vector_slot(strvec, i), true);
			if (!dst) {
				log_message(LOG_INFO, "unknown route keyword %s", FMT_STR_VSLOT(strvec, i));
				goto err;
			}
			if (new->family == AF_UNSPEC)
				new->family = dst->ifa.ifa_family;
			else if (new->family != dst->ifa.ifa_family) {
				log_message(LOG_INFO, "Cannot mix IPv4 and IPv6 addresses for route");
				goto err;
			}
			new->dst = dst;
		}
		i++;
	}

	if (do_nexthop)
		parse_nexthops(strvec, i, new);
	else if (i < vector_size(strvec)) {
		log_message(LOG_INFO, "Route has trailing nonsense - %s", FMT_STR_VSLOT(strvec, i));
		goto err;
	}

	list_add(rt_list, new);

	return;

err:
	free_iproute(new);
}

/* Try to find a route in a list */
static int
route_exist(list l, ip_route_t *iproute)
{
	ip_route_t *ipr;
	element e;

	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		ipr = ELEMENT_DATA(e);

		/* The kernel's key to a route is (to, tos, preference, table) */
		if (IP_ISEQ(ipr->dst, iproute->dst) &&
		    ipr->dst->ifa.ifa_prefixlen == iproute->dst->ifa.ifa_prefixlen &&
		    (!((ipr->mask ^ iproute->mask) & IPROUTE_BIT_METRIC)) &&
		    (!(ipr->mask & IPROUTE_BIT_METRIC) ||
		     ipr->metric == iproute->metric) &&
		    ipr->table == iproute->table) {
			ipr->set = iproute->set;
			return 1;
		}
	}
	return 0;
}

/* Clear diff routes */
void
clear_diff_routes(list l, list n)
{
	ip_route_t *iproute;
	element e;

	/* No route in previous conf */
	if (LIST_ISEMPTY(l))
		return;

	/* All routes removed */
	if (LIST_ISEMPTY(n)) {
		log_message(LOG_INFO, "Removing a VirtualRoute block");
		netlink_rtlist(l, IPROUTE_DEL);
		return;
	}

	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		iproute = ELEMENT_DATA(e);
		if (iproute->set) {
			if (!route_exist(n, iproute)) {
				log_message(LOG_INFO, "ip route %s/%d ... , no longer exist"
						    , ipaddresstos(NULL, iproute->dst), iproute->dst->ifa.ifa_prefixlen);
				netlink_route(iproute, IPROUTE_DEL);
			}
			else {
				/* There are too many route options to compare to see if the
				 * routes are the same or not, so just replace the existing route
				 * with the new one. */
				netlink_route(iproute, IPROUTE_REPLACE);
			}
		}
	}
}

/* Diff conf handler */
void
clear_diff_sroutes(void)
{
	clear_diff_routes(old_vrrp_data->static_routes, vrrp_data->static_routes);
}
