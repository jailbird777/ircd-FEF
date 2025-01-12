/*
 *  ircd-ratbox: A slightly useful ircd.
 *  m_resv.c: Reserves(jupes) a nickname or channel.
 *
 *  Copyright (C) 2001-2002 Hybrid Development Team
 *  Copyright (C) 2002-2005 ircd-ratbox development team
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
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
 *  USA
 */

#include "stdinc.h"
#include "client.h"
#include "channel.h"
#include "ircd.h"
#include "numeric.h"
#include "s_serv.h"
#include "send.h"
#include "msg.h"
#include "parse.h"
#include "modules.h"
#include "s_conf.h"
#include "s_newconf.h"
#include "hash.h"
#include "logger.h"
#include "bandbi.h"
#include "operhash.h"

static const char resv_desc[] =
	"Provides management of reserved nicknames and channels using (UN)RESV";

static void mo_resv(struct MsgBuf *, struct Client *, struct Client *, int, const char **);
static void ms_resv(struct MsgBuf *, struct Client *, struct Client *, int, const char **);
static void me_resv(struct MsgBuf *, struct Client *, struct Client *, int, const char **);
static void mo_unresv(struct MsgBuf *, struct Client *, struct Client *, int, const char **);
static void ms_unresv(struct MsgBuf *, struct Client *, struct Client *, int, const char **);
static void me_unresv(struct MsgBuf *, struct Client *, struct Client *, int, const char **);

struct Message resv_msgtab = {
	"RESV", 0, 0, 0, 0,
	{mg_ignore, mg_not_oper, {ms_resv, 4}, {ms_resv, 4}, {me_resv, 5}, {mo_resv, 3}}
};

struct Message unresv_msgtab = {
	"UNRESV", 0, 0, 0, 0,
	{mg_ignore, mg_not_oper, {ms_unresv, 3}, {ms_unresv, 3}, {me_unresv, 2}, {mo_unresv, 2}}
};

mapi_clist_av1 resv_clist[] = { &resv_msgtab, &unresv_msgtab, NULL };

DECLARE_MODULE_AV2(resv, NULL, NULL, resv_clist, NULL, NULL, NULL, NULL, resv_desc);

static void parse_resv(struct Client *source_p, const char *name,
		       const char *reason, int temp_time, int propagated);
static void propagate_resv(struct Client *source_p, const char *target,
			   int temp_time, const char *name, const char *reason);
static void cluster_resv(struct Client *source_p, int temp_time,
			 const char *name, const char *reason);

static void remove_resv(struct Client *source_p, const char *name, int propagated);

/*
 * mo_resv()
 *      parv[1] = channel/nick to forbid
 *      parv[2] = reason
 */
static void
mo_resv(struct MsgBuf *msgbuf_p, struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	const char *name;
	const char *reason;
	const char *target_server = NULL;
	int temp_time;
	int loc = 1;
	int propagated = 1;

	if(!IsOperResv(source_p))
	{
		sendto_one(source_p, form_str(ERR_NOPRIVS), me.name, source_p->name, "resv");
		return;
	}

	/* RESV [time] <name> [ON <server>] :<reason> */

	if((temp_time = valid_temp_time(parv[loc])) >= 0)
		loc++;
	/* we just set temp_time to -1! */
	else
		temp_time = 0;

	name = parv[loc];
	loc++;

	if((parc >= loc + 2) && (irccmp(parv[loc], "ON") == 0))
	{
		if(!IsOperRemoteBan(source_p))
		{
			sendto_one(source_p, form_str(ERR_NOPRIVS),
				   me.name, source_p->name, "remoteban");
			return;
		}

		target_server = parv[loc + 1];
		loc += 2;

		/* Set as local-only. */
		propagated = 0;
	}

	if(parc <= loc || EmptyString(parv[loc]))
	{
		sendto_one(source_p, form_str(ERR_NEEDMOREPARAMS), me.name, source_p->name, "RESV");
		return;
	}

	reason = parv[loc];

	/* remote resv.. */
	if(target_server)
	{
		if (temp_time)
			sendto_realops_snomask(SNO_GENERAL, L_NETWIDE, "%s is adding a %d min. RESV for [%s] on %s [%s]",
					get_oper_name(source_p), temp_time / 60, name, target_server, reason);
		else
			sendto_realops_snomask(SNO_GENERAL, L_NETWIDE, "%s is adding a permanent RESV for [%s] on %s [%s]",
					get_oper_name(source_p), name, target_server, reason);

		propagate_resv(source_p, target_server, temp_time, name, reason);

		if(match(target_server, me.name) == 0)
			return;
	}
	else if(!propagated)
		cluster_resv(source_p, temp_time, name, reason);

	if(propagated && temp_time == 0)
	{
		sendto_one_notice(source_p, ":Cannot set a permanent global ban");
		return;
	}

	parse_resv(source_p, name, reason, temp_time, propagated);
}

