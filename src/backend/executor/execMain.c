/*-------------------------------------------------------------------------
 *
 * execMain.c
 *	  顶层执行器接口例程
 *
 * 接口例程（INTERFACE ROUTINES）
 *	ExecutorStart()
 *	ExecutorRun()
 *	ExecutorFinish()
 *	ExecutorEnd()
 *
 *	这四个过程是执行器对外的外部接口。
 *	在每种情况下，查询描述符（QueryDesc）都作为必需参数传入。
 *
 *	ExecutorStart 必须在任何查询计划的执行开始时调用，
 *	而 ExecutorEnd 必须始终在计划执行结束时调用（除非因错误而中止）。
 *
 *	ExecutorRun 接受 direction 与 count 参数，用于指定计划是向前、
 *	向后执行，以及执行多少个元组。在某些情况下，ExecutorRun 可能会被
 *	多次调用来处理某个计划的全部元组。也可以在执行完整个计划之前
 *	提前停止（但仅限于 SELECT）。
 *
 *	ExecutorFinish 必须在最后一次 ExecutorRun 调用之后、ExecutorEnd
 *	之前调用。仅在使用 EXPLAIN 时可以省略它，此时也应一并省略
 *	ExecutorRun。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execMain.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/partition.h"
#include "commands/matview.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "executor/execPartition.h"
#include "executor/nodeSubplan.h"
#include "foreign/fdwapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/queryjumble.h"
#include "parser/parse_relation.h"
#include "pgstat.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/backend_status.h"
#include "utils/lsyscache.h"
#include "utils/partcache.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"


/* 供插件在 ExecutorStart/Run/Finish/End 中获取控制权的钩子 */
ExecutorStart_hook_type ExecutorStart_hook = NULL;
ExecutorRun_hook_type ExecutorRun_hook = NULL;
ExecutorFinish_hook_type ExecutorFinish_hook = NULL;
ExecutorEnd_hook_type ExecutorEnd_hook = NULL;

/* 供插件在 ExecCheckPermissions() 中获取控制权的钩子 */
ExecutorCheckPerms_hook_type ExecutorCheckPerms_hook = NULL;

/* 仅在本模块内使用的局部例程声明 */
static void InitPlan(QueryDesc *queryDesc, int eflags);
static void CheckValidRowMarkRel(Relation rel, RowMarkType markType);
static void ExecPostprocessPlan(EState *estate);
static void ExecEndPlan(PlanState *planstate, EState *estate);
static void ExecutePlan(QueryDesc *queryDesc,
						CmdType operation,
						bool sendTuples,
						uint64 numberTuples,
						ScanDirection direction,
						DestReceiver *dest);
static bool ExecCheckPermissionsModified(Oid relOid, Oid userid,
										 Bitmapset *modifiedCols,
										 AclMode requiredPerms);
static void ExecCheckXactReadOnly(PlannedStmt *plannedstmt);
static void EvalPlanQualStart(EPQState *epqstate, Plan *planTree);
static void ReportNotNullViolationError(ResultRelInfo *resultRelInfo,
										TupleTableSlot *slot,
										EState *estate, int attnum);

/* 局部声明结束 */


/* ----------------------------------------------------------------
 *		ExecutorStart
 *
 *		本例程必须在任何查询计划的任何一次执行开始时调用
 *
 * 接受一个先前由 CreateQueryDesc 创建的 QueryDesc（之所以把它分开，
 * 只是因为有些地方会把 QueryDescs 用于工具类命令）。QueryDesc 的
 * tupDesc 字段会被填入，以描述将要返回的元组，而内部字段
 * （estate 与 planstate）也会被设置好。
 *
 * eflags 包含 executor.h 中所描述的标志位。
 *
 * 注意：调用本函数时的 CurrentMemoryContext 将成为本次执行器调用
 * 所使用的每查询上下文的父上下文。
 *
 * 我们提供了一个函数钩子变量，让可加载的插件能够在 ExecutorStart 被调用时
 * 获取控制权。这样的插件通常会调用 standard_ExecutorStart()。
 *
 * ----------------------------------------------------------------
 */
