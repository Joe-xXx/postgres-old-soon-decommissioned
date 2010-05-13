/**********************************************************************
 * plperl.c - perl as a procedural language for PostgreSQL
 *
 * IDENTIFICATION
 *
 *	  This software is copyrighted by Mark Hollomon
 *	 but is shameless cribbed from pltcl.c by Jan Weick.
 *
 *	  The author hereby grants permission  to  use,  copy,	modify,
 *	  distribute,  and	license this software and its documentation
 *	  for any purpose, provided that existing copyright notices are
 *	  retained	in	all  copies  and  that	this notice is included
 *	  verbatim in any distributions. No written agreement, license,
 *	  or  royalty  fee	is required for any of the authorized uses.
 *	  Modifications to this software may be  copyrighted  by  their
 *	  author  and  need  not  follow  the licensing terms described
 *	  here, provided that the new terms are  clearly  indicated  on
 *	  the first page of each file where they apply.
 *
 *	  IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY
 *	  PARTY  FOR  DIRECT,	INDIRECT,	SPECIAL,   INCIDENTAL,	 OR
 *	  CONSEQUENTIAL   DAMAGES  ARISING	OUT  OF  THE  USE  OF  THIS
 *	  SOFTWARE, ITS DOCUMENTATION, OR ANY DERIVATIVES THEREOF, EVEN
 *	  IF  THE  AUTHOR  HAVE BEEN ADVISED OF THE POSSIBILITY OF SUCH
 *	  DAMAGE.
 *
 *	  THE  AUTHOR  AND	DISTRIBUTORS  SPECIFICALLY	 DISCLAIM	ANY
 *	  WARRANTIES,  INCLUDING,  BUT	NOT  LIMITED  TO,  THE	IMPLIED
 *	  WARRANTIES  OF  MERCHANTABILITY,	FITNESS  FOR  A  PARTICULAR
 *	  PURPOSE,	AND NON-INFRINGEMENT.  THIS SOFTWARE IS PROVIDED ON
 *	  AN "AS IS" BASIS, AND THE AUTHOR	AND  DISTRIBUTORS  HAVE  NO
 *	  OBLIGATION   TO	PROVIDE   MAINTENANCE,	 SUPPORT,  UPDATES,
 *	  ENHANCEMENTS, OR MODIFICATIONS.
 *
 * IDENTIFICATION
 *	  $PostgreSQL$
 *
 **********************************************************************/

#include "postgres.h"

/* system stuff */
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <locale.h>

/* postgreSQL stuff */
#include "commands/trigger.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"
#include "utils/hsearch.h"

/* perl stuff */

/* stop perl from hijacking stdio and other stuff */
#ifdef WIN32
#define WIN32IO_IS_STDIO
#endif

#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"
#include "ppport.h"

/* just in case these symbols aren't provided */
#ifndef pTHX_
#define pTHX_
#define pTHX void
#endif

/* perl may have a different width of "bool", don't buy it */
#ifdef bool
#undef bool
#endif

/* defines PLPERL_SET_OPMASK */
#include "plperl_opmask.h"


/**********************************************************************
 * The information we cache about loaded procedures
 **********************************************************************/
typedef struct plperl_proc_desc
{
	char	   *proname;
	TransactionId fn_xmin;
	CommandId	fn_cmin;
	bool		fn_readonly;
	bool		lanpltrusted;
	bool		fn_retistuple;	/* true, if function returns tuple */
	bool		fn_retisset;	/* true, if function returns set */
	Oid			result_oid;		/* Oid of result type */
	FmgrInfo	result_in_func; /* I/O function and arg for result type */
	Oid			result_typioparam;
	int			nargs;
	FmgrInfo	arg_out_func[FUNC_MAX_ARGS];
	Oid			arg_typioparam[FUNC_MAX_ARGS];
	bool		arg_is_rowtype[FUNC_MAX_ARGS];
	SV		   *reference;
} plperl_proc_desc;

/**********************************************************************
 * Global data
 **********************************************************************/

typedef enum
{
	INTERP_NONE,
	INTERP_HELD,
	INTERP_TRUSTED,
	INTERP_UNTRUSTED,
	INTERP_BOTH
} InterpState;

static InterpState interp_state = INTERP_NONE;
static bool can_run_two = false;

static int	plperl_firstcall = 1;
static bool plperl_safe_init_done = false;
static PerlInterpreter *plperl_trusted_interp = NULL;
static PerlInterpreter *plperl_untrusted_interp = NULL;
static PerlInterpreter *plperl_held_interp = NULL;
static OP  *(*pp_require_orig) (pTHX) = NULL;
static OP  *pp_require_safe(pTHX);
static bool trusted_context;
static HTAB *plperl_proc_hash = NULL;
static char plperl_opmask[MAXO];
static void set_interp_require(void);

/* this is saved and restored by plperl_call_handler */
static plperl_proc_desc *plperl_current_prodesc = NULL;

/**********************************************************************
 * Forward declarations
 **********************************************************************/
static void plperl_init_all(void);
static void plperl_init_interp(void);

Datum		plperl_call_handler(PG_FUNCTION_ARGS);
void		plperl_init(void);

HV		   *plperl_spi_exec(char *query, int limit);

static Datum plperl_func_handler(PG_FUNCTION_ARGS);

static Datum plperl_trigger_handler(PG_FUNCTION_ARGS);
static plperl_proc_desc *compile_plperl_function(Oid fn_oid, bool is_trigger);

static SV  *plperl_hash_from_tuple(HeapTuple tuple, TupleDesc tupdesc);
static void plperl_init_shared_libs(pTHX);
static HV  *plperl_spi_execute_fetch_result(SPITupleTable *, int, int);
static void check_interp(bool trusted);
static char *strip_trailing_ws(const char *msg);

#ifdef WIN32
static char *setlocale_perl(int category, char *locale);
#endif


/* hash table entry for proc desc  */

typedef struct plperl_proc_entry
{
	char		proc_name[NAMEDATALEN];
	plperl_proc_desc *proc_data;
} plperl_proc_entry;

/*
 * This routine is a crock, and so is everyplace that calls it.  The problem
 * is that the cached form of plperl functions/queries is allocated permanently
 * (mostly via malloc()) and never released until backend exit.  Subsidiary
 * data structures such as fmgr info records therefore must live forever
 * as well.  A better implementation would store all this stuff in a per-
 * function memory context that could be reclaimed at need.  In the meantime,
 * fmgr_info_cxt must be called specifying TopMemoryContext so that whatever
 * it might allocate, and whatever the eventual function might allocate using
 * fn_mcxt, will live forever too.
 */
static void
perm_fmgr_info(Oid functionId, FmgrInfo *finfo)
{
	fmgr_info_cxt(functionId, finfo, TopMemoryContext);
}

/**********************************************************************
 * plperl_init()			- Initialize everything that can be
 *							  safely initialized during postmaster
 *							  startup.
 *
 * DO NOT make this static --- it has to be callable by preload
 **********************************************************************/
