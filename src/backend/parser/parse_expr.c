/*-------------------------------------------------------------------------
 *
 * parse_expr.c
 *	  handle expressions in parser
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header$
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "postgres.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodes.h"
#include "nodes/params.h"
#include "nodes/relation.h"
#include "parse.h"
#include "parser/gramparse.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_node.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "utils/builtins.h"

static Node *parser_typecast(Value *expr, TypeName *typename, int typlen);

/*
 * transformExpr -
 *	  analyze and transform expressions. Type checking and type casting is
 *	  done here. The optimizer and the executor cannot handle the original
 *	  (raw) expressions collected by the parse tree. Hence the transformation
 *	  here.
 */
Node *
transformExpr(ParseState *pstate, Node *expr, int precedence)
{
	Node	   *result = NULL;

	if (expr == NULL)
		return NULL;

	switch (nodeTag(expr))
	{
		case T_Attr:
			{
				Attr	   *att = (Attr *) expr;
				Node	   *temp;

				/* what if att.attrs == "*"?? */
				temp = handleNestedDots(pstate, att, &pstate->p_last_resno);
				if (att->indirection != NIL)
				{
					List	   *idx = att->indirection;

					while (idx != NIL)
					{
						A_Indices  *ai = (A_Indices *) lfirst(idx);
						Node	   *lexpr = NULL,
								   *uexpr;

						uexpr = transformExpr(pstate, ai->uidx, precedence);	/* must exists */
						if (exprType(uexpr) != INT4OID)
							elog(WARN, "array index expressions must be int4's");
						if (ai->lidx != NULL)
						{
							lexpr = transformExpr(pstate, ai->lidx, precedence);
							if (exprType(lexpr) != INT4OID)
								elog(WARN, "array index expressions must be int4's");
						}
#if 0
						pfree(ai->uidx);
						if (ai->lidx != NULL)
							pfree(ai->lidx);
#endif
						ai->lidx = lexpr;
						ai->uidx = uexpr;

						/*
						 * note we reuse the list of indices, make sure we
						 * don't free them! Otherwise, make a new list
						 * here
						 */
						idx = lnext(idx);
					}
					result = (Node *) make_array_ref(temp, att->indirection);
				}
				else
				{
					result = temp;
				}
				break;
			}
		case T_A_Const:
			{
				A_Const    *con = (A_Const *) expr;
				Value	   *val = &con->val;

				if (con->typename != NULL)
				{
					result = parser_typecast(val, con->typename, -1);
				}
				else
				{
					result = (Node *) make_const(val);
				}
				break;
			}
		case T_ParamNo:
			{
				ParamNo    *pno = (ParamNo *) expr;
				Oid			toid;
				int			paramno;
				Param	   *param;

				paramno = pno->number;
				toid = param_type(paramno);
				if (!OidIsValid(toid))
				{
					elog(WARN, "Parameter '$%d' is out of range", paramno);
				}
				param = makeNode(Param);
				param->paramkind = PARAM_NUM;
				param->paramid = (AttrNumber) paramno;
				param->paramname = "<unnamed>";
				param->paramtype = (Oid) toid;
				param->param_tlist = (List *) NULL;

				result = (Node *) param;
				break;
			}
		case T_A_Expr:
			{
				A_Expr	   *a = (A_Expr *) expr;

				switch (a->oper)
				{
					case OP:
						{
							Node	   *lexpr = transformExpr(pstate, a->lexpr, precedence);
							Node	   *rexpr = transformExpr(pstate, a->rexpr, precedence);

							result = (Node *) make_op(a->opname, lexpr, rexpr);
						}
						break;
					case ISNULL:
						{
							Node	   *lexpr = transformExpr(pstate, a->lexpr, precedence);

							result = ParseFunc(pstate,
										  "nullvalue", lcons(lexpr, NIL),
											   &pstate->p_last_resno);
						}
						break;
					case NOTNULL:
						{
							Node	   *lexpr = transformExpr(pstate, a->lexpr, precedence);

							result = ParseFunc(pstate,
									   "nonnullvalue", lcons(lexpr, NIL),
											   &pstate->p_last_resno);
						}
						break;
					case AND:
						{
							Expr	   *expr = makeNode(Expr);
							Node	   *lexpr = transformExpr(pstate, a->lexpr, precedence);
							Node	   *rexpr = transformExpr(pstate, a->rexpr, precedence);

							if (exprType(lexpr) != BOOLOID)
								elog(WARN, "left-hand side of AND is type '%s', not bool",
									typeidTypeName(exprType(lexpr)));

							if (exprType(rexpr) != BOOLOID)
								elog(WARN, "right-hand side of AND is type '%s', not bool",
									typeidTypeName(exprType(rexpr)));

							expr->typeOid = BOOLOID;
							expr->opType = AND_EXPR;
							expr->args = makeList(lexpr, rexpr, -1);
							result = (Node *) expr;
						}
						break;
					case OR:
						{
							Expr	   *expr = makeNode(Expr);
							Node	   *lexpr = transformExpr(pstate, a->lexpr, precedence);
							Node	   *rexpr = transformExpr(pstate, a->rexpr, precedence);

							if (exprType(lexpr) != BOOLOID)
								elog(WARN, "left-hand side of OR is type '%s', not bool",
									 typeidTypeName(exprType(lexpr)));
							if (exprType(rexpr) != BOOLOID)
								elog(WARN, "right-hand side of OR is type '%s', not bool",
									 typeidTypeName(exprType(rexpr)));
							expr->typeOid = BOOLOID;
							expr->opType = OR_EXPR;
							expr->args = makeList(lexpr, rexpr, -1);
							result = (Node *) expr;
						}
						break;
					case NOT:
						{
							Expr	   *expr = makeNode(Expr);
							Node	   *rexpr = transformExpr(pstate, a->rexpr, precedence);

							if (exprType(rexpr) != BOOLOID)
								elog(WARN, "argument to NOT is type '%s', not bool",
									 typeidTypeName(exprType(rexpr)));
							expr->typeOid = BOOLOID;
							expr->opType = NOT_EXPR;
							expr->args = makeList(rexpr, -1);
							result = (Node *) expr;
						}
						break;
				}
				break;
			}
		case T_Ident:
			{

				/*
				 * look for a column name or a relation name (the default
				 * behavior)
				 */
				result = transformIdent(pstate, expr, precedence);
				break;
			}
		case T_FuncCall:
			{
				FuncCall   *fn = (FuncCall *) expr;
				List	   *args;

				/* transform the list of arguments */
				foreach(args, fn->args)
					lfirst(args) = transformExpr(pstate, (Node *) lfirst(args), precedence);
				result = ParseFunc(pstate,
						  fn->funcname, fn->args, &pstate->p_last_resno);
				break;
			}
		default:
			/* should not reach here */
			elog(WARN, "transformExpr: does not know how to transform node %d",
				 nodeTag(expr));
			break;
	}

	return result;
}