void
ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	/*
	 * 在某些情况下（例如 EXECUTE 语句，或使用扩展查询协议发送的执行消息），
	 * query_id 不会被报告，因此现在补报。
	 *
	 * 注意：多次报告 query_id 是无害的，因为如果顶层 query_id 已经被报告过，
	 * 该调用将被忽略。
	 */
	pgstat_report_query_id(queryDesc->plannedstmt->queryId, false);

	if (ExecutorStart_hook)
		(*ExecutorStart_hook) (queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

void
standard_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	EState	   *estate;
	MemoryContext oldcontext;

	/* 健全性检查：queryDesc 必须尚未被启动 */
	Assert(queryDesc != NULL);
	Assert(queryDesc->estate == NULL);

	/* 调用方必须确保查询的快照处于活动状态 */
	Assert(GetActiveSnapshot() == queryDesc->snapshot);

	/*
	 * 如果事务是只读的，我们需要检查是否计划向非临时表写入数据。
	 * EXPLAIN 被视为只读。
	 *
	 * 不允许在并行模式下写入。支持 UPDATE 和 DELETE 需要 (a) 把 combo CID
	 * 哈希存储到共享内存中（而不是仅在并行启动前同步一次），以及 (b) 一种
	 * 替代 heap_update() 依赖 xmax 实现互斥的方案。INSERT 或许没有这些麻烦，
	 * 但为了简化检查我们一并禁止它。
	 *
	 * 我们在 CommandCounterIncrement 及其它地方还有更低级别的防御，
	 * 防止在并行模式下执行不安全的操作，但此处能给出更友好的错误消息。
	 */
	if ((XactReadOnly || IsInParallelMode()) &&
		!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
		ExecCheckXactReadOnly(queryDesc->plannedstmt);

	/*
	 * 构建 EState，切换至每查询内存上下文以便启动。
	 */
	estate = CreateExecutorState();
	queryDesc->estate = estate;

	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	/*
	 * 若有的话，从 queryDesc 中填入外部参数；并为内部参数分配工作空间
	 */
	estate->es_param_list_info = queryDesc->params;

	if (queryDesc->plannedstmt->paramExecTypes != NIL)
	{
		int			nParamExec;

		nParamExec = list_length(queryDesc->plannedstmt->paramExecTypes);
		estate->es_param_exec_vals = (ParamExecData *)
			palloc0(nParamExec * sizeof(ParamExecData));
	}

	/* 我们现在要求所有调用方都提供 sourceText */
	Assert(queryDesc->sourceText != NULL);
	estate->es_sourceText = queryDesc->sourceText;

	/*
	 * 若有的话，从 queryDesc 中填入查询环境。
	 */
	estate->es_queryEnv = queryDesc->queryEnv;

	/*
	 * 如果非只读查询，设置命令 ID 以标记输出元组
	 */
	switch (queryDesc->operation)
	{
		case CMD_SELECT:

			/*
			 * SELECT FOR [KEY] UPDATE/SHARE 以及修改型 CTE 需要标记元组
			 */
			if (queryDesc->plannedstmt->rowMarks != NIL ||
				queryDesc->plannedstmt->hasModifyingCTE)
				estate->es_output_cid = GetCurrentCommandId(true);

			/*
			 * 不带修改型 CTE 的 SELECT 不可能排队触发器，
			 * 因此强制进入跳过触发器的模式。这只是一个边际效率上的小技巧，
			 * 毕竟 AfterTriggerBeginQuery/AfterTriggerEndQuery 开销并不大，
			 * 但我们不妨这么做。
			 */
			if (!queryDesc->plannedstmt->hasModifyingCTE)
				eflags |= EXEC_FLAG_SKIP_TRIGGERS;
			break;

		case CMD_INSERT:
		case CMD_DELETE:
		case CMD_UPDATE:
		case CMD_MERGE:
			estate->es_output_cid = GetCurrentCommandId(true);
			break;

		default:
			elog(ERROR, "unrecognized operation code: %d",
				 (int) queryDesc->operation);
			break;
	}

	/*
	 * 将其他重要信息复制到 EState 中
	 */
	estate->es_snapshot = RegisterSnapshot(queryDesc->snapshot);
	estate->es_crosscheck_snapshot = RegisterSnapshot(queryDesc->crosscheck_snapshot);
	estate->es_top_eflags = eflags;
	estate->es_instrument = queryDesc->instrument_options;
	estate->es_jit_flags = queryDesc->plannedstmt->jitFlags;

	/*
	 * 建立 AFTER 触发器的语句上下文，除非被告知不要建立，
	 * 或者处于 EXPLAIN-only 模式（此时不会调用 ExecutorFinish）。
	 */
	if (!(eflags & (EXEC_FLAG_SKIP_TRIGGERS | EXEC_FLAG_EXPLAIN_ONLY)))
		AfterTriggerBeginQuery();

	/*
	 * 初始化计划状态树
	 */
	InitPlan(queryDesc, eflags);

	MemoryContextSwitchTo(oldcontext);
}

/* ----------------------------------------------------------------
 *		ExecutorRun
 *
 *		这是执行器模块的主例程。它从交通警（traffic cop）处接收查询描述符，
 *		并执行查询计划。
 *
 *		ExecutorStart 必须已经被调用过。
 *
 *		如果 direction 为 NoMovementScanDirection，则除了启动/关闭目标之外
 *		不执行任何操作。否则，我们按指定方向抓取最多 'count' 个元组。
 *
 *		注意：count = 0 被解释为没有游标限制，即运行至完成。还要注意，
 *		count 限制只应用于抓取到的元组，而不应用于例如由 ModifyTable
 *		计划节点插入/更新/删除的那些元组。
 *
 *		它没有返回值，但输出元组（若有）会被发送到 QueryDesc 中指定的
 *		目标接收器；在顶层处理的元组数可以在 estate->es_processed 中找到。
 *		在所有 ExecutorRun 调用中处理的元组总数可以在 estate->es_total_processed
 *		中找到。
 *
 *		我们提供一个函数钩子变量，让可加载插件在 ExecutorRun 被调用时
 *		获取控制权。这样的插件通常会调用 standard_ExecutorRun()。
 *
 * ----------------------------------------------------------------
 */
void
ExecutorRun(QueryDesc *queryDesc,
			ScanDirection direction, uint64 count)
{
	if (ExecutorRun_hook)
		(*ExecutorRun_hook) (queryDesc, direction, count);
	else
		standard_ExecutorRun(queryDesc, direction, count);
}

void
standard_ExecutorRun(QueryDesc *queryDesc,
					 ScanDirection direction, uint64 count)
{
	EState	   *estate;
	CmdType		operation;
	DestReceiver *dest;
	bool		sendTuples;
	MemoryContext oldcontext;

	/* 健全性检查 */
	Assert(queryDesc != NULL);

	estate = queryDesc->estate;

	Assert(estate != NULL);
	Assert(!(estate->es_top_eflags & EXEC_FLAG_EXPLAIN_ONLY));

	/* 调用方必须确保查询的快照处于活动状态 */
	Assert(GetActiveSnapshot() == estate->es_snapshot);

	/*
	 * 切换至每查询内存上下文
	 */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	/* 允许对执行器整体运行时间进行插桩 */
	if (queryDesc->totaltime)
		InstrStartNode(queryDesc->totaltime);

	/*
	 * 从查询描述符与查询特性中提取信息。
	 */
	operation = queryDesc->operation;
	dest = queryDesc->dest;

	/*
	 * 启动元组接收器（如果我们将要发出元组）
	 */
	estate->es_processed = 0;

	sendTuples = (operation == CMD_SELECT ||
				  queryDesc->plannedstmt->hasReturning);

	if (sendTuples)
		dest->rStartup(dest, operation, queryDesc->tupDesc);

	/*
	 * 运行计划，除非 direction 为 NoMovement。
	 *
	 * 注意：pquery.c 会在先前的调用已到达用户指定抓取方向的
	 * 数据末尾时选择 NoMovement。这一点很重要，因为执行器的多个部分
	 * 在报告 EOF 后再次被调用时可能会行为异常。例如，heapam.c 会
	 * 实际重启一次堆扫描并重新返回其全部数据。此外，对于并行计划，
	 * 在并行执行完成之后又出现一个额外的、必然非并行的执行请求时，
	 * 能否正常工作也存在疑问。（那种情况理应可以工作，但尚未经过测试。）
	 */
	if (!ScanDirectionIsNoMovement(direction))
		ExecutePlan(queryDesc,
					operation,
					sendTuples,
					count,
					direction,
					dest);

	/*
	 * 更新 es_total_processed，以记录跨多次 ExecutorRun() 调用处理的元组数。
	 */
	estate->es_total_processed += estate->es_processed;

	/*
	 * 关闭元组接收器（如果我们曾启动过它）
	 */
	if (sendTuples)
		dest->rShutdown(dest);

	if (queryDesc->totaltime)
		InstrStopNode(queryDesc->totaltime, estate->es_processed);

	MemoryContextSwitchTo(oldcontext);
}

/* ----------------------------------------------------------------
 *		ExecutorFinish
 *
 *		本例程必须在最后一次 ExecutorRun 调用之后调用。
 *		它执行清理工作，例如触发 AFTER 触发器。它与 ExecutorEnd
 *		分离，是因为 EXPLAIN ANALYZE 需要把这些动作计入总运行时间。
 *
 *		我们提供一个函数钩子变量，让可加载插件在 ExecutorFinish 被调用时
 *		获取控制权。这样的插件通常会调用 standard_ExecutorFinish()。
 *
 * ----------------------------------------------------------------
 */
void
ExecutorFinish(QueryDesc *queryDesc)
{
	if (ExecutorFinish_hook)
		(*ExecutorFinish_hook) (queryDesc);
	else
		standard_ExecutorFinish(queryDesc);
}

void
standard_ExecutorFinish(QueryDesc *queryDesc)
{
	EState	   *estate;
	MemoryContext oldcontext;

	/* 健全性检查 */
	Assert(queryDesc != NULL);

	estate = queryDesc->estate;

	Assert(estate != NULL);
	Assert(!(estate->es_top_eflags & EXEC_FLAG_EXPLAIN_ONLY));

	/* 它应当在每个 Executor 实例中只运行一次，且只能运行一次 */
	Assert(!estate->es_finished);

	/* 切换至每查询内存上下文 */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	/* 允许对执行器整体运行时间进行插桩 */
	if (queryDesc->totaltime)
		InstrStartNode(queryDesc->totaltime);

	/* 将 ModifyTable 节点运行至完成 */
	ExecPostprocessPlan(estate);

	/* 执行已排队的 AFTER 触发器，除非被告知不要执行 */
	if (!(estate->es_top_eflags & EXEC_FLAG_SKIP_TRIGGERS))
		AfterTriggerEndQuery(estate);

	if (queryDesc->totaltime)
		InstrStopNode(queryDesc->totaltime, 0);

	MemoryContextSwitchTo(oldcontext);

	estate->es_finished = true;
}

/* ----------------------------------------------------------------
 *		ExecutorEnd
 *
 *		本例程必须在任何查询计划执行结束时调用。
 *
 *		我们提供一个函数钩子变量，让可加载插件在 ExecutorEnd 被调用时
 *		获取控制权。这样的插件通常会调用 standard_ExecutorEnd()。
 *
 * ----------------------------------------------------------------
 */
void
ExecutorEnd(QueryDesc *queryDesc)
{
	if (ExecutorEnd_hook)
		(*ExecutorEnd_hook) (queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

void
standard_ExecutorEnd(QueryDesc *queryDesc)
{
	EState	   *estate;
	MemoryContext oldcontext;

	/* 健全性检查 */
	Assert(queryDesc != NULL);

	estate = queryDesc->estate;

	Assert(estate != NULL);

	if (estate->es_parallel_workers_to_launch > 0)
		pgstat_update_parallel_workers_stats((PgStat_Counter) estate->es_parallel_workers_to_launch,
											 (PgStat_Counter) estate->es_parallel_workers_launched);

	/*
	 * 检查是否已经调用了 ExecutorFinish，除非处于 EXPLAIN-only 模式。
	 * 需要这个 Assert，是因为 ExecutorFinish 是 9.1 才新增的，
	 * 调用方可能会忘记调用它。
	 */
	Assert(estate->es_finished ||
		   (estate->es_top_eflags & EXEC_FLAG_EXPLAIN_ONLY));

	/*
	 * 切换至每查询内存上下文以运行 ExecEndPlan
	 */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	ExecEndPlan(queryDesc->planstate, estate);

	/* 释放我们的快照 */
	UnregisterSnapshot(estate->es_snapshot);
	UnregisterSnapshot(estate->es_crosscheck_snapshot);

	/*
	 * 必须在销毁上下文之前先切换出去
	 */
	MemoryContextSwitchTo(oldcontext);

	/*
	 * 释放 EState 与每查询内存上下文。这应当能释放执行器分配的所有内容。
	 */
	FreeExecutorState(estate);

	/* 重置不再指向任何内容的 queryDesc 字段 */
	queryDesc->tupDesc = NULL;
	queryDesc->estate = NULL;
	queryDesc->planstate = NULL;
	queryDesc->totaltime = NULL;
}

/* ----------------------------------------------------------------
 *		ExecutorRewind
 *
 *		本例程可以在一个已打开的 queryDesc 上调用，将其回溯到开头。
 * ----------------------------------------------------------------
 */
void
ExecutorRewind(QueryDesc *queryDesc)
{
	EState	   *estate;
	MemoryContext oldcontext;

	/* 健全性检查 */
	Assert(queryDesc != NULL);

	estate = queryDesc->estate;

	Assert(estate != NULL);

	/* 对更新型查询重新扫描大概并不合理 */
	Assert(queryDesc->operation == CMD_SELECT);

	/*
	 * 切换至每查询内存上下文
	 */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	/*
	 * 重新扫描计划
	 */
	ExecReScan(queryDesc->planstate);

	MemoryContextSwitchTo(oldcontext);
}


/*
 * ExecCheckPermissions
 *		检查查询中提及的关系的访问权限
 *
 *		若权限充足则返回 true。否则，若 ereport_on_violation 为真则抛出
 *		适当的错误，否则直接返回 false。
 *
 *		注意：这并不处理行级安全策略（即 RLS）。如果因该权限检查通过而
 *		有行要返回给用户，那么还需要咨询 RLS（即 check_enable_rls()）。
 *
 *		参见 rewrite/rowsecurity.c。
 *
 *		注意：rangeTable 我们不再使用，但为其钩子保留，钩子可能仍想
 *		查看这些 RTE。
 */
bool
ExecCheckPermissions(List *rangeTable, List *rteperminfos,
					 bool ereport_on_violation)
{
	ListCell   *l;
	bool		result = true;

#ifdef USE_ASSERT_CHECKING
	Bitmapset  *indexset = NULL;

	/* 检查 rteperminfos 与 rangeTable 是否一致 */
	foreach(l, rangeTable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, l);

		if (rte->perminfoindex != 0)
		{
			/* 健全性检查 */

		/*
		 * 只有关系 RTE 以及曾经是关系 RTE（视图）的子查询 RTE 才会
		 * 设置其 perminfoindex。
		 */
			Assert(rte->rtekind == RTE_RELATION ||
				   (rte->rtekind == RTE_SUBQUERY &&
					rte->relkind == RELKIND_VIEW));

			(void) getRTEPermissionInfo(rteperminfos, rte);
			/* 不允许多对一的映射 */
			Assert(!bms_is_member(rte->perminfoindex, indexset));
			indexset = bms_add_member(indexset, rte->perminfoindex);
		}
	}

	/* 所有 rteperminfos 都已被引用 */
	Assert(bms_num_members(indexset) == list_length(rteperminfos));
#endif

	foreach(l, rteperminfos)
	{
		RTEPermissionInfo *perminfo = lfirst_node(RTEPermissionInfo, l);

		Assert(OidIsValid(perminfo->relid));
		result = ExecCheckOneRelPerms(perminfo);
		if (!result)
		{
			if (ereport_on_violation)
				aclcheck_error(ACLCHECK_NO_PRIV,
							   get_relkind_objtype(get_rel_relkind(perminfo->relid)),
							   get_rel_name(perminfo->relid));
			return false;
		}
	}

	if (ExecutorCheckPerms_hook)
		result = (*ExecutorCheckPerms_hook) (rangeTable, rteperminfos,
											 ereport_on_violation);
	return result;
}

/*
 * ExecCheckOneRelPerms
 *		检查单个关系的访问权限。
 */
bool
ExecCheckOneRelPerms(RTEPermissionInfo *perminfo)
{
	AclMode		requiredPerms;
	AclMode		relPerms;
	AclMode		remainingPerms;
	Oid			userid;
	Oid			relOid = perminfo->relid;

	requiredPerms = perminfo->requiredPerms;
	Assert(requiredPerms != 0);

	/*
	 * 要以其身份进行检查的用户 ID：除非有 setuid 指示，否则为当前用户。
	 *
	 * 注意：GetUserId() 目前足够快，对每个关系单独调用并无损害。
	 * 若情况不再如此，我们可以在 ExecCheckPermissions 中调用一次，
	 * 并将 userid 向下传递。但就目前而言，没必要增加这种额外的繁琐。
	 */
	userid = OidIsValid(perminfo->checkAsUser) ?
		perminfo->checkAsUser : GetUserId();

	/*
	 * 我们必须拥有 *全部* requiredPerms 位，但其中某些位可以来自于列级
	 * 而非关系级的权限。首先，移除那些已由关系级权限满足的位。
	 */
	relPerms = pg_class_aclmask(relOid, userid, requiredPerms, ACLMASK_ALL);
	remainingPerms = requiredPerms & ~relPerms;
	if (remainingPerms != 0)
	{
		int			col = -1;

		/*
		 * 如果我们缺少任何仅以关系级权限形式存在的权限，
		 * 就可以直接失败。
		 */
		if (remainingPerms & ~(ACL_SELECT | ACL_INSERT | ACL_UPDATE))
			return false;

		/*
		 * 检查我们是否在列级拥有所需的特权。
		 *
		 * 注意：失败只会报告表级错误；如果我们拥有部分但并非全部列特权，
		 * 最好能报告列级错误，但当前并非如此。
		 */
		if (remainingPerms & ACL_SELECT)
		{
			/*
			 * 当查询没有显式引用任何列时（例如 SELECT COUNT(*) FROM 表），
			 * 只要我们对关系的任意列拥有 SELECT 权限，就允许该查询，
			 * 这符合 SQL 规范。
			 */
			if (bms_is_empty(perminfo->selectedCols))
			{
				if (pg_attribute_aclcheck_all(relOid, userid, ACL_SELECT,
											  ACLMASK_ANY) != ACLCHECK_OK)
					return false;
			}

			while ((col = bms_next_member(perminfo->selectedCols, col)) >= 0)
			{
				/* 位编号以 FirstLowInvalidHeapAttributeNumber 为偏移 */
				AttrNumber	attno = col + FirstLowInvalidHeapAttributeNumber;

				if (attno == InvalidAttrNumber)
				{
					/* 整行引用，必须对全部列都有特权 */
					if (pg_attribute_aclcheck_all(relOid, userid, ACL_SELECT,
												  ACLMASK_ALL) != ACLCHECK_OK)
						return false;
				}
				else
				{
					if (pg_attribute_aclcheck(relOid, attno, userid,
											  ACL_SELECT) != ACLCHECK_OK)
						return false;
				}
			}
		}

		/*
		 * 对于被修改的列基本相同，即按照 remainingPerms 所指定的 INSERT 与
		 * UPDATE 两种权限分别处理。
		 */
		if (remainingPerms & ACL_INSERT &&
			!ExecCheckPermissionsModified(relOid,
										  userid,
										  perminfo->insertedCols,
										  ACL_INSERT))
			return false;

		if (remainingPerms & ACL_UPDATE &&
			!ExecCheckPermissionsModified(relOid,
										  userid,
										  perminfo->updatedCols,
										  ACL_UPDATE))
			return false;
	}
	return true;
}

/*
 * ExecCheckPermissionsModified
 *		检查单个关系的 INSERT 或 UPDATE 访问权限（这两者统一处理）。
 */
static bool
ExecCheckPermissionsModified(Oid relOid, Oid userid, Bitmapset *modifiedCols,
							 AclMode requiredPerms)
{
	int			col = -1;

	/*
	 * 当查询并未显式更新任何列时，只要我们对关系的任意一列拥有权限就允许
	 * 该查询。这是为了处理 SELECT FOR UPDATE，以及 UPDATE 中可能出现的
	 * 边界情况。
	 */
	if (bms_is_empty(modifiedCols))
	{
		if (pg_attribute_aclcheck_all(relOid, userid, requiredPerms,
									  ACLMASK_ANY) != ACLCHECK_OK)
			return false;
	}

	while ((col = bms_next_member(modifiedCols, col)) >= 0)
	{
		/* 位编号以 FirstLowInvalidHeapAttributeNumber 为偏移 */
		AttrNumber	attno = col + FirstLowInvalidHeapAttributeNumber;

		if (attno == InvalidAttrNumber)
		{
			/* 整行引用不会出现在这里 */
			elog(ERROR, "whole-row update is not implemented");
		}
		else
		{
			if (pg_attribute_aclcheck(relOid, attno, userid,
									  requiredPerms) != ACLCHECK_OK)
				return false;
		}
	}
	return true;
}

/*
 * 确保查询不会隐含对任何非临时表的写入；
 * 除非我们处于并行模式，在那种情况下甚至不允许对临时表写入。
 *
 * 注意：在 Hot Standby 中，这需要像并行模式那样拒绝向临时表的写入；
 * 但 HS 备用节点本来就不可能创建任何临时表，因此无需检查这一点。
 */
static void
ExecCheckXactReadOnly(PlannedStmt *plannedstmt)
{
	ListCell   *l;

	/*
	 * 若并行模式下请求了对表（临时或非临时）的写入权限则失败，
	 * 否则对任何非临时表失败。
	 */
	foreach(l, plannedstmt->permInfos)
	{
		RTEPermissionInfo *perminfo = lfirst_node(RTEPermissionInfo, l);

		if ((perminfo->requiredPerms & (~ACL_SELECT)) == 0)
			continue;

		if (isTempNamespace(get_rel_namespace(perminfo->relid)))
			continue;

		PreventCommandIfReadOnly(CreateCommandName((Node *) plannedstmt));
	}

	if (plannedstmt->commandType != CMD_SELECT || plannedstmt->hasModifyingCTE)
		PreventCommandIfParallelMode(CreateCommandName((Node *) plannedstmt));
}


/* ----------------------------------------------------------------
 *		InitPlan
 *
 *		初始化查询计划：打开文件、分配存储，并启动规则管理器
 * ----------------------------------------------------------------
 */
static void
InitPlan(QueryDesc *queryDesc, int eflags)
{
	CmdType		operation = queryDesc->operation;
	PlannedStmt *plannedstmt = queryDesc->plannedstmt;
	Plan	   *plan = plannedstmt->planTree;
	List	   *rangeTable = plannedstmt->rtable;
	EState	   *estate = queryDesc->estate;
	PlanState  *planstate;
	TupleDesc	tupType;
	ListCell   *l;
	int			i;

	/*
	 * 进行权限检查
	 */
	ExecCheckPermissions(rangeTable, plannedstmt->permInfos, true);

	/*
	 * 初始化节点的执行状态
	 */
	ExecInitRangeTable(estate, rangeTable, plannedstmt->permInfos,
					   bms_copy(plannedstmt->unprunableRelids));

	estate->es_plannedstmt = plannedstmt;
	estate->es_part_prune_infos = plannedstmt->partPruneInfos;

	/*
	 * 执行运行时的“初始”裁剪，以识别出哪些子计划（对应于包含
	 * PartitionPruneInfo 的计划节点的子节点，例如 Append）不会被执行。
	 * 结果是将被执行的子计划索引的位图集合，保存在 es_part_prune_results 中。
	 * 这些结果对应于每个 PartitionPruneInfo 条目，且 es_part_prune_results
	 * 列表与 es_part_prune_infos 平行对应。
	 */
	ExecDoInitialPruning(estate);

	/*
	 * 接下来，从 PlanRowMark（如果有的话）构建 ExecRowMark 数组。
	 */
	if (plannedstmt->rowMarks)
	{
		estate->es_rowmarks = (ExecRowMark **)
			palloc0(estate->es_range_table_size * sizeof(ExecRowMark *));
		foreach(l, plannedstmt->rowMarks)
		{
			PlanRowMark *rc = (PlanRowMark *) lfirst(l);
			RangeTblEntry *rte = exec_rt_fetch(rc->rti, estate);
			Oid			relid;
			Relation	relation;
			ExecRowMark *erm;

			/* 忽略“父”行标记；它们在运行时无关紧要 */
			if (rc->isParent)
				continue;

			/*
			 * 同时忽略那些在 ExecDoInitialPruning() 中已被裁剪掉的、
			 * 属于子表的行标记。
			 */
			if (rte->rtekind == RTE_RELATION &&
				!bms_is_member(rc->rti, estate->es_unpruned_relids))
				continue;

			/* 获取关系的 OID（若是子查询则产生 InvalidOid） */
			relid = rte->relid;

			/* 打开关系，如果我们需要针对此标记类型访问它的话 */
			switch (rc->markType)
			{
				case ROW_MARK_EXCLUSIVE:
				case ROW_MARK_NOKEYEXCLUSIVE:
				case ROW_MARK_SHARE:
				case ROW_MARK_KEYSHARE:
				case ROW_MARK_REFERENCE:
					relation = ExecGetRangeTableRelation(estate, rc->rti, false);
					break;
				case ROW_MARK_COPY:
					/* 不需要访问物理表 */
					relation = NULL;
					break;
				default:
					elog(ERROR, "unrecognized markType: %d", rc->markType);
					relation = NULL;	/* 让编译器安静 */
					break;
			}

			/* 检查关系是否是一个合法的标记目标 */
			if (relation)
				CheckValidRowMarkRel(relation, rc->markType);

			erm = (ExecRowMark *) palloc(sizeof(ExecRowMark));
			erm->relation = relation;
			erm->relid = relid;
			erm->rti = rc->rti;
			erm->prti = rc->prti;
			erm->rowmarkId = rc->rowmarkId;
			erm->markType = rc->markType;
			erm->strength = rc->strength;
			erm->waitPolicy = rc->waitPolicy;
			erm->ermActive = false;
			ItemPointerSetInvalid(&(erm->curCtid));
			erm->ermExtra = NULL;

			Assert(erm->rti > 0 && erm->rti <= estate->es_range_table_size &&
				   estate->es_rowmarks[erm->rti - 1] == NULL);

			estate->es_rowmarks[erm->rti - 1] = erm;
		}
	}

	/*
	 * 将执行器的元组表初始化为空。
	 */
	estate->es_tupleTable = NIL;

	/* 标记此 EState 不用于 EPQ */
	estate->es_epq_active = NULL;

	/*
	 * 为每个 SubPlan 初始化私有状态信息。我们必须在对主查询树运行
	 * ExecInitNode 之前完成，因为 ExecInitSubPlan 期望能找到这些条目。
	 */
	Assert(estate->es_subplanstates == NIL);
	i = 1;						/* 子计划索引从 1 开始计数 */
	foreach(l, plannedstmt->subplans)
	{
		Plan	   *subplan = (Plan *) lfirst(l);
		PlanState  *subplanstate;
		int			sp_eflags;

		/*
		 * 子计划永远不需要做 BACKWARD 扫描或 MARK/RESTORE。如果它是一个
		 * 无参数子计划（非 initplan），我们建议它准备好高效处理 REWIND；
		 * 否则则无此必要。
		 */
		sp_eflags = eflags
			& ~(EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK);
		if (bms_is_member(i, plannedstmt->rewindPlanIDs))
			sp_eflags |= EXEC_FLAG_REWIND;

		subplanstate = ExecInitNode(subplan, estate, sp_eflags);

		estate->es_subplanstates = lappend(estate->es_subplanstates,
										   subplanstate);

		i++;
	}

	/*
	 * 初始化查询树中所有节点的私有状态信息。这会打开文件、分配存储，
	 * 并使我们准备好开始处理元组。
	 */
	planstate = ExecInitNode(plan, estate, eflags);

	/*
	 * 获取描述待返回元组类型的元组描述符。
	 */
	tupType = ExecGetResultType(planstate);

	/*
	 * 若需要则初始化 junk 过滤器。如果顶层目标列表中存在任何 junk 属性，
	 * SELECT 查询就需要一个过滤器。
	 */
	if (operation == CMD_SELECT)
	{
		bool		junk_filter_needed = false;
		ListCell   *tlist;

		foreach(tlist, plan->targetlist)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(tlist);

			if (tle->resjunk)
			{
				junk_filter_needed = true;
				break;
			}
		}

		if (junk_filter_needed)
		{
			JunkFilter *j;
			TupleTableSlot *slot;

			slot = ExecInitExtraTupleSlot(estate, NULL, &TTSOpsVirtual);
			j = ExecInitJunkFilter(planstate->plan->targetlist,
								   slot);
			estate->es_junkFilter = j;

			/* 希望返回清洗后的元组类型 */
			tupType = j->jf_cleanTupType;
		}
	}

	queryDesc->tupDesc = tupType;
	queryDesc->planstate = planstate;
}

/*
 * 检查所提议的结果关系是否为该操作合法的目标
 *
 * 一般来说，解析器和（或）规划器本应已经发现了任何此类错误，
 * 但我们还是确认一下。
 *
 * 对于 INSERT ON CONFLICT，结果关系必须支持 onConflictAction，
 * 无论冲突是否真的发生。
 *
 * 对于 MERGE，mergeActions 是可能被执行的动作列表。结果关系必须支持
 * 每一个动作，无论它们是否全部被执行。
 *
 * 注意：修改本函数时，你可能还需要查看 CheckValidRowMarkRel。
 */
void
CheckValidResultRel(ResultRelInfo *resultRelInfo, CmdType operation,
					OnConflictAction onConflictAction, List *mergeActions)
{
	Relation	resultRel = resultRelInfo->ri_RelationDesc;
	FdwRoutine *fdwroutine;

	/* 期望从 InitResultRelInfo() 得到一个已完整构造好的 ResultRelInfo。 */
	Assert(resultRelInfo->ri_needLockTagTuple ==
		   IsInplaceUpdateRelation(resultRel));

	switch (resultRel->rd_rel->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_PARTITIONED_TABLE:

		/*
		 * 对于 MERGE，检查目标关系是否支持每个动作。
		 * 对于其他操作，只需检查操作本身。
		 */
			if (operation == CMD_MERGE)
				foreach_node(MergeAction, action, mergeActions)
					CheckCmdReplicaIdentity(resultRel, action->commandType);
			else
				CheckCmdReplicaIdentity(resultRel, operation);

			/*
			 * 对于 INSERT ON CONFLICT DO UPDATE，额外检查目标关系是否
			 * 支持 UPDATE。
			 */
			if (onConflictAction == ONCONFLICT_UPDATE)
				CheckCmdReplicaIdentity(resultRel, CMD_UPDATE);
			break;
		case RELKIND_SEQUENCE:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change sequence \"%s\"",
							RelationGetRelationName(resultRel))));
			break;
		case RELKIND_TOASTVALUE:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change TOAST relation \"%s\"",
							RelationGetRelationName(resultRel))));
			break;
		case RELKIND_VIEW:

			/*
			 * 仅当存在合适的 INSTEAD OF 触发器时才允许。否则报错，
			 * 但省略 errdetail，因为我们手头没有相关信息（而且鉴于这几乎
			 * 不该发生，为此大费周章并不值得）。
			 */
			if (!view_has_instead_trigger(resultRel, operation, mergeActions))
				error_view_not_updatable(resultRel, operation, mergeActions,
										 NULL);
			break;
		case RELKIND_MATVIEW:
			if (!MatViewIncrementalMaintenanceIsEnabled())
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot change materialized view \"%s\"",
								RelationGetRelationName(resultRel))));
			break;
		case RELKIND_FOREIGN_TABLE:
			/* 仅当 FDW 支持时才允许 */
			fdwroutine = resultRelInfo->ri_FdwRoutine;
			switch (operation)
			{
				case CMD_INSERT:
					if (fdwroutine->ExecForeignInsert == NULL)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("cannot insert into foreign table \"%s\"",
										RelationGetRelationName(resultRel))));
					if (fdwroutine->IsForeignRelUpdatable != NULL &&
						(fdwroutine->IsForeignRelUpdatable(resultRel) & (1 << CMD_INSERT)) == 0)
						ereport(ERROR,
								(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
								 errmsg("foreign table \"%s\" does not allow inserts",
										RelationGetRelationName(resultRel))));
					break;
				case CMD_UPDATE:
					if (fdwroutine->ExecForeignUpdate == NULL)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("cannot update foreign table \"%s\"",
										RelationGetRelationName(resultRel))));
					if (fdwroutine->IsForeignRelUpdatable != NULL &&
						(fdwroutine->IsForeignRelUpdatable(resultRel) & (1 << CMD_UPDATE)) == 0)
						ereport(ERROR,
								(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
								 errmsg("foreign table \"%s\" does not allow updates",
										RelationGetRelationName(resultRel))));
					break;
				case CMD_DELETE:
					if (fdwroutine->ExecForeignDelete == NULL)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("cannot delete from foreign table \"%s\"",
										RelationGetRelationName(resultRel))));
					if (fdwroutine->IsForeignRelUpdatable != NULL &&
						(fdwroutine->IsForeignRelUpdatable(resultRel) & (1 << CMD_DELETE)) == 0)
						ereport(ERROR,
								(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
								 errmsg("foreign table \"%s\" does not allow deletes",
										RelationGetRelationName(resultRel))));
					break;
				default:
					elog(ERROR, "unrecognized CmdType: %d", (int) operation);
					break;
			}
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change relation \"%s\"",
							RelationGetRelationName(resultRel))));
			break;
	}
}

