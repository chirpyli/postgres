/*-------------------------------------------------------------------------
 *
 * analyze.c
 *	  将原始解析树转换为查询树
 *
 * 对于可优化的语句，我们会仔细地对所引用的每个表获取适当的锁，
 * 而后端的其他模块在依赖这些结果之前，会保留或重新获取这些锁。
 * 因此，对这些语句进行大量的语义分析是安全的。对于实用命令
 * （utility commands），此处不获取任何锁（即便获取了，也无法保证
 * 在执行时仍然持有）。因此，实用命令的一般规则是直接将其原样
 * 放入一个 Query 节点中。DECLARE CURSOR、EXPLAIN 以及
 * CREATE TABLE AS 是例外，因为它们包含可优化的语句，需要被转换。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	src/backend/parser/analyze.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/sysattr.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/queryjumble.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parse_agg.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_cte.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_merge.h"
#include "parser/parse_oper.h"
#include "parser/parse_param.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "parser/parsetree.h"
#include "utils/backend_status.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/* 供插件在解析分析结束时获取控制权的钩子 */
post_parse_analyze_hook_type post_parse_analyze_hook = NULL;

static Query *transformOptionalSelectInto(ParseState *pstate, Node *parseTree);
static Query *transformDeleteStmt(ParseState *pstate, DeleteStmt *stmt);
static Query *transformInsertStmt(ParseState *pstate, InsertStmt *stmt);
static OnConflictExpr *transformOnConflictClause(ParseState *pstate,
												 OnConflictClause *onConflictClause);
static int	count_rowexpr_columns(ParseState *pstate, Node *expr);
static Query *transformSelectStmt(ParseState *pstate, SelectStmt *stmt);
static Query *transformValuesClause(ParseState *pstate, SelectStmt *stmt);
static Query *transformSetOperationStmt(ParseState *pstate, SelectStmt *stmt);
static Node *transformSetOperationTree(ParseState *pstate, SelectStmt *stmt,
									   bool isTopLevel, List **targetlist);
static void determineRecursiveColTypes(ParseState *pstate,
									   Node *larg, List *nrtargetlist);
static Query *transformReturnStmt(ParseState *pstate, ReturnStmt *stmt);
static Query *transformUpdateStmt(ParseState *pstate, UpdateStmt *stmt);
static Query *transformPLAssignStmt(ParseState *pstate,
									PLAssignStmt *stmt);
static Query *transformDeclareCursorStmt(ParseState *pstate,
										 DeclareCursorStmt *stmt);
static Query *transformExplainStmt(ParseState *pstate,
								   ExplainStmt *stmt);
static Query *transformCreateTableAsStmt(ParseState *pstate,
										 CreateTableAsStmt *stmt);
static Query *transformCallStmt(ParseState *pstate,
								CallStmt *stmt);
static void transformLockingClause(ParseState *pstate, Query *qry,
								   LockingClause *lc, bool pushedDown);
#ifdef DEBUG_NODE_TESTS_ENABLED
static bool test_raw_expression_coverage(Node *node, void *context);
#endif


/*
 * parse_analyze_fixedparams
 *		分析原始解析树并将其转换为 Query 形式。
 *
 * 可以选择性地提供关于 $n 参数类型的信息。
 * 不允许引用 paramTypes[] 中未定义的 $n 索引。
 *
 * 结果是一个 Query 节点。可优化的语句需要进行大量的转换，
 * 而实用命令类型的语句则简单地挂在一个占位用的 CMD_UTILITY
 * Query 节点上。
 */
Query *
parse_analyze_fixedparams(RawStmt *parseTree, const char *sourceText,
						  const Oid *paramTypes, int numParams,
						  QueryEnvironment *queryEnv)
{
	ParseState *pstate = make_parsestate(NULL);
	Query	   *query;
	JumbleState *jstate = NULL;

	Assert(sourceText != NULL); /* required as of 8.4 */

	pstate->p_sourcetext = sourceText;

	if (numParams > 0)
		setup_parse_fixed_parameters(pstate, paramTypes, numParams);

	pstate->p_queryEnv = queryEnv;

	query = transformTopLevelStmt(pstate, parseTree);

	if (IsQueryIdEnabled())
		jstate = JumbleQuery(query);

	if (post_parse_analyze_hook)
		(*post_parse_analyze_hook) (pstate, query, jstate);

	free_parsestate(pstate);

	pgstat_report_query_id(query->queryId, false);

	return query;
}

/*
 * parse_analyze_varparams
 *
 * 当可以从上下文推断关于 $n 符号数据类型的信息时，使用此变体。
 * 传入的 paramTypes[] 数组可以被修改或扩展（通过 repalloc）。
 */
Query *
parse_analyze_varparams(RawStmt *parseTree, const char *sourceText,
						Oid **paramTypes, int *numParams,
						QueryEnvironment *queryEnv)
{
	ParseState *pstate = make_parsestate(NULL);
	Query	   *query;
	JumbleState *jstate = NULL;

	Assert(sourceText != NULL); /* required as of 8.4 */

	pstate->p_sourcetext = sourceText;

	setup_parse_variable_parameters(pstate, paramTypes, numParams);

	pstate->p_queryEnv = queryEnv;

	query = transformTopLevelStmt(pstate, parseTree);

	/* make sure all is well with parameter types */
	check_variable_parameters(pstate, query);

	if (IsQueryIdEnabled())
		jstate = JumbleQuery(query);

	if (post_parse_analyze_hook)
		(*post_parse_analyze_hook) (pstate, query, jstate);

	free_parsestate(pstate);

	pgstat_report_query_id(query->queryId, false);

	return query;
}

/*
 * parse_analyze_withcb
 *
 * 当调用者提供自己的解析器回调来解析参数以及可能的其他事项时，
 * 使用此变体。
 */
Query *
parse_analyze_withcb(RawStmt *parseTree, const char *sourceText,
					 ParserSetupHook parserSetup,
					 void *parserSetupArg,
					 QueryEnvironment *queryEnv)
{
	ParseState *pstate = make_parsestate(NULL);
	Query	   *query;
	JumbleState *jstate = NULL;

	Assert(sourceText != NULL); /* required as of 8.4 */

	pstate->p_sourcetext = sourceText;
	pstate->p_queryEnv = queryEnv;
	(*parserSetup) (pstate, parserSetupArg);

	query = transformTopLevelStmt(pstate, parseTree);

	if (IsQueryIdEnabled())
		jstate = JumbleQuery(query);

	if (post_parse_analyze_hook)
		(*post_parse_analyze_hook) (pstate, query, jstate);

	free_parsestate(pstate);

	pgstat_report_query_id(query->queryId, false);

	return query;
}


/*
 * parse_sub_analyze
 *		以递归方式分析子语句的入口点。
 */
Query *
parse_sub_analyze(Node *parseTree, ParseState *parentParseState,
				  CommonTableExpr *parentCTE,
				  bool locked_from_parent,
				  bool resolve_unknowns)
{
	ParseState *pstate = make_parsestate(parentParseState);
	Query	   *query;

	pstate->p_parent_cte = parentCTE;
	pstate->p_locked_from_parent = locked_from_parent;
	pstate->p_resolve_unknowns = resolve_unknowns;

	query = transformStmt(pstate, parseTree);

	free_parsestate(pstate);

	return query;
}

/*
 * transformTopLevelStmt -
 *		将一个 Parse 树转换为 Query 树。
 *
 * 本函数只负责将语句位置信息从 RawStmt 转移到最终生成的 Query 中。
 */
Query *
transformTopLevelStmt(ParseState *pstate, RawStmt *parseTree)
{
	Query	   *result;

	/* We're at top level, so allow SELECT INTO */
	result = transformOptionalSelectInto(pstate, parseTree->stmt);

	result->stmt_location = parseTree->stmt_location;
	result->stmt_len = parseTree->stmt_len;

	return result;
}

/*
 * transformOptionalSelectInto -
 *		如果 SELECT 带有 INTO，则将其转换为 CREATE TABLE AS。
 *
 * 我们在此处所做的、与 transformStmt() 不同的唯一事情，就是将
 * SELECT ... INTO 转换为 CREATE TABLE AS。由于实用命令不允许
 * 嵌套在更大的语句中，这只允许出现在解析树的顶层，因此我们只在
 * 进入递归的 transformStmt() 处理之前尝试这样做。
 */
static Query *
transformOptionalSelectInto(ParseState *pstate, Node *parseTree)
{
	if (IsA(parseTree, SelectStmt))
	{
		SelectStmt *stmt = (SelectStmt *) parseTree;

		/* If it's a set-operation tree, drill down to leftmost SelectStmt */
		while (stmt && stmt->op != SETOP_NONE)
			stmt = stmt->larg;
		Assert(stmt && IsA(stmt, SelectStmt) && stmt->larg == NULL);

		if (stmt->intoClause)
		{
			CreateTableAsStmt *ctas = makeNode(CreateTableAsStmt);

			ctas->query = parseTree;
			ctas->into = stmt->intoClause;
			ctas->objtype = OBJECT_TABLE;
			ctas->is_select_into = true;

			/*
			 * 从 SelectStmt 中移除 intoClause。这样可以让 transformSelectStmt
			 * 在发现 intoClause 被设置时（意味着 INTO 出现在了不允许的位置）
			 * 安全地报错。
			 */
			stmt->intoClause = NULL;

			parseTree = (Node *) ctas;
		}
	}

	return transformStmt(pstate, parseTree);
}

/*
 * transformStmt -
 *		以递归方式将一个 Parse 树转换为 Query 树。
 */
Query *
transformStmt(ParseState *pstate, Node *parseTree)
{
	Query	   *result;

#ifdef DEBUG_NODE_TESTS_ENABLED

	/*
	 * 我们将 debug_raw_expression_coverage_test 测试应用于基本的 DML
	 * 语句；不能简单地对所有内容运行它，因为
	 * raw_expression_tree_walker() 并不声称能处理实用命令语句。
	 */
	if (Debug_raw_expression_coverage_test)
	{
		switch (nodeTag(parseTree))
		{
			case T_SelectStmt:
			case T_InsertStmt:
			case T_UpdateStmt:
			case T_DeleteStmt:
			case T_MergeStmt:
				(void) test_raw_expression_coverage(parseTree, NULL);
				break;
			default:
				break;
		}
	}
#endif							/* DEBUG_NODE_TESTS_ENABLED */

	/*
	 * 注意：当修改在此处具有非默认处理的语句类型集合时，还要参考
	 * stmt_requires_parse_analysis() 和 analyze_requires_snapshot()。
	 */
	switch (nodeTag(parseTree))
	{
			/*
			 * 可优化的语句
			 */
		case T_InsertStmt:
			result = transformInsertStmt(pstate, (InsertStmt *) parseTree);
			break;

		case T_DeleteStmt:
			result = transformDeleteStmt(pstate, (DeleteStmt *) parseTree);
			break;

		case T_UpdateStmt:
			result = transformUpdateStmt(pstate, (UpdateStmt *) parseTree);
			break;

		case T_MergeStmt:
			result = transformMergeStmt(pstate, (MergeStmt *) parseTree);
			break;

		case T_SelectStmt:
			{
				SelectStmt *n = (SelectStmt *) parseTree;

				if (n->valuesLists)
					result = transformValuesClause(pstate, n);
				else if (n->op == SETOP_NONE)
					result = transformSelectStmt(pstate, n);
				else
					result = transformSetOperationStmt(pstate, n);
			}
			break;

		case T_ReturnStmt:
			result = transformReturnStmt(pstate, (ReturnStmt *) parseTree);
			break;

		case T_PLAssignStmt:
			result = transformPLAssignStmt(pstate,
										   (PLAssignStmt *) parseTree);
			break;

			/*
			 * 特殊情况
			 */
		case T_DeclareCursorStmt:
			result = transformDeclareCursorStmt(pstate,
												(DeclareCursorStmt *) parseTree);
			break;

		case T_ExplainStmt:
			result = transformExplainStmt(pstate,
										  (ExplainStmt *) parseTree);
			break;

		case T_CreateTableAsStmt:
			result = transformCreateTableAsStmt(pstate,
												(CreateTableAsStmt *) parseTree);
			break;

		case T_CallStmt:
			result = transformCallStmt(pstate,
									   (CallStmt *) parseTree);
			break;

		default:

		/*
		 * 其他语句不需要任何转换；只需返回原始解析树，并在其顶部
		 * 挂上一个 Query 节点即可。
		 */
			result = makeNode(Query);
			result->commandType = CMD_UTILITY;
			result->utilityStmt = (Node *) parseTree;
			break;
	}

	/* 在得知不同情况之前，先标记为原始查询 */
	result->querySource = QSRC_ORIGINAL;
	result->canSetTag = true;

	return result;
}

/*
 * stmt_requires_parse_analysis
 *		如果解析分析会对给定的原始解析树做某些非平凡的处理，
 *		则返回 true。
 *
 * 一般来说，对于任何 transformStmt() 不仅仅是在其外层包裹一个
 * CMD_UTILITY Query 的语句类型，本函数都应返回 true。当返回 false 时，
 * 调用者可以假定不存在任何需要重新进行原始语句解析分析的情况。
 *
 * 目前，由于重写器和规划器对 CMD_UTILITY 类型的 Query 不做任何处理，
 * 返回 false 意味着整个解析分析/重写/规划流水线都不需要被重新执行。
 * 如果这一点将来发生变化，调用者很可能需要相应调整。
 */
bool
stmt_requires_parse_analysis(RawStmt *parseTree)
{
	bool		result;

	switch (nodeTag(parseTree->stmt))
	{
			/*
			 * 可优化的语句
			 */
		case T_InsertStmt:
		case T_DeleteStmt:
		case T_UpdateStmt:
		case T_MergeStmt:
		case T_SelectStmt:
		case T_ReturnStmt:
		case T_PLAssignStmt:
			result = true;
			break;

			/*
			 * 特殊情况
			 */
		case T_DeclareCursorStmt:
		case T_ExplainStmt:
		case T_CreateTableAsStmt:
		case T_CallStmt:
			result = true;
			break;

		default:
			/* 所有其他语句只是被包裹在一个 CMD_UTILITY Query 中 */
			result = false;
			break;
	}

	return result;
}

/*
 * analyze_requires_snapshot
 *		如果必须对给定原始解析树设置快照后才能进行解析分析，
 *		则返回 true。
 */
bool
analyze_requires_snapshot(RawStmt *parseTree)
{
	/*
	 * 目前，本函数应在与 stmt_requires_parse_analysis() 完全相同的情形下
	 * 返回 true，因此我们直接调用该函数，而不是重复其逻辑。我们保留两个
	 * 独立的入口点，是为了让调用者的意图更清晰，因为在调用者看来这两者
	 * 是不同的条件。
	 *
	 * 虽然将来某天可能会出现这样一种语句类型：transformStmt() 对其做了
	 * 某些非平凡的处理，但处理过程却不需要快照；不过做出这样的选择
	 * 似乎很脆弱。如果你想设置例外，请在注释中记录其理由。
	 */
	return stmt_requires_parse_analysis(parseTree);
}

/*
 * query_requires_rewrite_plan()
 *		如果对本 Query 的重写或规划并非平凡操作，则返回 true。
 *
 * 这与 stmt_requires_parse_analysis() 非常相似，但作用在流水线
 * 更下游的一个步骤上。
 *
 * 我们不提供与 analyze_requires_snapshot() 等价的函数：调用者可以
 * 假定任何重写或规划活动都需要快照。
 */
