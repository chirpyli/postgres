/*-------------------------------------------------------------------------
 *
 * planner.c
 *	  查询优化器的外部接口。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/planner.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>
#include <math.h>

#include "access/genam.h"
#include "access/parallel.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "foreign/fdwapi.h"
#include "jit/jit.h"
#include "lib/bipartite_match.h"
#include "lib/knapsack.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#ifdef OPTIMIZER_DEBUG
#include "nodes/print.h"
#endif
#include "nodes/supportnodes.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/paramassign.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "parser/analyze.h"
#include "parser/parse_agg.h"
#include "parser/parse_clause.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "partitioning/partdesc.h"
#include "rewrite/rewriteManip.h"
#include "utils/acl.h"
#include "utils/backend_status.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"

/* GUC 参数 */
double		cursor_tuple_fraction = DEFAULT_CURSOR_TUPLE_FRACTION;
int			debug_parallel_query = DEBUG_PARALLEL_OFF;
bool		parallel_leader_participation = true;
bool		enable_distinct_reordering = true;

/* 供插件在 planner() 中获取控制权的钩子 */
planner_hook_type planner_hook = NULL;

/* 供插件在 grouping_planner() 规划上层关系时获取控制权的钩子 */
create_upper_paths_hook_type create_upper_paths_hook = NULL;


/* preprocess_expression 的表达式种类代码 */
#define EXPRKIND_QUAL				0
#define EXPRKIND_TARGET				1
#define EXPRKIND_RTFUNC				2
#define EXPRKIND_RTFUNC_LATERAL		3
#define EXPRKIND_VALUES				4
#define EXPRKIND_VALUES_LATERAL		5
#define EXPRKIND_LIMIT				6
#define EXPRKIND_APPINFO			7
#define EXPRKIND_PHV				8
#define EXPRKIND_TABLESAMPLE		9
#define EXPRKIND_ARBITER_ELEM		10
#define EXPRKIND_TABLEFUNC			11
#define EXPRKIND_TABLEFUNC_LATERAL	12
#define EXPRKIND_GROUPEXPR			13

/*
 * 与分组集（grouping sets）相关的专有数据
 */
typedef struct
{
	List	   *rollups;
	List	   *hash_sets_idx;
	double		dNumHashGroups;
	bool		any_hashable;
	Bitmapset  *unsortable_refs;
	Bitmapset  *unhashable_refs;
	List	   *unsortable_sets;
	int		   *tleref_to_colnum_map;
} grouping_sets_data;

/*
 * 在 WindowClause 重排序期间使用的临时结构，以便能够按照
 * 分区/排序前缀对 WindowClause 进行排序。
 */
typedef struct
{
	WindowClause *wc;
	List	   *uniqueOrder;	/* 每个 Window 的唯一 排序/分区
								 * 子句列表 */
} WindowClauseSortData;

/* standard_qp_callback 的透传数据 */
typedef struct
{
	List	   *activeWindows;	/* 活跃窗口（如果有） */
	grouping_sets_data *gset_data;	/* 分组集数据（如果有） */
	SetOperationStmt *setop;	/* 父集合操作，或若不是属于集合操作的
								 * 子查询则为 NULL */
} standard_qp_extra;

/*
 * find_having_collation_conflicts 遍历器的上下文。
 *
 * ancestor_collids 是由当前节点的感知排序规则（collation-aware）祖先所
 * 贡献的 inputcollid 栈。在进入节点的子节点递归之前压入条目、之后弹出，
 * 因此该栈精确地反映了当前根到节点路径上的 inputcollid。
 */
typedef struct
{
	Index		group_rtindex;
	List	   *ancestor_collids;
} having_collation_ctx;

/* 本地函数 */
static Node *preprocess_expression(PlannerInfo *root, Node *expr, int kind);
static void preprocess_qual_conditions(PlannerInfo *root, Node *jtnode);
static Bitmapset *find_having_collation_conflicts(Query *parse,
												  Index group_rtindex);
static bool having_collation_conflict_walker(Node *node,
											 having_collation_ctx *ctx);
static void grouping_planner(PlannerInfo *root, double tuple_fraction,
							 SetOperationStmt *setops);
static grouping_sets_data *preprocess_grouping_sets(PlannerInfo *root);
static List *remap_to_groupclause_idx(List *groupClause, List *gsets,
									  int *tleref_to_colnum_map);
static void preprocess_rowmarks(PlannerInfo *root);
static double preprocess_limit(PlannerInfo *root,
							   double tuple_fraction,
							   int64 *offset_est, int64 *count_est);
static List *preprocess_groupclause(PlannerInfo *root, List *force);
static List *extract_rollup_sets(List *groupingSets);
static List *reorder_grouping_sets(List *groupingSets, List *sortclause);
static void standard_qp_callback(PlannerInfo *root, void *extra);
static double get_number_of_groups(PlannerInfo *root,
								   double path_rows,
								   grouping_sets_data *gd,
								   List *target_list);
static RelOptInfo *create_grouping_paths(PlannerInfo *root,
										 RelOptInfo *input_rel,
										 PathTarget *target,
										 bool target_parallel_safe,
										 grouping_sets_data *gd);
static bool is_degenerate_grouping(PlannerInfo *root);
static void create_degenerate_grouping_paths(PlannerInfo *root,
											 RelOptInfo *input_rel,
											 RelOptInfo *grouped_rel);
static RelOptInfo *make_grouping_rel(PlannerInfo *root, RelOptInfo *input_rel,
									 PathTarget *target, bool target_parallel_safe,
									 Node *havingQual);
static void create_ordinary_grouping_paths(PlannerInfo *root,
										   RelOptInfo *input_rel,
										   RelOptInfo *grouped_rel,
										   const AggClauseCosts *agg_costs,
										   grouping_sets_data *gd,
										   GroupPathExtraData *extra,
										   RelOptInfo **partially_grouped_rel_p);
static void consider_groupingsets_paths(PlannerInfo *root,
										RelOptInfo *grouped_rel,
										Path *path,
										bool is_sorted,
										bool can_hash,
										grouping_sets_data *gd,
										const AggClauseCosts *agg_costs,
										double dNumGroups);
static RelOptInfo *create_window_paths(PlannerInfo *root,
									   RelOptInfo *input_rel,
									   PathTarget *input_target,
									   PathTarget *output_target,
									   bool output_target_parallel_safe,
									   WindowFuncLists *wflists,
									   List *activeWindows);
static void create_one_window_path(PlannerInfo *root,
								   RelOptInfo *window_rel,
								   Path *path,
								   PathTarget *input_target,
								   PathTarget *output_target,
								   WindowFuncLists *wflists,
								   List *activeWindows);
static RelOptInfo *create_distinct_paths(PlannerInfo *root,
										 RelOptInfo *input_rel,
										 PathTarget *target);
static void create_partial_distinct_paths(PlannerInfo *root,
										  RelOptInfo *input_rel,
										  RelOptInfo *final_distinct_rel,
										  PathTarget *target);
static RelOptInfo *create_final_distinct_paths(PlannerInfo *root,
											   RelOptInfo *input_rel,
											   RelOptInfo *distinct_rel);
static List *get_useful_pathkeys_for_distinct(PlannerInfo *root,
											  List *needed_pathkeys,
											  List *path_pathkeys);
static RelOptInfo *create_ordered_paths(PlannerInfo *root,
										RelOptInfo *input_rel,
										PathTarget *target,
										bool target_parallel_safe,
										double limit_tuples);
static PathTarget *make_group_input_target(PlannerInfo *root,
										   PathTarget *final_target);
static PathTarget *make_partial_grouping_target(PlannerInfo *root,
												PathTarget *grouping_target,
												Node *havingQual);
static List *postprocess_setop_tlist(List *new_tlist, List *orig_tlist);
static void optimize_window_clauses(PlannerInfo *root,
									WindowFuncLists *wflists);
static List *select_active_windows(PlannerInfo *root, WindowFuncLists *wflists);
static void name_active_windows(List *activeWindows);
static PathTarget *make_window_input_target(PlannerInfo *root,
											PathTarget *final_target,
											List *activeWindows);
static List *make_pathkeys_for_window(PlannerInfo *root, WindowClause *wc,
									  List *tlist);
static PathTarget *make_sort_input_target(PlannerInfo *root,
										  PathTarget *final_target,
										  bool *have_postponed_srfs);
static void adjust_paths_for_srfs(PlannerInfo *root, RelOptInfo *rel,
								  List *targets, List *targets_contain_srfs);
static void add_paths_to_grouping_rel(PlannerInfo *root, RelOptInfo *input_rel,
									  RelOptInfo *grouped_rel,
									  RelOptInfo *partially_grouped_rel,
									  const AggClauseCosts *agg_costs,
									  grouping_sets_data *gd,
									  double dNumGroups,
									  GroupPathExtraData *extra);
static RelOptInfo *create_partial_grouping_paths(PlannerInfo *root,
												 RelOptInfo *grouped_rel,
												 RelOptInfo *input_rel,
												 grouping_sets_data *gd,
												 GroupPathExtraData *extra,
												 bool force_rel_creation);
static Path *make_ordered_path(PlannerInfo *root,
							   RelOptInfo *rel,
							   Path *path,
							   Path *cheapest_path,
							   List *pathkeys,
							   double limit_tuples);
static void gather_grouping_paths(PlannerInfo *root, RelOptInfo *rel);
static bool can_partial_agg(PlannerInfo *root);
static void apply_scanjoin_target_to_paths(PlannerInfo *root,
										   RelOptInfo *rel,
										   List *scanjoin_targets,
										   List *scanjoin_targets_contain_srfs,
										   bool scanjoin_target_parallel_safe,
										   bool tlist_same_exprs);
static void create_partitionwise_grouping_paths(PlannerInfo *root,
												RelOptInfo *input_rel,
												RelOptInfo *grouped_rel,
												RelOptInfo *partially_grouped_rel,
												const AggClauseCosts *agg_costs,
												grouping_sets_data *gd,
												PartitionwiseAggregateType patype,
												GroupPathExtraData *extra);
static bool group_by_has_partkey(RelOptInfo *input_rel,
								 List *targetList,
								 List *groupClause);
static int	common_prefix_cmp(const void *a, const void *b);
static List *generate_setop_child_grouplist(SetOperationStmt *op,
											List *targetlist);


/*****************************************************************************
 *
 *	   查询优化器入口点
 *
 * 为了支持可加载的插件来监视或修改规划器行为，我们提供了一个钩子变量，
 * 让插件能够在标准规划过程之前和之后获取控制权。插件通常会调用
 * standard_planner()。
 *
 * 给插件作者的提示：standard_planner() 会涂抹（修改）其 Query 输入，
 * 因此如果你想规划多次，最好先复制该数据结构。
 *
 *****************************************************************************/
PlannedStmt *
planner(Query *parse, const char *query_string, int cursorOptions,
		ParamListInfo boundParams)
{
	PlannedStmt *result;

	if (planner_hook)
		result = (*planner_hook) (parse, query_string, cursorOptions, boundParams);
	else
		result = standard_planner(parse, query_string, cursorOptions, boundParams);

	pgstat_report_plan_id(result->planId, false);

	return result;
}

PlannedStmt *
standard_planner(Query *parse, const char *query_string, int cursorOptions,
				 ParamListInfo boundParams)
{
	PlannedStmt *result;
	PlannerGlobal *glob;
	double		tuple_fraction;
	PlannerInfo *root;
	RelOptInfo *final_rel;
	Path	   *best_path;
	Plan	   *top_plan;
	ListCell   *lp,
			   *lr;

	/*
	 * 为本次规划器调用设置全局状态。给定命令中可能存在的所有层级的子查询
	 * 都需要用到这些数据，因此我们将它保存在一个独立的结构体中，并由每个
	 * 每查询（per-Query）的 PlannerInfo 链接到它。
	 */
	glob = makeNode(PlannerGlobal);

	glob->boundParams = boundParams;
	glob->subplans = NIL;
	glob->subpaths = NIL;
	glob->subroots = NIL;
	glob->rewindPlanIDs = NULL;
	glob->finalrtable = NIL;
	glob->allRelids = NULL;
	glob->prunableRelids = NULL;
	glob->finalrteperminfos = NIL;
	glob->finalrowmarks = NIL;
	glob->resultRelations = NIL;
	glob->appendRelations = NIL;
	glob->partPruneInfos = NIL;
	glob->relationOids = NIL;
	glob->invalItems = NIL;
	glob->paramExecTypes = NIL;
	glob->lastPHId = 0;
	glob->lastRowMarkId = 0;
	glob->lastPlanNodeId = 0;
	glob->transientPlan = false;
	glob->dependsOnRole = false;
	glob->partition_directory = NULL;

	/*
	 * 评估此查询是否适合使用并行模式。在独立后端中、或命令会尝试修改任何数据、
	 * 或是游标操作、或 GUC 被设为不允许并行的值、或查询树中存在并行不安全
	 * 的函数时，我们都无法使用并行。
	 *
	 * （注意我们确实允许 CREATE TABLE AS、SELECT INTO 和 CREATE
	 * MATERIALIZED VIEW 使用并行计划，但这仅因为命令写入的是工作进程
	 * 完全看不到的全新表。如果工作进程能看到该表，那么组锁会导致它们忽略
	 * 领导者的重量级 GIN 页锁，这将使并行不安全。如果我们想普遍允许并行插入，
	 * 就必须以某种方式修复这个问题；更新和删除还有额外的问题，尤其是围绕
	 * 组合 CID。）
	 *
	 * 目前，如果我们运行在并行工作进程内部，则不会尝试使用并行模式。我们
	 * 最终可能能够放宽这一限制，但现在最好不让并行工作进程尝试创建它们自己
	 * 的并行工作进程。
	 */
	if ((cursorOptions & CURSOR_OPT_PARALLEL_OK) != 0 &&
		IsUnderPostmaster &&
		parse->commandType == CMD_SELECT &&
		!parse->hasModifyingCTE &&
		max_parallel_workers_per_gather > 0 &&
		!IsParallelWorker())
	{
		/* 所有廉价测试都通过，因此扫描查询树 */
		glob->maxParallelHazard = max_parallel_hazard(parse);
		glob->parallelModeOK = (glob->maxParallelHazard != PROPARALLEL_UNSAFE);
	}
	else
	{
		/* 跳过查询树扫描，直接假定其不安全 */
		glob->maxParallelHazard = PROPARALLEL_UNSAFE;
		glob->parallelModeOK = false;
	}

	/*
	 * glob->parallelModeNeeded is normally set to false here and changed to
	 * true during plan creation if a Gather or Gather Merge plan is actually
	 * created (cf. create_gather_plan, create_gather_merge_plan).
	 *
	 * 然而，如果 debug_parallel_query = on 或 debug_parallel_query =
	 * regress，那么只要安全我们就会强制使用并行模式，即使最终计划并不
	 * 使用并行。如果查询包含任何并行不安全的内容，这样做就不安全；
	 * 那种情况下 parallelModeOK 将为 false。注意 parallelModeOK 在此点之后
	 * 不能再改变。否则，查询中的一切要么并行安全，要么并行受限，无论哪种
	 * 情况施加并行模式限制都应该是可以的。如果最终破坏了什么，那么要么
	 * 是用户查询中某个函数被错误地标记为并行安全或并行受限（而实际上它
	 * 是并行不安全的），要么是查询规划器本身有 bug。
	 */
	glob->parallelModeNeeded = glob->parallelModeOK &&
		(debug_parallel_query != DEBUG_PARALLEL_OFF);

	/* 确定计划中可能有多大比例会被扫描 */
	if (cursorOptions & CURSOR_OPT_FAST_PLAN)
	{
		/*
		 * 我们无从确切知道用户最终会从游标中 FETCH 多少元组，但通常情况是他
		 * 并不想要全部，或者无论如何更偏好快速启动计划，以便他能更快地处理
		 * 其中一部分元组。使用一个 GUC 参数来决定优化多大比例。
		 */
		tuple_fraction = cursor_tuple_fraction;

		/*
		 * 我们将 cursor_tuple_fraction 文档化为一个简单的比例，这意味着边界
		 * 情况 0 和 1 必须在此特殊处理。我们将 1 转换为 0（"所有元组"），
		 * 将 0 转换为一个非常小的比例。
		 */
		if (tuple_fraction >= 1.0)
			tuple_fraction = 0.0;
		else if (tuple_fraction <= 0.0)
			tuple_fraction = 1e-10;
	}
	else
	{
		/* 默认假设为我们需要所有元组 */
		tuple_fraction = 0.0;
	}

	/* 主要的规划入口点（可能针对子查询递归） */
	root = subquery_planner(glob, parse, NULL, false, tuple_fraction, NULL);

	/* 选择最佳路径并将其转换为计划 */
	final_rel = fetch_upper_rel(root, UPPERREL_FINAL, NULL);
	best_path = get_cheapest_fractional_path(final_rel, tuple_fraction);

	top_plan = create_plan(root, best_path);

	/*
	 * 如果在为可滚动游标创建计划，确保它可以根据需要向后运行。
	 * 必要时在顶部添加一个 Material 节点。
	 */
	if (cursorOptions & CURSOR_OPT_SCROLL)
	{
		if (!ExecSupportsBackwardScan(top_plan))
			top_plan = materialize_finished_plan(top_plan);
	}

	/*
	 * 可选地添加一个 Gather 节点用于测试目的，前提是这确实是安全的。
	 *
	 * 即使 top_plan 有并行安全的 initPlan，我们也可以添加 Gather，但那样
	 * 由于 SS_finalize_plan 的限制，我们必须将 initPlan 移动到 Gather 节点。
	 * 当 debug_parallel_query = regress 时，这会导致回归测试的表象性破坏，
	 * 因为通常出现在 top_plan 上的 initPlan 会移动到 Gather，从而从 EXPLAIN
	 * 输出中消失。这似乎不值得用 hack EXPLAIN 来掩盖，因此在
	 * debug_parallel_query = regress 时跳过。
	 */
	if (debug_parallel_query != DEBUG_PARALLEL_OFF &&
		top_plan->parallel_safe &&
		(top_plan->initPlan == NIL ||
		 debug_parallel_query != DEBUG_PARALLEL_REGRESS))
	{
		Gather	   *gather = makeNode(Gather);
		Cost		initplan_cost;
		bool		unsafe_initplans;

		gather->plan.targetlist = top_plan->targetlist;
		gather->plan.qual = NIL;
		gather->plan.lefttree = top_plan;
		gather->plan.righttree = NULL;
		gather->num_workers = 1;
		gather->single_copy = true;
		gather->invisible = (debug_parallel_query == DEBUG_PARALLEL_REGRESS);

		/* 将任何 initPlan 转移到新的顶层节点 */
		gather->plan.initPlan = top_plan->initPlan;
		top_plan->initPlan = NIL;

		/*
		 * 由于这个 Gather 没有需要通知的并行感知后代，我们不需要 rescan
		 * Param。
		 */
		gather->rescan_param = -1;

		/*
		 * 理想情况下我们这里会调用 cost_gather，但为了满足它而设置虚拟路径
		 * 数据，并不比直接知道它在做什么更干净。
		 */
		gather->plan.startup_cost = top_plan->startup_cost +
			parallel_setup_cost;
		gather->plan.total_cost = top_plan->total_cost +
			parallel_setup_cost + parallel_tuple_cost * top_plan->plan_rows;
		gather->plan.plan_rows = top_plan->plan_rows;
		gather->plan.plan_width = top_plan->plan_width;
		gather->plan.parallel_aware = false;
		gather->plan.parallel_safe = false;

		/*
		 * 从 top_plan 中删除 initplan 的代价。我们无需将其加到 Gather 节点，
		 * 因为上面的代码已经把它包含在内了。
		 */
		SS_compute_initplan_cost(gather->plan.initPlan,
								 &initplan_cost, &unsafe_initplans);
		top_plan->startup_cost -= initplan_cost;
		top_plan->total_cost -= initplan_cost;

		/* 对并行计划使用并行模式。 */
		root->glob->parallelModeNeeded = true;

		top_plan = &gather->plan;
	}

	/*
	 * 如果生成了任何 Param，遍历计划树并计算每个计划节点的 extParam/allParam
	 * 集合。理想情况下我们会将此合并进 set_plan_references 的树遍历中，但
	 * 目前它必须分开，因为我们需要在主线计划之前而非之后访问子计划。
	 */
	if (glob->paramExecTypes != NIL)
	{
		Assert(list_length(glob->subplans) == list_length(glob->subroots));
		forboth(lp, glob->subplans, lr, glob->subroots)
		{
			Plan	   *subplan = (Plan *) lfirst(lp);
			PlannerInfo *subroot = lfirst_node(PlannerInfo, lr);

			SS_finalize_plan(subroot, subplan);
		}
		SS_finalize_plan(root, top_plan);
	}

	/* 计划的最后清理 */
	Assert(glob->finalrtable == NIL);
	Assert(glob->finalrteperminfos == NIL);
	Assert(glob->finalrowmarks == NIL);
	Assert(glob->resultRelations == NIL);
	Assert(glob->appendRelations == NIL);
	top_plan = set_plan_references(root, top_plan);
	/* ... 以及子计划（常规子计划和 initplan 都包括） */
	Assert(list_length(glob->subplans) == list_length(glob->subroots));
	forboth(lp, glob->subplans, lr, glob->subroots)
	{
		Plan	   *subplan = (Plan *) lfirst(lp);
		PlannerInfo *subroot = lfirst_node(PlannerInfo, lr);

		lfirst(lp) = set_plan_references(subroot, subplan);
	}

	/* 构建 PlannedStmt 结果 */
	result = makeNode(PlannedStmt);

	result->commandType = parse->commandType;
	result->queryId = parse->queryId;
	result->hasReturning = (parse->returningList != NIL);
	result->hasModifyingCTE = parse->hasModifyingCTE;
	result->canSetTag = parse->canSetTag;
	result->transientPlan = glob->transientPlan;
	result->dependsOnRole = glob->dependsOnRole;
	result->parallelModeNeeded = glob->parallelModeNeeded;
	result->planTree = top_plan;
	result->partPruneInfos = glob->partPruneInfos;
	result->rtable = glob->finalrtable;
	result->unprunableRelids = bms_difference(glob->allRelids,
											  glob->prunableRelids);
	result->permInfos = glob->finalrteperminfos;
	result->resultRelations = glob->resultRelations;
	result->appendRelations = glob->appendRelations;
	result->subplans = glob->subplans;
	result->rewindPlanIDs = glob->rewindPlanIDs;
	result->rowMarks = glob->finalrowmarks;
	result->relationOids = glob->relationOids;
	result->invalItems = glob->invalItems;
	result->paramExecTypes = glob->paramExecTypes;
	/* utilityStmt 应该为 null，但我们不妨也复制它 */
	result->utilityStmt = parse->utilityStmt;
	result->stmt_location = parse->stmt_location;
	result->stmt_len = parse->stmt_len;

	result->jitFlags = PGJIT_NONE;
	if (jit_enabled && jit_above_cost >= 0 &&
		top_plan->total_cost > jit_above_cost)
	{
		result->jitFlags |= PGJIT_PERFORM;

		/*
		 * 决定在生成更优代码方面应投入多少精力。
		 */
		if (jit_optimize_above_cost >= 0 &&
			top_plan->total_cost > jit_optimize_above_cost)
			result->jitFlags |= PGJIT_OPT3;
		if (jit_inline_above_cost >= 0 &&
			top_plan->total_cost > jit_inline_above_cost)
			result->jitFlags |= PGJIT_INLINE;

		/*
		 * 决定哪些操作应被 JIT 编译。
		 */
		if (jit_expressions)
			result->jitFlags |= PGJIT_EXPR;
		if (jit_tuple_deforming)
			result->jitFlags |= PGJIT_DEFORM;
	}

	if (glob->partition_directory != NULL)
		DestroyPartitionDirectory(glob->partition_directory);

	return result;
}


/*--------------------
 * subquery_planner
 *	  在子查询上调用规划器。查询树中发现的每个子 SELECT 都会递归到此处。
 *
 * glob 是当前规划器运行的全局状态。
 * parse 是解析器和重写器产生的查询树。
 * parent_root 是紧邻的父查询的信息（顶层为 NULL）。
 * hasRecursion 如果这是递归 WITH 查询则为 true。
 * tuple_fraction 是我们期望检索到的元组比例。
 * tuple_fraction 的解释见下文的 grouping_planner。
 * setops 用于集合操作子查询，为子查询提供其使用上下文，以便生成正确为
 * 集合操作排序的路径。当不规划集合操作子节点、或集合操作的子节点对
 * 排序输入不感兴趣时为 NULL。
 *
 * 基本上，本例程完成每个 Query 对象只应做一次的事情。然后它调用
 * grouping_planner。曾经 grouping_planner 可以对同一个 Query 对象递归调用；
 * 现在已非如此，但我们仍然保持这两个例程之间的分离，以防将来某天再次需要。
 *
 * subquery_planner 会被递归调用，以处理在查询的表达式和范围表中发现的
 * 子查询节点。
 *
 * 返回在规划子查询时生成的所有数据所在的 PlannerInfo 结构体（"root"）。
 * 特别地，附加到 (UPPERREL_FINAL, NULL) 上层关系的路径代表了我们关于
 * 实现该查询的最廉价方式的结论。顶层将选择最佳路径，并通过 createplan.c
 * 生成最终的计划。
 *--------------------
 */