/*
 * 检查一个被提议的行标记目标关系是否是一个合法的目标
 *
 * 在大多数情况下，解析器和/或规划器本应已经注意到这一点，但它们并不能
 * 覆盖所有情况。
 */
static void
CheckValidRowMarkRel(Relation rel, RowMarkType markType)
{
	FdwRoutine *fdwroutine;

	switch (rel->rd_rel->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_PARTITIONED_TABLE:
			/* 允许 */
			break;
		case RELKIND_SEQUENCE:
			/* 必须禁止，因为我们不会对序列做 vacuum */
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot lock rows in sequence \"%s\"",
							RelationGetRelationName(rel))));
			break;
		case RELKIND_TOASTVALUE:
			/* 我们本可以允许，但似乎没有充分的理由 */
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot lock rows in TOAST relation \"%s\"",
							RelationGetRelationName(rel))));
			break;
		case RELKIND_VIEW:
			/* 不应到达此处；规划器本应已展开视图 */
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot lock rows in view \"%s\"",
							RelationGetRelationName(rel))));
			break;
		case RELKIND_MATVIEW:
			/* 允许引用物化视图，但不允许实际的锁定子句 */
			if (markType != ROW_MARK_REFERENCE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot lock rows in materialized view \"%s\"",
								RelationGetRelationName(rel))));
			break;
		case RELKIND_FOREIGN_TABLE:
			/* 仅当 FDW 支持时才允许 */
			fdwroutine = GetFdwRoutineForRelation(rel, false);
			if (fdwroutine->RefetchForeignRow == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot lock rows in foreign table \"%s\"",
								RelationGetRelationName(rel))));
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot lock rows in relation \"%s\"",
							RelationGetRelationName(rel))));
			break;
	}
}

/*
 * 为一个结果关系初始化 ResultRelInfo 数据
 *
 * 注意：在 PostgreSQL 9.1 之前，本函数还包含现在位于 CheckValidResultRel
 * 中的 relkind 检查，并且会在适当时调用 ExecOpenIndices。请确保调用方
 * 覆盖了这些需求。
 */