bool
query_requires_rewrite_plan(Query *query)
{
	bool		result;

	if (query->commandType != CMD_UTILITY)
	{
		/* 所有可优化的语句都需要重写/规划 */
		result = true;
	}
	else
	{
		/* 此列表应与 stmt_requires_parse_analysis() 保持一致 */
		switch (nodeTag(query->utilityStmt))
		{
			case T_DeclareCursorStmt:
			case T_ExplainStmt:
			case T_CreateTableAsStmt:
			case T_CallStmt:
				result = true;
				break;
			default:
				result = false;
				break;
		}
	}
	return result;
}

/*
 * transformDeleteStmt -
 *		转换一个 DELETE 语句
 */
static Query *
transformDeleteStmt(ParseState *pstate, DeleteStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	ParseNamespaceItem *nsitem;
	Node	   *qual;

	qry->commandType = CMD_DELETE;

	/* 独立于其他处理，单独处理 WITH 子句 */
	if (stmt->withClause)
	{
		qry->hasRecursive = stmt->withClause->recursive;
		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	/* 仅用结果关系建立范围表 */
	qry->resultRelation = setTargetTable(pstate, stmt->relation,
										 stmt->relation->inh,
										 true,
										 ACL_DELETE);
	nsitem = pstate->p_target_nsitem;

	/* 禁止在视图上使用 DELETE ... WHERE CURRENT OF */
	if (stmt->whereClause &&
		IsA(stmt->whereClause, CurrentOfExpr) &&
		pstate->p_target_relation->rd_rel->relkind == RELKIND_VIEW)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("WHERE CURRENT OF on a view is not implemented"));

	/* DELETE 中没有 DISTINCT */
	qry->distinctClause = NIL;

	/* USING 中的子查询不能访问结果关系 */
	nsitem->p_lateral_only = true;
	nsitem->p_lateral_ok = false;

	/*
	 * USING 子句并非标准 SQL 语法，其功能等同于可以为 UPDATE 指定的
	 * FROM 列表。这里使用 USING 关键字而不是 FROM，是因为 FROM 在
	 * DELETE 语法中已经是一个关键字。
	 */
	transformFromClause(pstate, stmt->usingClause);

	/* 其余子句可以正常引用结果关系 */
	nsitem->p_lateral_only = false;
	nsitem->p_lateral_ok = true;

	qual = transformWhereClause(pstate, stmt->whereClause,
								EXPR_KIND_WHERE, "WHERE");

	transformReturningClause(pstate, qry, stmt->returningClause,
							 EXPR_KIND_RETURNING);

	/* 范围表和连接树构建完成 */
	qry->rtable = pstate->p_rtable;
	qry->rteperminfos = pstate->p_rteperminfos;
	qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
	qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
	qry->hasAggs = pstate->p_hasAggs;

	assign_query_collations(pstate, qry);

	/* 这必须在处理排序规则之后进行，以保证表达式比较的可靠性 */
	if (pstate->p_hasAggs)
		parseCheckAggregates(pstate, qry);

	return qry;
}

/*
 * transformInsertStmt -
 *		转换一个 INSERT 语句
 */
static Query *
transformInsertStmt(ParseState *pstate, InsertStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	SelectStmt *selectStmt = (SelectStmt *) stmt->selectStmt;
	List	   *exprList = NIL;
	bool		isGeneralSelect;
	List	   *sub_rtable;
	List	   *sub_rteperminfos;
	List	   *sub_namespace;
	List	   *icolumns;
	List	   *attrnos;
	ParseNamespaceItem *nsitem;
	RTEPermissionInfo *perminfo;
	ListCell   *icols;
	ListCell   *attnos;
	ListCell   *lc;
	bool		isOnConflictUpdate;
	AclMode		targetPerms;

	/* 不可能有需要担心的外层 WITH */
	Assert(pstate->p_ctenamespace == NIL);

	qry->commandType = CMD_INSERT;
	pstate->p_is_insert = true;

	/* 独立于其他处理，单独处理 WITH 子句 */
	if (stmt->withClause)
	{
		qry->hasRecursive = stmt->withClause->recursive;
		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	qry->override = stmt->override;

	isOnConflictUpdate = (stmt->onConflictClause &&
						  stmt->onConflictClause->action == ONCONFLICT_UPDATE);

	/*
	 * 我们需要处理三种情况：DEFAULT VALUES（selectStmt == NULL）、
	 * VALUES 列表，或一般的 SELECT 输入。我们对 VALUES 做特殊处理，
	 * 既是为了效率，也是为了能够处理 DEFAULT 说明。
	 *
	 * 语法允许在 VALUES 子句上附加 ORDER BY、LIMIT、FOR UPDATE 或 WITH。
	 * 如果带有其中任何一种，就将其当作一般的 SELECT 处理；这样它就能
	 * 正常工作，但你不能在这些情况下使用 DEFAULT 项。
	 */
	isGeneralSelect = (selectStmt && (selectStmt->valuesLists == NIL ||
									  selectStmt->sortClause != NIL ||
									  selectStmt->limitOffset != NULL ||
									  selectStmt->limitCount != NULL ||
									  selectStmt->lockingClause != NIL ||
									  selectStmt->withClause != NULL));

	/*
	 * 如果传入了一个非空的 rangetable/namespace，并且我们正在执行
	 * INSERT/SELECT，则安排将 rangetable/rteperminfos/namespace 下传给
	 * SELECT。这只可能发生在 CREATE RULE 内部，此时我们希望规则的 OLD 和
	 * NEW 范围表项作为 SELECT 的范围表的一部分出现，而不是作为它的
	 * 外层引用。（权宜之计！）不过 SELECT 的 joinlist 不受影响。我们
	 * 必须在将目标表加入 INSERT 的范围表之前完成这一步。
	 */
	if (isGeneralSelect)
	{
		sub_rtable = pstate->p_rtable;
		pstate->p_rtable = NIL;
		sub_rteperminfos = pstate->p_rteperminfos;
		pstate->p_rteperminfos = NIL;
		sub_namespace = pstate->p_namespace;
		pstate->p_namespace = NIL;
	}
	else
	{
		sub_rtable = NIL;		/* 未使用，但为避免编译器告警而保留 */
		sub_rteperminfos = NIL;
		sub_namespace = NIL;
	}

	/*
	 * 在扫描 SELECT 之前，必须获取 INSERT 目标表上的写锁，否则如果目标表
	 * 也在 SELECT 部分被提及，我们将获取错误类型的初始锁。注意目标表
	 * 不会被加入 joinlist 或 namespace。
	 */
	targetPerms = ACL_INSERT;
	if (isOnConflictUpdate)
		targetPerms |= ACL_UPDATE;
	qry->resultRelation = setTargetTable(pstate, stmt->relation,
										 false, false, targetPerms);

	/* 校验 stmt->cols 列表；若未给出列表则构建默认列表 */
	icolumns = checkInsertTargets(pstate, stmt->cols, &attrnos);
	Assert(list_length(icolumns) == list_length(attrnos));

	/*
	 * 确定我们遇到的是哪种形式的 INSERT。
	 */
	if (selectStmt == NULL)
	{
		/*
		 * 我们遇到的是 INSERT ... DEFAULT VALUES。我们可以通过发出一个
		 * 空的目标列表来处理这种情况——当规划器展开目标列表时，
		 * 所有列都将被赋予默认值。
		 */
		exprList = NIL;
	}
	else if (isGeneralSelect)
	{
		/*
		 * 我们将 sub-pstate 设为外层 pstate 的子节点，这样它就能看到
		 * 从上层提供的任何 Param 定义。由于外层 pstate 的 rtable 和
		 * namespace 当前为空，因此暴露那些子 SELECT 本不应看到的名字
		 * 不会带来副作用。
		 */
		ParseState *sub_pstate = make_parsestate(pstate);
		Query	   *selectQuery;

		/*
		 * 处理作为来源的 SELECT。
		 *
		 * 重要的一点是，这必须像处理独立的 SELECT 一样处理；否则 INSERT
		 * 内部 SELECT 的行为可能会与独立的 SELECT 不同。（事实上，Postgres
		 * 直到 6.5 版本都存在这类 bug……）
		 *
		 * 唯一的例外是，我们会阻止将未知类型的输出解析为 TEXT。这并不
		 * 改变语义，因为如果列类型在语义上很重要，它本来就会被解析为
		 * 其他类型。这样做可以让我们把此类输出解析为目标列的类型，
		 * 这一点我们在下面处理。
		 */
		sub_pstate->p_rtable = sub_rtable;
		sub_pstate->p_rteperminfos = sub_rteperminfos;
		sub_pstate->p_joinexprs = NIL;	/* sub_rtable has no joins */
		sub_pstate->p_nullingrels = NIL;
		sub_pstate->p_namespace = sub_namespace;
		sub_pstate->p_resolve_unknowns = false;

		selectQuery = transformStmt(sub_pstate, stmt->selectStmt);

		free_parsestate(sub_pstate);

		/* 语法本应生成一个 SELECT */
		if (!IsA(selectQuery, Query) ||
			selectQuery->commandType != CMD_SELECT)
			elog(ERROR, "unexpected non-SELECT command in INSERT ... SELECT");

		/*
		 * 让该来源成为 INSERT 范围表中的一个子查询，并将其加入 INSERT 的
		 * joinlist（但不加入 namespace）。
		 */
		nsitem = addRangeTableEntryForSubquery(pstate,
											   selectQuery,
											   makeAlias("*SELECT*", NIL),
											   false,
											   false);
		addNSItemToQuery(pstate, nsitem, true, false, false);

		/*----------
		 * 为 INSERT 生成一个表达式列表，选取子查询中所有非 resjunk 的列。
		 * （INSERT 的目标列表必须与子查询的目标列表分开，因为我们可能
		 * 会添加列、插入数据类型强制转换等。）
		 *
		 * 取巧之处（HACK）：SELECT 目标列表中的未知类型常量和参数会被
		 * 原样向上复制，而不是作为子查询的输出被引用。这是为了确保当
		 * 我们尝试将它们强制转换为目标列的数据类型时，能得到正确的
		 * 结果（参见 coerce_type 中的特殊情况）。否则，下面的语句会失败：
		 *		INSERT INTO foo SELECT 'bar', ... FROM baz
		 *----------
		 */
		exprList = NIL;
		foreach(lc, selectQuery->targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);
			Expr	   *expr;

			if (tle->resjunk)
				continue;
			if (tle->expr &&
				(IsA(tle->expr, Const) || IsA(tle->expr, Param)) &&
				exprType((Node *) tle->expr) == UNKNOWNOID)
				expr = tle->expr;
			else
			{
				Var		   *var = makeVarFromTargetEntry(nsitem->p_rtindex, tle);

				var->location = exprLocation((Node *) tle->expr);
				expr = (Expr *) var;
			}
			exprList = lappend(exprList, expr);
		}

		/* 为分配到目标表而准备行 */
		exprList = transformInsertRow(pstate, exprList,
									  stmt->cols,
									  icolumns, attrnos,
									  false);
	}
	else if (list_length(selectStmt->valuesLists) > 1)
	{
		/*
		 * 处理带有多个 VALUES 子列表的 INSERT ... VALUES。我们生成一个
		 * 保存已转换表达式列表的 VALUES RTE，并构建一个包含引用该 VALUES
		 * RTE 的 Var 的目标列表。
		 */
		List	   *exprsLists = NIL;
		List	   *coltypes = NIL;
		List	   *coltypmods = NIL;
		List	   *colcollations = NIL;
		int			sublist_length = -1;
		bool		lateral = false;

		Assert(selectStmt->intoClause == NULL);

		foreach(lc, selectStmt->valuesLists)
		{
			List	   *sublist = (List *) lfirst(lc);

			/*
			 * 进行基本的表达式转换（与 ROW() 表达式相同，但允许在
			 * 顶层使用 SetToDefault）
			 */
			sublist = transformExpressionList(pstate, sublist,
											  EXPR_KIND_VALUES, true);

			/*
			 * 所有子列表在经过转换*之后*必须具有相同的长度（转换可能会
			 * 将 '*' 展开为多个项）。VALUES RTE 无法处理长度不同的情况。
			 */
			if (sublist_length < 0)
			{
				/* Remember post-transformation length of first sublist */
				sublist_length = list_length(sublist);
			}
			else if (sublist_length != list_length(sublist))
			{
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("VALUES lists must all be the same length"),
						 parser_errposition(pstate,
											exprLocation((Node *) sublist))));
			}

			/*
			 * 为分配到目标表而准备行。我们会按正常方式处理目标列说明中的
			 * 任何间接引用，但随后会剥离由此产生的字段/数组赋值节点，
			 * 因为我们不希望解析后的语句在每个 VALUES 行中都包含这些节点
			 * 的副本。（不得不像这样一遍又一遍地转换间接引用说明令人
			 * 烦恼，但要避免它需要对 transformAssignmentIndirection 进行
			 * 相当混乱的重构。）
			 */
			sublist = transformInsertRow(pstate, sublist,
										 stmt->cols,
										 icolumns, attrnos,
										 true);

			/*
			 * 我们现在必须分配排序规则，因为 assign_query_collations 不会
			 * 处理范围表项。我们只是在每一行中独立地分配所有排序规则，
			 * 而不关心它们在纵向上是否一致。外层 INSERT 查询不会关心
			 * VALUES 列的排序规则，因此在这里为每个列确定公共排序规则
			 * 并不值得。（但请注意，这确实有一个用户可见的后果：
			 * INSERT ... VALUES 不会抱怨同一列中存在冲突的显式 COLLATE，
			 * 而在其他上下文中使用相同的 VALUES 结构则会抱怨。）
			 */
			assign_list_collations(pstate, sublist);

			exprsLists = lappend(exprsLists, sublist);
		}

		/*
		 * 为 VALUES RTE 构造列类型/typmod/排序规则列表。每一列中的每个
		 * 表达式都已被强制转换为对应目标列或子字段的类型/typmod，因此
		 * 只需查看第一行的 exprType/exprTypmod 即可。我们不关心排序规则
		 * 的标注，因此直接用 InvalidOid 填充。
		 */
		foreach(lc, (List *) linitial(exprsLists))
		{
			Node	   *val = (Node *) lfirst(lc);

			coltypes = lappend_oid(coltypes, exprType(val));
			coltypmods = lappend_int(coltypmods, exprTypmod(val));
			colcollations = lappend_oid(colcollations, InvalidOid);
		}

		/*
		 * 通常表达式列表中不可能出现当前层的 Var，因为 namespace 为空……
		 * 但如果我们在 CREATE RULE 内部，则可能出现 NEW/OLD 引用。在这种
		 * 情况下，我们必须将 VALUES RTE 标记为 LATERAL。
		 */
		if (list_length(pstate->p_rtable) != 1 &&
			contain_vars_of_level((Node *) exprsLists, 0))
			lateral = true;

		/*
		 * 生成 VALUES RTE
		 */
		nsitem = addRangeTableEntryForValues(pstate, exprsLists,
											 coltypes, coltypmods, colcollations,
											 NULL, lateral, true);
		addNSItemToQuery(pstate, nsitem, true, false, false);

		/*
		 * 生成引用该 RTE 的 Var 列表
		 */
		exprList = expandNSItemVars(pstate, nsitem, 0, -1, NULL);

		/*
		 * 将目标列说明中的任何间接引用重新应用到 Var 上
		 */
		exprList = transformInsertRow(pstate, exprList,
									  stmt->cols,
									  icolumns, attrnos,
									  false);
	}
	else
	{
		/*
		 * 处理带有一个 VALUES 子列表的 INSERT ... VALUES。为了效率，我们
		 * 单独处理这种情况。该子列表被直接作为 Query 的目标列表来计算，
		 * 不创建 VALUES RTE。因此它就像一个没有任何 FROM 的 SELECT 那样
		 * 工作。
		 */
		List	   *valuesLists = selectStmt->valuesLists;

		Assert(list_length(valuesLists) == 1);
		Assert(selectStmt->intoClause == NULL);

		/*
		 * Do basic expression transformation (same as a ROW() expr, but allow
		 * SetToDefault at top level)
		 */
		exprList = transformExpressionList(pstate,
										   (List *) linitial(valuesLists),
										   EXPR_KIND_VALUES_SINGLE,
										   true);

		/* 为分配到目标表而准备行 */
		exprList = transformInsertRow(pstate, exprList,
									  stmt->cols,
									  icolumns, attrnos,
									  false);
	}

	/*
	 * 使用计算得到的表达式列表生成查询的目标列表。
	 * 同时，将所有目标列标记为需要插入权限。
	 */
	perminfo = pstate->p_target_nsitem->p_perminfo;
	qry->targetList = NIL;
	Assert(list_length(exprList) <= list_length(icolumns));
	forthree(lc, exprList, icols, icolumns, attnos, attrnos)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		ResTarget  *col = lfirst_node(ResTarget, icols);
		AttrNumber	attr_num = (AttrNumber) lfirst_int(attnos);
		TargetEntry *tle;

		tle = makeTargetEntry(expr,
							  attr_num,
							  col->name,
							  false);
		qry->targetList = lappend(qry->targetList, tle);

		perminfo->insertedCols = bms_add_member(perminfo->insertedCols,
												attr_num - FirstLowInvalidHeapAttributeNumber);
	}

	/*
	 * 如果还有需要处理的子句，则将查询的 namespace 设置为只包含目标关系，
	 * 移除在子 SELECT 或 VALUES 列表中添加的任何条目。
	 */
	if (stmt->onConflictClause || stmt->returningClause)
	{
		pstate->p_namespace = NIL;
		addNSItemToQuery(pstate, pstate->p_target_nsitem,
						 false, true, true);
	}

	/* 处理 ON CONFLICT（如果有的话） */
	if (stmt->onConflictClause)
		qry->onConflict = transformOnConflictClause(pstate,
													stmt->onConflictClause);

	/* 处理 RETURNING（如果有的话） */
	if (stmt->returningClause)
		transformReturningClause(pstate, qry, stmt->returningClause,
								 EXPR_KIND_RETURNING);

	/* 范围表和连接树构建完成 */
	qry->rtable = pstate->p_rtable;
	qry->rteperminfos = pstate->p_rteperminfos;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

	qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
	qry->hasSubLinks = pstate->p_hasSubLinks;

	assign_query_collations(pstate, qry);

	return qry;
}