void
plperl_init(void)
{
	HASHCTL		hash_ctl;

	/************************************************************
	 * Do initialization only once
	 ************************************************************/
	if (!plperl_firstcall)
		return;

	MemSet(&hash_ctl, 0, sizeof(hash_ctl));

	hash_ctl.keysize = NAMEDATALEN;
	hash_ctl.entrysize = sizeof(plperl_proc_entry);

	plperl_proc_hash = hash_create("PLPerl Procedures",
								   32,
								   &hash_ctl,
								   HASH_ELEM);

	/************************************************************
	 * Create the Perl interpreter
	 ************************************************************/
	PLPERL_SET_OPMASK(plperl_opmask);

	plperl_init_interp();

	plperl_firstcall = 0;
}

/**********************************************************************
 * plperl_init_all()		- Initialize all
 **********************************************************************/
static void
plperl_init_all(void)
{

	/************************************************************
	 * Execute postmaster-startup safe initialization
	 ************************************************************/
	if (plperl_firstcall)
		plperl_init();

	/************************************************************
	 * Any other initialization that must be done each time a new
	 * backend starts -- currently none
	 ************************************************************/

}

#define PLC_TRUSTED \
	"require strict; "

#define TEST_FOR_MULTI \
	"use Config; " \
	"$Config{usemultiplicity} eq 'define' or "	\
	"($Config{usethreads} eq 'define' " \
	" and $Config{useithreads} eq 'define')"


static void
set_interp_require(void)
{
	if (trusted_context)
	{
		PL_ppaddr[OP_REQUIRE] = pp_require_safe;
		PL_ppaddr[OP_DOFILE] = pp_require_safe;
	}
	else
	{
		PL_ppaddr[OP_REQUIRE] = pp_require_orig;
		PL_ppaddr[OP_DOFILE] = pp_require_orig;
	}
}

/********************************************************************
 *
 * We start out by creating a "held" interpreter that we can use in
 * trusted or untrusted mode (but not both) as the need arises. Later, we
 * assign that interpreter if it is available to either the trusted or
 * untrusted interpreter. If it has already been assigned, and we need to
 * create the other interpreter, we do that if we can, or error out.
 * We detect if it is safe to run two interpreters during the setup of the
 * dummy interpreter.
 */


static void
check_interp(bool trusted)
{
	if (interp_state == INTERP_HELD)
	{
		if (trusted)
		{
			plperl_trusted_interp = plperl_held_interp;
			interp_state = INTERP_TRUSTED;
		}
		else
		{
			plperl_untrusted_interp = plperl_held_interp;
			interp_state = INTERP_UNTRUSTED;
		}
		plperl_held_interp = NULL;
		trusted_context = trusted;
		set_interp_require();
	}
	else if (interp_state == INTERP_BOTH ||
			 (trusted && interp_state == INTERP_TRUSTED) ||
			 (!trusted && interp_state == INTERP_UNTRUSTED))
	{
		if (trusted_context != trusted)
		{
			if (trusted)
				PERL_SET_CONTEXT(plperl_trusted_interp);
			else
				PERL_SET_CONTEXT(plperl_untrusted_interp);
			trusted_context = trusted;
			set_interp_require();
		}
	}
	else if (can_run_two)
	{
		PERL_SET_CONTEXT(plperl_held_interp);
		plperl_init_interp();
		if (trusted)
			plperl_trusted_interp = plperl_held_interp;
		else
			plperl_untrusted_interp = plperl_held_interp;
		interp_state = INTERP_BOTH;
		plperl_held_interp = NULL;
		trusted_context = trusted;
		set_interp_require();
	}
	else
	{
		elog(ERROR,
			 "can not allocate second Perl interpreter on this platform");

	}

}


static void
restore_context(bool old_context)
{
	if (trusted_context != old_context)
	{
		if (old_context)
			PERL_SET_CONTEXT(plperl_trusted_interp);
		else
			PERL_SET_CONTEXT(plperl_untrusted_interp);

		trusted_context = old_context;
		set_interp_require();
	}
}

/**********************************************************************
 * plperl_init_interp() - Create the Perl interpreter
 **********************************************************************/
static void
plperl_init_interp(void)
{
	static char *embedding[3] = {
		"", "-e",

		/*
		 * no commas between the next lines please. They are supposed to be
		 * one string
		 */
		"SPI::bootstrap(); use vars qw(%_SHARED);"
		"sub ::mkfunc {return eval(qq[ sub { $_[0] $_[1] } ]); }"
	};

#ifdef WIN32

	/*
	 * The perl library on startup does horrible things like call
	 * setlocale(LC_ALL,""). We have protected against that on most platforms
	 * by setting the environment appropriately. However, on Windows,
	 * setlocale() does not consult the environment, so we need to save the
	 * excisting locale settings before perl has a chance to mangle them and
	 * restore them after its dirty deeds are done.
	 *
	 * MSDN ref:
	 * http://msdn.microsoft.com/library/en-us/vclib/html/_crt_locale.asp
	 *
	 * It appaers that we only need to do this on interpreter startup, and
	 * subsequent calls to the interpreter don't mess with the locale
	 * settings.
	 *
	 * We restore them using Perl's perl_setlocale() function so that Perl
	 * doesn't have a different idea of the locale from Postgres.
	 *
	 */

	char	   *loc;
	char	   *save_collate,
			   *save_ctype,
			   *save_monetary,
			   *save_numeric,
			   *save_time;

	loc = setlocale(LC_COLLATE, NULL);
	save_collate = loc ? pstrdup(loc) : NULL;
	loc = setlocale(LC_CTYPE, NULL);
	save_ctype = loc ? pstrdup(loc) : NULL;
	loc = setlocale(LC_MONETARY, NULL);
	save_monetary = loc ? pstrdup(loc) : NULL;
	loc = setlocale(LC_NUMERIC, NULL);
	save_numeric = loc ? pstrdup(loc) : NULL;
	loc = setlocale(LC_TIME, NULL);
	save_time = loc ? pstrdup(loc) : NULL;

#define PLPERL_RESTORE_LOCALE(name, saved) \
	  STMT_START { \
			  if (saved != NULL) { setlocale_perl(name, saved); pfree(saved); } \
	  } STMT_END
#endif

	/****
	 * The perl API docs state that PERL_SYS_INIT3 should be called before
	 * allocating interprters. Unfortunately, on some platforms this fails
	 * in the Perl_do_taint() routine, which is called when the platform is
	 * using the system's malloc() instead of perl's own. Other platforms,
	 * notably Windows, fail if PERL_SYS_INIT3 is not called. So we call it
	 * if it's available, unless perl is using the system malloc(), which is
	 * true when MYMALLOC is set.
	 */
#if defined(PERL_SYS_INIT3) && !defined(MYMALLOC)
	if (interp_state == INTERP_NONE)
	{
		int			nargs;
		char	   *dummy_perl_env[1];

		/* initialize this way to silence silly compiler warnings */
		nargs = 3;
		dummy_perl_env[0] = NULL;
		PERL_SYS_INIT3(&nargs, (char ***) &embedding, (char ***) &dummy_perl_env);
	}
#endif

	plperl_held_interp = perl_alloc();
	if (!plperl_held_interp)
		elog(ERROR, "could not allocate Perl interpreter");

	perl_construct(plperl_held_interp);

	/*
	 * Record the original function for the 'require' and 'dofile' opcodes.
	 * (They share the same implementation.) Ensure it's used for new
	 * interpreters.
	 */
	if (!pp_require_orig)
	{
		pp_require_orig = PL_ppaddr[OP_REQUIRE];
	}
	else
	{
		PL_ppaddr[OP_REQUIRE] = pp_require_orig;
		PL_ppaddr[OP_DOFILE] = pp_require_orig;
	}

	perl_parse(plperl_held_interp, plperl_init_shared_libs,
			   3, embedding, NULL);
	perl_run(plperl_held_interp);

	if (interp_state == INTERP_NONE)
	{
		SV		   *res;

		res = eval_pv(TEST_FOR_MULTI, TRUE);
		can_run_two = SvIV(res);
		interp_state = INTERP_HELD;
	}

#ifdef PLPERL_RESTORE_LOCALE
	PLPERL_RESTORE_LOCALE(LC_COLLATE, save_collate);
	PLPERL_RESTORE_LOCALE(LC_CTYPE, save_ctype);
	PLPERL_RESTORE_LOCALE(LC_MONETARY, save_monetary);
	PLPERL_RESTORE_LOCALE(LC_NUMERIC, save_numeric);
	PLPERL_RESTORE_LOCALE(LC_TIME, save_time);
#endif

}