void
InitResultRelInfo(ResultRelInfo *resultRelInfo,
				  Relation resultRelationDesc,
				  Index resultRelationIndex,
				  ResultRelInfo *partition_root_rri,
				  int instrument_options)
{
	MemSet(resultRelInfo, 0, sizeof(ResultRelInfo));
	resultRelInfo->type = T_ResultRelInfo;
	resultRelInfo->ri_RangeTableIndex = resultRelationIndex;
	resultRelInfo->ri_RelationDesc = resultRelationDesc;
	resultRelInfo->ri_NumIndices = 0;
	resultRelInfo->ri_IndexRelationDescs = NULL;
	resultRelInfo->ri_IndexRelationInfo = NULL;
	resultRelInfo->ri_needLockTagTuple =
		IsInplaceUpdateRelation(resultRelationDesc);
	/* 制作一份副本，以免依赖可能发生变化的 relcache 信息…… */
	resultRelInfo->ri_TrigDesc = CopyTriggerDesc(resultRelationDesc->trigdesc);
	if (resultRelInfo->ri_TrigDesc)
	{
		int			n = resultRelInfo->ri_TrigDesc->numtriggers;

		resultRelInfo->ri_TrigFunctions = (FmgrInfo *)
			palloc0(n * sizeof(FmgrInfo));
		resultRelInfo->ri_TrigWhenExprs = (ExprState **)
			palloc0(n * sizeof(ExprState *));
		if (instrument_options)
			resultRelInfo->ri_TrigInstrument = InstrAlloc(n, instrument_options, false);
	}
	else
	{
		resultRelInfo->ri_TrigFunctions = NULL;
		resultRelInfo->ri_TrigWhenExprs = NULL;
		resultRelInfo->ri_TrigInstrument = NULL;
	}
	if (resultRelationDesc->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		resultRelInfo->ri_FdwRoutine = GetFdwRoutineForRelation(resultRelationDesc, true);
	else
		resultRelInfo->ri_FdwRoutine = NULL;

	/* 以下字段将在需要时随后设置 */
	resultRelInfo->ri_RowIdAttNo = 0;
	resultRelInfo->ri_extraUpdatedCols = NULL;
	resultRelInfo->ri_projectNew = NULL;
	resultRelInfo->ri_newTupleSlot = NULL;
	resultRelInfo->ri_oldTupleSlot = NULL;
	resultRelInfo->ri_projectNewInfoValid = false;
	resultRelInfo->ri_FdwState = NULL;
	resultRelInfo->ri_usesFdwDirectModify = false;
	resultRelInfo->ri_CheckConstraintExprs = NULL;
	resultRelInfo->ri_GenVirtualNotNullConstraintExprs = NULL;
	resultRelInfo->ri_GeneratedExprsI = NULL;
	resultRelInfo->ri_GeneratedExprsU = NULL;
	resultRelInfo->ri_projectReturning = NULL;
	resultRelInfo->ri_onConflictArbiterIndexes = NIL;
	resultRelInfo->ri_onConflict = NULL;
	resultRelInfo->ri_ReturningSlot = NULL;
	resultRelInfo->ri_TrigOldSlot = NULL;
	resultRelInfo->ri_TrigNewSlot = NULL;
	resultRelInfo->ri_AllNullSlot = NULL;
	resultRelInfo->ri_MergeActions[MERGE_WHEN_MATCHED] = NIL;
	resultRelInfo->ri_MergeActions[MERGE_WHEN_NOT_MATCHED_BY_SOURCE] = NIL;
	resultRelInfo->ri_MergeActions[MERGE_WHEN_NOT_MATCHED_BY_TARGET] = NIL;
	resultRelInfo->ri_MergeJoinCondition = NULL;

	/*
	 * 只有 ExecInitPartitionInfo() 和 ExecInitPartitionDispatchInfo() 会传入
	 * 非 NULL 的 partition_root_rri。对于属于初始查询一部分、而非由元组路由
	 * 动态添加的子关系，此字段在 ExecInitModifyTable() 中填充。
	 */
	resultRelInfo->ri_RootResultRelInfo = partition_root_rri;
	/* 由 ExecGetRootToChildMap 设置 */
	resultRelInfo->ri_RootToChildMap = NULL;
	resultRelInfo->ri_RootToChildMapValid = false;
	/* 由 ExecInitRoutingInfo 设置 */
	resultRelInfo->ri_PartitionTupleSlot = NULL;
	resultRelInfo->ri_ChildToRootMap = NULL;
	resultRelInfo->ri_ChildToRootMapValid = false;
	resultRelInfo->ri_CopyMultiInsertBuffer = NULL;
}

/*
 * ExecGetTriggerResultRel
 *		获取触发器目标关系对应的 ResultRelInfo。
 *
 *		大多数情况下，触发器是在查询的某个结果关系上触发的，
 *		因此我们只需返回一个我们已创建并保存在 es_opened_result_relations
 *		或 es_tuple_routing_result_relations 列表中的合适对象即可。
 *
 *		然而，有时有必要在其他关系上触发触发器；这主要发生在 RI 更新触发器
 *		在其他关系上排队额外的触发器时，这些触发器将在外层查询的上下文中
 *		被处理。出于效率考虑，我们也希望为这些触发器准备一个 ResultRelInfo，
 *		这样可以避免重复重新打开关系。（它也提供了一种途径，让 EXPLAIN
 *		ANALYZE 能够报告这类触发器的运行时间。）因此我们会按需创建额外的
 *		ResultRelInfo，并将其保存在 es_trig_target_relations 中。
 */
ResultRelInfo *
ExecGetTriggerResultRel(EState *estate, Oid relid,
						ResultRelInfo *rootRelInfo)
{
	ResultRelInfo *rInfo;
	ListCell   *l;
	Relation	rel;
	MemoryContext oldcontext;

	/*
	 * 在创建新的 ResultRelInfo 之前，先检查我们是否已经为该关系
	 * 创建并缓存了一个。我们必须确保给定的 'rootRelInfo' 与缓存在
	 * 该 ResultRelInfo 中的相匹配，因为分区的触发器处理可能导致对
	 * ri_RootResultRelInfo 应设为何值产生混合要求。
	 */

	/* 遍历查询的结果关系 */
	foreach(l, estate->es_opened_result_relations)
	{
		rInfo = lfirst(l);
		if (RelationGetRelid(rInfo->ri_RelationDesc) == relid &&
			rInfo->ri_RootResultRelInfo == rootRelInfo)
			return rInfo;
	}

	/*
	 * 遍历在元组路由过程中创建的结果关系（若有）。
	 */
	foreach(l, estate->es_tuple_routing_result_relations)
	{
		rInfo = (ResultRelInfo *) lfirst(l);
		if (RelationGetRelid(rInfo->ri_RelationDesc) == relid &&
			rInfo->ri_RootResultRelInfo == rootRelInfo)
			return rInfo;
	}

	/* 没有，但也许我们已经为它额外创建过一个 ResultRelInfo */
	foreach(l, estate->es_trig_target_relations)
	{
		rInfo = (ResultRelInfo *) lfirst(l);
		if (RelationGetRelid(rInfo->ri_RelationDesc) == relid &&
			rInfo->ri_RootResultRelInfo == rootRelInfo)
			return rInfo;
	}
	/* 没有，所以我们需要新创建一个 */

	/*
	 * 打开目标关系的 relcache 条目。我们假设后端从触发器事件入队时起
	 * 就一直持有着适当的锁，因此这里无需再获取新锁。另外，我们也不必
	 * 重新检查 relkind，所以不需要 CheckValidResultRel。
	 */
	rel = table_open(relid, NoLock);

	/*
	 * 在正确的上下文中创建新条目。
	 */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);
	rInfo = makeNode(ResultRelInfo);
	InitResultRelInfo(rInfo,
					  rel,
					  0,		/* 伪范围表索引 */
					  rootRelInfo,
					  estate->es_instrument);
	estate->es_trig_target_relations =
		lappend(estate->es_trig_target_relations, rInfo);
	MemoryContextSwitchTo(oldcontext);

	/*
	 * 目前，仅用于触发器的 ResultRelInfo 不需要任何索引信息，
	 * 因此无需调用 ExecOpenIndices。
	 */

	return rInfo;
}

/*
 * 返回给定叶子分区结果关系的祖先关系，直至并包含查询的根目标关系。
 *
 * 它们的行为与 ExecGetTriggerResultRel 打开的那些很相似，
 * 区别在于我们需要把它们保存在一个单独的列表中。
 *
 * 它们由 ExecCloseResultRelations 关闭。
 */
List *
ExecGetAncestorResultRels(EState *estate, ResultRelInfo *resultRelInfo)
{
	ResultRelInfo *rootRelInfo = resultRelInfo->ri_RootResultRelInfo;
	Relation	partRel = resultRelInfo->ri_RelationDesc;
	Oid			rootRelOid;

	if (!partRel->rd_rel->relispartition)
		elog(ERROR, "cannot find ancestors of a non-partition result relation");
	Assert(rootRelInfo != NULL);
	rootRelOid = RelationGetRelid(rootRelInfo->ri_RelationDesc);
	if (resultRelInfo->ri_ancestorResultRels == NIL)
	{
		ListCell   *lc;
		List	   *oids = get_partition_ancestors(RelationGetRelid(partRel));
		List	   *ancResultRels = NIL;

		foreach(lc, oids)
		{
			Oid			ancOid = lfirst_oid(lc);
			Relation	ancRel;
			ResultRelInfo *rInfo;

			/*
			 * 在此处忽略根祖先，改为使用其 ri_RootResultRelInfo（见下文）。
			 * 此外，当我们找到查询中提及的那张表时，便停止向上爬升层级。
			 */
			if (ancOid == rootRelOid)
				break;

			/*
			 * 直至根目标关系的所有祖先，必定都已被规划器或
			 * AcquireExecutorLocks() 锁定过。
			 */
			ancRel = table_open(ancOid, NoLock);
			rInfo = makeNode(ResultRelInfo);

			/* 伪范围表索引 */
			InitResultRelInfo(rInfo, ancRel, 0, NULL,
							  estate->es_instrument);
			ancResultRels = lappend(ancResultRels, rInfo);
		}
		ancResultRels = lappend(ancResultRels, rootRelInfo);
		resultRelInfo->ri_ancestorResultRels = ancResultRels;
	}

	/* 我们必定已经找到了某个祖先 */
	Assert(resultRelInfo->ri_ancestorResultRels != NIL);

	return resultRelInfo->ri_ancestorResultRels;
}

/* ----------------------------------------------------------------
 *		ExecPostprocessPlan
 *
 *		在关闭之前，给计划节点最后一次执行机会
 * ----------------------------------------------------------------
 */
static void
ExecPostprocessPlan(EState *estate)
{
	ListCell   *lc;

	/*
	 * 确保节点向前运行。
	 */
	estate->es_direction = ForwardScanDirection;

	/*
	 * 将任何次要的 ModifyTable 节点运行至完成，以防主查询没有从它们
	 * 取走全部行。（我们这样做是为了确保这些节点有可预测的结果。）
	 */
	foreach(lc, estate->es_auxmodifytables)
	{
		PlanState  *ps = (PlanState *) lfirst(lc);

		for (;;)
		{
			TupleTableSlot *slot;

			/* 每次都重置每个输出元组的 exprcontext */
			ResetPerTupleExprContext(estate);

			slot = ExecProcNode(ps);

			if (TupIsNull(slot))
				break;
		}
	}
}

/* ----------------------------------------------------------------
 *		ExecEndPlan
 *
 *		清理查询计划——关闭文件并释放存储
 *
 *		注意：我们不再非常担心释放存储本身；FreeExecutorState 应当能保证
 *		释放所有需要释放的内存。我们真正要做的是关闭关系并丢弃缓冲区引脚。
 *		因此，例如元组表必须被清空或丢弃，以确保引脚被释放。
 * ----------------------------------------------------------------
 */
static void
ExecEndPlan(PlanState *planstate, EState *estate)
{
	ListCell   *l;

	/*
	 * 关闭节点类型特定的查询处理
	 */
	ExecEndNode(planstate);

	/*
	 * 子计划同理
	 */
	foreach(l, estate->es_subplanstates)
	{
		PlanState  *subplanstate = (PlanState *) lfirst(l);

		ExecEndNode(subplanstate);
	}

	/*
	 * 销毁执行器的元组表。实际上我们只在乎释放缓冲区引脚和 tupdesc
	 * 引用计数；没有必要 pfree TupleTableSlot，因为所在的
	 * 内存上下文反正马上就要消失了。
	 */
	ExecResetTupleTable(estate->es_tupleTable, false);

	/*
	 * 关闭为范围表项或结果关系打开的任何关系。
	 */
	ExecCloseResultRelations(estate);
	ExecCloseRangeTableRelations(estate);
}

/*
 * 关闭为 ResultRelInfos 打开的任何关系。
 */
void
ExecCloseResultRelations(EState *estate)
{
	ListCell   *l;

	/*
	 * 若有结果关系的索引则关闭它们。（关系本身由
	 * ExecCloseRangeTableRelations() 关闭。）
	 *
	 * 此外，关闭可能存在于每个结果关系的 ri_ancestorResultRels 中的
	 * 桩 RT（stub RT）。
	 */
	foreach(l, estate->es_opened_result_relations)
	{
		ResultRelInfo *resultRelInfo = lfirst(l);
		ListCell   *lc;

		ExecCloseIndices(resultRelInfo);
		foreach(lc, resultRelInfo->ri_ancestorResultRels)
		{
			ResultRelInfo *rInfo = lfirst(lc);

			/*
			 * RTI > 0 的祖先关系（应当只有根祖先）由
			 * ExecCloseRangeTableRelations 负责关闭。
			 */
			if (rInfo->ri_RangeTableIndex > 0)
				continue;

			table_close(rInfo->ri_RelationDesc, NoLock);
		}
	}

	/* 关闭由 ExecGetTriggerResultRel() 打开的任何关系。 */
	foreach(l, estate->es_trig_target_relations)
	{
		ResultRelInfo *resultRelInfo = (ResultRelInfo *) lfirst(l);

		/*
		 * 断言这是一个“伪”ResultRelInfo，见上文。否则我们可能会对一个
		 * 由 ExecGetRangeTableRelation 打开的关系发出重复的关闭。
		 */
		Assert(resultRelInfo->ri_RangeTableIndex == 0);

		/*
		 * 由于 ExecGetTriggerResultRel 不会为这些关系调用 ExecOpenIndices，
		 * 我们也就无需调用 ExecCloseIndices。
		 */
		Assert(resultRelInfo->ri_NumIndices == 0);

		table_close(resultRelInfo->ri_RelationDesc, NoLock);
	}
}

/*
 * 关闭由 ExecGetRangeTableRelation() 打开的所有关系。
 *
 * 我们不会释放可能持有这些关系的任何锁。
 */
void
ExecCloseRangeTableRelations(EState *estate)
{
	int			i;

	for (i = 0; i < estate->es_range_table_size; i++)
	{
		if (estate->es_relations[i])
			table_close(estate->es_relations[i], NoLock);
	}
}

/* ----------------------------------------------------------------
 *		ExecutePlan
 *
 *		执行查询计划，直到我们取到了 'numberTuples' 个元组，
 *		按指定方向移动。
 *
 *		若 numberTuples 为 0，则运行至完成
 * ----------------------------------------------------------------
 */
