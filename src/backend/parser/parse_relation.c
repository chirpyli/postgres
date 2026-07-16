/*-------------------------------------------------------------------------
 *
 * parse_relation.c
 *	  处理关系的解析器支持例程
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_relation.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "access/relation.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/heap.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_enr.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "parser/parsetree.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/varlena.h"


/*
 * 支持对列进行模糊匹配。
 *
 * 这用于构建诊断消息，此时我们关注多个或不精确的匹配属性。
 *
 * 若 rfirst 不为 NULL，"distance" 表示当前最佳的模糊匹
 * 配距离；否则它表示可接受的最大距离加 1。
 *
 * rfirst/first 记录到目前为止最接近的非精确匹配，distance 为该匹配
 * 与目标名称之间的距离。如果我们找到了第二个距离完全相同的非精确匹配，
 * 则由 rsecond/second 记录。（如果我们找到了三个距离相同的匹配，
 * 就说明 "distance" 这个界限还不足以给出有用的提示，于是再次清空
 * rfirst/rsecond。只有后来找到了更近的匹配时，才会重新填充 rfirst。）
 *
 * rexact1/exact1 记录第一个精确匹配列的位置（如果存在）。如果找到了
 * 多个精确匹配，则再由 rexact2/exact2 记录另一个（我们不关心具体是
 * 哪一个）。目前这些字段的填充与模糊匹配字段相互独立。
 */
typedef struct
{
	int			distance;		/* 当前或最大距离 */
	RangeTblEntry *rfirst;		/* 最接近的非精确匹配的 RTE，或 NULL */
	AttrNumber	first;			/* rfirst 中的列索引 */
	RangeTblEntry *rsecond;		/* 另一个相同距离的非精确匹配的 RTE */
	AttrNumber	second;			/* rsecond 中的列索引 */
	RangeTblEntry *rexact1;		/* 第一个精确匹配的 RTE，或 NULL */
	AttrNumber	exact1;			/* rexact1 中的列索引 */
	RangeTblEntry *rexact2;		/* 第二个精确匹配的 RTE，或 NULL */
	AttrNumber	exact2;			/* rexact2 中的列索引 */
} FuzzyAttrMatchState;

#define MAX_FUZZY_DISTANCE				3


static ParseNamespaceItem *scanNameSpaceForRefname(ParseState *pstate,
												   const char *refname,
												   int location);
static ParseNamespaceItem *scanNameSpaceForRelid(ParseState *pstate, Oid relid,
												 int location);
static void check_lateral_ref_ok(ParseState *pstate, ParseNamespaceItem *nsitem,
								 int location);
static int	scanRTEForColumn(ParseState *pstate, RangeTblEntry *rte,
							 Alias *eref,
							 const char *colname, int location,
							 int fuzzy_rte_penalty,
							 FuzzyAttrMatchState *fuzzystate);
static void markRTEForSelectPriv(ParseState *pstate,
								 int rtindex, AttrNumber col);
static void expandRelation(Oid relid, Alias *eref,
						   int rtindex, int sublevels_up,
						   VarReturningType returning_type,
						   int location, bool include_dropped,
						   List **colnames, List **colvars);
static void expandTupleDesc(TupleDesc tupdesc, Alias *eref,
							int count, int offset,
							int rtindex, int sublevels_up,
							VarReturningType returning_type,
							int location, bool include_dropped,
							List **colnames, List **colvars);
static int	specialAttNum(const char *attname);
static bool rte_visible_if_lateral(ParseState *pstate, RangeTblEntry *rte);
static bool rte_visible_if_qualified(ParseState *pstate, RangeTblEntry *rte);
static bool isQueryUsingTempRelation_walker(Node *node, void *context);


/*
 * refnameNamespaceItem
 *	  给定一个可能带模式限定的引用名，查看它是否匹配任意可见的
 *	  命名空间项。若匹配，返回该 nsitem 的指针；否则返回 NULL。
 *
 *	  可选择将 nsitem 的嵌套深度（0 = 当前层）写入 *sublevels_up。
 *	  若 sublevels_up 为 NULL，则只考虑当前嵌套层中的项。
 *
 * 不带限定的引用名（schemaname == NULL）可以匹配任意别名相同的项，
 * 或者（对于无别名的关系项）匹配未限定关系名相同的项。这样的引用名
 * 有可能在最近一层有匹配的嵌套层级中匹配到多个项；若是如此，我们
 * 通过 ereport() 报告一个错误。
 *
 * 带限定的引用名（schemaname != NULL）只能匹配满足以下条件的关系项：
 * (a) 没有别名，且 (b) 对应于由 schemaname.refname 标识的同一个关系。
 * 在这种情况下，我们将 schemaname.refname 转换为关系 OID 并按 relid
 * 搜索，而不是按别名搜索。这看起来有些古怪，但 SQL 标准就是这么要求的。
 * 在处理查询的 RETURNING 列表时，可能会存在额外的 OLD 和 NEW 命名空间
 * 项，它们与目标命名空间项具有相同的 relation OID。这些项在搜索中被
 * 忽略，因为它们无法通过 schemaname.refname 匹配。
 */
ParseNamespaceItem *
refnameNamespaceItem(ParseState *pstate,
					 const char *schemaname,
					 const char *refname,
					 int location,
					 int *sublevels_up)
{
	Oid			relId = InvalidOid;

	if (sublevels_up)
		*sublevels_up = 0;

	if (schemaname != NULL)
	{
		Oid			namespaceId;

		/*
		 * 这里可以使用 LookupNamespaceNoError()，因为我们只关心找到已存在
		 * 的 RTE。检查模式的 USAGE 权限是不必要的，因为该权限在 RTE 创建
		 * 时就已经检查过了。此外，如果名称恰好与用户无权访问的某个模式名
		 * 相同，我们希望报告 "RTE not found"（未找到 RTE），而不是
		 * "no permissions for schema"（对模式没有权限）。
		 */
		namespaceId = LookupNamespaceNoError(schemaname);
		if (!OidIsValid(namespaceId))
			return NULL;
		relId = get_relname_relid(refname, namespaceId);
		if (!OidIsValid(relId))
			return NULL;
	}

	while (pstate != NULL)
	{
		ParseNamespaceItem *result;

		if (OidIsValid(relId))
			result = scanNameSpaceForRelid(pstate, relId, location);
		else
			result = scanNameSpaceForRefname(pstate, refname, location);

		if (result)
			return result;

		if (sublevels_up)
			(*sublevels_up)++;
		else
			break;

		pstate = pstate->parentParseState;
	}
	return NULL;
}

/*
 * 在查询的表命名空间中搜索与给定未限定引用名匹配的项。若唯一匹配则返回
 * nsitem；若无匹配则返回 NULL。若出现多个匹配则报错。
 *
 * 注意：看起来我们似乎不必担心出现多个匹配的可能性，毕竟 SQL 标准禁止在
 * 同一个 SELECT 层级内出现重复的表别名。然而历史上 PostgreSQL 一直比这
 * 更宽松。例如，我们允许
 *		SELECT ... FROM tab1 x CROSS JOIN (tab2 x CROSS JOIN tab3 y) z
 * 其理由是带别名的连接 (z) 隐藏了其内部的别名，因此两个名为 "x" 的 RTE
 * 之间并不存在冲突。不过，如果 tab3 是一个 LATERAL 子查询，那么在该子查询
 * 内部两个 "x" 都是可见的。我们没有拒绝这些曾经可以工作的查询，而是允许
 * 这种情况，并且仅在确实出现了对 "x" 的歧义引用时才报错。
 */
static ParseNamespaceItem *
scanNameSpaceForRefname(ParseState *pstate, const char *refname, int location)
{
	ParseNamespaceItem *result = NULL;
	ListCell   *l;

	foreach(l, pstate->p_namespace)
	{
		ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(l);

		/* 忽略仅列项 */
		if (!nsitem->p_rel_visible)
			continue;
		/* 若不在 LATERAL 内部，忽略仅 LATERAL 项 */
		if (nsitem->p_lateral_only && !pstate->p_lateral_active)
			continue;

		if (strcmp(nsitem->p_names->aliasname, refname) == 0)
		{
			if (result)
				ereport(ERROR,
						(errcode(ERRCODE_AMBIGUOUS_ALIAS),
						 errmsg("table reference \"%s\" is ambiguous",
								refname),
						 parser_errposition(pstate, location)));
			check_lateral_ref_ok(pstate, nsitem, location);
			result = nsitem;
		}
	}
	return result;
}

/*
 * 在查询的表命名空间中搜索与给定关系 OID 匹配的关系项。若唯一匹配则返回
 * nsitem；若无匹配则返回 NULL。若出现多个匹配则报错。
 *
 * 要了解它为何如此工作，请参阅 refnameNamespaceItem 的注释。
 */
static ParseNamespaceItem *
scanNameSpaceForRelid(ParseState *pstate, Oid relid, int location)
{
	ParseNamespaceItem *result = NULL;
	ListCell   *l;

	foreach(l, pstate->p_namespace)
	{
		ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(l);
		RangeTblEntry *rte = nsitem->p_rte;

		/* 忽略仅列项 */
		if (!nsitem->p_rel_visible)
			continue;
		/* 若不在 LATERAL 内部，忽略仅 LATERAL 项 */
		if (nsitem->p_lateral_only && !pstate->p_lateral_active)
			continue;
		/* 忽略可能出现在 RETURNING 中的 OLD/NEW 命名空间项 */
		if (nsitem->p_returning_type != VAR_RETURNING_DEFAULT)
			continue;

		/* 是的，这里应当有 alias == NULL 的判断... */
		if (rte->rtekind == RTE_RELATION &&
			rte->relid == relid &&
			rte->alias == NULL)
		{
			if (result)
				ereport(ERROR,
						(errcode(ERRCODE_AMBIGUOUS_ALIAS),
						 errmsg("table reference %u is ambiguous",
								relid),
						 parser_errposition(pstate, location)));
			check_lateral_ref_ok(pstate, nsitem, location);
			result = nsitem;
		}
	}
	return result;
}

/*
 * 在查询的 CTE 命名空间中搜索与给定未限定引用名匹配的 CTE。若匹配则返回
 * CTE（及其 levelsup 计数）；若无匹配则返回 NULL。我们不必担心出现多个
 * 匹配，因为 parse_cte.c 会拒绝包含重复 CTE 名称的 WITH 列表。
 */
CommonTableExpr *
scanNameSpaceForCTE(ParseState *pstate, const char *refname,
					Index *ctelevelsup)
{
	Index		levelsup;

	for (levelsup = 0;
		 pstate != NULL;
		 pstate = pstate->parentParseState, levelsup++)
	{
		ListCell   *lc;

		foreach(lc, pstate->p_ctenamespace)
		{
			CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

			if (strcmp(cte->ctename, refname) == 0)
			{
				*ctelevelsup = levelsup;
				return cte;
			}
		}
	}
	return NULL;
}

/*
 * 搜索一个可能的 "future CTE"，即按照 WITH 作用域规则尚不在作用域内的
 * CTE。这与合法的 SQL 语义无关，但对于错误报告很重要。
 */
static bool
isFutureCTE(ParseState *pstate, const char *refname)
{
	for (; pstate != NULL; pstate = pstate->parentParseState)
	{
		ListCell   *lc;

		foreach(lc, pstate->p_future_ctes)
		{
			CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

			if (strcmp(cte->ctename, refname) == 0)
				return true;
		}
	}
	return false;
}

/*
 * 在查询的临时命名关系（ephemeral named relation）命名空间中搜索与给定
 * 未限定引用名匹配的关系。
 */
bool
scanNameSpaceForENR(ParseState *pstate, const char *refname)
{
	return name_matches_visible_ENR(pstate, refname);
}

/*
 * searchRangeTableForRel
 *	  查看是否有任意 RangeTblEntry 有可能匹配该 RangeVar。
 *	  若如此，返回该 RangeTblEntry 的指针；否则返回 NULL。
 *
 * 这与 refnameNamespaceItem 不同之处在于：它会考虑 ParseState 的
 * rangetable（s）中的每一个条目，而不仅仅是当前在 p_namespace 列表（s）
 * 中可见的那些。这种行为按照 SQL 规范是无效的，并且可能产生歧义的结果
 * （可能存在多个同样有效的匹配，但只会返回其中一个）。它只能作为启发式
 * 手段用于给出合适的错误消息。请参阅 errorMissingRTE。
 *
 * 注意，我们既考虑实际关系（或 CTE）名称的匹配，也考虑别名的匹配。
 */
static RangeTblEntry *
searchRangeTableForRel(ParseState *pstate, RangeVar *relation)
{
	const char *refname = relation->relname;
	Oid			relId = InvalidOid;
	CommonTableExpr *cte = NULL;
	bool		isenr = false;
	Index		ctelevelsup = 0;
	Index		levelsup;

	/*
	 * 如果是不带限定的名称，则检查是否存在可能的 CTE 匹配。CTE 会隐藏任何
	 * 真实的匹配关系。如果没有 CTE，则查找匹配的关系。
	 *
	 * 注意：在这里，面对并发 DDL 时 RangeVarGetRelid 返回正确答案并不关键。
	 * 如果它没有返回正确答案，最坏情况只是错误消息不够清晰。此外，查询中
	 * 涉及的表已经被锁定，这减少了出现意外行为的情形。因此我们以不加锁的
	 * 方式进行名称查找。
	 */
	if (!relation->schemaname)
	{
		cte = scanNameSpaceForCTE(pstate, refname, &ctelevelsup);
		if (!cte)
			isenr = scanNameSpaceForENR(pstate, refname);
	}

	if (!cte && !isenr)
		relId = RangeVarGetRelid(relation, NoLock, true);

	/* 现在查找与关系/CTE/ENR 或别名匹配的 RTE */
	for (levelsup = 0;
		 pstate != NULL;
		 pstate = pstate->parentParseState, levelsup++)
	{
		ListCell   *l;

		foreach(l, pstate->p_rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);

			if (rte->rtekind == RTE_RELATION &&
				OidIsValid(relId) &&
				rte->relid == relId)
				return rte;
			if (rte->rtekind == RTE_CTE &&
				cte != NULL &&
				rte->ctelevelsup + levelsup == ctelevelsup &&
				strcmp(rte->ctename, refname) == 0)
				return rte;
			if (rte->rtekind == RTE_NAMEDTUPLESTORE &&
				isenr &&
				strcmp(rte->enrname, refname) == 0)
				return rte;
			if (strcmp(rte->eref->aliasname, refname) == 0)
				return rte;
		}
	}
	return NULL;
}

/*
 * 检查两个命名空间列表之间是否存在关系名称冲突。若发现冲突则报错。
 *
 * 注意：我们假设给定的每个参数自身不包含冲突；我们只想知道这两者能否
 * 合并在一起。
 *
 * 按照 SQL 标准，两个无别名的普通关系 RTE 即使具有相同的 eref->aliasname
 * （即相同的关系名）也不会冲突，前提是它们对应不同的关系 OID（意味着
 * 它们位于不同的模式中）。
 *
 * 我们忽略命名空间项中的 lateral-only 标志：即使所有项都被视为可见，这两
 * 个列表也不得冲突。不过，仅列项应当被忽略。
 */
void
checkNameSpaceConflicts(ParseState *pstate, List *namespace1,
						List *namespace2)
{
	ListCell   *l1;

	foreach(l1, namespace1)
	{
		ParseNamespaceItem *nsitem1 = (ParseNamespaceItem *) lfirst(l1);
		RangeTblEntry *rte1 = nsitem1->p_rte;
		const char *aliasname1 = nsitem1->p_names->aliasname;
		ListCell   *l2;

		if (!nsitem1->p_rel_visible)
			continue;

		foreach(l2, namespace2)
		{
			ParseNamespaceItem *nsitem2 = (ParseNamespaceItem *) lfirst(l2);
			RangeTblEntry *rte2 = nsitem2->p_rte;
			const char *aliasname2 = nsitem2->p_names->aliasname;

			if (!nsitem2->p_rel_visible)
				continue;
			if (strcmp(aliasname2, aliasname1) != 0)
				continue;		/* 确定无冲突 */
			if (rte1->rtekind == RTE_RELATION && rte1->alias == NULL &&
				rte2->rtekind == RTE_RELATION && rte2->alias == NULL &&
				rte1->relid != rte2->relid)
				continue;		/* 按照 SQL 规则无冲突 */
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_ALIAS),
					 errmsg("table name \"%s\" specified more than once",
							aliasname1)));
		}
	}
}