Node *
transformIdent(ParseState *pstate, Node *expr, int precedence)
{
	Ident	   *ident = (Ident *) expr;
	RangeTblEntry *rte;
	Node	   *column_result,
			   *relation_result,
			   *result;

	column_result = relation_result = result = 0;
	/* try to find the ident as a column */
	if ((rte = colnameRangeTableEntry(pstate, ident->name)) != NULL)
	{
		Attr	   *att = makeNode(Attr);

		att->relname = rte->refname;
		att->attrs = lcons(makeString(ident->name), NIL);
		column_result =
			(Node *) handleNestedDots(pstate, att, &pstate->p_last_resno);
	}

	/* try to find the ident as a relation */
	if (refnameRangeTableEntry(pstate->p_rtable, ident->name) != NULL)
	{
		ident->isRel = TRUE;
		relation_result = (Node *) ident;
	}

	/* choose the right result based on the precedence */
	if (precedence == EXPR_COLUMN_FIRST)
	{
		if (column_result)
			result = column_result;
		else
			result = relation_result;
	}
	else
	{
		if (relation_result)
			result = relation_result;
		else
			result = column_result;
	}

	if (result == NULL)
		elog(WARN, "attribute '%s' not found", ident->name);

	return result;
}

/*
 *	exprType -
 *	  returns the Oid of the type of the expression. (Used for typechecking.)
 */
Oid
exprType(Node *expr)
{
	Oid			type = (Oid) 0;

	switch (nodeTag(expr))
	{
		case T_Func:
			type = ((Func *) expr)->functype;
			break;
		case T_Iter:
			type = ((Iter *) expr)->itertype;
			break;
		case T_Var:
			type = ((Var *) expr)->vartype;
			break;
		case T_Expr:
			type = ((Expr *) expr)->typeOid;
			break;
		case T_Const:
			type = ((Const *) expr)->consttype;
			break;
		case T_ArrayRef:
			type = ((ArrayRef *) expr)->refelemtype;
			break;
		case T_Aggreg:
			type = ((Aggreg *) expr)->aggtype;
			break;
		case T_Param:
			type = ((Param *) expr)->paramtype;
			break;
		case T_Ident:
			/* is this right? */
			type = UNKNOWNOID;
			break;
		default:
			elog(WARN, "exprType: don't know how to get type for %d node",
				 nodeTag(expr));
			break;
	}
	return type;
}