static void
ExecutePlan(QueryDesc *queryDesc,
			CmdType operation,
			bool sendTuples,
			uint64 numberTuples,
			ScanDirection direction,
			DestReceiver *dest)
{
	EState	   *estate = queryDesc->estate;
	PlanState  *planstate = queryDesc->planstate;
	bool		use_parallel_mode;
	TupleTableSlot *slot;
	uint64		current_tuple_count;

	/*
	 * 初始化局部变量
	 */
	current_tuple_count = 0;

	/*
	 * 设置方向。
	 */
	estate->es_direction = direction;

	/*
	 * 若合适则设置并行模式。
	 *
	 * 并行模式只支持计划的完整执行。如果我们已经部分执行过它，
	 * 或者调用方要求我们提前退出，就必须强制计划在不使用并行的情况下运行。
	 */
	if (queryDesc->already_executed || numberTuples != 0)
		use_parallel_mode = false;
	else
		use_parallel_mode = queryDesc->plannedstmt->parallelModeNeeded;
	queryDesc->already_executed = true;

	estate->es_use_parallel_mode = use_parallel_mode;
	if (use_parallel_mode)
		EnterParallelMode();

	/*
	 * 循环，直到我们从计划中处理完恰当数量的元组。
	 */
	for (;;)
	{
		/* 重置每个输出元组的 exprcontext */
		ResetPerTupleExprContext(estate);

		/*
		 * 执行计划并获取一个元组
		 */
		slot = ExecProcNode(planstate);

		/*
		 * 如果元组为空，我们就认为没有更多要处理的内容，
		 * 因此直接结束循环……
		 */
		if (TupIsNull(slot))
			break;

		/*
		 * 如果我们有一个 junk 过滤器，则投影出一个去除了 junk 的新元组。
		 *
		 * 将这个新的“干净”元组存放在 junkfilter 的 resultSlot 中。
		 * （以前我们把它存回“脏”元组之上，那是错误的，因为那个元组槽
		 * 具有错误的描述符。）
		 */
		if (estate->es_junkFilter != NULL)
			slot = ExecFilterJunk(estate->es_junkFilter, slot);

		/*
		 * 如果我们应当把元组发往某处，就发送它。（实际上，到了这一步
		 * 这大概总是成立的。）
		 */
		if (sendTuples)
		{
			/*
			 * 如果我们无法发送该元组，就认为目标已经关闭、且无法再发送
			 * 更多元组。若是这种情况，则结束循环。
			 */
			if (!dest->receiveSlot(slot, dest))
				break;
		}

		/*
		 * 统计已处理的元组数（如果这是 SELECT）。（对于其它操作类型，
		 * 必须由 ModifyTable 计划节点来统计相应的事件。）
		 */
		if (operation == CMD_SELECT)
			(estate->es_processed)++;

		/*
		 * 检查元组计数……如果已处理完恰当数量则退出，否则再次循环
		 * 处理更多元组。numberTuples 为零表示没有限制。
		 */
		current_tuple_count++;
		if (numberTuples && numberTuples == current_tuple_count)
			break;
	}

	/*
	 * 如果我们知道不再需要回溯，就可以在此刻释放资源。
	 */
	if (!(estate->es_top_eflags & EXEC_FLAG_BACKWARD))
		ExecShutdownNode(planstate);

	if (use_parallel_mode)
		ExitParallelMode();
}


/*
 * ExecRelCheck --- 检查元组是否满足结果关系的检查约束
 *
 * 若满足则返回 NULL，否则返回失败的检查约束名称
 */
static const char *
ExecRelCheck(ResultRelInfo *resultRelInfo,
			 TupleTableSlot *slot, EState *estate)
{
	Relation	rel = resultRelInfo->ri_RelationDesc;
	int			ncheck = rel->rd_att->constr->num_check;
	ConstrCheck *check = rel->rd_att->constr->check;
	ExprContext *econtext;
	MemoryContext oldContext;

	/*
	 * CheckNNConstraintFetch 此前只是发出警告便放行，但现在我们应当
	 * 报错，而不是可能未能强制执行某个重要的约束。
	 */
	if (ncheck != rel->rd_rel->relchecks)
		elog(ERROR, "%d pg_constraint record(s) missing for relation \"%s\"",
			 rel->rd_rel->relchecks - ncheck, RelationGetRelationName(rel));

	/*
	 * 如果对于此结果关系是第一次经过，则构建该关系约束表达式的表达式
	 * 节点树。把它们保存在每查询内存上下文中，以便在整个查询期间都存在。
	 */
	if (resultRelInfo->ri_CheckConstraintExprs == NULL)
	{
		oldContext = MemoryContextSwitchTo(estate->es_query_cxt);
		resultRelInfo->ri_CheckConstraintExprs = palloc0_array(ExprState *, ncheck);
		for (int i = 0; i < ncheck; i++)
		{
			Expr	   *checkconstr;

			/* 跳过未被强制执行的约束 */
			if (!check[i].ccenforced)
				continue;

			checkconstr = stringToNode(check[i].ccbin);
			checkconstr = (Expr *) expand_generated_columns_in_expr((Node *) checkconstr, rel, 1);
			resultRelInfo->ri_CheckConstraintExprs[i] =
				ExecPrepareExpr(checkconstr, estate);
		}
		MemoryContextSwitchTo(oldContext);
	}

	/*
	 * 我们将使用 EState 的每元组上下文来求值约束表达式
	 * （如果它尚不存在则创建）。
	 */
	econtext = GetPerTupleExprContext(estate);

	/* 将 econtext 的扫描元组安排为待测元组 */
	econtext->ecxt_scantuple = slot;

	/* 然后计算约束 */
	for (int i = 0; i < ncheck; i++)
	{
		ExprState  *checkconstr = resultRelInfo->ri_CheckConstraintExprs[i];

		/*
		 * 注意：SQL 规定约束表达式返回 NULL 不应被视为失败。因此，
		 * 使用 ExecCheck 而非 ExecQual。
		 */
		if (checkconstr && !ExecCheck(checkconstr, econtext))
			return check[i].ccname;
	}

	/* NULL 结果表示没有错误 */
	return NULL;
}

/*
 * ExecPartitionCheck --- 检查元组是否满足分区约束。
 *
 * 若满足分区约束则返回 true。如果约束失败且我们要求报错，则报错并不返回；
 * 否则返回 false。
 */
bool
ExecPartitionCheck(ResultRelInfo *resultRelInfo, TupleTableSlot *slot,
				   EState *estate, bool emitError)
{
	ExprContext *econtext;
	bool		success;

	/*
	 * 如果第一次经过，则为分区检查表达式构建表达式状态树。
	 * （在一个极端情况下，分区检查表达式为空——即只有一个默认分区、
	 * 没有其它分区——我们会被骗得每次都执行这段代码。但在那种情况下
	 * 它相当廉价，因此我们并不担心。）
	 */
	if (resultRelInfo->ri_PartitionCheckExpr == NULL)
	{
		/*
		 * 确保 qual 树与预处理后的表达式位于查询生命周期的内存上下文中。
		 */
		MemoryContext oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);
		List	   *qual = RelationGetPartitionQual(resultRelInfo->ri_RelationDesc);

		resultRelInfo->ri_PartitionCheckExpr = ExecPrepareCheck(qual, estate);
		MemoryContextSwitchTo(oldcxt);
	}

	/*
	 * 我们将使用 EState 的每元组上下文来求值约束表达式
	 * （如果它尚不存在则创建）。
	 */
	econtext = GetPerTupleExprContext(estate);

	/* 将 econtext 的扫描元组安排为待测元组 */
	econtext->ecxt_scantuple = slot;

	/*
	 * 与目录化约束的情况一样，我们在这里将 NULL 结果视为成功，而非失败。
	 */
	success = ExecCheck(resultRelInfo->ri_PartitionCheckExpr, econtext);

	/* 如果要求报错，则失败时实际上不返回 */
	if (!success && emitError)
		ExecPartitionCheckEmitError(resultRelInfo, slot, estate);

	return success;
}

/*
 * ExecPartitionCheckEmitError - 在分区约束检查失败后，构造并发出错误消息。
 */
void
ExecPartitionCheckEmitError(ResultRelInfo *resultRelInfo,
							TupleTableSlot *slot,
							EState *estate)
{
	Oid			root_relid;
	TupleDesc	tupdesc;
	char	   *val_desc;
	Bitmapset  *modifiedCols;

	/*
	 * 如果元组已经被路由过，它会被转换为分区的行类型，这可能与根表的
	 * 行类型不同。我们必须把它转换回根表的行类型，以便错误消息中的
	 * val_desc 与输入元组相匹配。
	 */
	if (resultRelInfo->ri_RootResultRelInfo)
	{
		ResultRelInfo *rootrel = resultRelInfo->ri_RootResultRelInfo;
		TupleDesc	old_tupdesc;
		AttrMap    *map;

		root_relid = RelationGetRelid(rootrel->ri_RelationDesc);
		tupdesc = RelationGetDescr(rootrel->ri_RelationDesc);

		old_tupdesc = RelationGetDescr(resultRelInfo->ri_RelationDesc);
		/* 反向映射 */
		map = build_attrmap_by_name_if_req(old_tupdesc, tupdesc, false);

		/*
		 * 特定于分区的 slot 的 tupdesc 无法更改，因此分配一个新的。
		 */
		if (map != NULL)
			slot = execute_attr_map_slot(map, slot,
										 MakeTupleTableSlot(tupdesc, &TTSOpsVirtual));
		modifiedCols = bms_union(ExecGetInsertedCols(rootrel, estate),
								 ExecGetUpdatedCols(rootrel, estate));
	}
	else
	{
		root_relid = RelationGetRelid(resultRelInfo->ri_RelationDesc);
		tupdesc = RelationGetDescr(resultRelInfo->ri_RelationDesc);
		modifiedCols = bms_union(ExecGetInsertedCols(resultRelInfo, estate),
								 ExecGetUpdatedCols(resultRelInfo, estate));
	}

	val_desc = ExecBuildSlotValueDescription(root_relid,
											 slot,
											 tupdesc,
											 modifiedCols,
											 64);
	ereport(ERROR,
			(errcode(ERRCODE_CHECK_VIOLATION),
			 errmsg("new row for relation \"%s\" violates partition constraint",
					RelationGetRelationName(resultRelInfo->ri_RelationDesc)),
			 val_desc ? errdetail("Failing row contains %s.", val_desc) : 0,
			 errtable(resultRelInfo->ri_RelationDesc)));
}

/*
 * ExecConstraints - 检查 'slot' 中元组的约束
 *
 * 这会检查传统的 NOT NULL 约束与检查约束。
 *
 * 分区约束 *不* 在此检查。
 *
 * 注意：'slot' 中包含待检查约束的元组，它可能是元组路由之后
 * 从原始输入元组转换而来的。'resultRelInfo' 是元组路由之后的
 * 最终结果关系。
 */
void
ExecConstraints(ResultRelInfo *resultRelInfo,
				TupleTableSlot *slot, EState *estate)
{
	Relation	rel = resultRelInfo->ri_RelationDesc;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	TupleConstr *constr = tupdesc->constr;
	Bitmapset  *modifiedCols;
	List	   *notnull_virtual_attrs = NIL;

	Assert(constr);				/* 否则我们不应被调用 */

	/*
	 * 校验 NOT NULL 约束。
	 *
	 * 虚拟生成列上的 NOT NULL 约束被单独收集并在下方另行检查。
	 */
	if (constr->has_not_null)
	{
		for (AttrNumber attnum = 1; attnum <= tupdesc->natts; attnum++)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, attnum - 1);

			if (att->attnotnull && att->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
				notnull_virtual_attrs = lappend_int(notnull_virtual_attrs, attnum);
			else if (att->attnotnull && slot_attisnull(slot, attnum))
				ReportNotNullViolationError(resultRelInfo, slot, estate, attnum);
		}
	}

	/*
	 * 若有的话，校验虚拟生成列上的 NOT NULL 约束。
	 */
	if (notnull_virtual_attrs)
	{
		AttrNumber	attnum;

		attnum = ExecRelGenVirtualNotNull(resultRelInfo, slot, estate,
										  notnull_virtual_attrs);
		if (attnum != InvalidAttrNumber)
			ReportNotNullViolationError(resultRelInfo, slot, estate, attnum);
	}

	/*
	 * 校验检查约束。
	 */
	if (rel->rd_rel->relchecks > 0)
	{
		const char *failed;

		if ((failed = ExecRelCheck(resultRelInfo, slot, estate)) != NULL)
		{
			char	   *val_desc;
			Relation	orig_rel = rel;

		/*
		 * 如果元组已经被路由过，它会被转换为分区的行类型，这可能与
		 * 根表的行类型不同。我们必须把它转换回根表的行类型，以便
		 * 错误消息中显示的 val_desc 与输入元组相匹配。
		 */
			if (resultRelInfo->ri_RootResultRelInfo)
			{
				ResultRelInfo *rootrel = resultRelInfo->ri_RootResultRelInfo;
				TupleDesc	old_tupdesc = RelationGetDescr(rel);
				AttrMap    *map;

				tupdesc = RelationGetDescr(rootrel->ri_RelationDesc);
				/* 反向映射 */
			map = build_attrmap_by_name_if_req(old_tupdesc,
											   tupdesc,
											   false);

			/*
			 * 特定于分区的 slot 的 tupdesc 无法更改，因此分配一个新的。
			 */
			if (map != NULL)
					slot = execute_attr_map_slot(map, slot,
												 MakeTupleTableSlot(tupdesc, &TTSOpsVirtual));
				modifiedCols = bms_union(ExecGetInsertedCols(rootrel, estate),
										 ExecGetUpdatedCols(rootrel, estate));
				rel = rootrel->ri_RelationDesc;
			}
			else
				modifiedCols = bms_union(ExecGetInsertedCols(resultRelInfo, estate),
										 ExecGetUpdatedCols(resultRelInfo, estate));
			val_desc = ExecBuildSlotValueDescription(RelationGetRelid(rel),
													 slot,
													 tupdesc,
													 modifiedCols,
													 64);
			ereport(ERROR,
					(errcode(ERRCODE_CHECK_VIOLATION),
					 errmsg("new row for relation \"%s\" violates check constraint \"%s\"",
							RelationGetRelationName(orig_rel), failed),
					 val_desc ? errdetail("Failing row contains %s.", val_desc) : 0,
					 errtableconstraint(orig_rel, failed)));
		}
	}
}