/*
 * 如果一个命名空间项当前不允许作为 LATERAL 引用，则报错。
 * 这既强制执行 SQL:2008 对于位于外连接错误一侧的 LATERAL 引用该如何处理
 * 这一相当古怪的规定，也执行我们自身关于禁止在 FROM/USING 子句中将
 * UPDATE 或 DELETE 的目标表作为 LATERAL 引用这一禁令。
 *
 * 注意：pstate 应当与找到该 nsitem 的同一查询层级。
 *
 * 这是一个辅助子例程，用于避免多次复制一段相当难看的 ereport 代码。
 */
static void
check_lateral_ref_ok(ParseState *pstate, ParseNamespaceItem *nsitem,
					 int location)
{
	if (nsitem->p_lateral_only && !nsitem->p_lateral_ok)
	{
		/* SQL:2008 要求这应当是一个错误，而不是不可见项 */
		RangeTblEntry *rte = nsitem->p_rte;
		char	   *refname = nsitem->p_names->aliasname;

		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("invalid reference to FROM-clause entry for table \"%s\"",
						refname),
				 (pstate->p_target_nsitem != NULL &&
				  rte == pstate->p_target_nsitem->p_rte) ?
				 errhint("There is an entry for table \"%s\", but it cannot be referenced from this part of the query.",
						 refname) :
				 errdetail("The combining JOIN type must be INNER or LEFT for a LATERAL reference."),
				 parser_errposition(pstate, location)));
	}
}

/*
 * 给定一个 RT 索引和嵌套深度，找到对应的 ParseNamespaceItem
 * （必定存在这样一个项）。
 *
 * 注意：从 Var 出发的调用者应当考虑改用 GetNSItemByVar()，
 * 以查找具有匹配 varreturningtype 的命名空间项。
 */
ParseNamespaceItem *
GetNSItemByRangeTablePosn(ParseState *pstate,
						  int varno,
						  int sublevels_up)
{
	ListCell   *lc;

	while (sublevels_up-- > 0)
	{
		pstate = pstate->parentParseState;
		Assert(pstate != NULL);
	}
	foreach(lc, pstate->p_namespace)
	{
		ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(lc);

		if (nsitem->p_rtindex == varno)
			return nsitem;
	}
	elog(ERROR, "nsitem not found (internal error)");
	return NULL;				/* 让编译器保持安静（避免告警） */
}

/*
 * 给定一个 Var，找到对应的 ParseNamespaceItem（必定存在这样一个项）。
 *
 * 与 GetNSItemByRangeTablePosn() 类似，但除了 Var 的 varno 和 varlevelsup
 * 之外，还使用 Var 的 varreturningtype 来查找命名空间项。
 */
ParseNamespaceItem *
GetNSItemByVar(ParseState *pstate, Var *var)
{
	int			sublevels_up = var->varlevelsup;
	ListCell   *lc;

	while (sublevels_up-- > 0)
	{
		pstate = pstate->parentParseState;
		Assert(pstate != NULL);
	}
	foreach(lc, pstate->p_namespace)
	{
		ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(lc);

		if (nsitem->p_rtindex == var->varno &&
			nsitem->p_returning_type == var->varreturningtype)
			return nsitem;
	}
	elog(ERROR, "nsitem not found (internal error)");
	return NULL;				/* 让编译器保持安静（避免告警） */
}

/*
 * 给定一个 RT 索引和嵌套深度，找到对应的 RTE。
 * （注意该 RTE 不必位于查询的命名空间中。）
 */
RangeTblEntry *
GetRTEByRangeTablePosn(ParseState *pstate,
					   int varno,
					   int sublevels_up)
{
	while (sublevels_up-- > 0)
	{
		pstate = pstate->parentParseState;
		Assert(pstate != NULL);
	}
	Assert(varno > 0 && varno <= list_length(pstate->p_rtable));
	return rt_fetch(varno, pstate->p_rtable);
}

/*
 * 为 CTE 引用的 RTE 获取对应的 CTE。
 *
 * rtelevelsup 是该 RTE 所在查询层级相对于给定 pstate 之上的查询层级数。
 */
CommonTableExpr *
GetCTEForRTE(ParseState *pstate, RangeTblEntry *rte, int rtelevelsup)
{
	Index		levelsup;
	ListCell   *lc;

	Assert(rte->rtekind == RTE_CTE);
	levelsup = rte->ctelevelsup + rtelevelsup;
	while (levelsup-- > 0)
	{
		pstate = pstate->parentParseState;
		if (!pstate)			/* 不应发生 */
			elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
	}
	foreach(lc, pstate->p_ctenamespace)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

		if (strcmp(cte->ctename, rte->ctename) == 0)
			return cte;
	}
	/* 不应发生 */
	elog(ERROR, "could not find CTE \"%s\"", rte->ctename);
	return NULL;				/* 让编译器保持安静（避免告警） */
}

/*
 * updateFuzzyAttrMatchState
 *	  使用 Levenshtein 距离，判断该列是否为最佳模糊匹配。
 */
static void
updateFuzzyAttrMatchState(int fuzzy_rte_penalty,
						  FuzzyAttrMatchState *fuzzystate, RangeTblEntry *rte,
						  const char *actual, const char *match, int attnum)
{
	int			columndistance;
	int			matchlen;

	/* 若无希望，在计算 Levenshtein 距离之前就直接返回。 */
	if (fuzzy_rte_penalty > fuzzystate->distance)
		return;

		/*
		 * 直接拒绝已删除的列，按照 scanRTEForColumn() 中的说明，这些列可能
		 * 会以明显为空的真实名称出现在这里。
		 */
	if (actual[0] == '\0')
		return;

	/* 使用 Levenshtein 计算匹配距离。 */
	matchlen = strlen(match);
	columndistance =
		varstr_levenshtein_less_equal(actual, strlen(actual), match, matchlen,
									  1, 1, 1,
									  fuzzystate->distance + 1
									  - fuzzy_rte_penalty,
									  true);

	/*
	 * 如果超过一半的字符不同，则不要将其视为匹配，以避免给出荒谬的
	 * 建议。
	 */
	if (columndistance > matchlen / 2)
		return;

	/*
	 * 从这一点开始，我们可以忽略 RTE 名称距离与列名称距离之间的区别。
	 */
	columndistance += fuzzy_rte_penalty;

	/*
	 * 如果新距离小于或等于目前为止找到的最佳匹配的距离，则更新
	 * fuzzystate。
	 */
	if (columndistance < fuzzystate->distance)
	{
		/* 将新观察到的最小距离作为第一个（也是唯一的）匹配保存 */
		fuzzystate->distance = columndistance;
		fuzzystate->rfirst = rte;
		fuzzystate->first = attnum;
		fuzzystate->rsecond = NULL;
	}
	else if (columndistance == fuzzystate->distance)
	{
		/* 如果已经有一个该距离的匹配，则更新状态 */
		if (fuzzystate->rsecond != NULL)
		{
			/*
			 * 同一距离的匹配太多了。显然，这个距离值作为门槛太低了，因此
			 * 在保留当前距离值的同时丢弃这些条目，这样只有更小的距离才会
			 * 被视为有意义。只有当我们找到距离更小的匹配时，才会重新填充
			 * rfirst（经由上面的段落）。
			 */
			fuzzystate->rfirst = NULL;
			fuzzystate->rsecond = NULL;
		}
		else if (fuzzystate->rfirst != NULL)
		{
			/* 作为临时第二个匹配记录 */
			fuzzystate->rsecond = rte;
			fuzzystate->second = attnum;
		}
		else
		{
			/*
			 * 什么都不做。当 rfirst 为 NULL 时，距离超出了我们愿意视为
			 * 可接受的阈值，因此我们应当忽略这次匹配。
			 */
		}
	}
}

/*
 * scanNSItemForColumn
 *	  在单个命名空间项的列名中搜索给定的名称。
 *	  若找到，返回合适的 Var 节点；否则返回 NULL。
 *	  如果该名称在此 nsitem 中存在歧义，则报错。
 *
 * 副作用：如果找到了匹配项，则将对应的 RTE 标记为需要该列的读权限。
 */
Node *
scanNSItemForColumn(ParseState *pstate, ParseNamespaceItem *nsitem,
					int sublevels_up, const char *colname, int location)
{
	RangeTblEntry *rte = nsitem->p_rte;
	int			attnum;
	Var		   *var;

	/*
	 * 扫描该 nsitem 的列名（或别名）以寻找匹配项。若出现多个匹配则报错。
	 */
	attnum = scanRTEForColumn(pstate, rte, nsitem->p_names,
							  colname, location,
							  0, NULL);

	if (attnum == InvalidAttrNumber)
		return NULL;			/* 若无匹配则返回 NULL */

	/* 在约束检查中，除 tableOid 外不允许任何系统列 */
	if (pstate->p_expr_kind == EXPR_KIND_CHECK_CONSTRAINT &&
		attnum < InvalidAttrNumber && attnum != TableOidAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("system column \"%s\" reference in check constraint is invalid",
						colname),
				 parser_errposition(pstate, location)));

	/*
	 * 在生成列中，除 tableOid 外不允许任何系统列。
	 * （对于存储生成列这是必需的，但为了一致性，目前对虚拟生成列也执行
	 * 此检查。）
	 */
	if (pstate->p_expr_kind == EXPR_KIND_GENERATED_COLUMN &&
		attnum < InvalidAttrNumber && attnum != TableOidAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("cannot use system column \"%s\" in column generation expression",
						colname),
				 parser_errposition(pstate, location)));

	/*
	 * 在 MERGE WHEN 条件中，除 tableOid 外不允许任何系统列
	 */
	if (pstate->p_expr_kind == EXPR_KIND_MERGE_WHEN &&
		attnum < InvalidAttrNumber && attnum != TableOidAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("cannot use system column \"%s\" in MERGE WHEN condition",
						colname),
				 parser_errposition(pstate, location)));

	/* 找到了有效匹配，因此构建一个 Var */
	if (attnum > InvalidAttrNumber)
	{
		/* 从 ParseNamespaceColumn 数组中获取属性数据 */
		ParseNamespaceColumn *nscol = &nsitem->p_nscolumns[attnum - 1];

		/* 若列已删除则报错。请参阅 scanRTEForColumn 中的说明。 */
		if (nscol->p_varno == 0)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							colname,
							nsitem->p_names->aliasname)));

		var = makeVar(nscol->p_varno,
					  nscol->p_varattno,
					  nscol->p_vartype,
					  nscol->p_vartypmod,
					  nscol->p_varcollid,
					  sublevels_up);
		/* makeVar 不为这些字段提供参数，因此手动设置： */
		var->varnosyn = nscol->p_varnosyn;
		var->varattnosyn = nscol->p_varattnosyn;
	}
	else
	{
		/* 系统列，因此使用预定义的类型数据 */
		const FormData_pg_attribute *sysatt;

		sysatt = SystemAttributeDefinition(attnum);
		var = makeVar(nsitem->p_rtindex,
					  attnum,
					  sysatt->atttypid,
					  sysatt->atttypmod,
					  sysatt->attcollation,
					  sublevels_up);
	}
	var->location = location;

	/* 根据需要为 RETURNING OLD/NEW 标记 Var */
	var->varreturningtype = nsitem->p_returning_type;

	/* 如果 Var 被任意外连接置空，则做标记 */
	markNullableIfNeeded(pstate, var);

	/* 要求对该列有读权限 */
	markVarForSelectPriv(pstate, var);

	return (Node *) var;
}

/*
 * scanRTEForColumn
 *	  在单个 RTE 的列名中搜索给定的名称。
 *	  若找到，返回 attnum（对于系统列可能为负数）；否则返回
 *	  InvalidAttrNumber。
 *	  如果该名称在此 RTE 中存在歧义，则报错。
 *
 * 实际上，我们只搜索列在 "eref" 中的名称。它可以是 rte->eref，这种情况下
 * 我们确实是在搜索所有列名；或者对于连接，它可以是 rte->join_using_alias，
 * 这种情况下我们只考虑公共列名（即连接的前 N 列，因此一切都能正常工作）。
 *
 * pstate 和 location 仅用于错误报告。
 *
 * 副作用：如果 fuzzystate 非 NULL，则对非系统列检查近似匹配，并相应地
 * 更新 fuzzystate。
 *
 * 注意：这个函数从 scanNSItemForColumn 中抽取出来，是因为创建错误消息时
 * 可能需要检查不在命名空间中的 RTE。为了支持这种用法，这里应将执行的有效性
 * 检查次数降到最少。不过，对名称歧义的情况报错是可以的，因为既然我们正在
 * 为无效名称报错，那说明我们已经排除了这种情况。
 */
static int
scanRTEForColumn(ParseState *pstate, RangeTblEntry *rte,
				 Alias *eref,
				 const char *colname, int location,
				 int fuzzy_rte_penalty,
				 FuzzyAttrMatchState *fuzzystate)
{
	int			result = InvalidAttrNumber;
	int			attnum = 0;
	ListCell   *c;

	/*
	 * 扫描用户列名（或别名）以寻找匹配。若出现多个匹配则报错。
	 *
	 * 注意：eref->colnames 可能包含已删除列的条目，但那些会是无法匹配任何
	 * 合法 SQL 标识符的空字符串，因此我们不必在这里测试这种情况。
	 *
	 * 如果不知为何出错并尝试访问已删除的列，我们仍将通过
	 * scanNSItemForColumn() 中的检查捕获它。不过，那些希望找到距离最短
	 * 的匹配的调用者需要自行防范这种情况。
	 */
	foreach(c, eref->colnames)
	{
		const char *attcolname = strVal(lfirst(c));

		attnum++;
		if (strcmp(attcolname, colname) == 0)
		{
			if (result)
				ereport(ERROR,
						(errcode(ERRCODE_AMBIGUOUS_COLUMN),
						 errmsg("column reference \"%s\" is ambiguous",
								colname),
						 parser_errposition(pstate, location)));
			result = attnum;
		}

		/* 如果提供了，则更新模糊匹配状态。 */
		if (fuzzystate != NULL)
			updateFuzzyAttrMatchState(fuzzy_rte_penalty, fuzzystate,
									  rte, attcolname, colname, attnum);
	}

	/*
	 * 如果我们有一个唯一匹配，则返回它。注意，这使得用户别名可以在不出错
	 * 的情况下覆盖系统列名（例如 OID）。
	 */
	if (result)
		return result;

	/*
	 * 如果 RTE 代表一个真实关系，则考虑系统列名。
	 * 组合类型仅用于像 ON CONFLICT 的 excluded 这样的伪关系。
	 */
	if (rte->rtekind == RTE_RELATION &&
		rte->relkind != RELKIND_COMPOSITE_TYPE)
	{
		/* 快速检查名称是否可能是一个系统列 */
		attnum = specialAttNum(colname);
		if (attnum != InvalidAttrNumber)
		{
			/* 现在检查该列是否确实已定义 */
			if (SearchSysCacheExists2(ATTNUM,
									  ObjectIdGetDatum(rte->relid),
									  Int16GetDatum(attnum)))
				result = attnum;
		}
	}

	return result;
}

/*
 * colNameToVar
 *	  搜索一个未限定的列名。
 *	  若找到，返回合适的 Var 节点（或表达式）。
 *	  若未找到，返回 NULL。如果该名称存在歧义，则报错。
 *	  如果 localonly 为 true，则只考虑最内层查询中的名称。
 */
Node *
colNameToVar(ParseState *pstate, const char *colname, bool localonly,
			 int location)
{
	Node	   *result = NULL;
	int			sublevels_up = 0;
	ParseState *orig_pstate = pstate;

	while (pstate != NULL)
	{
		ListCell   *l;

		foreach(l, pstate->p_namespace)
		{
			ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(l);
			Node	   *newresult;

			/* 忽略仅表项 */
			if (!nsitem->p_cols_visible)
				continue;
			/* 若不在 LATERAL 内部，忽略仅 LATERAL 项 */
			if (nsitem->p_lateral_only && !pstate->p_lateral_active)
				continue;

			/* 这里使用 orig_pstate，以保持与其他调用者一致 */
			newresult = scanNSItemForColumn(orig_pstate, nsitem, sublevels_up,
											colname, location);

			if (newresult)
			{
				if (result)
					ereport(ERROR,
							(errcode(ERRCODE_AMBIGUOUS_COLUMN),
							 errmsg("column reference \"%s\" is ambiguous",
									colname),
							 parser_errposition(pstate, location)));
				check_lateral_ref_ok(pstate, nsitem, location);
				result = newresult;
			}
		}

		if (result != NULL || localonly)
			break;				/* 已找到，或不希望查看父查询 */

		pstate = pstate->parentParseState;
		sublevels_up++;
	}

	return result;
}