/*
 * 为分配到目标表而准备一个 INSERT 行。
 *
 * exprlist: 来源值的已转换表达式；这些可能来自一个 VALUES 行，或者是
 * 引用子 SELECT 或 VALUES RTE 输出的 Var。
 * stmtcols: INSERT 的原始目标列说明（我们只是测试其是否为 NIL）
 * icolumns: 有效的目标列说明（ResTarget 列表）
 * attrnos: 整数列号（长度必须与 icolumns 相同）
 * strip_indirection: 如果为真，则移除任何字段/数组赋值节点
 */
List *
transformInsertRow(ParseState *pstate, List *exprlist,
				   List *stmtcols, List *icolumns, List *attrnos,
				   bool strip_indirection)
{
	List	   *result;
	ListCell   *lc;
	ListCell   *icols;
	ListCell   *attnos;

	/*
	 * 检查表达式列表的长度。其表达式数量不能超过目标列的数量。我们允许
	 * 更少，但前提是未给出显式的列列表（剩余列会被隐式赋予默认值）。
	 * 注意，我们必须在转换*之后*才做此检查，因为转换可能会将 '*' 展开为
	 * 多个项。
	 */
	if (list_length(exprlist) > list_length(icolumns))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("INSERT has more expressions than target columns"),
				 parser_errposition(pstate,
									exprLocation(list_nth(exprlist,
														  list_length(icolumns))))));
	if (stmtcols != NIL &&
		list_length(exprlist) < list_length(icolumns))
	{
		/*
		 * 对于像 INSERT ... SELECT (a,b,c) FROM ... 这样的情况，用户可能
		 * 意外地创建了一个 RowExpr 而不是独立的列，从而走到这里。如果
		 * 看起来就是这个问题，就给出合适的提示，因为主要的错误消息在
		 * 这种情况下相当具有误导性。（如果没有 stmtcols，你会得到关于
		 * 数据类型不匹配的消息，其误导性较小，因此我们不担心在这种情况下
		 * 给出提示。）
		 */
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("INSERT has more target columns than expressions"),
				 ((list_length(exprlist) == 1 &&
				   count_rowexpr_columns(pstate, linitial(exprlist)) ==
				   list_length(icolumns)) ?
				  errhint("The insertion source is a row expression containing the same number of columns expected by the INSERT. Did you accidentally use extra parentheses?") : 0),
				 parser_errposition(pstate,
									exprLocation(list_nth(icolumns,
														  list_length(exprlist))))));
	}

	/*
	 * 准备用于分配到目标表的列。
	 */
	result = NIL;
	forthree(lc, exprlist, icols, icolumns, attnos, attrnos)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		ResTarget  *col = lfirst_node(ResTarget, icols);
		int			attno = lfirst_int(attnos);

		expr = transformAssignedExpr(pstate, expr,
									 EXPR_KIND_INSERT_TARGET,
									 col->name,
									 attno,
									 col->indirection,
									 col->location);

		if (strip_indirection)
		{
			/*
			 * 我们需要移除顶层的 FieldStore 和 SubscriptingRef，以及出现在
			 * 其中之一之上的任何 CoerceToDomain——但要保留不是位于其中之一
			 * 之上的 CoerceToDomain。
			 */
			while (expr)
			{
				Expr	   *subexpr = expr;

				while (IsA(subexpr, CoerceToDomain))
				{
					subexpr = ((CoerceToDomain *) subexpr)->arg;
				}
				if (IsA(subexpr, FieldStore))
				{
					FieldStore *fstore = (FieldStore *) subexpr;

					expr = (Expr *) linitial(fstore->newvals);
				}
				else if (IsA(subexpr, SubscriptingRef))
				{
					SubscriptingRef *sbsref = (SubscriptingRef *) subexpr;

					if (sbsref->refassgnexpr == NULL)
						break;

					expr = sbsref->refassgnexpr;
				}
				else
					break;
			}
		}

		result = lappend(result, expr);
	}

	return result;
}

/*
 * transformOnConflictClause -
 *		转换 INSERT 中的 OnConflictClause
 */
static OnConflictExpr *
transformOnConflictClause(ParseState *pstate,
						  OnConflictClause *onConflictClause)
{
	ParseNamespaceItem *exclNSItem = NULL;
	List	   *arbiterElems;
	Node	   *arbiterWhere;
	Oid			arbiterConstraint;
	List	   *onConflictSet = NIL;
	Node	   *onConflictWhere = NULL;
	int			exclRelIndex = 0;
	List	   *exclRelTlist = NIL;
	OnConflictExpr *result;

	/*
	 * 如果这是 ON CONFLICT ... UPDATE，首先为 EXCLUDED 伪关系创建范围表
	 * 项，这样在处理仲裁（arbiter）表达式时它就会存在。（你实际上无法
	 * 从那里引用它，但如果尝试引用，这能提供有用的错误消息。）
	 */
	if (onConflictClause->action == ONCONFLICT_UPDATE)
	{
		Relation	targetrel = pstate->p_target_relation;
		RangeTblEntry *exclRte;

		exclNSItem = addRangeTableEntryForRelation(pstate,
												   targetrel,
												   RowExclusiveLock,
												   makeAlias("excluded", NIL),
												   false, false);
		exclRte = exclNSItem->p_rte;
		exclRelIndex = exclNSItem->p_rtindex;

		/*
		 * 将 relkind 设为 composite，以表明我们处理的不是一个实际的关系，
		 * 因此不需要对其做权限检查。（我们会改为检查实际的目标关系。）
		 */
		exclRte->relkind = RELKIND_COMPOSITE_TYPE;

	/* 创建供 EXPLAIN 使用的 EXCLUDED 关系的目标列表 */
	exclRelTlist = BuildOnConflictExcludedTargetlist(targetrel,
													 exclRelIndex);
	}

	/* 处理仲裁子句，ON CONFLICT ON (...) */
	transformOnConflictArbiter(pstate, onConflictClause, &arbiterElems,
							   &arbiterWhere, &arbiterConstraint);

	/* 处理 DO UPDATE */
	if (onConflictClause->action == ONCONFLICT_UPDATE)
	{
		/*
		 * UPDATE 目标列表中的表达式需要像 UPDATE 而非 INSERT 那样处理。
		 * 我们不需要保存/恢复此状态，因为所有 INSERT 表达式都已被解析。
		 */
		pstate->p_is_insert = false;

		/*
		 * 将 EXCLUDED 伪关系加入查询的 namespace，使其在 UPDATE 子表达式
		 * 中可用。
		 */
		addNSItemToQuery(pstate, exclNSItem, false, true, true);

		/*
		 * 现在转换 UPDATE 子表达式。
		 */
		onConflictSet =
			transformUpdateTargetList(pstate, onConflictClause->targetList);

		onConflictWhere = transformWhereClause(pstate,
											   onConflictClause->whereClause,
											   EXPR_KIND_WHERE, "WHERE");

		/*
		 * 从查询的 namespace 中移除 EXCLUDED 伪关系，因为它不应在 RETURNING
		 * 中可用。（也许将来我们会允许那样，从而去掉这一步。）
		 */
		Assert((ParseNamespaceItem *) llast(pstate->p_namespace) == exclNSItem);
		pstate->p_namespace = list_delete_last(pstate->p_namespace);
	}

	/* 最后，构建 ON CONFLICT DO [NOTHING | UPDATE] 表达式 */
	result = makeNode(OnConflictExpr);

	result->action = onConflictClause->action;
	result->arbiterElems = arbiterElems;
	result->arbiterWhere = arbiterWhere;
	result->constraint = arbiterConstraint;
	result->onConflictSet = onConflictSet;
	result->onConflictWhere = onConflictWhere;
	result->exclRelIndex = exclRelIndex;
	result->exclRelTlist = exclRelTlist;

	return result;
}


/*
 * BuildOnConflictExcludedTargetlist
 *		为 ON CONFLICT 的 EXCLUDED 伪关系创建目标列表，
 *		以 varno exclRelIndex 表示 targetrel 的各列。
 *
 * 注意：导出供重写器（rewriter）使用。
 */
List *
BuildOnConflictExcludedTargetlist(Relation targetrel,
								  Index exclRelIndex)
{
	List	   *result = NIL;
	int			attno;
	Var		   *var;
	TargetEntry *te;

		/*
		 * 注意，tlist 的 resno 必须与底层关系的 attno 对应，因此即便对于
		 * 被删除的列，我们也需要保留条目。
		 */
	for (attno = 0; attno < RelationGetNumberOfAttributes(targetrel); attno++)
	{
		Form_pg_attribute attr = TupleDescAttr(targetrel->rd_att, attno);
		char	   *name;

		if (attr->attisdropped)
		{
			/*
			 * 这里不能使用 atttypid，但 Const 声称是什么类型其实并不重要。
			 */
			var = (Var *) makeNullConst(INT4OID, -1, InvalidOid);
			name = NULL;
		}
		else
		{
			var = makeVar(exclRelIndex, attno + 1,
						  attr->atttypid, attr->atttypmod,
						  attr->attcollation,
						  0);
			name = pstrdup(NameStr(attr->attname));
		}

		te = makeTargetEntry((Expr *) var,
							 attno + 1,
							 name,
							 false);

		result = lappend(result, te);
	}

	/*
	 * 添加一个 whole-row-Var 条目，以支持对 "EXCLUDED.*" 的引用。与
	 * EXCLUDED tlist 中的其他条目一样，它的 resno 必须与 Var 的 varattno
	 * 匹配，否则在 setrefs.c 中解析引用时会发生错误。这违背了目标列表
	 * 的常规约定，但由于我们不把它当作真正的 tlist 使用，所以没关系。
	 */
	var = makeVar(exclRelIndex, InvalidAttrNumber,
				  targetrel->rd_rel->reltype,
				  -1, InvalidOid, 0);
	te = makeTargetEntry((Expr *) var, InvalidAttrNumber, NULL, true);
	result = lappend(result, te);

	return result;
}


/*
 * count_rowexpr_columns -
 *		获取一个 ROW() 表达式所包含的列数；
 *		如果表达式不是 RowExpr 或引用 RowExpr 的 Var，则返回 -1。
 *
 * 目前这仅用于提示（hint）目的，因此我们并不需要严格识别所有可能
 * 的情况。Var 这种情况比较有意思，因为在 INSERT ... SELECT (...) 中
 * 我们就会得到这种形式。
 */
static int
count_rowexpr_columns(ParseState *pstate, Node *expr)
{
	if (expr == NULL)
		return -1;
	if (IsA(expr, RowExpr))
		return list_length(((RowExpr *) expr)->args);
	if (IsA(expr, Var))
	{
		Var		   *var = (Var *) expr;
		AttrNumber	attnum = var->varattno;

		if (attnum > 0 && var->vartype == RECORDOID)
		{
			RangeTblEntry *rte;

			rte = GetRTEByRangeTablePosn(pstate, var->varno, var->varlevelsup);
			if (rte->rtekind == RTE_SUBQUERY)
			{
				/* Subselect-in-FROM: examine sub-select's output expr */
				TargetEntry *ste = get_tle_by_resno(rte->subquery->targetList,
													attnum);

				if (ste == NULL || ste->resjunk)
					return -1;
				expr = (Node *) ste->expr;
				if (IsA(expr, RowExpr))
					return list_length(((RowExpr *) expr)->args);
			}
		}
	}
	return -1;
}


/*
 * transformSelectStmt -
 *		转换一个 SELECT 语句
 *
 * 注意：这里只覆盖没有集合操作且没有 VALUES 列表的情况；
 * 其他情况见下文。
 */