/*
 * 校验给定元组 slot 上虚拟生成列的 NOT NULL 约束。
 *
 * 返回值为 InvalidAttrNumber 表示所有虚拟生成列上的 NOT NULL 约束
 * 都得到满足。返回值 > 0 表示该属性发生了 NOT NULL 违例。
 *
 * notnull_virtual_attrs 是带有 NOT NULL 约束的虚拟生成列的 attnum 列表。
 */
AttrNumber
ExecRelGenVirtualNotNull(ResultRelInfo *resultRelInfo, TupleTableSlot *slot,
						 EState *estate, List *notnull_virtual_attrs)
{
	Relation	rel = resultRelInfo->ri_RelationDesc;
	ExprContext *econtext;
	MemoryContext oldContext;

	/*
	 * 我们的实现方式是：为每个虚拟生成列构建一个 NullTest 节点，
	 * 缓存在 resultRelInfo 中，然后让它们通过 ExecCheck() 求值。
	 */
	if (resultRelInfo->ri_GenVirtualNotNullConstraintExprs == NULL)
	{
		oldContext = MemoryContextSwitchTo(estate->es_query_cxt);
		resultRelInfo->ri_GenVirtualNotNullConstraintExprs =
			palloc0_array(ExprState *, list_length(notnull_virtual_attrs));

		foreach_int(attnum, notnull_virtual_attrs)
		{
			int			i = foreach_current_index(attnum);
			NullTest   *nnulltest;

			/* “generated_expression IS NOT NULL” 检查。 */
			nnulltest = makeNode(NullTest);
			nnulltest->arg = (Expr *) build_generation_expression(rel, attnum);
			nnulltest->nulltesttype = IS_NOT_NULL;
			nnulltest->argisrow = false;
			nnulltest->location = -1;

			resultRelInfo->ri_GenVirtualNotNullConstraintExprs[i] =
				ExecPrepareExpr((Expr *) nnulltest, estate);
		}
		MemoryContextSwitchTo(oldContext);
	}

	/*
	 * 我们将使用 EState 的每元组上下文来求值虚拟生成列 NOT NULL 约束
	 * 表达式（如果它尚不存在则创建）。
	 */
	econtext = GetPerTupleExprContext(estate);

	/* 将 econtext 的扫描元组安排为待测元组 */
	econtext->ecxt_scantuple = slot;

	/* 然后计算虚拟生成列的 NOT NULL 约束 */
	foreach_int(attnum, notnull_virtual_attrs)
	{
		int			i = foreach_current_index(attnum);
		ExprState  *exprstate = resultRelInfo->ri_GenVirtualNotNullConstraintExprs[i];

		Assert(exprstate != NULL);
		if (!ExecCheck(exprstate, econtext))
			return attnum;
	}

	/* InvalidAttrNumber 结果表示没有错误 */
	return InvalidAttrNumber;
}

/*
 * 报告一个已经被检测到的 NOT NULL 约束违例。
 */
static void
ReportNotNullViolationError(ResultRelInfo *resultRelInfo, TupleTableSlot *slot,
							EState *estate, int attnum)
{
	Bitmapset  *modifiedCols;
	char	   *val_desc;
	Relation	rel = resultRelInfo->ri_RelationDesc;
	Relation	orig_rel = rel;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	TupleDesc	orig_tupdesc = RelationGetDescr(rel);
	Form_pg_attribute att = TupleDescAttr(tupdesc, attnum - 1);

	Assert(attnum > 0);

	/*
	 * 如果元组已经被路由过，它会被转换为分区的行类型，这可能与
	 * 根表的行类型不同。我们必须把它转换回根表的行类型，以便
	 * 错误消息中显示的 val_desc 与输入元组相匹配。
	 */
	if (resultRelInfo->ri_RootResultRelInfo)
	{
		ResultRelInfo *rootrel = resultRelInfo->ri_RootResultRelInfo;
		AttrMap    *map;

		tupdesc = RelationGetDescr(rootrel->ri_RelationDesc);
		/* 反向映射 */
		map = build_attrmap_by_name_if_req(orig_tupdesc,
										   tupdesc,
										   false);

		/*
		 * 特定于分区的 slot 的 tupdesc 无法更改，因此分配一个新的。
		 */
		if (map != NULL)
			slot = execute_attr_map_slot(map, slot,
										 MakeTupleTableSlot(tupdesc, &TTSOpsVirtual));
		modifiedCols = bms_union(ExecGetInsertedCols(rootrel, estate),
								 ExecGetUpdatedCols(rootrel, estate));
		rel = rootrel->ri_RelationDesc;
	}
	else
		modifiedCols = bms_union(ExecGetInsertedCols(resultRelInfo, estate),
								 ExecGetUpdatedCols(resultRelInfo, estate));

	val_desc = ExecBuildSlotValueDescription(RelationGetRelid(rel),
											 slot,
											 tupdesc,
											 modifiedCols,
											 64);
	ereport(ERROR,
			errcode(ERRCODE_NOT_NULL_VIOLATION),
			errmsg("null value in column \"%s\" of relation \"%s\" violates not-null constraint",
				   NameStr(att->attname),
				   RelationGetRelationName(orig_rel)),
			val_desc ? errdetail("Failing row contains %s.", val_desc) : 0,
			errtablecol(orig_rel, attnum));
}

/*
 * ExecWithCheckOptions -- 检查元组是否满足指定种类的任何 WITH CHECK OPTION。
 *
 * 注意，这可能需要被调用多次，以确保所有种类的
 * 处理 WITH CHECK OPTION（既包括设置了 WITH CHECK OPTION 的视图，
 * 也包括行级安全策略）。参见 ExecInsert() 与 ExecUpdate()。
 */
void
ExecWithCheckOptions(WCOKind kind, ResultRelInfo *resultRelInfo,
					 TupleTableSlot *slot, EState *estate)
{
	Relation	rel = resultRelInfo->ri_RelationDesc;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	ExprContext *econtext;
	ListCell   *l1,
			   *l2;

	/*
	 * 我们将使用 EState 的每元组上下文来求值约束表达式
	 * （如果它尚不存在则创建）。
	 */
	econtext = GetPerTupleExprContext(estate);

	/* 将 econtext 的扫描元组安排为待测元组 */
	econtext->ecxt_scantuple = slot;

	/* 检查每一个约束 */
	forboth(l1, resultRelInfo->ri_WithCheckOptions,
			l2, resultRelInfo->ri_WithCheckOptionExprs)
	{
		WithCheckOption *wco = (WithCheckOption *) lfirst(l1);
		ExprState  *wcoExpr = (ExprState *) lfirst(l2);

		/*
		 * 跳过任何当前我们并不在寻找的那种 WCO。
		 */
		if (wco->kind != kind)
			continue;

		/*
		 * WITH CHECK OPTION 检查旨在确保新元组是可见的（对于视图而言），
		 * 或者能够通过“with-check”策略（对于行级安全而言）。如果 qual
		 * 求值为 NULL 或 FALSE，那么新元组就不会被包含在视图中，
		 * 或者通不过该表的“with-check”策略。
		 */
		if (!ExecQual(wcoExpr, econtext))
		{
			char	   *val_desc;
			Bitmapset  *modifiedCols;

			switch (wco->kind)
			{
				/*
				 * 对于来自视图的 WITH CHECK OPTION，我们可能能够提供该行的
				 * 详细信息，这取决于对该关系的权限（即，如果用户本来就能够
				 * 直接查看它的话）。对于 RLS 违例，我们不包含数据，因为我们
				 * 不知道用户是否应该能够查看该元组，因为这取决于 USING
				 * 策略。
				 */
				case WCO_VIEW_CHECK:
					/* 参见 ExecConstraints() 中的注释。 */
					if (resultRelInfo->ri_RootResultRelInfo)
					{
						ResultRelInfo *rootrel = resultRelInfo->ri_RootResultRelInfo;
						TupleDesc	old_tupdesc = RelationGetDescr(rel);
						AttrMap    *map;

						tupdesc = RelationGetDescr(rootrel->ri_RelationDesc);
						/* 反向映射 */
						map = build_attrmap_by_name_if_req(old_tupdesc,
														   tupdesc,
														   false);

					/*
					 * 特定于分区的 slot 的 tupdesc 无法更改，因此分配一个新的。
					 */
						if (map != NULL)
							slot = execute_attr_map_slot(map, slot,
														 MakeTupleTableSlot(tupdesc, &TTSOpsVirtual));

						modifiedCols = bms_union(ExecGetInsertedCols(rootrel, estate),
												 ExecGetUpdatedCols(rootrel, estate));
						rel = rootrel->ri_RelationDesc;
					}
					else
						modifiedCols = bms_union(ExecGetInsertedCols(resultRelInfo, estate),
												 ExecGetUpdatedCols(resultRelInfo, estate));
					val_desc = ExecBuildSlotValueDescription(RelationGetRelid(rel),
															 slot,
															 tupdesc,
															 modifiedCols,
															 64);

					ereport(ERROR,
							(errcode(ERRCODE_WITH_CHECK_OPTION_VIOLATION),
							 errmsg("new row violates check option for view \"%s\"",
									wco->relname),
							 val_desc ? errdetail("Failing row contains %s.",
												  val_desc) : 0));
					break;
				case WCO_RLS_INSERT_CHECK:
				case WCO_RLS_UPDATE_CHECK:
					if (wco->polname != NULL)
						ereport(ERROR,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
								 errmsg("new row violates row-level security policy \"%s\" for table \"%s\"",
										wco->polname, wco->relname)));
					else
						ereport(ERROR,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
								 errmsg("new row violates row-level security policy for table \"%s\"",
										wco->relname)));
					break;
				case WCO_RLS_MERGE_UPDATE_CHECK:
				case WCO_RLS_MERGE_DELETE_CHECK:
					if (wco->polname != NULL)
						ereport(ERROR,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
								 errmsg("target row violates row-level security policy \"%s\" (USING expression) for table \"%s\"",
										wco->polname, wco->relname)));
					else
						ereport(ERROR,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
								 errmsg("target row violates row-level security policy (USING expression) for table \"%s\"",
										wco->relname)));
					break;
				case WCO_RLS_CONFLICT_CHECK:
					if (wco->polname != NULL)
						ereport(ERROR,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
								 errmsg("new row violates row-level security policy \"%s\" (USING expression) for table \"%s\"",
										wco->polname, wco->relname)));
					else
						ereport(ERROR,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
								 errmsg("new row violates row-level security policy (USING expression) for table \"%s\"",
										wco->relname)));
					break;
				default:
					elog(ERROR, "unrecognized WCO kind: %u", wco->kind);
					break;
			}
		}
	}
}

/*
 * ExecBuildSlotValueDescription -- 构造一个表示元组的字符串
 *
 * 这有意与 BuildIndexValueDescription 非常相似，但与那个函数不同，
 * 我们会截断过长的字段值（最多 maxfieldlen 字节）。这在此处似乎是必要的，
 * 因为堆字段值可能非常长，而索引项通常不会那么宽。
 *
 * 此外，与索引项的情况不同，我们需要准备好忽略已删除的列。我们曾经使用
 * slot 的元组描述符来解码数据，但 slot 的描述符无法标识已删除的列，
 * 因此现在需要我们传入关系的描述符。
 *
 * 注意：与 BuildIndexValueDescription 一样，如果用户没有权限查看所涉及的
 * 任何列，则返回 NULL。与 BuildIndexValueDescription 不同的是，如果用户
 * 有权查看所涉及列的一个子集，则会返回该子集，并以一个键标识它们分别是哪些列。
 */