/*
 * searchRangeTableForCol
 *	  查看是否有任意 RangeTblEntry 可能提供给定的列名（或找到可用的最佳
 *	  匹配）。返回包含相关细节的状态。
 *
 * 这与 colNameToVar 不同之处在于：它会考虑 ParseState 的 rangetable（s）
 * 中的每一个条目，而不仅仅是当前在 p_namespace 列表（s）中可见的那些。
 * 这种行为按照 SQL 规范是无效的，并且可能产生歧义的结果（因为可能存在
 * 多个同样有效的匹配）。它只能作为启发式手段用于给出合适的错误消息。
 * 请参阅 errorMissingColumn。
 *
 * 这个函数的另一个不同之处是它会考虑近似匹配——如果用户输入的
 * 别名/列对与一个有效的对仅有细微差别，我们或许能够推断出他们本想输入的
 * 内容并给出合理的提示。我们会返回一个 FuzzyAttrMatchState 结构，其中
 * 提供关于精确匹配和近似匹配两方面的信息。
 */
static FuzzyAttrMatchState *
searchRangeTableForCol(ParseState *pstate, const char *alias, const char *colname,
					   int location)
{
	ParseState *orig_pstate = pstate;
	FuzzyAttrMatchState *fuzzystate = palloc(sizeof(FuzzyAttrMatchState));

	fuzzystate->distance = MAX_FUZZY_DISTANCE + 1;
	fuzzystate->rfirst = NULL;
	fuzzystate->rsecond = NULL;
	fuzzystate->rexact1 = NULL;
	fuzzystate->rexact2 = NULL;

	while (pstate != NULL)
	{
		ListCell   *l;

		foreach(l, pstate->p_rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);
			int			fuzzy_rte_penalty = 0;
			int			attnum;

		/*
		 * 通常，在连接 RTE 中寻找匹配是没有意义的；对我们的目的而言，它们
		 * 实际上重复了其他 RTE，而且如果从连接 RTE 中选择了匹配，最终的诊断
		 * 消息中会显示一个无用的别名。
		 */
			if (rte->rtekind == RTE_JOIN)
				continue;

		/*
		 * 如果用户没有指定别名，那么对任意一个 RTE 的匹配都一样好。但如果
		 * 用户指定了别名，那么我们希望对范围表条目至少有一个模糊匹配——
		 * 最好是一个精确匹配。
		 */
			if (alias != NULL)
				fuzzy_rte_penalty =
					varstr_levenshtein_less_equal(alias, strlen(alias),
												  rte->eref->aliasname,
												  strlen(rte->eref->aliasname),
												  1, 1, 1,
												  MAX_FUZZY_DISTANCE + 1,
												  true);

		/*
		 * 扫描匹配的列，并更新 fuzzystate。非精确匹配在 scanRTEForColumn
		 * 内部处理，而精确匹配在这里处理。（同一个 RTE 中不会有一个以上的
		 * 精确匹配，否则我们早就报错了。）
		 */
			attnum = scanRTEForColumn(orig_pstate, rte, rte->eref,
									  colname, location,
									  fuzzy_rte_penalty, fuzzystate);
			if (attnum != InvalidAttrNumber && fuzzy_rte_penalty == 0)
			{
				if (fuzzystate->rexact1 == NULL)
				{
					fuzzystate->rexact1 = rte;
					fuzzystate->exact1 = attnum;
				}
				else
				{
					/* 不必担心覆盖之前的 rexact2 */
					fuzzystate->rexact2 = rte;
					fuzzystate->exact2 = attnum;
				}
			}
		}

		pstate = pstate->parentParseState;
	}

	return fuzzystate;
}

/*
 * markNullableIfNeeded
 *		如果 Var 所引用的 RTE 在查询的这一点上可被外连接（们）置空，
 *		则设置 var->varnullingrels 以体现这一点。
 */
void
markNullableIfNeeded(ParseState *pstate, Var *var)
{
	int			rtindex = var->varno;
	Bitmapset  *relids;

	/* 找到合适的 pstate */
	for (int lv = 0; lv < var->varlevelsup; lv++)
		pstate = pstate->parentParseState;

	/* 为 Var 的关系查找当前相关的连接 relid 集合 */
	if (rtindex > 0 && rtindex <= list_length(pstate->p_nullingrels))
		relids = (Bitmapset *) list_nth(pstate->p_nullingrels, rtindex - 1);
	else
		relids = NULL;

	/*
	 * 与任何已经声明的置空 rel 集合合并。（通常不会有，但如果有，我们也
	 * 应当正确处理。）
	 */
	if (relids != NULL)
		var->varnullingrels = bms_union(var->varnullingrels, relids);
}

/*
 * markRTEForSelectPriv
 *	   将索引为 rtindex 的 RTE 的指定列标记为需要 SELECT 权限
 *
 * col == InvalidAttrNumber 表示 "整行" 引用
 */
static void
markRTEForSelectPriv(ParseState *pstate, int rtindex, AttrNumber col)
{
	RangeTblEntry *rte = rt_fetch(rtindex, pstate->p_rtable);

	if (rte->rtekind == RTE_RELATION)
	{
		RTEPermissionInfo *perminfo;

		/* 确保整个关系被标记为需要 SELECT 访问权限 */
		perminfo = getRTEPermissionInfo(pstate->p_rteperminfos, rte);
		perminfo->requiredPerms |= ACL_SELECT;
		/* 必须将 attnum 偏移以适配位图集合 */
		perminfo->selectedCols =
			bms_add_member(perminfo->selectedCols,
						   col - FirstLowInvalidHeapAttributeNumber);
	}
	else if (rte->rtekind == RTE_JOIN)
	{
		if (col == InvalidAttrNumber)
		{
			/*
			 * 对连接的整行引用必须被视为对其两个输入的整行引用。
			 */
			JoinExpr   *j;

			if (rtindex > 0 && rtindex <= list_length(pstate->p_joinexprs))
				j = list_nth_node(JoinExpr, pstate->p_joinexprs, rtindex - 1);
			else
				j = NULL;
			if (j == NULL)
				elog(ERROR, "could not find JoinExpr for whole-row reference");

			/* 注意：在这里我们看不到 FromExpr */
			if (IsA(j->larg, RangeTblRef))
			{
				int			varno = ((RangeTblRef *) j->larg)->rtindex;

				markRTEForSelectPriv(pstate, varno, InvalidAttrNumber);
			}
			else if (IsA(j->larg, JoinExpr))
			{
				int			varno = ((JoinExpr *) j->larg)->rtindex;

				markRTEForSelectPriv(pstate, varno, InvalidAttrNumber);
			}
			else
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(j->larg));
			if (IsA(j->rarg, RangeTblRef))
			{
				int			varno = ((RangeTblRef *) j->rarg)->rtindex;

				markRTEForSelectPriv(pstate, varno, InvalidAttrNumber);
			}
			else if (IsA(j->rarg, JoinExpr))
			{
				int			varno = ((JoinExpr *) j->rarg)->rtindex;

				markRTEForSelectPriv(pstate, varno, InvalidAttrNumber);
			}
			else
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(j->rarg));
		}
		else
		{
		/*
		 * 普通列的 JOIN 别名 Var 必须引用合并后的 JOIN USING 列。我们在这里
		 * 不需要做任何事，因为连接的输入列也会在连接的 qual 子句中被引用，
		 * 并在那里被标记为需要 SELECT 权限。
		 */
		}
	}
	/* 其他 RTE 类型不需要标记权限 */
}

/*
 * markVarForSelectPriv
 *	   将 Var 所引用的 RTE 标记为需要其列的 SELECT 权限
 *	   （该 Var 也可能是整行 Var）
 */
void
markVarForSelectPriv(ParseState *pstate, Var *var)
{
	Index		lv;

	Assert(IsA(var, Var));
	/* 如果是上层的 Var，则找到合适的 pstate */
	for (lv = 0; lv < var->varlevelsup; lv++)
		pstate = pstate->parentParseState;
	markRTEForSelectPriv(pstate, var->varno, var->varattno);
}

/*
 * buildRelationAliases
 *		为关系 RTE 构造 eref 列名列表。
 *		这段代码也用于函数 RTE。
 *
 * tupdesc: 物理列信息
 * alias: 用户提供的别名，如果没有则为 NULL
 * eref: 用于存放列名的 eref 别名
 *
 * eref->colnames 会被填充。此外，alias->colnames 会被重建，为任何已删除的
 * 列插入空字符串，从而使它与物理列号一一对应。
 *
 * 如果提供的别名多于所需的别名，则是一个错误。
 */
static void
buildRelationAliases(TupleDesc tupdesc, Alias *alias, Alias *eref)
{
	int			maxattrs = tupdesc->natts;
	List	   *aliaslist;
	ListCell   *aliaslc;
	int			numaliases;
	int			varattno;
	int			numdropped = 0;

	Assert(eref->colnames == NIL);

	if (alias)
	{
		aliaslist = alias->colnames;
		aliaslc = list_head(aliaslist);
		numaliases = list_length(aliaslist);
		/* 我们将重建别名 colname 列表 */
		alias->colnames = NIL;
	}
	else
	{
		aliaslist = NIL;
		aliaslc = NULL;
		numaliases = 0;
	}

	for (varattno = 0; varattno < maxattrs; varattno++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, varattno);
		String	   *attrname;

		if (attr->attisdropped)
		{
			/* 对于已删除的列，始终插入一个空字符串 */
			attrname = makeString(pstrdup(""));
			if (aliaslc)
				alias->colnames = lappend(alias->colnames, attrname);
			numdropped++;
		}
		else if (aliaslc)
		{
			/* 使用下一个用户提供的别名 */
			attrname = lfirst_node(String, aliaslc);
			aliaslc = lnext(aliaslist, aliaslc);
			alias->colnames = lappend(alias->colnames, attrname);
		}
		else
		{
			attrname = makeString(pstrdup(NameStr(attr->attname)));
			/* 若存在别名，则已使用完毕 */
		}

		eref->colnames = lappend(eref->colnames, attrname);
	}

	/* 用户提供的别名太多了？ */
	if (aliaslc)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("table \"%s\" has %d columns available but %d columns specified",
						eref->aliasname, maxattrs - numdropped, numaliases)));
}

/*
 * chooseScalarFunctionAlias
 *		当函数返回标量类型（非组合类型或 RECORD）时，为函数 RTE 中的
 *		函数选择列别名。
 *
 * funcexpr: 函数调用的转换后表达式树
 * funcname: 函数名（由 FigureColname 确定）
 * alias: 用户为 RTE 提供的别名，如果没有则为 NULL
 * nfuncs: 函数 RTE 中出现的函数个数
 *
 * 注意，如果我们选择的名称可能稍后被覆盖——如果用户给出的别名包含了列
 * 别名。这在这里无需关心。
 */
static char *
chooseScalarFunctionAlias(Node *funcexpr, char *funcname,
						  Alias *alias, int nfuncs)
{
	char	   *pname;

	/*
	 * 如果表达式是一个简单的函数调用，并且该函数有一个已命名的单一 OUT
	 * 参数，则使用该参数的名称。
	 */
	if (funcexpr && IsA(funcexpr, FuncExpr))
	{
		pname = get_func_result_name(((FuncExpr *) funcexpr)->funcid);
		if (pname)
			return pname;
	}

	/*
	 * 如果 RTE 中只有一个函数，并且用户给出了 RTE 别名，则使用该名称。
	 * （这使得 FROM func() AS foo 将 "foo" 同时用作列名和表别名。）
	 */
	if (nfuncs == 1 && alias)
		return alias->aliasname;

	/*
	 * 否则使用函数名。
	 */
	return funcname;
}

/*
 * buildNSItemFromTupleDesc
 *		给定一个描述各列的 tupdesc，构造一个 ParseNamespaceItem。
 *
 * rte: 该关系的新 RangeTblEntry
 * rtindex: 它在 rangetable 列表中的索引
 * perminfo: 该关系的权限列表条目
 * tupdesc: 物理列信息
 */
static ParseNamespaceItem *
buildNSItemFromTupleDesc(RangeTblEntry *rte, Index rtindex,
						 RTEPermissionInfo *perminfo,
						 TupleDesc tupdesc)
{
	ParseNamespaceItem *nsitem;
	ParseNamespaceColumn *nscolumns;
	int			maxattrs = tupdesc->natts;
	int			varattno;

	/* colnames 必须具有与 nsitem 相同数量的条目 */
	Assert(maxattrs == list_length(rte->eref->colnames));

	/* 从 tupdesc 中提取每列的数据 */
	nscolumns = (ParseNamespaceColumn *)
		palloc0(maxattrs * sizeof(ParseNamespaceColumn));

	for (varattno = 0; varattno < maxattrs; varattno++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, varattno);

		/* 对于已删除的列，直接将该条目留作零值 */
		if (attr->attisdropped)
			continue;

		nscolumns[varattno].p_varno = rtindex;
		nscolumns[varattno].p_varattno = varattno + 1;
		nscolumns[varattno].p_vartype = attr->atttypid;
		nscolumns[varattno].p_vartypmod = attr->atttypmod;
		nscolumns[varattno].p_varcollid = attr->attcollation;
		nscolumns[varattno].p_varnosyn = rtindex;
		nscolumns[varattno].p_varattnosyn = varattno + 1;
	}

	/* ... 并构造 nsitem */
	nsitem = (ParseNamespaceItem *) palloc(sizeof(ParseNamespaceItem));
	nsitem->p_names = rte->eref;
	nsitem->p_rte = rte;
	nsitem->p_rtindex = rtindex;
	nsitem->p_perminfo = perminfo;
	nsitem->p_nscolumns = nscolumns;
	/* 设置默认的可见性标志；稍后可能会被修改 */
	nsitem->p_rel_visible = true;
	nsitem->p_cols_visible = true;
	nsitem->p_lateral_only = false;
	nsitem->p_lateral_ok = true;
	nsitem->p_returning_type = VAR_RETURNING_DEFAULT;

	return nsitem;
}

/*
 * buildNSItemFromLists
 *		给定以列表形式给出的列类型信息，构造一个 ParseNamespaceItem。
 *
 * rte: 该关系的新 RangeTblEntry
 * rtindex: 它在 rangetable 列表中的索引
 * coltypes: 每列的数据类型 OID
 * coltypmods: 每列的类型修饰符
 * colcollations: 每列的排序规则 OID
 */
static ParseNamespaceItem *
buildNSItemFromLists(RangeTblEntry *rte, Index rtindex,
					 List *coltypes, List *coltypmods, List *colcollations)
{
	ParseNamespaceItem *nsitem;
	ParseNamespaceColumn *nscolumns;
	int			maxattrs = list_length(coltypes);
	int			varattno;
	ListCell   *lct;
	ListCell   *lcm;
	ListCell   *lcc;

	/* colnames 必须具有与 nsitem 相同数量的条目 */
	Assert(maxattrs == list_length(rte->eref->colnames));

	Assert(maxattrs == list_length(coltypmods));
	Assert(maxattrs == list_length(colcollations));

	/* 从列表中提取每列的数据 */
	nscolumns = (ParseNamespaceColumn *)
		palloc0(maxattrs * sizeof(ParseNamespaceColumn));

	varattno = 0;
	forthree(lct, coltypes,
			 lcm, coltypmods,
			 lcc, colcollations)
	{
		nscolumns[varattno].p_varno = rtindex;
		nscolumns[varattno].p_varattno = varattno + 1;
		nscolumns[varattno].p_vartype = lfirst_oid(lct);
		nscolumns[varattno].p_vartypmod = lfirst_int(lcm);
		nscolumns[varattno].p_varcollid = lfirst_oid(lcc);
		nscolumns[varattno].p_varnosyn = rtindex;
		nscolumns[varattno].p_varattnosyn = varattno + 1;
		varattno++;
	}

	/* ... 并构造 nsitem */
	nsitem = (ParseNamespaceItem *) palloc(sizeof(ParseNamespaceItem));
	nsitem->p_names = rte->eref;
	nsitem->p_rte = rte;
	nsitem->p_rtindex = rtindex;
	nsitem->p_perminfo = NULL;
	nsitem->p_nscolumns = nscolumns;
	/* 设置默认的可见性标志；稍后可能会被修改 */
	nsitem->p_rel_visible = true;
	nsitem->p_cols_visible = true;
	nsitem->p_lateral_only = false;
	nsitem->p_lateral_ok = true;
	nsitem->p_returning_type = VAR_RETURNING_DEFAULT;

	return nsitem;
}

