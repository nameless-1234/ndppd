/*
 * This file is part of ndppd.
 *
 * Copyright (C) 2011-2019  Daniel Adolfsson <daniel@ashen.se>
 *
 * ndppd is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ndppd is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ndppd.  If not, see <https://www.gnu.org/licenses/>.
 */
#include <errno.h>
#include <linux/rtnetlink.h>
#include <string.h>
#include <sys/socket.h>

#include "addr.h"
#include "ndppd.h"
#include "rtnl.h"
#include "sio.h"

static nd_sio_t *ndL_sio;
static nd_rtnl_route_t *ndL_routes, *ndL_free_routes;
static nd_rtnl_addr_t *ndL_addrs, *ndL_free_addrs;

long nd_rtnl_dump_timeout;

/*
 * This will ensure the linked list is kept sorted, so it will be easier to find a match.
 */
static void ndL_insert_route(nd_rtnl_route_t *route)
{
    nd_rtnl_route_t *prev = NULL;

    ND_LL_FOREACH(ndL_routes, cur, next)
    {
        if (route->pflen >= cur->pflen && route->metrics <= cur->metrics)
            break;

        prev = cur;
    }

    if (prev)
    {
        route->next = prev->next;
        prev->next = route;
    }
    else
    {
        ND_LL_PREPEND(ndL_routes, route, next);
    }
}

static void ndL_handle_newaddress(struct ifaddrmsg *msg, int length)
{
    nd_addr_t *addr = NULL;

    for (struct rtattr *rta = IFA_RTA(msg); RTA_OK(rta, length); rta = RTA_NEXT(rta, length))
    {
        if (rta->rta_type == IFA_ADDRESS)
            addr = (nd_addr_t *)RTA_DATA(rta);
    }

    if (!addr)
        return;

    nd_rtnl_addr_t *rt_addr;

    ND_LL_FOREACH_NODEF(ndL_addrs, rt_addr, next)
    {
        if (nd_addr_eq(&rt_addr->addr, addr) && rt_addr->pflen == msg->ifa_prefixlen)
            return;
    }

    if ((rt_addr = ndL_free_addrs))
        ND_LL_DELETE(ndL_free_addrs, rt_addr, next);
    else
        rt_addr = ND_ALLOC(nd_rtnl_addr_t);

    ND_LL_PREPEND(ndL_addrs, rt_addr, next);

    rt_addr->pflen = msg->ifa_prefixlen;
    rt_addr->iif = msg->ifa_index;
    rt_addr->addr = *addr;

    nd_log_debug("rtnl: NEWADDR %s/%d if %d", nd_addr_to_string(addr), msg->ifa_prefixlen, msg->ifa_index);
}

static void ndL_handle_delroute(struct rtmsg *msg, int rtl)
{
    nd_addr_t *dst = NULL;
    int oif = 0;

    for (struct rtattr *rta = RTM_RTA(msg); RTA_OK(rta, rtl); rta = RTA_NEXT(rta, rtl))
    {
        if (rta->rta_type == RTA_OIF)
            oif = *(int *)RTA_DATA(rta);
        else if (rta->rta_type == RTA_DST)
            dst = (nd_addr_t *)RTA_DATA(rta);
    }

    if (!dst || !oif)
        return;

    ND_LL_FOREACH(ndL_routes, route, next)
    {
        if (nd_addr_eq(&route->addr, dst) && route->pflen == msg->rtm_dst_len && route->table == msg->rtm_table)
        {
            nd_log_debug("rtnl: DELROUTE %s/%d dev %d table %d", nd_addr_to_string(dst), msg->rtm_dst_len, oif,
                         msg->rtm_table);
            ND_LL_DELETE(ndL_routes, route, next);
            ND_LL_PREPEND(ndL_free_routes, route, next);
            return;
        }
    }
}

static void ndL_handle_newroute(struct rtmsg *msg, int rtl)
{
    nd_addr_t *dst = NULL;
    int oif = 0;
    int metrics = 0;

    for (struct rtattr *rta = RTM_RTA(msg); RTA_OK(rta, rtl); rta = RTA_NEXT(rta, rtl))
    {
        if (rta->rta_type == RTA_OIF)
            oif = *(int *)RTA_DATA(rta);
        else if (rta->rta_type == RTA_DST)
            dst = (nd_addr_t *)RTA_DATA(rta);
        else if (rta->rta_type == RTA_METRICS)
            metrics = *(int *)RTA_DATA(rta);
    }

    if (!dst || !oif)
        return;

    nd_rtnl_route_t *route;

    ND_LL_FOREACH_NODEF(ndL_routes, route, next)
    {
        if (nd_addr_eq(&route->addr, dst) && route->pflen == msg->rtm_dst_len && route->table == msg->rtm_table)
            return;
    }

    if ((route = ndL_free_routes))
        ND_LL_DELETE(ndL_free_routes, route, next);
    else
        route = ND_ALLOC(nd_rtnl_route_t);

    route->addr = *dst;
    route->pflen = msg->rtm_dst_len;
    route->oif = oif;
    route->table = msg->rtm_table;
    route->metrics = metrics;

    ndL_insert_route(route);

    nd_log_debug("rtnl: NEWROUTE %s/%d dev %d table %d", nd_addr_to_string(dst), msg->rtm_dst_len, oif, msg->rtm_table);
}

