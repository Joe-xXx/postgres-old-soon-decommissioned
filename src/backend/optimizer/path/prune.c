/*-------------------------------------------------------------------------
 *
 * prune.c--
 *	  Routines to prune redundant paths and relations
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/relation.h"

#include "optimizer/internal.h"
#include "optimizer/cost.h"
#include "optimizer/paths.h"
#include "optimizer/pathnode.h"

#include "utils/elog.h"


static List *prune_joinrel(RelOptInfo * rel, List *other_rels);

/*
 * prune-joinrels--
 *	  Removes any redundant relation entries from a list of rel nodes
 *	  'rel-list'.  Obviously, the first relation can't be a duplicate.
 *
 * Returns the resulting list.
 *
 */
void
prune_joinrels(List *rel_list)
{
	List	   *i;

	/*
	 * rel_list can shorten while running as duplicate relations are
	 * deleted
	 */
	foreach(i, rel_list)
		lnext(i) = prune_joinrel((RelOptInfo *) lfirst(i), lnext(i));
}

/*
 * prune-joinrel--
 *	  Prunes those relations from 'other-rels' that are redundant with
 *	  'rel'.  A relation is redundant if it is built up of the same
 *	  relations as 'rel'.  Paths for the redundant relation are merged into
 *	  the pathlist of 'rel'.
 *
 * Returns a list of non-redundant relations, and sets the pathlist field
 * of 'rel' appropriately.
 *
 */
static List *
prune_joinrel(RelOptInfo *rel, List *other_rels)
{
	List	   *i = NIL;
	List	   *result = NIL;

	foreach(r1, other_rels)
	{
		RelOptInfo *other_rel = (RelOptInfo *) lfirst(r1);

		if (same(rel->relids, other_rel->relids))
			/*
			 *	This are on the same relations,
			 *	so get the best of their pathlists.
			 */
			rel->pathlist = add_pathlist(rel,
										 rel->pathlist,
										 other_rel->pathlist);
		else
			result = nconc(result, lcons(other_rel, NIL));
	}
	return result;
}

/*
 * prune-rel-paths--
 *	  For each relation entry in 'rel-list' (which corresponds to a join
 *	  relation), set pointers to the unordered path and cheapest paths
 *	  (if the unordered path isn't the cheapest, it is pruned), and
 *	  reset the relation's size field to reflect the join.
 *
 * Returns nothing of interest.
 *
 */
void
prune_rel_paths(List *rel_list)
{
	List	   *x = NIL;
	List	   *y = NIL;
	Path	   *path = NULL;
	RelOptInfo *rel = (RelOptInfo *) NULL;
	JoinPath   *cheapest = (JoinPath *) NULL;

	foreach(x, rel_list)
	{
		rel = (RelOptInfo *) lfirst(x);
		rel->size = 0;
		foreach(y, rel->pathlist)
		{
			path = (Path *) lfirst(y);

			if (!path->p_ordering.ord.sortop)
				break;
		}
		cheapest = (JoinPath *) prune_rel_path(rel, path);
		if (IsA_JoinPath(cheapest))
			rel->size = compute_joinrel_size(cheapest);
		else
			elog(ERROR, "non JoinPath called");
	}
}


/*
 * prune-rel-path--
 *	  Compares the unordered path for a relation with the cheapest path. If
 *	  the unordered path is not cheapest, it is pruned.
 *
 *	  Resets the pointers in 'rel' for unordered and cheapest paths.
 *
 * Returns the cheapest path.
 *
 */
Path *
prune_rel_path(RelOptInfo * rel, Path *unorderedpath)
{
	Path	   *cheapest = set_cheapest(rel, rel->pathlist);

	/* don't prune if not pruneable  -- JMH, 11/23/92 */
	if (unorderedpath != cheapest && rel->pruneable)
	{

		rel->unorderedpath = (Path *) NULL;
		rel->pathlist = lremove(unorderedpath, rel->pathlist);
	}
	else
		rel->unorderedpath = (Path *) unorderedpath;

	return cheapest;
}

/*
 * merge-joinrels--
 *	  Given two lists of rel nodes that are already
 *	  pruned, merge them into one pruned rel node list
 *
 * 'rel-list1' and
 * 'rel-list2' are the rel node lists
 *
 * Returns one pruned rel node list
 */
List *
merge_joinrels(List *rel_list1, List *rel_list2)
{
	List	   *xrel = NIL;

	foreach(xrel, rel_list1)
	{
		RelOptInfo *rel = (RelOptInfo *) lfirst(xrel);

		rel_list2 = prune_joinrel(rel, rel_list2);
	}
	return append(rel_list1, rel_list2);
}

/*
 * prune_oldrels--
 *	  If all the joininfo's in a rel node are inactive,
 *	  that means that this node has been joined into
 *	  other nodes in all possible ways, therefore
 *	  this node can be discarded.  If not, it will cause
 *	  extra complexity of the optimizer.
 *
 * old_rels is a list of rel nodes
 *
 * Returns a new list of rel nodes
 */
List *
prune_oldrels(List *old_rels)
{
	RelOptInfo *rel;
	List	   *joininfo_list,
			   *xjoininfo,
			   *i,
			   *temp_list = NIL;

	foreach(i, old_rels)
	{
		rel = (RelOptInfo *) lfirst(i);
		joininfo_list = rel->joininfo;

		if (joininfo_list == NIL)
			temp_list = lcons(rel, temp_list);
		else
		{
			foreach(xjoininfo, joininfo_list)
			{
				JoinInfo   *joininfo = (JoinInfo *) lfirst(xjoininfo);

				if (!joininfo->inactive)
				{
					temp_list = lcons(rel, temp_list);
					break;
				}
			}
		}
	}
	return temp_list;
}