PlannerInfo *
subquery_planner(PlannerGlobal *glob, Query *parse, PlannerInfo *parent_root,
				 bool hasRecursion, double tuple_fraction,
				 SetOperationStmt *setops)
{
	PlannerInfo *root;
	List	   *newWithCheckOptions;
	List	   *newHaving;
	Bitmapset  *havingCollationConflicts;
	int			havingIdx;
	bool		hasOuterJoins;
	bool		hasResultRTEs;
	RelOptInfo *final_rel;
	ListCell   *l;

	/* 为此子查询创建一个 PlannerInfo 数据结构 */
	root = makeNode(PlannerInfo);
	root->parse = parse;
	root->glob = glob;
	root->query_level = parent_root ? parent_root->query_level + 1 : 1;
	root->parent_root = parent_root;
	root->plan_params = NIL;
	root->outer_params = NULL;
	root->planner_cxt = CurrentMemoryContext;
	root->init_plans = NIL;
	root->cte_plan_ids = NIL;
	root->multiexpr_params = NIL;
	root->join_domains = NIL;
	root->eq_classes = NIL;
	root->ec_merging_done = false;
	root->last_rinfo_serial = 0;
	root->all_result_relids =
		parse->resultRelation ? bms_make_singleton(parse->resultRelation) : NULL;
	root->leaf_result_relids = NULL;	/* 我们稍后会确定其是否为叶子 */
	root->append_rel_list = NIL;
	root->row_identity_vars = NIL;
	root->rowMarks = NIL;
	memset(root->upper_rels, 0, sizeof(root->upper_rels));
	memset(root->upper_targets, 0, sizeof(root->upper_targets));
	root->processed_groupClause = NIL;
	root->processed_distinctClause = NIL;
	root->processed_tlist = NIL;
	root->update_colnos = NIL;
	root->grouping_map = NULL;
	root->minmax_aggs = NIL;
	root->qual_security_level = 0;
	root->hasPseudoConstantQuals = false;
	root->hasAlternativeSubPlans = false;
	root->placeholdersFrozen = false;
	root->hasRecursion = hasRecursion;
	if (hasRecursion)
		root->wt_param_id = assign_special_exec_param(root);
	else
		root->wt_param_id = -1;
	root->non_recursive_path = NULL;
	root->partColsUpdated = false;

	/*
	 * 创建顶层的连接域（join domain）。在 deconstruct_jointree 填充它之前，
	 * 它不会有有效内容，但该节点需要在此之前就存在，以便我们构建引用它的
	 * 等价类（EquivalenceClass）。
	 */
	root->join_domains = list_make1(makeNode(JoinDomain));

	/*
	 * 如果存在 WITH 列表，处理每个 WITH 查询，将其要么转换为 RTE_SUBQUERY
	 * 类型的 RTE，要么为其构建 initplan 子计划结构。
	 */
	if (parse->cteList)
		SS_process_ctes(root);

	/*
	 * 如果是 MERGE 命令，对 joinlist 做适当转换。
	 */
	transform_MERGE_to_join(parse);

	/*
	 * 如果 FROM 子句为空，用一个虚拟的 RTE_RESULT 类型 RTE 替换它，这样我们
	 * 就不需要那么多特殊情况来处理这种情形。
	 */
	replace_empty_jointree(parse);

	/*
	 * 在 WHERE 和 JOIN/ON 子句中查找 ANY 和 EXISTS 子链接，并尝试将它们
	 * 转换为连接。注意这一步不会下钻到子查询中；如果我们下面拉起了任何子查询，
	 * 它们的子链接会在拉起之前被处理。
	 */
	if (parse->hasSubLinks)
		pull_up_sublinks(root);

	/*
	 * 扫描范围表找到函数类型的 RTE，对它们做常量简化，然后如果可能就内联
	 * 它们（产生可能接下来被拉起的子查询）。此处的递归问题与子链接的
	 * 处理方式相同。
	 */
	preprocess_function_rtes(root);

	/*
	 * 扫描范围表找到带有虚拟生成列的关系，并将查询中所有引用这些列的 Var
	 * 节点替换为生成表达式。此处的递归问题与子链接的处理方式相同。
	 */
	parse = root->parse = expand_virtual_generated_columns(root);

	/*
	 * 检查连接树中的任何子查询是否可以合并到本查询中。
	 */
	pull_up_subqueries(root);

	/*
	 * 如果这是一个简单的 UNION ALL 查询，将其扁平化为一个 appendrel。我们
	 * 现在做这件事，因为它需要对 UNION ALL 的叶子查询应用 pull_up_subqueries，
	 * 而那些查询上面没有触及，因为它们没有被连接树引用（在我们做完这步之后
	 * 它们就会被引用）。
	 */
	if (parse->setOperations)
		flatten_simple_union_all(root);

	/*
	 * 调查范围表以查看存在哪些种类的条目。如果没有使用相关的 SQL 特性，
	 * 我们可以跳过一些后续处理；例如如果没有 JOIN 类型的 RTE，我们就可以
	 * 避免 flatten_join_alias_vars() 的开销。这当然必须在我们完成添加范围表
	 * 条目之后进行。（注意：实际上，继承或分区关系的处理可能导致它们的子表
	 * 的 RTE 在之后被添加；但那些必须都是 RTE_RELATION 条目，因此它们不会
	 * 使此处得出的结论失效。）
	 */
	root->hasJoinRTEs = false;
	root->hasLateralRTEs = false;
	root->group_rtindex = 0;
	hasOuterJoins = false;
	hasResultRTEs = false;
	foreach(l, parse->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, l);

		switch (rte->rtekind)
		{
			case RTE_RELATION:
				if (rte->inh)
				{
					/*
					 * Check to see if the relation actually has any children;
					 * if not, clear the inh flag so we can treat it as a
					 * plain base relation.
					 *
					 * Note: this could give a false-positive result, if the
					 * rel once had children but no longer does.  We used to
					 * be able to clear rte->inh later on when we discovered
					 * that, but no more; we have to handle such cases as
					 * full-fledged inheritance.
					 */
					rte->inh = has_subclass(rte->relid);
				}
				break;
			case RTE_JOIN:
				root->hasJoinRTEs = true;
				if (IS_OUTER_JOIN(rte->jointype))
					hasOuterJoins = true;
				break;
			case RTE_RESULT:
				hasResultRTEs = true;
				break;
			case RTE_GROUP:
				Assert(parse->hasGroupRTE);
				root->group_rtindex = list_cell_number(parse->rtable, l) + 1;
				break;
			default:
				/* 其他 RTE 类型在此无需处理 */
				break;
		}

		if (rte->lateral)
			root->hasLateralRTEs = true;

		/*
		 * 我们现在还可以确定任何 securityQuals 所需的最大安全级别。
		 * 继承子 RTE 的添加不会影响这一点，因为子表没有它们自己的
		 * securityQuals；见 expand_single_inheritance_child()。
		 */
		if (rte->securityQuals)
			root->qual_security_level = Max(root->qual_security_level,
											list_length(rte->securityQuals));
	}

	/*
	 * 如果我们现在已确认查询的目标关系是非继承的，将其标记为叶子目标。
	 */
	if (parse->resultRelation)
	{
		RangeTblEntry *rte = rt_fetch(parse->resultRelation, parse->rtable);

		if (!rte->inh)
			root->leaf_result_relids =
				bms_make_singleton(parse->resultRelation);
	}

	/*
	 * 这原本是检查查询中提及的所有关系的访问权限的合适时机，因为最好在
	 * 进行任何详细规划之前就失败。然而，出于历史原因，我们将此事留到
	 * 执行器启动时再做。
	 *
	 * 但是请注意，我们确实需要检查查询中提及的任何视图关系的访问权限，
	 * 以防止信息被选择性估计函数泄露，那些函数只检查视图所有者对底层表
	 * 的权限（见 all_rows_selectable() 及其调用者）。这有点丑陋，因为这意味着
	 * 视图的访问权限会被检查两次，这也是为什么最好在这里完成所有 ACL
	 * 检查的另一条理由。
	 */
	foreach(l, parse->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, l);

		if (rte->perminfoindex != 0 &&
			rte->relkind == RELKIND_VIEW)
		{
			RTEPermissionInfo *perminfo;
			bool		result;

			perminfo = getRTEPermissionInfo(parse->rteperminfos, rte);
			result = ExecCheckOneRelPerms(perminfo);
			if (!result)
				aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_VIEW,
							   get_rel_name(perminfo->relid));
		}
	}

	/*
	 * 预处理 RowMark 信息。我们需要在子查询拉起之后做这件事，
	 * 以便所有基关系都已存在。
	 */
	preprocess_rowmarks(root);

	/*
	 * 设置 hasHavingQual 以记住是否存在 HAVING 子句。这是必要的，因为
	 * preprocess_expression 会把常量真条件简化为一个空的限定列表……
	 * 但 "HAVING TRUE" 在语义上并非空操作。
	 */
	root->hasHavingQual = (parse->havingQual != NULL);

	/*
	 * 对目标列表和限定条件，以及查询树中的其他零散表达式做表达式预处理。
	 * 注意我们不需要显式处理排序/分组表达式，因为它们实际上是目标列表
	 * 的一部分。
	 */
	parse->targetList = (List *)
		preprocess_expression(root, (Node *) parse->targetList,
							  EXPRKIND_TARGET);

	newWithCheckOptions = NIL;
	foreach(l, parse->withCheckOptions)
	{
		WithCheckOption *wco = lfirst_node(WithCheckOption, l);

		wco->qual = preprocess_expression(root, wco->qual,
										  EXPRKIND_QUAL);
		if (wco->qual != NULL)
			newWithCheckOptions = lappend(newWithCheckOptions, wco);
	}
	parse->withCheckOptions = newWithCheckOptions;

	parse->returningList = (List *)
		preprocess_expression(root, (Node *) parse->returningList,
							  EXPRKIND_TARGET);

	preprocess_qual_conditions(root, (Node *) parse->jointree);

	parse->havingQual = preprocess_expression(root, parse->havingQual,
											  EXPRKIND_QUAL);

	foreach(l, parse->windowClause)
	{
		WindowClause *wc = lfirst_node(WindowClause, l);

		/* partitionClause/orderClause 是排序/分组表达式 */
		wc->startOffset = preprocess_expression(root, wc->startOffset,
												EXPRKIND_LIMIT);
		wc->endOffset = preprocess_expression(root, wc->endOffset,
											  EXPRKIND_LIMIT);
	}

	parse->limitOffset = preprocess_expression(root, parse->limitOffset,
											   EXPRKIND_LIMIT);
	parse->limitCount = preprocess_expression(root, parse->limitCount,
											  EXPRKIND_LIMIT);

	if (parse->onConflict)
	{
		parse->onConflict->arbiterElems = (List *)
			preprocess_expression(root,
								  (Node *) parse->onConflict->arbiterElems,
								  EXPRKIND_ARBITER_ELEM);
		parse->onConflict->arbiterWhere =
			preprocess_expression(root,
								  parse->onConflict->arbiterWhere,
								  EXPRKIND_QUAL);
		parse->onConflict->onConflictSet = (List *)
			preprocess_expression(root,
								  (Node *) parse->onConflict->onConflictSet,
								  EXPRKIND_TARGET);
		parse->onConflict->onConflictWhere =
			preprocess_expression(root,
								  parse->onConflict->onConflictWhere,
								  EXPRKIND_QUAL);
		/* exclRelTlist 只包含 Var，因此无需预处理 */
	}

	foreach(l, parse->mergeActionList)
	{
		MergeAction *action = (MergeAction *) lfirst(l);

		action->targetList = (List *)
			preprocess_expression(root,
								  (Node *) action->targetList,
								  EXPRKIND_TARGET);
		action->qual =
			preprocess_expression(root,
								  (Node *) action->qual,
								  EXPRKIND_QUAL);
	}

	parse->mergeJoinCondition =
		preprocess_expression(root, parse->mergeJoinCondition, EXPRKIND_QUAL);

	root->append_rel_list = (List *)
		preprocess_expression(root, (Node *) root->append_rel_list,
							  EXPRKIND_APPINFO);

	/* Also need to preprocess expressions within RTEs */
	foreach(l, parse->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, l);
		int			kind;
		ListCell   *lcsq;

		if (rte->rtekind == RTE_RELATION)
		{
			if (rte->tablesample)
				rte->tablesample = (TableSampleClause *)
					preprocess_expression(root,
										  (Node *) rte->tablesample,
										  EXPRKIND_TABLESAMPLE);
		}
		else if (rte->rtekind == RTE_SUBQUERY)
		{
			/*
			 * 我们还不想对子查询的表达式做全部预处理，因为那将在规划它时
			 * 发生。但如果它包含我们这一层的任何连接别名，那些现在就必须
			 * 展开，因为子查询的规划不会去做。这只可能在子查询是 LATERAL
			 * 时成立。
			 */
			if (rte->lateral && root->hasJoinRTEs)
				rte->subquery = (Query *)
					flatten_join_alias_vars(root, root->parse,
											(Node *) rte->subquery);
		}
		else if (rte->rtekind == RTE_FUNCTION)
		{
			/* 完整地预处理函数表达式 */
			kind = rte->lateral ? EXPRKIND_RTFUNC_LATERAL : EXPRKIND_RTFUNC;
			rte->functions = (List *)
				preprocess_expression(root, (Node *) rte->functions, kind);
		}
		else if (rte->rtekind == RTE_TABLEFUNC)
		{
			/* 完整地预处理函数表达式 */
			kind = rte->lateral ? EXPRKIND_TABLEFUNC_LATERAL : EXPRKIND_TABLEFUNC;
			rte->tablefunc = (TableFunc *)
				preprocess_expression(root, (Node *) rte->tablefunc, kind);
		}
		else if (rte->rtekind == RTE_VALUES)
		{
			/* 完整地预处理 VALUES 列表 */
			kind = rte->lateral ? EXPRKIND_VALUES_LATERAL : EXPRKIND_VALUES;
			rte->values_lists = (List *)
				preprocess_expression(root, (Node *) rte->values_lists, kind);
		}
		else if (rte->rtekind == RTE_GROUP)
		{
			/* 完整地预处理 groupexprs 列表 */
			rte->groupexprs = (List *)
				preprocess_expression(root, (Node *) rte->groupexprs,
									  EXPRKIND_GROUPEXPR);
		}

		/*
		 * 将 securityQuals 列表的每个元素当作一个独立的限定表达式（它确实
		 * 如此）来处理。我们需要这样做以获得 AND/OR 结构的正确规范化。
		 * 注意这会将每个元素转换为一个隐式 AND 子列表。
		 */
		foreach(lcsq, rte->securityQuals)
		{
			lfirst(lcsq) = preprocess_expression(root,
												 (Node *) lfirst(lcsq),
												 EXPRKIND_QUAL);
		}
	}

	/*
	 * 既然表达式预处理已经完成，特别是连接别名变量的扁平化已经完成，
	 * 就移除 joinaliasvars 列表。它们不再与树其余部分的表达式相符，因为
	 * 我们没有预处理那些列表中的表达式（也不想这样做；例如在那里展开
	 * SubLink 会产生一个无用的未引用子计划）。把它们留在原地只会给后续
	 * 对树的扫描制造隐患。我们可以试图通过在这一点之后做的每次树扫描中
	 * 使用 QTW_IGNORE_JOINALIASES 来避免，但那听起来不太可靠。
	 */
	if (root->hasJoinRTEs)
	{
		foreach(l, parse->rtable)
		{
			RangeTblEntry *rte = lfirst_node(RangeTblEntry, l);

			rte->joinaliasvars = NIL;
		}
	}

	/*
	 * 在扁平化 GROUP Var 之前，检查哪些 HAVING 子句存在排序规则冲突。
	 * 当 GROUP BY 使用非确定性排序规则时，对分组而言"相等"的值在另一种
	 * 排序规则下可能是可区分的。如果这样的 HAVING 子句被移到 WHERE，它将
	 * 在分组之前过滤单独的各行，可能消除某个组的某些成员，从而改变聚合
	 * 结果。
	 *
	 * 我们在 flatten_group_exprs 之前做这个检查，因为我们通过检查 Var 是否
	 * 引用 RTE_GROUP 就能轻易识别分组表达式，而这样的 Var 直接以它们的
	 * varcollid 携带 GROUP BY 排序规则。扁平化之后，这些 Var 被底层表达式
	 * 替换，我们就必须将 HAVING 子句中的表达式匹配回分组表达式，这要复杂得多。
	 */
	if (parse->hasGroupRTE)
		havingCollationConflicts =
			find_having_collation_conflicts(parse, root->group_rtindex);
	else
		havingCollationConflicts = NULL;

	/*
	 * 将子查询目标列表和 havingQual 中引用 GROUP 输出的任何 Var 替换为底层的
	 * 分组表达式。
	 *
	 * 注意我们需要在对分组表达式预处理之后执行这个替换。这是为了确保
	 * 分组表达式中包含的每个 SubLink 只有一个 SubPlan 实例。
	 */
	if (parse->hasGroupRTE)
	{
		parse->targetList = (List *)
			flatten_group_exprs(root, root->parse, (Node *) parse->targetList);
		parse->havingQual =
			flatten_group_exprs(root, root->parse, parse->havingQual);
	}

	/* 常量折叠可能已经移除了所有集合返回函数 */
	if (parse->hasTargetSRFs)
		parse->hasTargetSRFs = expression_returns_set((Node *) parse->targetList);

	/*
	 * 如果我们有分组集（grouping sets），将本查询的 groupingSets 树展开为
	 * 一个扁平的分组集列表。我们需要在优化 HAVING 之前做这件事，因为在获得
	 * 这种表示形式之前，我们无法轻易判断是否存在空分组集。
	 */
	if (parse->groupingSets)
	{
		parse->groupingSets =
			expand_grouping_sets(parse->groupingSets, parse->groupDistinct, -1);
	}

	/*
	 * 在某些情况下，我们可能希望将 HAVING 子句转移到 WHERE。如果 HAVING 子句
	 * 包含聚合函数（显然）或易失函数（因为 HAVING 子句应当每组只执行一次），
	 * 我们就不能这样做。如果存在任何分组集、并且该子句引用了被分组集置为
	 * 可空的列，我们也不能这样做；这些列的置空值在分组步骤之前不可用。
	 * （对 groupClause 的测试看起来似乎不对，但没问题：它只是一个优化，
	 * 用于避免在没有 HAVING 子句不可能含有任何 Var 的情况下仍运行 pull_varnos。）
	 *
	 * 如果 HAVING 子句对某个 GROUP BY 排序规则为非确定性的分组表达式使用了
	 * 与 GROUP BY 不同的排序规则，我们同样不能这样做。这在 flatten_group_exprs
	 * 之前就被检测（见上文 find_having_collation_conflicts）并记录在
	 * havingCollationConflicts 位图中。该位图的索引此处仍然有效，因为
	 * flatten_group_exprs 使用 expression_tree_mutator，它保留了 havingQual
	 * 的列表长度和顺序。
	 *
	 * 此外，该子句的执行可能非常昂贵，以至于我们最好每组只执行一次，即使
	 * 损失了选择率。这很难在不把整个规划过程做两遍的情况下估算，因此我们
	 * 使用一个启发式方法：包含子计划的子句留在 HAVING 中。否则，我们将
	 * HAVING 子句移动或复制到 WHERE，希望在聚合之前而非之后消除元组。
	 *
	 * 如果查询没有空分组集，那么我们可以简单地将这样的子句移入 WHERE；
	 * 任何不满足子句的组都不会出现在输出中，因为它的元组都不会到达分组或
	 * 聚合阶段。否则我们必须将子句保留在 HAVING 中，以确保不会发出虚假的
	 * 聚合行。但那样 HAVING 子句必须是退化的（无变量），因此我们可以把它
	 * 复制到 WHERE，以便 query_planner() 能在一个门控 Result 节点中使用它。
	 * （这本来可以做得更好，但似乎不值得优化。）
	 *
	 * 注意 HAVING 子句可能包含未完全预处理的表达式。这可能在表达式是分组项
	 * 的一部分时发生。这种情况下，它们在解析器中被替换为 GROUP Var，然后在
	 * 对 havingQual 的表达式预处理完成之后被替换回来。如果子句保留在 HAVING
	 * 中，这就不是问题，因为这些表达式会在 setrefs.c 中匹配到底层目标项。
	 * 但是，如果子句被移动或复制到 WHERE，我们需要确保这些表达式已被完全
	 * 预处理。
	 *
	 * 注意 havingQual 和 parse->jointree->quals 此时都采用隐式 AND 列表形式，
	 * 尽管它们被声明为 Node *。
	 */
	newHaving = NIL;
	havingIdx = 0;
	foreach(l, (List *) parse->havingQual)
	{
		Node	   *havingclause = (Node *) lfirst(l);

		if (contain_agg_clause(havingclause) ||
			contain_volatile_functions(havingclause) ||
			contain_subplans(havingclause) ||
			bms_is_member(havingIdx, havingCollationConflicts) ||
			(parse->groupClause && parse->groupingSets &&
			 bms_is_member(root->group_rtindex, pull_varnos(root, havingclause))))
		{
			/* keep it in HAVING */
			newHaving = lappend(newHaving, havingclause);
		}
		else if (parse->groupClause &&
				 (parse->groupingSets == NIL ||
				  (List *) linitial(parse->groupingSets) != NIL))
		{
			/* There is GROUP BY, but no empty grouping set */
			Node	   *whereclause;

			/* Preprocess the HAVING clause fully */
			whereclause = preprocess_expression(root, havingclause,
												EXPRKIND_QUAL);
			/* ... and move it to WHERE */
			parse->jointree->quals = (Node *)
				list_concat((List *) parse->jointree->quals,
							(List *) whereclause);
		}
		else
		{
			/* There is an empty grouping set (perhaps implicitly) */
			Node	   *whereclause;

			/* Preprocess the HAVING clause fully */
			whereclause = preprocess_expression(root, copyObject(havingclause),
												EXPRKIND_QUAL);
			/* ... and put a copy in WHERE */
			parse->jointree->quals = (Node *)
				list_concat((List *) parse->jointree->quals,
							(List *) whereclause);
			/* ... and also keep it in HAVING */
			newHaving = lappend(newHaving, havingclause);
		}

		havingIdx++;
	}
	parse->havingQual = (Node *) newHaving;

	/*
	 * If we have any outer joins, try to reduce them to plain inner joins.
	 * This step is most easily done after we've done expression
	 * preprocessing.
	 */
	if (hasOuterJoins)
		reduce_outer_joins(root);

	/*
	 * 如果我们有任何 RTE_RESULT 关系，看它们能否从连接树中删除。我们也依赖
	 * 这个处理来扁平化外连接下的单子节点 FromExpr。这一步在表达式预处理和
	 * 外连接归约之后做最为有效。
	 */
	if (hasResultRTEs || hasOuterJoins)
		remove_useless_result_rtes(root);

	/*
	 * 执行主要的规划。
	 */
	grouping_planner(root, tuple_fraction, setops);

	/*
	 * 捕获我们所能访问的外层 param ID 集合，供后续的 extParam/allParam
	 * 计算使用。
	 */
	SS_identify_outer_params(root);

	/*
	 * 如果在本查询层级创建了任何 initPlan，调整存活路径的代价和并行安全标志
	 * 以将其考虑在内。initPlan 实际上要到 create_plan() 运行才会被附加到计划树，
	 * 但我们现在就必须包含它们的影响。
	 */
	final_rel = fetch_upper_rel(root, UPPERREL_FINAL, NULL);
	SS_charge_for_initplans(root, final_rel);

	/*
	 * 确保我们已经为最终关系确定了最廉价的路径。（在这里而非 grouping_planner
	 * 中做这件事，我们能将 initPlan 代价纳入决策，尽管这不太可能改变任何东西。）
	 */
	set_cheapest(final_rel);

	return root;
}

/*
 * preprocess_expression
 *		为表达式做 subquery_planner 的预处理工作，表达式可以是目标列表、
 *		一个 WHERE 子句（包括 JOIN/ON 条件）、一个 HAVING 子句，或少数
 *		其他东西。
 */
static Node *
preprocess_expression(PlannerInfo *root, Node *expr, int kind)
{
	/*
	 * 如果表达式为空则快速返回。这种情况出现得足够频繁，值得检查一下。
	 * 注意 null->null 也是隐式 AND 结果格式的正确转换。
	 */
	if (expr == NULL)
		return NULL;

	/*
	 * 如果查询有任何连接类型的 RTE，将连接别名变量替换为基关系变量。
	 * 我们必须先做这件事，因为我们可能从 joinaliasvars 列表中提取出的
	 * 表达式尚未被预处理。例如，如果我们在子链接处理之后才做此事，从连接
	 * 别名展开出的子链接就不会被处理。但在非 LATERAL 的函数 RTE、VALUES
	 * 列表和 TABLESAMPLE 子句中可以跳过，因为它们不可能包含当前查询层级的
	 * 任何 Var。
	 */
	if (root->hasJoinRTEs &&
		!(kind == EXPRKIND_RTFUNC ||
		  kind == EXPRKIND_VALUES ||
		  kind == EXPRKIND_TABLESAMPLE ||
		  kind == EXPRKIND_TABLEFUNC))
		expr = flatten_join_alias_vars(root, root->parse, expr);

	/*
	 * 简化常量表达式。对于函数类型的 RTE，这已被 preprocess_function_rtes
	 * 做过。（但注意我们必须对 EXPRKIND_RTFUNC_LATERAL 再做一次，因为它们
	 * 此刻可能含有由子查询或连接别名变量扁平化插入的未简化子表达式。）
	 *
	 * 注意：这一步骤的一个必要效果是将在名参数函数调用转换为位置表示法，
	 * 并插入函数任何默认参数的当前实际值。为确保这一点发生，我们*必须*在
	 * 此处处理所有表达式。以前的 PG 版本有时会跳过常量简化，如果它看起来
	 * 不值得麻烦的话，但我们现在不能再这样做了。
	 *
	 * 注意：这还会将嵌套的 AND 和 OR 表达式扁平化为 N 元形式。此点之后的
	 * 所有限定表达式处理都必须小心维持 AND/OR 的扁平性 —— 即不要生成 AND
	 * 直接位于 AND 之下、或 OR 直接位于 OR 之下的树。
	 */
	if (kind != EXPRKIND_RTFUNC)
		expr = eval_const_expressions(root, expr);

	/*
	 * 如果是限定条件或 havingQual，将其规范化。
	 */
	if (kind == EXPRKIND_QUAL)
	{
		expr = (Node *) canonicalize_qual((Expr *) expr, false);

#ifdef OPTIMIZER_DEBUG
		printf("After canonicalize_qual()\n");
		pprint(expr);
#endif
	}

	/*
	 * 检查带有 Const 数组的 ANY ScalarArrayOpExpr，并为那些改用哈希查找
	 * 可能执行得更快的设置其 hashfuncid，以取代线性搜索。
	 */
	if (kind == EXPRKIND_QUAL || kind == EXPRKIND_TARGET)
	{
		convert_saop_to_hashed_saop(expr);
	}

	/* 将子链接展开为子计划 */
	if (root->parse->hasSubLinks)
		expr = SS_process_sublinks(root, expr, (kind == EXPRKIND_QUAL));

	/*
	 * XXX 除非你已经读懂了 SS_replace_correlation_vars 中的注释，否则不要
	 * 在这里插入任何东西……
	 */

	/* 将上层 Var 替换为 Param 节点（这在 VALUES 中是可能发生的） */
	if (root->query_level > 1)
		expr = SS_replace_correlation_vars(root, expr);

	/*
	 * 如果是限定条件或 havingQual，将其转换为隐式 AND 格式。（我们不想在
	 * eval_const_expressions 之前做这件事，因为后者将无法正确简化顶层
	 * AND。而且 SS_process_sublinks 期望显式 AND 格式。）
	 */
	if (kind == EXPRKIND_QUAL)
		expr = (Node *) make_ands_implicit((Expr *) expr);

	return expr;
}

/*
 * preprocess_qual_conditions
 *		Recursively scan the query's jointree and do subquery_planner's
 *		preprocessing work on each qual condition found therein.
 */
static void
preprocess_qual_conditions(PlannerInfo *root, Node *jtnode)
{
	if (jtnode == NULL)
		return;
	if (IsA(jtnode, RangeTblRef))
	{
		/* 这里无需处理 */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
			preprocess_qual_conditions(root, lfirst(l));

		f->quals = preprocess_expression(root, f->quals, EXPRKIND_QUAL);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		preprocess_qual_conditions(root, j->larg);
		preprocess_qual_conditions(root, j->rarg);

		j->quals = preprocess_expression(root, j->quals, EXPRKIND_QUAL);
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}

/*
 * find_having_collation_conflicts
 *	  识别那些由于与 GROUP BY 存在排序规则（collation）不匹配而不能移动到
 *	  WHERE 的 HAVING 子句。
 *
 * 本函数必须在 flatten_group_exprs 之前调用，此时 HAVING 子句仍然包含
 * GROUP Var（引用 RTE_GROUP 的 Var）。这些 GROUP Var 以其 varcollid 携带
 * GROUP BY 的排序规则。当某个 GROUP Var 的 varcollid 是非确定性的，并且其
 * 路径上某个感知排序规则的祖先节点应用了不同的 inputcollid 时，就会发生
 * 冲突：因为该运算符会区分 GROUP BY 认为相等的值，所以将该子句下推到
 * WHERE 是不安全的。
 *
 * 返回一个 Bitmapset，其中包含 havingQual 列表中那些存在排序规则冲突、
 * 必须保留在 HAVING 里的子句的从零开始的下标。
 */
static Bitmapset *
find_having_collation_conflicts(Query *parse, Index group_rtindex)
{
	Bitmapset  *result = NULL;
	having_collation_ctx ctx;
	int			idx;

	if (parse->havingQual == NULL)
		return NULL;

	ctx.group_rtindex = group_rtindex;
	ctx.ancestor_collids = NIL;

	idx = 0;
	foreach_ptr(Node, clause, (List *) parse->havingQual)
	{
		if (having_collation_conflict_walker(clause, &ctx))
			result = bms_add_member(result, idx);
		idx++;
		Assert(ctx.ancestor_collids == NIL);
	}

	return result;
}

/*
 * find_having_collation_conflicts 的遍历（walker）函数。
 *
 * 自顶向下遍历子句，维护一个由感知排序规则的祖先节点贡献的 inputcollid
 * 栈。在每个具有非确定性 varcollid 的 GROUP Var 处，如果任一祖先的
 * inputcollid 与该 GROUP Var 的 varcollid 不同，则该子句存在冲突。大多数
 * 感知排序规则的节点通过 exprInputCollation() 暴露其 inputcollid。有两种
 * 结构性的例外需要特殊处理：
 *
 * - RowCompareExpr 在 inputcollids[] 中为每一列携带一个 inputcollid，因此
 *   我们显式地下降到它的 (largs[i], rargs[i]) 对中，并将对应的排序规则压入
 *   栈中。
 *
 * - 简单 CASE（arg 非 NULL 的 CaseExpr）把 arg 保存在 WHEN 的 OpExpr 之外，
 *   尽管 WHEN 的 OpExpr 才是比较的 inputcollid 所在之处。解析分析会将每个
 *   WHEN 构建为 "OpExpr(CaseTestExpr op val)"——其中 CaseTestExpr 是 arg 的
 *   占位符。因此在遍历 cexpr->arg 之前，我们将每个 WHEN 的 inputcollid 压入
 *   祖先栈，这样位于 arg 处的 GROUP Var 就会针对与那些 WHEN 比较相同的
 *   排序规则进行检查。随后在未改变的栈下遍历 WHEN 体和 defresult，使它们
 *   自身的排序规则上下文由默认路径捕获。
 */
static bool
having_collation_conflict_walker(Node *node, having_collation_ctx *ctx)
{
	Oid			this_collid;
	bool		result;

	if (node == NULL)
		return false;

	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		/* 这里不应该看到任何上层的 Var */
		Assert(var->varlevelsup == 0);

		if (var->varno == ctx->group_rtindex &&
			OidIsValid(var->varcollid) &&
			!get_collation_isdeterministic(var->varcollid))
		{
			foreach_oid(collid, ctx->ancestor_collids)
			{
				if (collid != var->varcollid)
					return true;
			}
		}
		return false;
	}

	if (IsA(node, RowCompareExpr))
	{
		RowCompareExpr *rcexpr = (RowCompareExpr *) node;
		ListCell   *lc_l;
		ListCell   *lc_r;
		ListCell   *lc_c;

		/*
		 * 行比较的每一列都在其各自的 inputcollids[i] 下进行比较。遍历每个
		 * (largs[i], rargs[i]) 对，并压入对应的排序规则，这样第 i 列中的 Var
		 * 就会针对实际应用于它的排序规则进行检查。
		 */
		forthree(lc_l, rcexpr->largs,
				 lc_r, rcexpr->rargs,
				 lc_c, rcexpr->inputcollids)
		{
			Oid			collid = lfirst_oid(lc_c);
			bool		found;

			if (OidIsValid(collid))
				ctx->ancestor_collids = lappend_oid(ctx->ancestor_collids,
													collid);

			found = having_collation_conflict_walker((Node *) lfirst(lc_l),
													 ctx) ||
				having_collation_conflict_walker((Node *) lfirst(lc_r),
												 ctx);

			if (OidIsValid(collid))
				ctx->ancestor_collids =
					list_delete_last(ctx->ancestor_collids);

			if (found)
				return true;
		}
		return false;
	}

	if (IsA(node, CaseExpr) && ((CaseExpr *) node)->arg != NULL)
	{
		CaseExpr   *cexpr = (CaseExpr *) node;
		int			saved_len = list_length(ctx->ancestor_collids);
		bool		found;

		/*
		 * 在遍历 cexpr->arg 之前压入每个 WHEN 的 inputcollid，因为每个 WHEN
		 * 都隐式地在该 inputcollid 下比较 arg。
		 */
		foreach_node(CaseWhen, cw, cexpr->args)
		{
			Oid			collid = exprInputCollation((Node *) cw->expr);

			if (OidIsValid(collid))
				ctx->ancestor_collids = lappend_oid(ctx->ancestor_collids,
													collid);
		}

		found = having_collation_conflict_walker((Node *) cexpr->arg, ctx);

		ctx->ancestor_collids = list_truncate(ctx->ancestor_collids,
											  saved_len);

		if (found)
			return true;

		/*
		 * 在未改变的祖先栈下遍历 WHEN 体和 defresult；它们内部的任何
		 * inputcollid 都由默认路径捕获。
		 */
		foreach_node(CaseWhen, cw, cexpr->args)
		{
			if (having_collation_conflict_walker((Node *) cw->expr, ctx) ||
				having_collation_conflict_walker((Node *) cw->result, ctx))
				return true;
		}
		return having_collation_conflict_walker((Node *) cexpr->defresult,
												ctx);
	}

	this_collid = exprInputCollation(node);
	if (OidIsValid(this_collid))
		ctx->ancestor_collids = lappend_oid(ctx->ancestor_collids,
											this_collid);

	result = expression_tree_walker(node, having_collation_conflict_walker,
									ctx);

	if (OidIsValid(this_collid))
		ctx->ancestor_collids = list_delete_last(ctx->ancestor_collids);

	return result;
}

/*
 * preprocess_phv_expression
 *	  对一个已被上拉（pulled up）的 PlaceHolderVar 表达式进行预处理。
 *
 * 如果一个 LATERAL 子查询引用了另一个子查询的输出，而由于中间存在一个外
 * 连接，该输出必须被包装在 PlaceHolderVar 中，那么我们会将该 PlaceHolderVar
 * 表达式下推到子查询中，随后在 find_lateral_references 期间再把它上拉回来；
 * find_lateral_references 在 subquery_planner 已经预处理完当前查询层级最初
 * 存在的所有表达式之后运行。因此我们需要在那时对它进行预处理。
 */
Expr *
preprocess_phv_expression(PlannerInfo *root, Expr *expr)
{
	return (Expr *) preprocess_expression(root, (Node *) expr, EXPRKIND_PHV);
}

/*--------------------
 * grouping_planner
 *	  执行与分组、聚合等相关的规划步骤。
 *
 * 本函数将所有必要的顶层处理添加到 query_planner 产生的扫描/连接 Path 上。
 *
 * tuple_fraction 是我们预期将被检索的元组比例。tuple_fraction 的解释如下：
 *	  0：预期检索所有元组（常规情况）
 *	  0 < tuple_fraction < 1：预期检索计划可提供元组的给定比例
 *	  tuple_fraction >= 1：tuple_fraction 是预期被检索的元组的绝对数量
 *		（即一个 LIMIT 规格）。
 * setops 用于集合操作子查询，为子查询提供其被使用的上下文，以便能够生成
 * 为集合操作正确排序的 Path。当不是在规划集合操作的子节点时，或者作为一个
 * 对有序输入不感兴趣的集合操作子节点时，为 NULL。
 *
 * 无返回值；有用的输出在我们附加到 *root 中 (UPPERREL_FINAL, NULL) upperrel
 * 上的 Path 里。此外，root->processed_tlist 包含最终处理后的目标列表。
 *
 * 注意我们尚未对最终 rel 执行 set_cheapest()；把这一步留给调用者更方便。
 *--------------------
 */