static Query *
transformSelectStmt(ParseState *pstate, SelectStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	Node	   *qual;
	ListCell   *l;

	qry->commandType = CMD_SELECT;

	/* 独立于其他处理，单独处理 WITH 子句 */
	if (stmt->withClause)
	{
		qry->hasRecursive = stmt->withClause->recursive;
		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	/* 如果我们被从不允许 INTO 的地方调用，则报错 */
	if (stmt->intoClause)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("SELECT ... INTO is not allowed here"),
				 parser_errposition(pstate,
									exprLocation((Node *) stmt->intoClause))));

	/* 让 addRangeTableEntry 能够获取 FOR UPDATE/FOR SHARE 信息 */
	pstate->p_locking_clause = stmt->lockingClause;

	/* 让窗口函数也能获取 WINDOW 信息 */
	pstate->p_windowdefs = stmt->windowClause;

	/* 处理 FROM 子句 */
	transformFromClause(pstate, stmt->fromClause);

	/* 转换目标列表 */
	qry->targetList = transformTargetList(pstate, stmt->targetList,
										  EXPR_KIND_SELECT_TARGET);

	/* 标记列的来源 */
	markTargetListOrigins(pstate, qry->targetList);

	/* 转换 WHERE 子句 */
	qual = transformWhereClause(pstate, stmt->whereClause,
								EXPR_KIND_WHERE, "WHERE");

	/* HAVING 子句的初始处理与 WHERE 子句非常相似 */
	qry->havingQual = transformWhereClause(pstate, stmt->havingClause,
										   EXPR_KIND_HAVING, "HAVING");

	/*
	 * Transform sorting/grouping stuff.  Do ORDER BY first because both
	 * transformGroupClause and transformDistinctClause need the results. Note
	 * that these functions can also change the targetList, so it's passed to
	 * them by reference.
	 */
	qry->sortClause = transformSortClause(pstate,
										  stmt->sortClause,
										  &qry->targetList,
										  EXPR_KIND_ORDER_BY,
										  false /* allow SQL92 rules */ );

	qry->groupClause = transformGroupClause(pstate,
											stmt->groupClause,
											&qry->groupingSets,
											&qry->targetList,
											qry->sortClause,
											EXPR_KIND_GROUP_BY,
											false /* allow SQL92 rules */ );
	qry->groupDistinct = stmt->groupDistinct;

	if (stmt->distinctClause == NIL)
	{
		qry->distinctClause = NIL;
		qry->hasDistinctOn = false;
	}
	else if (linitial(stmt->distinctClause) == NULL)
	{
		/* 我们遇到的是 SELECT DISTINCT */
		qry->distinctClause = transformDistinctClause(pstate,
													  &qry->targetList,
													  qry->sortClause,
													  false);
		qry->hasDistinctOn = false;
	}
	else
	{
		/* We had SELECT DISTINCT ON */
		qry->distinctClause = transformDistinctOnClause(pstate,
														stmt->distinctClause,
														&qry->targetList,
														qry->sortClause);
		qry->hasDistinctOn = true;
	}

	/* 转换 LIMIT 子句 */
	qry->limitOffset = transformLimitClause(pstate, stmt->limitOffset,
											EXPR_KIND_OFFSET, "OFFSET",
											stmt->limitOption);
	qry->limitCount = transformLimitClause(pstate, stmt->limitCount,
										   EXPR_KIND_LIMIT, "LIMIT",
										   stmt->limitOption);
	qry->limitOption = stmt->limitOption;

	/* transform window clauses after we have seen all window functions */
	qry->windowClause = transformWindowDefinitions(pstate,
												   pstate->p_windowdefs,
												   &qry->targetList);

	/* 将仍未被解析的输出列视作 text 类型来解析 */
	if (pstate->p_resolve_unknowns)
		resolveTargetListUnknowns(pstate, qry->targetList);

	qry->rtable = pstate->p_rtable;
	qry->rteperminfos = pstate->p_rteperminfos;
	qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
	qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
	qry->hasAggs = pstate->p_hasAggs;

	foreach(l, stmt->lockingClause)
	{
		transformLockingClause(pstate, qry,
							   (LockingClause *) lfirst(l), false);
	}

	assign_query_collations(pstate, qry);

	/* 这必须在处理排序规则之后进行，以保证表达式比较的可靠性 */
	if (pstate->p_hasAggs || qry->groupClause || qry->groupingSets || qry->havingQual)
		parseCheckAggregates(pstate, qry);

	return qry;
}

/*
 * transformValuesClause -
 *		转换一个被用作独立 SELECT 的 VALUES 子句
 *
 * 我们构建一个包含 VALUES RTE 的 Query，就好比用户写了
 *			SELECT * FROM (VALUES ...) AS "*VALUES*"
 */
static Query *
transformValuesClause(ParseState *pstate, SelectStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	List	   *exprsLists = NIL;
	List	   *coltypes = NIL;
	List	   *coltypmods = NIL;
	List	   *colcollations = NIL;
	List	  **colexprs = NULL;
	int			sublist_length = -1;
	bool		lateral = false;
	ParseNamespaceItem *nsitem;
	ListCell   *lc;
	ListCell   *lc2;
	int			i;

	qry->commandType = CMD_SELECT;

	/* 大多数 SELECT 相关处理不适用于 VALUES 子句 */
	Assert(stmt->distinctClause == NIL);
	Assert(stmt->intoClause == NULL);
	Assert(stmt->targetList == NIL);
	Assert(stmt->fromClause == NIL);
	Assert(stmt->whereClause == NULL);
	Assert(stmt->groupClause == NIL);
	Assert(stmt->havingClause == NULL);
	Assert(stmt->windowClause == NIL);
	Assert(stmt->op == SETOP_NONE);

	/* 独立于其他处理，单独处理 WITH 子句 */
	if (stmt->withClause)
	{
		qry->hasRecursive = stmt->withClause->recursive;
		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	/*
	 * 对于 VALUES 的每一行，转换其原始表达式。
	 *
	 * 注意，我们构建的中间表示是按列组织的，而不是按行组织的。这简化了
	 * 下面的类型和排序规则处理。
	 */
	foreach(lc, stmt->valuesLists)
	{
		List	   *sublist = (List *) lfirst(lc);

		/*
		 * 进行基本的表达式转换（与 ROW() 表达式相同，但这里不允许
		 * 使用 SetToDefault）
		 */
		sublist = transformExpressionList(pstate, sublist,
										  EXPR_KIND_VALUES, false);

		/*
		 * 所有子列表在经过转换*之后*必须具有相同的长度（转换可能会将 '*'
		 * 展开为多个项）。VALUES RTE 无法处理长度不同的情况。
		 */
		if (sublist_length < 0)
		{
			/* 记住第一个子列表转换后的长度 */
			sublist_length = list_length(sublist);
			/* 并为每个列的列表分配数组 */
			colexprs = (List **) palloc0(sublist_length * sizeof(List *));
		}
		else if (sublist_length != list_length(sublist))
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("VALUES lists must all be the same length"),
					 parser_errposition(pstate,
										exprLocation((Node *) sublist))));
		}

		/* 为每个列构建表达式列表 */
		i = 0;
		foreach(lc2, sublist)
		{
			Node	   *col = (Node *) lfirst(lc2);

			colexprs[i] = lappend(colexprs[i], col);
			i++;
		}

		/* 释放子列表的单元格以节省内存 */
		list_free(sublist);

		/* 为本行准备一个 exprsLists 元素 */
		exprsLists = lappend(exprsLists, NIL);
	}

	/*
	 * 现在解析各列的公共类型，并将所有内容强制转换为这些类型。然后确定
	 * 每一列的公共 typmod 和公共排序规则（如果有的话）。
	 *
	 * 我们必须现在就处理排序规则，因为：(1) assign_query_collations 不处理
	 * 范围表项，(2) 我们需要用列排序规则标注 VALUES RTE，以供外层查询
	 * 使用。我们不认为隐式排序规则的冲突在这里是错误；相反，该列会直接
	 * 显示 InvalidOid 作为其排序规则，如果这导致无法解析排序规则，稍后会
	 * 失败。
	 *
	 * 注意，我们原地修改了每列的表达式列表。
	 */
	for (i = 0; i < sublist_length; i++)
	{
		Oid			coltype;
		int32		coltypmod;
		Oid			colcoll;

		coltype = select_common_type(pstate, colexprs[i], "VALUES", NULL);

		foreach(lc, colexprs[i])
		{
			Node	   *col = (Node *) lfirst(lc);

			col = coerce_to_common_type(pstate, col, coltype, "VALUES");
			lfirst(lc) = col;
		}

		coltypmod = select_common_typmod(pstate, colexprs[i], coltype);
		colcoll = select_common_collation(pstate, colexprs[i], true);

		coltypes = lappend_oid(coltypes, coltype);
		coltypmods = lappend_int(coltypmods, coltypmod);
		colcollations = lappend_oid(colcollations, colcoll);
	}

	/*
	 * 最后，将强制转换后的表达式重新整理为按行组织的列表。
	 */
	for (i = 0; i < sublist_length; i++)
	{
		forboth(lc, colexprs[i], lc2, exprsLists)
		{
			Node	   *col = (Node *) lfirst(lc);
			List	   *sublist = lfirst(lc2);

			sublist = lappend(sublist, col);
			lfirst(lc2) = sublist;
		}
		list_free(colexprs[i]);
	}

	/*
	 * 通常表达式列表中不可能出现当前层的 Var，因为 namespace 为空……
	 * 但如果我们在 CREATE RULE 内部，则可能出现 NEW/OLD 引用。在这种
	 * 情况下，我们必须将 VALUES RTE 标记为 LATERAL。
	 */
	if (pstate->p_rtable != NIL &&
		contain_vars_of_level((Node *) exprsLists, 0))
		lateral = true;

	/*
	 * 生成 VALUES RTE
	 */
	nsitem = addRangeTableEntryForValues(pstate, exprsLists,
										 coltypes, coltypmods, colcollations,
										 NULL, lateral, true);
	addNSItemToQuery(pstate, nsitem, true, true, true);

	/*
	 * 生成一个目标列表，就好像展开了 "*"
	 */
	Assert(pstate->p_next_resno == 1);
	qry->targetList = expandNSItemAttrs(pstate, nsitem, 0, true, -1);

	/*
	 * 语法允许在 VALUES 上附加 ORDER BY、LIMIT 和 FOR UPDATE，因此需要
	 * 相应处理。
	 */
	qry->sortClause = transformSortClause(pstate,
										  stmt->sortClause,
										  &qry->targetList,
										  EXPR_KIND_ORDER_BY,
										  false /* allow SQL92 rules */ );

	qry->limitOffset = transformLimitClause(pstate, stmt->limitOffset,
											EXPR_KIND_OFFSET, "OFFSET",
											stmt->limitOption);
	qry->limitCount = transformLimitClause(pstate, stmt->limitCount,
										   EXPR_KIND_LIMIT, "LIMIT",
										   stmt->limitOption);
	qry->limitOption = stmt->limitOption;

	if (stmt->lockingClause)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  译者注：%s 是类似 FOR UPDATE 的 SQL 行锁子句 */
				 errmsg("%s cannot be applied to VALUES",
						LCS_asString(((LockingClause *)
									  linitial(stmt->lockingClause))->strength))));

	qry->rtable = pstate->p_rtable;
	qry->rteperminfos = pstate->p_rteperminfos;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

	qry->hasSubLinks = pstate->p_hasSubLinks;

	assign_query_collations(pstate, qry);

	return qry;
}

/*
 * transformSetOperationStmt -
 *		转换一个集合操作树
 *
 * 集合操作树本质上就是一个 SELECT，只是带有 UNION/INTERSECT/EXCEPT
 * 的结构。我们必须转换每个叶子 SELECT，并构建一个顶层 Query，其中
 * 叶子 SELECT 作为子查询包含在其范围表中。集合操作树被转换为顶层
 * Query 的 setOperations 字段。
 */
static Query *
transformSetOperationStmt(ParseState *pstate, SelectStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	SelectStmt *leftmostSelect;
	int			leftmostRTI;
	Query	   *leftmostQuery;
	SetOperationStmt *sostmt;
	List	   *sortClause;
	Node	   *limitOffset;
	Node	   *limitCount;
	List	   *lockingClause;
	WithClause *withClause;
	Node	   *node;
	ListCell   *left_tlist,
			   *lct,
			   *lcm,
			   *lcc,
			   *l;
	List	   *targetvars,
			   *targetnames,
			   *sv_namespace;
	int			sv_rtable_length;
	ParseNamespaceItem *jnsitem;
	ParseNamespaceColumn *sortnscolumns;
	int			sortcolindex;
	int			tllen;

	qry->commandType = CMD_SELECT;

	/*
	 * 找到最左边的叶子 SelectStmt。目前我们需要这样做，仅是为了在
	 * 那里存在 INTO 子句时给出合适的错误消息，这意味着该集合操作树
	 * 处于不允许 INTO 的上下文中。（transformSetOperationTree 反正也会
	 * 报错，但为非最左边的 INTO 抛出不同的错误似乎值得费这个功夫，
	 * 因此我们在 transformSetOperationTree 中生成该错误。）
	 */
	leftmostSelect = stmt->larg;
	while (leftmostSelect && leftmostSelect->op != SETOP_NONE)
		leftmostSelect = leftmostSelect->larg;
	Assert(leftmostSelect && IsA(leftmostSelect, SelectStmt) &&
		   leftmostSelect->larg == NULL);
	if (leftmostSelect->intoClause)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("SELECT ... INTO is not allowed here"),
				 parser_errposition(pstate,
									exprLocation((Node *) leftmostSelect->intoClause))));

	/*
	 * 我们需要在这里提取 ORDER BY 以及其他顶层子句，而不能让
	 * transformSetOperationTree() 看到它们——否则它只会递归回到这里！
	 */
	sortClause = stmt->sortClause;
	limitOffset = stmt->limitOffset;
	limitCount = stmt->limitCount;
	lockingClause = stmt->lockingClause;
	withClause = stmt->withClause;

	stmt->sortClause = NIL;
	stmt->limitOffset = NULL;
	stmt->limitCount = NULL;
	stmt->lockingClause = NIL;
	stmt->withClause = NULL;

	/* We don't support FOR UPDATE/SHARE with set ops at the moment. */
	if (lockingClause)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  译者注：%s 是类似 FOR UPDATE 的 SQL 行锁子句 */
				 errmsg("%s is not allowed with UNION/INTERSECT/EXCEPT",
						LCS_asString(((LockingClause *)
									  linitial(lockingClause))->strength))));

	/* 独立于其他处理，单独处理 WITH 子句 */
	if (withClause)
	{
		qry->hasRecursive = withClause->recursive;
		qry->cteList = transformWithClause(pstate, withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	/*
	 * 以递归方式转换树的各个组成部分。
	 */
	sostmt = castNode(SetOperationStmt,
					  transformSetOperationTree(pstate, stmt, true, NULL));
	Assert(sostmt);
	qry->setOperations = (Node *) sostmt;

	/*
	 * 重新找到最左边的 SELECT（现在它是范围表中的一个子查询）
	 */
	node = sostmt->larg;
	while (node && IsA(node, SetOperationStmt))
		node = ((SetOperationStmt *) node)->larg;
	Assert(node && IsA(node, RangeTblRef));
	leftmostRTI = ((RangeTblRef *) node)->rtindex;
	leftmostQuery = rt_fetch(leftmostRTI, pstate->p_rtable)->subquery;
	Assert(leftmostQuery != NULL);

	/*
	 * 使用最左边 select 的列名，以及最顶层集合操作的公共数据类型/排序规则，
	 * 为外层查询生成一个虚拟目标列表。同时构建虚拟 Var 及其名称的列表，
	 * 供解析 ORDER BY 时使用。
	 *
	 * 注意：我们使用 leftmostRTI 作为虚拟变量的 varno。它们具体拥有哪个
	 * RT 索引并不太要紧，只要该索引对应一个真实的 RT 项即可；否则当树
	 * 被规则重写打乱时，可能会发生奇怪的事情。
	 */
	qry->targetList = NIL;
	targetvars = NIL;
	targetnames = NIL;
	sortnscolumns = (ParseNamespaceColumn *)
		palloc0(list_length(sostmt->colTypes) * sizeof(ParseNamespaceColumn));
	sortcolindex = 0;

	forfour(lct, sostmt->colTypes,
			lcm, sostmt->colTypmods,
			lcc, sostmt->colCollations,
			left_tlist, leftmostQuery->targetList)
	{
		Oid			colType = lfirst_oid(lct);
		int32		colTypmod = lfirst_int(lcm);
		Oid			colCollation = lfirst_oid(lcc);
		TargetEntry *lefttle = (TargetEntry *) lfirst(left_tlist);
		char	   *colName;
		TargetEntry *tle;
		Var		   *var;

		Assert(!lefttle->resjunk);
		colName = pstrdup(lefttle->resname);
		var = makeVar(leftmostRTI,
					  lefttle->resno,
					  colType,
					  colTypmod,
					  colCollation,
					  0);
		var->location = exprLocation((Node *) lefttle->expr);
		tle = makeTargetEntry((Expr *) var,
							  (AttrNumber) pstate->p_next_resno++,
							  colName,
							  false);
		qry->targetList = lappend(qry->targetList, tle);
		targetvars = lappend(targetvars, var);
		targetnames = lappend(targetnames, makeString(colName));
		sortnscolumns[sortcolindex].p_varno = leftmostRTI;
		sortnscolumns[sortcolindex].p_varattno = lefttle->resno;
		sortnscolumns[sortcolindex].p_vartype = colType;
		sortnscolumns[sortcolindex].p_vartypmod = colTypmod;
		sortnscolumns[sortcolindex].p_varcollid = colCollation;
		sortnscolumns[sortcolindex].p_varnosyn = leftmostRTI;
		sortnscolumns[sortcolindex].p_varattnosyn = lefttle->resno;
		sortcolindex++;
	}

	/*
	 * 作为支持使用输出列的表达式作为排序子句的第一步，生成一个命名空间
	 * 项，使输出列可见。Join RTE 节点对此很方便，因为我们可以轻松控制
	 * 匹配时生成的 Var。
	 *
	 * 注意：对于这类情况我们目前还没做任何有用的事，但至少
	 * "ORDER BY upper(foo)" 会给出正确的错误消息，而不是 "foo not found"。
	 */
	sv_rtable_length = list_length(pstate->p_rtable);

	jnsitem = addRangeTableEntryForJoin(pstate,
										targetnames,
										sortnscolumns,
										JOIN_INNER,
										0,
										targetvars,
										NIL,
										NIL,
										NULL,
										NULL,
										false);

	sv_namespace = pstate->p_namespace;
	pstate->p_namespace = NIL;

	/* 仅将 jnsitem 加入列命名空间 */
	addNSItemToQuery(pstate, jnsitem, false, false, true);

	/*
	 * 目前，我们不支持集合操作树输出上的 resjunk 排序子句——你只能使用
	 * SQL92 规范的选项，即按名称或编号选择输出列。通过检查 transformSortClause
	 * 没有向 tlist 添加任何项来强制这一点。注意，如果要修改此行为，
	 * add_setop_child_rel_equivalences() 也需要相应更新。
	 */
	tllen = list_length(qry->targetList);

	qry->sortClause = transformSortClause(pstate,
										  sortClause,
										  &qry->targetList,
										  EXPR_KIND_ORDER_BY,
										  false /* allow SQL92 rules */ );

	/* 恢复命名空间，并从范围表中移除 join RTE */
	pstate->p_namespace = sv_namespace;
	pstate->p_rtable = list_truncate(pstate->p_rtable, sv_rtable_length);

	if (tllen != list_length(qry->targetList))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("invalid UNION/INTERSECT/EXCEPT ORDER BY clause"),
				 errdetail("Only result column names can be used, not expressions or functions."),
				 errhint("Add the expression/function to every SELECT, or move the UNION into a FROM clause."),
				 parser_errposition(pstate,
									exprLocation(list_nth(qry->targetList, tllen)))));

	qry->limitOffset = transformLimitClause(pstate, limitOffset,
											EXPR_KIND_OFFSET, "OFFSET",
											stmt->limitOption);
	qry->limitCount = transformLimitClause(pstate, limitCount,
										   EXPR_KIND_LIMIT, "LIMIT",
										   stmt->limitOption);
	qry->limitOption = stmt->limitOption;

	qry->rtable = pstate->p_rtable;
	qry->rteperminfos = pstate->p_rteperminfos;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
	qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
	qry->hasAggs = pstate->p_hasAggs;

	foreach(l, lockingClause)
	{
		transformLockingClause(pstate, qry,
							   (LockingClause *) lfirst(l), false);
	}

	assign_query_collations(pstate, qry);

	/* 这必须在处理排序规则之后进行，以保证表达式比较的可靠性 */
	if (pstate->p_hasAggs || qry->groupClause || qry->groupingSets || qry->havingQual)
		parseCheckAggregates(pstate, qry);

	return qry;
}