/* ms_resv()
 *     parv[1] = target server
 *     parv[2] = channel/nick to forbid
 *     parv[3] = reason
 */
static void
ms_resv(struct MsgBuf *msgbuf_p, struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	/* parv[0]  parv[1]        parv[2]  parv[3]
	 * oper     target server  resv     reason
	 */
	propagate_resv(source_p, parv[1], 0, parv[2], parv[3]);

	if(!match(parv[1], me.name))
		return;

	if(!IsPerson(source_p))
		return;

	parse_resv(source_p, parv[2], parv[3], 0, 0);
}

static void
me_resv(struct MsgBuf *msgbuf_p, struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	/* time name 0 :reason */
	if(!IsPerson(source_p))
		return;

	parse_resv(source_p, parv[2], parv[4], atoi(parv[1]), 0);
}

/* parse_resv()
 *
 * inputs       - source_p if error messages wanted
 * 		- thing to resv
 * 		- reason for resv
 * outputs	-
 * side effects - will parse the resv and create it if valid
 */
static void
parse_resv(struct Client *source_p, const char *name, const char *reason, int temp_time, int propagated)
{
	struct ConfItem *aconf;

	if(IsChannelName(name))
	{
		if(hash_find_resv(name))
		{
			sendto_one_notice(source_p,
					  ":A RESV has already been placed on channel: %s", name);
			return;
		}

		if(strlen(name) > CHANNELLEN)
		{
			sendto_one_notice(source_p, ":Invalid RESV length: %s", name);
			return;
		}

		aconf = make_conf();
		aconf->status = CONF_RESV_CHANNEL;
		aconf->port = 0;
		aconf->created = rb_current_time();
		aconf->host = rb_strdup(name);
		aconf->passwd = rb_strdup(reason);
		aconf->info.oper = operhash_add(get_oper_name(source_p));

		if(propagated)
		{
			aconf->flags |= CONF_FLAGS_MYOPER | CONF_FLAGS_TEMPORARY;
			aconf->hold = rb_current_time() + temp_time;
			aconf->lifetime = aconf->hold;
			replace_old_ban(aconf);
			add_prop_ban(aconf);

			sendto_realops_snomask(SNO_GENERAL, L_ALL,
					       "%s added global %d min. RESV for [%s] [%s]",
					       get_oper_name(source_p), temp_time / 60,
					       name, reason);
			ilog(L_KLINE, "R %s %d %s %s",
			     get_oper_name(source_p), temp_time / 60, name, reason);
			sendto_one_notice(source_p, ":Added global %d min. RESV [%s]",
					  temp_time / 60, name);
			sendto_server(NULL, NULL, CAP_BAN|CAP_TS6, NOCAPS,
					":%s BAN R * %s %lu %d %d * :%s",
					source_p->id, aconf->host,
					(unsigned long)aconf->created,
					(int)(aconf->hold - aconf->created),
					(int)(aconf->lifetime - aconf->created),
					reason);
		}
		else if(temp_time > 0)
		{
			aconf->hold = rb_current_time() + temp_time;

			sendto_realops_snomask(SNO_GENERAL, L_ALL,
					       "%s added temporary %d min. RESV for [%s] [%s]",
					       get_oper_name(source_p), temp_time / 60,
					       name, reason);
			ilog(L_KLINE, "R %s %d %s %s",
			     get_oper_name(source_p), temp_time / 60, name, reason);
			sendto_one_notice(source_p, ":Added temporary %d min. RESV [%s]",
					  temp_time / 60, name);
		}
		else
		{
			sendto_realops_snomask(SNO_GENERAL, L_ALL,
					       "%s added RESV for [%s] [%s]",
					       get_oper_name(source_p), name, reason);
			ilog(L_KLINE, "R %s 0 %s %s",
			     get_oper_name(source_p), name, reason);
			sendto_one_notice(source_p, ":Added RESV [%s]", name);

			bandb_add(BANDB_RESV, source_p, aconf->host, NULL, aconf->passwd, NULL, 0);
		}

		add_to_resv_hash(aconf->host, aconf);
		resv_chan_forcepart(aconf->host, aconf->passwd, temp_time);
	}
	else if(clean_resv_nick(name))
	{
		if(strlen(name) > NICKLEN * 2)
		{
			sendto_one_notice(source_p, ":Invalid RESV length: %s", name);
			return;
		}

		if(!valid_wild_card_simple(name))
		{
			sendto_one_notice(source_p,
					  ":Please include at least %d non-wildcard "
					  "characters with the resv",
					  ConfigFileEntry.min_nonwildcard_simple);
			return;
		}

		if(find_nick_resv_mask(name))
		{
			sendto_one_notice(source_p,
					  ":A RESV has already been placed on nick: %s", name);
			return;
		}

		aconf = make_conf();
		aconf->status = CONF_RESV_NICK;
		aconf->port = 0;
		aconf->created = rb_current_time();
		aconf->host = rb_strdup(name);
		aconf->passwd = rb_strdup(reason);
		aconf->info.oper = operhash_add(get_oper_name(source_p));

		if(propagated)
		{
			aconf->flags |= CONF_FLAGS_MYOPER | CONF_FLAGS_TEMPORARY;
			aconf->hold = rb_current_time() + temp_time;
			aconf->lifetime = aconf->hold;
			replace_old_ban(aconf);
			add_prop_ban(aconf);

			sendto_realops_snomask(SNO_GENERAL, L_ALL,
					       "%s added global %d min. RESV for [%s] [%s]",
					       get_oper_name(source_p), temp_time / 60,
					       name, reason);
			ilog(L_KLINE, "R %s %d %s %s",
			     get_oper_name(source_p), temp_time / 60, name, reason);
			sendto_one_notice(source_p, ":Added global %d min. RESV [%s]",
					  temp_time / 60, name);
			sendto_server(NULL, NULL, CAP_BAN|CAP_TS6, NOCAPS,
					":%s BAN R * %s %lu %d %d * :%s",
					source_p->id, aconf->host,
					(unsigned long)aconf->created,
					(int)(aconf->hold - aconf->created),
					(int)(aconf->lifetime - aconf->created),
					reason);
		}
		else if(temp_time > 0)
		{
			aconf->hold = rb_current_time() + temp_time;

			sendto_realops_snomask(SNO_GENERAL, L_ALL,
					       "%s added temporary %d min. RESV for [%s] [%s]",
					       get_oper_name(source_p), temp_time / 60,
					       name, reason);
			ilog(L_KLINE, "R %s %d %s %s",
			     get_oper_name(source_p), temp_time / 60, name, reason);
			sendto_one_notice(source_p, ":Added temporary %d min. RESV [%s]",
					  temp_time / 60, name);
		}
		else
		{
			sendto_realops_snomask(SNO_GENERAL, L_ALL,
					       "%s added RESV for [%s] [%s]",
					       get_oper_name(source_p), name, reason);
			ilog(L_KLINE, "R %s 0 %s %s",
			     get_oper_name(source_p), name, reason);
			sendto_one_notice(source_p, ":Added RESV [%s]", name);

			bandb_add(BANDB_RESV, source_p, aconf->host, NULL, aconf->passwd, NULL, 0);
		}

		rb_dlinkAddAlloc(aconf, &resv_conf_list);
		resv_nick_fnc(aconf->host, aconf->passwd, temp_time);
	}
	else
		sendto_one_notice(source_p, ":You have specified an invalid resv: [%s]", name);
}