static void
grouping_planner(PlannerInfo *root, double tuple_fraction,
				 SetOperationStmt *setops)
{
	Query	   *parse = root->parse;
	int64		offset_est = 0;
	int64		count_est = 0;
	double		limit_tuples = -1.0;
	bool		have_postponed_srfs = false;
	PathTarget *final_target;
	List	   *final_targets;
	List	   *final_targets_contain_srfs;
	bool		final_target_parallel_safe;
	RelOptInfo *current_rel;
	RelOptInfo *final_rel;
	FinalPathExtraData extra;
	ListCell   *lc;

	/* 如果有 LIMIT/OFFSET，则调整调用者提供的 tuple_fraction */
	if (parse->limitCount || parse->limitOffset)
	{
		tuple_fraction = preprocess_limit(root, tuple_fraction,
										  &offset_est, &count_est);

		/*
		 * 如果我们有一个已知的 LIMIT，且没有未知的 OFFSET，我们就可以估算
		 * 使用有界排序（bounded sort）的效果。
		 */
		if (count_est > 0 && offset_est >= 0)
			limit_tuples = (double) count_est + (double) offset_est;
	}

	/* 使 tuple_fraction 可被较低层级的例程访问 */
	root->tuple_fraction = tuple_fraction;

	if (parse->setOperations)
	{
		/*
		 * 为集合操作构造 Path。除了可能的顶层排序和/或 LIMIT 之外，结果不
		 * 再需要任何处理。注意，递归 union 的任何特殊处理由
		 * plan_set_operations 负责。
		 */
		current_rel = plan_set_operations(root);

		/*
		 * 我们不应该需要调用 preprocess_targetlist，因为此处必定处于一个
		 * SELECT 查询节点中。相反，使用 plan_set_operations 返回的
		 * processed_tlist（因为它告诉我们是否返回了任何 resjunk 列！），并
		 * 从原始 tlist 中转移任何排序键信息。
		 */
		Assert(parse->commandType == CMD_SELECT);

		/* 为安全起见，复制 processed_tlist 而不是就地修改 */
		root->processed_tlist =
			postprocess_setop_tlist(copyObject(root->processed_tlist),
									parse->targetList);

		/* 同时提取集合操作结果 tlist 的 PathTarget 形式 */
		final_target = current_rel->cheapest_total_path->pathtarget;

		/* 并检查它是否并行安全 */
		final_target_parallel_safe =
			is_parallel_safe(root, (Node *) final_target->exprs);

		/* 集合操作结果 tlist 不可能包含任何 SRF */
		Assert(!parse->hasTargetSRFs);
		final_targets = final_targets_contain_srfs = NIL;

		/*
		 * 这里无法处理 FOR [KEY] UPDATE/SHARE（解析器应该已经检查过了，但
		 * 我们还是确认一下）。
		 */
		if (parse->rowMarks)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			/*------
			  translator: %s is a SQL row locking clause such as FOR UPDATE */
					 errmsg("%s is not allowed with UNION/INTERSECT/EXCEPT",
							LCS_asString(linitial_node(RowMarkClause,
													   parse->rowMarks)->strength))));

		/*
		 * 计算表示结果排序要求的 pathkeys
		 */
		Assert(parse->distinctClause == NIL);
		root->sort_pathkeys = make_pathkeys_for_sortclauses(root,
															parse->sortClause,
															root->processed_tlist);
	}
	else
	{
		/* 没有集合操作，执行常规规划 */
		PathTarget *sort_input_target;
		List	   *sort_input_targets;
		List	   *sort_input_targets_contain_srfs;
		bool		sort_input_target_parallel_safe;
		PathTarget *grouping_target;
		List	   *grouping_targets;
		List	   *grouping_targets_contain_srfs;
		bool		grouping_target_parallel_safe;
		PathTarget *scanjoin_target;
		List	   *scanjoin_targets;
		List	   *scanjoin_targets_contain_srfs;
		bool		scanjoin_target_parallel_safe;
		bool		scanjoin_target_same_exprs;
		bool		have_grouping;
		WindowFuncLists *wflists = NULL;
		List	   *activeWindows = NIL;
		grouping_sets_data *gset_data = NULL;
		standard_qp_extra qp_extra;

		/* 递归查询应始终具有 setOperations */
		Assert(!root->hasRecursion);

		/* 预处理 grouping sets 和 GROUP BY 子句（如果有的话） */
		if (parse->groupingSets)
		{
			gset_data = preprocess_grouping_sets(root);
		}
		else if (parse->groupClause)
		{
			/* 预处理常规 GROUP BY 子句（如果有的话） */
			root->processed_groupClause = preprocess_groupclause(root, NIL);
		}

		/*
		 * 预处理目标列表。注意，剩余的大部分规划工作将使用 tlist 的
		 * PathTarget 表示来完成，但我们还必须维护最终 tlist 的完整表示，以便
		 * 能够将其修饰信息（resnames 等）转移到已完成 Plan 的最顶层 tlist 上。
		 * 这个完整表示保存在 processed_tlist 中。
		 */
		preprocess_targetlist(root);

		/*
		 * 用已解析的 aggtranstype 标记所有聚合，并检测那些重复的、或者可以
		 * 共享转换状态（transition state）的聚合。我们
		 * must do this before slicing and dicing the tlist into various
		 * pathtargets, else some copies of the Aggref nodes might escape
		 * being marked.
		 */
		if (parse->hasAggs)
		{
			preprocess_aggrefs(root, (Node *) root->processed_tlist);
			preprocess_aggrefs(root, (Node *) parse->havingQual);
		}

		/*
		 * Locate any window functions in the tlist.  (We don't need to look
		 * anywhere else, since expressions used in ORDER BY will be in there
		 * too.)  Note that they could all have been eliminated by constant
		 * folding, in which case we don't need to do any more work.
		 */
		if (parse->hasWindowFuncs)
		{
			wflists = find_window_functions((Node *) root->processed_tlist,
											list_length(parse->windowClause));
			if (wflists->numWindowFuncs > 0)
			{
				/*
				 * 看看是否可以对每个 WindowClause 做一些修改，以便让执行器
				 * 更快地执行 WindowFunc。
				 */
				optimize_window_clauses(root, wflists);

				/* 提取实际使用的窗口列表。 */
				activeWindows = select_active_windows(root, wflists);

				/* 确保它们都有名称，供 EXPLAIN 使用。 */
				name_active_windows(activeWindows);
			}
			else
				parse->hasWindowFuncs = false;
		}

		/*
		 * 预处理 MIN/MAX 聚合（如果有的话）。注意：在这里与 query_planner()
		 * 调用之间添加逻辑时要小心。任何在 MIN/MAX 可优化情况下所需的处理，
		 * 都必须在 planagg.c 中重复实现一份。
		 */
		if (parse->hasAggs)
			preprocess_minmax_aggregates(root);

		/*
		 * 判断 query_planner 的结果子计划需要返回的行数是否存在硬性上限。
		 * 即使我们知道一个整体的硬性上限，如果查询包含任何分组/聚合操作，
		 * 或者 tlist 中含有 SRF，该上限也不适用。
		 */
		if (parse->groupClause ||
			parse->groupingSets ||
			parse->distinctClause ||
			parse->hasAggs ||
			parse->hasWindowFuncs ||
			parse->hasTargetSRFs ||
			root->hasHavingQual)
			root->limit_tuples = -1.0;
		else
			root->limit_tuples = limit_tuples;

		/* 设置 standard_qp_callback 所需的数据 */
		qp_extra.activeWindows = activeWindows;
		qp_extra.gset_data = gset_data;

		/*
		 * 如果我们是某个集合操作的子查询，则将 SetOperationStmt 存入
		 * qp_extra。
		 */
		qp_extra.setop = setops;

		/*
		 * 为该 Query 的扫描/连接部分（即 FROM/WHERE 子句所代表的处理）生成
		 * 最优的未排序路径和预排序路径。（注意可能没有任何预排序路径。）
		 * 我们还会（在 standard_qp_callback 中）生成查询的排序子句、distinct
		 * 子句等的 pathkey 表示。
		 */
		current_rel = query_planner(root, standard_qp_callback, &qp_extra);

		/*
		 * 将查询的结果 tlist 转换为 PathTarget 格式。
		 *
		 * 注意：这一步不能在 query_planner() 执行 appendrel 展开之前进行，
		 * 因为那可能会向 root->processed_tlist 添加 resjunk 条目。等到其后
		 * 再做还有一个好处：目标宽度估算可以使用在 query_planner() 内部
		 * 得到的每个 Var 的宽度数值。
		 */
		final_target = create_pathtarget(root, root->processed_tlist);
		final_target_parallel_safe =
			is_parallel_safe(root, (Node *) final_target->exprs);

		/*
		 * 如果给定了 ORDER BY，考虑我们是否应该使用一个排序后（post-sort）
		 * 的投影，若是，则为前面的步骤计算调整后的 target。
		 */
		if (parse->sortClause)
		{
			sort_input_target = make_sort_input_target(root,
													   final_target,
													   &have_postponed_srfs);
			sort_input_target_parallel_safe =
				is_parallel_safe(root, (Node *) sort_input_target->exprs);
		}
		else
		{
			sort_input_target = final_target;
			sort_input_target_parallel_safe = final_target_parallel_safe;
		}

		/*
		 * 如果我们有窗口函数要处理，则任何分组步骤的输出都需要是窗口函数
		 * 所需要的内容；否则，它应当是 sort_input_target。
		 */
		if (activeWindows)
		{
			grouping_target = make_window_input_target(root,
													   final_target,
													   activeWindows);
			grouping_target_parallel_safe =
				is_parallel_safe(root, (Node *) grouping_target->exprs);
		}
		else
		{
			grouping_target = sort_input_target;
			grouping_target_parallel_safe = sort_input_target_parallel_safe;
		}

		/*
		 * 如果我们有分组或聚合要做，最顶层的扫描/连接计划节点必须发出分组
		 * 步骤所需要的内容；否则，它应当发出 grouping_target。
		 */
		have_grouping = (parse->groupClause || parse->groupingSets ||
						 parse->hasAggs || root->hasHavingQual);
		if (have_grouping)
		{
			scanjoin_target = make_group_input_target(root, final_target);
			scanjoin_target_parallel_safe =
				is_parallel_safe(root, (Node *) scanjoin_target->exprs);
		}
		else
		{
			scanjoin_target = grouping_target;
			scanjoin_target_parallel_safe = grouping_target_parallel_safe;
		}

		/*
		 * 如果目标列表中有任何 SRF，我们必须将每个这样的 PathTarget 分离为
		 * 计算 SRF 的 target 和不含 SRF 的 target。用不含 SRF 的版本替换每个
		 * 命名的 target，并记住我们之后需要添加的额外投影步骤列表。
		 */
		if (parse->hasTargetSRFs)
		{
			/* final_target 不重新计算 sort_input_target 中的任何 SRF */
			split_pathtarget_at_srfs(root, final_target, sort_input_target,
									 &final_targets,
									 &final_targets_contain_srfs);
			final_target = linitial_node(PathTarget, final_targets);
			Assert(!linitial_int(final_targets_contain_srfs));
			/* 对 sort_input_target 与 grouping_target 同理 */
			split_pathtarget_at_srfs(root, sort_input_target, grouping_target,
									 &sort_input_targets,
									 &sort_input_targets_contain_srfs);
			sort_input_target = linitial_node(PathTarget, sort_input_targets);
			Assert(!linitial_int(sort_input_targets_contain_srfs));
			/* 对 grouping_target 与 scanjoin_target 同理 */
			split_pathtarget_at_srfs_grouping(root,
											  grouping_target, scanjoin_target,
											  &grouping_targets,
											  &grouping_targets_contain_srfs);
			grouping_target = linitial_node(PathTarget, grouping_targets);
			Assert(!linitial_int(grouping_targets_contain_srfs));
			/* scanjoin_target 不会为其预先计算任何 SRF */
			split_pathtarget_at_srfs(root, scanjoin_target, NULL,
									 &scanjoin_targets,
									 &scanjoin_targets_contain_srfs);
			scanjoin_target = linitial_node(PathTarget, scanjoin_targets);
			Assert(!linitial_int(scanjoin_targets_contain_srfs));
		}
		else
		{
			/* 初始化列表；对其中大多数来说，哑值即可 */
			final_targets = final_targets_contain_srfs = NIL;
			sort_input_targets = sort_input_targets_contain_srfs = NIL;
			grouping_targets = grouping_targets_contain_srfs = NIL;
			scanjoin_targets = list_make1(scanjoin_target);
			scanjoin_targets_contain_srfs = NIL;
		}

		/* 应用扫描/连接 target。 */
		scanjoin_target_same_exprs = list_length(scanjoin_targets) == 1
			&& equal(scanjoin_target->exprs, current_rel->reltarget->exprs);
		apply_scanjoin_target_to_paths(root, current_rel, scanjoin_targets,
									   scanjoin_targets_contain_srfs,
									   scanjoin_target_parallel_safe,
									   scanjoin_target_same_exprs);

		/*
		 * 将我们刚刚计算出的各种 upper-rel 的 PathTarget 保存到
		 * root->upper_targets[] 中。核心代码并不使用它，但它为扩展提供了一个
		 * 方便的位置来获取这些信息。为了保持一致，我们保存所有中间 target，
		 * 即便其中某些对应的 upperrel 在本次查询中可能并不需要。
		 */
		root->upper_targets[UPPERREL_FINAL] = final_target;
		root->upper_targets[UPPERREL_ORDERED] = final_target;
		root->upper_targets[UPPERREL_DISTINCT] = sort_input_target;
		root->upper_targets[UPPERREL_PARTIAL_DISTINCT] = sort_input_target;
		root->upper_targets[UPPERREL_WINDOW] = sort_input_target;
		root->upper_targets[UPPERREL_GROUP_AGG] = grouping_target;

		/*
		 * 如果我们有分组和/或聚合，考虑实现它的各种方式。我们构建一个新的
		 * upperrel 来表示这一阶段的输出。
		 */
		if (have_grouping)
		{
			current_rel = create_grouping_paths(root,
												current_rel,
												grouping_target,
												grouping_target_parallel_safe,
												gset_data);
			/* 如果 grouping_target 包含 SRF，则做相应修正 */
			if (parse->hasTargetSRFs)
				adjust_paths_for_srfs(root, current_rel,
									  grouping_targets,
									  grouping_targets_contain_srfs);
		}

		/*
		 * 如果我们有窗口函数，考虑实现它们的各种方式。我们构建一个新的
		 * upperrel 来表示这一阶段的输出。
		 */
		if (activeWindows)
		{
			current_rel = create_window_paths(root,
											  current_rel,
											  grouping_target,
											  sort_input_target,
											  sort_input_target_parallel_safe,
											  wflists,
											  activeWindows);
			/* 如果 sort_input_target 包含 SRF，则做相应修正 */
			if (parse->hasTargetSRFs)
				adjust_paths_for_srfs(root, current_rel,
									  sort_input_targets,
									  sort_input_targets_contain_srfs);
		}

		/*
		 * 如果有 DISTINCT 子句，考虑实现它的各种方式。我们构建一个新的
		 * upperrel 来表示这一阶段的输出。
		 */
		if (parse->distinctClause)
		{
			current_rel = create_distinct_paths(root,
												current_rel,
												sort_input_target);
		}
	}							/* if (setOperations) 结束 */

	/*
	 * 如果给定了 ORDER BY，考虑实现它的各种方式，并生成一个新的 upperrel，
	 * 其中只包含发出正确排序、并投影正确 final_target 的路径。我们可以在此处
	 * 的排序代价计算中应用原始的 limit_tuples 限制，但仅当没有被推迟的 SRF
	 * 时才行。
	 */
	if (parse->sortClause)
	{
		current_rel = create_ordered_paths(root,
										   current_rel,
										   final_target,
										   final_target_parallel_safe,
										   have_postponed_srfs ? -1.0 :
										   limit_tuples);
		/* 如果 final_target 包含 SRF，则做相应修正 */
		if (parse->hasTargetSRFs)
			adjust_paths_for_srfs(root, current_rel,
								  final_targets,
								  final_targets_contain_srfs);
	}

	/*
	 * 现在我们准备构建最终输出的 upperrel。
	 */
	final_rel = fetch_upper_rel(root, UPPERREL_FINAL, NULL);

	/*
	 * 如果输入 rel 被标记为 consider_parallel，并且 LIMIT 子句中没有任何非
	 * 并行安全的内容，那么 final_rel 也可以被标记为 consider_parallel。注意，
	 * 如果查询有 rowMarks，或者不是 SELECT，则该查询中每个关系的
	 * consider_parallel 都将为 false。
	 */
	if (current_rel->consider_parallel &&
		is_parallel_safe(root, parse->limitOffset) &&
		is_parallel_safe(root, parse->limitCount))
		final_rel->consider_parallel = true;

	/*
	 * 如果 current_rel 属于单个 FDW，那么 final_rel 也是如此。
	 */
	final_rel->serverid = current_rel->serverid;
	final_rel->userid = current_rel->userid;
	final_rel->useridiscurrent = current_rel->useridiscurrent;
	final_rel->fdwroutine = current_rel->fdwroutine;

	/*
	 * 为 final_rel 生成路径。插入所有存活下来的路径，并在需要时添加
	 * LockRows、Limit 和/或 ModifyTable 步骤。
	 */
	foreach(lc, current_rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);

		/*
		 * 如果有 FOR [KEY] UPDATE/SHARE 子句，则添加 LockRows 节点。
		 * （注意：我们这里有意测试 parse->rowMarks 而非 root->rowMarks。
		 * 如果只有非加锁的 rowmark，它们应改由 ModifyTable 节点处理。不过，
		 * 进入 LockRows 节点的是 root->rowMarks。）
		 */
		if (parse->rowMarks)
		{
			path = (Path *) create_lockrows_path(root, final_rel, path,
												 root->rowMarks,
												 assign_special_exec_param(root));
		}

		/*
		 * 如果有 LIMIT/OFFSET 子句，则添加 LIMIT 节点。
		 */
		if (limit_needed(parse))
		{
			path = (Path *) create_limit_path(root, final_rel, path,
											  parse->limitOffset,
											  parse->limitCount,
											  parse->limitOption,
											  offset_est, count_est);
		}

		/*
		 * 如果这是一个 INSERT/UPDATE/DELETE/MERGE，则添加 ModifyTable 节点。
		 */
		if (parse->commandType != CMD_SELECT)
		{
			Index		rootRelation;
			List	   *resultRelations = NIL;
			List	   *updateColnosLists = NIL;
			List	   *withCheckOptionLists = NIL;
			List	   *returningLists = NIL;
			List	   *mergeActionLists = NIL;
			List	   *mergeJoinConditions = NIL;
			List	   *rowMarks;

			if (bms_membership(root->all_result_relids) == BMS_MULTIPLE)
			{
				/* 继承的 UPDATE/DELETE/MERGE */
				RelOptInfo *top_result_rel = find_base_rel(root,
														   parse->resultRelation);
				int			resultRelation = -1;

				/* 将根结果 rel 向前传递给执行器。 */
				rootRelation = parse->resultRelation;

				/* 只把叶子子节点添加到 ModifyTable。 */
				while ((resultRelation = bms_next_member(root->leaf_result_relids,
														 resultRelation)) >= 0)
				{
					RelOptInfo *this_result_rel = find_base_rel(root,
																resultRelation);

					/*
					 * 同时排除任何自加入列表以来已变为哑（dummy）的叶子
					 * rel，例如被约束排除（constraint exclusion）排除掉的。
					 */
					if (IS_DUMMY_REL(this_result_rel))
						continue;

					/* 构建 ModifyTable 所需的每个目标 rel 的列表 */
					resultRelations = lappend_int(resultRelations,
												  resultRelation);
					if (parse->commandType == CMD_UPDATE)
					{
						List	   *update_colnos = root->update_colnos;

						if (this_result_rel != top_result_rel)
							update_colnos =
								adjust_inherited_attnums_multilevel(root,
																	update_colnos,
																	this_result_rel->relid,
																	top_result_rel->relid);
						updateColnosLists = lappend(updateColnosLists,
													update_colnos);
					}
					if (parse->withCheckOptions)
					{
						List	   *withCheckOptions = parse->withCheckOptions;

						if (this_result_rel != top_result_rel)
							withCheckOptions = (List *)
								adjust_appendrel_attrs_multilevel(root,
																  (Node *) withCheckOptions,
																  this_result_rel,
																  top_result_rel);
						withCheckOptionLists = lappend(withCheckOptionLists,
													   withCheckOptions);
					}
					if (parse->returningList)
					{
						List	   *returningList = parse->returningList;

						if (this_result_rel != top_result_rel)
							returningList = (List *)
								adjust_appendrel_attrs_multilevel(root,
																  (Node *) returningList,
																  this_result_rel,
																  top_result_rel);
						returningLists = lappend(returningLists,
												 returningList);
					}
					if (parse->mergeActionList)
					{
						ListCell   *l;
						List	   *mergeActionList = NIL;

						/*
						 * 复制 MergeAction，并转换其中引用属性编号的内容。
						 */
						foreach(l, parse->mergeActionList)
						{
							MergeAction *action = lfirst(l),
									   *leaf_action = copyObject(action);

							leaf_action->qual =
								adjust_appendrel_attrs_multilevel(root,
																  (Node *) action->qual,
																  this_result_rel,
																  top_result_rel);
							leaf_action->targetList = (List *)
								adjust_appendrel_attrs_multilevel(root,
																  (Node *) action->targetList,
																  this_result_rel,
																  top_result_rel);
							if (leaf_action->commandType == CMD_UPDATE)
								leaf_action->updateColnos =
									adjust_inherited_attnums_multilevel(root,
																		action->updateColnos,
																		this_result_rel->relid,
																		top_result_rel->relid);
							mergeActionList = lappend(mergeActionList,
													  leaf_action);
						}

						mergeActionLists = lappend(mergeActionLists,
												   mergeActionList);
					}
					if (parse->commandType == CMD_MERGE)
					{
						Node	   *mergeJoinCondition = parse->mergeJoinCondition;

						if (this_result_rel != top_result_rel)
							mergeJoinCondition =
								adjust_appendrel_attrs_multilevel(root,
																  mergeJoinCondition,
																  this_result_rel,
																  top_result_rel);
						mergeJoinConditions = lappend(mergeJoinConditions,
													  mergeJoinCondition);
					}
				}

				if (resultRelations == NIL)
				{
					/*
					 * 我们把每一个子 rel 都排除掉了，因此使用顶层目标 rel
					 * 的信息生成一个哑的单关系计划（即便它可能不是叶子
					 * 目标）。虽然很明显不会有任何数据被更新或删除，我们仍然
					 * 需要有一个 ModifyTable 节点，以便执行任何语句级触发器。
					 * （如果我们修改 nodeModifyTable.c 以允许零个目标关系，
					 * 这里会更整洁一些，但那大概得不偿失。）
					 */
					resultRelations = list_make1_int(parse->resultRelation);
					if (parse->commandType == CMD_UPDATE)
						updateColnosLists = list_make1(root->update_colnos);
					if (parse->withCheckOptions)
						withCheckOptionLists = list_make1(parse->withCheckOptions);
					if (parse->returningList)
						returningLists = list_make1(parse->returningList);
					if (parse->mergeActionList)
						mergeActionLists = list_make1(parse->mergeActionList);
					if (parse->commandType == CMD_MERGE)
						mergeJoinConditions = list_make1(parse->mergeJoinCondition);
				}
			}
			else
			{
				/* 单关系的 INSERT/UPDATE/DELETE/MERGE。 */
				rootRelation = 0;	/* 没有单独的根 rel */
				resultRelations = list_make1_int(parse->resultRelation);
				if (parse->commandType == CMD_UPDATE)
					updateColnosLists = list_make1(root->update_colnos);
				if (parse->withCheckOptions)
					withCheckOptionLists = list_make1(parse->withCheckOptions);
				if (parse->returningList)
					returningLists = list_make1(parse->returningList);
				if (parse->mergeActionList)
					mergeActionLists = list_make1(parse->mergeActionList);
				if (parse->commandType == CMD_MERGE)
					mergeJoinConditions = list_make1(parse->mergeJoinCondition);
			}

			/*
			 * 如果有 FOR [KEY] UPDATE/SHARE 子句，LockRows 节点将已经处理了
			 * 获取未加锁的被标记行的工作，否则我们需要让 ModifyTable 来做。
			 */
			if (parse->rowMarks)
				rowMarks = NIL;
			else
				rowMarks = root->rowMarks;

			path = (Path *)
				create_modifytable_path(root, final_rel,
										path,
										parse->commandType,
										parse->canSetTag,
										parse->resultRelation,
										rootRelation,
										root->partColsUpdated,
										resultRelations,
										updateColnosLists,
										withCheckOptionLists,
										returningLists,
										rowMarks,
										parse->onConflict,
										mergeActionLists,
										mergeJoinConditions,
										assign_special_exec_param(root));
		}

		/* 然后把它塞进 final_rel */
		add_path(final_rel, path);
	}

	/*
	 * 如果外层查询层级可能能够利用它们，也为 final_rel 生成 partial path。
	 */
	if (final_rel->consider_parallel && root->query_level > 1 &&
		!limit_needed(parse))
	{
		Assert(!parse->rowMarks && parse->commandType == CMD_SELECT);
		foreach(lc, current_rel->partial_pathlist)
		{
			Path	   *partial_path = (Path *) lfirst(lc);

			add_partial_path(final_rel, partial_path);
		}
	}

	extra.limit_needed = limit_needed(parse);
	extra.limit_tuples = limit_tuples;
	extra.count_est = count_est;
	extra.offset_est = offset_est;

	/*
	 * 如果有一个 FDW 负责该查询的所有 baserel，则让它考虑添加 ForeignPath。
	 */
	if (final_rel->fdwroutine &&
		final_rel->fdwroutine->GetForeignUpperPaths)
		final_rel->fdwroutine->GetForeignUpperPaths(root, UPPERREL_FINAL,
													current_rel, final_rel,
													&extra);

	/* 让扩展有可能添加更多的路径 */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_FINAL,
									current_rel, final_rel, &extra);

	/* 注意：目前我们把 set_cheapest() 留给调用者来做 */
}

/*
 * 对 groupingSets 子句及相关数据进行预处理。
 *
 * 我们期望 parse->groupingSets 已经由 expand_grouping_sets() 展开为一个扁平
 * 的 grouping set 列表（即只是由 ressortgroupref 编号组成的整数 List）。本
 * 函数处理前期步骤：把这些 grouping set 组织成 rollup 列表，并准备好稍后将
 * 填入尺寸估算的注解（annotation）。
 */
static grouping_sets_data *
preprocess_grouping_sets(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	List	   *sets;
	int			maxref = 0;
	ListCell   *lc_set;
	grouping_sets_data *gd = palloc0(sizeof(grouping_sets_data));

	/*
	 * 当存在 grouping set 时，我们目前不尝试对 groupClause 做任何优化，因此
	 * 只是把它原样复制到 processed_groupClause 中。
	 */
	root->processed_groupClause = parse->groupClause;

	/* 检测不可哈希和不可排序的分组表达式 */
	gd->any_hashable = false;
	gd->unhashable_refs = NULL;
	gd->unsortable_refs = NULL;
	gd->unsortable_sets = NIL;

	if (parse->groupClause)
	{
		ListCell   *lc;

		foreach(lc, parse->groupClause)
		{
			SortGroupClause *gc = lfirst_node(SortGroupClause, lc);
			Index		ref = gc->tleSortGroupRef;

			if (ref > maxref)
				maxref = ref;

			if (!gc->hashable)
				gd->unhashable_refs = bms_add_member(gd->unhashable_refs, ref);

			if (!OidIsValid(gc->sortop))
				gd->unsortable_refs = bms_add_member(gd->unsortable_refs, ref);
		}
	}

	/* 为重映射分配工作区数组 */
	gd->tleref_to_colnum_map = (int *) palloc((maxref + 1) * sizeof(int));

	/*
	 * 如果我们有任何不可排序的 set，则必须在尝试准备 rollup 之前先把它们
	 * 提取出来。不可排序的 set 不会经过 reorder_grouping_sets，所以我们必须
	 * 在这里应用 GroupingSetData 注解。
	 */
	if (!bms_is_empty(gd->unsortable_refs))
	{
		List	   *sortable_sets = NIL;
		ListCell   *lc;

		foreach(lc, parse->groupingSets)
		{
			List	   *gset = (List *) lfirst(lc);

			if (bms_overlap_list(gd->unsortable_refs, gset))
			{
				GroupingSetData *gs = makeNode(GroupingSetData);

				gs->set = gset;
				gd->unsortable_sets = lappend(gd->unsortable_sets, gs);

				/*
				 * 我们必须在这里强制要求不可排序的 set 是可哈希的；后续
				 * 代码假定了这一点。解析分析只检查每一个单独的列要么可哈希、
				 * 要么可排序。
				 *
				 * 注意，通过此项测试并不能保证我们一定能生成计划；可能还有
				 * 其他障碍。
				 */
				if (bms_overlap_list(gd->unhashable_refs, gset))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("could not implement GROUP BY"),
							 errdetail("Some of the datatypes only support hashing, while others only support sorting.")));
			}
			else
				sortable_sets = lappend(sortable_sets, gset);
		}

		if (sortable_sets)
			sets = extract_rollup_sets(sortable_sets);
		else
			sets = NIL;
	}
	else
		sets = extract_rollup_sets(parse->groupingSets);

	foreach(lc_set, sets)
	{
		List	   *current_sets = (List *) lfirst(lc_set);
		RollupData *rollup = makeNode(RollupData);
		GroupingSetData *gs;

		/*
		 * 将当前的 grouping set 列表重新排序为正确的前缀顺序。如果只需要一次
		 * 聚合遍历（aggregation pass），则尝试让该列表匹配 ORDER BY 子句；
		 * 如果需要多于一次遍历，我们就不做这件事。
		 *
		 * 注意，这会把这些 set 从成员最少者在前重新排序为成员最多者在前，
		 * 并应用 GroupingSetData 注解，尽管其数据要到稍后才会填入。
		 */
		current_sets = reorder_grouping_sets(current_sets,
											 (list_length(sets) == 1
											  ? parse->sortClause
											  : NIL));

		/*
		 * 获取初始的（因而也是最大的）grouping set。
		 */
		gs = linitial_node(GroupingSetData, current_sets);

		/*
		 * 适当地排列 groupClause 的顺序。如果第一个 grouping set 为空，那么
		 * groupClause 也必须为空；否则我们必须强制 groupClause 匹配该
		 * grouping set 的顺序。
		 *
		 * （只有当所有非空的 grouping set 都不可排序时，第一个 grouping set
		 * 才可能为空而 parse->groupClause 却非空。用于哈希 grouping set 的
		 * groupClause 会在稍后构建。）
		 */
		if (gs->set)
			rollup->groupClause = preprocess_groupclause(root, gs->set);
		else
			rollup->groupClause = NIL;

		/*
		 * 它可哈希吗？我们假装空 set 是可哈希的，尽管实际上我们稍后会强制
		 * 它们不被哈希。但如果只有空 set 而没有别的，就不必费心（因为那种
		 * 情况下我们无法哈希任何东西）。
		 */
		if (gs->set &&
			!bms_overlap_list(gd->unhashable_refs, gs->set))
		{
			rollup->hashable = true;
			gd->any_hashable = true;
		}

		/*
		 * 既然我们已经为这个 grouping set 列表确定了 groupClause 的顺序，
		 * 我们需要把 grouping set 中的条目从 sortgroupref 重映射为针对这组
		 * grouping set 的 groupClause 的普通下标（从 0 开始）。不过我们会保留
		 * 原始形式以供稍后使用。
		 */
		rollup->gsets = remap_to_groupclause_idx(rollup->groupClause,
												 current_sets,
												 gd->tleref_to_colnum_map);
		rollup->gsets_data = current_sets;

		gd->rollups = lappend(gd->rollups, rollup);
	}

	if (gd->unsortable_sets)
	{
		/*
		 * 我们尚未为此确定一个 groupclause，但出于估算目的将需要基于下标的
		 * 列表。目前先基于整个原始 groupclause 构造 hash_sets_idx。
		 */
		gd->hash_sets_idx = remap_to_groupclause_idx(parse->groupClause,
													 gd->unsortable_sets,
													 gd->tleref_to_colnum_map);
		gd->any_hashable = true;
	}

	return gd;
}

/*
 * 给定一个 groupclause 和一个 GroupingSetData 列表，返回等价的 set（不带
 * 注解），这些 set 被映射为给定 groupclause 中的下标。
 */
static List *
remap_to_groupclause_idx(List *groupClause,
						 List *gsets,
						 int *tleref_to_colnum_map)
{
	int			ref = 0;
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, groupClause)
	{
		SortGroupClause *gc = lfirst_node(SortGroupClause, lc);

		tleref_to_colnum_map[gc->tleSortGroupRef] = ref++;
	}

	foreach(lc, gsets)
	{
		List	   *set = NIL;
		ListCell   *lc2;
		GroupingSetData *gs = lfirst_node(GroupingSetData, lc);

		foreach(lc2, gs->set)
		{
			set = lappend_int(set, tleref_to_colnum_map[lfirst_int(lc2)]);
		}

		result = lappend(result, set);
	}

	return result;
}


/*
 * preprocess_rowmarks - 如有需要则设置 PlanRowMark
 */
static void
preprocess_rowmarks(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	Bitmapset  *rels;
	List	   *prowmarks;
	ListCell   *l;
	int			i;

	if (parse->rowMarks)
	{
		/*
		 * 如果 FOR [KEY] UPDATE/SHARE 出现在分组内部，就会有麻烦，因为分组
		 * 会使对单个元组 CTID 的引用失效。这在解析时也会检查，但由于规则
		 * 替换、查询上拉（pullup）等原因，那还不够。
		 */
		CheckSelectLocking(parse, linitial_node(RowMarkClause,
												parse->rowMarks)->strength);
	}
	else
	{
		/*
		 * 我们只在 UPDATE、DELETE、MERGE，或 FOR [KEY] UPDATE/SHARE 时才需要
		 * rowmark。
		 */
		if (parse->commandType != CMD_UPDATE &&
			parse->commandType != CMD_DELETE &&
			parse->commandType != CMD_MERGE)
			return;
	}

	/*
	 * 除目标外，我们需要为所有基础关系设置 rowmark。我们构造一个包含所有
	 * base rel 的 bitmapset，然后移除那些我们不需要的、或者已经有
	 * FOR [KEY] UPDATE/SHARE 标记的项。
	 */
	rels = get_relids_in_jointree((Node *) parse->jointree, false, false);
	if (parse->resultRelation)
		rels = bms_del_member(rels, parse->resultRelation);

	/*
	 * 将 RowMarkClause 转换为 PlanRowMark 表示。
	 */
	prowmarks = NIL;
	foreach(l, parse->rowMarks)
	{
		RowMarkClause *rc = lfirst_node(RowMarkClause, l);
		RangeTblEntry *rte = rt_fetch(rc->rti, parse->rtable);
		PlanRowMark *newrc;

		/*
		 * 目前，从语法上不可能把 FOR UPDATE 等应用到 update/delete 的目标
		 * rel 上。如果将来这成为可能，我们应该把目标从 PlanRowMark 列表中
		 * 剔除。
		 */
		Assert(rc->rti != parse->resultRelation);

		/*
		 * 忽略子查询的 RowMarkClause；它们不是真正的表，无法支持真正的加锁。
		 * 被扁平化合并进主查询的子查询应被完全忽略。任何未被扁平化的子查询
		 * 会在下一个循环中获得 ROW_MARK_COPY 项。
		 */
		if (rte->rtekind != RTE_RELATION)
			continue;

		rels = bms_del_member(rels, rc->rti);

		newrc = makeNode(PlanRowMark);
		newrc->rti = newrc->prti = rc->rti;
		newrc->rowmarkId = ++(root->glob->lastRowMarkId);
		newrc->markType = select_rowmark_type(rte, rc->strength);
		newrc->allMarkTypes = (1 << newrc->markType);
		newrc->strength = rc->strength;
		newrc->waitPolicy = rc->waitPolicy;
		newrc->isParent = false;

		prowmarks = lappend(prowmarks, newrc);
	}

	/*
	 * 现在，为任何非目标、未加锁的基础关系添加 rowmark。
	 */
	i = 0;
	foreach(l, parse->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, l);
		PlanRowMark *newrc;

		i++;
		if (!bms_is_member(i, rels))
			continue;

		newrc = makeNode(PlanRowMark);
		newrc->rti = newrc->prti = i;
		newrc->rowmarkId = ++(root->glob->lastRowMarkId);
		newrc->markType = select_rowmark_type(rte, LCS_NONE);
		newrc->allMarkTypes = (1 << newrc->markType);
		newrc->strength = LCS_NONE;
		newrc->waitPolicy = LockWaitBlock;	/* 无所谓 */
		newrc->isParent = false;

		prowmarks = lappend(prowmarks, newrc);
	}

	root->rowMarks = prowmarks;
}

/*
 * 为给定的表选择要使用的 RowMarkType
 */
RowMarkType
select_rowmark_type(RangeTblEntry *rte, LockClauseStrength strength)
{
	if (rte->rtekind != RTE_RELATION)
	{
		/* 如果它根本不是一个表，则使用 ROW_MARK_COPY */
		return ROW_MARK_COPY;
	}
	else if (rte->relkind == RELKIND_FOREIGN_TABLE)
	{
		/* 如果 FDW 愿意，就让它选择 rowmark 类型 */
		FdwRoutine *fdwroutine = GetFdwRoutineByRelId(rte->relid);

		if (fdwroutine->GetForeignRowMarkType != NULL)
			return fdwroutine->GetForeignRowMarkType(rte, strength);
		/* 否则，默认使用 ROW_MARK_COPY */
		return ROW_MARK_COPY;
	}
	else
	{
		/* 常规表，应用适当的锁类型 */
		switch (strength)
		{
			case LCS_NONE:

				/*
				 * 我们不需要元组锁，只需要重新获取该行的能力。
				 */
				return ROW_MARK_REFERENCE;
				break;
			case LCS_FORKEYSHARE:
				return ROW_MARK_KEYSHARE;
				break;
			case LCS_FORSHARE:
				return ROW_MARK_SHARE;
				break;
			case LCS_FORNOKEYUPDATE:
				return ROW_MARK_NOKEYEXCLUSIVE;
				break;
			case LCS_FORUPDATE:
				return ROW_MARK_EXCLUSIVE;
				break;
		}
		elog(ERROR, "unrecognized LockClauseStrength %d", (int) strength);
		return ROW_MARK_EXCLUSIVE;	/* 让编译器闭嘴 */
	}
}

/*
 * preprocess_limit - 对 LIMIT 和/或 OFFSET 子句做预估算
 *
 * 我们尝试估算 LIMIT/OFFSET 子句的值，并把结果通过 *count_est 和
 * *offset_est 返回。如果对应子句不存在，这些变量被设为 0；如果存在但我们
 * 无法估算其值，则被设为 -1。（对 OFFSET 而言 “0” 的约定是可以的，但对
 * LIMIT 稍有些不准确：实际上我们把 LIMIT 0 当作 LIMIT 1 来估算。不过这与
 * 规划器从不估算少于一行的一贯做法是一致的。）这些值将被传递给
 * create_limit_path，如果你修改此代码请参阅它。
 *
 * 返回值是用于规划该查询的、经过适当调整的 tuple_fraction。此调整不可被
 * 覆盖，因为它反映的是 grouping_planner() 必定会采取的计划动作，而非关于
 * 上下文的假设。
 */
static double
preprocess_limit(PlannerInfo *root, double tuple_fraction,
				 int64 *offset_est, int64 *count_est)
{
	Query	   *parse = root->parse;
	Node	   *est;
	double		limit_fraction;

	/* 除非有 LIMIT 或 OFFSET，否则不应调用本函数 */
	Assert(parse->limitCount || parse->limitOffset);

	/*
	 * 尝试获取子句的值。我们使用 estimate_expression_value，主要是因为它有时
	 * 能对 Param 做一些有用的处理。
	 */
	if (parse->limitCount)
	{
		est = estimate_expression_value(root, parse->limitCount);
		if (est && IsA(est, Const))
		{
			if (((Const *) est)->constisnull)
			{
				/* NULL 表示 LIMIT ALL，即无限制 */
				*count_est = 0; /* 视为不存在 */
			}
			else
			{
				*count_est = DatumGetInt64(((Const *) est)->constvalue);
				if (*count_est <= 0)
					*count_est = 1; /* 强制为至少 1 */
			}
		}
		else
			*count_est = -1;	/* 无法估算 */
	}
	else
		*count_est = 0;			/* 不存在 */

	if (parse->limitOffset)
	{
		est = estimate_expression_value(root, parse->limitOffset);
		if (est && IsA(est, Const))
		{
			if (((Const *) est)->constisnull)
			{
				/* 把 NULL 当作无 offset；执行器也会这样做 */
				*offset_est = 0;	/* 视为不存在 */
			}
			else
			{
				*offset_est = DatumGetInt64(((Const *) est)->constvalue);
				if (*offset_est < 0)
					*offset_est = 0;	/* 视为不存在 */
			}
		}
		else
			*offset_est = -1;	/* 无法估算 */
	}
	else
		*offset_est = 0;		/* 不存在 */

	if (*count_est != 0)
	{
		/*
		 * LIMIT 子句限制返回元组的绝对数量。然而，如果它不是常量 LIMIT，
		 * 我们就只能猜测；由于没有更好的办法，假设需要计划结果的 10%。
		 */
		if (*count_est < 0 || *offset_est < 0)
		{
			/* LIMIT 或 OFFSET 是一个表达式……只好放弃精确估算…… */
			limit_fraction = 0.10;
		}
		else
		{
			/* LIMIT（加上 OFFSET，如果有的话）是所需元组的最大数量 */
			limit_fraction = (double) *count_est + (double) *offset_est;
		}

		/*
		 * 如果我们从调用者和 LIMIT 都得到了绝对限制，则使用较小的值；两者
		 * 都是比例（fractional）时同理。如果一个是比例、另一个是绝对值，我们
		 * 无法轻易判断哪个更小，但我们采用一种启发式：通常绝对值会更小。
		 */
		if (tuple_fraction >= 1.0)
		{
			if (limit_fraction >= 1.0)
			{
				/* 两者都是绝对值 */
				tuple_fraction = Min(tuple_fraction, limit_fraction);
			}
			else
			{
				/* 调用者是绝对值，limit 是比例；使用调用者的值 */
			}
		}
		else if (tuple_fraction > 0.0)
		{
			if (limit_fraction >= 1.0)
			{
				/* 调用者是比例，limit 是绝对值；使用 limit */
				tuple_fraction = limit_fraction;
			}
			else
			{
				/* 两者都是比例 */
				tuple_fraction = Min(tuple_fraction, limit_fraction);
			}
		}
		else
		{
			/* 调用者没有提供信息，就直接使用 limit */
			tuple_fraction = limit_fraction;
		}
	}
	else if (*offset_est != 0 && tuple_fraction > 0.0)
	{
		/*
		 * 我们有 OFFSET 但没有 LIMIT。这与 LIMIT 情况的行为完全不同：这里，
		 * 我们需要增大而不是减小调用者的 tuple_fraction，因为 OFFSET 的作用
		 * 是导致获取更多而非更少的元组。不过，这只有在我们得到
		 * tuple_fraction > 0 时才有意义。
		 *
		 * 与上面一样，如果 OFFSET 存在但无法估算，则使用 10%。
		 */
		if (*offset_est < 0)
			limit_fraction = 0.10;
		else
			limit_fraction = (double) *offset_est;

		/*
		 * 如果我们从调用者和 OFFSET 都得到了绝对计数，则把它们相加；两者都是
		 * 比例时同理。如果一个是比例、另一个是绝对值，我们想取较大者，并
		 * 启发式地假设那就是比例的那个。
		 */
		if (tuple_fraction >= 1.0)
		{
			if (limit_fraction >= 1.0)
			{
				/* 两者都是绝对值，所以把它们相加 */
				tuple_fraction += limit_fraction;
			}
			else
			{
				/* 调用者是绝对值，limit 是比例；使用 limit */
				tuple_fraction = limit_fraction;
			}
		}
		else
		{
			if (limit_fraction >= 1.0)
			{
				/* 调用者是比例，limit 是绝对值；使用调用者的值 */
			}
			else
			{
				/* 两者都是比例，所以把它们相加 */
				tuple_fraction += limit_fraction;
				if (tuple_fraction >= 1.0)
					tuple_fraction = 0.0;	/* 假设获取全部 */
			}
		}
	}

	return tuple_fraction;
}

/*
 * limit_needed - 我们真的需要一个 Limit 计划节点吗？
 *
 * 如果我们有常量 0 的 OFFSET 和常量 NULL 的 LIMIT，就可以跳过添加 Limit
 * 节点。这值得检查，因为 "OFFSET 0" 是一种常见的优化栅栏（optimization
 * fence）写法。（因为规划器中的其他地方只是检查 parse->limitOffset 是否非
 * NULL，所以它仍然可以作为优化栅栏——我们只是在抑制不必要的运行时开销。）
 *
 * 这看起来似乎可以合并进 preprocess_limit，但有一个关键区别：这里我们需要
 * OFFSET/LIMIT 是硬常量，而在 preprocess_limit 中考虑估算值就足够了。
 */
bool
limit_needed(Query *parse)
{
	Node	   *node;

	node = parse->limitCount;
	if (node)
	{
		if (IsA(node, Const))
		{
			/* NULL 表示 LIMIT ALL，即无限制 */
			if (!((Const *) node)->constisnull)
				return true;	/* 带常量值的 LIMIT */
		}
		else
			return true;		/* 非常量的 LIMIT */
	}

	node = parse->limitOffset;
	if (node)
	{
		if (IsA(node, Const))
		{
			/* 把 NULL 当作无 offset；执行器也会这样做 */
			if (!((Const *) node)->constisnull)
			{
				int64		offset = DatumGetInt64(((Const *) node)->constvalue);

				if (offset != 0)
					return true;	/* 带非零值的 OFFSET */
			}
		}
		else
			return true;		/* 非常量的 OFFSET */
	}

	return false;				/* 不需要 Limit 计划节点 */
}