/*
 * 在解析分析期间打开一个表
 *
 * 这本质上与 table_openrv() 相同，只是它迎合了某些解析器特有的错误报告
 * 需求，特别是它安排将 RangeVar 的解析位置包含在由此产生的任何错误中。
 *
 * 注意：严格来说，lockmode 应当声明为 LOCKMODE 而非 int，但那将要求把
 * storage/lock.h 引入 parse_relation.h。既然 LOCKMODE 本身 typedef 为 int，
 * 那样做似乎就有些小题大做了。
 */
Relation
parserOpenTable(ParseState *pstate, const RangeVar *relation, int lockmode)
{
	Relation	rel;
	ParseCallbackState pcbstate;

	setup_parser_errposition_callback(&pcbstate, pstate, relation->location);
	rel = table_openrv_extended(relation, lockmode, true);
	if (rel == NULL)
	{
		if (relation->schemaname)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("relation \"%s.%s\" does not exist",
							relation->schemaname, relation->relname)));
		else
		{
			/*
			 * 一个未限定的名称可能本意是引用某个尚不在作用域内的 CTE。
			 * 单纯的 "does not exist"（不存在）消息在排查此类问题时被证明
			 * 极无帮助，因此我们费心提供一个具体的提示。
			 */
			if (isFutureCTE(pstate, relation->relname))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_TABLE),
						 errmsg("relation \"%s\" does not exist",
								relation->relname),
						 errdetail("There is a WITH item named \"%s\", but it cannot be referenced from this part of the query.",
								   relation->relname),
						 errhint("Use WITH RECURSIVE, or re-order the WITH items to remove forward references.")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_TABLE),
						 errmsg("relation \"%s\" does not exist",
								relation->relname)));
		}
	}
	cancel_parser_errposition_callback(&pcbstate);
	return rel;
}

/*
 * 向 pstate 的范围表（p_rtable）中添加一个关系条目。
 * 然后，为新的 RTE 构造并返回一个 ParseNamespaceItem。
 *
 * 我们不会在这里把 ParseNamespaceItem 链接进 pstate；由调用者以适当的方式
 * 完成这件事。
 *
 * 注意：以前这里会检查引用名冲突，但那是错误的做法。调用者负责在适当的
 * 作用域内检查冲突。
 */
ParseNamespaceItem *
addRangeTableEntry(ParseState *pstate,
				   RangeVar *relation,
				   Alias *alias,
				   bool inh,
				   bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	RTEPermissionInfo *perminfo;
	char	   *refname = alias ? alias->aliasname : relation->relname;
	LOCKMODE	lockmode;
	Relation	rel;
	ParseNamespaceItem *nsitem;

	Assert(pstate != NULL);

	rte->rtekind = RTE_RELATION;
	rte->alias = alias;

	/*
	 * 确定我们需要在此关系上持有的锁类型。它不是查询的目标表（那种情况
	 * 在别处处理），因此我们需要的是：如果被 FOR UPDATE/SHARE 锁定，则为
	 * RowShareLock，否则为普通的 AccessShareLock。
	 */
	lockmode = isLockedRefname(pstate, refname) ? RowShareLock : AccessShareLock;

	/*
	 * 获取关系的 OID。这次访问还确保我们拥有一个最新的关系缓存条目。由于
	 * 这通常是一条语句中对该关系的首次访问，我们必须以适当的锁模式打开
	 * 该关系。
	 */
	rel = parserOpenTable(pstate, relation, lockmode);
	rte->relid = RelationGetRelid(rel);
	rte->inh = inh;
	rte->relkind = rel->rd_rel->relkind;
	rte->rellockmode = lockmode;

	/*
	 * 使用用户提供的别名和/或实际列名构造有效列名列表。
	 */
	rte->eref = makeAlias(refname, NIL);
	buildRelationAliases(rel->rd_att, alias, rte->eref);

	/*
	 * 设置标志并初始化访问权限。
	 *
	 * 访问检查的初始默认值始终是 check-for-READ-access（检查读访问），
	 * 这对于除目标表之外的所有情况都是正确的。
	 */
	rte->lateral = false;
	rte->inFromCl = inFromCl;

	perminfo = addRTEPermissionInfo(&pstate->p_rteperminfos, rte);
	perminfo->requiredPerms = ACL_SELECT;

	/*
	 * 将已完成的 RTE 加入 pstate 的范围表列表，这样我们就知道它的索引。
	 * 但我们不把它加入连接列表——这由调用者在适当的时候完成（如果适用）。
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * 构造一个 ParseNamespaceItem，但不把它加入 pstate 的命名空间列表——
	 * 这由调用者在适当的时候完成（如果适用）。
	 */
	nsitem = buildNSItemFromTupleDesc(rte, list_length(pstate->p_rtable),
									  perminfo, rel->rd_att);

	/*
	 * 减少关系的引用计数，但保留访问锁直到事务结束，这样表就不会在我们
	 * 不知情的情况下被删除或修改其模式。
	 */
	table_close(rel, NoLock);

	return nsitem;
}

/*
 * 向 pstate 的范围表（p_rtable）中添加一个关系条目。
 * 然后，为新的 RTE 构造并返回一个 ParseNamespaceItem。
 *
 * 这与 addRangeTableEntry() 类似，只是它基于一个已经打开的关系来构造 RTE，
 * 而不是基于 RangeVar 引用。
 *
 * lockmode 是查询执行所需的锁类型；根据 RTE 在查询中的角色，它必须是
 * AccessShareLock、RowShareLock 或 RowExclusiveLock 之一。调用者必须持有
 * 该锁模式或更强的锁。
 *
 * 注意：严格来说，lockmode 应当声明为 LOCKMODE 而非 int，但那将要求把
 * storage/lock.h 引入 parse_relation.h。既然 LOCKMODE 本身 typedef 为 int，
 * 那样做似乎就有些小题大做了。
 */
ParseNamespaceItem *
addRangeTableEntryForRelation(ParseState *pstate,
							  Relation rel,
							  int lockmode,
							  Alias *alias,
							  bool inh,
							  bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	RTEPermissionInfo *perminfo;
	char	   *refname = alias ? alias->aliasname : RelationGetRelationName(rel);

	Assert(pstate != NULL);

	Assert(lockmode == AccessShareLock ||
		   lockmode == RowShareLock ||
		   lockmode == RowExclusiveLock);
	Assert(CheckRelationLockedByMe(rel, lockmode, true));

	rte->rtekind = RTE_RELATION;
	rte->alias = alias;
	rte->relid = RelationGetRelid(rel);
	rte->inh = inh;
	rte->relkind = rel->rd_rel->relkind;
	rte->rellockmode = lockmode;

	/*
	 * 使用用户提供的别名和/或实际列名构造有效列名列表。
	 */
	rte->eref = makeAlias(refname, NIL);
	buildRelationAliases(rel->rd_att, alias, rte->eref);

	/*
	 * 设置标志并初始化访问权限。
	 *
	 * 访问检查的初始默认值始终是 check-for-READ-access（检查读访问），
	 * 这对于除目标表之外的所有情况都是正确的。
	 */
	rte->lateral = false;
	rte->inFromCl = inFromCl;

	perminfo = addRTEPermissionInfo(&pstate->p_rteperminfos, rte);
	perminfo->requiredPerms = ACL_SELECT;

	/*
	 * 将已完成的 RTE 加入 pstate 的范围表列表，这样我们就知道它的索引。
	 * 但我们不把它加入连接列表——这由调用者在适当的时候完成（如果适用）。
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * 构造一个 ParseNamespaceItem，但不把它加入 pstate 的命名空间列表——
	 * 这由调用者在适当的时候完成（如果适用）。
	 */
	return buildNSItemFromTupleDesc(rte, list_length(pstate->p_rtable),
									perminfo, rel->rd_att);
}

/*
 * 向 pstate 的范围表（p_rtable）中添加一个子查询条目。
 * 然后，为新的 RTE 构造并返回一个 ParseNamespaceItem。
 *
 * 这与 addRangeTableEntry() 很像，只是它构造的是一个子查询 RTE。
 *
 * 如果子查询没有别名，则返回的 ParseNamespaceItem 中自动生成的关系名将被
 * 标记为不可见，因此只允许对子查询列进行未限定引用，并且该关系名不会与
 * pstate 命名空间列表中的其他名称冲突。
 */
ParseNamespaceItem *
addRangeTableEntryForSubquery(ParseState *pstate,
							  Query *subquery,
							  Alias *alias,
							  bool lateral,
							  bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	Alias	   *eref;
	int			numaliases;
	List	   *coltypes,
			   *coltypmods,
			   *colcollations;
	int			varattno;
	ListCell   *tlistitem;
	ParseNamespaceItem *nsitem;

	Assert(pstate != NULL);

	rte->rtekind = RTE_SUBQUERY;
	rte->subquery = subquery;
	rte->alias = alias;

	eref = alias ? copyObject(alias) : makeAlias("unnamed_subquery", NIL);
	numaliases = list_length(eref->colnames);

	/* 填充任何未指定的别名列，并提取列类型信息 */
	coltypes = coltypmods = colcollations = NIL;
	varattno = 0;
	foreach(tlistitem, subquery->targetList)
	{
		TargetEntry *te = (TargetEntry *) lfirst(tlistitem);

		if (te->resjunk)
			continue;
		varattno++;
		Assert(varattno == te->resno);
		if (varattno > numaliases)
		{
			char	   *attrname;

			attrname = pstrdup(te->resname);
			eref->colnames = lappend(eref->colnames, makeString(attrname));
		}
		coltypes = lappend_oid(coltypes,
							   exprType((Node *) te->expr));
		coltypmods = lappend_int(coltypmods,
								 exprTypmod((Node *) te->expr));
		colcollations = lappend_oid(colcollations,
									exprCollation((Node *) te->expr));
	}
	if (varattno < numaliases)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("table \"%s\" has %d columns available but %d columns specified",
						eref->aliasname, varattno, numaliases)));

	rte->eref = eref;

	/*
	 * 设置标志。
	 *
	 * 子查询永远不会被检查访问权限，因此不需要执行
	 * addRTEPermissionInfo()。
	 */
	rte->lateral = lateral;
	rte->inFromCl = inFromCl;

	/*
	 * 将已完成的 RTE 加入 pstate 的范围表列表，这样我们就知道它的索引。
	 * 但我们不把它加入连接列表——这由调用者在适当的时候完成（如果适用）。
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * 构造一个 ParseNamespaceItem，但不把它加入 pstate 的命名空间列表——
	 * 这由调用者在适当的时候完成（如果适用）。
	 */
	nsitem = buildNSItemFromLists(rte, list_length(pstate->p_rtable),
								  coltypes, coltypmods, colcollations);

	/*
	 * 仅当它具有用户编写的别名时，才将其标记为作为关系名可见。
	 */
	nsitem->p_rel_visible = (alias != NULL);

	return nsitem;
}

/*
 * 向 pstate 的范围表（p_rtable）中添加一个函数（或多个函数）条目。
 * 然后，为新的 RTE 构造并返回一个 ParseNamespaceItem。
 *
 * 这与 addRangeTableEntry() 很像，只是它构造的是一个函数 RTE。
 */
ParseNamespaceItem *
addRangeTableEntryForFunction(ParseState *pstate,
							  List *funcnames,
							  List *funcexprs,
							  List *coldeflists,
							  RangeFunction *rangefunc,
							  bool lateral,
							  bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	Alias	   *alias = rangefunc->alias;
	Alias	   *eref;
	char	   *aliasname;
	int			nfuncs = list_length(funcexprs);
	TupleDesc  *functupdescs;
	TupleDesc	tupdesc;
	ListCell   *lc1,
			   *lc2,
			   *lc3;
	int			i;
	int			j;
	int			funcno;
	int			natts,
				totalatts;

	Assert(pstate != NULL);

	rte->rtekind = RTE_FUNCTION;
	rte->relid = InvalidOid;
	rte->subquery = NULL;
	rte->functions = NIL;		/* 我们将在下方填充这个列表 */
	rte->funcordinality = rangefunc->ordinality;
	rte->alias = alias;

	/*
	 * 选择 RTE 别名。我们默认使用第一个函数的名称，即使存在多个函数；这
	 * 或许有争议，但总好过使用像 "table" 这样的常量。
	 */
	if (alias)
		aliasname = alias->aliasname;
	else
		aliasname = linitial(funcnames);

	eref = makeAlias(aliasname, NIL);
	rte->eref = eref;

	/* 逐个处理函数 ... */
	functupdescs = (TupleDesc *) palloc(nfuncs * sizeof(TupleDesc));

	totalatts = 0;
	funcno = 0;
	forthree(lc1, funcexprs, lc2, funcnames, lc3, coldeflists)
	{
		Node	   *funcexpr = (Node *) lfirst(lc1);
		char	   *funcname = (char *) lfirst(lc2);
		List	   *coldeflist = (List *) lfirst(lc3);
		RangeTblFunction *rtfunc = makeNode(RangeTblFunction);
		TypeFuncClass functypclass;
		Oid			funcrettype;

		/* 初始化 RangeTblFunction 节点 */
		rtfunc->funcexpr = funcexpr;
		rtfunc->funccolnames = NIL;
		rtfunc->funccoltypes = NIL;
		rtfunc->funccoltypmods = NIL;
		rtfunc->funccolcollations = NIL;
		rtfunc->funcparams = NULL;	/* 直到规划阶段才设置 */

		/*
		 * 现在确定函数返回的是简单类型还是组合类型。
		 */
		functypclass = get_expr_result_type(funcexpr,
											&funcrettype,
											&tupdesc);

		/*
		 * 如果函数返回 RECORD 且尚未确定记录类型，则需要列定义列表，否则
		 * 禁止。这可能有些令人困惑，因此我们花些力气给出相关的错误消息。
		 */
		if (coldeflist != NIL)
		{
			switch (functypclass)
			{
				case TYPEFUNC_RECORD:
					/* 没问题 */
					break;
				case TYPEFUNC_COMPOSITE:
				case TYPEFUNC_COMPOSITE_DOMAIN:

					/*
					 * 如果函数原始结果类型是 RECORD，则我们必定已经通过其
					 * OUT 参数解析了它。否则，它必须有一个已命名的组合类型。
					 */
					if (exprType(funcexpr) == RECORDOID)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("a column definition list is redundant for a function with OUT parameters"),
								 parser_errposition(pstate,
													exprLocation((Node *) coldeflist))));
					else
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("a column definition list is redundant for a function returning a named composite type"),
								 parser_errposition(pstate,
													exprLocation((Node *) coldeflist))));
					break;
				default:
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("a column definition list is only allowed for functions returning \"record\""),
							 parser_errposition(pstate,
												exprLocation((Node *) coldeflist))));
					break;
			}
		}
		else
		{
			if (functypclass == TYPEFUNC_RECORD)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("a column definition list is required for functions returning \"record\""),
						 parser_errposition(pstate, exprLocation(funcexpr))));
		}

		if (functypclass == TYPEFUNC_COMPOSITE ||
			functypclass == TYPEFUNC_COMPOSITE_DOMAIN)
		{
			/* 组合数据类型，例如表的行类型 */
			Assert(tupdesc);
		}
		else if (functypclass == TYPEFUNC_SCALAR)
		{
			/* 基础数据类型，即标量 */
			tupdesc = CreateTemplateTupleDesc(1);
			TupleDescInitEntry(tupdesc,
							   (AttrNumber) 1,
							   chooseScalarFunctionAlias(funcexpr, funcname,
														 alias, nfuncs),
							   funcrettype,
							   exprTypmod(funcexpr),
							   0);
			TupleDescInitEntryCollation(tupdesc,
										(AttrNumber) 1,
										exprCollation(funcexpr));
		}
		else if (functypclass == TYPEFUNC_RECORD)
		{
			ListCell   *col;

			/*
			 * 使用列定义列表构造一个 tupdesc 并填充 RangeTblFunction 的列表。
			 * 将列数限制为 MaxHeapAttributeNumber，因为 CheckAttributeNamesTypes
			 * 会这样做。
			 */
			if (list_length(coldeflist) > MaxHeapAttributeNumber)
				ereport(ERROR,
						(errcode(ERRCODE_TOO_MANY_COLUMNS),
						 errmsg("column definition lists can have at most %d entries",
								MaxHeapAttributeNumber),
						 parser_errposition(pstate,
											exprLocation((Node *) coldeflist))));
			tupdesc = CreateTemplateTupleDesc(list_length(coldeflist));
			i = 1;
			foreach(col, coldeflist)
			{
				ColumnDef  *n = (ColumnDef *) lfirst(col);
				char	   *attrname;
				Oid			attrtype;
				int32		attrtypmod;
				Oid			attrcollation;

				attrname = n->colname;
				if (n->typeName->setof)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("column \"%s\" cannot be declared SETOF",
									attrname),
							 parser_errposition(pstate, n->location)));
				typenameTypeIdAndMod(pstate, n->typeName,
									 &attrtype, &attrtypmod);
				attrcollation = GetColumnDefCollation(pstate, n, attrtype);
				TupleDescInitEntry(tupdesc,
								   (AttrNumber) i,
								   attrname,
								   attrtype,
								   attrtypmod,
								   0);
				TupleDescInitEntryCollation(tupdesc,
											(AttrNumber) i,
											attrcollation);
				rtfunc->funccolnames = lappend(rtfunc->funccolnames,
											   makeString(pstrdup(attrname)));
				rtfunc->funccoltypes = lappend_oid(rtfunc->funccoltypes,
												   attrtype);
				rtfunc->funccoltypmods = lappend_int(rtfunc->funccoltypmods,
													 attrtypmod);
				rtfunc->funccolcollations = lappend_oid(rtfunc->funccolcollations,
														attrcollation);

				i++;
			}

			/*
			 * 确保 coldeflist 定义了一组合法的名称（无重复，但我们无需担心
			 * 系统列名）和类型。虽然我们通常不允许伪类型，但允许 RECORD 和
			 * RECORD[] 似乎是安全的，因为这些类型类中的值在运行时是可自识别的，
			 * 而且 coldeflist 并不代表任何对其他会话可见的东西。
			 */
			CheckAttributeNamesTypes(tupdesc, RELKIND_COMPOSITE_TYPE,
									 CHKATYPE_ANYRECORD);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("function \"%s\" in FROM has unsupported return type %s",
							funcname, format_type_be(funcrettype)),
					 parser_errposition(pstate, exprLocation(funcexpr))));

		/* 完成 RangeTblFunction 并将其加入 RTE 的列表 */
		rtfunc->funccolcount = tupdesc->natts;
		rte->functions = lappend(rte->functions, rtfunc);

		/* 保存 tupdesc 供下文使用 */
		functupdescs[funcno] = tupdesc;
		totalatts += tupdesc->natts;
		funcno++;
	}

	/*
	 * 如果有多个函数，或者我们需要一个序数（ordinality）列，则必须生成一个
	 * 合并后的 tupdesc。
	 */
	if (nfuncs > 1 || rangefunc->ordinality)
	{
		if (rangefunc->ordinality)
			totalatts++;

		/* 禁止列数超过一个元组所能容纳的数量 */
		if (totalatts > MaxTupleAttributeNumber)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_COLUMNS),
					 errmsg("functions in FROM can return at most %d columns",
							MaxTupleAttributeNumber),
					 parser_errposition(pstate,
										exprLocation((Node *) funcexprs))));

		/* 将每个函数的元组描述符合并为一个组合类型 */
		tupdesc = CreateTemplateTupleDesc(totalatts);
		natts = 0;
		for (i = 0; i < nfuncs; i++)
		{
			for (j = 1; j <= functupdescs[i]->natts; j++)
				TupleDescCopyEntry(tupdesc, ++natts, functupdescs[i], j);
		}

		/* 如果需要，添加序数（ordinality）列 */
		if (rangefunc->ordinality)
		{
			TupleDescInitEntry(tupdesc,
							   (AttrNumber) ++natts,
							   "ordinality",
							   INT8OID,
							   -1,
							   0);
			/* 无需设置排序规则 */
		}

		Assert(natts == totalatts);
	}
	else
	{
		/* 我们可以直接使用该单个函数的 tupdesc */
		tupdesc = functupdescs[0];
	}

	/* 在为 RTE 分配列别名时使用 tupdesc */
	buildRelationAliases(tupdesc, alias, eref);

	/*
	 * 设置标志和访问权限。
	 *
	 * 函数永远不会被检查访问权限（至少不会被 ExecCheckPermissions() 检查），
	 * 因此不需要执行 addRTEPermissionInfo()。
	 */
	rte->lateral = lateral;
	rte->inFromCl = inFromCl;

	/*
	 * 将已完成的 RTE 加入 pstate 的范围表列表，这样我们就知道它的索引。
	 * 但我们不把它加入连接列表——这由调用者在适当的时候完成（如果适用）。
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * 构造一个 ParseNamespaceItem，但不把它加入 pstate 的命名空间列表——
	 * 这由调用者在适当的时候完成（如果适用）。
	 */
	return buildNSItemFromTupleDesc(rte, list_length(pstate->p_rtable), NULL,
									tupdesc);
}

