/*
 * Remote client connect/exit notices on snomask +F (far).
 * To avoid flooding, connects/exits part of netjoins/netsplits are not shown.
 * Consequently, it is not possible to use these notices to keep track
 * of all clients.
 * -- jilles
 */

#include "stdinc.h"
#include "modules.h"
#include "client.h"
#include "hook.h"
#include "ircd.h"
#include "send.h"
#include "s_conf.h"
#include "snomask.h"

static const char sno_desc[] =
	"Adds server notice mask +F that allows operators to receive notices for connections on other servers";

static int _modinit(void);
static void _moddeinit(void);
static void h_gcn_new_remote_user(void *);
static void h_gcn_client_exit(void *);

mapi_hfn_list_av1 gcn_hfnlist[] = {
	{ "new_remote_user", h_gcn_new_remote_user },
	{ "client_exit", h_gcn_client_exit },
	{ NULL, NULL }
};

DECLARE_MODULE_AV2(globalconnexit, _modinit, _moddeinit, NULL, NULL, gcn_hfnlist, NULL, NULL, sno_desc);

static int
_modinit(void)
{
	/* add the snomask to the available slot */
	snomask_modes['F'] = find_snomask_slot();

	return 0;
}

static void
_moddeinit(void)
{
	/* disable the snomask and remove it from the available list */
	snomask_modes['F'] = 0;
}

static void
h_gcn_new_remote_user(void *data)
{
	struct Client *source_p = data;

	if (!HasSentEob(source_p->servptr))
		return;
	sendto_realops_snomask_from(snomask_modes['F'], L_ALL, source_p->servptr,
			"Client connecting: %s (%s@%s) [%s] {%s} <%s> [%s]",
			source_p->name, source_p->username, source_p->orighost,
			show_ip(NULL, source_p) ? source_p->sockhost : "255.255.255.255",
			"?", *source_p->user->suser ? source_p->user->suser : "*",
			source_p->info);
}

static void
h_gcn_client_exit(void *data)
{
	hook_data_client_exit *hdata = data;
	struct Client *source_p;

	source_p = hdata->target;

	if (MyConnect(source_p) || !IsClient(source_p))
		return;
	if (!HasSentEob(source_p->servptr))
		return;
	sendto_realops_snomask_from(snomask_modes['F'], L_ALL, source_p->servptr,
			     "Client exiting: %s (%s@%s) [%s] [%s]",
			     source_p->name,
			     source_p->username, source_p->host, hdata->comment,
                             show_ip(NULL, source_p) ? source_p->sockhost : "255.255.255.255");
}