/*
 * preprocess_groupclause - 对 GROUP BY 子句做准备工作
 *
 * 这里的思路是调整 GROUP BY 元素的顺序（其本身在语义上无关紧要）以匹配
 * ORDER BY，从而使单次排序操作既能实现 ORDER BY 要求，又能为实现 GROUP BY
 * 的 Unique 步骤做好准备。我们还会考虑 GROUP BY 与 ORDER BY 元素之间的部分
 * 匹配，这可能允许使用增量排序（incremental sort）来实现 ORDER BY。
 *
 * 我们也会考虑 GROUP BY 元素的其他排序方式，它们可能匹配其他可能计划（例如
 * 索引扫描）的排序顺序，从而降低代价。这在生成分组路径（grouping path）
 * 期间实现。详见 get_useful_group_keys_orderings()。
 *
 * Note: we need no comparable processing of the distinctClause because
 * the parser already enforced that that matches ORDER BY.
 *
 * Note: we return a fresh List, but its elements are the same
 * SortGroupClauses appearing in parse->groupClause.  This is important
 * because later processing may modify the processed_groupClause list.
 *
 * For grouping sets, the order of items is instead forced to agree with that
 * of the grouping set (and items not in the grouping set are skipped). The
 * work of sorting the order of grouping set elements to match the ORDER BY if
 * possible is done elsewhere.
 */
static List *
preprocess_groupclause(PlannerInfo *root, List *force)
{
	Query	   *parse = root->parse;
	List	   *new_groupclause = NIL;
	ListCell   *sl;
	ListCell   *gl;

	/* 对于 grouping set，我们需要强制其顺序 */
	if (force)
	{
		foreach(sl, force)
		{
			Index		ref = lfirst_int(sl);
			SortGroupClause *cl = get_sortgroupref_clause(ref, parse->groupClause);

			new_groupclause = lappend(new_groupclause, cl);
		}

		return new_groupclause;
	}

	/* 如果没有 ORDER BY，这里就没有什么有用的事可做 */
	if (parse->sortClause == NIL)
		return list_copy(parse->groupClause);

	/*
	 * 扫描 ORDER BY 子句并构造一个匹配的 GROUP BY 项列表，但只到我们能构成
	 * 匹配前缀（prefix）的程度为止。
	 *
	 * 此代码假定 sortClause 中不包含重复项。
	 */
	foreach(sl, parse->sortClause)
	{
		SortGroupClause *sc = lfirst_node(SortGroupClause, sl);

		foreach(gl, parse->groupClause)
		{
			SortGroupClause *gc = lfirst_node(SortGroupClause, gl);

			if (equal(gc, sc))
			{
				new_groupclause = lappend(new_groupclause, gc);
				break;
			}
		}
		if (gl == NULL)
			break;				/* 没有匹配，因此停止扫描 */
	}


	/* 如果根本没有任何匹配，就没必要重排 GROUP BY */
	if (new_groupclause == NIL)
		return list_copy(parse->groupClause);

	/*
	 * 将任何剩余的 GROUP BY 项添加到新列表中。我们不要求完全匹配，因为即便
	 * 部分匹配也允许使用增量排序来实现 ORDER BY。另外，如果存在任何不可
	 * 排序的 GROUP BY 项就放弃，因为那样反正也没有希望。
	 */
	foreach(gl, parse->groupClause)
	{
		SortGroupClause *gc = lfirst_node(SortGroupClause, gl);

		if (list_member_ptr(new_groupclause, gc))
			continue;			/* 它匹配了某个 ORDER BY 项 */
		if (!OidIsValid(gc->sortop))	/* 放弃，GROUP BY 无法排序 */
			return list_copy(parse->groupClause);
		new_groupclause = lappend(new_groupclause, gc);
	}

	/* 成功——安装重排后的 GROUP BY 列表 */
	Assert(list_length(parse->groupClause) == list_length(new_groupclause));
	return new_groupclause;
}

/*
 * 提取那些各自可以用单次 rollup 型聚合遍历实现的 grouping set 列表。返回一个
 * 由 grouping set 列表组成的列表。
 *
 * 输入必须按最小集在前排序。结果中每个子列表也按最小集在前排序。
 *
 * 我们希望在这里生成绝对最少数量的列表，以避免多余的排序。幸运的是，为此
 * 存在一种算法；将一个偏序集划分为若干链（chain）的最小划分问题（这正是
 * 我们所需要的，把 grouping set 列表视为按集合包含关系排序的偏序集），可以
 * 映射为在二部图上寻找最大基数匹配（maximum cardinality matching）的问题，
 * 后者可在多项式时间内求解，最坏情况不差于 O(n^2.5)，通常好得多。由于我们的
 * N 至多为 4096，我们无需考虑退化到启发式或近似方法。（在我这台一般配置的
 * 机器上，即便关闭优化并开启断言，一个 12 维 cube 的规划时间也不到半秒。）
 */
static List *
extract_rollup_sets(List *groupingSets)
{
	int			num_sets_raw = list_length(groupingSets);
	int			num_empty = 0;
	int			num_sets = 0;	/* 去重后的集合数 */
	int			num_chains = 0;
	List	   *result = NIL;
	List	  **results;
	List	  **orig_sets;
	Bitmapset **set_masks;
	int		   *chains;
	short	  **adjacency;
	short	   *adjacency_buf;
	BipartiteMatchState *state;
	int			i;
	int			j;
	int			j_size;
	ListCell   *lc1 = list_head(groupingSets);
	ListCell   *lc;

	/*
	 * 首先剥离掉空集。算法本身并不要求这么做，但规划器目前需要所有空集都
	 * 在第一个列表中返回，所以我们在这里把它们剥离，之后再加回去。
	 */
	while (lc1 && lfirst(lc1) == NIL)
	{
		++num_empty;
		lc1 = lnext(groupingSets, lc1);
	}

	/* 如果结果是我们拥有的全部都是空集，就现在退出。 */
	if (!lc1)
		return list_make1(groupingSets);

	/*----------
	 * 严格来说我们在这里并不需要移除重复的集合，但如果不移除，它们往往会
	 * 散布在结果各处，这有点令人困惑（而且如果我们哪天决定把它们优化掉，
	 * 还会很烦人）。所以我们在这里移除它们，之后再加回去。
	 *
	 * 对于每个非重复集合，我们填入以下内容：
	 *
	 * orig_sets[i] = 原始集合列表的列表
	 * set_masks[i] = 用于测试包含关系的 bitmapset
	 * adjacency[i] = 邻接下标数组 [n, v1, v2, ... vn]
	 *
	 * chains[i] 将是该集合被分配到的结果分组。
	 *
	 * 我们把所有这些都从 1 而非 0 开始编号，因为在图算法中把 0 留给 NIL
	 * 节点很方便。
	 *----------
	 */
	orig_sets = palloc0((num_sets_raw + 1) * sizeof(List *));
	set_masks = palloc0((num_sets_raw + 1) * sizeof(Bitmapset *));
	adjacency = palloc0((num_sets_raw + 1) * sizeof(short *));
	adjacency_buf = palloc((num_sets_raw + 1) * sizeof(short));

	j_size = 0;
	j = 0;
	i = 1;

	for_each_cell(lc, groupingSets, lc1)
	{
		List	   *candidate = (List *) lfirst(lc);
		Bitmapset  *candidate_set = NULL;
		ListCell   *lc2;
		int			dup_of = 0;

		foreach(lc2, candidate)
		{
			candidate_set = bms_add_member(candidate_set, lfirst_int(lc2));
		}

		/* 只有当我们与之前某个集合长度相同时，才可能是重复 */
		if (j_size == list_length(candidate))
		{
			int			k;

			for (k = j; k < i; ++k)
			{
				if (bms_equal(set_masks[k], candidate_set))
				{
					dup_of = k;
					break;
				}
			}
		}
		else if (j_size < list_length(candidate))
		{
			j_size = list_length(candidate);
			j = i;
		}

		if (dup_of > 0)
		{
			orig_sets[dup_of] = lappend(orig_sets[dup_of], candidate);
			bms_free(candidate_set);
		}
		else
		{
			int			k;
			int			n_adj = 0;

			orig_sets[i] = list_make1(candidate);
			set_masks[i] = candidate_set;

			/* 填充邻接表；无需比较大小相同的集合 */

			for (k = j - 1; k > 0; --k)
			{
				if (bms_is_subset(set_masks[k], candidate_set))
					adjacency_buf[++n_adj] = k;
			}

			if (n_adj > 0)
			{
				adjacency_buf[0] = n_adj;
				adjacency[i] = palloc((n_adj + 1) * sizeof(short));
				memcpy(adjacency[i], adjacency_buf, (n_adj + 1) * sizeof(short));
			}
			else
				adjacency[i] = NULL;

			++i;
		}
	}

	num_sets = i - 1;

	/*
	 * 应用图匹配算法来完成这项工作。
	 */
	state = BipartiteMatch(num_sets, num_sets, adjacency);

	/*
	 * 现在，state->pair* 字段拥有我们把集合分配到链所需的信息。如果
	 * pair_uv[u] = v 或 pair_vu[v] = u，则两个集合 (u,v) 属于同一条链（两者
	 * 都会为真，但我们两个都检查，以便可以一次遍历完成）
	 */
	chains = palloc0((num_sets + 1) * sizeof(int));

	for (i = 1; i <= num_sets; ++i)
	{
		int			u = state->pair_vu[i];
		int			v = state->pair_uv[i];

		if (u > 0 && u < i)
			chains[i] = chains[u];
		else if (v > 0 && v < i)
			chains[i] = chains[v];
		else
			chains[i] = ++num_chains;
	}

	/* 构建结果列表。 */
	results = palloc0((num_chains + 1) * sizeof(List *));

	for (i = 1; i <= num_sets; ++i)
	{
		int			c = chains[i];

		Assert(c > 0);

		results[c] = list_concat(results[c], orig_sets[i]);
	}

	/* 把所有空集重新压回第一个列表。 */
	while (num_empty-- > 0)
		results[1] = lcons(NIL, results[1]);

	/* 生成结果列表 */
	for (i = 1; i <= num_chains; ++i)
		result = lappend(result, results[i]);

	/*
	 * 释放所有东西。
	 *
	 * （对小集合来说这有点过于讲究，但对大集合我们可能占用了相当可观的
	 * 内存。）
	 */
	BipartiteMatchFree(state);
	pfree(results);
	pfree(chains);
	for (i = 1; i <= num_sets; ++i)
		if (adjacency[i])
			pfree(adjacency[i]);
	pfree(adjacency);
	pfree(adjacency_buf);
	pfree(orig_sets);
	for (i = 1; i <= num_sets; ++i)
		bms_free(set_masks[i]);
	pfree(set_masks);

	return result;
}

/*
 * 重排 grouping set 列表中各元素的顺序，使它们具有正确的前缀（prefix）
 * 关系。同时插入 GroupingSetData 注解。
 *
 * 输入必须按最小集在前排序；结果按最大集在前返回。注意结果与输入不共享
 * 任何列表子结构，因此调用者稍后修改它是安全的。
 *
 * 如果传入了 sortclause，我们会尽可能地遵循其列顺序，以最小化我们添加不
 * 必要排序的可能性。（这里我们试图确保 GROUPING SETS ((a,b,c),(c))
 * ORDER BY c,b,a 能在一次遍历中实现。）
 */
static List *
reorder_grouping_sets(List *groupingSets, List *sortclause)
{
	ListCell   *lc;
	List	   *previous = NIL;
	List	   *result = NIL;

	foreach(lc, groupingSets)
	{
		List	   *candidate = (List *) lfirst(lc);
		List	   *new_elems = list_difference_int(candidate, previous);
		GroupingSetData *gs = makeNode(GroupingSetData);

		while (list_length(sortclause) > list_length(previous) &&
			   new_elems != NIL)
		{
			SortGroupClause *sc = list_nth(sortclause, list_length(previous));
			int			ref = sc->tleSortGroupRef;

			if (list_member_int(new_elems, ref))
			{
				previous = lappend_int(previous, ref);
				new_elems = list_delete_int(new_elems, ref);
			}
			else
			{
				/* 与 sortclause 出现分歧；放弃它 */
				sortclause = NIL;
				break;
			}
		}

		previous = list_concat(previous, new_elems);

		gs->set = list_copy(previous);
		result = lcons(gs, result);
	}

	list_free(previous);

	return result;
}

/*
 * has_volatile_pathkey
 *		如果 'keys' 中任何 PathKey 拥有一个包含 volatile 函数的
 *		EquivalenceClass，则返回 true。否则返回 false。
 */
static bool
has_volatile_pathkey(List *keys)
{
	ListCell   *lc;

	foreach(lc, keys)
	{
		PathKey    *pathkey = lfirst_node(PathKey, lc);

		if (pathkey->pk_eclass->ec_has_volatile)
			return true;
	}

	return false;
}

/*
 * adjust_group_pathkeys_for_groupagg
 *		向 root->group_pathkeys 添加 pathkey，以反映有序聚合（ordered
 *		aggregate）的最佳预排序输入集合。
 *
 * 我们把 “最佳” 定义为适合最多聚合函数的 pathkey。我们通过查看第一个
 * ORDER BY / DISTINCT 聚合并取其 pathkey，然后再搜索其他需要相同、或相同
 * pathkey 的更严格变体的聚合，来找到这些 pathkey。接着我们对任何具有不同
 * pathkey 的剩余聚合重复该过程，如果我们找到另一组适合更多聚合的 pathkey，
 * 就改为选择那组 pathkey。
 *
 * 当找到最佳 pathkey 后，我们还会把每个能使用这些 pathkey 的 Aggref 标记为
 * aggpresorted = true。
 *
 * 注意：当某个聚合函数的 ORDER BY / DISTINCT 子句包含任何 volatile 函数时，
 * 我们绝不使用这些 pathkey。我们希望确保使用 volatile 函数的排序在每个
 * Aggref 中独立完成，而不是在查询层级统一进行一次。如果我们允许这么做，那么
 * 当那些 pathkey 被认定为最佳排序 pathkey 时，具有兼容排序顺序的多个 Aggref
 * 就会以相同顺序转换（transition）它们的行。而如果碰巧另一组 Aggref 的
 * pathkey 被认定为更好的排序 pathkey，那么这些使用 volatile 函数的 Aggref
 * 就会被留下来各自单独执行排序。为避免这种可能使 Aggref 结果依赖于查询中
 * 包含哪些其他 Aggref 的不一致行为，我们总是强制使用 volatile 函数的 Aggref
 * 执行它们自己的排序。
 */
static void
adjust_group_pathkeys_for_groupagg(PlannerInfo *root)
{
	List	   *grouppathkeys = root->group_pathkeys;
	List	   *bestpathkeys;
	Bitmapset  *bestaggs;
	Bitmapset  *unprocessed_aggs;
	ListCell   *lc;
	int			i;

	/* Shouldn't be here if there are grouping sets */
	Assert(root->parse->groupingSets == NIL);
	/* Shouldn't be here unless there are some ordered aggregates */
	Assert(root->numOrderedAggs > 0);

	/* Do nothing if disabled */
	if (!enable_presorted_aggregate)
		return;

	/*
	 * 先对所有 AggInfo 做第一遍遍历，收集一个包含下面将要处理的所有 AggInfo
	 * 下标的 Bitmapset。
	 */
	unprocessed_aggs = NULL;
	foreach(lc, root->agginfos)
	{
		AggInfo    *agginfo = lfirst_node(AggInfo, lc);
		Aggref	   *aggref = linitial_node(Aggref, agginfo->aggrefs);

		if (AGGKIND_IS_ORDERED_SET(aggref->aggkind))
			continue;

		/* 除非有 DISTINCT 或 ORDER BY 子句，否则跳过 */
		if (aggref->aggdistinct == NIL && aggref->aggorder == NIL)
			continue;

		/* 如果有 FILTER 子句，则需要额外的安全检查 */
		if (aggref->aggfilter != NULL)
		{
			ListCell   *lc2;
			bool		allow_presort = true;

			/*
			 * 当 Aggref 有 FILTER 子句时，有可能该 filter 会移除那些无法排序
			 * 的行——因为用于排序的表达式在其求值过程中会导致错误。这对预排序
			 * 是个问题，因为预排序发生在 FILTER 之前；而如果不预排序，
			 * Aggregate 节点会在排序*之前*应用 FILTER。因此，为了确保我们
			 * 绝不尝试对任何可能出错的东西排序，这里我们打算跳过任何参数
			 * 表达式在求值时可能导致 ERROR 的 Aggref。Var 和 Const 是可以的。
			 * 可能还有更多应被允许的情况，但需要更多考量。宁可谨慎为上。
			 */
			foreach(lc2, aggref->args)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(lc2);
				Expr	   *expr = tle->expr;

				while (IsA(expr, RelabelType))
					expr = (Expr *) (castNode(RelabelType, expr))->arg;

				/* 常见情况，Var 和 Const 是可以的 */
				if (IsA(expr, Var) || IsA(expr, Const))
					continue;

				/* 不支持。不要为这个 Aggref 尝试预排序 */
				allow_presort = false;
				break;
			}

			/* 跳过不支持的 Aggref */
			if (!allow_presort)
				continue;
		}

		unprocessed_aggs = bms_add_member(unprocessed_aggs,
										  foreach_current_index(lc));
	}

	/*
	 * 现在处理所有 unprocessed_aggs，以便为给定的聚合集合找到最佳的 pathkey
	 * 集合。
	 *
	 * 在这里第一次外层循环时 'bestaggs' 将为空。我们会在第一轮循环中用最开头
	 * 那个 AggInfo 的 pathkey 来填充它，然后从任何拥有更严格的兼容 pathkey
	 * 集合的其他 AggInfo 那里获取更强的 pathkey。外层循环完成一次后，我们把
	 * 所有具有兼容 pathkey 的聚合标记出来，然后从 unprocessed_aggs 中移除它们
	 * 并重复该过程，以尝试找到另一组适合更多聚合的 pathkey。当剩余未处理的
	 * 聚合数量已不足以找到一组适合更多聚合的 pathkey 时，外层循环将停止。
	 */
	bestpathkeys = NIL;
	bestaggs = NULL;
	while (bms_num_members(unprocessed_aggs) > bms_num_members(bestaggs))
	{
		Bitmapset  *aggindexes = NULL;
		List	   *currpathkeys = NIL;

		i = -1;
		while ((i = bms_next_member(unprocessed_aggs, i)) >= 0)
		{
			AggInfo    *agginfo = list_nth_node(AggInfo, root->agginfos, i);
			Aggref	   *aggref = linitial_node(Aggref, agginfo->aggrefs);
			List	   *sortlist;
			List	   *pathkeys;

			if (aggref->aggdistinct != NIL)
				sortlist = aggref->aggdistinct;
			else
				sortlist = aggref->aggorder;

			pathkeys = make_pathkeys_for_sortclauses(root, sortlist,
													 aggref->args);

			/*
			 * 忽略那些在其 ORDER BY 或 DISTINCT 子句中含有 volatile 函数的
			 * Aggref。
			 */
			if (has_volatile_pathkey(pathkeys))
			{
				unprocessed_aggs = bms_del_member(unprocessed_aggs, i);
				continue;
			}

			/*
			 * 当尚未设置时，取第一个未处理聚合的 pathkey。
			 */
			if (currpathkeys == NIL)
			{
				currpathkeys = pathkeys;

				/* 如果 GROUP BY pathkey 存在，则将其包含进来 */
				if (grouppathkeys != NIL)
					currpathkeys = append_pathkeys(list_copy(grouppathkeys),
												   currpathkeys);

				/* 记录我们为这个聚合找到了 pathkey */
				aggindexes = bms_add_member(aggindexes, i);
			}
			else
			{
				/* 现在寻找一组更强的匹配 pathkey */

				/* 如果 GROUP BY pathkey 存在，则将其包含进来 */
				if (grouppathkeys != NIL)
					pathkeys = append_pathkeys(list_copy(grouppathkeys),
											   pathkeys);

				/* 'pathkeys' 是否与 'currpathkeys' 兼容或更好？ */
				switch (compare_pathkeys(currpathkeys, pathkeys))
				{
					case PATHKEYS_BETTER2:
						/* 'pathkeys' 更强，改用它们 */
						currpathkeys = pathkeys;
						/* FALLTHROUGH */

					case PATHKEYS_BETTER1:
						/* 'pathkeys' 不那么严格 */
						/* FALLTHROUGH */

					case PATHKEYS_EQUAL:
						/* 把这个聚合标记为被 'currpathkeys' 覆盖 */
						aggindexes = bms_add_member(aggindexes, i);
						break;

					case PATHKEYS_DIFFERENT:
						break;
				}
			}
		}

		/* 移除我们刚刚处理过的聚合 */
		unprocessed_aggs = bms_del_members(unprocessed_aggs, aggindexes);

		/*
		 * 如果这一轮包含的聚合比之前的最佳结果更多，就把它们作为最佳集合。
		 */
		if (bms_num_members(aggindexes) > bms_num_members(bestaggs))
		{
			bestaggs = aggindexes;
			bestpathkeys = currpathkeys;
		}
	}

	/*
	 * 如果我们找到了任何有序聚合，则更新 root->group_pathkeys 以添加最佳的
	 * 聚合 pathkey 集合。注意 bestpathkeys 已经包含了原始的 GROUP BY
	 * pathkey。
	 */
	if (bestpathkeys != NIL)
		root->group_pathkeys = bestpathkeys;

	/*
	 * 既然我们已经找到了最佳的聚合集合，就可以设置 presorted 标志，以告知
	 * 执行器它不必费心为这些 Aggref 执行排序。我们现在可以这样做，是因为
	 * 由于存在有序聚合，create_grouping_paths 不会把该 GROUP BY 标记为
	 * GROUPING_CAN_USE_HASH，因此不可能出现 Hash Aggregate 计划。
	 */
	i = -1;
	while ((i = bms_next_member(bestaggs, i)) >= 0)
	{
		AggInfo    *agginfo = list_nth_node(AggInfo, root->agginfos, i);

		foreach(lc, agginfo->aggrefs)
		{
			Aggref	   *aggref = lfirst_node(Aggref, lc);

			aggref->aggpresorted = true;
		}
	}
}

/*
 * 在计划生成期间计算 query_pathkeys 和其他 pathkey
 */
static void
standard_qp_callback(PlannerInfo *root, void *extra)
{
	Query	   *parse = root->parse;
	standard_qp_extra *qp_extra = (standard_qp_extra *) extra;
	List	   *tlist = root->processed_tlist;
	List	   *activeWindows = qp_extra->activeWindows;

	/*
	 * 计算表示分组/排序和/或有序聚合要求的 pathkey。
	 */
	if (qp_extra->gset_data)
	{
		/*
		 * 对于 grouping set，直接使用第一个 RollupData 的 groupClause。当存在
		 * grouping set 时，我们不做任何优化分组子句的努力，也无法将聚合排序
		 * 键与分组结合起来。
		 */
		List	   *rollups = qp_extra->gset_data->rollups;
		List	   *groupClause = (rollups ? linitial_node(RollupData, rollups)->groupClause : NIL);

		if (grouping_is_sortable(groupClause))
		{
			bool		sortable;

			/*
			 * groupClause 在逻辑上位于分组步骤之下。因此，如果分组步骤有一个
			 * RTE 条目，我们需要在为这些排序表达式生成 PathKey 之前，先从中
			 * 移除它的 RT 索引。
			 */
			root->group_pathkeys =
				make_pathkeys_for_sortclauses_extended(root,
													   &groupClause,
													   tlist,
													   false,
													   parse->hasGroupRTE,
													   &sortable,
													   false);
			Assert(sortable);
			root->num_groupby_pathkeys = list_length(root->group_pathkeys);
		}
		else
		{
			root->group_pathkeys = NIL;
			root->num_groupby_pathkeys = 0;
		}
	}
	else if (parse->groupClause || root->numOrderedAggs > 0)
	{
		/*
		 * 对于普通的 GROUP BY 列表，我们可以移除任何被 EquivalenceClass
		 * 处理证明是冗余的分组项。例如，在 "WHERE x = y GROUP BY x, y" 中我们
		 * 可以移除 y。这些不是特别常见的情况，但检测它们几乎不需要成本。注意
		 * 我们从 processed_groupClause 中移除冗余项，而不是从原始的
		 * parse->groupClause 中移除。
		 */
		bool		sortable;

		/*
		 * 将分组子句转换为 pathkey。如果 EquivalenceClass 的 ec_sortref 字段
		 * 尚未设置，则将其设置。
		 */
		root->group_pathkeys =
			make_pathkeys_for_sortclauses_extended(root,
												   &root->processed_groupClause,
												   tlist,
												   true,
												   false,
												   &sortable,
												   true);
		if (!sortable)
		{
			/* 无法排序；那么也没必要考虑聚合排序 */
			root->group_pathkeys = NIL;
			root->num_groupby_pathkeys = 0;
		}
		else
		{
			root->num_groupby_pathkeys = list_length(root->group_pathkeys);
			/* 如果我们有有序聚合，考虑追加到 group_pathkeys 上 */
			if (root->numOrderedAggs > 0)
				adjust_group_pathkeys_for_groupagg(root);
		}
	}
	else
	{
		root->group_pathkeys = NIL;
		root->num_groupby_pathkeys = 0;
	}

	/* 在 pathkey 逻辑中我们只考虑第一个（最底部的）窗口 */
	if (activeWindows != NIL)
	{
		WindowClause *wc = linitial_node(WindowClause, activeWindows);

		root->window_pathkeys = make_pathkeys_for_window(root,
														 wc,
														 tlist);
	}
	else
		root->window_pathkeys = NIL;

	/*
	 * 与 GROUP BY 一样，我们可以丢弃任何被 EquivalenceClass 处理证明是冗余的
	 * DISTINCT 项。非冗余列表保存在 root->processed_distinctClause 中，而
	 * 原始的 parse->distinctClause 保持不变。
	 */
	if (parse->distinctClause)
	{
		bool		sortable;

		/* 复制一份，因为 pathkey 处理可能会修改该列表 */
		root->processed_distinctClause = list_copy(parse->distinctClause);
		root->distinct_pathkeys =
			make_pathkeys_for_sortclauses_extended(root,
												   &root->processed_distinctClause,
												   tlist,
												   true,
												   false,
												   &sortable,
												   false);
		if (!sortable)
			root->distinct_pathkeys = NIL;
	}
	else
		root->distinct_pathkeys = NIL;

	root->sort_pathkeys =
		make_pathkeys_for_sortclauses(root,
									  parse->sortClause,
									  tlist);

	/* 设置 setop_pathkeys 可能对 union 规划器有用 */
	if (qp_extra->setop != NULL)
	{
		List	   *groupClauses;
		bool		sortable;

		groupClauses = generate_setop_child_grouplist(qp_extra->setop, tlist);

		root->setop_pathkeys =
			make_pathkeys_for_sortclauses_extended(root,
												   &groupClauses,
												   tlist,
												   false,
												   false,
												   &sortable,
												   false);
		if (!sortable)
			root->setop_pathkeys = NIL;
	}
	else
		root->setop_pathkeys = NIL;

	/*
	 * 判断我们是否希望从 query_planner 得到一个已排序的结果。
	 *
	 * 如果我们有一个可排序的 GROUP BY 子句，那么我们希望得到一个为分组正确
	 * 排序的结果。否则，如果我们有窗口函数要求值，我们尝试为第一个窗口排序。
	 * 否则，如果有一个比 ORDER BY 子句更严格的、可排序的 DISTINCT 子句，我们
	 * 尝试产生对该 DISTINCT 而言排序足够好的输出。否则，如果有 ORDER BY
	 * 子句，我们希望按 ORDER BY 子句排序。否则，如果我们是一个正在为某个能从
	 * 预排序结果获益的集合操作而规划、并且拥有可排序目标列表的子查询，我们
	 * 希望按目标列表排序。
	 *
	 * 注意：如果我们同时有 ORDER BY 和 GROUP BY，并且 ORDER BY 是 GROUP BY
	 * 的超集，那么请求按 ORDER BY 排序会很有诱惑力——但那可能会让我们完全
	 * 无法利用某个可用的排序顺序。这需要更多思考。DISTINCT 与 ORDER BY 之间
	 * 的选择要容易得多，因为我们知道解析器已确保其中一个是另一个的超集。
	 */
	if (root->group_pathkeys)
		root->query_pathkeys = root->group_pathkeys;
	else if (root->window_pathkeys)
		root->query_pathkeys = root->window_pathkeys;
	else if (list_length(root->distinct_pathkeys) >
			 list_length(root->sort_pathkeys))
		root->query_pathkeys = root->distinct_pathkeys;
	else if (root->sort_pathkeys)
		root->query_pathkeys = root->sort_pathkeys;
	else if (root->setop_pathkeys != NIL)
		root->query_pathkeys = root->setop_pathkeys;
	else
		root->query_pathkeys = NIL;
}

/*
 * 估算分组子句产生的分组数量（如果没有分组则为 1）
 *
 * path_rows：扫描/连接步骤的输出行数
 * gd：grouping set 数据，包括 grouping set 列表及其子句
 * target_list：包含分组子句引用的目标列表
 *
 * 如果在执行 grouping set，我们还会用每个 set 和每个单独 rollup 列表的估算
 * 来标注 gsets 数据，以便稍后判断它们的某种组合是否可以改为哈希实现。
 */
static double
get_number_of_groups(PlannerInfo *root,
					 double path_rows,
					 grouping_sets_data *gd,
					 List *target_list)
{
	Query	   *parse = root->parse;
	double		dNumGroups;

	if (parse->groupClause)
	{
		List	   *groupExprs;

		if (parse->groupingSets)
		{
			/* 把每个 grouping set 的估算值累加起来 */
			ListCell   *lc;

			Assert(gd);			/* 让 Coverity 满意 */

			dNumGroups = 0;

			foreach(lc, gd->rollups)
			{
				RollupData *rollup = lfirst_node(RollupData, lc);
				ListCell   *lc2;
				ListCell   *lc3;

				groupExprs = get_sortgrouplist_exprs(rollup->groupClause,
													 target_list);

				rollup->numGroups = 0.0;

				forboth(lc2, rollup->gsets, lc3, rollup->gsets_data)
				{
					List	   *gset = (List *) lfirst(lc2);
					GroupingSetData *gs = lfirst_node(GroupingSetData, lc3);
					double		numGroups = estimate_num_groups(root,
																groupExprs,
																path_rows,
																&gset,
																NULL);

					gs->numGroups = numGroups;
					rollup->numGroups += numGroups;
				}

				dNumGroups += rollup->numGroups;
			}

			if (gd->hash_sets_idx)
			{
				ListCell   *lc2;

				gd->dNumHashGroups = 0;

				groupExprs = get_sortgrouplist_exprs(parse->groupClause,
													 target_list);

				forboth(lc, gd->hash_sets_idx, lc2, gd->unsortable_sets)
				{
					List	   *gset = (List *) lfirst(lc);
					GroupingSetData *gs = lfirst_node(GroupingSetData, lc2);
					double		numGroups = estimate_num_groups(root,
																groupExprs,
																path_rows,
																&gset,
																NULL);

					gs->numGroups = numGroups;
					gd->dNumHashGroups += numGroups;
				}

				dNumGroups += gd->dNumHashGroups;
			}
		}
		else
		{
			/* 普通 GROUP BY —— 基于优化后的 groupClause 进行估算 */
			groupExprs = get_sortgrouplist_exprs(root->processed_groupClause,
												 target_list);

			dNumGroups = estimate_num_groups(root, groupExprs, path_rows,
											 NULL, NULL);
		}
	}
	else if (parse->groupingSets)
	{
		/* 空的 grouping set……每一个产生一行结果 */
		dNumGroups = list_length(parse->groupingSets);
	}
	else if (parse->hasAggs || root->hasHavingQual)
	{
		/* 普通聚合，一行结果 */
		dNumGroups = 1;
	}
	else
	{
		/* 不分组 */
		dNumGroups = 1;
	}

	return dNumGroups;
}

/*
 * create_grouping_paths
 *
 * 构建一个新的 upperrel，其中包含用于分组和/或聚合的 Path。在此过程中，我们
 * 还会为部分分组和/或部分聚合的 Path 构建一个 upperrel。部分分组和/或部分
 * 聚合的路径需要一个 FinalizeAggregate 节点来完成聚合。目前，我们构建的唯一
 * 一种部分分组路径也是 partial path；也就是说，它们需要一个 Gather，然后再
 * 接一个 FinalizeAggregate。
 *
 * input_rel：包含源数据的 Path
 * target：结果 Path 要计算的 pathtarget
 * gd：grouping set 数据，包括 grouping set 列表及其子句
 *
 * 注意：期望 input_rel 中的所有 Path 都返回由 make_group_input_target 计算
 * 出的 target。
 */
static RelOptInfo *
create_grouping_paths(PlannerInfo *root,
					  RelOptInfo *input_rel,
					  PathTarget *target,
					  bool target_parallel_safe,
					  grouping_sets_data *gd)
{
	Query	   *parse = root->parse;
	RelOptInfo *grouped_rel;
	RelOptInfo *partially_grouped_rel;
	AggClauseCosts agg_costs;

	MemSet(&agg_costs, 0, sizeof(AggClauseCosts));
	get_agg_clause_costs(root, AGGSPLIT_SIMPLE, &agg_costs);

	/*
	 * 创建分组关系，用于保存完全聚合的分组和/或聚合路径。
	 */
	grouped_rel = make_grouping_rel(root, input_rel, target,
									target_parallel_safe, parse->havingQual);

	/*
	 * 视情况而定，创建退化分组（degenerate grouping）的路径，或者普通分组的
	 * 路径。
	 */
	if (is_degenerate_grouping(root))
		create_degenerate_grouping_paths(root, input_rel, grouped_rel);
	else
	{
		int			flags = 0;
		GroupPathExtraData extra;

		/*
		 * 判断是否可以执行基于排序的分组实现。（注意，如果
		 * processed_groupClause 为空，grouping_is_sortable() 平凡地为真，并且
		 * 所有 pathkeys_contained_in() 测试也都会成功，因此我们会考虑每一条
		 * 存活下来的输入路径。）
		 *
		 * 如果我们有 grouping set，我们可能能对其中一些而非全部排序；在这种
		 * 情况下，只要我们必须考虑任何有序输入计划，就需要 can_sort 为真。
		 */
		if ((gd && gd->rollups != NIL)
			|| grouping_is_sortable(root->processed_groupClause))
			flags |= GROUPING_CAN_USE_SORT;

		/*
		 * 判断我们是否应当考虑基于哈希的分组实现。
		 *
		 * 哈希聚合仅在我们进行分组时适用。如果我们有 grouping set，某些分组
		 * 可能可哈希而另一些不可；在这种情况下，只要没有全局性的因素阻止我们
		 * 哈希（因此我们应当考虑带哈希的计划），我们就把 can_hash 设为真。
		 *
		 * 执行器不支持带 DISTINCT 或 ORDER BY 聚合的哈希聚合。（这样做将意味着
		 * 把*所有*输入值存储在哈希表中，和/或并行运行大量排序，两者看起来都
		 * 必定得不偿失。）我们同样不支持在哈希聚合中使用有序集聚合
		 * （ordered-set aggregate），不过那种情况也已包含在 numOrderedAggs
		 * 计数中。
		 *
		 * 注意：grouping_is_hashable() 的检查开销比其他门控条件大得多，所以
		 * 我们希望最后再做它。
		 */
		if ((parse->groupClause != NIL &&
			 root->numOrderedAggs == 0 &&
			 (gd ? gd->any_hashable : grouping_is_hashable(root->processed_groupClause))))
			flags |= GROUPING_CAN_USE_HASH;

		/*
		 * 判断是否可能进行部分聚合（partial aggregation）。
		 */
		if (can_partial_agg(root))
			flags |= GROUPING_CAN_PARTIAL_AGG;

		extra.flags = flags;
		extra.target_parallel_safe = target_parallel_safe;
		extra.havingQual = parse->havingQual;
		extra.targetList = parse->targetList;
		extra.partial_costs_set = false;

		/*
		 * 判断分区级聚合（partitionwise aggregation）在理论上是否可能。它可以
		 * 被用户禁用，而且目前我们不尝试支持 grouping set。
		 * create_ordinary_grouping_paths() 会检查额外的条件，例如 input_rel
		 * 是否已分区。
		 */
		if (enable_partitionwise_aggregate && !parse->groupingSets)
			extra.patype = PARTITIONWISE_AGGREGATE_FULL;
		else
			extra.patype = PARTITIONWISE_AGGREGATE_NONE;

		create_ordinary_grouping_paths(root, input_rel, grouped_rel,
									   &agg_costs, gd, &extra,
									   &partially_grouped_rel);
	}

	set_cheapest(grouped_rel);
	return grouped_rel;
}