/*
 * 向 pstate 的范围表（p_rtable）中添加一个表函数条目。
 * 然后，为新的 RTE 构造并返回一个 ParseNamespaceItem。
 *
 * 这与 addRangeTableEntry() 很像，只是它构造的是一个 tablefunc RTE。
 */
ParseNamespaceItem *
addRangeTableEntryForTableFunc(ParseState *pstate,
							   TableFunc *tf,
							   Alias *alias,
							   bool lateral,
							   bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	char	   *refname;
	Alias	   *eref;
	int			numaliases;

	Assert(pstate != NULL);

	/* 禁止列数超过一个元组所能容纳的数量 */
	if (list_length(tf->colnames) > MaxTupleAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("functions in FROM can return at most %d columns",
						MaxTupleAttributeNumber),
				 parser_errposition(pstate,
									exprLocation((Node *) tf))));
	Assert(list_length(tf->coltypes) == list_length(tf->colnames));
	Assert(list_length(tf->coltypmods) == list_length(tf->colnames));
	Assert(list_length(tf->colcollations) == list_length(tf->colnames));

	rte->rtekind = RTE_TABLEFUNC;
	rte->relid = InvalidOid;
	rte->subquery = NULL;
	rte->tablefunc = tf;
	rte->coltypes = tf->coltypes;
	rte->coltypmods = tf->coltypmods;
	rte->colcollations = tf->colcollations;
	rte->alias = alias;

	refname = alias ? alias->aliasname :
		pstrdup(tf->functype == TFT_XMLTABLE ? "xmltable" : "json_table");
	eref = alias ? copyObject(alias) : makeAlias(refname, NIL);
	numaliases = list_length(eref->colnames);

	/* 填充任何未指定的别名列 */
	if (numaliases < list_length(tf->colnames))
		eref->colnames = list_concat(eref->colnames,
									 list_copy_tail(tf->colnames, numaliases));

	if (numaliases > list_length(tf->colnames))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("%s function has %d columns available but %d columns specified",
						tf->functype == TFT_XMLTABLE ? "XMLTABLE" : "JSON_TABLE",
						list_length(tf->colnames), numaliases)));

	rte->eref = eref;

	/*
	 * 设置标志和访问权限。
	 *
	 * 表函数永远不会被检查访问权限（至少不会被 ExecCheckPermissions() 检查），
	 * 因此不需要执行 addRTEPermissionInfo()。
	 */
	rte->lateral = lateral;
	rte->inFromCl = inFromCl;

	/*
	 * 将已完成的 RTE 加入 pstate 的范围表列表，这样我们就知道它的索引。
	 * 但我们不把它加入连接列表——这由调用者在适当的时候完成（如果适用）。
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * 构造一个 ParseNamespaceItem，但不把它加入 pstate 的命名空间列表——
	 * 这由调用者在适当的时候完成（如果适用）。
	 */
	return buildNSItemFromLists(rte, list_length(pstate->p_rtable),
								rte->coltypes, rte->coltypmods,
								rte->colcollations);
}

/*
 * 向 pstate 的范围表（p_rtable）中添加一个 VALUES 列表条目。
 * 然后，为新的 RTE 构造并返回一个 ParseNamespaceItem。
 *
 * 这与 addRangeTableEntry() 很像，只是它构造的是一个 values RTE。
 */
ParseNamespaceItem *
addRangeTableEntryForValues(ParseState *pstate,
							List *exprs,
							List *coltypes,
							List *coltypmods,
							List *colcollations,
							Alias *alias,
							bool lateral,
							bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	char	   *refname = alias ? alias->aliasname : pstrdup("*VALUES*");
	Alias	   *eref;
	int			numaliases;
	int			numcolumns;

	Assert(pstate != NULL);

	rte->rtekind = RTE_VALUES;
	rte->relid = InvalidOid;
	rte->subquery = NULL;
	rte->values_lists = exprs;
	rte->coltypes = coltypes;
	rte->coltypmods = coltypmods;
	rte->colcollations = colcollations;
	rte->alias = alias;

	eref = alias ? copyObject(alias) : makeAlias(refname, NIL);

	/* 填充任何未指定的别名列 */
	numcolumns = list_length((List *) linitial(exprs));
	numaliases = list_length(eref->colnames);
	while (numaliases < numcolumns)
	{
		char		attrname[64];

		numaliases++;
		snprintf(attrname, sizeof(attrname), "column%d", numaliases);
		eref->colnames = lappend(eref->colnames,
								 makeString(pstrdup(attrname)));
	}
	if (numcolumns < numaliases)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("VALUES lists \"%s\" have %d columns available but %d columns specified",
						refname, numcolumns, numaliases)));

	rte->eref = eref;

	/*
	 * 设置标志和访问权限。
	 *
	 * 子查询永远不会被检查访问权限，因此不需要执行
	 * addRTEPermissionInfo()。
	 */
	rte->lateral = lateral;
	rte->inFromCl = inFromCl;

	/*
	 * 将已完成的 RTE 加入 pstate 的范围表列表，这样我们就知道它的索引。
	 * 但我们不把它加入连接列表——这由调用者在适当的时候完成（如果适用）。
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * 构造一个 ParseNamespaceItem，但不把它加入 pstate 的命名空间列表——
	 * 这由调用者在适当的时候完成（如果适用）。
	 */
	return buildNSItemFromLists(rte, list_length(pstate->p_rtable),
								rte->coltypes, rte->coltypmods,
								rte->colcollations);
}

/*
 * 向 pstate 的范围表（p_rtable）中添加一个连接条目。
 * 然后，为新的 RTE 构造并返回一个 ParseNamespaceItem。
 *
 * 这与 addRangeTableEntry() 很像，只是它构造的是一个连接 RTE。
 * 此外，由调用者构造 ParseNamespaceColumn 数组更为方便，因此我们将其传入。
 */
ParseNamespaceItem *
addRangeTableEntryForJoin(ParseState *pstate,
						  List *colnames,
						  ParseNamespaceColumn *nscolumns,
						  JoinType jointype,
						  int nummergedcols,
						  List *aliasvars,
						  List *leftcols,
						  List *rightcols,
						  Alias *join_using_alias,
						  Alias *alias,
						  bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	Alias	   *eref;
	int			numaliases;
	ParseNamespaceItem *nsitem;

	Assert(pstate != NULL);

	/*
	 * 如果连接拥有过多列则报错——我们必须能够用 AttrNumber 引用其中任意
	 * 一列。
	 */
	if (list_length(aliasvars) > MaxAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("joins can have at most %d columns",
						MaxAttrNumber)));

	rte->rtekind = RTE_JOIN;
	rte->relid = InvalidOid;
	rte->subquery = NULL;
	rte->jointype = jointype;
	rte->joinmergedcols = nummergedcols;
	rte->joinaliasvars = aliasvars;
	rte->joinleftcols = leftcols;
	rte->joinrightcols = rightcols;
	rte->join_using_alias = join_using_alias;
	rte->alias = alias;

	eref = alias ? copyObject(alias) : makeAlias("unnamed_join", NIL);
	numaliases = list_length(eref->colnames);

	/* 填充任何未指定的别名列 */
	if (numaliases < list_length(colnames))
		eref->colnames = list_concat(eref->colnames,
									 list_copy_tail(colnames, numaliases));

	if (numaliases > list_length(colnames))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("join expression \"%s\" has %d columns available but %d columns specified",
						eref->aliasname, list_length(colnames), numaliases)));

	rte->eref = eref;

	/*
	 * 设置标志和访问权限。
	 *
	 * 连接永远不会被检查访问权限，因此不需要执行
	 * addRTEPermissionInfo()。
	 */
	rte->lateral = false;
	rte->inFromCl = inFromCl;

	/*
	 * 将已完成的 RTE 加入 pstate 的范围表列表，这样我们就知道它的索引。
	 * 但我们不把它加入连接列表——这由调用者在适当的时候完成（如果适用）。
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * 构造一个 ParseNamespaceItem，但不把它加入 pstate 的命名空间列表——
	 * 这由调用者在适当的时候完成（如果适用）。
	 */
	nsitem = (ParseNamespaceItem *) palloc(sizeof(ParseNamespaceItem));
	nsitem->p_names = rte->eref;
	nsitem->p_rte = rte;
	nsitem->p_perminfo = NULL;
	nsitem->p_rtindex = list_length(pstate->p_rtable);
	nsitem->p_nscolumns = nscolumns;
	/* 设置默认的可见性标志；稍后可能会被修改 */
	nsitem->p_rel_visible = true;
	nsitem->p_cols_visible = true;
	nsitem->p_lateral_only = false;
	nsitem->p_lateral_ok = true;
	nsitem->p_returning_type = VAR_RETURNING_DEFAULT;

	return nsitem;
}

/*
 * 向 pstate 的范围表（p_rtable）中添加一个 CTE 引用条目。
 * 然后，为新的 RTE 构造并返回一个 ParseNamespaceItem。
 *
 * 这与 addRangeTableEntry() 很像，只是它构造的是一个 CTE RTE。
 */
ParseNamespaceItem *
addRangeTableEntryForCTE(ParseState *pstate,
						 CommonTableExpr *cte,
						 Index levelsup,
						 RangeVar *rv,
						 bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	Alias	   *alias = rv->alias;
	char	   *refname = alias ? alias->aliasname : cte->ctename;
	Alias	   *eref;
	int			numaliases;
	int			varattno;
	ListCell   *lc;
	int			n_dontexpand_columns = 0;
	ParseNamespaceItem *psi;

	Assert(pstate != NULL);

	rte->rtekind = RTE_CTE;
	rte->ctename = cte->ctename;
	rte->ctelevelsup = levelsup;

	/* 当且仅当 CTE 的解析分析尚未完成时，才是自引用 */
	rte->self_reference = !IsA(cte->ctequery, Query);
	Assert(cte->cterecursive || !rte->self_reference);
	/* 如果这不是自引用，则增加 CTE 的引用计数 */
	if (!rte->self_reference)
		cte->cterefcount++;

	/*
	 * 如果 CTE 是 INSERT/UPDATE/DELETE/MERGE 却没有 RETURNING，则报错。
	 * 在自引用的情况下这不会被检查，但那没关系，因为数据修改型 CTE 本来就
	 * 不允许是递归的。
	 */
	if (IsA(cte->ctequery, Query))
	{
		Query	   *ctequery = (Query *) cte->ctequery;

		if (ctequery->commandType != CMD_SELECT &&
			ctequery->returningList == NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("WITH query \"%s\" does not have a RETURNING clause",
							cte->ctename),
					 parser_errposition(pstate, rv->location)));
	}

	rte->coltypes = list_copy(cte->ctecoltypes);
	rte->coltypmods = list_copy(cte->ctecoltypmods);
	rte->colcollations = list_copy(cte->ctecolcollations);

	rte->alias = alias;
	if (alias)
		eref = copyObject(alias);
	else
		eref = makeAlias(refname, NIL);
	numaliases = list_length(eref->colnames);

	/* 填充任何未指定的别名列 */
	varattno = 0;
	foreach(lc, cte->ctecolnames)
	{
		varattno++;
		if (varattno > numaliases)
			eref->colnames = lappend(eref->colnames, lfirst(lc));
	}
	if (varattno < numaliases)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("table \"%s\" has %d columns available but %d columns specified",
						refname, varattno, numaliases)));

	rte->eref = eref;

	if (cte->search_clause)
	{
		rte->eref->colnames = lappend(rte->eref->colnames, makeString(cte->search_clause->search_seq_column));
		if (cte->search_clause->search_breadth_first)
			rte->coltypes = lappend_oid(rte->coltypes, RECORDOID);
		else
			rte->coltypes = lappend_oid(rte->coltypes, RECORDARRAYOID);
		rte->coltypmods = lappend_int(rte->coltypmods, -1);
		rte->colcollations = lappend_oid(rte->colcollations, InvalidOid);

		n_dontexpand_columns += 1;
	}

	if (cte->cycle_clause)
	{
		rte->eref->colnames = lappend(rte->eref->colnames, makeString(cte->cycle_clause->cycle_mark_column));
		rte->coltypes = lappend_oid(rte->coltypes, cte->cycle_clause->cycle_mark_type);
		rte->coltypmods = lappend_int(rte->coltypmods, cte->cycle_clause->cycle_mark_typmod);
		rte->colcollations = lappend_oid(rte->colcollations, cte->cycle_clause->cycle_mark_collation);

		rte->eref->colnames = lappend(rte->eref->colnames, makeString(cte->cycle_clause->cycle_path_column));
		rte->coltypes = lappend_oid(rte->coltypes, RECORDARRAYOID);
		rte->coltypmods = lappend_int(rte->coltypmods, -1);
		rte->colcollations = lappend_oid(rte->colcollations, InvalidOid);

		n_dontexpand_columns += 2;
	}

	/*
	 * 设置标志和访问权限。
	 *
	 * 子查询永远不会被检查访问权限，因此不需要执行
	 * addRTEPermissionInfo()。
	 */
	rte->lateral = false;
	rte->inFromCl = inFromCl;

	/*
	 * 将已完成的 RTE 加入 pstate 的范围表列表，这样我们就知道它的索引。
	 * 但我们不把它加入连接列表——这由调用者在适当的时候完成（如果适用）。
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * 构造一个 ParseNamespaceItem，但不把它加入 pstate 的命名空间列表——
	 * 这由调用者在适当的时候完成（如果适用）。
	 */
	psi = buildNSItemFromLists(rte, list_length(pstate->p_rtable),
							   rte->coltypes, rte->coltypmods,
							   rte->colcollations);

	/*
	 * search 和 cycle 子句添加的列不包括在 CTE 所包含查询中的星号
	 * 展开里。
	 */
	if (rte->ctelevelsup > 0)
		for (int i = 0; i < n_dontexpand_columns; i++)
			psi->p_nscolumns[list_length(psi->p_names->colnames) - 1 - i].p_dontexpand = true;

	return psi;
}