/*
 * 为 SetOperationStmt 的 groupClauses 创建一个 SortGroupClause 节点
 *
 * 如果 require_hash 为真，调用者表示它们需要哈希支持，否则会失败。
 * 因此要更加努力地查找哈希支持。
 */
SortGroupClause *
makeSortGroupClauseForSetOp(Oid rescoltype, bool require_hash)
{
	SortGroupClause *grpcl = makeNode(SortGroupClause);
	Oid			sortop;
	Oid			eqop;
	bool		hashable;

	/* 确定 eqop 和可选的 sortop */
	get_sort_group_operators(rescoltype,
							 false, true, false,
							 &sortop, &eqop, NULL,
							 &hashable);

	/*
	 * 类型缓存不认为 record 是可哈希的（参见
	 * cache_record_field_properties()），但如果调用者确实需要哈希支持，
	 * 我们可以假定它是可哈希的。最坏的情况是，如果 record 的任何组成部分
	 * 不支持哈希，我们会在执行时失败。
	 */
	if (require_hash && (rescoltype == RECORDOID || rescoltype == RECORDARRAYOID))
		hashable = true;

	/* 我们还没有 tlist，因此无法分配 sortgrouprefs */
	grpcl->tleSortGroupRef = 0;
	grpcl->eqop = eqop;
	grpcl->sortop = sortop;
	grpcl->reverse_sort = false;	/* Sort-op is "less than", or InvalidOid */
	grpcl->nulls_first = false; /* OK with or without sortop */
	grpcl->hashable = hashable;

	return grpcl;
}

/*
 * transformSetOperationTree
 *		Recursively transform leaves and internal nodes of a set-op tree
 *
 * In addition to returning the transformed node, if targetlist isn't NULL
 * then we return a list of its non-resjunk TargetEntry nodes.  For a leaf
 * set-op node these are the actual targetlist entries; otherwise they are
 * dummy entries created to carry the type, typmod, collation, and location
 * (for error messages) of each output column of the set-op node.  This info
 * is needed only during the internal recursion of this function, so outside
 * callers pass NULL for targetlist.  Note: the reason for passing the
 * actual targetlist entries of a leaf node is so that upper levels can
 * replace UNKNOWN Consts with properly-coerced constants.
 */
static Node *
transformSetOperationTree(ParseState *pstate, SelectStmt *stmt,
						  bool isTopLevel, List **targetlist)
{
	bool		isLeaf;

	Assert(stmt && IsA(stmt, SelectStmt));

	/* 防止由于过于复杂的集合表达式导致栈溢出 */
	check_stack_depth();

	/*
	 * 对叶子和内部 SELECT 都做合法性检查，排查不允许的操作。
	 */
	if (stmt->intoClause)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("INTO is only allowed on first SELECT of UNION/INTERSECT/EXCEPT"),
				 parser_errposition(pstate,
									exprLocation((Node *) stmt->intoClause))));

	/* We don't support FOR UPDATE/SHARE with set ops at the moment. */
	if (stmt->lockingClause)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  译者注：%s 是类似 FOR UPDATE 的 SQL 行锁子句 */
				 errmsg("%s is not allowed with UNION/INTERSECT/EXCEPT",
						LCS_asString(((LockingClause *)
									  linitial(stmt->lockingClause))->strength))));

	/*
	 * If an internal node of a set-op tree has ORDER BY, LIMIT, FOR UPDATE,
	 * or WITH clauses attached, we need to treat it like a leaf node to
	 * generate an independent sub-Query tree.  Otherwise, it can be
	 * represented by a SetOperationStmt node underneath the parent Query.
	 */
	if (stmt->op == SETOP_NONE)
	{
		Assert(stmt->larg == NULL && stmt->rarg == NULL);
		isLeaf = true;
	}
	else
	{
		Assert(stmt->larg != NULL && stmt->rarg != NULL);
		if (stmt->sortClause || stmt->limitOffset || stmt->limitCount ||
			stmt->lockingClause || stmt->withClause)
			isLeaf = true;
		else
			isLeaf = false;
	}

	if (isLeaf)
	{
		/* 处理叶子 SELECT */
		Query	   *selectQuery;
		char		selectName[32];
		ParseNamespaceItem *nsitem;
		RangeTblRef *rtr;
		ListCell   *tl;

		/*
		 * 将 SelectStmt 转换为 Query。
		 *
		 * 这与通常的 SELECT 转换工作方式相同，只是我们阻止将未知类型的
		 * 输出解析为 TEXT。这不会改变子查询的语义，因为如果列类型在语义
		 * 上很重要，它本来就会被解析为其他类型。这样做可以让我们在下面
		 * 使用 select_common_type() 来解析此类输出。
		 *
		 * 注意：之前转换过的子查询不会影响本子查询的解析，因为它们不在
		 * 顶层 pstate 的 namespace 列表中。
		 */
		selectQuery = parse_sub_analyze((Node *) stmt, pstate,
										NULL, false, false);

		/*
		 * 检查对当前查询层 Var 的非法引用（但上层引用是允许的）。通常这
		 * 不会发生，因为 namespace 会为空，但如果我们在规则（rule）内部，
		 * 则可能发生。
		 */
		if (pstate->p_namespace)
		{
			if (contain_vars_of_level((Node *) selectQuery, 1))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
						 errmsg("UNION/INTERSECT/EXCEPT member statement cannot refer to other relations of same query level"),
						 parser_errposition(pstate,
											locate_var_of_level((Node *) selectQuery, 1))));
		}

		/*
		 * Extract a list of the non-junk TLEs for upper-level processing.
		 */
		if (targetlist)
		{
			*targetlist = NIL;
			foreach(tl, selectQuery->targetList)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(tl);

				if (!tle->resjunk)
					*targetlist = lappend(*targetlist, tle);
			}
		}

		/*
		 * 让该叶子查询成为顶层范围表中的一个子查询。
		 */
		snprintf(selectName, sizeof(selectName), "*SELECT* %d",
				 list_length(pstate->p_rtable) + 1);
		nsitem = addRangeTableEntryForSubquery(pstate,
											   selectQuery,
											   makeAlias(selectName, NIL),
											   false,
											   false);

		/*
		 * Return a RangeTblRef to replace the SelectStmt in the set-op tree.
		 */
		rtr = makeNode(RangeTblRef);
		rtr->rtindex = nsitem->p_rtindex;
		return (Node *) rtr;
	}
	else
	{
		/* 处理一个内部节点（集合操作节点） */
		SetOperationStmt *op = makeNode(SetOperationStmt);
		List	   *ltargetlist;
		List	   *rtargetlist;
		ListCell   *ltl;
		ListCell   *rtl;
		const char *context;
		bool		recursive = (pstate->p_parent_cte &&
								 pstate->p_parent_cte->cterecursive);

		context = (stmt->op == SETOP_UNION ? "UNION" :
				   (stmt->op == SETOP_INTERSECT ? "INTERSECT" :
					"EXCEPT"));

		op->op = stmt->op;
		op->all = stmt->all;

		/*
		 * Recursively transform the left child node.
		 */
		op->larg = transformSetOperationTree(pstate, stmt->larg,
											 false,
											 &ltargetlist);

		/*
		 * 如果我们正在处理一个递归 union 查询，现在正是检查非递归项的输出
		 * 列、并将包含它的 CTE 标记为具有这些结果列的时机。当然，我们只
		 * 应在 CTE 最顶层的 setop 上执行此操作。
		 */
		if (isTopLevel && recursive)
			determineRecursiveColTypes(pstate, op->larg, ltargetlist);

		/*
		 * Recursively transform the right child node.
		 */
		op->rarg = transformSetOperationTree(pstate, stmt->rarg,
											 false,
											 &rtargetlist);

		/*
		 * 验证两个子节点拥有相同数量的非 junk 列，并确定合并后输出列的
		 * 类型。
		 */
		if (list_length(ltargetlist) != list_length(rtargetlist))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("each %s query must have the same number of columns",
							context),
					 parser_errposition(pstate,
										exprLocation((Node *) rtargetlist))));

		if (targetlist)
			*targetlist = NIL;
		op->colTypes = NIL;
		op->colTypmods = NIL;
		op->colCollations = NIL;
		op->groupClauses = NIL;
		forboth(ltl, ltargetlist, rtl, rtargetlist)
		{
			TargetEntry *ltle = (TargetEntry *) lfirst(ltl);
			TargetEntry *rtle = (TargetEntry *) lfirst(rtl);
			Node	   *lcolnode = (Node *) ltle->expr;
			Node	   *rcolnode = (Node *) rtle->expr;
			Oid			lcoltype = exprType(lcolnode);
			Oid			rcoltype = exprType(rcolnode);
			Node	   *bestexpr;
			int			bestlocation;
			Oid			rescoltype;
			int32		rescoltypmod;
			Oid			rescolcoll;

			/* 选择公共类型，与 CASE 等相同 */
			rescoltype = select_common_type(pstate,
											list_make2(lcolnode, rcolnode),
											context,
											&bestexpr);
			bestlocation = exprLocation(bestexpr);

		/*
		 * 验证强制转换确实是可行的。如果不可行，我们迟早也会失败，但
		 * 我们希望现在就失败，因为此时我们有足够的上下文来生成错误光标
		 * 位置。
		 *
		 * 对于所有非 UNKNOWN 类型的情况，我们验证可强制转换性，但不修改
		 * 子节点的表达式，以免改变子查询的语义。
		 *
		 * 如果子节点表达式是一个 UNKNOWN 类型的 Const 或 Param，我们希望
		 * 将其替换为经过强制转换的表达式。这只能发生在子节点是叶子的
		 * 集合操作节点时。替换表达式是安全的，因为如果子查询的语义依赖于
		 * 此输出列的类型，它本来就已经将该 UNKNOWN 强制转换为其他类型了。
		 * 我们这样做是因为：(a) 我们希望验证某个 Const 对目标类型是否有效，
		 * 或者解析 UNKNOWN Param 的实际类型；(b) 我们希望避免子查询的输出
		 * 类型与解析出的目标类型之间出现不必要的差异。这种差异会使得
		 * 规划器中的优化失效。
		 *
		 * 如果是其他某种 UNKNOWN 类型的节点（例如 Var），我们不做任何事
		 * （因为知道 coerce_to_common_type 会失败）。规划器有时能够在
		 * 必须强制转换类型之前，将一个 UNKNOWN Var 折叠为常量，因此现在
		 * 就失败只会破坏那些本可正常工作的情况。
		 */
			if (lcoltype != UNKNOWNOID)
				lcolnode = coerce_to_common_type(pstate, lcolnode,
												 rescoltype, context);
			else if (IsA(lcolnode, Const) ||
					 IsA(lcolnode, Param))
			{
				lcolnode = coerce_to_common_type(pstate, lcolnode,
												 rescoltype, context);
				ltle->expr = (Expr *) lcolnode;
			}

			if (rcoltype != UNKNOWNOID)
				rcolnode = coerce_to_common_type(pstate, rcolnode,
												 rescoltype, context);
			else if (IsA(rcolnode, Const) ||
					 IsA(rcolnode, Param))
			{
				rcolnode = coerce_to_common_type(pstate, rcolnode,
												 rescoltype, context);
				rtle->expr = (Expr *) rcolnode;
			}

			rescoltypmod = select_common_typmod(pstate,
												list_make2(lcolnode, rcolnode),
												rescoltype);

		/*
		 * 选择公共排序规则。所有集合操作符（UNION ALL 除外）都要求有公共
		 * 排序规则；参见 SQL:2008 7.13 <query expression> 语法规则 15c。
		 * （如果我们未能为 UNION ALL 列确定公共排序规则，colCollations
		 * 元素将被设为 InvalidOid，如果更高查询层的某处想使用该列的
		 * 排序规则，可能会导致运行时错误。）
		 */
			rescolcoll = select_common_collation(pstate,
												 list_make2(lcolnode, rcolnode),
												 (op->op == SETOP_UNION && op->all));

			/* 输出结果 */
			op->colTypes = lappend_oid(op->colTypes, rescoltype);
			op->colTypmods = lappend_int(op->colTypmods, rescoltypmod);
			op->colCollations = lappend_oid(op->colCollations, rescolcoll);

			/*
			 * For all cases except UNION ALL, identify the grouping operators
			 * (and, if available, sorting operators) that will be used to
			 * eliminate duplicates.
			 */
			if (op->op != SETOP_UNION || !op->all)
			{
				ParseCallbackState pcbstate;

				setup_parser_errposition_callback(&pcbstate, pstate,
												  bestlocation);

		/*
		 * 如果是递归 union，我们需要要求提供哈希支持。
		 */
				op->groupClauses = lappend(op->groupClauses,
										   makeSortGroupClauseForSetOp(rescoltype, recursive));

				cancel_parser_errposition_callback(&pcbstate);
			}

		/*
		 * 构造一个要返回的虚拟 tlist 项。我们使用 SetToDefault 节点作为
		 * 表达式，因为它恰好携带所需的字段，但其他任何表达式节点类型
		 * 也都可行。
		 */
			if (targetlist)
			{
				SetToDefault *rescolnode = makeNode(SetToDefault);
				TargetEntry *restle;

				rescolnode->typeId = rescoltype;
				rescolnode->typeMod = rescoltypmod;
				rescolnode->collation = rescolcoll;
				rescolnode->location = bestlocation;
				restle = makeTargetEntry((Expr *) rescolnode,
										 0, /* no need to set resno */
										 NULL,
										 false);
				*targetlist = lappend(*targetlist, restle);
			}
		}

		return (Node *) op;
	}
}

