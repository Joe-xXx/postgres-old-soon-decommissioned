/*-------------------------------------------------------------------------
 *
 * defrem.h
 *	  POSTGRES define and remove utility definitions.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef DEFREM_H
#define DEFREM_H

#include "nodes/parsenodes.h"

#define DEFAULT_TYPDELIM		','

/*
 * prototypes in indexcmds.c
 */
extern void DefineIndex(RangeVar *heapRelation,
			char *indexRelationName,
			char *accessMethodName,
			List *attributeList,
			bool unique,
			bool primary,
			Expr *predicate,
			List *rangetable);
extern void RemoveIndex(RangeVar *relation, DropBehavior behavior);
extern void ReindexIndex(RangeVar *indexRelation, bool force);
extern void ReindexTable(RangeVar *relation, bool force);
extern void ReindexDatabase(const char *databaseName, bool force, bool all);

/*
 * DefineFoo and RemoveFoo are now both in foocmds.c
 */

extern void CreateFunction(CreateFunctionStmt *stmt);
extern void RemoveFunction(List *functionName, List *argTypes);

extern void DefineOperator(List *names, List *parameters);
extern void RemoveOperator(RemoveOperStmt *stmt);

extern void DefineAggregate(List *names, List *parameters);
extern void RemoveAggregate(List *aggName, TypeName *aggType);

extern void DefineType(List *names, List *parameters);
extern void RemoveType(List *names, DropBehavior behavior);
extern void DefineDomain(CreateDomainStmt *stmt);
extern void RemoveDomain(List *names, DropBehavior behavior);


/* support routines in commands/define.c */

extern void case_translate_language_name(const char *input, char *output);

extern char *defGetString(DefElem *def);
extern double defGetNumeric(DefElem *def);
extern int64 defGetInt64(DefElem *def);
extern List *defGetQualifiedName(DefElem *def);
extern TypeName *defGetTypeName(DefElem *def);
extern int	defGetTypeLength(DefElem *def);

#endif   /* DEFREM_H */