/*
 * 向 pstate 的范围表（p_rtable）中添加一个临时命名关系（ephemeral named
 * relation）引用条目。
 * 然后，为新的 RTE 构造并返回一个 ParseNamespaceItem。
 *
 * 可以预期的是：这个到目前为止只知道是一个临时命名关系的 RangeVar，将
 * （结合 ParseState 中的 QueryEnvironment）根据 enrtype 为某一特定*种类*
 * 的临时命名关系创建一个 RangeTblEntry。
 *
 * 这与 addRangeTableEntry() 很像，只是它为一个临时命名关系构造 RTE。
 */
ParseNamespaceItem *
addRangeTableEntryForENR(ParseState *pstate,
						 RangeVar *rv,
						 bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	Alias	   *alias = rv->alias;
	char	   *refname = alias ? alias->aliasname : rv->relname;
	EphemeralNamedRelationMetadata enrmd;
	TupleDesc	tupdesc;
	int			attno;

	Assert(pstate != NULL);
	enrmd = get_visible_ENR(pstate, rv->relname);
	Assert(enrmd != NULL);

	switch (enrmd->enrtype)
	{
		case ENR_NAMED_TUPLESTORE:
			rte->rtekind = RTE_NAMEDTUPLESTORE;
			break;

		default:
			elog(ERROR, "unexpected enrtype: %d", enrmd->enrtype);
			return NULL;		/* 为了严谨的编译器 */
	}

	/*
	 * 记录对关系的依赖。这样，如果计划访问的过渡表所链接的表被修改，
	 * 计划就可以被置为无效。
	 */
	rte->relid = enrmd->reliddesc;

	/*
	 * 使用用户提供的别名和/或实际列名构造有效列名列表。
	 */
	tupdesc = ENRMetadataGetTupDesc(enrmd);
	rte->eref = makeAlias(refname, NIL);
	buildRelationAliases(tupdesc, alias, rte->eref);

	/* 记录 ENR 的附加数据，包括列类型信息 */
	rte->enrname = enrmd->name;
	rte->enrtuples = enrmd->enrtuples;
	rte->coltypes = NIL;
	rte->coltypmods = NIL;
	rte->colcollations = NIL;
	for (attno = 1; attno <= tupdesc->natts; ++attno)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, attno - 1);

		if (att->attisdropped)
		{
			/* 为已删除的列记录零值 */
			rte->coltypes = lappend_oid(rte->coltypes, InvalidOid);
			rte->coltypmods = lappend_int(rte->coltypmods, 0);
			rte->colcollations = lappend_oid(rte->colcollations, InvalidOid);
		}
		else
		{
			/* 我们只想确保能够分辨出这并非已删除列 */
			if (att->atttypid == InvalidOid)
				elog(ERROR, "atttypid is invalid for non-dropped column in \"%s\"",
					 rv->relname);
			rte->coltypes = lappend_oid(rte->coltypes, att->atttypid);
			rte->coltypmods = lappend_int(rte->coltypmods, att->atttypmod);
			rte->colcollations = lappend_oid(rte->colcollations,
											 att->attcollation);
		}
	}

	/*
	 * 设置标志和访问权限。
	 *
	 * 临时命名关系永远不会被检查访问权限，因此不需要执行
	 * addRTEPermissionInfo()。
	 */
	rte->lateral = false;
	rte->inFromCl = inFromCl;

	/*
	 * 将已完成的 RTE 加入 pstate 的范围表列表，这样我们就知道它的索引。
	 * 但我们不把它加入连接列表——这由调用者在适当的时候完成（如果适用）。
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * 构造一个 ParseNamespaceItem，但不把它加入 pstate 的命名空间列表——
	 * 这由调用者在适当的时候完成（如果适用）。
	 */
	return buildNSItemFromTupleDesc(rte, list_length(pstate->p_rtable), NULL,
									tupdesc);
}

/*
 * 向 pstate 的范围表（p_rtable）中添加一个分组步骤条目。
 * 然后，为新的 RTE 构造并返回一个 ParseNamespaceItem。
 */
ParseNamespaceItem *
addRangeTableEntryForGroup(ParseState *pstate,
						   List *groupClauses)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	Alias	   *eref;
	List	   *groupexprs;
	List	   *coltypes,
			   *coltypmods,
			   *colcollations;
	ListCell   *lc;
	ParseNamespaceItem *nsitem;

	Assert(pstate != NULL);

	rte->rtekind = RTE_GROUP;
	rte->alias = NULL;

	eref = makeAlias("*GROUP*", NIL);

	/* 填充任何未指定的别名列，并提取列类型信息 */
	groupexprs = NIL;
	coltypes = coltypmods = colcollations = NIL;
	foreach(lc, groupClauses)
	{
		TargetEntry *te = (TargetEntry *) lfirst(lc);
		char	   *colname = te->resname ? pstrdup(te->resname) : "?column?";

		eref->colnames = lappend(eref->colnames, makeString(colname));

		groupexprs = lappend(groupexprs, copyObject(te->expr));

		coltypes = lappend_oid(coltypes,
							   exprType((Node *) te->expr));
		coltypmods = lappend_int(coltypmods,
								 exprTypmod((Node *) te->expr));
		colcollations = lappend_oid(colcollations,
									exprCollation((Node *) te->expr));
	}

	rte->eref = eref;
	rte->groupexprs = groupexprs;

	/*
	 * 设置标志。
	 *
	 * 分组步骤永远不会被检查访问权限，因此不需要执行
	 * addRTEPermissionInfo()。
	 */
	rte->lateral = false;
	rte->inFromCl = false;

	/*
	 * 将已完成的 RTE 加入 pstate 的范围表列表，这样我们就知道它的索引。
	 * 但我们不把它加入连接列表——这由调用者在适当的时候完成（如果适用）。
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * 构造一个 ParseNamespaceItem，但不把它加入 pstate 的命名空间列表——
	 * 这由调用者在适当的时候完成（如果适用）。
	 */
	nsitem = buildNSItemFromLists(rte, list_length(pstate->p_rtable),
								  coltypes, coltypmods, colcollations);

	return nsitem;
}


/*
 * 指定的引用名是否被选中用于 FOR UPDATE/FOR SHARE？
 *
 * 这用于我们尚未完成 transformLockingClause，但需要知道在初始打开关系时
 * 应当取哪种锁的情况。
 *
 * 注意，refname 可能为 NULL（对于没有别名的子查询），在这种情况下关系无法
 * 按名称锁定，但如果锁定子句要求锁定所有表，它仍可能被锁定。
 *
 * 注意：我们不关心它是 FOR UPDATE 还是 FOR SHARE，因为表级锁在两种情况下
 * 是一样的。
 */
bool
isLockedRefname(ParseState *pstate, const char *refname)
{
	ListCell   *l;

		/*
		 * 如果我们处于一个从父层级起就被指定为以 FOR UPDATE/SHARE 锁定的
		 * 子查询中，则表现得好像这里有一个通用的 FOR UPDATE。
		 */
	if (pstate->p_locked_from_parent)
		return true;

	foreach(l, pstate->p_locking_clause)
	{
		LockingClause *lc = (LockingClause *) lfirst(l);

		if (lc->lockedRels == NIL)
		{
			/* 查询中使用的所有表 */
			return true;
		}
		else if (refname != NULL)
		{
			/* 仅指定的那些表 */
			ListCell   *l2;

			foreach(l2, lc->lockedRels)
			{
				RangeVar   *thisrel = (RangeVar *) lfirst(l2);

				if (strcmp(refname, thisrel->relname) == 0)
					return true;
			}
		}
	}
	return false;
}

/*
 * 将给定的 nsitem/RTE 作为顶层条目加入 pstate 的连接列表和/或命名空间
 * 列表。（我们假设调用者已经检查过任何命名空间冲突。）该 nsitem 总是被
 * 标记为无条件可见，也就是说，不是仅 LATERAL 的。
 */
void
addNSItemToQuery(ParseState *pstate, ParseNamespaceItem *nsitem,
				 bool addToJoinList,
				 bool addToRelNameSpace, bool addToVarNameSpace)
{
	if (addToJoinList)
	{
		RangeTblRef *rtr = makeNode(RangeTblRef);

		rtr->rtindex = nsitem->p_rtindex;
		pstate->p_joinlist = lappend(pstate->p_joinlist, rtr);
	}
	if (addToRelNameSpace || addToVarNameSpace)
	{
		/* 正确设置新 nsitem 的可见性标志 */
		nsitem->p_rel_visible = addToRelNameSpace;
		nsitem->p_cols_visible = addToVarNameSpace;
		nsitem->p_lateral_only = false;
		nsitem->p_lateral_ok = true;
		pstate->p_namespace = lappend(pstate->p_namespace, nsitem);
	}
}

/*
 * expandRTE —— 展开一个范围表条目的各列
 *
 * 这会创建 RTE 各列的名称列表（如果提供了别名则为别名，否则为真实名称）
 * 以及每列的 Var。只考虑用户列。如果 include_dropped 为 false，则已删除
 * 的列不会出现在结果中。如果 include_dropped 为 true，则为已删除的列返回
 * 空字符串和 NULL 常量（不是 Var！）。
 *
 * rtindex、sublevels_up、returning_type 和 location 是要在所创建的 Var 中
 * 使用的 varno、varlevelsup、varreturningtype 和 location 值。通常 rtindex
 * 应当与 RTE 在其范围表中的实际位置相匹配。
 *
 * 输出列表放入 *colnames 和 *colvars。如果只需要两种输出列表之一，则为
 * 不需要的那个的输出指针传入 NULL。
 */
void
expandRTE(RangeTblEntry *rte, int rtindex, int sublevels_up,
		  VarReturningType returning_type,
		  int location, bool include_dropped,
		  List **colnames, List **colvars)
{
	int			varattno;

	if (colnames)
		*colnames = NIL;
	if (colvars)
		*colvars = NIL;

	switch (rte->rtekind)
	{
		case RTE_RELATION:
			/* 普通关系 RTE */
			expandRelation(rte->relid, rte->eref,
						   rtindex, sublevels_up, returning_type, location,
						   include_dropped, colnames, colvars);
			break;
		case RTE_SUBQUERY:
			{
				/* 子查询 RTE */
				ListCell   *aliasp_item = list_head(rte->eref->colnames);
				ListCell   *tlistitem;

				varattno = 0;
				foreach(tlistitem, rte->subquery->targetList)
				{
					TargetEntry *te = (TargetEntry *) lfirst(tlistitem);

					if (te->resjunk)
						continue;
					varattno++;
					Assert(varattno == te->resno);

					/*
					 * 以前，子查询的 tlist 可能拥有比 colnames 列表更多的非
					 * junk 条目（如果这个 RTE 是从一个比当前查询被解析时拥有
					 * 更多列的视图展开而来的）。既然 ApplyRetrieveRule 会清理
					 * 此类情况，我们不应该再看到这种情况了，但还是检查一下。
					 */
					if (!aliasp_item)
						elog(ERROR, "too few column names for subquery %s",
							 rte->eref->aliasname);

					if (colnames)
					{
						char	   *label = strVal(lfirst(aliasp_item));

						*colnames = lappend(*colnames, makeString(pstrdup(label)));
					}

					if (colvars)
					{
						Var		   *varnode;

						varnode = makeVar(rtindex, varattno,
										  exprType((Node *) te->expr),
										  exprTypmod((Node *) te->expr),
										  exprCollation((Node *) te->expr),
										  sublevels_up);
						varnode->varreturningtype = returning_type;
						varnode->location = location;

						*colvars = lappend(*colvars, varnode);
					}

					aliasp_item = lnext(rte->eref->colnames, aliasp_item);
				}
			}
			break;
		case RTE_FUNCTION:
			{
				/* 函数 RTE */
				int			atts_done = 0;
				ListCell   *lc;

				foreach(lc, rte->functions)
				{
					RangeTblFunction *rtfunc = (RangeTblFunction *) lfirst(lc);
					TypeFuncClass functypclass;
					Oid			funcrettype = InvalidOid;
					TupleDesc	tupdesc = NULL;

					/* 如果它有 coldeflist，则返回 RECORD */
					if (rtfunc->funccolnames != NIL)
						functypclass = TYPEFUNC_RECORD;
					else
						functypclass = get_expr_result_type(rtfunc->funcexpr,
															&funcrettype,
															&tupdesc);

					if (functypclass == TYPEFUNC_COMPOSITE ||
						functypclass == TYPEFUNC_COMPOSITE_DOMAIN)
					{
						/* 组合数据类型，例如表的行类型 */
						Assert(tupdesc);
						expandTupleDesc(tupdesc, rte->eref,
										rtfunc->funccolcount, atts_done,
										rtindex, sublevels_up,
										returning_type, location,
										include_dropped, colnames, colvars);
					}
					else if (functypclass == TYPEFUNC_SCALAR)
					{
						/* 基础数据类型，即标量 */
						if (colnames)
							*colnames = lappend(*colnames,
												list_nth(rte->eref->colnames,
														 atts_done));

						if (colvars)
						{
							Var		   *varnode;

							varnode = makeVar(rtindex, atts_done + 1,
											  funcrettype,
											  exprTypmod(rtfunc->funcexpr),
											  exprCollation(rtfunc->funcexpr),
											  sublevels_up);
							varnode->varreturningtype = returning_type;
							varnode->location = location;

							*colvars = lappend(*colvars, varnode);
						}
					}
					else if (functypclass == TYPEFUNC_RECORD)
					{
						if (colnames)
						{
							List	   *namelist;

							/* 提取列列表中适当的子集 */
							namelist = list_copy_tail(rte->eref->colnames,
													  atts_done);
							namelist = list_truncate(namelist,
													 rtfunc->funccolcount);
							*colnames = list_concat(*colnames, namelist);
						}

						if (colvars)
						{
							ListCell   *l1;
							ListCell   *l2;
							ListCell   *l3;
							int			attnum = atts_done;

							forthree(l1, rtfunc->funccoltypes,
									 l2, rtfunc->funccoltypmods,
									 l3, rtfunc->funccolcollations)
							{
								Oid			attrtype = lfirst_oid(l1);
								int32		attrtypmod = lfirst_int(l2);
								Oid			attrcollation = lfirst_oid(l3);
								Var		   *varnode;

								attnum++;
								varnode = makeVar(rtindex,
												  attnum,
												  attrtype,
												  attrtypmod,
												  attrcollation,
												  sublevels_up);
								varnode->varreturningtype = returning_type;
								varnode->location = location;
								*colvars = lappend(*colvars, varnode);
							}
						}
					}
					else
					{
						/* 这种错误本应由 addRangeTableEntryForFunction 捕获 */
						elog(ERROR, "function in FROM has unsupported return type");
					}
					atts_done += rtfunc->funccolcount;
				}

				/* 如果有序数（ordinality）列，则追加它 */
				if (rte->funcordinality)
				{
					if (colnames)
						*colnames = lappend(*colnames,
											llast(rte->eref->colnames));

					if (colvars)
					{
						Var		   *varnode = makeVar(rtindex,
													  atts_done + 1,
													  INT8OID,
													  -1,
													  InvalidOid,
													  sublevels_up);

						varnode->varreturningtype = returning_type;
						*colvars = lappend(*colvars, varnode);
					}
				}
			}
			break;
		case RTE_JOIN:
			{
				/* 连接 RTE */
				ListCell   *colname;
				ListCell   *aliasvar;

				Assert(list_length(rte->eref->colnames) == list_length(rte->joinaliasvars));

				varattno = 0;
				forboth(colname, rte->eref->colnames, aliasvar, rte->joinaliasvars)
				{
					Node	   *avar = (Node *) lfirst(aliasvar);

					varattno++;

				/*
				 * 在普通解析期间，连接中永远不会出现已删除的列。虽然这个函数
				 * 也被重写器和规划器使用，但它们目前不会对任何 JOIN RTE 调用
				 * 它。因此，接下来的这段代码是死代码，但正确地处理这种情况
				 * 似乎仍是谨慎之举。
				 */
					if (avar == NULL)
					{
						if (include_dropped)
						{
							if (colnames)
								*colnames = lappend(*colnames,
													makeString(pstrdup("")));
							if (colvars)
							{
							/*
							 * 这里不能使用连接的列类型（它可能被删除了！）；
							 * 不过 Const 声称是什么类型其实无所谓。
							 */
								*colvars = lappend(*colvars,
												   makeNullConst(INT4OID, -1,
																 InvalidOid));
							}
						}
						continue;
					}

					if (colnames)
					{
						char	   *label = strVal(lfirst(colname));

						*colnames = lappend(*colnames,
											makeString(pstrdup(label)));
					}

					if (colvars)
					{
						Var		   *varnode;

						/*
						 * 如果 joinaliasvars 条目是一个简单的 Var，只需复制它
						 * （同时调整 varlevelsup 和 location）；否则它就是
						 * 一个 JOIN USING 列，我们必须生成一个连接别名 Var。
						 * 这与 expandNSItemVars 对 "join.*" 展开本应产生的
						 * 结果一致——前提是我们能访问该连接的
						 * ParseNamespaceItem。
						 */
						if (IsA(avar, Var))
						{
							varnode = copyObject((Var *) avar);
							varnode->varlevelsup = sublevels_up;
						}
						else
							varnode = makeVar(rtindex, varattno,
											  exprType(avar),
											  exprTypmod(avar),
											  exprCollation(avar),
											  sublevels_up);
						varnode->varreturningtype = returning_type;
						varnode->location = location;

						*colvars = lappend(*colvars, varnode);
					}
				}
			}
			break;
		case RTE_TABLEFUNC:
		case RTE_VALUES:
		case RTE_CTE:
		case RTE_NAMEDTUPLESTORE:
			{
				/* Tablefunc、Values、CTE 或 ENR RTE */
				ListCell   *aliasp_item = list_head(rte->eref->colnames);
				ListCell   *lct;
				ListCell   *lcm;
				ListCell   *lcc;

				varattno = 0;
				forthree(lct, rte->coltypes,
						 lcm, rte->coltypmods,
						 lcc, rte->colcollations)
				{
					Oid			coltype = lfirst_oid(lct);
					int32		coltypmod = lfirst_int(lcm);
					Oid			colcoll = lfirst_oid(lcc);

					varattno++;

					if (colnames)
					{
						/* 假设每个输出列对应一个别名 */
						if (OidIsValid(coltype))
						{
							char	   *label = strVal(lfirst(aliasp_item));

							*colnames = lappend(*colnames,
												makeString(pstrdup(label)));
						}
						else if (include_dropped)
							*colnames = lappend(*colnames,
												makeString(pstrdup("")));

						aliasp_item = lnext(rte->eref->colnames, aliasp_item);
					}

					if (colvars)
					{
						if (OidIsValid(coltype))
						{
							Var		   *varnode;

							varnode = makeVar(rtindex, varattno,
											  coltype, coltypmod, colcoll,
											  sublevels_up);
							varnode->varreturningtype = returning_type;
							varnode->location = location;

							*colvars = lappend(*colvars, varnode);
						}
						else if (include_dropped)
						{
							/*
							 * Const 声称是什么类型其实无所谓。
							 */
							*colvars = lappend(*colvars,
											   makeNullConst(INT4OID, -1,
															 InvalidOid));
						}
					}
				}
			}
			break;
		case RTE_RESULT:
		case RTE_GROUP:
			/* 这些不暴露列，因此无需处理 */
			break;
		default:
			elog(ERROR, "unrecognized RTE kind: %d", (int) rte->rtekind);
	}
}