/*
 * Our safe implementation of the require opcode.
 * This is safe because it's completely unable to load any code.
 * If the requested file/module has already been loaded it'll return true.
 * If not, it'll die.
 * So now "use Foo;" will work iff Foo has already been loaded.
 */
static OP  *
pp_require_safe(pTHX)
{
	dVAR;
	dSP;
	SV		   *sv,
			  **svp;
	char	   *name;
	STRLEN		len;

	sv = POPs;
	name = SvPV(sv, len);
	if (!(name && len > 0 && *name))
		RETPUSHNO;

	svp = hv_fetch(GvHVn(PL_incgv), name, len, 0);
	if (svp && *svp != &PL_sv_undef)
		RETPUSHYES;

	DIE(aTHX_ "Unable to load %s into plperl", name);
}


static void
plperl_safe_init(void)
{
	HV		   *stash;
	SV		   *sv;
	char	   *key;
	I32			klen;

	/* use original require while we set up */
	PL_ppaddr[OP_REQUIRE] = pp_require_orig;
	PL_ppaddr[OP_DOFILE] = pp_require_orig;

	eval_pv(PLC_TRUSTED, FALSE);
	if (SvTRUE(ERRSV))
		ereport(ERROR,
				(errmsg("%s", strip_trailing_ws(SvPV_nolen(ERRSV))),
				 errcontext("While executing PLC_TRUSTED.")));

	if (GetDatabaseEncoding() == PG_UTF8)
	{
		/*
		 * Force loading of utf8 module now to prevent errors that can arise
		 * from the regex code later trying to load utf8 modules. See
		 * http://rt.perl.org/rt3/Ticket/Display.html?id=47576
		 */
		eval_pv("my $a=chr(0x100); return $a =~ /\\xa9/i", FALSE);
		if (SvTRUE(ERRSV))
			ereport(ERROR,
					(errmsg("%s", strip_trailing_ws(SvPV_nolen(ERRSV))),
					 errcontext("While executing utf8fix.")));

	}

	/*
	 * Lock down the interpreter
	 */

	/* switch to the safe require/dofile opcode for future code */
	PL_ppaddr[OP_REQUIRE] = pp_require_safe;
	PL_ppaddr[OP_DOFILE] = pp_require_safe;

	/* 
	 * prevent (any more) unsafe opcodes being compiled 
	 * PL_op_mask is per interpreter, so this only needs to be set once 
	 */
	PL_op_mask = plperl_opmask;

	/* delete the DynaLoader:: namespace so extensions can't be loaded */
	stash = gv_stashpv("DynaLoader", GV_ADDWARN);
	hv_iterinit(stash);
	while ((sv = hv_iternextsv(stash, &key, &klen)))
	{
		if (!isGV_with_GP(sv) || !GvCV(sv))
			continue;
		SvREFCNT_dec(GvCV(sv)); /* free the CV */
		GvCV(sv) = NULL;		/* prevent call via GV */
	}

	hv_clear(stash);
	/* invalidate assorted caches */
	++PL_sub_generation;
#ifdef PL_stashcache
	hv_clear(PL_stashcache);
#endif

	plperl_safe_init_done = true;
}

/*
 * Perl likes to put a newline after its error messages; clean up such
 */
static char *
strip_trailing_ws(const char *msg)
{
	char	   *res = pstrdup(msg);
	int			len = strlen(res);

	while (len > 0 && isspace((unsigned char) res[len - 1]))
		res[--len] = '\0';
	return res;
}


/*
 * Build a tuple from a hash
 */
static HeapTuple
plperl_build_tuple_result(HV *perlhash, AttInMetadata *attinmeta)
{
	TupleDesc	td = attinmeta->tupdesc;
	char	  **values;
	SV		   *val;
	char	   *key;
	I32			klen;
	HeapTuple	tup;

	values = (char **) palloc0(td->natts * sizeof(char *));

	hv_iterinit(perlhash);
	while ((val = hv_iternextsv(perlhash, &key, &klen)))
	{
		int			attn = SPI_fnumber(td, key);

		if (attn <= 0 || td->attrs[attn - 1]->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("Perl hash contains nonexistent column \"%s\"",
							key)));
		if (SvOK(val))
			values[attn - 1] = SvPV(val, PL_na);
	}
	hv_iterinit(perlhash);

	tup = BuildTupleFromCStrings(attinmeta, values);
	pfree(values);
	return tup;
}


/**********************************************************************
 * set up arguments for a trigger call
 **********************************************************************/
static SV  *
plperl_trigger_build_args(FunctionCallInfo fcinfo)
{
	TriggerData *tdata;
	TupleDesc	tupdesc;
	int			i;
	char	   *level;
	char	   *event;
	char	   *relid;
	char	   *when;
	HV		   *hv;

	hv = newHV();

	tdata = (TriggerData *) fcinfo->context;
	tupdesc = tdata->tg_relation->rd_att;

	relid = DatumGetCString(
							DirectFunctionCall1(oidout,
								  ObjectIdGetDatum(tdata->tg_relation->rd_id)
												)
		);

	(void) hv_store(hv, "name", 4, newSVpv(tdata->tg_trigger->tgname, 0), 0);
	(void) hv_store(hv, "relid", 5, newSVpv(relid, 0), 0);

	if (TRIGGER_FIRED_BY_INSERT(tdata->tg_event))
	{
		event = "INSERT";
		if (TRIGGER_FIRED_FOR_ROW(tdata->tg_event))
			(void) hv_store(hv, "new", 3,
						plperl_hash_from_tuple(tdata->tg_trigtuple, tupdesc),
							0);
	}
	else if (TRIGGER_FIRED_BY_DELETE(tdata->tg_event))
	{
		event = "DELETE";
		if (TRIGGER_FIRED_FOR_ROW(tdata->tg_event))
			(void) hv_store(hv, "old", 3,
						plperl_hash_from_tuple(tdata->tg_trigtuple, tupdesc),
							0);
	}
	else if (TRIGGER_FIRED_BY_UPDATE(tdata->tg_event))
	{
		event = "UPDATE";
		if (TRIGGER_FIRED_FOR_ROW(tdata->tg_event))
		{
			(void) hv_store(hv, "old", 3,
						plperl_hash_from_tuple(tdata->tg_trigtuple, tupdesc),
							0);
			(void) hv_store(hv, "new", 3,
						 plperl_hash_from_tuple(tdata->tg_newtuple, tupdesc),
							0);
		}
	}
	else
		event = "UNKNOWN";

	(void) hv_store(hv, "event", 5, newSVpv(event, 0), 0);
	(void) hv_store(hv, "argc", 4, newSViv(tdata->tg_trigger->tgnargs), 0);

	if (tdata->tg_trigger->tgnargs > 0)
	{
		AV		   *av = newAV();

		for (i = 0; i < tdata->tg_trigger->tgnargs; i++)
			av_push(av, newSVpv(tdata->tg_trigger->tgargs[i], 0));
		(void) hv_store(hv, "args", 4, newRV_noinc((SV *) av), 0);
	}

	(void) hv_store(hv, "relname", 7,
					newSVpv(SPI_getrelname(tdata->tg_relation), 0), 0);

	if (TRIGGER_FIRED_BEFORE(tdata->tg_event))
		when = "BEFORE";
	else if (TRIGGER_FIRED_AFTER(tdata->tg_event))
		when = "AFTER";
	else
		when = "UNKNOWN";
	(void) hv_store(hv, "when", 4, newSVpv(when, 0), 0);

	if (TRIGGER_FIRED_FOR_ROW(tdata->tg_event))
		level = "ROW";
	else if (TRIGGER_FIRED_FOR_STATEMENT(tdata->tg_event))
		level = "STATEMENT";
	else
		level = "UNKNOWN";
	(void) hv_store(hv, "level", 5, newSVpv(level, 0), 0);

	return newRV_noinc((SV *) hv);
}