/*
 ** HandleNestedDots --
 **    Given a nested dot expression (i.e. (relation func ... attr), build up
 ** a tree with of Iter and Func nodes.
 */
Node *
handleNestedDots(ParseState *pstate, Attr *attr, int *curr_resno)
{
	List	   *mutator_iter;
	Node	   *retval = NULL;

	if (attr->paramNo != NULL)
	{
		Param	   *param = (Param *) transformExpr(pstate, (Node *) attr->paramNo, EXPR_RELATION_FIRST);

		retval =
			ParseFunc(pstate, strVal(lfirst(attr->attrs)),
					  lcons(param, NIL),
					  curr_resno);
	}
	else
	{
		Ident	   *ident = makeNode(Ident);

		ident->name = attr->relname;
		ident->isRel = TRUE;
		retval =
			ParseFunc(pstate, strVal(lfirst(attr->attrs)),
					  lcons(ident, NIL),
					  curr_resno);
	}

	foreach(mutator_iter, lnext(attr->attrs))
	{
		retval = ParseFunc(pstate, strVal(lfirst(mutator_iter)),
						   lcons(retval, NIL),
						   curr_resno);
	}

	return (retval);
}

static Node	   *
parser_typecast(Value *expr, TypeName *typename, int typlen)
{
	/* check for passing non-ints */
	Const	   *adt;
	Datum		lcp;
	Type		tp;
	char		type_string[NAMEDATALEN];
	int32		len;
	char	   *cp = NULL;
	char	   *const_string = NULL;
	bool		string_palloced = false;

	switch (nodeTag(expr))
	{
		case T_String:
			const_string = DatumGetPointer(expr->val.str);
			break;
		case T_Integer:
			const_string = (char *) palloc(256);
			string_palloced = true;
			sprintf(const_string, "%ld", expr->val.ival);
			break;
		default:
			elog(WARN,
			"parser_typecast: cannot cast this expression to type '%s'",
				 typename->name);
	}

	if (typename->arrayBounds != NIL)
	{
		sprintf(type_string, "_%s", typename->name);
		tp = (Type) typenameType(type_string);
	}
	else
	{
		tp = (Type) typenameType(typename->name);
	}

	len = typeLen(tp);

#if 0							/* fix me */
	switch (CInteger(lfirst(expr)))
	{
		case INT4OID:			/* int4 */
			const_string = (char *) palloc(256);
			string_palloced = true;
			sprintf(const_string, "%d", ((Const *) lnext(expr))->constvalue);
			break;

		case NAMEOID:			/* char16 */
			const_string = (char *) palloc(256);
			string_palloced = true;
			sprintf(const_string, "%s", ((Const *) lnext(expr))->constvalue);
			break;

		case CHAROID:			/* char */
			const_string = (char *) palloc(256);
			string_palloced = true;
			sprintf(const_string, "%c", ((Const) lnext(expr))->constvalue);
			break;

		case FLOAT8OID: /* float8 */
			const_string = (char *) palloc(256);
			string_palloced = true;
			sprintf(const_string, "%f", ((Const) lnext(expr))->constvalue);
			break;

		case CASHOID:			/* money */
			const_string = (char *) palloc(256);
			string_palloced = true;
			sprintf(const_string, "%d",
					(int) ((Const *) expr)->constvalue);
			break;

		case TEXTOID:			/* text */
			const_string = DatumGetPointer(((Const) lnext(expr))->constvalue);
			const_string = (char *) textout((struct varlena *) const_string);
			break;

		case UNKNOWNOID:		/* unknown */
			const_string = DatumGetPointer(((Const) lnext(expr))->constvalue);
			const_string = (char *) textout((struct varlena *) const_string);
			break;

		default:
			elog(WARN, "unknown type %d", CInteger(lfirst(expr)));
	}
#endif

	cp = stringTypeString(tp, const_string, typlen);

	if (!typeByVal(tp))
	{
/*
		if (len >= 0 && len != PSIZE(cp)) {
			char *pp;
			pp = (char *) palloc(len);
			memmove(pp, cp, len);
			cp = pp;
		}
*/
		lcp = PointerGetDatum(cp);
	}
	else
	{
		switch (len)
		{
			case 1:
				lcp = Int8GetDatum(cp);
				break;
			case 2:
				lcp = Int16GetDatum(cp);
				break;
			case 4:
				lcp = Int32GetDatum(cp);
				break;
			default:
				lcp = PointerGetDatum(cp);
				break;
		}
	}

	adt = makeConst(typeTypeId(tp),
					len,
					(Datum) lcp,
					false,
					typeByVal(tp),
					false,		/* not a set */
					true /* is cast */ );

	if (string_palloced)
		pfree(const_string);

	return (Node *) adt;
}