char *
ExecBuildSlotValueDescription(Oid reloid,
							  TupleTableSlot *slot,
							  TupleDesc tupdesc,
							  Bitmapset *modifiedCols,
							  int maxfieldlen)
{
	StringInfoData buf;
	StringInfoData collist;
	bool		write_comma = false;
	bool		write_comma_collist = false;
	int			i;
	AclResult	aclresult;
	bool		table_perm = false;
	bool		any_perm = false;

	/*
	 * 检查 RLS 是否对关系启用且应当处于活动状态；若是，则不返回任何内容。
	 * 否则，走正常的权限检查。
	 */
	if (check_enable_rls(reloid, InvalidOid, true) == RLS_ENABLED)
		return NULL;

	initStringInfo(&buf);

	appendStringInfoChar(&buf, '(');

	/*
	 * 检查用户是否有权限查看该行。表级 SELECT 允许访问所有列。
	 * 如果用户没有表级 SELECT，我们就逐个检查各列，并把用户拥有 SELECT
	 * 权限的那些列包含进来。此外，我们总是包含用户提供过数据的列。
	 */
	aclresult = pg_class_aclcheck(reloid, GetUserId(), ACL_SELECT);
	if (aclresult != ACLCHECK_OK)
	{
		/* 为列列表设置缓冲区 */
		initStringInfo(&collist);
		appendStringInfoChar(&collist, '(');
	}
	else
		table_perm = any_perm = true;

	/* 确保元组已被完全解构 */
	slot_getallattrs(slot);

	for (i = 0; i < tupdesc->natts; i++)
	{
		bool		column_perm = false;
		char	   *val;
		int			vallen;
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		/* 忽略已删除的列 */
		if (att->attisdropped)
			continue;

		if (!table_perm)
		{
			/*
			 * 没有表级 SELECT，因此需要确保他们要么对该列拥有 SELECT 权限，
			 * 要么为该列提供了数据。否则，就从错误消息中省略此列。
			 */
			aclresult = pg_attribute_aclcheck(reloid, att->attnum,
											  GetUserId(), ACL_SELECT);
			if (bms_is_member(att->attnum - FirstLowInvalidHeapAttributeNumber,
							  modifiedCols) || aclresult == ACLCHECK_OK)
			{
				column_perm = any_perm = true;

				if (write_comma_collist)
					appendStringInfoString(&collist, ", ");
				else
					write_comma_collist = true;

				appendStringInfoString(&collist, NameStr(att->attname));
			}
		}

		if (table_perm || column_perm)
		{
			if (att->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
				val = "virtual";
			else if (slot->tts_isnull[i])
				val = "null";
			else
			{
				Oid			foutoid;
				bool		typisvarlena;

				getTypeOutputInfo(att->atttypid,
								  &foutoid, &typisvarlena);
				val = OidOutputFunctionCall(foutoid, slot->tts_values[i]);
			}

			if (write_comma)
				appendStringInfoString(&buf, ", ");
			else
				write_comma = true;

			/* 必要时截断 */
			vallen = strlen(val);
			if (vallen <= maxfieldlen)
				appendBinaryStringInfo(&buf, val, vallen);
			else
			{
				vallen = pg_mbcliplen(val, vallen, maxfieldlen);
				appendBinaryStringInfo(&buf, val, vallen);
				appendStringInfoString(&buf, "...");
			}
		}
	}

	/* 如果最终没有要返回的列，则返回 NULL。 */
	if (!any_perm)
		return NULL;

	appendStringInfoChar(&buf, ')');

	if (!table_perm)
	{
		appendStringInfoString(&collist, ") = ");
		appendBinaryStringInfo(&collist, buf.data, buf.len);

		return collist.data;
	}

	return buf.data;
}


/*
 * ExecUpdateLockMode -- 为给定的 ResultRelInfo 找到合适的 UPDATE 元组锁模式
 */
LockTupleMode
ExecUpdateLockMode(EState *estate, ResultRelInfo *relinfo)
{
	Bitmapset  *keyCols;
	Bitmapset  *updatedCols;

	/*
	 * 计算要使用的锁模式。如果属于键的列未被修改，我们就能使用较弱的锁，
	 * 从而获得更好的并发性。
	 */
	updatedCols = ExecGetAllUpdatedCols(relinfo, estate);
	keyCols = RelationGetIndexAttrBitmap(relinfo->ri_RelationDesc,
										 INDEX_ATTR_BITMAP_KEY);

	if (bms_overlap(keyCols, updatedCols))
		return LockTupleExclusive;

	return LockTupleNoKeyExclusive;
}

/*
 * ExecFindRowMark -- 为给定的范围表索引查找对应的 ExecRowMark 结构
 *
 * 若不存在这样的结构，则根据 missing_ok 返回 NULL 或抛出错误
 */
ExecRowMark *
ExecFindRowMark(EState *estate, Index rti, bool missing_ok)
{
	if (rti > 0 && rti <= estate->es_range_table_size &&
		estate->es_rowmarks != NULL)
	{
		ExecRowMark *erm = estate->es_rowmarks[rti - 1];

		if (erm)
			return erm;
	}
	if (!missing_ok)
		elog(ERROR, "failed to find ExecRowMark for rangetable index %u", rti);
	return NULL;
}

/*
 * ExecBuildAuxRowMark -- 创建一个 ExecAuxRowMark 结构
 *
 * 输入是底层的 ExecRowMark 结构和输入计划节点的目标列表
 * （注意不是 planstate 节点！）。我们需要后者来查明 resjunk 列的列号。
 */
ExecAuxRowMark *
ExecBuildAuxRowMark(ExecRowMark *erm, List *targetlist)
{
	ExecAuxRowMark *aerm = (ExecAuxRowMark *) palloc0(sizeof(ExecAuxRowMark));
	char		resname[32];

	aerm->rowmark = erm;

	/* 查找与此行标记相关联的 resjunk 列 */
	if (erm->markType != ROW_MARK_COPY)
	{
		/* 除 COPY 之外所有方法都需要 ctid */
		snprintf(resname, sizeof(resname), "ctid%u", erm->rowmarkId);
		aerm->ctidAttNo = ExecFindJunkAttributeInTlist(targetlist,
													   resname);
		if (!AttributeNumberIsValid(aerm->ctidAttNo))
			elog(ERROR, "could not find junk %s column", resname);
	}
	else
	{
		/* 若为 COPY，则需要 wholerow */
		snprintf(resname, sizeof(resname), "wholerow%u", erm->rowmarkId);
		aerm->wholeAttNo = ExecFindJunkAttributeInTlist(targetlist,
														resname);
		if (!AttributeNumberIsValid(aerm->wholeAttNo))
			elog(ERROR, "could not find junk %s column", resname);
	}

	/* 若为子关系，则需要 tableoid */
	if (erm->rti != erm->prti)
	{
		snprintf(resname, sizeof(resname), "tableoid%u", erm->rowmarkId);
		aerm->toidAttNo = ExecFindJunkAttributeInTlist(targetlist,
													   resname);
		if (!AttributeNumberIsValid(aerm->toidAttNo))
			elog(ERROR, "could not find junk %s column", resname);
	}

	return aerm;
}


/*
 * EvalPlanQual 逻辑 --- 重检查被修改的元组，判断我们是否要在
 * READ COMMITTED 规则下处理其更新后的版本。
 *
 * 关于其工作原理的更多信息，请参见 backend/executor/README。
 */


/*
 * 检查元组的更新版本，判断我们是否要在 READ COMMITTED 规则下处理它。
 *
 *	epqstate - 用于 EvalPlanQual 重检查的 EPQ 状态
 *	relation - 包含该元组的关系
 *	rti - 包含该元组的关系在范围表中的索引
 *	inputslot - 待处理的元组——为提高效率，它也可以是本关系的
 *		EvalPlanQualSlot() 返回的 slot。
 *
 * 本函数测试 inputslot 中的元组是否仍然匹配相关的 quals。为了让结果
 * 有意义，输入元组通常必须是最后一行版本（否则结果意义不大），并且必须
 * 被加锁（否则结果可能已过时）。这通常是通过使用带有
 * TUPLE_LOCK_FLAG_FIND_LAST_VERSION 标志的 table_tuple_lock() 实现的。
 *
 * 返回一个包含新的候选 update/delete 元组的 slot，或者当我们判定
 * 不应处理该行时返回 NULL。
 */
TupleTableSlot *
EvalPlanQual(EPQState *epqstate, Relation relation,
			 Index rti, TupleTableSlot *inputslot)
{
	TupleTableSlot *slot;
	TupleTableSlot *testslot;

	Assert(rti > 0);

	/*
	 * 需要运行一个重检查子查询。初始化或重新初始化 EPQ 状态。
	 */
	EvalPlanQualBegin(epqstate);

	/*
	 * 调用方通常会使用 EvalPlanQualSlot 来存储元组，以避免一次不必要的拷贝。
	 */
	testslot = EvalPlanQualSlot(epqstate, relation, rti);
	if (testslot != inputslot)
		ExecCopySlot(testslot, inputslot);

	/*
	 * 标记此关系有一个 EPQ 元组可用。（如果有多个结果关系，其他的
	 * 仍然标记为没有可用元组。）
	 */
	epqstate->relsubs_done[rti - 1] = false;
	epqstate->relsubs_blocked[rti - 1] = false;

	/*
	 * 运行 EPQ 查询。我们假设它最多会返回一行元组。
	 */
	slot = EvalPlanQualNext(epqstate);

	/*
	 * 如果我们得到了一个元组，强制该 slot 物化该元组，使它不依赖于 EPQ 查询
	 * 中的任何本地状态（特别是，该 slot 极有可能包含对 copyTuple 中可能存在的
	 * 任何按引用传递的 datum 的引用）。与下一步一样，这样做是为了防止
	 * EPQ 查询被过早重用。
	 */
	if (!TupIsNull(slot))
		ExecMaterializeSlot(slot);

	/*
	 * 清除测试元组，并标记此处没有可用元组。这是为了防止 EPQ 状态被重用于
	 * 测试另一个目标关系的元组时而需要做的。
	 */
	ExecClearTuple(testslot);
	epqstate->relsubs_blocked[rti - 1] = true;

	return slot;
}

/*
 * EvalPlanQualInit -- 在创建可能需要调用 EPQ 处理的计划状态节点时初始化。
 *
 * 如果调用方打算使用 EvalPlanQual()，那么 resultRelations 应当是一个
 * 潜在目标关系的 RT 索引列表，我们会安排让其他被列出的关系在
 * EvalPlanQual() 调用期间不返回任何元组。否则 resultRelations 应为 NIL。
 *
 * 注意：subplan/auxrowmarks 可以是 NULL/NIL，如果它们将在稍后通过
 * EvalPlanQualSetPlan 设置的话。
 */
void
EvalPlanQualInit(EPQState *epqstate, EState *parentestate,
				 Plan *subplan, List *auxrowmarks,
				 int epqParam, List *resultRelations)
{
	Index		rtsize = parentestate->es_range_table_size;

	/* 初始化在 EPQState 生命周期内不会改变的数据 */
	epqstate->parentestate = parentestate;
	epqstate->epqParam = epqParam;
	epqstate->resultRelations = resultRelations;

	/*
	 * 为每个潜在的 rti 分配用于引用 slot 的空间——现在就做，而不是像其他
	 * 动态分配的资源那样放到 EvalPlanQualBegin() 中去做，这样
	 * EvalPlanQualSlot() 就可以用来保存那些*可能*稍后需要 EPQ 的元组，
	 * 而不必强制付出 EvalPlanQualBegin() 的开销。
	 */
	epqstate->tuple_table = NIL;
	epqstate->relsubs_slot = (TupleTableSlot **)
		palloc0(rtsize * sizeof(TupleTableSlot *));

	/* ... 并记住 EvalPlanQualBegin 将需要的数据 */
	epqstate->plan = subplan;
	epqstate->arowMarks = auxrowmarks;

	/* ... 并将 EPQ 状态标记为不活跃 */
	epqstate->origslot = NULL;
	epqstate->recheckestate = NULL;
	epqstate->recheckplanstate = NULL;
	epqstate->relsubs_rowmark = NULL;
	epqstate->relsubs_done = NULL;
	epqstate->relsubs_blocked = NULL;
}

/*
 * EvalPlanQualSetPlan -- 设置或更改一个 EPQState 的子计划。
 *
 * 我们以前需要它来让 ModifyTable 能够处理多个子计划。现在它可以被
 * 重构以消除。
 */
void
EvalPlanQualSetPlan(EPQState *epqstate, Plan *subplan, List *auxrowmarks)
{
	/* 如果我们有一个活跃的 EPQ 查询，则将其关闭 */
	EvalPlanQualEnd(epqstate);
	/* 然后设置/更改计划指针 */
	epqstate->plan = subplan;
	/* 行标记同样依赖于计划 */
	epqstate->arowMarks = auxrowmarks;
}

/*
 * 返回（并在必要时创建）一个用于 EPQ 测试元组的 slot。
 *
 * 注意这只需要已调用过 EvalPlanQualInit() 即可，并不需要
 * EvalPlanQualBegin()。
 */
TupleTableSlot *
EvalPlanQualSlot(EPQState *epqstate,
				 Relation relation, Index rti)
{
	TupleTableSlot **slot;

	Assert(relation);
	Assert(rti > 0 && rti <= epqstate->parentestate->es_range_table_size);
	slot = &epqstate->relsubs_slot[rti - 1];

	if (*slot == NULL)
	{
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(epqstate->parentestate->es_query_cxt);
		*slot = table_slot_create(relation, &epqstate->tuple_table);
		MemoryContextSwitchTo(oldcontext);
	}

	return *slot;
}

/*
 * 获取一个由 rti 标识、且需要被 EvalPlanQual 操作扫描的非锁定关系的
 * 当前行值。origslot 必须已被设为包含我们需要重查的当前结果行
 * （顶层行）。如果找到了替换元组则返回 true，否则返回 false。
 */
bool
EvalPlanQualFetchRowMark(EPQState *epqstate, Index rti, TupleTableSlot *slot)
{
	ExecAuxRowMark *earm = epqstate->relsubs_rowmark[rti - 1];
	ExecRowMark *erm;
	Datum		datum;
	bool		isNull;

	Assert(earm != NULL);
	Assert(epqstate->origslot != NULL);

	erm = earm->rowmark;

	if (RowMarkRequiresRowShareLock(erm->markType))
		elog(ERROR, "EvalPlanQual doesn't support locking rowmarks");

	/* 若是子关系，则必须检查它是否产生了这一行 */
	if (erm->rti != erm->prti)
	{
		Oid			tableoid;

		datum = ExecGetJunkAttribute(epqstate->origslot,
									 earm->toidAttNo,
									 &isNull);
		/* 未被锁定的关系可能位于外连接的内部 */
		if (isNull)
			return false;

		tableoid = DatumGetObjectId(datum);

		Assert(OidIsValid(erm->relid));
		if (tableoid != erm->relid)
		{
			/* 这个子关系此刻处于不活跃状态 */
			return false;
		}
	}

	if (erm->markType == ROW_MARK_REFERENCE)
	{
		Assert(erm->relation != NULL);

		/* 获取元组的 ctid */
		datum = ExecGetJunkAttribute(epqstate->origslot,
									 earm->ctidAttNo,
									 &isNull);
		/* 未被锁定的关系可能位于外连接的内部 */
		if (isNull)
			return false;

		/* 对外部表的获取请求必须转交给其 FDW */
		if (erm->relation->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		{
			FdwRoutine *fdwroutine;
			bool		updated = false;

			fdwroutine = GetFdwRoutineForRelation(erm->relation, false);
			/* 这本来应该已经被检查过了，但我们还是稳妥起见 */
			if (fdwroutine->RefetchForeignRow == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot lock rows in foreign table \"%s\"",
								RelationGetRelationName(erm->relation))));

			fdwroutine->RefetchForeignRow(epqstate->recheckestate,
										  erm,
										  datum,
										  slot,
										  &updated);
			if (TupIsNull(slot))
				elog(ERROR, "failed to fetch tuple for EvalPlanQual recheck");

			/*
			 * 理想情况下我们会坚持要求 updated == false，但这假设 FDW 能够
			 * 精确跟踪这一点，而它们未必能做到。因此直接忽略该标志。
			 */
			return true;
		}
		else
		{
			/* 普通表，获取元组 */
			if (!table_tuple_fetch_row_version(erm->relation,
											   (ItemPointer) DatumGetPointer(datum),
											   SnapshotAny, slot))
				elog(ERROR, "failed to fetch tuple for EvalPlanQual recheck");
			return true;
		}
	}
	else
	{
		Assert(erm->markType == ROW_MARK_COPY);

		/* 获取关系的整行 Var */
		datum = ExecGetJunkAttribute(epqstate->origslot,
									 earm->wholeAttNo,
									 &isNull);
		/* 未被锁定的关系可能位于外连接的内部 */
		if (isNull)
			return false;

		ExecStoreHeapTupleDatum(datum, slot);
		return true;
	}
}