static void ndL_sio_handler(__attribute__((unused)) nd_sio_t *unused1, __attribute__((unused)) int unused2)
{
    uint8_t buf[4096];

    for (;;)
    {
        ssize_t len = nd_sio_recv(ndL_sio, NULL, 0, buf, sizeof(buf));

        if (len < 0)
            /* Failed. */
            return;

        for (struct nlmsghdr *hdr = (struct nlmsghdr *)buf; NLMSG_OK(hdr, len); hdr = NLMSG_NEXT(hdr, len))
        {
            if (hdr->nlmsg_type == NLMSG_DONE)
            {
                nd_rtnl_dump_timeout = 0;
                break;
            }

            if (hdr->nlmsg_type == NLMSG_ERROR)
            {
                struct nlmsgerr *e = (struct nlmsgerr *)NLMSG_DATA(hdr);
                nd_log_error("rtnl: Error \"%s\", type=%d", strerror(-e->error), e->msg.nlmsg_type);
                continue;
            }

            if (hdr->nlmsg_type == RTM_NEWROUTE)
                ndL_handle_newroute((struct rtmsg *)NLMSG_DATA(hdr), RTM_PAYLOAD(hdr));
            else if (hdr->nlmsg_type == RTM_DELROUTE)
                ndL_handle_delroute((struct rtmsg *)NLMSG_DATA(hdr), RTM_PAYLOAD(hdr));
            else if (hdr->nlmsg_type == RTM_NEWADDR)
                ndL_handle_newaddress((struct ifaddrmsg *)NLMSG_DATA(hdr), IFA_PAYLOAD(hdr));
        }
    }
}

bool nd_rtnl_open()
{
    if (ndL_sio != NULL)
        return true;

    if (!(ndL_sio = nd_sio_open(PF_NETLINK, SOCK_RAW, NETLINK_ROUTE)))
    {
        nd_log_error("Failed to open netlink socket: %s", strerror(errno));
        return false;
    }

    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = (1 << (RTNLGRP_IPV6_IFADDR - 1)) | (1 << (RTNLGRP_IPV6_ROUTE - 1));

    if (!nd_sio_bind(ndL_sio, (struct sockaddr *)&addr, sizeof(addr)))
    {
        nd_log_error("Failed to bind netlink socket: %s", strerror(errno));
        nd_sio_close(ndL_sio);
        ndL_sio = NULL;
        return false;
    }

    ndL_sio->handler = ndL_sio_handler;

    return true;
}

void nd_rtnl_cleanup()
{
    if (ndL_sio)
        nd_sio_close(ndL_sio);
}

bool nd_rtnl_query_routes()
{
    if (nd_rtnl_dump_timeout)
        return false;

    struct
    {
        struct nlmsghdr hdr;
        struct rtmsg msg;
    } req;

    memset(&req, 0, sizeof(req));

    req.hdr.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    req.hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.hdr.nlmsg_type = RTM_GETROUTE;

    req.msg.rtm_protocol = RTPROT_UNSPEC;
    req.msg.rtm_table = RT_TABLE_UNSPEC;
    req.msg.rtm_family = AF_INET6;

    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;

    nd_rtnl_dump_timeout = nd_current_time + 5000;

    nd_sio_send(ndL_sio, (struct sockaddr *)&addr, sizeof(addr), &req, sizeof(req));
    return false;
}

bool nd_rtnl_query_addresses()
{
    if (nd_rtnl_dump_timeout)
        return false;

    struct
    {
        struct nlmsghdr hdr;
        struct ifaddrmsg msg;
    } req;

    memset(&req, 0, sizeof(req));

    req.hdr.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    req.hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.hdr.nlmsg_type = RTM_GETADDR;
    req.hdr.nlmsg_seq = 1;

    req.msg.ifa_family = AF_INET6;

    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;

    nd_rtnl_dump_timeout = nd_current_time + 5000;

    nd_sio_send(ndL_sio, (struct sockaddr *)&addr, sizeof(addr), &req, sizeof(req));
    return false;
}

nd_rtnl_route_t *nd_rtnl_find_route(nd_addr_t *addr, int table)
{
    ND_LL_FOREACH(ndL_routes, route, next)
    {
        if (nd_addr_match(&route->addr, addr, route->pflen) && route->table == table)
            return route;
    }

    return NULL;
}