/*
 * make_grouping_rel
 *
 * 创建一个新的分组 rel 并设置基本属性。
 *
 * input_rel 表示底层的扫描/连接关系。
 * target 是期望从分组关系得到的输出。
 */
static RelOptInfo *
make_grouping_rel(PlannerInfo *root, RelOptInfo *input_rel,
				  PathTarget *target, bool target_parallel_safe,
				  Node *havingQual)
{
	RelOptInfo *grouped_rel;

	if (IS_OTHER_REL(input_rel))
	{
		grouped_rel = fetch_upper_rel(root, UPPERREL_GROUP_AGG,
									  input_rel->relids);
		grouped_rel->reloptkind = RELOPT_OTHER_UPPER_REL;
	}
	else
	{
		/*
		 * 按照惯例，主分组关系的 relids 集合为 NULL。（这可以更改，但可能
		 * 需要在其他地方做相应调整。）
		 */
		grouped_rel = fetch_upper_rel(root, UPPERREL_GROUP_AGG, NULL);
	}

	/* 设置 target。 */
	grouped_rel->reltarget = target;

	/*
	 * 如果输入关系不是并行安全的，那么分组关系也不可能是并行安全的。否则，
	 * 如果目标列表和 HAVING qual 都是并行安全的，它就是并行安全的。
	 */
	if (input_rel->consider_parallel && target_parallel_safe &&
		is_parallel_safe(root, (Node *) havingQual))
		grouped_rel->consider_parallel = true;

	/*
	 * 如果输入 rel 属于单个 FDW，那么分组 rel 也是如此。
	 */
	grouped_rel->serverid = input_rel->serverid;
	grouped_rel->userid = input_rel->userid;
	grouped_rel->useridiscurrent = input_rel->useridiscurrent;
	grouped_rel->fdwroutine = input_rel->fdwroutine;

	return grouped_rel;
}

/*
 * is_degenerate_grouping
 *
 * 退化分组（degenerate grouping）是指查询有 HAVING qual 和/或 grouping set，
 * 但没有聚合、也没有 GROUP BY（这意味着这些 grouping set 全都为空）的情形。
 */
static bool
is_degenerate_grouping(PlannerInfo *root)
{
	Query	   *parse = root->parse;

	return (root->hasHavingQual || parse->groupingSets) &&
		!parse->hasAggs && parse->groupClause == NIL;
}

/*
 * create_degenerate_grouping_paths
 *
 * 当分组是退化的（见 is_degenerate_grouping）时，我们应当为每个 grouping set
 * 发出零行或一行，取决于 HAVING 是否成立。此外，HAVING 或目标列表中都不可能
 * 有任何变量，所以我们实际上根本不需要 FROM 表！我们可以直接丢弃到目前为止
 * 的计划并生成一个 Result 节点。这是一个足够不寻常的边角情况，不值得为了
 * 一开始就避免生成前面的路径而扭曲本模块的结构。
 */
static void
create_degenerate_grouping_paths(PlannerInfo *root, RelOptInfo *input_rel,
								 RelOptInfo *grouped_rel)
{
	Query	   *parse = root->parse;
	int			nrows;
	Path	   *path;

	nrows = list_length(parse->groupingSets);
	if (nrows > 1)
	{
		/*
		 * 似乎不值得编写代码去拼凑一个 generate_series 或一个 values 扫描来
		 * 发出多行。相反，只需制作 N 个克隆并把它们 append 起来。（对于
		 * volatile 的 HAVING 子句，这意味着你可能得到 0 到 N 之间的输出行。
		 * 直觉上我认为这是符合预期的。）
		 */
		List	   *paths = NIL;

		while (--nrows >= 0)
		{
			path = (Path *)
				create_group_result_path(root, grouped_rel,
										 grouped_rel->reltarget,
										 (List *) parse->havingQual);
			paths = lappend(paths, path);
		}
		path = (Path *)
			create_append_path(root,
							   grouped_rel,
							   paths,
							   NIL,
							   NIL,
							   NULL,
							   0,
							   false,
							   -1);
	}
	else
	{
		/* 没有 grouping set，或只有一个，因此只有一行输出 */
		path = (Path *)
			create_group_result_path(root, grouped_rel,
									 grouped_rel->reltarget,
									 (List *) parse->havingQual);
	}

	add_path(grouped_rel, path);
}

/*
 * create_ordinary_grouping_paths
 *
 * 为普通（即非退化）情形创建分组路径。
 *
 * 我们需要在同一个函数中同时考虑排序聚合和哈希聚合，因为否则（1）如果两种
 * 方式都不可行，抛出恰当的错误消息会更困难；并且（2）如果排序不可行，我们
 * 不应让哈希表大小方面的考量劝阻我们使用哈希。
 *
 * *partially_grouped_rel_p 将被设置为本函数创建的部分分组 rel，如果它没有
 * 创建，则设置为 NULL。
 */
static void
create_ordinary_grouping_paths(PlannerInfo *root, RelOptInfo *input_rel,
							   RelOptInfo *grouped_rel,
							   const AggClauseCosts *agg_costs,
							   grouping_sets_data *gd,
							   GroupPathExtraData *extra,
							   RelOptInfo **partially_grouped_rel_p)
{
	Path	   *cheapest_path = input_rel->cheapest_total_path;
	RelOptInfo *partially_grouped_rel = NULL;
	double		dNumGroups;
	PartitionwiseAggregateType patype = PARTITIONWISE_AGGREGATE_NONE;

	/*
	 * 如果这是最顶层的分组关系，或者父关系正在执行某种形式的分区级聚合，
	 * 那么我们在这一层也许也能做。然而，如果输入关系没有分区，分区级聚合
	 * 就不可能。
	 */
	if (extra->patype != PARTITIONWISE_AGGREGATE_NONE &&
		IS_PARTITIONED_REL(input_rel))
	{
		/*
		 * 如果这是最顶层关系，或者父关系正在执行完整的分区级聚合，那么只要
		 * GROUP BY 子句在这一层包含所有分区列、且 GROUP BY 使用的排序规则
		 * 匹配分区的排序规则，我们就可以执行完整的分区级聚合。否则，我们至多
		 * 只能执行部分分区级聚合。但如果部分聚合总体上不被支持，那么我们也
		 * 无法将它用于分区级聚合。
		 *
		 * 检查 parse->groupClause 而非 processed_groupClause，因为即便某些
		 * 分区列被证明是冗余的也没有关系。
		 */
		if (extra->patype == PARTITIONWISE_AGGREGATE_FULL &&
			group_by_has_partkey(input_rel, extra->targetList,
								 root->parse->groupClause))
			patype = PARTITIONWISE_AGGREGATE_FULL;
		else if ((extra->flags & GROUPING_CAN_PARTIAL_AGG) != 0)
			patype = PARTITIONWISE_AGGREGATE_PARTIAL;
		else
			patype = PARTITIONWISE_AGGREGATE_NONE;
	}

	/*
	 * 在为 grouped_rel 生成路径之前，我们首先生成任何可能的部分分组路径；
	 * 这样，后续代码就可以轻松地同时考虑分组的并行方式和非并行方式。
	 */
	if ((extra->flags & GROUPING_CAN_PARTIAL_AGG) != 0)
	{
		bool		force_rel_creation;

		/*
		 * 如果我们在这一层执行分区级聚合，则强制创建一个
		 * partially_grouped_rel，以便我们可以向它添加分区级路径。
		 */
		force_rel_creation = (patype == PARTITIONWISE_AGGREGATE_PARTIAL);

		partially_grouped_rel =
			create_partial_grouping_paths(root,
										  grouped_rel,
										  input_rel,
										  gd,
										  extra,
										  force_rel_creation);
	}

	/* 设置输出参数。 */
	*partially_grouped_rel_p = partially_grouped_rel;

	/* 如果可能，应用分区级聚合技术。 */
	if (patype != PARTITIONWISE_AGGREGATE_NONE)
		create_partitionwise_grouping_paths(root, input_rel, grouped_rel,
											partially_grouped_rel, agg_costs,
											gd, patype, extra);

	/* 如果我们只做部分聚合，则返回。 */
	if (extra->patype == PARTITIONWISE_AGGREGATE_PARTIAL)
	{
		Assert(partially_grouped_rel);

		if (partially_grouped_rel->pathlist)
			set_cheapest(partially_grouped_rel);

		return;
	}

	/* 收集（Gather）任何部分分组的 partial path。 */
	if (partially_grouped_rel && partially_grouped_rel->partial_pathlist)
	{
		gather_grouping_paths(root, partially_grouped_rel);
		set_cheapest(partially_grouped_rel);
	}

	/*
	 * 估算分组数量。
	 */
	dNumGroups = get_number_of_groups(root,
									  cheapest_path->rows,
									  gd,
									  extra->targetList);

	/* 构建最终的分组路径 */
	add_paths_to_grouping_rel(root, input_rel, grouped_rel,
							  partially_grouped_rel, agg_costs, gd,
							  dNumGroups, extra);

	/* 如果我们未能找到任何实现方式，给出一个有帮助的错误 */
	if (grouped_rel->pathlist == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not implement GROUP BY"),
				 errdetail("Some of the datatypes only support hashing, while others only support sorting.")));

	/*
	 * 如果有一个 FDW 负责该查询的所有 baserel，则让它考虑添加 ForeignPath。
	 */
	if (grouped_rel->fdwroutine &&
		grouped_rel->fdwroutine->GetForeignUpperPaths)
		grouped_rel->fdwroutine->GetForeignUpperPaths(root, UPPERREL_GROUP_AGG,
													  input_rel, grouped_rel,
													  extra);

	/* 让扩展有可能添加更多的路径 */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_GROUP_AGG,
									input_rel, grouped_rel,
									extra);
}

/*
 * 对于给定的输入路径，通过哈希与排序的各种组合，考虑在其上执行 grouping set
 * 的各种可能方式。本函数可能被多次调用，因此重要的是它不能改写（scribble）
 * 输入。不返回任何结果，但任何生成的路径都会被添加到 grouped_rel。
 */
static void
consider_groupingsets_paths(PlannerInfo *root,
							RelOptInfo *grouped_rel,
							Path *path,
							bool is_sorted,
							bool can_hash,
							grouping_sets_data *gd,
							const AggClauseCosts *agg_costs,
							double dNumGroups)
{
	Query	   *parse = root->parse;
	Size		hash_mem_limit = get_hash_memory_limit();

	/*
	 * If we're not being offered sorted input, then only consider plans that
	 * can be done entirely by hashing.
	 *
	 * We can hash everything if it looks like it'll fit in hash_mem. But if
	 * the input is actually sorted despite not being advertised as such, we
	 * prefer to make use of that in order to use less memory.
	 *
	 * If none of the grouping sets are sortable, then ignore the hash_mem
	 * limit and generate a path anyway, since otherwise we'll just fail.
	 */
	if (!is_sorted)
	{
		List	   *new_rollups = NIL;
		RollupData *unhashed_rollup = NULL;
		List	   *sets_data;
		List	   *empty_sets_data = NIL;
		List	   *empty_sets = NIL;
		ListCell   *lc;
		ListCell   *l_start = list_head(gd->rollups);
		AggStrategy strat = AGG_HASHED;
		double		hashsize;
		double		exclude_groups = 0.0;

		Assert(can_hash);

		/*
		 * If the input is coincidentally sorted usefully (which can happen
		 * even if is_sorted is false, since that only means that our caller
		 * has set up the sorting for us), then save some hashtable space by
		 * making use of that. But we need to watch out for degenerate cases:
		 *
		 * 1) If there are any empty grouping sets, then group_pathkeys might
		 * be NIL if all non-empty grouping sets are unsortable. In this case,
		 * there will be a rollup containing only empty groups, and the
		 * pathkeys_contained_in test is vacuously true; this is ok.
		 *
		 * XXX: the above relies on the fact that group_pathkeys is generated
		 * from the first rollup. If we add the ability to consider multiple
		 * sort orders for grouping input, this assumption might fail.
		 *
		 * 2) If there are no empty sets and only unsortable sets, then the
		 * rollups list will be empty (and thus l_start == NULL), and
		 * group_pathkeys will be NIL; we must ensure that the vacuously-true
		 * pathkeys_contained_in test doesn't cause us to crash.
		 */
		if (l_start != NULL &&
			pathkeys_contained_in(root->group_pathkeys, path->pathkeys))
		{
			unhashed_rollup = lfirst_node(RollupData, l_start);
			exclude_groups = unhashed_rollup->numGroups;
			l_start = lnext(gd->rollups, l_start);
		}

		hashsize = estimate_hashagg_tablesize(root,
											  path,
											  agg_costs,
											  dNumGroups - exclude_groups);

		/*
		 * gd->rollups is empty if we have only unsortable columns to work
		 * with.  Override hash_mem in that case; otherwise, we'll rely on the
		 * sorted-input case to generate usable mixed paths.
		 */
		if (hashsize > hash_mem_limit && gd->rollups)
			return;				/* nope, won't fit */

		/*
		 * We need to burst the existing rollups list into individual grouping
		 * sets and recompute a groupClause for each set.
		 */
		sets_data = list_copy(gd->unsortable_sets);

		for_each_cell(lc, gd->rollups, l_start)
		{
			RollupData *rollup = lfirst_node(RollupData, lc);

			/*
			 * If we find an unhashable rollup that's not been skipped by the
			 * "actually sorted" check above, we can't cope; we'd need sorted
			 * input (with a different sort order) but we can't get that here.
			 * So bail out; we'll get a valid path from the is_sorted case
			 * instead.
			 *
			 * The mere presence of empty grouping sets doesn't make a rollup
			 * unhashable (see preprocess_grouping_sets), we handle those
			 * specially below.
			 */
			if (!rollup->hashable)
				return;

			sets_data = list_concat(sets_data, rollup->gsets_data);
		}
		foreach(lc, sets_data)
		{
			GroupingSetData *gs = lfirst_node(GroupingSetData, lc);
			List	   *gset = gs->set;
			RollupData *rollup;

			if (gset == NIL)
			{
				/* Empty grouping sets can't be hashed. */
				empty_sets_data = lappend(empty_sets_data, gs);
				empty_sets = lappend(empty_sets, NIL);
			}
			else
			{
				rollup = makeNode(RollupData);

				rollup->groupClause = preprocess_groupclause(root, gset);
				rollup->gsets_data = list_make1(gs);
				rollup->gsets = remap_to_groupclause_idx(rollup->groupClause,
														 rollup->gsets_data,
														 gd->tleref_to_colnum_map);
				rollup->numGroups = gs->numGroups;
				rollup->hashable = true;
				rollup->is_hashed = true;
				new_rollups = lappend(new_rollups, rollup);
			}
		}

		/*
		 * If we didn't find anything nonempty to hash, then bail.  We'll
		 * generate a path from the is_sorted case.
		 */
		if (new_rollups == NIL)
			return;

		/*
		 * If there were empty grouping sets they should have been in the
		 * first rollup.
		 */
		Assert(!unhashed_rollup || !empty_sets);

		if (unhashed_rollup)
		{
			new_rollups = lappend(new_rollups, unhashed_rollup);
			strat = AGG_MIXED;
		}
		else if (empty_sets)
		{
			RollupData *rollup = makeNode(RollupData);

			rollup->groupClause = NIL;
			rollup->gsets_data = empty_sets_data;
			rollup->gsets = empty_sets;
			rollup->numGroups = list_length(empty_sets);
			rollup->hashable = false;
			rollup->is_hashed = false;
			new_rollups = lappend(new_rollups, rollup);
			strat = AGG_MIXED;
		}

		add_path(grouped_rel, (Path *)
				 create_groupingsets_path(root,
										  grouped_rel,
										  path,
										  (List *) parse->havingQual,
										  strat,
										  new_rollups,
										  agg_costs));
		return;
	}

	/*
	 * If we have sorted input but nothing we can do with it, bail.
	 */
	if (gd->rollups == NIL)
		return;

	/*
	 * Given sorted input, we try and make two paths: one sorted and one mixed
	 * sort/hash. (We need to try both because hashagg might be disabled, or
	 * some columns might not be sortable.)
	 *
	 * can_hash is passed in as false if some obstacle elsewhere (such as
	 * ordered aggs) means that we shouldn't consider hashing at all.
	 */
	if (can_hash && gd->any_hashable)
	{
		List	   *rollups = NIL;
		List	   *hash_sets = list_copy(gd->unsortable_sets);
		double		availspace = hash_mem_limit;
		ListCell   *lc;

		/*
		 * Account first for space needed for groups we can't sort at all.
		 */
		availspace -= estimate_hashagg_tablesize(root,
												 path,
												 agg_costs,
												 gd->dNumHashGroups);

		if (availspace > 0 && list_length(gd->rollups) > 1)
		{
			double		scale;
			int			num_rollups = list_length(gd->rollups);
			int			k_capacity;
			int		   *k_weights = palloc(num_rollups * sizeof(int));
			Bitmapset  *hash_items = NULL;
			int			i;

			/*
			 * We treat this as a knapsack problem: the knapsack capacity
			 * represents hash_mem, the item weights are the estimated memory
			 * usage of the hashtables needed to implement a single rollup,
			 * and we really ought to use the cost saving as the item value;
			 * however, currently the costs assigned to sort nodes don't
			 * reflect the comparison costs well, and so we treat all items as
			 * of equal value (each rollup we hash instead saves us one sort).
			 *
			 * To use the discrete knapsack, we need to scale the values to a
			 * reasonably small bounded range.  We choose to allow a 5% error
			 * margin; we have no more than 4096 rollups in the worst possible
			 * case, which with a 5% error margin will require a bit over 42MB
			 * of workspace. (Anyone wanting to plan queries that complex had
			 * better have the memory for it.  In more reasonable cases, with
			 * no more than a couple of dozen rollups, the memory usage will
			 * be negligible.)
			 *
			 * k_capacity is naturally bounded, but we clamp the values for
			 * scale and weight (below) to avoid overflows or underflows (or
			 * uselessly trying to use a scale factor less than 1 byte).
			 */
			scale = Max(availspace / (20.0 * num_rollups), 1.0);
			k_capacity = (int) floor(availspace / scale);

			/*
			 * We leave the first rollup out of consideration since it's the
			 * one that matches the input sort order.  We assign indexes "i"
			 * to only those entries considered for hashing; the second loop,
			 * below, must use the same condition.
			 */
			i = 0;
			for_each_from(lc, gd->rollups, 1)
			{
				RollupData *rollup = lfirst_node(RollupData, lc);

				if (rollup->hashable)
				{
					double		sz = estimate_hashagg_tablesize(root,
																path,
																agg_costs,
																rollup->numGroups);

					/*
					 * If sz is enormous, but hash_mem (and hence scale) is
					 * small, avoid integer overflow here.
					 */
					k_weights[i] = (int) Min(floor(sz / scale),
											 k_capacity + 1.0);
					++i;
				}
			}

			/*
			 * Apply knapsack algorithm; compute the set of items which
			 * maximizes the value stored (in this case the number of sorts
			 * saved) while keeping the total size (approximately) within
			 * capacity.
			 */
			if (i > 0)
				hash_items = DiscreteKnapsack(k_capacity, i, k_weights, NULL);

			if (!bms_is_empty(hash_items))
			{
				rollups = list_make1(linitial(gd->rollups));

				i = 0;
				for_each_from(lc, gd->rollups, 1)
				{
					RollupData *rollup = lfirst_node(RollupData, lc);

					if (rollup->hashable)
					{
						if (bms_is_member(i, hash_items))
							hash_sets = list_concat(hash_sets,
													rollup->gsets_data);
						else
							rollups = lappend(rollups, rollup);
						++i;
					}
					else
						rollups = lappend(rollups, rollup);
				}
			}
		}

		if (!rollups && hash_sets)
			rollups = list_copy(gd->rollups);

		foreach(lc, hash_sets)
		{
			GroupingSetData *gs = lfirst_node(GroupingSetData, lc);
			RollupData *rollup = makeNode(RollupData);

			Assert(gs->set != NIL);

			rollup->groupClause = preprocess_groupclause(root, gs->set);
			rollup->gsets_data = list_make1(gs);
			rollup->gsets = remap_to_groupclause_idx(rollup->groupClause,
													 rollup->gsets_data,
													 gd->tleref_to_colnum_map);
			rollup->numGroups = gs->numGroups;
			rollup->hashable = true;
			rollup->is_hashed = true;
			rollups = lcons(rollup, rollups);
		}

		if (rollups)
		{
			add_path(grouped_rel, (Path *)
					 create_groupingsets_path(root,
											  grouped_rel,
											  path,
											  (List *) parse->havingQual,
											  AGG_MIXED,
											  rollups,
											  agg_costs));
		}
	}

	/*
	 * Now try the simple sorted case.
	 */
	if (!gd->unsortable_sets)
		add_path(grouped_rel, (Path *)
				 create_groupingsets_path(root,
										  grouped_rel,
										  path,
										  (List *) parse->havingQual,
										  AGG_SORTED,
										  gd->rollups,
										  agg_costs));
}

/*
 * create_window_paths
 *
 * Build a new upperrel containing Paths for window-function evaluation.
 *
 * input_rel: contains the source-data Paths
 * input_target: result of make_window_input_target
 * output_target: what the topmost WindowAggPath should return
 * wflists: result of find_window_functions
 * activeWindows: result of select_active_windows
 *
 * Note: all Paths in input_rel are expected to return input_target.
 */
static RelOptInfo *
create_window_paths(PlannerInfo *root,
					RelOptInfo *input_rel,
					PathTarget *input_target,
					PathTarget *output_target,
					bool output_target_parallel_safe,
					WindowFuncLists *wflists,
					List *activeWindows)
{
	RelOptInfo *window_rel;
	ListCell   *lc;

	/* For now, do all work in the (WINDOW, NULL) upperrel */
	window_rel = fetch_upper_rel(root, UPPERREL_WINDOW, NULL);

	/*
	 * If the input relation is not parallel-safe, then the window relation
	 * can't be parallel-safe, either.  Otherwise, we need to examine the
	 * target list and active windows for non-parallel-safe constructs.
	 */
	if (input_rel->consider_parallel && output_target_parallel_safe &&
		is_parallel_safe(root, (Node *) activeWindows))
		window_rel->consider_parallel = true;

	/*
	 * If the input rel belongs to a single FDW, so does the window rel.
	 */
	window_rel->serverid = input_rel->serverid;
	window_rel->userid = input_rel->userid;
	window_rel->useridiscurrent = input_rel->useridiscurrent;
	window_rel->fdwroutine = input_rel->fdwroutine;

	/*
	 * Consider computing window functions starting from the existing
	 * cheapest-total path (which will likely require a sort) as well as any
	 * existing paths that satisfy or partially satisfy root->window_pathkeys.
	 */
	foreach(lc, input_rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);
		int			presorted_keys;

		if (path == input_rel->cheapest_total_path ||
			pathkeys_count_contained_in(root->window_pathkeys, path->pathkeys,
										&presorted_keys) ||
			presorted_keys > 0)
			create_one_window_path(root,
								   window_rel,
								   path,
								   input_target,
								   output_target,
								   wflists,
								   activeWindows);
	}

	/*
	 * If there is an FDW that's responsible for all baserels of the query,
	 * let it consider adding ForeignPaths.
	 */
	if (window_rel->fdwroutine &&
		window_rel->fdwroutine->GetForeignUpperPaths)
		window_rel->fdwroutine->GetForeignUpperPaths(root, UPPERREL_WINDOW,
													 input_rel, window_rel,
													 NULL);

	/* Let extensions possibly add some more paths */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_WINDOW,
									input_rel, window_rel, NULL);

	/* Now choose the best path(s) */
	set_cheapest(window_rel);

	return window_rel;
}

/*
 * Stack window-function implementation steps atop the given Path, and
 * add the result to window_rel.
 *
 * window_rel: upperrel to contain result
 * path: input Path to use (must return input_target)
 * input_target: result of make_window_input_target
 * output_target: what the topmost WindowAggPath should return
 * wflists: result of find_window_functions
 * activeWindows: result of select_active_windows
 */
static void
create_one_window_path(PlannerInfo *root,
					   RelOptInfo *window_rel,
					   Path *path,
					   PathTarget *input_target,
					   PathTarget *output_target,
					   WindowFuncLists *wflists,
					   List *activeWindows)
{
	PathTarget *window_target;
	ListCell   *l;
	List	   *topqual = NIL;

	/*
	 * Since each window clause could require a different sort order, we stack
	 * up a WindowAgg node for each clause, with sort steps between them as
	 * needed.  (We assume that select_active_windows chose a good order for
	 * executing the clauses in.)
	 *
	 * input_target should contain all Vars and Aggs needed for the result.
	 * (In some cases we wouldn't need to propagate all of these all the way
	 * to the top, since they might only be needed as inputs to WindowFuncs.
	 * It's probably not worth trying to optimize that though.)  It must also
	 * contain all window partitioning and sorting expressions, to ensure
	 * they're computed only once at the bottom of the stack (that's critical
	 * for volatile functions).  As we climb up the stack, we'll add outputs
	 * for the WindowFuncs computed at each level.
	 */
	window_target = input_target;

	foreach(l, activeWindows)
	{
		WindowClause *wc = lfirst_node(WindowClause, l);
		List	   *window_pathkeys;
		List	   *runcondition = NIL;
		int			presorted_keys;
		bool		is_sorted;
		bool		topwindow;
		ListCell   *lc2;

		window_pathkeys = make_pathkeys_for_window(root,
												   wc,
												   root->processed_tlist);

		is_sorted = pathkeys_count_contained_in(window_pathkeys,
												path->pathkeys,
												&presorted_keys);

		/* Sort if necessary */
		if (!is_sorted)
		{
			/*
			 * No presorted keys or incremental sort disabled, just perform a
			 * complete sort.
			 */
			if (presorted_keys == 0 || !enable_incremental_sort)
				path = (Path *) create_sort_path(root, window_rel,
												 path,
												 window_pathkeys,
												 -1.0);
			else
			{
				/*
				 * Since we have presorted keys and incremental sort is
				 * enabled, just use incremental sort.
				 */
				path = (Path *) create_incremental_sort_path(root,
															 window_rel,
															 path,
															 window_pathkeys,
															 presorted_keys,
															 -1.0);
			}
		}

		if (lnext(activeWindows, l))
		{
			/*
			 * Add the current WindowFuncs to the output target for this
			 * intermediate WindowAggPath.  We must copy window_target to
			 * avoid changing the previous path's target.
			 *
			 * Note: a WindowFunc adds nothing to the target's eval costs; but
			 * we do need to account for the increase in tlist width.
			 */
			int64		tuple_width = window_target->width;

			window_target = copy_pathtarget(window_target);
			foreach(lc2, wflists->windowFuncs[wc->winref])
			{
				WindowFunc *wfunc = lfirst_node(WindowFunc, lc2);

				add_column_to_pathtarget(window_target, (Expr *) wfunc, 0);
				tuple_width += get_typavgwidth(wfunc->wintype, -1);
			}
			window_target->width = clamp_width_est(tuple_width);
		}
		else
		{
			/* Install the goal target in the topmost WindowAgg */
			window_target = output_target;
		}

		/* mark the final item in the list as the top-level window */
		topwindow = foreach_current_index(l) == list_length(activeWindows) - 1;

		/*
		 * Collect the WindowFuncRunConditions from each WindowFunc and
		 * convert them into OpExprs
		 */
		foreach(lc2, wflists->windowFuncs[wc->winref])
		{
			ListCell   *lc3;
			WindowFunc *wfunc = lfirst_node(WindowFunc, lc2);

			foreach(lc3, wfunc->runCondition)
			{
				WindowFuncRunCondition *wfuncrc =
					lfirst_node(WindowFuncRunCondition, lc3);
				Expr	   *opexpr;
				Expr	   *leftop;
				Expr	   *rightop;

				if (wfuncrc->wfunc_left)
				{
					leftop = (Expr *) copyObject(wfunc);
					rightop = copyObject(wfuncrc->arg);
				}
				else
				{
					leftop = copyObject(wfuncrc->arg);
					rightop = (Expr *) copyObject(wfunc);
				}

				opexpr = make_opclause(wfuncrc->opno,
									   BOOLOID,
									   false,
									   leftop,
									   rightop,
									   InvalidOid,
									   wfuncrc->inputcollid);

				runcondition = lappend(runcondition, opexpr);

				if (!topwindow)
					topqual = lappend(topqual, opexpr);
			}
		}

		path = (Path *)
			create_windowagg_path(root, window_rel, path, window_target,
								  wflists->windowFuncs[wc->winref],
								  runcondition, wc,
								  topwindow ? topqual : NIL, topwindow);
	}

	add_path(window_rel, path);
}

/*
 * create_distinct_paths
 *
 * Build a new upperrel containing Paths for SELECT DISTINCT evaluation.
 *
 * input_rel: contains the source-data Paths
 * target: the pathtarget for the result Paths to compute
 *
 * Note: input paths should already compute the desired pathtarget, since
 * Sort/Unique won't project anything.
 */
static RelOptInfo *
create_distinct_paths(PlannerInfo *root, RelOptInfo *input_rel,
					  PathTarget *target)
{
	RelOptInfo *distinct_rel;

	/* For now, do all work in the (DISTINCT, NULL) upperrel */
	distinct_rel = fetch_upper_rel(root, UPPERREL_DISTINCT, NULL);

	/*
	 * We don't compute anything at this level, so distinct_rel will be
	 * parallel-safe if the input rel is parallel-safe.  In particular, if
	 * there is a DISTINCT ON (...) clause, any path for the input_rel will
	 * output those expressions, and will not be parallel-safe unless those
	 * expressions are parallel-safe.
	 */
	distinct_rel->consider_parallel = input_rel->consider_parallel;

	/*
	 * If the input rel belongs to a single FDW, so does the distinct_rel.
	 */
	distinct_rel->serverid = input_rel->serverid;
	distinct_rel->userid = input_rel->userid;
	distinct_rel->useridiscurrent = input_rel->useridiscurrent;
	distinct_rel->fdwroutine = input_rel->fdwroutine;

	/* build distinct paths based on input_rel's pathlist */
	create_final_distinct_paths(root, input_rel, distinct_rel);

	/* now build distinct paths based on input_rel's partial_pathlist */
	create_partial_distinct_paths(root, input_rel, distinct_rel, target);

	/* Give a helpful error if we failed to create any paths */
	if (distinct_rel->pathlist == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not implement DISTINCT"),
				 errdetail("Some of the datatypes only support hashing, while others only support sorting.")));

	/*
	 * If there is an FDW that's responsible for all baserels of the query,
	 * let it consider adding ForeignPaths.
	 */
	if (distinct_rel->fdwroutine &&
		distinct_rel->fdwroutine->GetForeignUpperPaths)
		distinct_rel->fdwroutine->GetForeignUpperPaths(root,
													   UPPERREL_DISTINCT,
													   input_rel,
													   distinct_rel,
													   NULL);

	/* Let extensions possibly add some more paths */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_DISTINCT, input_rel,
									distinct_rel, NULL);

	/* Now choose the best path(s) */
	set_cheapest(distinct_rel);

	return distinct_rel;
}

/*
 * create_partial_distinct_paths
 *
 * Process 'input_rel' partial paths and add unique/aggregate paths to the
 * UPPERREL_PARTIAL_DISTINCT rel.  For paths created, add Gather/GatherMerge
 * paths on top and add a final unique/aggregate path to remove any duplicate
 * produced from combining rows from parallel workers.
 */
static void
create_partial_distinct_paths(PlannerInfo *root, RelOptInfo *input_rel,
							  RelOptInfo *final_distinct_rel,
							  PathTarget *target)
{
	RelOptInfo *partial_distinct_rel;
	Query	   *parse;
	List	   *distinctExprs;
	double		numDistinctRows;
	Path	   *cheapest_partial_path;
	ListCell   *lc;

	/* nothing to do when there are no partial paths in the input rel */
	if (!input_rel->consider_parallel || input_rel->partial_pathlist == NIL)
		return;

	parse = root->parse;

	/* can't do parallel DISTINCT ON */
	if (parse->hasDistinctOn)
		return;

	partial_distinct_rel = fetch_upper_rel(root, UPPERREL_PARTIAL_DISTINCT,
										   NULL);
	partial_distinct_rel->reltarget = target;
	partial_distinct_rel->consider_parallel = input_rel->consider_parallel;

	/*
	 * If input_rel belongs to a single FDW, so does the partial_distinct_rel.
	 */
	partial_distinct_rel->serverid = input_rel->serverid;
	partial_distinct_rel->userid = input_rel->userid;
	partial_distinct_rel->useridiscurrent = input_rel->useridiscurrent;
	partial_distinct_rel->fdwroutine = input_rel->fdwroutine;

	cheapest_partial_path = linitial(input_rel->partial_pathlist);

	distinctExprs = get_sortgrouplist_exprs(root->processed_distinctClause,
											parse->targetList);

	/* estimate how many distinct rows we'll get from each worker */
	numDistinctRows = estimate_num_groups(root, distinctExprs,
										  cheapest_partial_path->rows,
										  NULL, NULL);

	/*
	 * Try sorting the cheapest path and incrementally sorting any paths with
	 * presorted keys and put a unique paths atop of those.  We'll also
	 * attempt to reorder the required pathkeys to match the input path's
	 * pathkeys as much as possible, in hopes of avoiding a possible need to
	 * re-sort.
	 */
	if (grouping_is_sortable(root->processed_distinctClause))
	{
		foreach(lc, input_rel->partial_pathlist)
		{
			Path	   *input_path = (Path *) lfirst(lc);
			Path	   *sorted_path;
			List	   *useful_pathkeys_list = NIL;

			useful_pathkeys_list =
				get_useful_pathkeys_for_distinct(root,
												 root->distinct_pathkeys,
												 input_path->pathkeys);
			Assert(list_length(useful_pathkeys_list) > 0);

			foreach_node(List, useful_pathkeys, useful_pathkeys_list)
			{
				sorted_path = make_ordered_path(root,
												partial_distinct_rel,
												input_path,
												cheapest_partial_path,
												useful_pathkeys,
												-1.0);

				if (sorted_path == NULL)
					continue;

				/*
				 * An empty distinct_pathkeys means all tuples have the same
				 * value for the DISTINCT clause.  See
				 * create_final_distinct_paths()
				 */
				if (root->distinct_pathkeys == NIL)
				{
					Node	   *limitCount;

					limitCount = (Node *) makeConst(INT8OID, -1, InvalidOid,
													sizeof(int64),
													Int64GetDatum(1), false,
													FLOAT8PASSBYVAL);

					/*
					 * Apply a LimitPath onto the partial path to restrict the
					 * tuples from each worker to 1.
					 * create_final_distinct_paths will need to apply an
					 * additional LimitPath to restrict this to a single row
					 * after the Gather node.  If the query already has a
					 * LIMIT clause, then we could end up with three Limit
					 * nodes in the final plan.  Consolidating the top two of
					 * these could be done, but does not seem worth troubling
					 * over.
					 */
					add_partial_path(partial_distinct_rel, (Path *)
									 create_limit_path(root, partial_distinct_rel,
													   sorted_path,
													   NULL,
													   limitCount,
													   LIMIT_OPTION_COUNT,
													   0, 1));
				}
				else
				{
					add_partial_path(partial_distinct_rel, (Path *)
									 create_upper_unique_path(root, partial_distinct_rel,
															  sorted_path,
															  list_length(root->distinct_pathkeys),
															  numDistinctRows));
				}
			}
		}
	}

	/*
	 * Now try hash aggregate paths, if enabled and hashing is possible. Since
	 * we're not on the hook to ensure we do our best to create at least one
	 * path here, we treat enable_hashagg as a hard off-switch rather than the
	 * slightly softer variant in create_final_distinct_paths.
	 */
	if (enable_hashagg && grouping_is_hashable(root->processed_distinctClause))
	{
		add_partial_path(partial_distinct_rel, (Path *)
						 create_agg_path(root,
										 partial_distinct_rel,
										 cheapest_partial_path,
										 cheapest_partial_path->pathtarget,
										 AGG_HASHED,
										 AGGSPLIT_SIMPLE,
										 root->processed_distinctClause,
										 NIL,
										 NULL,
										 numDistinctRows));
	}

	/*
	 * If there is an FDW that's responsible for all baserels of the query,
	 * let it consider adding ForeignPaths.
	 */
	if (partial_distinct_rel->fdwroutine &&
		partial_distinct_rel->fdwroutine->GetForeignUpperPaths)
		partial_distinct_rel->fdwroutine->GetForeignUpperPaths(root,
															   UPPERREL_PARTIAL_DISTINCT,
															   input_rel,
															   partial_distinct_rel,
															   NULL);

	/* Let extensions possibly add some more partial paths */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_PARTIAL_DISTINCT,
									input_rel, partial_distinct_rel, NULL);

	if (partial_distinct_rel->partial_pathlist != NIL)
	{
		generate_useful_gather_paths(root, partial_distinct_rel, true);
		set_cheapest(partial_distinct_rel);

		/*
		 * Finally, create paths to distinctify the final result.  This step
		 * is needed to remove any duplicates due to combining rows from
		 * parallel workers.
		 */
		create_final_distinct_paths(root, partial_distinct_rel,
									final_distinct_rel);
	}
}