static void
propagate_resv(struct Client *source_p, const char *target,
	       int temp_time, const char *name, const char *reason)
{
	if(!temp_time)
	{
		sendto_match_servs(source_p, target,
				   CAP_CLUSTER, NOCAPS, "RESV %s %s :%s", target, name, reason);
		sendto_match_servs(source_p, target,
				   CAP_ENCAP, CAP_CLUSTER,
				   "ENCAP %s RESV %d %s 0 :%s", target, temp_time, name, reason);
	}
	else
		sendto_match_servs(source_p, target,
				   CAP_ENCAP, NOCAPS,
				   "ENCAP %s RESV %d %s 0 :%s", target, temp_time, name, reason);
}

static void
cluster_resv(struct Client *source_p, int temp_time, const char *name, const char *reason)
{
	/* old protocol cant handle temps, and we dont really want
	 * to convert them to perm.. --fl
	 */
	if(!temp_time)
	{
		sendto_server(source_p, NULL,
				   CAP_CLUSTER, NOCAPS,
				   "RESV * %s :%s", name, reason);
		sendto_server(source_p, NULL,
				   CAP_ENCAP, CAP_CLUSTER,
				   "ENCAP * RESV 0 %s 0 :%s",
				   name, reason);
	}
	else
		sendto_server(source_p, NULL,
				   CAP_ENCAP, NOCAPS,
				   "ENCAP * RESV %d %s 0 :%s",
				   temp_time, name, reason);
}