/*
 * expandRelation —— expandRTE 的子例程
 */
static void
expandRelation(Oid relid, Alias *eref, int rtindex, int sublevels_up,
			   VarReturningType returning_type,
			   int location, bool include_dropped,
			   List **colnames, List **colvars)
{
	Relation	rel;

	/* 获取元组描述符，并将其交给 expandTupleDesc 处理 */
	rel = relation_open(relid, AccessShareLock);
	expandTupleDesc(rel->rd_att, eref, rel->rd_att->natts, 0,
					rtindex, sublevels_up, returning_type,
					location, include_dropped,
					colnames, colvars);
	relation_close(rel, AccessShareLock);
}

/*
 * expandTupleDesc —— expandRTE 的子例程
 *
 * 为 tupdesc 的前 "count" 个属性生成名称和/或 Var，并将它们追加到
 * colnames/colvars。"offset" 会被加到每个 Var 原本应有的 varattno 上，同时
 * 我们也会跳过 eref->colnames 中前 "offset" 个条目。（这些安排使得这段代码
 * 也可用于 RTE_FUNCTION RTE 中单个返回组合类型的函数。）
 */
static void
expandTupleDesc(TupleDesc tupdesc, Alias *eref, int count, int offset,
				int rtindex, int sublevels_up,
				VarReturningType returning_type,
				int location, bool include_dropped,
				List **colnames, List **colvars)
{
	ListCell   *aliascell;
	int			varattno;

	aliascell = (offset < list_length(eref->colnames)) ?
		list_nth_cell(eref->colnames, offset) : NULL;

	Assert(count <= tupdesc->natts);
	for (varattno = 0; varattno < count; varattno++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, varattno);

		if (attr->attisdropped)
		{
			if (include_dropped)
			{
				if (colnames)
					*colnames = lappend(*colnames, makeString(pstrdup("")));
				if (colvars)
				{
					/*
					 * 这里不能使用 atttypid，不过 Const 声称是什么类型
					 * 其实无所谓。
					 */
					*colvars = lappend(*colvars,
									   makeNullConst(INT4OID, -1, InvalidOid));
				}
			}
			if (aliascell)
				aliascell = lnext(eref->colnames, aliascell);
			continue;
		}

		if (colnames)
		{
			char	   *label;

			if (aliascell)
			{
				label = strVal(lfirst(aliascell));
				aliascell = lnext(eref->colnames, aliascell);
			}
			else
			{
				/* 如果别名用完了，则使用底层名称 */
				label = NameStr(attr->attname);
			}
			*colnames = lappend(*colnames, makeString(pstrdup(label)));
		}

		if (colvars)
		{
			Var		   *varnode;

			varnode = makeVar(rtindex, varattno + offset + 1,
							  attr->atttypid, attr->atttypmod,
							  attr->attcollation,
							  sublevels_up);
			varnode->varreturningtype = returning_type;
			varnode->location = location;

			*colvars = lappend(*colvars, varnode);
		}
	}
}

/*
 * expandNSItemVars
 *	  为 nsitem 的未删除列生成 Var 列表，以及可选的列名列表。
 *
 * 生成的 Var 被标记为给定的 sublevels_up 和 location。
 *
 * 如果 colnames 不为 NULL，则会将各列的 String 项列表存放在那里；注意它
 * 只是 RTE 的 eref 列表的一个子集，因此其列表元素不得被修改。
 */
List *
expandNSItemVars(ParseState *pstate, ParseNamespaceItem *nsitem,
				 int sublevels_up, int location,
				 List **colnames)
{
	List	   *result = NIL;
	int			colindex;
	ListCell   *lc;

	if (colnames)
		*colnames = NIL;
	colindex = 0;
	foreach(lc, nsitem->p_names->colnames)
	{
		String	   *colnameval = lfirst(lc);
		const char *colname = strVal(colnameval);
		ParseNamespaceColumn *nscol = nsitem->p_nscolumns + colindex;

		if (nscol->p_dontexpand)
		{
			/* 跳过 */
		}
		else if (colname[0])
		{
			Var		   *var;

			Assert(nscol->p_varno > 0);
			var = makeVar(nscol->p_varno,
						  nscol->p_varattno,
						  nscol->p_vartype,
						  nscol->p_vartypmod,
						  nscol->p_varcollid,
						  sublevels_up);
			/* makeVar 不为这些字段提供参数，因此手动设置： */
			var->varreturningtype = nscol->p_varreturningtype;
			var->varnosyn = nscol->p_varnosyn;
			var->varattnosyn = nscol->p_varattnosyn;
			var->location = location;

			/* ... 并更新 varnullingrels */
			markNullableIfNeeded(pstate, var);

			result = lappend(result, var);
			if (colnames)
				*colnames = lappend(*colnames, colnameval);
		}
		else
		{
			/* 已删除列，忽略 */
			Assert(nscol->p_varno == 0);
		}
		colindex++;
	}
	return result;
}

/*
 * expandNSItemAttrs -
 *	  "*" 展开的主力：为 nsitem 的属性生成一组目标项（TargetEntry）
 *
 * pstate->p_next_resno 决定了分配给各 TLE 的 resno。
 * 如果调用者要求，被引用的列会被标记为需要 SELECT 访问权限。
 */
List *
expandNSItemAttrs(ParseState *pstate, ParseNamespaceItem *nsitem,
				  int sublevels_up, bool require_col_privs, int location)
{
	RangeTblEntry *rte = nsitem->p_rte;
	RTEPermissionInfo *perminfo = nsitem->p_perminfo;
	List	   *names,
			   *vars;
	ListCell   *name,
			   *var;
	List	   *te_list = NIL;

	vars = expandNSItemVars(pstate, nsitem, sublevels_up, location, &names);

	/*
	 * 要求对表有读权限。这通常与下面的 markVarForSelectPriv 调用是冗余的，
	 * 但当表没有列时则不然。如果 nsitem 是连接，我们无需做任何事：其组成表
	 * 在加入范围表时就已经被标记为 ACL_SELECT 了。（这一步只影响 UPDATE/
	 * DELETE 的目标关系，而它不可能位于连接之下。）
	 */
	if (rte->rtekind == RTE_RELATION)
	{
		Assert(perminfo != NULL);
		perminfo->requiredPerms |= ACL_SELECT;
	}

	forboth(name, names, var, vars)
	{
		char	   *label = strVal(lfirst(name));
		Var		   *varnode = (Var *) lfirst(var);
		TargetEntry *te;

		te = makeTargetEntry((Expr *) varnode,
							 (AttrNumber) pstate->p_next_resno++,
							 label,
							 false);
		te_list = lappend(te_list, te);

		if (require_col_privs)
		{
			/* 要求对每列有读权限 */
			markVarForSelectPriv(pstate, varnode);
		}
	}

	Assert(name == NULL && var == NULL);	/* 列表长度不一致？ */

	return te_list;
}

/*
 * get_rte_attribute_name
 *		从一个 RangeTblEntry 获取属性名
 *
 * 这与 get_attname() 不同，因为我们在可用时会使用别名。特别是，它能用于
 * 子选择或连接的 RTE，而 get_attname() 只能用于真实关系。
 *
 * 如果给定的 attnum 是 InvalidAttrNumber，则返回 "*"——这种情况出现在 Var
 * 表示一个关系的整个元组时。
 *
 * 调用者有责任不要对已经删除的属性调用本函数。（对于这种情况你也会得到
 * 某个答案，但它可能并不合理。）
 */
char *
get_rte_attribute_name(RangeTblEntry *rte, AttrNumber attnum)
{
	if (attnum == InvalidAttrNumber)
		return "*";

		/*
		 * 如果存在用户编写的列别名，则使用它。
		 */
	if (rte->alias &&
		attnum > 0 && attnum <= list_length(rte->alias->colnames))
		return strVal(list_nth(rte->alias->colnames, attnum - 1));

	/*
	 * 如果 RTE 是一个关系，则去系统目录中查找，而不是用 eref->colnames
	 * 列表。这稍慢一些，但如果在 eref 列表构建之后列被重命名了（对于规则
	 * 这很容易发生），它能给出正确答案。
	 */
	if (rte->rtekind == RTE_RELATION)
		return get_attname(rte->relid, attnum, false);

	/*
	 * 否则使用 eref 中的列名。这里应当总是有名字的。
	 */
	if (attnum > 0 && attnum <= list_length(rte->eref->colnames))
		return strVal(list_nth(rte->eref->colnames, attnum - 1));

	/* 否则调用者给了我们一个无效的 attnum */
	elog(ERROR, "invalid attnum %d for rangetable entry %s",
		 attnum, rte->eref->aliasname);
	return NULL;				/* 让编译器保持安静（避免告警） */
}

/*
 * get_rte_attribute_is_dropped
 *		检查所尝试的属性引用是否指向一个已删除的列
 */
bool
get_rte_attribute_is_dropped(RangeTblEntry *rte, AttrNumber attnum)
{
	bool		result;

	switch (rte->rtekind)
	{
		case RTE_RELATION:
			{
				/*
				 * 普通关系 RTE —— 获取该属性的系统目录条目
				 */
				HeapTuple	tp;
				Form_pg_attribute att_tup;

				tp = SearchSysCache2(ATTNUM,
									 ObjectIdGetDatum(rte->relid),
									 Int16GetDatum(attnum));
				if (!HeapTupleIsValid(tp))	/* 不应发生 */
					elog(ERROR, "cache lookup failed for attribute %d of relation %u",
						 attnum, rte->relid);
				att_tup = (Form_pg_attribute) GETSTRUCT(tp);
				result = att_tup->attisdropped;
				ReleaseSysCache(tp);
			}
			break;
		case RTE_SUBQUERY:
		case RTE_TABLEFUNC:
		case RTE_VALUES:
		case RTE_CTE:
		case RTE_GROUP:

			/*
			 * 子选择、表函数、Values、CTE、GROUP 类型的 RTE 永远不会
			 * 有已删除的列
			 */
			result = false;
			break;
		case RTE_NAMEDTUPLESTORE:
			{
				/* 通过测试 coltype 是否有效来检查是否已删除 */
				if (attnum <= 0 ||
					attnum > list_length(rte->coltypes))
					elog(ERROR, "invalid varattno %d", attnum);
				result = !OidIsValid((list_nth_oid(rte->coltypes, attnum - 1)));
			}
			break;
		case RTE_JOIN:
			{
				/*
				 * 一个连接 RTE 在构造时不会含有已删除的列，但存储规则中的连接
				 * 可能包含那些从底层表删除的列——前提是这些列在规则中没有任何
				 * 显式引用。这种情况会通过 joinaliasvars 列表中的一个空指针
				 * 传递给我们。
				 */
				Var		   *aliasvar;

				if (attnum <= 0 ||
					attnum > list_length(rte->joinaliasvars))
					elog(ERROR, "invalid varattno %d", attnum);
				aliasvar = (Var *) list_nth(rte->joinaliasvars, attnum - 1);

				result = (aliasvar == NULL);
			}
			break;
		case RTE_FUNCTION:
			{
				/* 函数 RTE */
				ListCell   *lc;
				int			atts_done = 0;

				/*
				 * 已删除的属性只可能出现在返回已命名组合类型的函数中。在这种
				 * 情况下，我们必须查找结果类型，看看它当前是否已删除了这一列。
				 * 因此，首先遍历各函数，直到找到覆盖所请求列的那个函数。
				 */
				foreach(lc, rte->functions)
				{
					RangeTblFunction *rtfunc = (RangeTblFunction *) lfirst(lc);

					if (attnum > atts_done &&
						attnum <= atts_done + rtfunc->funccolcount)
					{
						TupleDesc	tupdesc;

						/* 如果它有 coldeflist，则返回 RECORD */
						if (rtfunc->funccolnames != NIL)
							return false;	/* 不可能有任何已删除的列 */

						tupdesc = get_expr_result_tupdesc(rtfunc->funcexpr,
														  true);
						if (tupdesc)
						{
							/* 组合数据类型，例如表的行类型 */
							Form_pg_attribute att_tup;

							Assert(tupdesc);
							Assert(attnum - atts_done <= tupdesc->natts);
							att_tup = TupleDescAttr(tupdesc,
													attnum - atts_done - 1);
							return att_tup->attisdropped;
						}
						/* 否则，它不可能有任何已删除的列 */
						return false;
					}
					atts_done += rtfunc->funccolcount;
				}

				/* 如果走到这里，必定是在查找序数（ordinality）列 */
				if (rte->funcordinality && attnum == atts_done + 1)
					return false;

				/* 这大概不会发生 ... */
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column %d of relation \"%s\" does not exist",
								attnum,
								rte->eref->aliasname)));
				result = false; /* 让编译器保持安静（避免告警） */
			}
			break;
		case RTE_RESULT:
			/* 这大概不会发生 ... */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column %d of relation \"%s\" does not exist",
							attnum,
							rte->eref->aliasname)));
			result = false;		/* 让编译器保持安静（避免告警） */
			break;
		default:
			elog(ERROR, "unrecognized RTE kind: %d", (int) rte->rtekind);
			result = false;		/* 让编译器保持安静（避免告警） */
	}

	return result;
}