/*
 * Process the outputs of the non-recursive term of a recursive union
 * to set up the parent CTE's columns
 */
static void
determineRecursiveColTypes(ParseState *pstate, Node *larg, List *nrtargetlist)
{
	Node	   *node;
	int			leftmostRTI;
	Query	   *leftmostQuery;
	List	   *targetList;
	ListCell   *left_tlist;
	ListCell   *nrtl;
	int			next_resno;

	/*
	 * 找到最左边的叶子 SELECT
	 */
	node = larg;
	while (node && IsA(node, SetOperationStmt))
		node = ((SetOperationStmt *) node)->larg;
	Assert(node && IsA(node, RangeTblRef));
	leftmostRTI = ((RangeTblRef *) node)->rtindex;
	leftmostQuery = rt_fetch(leftmostRTI, pstate->p_rtable)->subquery;
	Assert(leftmostQuery != NULL);

	/*
	 * Generate dummy targetlist using column names of leftmost select and
	 * dummy result expressions of the non-recursive term.
	 */
	targetList = NIL;
	next_resno = 1;

	forboth(nrtl, nrtargetlist, left_tlist, leftmostQuery->targetList)
	{
		TargetEntry *nrtle = (TargetEntry *) lfirst(nrtl);
		TargetEntry *lefttle = (TargetEntry *) lfirst(left_tlist);
		char	   *colName;
		TargetEntry *tle;

		Assert(!lefttle->resjunk);
		colName = pstrdup(lefttle->resname);
		tle = makeTargetEntry(nrtle->expr,
							  next_resno++,
							  colName,
							  false);
		targetList = lappend(targetList, tle);
	}

	/* Now build CTE's output column info using dummy targetlist */
	analyzeCTETargetList(pstate, pstate->p_parent_cte, targetList);
}


/*
 * transformReturnStmt -
 *		转换一个 RETURN 语句
 */
static Query *
transformReturnStmt(ParseState *pstate, ReturnStmt *stmt)
{
	Query	   *qry = makeNode(Query);

	qry->commandType = CMD_SELECT;
	qry->isReturn = true;

	qry->targetList = list_make1(makeTargetEntry((Expr *) transformExpr(pstate, stmt->returnval, EXPR_KIND_SELECT_TARGET),
												 1, NULL, false));

	if (pstate->p_resolve_unknowns)
		resolveTargetListUnknowns(pstate, qry->targetList);
	qry->rtable = pstate->p_rtable;
	qry->rteperminfos = pstate->p_rteperminfos;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);
	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
	qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
	qry->hasAggs = pstate->p_hasAggs;

	assign_query_collations(pstate, qry);

	return qry;
}


/*
 * transformUpdateStmt -
 *	  transforms an update statement
 */
static Query *
transformUpdateStmt(ParseState *pstate, UpdateStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	ParseNamespaceItem *nsitem;
	Node	   *qual;

	qry->commandType = CMD_UPDATE;
	pstate->p_is_insert = false;

	/* 独立于其他处理，单独处理 WITH 子句 */
	if (stmt->withClause)
	{
		qry->hasRecursive = stmt->withClause->recursive;
		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	qry->resultRelation = setTargetTable(pstate, stmt->relation,
										 stmt->relation->inh,
										 true,
										 ACL_UPDATE);
	nsitem = pstate->p_target_nsitem;

	/* 禁止在视图上使用 UPDATE ... WHERE CURRENT OF */
	if (stmt->whereClause &&
		IsA(stmt->whereClause, CurrentOfExpr) &&
		pstate->p_target_relation->rd_rel->relkind == RELKIND_VIEW)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("WHERE CURRENT OF on a view is not implemented"));

	/* subqueries in FROM cannot access the result relation */
	nsitem->p_lateral_only = true;
	nsitem->p_lateral_ok = false;

	/*
	 * FROM 子句并非标准 SQL 语法。我们过去在 POSTQUEL 中能够通过 REPLACE
	 * 实现这一点，因此我们保留了该功能。
	 */
	transformFromClause(pstate, stmt->fromClause);

	/* remaining clauses can reference the result relation normally */
	nsitem->p_lateral_only = false;
	nsitem->p_lateral_ok = true;

	qual = transformWhereClause(pstate, stmt->whereClause,
								EXPR_KIND_WHERE, "WHERE");

	transformReturningClause(pstate, qry, stmt->returningClause,
							 EXPR_KIND_RETURNING);

	/*
	 * Now we are done with SELECT-like processing, and can get on with
	 * transforming the target list to match the UPDATE target columns.
	 */
	qry->targetList = transformUpdateTargetList(pstate, stmt->targetList);

	qry->rtable = pstate->p_rtable;
	qry->rteperminfos = pstate->p_rteperminfos;
	qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

	qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
	qry->hasSubLinks = pstate->p_hasSubLinks;

	assign_query_collations(pstate, qry);

	return qry;
}

/*
 * transformUpdateTargetList -
 *		处理 UPDATE/MERGE/INSERT ... ON CONFLICT UPDATE 中的 SET 子句
 */
List *
transformUpdateTargetList(ParseState *pstate, List *origTlist)
{
	List	   *tlist = NIL;
	RTEPermissionInfo *target_perminfo;
	ListCell   *orig_tl;
	ListCell   *tl;

	tlist = transformTargetList(pstate, origTlist,
								EXPR_KIND_UPDATE_SOURCE);

	/* Prepare to assign non-conflicting resnos to resjunk attributes */
	if (pstate->p_next_resno <= RelationGetNumberOfAttributes(pstate->p_target_relation))
		pstate->p_next_resno = RelationGetNumberOfAttributes(pstate->p_target_relation) + 1;

	/* Prepare non-junk columns for assignment to target table */
	target_perminfo = pstate->p_target_nsitem->p_perminfo;
	orig_tl = list_head(origTlist);

	foreach(tl, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(tl);
		ResTarget  *origTarget;
		int			attrno;

		if (tle->resjunk)
		{
			/*
			 * Resjunk 节点不需要额外的处理，但必须确保它们的 resno 与任何
			 * 目标列都不匹配；否则重写器或规划器可能会混淆。它们也不需要
			 * resname。
			 */
			tle->resno = (AttrNumber) pstate->p_next_resno++;
			tle->resname = NULL;
			continue;
		}
		if (orig_tl == NULL)
			elog(ERROR, "UPDATE target count mismatch --- internal error");
		origTarget = lfirst_node(ResTarget, orig_tl);

		attrno = attnameAttNum(pstate->p_target_relation,
							   origTarget->name, true);
		if (attrno == InvalidAttrNumber)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							origTarget->name,
							RelationGetRelationName(pstate->p_target_relation)),
					 (origTarget->indirection != NIL &&
					  strcmp(origTarget->name, pstate->p_target_nsitem->p_names->aliasname) == 0) ?
					 errhint("SET target columns cannot be qualified with the relation name.") : 0,
					 parser_errposition(pstate, origTarget->location)));

		updateTargetListEntry(pstate, tle, origTarget->name,
							  attrno,
							  origTarget->indirection,
							  origTarget->location);

		/* Mark the target column as requiring update permissions */
		target_perminfo->updatedCols = bms_add_member(target_perminfo->updatedCols,
													  attrno - FirstLowInvalidHeapAttributeNumber);

		orig_tl = lnext(origTlist, orig_tl);
	}
	if (orig_tl != NULL)
		elog(ERROR, "UPDATE target count mismatch --- internal error");

	return tlist;
}

/*
 * addNSItemForReturning -
 *		为 RETURNING 中的 OLD 或 NEW 别名添加一个 ParseNamespaceItem。
 */
static void
addNSItemForReturning(ParseState *pstate, const char *aliasname,
					  VarReturningType returning_type)
{
	List	   *colnames;
	int			numattrs;
	ParseNamespaceColumn *nscolumns;
	ParseNamespaceItem *nsitem;

	/* copy per-column data from the target relation */
	colnames = pstate->p_target_nsitem->p_rte->eref->colnames;
	numattrs = list_length(colnames);

	nscolumns = (ParseNamespaceColumn *)
		palloc(numattrs * sizeof(ParseNamespaceColumn));

	memcpy(nscolumns, pstate->p_target_nsitem->p_nscolumns,
		   numattrs * sizeof(ParseNamespaceColumn));

	/* 将所有列标记为返回 OLD/NEW */
	for (int i = 0; i < numattrs; i++)
		nscolumns[i].p_varreturningtype = returning_type;

	/* 构建 nsitem，大部分字段从目标关系复制 */
	nsitem = (ParseNamespaceItem *) palloc(sizeof(ParseNamespaceItem));
	nsitem->p_names = makeAlias(aliasname, colnames);
	nsitem->p_rte = pstate->p_target_nsitem->p_rte;
	nsitem->p_rtindex = pstate->p_target_nsitem->p_rtindex;
	nsitem->p_perminfo = pstate->p_target_nsitem->p_perminfo;
	nsitem->p_nscolumns = nscolumns;
	nsitem->p_returning_type = returning_type;

	/* add it to the query namespace as a table-only item */
	addNSItemToQuery(pstate, nsitem, false, true, false);
}

/*
 * transformReturningClause -
 *		处理 INSERT/UPDATE/DELETE/MERGE 中的 RETURNING 子句
 */
void
transformReturningClause(ParseState *pstate, Query *qry,
						 ReturningClause *returningClause,
						 ParseExprKind exprKind)
{
	int			save_nslen = list_length(pstate->p_namespace);
	int			save_next_resno;

	if (returningClause == NULL)
		return;					/* nothing to do */

	/*
	 * Scan RETURNING WITH(...) options for OLD/NEW alias names.  Complain if
	 * there is any conflict with existing relations.
	 */
	foreach_node(ReturningOption, option, returningClause->options)
	{
		switch (option->option)
		{
			case RETURNING_OPTION_OLD:
				if (qry->returningOldAlias != NULL)
					ereport(ERROR,
							errcode(ERRCODE_SYNTAX_ERROR),
					/* 译者注：%s 是 OLD 或 NEW */
							errmsg("%s cannot be specified multiple times", "OLD"),
							parser_errposition(pstate, option->location));
				qry->returningOldAlias = option->value;
				break;

			case RETURNING_OPTION_NEW:
				if (qry->returningNewAlias != NULL)
					ereport(ERROR,
							errcode(ERRCODE_SYNTAX_ERROR),
					/* 译者注：%s 是 OLD 或 NEW */
							errmsg("%s cannot be specified multiple times", "NEW"),
							parser_errposition(pstate, option->location));
				qry->returningNewAlias = option->value;
				break;

			default:
				elog(ERROR, "unrecognized returning option: %d", option->option);
		}

		if (refnameNamespaceItem(pstate, NULL, option->value, -1, NULL) != NULL)
			ereport(ERROR,
					errcode(ERRCODE_DUPLICATE_ALIAS),
					errmsg("table name \"%s\" specified more than once",
						   option->value),
					parser_errposition(pstate, option->location));

		addNSItemForReturning(pstate, option->value,
							  option->option == RETURNING_OPTION_OLD ?
							  VAR_RETURNING_OLD : VAR_RETURNING_NEW);
	}

	/*
	 * 如果未显式指定 OLD/NEW 别名，则使用 "old"/"new"，除非被现有关系
	 * 遮蔽。
	 */
	if (qry->returningOldAlias == NULL &&
		refnameNamespaceItem(pstate, NULL, "old", -1, NULL) == NULL)
	{
		qry->returningOldAlias = "old";
		addNSItemForReturning(pstate, "old", VAR_RETURNING_OLD);
	}
	if (qry->returningNewAlias == NULL &&
		refnameNamespaceItem(pstate, NULL, "new", -1, NULL) == NULL)
	{
		qry->returningNewAlias = "new";
		addNSItemForReturning(pstate, "new", VAR_RETURNING_NEW);
	}

	/*
	 * We need to assign resnos starting at one in the RETURNING list. Save
	 * and restore the main tlist's value of p_next_resno, just in case
	 * someone looks at it later (probably won't happen).
	 */
	save_next_resno = pstate->p_next_resno;
	pstate->p_next_resno = 1;

	/* 转换 RETURNING 表达式，方式与 SELECT 目标列表相同 */
	qry->returningList = transformTargetList(pstate,
											 returningClause->exprs,
											 exprKind);

	/*
	 * Complain if the nonempty tlist expanded to nothing (which is possible
	 * if it contains only a star-expansion of a zero-column table).  If we
	 * allow this, the parsed Query will look like it didn't have RETURNING,
	 * with results that would probably surprise the user.
	 */
	if (qry->returningList == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("RETURNING must have at least one column"),
				 parser_errposition(pstate,
									exprLocation(linitial(returningClause->exprs)))));

	/* mark column origins */
	markTargetListOrigins(pstate, qry->returningList);

	/* 将仍未被解析的输出列视作 text 类型来解析 */
	if (pstate->p_resolve_unknowns)
		resolveTargetListUnknowns(pstate, qry->returningList);

	/* 恢复状态 */
	pstate->p_namespace = list_truncate(pstate->p_namespace, save_nslen);
	pstate->p_next_resno = save_next_resno;
}