/*
 * Obtain tuple descriptor for a function returning tuple
 *
 * NB: copy the result if needed for any great length of time
 */
static TupleDesc
get_function_tupdesc(Oid result_type, ReturnSetInfo *rsinfo)
{
	if (result_type == RECORDOID)
	{
		/* We must get the information from call context */
		if (!rsinfo || !IsA(rsinfo, ReturnSetInfo) ||
			rsinfo->expectedDesc == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
							"that cannot accept type record")));
		return rsinfo->expectedDesc;
	}
	else	/* ordinary composite type */
		return lookup_rowtype_tupdesc(result_type, -1);
}

/**********************************************************************
 * set up the new tuple returned from a trigger
 **********************************************************************/
static HeapTuple
plperl_modify_tuple(HV *hvTD, TriggerData *tdata, HeapTuple otup)
{
	SV		  **svp;
	HV		   *hvNew;
	HeapTuple	rtup;
	SV		   *val;
	char	   *key;
	I32			klen;
	int			slotsused;
	int		   *modattrs;
	Datum	   *modvalues;
	char	   *modnulls;

	TupleDesc	tupdesc;

	tupdesc = tdata->tg_relation->rd_att;

	svp = hv_fetch(hvTD, "new", 3, FALSE);
	if (!svp)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("$_TD->{new} does not exist")));
	if (!SvOK(*svp) || !SvROK(*svp) || SvTYPE(SvRV(*svp)) != SVt_PVHV)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("$_TD->{new} is not a hash reference")));
	hvNew = (HV *) SvRV(*svp);

	modattrs = palloc(tupdesc->natts * sizeof(int));
	modvalues = palloc(tupdesc->natts * sizeof(Datum));
	modnulls = palloc(tupdesc->natts * sizeof(char));
	slotsused = 0;

	hv_iterinit(hvNew);
	while ((val = hv_iternextsv(hvNew, &key, &klen)))
	{
		int			attn = SPI_fnumber(tupdesc, key);

		if (attn <= 0 || tupdesc->attrs[attn - 1]->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("Perl hash contains nonexistent column \"%s\"",
							key)));
		if (SvOK(val))
		{
			Oid			typinput;
			Oid			typioparam;
			FmgrInfo	finfo;

			/* XXX would be better to cache these lookups */
			getTypeInputInfo(tupdesc->attrs[attn - 1]->atttypid,
							 &typinput, &typioparam);
			fmgr_info(typinput, &finfo);
			modvalues[slotsused] = FunctionCall3(&finfo,
										   CStringGetDatum(SvPV(val, PL_na)),
												 ObjectIdGetDatum(typioparam),
						 Int32GetDatum(tupdesc->attrs[attn - 1]->atttypmod));
			modnulls[slotsused] = ' ';
		}
		else
		{
			modvalues[slotsused] = (Datum) 0;
			modnulls[slotsused] = 'n';
		}
		modattrs[slotsused] = attn;
		slotsused++;
	}
	hv_iterinit(hvNew);

	rtup = SPI_modifytuple(tdata->tg_relation, otup, slotsused,
						   modattrs, modvalues, modnulls);

	pfree(modattrs);
	pfree(modvalues);
	pfree(modnulls);

	if (rtup == NULL)
		elog(ERROR, "SPI_modifytuple failed: %s",
			 SPI_result_code_string(SPI_result));

	return rtup;
}

/**********************************************************************
 * plperl_call_handler		- This is the only visible function
 *				  of the PL interpreter. The PostgreSQL
 *				  function manager and trigger manager
 *				  call this function for execution of
 *				  perl procedures.
 **********************************************************************/
PG_FUNCTION_INFO_V1(plperl_call_handler);

/* keep non-static */
Datum
plperl_call_handler(PG_FUNCTION_ARGS)
{
	Datum		retval;
	plperl_proc_desc *save_prodesc;
	bool		oldcontext = trusted_context;

	/*
	 * Initialize interpreter if first time through
	 */
	plperl_init_all();

	/*
	 * Ensure that static pointers are saved/restored properly
	 */
	save_prodesc = plperl_current_prodesc;

	PG_TRY();
	{
		/*
		 * Determine if called as function or trigger and call appropriate
		 * subhandler
		 */
		if (CALLED_AS_TRIGGER(fcinfo))
			retval = PointerGetDatum(plperl_trigger_handler(fcinfo));
		else
			retval = plperl_func_handler(fcinfo);
	}
	PG_CATCH();
	{
		plperl_current_prodesc = save_prodesc;
		restore_context(oldcontext);
		PG_RE_THROW();
	}
	PG_END_TRY();

	plperl_current_prodesc = save_prodesc;
	restore_context(oldcontext);
	return retval;
}


/**********************************************************************
 * plperl_create_sub()		- calls the perl interpreter to
 *		create the anonymous subroutine whose text is in the SV.
 *		Returns the SV containing the RV to the closure.
 **********************************************************************/