Node	   *
parser_typecast2(Node *expr, Oid exprType, Type tp, int typlen)
{
	/* check for passing non-ints */
	Const	   *adt;
	Datum		lcp;
	int32		len = typeLen(tp);
	char	   *cp = NULL;

	char	   *const_string = NULL;
	bool		string_palloced = false;

	Assert(IsA(expr, Const));

	switch (exprType)
	{
		case 0:			/* NULL */
			break;
		case INT4OID:			/* int4 */
			const_string = (char *) palloc(256);
			string_palloced = true;
			sprintf(const_string, "%d",
					(int) ((Const *) expr)->constvalue);
			break;
		case NAMEOID:			/* char16 */
			const_string = (char *) palloc(256);
			string_palloced = true;
			sprintf(const_string, "%s",
					(char *) ((Const *) expr)->constvalue);
			break;
		case CHAROID:			/* char */
			const_string = (char *) palloc(256);
			string_palloced = true;
			sprintf(const_string, "%c",
					(char) ((Const *) expr)->constvalue);
			break;
		case FLOAT4OID: /* float4 */
			{
				float32		floatVal =
				DatumGetFloat32(((Const *) expr)->constvalue);

				const_string = (char *) palloc(256);
				string_palloced = true;
				sprintf(const_string, "%f", *floatVal);
				break;
			}
		case FLOAT8OID: /* float8 */
			{
				float64		floatVal =
				DatumGetFloat64(((Const *) expr)->constvalue);

				const_string = (char *) palloc(256);
				string_palloced = true;
				sprintf(const_string, "%f", *floatVal);
				break;
			}
		case CASHOID:			/* money */
			const_string = (char *) palloc(256);
			string_palloced = true;
			sprintf(const_string, "%ld",
					(long) ((Const *) expr)->constvalue);
			break;
		case TEXTOID:			/* text */
			const_string =
				DatumGetPointer(((Const *) expr)->constvalue);
			const_string = (char *) textout((struct varlena *) const_string);
			break;
		case UNKNOWNOID:		/* unknown */
			const_string =
				DatumGetPointer(((Const *) expr)->constvalue);
			const_string = (char *) textout((struct varlena *) const_string);
			break;
		default:
			elog(WARN, "unknown type %u", exprType);
	}

	if (!exprType)
	{
		adt = makeConst(typeTypeId(tp),
						(Size) 0,
						(Datum) NULL,
						true,	/* isnull */
						false,	/* was omitted */
						false,	/* not a set */
						true /* is cast */ );
		return ((Node *) adt);
	}

	cp = stringTypeString(tp, const_string, typlen);


	if (!typeByVal(tp))
	{
/*
		if (len >= 0 && len != PSIZE(cp)) {
			char *pp;
			pp = (char *) palloc(len);
			memmove(pp, cp, len);
			cp = pp;
		}
*/
		lcp = PointerGetDatum(cp);
	}
	else
	{
		switch (len)
		{
			case 1:
				lcp = Int8GetDatum(cp);
				break;
			case 2:
				lcp = Int16GetDatum(cp);
				break;
			case 4:
				lcp = Int32GetDatum(cp);
				break;
			default:
				lcp = PointerGetDatum(cp);
				break;
		}
	}

	adt = makeConst(typeTypeId(tp),
					(Size) len,
					(Datum) lcp,
					false,
					false,		/* was omitted */
					false,		/* not a set */
					true /* is cast */ );

	/*
	 * printf("adt %s : %u %d %d\n",CString(expr),typeTypeId(tp) , len,cp);
	 */
	if (string_palloced)
		pfree(const_string);

	return ((Node *) adt);
} 