/*
 * 给定一个目标列表和一个 resno，返回匹配的 TargetEntry
 *
 * 如果列表中不存在该 resno，则返回 NULL。
 *
 * 注意：我们需要搜索，而不是简单地用 list_nth() 索引，因为并非所有
 * tlist 都是按 resno 排序的。
 */
TargetEntry *
get_tle_by_resno(List *tlist, AttrNumber resno)
{
	ListCell   *l;

	foreach(l, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->resno == resno)
			return tle;
	}
	return NULL;
}

/*
 * 给定一个 Query 和范围表索引，返回该关系的 RowMarkClause（如果存在）
 *
 * 如果关系未被选中用于 FOR UPDATE/SHARE，则返回 NULL
 */
RowMarkClause *
get_parse_rowmark(Query *qry, Index rtindex)
{
	ListCell   *l;

	foreach(l, qry->rowMarks)
	{
		RowMarkClause *rc = (RowMarkClause *) lfirst(l);

		if (rc->rti == rtindex)
			return rc;
	}
	return NULL;
}

/*
 *	给定一个关系和属性名，返回该变量的 attnum
 *
 *	如果属性不存在（或已删除），则返回 InvalidAttrNumber。
 *
 *	这应当仅在关系已经 table_open() 之后使用。对于尚未打开的关系，
 *	请使用缓存版本的 get_attnum()。
 */
int
attnameAttNum(Relation rd, const char *attname, bool sysColOK)
{
	int			i;

	for (i = 0; i < RelationGetNumberOfAttributes(rd); i++)
	{
		Form_pg_attribute att = TupleDescAttr(rd->rd_att, i);

		if (namestrcmp(&(att->attname), attname) == 0 && !att->attisdropped)
			return i + 1;
	}

	if (sysColOK)
	{
		if ((i = specialAttNum(attname)) != InvalidAttrNumber)
			return i;
	}

	/* 失败时 */
	return InvalidAttrNumber;
}

/* specialAttNum()
 *
 * 检查属性名，看它是否 "特殊"，例如 "xmin"。
 * - thomas 2000-02-07
 *
 * 注意：这只是判断该名称是否可能是一个系统属性。调用者需要确保它确实
 * 是该关系的一个属性。
 */
static int
specialAttNum(const char *attname)
{
	const FormData_pg_attribute *sysatt;

	sysatt = SystemAttributeByName(attname);
	if (sysatt != NULL)
		return sysatt->attnum;
	return InvalidAttrNumber;
}


/*
 * 给定一个属性 id，返回该属性的名称
 *
 *	这应当仅在关系已经 table_open() 之后使用。对于尚未打开的关系，
 *	请使用缓存版本的 get_atttype()。
 */
const NameData *
attnumAttName(Relation rd, int attid)
{
	if (attid <= 0)
	{
		const FormData_pg_attribute *sysatt;

		sysatt = SystemAttributeDefinition(attid);
		return &sysatt->attname;
	}
	if (attid > rd->rd_att->natts)
		elog(ERROR, "invalid attribute number %d", attid);
	return &TupleDescAttr(rd->rd_att, attid - 1)->attname;
}

/*
 * 给定一个属性 id，返回该属性的类型
 *
 *	这应当仅在关系已经 table_open() 之后使用。对于尚未打开的关系，
 *	请使用缓存版本的 get_atttype()。
 */
Oid
attnumTypeId(Relation rd, int attid)
{
	if (attid <= 0)
	{
		const FormData_pg_attribute *sysatt;

		sysatt = SystemAttributeDefinition(attid);
		return sysatt->atttypid;
	}
	if (attid > rd->rd_att->natts)
		elog(ERROR, "invalid attribute number %d", attid);
	return TupleDescAttr(rd->rd_att, attid - 1)->atttypid;
}

/*
 * 给定一个属性 id，返回该属性的排序规则
 *
 *	这应当仅在关系已经 table_open() 之后使用。
 */
Oid
attnumCollationId(Relation rd, int attid)
{
	if (attid <= 0)
	{
		/* 所有系统属性都是不可排序（noncollatable）的类型。 */
		return InvalidOid;
	}
	if (attid > rd->rd_att->natts)
		elog(ERROR, "invalid attribute number %d", attid);
	return TupleDescAttr(rd->rd_att, attid - 1)->attcollation;
}

/*
 * 针对缺失的 RTE 生成一条合适的错误消息。
 *
 * 由于这是一种非常常见的错误类型，我们颇费了一番功夫以产生有帮助的消息。
 */
void
errorMissingRTE(ParseState *pstate, RangeVar *relation)
{
	RangeTblEntry *rte;
	const char *badAlias = NULL;

	/*
	 * 检查查询的范围表中是否存在任何潜在的匹配。（注意：涉及 RangeVar 中
	 * 错误模式名的情况会立即在这里报错。这似乎没问题。）
	 */
	rte = searchRangeTableForRel(pstate, relation);

	/*
	 * 如果我们找到的匹配带有别名，且该别名在命名空间中可见，那么问题很可能
	 * 是使用了关系的真实名称而非其别名，即 "SELECT foo.* FROM foo f"。这种
	 * 错误很常见，足以让我们给出具体的提示。
	 *
	 * 如果我们找到的匹配不满足这些条件，则假定问题是非法地在作用域之外使用
	 * 了关系，就像 MySQL 风格的 "SELECT ... FROM a, b LEFT JOIN c ON
	 * (a.x = c.y)" 那样。
	 */
	if (rte && rte->alias &&
		strcmp(rte->eref->aliasname, relation->relname) != 0)
	{
		ParseNamespaceItem *nsitem;
		int			sublevels_up;

		nsitem = refnameNamespaceItem(pstate, NULL, rte->eref->aliasname,
									  relation->location,
									  &sublevels_up);
		if (nsitem && nsitem->p_rte == rte)
			badAlias = rte->eref->aliasname;
	}

	/* 如果用户看起来是忘了使用别名，则就此给出提示 */
	if (badAlias)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("invalid reference to FROM-clause entry for table \"%s\"",
						relation->relname),
				 errhint("Perhaps you meant to reference the table alias \"%s\".",
						 badAlias),
				 parser_errposition(pstate, relation->location)));
	/* 针对我们找到了（不可访问的）精确匹配的情况给出提示 */
	else if (rte)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("invalid reference to FROM-clause entry for table \"%s\"",
						relation->relname),
				 errdetail("There is an entry for table \"%s\", but it cannot be referenced from this part of the query.",
						   rte->eref->aliasname),
				 rte_visible_if_lateral(pstate, rte) ?
				 errhint("To reference that table, you must mark this subquery with LATERAL.") : 0,
				 parser_errposition(pstate, relation->location)));
	/* 否则，除了直白地陈述错误之外我们别无他法 */
	else
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("missing FROM-clause entry for table \"%s\"",
						relation->relname),
				 parser_errposition(pstate, relation->location)));
}

/*
 * 针对缺失的列生成一条合适的错误消息。
 *
 * 由于这是一种非常常见的错误类型，我们颇费了一番功夫以产生有帮助的消息。
 */
void
errorMissingColumn(ParseState *pstate,
				   const char *relname, const char *colname, int location)
{
	FuzzyAttrMatchState *state;

	/*
	 * 搜索整个 rtable 以寻找可能的匹配。如果找到，则针对它发出一条提示。
	 */
	state = searchRangeTableForCol(pstate, relname, colname, location);

	/*
	 * 如果存在精确匹配，那它必定因某种原因而不可访问。
	 */
	if (state->rexact1)
	{
		/*
		 * 当存在多个不可访问的精确匹配时，我们不会太费力，但至少要确保
		 * 不会误导性地暗示只有一个匹配。
		 */
		if (state->rexact2)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 relname ?
					 errmsg("column %s.%s does not exist", relname, colname) :
					 errmsg("column \"%s\" does not exist", colname),
					 errdetail("There are columns named \"%s\", but they are in tables that cannot be referenced from this part of the query.",
							   colname),
					 !relname ? errhint("Try using a table-qualified name.") : 0,
					 parser_errposition(pstate, location)));
		/* 单个精确匹配，因此尝试判断它为何不可访问。 */
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 relname ?
				 errmsg("column %s.%s does not exist", relname, colname) :
				 errmsg("column \"%s\" does not exist", colname),
				 errdetail("There is a column named \"%s\" in table \"%s\", but it cannot be referenced from this part of the query.",
						   colname, state->rexact1->eref->aliasname),
				 rte_visible_if_lateral(pstate, state->rexact1) ?
				 errhint("To reference that column, you must mark this subquery with LATERAL.") :
				 (!relname && rte_visible_if_qualified(pstate, state->rexact1)) ?
				 errhint("To reference that column, you must use a table-qualified name.") : 0,
				 parser_errposition(pstate, location)));
	}

	if (!state->rsecond)
	{
		/* 如果我们根本没找到任何匹配，那就没什么可报告的了 */
		if (!state->rfirst)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 relname ?
					 errmsg("column %s.%s does not exist", relname, colname) :
					 errmsg("column \"%s\" does not exist", colname),
					 parser_errposition(pstate, location)));
		/* 处理我们只有一个备选拼写可提供的情况 */
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 relname ?
				 errmsg("column %s.%s does not exist", relname, colname) :
				 errmsg("column \"%s\" does not exist", colname),
				 errhint("Perhaps you meant to reference the column \"%s.%s\".",
						 state->rfirst->eref->aliasname,
						 strVal(list_nth(state->rfirst->eref->colnames,
										 state->first - 1))),
				 parser_errposition(pstate, location)));
	}
	else
	{
		/* 处理存在两个同样有用的列提示的情况 */
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 relname ?
				 errmsg("column %s.%s does not exist", relname, colname) :
				 errmsg("column \"%s\" does not exist", colname),
				 errhint("Perhaps you meant to reference the column \"%s.%s\" or the column \"%s.%s\".",
						 state->rfirst->eref->aliasname,
						 strVal(list_nth(state->rfirst->eref->colnames,
										 state->first - 1)),
						 state->rsecond->eref->aliasname,
						 strVal(list_nth(state->rsecond->eref->colnames,
										 state->second - 1))),
				 parser_errposition(pstate, location)));
	}
}

/*
 * 查找 RTE 对应的 ParseNamespaceItem（只要它是可见的）。
 * 我们假设一个 RTE 在命名空间列表中不会出现超过一次。
 */
static ParseNamespaceItem *
findNSItemForRTE(ParseState *pstate, RangeTblEntry *rte)
{
	while (pstate != NULL)
	{
		ListCell   *l;

		foreach(l, pstate->p_namespace)
		{
			ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(l);

			if (nsitem->p_rte == rte)
				return nsitem;
		}
		pstate = pstate->parentParseState;
	}
	return NULL;
}

/*
 * 如果用户当时写了 LATERAL，这个 RTE 会可见吗？
 *
 * 这是一个用于决定是否发出关于 LATERAL 的提示（HINT）的辅助函数。
 * 因此，它不需要 100% 准确；即使提示不太准确，也可能有用。所以，我们不
 * 会深入探究所找到的 nsitem 是否设置了适当的 p_rel_visible 或 p_cols_visible。
 */
static bool
rte_visible_if_lateral(ParseState *pstate, RangeTblEntry *rte)
{
	ParseNamespaceItem *nsitem;

	/* 如果 LATERAL 处于活动状态，那我们显然找错了方向 */
	if (pstate->p_lateral_active)
		return false;
	nsitem = findNSItemForRTE(pstate, rte);
	if (nsitem)
	{
		/* 找到了，报告它是否仅为 LATERAL */
		return nsitem->p_lateral_only && nsitem->p_lateral_ok;
	}
	return false;
}

/*
 * 如果加上限定名，这个 RTE 中的列会可见吗？
 */
static bool
rte_visible_if_qualified(ParseState *pstate, RangeTblEntry *rte)
{
	ParseNamespaceItem *nsitem = findNSItemForRTE(pstate, rte);

	if (nsitem)
	{
		/* 找到了，报告它是否仅为关系级 */
		return nsitem->p_rel_visible && !nsitem->p_cols_visible;
	}
	return false;
}


/*
 * 检查一个完全解析后的查询，当且仅当查询底层有任何关系是临时关系
 * （表、视图或物化视图）时返回 true。
 */
bool
isQueryUsingTempRelation(Query *query)
{
	return isQueryUsingTempRelation_walker((Node *) query, NULL);
}

static bool
isQueryUsingTempRelation_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Query))
	{
		Query	   *query = (Query *) node;
		ListCell   *rtable;

		foreach(rtable, query->rtable)
		{
			RangeTblEntry *rte = lfirst(rtable);

			if (rte->rtekind == RTE_RELATION)
			{
				Relation	rel = table_open(rte->relid, AccessShareLock);
				char		relpersistence = rel->rd_rel->relpersistence;

				table_close(rel, AccessShareLock);
				if (relpersistence == RELPERSISTENCE_TEMP)
					return true;
			}
		}

		return query_tree_walker(query,
								 isQueryUsingTempRelation_walker,
								 context,
								 QTW_IGNORE_JOINALIASES);
	}

	return expression_tree_walker(node,
								  isQueryUsingTempRelation_walker,
								  context);
}

/*
 * addRTEPermissionInfo
 *		为给定的 RTE 创建 RTEPermissionInfo 并将其加入所提供的列表。
 *
 * 返回该 RTEPermissionInfo，并设置 rte->perminfoindex。
 */
RTEPermissionInfo *
addRTEPermissionInfo(List **rteperminfos, RangeTblEntry *rte)
{
	RTEPermissionInfo *perminfo;

	Assert(OidIsValid(rte->relid));
	Assert(rte->perminfoindex == 0);

	/* 没有，则创建一个并加入列表。 */
	perminfo = makeNode(RTEPermissionInfo);
	perminfo->relid = rte->relid;
	perminfo->inh = rte->inh;
	/* 其他信息在需要时被提取该节点时设置。 */

	*rteperminfos = lappend(*rteperminfos, perminfo);

	/* 记下它的索引（从 1 开始！） */
	rte->perminfoindex = list_length(*rteperminfos);

	return perminfo;
}

/*
 * getRTEPermissionInfo
 *		在所提供的列表中为给定关系查找 RTEPermissionInfo。
 *
 * 这是一个简单的 list_nth() 操作，不过有这个函数来进行各种合理性检查
 * 是件好事。
 */
RTEPermissionInfo *
getRTEPermissionInfo(List *rteperminfos, RangeTblEntry *rte)
{
	RTEPermissionInfo *perminfo;

	if (rte->perminfoindex == 0 ||
		rte->perminfoindex > list_length(rteperminfos))
		elog(ERROR, "invalid perminfoindex %u in RTE with relid %u",
			 rte->perminfoindex, rte->relid);
	perminfo = list_nth_node(RTEPermissionInfo, rteperminfos,
							 rte->perminfoindex - 1);
	if (perminfo->relid != rte->relid)
		elog(ERROR, "permission info at index %u (with relid=%u) does not match provided RTE (with relid=%u)",
			 rte->perminfoindex, perminfo->relid, rte->relid);

	return perminfo;
}