static SV  *
plperl_create_sub(char *s, bool trusted)
{
	dSP;
	SV		   *subref;
	int			count;

	if (trusted && !plperl_safe_init_done)
	{
		plperl_safe_init();
		SPAGAIN;
	}

	ENTER;
	SAVETMPS;
	PUSHMARK(SP);
	XPUSHs(sv_2mortal(newSVpv("my $_TD=$_[0]; shift;", 0)));
	XPUSHs(sv_2mortal(newSVpv(s, 0)));
	PUTBACK;

	/*
	 * G_KEEPERR seems to be needed here, else we don't recognize compile
	 * errors properly.  Perhaps it's because there's another level of eval
	 * inside mkfunc?
	 */
	count = perl_call_pv("::mkfunc", G_SCALAR | G_EVAL | G_KEEPERR);
	SPAGAIN;

	if (count != 1)
	{
		PUTBACK;
		FREETMPS;
		LEAVE;
		elog(ERROR, "didn't get a return item from mkfunc");
	}

	if (SvTRUE(ERRSV))
	{
		(void) POPs;
		PUTBACK;
		FREETMPS;
		LEAVE;
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("creation of Perl function failed: %s",
						strip_trailing_ws(SvPV(ERRSV, PL_na)))));
	}

	/*
	 * need to make a deep copy of the return. it comes off the stack as a
	 * temporary.
	 */
	subref = newSVsv(POPs);

	if (!SvROK(subref))
	{
		PUTBACK;
		FREETMPS;
		LEAVE;

		/*
		 * subref is our responsibility because it is not mortal
		 */
		SvREFCNT_dec(subref);
		elog(ERROR, "didn't get a code ref");
	}

	PUTBACK;
	FREETMPS;
	LEAVE;

	return subref;
}

/**********************************************************************
 * plperl_init_shared_libs()		-
 *
 * We cannot use the DynaLoader directly to get at the Opcode
 * module. So, we link Opcode into ourselves
 * and do the initialization behind perl's back.
 *
 **********************************************************************/

EXTERN_C void boot_DynaLoader(pTHX_ CV *cv);
EXTERN_C void boot_SPI(pTHX_ CV *cv);

static void
plperl_init_shared_libs(pTHX)
{
	char	   *file = __FILE__;

	newXS("DynaLoader::boot_DynaLoader", boot_DynaLoader, file);
	newXS("SPI::bootstrap", boot_SPI, file);
}

/**********************************************************************
 * plperl_call_perl_func()		- calls a perl function through the RV
 *	stored in the prodesc structure. massages the input parms properly
 **********************************************************************/
static SV  *
plperl_call_perl_func(plperl_proc_desc *desc, FunctionCallInfo fcinfo)
{
	dSP;
	SV		   *retval;
	int			i;
	int			count;

	ENTER;
	SAVETMPS;

	PUSHMARK(SP);

	XPUSHs(sv_2mortal(newSVpv("undef", 0)));	/* no trigger data */

	for (i = 0; i < desc->nargs; i++)
	{
		if (fcinfo->argnull[i])
			XPUSHs(&PL_sv_undef);
		else if (desc->arg_is_rowtype[i])
		{
			HeapTupleHeader td;
			Oid			tupType;
			int32		tupTypmod;
			TupleDesc	tupdesc;
			HeapTupleData tmptup;
			SV		   *hashref;

			td = DatumGetHeapTupleHeader(fcinfo->arg[i]);
			/* Extract rowtype info and find a tupdesc */
			tupType = HeapTupleHeaderGetTypeId(td);
			tupTypmod = HeapTupleHeaderGetTypMod(td);
			tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
			tupdesc = CreateTupleDescCopy(tupdesc);
			/* Build a temporary HeapTuple control structure */
			tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
			tmptup.t_data = td;

			hashref = plperl_hash_from_tuple(&tmptup, tupdesc);
			XPUSHs(sv_2mortal(hashref));
			FreeTupleDesc(tupdesc);
		}
		else
		{
			char	   *tmp;

			tmp = DatumGetCString(FunctionCall3(&(desc->arg_out_func[i]),
												fcinfo->arg[i],
								   ObjectIdGetDatum(desc->arg_typioparam[i]),
												Int32GetDatum(-1)));
			XPUSHs(sv_2mortal(newSVpv(tmp, 0)));
			pfree(tmp);
		}
	}
	PUTBACK;

	/* Do NOT use G_KEEPERR here */
	count = perl_call_sv(desc->reference, G_SCALAR | G_EVAL);

	SPAGAIN;

	if (count != 1)
	{
		PUTBACK;
		FREETMPS;
		LEAVE;
		elog(ERROR, "didn't get a return item from function");
	}

	if (SvTRUE(ERRSV))
	{
		(void) POPs;
		PUTBACK;
		FREETMPS;
		LEAVE;
		/* XXX need to find a way to assign an errcode here */
		ereport(ERROR,
				(errmsg("error from Perl function: %s",
						strip_trailing_ws(SvPV(ERRSV, PL_na)))));
	}

	retval = newSVsv(POPs);

	PUTBACK;
	FREETMPS;
	LEAVE;

	return retval;
}

/**********************************************************************
 * plperl_call_perl_trigger_func()	- calls a perl trigger function
 *	through the RV stored in the prodesc structure.
 **********************************************************************/
static SV  *
plperl_call_perl_trigger_func(plperl_proc_desc *desc, FunctionCallInfo fcinfo,
							  SV *td)
{
	dSP;
	SV		   *retval;
	Trigger    *tg_trigger;
	int			i;
	int			count;

	ENTER;
	SAVETMPS;

	PUSHMARK(sp);

	XPUSHs(td);

	tg_trigger = ((TriggerData *) fcinfo->context)->tg_trigger;
	for (i = 0; i < tg_trigger->tgnargs; i++)
		XPUSHs(sv_2mortal(newSVpv(tg_trigger->tgargs[i], 0)));
	PUTBACK;

	/* Do NOT use G_KEEPERR here */
	count = perl_call_sv(desc->reference, G_SCALAR | G_EVAL);

	SPAGAIN;

	if (count != 1)
	{
		PUTBACK;
		FREETMPS;
		LEAVE;
		elog(ERROR, "didn't get a return item from trigger function");
	}

	if (SvTRUE(ERRSV))
	{
		(void) POPs;
		PUTBACK;
		FREETMPS;
		LEAVE;
		/* XXX need to find a way to assign an errcode here */
		ereport(ERROR,
				(errmsg("error from Perl trigger function: %s",
						strip_trailing_ws(SvPV(ERRSV, PL_na)))));
	}

	retval = newSVsv(POPs);

	PUTBACK;
	FREETMPS;
	LEAVE;

	return retval;
}

/**********************************************************************
 * plperl_func_handler()		- Handler for regular function calls
 **********************************************************************/