/*
 * 从 EvalPlanQual 测试中获取下一行（若有）
 *
 * （实际上，应该永远不会有超过一行……）
 */
TupleTableSlot *
EvalPlanQualNext(EPQState *epqstate)
{
	MemoryContext oldcontext;
	TupleTableSlot *slot;

	oldcontext = MemoryContextSwitchTo(epqstate->recheckestate->es_query_cxt);
	slot = ExecProcNode(epqstate->recheckplanstate);
	MemoryContextSwitchTo(oldcontext);

	return slot;
}

/*
 * 初始化或重置一个 EvalPlanQual 状态树
 */
void
EvalPlanQualBegin(EPQState *epqstate)
{
	EState	   *parentestate = epqstate->parentestate;
	EState	   *recheckestate = epqstate->recheckestate;

	if (recheckestate == NULL)
	{
		/* 第一次经过，因此创建一个子 EState */
		EvalPlanQualStart(epqstate, epqstate->plan);
	}
	else
	{
		/*
		 * 我们已经有了一个合适的子 EPQ 树，因此只需重置它。
		 */
		Index		rtsize = parentestate->es_range_table_size;
		PlanState  *rcplanstate = epqstate->recheckplanstate;

		/*
		 * 将 relsubs_done[] 标志重置为与 relsubs_blocked[] 相等，这样 EPQ
		 * 运行就永远不会尝试从被阻塞的目标关系获取元组。
		 */
		memcpy(epqstate->relsubs_done, epqstate->relsubs_blocked,
			   rtsize * sizeof(bool));

		/* 重新复制父参数的当前值 */
		if (parentestate->es_plannedstmt->paramExecTypes != NIL)
		{
			int			i;

			/*
			 * 强制求值任何子计划可能需要的 InitPlan 输出，以防它们自
			 * EvalPlanQualStart 以来被重置（详见其中的注释）。
			 */
			ExecSetParamPlanMulti(rcplanstate->plan->extParam,
								  GetPerTupleExprContext(parentestate));

			i = list_length(parentestate->es_plannedstmt->paramExecTypes);

			while (--i >= 0)
			{
				/* 若有的话复制其值，但不复制 execPlan 链接 */
				recheckestate->es_param_exec_vals[i].value =
					parentestate->es_param_exec_vals[i].value;
				recheckestate->es_param_exec_vals[i].isnull =
					parentestate->es_param_exec_vals[i].isnull;
			}
		}

		/*
		 * 标记子计划树需要在所有扫描节点处重新扫描。首次 ExecProcNode
		 * 将负责真正执行重新扫描。
		 */
		rcplanstate->chgParam = bms_add_member(rcplanstate->chgParam,
											   epqstate->epqParam);
	}
}

/*
 * 启动一个 EvalPlanQual 计划树的执行。
 *
 * 这是 ExecutorStart() 的精简版本：我们从顶层 estate 复制部分状态，
 * 而不是全新初始化。
 */
static void
EvalPlanQualStart(EPQState *epqstate, Plan *planTree)
{
	EState	   *parentestate = epqstate->parentestate;
	Index		rtsize = parentestate->es_range_table_size;
	EState	   *rcestate;
	MemoryContext oldcontext;
	ListCell   *l;

	epqstate->recheckestate = rcestate = CreateExecutorState();

	oldcontext = MemoryContextSwitchTo(rcestate->es_query_cxt);

	/* 标记这是一个用于执行 EPQ 的 EState */
	rcestate->es_epq_active = epqstate;

	/*
	 * 子 EPQ EState 共享父 EState 中那些不变状态（如快照、范围表、
	 * 外部 Param 信息）的副本。它们需要自己拥有本地状态的副本，
	 * 包括元组表、es_param_exec_vals、结果关系信息等。
	 */
	rcestate->es_direction = ForwardScanDirection;
	rcestate->es_snapshot = parentestate->es_snapshot;
	rcestate->es_crosscheck_snapshot = parentestate->es_crosscheck_snapshot;
	rcestate->es_range_table = parentestate->es_range_table;
	rcestate->es_range_table_size = parentestate->es_range_table_size;
	rcestate->es_relations = parentestate->es_relations;
	rcestate->es_rowmarks = parentestate->es_rowmarks;
	rcestate->es_rteperminfos = parentestate->es_rteperminfos;
	rcestate->es_plannedstmt = parentestate->es_plannedstmt;
	rcestate->es_junkFilter = parentestate->es_junkFilter;
	rcestate->es_output_cid = parentestate->es_output_cid;
	rcestate->es_queryEnv = parentestate->es_queryEnv;

	/*
	 * 子计划所需的 ResultRelInfo 在子计划自身被初始化时从零开始构建。
	 */
	rcestate->es_result_relations = NULL;
	/* es_trig_target_relations 绝不能被复制 */
	rcestate->es_top_eflags = parentestate->es_top_eflags;
	rcestate->es_instrument = parentestate->es_instrument;
	/* es_auxmodifytables 绝不能被复制 */

	/*
	 * 外部参数列表只是简单地从父 EState 共享。内部参数工作区必须是本地状态，
	 * 但我们从父 EState 复制其初始值，以便能够访问那些已由父计划树的
	 * 其它部分设置过的参数值。
	 */
	rcestate->es_param_list_info = parentestate->es_param_list_info;
	if (parentestate->es_plannedstmt->paramExecTypes != NIL)
	{
		int			i;

		/*
		 * 强制求值任何子计划可能需要的 InitPlan 输出。（如果更复杂一些，
		 * 也许我们可以推迟到子计划真正需要它们时才求值，但这似乎不值得
		 * 费劲；这已经是一个极端情况了，因为通常 InitPlan 会在到达
		 * EvalPlanQual 之前就被求值。）
		 *
		 * 这不会触碰那些出现在子计划树某处的 InitPlan 的输出参数，只处理
		 * 附加在 ModifyTable 节点或其之上、且在子计划中被引用的那些。
		 * 不过这没有问题，因为规划器只会把此类 InitPlan 附加到较低层级的
		 * SubqueryScan 节点上，而 EPQ 执行不会深入到 SubqueryScan 内部。
		 *
		 * EState 的每输出元组 econtext 生命周期足够短，因为在进行下一次
		 * EvalPlanQual 之前，它应该会被重置。
		 */
		ExecSetParamPlanMulti(planTree->extParam,
							  GetPerTupleExprContext(parentestate));

		/* 现在创建内部参数工作区…… */
		i = list_length(parentestate->es_plannedstmt->paramExecTypes);
		rcestate->es_param_exec_vals = (ParamExecData *)
			palloc0(i * sizeof(ParamExecData));
		/* ……并复制所有的值，无论是否真正需要 */
		while (--i >= 0)
		{
			/* 若有的话复制其值，但不复制 execPlan 链接 */
			rcestate->es_param_exec_vals[i].value =
				parentestate->es_param_exec_vals[i].value;
			rcestate->es_param_exec_vals[i].isnull =
				parentestate->es_param_exec_vals[i].isnull;
		}
	}

	/*
	 * 复制 es_unpruned_relids，以便下方初始化计划树时，被裁剪的关系会被
	 * ExecInitLockRows() 和 ExecInitModifyTable() 忽略。
	 */
	rcestate->es_unpruned_relids = parentestate->es_unpruned_relids;

	/*
	 * 同时让 PartitionPruneInfo 与裁剪结果可用。它们必须完全匹配，
	 * 这样我们才能像父计划那样初始化完全相同的 Append 与 MergeAppend 子计划。
	 */
	rcestate->es_part_prune_infos = parentestate->es_part_prune_infos;
	rcestate->es_part_prune_states = parentestate->es_part_prune_states;
	rcestate->es_part_prune_results = parentestate->es_part_prune_results;

	/* 我们还会从父状态借用 es_partition_directory */
	rcestate->es_partition_directory = parentestate->es_partition_directory;

	/*
	 * 为每个 SubPlan 初始化私有状态信息。我们必须在对主查询树运行
	 * ExecInitNode 之前完成，因为 ExecInitSubPlan 期望能找到这些条目。
	 * 某些 SubPlan 可能不会被用于我们打算运行的那部分计划树，但由于难以
	 * 判断是哪些，我们索性把它们全部初始化。
	 */
	Assert(rcestate->es_subplanstates == NIL);
	foreach(l, parentestate->es_plannedstmt->subplans)
	{
		Plan	   *subplan = (Plan *) lfirst(l);
		PlanState  *subplanstate;

		subplanstate = ExecInitNode(subplan, rcestate, 0);
		rcestate->es_subplanstates = lappend(rcestate->es_subplanstates,
											 subplanstate);
	}

	/*
	 * 构建一个以 RTI 为索引的行标记数组，以便 EvalPlanQualFetchRowMark()
	 * 能够高效地访问待获取的 rowmark。
	 */
	epqstate->relsubs_rowmark = (ExecAuxRowMark **)
		palloc0(rtsize * sizeof(ExecAuxRowMark *));
	foreach(l, epqstate->arowMarks)
	{
		ExecAuxRowMark *earm = (ExecAuxRowMark *) lfirst(l);

		epqstate->relsubs_rowmark[earm->rowmark->rti - 1] = earm;
	}

	/*
	 * 初始化每个关系的 EPQ 元组状态。结果关系（若有）被标记为已阻塞，
	 * 其余标记为未获取。
	 */
	epqstate->relsubs_done = palloc_array(bool, rtsize);
	epqstate->relsubs_blocked = palloc0_array(bool, rtsize);

	foreach(l, epqstate->resultRelations)
	{
		int			rtindex = lfirst_int(l);

		Assert(rtindex > 0 && rtindex <= rtsize);
		epqstate->relsubs_blocked[rtindex - 1] = true;
	}

	memcpy(epqstate->relsubs_done, epqstate->relsubs_blocked,
		   rtsize * sizeof(bool));

	/*
	 * 为我们需要运行的那部分计划树中的所有节点初始化私有状态信息。
	 * 这会打开文件、分配存储，并使我们准备好开始处理元组。
	 */
	epqstate->recheckplanstate = ExecInitNode(planTree, rcestate, 0);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * EvalPlanQualEnd -- 在父计划状态节点终止时，或者当我们已处理完当前
 * EPQ 子节点时，进行关闭。
 *
 * 这是 ExecutorEnd() 的精简版本；基本上我们想做大部分常规的清理工作，
 * 但*不要*关闭结果关系（我们只是从外层查询共享它们）。不过，我们确实
 * 需要关闭任何被打开过的结果关系与触发器目标关系，因为它们并未被共享。
 * （后者大概本不该存在，但以防万一……）
 */
void
EvalPlanQualEnd(EPQState *epqstate)
{
	EState	   *estate = epqstate->recheckestate;
	Index		rtsize;
	MemoryContext oldcontext;
	ListCell   *l;

	rtsize = epqstate->parentestate->es_range_table_size;

	/*
	 * 我们可能拥有一个元组表，即使 EPQ 尚未启动，因为我们允许在不调用
	 * EvalPlanQualBegin() 的情况下使用 EvalPlanQualSlot()。
	 */
	if (epqstate->tuple_table != NIL)
	{
		memset(epqstate->relsubs_slot, 0,
			   rtsize * sizeof(TupleTableSlot *));
		ExecResetTupleTable(epqstate->tuple_table, true);
		epqstate->tuple_table = NIL;
	}

	/* EPQ 尚未启动，无需再做其它事情 */
	if (estate == NULL)
		return;

	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	ExecEndNode(epqstate->recheckplanstate);

	foreach(l, estate->es_subplanstates)
	{
		PlanState  *subplanstate = (PlanState *) lfirst(l);

		ExecEndNode(subplanstate);
	}

	/* 丢弃每 estate 的元组表，某些节点可能使用过它 */
	ExecResetTupleTable(estate->es_tupleTable, false);

	/* 关闭附加到该 EState 的任何结果关系与触发器目标关系 */
	ExecCloseResultRelations(estate);

	MemoryContextSwitchTo(oldcontext);

	/*
	 * 在释放执行器状态之前，先将分区目录置为 NULL。由于
	 * EvalPlanQualStart() 只是借用了父 EState 的目录，最好把它留给
	 * 父 EState 去删除。
	 */
	estate->es_partition_directory = NULL;

	FreeExecutorState(estate);

	/* 将 EPQState 标记为空闲 */
	epqstate->origslot = NULL;
	epqstate->recheckestate = NULL;
	epqstate->recheckplanstate = NULL;
	epqstate->relsubs_rowmark = NULL;
	epqstate->relsubs_done = NULL;
	epqstate->relsubs_blocked = NULL;
}