/*
 * create_final_distinct_paths
 *		Create distinct paths in 'distinct_rel' based on 'input_rel' pathlist
 *
 * input_rel: contains the source-data paths
 * distinct_rel: destination relation for storing created paths
 */
static RelOptInfo *
create_final_distinct_paths(PlannerInfo *root, RelOptInfo *input_rel,
							RelOptInfo *distinct_rel)
{
	Query	   *parse = root->parse;
	Path	   *cheapest_input_path = input_rel->cheapest_total_path;
	double		numDistinctRows;
	bool		allow_hash;

	/* Estimate number of distinct rows there will be */
	if (parse->groupClause || parse->groupingSets || parse->hasAggs ||
		root->hasHavingQual)
	{
		/*
		 * If there was grouping or aggregation, use the number of input rows
		 * as the estimated number of DISTINCT rows (ie, assume the input is
		 * already mostly unique).
		 */
		numDistinctRows = cheapest_input_path->rows;
	}
	else
	{
		/*
		 * Otherwise, the UNIQUE filter has effects comparable to GROUP BY.
		 */
		List	   *distinctExprs;

		distinctExprs = get_sortgrouplist_exprs(root->processed_distinctClause,
												parse->targetList);
		numDistinctRows = estimate_num_groups(root, distinctExprs,
											  cheapest_input_path->rows,
											  NULL, NULL);
	}

	/*
	 * Consider sort-based implementations of DISTINCT, if possible.
	 */
	if (grouping_is_sortable(root->processed_distinctClause))
	{
		/*
		 * Firstly, if we have any adequately-presorted paths, just stick a
		 * Unique node on those.  We also, consider doing an explicit sort of
		 * the cheapest input path and Unique'ing that.  If any paths have
		 * presorted keys then we'll create an incremental sort atop of those
		 * before adding a unique node on the top.  We'll also attempt to
		 * reorder the required pathkeys to match the input path's pathkeys as
		 * much as possible, in hopes of avoiding a possible need to re-sort.
		 *
		 * When we have DISTINCT ON, we must sort by the more rigorous of
		 * DISTINCT and ORDER BY, else it won't have the desired behavior.
		 * Also, if we do have to do an explicit sort, we might as well use
		 * the more rigorous ordering to avoid a second sort later.  (Note
		 * that the parser will have ensured that one clause is a prefix of
		 * the other.)
		 */
		List	   *needed_pathkeys;
		ListCell   *lc;
		double		limittuples = root->distinct_pathkeys == NIL ? 1.0 : -1.0;

		if (parse->hasDistinctOn &&
			list_length(root->distinct_pathkeys) <
			list_length(root->sort_pathkeys))
			needed_pathkeys = root->sort_pathkeys;
		else
			needed_pathkeys = root->distinct_pathkeys;

		foreach(lc, input_rel->pathlist)
		{
			Path	   *input_path = (Path *) lfirst(lc);
			Path	   *sorted_path;
			List	   *useful_pathkeys_list = NIL;

			useful_pathkeys_list =
				get_useful_pathkeys_for_distinct(root,
												 needed_pathkeys,
												 input_path->pathkeys);
			Assert(list_length(useful_pathkeys_list) > 0);

			foreach_node(List, useful_pathkeys, useful_pathkeys_list)
			{
				sorted_path = make_ordered_path(root,
												distinct_rel,
												input_path,
												cheapest_input_path,
												useful_pathkeys,
												limittuples);

				if (sorted_path == NULL)
					continue;

				/*
				 * distinct_pathkeys may have become empty if all of the
				 * pathkeys were determined to be redundant.  If all of the
				 * pathkeys are redundant then each DISTINCT target must only
				 * allow a single value, therefore all resulting tuples must
				 * be identical (or at least indistinguishable by an equality
				 * check).  We can uniquify these tuples simply by just taking
				 * the first tuple.  All we do here is add a path to do "LIMIT
				 * 1" atop of 'sorted_path'.  When doing a DISTINCT ON we may
				 * still have a non-NIL sort_pathkeys list, so we must still
				 * only do this with paths which are correctly sorted by
				 * sort_pathkeys.
				 */
				if (root->distinct_pathkeys == NIL)
				{
					Node	   *limitCount;

					limitCount = (Node *) makeConst(INT8OID, -1, InvalidOid,
													sizeof(int64),
													Int64GetDatum(1), false,
													FLOAT8PASSBYVAL);

					/*
					 * If the query already has a LIMIT clause, then we could
					 * end up with a duplicate LimitPath in the final plan.
					 * That does not seem worth troubling over too much.
					 */
					add_path(distinct_rel, (Path *)
							 create_limit_path(root, distinct_rel, sorted_path,
											   NULL, limitCount,
											   LIMIT_OPTION_COUNT, 0, 1));
				}
				else
				{
					add_path(distinct_rel, (Path *)
							 create_upper_unique_path(root, distinct_rel,
													  sorted_path,
													  list_length(root->distinct_pathkeys),
													  numDistinctRows));
				}
			}
		}
	}

	/*
	 * Consider hash-based implementations of DISTINCT, if possible.
	 *
	 * If we were not able to make any other types of path, we *must* hash or
	 * die trying.  If we do have other choices, there are two things that
	 * should prevent selection of hashing: if the query uses DISTINCT ON
	 * (because it won't really have the expected behavior if we hash), or if
	 * enable_hashagg is off.
	 *
	 * Note: grouping_is_hashable() is much more expensive to check than the
	 * other gating conditions, so we want to do it last.
	 */
	if (distinct_rel->pathlist == NIL)
		allow_hash = true;		/* we have no alternatives */
	else if (parse->hasDistinctOn || !enable_hashagg)
		allow_hash = false;		/* policy-based decision not to hash */
	else
		allow_hash = true;		/* default */

	if (allow_hash && grouping_is_hashable(root->processed_distinctClause))
	{
		/* Generate hashed aggregate path --- no sort needed */
		add_path(distinct_rel, (Path *)
				 create_agg_path(root,
								 distinct_rel,
								 cheapest_input_path,
								 cheapest_input_path->pathtarget,
								 AGG_HASHED,
								 AGGSPLIT_SIMPLE,
								 root->processed_distinctClause,
								 NIL,
								 NULL,
								 numDistinctRows));
	}

	return distinct_rel;
}

/*
 * get_useful_pathkeys_for_distinct
 * 	  Get useful orderings of pathkeys for distinctClause by reordering
 * 	  'needed_pathkeys' to match the given 'path_pathkeys' as much as possible.
 *
 * This returns a list of pathkeys that can be useful for DISTINCT or DISTINCT
 * ON clause.  For convenience, it always includes the given 'needed_pathkeys'.
 */
static List *
get_useful_pathkeys_for_distinct(PlannerInfo *root, List *needed_pathkeys,
								 List *path_pathkeys)
{
	List	   *useful_pathkeys_list = NIL;
	List	   *useful_pathkeys = NIL;

	/* always include the given 'needed_pathkeys' */
	useful_pathkeys_list = lappend(useful_pathkeys_list,
								   needed_pathkeys);

	if (!enable_distinct_reordering)
		return useful_pathkeys_list;

	/*
	 * Scan the given 'path_pathkeys' and construct a list of PathKey nodes
	 * that match 'needed_pathkeys', but only up to the longest matching
	 * prefix.
	 *
	 * When we have DISTINCT ON, we must ensure that the resulting pathkey
	 * list matches initial distinctClause pathkeys; otherwise, it won't have
	 * the desired behavior.
	 */
	foreach_node(PathKey, pathkey, path_pathkeys)
	{
		/*
		 * The PathKey nodes are canonical, so they can be checked for
		 * equality by simple pointer comparison.
		 */
		if (!list_member_ptr(needed_pathkeys, pathkey))
			break;
		if (root->parse->hasDistinctOn &&
			!list_member_ptr(root->distinct_pathkeys, pathkey))
			break;

		useful_pathkeys = lappend(useful_pathkeys, pathkey);
	}

	/* If no match at all, no point in reordering needed_pathkeys */
	if (useful_pathkeys == NIL)
		return useful_pathkeys_list;

	/*
	 * If not full match, the resulting pathkey list is not useful without
	 * incremental sort.
	 */
	if (list_length(useful_pathkeys) < list_length(needed_pathkeys) &&
		!enable_incremental_sort)
		return useful_pathkeys_list;

	/* Append the remaining PathKey nodes in needed_pathkeys */
	useful_pathkeys = list_concat_unique_ptr(useful_pathkeys,
											 needed_pathkeys);

	/*
	 * If the resulting pathkey list is the same as the 'needed_pathkeys',
	 * just drop it.
	 */
	if (compare_pathkeys(needed_pathkeys,
						 useful_pathkeys) == PATHKEYS_EQUAL)
		return useful_pathkeys_list;

	useful_pathkeys_list = lappend(useful_pathkeys_list,
								   useful_pathkeys);

	return useful_pathkeys_list;
}

/*
 * create_ordered_paths
 *
 * Build a new upperrel containing Paths for ORDER BY evaluation.
 *
 * All paths in the result must satisfy the ORDER BY ordering.
 * The only new paths we need consider are an explicit full sort
 * and incremental sort on the cheapest-total existing path.
 *
 * input_rel: contains the source-data Paths
 * target: the output tlist the result Paths must emit
 * limit_tuples: estimated bound on the number of output tuples,
 *		or -1 if no LIMIT or couldn't estimate
 *
 * XXX This only looks at sort_pathkeys. I wonder if it needs to look at the
 * other pathkeys (grouping, ...) like generate_useful_gather_paths.
 */
static RelOptInfo *
create_ordered_paths(PlannerInfo *root,
					 RelOptInfo *input_rel,
					 PathTarget *target,
					 bool target_parallel_safe,
					 double limit_tuples)
{
	Path	   *cheapest_input_path = input_rel->cheapest_total_path;
	RelOptInfo *ordered_rel;
	ListCell   *lc;

	/* For now, do all work in the (ORDERED, NULL) upperrel */
	ordered_rel = fetch_upper_rel(root, UPPERREL_ORDERED, NULL);

	/*
	 * If the input relation is not parallel-safe, then the ordered relation
	 * can't be parallel-safe, either.  Otherwise, it's parallel-safe if the
	 * target list is parallel-safe.
	 */
	if (input_rel->consider_parallel && target_parallel_safe)
		ordered_rel->consider_parallel = true;

	/*
	 * If the input rel belongs to a single FDW, so does the ordered_rel.
	 */
	ordered_rel->serverid = input_rel->serverid;
	ordered_rel->userid = input_rel->userid;
	ordered_rel->useridiscurrent = input_rel->useridiscurrent;
	ordered_rel->fdwroutine = input_rel->fdwroutine;

	foreach(lc, input_rel->pathlist)
	{
		Path	   *input_path = (Path *) lfirst(lc);
		Path	   *sorted_path;
		bool		is_sorted;
		int			presorted_keys;

		is_sorted = pathkeys_count_contained_in(root->sort_pathkeys,
												input_path->pathkeys, &presorted_keys);

		if (is_sorted)
			sorted_path = input_path;
		else
		{
			/*
			 * Try at least sorting the cheapest path and also try
			 * incrementally sorting any path which is partially sorted
			 * already (no need to deal with paths which have presorted keys
			 * when incremental sort is disabled unless it's the cheapest
			 * input path).
			 */
			if (input_path != cheapest_input_path &&
				(presorted_keys == 0 || !enable_incremental_sort))
				continue;

			/*
			 * We've no need to consider both a sort and incremental sort.
			 * We'll just do a sort if there are no presorted keys and an
			 * incremental sort when there are presorted keys.
			 */
			if (presorted_keys == 0 || !enable_incremental_sort)
				sorted_path = (Path *) create_sort_path(root,
														ordered_rel,
														input_path,
														root->sort_pathkeys,
														limit_tuples);
			else
				sorted_path = (Path *) create_incremental_sort_path(root,
																	ordered_rel,
																	input_path,
																	root->sort_pathkeys,
																	presorted_keys,
																	limit_tuples);
		}

		/*
		 * If the pathtarget of the result path has different expressions from
		 * the target to be applied, a projection step is needed.
		 */
		if (!equal(sorted_path->pathtarget->exprs, target->exprs))
			sorted_path = apply_projection_to_path(root, ordered_rel,
												   sorted_path, target);

		add_path(ordered_rel, sorted_path);
	}

	/*
	 * generate_gather_paths() will have already generated a simple Gather
	 * path for the best parallel path, if any, and the loop above will have
	 * considered sorting it.  Similarly, generate_gather_paths() will also
	 * have generated order-preserving Gather Merge plans which can be used
	 * without sorting if they happen to match the sort_pathkeys, and the loop
	 * above will have handled those as well.  However, there's one more
	 * possibility: it may make sense to sort the cheapest partial path or
	 * incrementally sort any partial path that is partially sorted according
	 * to the required output order and then use Gather Merge.
	 */
	if (ordered_rel->consider_parallel && root->sort_pathkeys != NIL &&
		input_rel->partial_pathlist != NIL)
	{
		Path	   *cheapest_partial_path;

		cheapest_partial_path = linitial(input_rel->partial_pathlist);

		foreach(lc, input_rel->partial_pathlist)
		{
			Path	   *input_path = (Path *) lfirst(lc);
			Path	   *sorted_path;
			bool		is_sorted;
			int			presorted_keys;
			double		total_groups;

			is_sorted = pathkeys_count_contained_in(root->sort_pathkeys,
													input_path->pathkeys,
													&presorted_keys);

			if (is_sorted)
				continue;

			/*
			 * Try at least sorting the cheapest path and also try
			 * incrementally sorting any path which is partially sorted
			 * already (no need to deal with paths which have presorted keys
			 * when incremental sort is disabled unless it's the cheapest
			 * partial path).
			 */
			if (input_path != cheapest_partial_path &&
				(presorted_keys == 0 || !enable_incremental_sort))
				continue;

			/*
			 * We've no need to consider both a sort and incremental sort.
			 * We'll just do a sort if there are no presorted keys and an
			 * incremental sort when there are presorted keys.
			 */
			if (presorted_keys == 0 || !enable_incremental_sort)
				sorted_path = (Path *) create_sort_path(root,
														ordered_rel,
														input_path,
														root->sort_pathkeys,
														limit_tuples);
			else
				sorted_path = (Path *) create_incremental_sort_path(root,
																	ordered_rel,
																	input_path,
																	root->sort_pathkeys,
																	presorted_keys,
																	limit_tuples);
			total_groups = compute_gather_rows(sorted_path);
			sorted_path = (Path *)
				create_gather_merge_path(root, ordered_rel,
										 sorted_path,
										 sorted_path->pathtarget,
										 root->sort_pathkeys, NULL,
										 &total_groups);

			/*
			 * If the pathtarget of the result path has different expressions
			 * from the target to be applied, a projection step is needed.
			 */
			if (!equal(sorted_path->pathtarget->exprs, target->exprs))
				sorted_path = apply_projection_to_path(root, ordered_rel,
													   sorted_path, target);

			add_path(ordered_rel, sorted_path);
		}
	}

	/*
	 * If there is an FDW that's responsible for all baserels of the query,
	 * let it consider adding ForeignPaths.
	 */
	if (ordered_rel->fdwroutine &&
		ordered_rel->fdwroutine->GetForeignUpperPaths)
		ordered_rel->fdwroutine->GetForeignUpperPaths(root, UPPERREL_ORDERED,
													  input_rel, ordered_rel,
													  NULL);

	/* Let extensions possibly add some more paths */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_ORDERED,
									input_rel, ordered_rel, NULL);

	/*
	 * No need to bother with set_cheapest here; grouping_planner does not
	 * need us to do it.
	 */
	Assert(ordered_rel->pathlist != NIL);

	return ordered_rel;
}


/*
 * make_group_input_target
 *	  Generate appropriate PathTarget for initial input to grouping nodes.
 *
 * If there is grouping or aggregation, the scan/join subplan cannot emit
 * the query's final targetlist; for example, it certainly can't emit any
 * aggregate function calls.  This routine generates the correct target
 * for the scan/join subplan.
 *
 * The query target list passed from the parser already contains entries
 * for all ORDER BY and GROUP BY expressions, but it will not have entries
 * for variables used only in HAVING clauses; so we need to add those
 * variables to the subplan target list.  Also, we flatten all expressions
 * except GROUP BY items into their component variables; other expressions
 * will be computed by the upper plan nodes rather than by the subplan.
 * For example, given a query like
 *		SELECT a+b,SUM(c+d) FROM table GROUP BY a+b;
 * we want to pass this targetlist to the subplan:
 *		a+b,c,d
 * where the a+b target will be used by the Sort/Group steps, and the
 * other targets will be used for computing the final results.
 *
 * 'final_target' is the query's final target list (in PathTarget form)
 *
 * The result is the PathTarget to be computed by the Paths returned from
 * query_planner().
 */
static PathTarget *
make_group_input_target(PlannerInfo *root, PathTarget *final_target)
{
	Query	   *parse = root->parse;
	PathTarget *input_target;
	List	   *non_group_cols;
	List	   *non_group_vars;
	int			i;
	ListCell   *lc;

	/*
	 * We must build a target containing all grouping columns, plus any other
	 * Vars mentioned in the query's targetlist and HAVING qual.
	 */
	input_target = create_empty_pathtarget();
	non_group_cols = NIL;

	i = 0;
	foreach(lc, final_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(final_target, i);

		if (sgref && root->processed_groupClause &&
			get_sortgroupref_clause_noerr(sgref,
										  root->processed_groupClause) != NULL)
		{
			/*
			 * It's a grouping column, so add it to the input target as-is.
			 *
			 * Note that the target is logically below the grouping step.  So
			 * with grouping sets we need to remove the RT index of the
			 * grouping step if there is any from the target expression.
			 */
			if (parse->hasGroupRTE && parse->groupingSets != NIL)
			{
				Assert(root->group_rtindex > 0);
				expr = (Expr *)
					remove_nulling_relids((Node *) expr,
										  bms_make_singleton(root->group_rtindex),
										  NULL);
			}
			add_column_to_pathtarget(input_target, expr, sgref);
		}
		else
		{
			/*
			 * Non-grouping column, so just remember the expression for later
			 * call to pull_var_clause.
			 */
			non_group_cols = lappend(non_group_cols, expr);
		}

		i++;
	}

	/*
	 * If there's a HAVING clause, we'll need the Vars it uses, too.
	 */
	if (parse->havingQual)
		non_group_cols = lappend(non_group_cols, parse->havingQual);

	/*
	 * Pull out all the Vars mentioned in non-group cols (plus HAVING), and
	 * add them to the input target if not already present.  (A Var used
	 * directly as a GROUP BY item will be present already.)  Note this
	 * includes Vars used in resjunk items, so we are covering the needs of
	 * ORDER BY and window specifications.  Vars used within Aggrefs and
	 * WindowFuncs will be pulled out here, too.
	 *
	 * Note that the target is logically below the grouping step.  So with
	 * grouping sets we need to remove the RT index of the grouping step if
	 * there is any from the non-group Vars.
	 */
	non_group_vars = pull_var_clause((Node *) non_group_cols,
									 PVC_RECURSE_AGGREGATES |
									 PVC_RECURSE_WINDOWFUNCS |
									 PVC_INCLUDE_PLACEHOLDERS);
	if (parse->hasGroupRTE && parse->groupingSets != NIL)
	{
		Assert(root->group_rtindex > 0);
		non_group_vars = (List *)
			remove_nulling_relids((Node *) non_group_vars,
								  bms_make_singleton(root->group_rtindex),
								  NULL);
	}
	add_new_columns_to_pathtarget(input_target, non_group_vars);

	/* clean up cruft */
	list_free(non_group_vars);
	list_free(non_group_cols);

	/* XXX this causes some redundant cost calculation ... */
	return set_pathtarget_cost_width(root, input_target);
}

/*
 * make_partial_grouping_target
 *	  Generate appropriate PathTarget for output of partial aggregate
 *	  (or partial grouping, if there are no aggregates) nodes.
 *
 * A partial aggregation node needs to emit all the same aggregates that
 * a regular aggregation node would, plus any aggregates used in HAVING;
 * except that the Aggref nodes should be marked as partial aggregates.
 *
 * In addition, we'd better emit any Vars and PlaceHolderVars that are
 * used outside of Aggrefs in the aggregation tlist and HAVING.  (Presumably,
 * these would be Vars that are grouped by or used in grouping expressions.)
 *
 * grouping_target is the tlist to be emitted by the topmost aggregation step.
 * havingQual represents the HAVING clause.
 */
static PathTarget *
make_partial_grouping_target(PlannerInfo *root,
							 PathTarget *grouping_target,
							 Node *havingQual)
{
	PathTarget *partial_target;
	List	   *non_group_cols;
	List	   *non_group_exprs;
	int			i;
	ListCell   *lc;

	partial_target = create_empty_pathtarget();
	non_group_cols = NIL;

	i = 0;
	foreach(lc, grouping_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(grouping_target, i);

		if (sgref && root->processed_groupClause &&
			get_sortgroupref_clause_noerr(sgref,
										  root->processed_groupClause) != NULL)
		{
			/*
			 * It's a grouping column, so add it to the partial_target as-is.
			 * (This allows the upper agg step to repeat the grouping calcs.)
			 */
			add_column_to_pathtarget(partial_target, expr, sgref);
		}
		else
		{
			/*
			 * Non-grouping column, so just remember the expression for later
			 * call to pull_var_clause.
			 */
			non_group_cols = lappend(non_group_cols, expr);
		}

		i++;
	}

	/*
	 * If there's a HAVING clause, we'll need the Vars/Aggrefs it uses, too.
	 */
	if (havingQual)
		non_group_cols = lappend(non_group_cols, havingQual);

	/*
	 * Pull out all the Vars, PlaceHolderVars, and Aggrefs mentioned in
	 * non-group cols (plus HAVING), and add them to the partial_target if not
	 * already present.  (An expression used directly as a GROUP BY item will
	 * be present already.)  Note this includes Vars used in resjunk items, so
	 * we are covering the needs of ORDER BY and window specifications.
	 */
	non_group_exprs = pull_var_clause((Node *) non_group_cols,
									  PVC_INCLUDE_AGGREGATES |
									  PVC_RECURSE_WINDOWFUNCS |
									  PVC_INCLUDE_PLACEHOLDERS);

	add_new_columns_to_pathtarget(partial_target, non_group_exprs);

	/*
	 * Adjust Aggrefs to put them in partial mode.  At this point all Aggrefs
	 * are at the top level of the target list, so we can just scan the list
	 * rather than recursing through the expression trees.
	 */
	foreach(lc, partial_target->exprs)
	{
		Aggref	   *aggref = (Aggref *) lfirst(lc);

		if (IsA(aggref, Aggref))
		{
			Aggref	   *newaggref;

			/*
			 * We shouldn't need to copy the substructure of the Aggref node,
			 * but flat-copy the node itself to avoid damaging other trees.
			 */
			newaggref = makeNode(Aggref);
			memcpy(newaggref, aggref, sizeof(Aggref));

			/* For now, assume serialization is required */
			mark_partial_aggref(newaggref, AGGSPLIT_INITIAL_SERIAL);

			lfirst(lc) = newaggref;
		}
	}

	/* clean up cruft */
	list_free(non_group_exprs);
	list_free(non_group_cols);

	/* XXX this causes some redundant cost calculation ... */
	return set_pathtarget_cost_width(root, partial_target);
}

/*
 * mark_partial_aggref
 *	  Adjust an Aggref to make it represent a partial-aggregation step.
 *
 * The Aggref node is modified in-place; caller must do any copying required.
 */
void
mark_partial_aggref(Aggref *agg, AggSplit aggsplit)
{
	/* aggtranstype should be computed by this point */
	Assert(OidIsValid(agg->aggtranstype));
	/* ... but aggsplit should still be as the parser left it */
	Assert(agg->aggsplit == AGGSPLIT_SIMPLE);

	/* Mark the Aggref with the intended partial-aggregation mode */
	agg->aggsplit = aggsplit;

	/*
	 * Adjust result type if needed.  Normally, a partial aggregate returns
	 * the aggregate's transition type; but if that's INTERNAL and we're
	 * serializing, it returns BYTEA instead.
	 */
	if (DO_AGGSPLIT_SKIPFINAL(aggsplit))
	{
		if (agg->aggtranstype == INTERNALOID && DO_AGGSPLIT_SERIALIZE(aggsplit))
			agg->aggtype = BYTEAOID;
		else
			agg->aggtype = agg->aggtranstype;
	}
}

/*
 * postprocess_setop_tlist
 *	  Fix up targetlist returned by plan_set_operations().
 *
 * We need to transpose sort key info from the orig_tlist into new_tlist.
 * NOTE: this would not be good enough if we supported resjunk sort keys
 * for results of set operations --- then, we'd need to project a whole
 * new tlist to evaluate the resjunk columns.  For now, just ereport if we
 * find any resjunk columns in orig_tlist.
 */
static List *
postprocess_setop_tlist(List *new_tlist, List *orig_tlist)
{
	ListCell   *l;
	ListCell   *orig_tlist_item = list_head(orig_tlist);

	foreach(l, new_tlist)
	{
		TargetEntry *new_tle = lfirst_node(TargetEntry, l);
		TargetEntry *orig_tle;

		/* ignore resjunk columns in setop result */
		if (new_tle->resjunk)
			continue;

		Assert(orig_tlist_item != NULL);
		orig_tle = lfirst_node(TargetEntry, orig_tlist_item);
		orig_tlist_item = lnext(orig_tlist, orig_tlist_item);
		if (orig_tle->resjunk)	/* should not happen */
			elog(ERROR, "resjunk output columns are not implemented");
		Assert(new_tle->resno == orig_tle->resno);
		new_tle->ressortgroupref = orig_tle->ressortgroupref;
	}
	if (orig_tlist_item != NULL)
		elog(ERROR, "resjunk output columns are not implemented");
	return new_tlist;
}

/*
 * optimize_window_clauses
 *		Call each WindowFunc's prosupport function to see if we're able to
 *		make any adjustments to any of the WindowClause's so that the executor
 *		can execute the window functions in a more optimal way.
 *
 * Currently we only allow adjustments to the WindowClause's frameOptions.  We
 * may allow more things to be done here in the future.
 */
static void
optimize_window_clauses(PlannerInfo *root, WindowFuncLists *wflists)
{
	List	   *windowClause = root->parse->windowClause;
	ListCell   *lc;

	foreach(lc, windowClause)
	{
		WindowClause *wc = lfirst_node(WindowClause, lc);
		ListCell   *lc2;
		int			optimizedFrameOptions = 0;

		Assert(wc->winref <= wflists->maxWinRef);

		/* skip any WindowClauses that have no WindowFuncs */
		if (wflists->windowFuncs[wc->winref] == NIL)
			continue;

		foreach(lc2, wflists->windowFuncs[wc->winref])
		{
			SupportRequestOptimizeWindowClause req;
			SupportRequestOptimizeWindowClause *res;
			WindowFunc *wfunc = lfirst_node(WindowFunc, lc2);
			Oid			prosupport;

			prosupport = get_func_support(wfunc->winfnoid);

			/* Check if there's a support function for 'wfunc' */
			if (!OidIsValid(prosupport))
				break;			/* can't optimize this WindowClause */

			req.type = T_SupportRequestOptimizeWindowClause;
			req.window_clause = wc;
			req.window_func = wfunc;
			req.frameOptions = wc->frameOptions;

			/* call the support function */
			res = (SupportRequestOptimizeWindowClause *)
				DatumGetPointer(OidFunctionCall1(prosupport,
												 PointerGetDatum(&req)));

			/*
			 * Skip to next WindowClause if the support function does not
			 * support this request type.
			 */
			if (res == NULL)
				break;

			/*
			 * Save these frameOptions for the first WindowFunc for this
			 * WindowClause.
			 */
			if (foreach_current_index(lc2) == 0)
				optimizedFrameOptions = res->frameOptions;

			/*
			 * On subsequent WindowFuncs, if the frameOptions are not the same
			 * then we're unable to optimize the frameOptions for this
			 * WindowClause.
			 */
			else if (optimizedFrameOptions != res->frameOptions)
				break;			/* skip to the next WindowClause, if any */
		}

		/* adjust the frameOptions if all WindowFunc's agree that it's ok */
		if (lc2 == NULL && wc->frameOptions != optimizedFrameOptions)
		{
			ListCell   *lc3;

			/* apply the new frame options */
			wc->frameOptions = optimizedFrameOptions;

			/*
			 * We now check to see if changing the frameOptions has caused
			 * this WindowClause to be a duplicate of some other WindowClause.
			 * This can only happen if we have multiple WindowClauses, so
			 * don't bother if there's only 1.
			 */
			if (list_length(windowClause) == 1)
				continue;

			/*
			 * Do the duplicate check and reuse the existing WindowClause if
			 * we find a duplicate.
			 */
			foreach(lc3, windowClause)
			{
				WindowClause *existing_wc = lfirst_node(WindowClause, lc3);

				/* skip over the WindowClause we're currently editing */
				if (existing_wc == wc)
					continue;

				/*
				 * Perform the same duplicate check that is done in
				 * transformWindowFuncCall.
				 */
				if (equal(wc->partitionClause, existing_wc->partitionClause) &&
					equal(wc->orderClause, existing_wc->orderClause) &&
					wc->frameOptions == existing_wc->frameOptions &&
					equal(wc->startOffset, existing_wc->startOffset) &&
					equal(wc->endOffset, existing_wc->endOffset))
				{
					ListCell   *lc4;

					/*
					 * Now move each WindowFunc in 'wc' into 'existing_wc'.
					 * This required adjusting each WindowFunc's winref and
					 * moving the WindowFuncs in 'wc' to the list of
					 * WindowFuncs in 'existing_wc'.
					 */
					foreach(lc4, wflists->windowFuncs[wc->winref])
					{
						WindowFunc *wfunc = lfirst_node(WindowFunc, lc4);

						wfunc->winref = existing_wc->winref;
					}

					/* move list items */
					wflists->windowFuncs[existing_wc->winref] = list_concat(wflists->windowFuncs[existing_wc->winref],
																			wflists->windowFuncs[wc->winref]);
					wflists->windowFuncs[wc->winref] = NIL;

					/*
					 * transformWindowFuncCall() should have made sure there
					 * are no other duplicates, so we needn't bother looking
					 * any further.
					 */
					break;
				}
			}
		}
	}

	/*
	 * XXX remove any duplicate WindowFuncs from each WindowClause.  This has
	 * been done only in the back branches.  Previously, the deduplication was
	 * done in find_window_functions(), but that caused issues with the code
	 * above when moving a WindowFunc to another WindowClause as any duplicate
	 * WindowFuncs won't receive the adjusted winref when merging
	 * WindowClauses.  The deduplication below has been done only so that we
	 * maintain the same cost calculations.  As it turns out, the previous
	 * deduplication code thought it was saving effort during execution by
	 * getting rid of duplicates, but that was not true as the expression
	 * evaluation code will evaluate each WindowFunc mentioned in the
	 * targetlist.
	 */
	foreach(lc, windowClause)
	{
		WindowClause *wc = lfirst_node(WindowClause, lc);
		ListCell   *lc2;
		List	   *list = wflists->windowFuncs[wc->winref];
		List	   *newlist = NIL;

		if (list == NIL)
			continue;

		foreach(lc2, list)
		{
			if (!list_member(newlist, lfirst(lc2)))
				newlist = lappend(newlist, lfirst(lc2));
			else
				wflists->numWindowFuncs--;
		}
		list_free(list);

		wflists->windowFuncs[wc->winref] = newlist;
	}
}

/*
 * select_active_windows
 *		Create a list of the "active" window clauses (ie, those referenced
 *		by non-deleted WindowFuncs) in the order they are to be executed.
 */
static List *
select_active_windows(PlannerInfo *root, WindowFuncLists *wflists)
{
	List	   *windowClause = root->parse->windowClause;
	List	   *result = NIL;
	ListCell   *lc;
	int			nActive = 0;
	WindowClauseSortData *actives = palloc(sizeof(WindowClauseSortData)
										   * list_length(windowClause));

	/* First, construct an array of the active windows */
	foreach(lc, windowClause)
	{
		WindowClause *wc = lfirst_node(WindowClause, lc);

		/* It's only active if wflists shows some related WindowFuncs */
		Assert(wc->winref <= wflists->maxWinRef);
		if (wflists->windowFuncs[wc->winref] == NIL)
			continue;

		actives[nActive].wc = wc;	/* original clause */

		/*
		 * For sorting, we want the list of partition keys followed by the
		 * list of sort keys. But pathkeys construction will remove duplicates
		 * between the two, so we can as well (even though we can't detect all
		 * of the duplicates, since some may come from ECs - that might mean
		 * we miss optimization chances here). We must, however, ensure that
		 * the order of entries is preserved with respect to the ones we do
		 * keep.
		 *
		 * partitionClause and orderClause had their own duplicates removed in
		 * parse analysis, so we're only concerned here with removing
		 * orderClause entries that also appear in partitionClause.
		 */
		actives[nActive].uniqueOrder =
			list_concat_unique(list_copy(wc->partitionClause),
							   wc->orderClause);
		nActive++;
	}

	/*
	 * Sort active windows by their partitioning/ordering clauses, ignoring
	 * any framing clauses, so that the windows that need the same sorting are
	 * adjacent in the list. When we come to generate paths, this will avoid
	 * inserting additional Sort nodes.
	 *
	 * This is how we implement a specific requirement from the SQL standard,
	 * which says that when two or more windows are order-equivalent (i.e.
	 * have matching partition and order clauses, even if their names or
	 * framing clauses differ), then all peer rows must be presented in the
	 * same order in all of them. If we allowed multiple sort nodes for such
	 * cases, we'd risk having the peer rows end up in different orders in
	 * equivalent windows due to sort instability. (See General Rule 4 of
	 * <window clause> in SQL2008 - SQL2016.)
	 *
	 * Additionally, if the entire list of clauses of one window is a prefix
	 * of another, put first the window with stronger sorting requirements.
	 * This way we will first sort for stronger window, and won't have to sort
	 * again for the weaker one.
	 */
	qsort(actives, nActive, sizeof(WindowClauseSortData), common_prefix_cmp);

	/* build ordered list of the original WindowClause nodes */
	for (int i = 0; i < nActive; i++)
		result = lappend(result, actives[i].wc);

	pfree(actives);

	return result;
}

/*
 * name_active_windows
 *	  Ensure all active windows have unique names.
 *
 * The parser will have checked that user-assigned window names are unique
 * within the Query.  Here we assign made-up names to any unnamed
 * WindowClauses for the benefit of EXPLAIN.  (We don't want to do this
 * at parse time, because it'd mess up decompilation of views.)
 *
 * activeWindows: result of select_active_windows
 */