static Datum
plperl_func_handler(PG_FUNCTION_ARGS)
{
	plperl_proc_desc *prodesc;
	SV		   *perlret;
	Datum		retval;

	/* Connect to SPI manager */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI manager");

	/* Find or compile the function */
	prodesc = compile_plperl_function(fcinfo->flinfo->fn_oid, false);

	plperl_current_prodesc = prodesc;

	check_interp(prodesc->lanpltrusted);

	/************************************************************
	 * Call the Perl function if not returning set
	 ************************************************************/
	if (!prodesc->fn_retisset)
		perlret = plperl_call_perl_func(prodesc, fcinfo);
	else if (SRF_IS_FIRSTCALL())
		perlret = plperl_call_perl_func(prodesc, fcinfo);
	else
	{
		/* Get back the SV stashed on initial call */
		FuncCallContext *funcctx = (FuncCallContext *) fcinfo->flinfo->fn_extra;

		perlret = (SV *) funcctx->user_fctx;
	}

	/************************************************************
	 * Disconnect from SPI manager and then create the return
	 * values datum (if the input function does a palloc for it
	 * this must not be allocated in the SPI memory context
	 * because SPI_finish would free it).
	 ************************************************************/
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish() failed");

	if (!(perlret && SvOK(perlret)))
	{
		/* return NULL if Perl code returned undef */
		ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;

		if (perlret)
			SvREFCNT_dec(perlret);
		if (rsi && IsA(rsi, ReturnSetInfo))
			rsi->isDone = ExprEndResult;
		PG_RETURN_NULL();
	}

	if (prodesc->fn_retisset && prodesc->fn_retistuple)
	{
		/* set of tuples */
		AV		   *ret_av;
		FuncCallContext *funcctx;
		TupleDesc	tupdesc;
		AttInMetadata *attinmeta;

		if (!SvOK(perlret) || !SvROK(perlret) || SvTYPE(SvRV(perlret)) != SVt_PVAV)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("set-returning Perl function must return reference to array")));
		ret_av = (AV *) SvRV(perlret);

		if (SRF_IS_FIRSTCALL())
		{
			MemoryContext oldcontext;

			funcctx = SRF_FIRSTCALL_INIT();

			funcctx->user_fctx = (void *) perlret;

			funcctx->max_calls = av_len(ret_av) + 1;

			/* Cache a copy of the result's tupdesc and attinmeta */
			oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
			tupdesc = get_function_tupdesc(prodesc->result_oid,
									   (ReturnSetInfo *) fcinfo->resultinfo);
			tupdesc = CreateTupleDescCopy(tupdesc);
			funcctx->attinmeta = TupleDescGetAttInMetadata(tupdesc);
			MemoryContextSwitchTo(oldcontext);
		}

		funcctx = SRF_PERCALL_SETUP();
		attinmeta = funcctx->attinmeta;
		tupdesc = attinmeta->tupdesc;

		if (funcctx->call_cntr < funcctx->max_calls)
		{
			SV		  **svp;
			HV		   *row_hv;
			HeapTuple	tuple;

			svp = av_fetch(ret_av, funcctx->call_cntr, FALSE);
			Assert(svp != NULL);

			if (!SvOK(*svp) || !SvROK(*svp) || SvTYPE(SvRV(*svp)) != SVt_PVHV)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("elements of Perl result array must be reference to hash")));
			row_hv = (HV *) SvRV(*svp);

			tuple = plperl_build_tuple_result(row_hv, attinmeta);
			retval = HeapTupleGetDatum(tuple);
			SRF_RETURN_NEXT(funcctx, retval);
		}
		else
		{
			SvREFCNT_dec(perlret);
			SRF_RETURN_DONE(funcctx);
		}
	}
	else if (prodesc->fn_retisset)
	{
		/* set of non-tuples */
		AV		   *ret_av;
		FuncCallContext *funcctx;

		if (!SvOK(perlret) || !SvROK(perlret) || SvTYPE(SvRV(perlret)) != SVt_PVAV)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("set-returning Perl function must return reference to array")));
		ret_av = (AV *) SvRV(perlret);

		if (SRF_IS_FIRSTCALL())
		{
			funcctx = SRF_FIRSTCALL_INIT();

			funcctx->user_fctx = (void *) perlret;

			funcctx->max_calls = av_len(ret_av) + 1;
		}

		funcctx = SRF_PERCALL_SETUP();

		if (funcctx->call_cntr < funcctx->max_calls)
		{
			SV		  **svp;

			svp = av_fetch(ret_av, funcctx->call_cntr, FALSE);
			Assert(svp != NULL);

			if (SvOK(*svp))
			{
				char	   *val = SvPV(*svp, PL_na);

				fcinfo->isnull = false;
				retval = FunctionCall3(&prodesc->result_in_func,
									   PointerGetDatum(val),
								ObjectIdGetDatum(prodesc->result_typioparam),
									   Int32GetDatum(-1));
			}
			else
			{
				fcinfo->isnull = true;
				retval = (Datum) 0;
			}
			SRF_RETURN_NEXT(funcctx, retval);
		}
		else
		{
			SvREFCNT_dec(perlret);
			SRF_RETURN_DONE(funcctx);
		}
	}
	else if (prodesc->fn_retistuple)
	{
		/* singleton perl hash to Datum */
		HV		   *perlhash;
		TupleDesc	td;
		AttInMetadata *attinmeta;
		HeapTuple	tup;

		if (!SvOK(perlret) || !SvROK(perlret) || SvTYPE(SvRV(perlret)) != SVt_PVHV)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("composite-returning Perl function must return reference to hash")));
		perlhash = (HV *) SvRV(perlret);

		/*
		 * XXX should cache the attinmeta data instead of recomputing
		 */
		td = get_function_tupdesc(prodesc->result_oid,
								  (ReturnSetInfo *) fcinfo->resultinfo);
		td = CreateTupleDescCopy(td);
		attinmeta = TupleDescGetAttInMetadata(td);

		tup = plperl_build_tuple_result(perlhash, attinmeta);
		retval = HeapTupleGetDatum(tup);
	}
	else
	{
		/* perl string to Datum */
		char	   *val = SvPV(perlret, PL_na);

		retval = FunctionCall3(&prodesc->result_in_func,
							   CStringGetDatum(val),
							   ObjectIdGetDatum(prodesc->result_typioparam),
							   Int32GetDatum(-1));
	}

	SvREFCNT_dec(perlret);

	return retval;
}

/**********************************************************************
 * plperl_trigger_handler()		- Handler for trigger function calls
 **********************************************************************/
static Datum
plperl_trigger_handler(PG_FUNCTION_ARGS)
{
	plperl_proc_desc *prodesc;
	SV		   *perlret;
	Datum		retval;
	SV		   *svTD;
	HV		   *hvTD;

	/* Connect to SPI manager */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI manager");

	/* Find or compile the function */
	prodesc = compile_plperl_function(fcinfo->flinfo->fn_oid, true);

	plperl_current_prodesc = prodesc;

	/************************************************************
	* Call the Perl function
	************************************************************/

	check_interp(prodesc->lanpltrusted);

	/*
	 * call perl trigger function and build TD hash
	 */
	svTD = plperl_trigger_build_args(fcinfo);
	perlret = plperl_call_perl_trigger_func(prodesc, fcinfo, svTD);

	hvTD = (HV *) SvRV(svTD);	/* convert SV TD structure to Perl Hash
								 * structure */

	/************************************************************
	* Disconnect from SPI manager and then create the return
	* values datum (if the input function does a palloc for it
	* this must not be allocated in the SPI memory context
	* because SPI_finish would free it).
	************************************************************/
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish() failed");

	if (perlret == NULL || !SvOK(perlret))
	{
		/* undef result means go ahead with original tuple */
		TriggerData *trigdata = ((TriggerData *) fcinfo->context);

		if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
			retval = (Datum) trigdata->tg_trigtuple;
		else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
			retval = (Datum) trigdata->tg_newtuple;
		else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
			retval = (Datum) trigdata->tg_trigtuple;
		else
			retval = (Datum) 0; /* can this happen? */
	}
	else
	{
		HeapTuple	trv;
		char	   *tmp;

		tmp = SvPV(perlret, PL_na);

		if (pg_strcasecmp(tmp, "SKIP") == 0)
			trv = NULL;
		else if (pg_strcasecmp(tmp, "MODIFY") == 0)
		{
			TriggerData *trigdata = (TriggerData *) fcinfo->context;

			if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
				trv = plperl_modify_tuple(hvTD, trigdata,
										  trigdata->tg_trigtuple);
			else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
				trv = plperl_modify_tuple(hvTD, trigdata,
										  trigdata->tg_newtuple);
			else
			{
				ereport(WARNING,
						(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
					   errmsg("ignoring modified tuple in DELETE trigger")));
				trv = NULL;
			}
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
					 errmsg("result of Perl trigger function must be undef, \"SKIP\" or \"MODIFY\"")));
			trv = NULL;
		}
		retval = PointerGetDatum(trv);
	}

	SvREFCNT_dec(svTD);
	if (perlret)
		SvREFCNT_dec(perlret);

	return retval;
}

