/*
 * Remote client nick change notices.
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
	"Adds server notices for remote nick changes";

static int _modinit(void);
static void h_gnc_nick_change(void *data);

mapi_hfn_list_av1 gcn_hfnlist[] = {
	{ "remote_nick_change", h_gnc_nick_change },
	{ NULL, NULL }
};

DECLARE_MODULE_AV2(globalnickchange, _modinit, NULL, NULL, NULL, gcn_hfnlist, NULL, NULL, sno_desc);

static int
_modinit(void)
{
	return 0;
}

static void
h_gnc_nick_change(void *data_)
{
	hook_data *data = data_;
	struct Client *source_p = data->client;
	const char *oldnick = data->arg1;
	const char *newnick = data->arg2;

	sendto_realops_snomask_from(SNO_NCHANGE, L_ALL, source_p->servptr,
				"Nick change: From %s to %s [%s@%s]",
				oldnick, newnick, source_p->username, source_p->host);
}