/*
 * transformPLAssignStmt -
 *		转换一个 PL/pgSQL 赋值语句
 *
 * 如果没有 opt_indirection，转换后的语句看起来像 "SELECT a_expr ..."，
 * 只是表达式已被强制转换为目标的类型。带有间接引用时，它仍然是一个
 * SELECT，但表达式会包含 FieldStore 和/或赋值的 SubscriptingRef 节点，
 * 以计算由目标表示的容器类型变量的新值。该表达式以目标作为容器来源
 * 对其进行引用。
 */
static Query *
transformPLAssignStmt(ParseState *pstate, PLAssignStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	ColumnRef  *cref = makeNode(ColumnRef);
	List	   *indirection = stmt->indirection;
	int			nnames = stmt->nnames;
	SelectStmt *sstmt = stmt->val;
	Node	   *target;
	Oid			targettype;
	int32		targettypmod;
	Oid			targetcollation;
	List	   *tlist;
	TargetEntry *tle;
	Oid			type_id;
	Node	   *qual;
	ListCell   *l;

	/*
	 * First, construct a ColumnRef for the target variable.  If the target
	 * has more than one dotted name, we have to pull the extra names out of
	 * the indirection list.
	 */
	cref->fields = list_make1(makeString(stmt->name));
	cref->location = stmt->location;
	if (nnames > 1)
	{
		/* 避免破坏原始解析树 */
		indirection = list_copy(indirection);
		while (--nnames > 0 && indirection != NIL)
		{
			Node	   *ind = (Node *) linitial(indirection);

			if (!IsA(ind, String))
				elog(ERROR, "invalid name count in PLAssignStmt");
			cref->fields = lappend(cref->fields, ind);
			indirection = list_delete_first(indirection);
		}
	}

	/*
	 * 转换目标引用。通常我们会得到一个 Param 节点，但没有理由对其类型
	 * 过于挑剔。
	 */
	target = transformExpr(pstate, (Node *) cref,
						   EXPR_KIND_UPDATE_TARGET);
	targettype = exprType(target);
	targettypmod = exprTypmod(target);
	targetcollation = exprCollation(target);

	/*
	 * 其余部分大多与 transformSelectStmt 一致，只是我们不需要考虑 WITH 或
	 * INTO，并且我们以自己特有的方式构建目标列表。
	 */
	qry->commandType = CMD_SELECT;
	pstate->p_is_insert = false;

	/* make FOR UPDATE/FOR SHARE info available to addRangeTableEntry */
	pstate->p_locking_clause = sstmt->lockingClause;

	/* make WINDOW info available for window functions, too */
	pstate->p_windowdefs = sstmt->windowClause;

	/* process the FROM clause */
	transformFromClause(pstate, sstmt->fromClause);

	/* 最初像在 SELECT 中那样转换目标列表 */
	tlist = transformTargetList(pstate, sstmt->targetList,
								EXPR_KIND_SELECT_TARGET);

	/* 我们应该恰好有一个目标列表项 */
	if (list_length(tlist) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg_plural("assignment source returned %d column",
							   "assignment source returned %d columns",
							   list_length(tlist),
							   list_length(tlist))));

	tle = linitial_node(TargetEntry, tlist);

	/*
	 * 接下来这部分与 transformAssignedExpr 类似；关键区别在于我们使用
	 * COERCION_PLPGSQL 而不是 COERCION_ASSIGNMENT。
	 */
	type_id = exprType((Node *) tle->expr);

	pstate->p_expr_kind = EXPR_KIND_UPDATE_TARGET;

	if (indirection)
	{
		tle->expr = (Expr *)
			transformAssignmentIndirection(pstate,
										   target,
										   stmt->name,
										   false,
										   targettype,
										   targettypmod,
										   targetcollation,
										   indirection,
										   list_head(indirection),
										   (Node *) tle->expr,
										   COERCION_PLPGSQL,
										   exprLocation(target));
	}
	else if (targettype != type_id &&
			 (targettype == RECORDOID || ISCOMPLEX(targettype)) &&
			 (type_id == RECORDOID || ISCOMPLEX(type_id)))
	{
		/*
		 * 取巧之处：不要让 coerce_to_target_type() 处理不一致的复合类型。
		 * 直接将表达式结果原样传递，让 PL/pgSQL 执行器以自己的方式完成
		 * 转换。这相当蹩脚，但为了向后兼容是必需的。
		 */
	}
	else
	{
		/*
		 * For normal non-qualified target column, do type checking and
		 * coercion.
		 */
		Node	   *orig_expr = (Node *) tle->expr;

		tle->expr = (Expr *)
			coerce_to_target_type(pstate,
								  orig_expr, type_id,
								  targettype, targettypmod,
								  COERCION_PLPGSQL,
								  COERCE_IMPLICIT_CAST,
								  -1);
		/* 在 COERCION_PLPGSQL 下，这个错误大概不可达 */
		if (tle->expr == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("variable \"%s\" is of type %s"
							" but expression is of type %s",
							stmt->name,
							format_type_be(targettype),
							format_type_be(type_id)),
					 errhint("You will need to rewrite or cast the expression."),
					 parser_errposition(pstate, exprLocation(orig_expr))));
	}

	pstate->p_expr_kind = EXPR_KIND_NONE;

	qry->targetList = list_make1(tle);

	/* transform WHERE */
	qual = transformWhereClause(pstate, sstmt->whereClause,
								EXPR_KIND_WHERE, "WHERE");

	/* initial processing of HAVING clause is much like WHERE clause */
	qry->havingQual = transformWhereClause(pstate, sstmt->havingClause,
										   EXPR_KIND_HAVING, "HAVING");

	/*
	 * Transform sorting/grouping stuff.  Do ORDER BY first because both
	 * transformGroupClause and transformDistinctClause need the results. Note
	 * that these functions can also change the targetList, so it's passed to
	 * them by reference.
	 */
	qry->sortClause = transformSortClause(pstate,
										  sstmt->sortClause,
										  &qry->targetList,
										  EXPR_KIND_ORDER_BY,
										  false /* allow SQL92 rules */ );

	qry->groupClause = transformGroupClause(pstate,
											sstmt->groupClause,
											&qry->groupingSets,
											&qry->targetList,
											qry->sortClause,
											EXPR_KIND_GROUP_BY,
											false /* allow SQL92 rules */ );
	qry->groupDistinct = sstmt->groupDistinct;

	if (sstmt->distinctClause == NIL)
	{
		qry->distinctClause = NIL;
		qry->hasDistinctOn = false;
	}
	else if (linitial(sstmt->distinctClause) == NULL)
	{
		/* 我们遇到的是 SELECT DISTINCT */
		qry->distinctClause = transformDistinctClause(pstate,
													  &qry->targetList,
													  qry->sortClause,
													  false);
		qry->hasDistinctOn = false;
	}
	else
	{
		/* We had SELECT DISTINCT ON */
		qry->distinctClause = transformDistinctOnClause(pstate,
														sstmt->distinctClause,
														&qry->targetList,
														qry->sortClause);
		qry->hasDistinctOn = true;
	}

	/* 转换 LIMIT 子句 */
	qry->limitOffset = transformLimitClause(pstate, sstmt->limitOffset,
											EXPR_KIND_OFFSET, "OFFSET",
											sstmt->limitOption);
	qry->limitCount = transformLimitClause(pstate, sstmt->limitCount,
										   EXPR_KIND_LIMIT, "LIMIT",
										   sstmt->limitOption);
	qry->limitOption = sstmt->limitOption;

	/* transform window clauses after we have seen all window functions */
	qry->windowClause = transformWindowDefinitions(pstate,
												   pstate->p_windowdefs,
												   &qry->targetList);

	qry->rtable = pstate->p_rtable;
	qry->rteperminfos = pstate->p_rteperminfos;
	qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
	qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
	qry->hasAggs = pstate->p_hasAggs;

	foreach(l, sstmt->lockingClause)
	{
		transformLockingClause(pstate, qry,
							   (LockingClause *) lfirst(l), false);
	}

	assign_query_collations(pstate, qry);

	/* 这必须在处理排序规则之后进行，以保证表达式比较的可靠性 */
	if (pstate->p_hasAggs || qry->groupClause || qry->groupingSets || qry->havingQual)
		parseCheckAggregates(pstate, qry);

	return qry;
}


/*
 * transformDeclareCursorStmt -
 *		转换一个 DECLARE CURSOR 语句
 *
 * DECLARE CURSOR 与其他实用命令类似，我们将其作为一个 CMD_UTILITY 的
 * Query 节点发出；不过，我们必须先转换其包含的查询。过去我们会将这
 * 一步推迟到执行时进行，但为了确保解析器钩子的副作用在预期的时间发生，
 * 在正常的解析分析阶段完成这一转换确实是必要的。
 */
static Query *
transformDeclareCursorStmt(ParseState *pstate, DeclareCursorStmt *stmt)
{
	Query	   *result;
	Query	   *query;

	if ((stmt->options & CURSOR_OPT_SCROLL) &&
		(stmt->options & CURSOR_OPT_NO_SCROLL))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
		/* 译者注：%s 是一个 SQL 关键字 */
				 errmsg("cannot specify both %s and %s",
						"SCROLL", "NO SCROLL")));

	if ((stmt->options & CURSOR_OPT_ASENSITIVE) &&
		(stmt->options & CURSOR_OPT_INSENSITIVE))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
		/* 译者注：%s 是一个 SQL 关键字 */
				 errmsg("cannot specify both %s and %s",
						"ASENSITIVE", "INSENSITIVE")));

	/* 转换包含的查询，不允许 SELECT INTO */
	query = transformStmt(pstate, stmt->query);
	stmt->query = (Node *) query;

	/* 语法本不应允许除 SELECT 以外的任何内容 */
	if (!IsA(query, Query) ||
		query->commandType != CMD_SELECT)
		elog(ERROR, "unexpected non-SELECT command in DECLARE CURSOR");

	/*
	 * 我们也禁止在游标中使用修改数据的 WITH。（这本来可以允许，但更新
	 * 发生的时机语义可能会令人意外。）
	 */
	if (query->hasModifyingCTE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("DECLARE CURSOR must not contain data-modifying statements in WITH")));

	/* FOR UPDATE and WITH HOLD are not compatible */
	if (query->rowMarks != NIL && (stmt->options & CURSOR_OPT_HOLD))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  译者注：%s 是类似 FOR UPDATE 的 SQL 行锁子句 */
				 errmsg("DECLARE CURSOR WITH HOLD ... %s is not supported",
						LCS_asString(((RowMarkClause *)
									  linitial(query->rowMarks))->strength)),
				 errdetail("Holdable cursors must be READ ONLY.")));

	/* FOR UPDATE and SCROLL are not compatible */
	if (query->rowMarks != NIL && (stmt->options & CURSOR_OPT_SCROLL))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  译者注：%s 是类似 FOR UPDATE 的 SQL 行锁子句 */
				 errmsg("DECLARE SCROLL CURSOR ... %s is not supported",
						LCS_asString(((RowMarkClause *)
									  linitial(query->rowMarks))->strength)),
				 errdetail("Scrollable cursors must be READ ONLY.")));

	/* FOR UPDATE 与 INSENSITIVE 不兼容 */
	if (query->rowMarks != NIL && (stmt->options & CURSOR_OPT_INSENSITIVE))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
		/*------
		  译者注：%s 是类似 FOR UPDATE 的 SQL 行锁子句 */
				 errmsg("DECLARE INSENSITIVE CURSOR ... %s is not valid",
						LCS_asString(((RowMarkClause *)
									  linitial(query->rowMarks))->strength)),
				 errdetail("Insensitive cursors must be READ ONLY.")));

	/* represent the command as a utility Query */
	result = makeNode(Query);
	result->commandType = CMD_UTILITY;
	result->utilityStmt = (Node *) stmt;

	return result;
}


/*
 * transformExplainStmt -
 *	transform an EXPLAIN Statement
 *
 * EXPLAIN is like other utility statements in that we emit it as a
 * CMD_UTILITY Query node; however, we must first transform the contained
 * query.  We used to postpone that until execution, but it's really necessary
 * to do it during the normal parse analysis phase to ensure that side effects
 * of parser hooks happen at the expected time.
 */
static Query *
transformExplainStmt(ParseState *pstate, ExplainStmt *stmt)
{
	Query	   *result;
	bool		generic_plan = false;
	Oid		   *paramTypes = NULL;
	int			numParams = 0;

	/*
	 * 如果我们没有外部的参数定义来源，并且指定了 GENERIC_PLAN 选项，
	 * 则接受可变的参数定义（类似于 PREPARE 等）。
	 */
	if (pstate->p_paramref_hook == NULL)
	{
		ListCell   *lc;

		foreach(lc, stmt->options)
		{
			DefElem    *opt = (DefElem *) lfirst(lc);

			if (strcmp(opt->defname, "generic_plan") == 0)
				generic_plan = defGetBoolean(opt);
			/* don't "break", as we want the last value */
		}
		if (generic_plan)
			setup_parse_variable_parameters(pstate, &paramTypes, &numParams);
	}

	/* transform contained query, allowing SELECT INTO */
	stmt->query = (Node *) transformOptionalSelectInto(pstate, stmt->query);

	/* make sure all is well with parameter types */
	if (generic_plan)
		check_variable_parameters(pstate, (Query *) stmt->query);

	/* represent the command as a utility Query */
	result = makeNode(Query);
	result->commandType = CMD_UTILITY;
	result->utilityStmt = (Node *) stmt;

	return result;
}


/*
 * transformCreateTableAsStmt -
 *		转换一个 CREATE TABLE AS、SELECT ... INTO 或 CREATE MATERIALIZED VIEW
 *		语句
 *
 * 与 DECLARE CURSOR 和 EXPLAIN 一样，现在就转换所包含的语句。
 */
static Query *
transformCreateTableAsStmt(ParseState *pstate, CreateTableAsStmt *stmt)
{
	Query	   *result;
	Query	   *query;

	/* 转换包含的查询，不允许 SELECT INTO */
	query = transformStmt(pstate, stmt->query);
	stmt->query = (Node *) query;

	/* 创建物化视图（MATERIALIZED VIEW）所需的额外工作 */
	if (stmt->objtype == OBJECT_MATVIEW)
	{
		/*
		 * 禁止在用于创建物化视图的查询中使用修改数据的 CTE。如果物化视图
		 * 被刷新或增量维护，用户希望发生什么并不足够清晰。
		 */
		if (query->hasModifyingCTE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("materialized views must not use data-modifying statements in WITH")));

		/*
		 * 检查创建查询中是否使用了任何临时数据库对象。如果来源消失了，
		 * 将很难刷新数据或增量维护它。
		 */
		if (isQueryUsingTempRelation(query))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("materialized views must not use temporary tables or views")));

		/*
		 * 物化视图要么需要保存参数供维护/加载数据使用，要么完全禁止参数。
		 * 后者似乎更安全、更合理。
		 */
		if (query_contains_extern_params(query))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("materialized views may not be defined using bound parameters")));

		/*
		 * 目前，我们禁止未记录的（unlogged）物化视图，因为让它们在崩溃后
		 * 直接变为空似乎是个坏主意。（如果我们能将它们标记为未填充，那
		 * 会更好，但这需要崩溃恢复目前无法处理的目录变更。）
		 */
		if (stmt->into->rel->relpersistence == RELPERSISTENCE_UNLOGGED)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("materialized views cannot be unlogged")));

		/*
		 * 在运行时，我们需要一份已解析但未经重写的 Query 副本，用于创建
		 * 视图的 ON SELECT 规则。我们将其暂存在 IntoClause 中，因为那是
		 * intorel_startup() 可以方便地从中获取它的地方。
		 */
		stmt->into->viewQuery = copyObject(query);
	}

	/* represent the command as a utility Query */
	result = makeNode(Query);
	result->commandType = CMD_UTILITY;
	result->utilityStmt = (Node *) stmt;

	return result;
}