/**********************************************************************
 * compile_plperl_function	- compile (or hopefully just look up) function
 **********************************************************************/
static plperl_proc_desc *
compile_plperl_function(Oid fn_oid, bool is_trigger)
{
	HeapTuple	procTup;
	Form_pg_proc procStruct;
	char		internal_proname[64];
	int			proname_len;
	plperl_proc_desc *prodesc = NULL;
	int			i;
	plperl_proc_entry *hash_entry;
	bool		found;
	bool		oldcontext = trusted_context;

	/* We'll need the pg_proc tuple in any case... */
	procTup = SearchSysCache(PROCOID,
							 ObjectIdGetDatum(fn_oid),
							 0, 0, 0);
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);
	procStruct = (Form_pg_proc) GETSTRUCT(procTup);

	/************************************************************
	 * Build our internal proc name from the functions Oid
	 ************************************************************/
	if (!is_trigger)
		sprintf(internal_proname, "__PLPerl_proc_%u", fn_oid);
	else
		sprintf(internal_proname, "__PLPerl_proc_%u_trigger", fn_oid);

	proname_len = strlen(internal_proname);

	/************************************************************
	 * Lookup the internal proc name in the hashtable
	 ************************************************************/
	hash_entry = hash_search(plperl_proc_hash, internal_proname,
							 HASH_FIND, NULL);

	if (hash_entry)
	{
		bool		uptodate;

		prodesc = hash_entry->proc_data;

		/************************************************************
		 * If it's present, must check whether it's still up to date.
		 * This is needed because CREATE OR REPLACE FUNCTION can modify the
		 * function's pg_proc entry without changing its OID.
		 ************************************************************/
		uptodate = (prodesc->fn_xmin == HeapTupleHeaderGetXmin(procTup->t_data) &&
				prodesc->fn_cmin == HeapTupleHeaderGetCmin(procTup->t_data));

		if (!uptodate)
		{
			hash_search(plperl_proc_hash, internal_proname,
						HASH_REMOVE, NULL);
			if (prodesc->reference)
			{
				check_interp(prodesc->lanpltrusted);
				SvREFCNT_dec(prodesc->reference);
				restore_context(oldcontext);
			}
			free(prodesc->proname);
			free(prodesc);
			prodesc = NULL;
		}
	}

	/************************************************************
	 * If we haven't found it in the hashtable, we analyze
	 * the functions arguments and returntype and store
	 * the in-/out-functions in the prodesc block and create
	 * a new hashtable entry for it.
	 *
	 * Then we load the procedure into the Perl interpreter.
	 ************************************************************/
	if (prodesc == NULL)
	{
		HeapTuple	langTup;
		HeapTuple	typeTup;
		Form_pg_language langStruct;
		Form_pg_type typeStruct;
		Datum		prosrcdatum;
		bool		isnull;
		char	   *proc_source;

		/************************************************************
		 * Allocate a new procedure description block
		 ************************************************************/
		prodesc = (plperl_proc_desc *) malloc(sizeof(plperl_proc_desc));
		if (prodesc == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		MemSet(prodesc, 0, sizeof(plperl_proc_desc));
		prodesc->proname = strdup(internal_proname);
		prodesc->fn_xmin = HeapTupleHeaderGetXmin(procTup->t_data);
		prodesc->fn_cmin = HeapTupleHeaderGetCmin(procTup->t_data);

		/* Remember if function is STABLE/IMMUTABLE */
		prodesc->fn_readonly =
			(procStruct->provolatile != PROVOLATILE_VOLATILE);

		/************************************************************
		 * Lookup the pg_language tuple by Oid
		 ************************************************************/
		langTup = SearchSysCache(LANGOID,
								 ObjectIdGetDatum(procStruct->prolang),
								 0, 0, 0);
		if (!HeapTupleIsValid(langTup))
		{
			free(prodesc->proname);
			free(prodesc);
			elog(ERROR, "cache lookup failed for language %u",
				 procStruct->prolang);
		}
		langStruct = (Form_pg_language) GETSTRUCT(langTup);
		prodesc->lanpltrusted = langStruct->lanpltrusted;
		ReleaseSysCache(langTup);

		/************************************************************
		 * Get the required information for input conversion of the
		 * return value.
		 ************************************************************/
		if (!is_trigger)
		{
			typeTup = SearchSysCache(TYPEOID,
									 ObjectIdGetDatum(procStruct->prorettype),
									 0, 0, 0);
			if (!HeapTupleIsValid(typeTup))
			{
				free(prodesc->proname);
				free(prodesc);
				elog(ERROR, "cache lookup failed for type %u",
					 procStruct->prorettype);
			}
			typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

			/* Disallow pseudotype result, except VOID or RECORD */
			if (typeStruct->typtype == 'p')
			{
				if (procStruct->prorettype == VOIDOID ||
					procStruct->prorettype == RECORDOID)
					 /* okay */ ;
				else if (procStruct->prorettype == TRIGGEROID)
				{
					free(prodesc->proname);
					free(prodesc);
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("trigger functions may only be called as triggers")));
				}
				else
				{
					free(prodesc->proname);
					free(prodesc);
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("plperl functions cannot return type %s",
									format_type_be(procStruct->prorettype))));
				}
			}

			prodesc->result_oid = procStruct->prorettype;
			prodesc->fn_retisset = procStruct->proretset;
			prodesc->fn_retistuple = (typeStruct->typtype == 'c' ||
									  procStruct->prorettype == RECORDOID);

			perm_fmgr_info(typeStruct->typinput, &(prodesc->result_in_func));
			prodesc->result_typioparam = getTypeIOParam(typeTup);

			ReleaseSysCache(typeTup);
		}

		/************************************************************
		 * Get the required information for output conversion
		 * of all procedure arguments
		 ************************************************************/
		if (!is_trigger)
		{
			prodesc->nargs = procStruct->pronargs;
			for (i = 0; i < prodesc->nargs; i++)
			{
				typeTup = SearchSysCache(TYPEOID,
								ObjectIdGetDatum(procStruct->proargtypes[i]),
										 0, 0, 0);
				if (!HeapTupleIsValid(typeTup))
				{
					free(prodesc->proname);
					free(prodesc);
					elog(ERROR, "cache lookup failed for type %u",
						 procStruct->proargtypes[i]);
				}
				typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

				/* Disallow pseudotype argument */
				if (typeStruct->typtype == 'p')
				{
					free(prodesc->proname);
					free(prodesc);
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("plperl functions cannot take type %s",
							   format_type_be(procStruct->proargtypes[i]))));
				}

				if (typeStruct->typtype == 'c')
					prodesc->arg_is_rowtype[i] = true;
				else
				{
					prodesc->arg_is_rowtype[i] = false;
					perm_fmgr_info(typeStruct->typoutput,
								   &(prodesc->arg_out_func[i]));
					prodesc->arg_typioparam[i] = getTypeIOParam(typeTup);
				}

				ReleaseSysCache(typeTup);
			}
		}

		/************************************************************
		 * create the text of the anonymous subroutine.
		 * we do not use a named subroutine so that we can call directly
		 * through the reference.
		 ************************************************************/
		prosrcdatum = SysCacheGetAttr(PROCOID, procTup,
									  Anum_pg_proc_prosrc, &isnull);
		if (isnull)
			elog(ERROR, "null prosrc");
		proc_source = DatumGetCString(DirectFunctionCall1(textout,
														  prosrcdatum));

		/************************************************************
		 * Create the procedure in the interpreter
		 ************************************************************/

		check_interp(prodesc->lanpltrusted);

		prodesc->reference = plperl_create_sub(proc_source, prodesc->lanpltrusted);

		restore_context(oldcontext);

		pfree(proc_source);
		if (!prodesc->reference)	/* can this happen? */
		{
			free(prodesc->proname);
			free(prodesc);
			elog(ERROR, "could not create internal procedure \"%s\"",
				 internal_proname);
		}

		/************************************************************
		 * Add the proc description block to the hashtable
		 ************************************************************/
		hash_entry = hash_search(plperl_proc_hash, internal_proname,
								 HASH_ENTER, &found);
		hash_entry->proc_data = prodesc;
	}

	ReleaseSysCache(procTup);

	return prodesc;
}