static void
name_active_windows(List *activeWindows)
{
	int			next_n = 1;
	char		newname[16];
	ListCell   *lc;

	foreach(lc, activeWindows)
	{
		WindowClause *wc = lfirst_node(WindowClause, lc);

		/* Nothing to do if it has a name already. */
		if (wc->name)
			continue;

		/* Select a name not currently present in the list. */
		for (;;)
		{
			ListCell   *lc2;

			snprintf(newname, sizeof(newname), "w%d", next_n++);
			foreach(lc2, activeWindows)
			{
				WindowClause *wc2 = lfirst_node(WindowClause, lc2);

				if (wc2->name && strcmp(wc2->name, newname) == 0)
					break;		/* matched */
			}
			if (lc2 == NULL)
				break;			/* reached the end with no match */
		}
		wc->name = pstrdup(newname);
	}
}

/*
 * common_prefix_cmp
 *	  QSort comparison function for WindowClauseSortData
 *
 * Sort the windows by the required sorting clauses. First, compare the sort
 * clauses themselves. Second, if one window's clauses are a prefix of another
 * one's clauses, put the window with more sort clauses first.
 *
 * We purposefully sort by the highest tleSortGroupRef first.  Since
 * tleSortGroupRefs are assigned for the query's DISTINCT and ORDER BY first
 * and because here we sort the lowest tleSortGroupRefs last, if a
 * WindowClause is sharing a tleSortGroupRef with the query's DISTINCT or
 * ORDER BY clause, this makes it more likely that the final WindowAgg will
 * provide presorted input for the query's DISTINCT or ORDER BY clause, thus
 * reducing the total number of sorts required for the query.
 */
static int
common_prefix_cmp(const void *a, const void *b)
{
	const WindowClauseSortData *wcsa = a;
	const WindowClauseSortData *wcsb = b;
	ListCell   *item_a;
	ListCell   *item_b;

	forboth(item_a, wcsa->uniqueOrder, item_b, wcsb->uniqueOrder)
	{
		SortGroupClause *sca = lfirst_node(SortGroupClause, item_a);
		SortGroupClause *scb = lfirst_node(SortGroupClause, item_b);

		if (sca->tleSortGroupRef > scb->tleSortGroupRef)
			return -1;
		else if (sca->tleSortGroupRef < scb->tleSortGroupRef)
			return 1;
		else if (sca->sortop > scb->sortop)
			return -1;
		else if (sca->sortop < scb->sortop)
			return 1;
		else if (sca->nulls_first && !scb->nulls_first)
			return -1;
		else if (!sca->nulls_first && scb->nulls_first)
			return 1;
		/* no need to compare eqop, since it is fully determined by sortop */
	}

	if (list_length(wcsa->uniqueOrder) > list_length(wcsb->uniqueOrder))
		return -1;
	else if (list_length(wcsa->uniqueOrder) < list_length(wcsb->uniqueOrder))
		return 1;

	return 0;
}

/*
 * make_window_input_target
 *	  Generate appropriate PathTarget for initial input to WindowAgg nodes.
 *
 * When the query has window functions, this function computes the desired
 * target to be computed by the node just below the first WindowAgg.
 * This tlist must contain all values needed to evaluate the window functions,
 * compute the final target list, and perform any required final sort step.
 * If multiple WindowAggs are needed, each intermediate one adds its window
 * function results onto this base tlist; only the topmost WindowAgg computes
 * the actual desired target list.
 *
 * This function is much like make_group_input_target, though not quite enough
 * like it to share code.  As in that function, we flatten most expressions
 * into their component variables.  But we do not want to flatten window
 * PARTITION BY/ORDER BY clauses, since that might result in multiple
 * evaluations of them, which would be bad (possibly even resulting in
 * inconsistent answers, if they contain volatile functions).
 * Also, we must not flatten GROUP BY clauses that were left unflattened by
 * make_group_input_target, because we may no longer have access to the
 * individual Vars in them.
 *
 * Another key difference from make_group_input_target is that we don't
 * flatten Aggref expressions, since those are to be computed below the
 * window functions and just referenced like Vars above that.
 *
 * 'final_target' is the query's final target list (in PathTarget form)
 * 'activeWindows' is the list of active windows previously identified by
 *			select_active_windows.
 *
 * The result is the PathTarget to be computed by the plan node immediately
 * below the first WindowAgg node.
 */
static PathTarget *
make_window_input_target(PlannerInfo *root,
						 PathTarget *final_target,
						 List *activeWindows)
{
	PathTarget *input_target;
	Bitmapset  *sgrefs;
	List	   *flattenable_cols;
	List	   *flattenable_vars;
	int			i;
	ListCell   *lc;

	Assert(root->parse->hasWindowFuncs);

	/*
	 * Collect the sortgroupref numbers of window PARTITION/ORDER BY clauses
	 * into a bitmapset for convenient reference below.
	 */
	sgrefs = NULL;
	foreach(lc, activeWindows)
	{
		WindowClause *wc = lfirst_node(WindowClause, lc);
		ListCell   *lc2;

		foreach(lc2, wc->partitionClause)
		{
			SortGroupClause *sortcl = lfirst_node(SortGroupClause, lc2);

			sgrefs = bms_add_member(sgrefs, sortcl->tleSortGroupRef);
		}
		foreach(lc2, wc->orderClause)
		{
			SortGroupClause *sortcl = lfirst_node(SortGroupClause, lc2);

			sgrefs = bms_add_member(sgrefs, sortcl->tleSortGroupRef);
		}
	}

	/* Add in sortgroupref numbers of GROUP BY clauses, too */
	foreach(lc, root->processed_groupClause)
	{
		SortGroupClause *grpcl = lfirst_node(SortGroupClause, lc);

		sgrefs = bms_add_member(sgrefs, grpcl->tleSortGroupRef);
	}

	/*
	 * Construct a target containing all the non-flattenable targetlist items,
	 * and save aside the others for a moment.
	 */
	input_target = create_empty_pathtarget();
	flattenable_cols = NIL;

	i = 0;
	foreach(lc, final_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(final_target, i);

		/*
		 * Don't want to deconstruct window clauses or GROUP BY items.  (Note
		 * that such items can't contain window functions, so it's okay to
		 * compute them below the WindowAgg nodes.)
		 */
		if (sgref != 0 && bms_is_member(sgref, sgrefs))
		{
			/*
			 * Don't want to deconstruct this value, so add it to the input
			 * target as-is.
			 */
			add_column_to_pathtarget(input_target, expr, sgref);
		}
		else
		{
			/*
			 * Column is to be flattened, so just remember the expression for
			 * later call to pull_var_clause.
			 */
			flattenable_cols = lappend(flattenable_cols, expr);
		}

		i++;
	}

	/*
	 * Pull out all the Vars and Aggrefs mentioned in flattenable columns, and
	 * add them to the input target if not already present.  (Some might be
	 * there already because they're used directly as window/group clauses.)
	 *
	 * Note: it's essential to use PVC_INCLUDE_AGGREGATES here, so that any
	 * Aggrefs are placed in the Agg node's tlist and not left to be computed
	 * at higher levels.  On the other hand, we should recurse into
	 * WindowFuncs to make sure their input expressions are available.
	 */
	flattenable_vars = pull_var_clause((Node *) flattenable_cols,
									   PVC_INCLUDE_AGGREGATES |
									   PVC_RECURSE_WINDOWFUNCS |
									   PVC_INCLUDE_PLACEHOLDERS);
	add_new_columns_to_pathtarget(input_target, flattenable_vars);

	/* clean up cruft */
	list_free(flattenable_vars);
	list_free(flattenable_cols);

	/* XXX this causes some redundant cost calculation ... */
	return set_pathtarget_cost_width(root, input_target);
}

/*
 * make_pathkeys_for_window
 *		Create a pathkeys list describing the required input ordering
 *		for the given WindowClause.
 *
 * Modifies wc's partitionClause to remove any clauses which are deemed
 * redundant by the pathkey logic.
 *
 * The required ordering is first the PARTITION keys, then the ORDER keys.
 * In the future we might try to implement windowing using hashing, in which
 * case the ordering could be relaxed, but for now we always sort.
 */
static List *
make_pathkeys_for_window(PlannerInfo *root, WindowClause *wc,
						 List *tlist)
{
	List	   *window_pathkeys = NIL;

	/* Throw error if can't sort */
	if (!grouping_is_sortable(wc->partitionClause))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not implement window PARTITION BY"),
				 errdetail("Window partitioning columns must be of sortable datatypes.")));
	if (!grouping_is_sortable(wc->orderClause))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not implement window ORDER BY"),
				 errdetail("Window ordering columns must be of sortable datatypes.")));

	/*
	 * First fetch the pathkeys for the PARTITION BY clause.  We can safely
	 * remove any clauses from the wc->partitionClause for redundant pathkeys.
	 */
	if (wc->partitionClause != NIL)
	{
		bool		sortable;

		window_pathkeys = make_pathkeys_for_sortclauses_extended(root,
																 &wc->partitionClause,
																 tlist,
																 true,
																 false,
																 &sortable,
																 false);

		Assert(sortable);
	}

	/*
	 * In principle, we could also consider removing redundant ORDER BY items
	 * too as doing so does not alter the result of peer row checks done by
	 * the executor.  However, we must *not* remove the ordering column for
	 * RANGE OFFSET cases, as the executor needs that for in_range tests even
	 * if it's known to be equal to some partitioning column.
	 */
	if (wc->orderClause != NIL)
	{
		List	   *orderby_pathkeys;

		orderby_pathkeys = make_pathkeys_for_sortclauses(root,
														 wc->orderClause,
														 tlist);

		/* Okay, make the combined pathkeys */
		if (window_pathkeys != NIL)
			window_pathkeys = append_pathkeys(window_pathkeys, orderby_pathkeys);
		else
			window_pathkeys = orderby_pathkeys;
	}

	return window_pathkeys;
}

/*
 * make_sort_input_target
 *	  Generate appropriate PathTarget for initial input to Sort step.
 *
 * If the query has ORDER BY, this function chooses the target to be computed
 * by the node just below the Sort (and DISTINCT, if any, since Unique can't
 * project) steps.  This might or might not be identical to the query's final
 * output target.
 *
 * The main argument for keeping the sort-input tlist the same as the final
 * is that we avoid a separate projection node (which will be needed if
 * they're different, because Sort can't project).  However, there are also
 * advantages to postponing tlist evaluation till after the Sort: it ensures
 * a consistent order of evaluation for any volatile functions in the tlist,
 * and if there's also a LIMIT, we can stop the query without ever computing
 * tlist functions for later rows, which is beneficial for both volatile and
 * expensive functions.
 *
 * Our current policy is to postpone volatile expressions till after the sort
 * unconditionally (assuming that that's possible, ie they are in plain tlist
 * columns and not ORDER BY/GROUP BY/DISTINCT columns).  We also prefer to
 * postpone set-returning expressions, because running them beforehand would
 * bloat the sort dataset, and because it might cause unexpected output order
 * if the sort isn't stable.  However there's a constraint on that: all SRFs
 * in the tlist should be evaluated at the same plan step, so that they can
 * run in sync in nodeProjectSet.  So if any SRFs are in sort columns, we
 * mustn't postpone any SRFs.  (Note that in principle that policy should
 * probably get applied to the group/window input targetlists too, but we
 * have not done that historically.)  Lastly, expensive expressions are
 * postponed if there is a LIMIT, or if root->tuple_fraction shows that
 * partial evaluation of the query is possible (if neither is true, we expect
 * to have to evaluate the expressions for every row anyway), or if there are
 * any volatile or set-returning expressions (since once we've put in a
 * projection at all, it won't cost any more to postpone more stuff).
 *
 * Another issue that could potentially be considered here is that
 * evaluating tlist expressions could result in data that's either wider
 * or narrower than the input Vars, thus changing the volume of data that
 * has to go through the Sort.  However, we usually have only a very bad
 * idea of the output width of any expression more complex than a Var,
 * so for now it seems too risky to try to optimize on that basis.
 *
 * Note that if we do produce a modified sort-input target, and then the
 * query ends up not using an explicit Sort, no particular harm is done:
 * we'll initially use the modified target for the preceding path nodes,
 * but then change them to the final target with apply_projection_to_path.
 * Moreover, in such a case the guarantees about evaluation order of
 * volatile functions still hold, since the rows are sorted already.
 *
 * This function has some things in common with make_group_input_target and
 * make_window_input_target, though the detailed rules for what to do are
 * different.  We never flatten/postpone any grouping or ordering columns;
 * those are needed before the sort.  If we do flatten a particular
 * expression, we leave Aggref and WindowFunc nodes alone, since those were
 * computed earlier.
 *
 * 'final_target' is the query's final target list (in PathTarget form)
 * 'have_postponed_srfs' is an output argument, see below
 *
 * The result is the PathTarget to be computed by the plan node immediately
 * below the Sort step (and the Distinct step, if any).  This will be
 * exactly final_target if we decide a projection step wouldn't be helpful.
 *
 * In addition, *have_postponed_srfs is set to true if we choose to postpone
 * any set-returning functions to after the Sort.
 */
static PathTarget *
make_sort_input_target(PlannerInfo *root,
					   PathTarget *final_target,
					   bool *have_postponed_srfs)
{
	Query	   *parse = root->parse;
	PathTarget *input_target;
	int			ncols;
	bool	   *col_is_srf;
	bool	   *postpone_col;
	bool		have_srf;
	bool		have_volatile;
	bool		have_expensive;
	bool		have_srf_sortcols;
	bool		postpone_srfs;
	List	   *postponable_cols;
	List	   *postponable_vars;
	int			i;
	ListCell   *lc;

	/* Shouldn't get here unless query has ORDER BY */
	Assert(parse->sortClause);

	*have_postponed_srfs = false;	/* default result */

	/* Inspect tlist and collect per-column information */
	ncols = list_length(final_target->exprs);
	col_is_srf = (bool *) palloc0(ncols * sizeof(bool));
	postpone_col = (bool *) palloc0(ncols * sizeof(bool));
	have_srf = have_volatile = have_expensive = have_srf_sortcols = false;

	i = 0;
	foreach(lc, final_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);

		/*
		 * If the column has a sortgroupref, assume it has to be evaluated
		 * before sorting.  Generally such columns would be ORDER BY, GROUP
		 * BY, etc targets.  One exception is columns that were removed from
		 * GROUP BY by remove_useless_groupby_columns() ... but those would
		 * only be Vars anyway.  There don't seem to be any cases where it
		 * would be worth the trouble to double-check.
		 */
		if (get_pathtarget_sortgroupref(final_target, i) == 0)
		{
			/*
			 * Check for SRF or volatile functions.  Check the SRF case first
			 * because we must know whether we have any postponed SRFs.
			 */
			if (parse->hasTargetSRFs &&
				expression_returns_set((Node *) expr))
			{
				/* We'll decide below whether these are postponable */
				col_is_srf[i] = true;
				have_srf = true;
			}
			else if (contain_volatile_functions((Node *) expr))
			{
				/* Unconditionally postpone */
				postpone_col[i] = true;
				have_volatile = true;
			}
			else
			{
				/*
				 * Else check the cost.  XXX it's annoying to have to do this
				 * when set_pathtarget_cost_width() just did it.  Refactor to
				 * allow sharing the work?
				 */
				QualCost	cost;

				cost_qual_eval_node(&cost, (Node *) expr, root);

				/*
				 * We arbitrarily define "expensive" as "more than 10X
				 * cpu_operator_cost".  Note this will take in any PL function
				 * with default cost.
				 */
				if (cost.per_tuple > 10 * cpu_operator_cost)
				{
					postpone_col[i] = true;
					have_expensive = true;
				}
			}
		}
		else
		{
			/* For sortgroupref cols, just check if any contain SRFs */
			if (!have_srf_sortcols &&
				parse->hasTargetSRFs &&
				expression_returns_set((Node *) expr))
				have_srf_sortcols = true;
		}

		i++;
	}

	/*
	 * We can postpone SRFs if we have some but none are in sortgroupref cols.
	 */
	postpone_srfs = (have_srf && !have_srf_sortcols);

	/*
	 * If we don't need a post-sort projection, just return final_target.
	 */
	if (!(postpone_srfs || have_volatile ||
		  (have_expensive &&
		   (parse->limitCount || root->tuple_fraction > 0))))
		return final_target;

	/*
	 * Report whether the post-sort projection will contain set-returning
	 * functions.  This is important because it affects whether the Sort can
	 * rely on the query's LIMIT (if any) to bound the number of rows it needs
	 * to return.
	 */
	*have_postponed_srfs = postpone_srfs;

	/*
	 * Construct the sort-input target, taking all non-postponable columns and
	 * then adding Vars, PlaceHolderVars, Aggrefs, and WindowFuncs found in
	 * the postponable ones.
	 */
	input_target = create_empty_pathtarget();
	postponable_cols = NIL;

	i = 0;
	foreach(lc, final_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);

		if (postpone_col[i] || (postpone_srfs && col_is_srf[i]))
			postponable_cols = lappend(postponable_cols, expr);
		else
			add_column_to_pathtarget(input_target, expr,
									 get_pathtarget_sortgroupref(final_target, i));

		i++;
	}

	/*
	 * Pull out all the Vars, Aggrefs, and WindowFuncs mentioned in
	 * postponable columns, and add them to the sort-input target if not
	 * already present.  (Some might be there already.)  We mustn't
	 * deconstruct Aggrefs or WindowFuncs here, since the projection node
	 * would be unable to recompute them.
	 */
	postponable_vars = pull_var_clause((Node *) postponable_cols,
									   PVC_INCLUDE_AGGREGATES |
									   PVC_INCLUDE_WINDOWFUNCS |
									   PVC_INCLUDE_PLACEHOLDERS);
	add_new_columns_to_pathtarget(input_target, postponable_vars);

	/* clean up cruft */
	list_free(postponable_vars);
	list_free(postponable_cols);

	/* XXX this represents even more redundant cost calculation ... */
	return set_pathtarget_cost_width(root, input_target);
}

/*
 * get_cheapest_fractional_path
 *	  Find the cheapest path for retrieving a specified fraction of all
 *	  the tuples expected to be returned by the given relation.
 *
 * Do not consider parameterized paths.  If the caller needs a path for upper
 * rel, it can't have parameterized paths.  If the caller needs an append
 * subpath, it could become limited by the treatment of similar
 * parameterization of all the subpaths.
 *
 * We interpret tuple_fraction the same way as grouping_planner.
 *
 * We assume set_cheapest() has been run on the given rel.
 */
Path *
get_cheapest_fractional_path(RelOptInfo *rel, double tuple_fraction)
{
	Path	   *best_path = rel->cheapest_total_path;
	ListCell   *l;

	/* If all tuples will be retrieved, just return the cheapest-total path */
	if (tuple_fraction <= 0.0)
		return best_path;

	/* Convert absolute # of tuples to a fraction; no need to clamp to 0..1 */
	if (tuple_fraction >= 1.0 && best_path->rows > 0)
		tuple_fraction /= best_path->rows;

	foreach(l, rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(l);

		if (path->param_info)
			continue;

		if (path == rel->cheapest_total_path ||
			compare_fractional_path_costs(best_path, path, tuple_fraction) <= 0)
			continue;

		best_path = path;
	}

	return best_path;
}

/*
 * adjust_paths_for_srfs
 *		Fix up the Paths of the given upperrel to handle tSRFs properly.
 *
 * The executor can only handle set-returning functions that appear at the
 * top level of the targetlist of a ProjectSet plan node.  If we have any SRFs
 * that are not at top level, we need to split up the evaluation into multiple
 * plan levels in which each level satisfies this constraint.  This function
 * modifies each Path of an upperrel that (might) compute any SRFs in its
 * output tlist to insert appropriate projection steps.
 *
 * The given targets and targets_contain_srfs lists are from
 * split_pathtarget_at_srfs().  We assume the existing Paths emit the first
 * target in targets.
 */
static void
adjust_paths_for_srfs(PlannerInfo *root, RelOptInfo *rel,
					  List *targets, List *targets_contain_srfs)
{
	ListCell   *lc;

	Assert(list_length(targets) == list_length(targets_contain_srfs));
	Assert(!linitial_int(targets_contain_srfs));

	/* If no SRFs appear at this plan level, nothing to do */
	if (list_length(targets) == 1)
		return;

	/*
	 * Stack SRF-evaluation nodes atop each path for the rel.
	 *
	 * In principle we should re-run set_cheapest() here to identify the
	 * cheapest path, but it seems unlikely that adding the same tlist eval
	 * costs to all the paths would change that, so we don't bother. Instead,
	 * just assume that the cheapest-startup and cheapest-total paths remain
	 * so.  (There should be no parameterized paths anymore, so we needn't
	 * worry about updating cheapest_parameterized_paths.)
	 */
	foreach(lc, rel->pathlist)
	{
		Path	   *subpath = (Path *) lfirst(lc);
		Path	   *newpath = subpath;
		ListCell   *lc1,
				   *lc2;

		Assert(subpath->param_info == NULL);
		forboth(lc1, targets, lc2, targets_contain_srfs)
		{
			PathTarget *thistarget = lfirst_node(PathTarget, lc1);
			bool		contains_srfs = (bool) lfirst_int(lc2);

			/* If this level doesn't contain SRFs, do regular projection */
			if (contains_srfs)
				newpath = (Path *) create_set_projection_path(root,
															  rel,
															  newpath,
															  thistarget);
			else
				newpath = (Path *) apply_projection_to_path(root,
															rel,
															newpath,
															thistarget);
		}
		lfirst(lc) = newpath;
		if (subpath == rel->cheapest_startup_path)
			rel->cheapest_startup_path = newpath;
		if (subpath == rel->cheapest_total_path)
			rel->cheapest_total_path = newpath;
	}

	/* Likewise for partial paths, if any */
	foreach(lc, rel->partial_pathlist)
	{
		Path	   *subpath = (Path *) lfirst(lc);
		Path	   *newpath = subpath;
		ListCell   *lc1,
				   *lc2;

		Assert(subpath->param_info == NULL);
		forboth(lc1, targets, lc2, targets_contain_srfs)
		{
			PathTarget *thistarget = lfirst_node(PathTarget, lc1);
			bool		contains_srfs = (bool) lfirst_int(lc2);

			/* If this level doesn't contain SRFs, do regular projection */
			if (contains_srfs)
				newpath = (Path *) create_set_projection_path(root,
															  rel,
															  newpath,
															  thistarget);
			else
			{
				/* avoid apply_projection_to_path, in case of multiple refs */
				newpath = (Path *) create_projection_path(root,
														  rel,
														  newpath,
														  thistarget);
			}
		}
		lfirst(lc) = newpath;
	}
}

/*
 * expression_planner
 *		Perform planner's transformations on a standalone expression.
 *
 * Various utility commands need to evaluate expressions that are not part
 * of a plannable query.  They can do so using the executor's regular
 * expression-execution machinery, but first the expression has to be fed
 * through here to transform it from parser output to something executable.
 *
 * Currently, we disallow sublinks in standalone expressions, so there's no
 * real "planning" involved here.  (That might not always be true though.)
 * What we must do is run eval_const_expressions to ensure that any function
 * calls are converted to positional notation and function default arguments
 * get inserted.  The fact that constant subexpressions get simplified is a
 * side-effect that is useful when the expression will get evaluated more than
 * once.  Also, we must fix operator function IDs.
 *
 * This does not return any information about dependencies of the expression.
 * Hence callers should use the results only for the duration of the current
 * query.  Callers that would like to cache the results for longer should use
 * expression_planner_with_deps, probably via the plancache.
 *
 * Note: this must not make any damaging changes to the passed-in expression
 * tree.  (It would actually be okay to apply fix_opfuncids to it, but since
 * we first do an expression_tree_mutator-based walk, what is returned will
 * be a new node tree.)  The result is constructed in the current memory
 * context; beware that this can leak a lot of additional stuff there, too.
 */
Expr *
expression_planner(Expr *expr)
{
	Node	   *result;

	/*
	 * Convert named-argument function calls, insert default arguments and
	 * simplify constant subexprs
	 */
	result = eval_const_expressions(NULL, (Node *) expr);

	/* Fill in opfuncid values if missing */
	fix_opfuncids(result);

	return (Expr *) result;
}

/*
 * expression_planner_with_deps
 *		Perform planner's transformations on a standalone expression,
 *		returning expression dependency information along with the result.
 *
 * This is identical to expression_planner() except that it also returns
 * information about possible dependencies of the expression, ie identities of
 * objects whose definitions affect the result.  As in a PlannedStmt, these
 * are expressed as a list of relation Oids and a list of PlanInvalItems.
 */
Expr *
expression_planner_with_deps(Expr *expr,
							 List **relationOids,
							 List **invalItems)
{
	Node	   *result;
	PlannerGlobal glob;
	PlannerInfo root;

	/* Make up dummy planner state so we can use setrefs machinery */
	MemSet(&glob, 0, sizeof(glob));
	glob.type = T_PlannerGlobal;
	glob.relationOids = NIL;
	glob.invalItems = NIL;

	MemSet(&root, 0, sizeof(root));
	root.type = T_PlannerInfo;
	root.glob = &glob;

	/*
	 * Convert named-argument function calls, insert default arguments and
	 * simplify constant subexprs.  Collect identities of inlined functions
	 * and elided domains, too.
	 */
	result = eval_const_expressions(&root, (Node *) expr);

	/* Fill in opfuncid values if missing */
	fix_opfuncids(result);

	/*
	 * Now walk the finished expression to find anything else we ought to
	 * record as an expression dependency.
	 */
	(void) extract_query_dependencies_walker(result, &root);

	*relationOids = glob.relationOids;
	*invalItems = glob.invalItems;

	return (Expr *) result;
}


/*
 * plan_cluster_use_sort
 *		Use the planner to decide how CLUSTER should implement sorting
 *
 * tableOid is the OID of a table to be clustered on its index indexOid
 * (which is already known to be a btree index).  Decide whether it's
 * cheaper to do an indexscan or a seqscan-plus-sort to execute the CLUSTER.
 * Return true to use sorting, false to use an indexscan.
 *
 * Note: caller had better already hold some type of lock on the table.
 */
bool
plan_cluster_use_sort(Oid tableOid, Oid indexOid)
{
	PlannerInfo *root;
	Query	   *query;
	PlannerGlobal *glob;
	RangeTblEntry *rte;
	RelOptInfo *rel;
	IndexOptInfo *indexInfo;
	QualCost	indexExprCost;
	Cost		comparisonCost;
	Path	   *seqScanPath;
	Path		seqScanAndSortPath;
	IndexPath  *indexScanPath;
	ListCell   *lc;

	/* We can short-circuit the cost comparison if indexscans are disabled */
	if (!enable_indexscan)
		return true;			/* use sort */

	/* Set up mostly-dummy planner state */
	query = makeNode(Query);
	query->commandType = CMD_SELECT;

	glob = makeNode(PlannerGlobal);

	root = makeNode(PlannerInfo);
	root->parse = query;
	root->glob = glob;
	root->query_level = 1;
	root->planner_cxt = CurrentMemoryContext;
	root->wt_param_id = -1;
	root->join_domains = list_make1(makeNode(JoinDomain));

	/* Build a minimal RTE for the rel */
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = tableOid;
	rte->relkind = RELKIND_RELATION;	/* Don't be too picky. */
	rte->rellockmode = AccessShareLock;
	rte->lateral = false;
	rte->inh = false;
	rte->inFromCl = true;
	query->rtable = list_make1(rte);
	addRTEPermissionInfo(&query->rteperminfos, rte);

	/* Set up RTE/RelOptInfo arrays */
	setup_simple_rel_arrays(root);

	/* Build RelOptInfo */
	rel = build_simple_rel(root, 1, NULL);

	/* Locate IndexOptInfo for the target index */
	indexInfo = NULL;
	foreach(lc, rel->indexlist)
	{
		indexInfo = lfirst_node(IndexOptInfo, lc);
		if (indexInfo->indexoid == indexOid)
			break;
	}

	/*
	 * It's possible that get_relation_info did not generate an IndexOptInfo
	 * for the desired index; this could happen if it's not yet reached its
	 * indcheckxmin usability horizon, or if it's a system index and we're
	 * ignoring system indexes.  In such cases we should tell CLUSTER to not
	 * trust the index contents but use seqscan-and-sort.
	 */
	if (lc == NULL)				/* not in the list? */
		return true;			/* use sort */

	/*
	 * Rather than doing all the pushups that would be needed to use
	 * set_baserel_size_estimates, just do a quick hack for rows and width.
	 */
	rel->rows = rel->tuples;
	rel->reltarget->width = get_relation_data_width(tableOid, NULL);

	root->total_table_pages = rel->pages;

	/*
	 * Determine eval cost of the index expressions, if any.  We need to
	 * charge twice that amount for each tuple comparison that happens during
	 * the sort, since tuplesort.c will have to re-evaluate the index
	 * expressions each time.  (XXX that's pretty inefficient...)
	 */
	cost_qual_eval(&indexExprCost, indexInfo->indexprs, root);
	comparisonCost = 2.0 * (indexExprCost.startup + indexExprCost.per_tuple);

	/* Estimate the cost of seq scan + sort */
	seqScanPath = create_seqscan_path(root, rel, NULL, 0);
	cost_sort(&seqScanAndSortPath, root, NIL,
			  seqScanPath->disabled_nodes,
			  seqScanPath->total_cost, rel->tuples, rel->reltarget->width,
			  comparisonCost, maintenance_work_mem, -1.0);

	/* Estimate the cost of index scan */
	indexScanPath = create_index_path(root, indexInfo,
									  NIL, NIL, NIL, NIL,
									  ForwardScanDirection, false,
									  NULL, 1.0, false);

	return (seqScanAndSortPath.total_cost < indexScanPath->path.total_cost);
}

/*
 * plan_create_index_workers
 *		Use the planner to decide how many parallel worker processes
 *		CREATE INDEX should request for use
 *
 * tableOid is the table on which the index is to be built.  indexOid is the
 * OID of an index to be created or reindexed (which must be an index with
 * support for parallel builds - currently btree, GIN, or BRIN).
 *
 * Return value is the number of parallel worker processes to request.  It
 * may be unsafe to proceed if this is 0.  Note that this does not include the
 * leader participating as a worker (value is always a number of parallel
 * worker processes).
 *
 * Note: caller had better already hold some type of lock on the table and
 * index.
 */
int
plan_create_index_workers(Oid tableOid, Oid indexOid)
{
	PlannerInfo *root;
	Query	   *query;
	PlannerGlobal *glob;
	RangeTblEntry *rte;
	Relation	heap;
	Relation	index;
	RelOptInfo *rel;
	int			parallel_workers;
	BlockNumber heap_blocks;
	double		reltuples;
	double		allvisfrac;

	/*
	 * We don't allow performing parallel operation in standalone backend or
	 * when parallelism is disabled.
	 */
	if (!IsUnderPostmaster || max_parallel_maintenance_workers == 0)
		return 0;

	/* Set up largely-dummy planner state */
	query = makeNode(Query);
	query->commandType = CMD_SELECT;

	glob = makeNode(PlannerGlobal);

	root = makeNode(PlannerInfo);
	root->parse = query;
	root->glob = glob;
	root->query_level = 1;
	root->planner_cxt = CurrentMemoryContext;
	root->wt_param_id = -1;
	root->join_domains = list_make1(makeNode(JoinDomain));

	/*
	 * Build a minimal RTE.
	 *
	 * Mark the RTE with inh = true.  This is a kludge to prevent
	 * get_relation_info() from fetching index info, which is necessary
	 * because it does not expect that any IndexOptInfo is currently
	 * undergoing REINDEX.
	 */
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = tableOid;
	rte->relkind = RELKIND_RELATION;	/* Don't be too picky. */
	rte->rellockmode = AccessShareLock;
	rte->lateral = false;
	rte->inh = true;
	rte->inFromCl = true;
	query->rtable = list_make1(rte);
	addRTEPermissionInfo(&query->rteperminfos, rte);

	/* Set up RTE/RelOptInfo arrays */
	setup_simple_rel_arrays(root);

	/* Build RelOptInfo */
	rel = build_simple_rel(root, 1, NULL);

	/* Rels are assumed already locked by the caller */
	heap = table_open(tableOid, NoLock);
	index = index_open(indexOid, NoLock);

	/*
	 * Determine if it's safe to proceed.
	 *
	 * Currently, parallel workers can't access the leader's temporary tables.
	 * Furthermore, any index predicate or index expressions must be parallel
	 * safe.
	 */
	if (heap->rd_rel->relpersistence == RELPERSISTENCE_TEMP ||
		!is_parallel_safe(root, (Node *) RelationGetIndexExpressions(index)) ||
		!is_parallel_safe(root, (Node *) RelationGetIndexPredicate(index)))
	{
		parallel_workers = 0;
		goto done;
	}

	/*
	 * If parallel_workers storage parameter is set for the table, accept that
	 * as the number of parallel worker processes to launch (though still cap
	 * at max_parallel_maintenance_workers).  Note that we deliberately do not
	 * consider any other factor when parallel_workers is set. (e.g., memory
	 * use by workers.)
	 */
	if (rel->rel_parallel_workers != -1)
	{
		parallel_workers = Min(rel->rel_parallel_workers,
							   max_parallel_maintenance_workers);
		goto done;
	}

	/*
	 * Estimate heap relation size ourselves, since rel->pages cannot be
	 * trusted (heap RTE was marked as inheritance parent)
	 */
	estimate_rel_size(heap, NULL, &heap_blocks, &reltuples, &allvisfrac);

	/*
	 * Determine number of workers to scan the heap relation using generic
	 * model
	 */
	parallel_workers = compute_parallel_worker(rel, heap_blocks, -1,
											   max_parallel_maintenance_workers);

	/*
	 * Cap workers based on available maintenance_work_mem as needed.
	 *
	 * Note that each tuplesort participant receives an even share of the
	 * total maintenance_work_mem budget.  Aim to leave participants
	 * (including the leader as a participant) with no less than 32MB of
	 * memory.  This leaves cases where maintenance_work_mem is set to 64MB
	 * immediately past the threshold of being capable of launching a single
	 * parallel worker to sort.
	 */
	while (parallel_workers > 0 &&
		   maintenance_work_mem / (parallel_workers + 1) < 32 * 1024)
		parallel_workers--;

done:
	index_close(index, NoLock);
	table_close(heap, NoLock);

	return parallel_workers;
}

/*
 * add_paths_to_grouping_rel
 *
 * Add non-partial paths to grouping relation.
 */