/*
 * mo_unresv()
 *     parv[1] = channel/nick to unforbid
 */
static void
mo_unresv(struct MsgBuf *msgbuf_p, struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	int propagated = 1;

	if(!IsOperResv(source_p))
	{
		sendto_one(source_p, form_str(ERR_NOPRIVS), me.name, source_p->name, "resv");
		return;
	}

	if((parc == 4) && (irccmp(parv[2], "ON") == 0))
	{
		if(!IsOperRemoteBan(source_p))
		{
			sendto_one(source_p, form_str(ERR_NOPRIVS),
				   me.name, source_p->name, "remoteban");
			return;
		}

		propagate_generic(source_p, "UNRESV", parv[3], CAP_CLUSTER, "%s", parv[1]);

		if(match(parv[3], me.name) == 0)
			return;

		propagated = 0;
	}

	remove_resv(source_p, parv[1], propagated);
}

/* ms_unresv()
 *     parv[1] = target server
 *     parv[2] = resv to remove
 */
static void
ms_unresv(struct MsgBuf *msgbuf_p, struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	/* parv[0]  parv[1]        parv[2]
	 * oper     target server  resv to remove
	 */
	propagate_generic(source_p, "UNRESV", parv[1], CAP_CLUSTER, "%s", parv[2]);

	if(!match(parv[1], me.name))
		return;

	if(!IsPerson(source_p))
		return;

	remove_resv(source_p, parv[2], 0);
}

static void
me_unresv(struct MsgBuf *msgbuf_p, struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	/* name */
	if(!IsPerson(source_p))
		return;

	remove_resv(source_p, parv[1], 0);
}