/**********************************************************************
 * plperl_hash_from_tuple() - Build a ref to a hash
 *				  from all attributes of a given tuple
 **********************************************************************/
static SV  *
plperl_hash_from_tuple(HeapTuple tuple, TupleDesc tupdesc)
{
	HV		   *hv;
	int			i;

	hv = newHV();

	for (i = 0; i < tupdesc->natts; i++)
	{
		Datum		attr;
		bool		isnull;
		char	   *attname;
		char	   *outputstr;
		Oid			typoutput;
		Oid			typioparam;
		bool		typisvarlena;
		int			namelen;

		if (tupdesc->attrs[i]->attisdropped)
			continue;

		attname = NameStr(tupdesc->attrs[i]->attname);
		namelen = strlen(attname);
		attr = heap_getattr(tuple, i + 1, tupdesc, &isnull);

		if (isnull)
		{
			/* Store (attname => undef) and move on. */
			(void) hv_store(hv, attname, namelen, newSV(0), 0);
			continue;
		}

		/* XXX should have a way to cache these lookups */

		getTypeOutputInfo(tupdesc->attrs[i]->atttypid,
						  &typoutput, &typioparam, &typisvarlena);

		outputstr = DatumGetCString(OidFunctionCall3(typoutput,
													 attr,
												ObjectIdGetDatum(typioparam),
							   Int32GetDatum(tupdesc->attrs[i]->atttypmod)));

		(void) hv_store(hv, attname, namelen, newSVpv(outputstr, 0), 0);

		pfree(outputstr);
	}

	return newRV_noinc((SV *) hv);
}


/*
 * Implementation of spi_exec_query() Perl function
 */
HV *
plperl_spi_exec(char *query, int limit)
{
	HV		   *ret_hv;

	/*
	 * Execute the query inside a sub-transaction, so we can cope with errors
	 * sanely
	 */
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;

	BeginInternalSubTransaction(NULL);
	/* Want to run inside function's memory context */
	MemoryContextSwitchTo(oldcontext);

	PG_TRY();
	{
		int			spi_rv;

		spi_rv = SPI_execute(query, plperl_current_prodesc->fn_readonly,
							 limit);
		ret_hv = plperl_spi_execute_fetch_result(SPI_tuptable, SPI_processed,
												 spi_rv);

		/* Commit the inner transaction, return to outer xact context */
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;

		/*
		 * AtEOSubXact_SPI() should not have popped any SPI context, but just
		 * in case it did, make sure we remain connected.
		 */
		SPI_restore_connection();
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		/* Save error info */
		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();

		/* Abort the inner transaction */
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;

		/*
		 * If AtEOSubXact_SPI() popped any SPI context of the subxact, it will
		 * have left us in a disconnected state.  We need this hack to return
		 * to connected state.
		 */
		SPI_restore_connection();

		/* Punt the error to Perl */
		croak("%s", edata->message);

		/* Can't get here, but keep compiler quiet */
		return NULL;
	}
	PG_END_TRY();

	return ret_hv;
}

static HV  *
plperl_spi_execute_fetch_result(SPITupleTable *tuptable, int processed,
								int status)
{
	HV		   *result;

	result = newHV();

	(void) hv_store(result, "status", strlen("status"),
					newSVpv((char *) SPI_result_code_string(status), 0), 0);
	(void) hv_store(result, "processed", strlen("processed"),
					newSViv(processed), 0);

	if (status == SPI_OK_SELECT)
	{
		AV		   *rows;
		SV		   *row;
		int			i;

		rows = newAV();
		for (i = 0; i < processed; i++)
		{
			row = plperl_hash_from_tuple(tuptable->vals[i], tuptable->tupdesc);
			av_push(rows, row);
		}
		(void) hv_store(result, "rows", strlen("rows"),
						newRV_noinc((SV *) rows), 0);
	}

	SPI_freetuptable(tuptable);

	return result;
}


/*
 * Perl's own setlocal() copied from POSIX.xs
 * (needed because of the calls to new_*())
 */
#ifdef WIN32
static char *
setlocale_perl(int category, char *locale)
{
	char	   *RETVAL = setlocale(category, locale);

	if (RETVAL)
	{
#ifdef USE_LOCALE_CTYPE
		if (category == LC_CTYPE
#ifdef LC_ALL
			|| category == LC_ALL
#endif
			)
		{
			char	   *newctype;

#ifdef LC_ALL
			if (category == LC_ALL)
				newctype = setlocale(LC_CTYPE, NULL);
			else
#endif
				newctype = RETVAL;
			new_ctype(newctype);
		}
#endif   /* USE_LOCALE_CTYPE */
#ifdef USE_LOCALE_COLLATE
		if (category == LC_COLLATE
#ifdef LC_ALL
			|| category == LC_ALL
#endif
			)
		{
			char	   *newcoll;

#ifdef LC_ALL
			if (category == LC_ALL)
				newcoll = setlocale(LC_COLLATE, NULL);
			else
#endif
				newcoll = RETVAL;
			new_collate(newcoll);
		}
#endif   /* USE_LOCALE_COLLATE */


#ifdef USE_LOCALE_NUMERIC
		if (category == LC_NUMERIC
#ifdef LC_ALL
			|| category == LC_ALL
#endif
			)
		{
			char	   *newnum;

#ifdef LC_ALL
			if (category == LC_ALL)
				newnum = setlocale(LC_NUMERIC, NULL);
			else
#endif
				newnum = RETVAL;
			new_numeric(newnum);
		}
#endif   /* USE_LOCALE_NUMERIC */
	}

	return RETVAL;
}

#endif