static void
add_paths_to_grouping_rel(PlannerInfo *root, RelOptInfo *input_rel,
						  RelOptInfo *grouped_rel,
						  RelOptInfo *partially_grouped_rel,
						  const AggClauseCosts *agg_costs,
						  grouping_sets_data *gd, double dNumGroups,
						  GroupPathExtraData *extra)
{
	Query	   *parse = root->parse;
	Path	   *cheapest_path = input_rel->cheapest_total_path;
	ListCell   *lc;
	bool		can_hash = (extra->flags & GROUPING_CAN_USE_HASH) != 0;
	bool		can_sort = (extra->flags & GROUPING_CAN_USE_SORT) != 0;
	List	   *havingQual = (List *) extra->havingQual;
	AggClauseCosts *agg_final_costs = &extra->agg_final_costs;

	if (can_sort)
	{
		/*
		 * Use any available suitably-sorted path as input, and also consider
		 * sorting the cheapest-total path and incremental sort on any paths
		 * with presorted keys.
		 */
		foreach(lc, input_rel->pathlist)
		{
			ListCell   *lc2;
			Path	   *path = (Path *) lfirst(lc);
			Path	   *path_save = path;
			List	   *pathkey_orderings = NIL;

			/* generate alternative group orderings that might be useful */
			pathkey_orderings = get_useful_group_keys_orderings(root, path);

			Assert(list_length(pathkey_orderings) > 0);

			foreach(lc2, pathkey_orderings)
			{
				GroupByOrdering *info = (GroupByOrdering *) lfirst(lc2);

				/* restore the path (we replace it in the loop) */
				path = path_save;

				path = make_ordered_path(root,
										 grouped_rel,
										 path,
										 cheapest_path,
										 info->pathkeys,
										 -1.0);
				if (path == NULL)
					continue;

				/* Now decide what to stick atop it */
				if (parse->groupingSets)
				{
					consider_groupingsets_paths(root, grouped_rel,
												path, true, can_hash,
												gd, agg_costs, dNumGroups);
				}
				else if (parse->hasAggs)
				{
					/*
					 * We have aggregation, possibly with plain GROUP BY. Make
					 * an AggPath.
					 */
					add_path(grouped_rel, (Path *)
							 create_agg_path(root,
											 grouped_rel,
											 path,
											 grouped_rel->reltarget,
											 parse->groupClause ? AGG_SORTED : AGG_PLAIN,
											 AGGSPLIT_SIMPLE,
											 info->clauses,
											 havingQual,
											 agg_costs,
											 dNumGroups));
				}
				else if (parse->groupClause)
				{
					/*
					 * We have GROUP BY without aggregation or grouping sets.
					 * Make a GroupPath.
					 */
					add_path(grouped_rel, (Path *)
							 create_group_path(root,
											   grouped_rel,
											   path,
											   info->clauses,
											   havingQual,
											   dNumGroups));
				}
				else
				{
					/* Other cases should have been handled above */
					Assert(false);
				}
			}
		}

		/*
		 * Instead of operating directly on the input relation, we can
		 * consider finalizing a partially aggregated path.
		 */
		if (partially_grouped_rel != NULL)
		{
			foreach(lc, partially_grouped_rel->pathlist)
			{
				ListCell   *lc2;
				Path	   *path = (Path *) lfirst(lc);
				Path	   *path_save = path;
				List	   *pathkey_orderings = NIL;

				/* generate alternative group orderings that might be useful */
				pathkey_orderings = get_useful_group_keys_orderings(root, path);

				Assert(list_length(pathkey_orderings) > 0);

				/* process all potentially interesting grouping reorderings */
				foreach(lc2, pathkey_orderings)
				{
					GroupByOrdering *info = (GroupByOrdering *) lfirst(lc2);

					/* restore the path (we replace it in the loop) */
					path = path_save;

					path = make_ordered_path(root,
											 grouped_rel,
											 path,
											 partially_grouped_rel->cheapest_total_path,
											 info->pathkeys,
											 -1.0);

					if (path == NULL)
						continue;

					if (parse->hasAggs)
						add_path(grouped_rel, (Path *)
								 create_agg_path(root,
												 grouped_rel,
												 path,
												 grouped_rel->reltarget,
												 parse->groupClause ? AGG_SORTED : AGG_PLAIN,
												 AGGSPLIT_FINAL_DESERIAL,
												 info->clauses,
												 havingQual,
												 agg_final_costs,
												 dNumGroups));
					else
						add_path(grouped_rel, (Path *)
								 create_group_path(root,
												   grouped_rel,
												   path,
												   info->clauses,
												   havingQual,
												   dNumGroups));

				}
			}
		}
	}

	if (can_hash)
	{
		if (parse->groupingSets)
		{
			/*
			 * Try for a hash-only groupingsets path over unsorted input.
			 */
			consider_groupingsets_paths(root, grouped_rel,
										cheapest_path, false, true,
										gd, agg_costs, dNumGroups);
		}
		else
		{
			/*
			 * Generate a HashAgg Path.  We just need an Agg over the
			 * cheapest-total input path, since input order won't matter.
			 */
			add_path(grouped_rel, (Path *)
					 create_agg_path(root, grouped_rel,
									 cheapest_path,
									 grouped_rel->reltarget,
									 AGG_HASHED,
									 AGGSPLIT_SIMPLE,
									 root->processed_groupClause,
									 havingQual,
									 agg_costs,
									 dNumGroups));
		}

		/*
		 * Generate a Finalize HashAgg Path atop of the cheapest partially
		 * grouped path, assuming there is one
		 */
		if (partially_grouped_rel && partially_grouped_rel->pathlist)
		{
			Path	   *path = partially_grouped_rel->cheapest_total_path;

			add_path(grouped_rel, (Path *)
					 create_agg_path(root,
									 grouped_rel,
									 path,
									 grouped_rel->reltarget,
									 AGG_HASHED,
									 AGGSPLIT_FINAL_DESERIAL,
									 root->processed_groupClause,
									 havingQual,
									 agg_final_costs,
									 dNumGroups));
		}
	}

	/*
	 * When partitionwise aggregate is used, we might have fully aggregated
	 * paths in the partial pathlist, because add_paths_to_append_rel() will
	 * consider a path for grouped_rel consisting of a Parallel Append of
	 * non-partial paths from each child.
	 */
	if (grouped_rel->partial_pathlist != NIL)
		gather_grouping_paths(root, grouped_rel);
}

/*
 * create_partial_grouping_paths
 *
 * Create a new upper relation representing the result of partial aggregation
 * and populate it with appropriate paths.  Note that we don't finalize the
 * lists of paths here, so the caller can add additional partial or non-partial
 * paths and must afterward call gather_grouping_paths and set_cheapest on
 * the returned upper relation.
 *
 * All paths for this new upper relation -- both partial and non-partial --
 * have been partially aggregated but require a subsequent FinalizeAggregate
 * step.
 *
 * NB: This function is allowed to return NULL if it determines that there is
 * no real need to create a new RelOptInfo.
 */
static RelOptInfo *
create_partial_grouping_paths(PlannerInfo *root,
							  RelOptInfo *grouped_rel,
							  RelOptInfo *input_rel,
							  grouping_sets_data *gd,
							  GroupPathExtraData *extra,
							  bool force_rel_creation)
{
	Query	   *parse = root->parse;
	RelOptInfo *partially_grouped_rel;
	AggClauseCosts *agg_partial_costs = &extra->agg_partial_costs;
	AggClauseCosts *agg_final_costs = &extra->agg_final_costs;
	Path	   *cheapest_partial_path = NULL;
	Path	   *cheapest_total_path = NULL;
	double		dNumPartialGroups = 0;
	double		dNumPartialPartialGroups = 0;
	ListCell   *lc;
	bool		can_hash = (extra->flags & GROUPING_CAN_USE_HASH) != 0;
	bool		can_sort = (extra->flags & GROUPING_CAN_USE_SORT) != 0;

	/*
	 * Consider whether we should generate partially aggregated non-partial
	 * paths.  We can only do this if we have a non-partial path, and only if
	 * the parent of the input rel is performing partial partitionwise
	 * aggregation.  (Note that extra->patype is the type of partitionwise
	 * aggregation being used at the parent level, not this level.)
	 */
	if (input_rel->pathlist != NIL &&
		extra->patype == PARTITIONWISE_AGGREGATE_PARTIAL)
		cheapest_total_path = input_rel->cheapest_total_path;

	/*
	 * If parallelism is possible for grouped_rel, then we should consider
	 * generating partially-grouped partial paths.  However, if the input rel
	 * has no partial paths, then we can't.
	 */
	if (grouped_rel->consider_parallel && input_rel->partial_pathlist != NIL)
		cheapest_partial_path = linitial(input_rel->partial_pathlist);

	/*
	 * If we can't partially aggregate partial paths, and we can't partially
	 * aggregate non-partial paths, then don't bother creating the new
	 * RelOptInfo at all, unless the caller specified force_rel_creation.
	 */
	if (cheapest_total_path == NULL &&
		cheapest_partial_path == NULL &&
		!force_rel_creation)
		return NULL;

	/*
	 * Build a new upper relation to represent the result of partially
	 * aggregating the rows from the input relation.
	 */
	partially_grouped_rel = fetch_upper_rel(root,
											UPPERREL_PARTIAL_GROUP_AGG,
											grouped_rel->relids);
	partially_grouped_rel->consider_parallel =
		grouped_rel->consider_parallel;
	partially_grouped_rel->reloptkind = grouped_rel->reloptkind;
	partially_grouped_rel->serverid = grouped_rel->serverid;
	partially_grouped_rel->userid = grouped_rel->userid;
	partially_grouped_rel->useridiscurrent = grouped_rel->useridiscurrent;
	partially_grouped_rel->fdwroutine = grouped_rel->fdwroutine;

	/*
	 * Build target list for partial aggregate paths.  These paths cannot just
	 * emit the same tlist as regular aggregate paths, because (1) we must
	 * include Vars and Aggrefs needed in HAVING, which might not appear in
	 * the result tlist, and (2) the Aggrefs must be set in partial mode.
	 */
	partially_grouped_rel->reltarget =
		make_partial_grouping_target(root, grouped_rel->reltarget,
									 extra->havingQual);

	if (!extra->partial_costs_set)
	{
		/*
		 * Collect statistics about aggregates for estimating costs of
		 * performing aggregation in parallel.
		 */
		MemSet(agg_partial_costs, 0, sizeof(AggClauseCosts));
		MemSet(agg_final_costs, 0, sizeof(AggClauseCosts));
		if (parse->hasAggs)
		{
			/* partial phase */
			get_agg_clause_costs(root, AGGSPLIT_INITIAL_SERIAL,
								 agg_partial_costs);

			/* final phase */
			get_agg_clause_costs(root, AGGSPLIT_FINAL_DESERIAL,
								 agg_final_costs);
		}

		extra->partial_costs_set = true;
	}

	/* Estimate number of partial groups. */
	if (cheapest_total_path != NULL)
		dNumPartialGroups =
			get_number_of_groups(root,
								 cheapest_total_path->rows,
								 gd,
								 extra->targetList);
	if (cheapest_partial_path != NULL)
		dNumPartialPartialGroups =
			get_number_of_groups(root,
								 cheapest_partial_path->rows,
								 gd,
								 extra->targetList);

	if (can_sort && cheapest_total_path != NULL)
	{
		/* This should have been checked previously */
		Assert(parse->hasAggs || parse->groupClause);

		/*
		 * Use any available suitably-sorted path as input, and also consider
		 * sorting the cheapest partial path.
		 */
		foreach(lc, input_rel->pathlist)
		{
			ListCell   *lc2;
			Path	   *path = (Path *) lfirst(lc);
			Path	   *path_save = path;
			List	   *pathkey_orderings = NIL;

			/* generate alternative group orderings that might be useful */
			pathkey_orderings = get_useful_group_keys_orderings(root, path);

			Assert(list_length(pathkey_orderings) > 0);

			/* process all potentially interesting grouping reorderings */
			foreach(lc2, pathkey_orderings)
			{
				GroupByOrdering *info = (GroupByOrdering *) lfirst(lc2);

				/* restore the path (we replace it in the loop) */
				path = path_save;

				path = make_ordered_path(root,
										 partially_grouped_rel,
										 path,
										 cheapest_total_path,
										 info->pathkeys,
										 -1.0);

				if (path == NULL)
					continue;

				if (parse->hasAggs)
					add_path(partially_grouped_rel, (Path *)
							 create_agg_path(root,
											 partially_grouped_rel,
											 path,
											 partially_grouped_rel->reltarget,
											 parse->groupClause ? AGG_SORTED : AGG_PLAIN,
											 AGGSPLIT_INITIAL_SERIAL,
											 info->clauses,
											 NIL,
											 agg_partial_costs,
											 dNumPartialGroups));
				else
					add_path(partially_grouped_rel, (Path *)
							 create_group_path(root,
											   partially_grouped_rel,
											   path,
											   info->clauses,
											   NIL,
											   dNumPartialGroups));
			}
		}
	}

	if (can_sort && cheapest_partial_path != NULL)
	{
		/* Similar to above logic, but for partial paths. */
		foreach(lc, input_rel->partial_pathlist)
		{
			ListCell   *lc2;
			Path	   *path = (Path *) lfirst(lc);
			Path	   *path_save = path;
			List	   *pathkey_orderings = NIL;

			/* generate alternative group orderings that might be useful */
			pathkey_orderings = get_useful_group_keys_orderings(root, path);

			Assert(list_length(pathkey_orderings) > 0);

			/* process all potentially interesting grouping reorderings */
			foreach(lc2, pathkey_orderings)
			{
				GroupByOrdering *info = (GroupByOrdering *) lfirst(lc2);


				/* restore the path (we replace it in the loop) */
				path = path_save;

				path = make_ordered_path(root,
										 partially_grouped_rel,
										 path,
										 cheapest_partial_path,
										 info->pathkeys,
										 -1.0);

				if (path == NULL)
					continue;

				if (parse->hasAggs)
					add_partial_path(partially_grouped_rel, (Path *)
									 create_agg_path(root,
													 partially_grouped_rel,
													 path,
													 partially_grouped_rel->reltarget,
													 parse->groupClause ? AGG_SORTED : AGG_PLAIN,
													 AGGSPLIT_INITIAL_SERIAL,
													 info->clauses,
													 NIL,
													 agg_partial_costs,
													 dNumPartialPartialGroups));
				else
					add_partial_path(partially_grouped_rel, (Path *)
									 create_group_path(root,
													   partially_grouped_rel,
													   path,
													   info->clauses,
													   NIL,
													   dNumPartialPartialGroups));
			}
		}
	}

	/*
	 * Add a partially-grouped HashAgg Path where possible
	 */
	if (can_hash && cheapest_total_path != NULL)
	{
		/* Checked above */
		Assert(parse->hasAggs || parse->groupClause);

		add_path(partially_grouped_rel, (Path *)
				 create_agg_path(root,
								 partially_grouped_rel,
								 cheapest_total_path,
								 partially_grouped_rel->reltarget,
								 AGG_HASHED,
								 AGGSPLIT_INITIAL_SERIAL,
								 root->processed_groupClause,
								 NIL,
								 agg_partial_costs,
								 dNumPartialGroups));
	}

	/*
	 * Now add a partially-grouped HashAgg partial Path where possible
	 */
	if (can_hash && cheapest_partial_path != NULL)
	{
		add_partial_path(partially_grouped_rel, (Path *)
						 create_agg_path(root,
										 partially_grouped_rel,
										 cheapest_partial_path,
										 partially_grouped_rel->reltarget,
										 AGG_HASHED,
										 AGGSPLIT_INITIAL_SERIAL,
										 root->processed_groupClause,
										 NIL,
										 agg_partial_costs,
										 dNumPartialPartialGroups));
	}

	/*
	 * If there is an FDW that's responsible for all baserels of the query,
	 * let it consider adding partially grouped ForeignPaths.
	 */
	if (partially_grouped_rel->fdwroutine &&
		partially_grouped_rel->fdwroutine->GetForeignUpperPaths)
	{
		FdwRoutine *fdwroutine = partially_grouped_rel->fdwroutine;

		fdwroutine->GetForeignUpperPaths(root,
										 UPPERREL_PARTIAL_GROUP_AGG,
										 input_rel, partially_grouped_rel,
										 extra);
	}

	return partially_grouped_rel;
}

/*
 * make_ordered_path
 *		Return a path ordered by 'pathkeys' based on the given 'path'.  May
 *		return NULL if it doesn't make sense to generate an ordered path in
 *		this case.
 */
static Path *
make_ordered_path(PlannerInfo *root, RelOptInfo *rel, Path *path,
				  Path *cheapest_path, List *pathkeys, double limit_tuples)
{
	bool		is_sorted;
	int			presorted_keys;

	is_sorted = pathkeys_count_contained_in(pathkeys,
											path->pathkeys,
											&presorted_keys);

	if (!is_sorted)
	{
		/*
		 * Try at least sorting the cheapest path and also try incrementally
		 * sorting any path which is partially sorted already (no need to deal
		 * with paths which have presorted keys when incremental sort is
		 * disabled unless it's the cheapest input path).
		 */
		if (path != cheapest_path &&
			(presorted_keys == 0 || !enable_incremental_sort))
			return NULL;

		/*
		 * We've no need to consider both a sort and incremental sort. We'll
		 * just do a sort if there are no presorted keys and an incremental
		 * sort when there are presorted keys.
		 */
		if (presorted_keys == 0 || !enable_incremental_sort)
			path = (Path *) create_sort_path(root,
											 rel,
											 path,
											 pathkeys,
											 limit_tuples);
		else
			path = (Path *) create_incremental_sort_path(root,
														 rel,
														 path,
														 pathkeys,
														 presorted_keys,
														 limit_tuples);
	}

	return path;
}

/*
 * Generate Gather and Gather Merge paths for a grouping relation or partial
 * grouping relation.
 *
 * generate_useful_gather_paths does most of the work, but we also consider a
 * special case: we could try sorting the data by the group_pathkeys and then
 * applying Gather Merge.
 *
 * NB: This function shouldn't be used for anything other than a grouped or
 * partially grouped relation not only because of the fact that it explicitly
 * references group_pathkeys but we pass "true" as the third argument to
 * generate_useful_gather_paths().
 */
static void
gather_grouping_paths(PlannerInfo *root, RelOptInfo *rel)
{
	ListCell   *lc;
	Path	   *cheapest_partial_path;
	List	   *groupby_pathkeys;

	/*
	 * This occurs after any partial aggregation has taken place, so trim off
	 * any pathkeys added for ORDER BY / DISTINCT aggregates.
	 */
	if (list_length(root->group_pathkeys) > root->num_groupby_pathkeys)
		groupby_pathkeys = list_copy_head(root->group_pathkeys,
										  root->num_groupby_pathkeys);
	else
		groupby_pathkeys = root->group_pathkeys;

	/* Try Gather for unordered paths and Gather Merge for ordered ones. */
	generate_useful_gather_paths(root, rel, true);

	cheapest_partial_path = linitial(rel->partial_pathlist);

	/* XXX Shouldn't this also consider the group-key-reordering? */
	foreach(lc, rel->partial_pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);
		bool		is_sorted;
		int			presorted_keys;
		double		total_groups;

		is_sorted = pathkeys_count_contained_in(groupby_pathkeys,
												path->pathkeys,
												&presorted_keys);

		if (is_sorted)
			continue;

		/*
		 * Try at least sorting the cheapest path and also try incrementally
		 * sorting any path which is partially sorted already (no need to deal
		 * with paths which have presorted keys when incremental sort is
		 * disabled unless it's the cheapest input path).
		 */
		if (path != cheapest_partial_path &&
			(presorted_keys == 0 || !enable_incremental_sort))
			continue;

		/*
		 * We've no need to consider both a sort and incremental sort. We'll
		 * just do a sort if there are no presorted keys and an incremental
		 * sort when there are presorted keys.
		 */
		if (presorted_keys == 0 || !enable_incremental_sort)
			path = (Path *) create_sort_path(root, rel, path,
											 groupby_pathkeys,
											 -1.0);
		else
			path = (Path *) create_incremental_sort_path(root,
														 rel,
														 path,
														 groupby_pathkeys,
														 presorted_keys,
														 -1.0);
		total_groups = compute_gather_rows(path);
		path = (Path *)
			create_gather_merge_path(root,
									 rel,
									 path,
									 rel->reltarget,
									 groupby_pathkeys,
									 NULL,
									 &total_groups);

		add_path(rel, path);
	}
}

/*
 * can_partial_agg
 *
 * Determines whether or not partial grouping and/or aggregation is possible.
 * Returns true when possible, false otherwise.
 */
static bool
can_partial_agg(PlannerInfo *root)
{
	Query	   *parse = root->parse;

	if (!parse->hasAggs && parse->groupClause == NIL)
	{
		/*
		 * We don't know how to do parallel aggregation unless we have either
		 * some aggregates or a grouping clause.
		 */
		return false;
	}
	else if (parse->groupingSets)
	{
		/* We don't know how to do grouping sets in parallel. */
		return false;
	}
	else if (root->hasNonPartialAggs || root->hasNonSerialAggs)
	{
		/* Insufficient support for partial mode. */
		return false;
	}

	/* Everything looks good. */
	return true;
}

/*
 * apply_scanjoin_target_to_paths
 *
 * Adjust the final scan/join relation, and recursively all of its children,
 * to generate the final scan/join target.  It would be more correct to model
 * this as a separate planning step with a new RelOptInfo at the toplevel and
 * for each child relation, but doing it this way is noticeably cheaper.
 * Maybe that problem can be solved at some point, but for now we do this.
 *
 * If tlist_same_exprs is true, then the scan/join target to be applied has
 * the same expressions as the existing reltarget, so we need only insert the
 * appropriate sortgroupref information.  By avoiding the creation of
 * projection paths we save effort both immediately and at plan creation time.
 */
static void
apply_scanjoin_target_to_paths(PlannerInfo *root,
							   RelOptInfo *rel,
							   List *scanjoin_targets,
							   List *scanjoin_targets_contain_srfs,
							   bool scanjoin_target_parallel_safe,
							   bool tlist_same_exprs)
{
	bool		rel_is_partitioned = IS_PARTITIONED_REL(rel);
	PathTarget *scanjoin_target;
	ListCell   *lc;

	/* This recurses, so be paranoid. */
	check_stack_depth();

	/*
	 * If the rel is partitioned, we want to drop its existing paths and
	 * generate new ones.  This function would still be correct if we kept the
	 * existing paths: we'd modify them to generate the correct target above
	 * the partitioning Append, and then they'd compete on cost with paths
	 * generating the target below the Append.  However, in our current cost
	 * model the latter way is always the same or cheaper cost, so modifying
	 * the existing paths would just be useless work.  Moreover, when the cost
	 * is the same, varying roundoff errors might sometimes allow an existing
	 * path to be picked, resulting in undesirable cross-platform plan
	 * variations.  So we drop old paths and thereby force the work to be done
	 * below the Append, except in the case of a non-parallel-safe target.
	 *
	 * Some care is needed, because we have to allow
	 * generate_useful_gather_paths to see the old partial paths in the next
	 * stanza.  Hence, zap the main pathlist here, then allow
	 * generate_useful_gather_paths to add path(s) to the main list, and
	 * finally zap the partial pathlist.
	 */
	if (rel_is_partitioned)
		rel->pathlist = NIL;

	/*
	 * If the scan/join target is not parallel-safe, partial paths cannot
	 * generate it.
	 */
	if (!scanjoin_target_parallel_safe)
	{
		/*
		 * Since we can't generate the final scan/join target in parallel
		 * workers, this is our last opportunity to use any partial paths that
		 * exist; so build Gather path(s) that use them and emit whatever the
		 * current reltarget is.  We don't do this in the case where the
		 * target is parallel-safe, since we will be able to generate superior
		 * paths by doing it after the final scan/join target has been
		 * applied.
		 */
		generate_useful_gather_paths(root, rel, false);

		/* Can't use parallel query above this level. */
		rel->partial_pathlist = NIL;
		rel->consider_parallel = false;
	}

	/* Finish dropping old paths for a partitioned rel, per comment above */
	if (rel_is_partitioned)
		rel->partial_pathlist = NIL;

	/* Extract SRF-free scan/join target. */
	scanjoin_target = linitial_node(PathTarget, scanjoin_targets);

	/*
	 * Apply the SRF-free scan/join target to each existing path.
	 *
	 * If the tlist exprs are the same, we can just inject the sortgroupref
	 * information into the existing pathtargets.  Otherwise, replace each
	 * path with a projection path that generates the SRF-free scan/join
	 * target.  This can't change the ordering of paths within rel->pathlist,
	 * so we just modify the list in place.
	 */
	foreach(lc, rel->pathlist)
	{
		Path	   *subpath = (Path *) lfirst(lc);

		/* Shouldn't have any parameterized paths anymore */
		Assert(subpath->param_info == NULL);

		if (tlist_same_exprs)
			subpath->pathtarget->sortgrouprefs =
				scanjoin_target->sortgrouprefs;
		else
		{
			Path	   *newpath;

			newpath = (Path *) create_projection_path(root, rel, subpath,
													  scanjoin_target);
			lfirst(lc) = newpath;
		}
	}

	/* Likewise adjust the targets for any partial paths. */
	foreach(lc, rel->partial_pathlist)
	{
		Path	   *subpath = (Path *) lfirst(lc);

		/* Shouldn't have any parameterized paths anymore */
		Assert(subpath->param_info == NULL);

		if (tlist_same_exprs)
			subpath->pathtarget->sortgrouprefs =
				scanjoin_target->sortgrouprefs;
		else
		{
			Path	   *newpath;

			newpath = (Path *) create_projection_path(root, rel, subpath,
													  scanjoin_target);
			lfirst(lc) = newpath;
		}
	}

	/*
	 * Now, if final scan/join target contains SRFs, insert ProjectSetPath(s)
	 * atop each existing path.  (Note that this function doesn't look at the
	 * cheapest-path fields, which is a good thing because they're bogus right
	 * now.)
	 */
	if (root->parse->hasTargetSRFs)
		adjust_paths_for_srfs(root, rel,
							  scanjoin_targets,
							  scanjoin_targets_contain_srfs);

	/*
	 * Update the rel's target to be the final (with SRFs) scan/join target.
	 * This now matches the actual output of all the paths, and we might get
	 * confused in createplan.c if they don't agree.  We must do this now so
	 * that any append paths made in the next part will use the correct
	 * pathtarget (cf. create_append_path).
	 *
	 * Note that this is also necessary if GetForeignUpperPaths() gets called
	 * on the final scan/join relation or on any of its children, since the
	 * FDW might look at the rel's target to create ForeignPaths.
	 */
	rel->reltarget = llast_node(PathTarget, scanjoin_targets);

	/*
	 * If the relation is partitioned, recursively apply the scan/join target
	 * to all partitions, and generate brand-new Append paths in which the
	 * scan/join target is computed below the Append rather than above it.
	 * Since Append is not projection-capable, that might save a separate
	 * Result node, and it also is important for partitionwise aggregate.
	 */
	if (rel_is_partitioned)
	{
		List	   *live_children = NIL;
		int			i;

		/* Adjust each partition. */
		i = -1;
		while ((i = bms_next_member(rel->live_parts, i)) >= 0)
		{
			RelOptInfo *child_rel = rel->part_rels[i];
			AppendRelInfo **appinfos;
			int			nappinfos;
			List	   *child_scanjoin_targets = NIL;

			Assert(child_rel != NULL);

			/* Dummy children can be ignored. */
			if (IS_DUMMY_REL(child_rel))
				continue;

			/* Translate scan/join targets for this child. */
			appinfos = find_appinfos_by_relids(root, child_rel->relids,
											   &nappinfos);
			foreach(lc, scanjoin_targets)
			{
				PathTarget *target = lfirst_node(PathTarget, lc);

				target = copy_pathtarget(target);
				target->exprs = (List *)
					adjust_appendrel_attrs(root,
										   (Node *) target->exprs,
										   nappinfos, appinfos);
				child_scanjoin_targets = lappend(child_scanjoin_targets,
												 target);
			}
			pfree(appinfos);

			/* Recursion does the real work. */
			apply_scanjoin_target_to_paths(root, child_rel,
										   child_scanjoin_targets,
										   scanjoin_targets_contain_srfs,
										   scanjoin_target_parallel_safe,
										   tlist_same_exprs);

			/* Save non-dummy children for Append paths. */
			if (!IS_DUMMY_REL(child_rel))
				live_children = lappend(live_children, child_rel);
		}

		/* Build new paths for this relation by appending child paths. */
		add_paths_to_append_rel(root, rel, live_children);
	}

	/*
	 * Consider generating Gather or Gather Merge paths.  We must only do this
	 * if the relation is parallel safe, and we don't do it for child rels to
	 * avoid creating multiple Gather nodes within the same plan. We must do
	 * this after all paths have been generated and before set_cheapest, since
	 * one of the generated paths may turn out to be the cheapest one.
	 */
	if (rel->consider_parallel && !IS_OTHER_REL(rel))
		generate_useful_gather_paths(root, rel, false);

	/*
	 * Reassess which paths are the cheapest, now that we've potentially added
	 * new Gather (or Gather Merge) and/or Append (or MergeAppend) paths to
	 * this relation.
	 */
	set_cheapest(rel);
}

/*
 * create_partitionwise_grouping_paths
 *
 * If the partition keys of input relation are part of the GROUP BY clause, all
 * the rows belonging to a given group come from a single partition.  This
 * allows aggregation/grouping over a partitioned relation to be broken down
 * into aggregation/grouping on each partition.  This should be no worse, and
 * often better, than the normal approach.
 *
 * However, if the GROUP BY clause does not contain all the partition keys,
 * rows from a given group may be spread across multiple partitions. In that
 * case, we perform partial aggregation for each group, append the results,
 * and then finalize aggregation.  This is less certain to win than the
 * previous case.  It may win if the PartialAggregate stage greatly reduces
 * the number of groups, because fewer rows will pass through the Append node.
 * It may lose if we have lots of small groups.
 */
static void
create_partitionwise_grouping_paths(PlannerInfo *root,
									RelOptInfo *input_rel,
									RelOptInfo *grouped_rel,
									RelOptInfo *partially_grouped_rel,
									const AggClauseCosts *agg_costs,
									grouping_sets_data *gd,
									PartitionwiseAggregateType patype,
									GroupPathExtraData *extra)
{
	List	   *grouped_live_children = NIL;
	List	   *partially_grouped_live_children = NIL;
	PathTarget *target = grouped_rel->reltarget;
	bool		partial_grouping_valid = true;
	int			i;

	Assert(patype != PARTITIONWISE_AGGREGATE_NONE);
	Assert(patype != PARTITIONWISE_AGGREGATE_PARTIAL ||
		   partially_grouped_rel != NULL);

	/* Add paths for partitionwise aggregation/grouping. */
	i = -1;
	while ((i = bms_next_member(input_rel->live_parts, i)) >= 0)
	{
		RelOptInfo *child_input_rel = input_rel->part_rels[i];
		PathTarget *child_target;
		AppendRelInfo **appinfos;
		int			nappinfos;
		GroupPathExtraData child_extra;
		RelOptInfo *child_grouped_rel;
		RelOptInfo *child_partially_grouped_rel;

		Assert(child_input_rel != NULL);

		/* Dummy children can be ignored. */
		if (IS_DUMMY_REL(child_input_rel))
			continue;

		child_target = copy_pathtarget(target);

		/*
		 * Copy the given "extra" structure as is and then override the
		 * members specific to this child.
		 */
		memcpy(&child_extra, extra, sizeof(child_extra));

		appinfos = find_appinfos_by_relids(root, child_input_rel->relids,
										   &nappinfos);

		child_target->exprs = (List *)
			adjust_appendrel_attrs(root,
								   (Node *) target->exprs,
								   nappinfos, appinfos);

		/* Translate havingQual and targetList. */
		child_extra.havingQual = (Node *)
			adjust_appendrel_attrs(root,
								   extra->havingQual,
								   nappinfos, appinfos);
		child_extra.targetList = (List *)
			adjust_appendrel_attrs(root,
								   (Node *) extra->targetList,
								   nappinfos, appinfos);

		/*
		 * extra->patype was the value computed for our parent rel; patype is
		 * the value for this relation.  For the child, our value is its
		 * parent rel's value.
		 */
		child_extra.patype = patype;

		/*
		 * Create grouping relation to hold fully aggregated grouping and/or
		 * aggregation paths for the child.
		 */
		child_grouped_rel = make_grouping_rel(root, child_input_rel,
											  child_target,
											  extra->target_parallel_safe,
											  child_extra.havingQual);

		/* Create grouping paths for this child relation. */
		create_ordinary_grouping_paths(root, child_input_rel,
									   child_grouped_rel,
									   agg_costs, gd, &child_extra,
									   &child_partially_grouped_rel);

		if (child_partially_grouped_rel)
		{
			partially_grouped_live_children =
				lappend(partially_grouped_live_children,
						child_partially_grouped_rel);
		}
		else
			partial_grouping_valid = false;

		if (patype == PARTITIONWISE_AGGREGATE_FULL)
		{
			set_cheapest(child_grouped_rel);
			grouped_live_children = lappend(grouped_live_children,
											child_grouped_rel);
		}

		pfree(appinfos);
	}

	/*
	 * Try to create append paths for partially grouped children. For full
	 * partitionwise aggregation, we might have paths in the partial_pathlist
	 * if parallel aggregation is possible.  For partial partitionwise
	 * aggregation, we may have paths in both pathlist and partial_pathlist.
	 *
	 * NB: We must have a partially grouped path for every child in order to
	 * generate a partially grouped path for this relation.
	 */
	if (partially_grouped_rel && partial_grouping_valid)
	{
		Assert(partially_grouped_live_children != NIL);

		add_paths_to_append_rel(root, partially_grouped_rel,
								partially_grouped_live_children);

		/*
		 * We need call set_cheapest, since the finalization step will use the
		 * cheapest path from the rel.
		 */
		if (partially_grouped_rel->pathlist)
			set_cheapest(partially_grouped_rel);
	}

	/* If possible, create append paths for fully grouped children. */
	if (patype == PARTITIONWISE_AGGREGATE_FULL)
	{
		Assert(grouped_live_children != NIL);

		add_paths_to_append_rel(root, grouped_rel, grouped_live_children);
	}
}

/*
 * group_by_has_partkey
 *
 * Returns true if all the partition keys of the given relation are part of
 * the GROUP BY clauses, including having matching collation, false otherwise.
 */
static bool
group_by_has_partkey(RelOptInfo *input_rel,
					 List *targetList,
					 List *groupClause)
{
	List	   *groupexprs = get_sortgrouplist_exprs(groupClause, targetList);
	int			cnt = 0;
	int			partnatts;

	/* Input relation should be partitioned. */
	Assert(input_rel->part_scheme);

	/* Rule out early, if there are no partition keys present. */
	if (!input_rel->partexprs)
		return false;

	partnatts = input_rel->part_scheme->partnatts;

	for (cnt = 0; cnt < partnatts; cnt++)
	{
		List	   *partexprs = input_rel->partexprs[cnt];
		ListCell   *lc;
		bool		found = false;

		foreach(lc, partexprs)
		{
			ListCell   *lg;
			Expr	   *partexpr = lfirst(lc);
			Oid			partcoll = input_rel->part_scheme->partcollation[cnt];

			foreach(lg, groupexprs)
			{
				Expr	   *groupexpr = lfirst(lg);
				Oid			groupcoll = exprCollation((Node *) groupexpr);

				/*
				 * Note: we can assume there is at most one RelabelType node;
				 * eval_const_expressions() will have simplified if more than
				 * one.
				 */
				if (IsA(groupexpr, RelabelType))
					groupexpr = ((RelabelType *) groupexpr)->arg;

				if (equal(groupexpr, partexpr))
				{
					/*
					 * Reject a match if the grouping collation does not match
					 * the partitioning collation.
					 */
					if (OidIsValid(partcoll) && OidIsValid(groupcoll) &&
						partcoll != groupcoll)
						return false;

					found = true;
					break;
				}
			}

			if (found)
				break;
		}

		/*
		 * If none of the partition key expressions match with any of the
		 * GROUP BY expression, return false.
		 */
		if (!found)
			return false;
	}

	return true;
}

/*
 * generate_setop_child_grouplist
 *		Build a SortGroupClause list defining the sort/grouping properties
 *		of the child of a set operation.
 *
 * This is similar to generate_setop_grouplist() but differs as the setop
 * child query's targetlist entries may already have a tleSortGroupRef
 * assigned for other purposes, such as GROUP BYs.  Here we keep the
 * SortGroupClause list in the same order as 'op' groupClauses and just adjust
 * the tleSortGroupRef to reference the TargetEntry's 'ressortgroupref'.  If
 * any of the columns in the targetlist don't match to the setop's colTypes
 * then we return an empty list.  This may leave some TLEs with unreferenced
 * ressortgroupref markings, but that's harmless.
 */
static List *
generate_setop_child_grouplist(SetOperationStmt *op, List *targetlist)
{
	List	   *grouplist = copyObject(op->groupClauses);
	ListCell   *lg;
	ListCell   *lt;
	ListCell   *ct;

	lg = list_head(grouplist);
	ct = list_head(op->colTypes);
	foreach(lt, targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lt);
		SortGroupClause *sgc;
		Oid			coltype;

		/* resjunk columns could have sortgrouprefs.  Leave these alone */
		if (tle->resjunk)
			continue;

		/*
		 * We expect every non-resjunk target to have a SortGroupClause and
		 * colTypes.
		 */
		Assert(lg != NULL);
		Assert(ct != NULL);
		sgc = (SortGroupClause *) lfirst(lg);
		coltype = lfirst_oid(ct);

		/* reject if target type isn't the same as the setop target type */
		if (coltype != exprType((Node *) tle->expr))
			return NIL;

		lg = lnext(grouplist, lg);
		ct = lnext(op->colTypes, ct);

		/* assign a tleSortGroupRef, or reuse the existing one */
		sgc->tleSortGroupRef = assignSortGroupRef(tle, targetlist);
	}

	Assert(lg == NULL);
	Assert(ct == NULL);

	return grouplist;
}