static void
remove_resv(struct Client *source_p, const char *name, int propagated)
{
	struct ConfItem *aconf = NULL;
	rb_dlink_node *ptr;
	time_t now;

	if(IsChannelName(name))
	{
		if((aconf = hash_find_resv(name)) == NULL)
		{
			if(propagated)
				cluster_generic(source_p, "UNRESV", SHARED_UNRESV, CAP_CLUSTER, "%s", name);

			sendto_one_notice(source_p, ":No RESV for %s", name);
			return;
		}

		if(aconf->lifetime)
		{
			if(!propagated)
			{
				sendto_one_notice(source_p, ":Cannot remove global RESV %s on specific servers", name);
				return;
			}
			if (!lookup_prop_ban(aconf))
				return;
			sendto_one_notice(source_p, ":RESV for [%s] is removed", name);
			sendto_realops_snomask(SNO_GENERAL, L_ALL,
					       "%s has removed the global RESV for: [%s]",
					       get_oper_name(source_p), name);
			ilog(L_KLINE, "UR %s %s", get_oper_name(source_p), name);
			now = rb_current_time();
			if(aconf->created < now)
				aconf->created = now;
			else
				aconf->created++;
			aconf->hold = aconf->created;
			operhash_delete(aconf->info.oper);
			aconf->info.oper = operhash_add(get_oper_name(source_p));
			aconf->flags |= CONF_FLAGS_MYOPER | CONF_FLAGS_TEMPORARY;
			sendto_server(NULL, NULL, CAP_BAN|CAP_TS6, NOCAPS,
					":%s BAN R * %s %lu %d %d * :*",
					source_p->id, aconf->host,
					(unsigned long)aconf->created,
					0,
					(int)(aconf->lifetime - aconf->created));
			deactivate_conf(aconf, now);
			return;
		}
		else if(propagated)
			cluster_generic(source_p, "UNRESV", SHARED_UNRESV, CAP_CLUSTER, "%s", name);

		sendto_one_notice(source_p, ":RESV for [%s] is removed", name);
		ilog(L_KLINE, "UR %s %s", get_oper_name(source_p), name);
		if(!aconf->hold)
		{
			bandb_del(BANDB_RESV, aconf->host, NULL);
			sendto_realops_snomask(SNO_GENERAL, L_ALL,
					       "%s has removed the RESV for: [%s]",
					       get_oper_name(source_p), name);
		}
		else
		{
			sendto_realops_snomask(SNO_GENERAL, L_ALL,
					       "%s has removed the temporary RESV for: [%s]",
					       get_oper_name(source_p), name);
		}
		del_from_resv_hash(name, aconf);
	}
	else
	{
		RB_DLINK_FOREACH(ptr, resv_conf_list.head)
		{
			aconf = ptr->data;

			if(irccmp(aconf->host, name))
				aconf = NULL;
			else
				break;
		}

		if(aconf == NULL)
		{
			if(propagated)
				cluster_generic(source_p, "UNRESV", SHARED_UNRESV, CAP_CLUSTER, "%s", name);

			sendto_one_notice(source_p, ":No RESV for %s", name);
			return;
		}

		if(aconf->lifetime)
		{
			if(!propagated)
			{
				sendto_one_notice(source_p, ":Cannot remove global RESV %s on specific servers", name);
				return;
			}
			if (!lookup_prop_ban(aconf))
				return;
			sendto_one_notice(source_p, ":RESV for [%s] is removed", name);
			sendto_realops_snomask(SNO_GENERAL, L_ALL,
					       "%s has removed the global RESV for: [%s]",
					       get_oper_name(source_p), name);
			ilog(L_KLINE, "UR %s %s", get_oper_name(source_p), name);
			now = rb_current_time();
			if(aconf->created < now)
				aconf->created = now;
			else
				aconf->created++;
			aconf->hold = aconf->created;
			operhash_delete(aconf->info.oper);
			aconf->info.oper = operhash_add(get_oper_name(source_p));
			aconf->flags |= CONF_FLAGS_MYOPER | CONF_FLAGS_TEMPORARY;
			sendto_server(NULL, NULL, CAP_BAN|CAP_TS6, NOCAPS,
					":%s BAN R * %s %lu %d %d * :*",
					source_p->id, aconf->host,
					(unsigned long)aconf->created,
					0,
					(int)(aconf->lifetime - aconf->created));
			deactivate_conf(aconf, now);
			return;
		}
		else if(propagated)
			cluster_generic(source_p, "UNRESV", SHARED_UNRESV, CAP_CLUSTER, "%s", name);

		sendto_one_notice(source_p, ":RESV for [%s] is removed", name);
		ilog(L_KLINE, "UR %s %s", get_oper_name(source_p), name);
		if(!aconf->hold)
		{
			bandb_del(BANDB_RESV, aconf->host, NULL);
			sendto_realops_snomask(SNO_GENERAL, L_ALL,
					       "%s has removed the RESV for: [%s]",
					       get_oper_name(source_p), name);
		}
		else
		{
			sendto_realops_snomask(SNO_GENERAL, L_ALL,
					       "%s has removed the temporary RESV for: [%s]",
					       get_oper_name(source_p), name);
		}
		/* already have ptr from the loop above.. */
		rb_dlinkDestroy(ptr, &resv_conf_list);
	}
	free_conf(aconf);

	return;
}