/*
 * 转换一个 CallStmt
 */
static Query *
transformCallStmt(ParseState *pstate, CallStmt *stmt)
{
	List	   *targs;
	ListCell   *lc;
	Node	   *node;
	FuncExpr   *fexpr;
	HeapTuple	proctup;
	Datum		proargmodes;
	bool		isNull;
	List	   *outargs = NIL;
	Query	   *result;

	/*
	 * 首先，对过程调用及其参数做标准的解析分析，使我们能够识别出被
	 * 调用的过程。
	 */
	targs = NIL;
	foreach(lc, stmt->funccall->args)
	{
		targs = lappend(targs, transformExpr(pstate,
											 (Node *) lfirst(lc),
											 EXPR_KIND_CALL_ARGUMENT));
	}

	node = ParseFuncOrColumn(pstate,
							 stmt->funccall->funcname,
							 targs,
							 pstate->p_last_srf,
							 stmt->funccall,
							 true,
							 stmt->funccall->location);

	assign_expr_collations(pstate, node);

	fexpr = castNode(FuncExpr, node);

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(fexpr->funcid));
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup failed for function %u", fexpr->funcid);

	/*
	 * 展开参数列表，以处理命名参数表示法和默认参数。对于普通的 FuncExpr，
	 * 这会在规划阶段完成，但 CallStmt 不会经过规划，而且似乎没有理由不
	 * 在这里完成它。
	 */
	fexpr->args = expand_function_arguments(fexpr->args,
											true,
											fexpr->funcresulttype,
											proctup);

	/* 获取 proargmodes；如果为空，则没有输出参数 */
	proargmodes = SysCacheGetAttr(PROCOID, proctup,
								  Anum_pg_proc_proargmodes,
								  &isNull);
	if (!isNull)
	{
		/*
		 * Split the list into input arguments in fexpr->args and output
		 * arguments in stmt->outargs.  INOUT arguments appear in both lists.
		 */
		ArrayType  *arr;
		int			numargs;
		char	   *argmodes;
		List	   *inargs;
		int			i;

		arr = DatumGetArrayTypeP(proargmodes);	/* ensure not toasted */
		numargs = list_length(fexpr->args);
		if (ARR_NDIM(arr) != 1 ||
			ARR_DIMS(arr)[0] != numargs ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != CHAROID)
			elog(ERROR, "proargmodes is not a 1-D char array of length %d or it contains nulls",
				 numargs);
		argmodes = (char *) ARR_DATA_PTR(arr);

		inargs = NIL;
		i = 0;
		foreach(lc, fexpr->args)
		{
			Node	   *n = lfirst(lc);

			switch (argmodes[i])
			{
				case PROARGMODE_IN:
				case PROARGMODE_VARIADIC:
					inargs = lappend(inargs, n);
					break;
				case PROARGMODE_OUT:
					outargs = lappend(outargs, n);
					break;
				case PROARGMODE_INOUT:
					inargs = lappend(inargs, n);
					outargs = lappend(outargs, copyObject(n));
					break;
				default:
				/* 注意我们不支持 PROARGMODE_TABLE */
				elog(ERROR, "invalid argmode %c for procedure",
						 argmodes[i]);
					break;
			}
			i++;
		}
		fexpr->args = inargs;
	}

	stmt->funcexpr = fexpr;
	stmt->outargs = outargs;

	ReleaseSysCache(proctup);

	/* represent the command as a utility Query */
	result = makeNode(Query);
	result->commandType = CMD_UTILITY;
	result->utilityStmt = (Node *) stmt;

	return result;
}

/*
 * Produce a string representation of a LockClauseStrength value.
 * This should only be applied to valid values (not LCS_NONE).
 */
const char *
LCS_asString(LockClauseStrength strength)
{
	switch (strength)
	{
		case LCS_NONE:
			Assert(false);
			break;
		case LCS_FORKEYSHARE:
			return "FOR KEY SHARE";
		case LCS_FORSHARE:
			return "FOR SHARE";
		case LCS_FORNOKEYUPDATE:
			return "FOR NO KEY UPDATE";
		case LCS_FORUPDATE:
			return "FOR UPDATE";
	}
	return "FOR some";			/* shouldn't happen */
}

/*
 * 检查与 FOR [KEY] UPDATE/SHARE 不兼容的特性。
 *
 * 导出此函数，以便规划器在重写、查询上拉（pullup）等操作之后再次检查。
 */
void
CheckSelectLocking(Query *qry, LockClauseStrength strength)
{
	Assert(strength != LCS_NONE);	/* else caller error */

	if (qry->setOperations)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  译者注：%s 是类似 FOR UPDATE 的 SQL 行锁子句 */
				 errmsg("%s is not allowed with UNION/INTERSECT/EXCEPT",
						LCS_asString(strength))));
	if (qry->distinctClause != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  译者注：%s 是类似 FOR UPDATE 的 SQL 行锁子句 */
				 errmsg("%s is not allowed with DISTINCT clause",
						LCS_asString(strength))));
	if (qry->groupClause != NIL || qry->groupingSets != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  译者注：%s 是类似 FOR UPDATE 的 SQL 行锁子句 */
				 errmsg("%s is not allowed with GROUP BY clause",
						LCS_asString(strength))));
	if (qry->havingQual != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  译者注：%s 是类似 FOR UPDATE 的 SQL 行锁子句 */
				 errmsg("%s is not allowed with HAVING clause",
						LCS_asString(strength))));
	if (qry->hasAggs)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  译者注：%s 是类似 FOR UPDATE 的 SQL 行锁子句 */
				 errmsg("%s is not allowed with aggregate functions",
						LCS_asString(strength))));
	if (qry->hasWindowFuncs)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  译者注：%s 是类似 FOR UPDATE 的 SQL 行锁子句 */
				 errmsg("%s is not allowed with window functions",
						LCS_asString(strength))));
	if (qry->hasTargetSRFs)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  译者注：%s 是类似 FOR UPDATE 的 SQL 行锁子句 */
				 errmsg("%s is not allowed with set-returning functions in the target list",
						LCS_asString(strength))));
}

/*
 * Transform a FOR [KEY] UPDATE/SHARE clause
 *
 * This basically involves replacing names by integer relids.
 *
 * NB: if you need to change this, see also markQueryForLocking()
 * in rewriteHandler.c, and isLockedRefname() in parse_relation.c.
 */
static void
transformLockingClause(ParseState *pstate, Query *qry, LockingClause *lc,
					   bool pushedDown)
{
	List	   *lockedRels = lc->lockedRels;
	ListCell   *l;
	ListCell   *rt;
	Index		i;
	LockingClause *allrels;

	CheckSelectLocking(qry, lc->strength);

	/* 构造一个可以下传给子查询以选择所有关系的子句 */
	allrels = makeNode(LockingClause);
	allrels->lockedRels = NIL;	/* indicates all rels */
	allrels->strength = lc->strength;
	allrels->waitPolicy = lc->waitPolicy;

	if (lockedRels == NIL)
	{
		/*
		 * 锁定查询及其子查询中使用的所有常规表。我们检查 inFromCl 以排除
		 * 自动添加的 RTE，尤其是规则中的 NEW/OLD。这有点滥用一个基本已过时的
		 * 标志，但很方便。我们不能依赖已在很大程度上取代 inFromCl 的命名空间
		 * 机制，例如，即便基础关系 RTE 被上层连接遮蔽，我们仍需要锁定它们。
		 */
		i = 0;
		foreach(rt, qry->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);

			++i;
			if (!rte->inFromCl)
				continue;
			switch (rte->rtekind)
			{
				case RTE_RELATION:
					{
						RTEPermissionInfo *perminfo;

						applyLockingClause(qry, i,
										   lc->strength,
										   lc->waitPolicy,
										   pushedDown);
						perminfo = getRTEPermissionInfo(qry->rteperminfos, rte);
						perminfo->requiredPerms |= ACL_SELECT_FOR_UPDATE;
					}
					break;
				case RTE_SUBQUERY:
					applyLockingClause(qry, i, lc->strength, lc->waitPolicy,
									   pushedDown);

					/*
					 * FOR UPDATE/SHARE of subquery is propagated to all of
					 * subquery's rels, too.  We could do this later (based on
					 * the marking of the subquery RTE) but it is convenient
					 * to have local knowledge in each query level about which
					 * rels need to be opened with RowShareLock.
					 */
					transformLockingClause(pstate, rte->subquery,
										   allrels, true);
					break;
				default:
				/* 忽略 JOIN、SPECIAL、FUNCTION、VALUES、CTE 类型的 RTE */
				break;
			}
		}
	}
	else
	{
		/*
		 * Lock just the named tables.  As above, we allow locking any base
		 * relation regardless of alias-visibility rules, so we need to
		 * examine inFromCl to exclude OLD/NEW.
		 */
		foreach(l, lockedRels)
		{
			RangeVar   *thisrel = (RangeVar *) lfirst(l);

			/* 为简单起见，这里坚持要求使用非限定（unqualified）的别名 */
			if (thisrel->catalogname || thisrel->schemaname)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
				/*------
				  translator: %s is a SQL row locking clause such as FOR UPDATE */
						 errmsg("%s must specify unqualified relation names",
								LCS_asString(lc->strength)),
						 parser_errposition(pstate, thisrel->location)));

			i = 0;
			foreach(rt, qry->rtable)
			{
				RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);
				char	   *rtename = rte->eref->aliasname;

				++i;
				if (!rte->inFromCl)
					continue;

				/*
				 * 没有别名的 join RTE 不会作为关系名可见，需要跳过（否则
				 * 它可能会遮蔽同名的基础关系），除非它带有 USING 别名，
				 * 而 USING 别名*是*可见的。
				 *
				 * 没有别名的子查询和 VALUES RTE 永远不会作为关系名可见，
				 * 必须始终跳过。
				 */
				if (rte->alias == NULL)
				{
					if (rte->rtekind == RTE_JOIN)
					{
						if (rte->join_using_alias == NULL)
							continue;
						rtename = rte->join_using_alias->aliasname;
					}
					else if (rte->rtekind == RTE_SUBQUERY ||
							 rte->rtekind == RTE_VALUES)
						continue;
				}

				if (strcmp(rtename, thisrel->relname) == 0)
				{
					switch (rte->rtekind)
					{
						case RTE_RELATION:
							{
								RTEPermissionInfo *perminfo;

								applyLockingClause(qry, i,
												   lc->strength,
												   lc->waitPolicy,
												   pushedDown);
								perminfo = getRTEPermissionInfo(qry->rteperminfos, rte);
								perminfo->requiredPerms |= ACL_SELECT_FOR_UPDATE;
							}
							break;
						case RTE_SUBQUERY:
							applyLockingClause(qry, i, lc->strength,
											   lc->waitPolicy, pushedDown);
							/* see comment above */
							transformLockingClause(pstate, rte->subquery,
												   allrels, true);
							break;
						case RTE_JOIN:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							/*------
							  translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to a join",
											LCS_asString(lc->strength)),
									 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_FUNCTION:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							/*------
							  translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to a function",
											LCS_asString(lc->strength)),
									 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_TABLEFUNC:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							/*------
							  translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to a table function",
											LCS_asString(lc->strength)),
									 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_VALUES:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							/*------
							  translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to VALUES",
											LCS_asString(lc->strength)),
									 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_CTE:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							/*------
							  translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to a WITH query",
											LCS_asString(lc->strength)),
									 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_NAMEDTUPLESTORE:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							/*------
							  translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to a named tuplestore",
											LCS_asString(lc->strength)),
									 parser_errposition(pstate, thisrel->location)));
							break;

							/* Shouldn't be possible to see RTE_RESULT here */

						default:
							elog(ERROR, "unrecognized RTE type: %d",
								 (int) rte->rtekind);
							break;
					}
					break;		/* out of foreach loop */
				}
			}
			if (rt == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_TABLE),
				/*------
				  translator: %s is a SQL row locking clause such as FOR UPDATE */
						 errmsg("relation \"%s\" in %s clause not found in FROM clause",
								thisrel->relname,
								LCS_asString(lc->strength)),
						 parser_errposition(pstate, thisrel->location)));
		}
	}
}

/*
 * 为单个范围表项记录锁定信息
 */
void
applyLockingClause(Query *qry, Index rtindex,
				   LockClauseStrength strength, LockWaitPolicy waitPolicy,
				   bool pushedDown)
{
	RowMarkClause *rc;

	Assert(strength != LCS_NONE);	/* else caller error */

	/* 如果是显式子句，确保设置 hasForUpdate */
	if (!pushedDown)
		qry->hasForUpdate = true;

	/* 检查是否已有相同 rtindex 的已有项 */
	if ((rc = get_parse_rowmark(qry, rtindex)) != NULL)
	{
		/*
		 * If the same RTE is specified with more than one locking strength,
		 * use the strongest.  (Reasonable, since you can't take both a shared
		 * and exclusive lock at the same time; it'll end up being exclusive
		 * anyway.)
		 *
		 * Similarly, if the same RTE is specified with more than one lock
		 * wait policy, consider that NOWAIT wins over SKIP LOCKED, which in
		 * turn wins over waiting for the lock (the default).  This is a bit
		 * more debatable but raising an error doesn't seem helpful. (Consider
		 * for instance SELECT FOR UPDATE NOWAIT from a view that internally
		 * contains a plain FOR UPDATE spec.)  Having NOWAIT win over SKIP
		 * LOCKED is reasonable since the former throws an error in case of
		 * coming across a locked tuple, which may be undesirable in some
		 * cases but it seems better than silently returning inconsistent
		 * results.
		 *
		 * And of course pushedDown becomes false if any clause is explicit.
		 */
		rc->strength = Max(rc->strength, strength);
		rc->waitPolicy = Max(rc->waitPolicy, waitPolicy);
		rc->pushedDown &= pushedDown;
		return;
	}

	/* 创建一个新的 RowMarkClause */
	rc = makeNode(RowMarkClause);
	rc->rti = rtindex;
	rc->strength = strength;
	rc->waitPolicy = waitPolicy;
	rc->pushedDown = pushedDown;
	qry->rowMarks = lappend(qry->rowMarks, rc);
}

#ifdef DEBUG_NODE_TESTS_ENABLED
/*
 * Coverage testing for raw_expression_tree_walker().
 *
 * 启用时，我们会对提交给解析分析的每个 DML 语句运行
 * submitted to parse analysis.  Without this provision, that function is only
 * applied in limited cases involving CTEs, and we don't really want to have
 * 有限情况下被应用，而我们并不真的想同时测试 CTE 内部和外部的所有内容。
 */
static bool
test_raw_expression_coverage(Node *node, void *context)
{
	if (node == NULL)
		return false;
	return raw_expression_tree_walker(node,
									  test_raw_expression_coverage,
									  context);
}
#endif							/* DEBUG_NODE_TESTS_ENABLED */
