/*-------------------------------------------------------------------------
 *
 * hba.h
 *	  Interface to hba.c
 *
 *
 * $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#ifndef HBA_H
#define HBA_H

#ifndef WIN32
#include <netinet/in.h>
#endif

#include "nodes/pg_list.h"

typedef enum UserAuth
{
	uaReject,
	uaKrb4,
	uaKrb5,
	uaTrust,
	uaIdent,
	uaPassword,
	uaCrypt,
	uaMD5
#ifdef USE_PAM
	,uaPAM
#endif   /* USE_PAM */
} UserAuth;

typedef struct Port hbaPort;

extern List **get_user_line(const char *user);
extern void load_hba(void);
extern void load_ident(void);
extern void load_user(void);
extern void load_group(void);
extern int	hba_getauthmethod(hbaPort *port);
extern int	authident(hbaPort *port);

#endif
