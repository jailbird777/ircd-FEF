/*
 *  ircd-ratbox: A slightly useful ircd.
 *  m_names.c: Shows the users who are online.
 *
 *  Copyright (C) 1990 Jarkko Oikarinen and University of Oulu, Co Center
 *  Copyright (C) 1996-2002 Hybrid Development Team
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
#include "channel.h"
#include "client.h"
#include "hash.h"
#include "match.h"
#include "ircd.h"
#include "numeric.h"
#include "send.h"
#include "s_serv.h"
#include "s_conf.h"
#include "msg.h"
#include "parse.h"
#include "modules.h"
#include "s_newconf.h"

static const char names_desc[] = "Provides the NAMES command to view users on a channel";

static void m_names(struct MsgBuf *, struct Client *, struct Client *, int, const char **);

struct Message names_msgtab = {
	"NAMES", 0, 0, 0, 0,
	{mg_unreg, {m_names, 0}, mg_ignore, mg_ignore, mg_ignore, {m_names, 0}}
};

mapi_clist_av1 names_clist[] = { &names_msgtab, NULL };

DECLARE_MODULE_AV2(names, NULL, NULL, names_clist, NULL, NULL, NULL, NULL, names_desc);

static void names_global(struct Client *source_p);

/************************************************************************
 * m_names() - Added by Jto 27 Apr 1989
 ************************************************************************/

/*
 * m_names
 *      parv[1] = channel
 */
static void
m_names(struct MsgBuf *msgbuf_p, struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	static time_t last_used = 0;
	struct Channel *chptr = NULL;
	char *s;

	if(parc > 1 && !EmptyString(parv[1]))
	{
		char *p = LOCAL_COPY(parv[1]);
		if((s = strchr(p, ',')))
			*s = '\0';

		if(!check_channel_name(p))
		{
			sendto_one_numeric(source_p, ERR_BADCHANNAME,
					   form_str(ERR_BADCHANNAME),
					   (unsigned char *) p);
			return;
		}

		if((chptr = find_channel(p)) != NULL)
			channel_member_names(chptr, source_p, 1);
		else
			sendto_one(source_p, form_str(RPL_ENDOFNAMES),
				   me.name, source_p->name, p);
	}
	else
	{
		if(!IsOperGeneral(source_p))
		{
			if((last_used + ConfigFileEntry.pace_wait) > rb_current_time())
			{
				sendto_one(source_p, form_str(RPL_LOAD2HI),
					   me.name, source_p->name, "NAMES");
				sendto_one(source_p, form_str(RPL_ENDOFNAMES),
					   me.name, source_p->name, "*");
				return;
			}
			else
				last_used = rb_current_time();
		}

		names_global(source_p);
		sendto_one(source_p, form_str(RPL_ENDOFNAMES),
			   me.name, source_p->name, "*");
	}
}

/*
 * names_global
 *
 * inputs       - pointer to client struct requesting names
 * output       - none
 * side effects - lists all non public non secret channels
 */
static void
names_global(struct Client *source_p)
{
	bool dont_show;
	rb_dlink_node *ptr;
	struct Client *target_p;
	struct Channel *chptr = NULL;

	/* first do all visible channels */
	RB_DLINK_FOREACH(ptr, global_channel_list.head)
	{
		chptr = ptr->data;
		channel_member_names(chptr, source_p, 0);
	}

	send_multiline_init(source_p, " ", form_str(RPL_NAMREPLY),
				    me.name,
				    source_p->name,
				    "*",
				    "*");

	/* Second, do all clients in one big sweep */
	RB_DLINK_FOREACH(ptr, global_client_list.head)
	{
		rb_dlink_node *ps, *pt;
		struct membership *ms, *mt;

		target_p = ptr->data;
		dont_show = false;

		if(!IsPerson(target_p) || IsInvisible(target_p))
			continue;

		/* we want to show -i clients that are either:
		 *   a) not on any channels
		 *   b) only on +p channels
		 *
		 * both were missed out above.  if the target is on a
		 * common channel with source, its already been shown.
		 */
		ITER_COMM_CHANNELS(ps, pt, source_p->user->channel.head, target_p->user->channel.head, ms, mt, chptr)
		{
			if (!mt) continue;

			if (PubChannel(chptr) || SecretChannel(chptr) || ms)
			{
				dont_show = true;
				break;
			}
		}

		if(dont_show)
			continue;

		if (IsCapable(source_p, CLICAP_USERHOST_IN_NAMES))
		{
			send_multiline_item(source_p, "%s!%s@%s",
					target_p->name,
					target_p->username,
					target_p->host);
		}
		else
		{
			send_multiline_item(source_p, "%s", target_p->name);
		}
	}

	send_multiline_fini(source_p, NULL);
}
