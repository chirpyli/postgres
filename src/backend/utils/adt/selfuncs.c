/*-------------------------------------------------------------------------
 *
 * selfuncs.c
 *	  标准操作符与索引访问方法的选择性函数及索引代价估算函数。
 *
 *	  选择性例程注册于 pg_operator 系统表的 "oprrest" 与 "oprjoin" 属性中。
 *
 *	  索引代价函数通过索引访问方法（AM）的 API 结构体定位，
 *	  该结构体由注册在 pg_am 中的处理函数提供。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/selfuncs.c
 *
 *-------------------------------------------------------------------------
 */

/*----------
 * 选择性（selectivity）估计函数用于估算 WHERE 子句中顶层操作符为其本身的选择性。
 * 我们把问题分为两种情况：
 *		限制子句估计：子句只涉及单个关系的变量。
 *		连接子句估计：子句涉及多个关系的变量。
 * 连接选择性估计远比限制子句估计困难，且通常精度更低。
 *
 * 处理嵌套循环连接（nestloop join）的内表扫描时，我们把
 * 连接的连接条件（joinclauses）视为内表关系的限制子句，并将
 * 外表关系的变量视为参数（即取值未知的常量）。因此，限制子句
 * 估计器需要能够接受一个参数来指明哪个关系被当作变量。
 *
 * 限制子句估计器（oprrest 函数）的调用约定如下：
 *
 *		Selectivity oprrest (PlannerInfo *root,
 *							 Oid operator,
 *							 List *args,
 *							 int varRelid);
 *
 * root: 关于查询的总体信息（rtable 与 RelOptInfo 列表
 * 对估计器尤为重要）。
 * operator: 所讨论的具体操作符的 OID。
 * args: 来自操作符子句的参数列表。
 * varRelid: 若非零，表示被当作变量关系的关系 id（rtable 索引）。
 * 若已知 args 列表仅包含单个关系的变量，则可为零。
 *
 * 在 SQL 层面（pg_proc 中）其表示为：
 *
 *		float8 oprrest (internal, oid, internal, int4);
 *
 * 返回值为一个选择性，即期望对给定操作符产生 TRUE 结果的
 * 关系行数所占的比例（0 到 1）。
 *
 * 连接估计器（oprjoin 函数）的调用约定类似，只是不需要 varRelid，
 * 而是提供连接信息：
 *
 *		Selectivity oprjoin (PlannerInfo *root,
 *							 Oid operator,
 *							 List *args,
 *							 JoinType jointype,
 *							 SpecialJoinInfo *sjinfo);
 *
 *		float8 oprjoin (internal, oid, internal, int2, internal);
 *
 * （在 Postgres 8.4 之前，连接估计器只有前面四个参数。
 * 该签名目前仍被允许，但已废弃。）jointype 与 sjinfo 之间的关系
 * 在 clause_selectivity() 的注释中有说明——简而言之，通常应忽略
 * jointype，转而检查 sjinfo。
 *
 * 对于常规内连接与外连接，连接选择性定义为关系的笛卡尔积中
 * 期望对给定操作符产生 TRUE 结果的部分所占比例（0 到 1）。
 * 但对于半连接（semi）和反连接（anti），选择性定义为左表关系的
 * 行中期望在右表中存在匹配（即至少一行产生 TRUE 结果）的比例。
 *
 * 对于 oprrest 与 oprjoin 函数，操作符的输入排序规则 OID（若有）
 * 通过标准 fmgr 机制传递，估计器函数可用 PG_GET_COLLATION() 获取。
 * 但需注意，pg_statistic 中的所有统计信息目前都是使用相关列的
 * 排序规则构建的。
 *----------
 */

#include "postgres.h"

#include <ctype.h>
#include <math.h>

#include "access/brin.h"
#include "access/brin_page.h"
#include "access/gin.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/visibilitymap.h"
#include "catalog/pg_am.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_statistic_ext.h"
#include "executor/nodeAgg.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "parser/parse_clause.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "statistics/statistics.h"
#include "storage/bufmgr.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/index_selfuncs.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/snapmgr.h"
#include "utils/spccache.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/typcache.h"

#define DEFAULT_PAGE_CPU_MULTIPLIER 50.0

/* 插件在请求统计信息时获取控制权的钩子 */
get_relation_stats_hook_type get_relation_stats_hook = NULL;
get_index_stats_hook_type get_index_stats_hook = NULL;

static double eqsel_internal(PG_FUNCTION_ARGS, bool negate);
static double eqjoinsel_inner(Oid opfuncoid, Oid collation,
							  VariableStatData *vardata1, VariableStatData *vardata2,
							  double nd1, double nd2,
							  bool isdefault1, bool isdefault2,
							  AttStatsSlot *sslot1, AttStatsSlot *sslot2,
							  Form_pg_statistic stats1, Form_pg_statistic stats2,
							  bool have_mcvs1, bool have_mcvs2);
static double eqjoinsel_semi(Oid opfuncoid, Oid collation,
							 VariableStatData *vardata1, VariableStatData *vardata2,
							 double nd1, double nd2,
							 bool isdefault1, bool isdefault2,
							 AttStatsSlot *sslot1, AttStatsSlot *sslot2,
							 Form_pg_statistic stats1, Form_pg_statistic stats2,
							 bool have_mcvs1, bool have_mcvs2,
							 RelOptInfo *inner_rel);
static bool estimate_multivariate_ndistinct(PlannerInfo *root,
											RelOptInfo *rel, List **varinfos, double *ndistinct);
static bool convert_to_scalar(Datum value, Oid valuetypid, Oid collid,
							  double *scaledvalue,
							  Datum lobound, Datum hibound, Oid boundstypid,
							  double *scaledlobound, double *scaledhibound);
static double convert_numeric_to_scalar(Datum value, Oid typid, bool *failure);
static void convert_string_to_scalar(char *value,
									 double *scaledvalue,
									 char *lobound,
									 double *scaledlobound,
									 char *hibound,
									 double *scaledhibound);
static void convert_bytea_to_scalar(Datum value,
									double *scaledvalue,
									Datum lobound,
									double *scaledlobound,
									Datum hibound,
									double *scaledhibound);
static double convert_one_string_to_scalar(char *value,
										   int rangelo, int rangehi);
static double convert_one_bytea_to_scalar(unsigned char *value, int valuelen,
										  int rangelo, int rangehi);
static char *convert_string_datum(Datum value, Oid typid, Oid collid,
								  bool *failure);
static double convert_timevalue_to_scalar(Datum value, Oid typid,
										  bool *failure);
static Node *strip_all_phvs_deep(PlannerInfo *root, Node *node);
static bool contain_placeholder_walker(Node *node, void *context);
static Node *strip_all_phvs_mutator(Node *node, void *context);
static void examine_simple_variable(PlannerInfo *root, Var *var,
									VariableStatData *vardata);
static void examine_indexcol_variable(PlannerInfo *root, IndexOptInfo *index,
									  int indexcol, VariableStatData *vardata);
static bool get_variable_range(PlannerInfo *root, VariableStatData *vardata,
							   Oid sortop, Oid collation,
							   Datum *min, Datum *max);
static void get_stats_slot_range(AttStatsSlot *sslot,
								 Oid opfuncoid, FmgrInfo *opproc,
								 Oid collation, int16 typLen, bool typByVal,
								 Datum *min, Datum *max, bool *p_have_data);
static bool get_actual_variable_range(PlannerInfo *root,
									  VariableStatData *vardata,
									  Oid sortop, Oid collation,
									  Datum *min, Datum *max);
static bool get_actual_variable_endpoint(Relation heapRel,
										 Relation indexRel,
										 ScanDirection indexscandir,
										 ScanKey scankeys,
										 int16 typLen,
										 bool typByVal,
										 TupleTableSlot *tableslot,
										 MemoryContext outercontext,
										 Datum *endpointDatum);
static RelOptInfo *find_join_input_rel(PlannerInfo *root, Relids relids);
static double btcost_correlation(IndexOptInfo *index,
								 VariableStatData *vardata);


/*
 *		eqsel			- 任意数据类型的 "=" 选择性。
 *
 * 注意：本例程也用于估算某些并非 "=" 但具有类似选择性行为的
 * 操作符的选择性，例如 "~="（几何近似匹配）。即使是 "=",
 * 我们也必须记住左右两侧的数据类型可能不同。
 */
Datum
eqsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8((float8) eqsel_internal(fcinfo, false));
}

/*
 * eqsel() 与 neqsel() 的共用代码
 */
static double
eqsel_internal(PG_FUNCTION_ARGS, bool negate)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	Oid			collation = PG_GET_COLLATION();
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;
	double		selec;

	/*
	 * 当被问及 <> 时，我们先使用对应的 = 操作符进行估计，
	 * 然后通过 "1.0 - eq_selectivity - nullfrac" 转换为 <>。
	 */
	if (negate)
	{
		operator = get_negator(operator);
		if (!OidIsValid(operator))
		{
			/* 使用默认选择性（或者我们是否应该改为报错？） */
			return 1.0 - DEFAULT_EQ_SEL;
		}
	}

	/*
	 * 如果表达式不是 变量 = 某值 或 某值 = 变量 的形式，
	 * 则放弃并返回默认估计值。
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		return negate ? (1.0 - DEFAULT_EQ_SEL) : DEFAULT_EQ_SEL;

	/*
	 * 如果另一侧是一个常量，我们可以做得更好。（注意：该
	 * Const 可能是由估计过程产生的，而不一定是查询中简单的常量。）
	 */
	if (IsA(other, Const))
		selec = var_eq_const(&vardata, operator, collation,
							 ((Const *) other)->constvalue,
							 ((Const *) other)->constisnull,
							 varonleft, negate);
	else
		selec = var_eq_non_const(&vardata, operator, collation, other,
								 varonleft, negate);

	ReleaseVariableStats(vardata);

	return selec;
}

/*
 * var_eq_const --- eqsel for var = const case
 *
 * This is exported so that some other estimation functions can use it.
 */
double
var_eq_const(VariableStatData *vardata, Oid oproid, Oid collation,
			 Datum constval, bool constisnull,
			 bool varonleft, bool negate)
{
	double		selec;
	double		nullfrac = 0.0;
	bool		isdefault;
	Oid			opfuncoid;

	/*
	 * If the constant is NULL, assume operator is strict and return zero, ie,
	 * operator will never return TRUE.  (It's zero even for a negator op.)
	 */
	if (constisnull)
		return 0.0;

	/*
	 * 抓取 nullfrac 以备后续使用。注意我们允许使用 nullfrac，
	 * 而不受安全性检查的限制。
	 */
	if (HeapTupleIsValid(vardata->statsTuple))
	{
		Form_pg_statistic stats;

		stats = (Form_pg_statistic) GETSTRUCT(vardata->statsTuple);
		nullfrac = stats->stanullfrac;
	}

	/*
	 * 如果变量匹配到了唯一索引、DISTINCT 或 GROUP-BY 子句，
	 * 则假定恰好只有一个匹配，而不考虑其他任何因素。（这
	 * 稍微有些不可靠，因为该索引或子句所用的等值操作符
	 * 可能和我们不同，但忽略这一信息反而更可能出错。）
	 */
	if (vardata->isunique && vardata->rel && vardata->rel->tuples >= 1.0)
	{
		selec = 1.0 / vardata->rel->tuples;
	}
	else if (HeapTupleIsValid(vardata->statsTuple) &&
			 statistic_proc_security_check(vardata,
										   (opfuncoid = get_opcode(oproid))))
	{
		AttStatsSlot sslot;
		bool		match = false;
		int			i;

		/*
		 * 该常量是否 "=" 于列的任一最常见值（MCV）？
		 * （尽管给定的操作符可能并非真正的 "="，我们仍假定
		 * 判断其是否返回 TRUE 是一种合适的测试。如果你
		 * 不喜欢这样，也许你不该把 eqsel 用于你的
		 * 操作符……）
		 */
		if (get_attstatsslot(&sslot, vardata->statsTuple,
							 STATISTIC_KIND_MCV, InvalidOid,
							 ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS))
		{
			LOCAL_FCINFO(fcinfo, 2);
			FmgrInfo	eqproc;

			fmgr_info(opfuncoid, &eqproc);

			/*
			 * 通过只初始化一次 fcinfo 结构体来节省若干周期。
			 * 直接使用 FunctionCallInvoke 也能避免 eqproc 返回
			 * NULL 时失败，尽管等值函数本不应返回 NULL。
			 */
			InitFunctionCallInfoData(*fcinfo, &eqproc, 2, collation,
									 NULL, NULL);
			fcinfo->args[0].isnull = false;
			fcinfo->args[1].isnull = false;
			/* be careful to apply operator right way 'round */
			if (varonleft)
				fcinfo->args[1].value = constval;
			else
				fcinfo->args[0].value = constval;

			for (i = 0; i < sslot.nvalues; i++)
			{
				Datum		fresult;

				if (varonleft)
					fcinfo->args[0].value = sslot.values[i];
				else
					fcinfo->args[1].value = sslot.values[i];
				fcinfo->isnull = false;
				fresult = FunctionCallInvoke(fcinfo);
				if (!fcinfo->isnull && DatumGetBool(fresult))
				{
					match = true;
					break;
				}
			}
		}
		else
		{
		/* 没有可用的常见值（MCV）信息 */
		i = 0;				/* 避免编译器告警 */
		}

		if (match)
		{
			/*
			 * Constant is "=" to this common value.  We know selectivity
			 * exactly (or as exactly as ANALYZE could calculate it, anyway).
			 */
			selec = sslot.numbers[i];
		}
		else
		{
			/*
			 * 被比较的常量既不是 NULL 也不属于任何常见值。
			 * 其选择性不可能超过这个值：
			 */
			double		sumcommon = 0.0;
			double		otherdistinct;

			for (i = 0; i < sslot.nnumbers; i++)
				sumcommon += sslot.numbers[i];
			selec = 1.0 - sumcommon - nullfrac;
			CLAMP_PROBABILITY(selec);

			/*
			 * 而实际上它可能要小得多。我们近似认为所有
			 * 非常见值平分剩余的这部分比例，因此用其他
			 * 不同值的数量来除。
			 */
			otherdistinct = get_variable_numdistinct(vardata, &isdefault) -
				sslot.nnumbers;
			if (otherdistinct > 1)
				selec /= otherdistinct;

			/*
			 * 再做一个交叉校验：选择性不应被估计得高于
			 * 最不常见的那个“最常见值”。
			 */
			if (sslot.nnumbers > 0 && selec > sslot.numbers[sslot.nnumbers - 1])
				selec = sslot.numbers[sslot.nnumbers - 1];
		}

		free_attstatsslot(&sslot);
	}
	else
	{
		/*
		 * 没有可用的 ANALYZE 统计信息，于是利用估计的不同值
		 * 数量并假设它们出现频率相同来做一次猜测。（该猜测
		 * 不太可能很准确，但我们的确知道一些特殊情况。）
		 */
		selec = 1.0 / get_variable_numdistinct(vardata, &isdefault);
	}

	/* 如果需要的是 <> 而非 =，则在此调整 */
	if (negate)
		selec = 1.0 - selec - nullfrac;

	/* 结果应在 [0,1] 范围内，但仍需确保…… */
	CLAMP_PROBABILITY(selec);

	return selec;
}

/*
 * var_eq_non_const --- eqsel for var = something-other-than-const case
 *
 * This is exported so that some other estimation functions can use it.
 */
double
var_eq_non_const(VariableStatData *vardata, Oid oproid, Oid collation,
				 Node *other,
				 bool varonleft, bool negate)
{
	double		selec;
	double		nullfrac = 0.0;
	bool		isdefault;

	/*
	 * 抓取 nullfrac 以备后续使用。
	 */
	if (HeapTupleIsValid(vardata->statsTuple))
	{
		Form_pg_statistic stats;

		stats = (Form_pg_statistic) GETSTRUCT(vardata->statsTuple);
		nullfrac = stats->stanullfrac;
	}

	/*
	 * 如果变量匹配到了唯一索引、DISTINCT 或 GROUP-BY 子句，
	 * 则假定恰好只有一个匹配，而不考虑其他任何因素。（这
	 * 稍微有些不可靠，因为该索引或子句所用的等值操作符
	 * 可能和我们不同，但忽略这一信息反而更可能出错。）
	 */
	if (vardata->isunique && vardata->rel && vardata->rel->tuples >= 1.0)
	{
		selec = 1.0 / vardata->rel->tuples;
	}
	else if (HeapTupleIsValid(vardata->statsTuple))
	{
		double		ndistinct;
		AttStatsSlot sslot;

		/*
		 * 这里搜索的是一个我们先前并不知道的值，但我们会
		 * 假定它不为 NULL。把选择性估计为非空比例除以不同值的
		 * 数量，这样我们得到的结果就是对所有可能值（无论是常见
		 * 还是少见）的平均。（本质上，我们是假定这个尚未
		 * 可知的比较值等可能地是任意一个可能的值，而不管它们在
		 * 表中的实际频率。这算是个好主意吗？）
		 */
		selec = 1.0 - nullfrac;
		ndistinct = get_variable_numdistinct(vardata, &isdefault);
		if (ndistinct > 1)
			selec /= ndistinct;

		/*
		 * 交叉校验：选择性永远不应被估计得高于
		 * 最常见值的比例。
		 */
		if (get_attstatsslot(&sslot, vardata->statsTuple,
							 STATISTIC_KIND_MCV, InvalidOid,
							 ATTSTATSSLOT_NUMBERS))
		{
			if (sslot.nnumbers > 0 && selec > sslot.numbers[0])
				selec = sslot.numbers[0];
			free_attstatsslot(&sslot);
		}
	}
	else
	{
		/*
		 * 没有可用的 ANALYZE 统计信息，于是利用估计的不同值
		 * 数量并假设它们出现频率相同来做一次猜测。（该猜测
		 * 不太可能很准确，但我们的确知道一些特殊情况。）
		 */
		selec = 1.0 / get_variable_numdistinct(vardata, &isdefault);
	}

	/* 如果需要的是 <> 而非 =，则在此调整 */
	if (negate)
		selec = 1.0 - selec - nullfrac;

	/* 结果应在 [0,1] 范围内，但仍需确保…… */
	CLAMP_PROBABILITY(selec);

	return selec;
}

/*
 *		neqsel			- Selectivity of "!=" for any data types.
 *
 * This routine is also used for some operators that are not "!="
 * but have comparable selectivity behavior.  See above comments
 * for eqsel().
 */
Datum
neqsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8((float8) eqsel_internal(fcinfo, true));
}

/*
 *	scalarineqsel		- 标量类型 "<"、"<="、">"、">=" 的选择性。
 *
 * 这是 scalarltsel/scalarlesel/scalargtsel/scalargesel 的核心实现。
 * isgt 与 iseq 标志用于区分上述四种情况中哪一种适用。
 *
 * 调用方（必要时）已经对子句做了交换律变换，使我们可以把
 * 变量视为位于左侧。调用方还须确保子句另一侧是一个非 NULL 的
 * Const，并将其拆解为值与数据类型。（这样定义简化了某些
 * 希望针对计算值而非 Const 节点进行估计的调用方。）
 *
 * 本例程适用于任何 convert_to_scalar() 所能处理的
 * 数据类型（或数据类型对）。如果把它用于其它数据类型，
 * 它将返回一个近似估计，假定常量值落在二分查找所定位的
 * 区间（bin）的中间位置。
 */
static double
scalarineqsel(PlannerInfo *root, Oid operator, bool isgt, bool iseq,
			  Oid collation,
			  VariableStatData *vardata, Datum constval, Oid consttype)
{
	Form_pg_statistic stats;
	FmgrInfo	opproc;
	double		mcv_selec,
				hist_selec,
				sumcommon;
	double		selec;

	if (!HeapTupleIsValid(vardata->statsTuple))
	{
		/*
		 * No stats are available.  Typically this means we have to fall back
		 * on the default estimate; but if the variable is CTID then we can
		 * make an estimate based on comparing the constant to the table size.
		 */
		if (vardata->var && IsA(vardata->var, Var) &&
			((Var *) vardata->var)->varattno == SelfItemPointerAttributeNumber)
		{
			ItemPointer itemptr;
			double		block;
			double		density;

			/*
			 * 如果关系为空，我们将包含它的全部。
			 * （这主要是为了避免下面的除以零。）
			 */
			if (vardata->rel->pages == 0)
				return 1.0;

			itemptr = (ItemPointer) DatumGetPointer(constval);
			block = ItemPointerGetBlockNumberNoCheck(itemptr);

			/*
			 * Determine the average number of tuples per page (density).
			 *
			 * Since the last page will, on average, be only half full, we can
			 * estimate it to have half as many tuples as earlier pages.  So
			 * give it half the weight of a regular page.
			 */
			density = vardata->rel->tuples / (vardata->rel->pages - 0.5);

			/* 如果目标为最后一页，则使用一半的密度。 */
			if (block >= vardata->rel->pages - 1)
				density *= 0.5;

			/*
			 * 利用每页的平均元组数，估算 itemptr 大约位于
			 * 页内的什么位置，并据以调整 block 值，方法是
			 * 加上一整块中的该比例部分（但无论 itemptr 的偏移
			 * 量多大，都绝不超过一整块）。这里
			 * 我们忽略了死元组行指针的可能性，这相当不严谨，
			 * 但我们缺乏更好的信息。
			 */
			if (density > 0.0)
			{
				OffsetNumber offset = ItemPointerGetOffsetNumberNoCheck(itemptr);

				block += Min(offset / density, 1.0);
			}

			/*
			 * 将相对块号转换为选择性。同样，最后一页
			 * 只有一半的权重。
			 */
			selec = block / (vardata->rel->pages - 0.5);

			/*
			 * 到目前为止的计算给出了 "<=" 情况下的选择性。
			 * 对于 "<" 我们会少算一个元组，对于 ">=" 会多算一个
			 * 元组（后者我们会在下方做选择性反转），因此两种情况
			 * 都可以简单地减去一个元组。需要此调整的情况可由
			 * iseq 等于 isgt 来识别。
			 */
			if (iseq == isgt && vardata->rel->tuples >= 1.0)
				selec -= (1.0 / vardata->rel->tuples);

			/* 最后，对 ">"、">=" 情况做选择性反转。 */
			if (isgt)
				selec = 1.0 - selec;

			CLAMP_PROBABILITY(selec);
			return selec;
		}

		/* 没有可用的统计信息，因此返回默认结果 */
		return DEFAULT_INEQ_SEL;
	}
	stats = (Form_pg_statistic) GETSTRUCT(vardata->statsTuple);

	fmgr_info(get_opcode(operator), &opproc);

	/*
	 * 如果拥有最常见值（MCV）信息，则累加满足
	 * MCV OP CONST 的 MCV 条目的比例。这些比例直接
	 * 贡献于最终的选择性。同时累加 MCV 条目所代表的
	 * 总体比例。
	 */
	mcv_selec = mcv_selectivity(vardata, &opproc, collation, constval, true,
								&sumcommon);

	/*
	 * If there is a histogram, determine which bin the constant falls in, and
	 * compute the resulting contribution to selectivity.
	 */
	hist_selec = ineq_histogram_selectivity(root, vardata,
											operator, &opproc, isgt, iseq,
											collation,
											constval, consttype);

	/*
	 * Now merge the results from the MCV and histogram calculations,
	 * realizing that the histogram covers only the non-null values that are
	 * not listed in MCV.
	 */
	selec = 1.0 - stats->stanullfrac - sumcommon;

	if (hist_selec >= 0.0)
		selec *= hist_selec;
	else
	{
		/*
		 * 如果没有直方图，但存在 MCV 未覆盖的值，
		 * 则武断地假定其中一半会匹配。
		 */
		selec *= 0.5;
	}

	selec += mcv_selec;

	/* 结果应在 [0,1] 范围内，但仍需确保…… */
	CLAMP_PROBABILITY(selec);

	return selec;
}

/*
 *	mcv_selectivity			- 检查 MCV 列表以估算选择性
 *
 * 确定变量的 MCV 群体中满足谓词 (VAR OP CONST)（若 !varonleft
 * 则为 (CONST OP VAR)）的部分所占比例。同时计算
 * MCV 列表所代表的总列群体的比例。本代码适用于任何
 * 返回布尔值的谓词操作符。
 *
 * 函数返回值为 MCV 选择性，总群体的比例通过
 * *sumcommonp 返回。若没有 MCV 列表，则返回零。
 */
double
mcv_selectivity(VariableStatData *vardata, FmgrInfo *opproc, Oid collation,
				Datum constval, bool varonleft,
				double *sumcommonp)
{
	double		mcv_selec,
				sumcommon;
	AttStatsSlot sslot;
	int			i;

	mcv_selec = 0.0;
	sumcommon = 0.0;

	if (HeapTupleIsValid(vardata->statsTuple) &&
		statistic_proc_security_check(vardata, opproc->fn_oid) &&
		get_attstatsslot(&sslot, vardata->statsTuple,
						 STATISTIC_KIND_MCV, InvalidOid,
						 ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS))
	{
		LOCAL_FCINFO(fcinfo, 2);

		/*
		 * 我们“手工”调用 opproc，以免在结果为 NULL 时失败。
		 * 对于普通比较函数这种情况不会出现，但
		 * generic_restriction_selectivity 可能会被用于
		 * 能够返回 NULL 的操作符。一个附带的小好处是
		 * 无需每次都从头重新初始化 fcinfo 结构体。
		 */
		InitFunctionCallInfoData(*fcinfo, opproc, 2, collation,
								 NULL, NULL);
		fcinfo->args[0].isnull = false;
		fcinfo->args[1].isnull = false;
		/* be careful to apply operator right way 'round */
		if (varonleft)
			fcinfo->args[1].value = constval;
		else
			fcinfo->args[0].value = constval;

		for (i = 0; i < sslot.nvalues; i++)
		{
			Datum		fresult;

			if (varonleft)
				fcinfo->args[0].value = sslot.values[i];
			else
				fcinfo->args[1].value = sslot.values[i];
			fcinfo->isnull = false;
			fresult = FunctionCallInvoke(fcinfo);
			if (!fcinfo->isnull && DatumGetBool(fresult))
				mcv_selec += sslot.numbers[i];
			sumcommon += sslot.numbers[i];
		}
		free_attstatsslot(&sslot);
	}

	*sumcommonp = sumcommon;
	return mcv_selec;
}

/*
 *	histogram_selectivity	- 检查直方图以估算选择性
 *
 * 确定变量的直方图条目中满足谓词 (VAR OP CONST)（若 !varonleft
 * 则为 (CONST OP VAR)）的部分所占比例。
 *
 * 本代码适用于任何返回布尔值的谓词操作符，无论它
 * 是否与直方图的排序操作符有关。我们本质上只是把直方图
 * 当作一个有代表性的样本来使用。然而，较小的直方图
 * 不太可能具有很强的代表性，因此调用方在直方图缺失或
 * 非常小时应准备好退回到其它估计方法。在直方图较小
 * 时，将该方法与另一种方法结合使用也许是审慎之举。
 *
 * 如果实际直方图的大小不足 min_hist_size，我们将干脆
 * 不做此计算。此外，如果 n_skip 参数大于 0，我们会
 * 忽略直方图的前 n_skip 个和最后 n_skip 个元素，理由
 * 是它们是离群值，因此代表性不强。这些参数的典型值
 * 为 10 和 1。
 *
 * 函数返回值为选择性；若没有直方图或它小于 min_hist_size，
 * 则返回 -1。
 *
 * 输出参数 *hist_size 接收实际的直方图大小，
 * 若无直方图则为零。调用方可用该数值来判断对函数
 * 结果的信任程度。
 *
 * 注意，该结果既忽略了最常见值（MCV，若有）也忽略了
 * NULL 条目。调用方应将此结果与针对列群体中那些
 * 部分（MCV、NULL）的统计信息结合起来使用。将结果
 * 范围钳制一下也许是审慎的，即不要完全相信精确的
 * 0 或 1 输出。
 */
double
histogram_selectivity(VariableStatData *vardata,
					  FmgrInfo *opproc, Oid collation,
					  Datum constval, bool varonleft,
					  int min_hist_size, int n_skip,
					  int *hist_size)
{
	double		result;
	AttStatsSlot sslot;

	/* check sanity of parameters */
	Assert(n_skip >= 0);
	Assert(min_hist_size > 2 * n_skip);

	if (HeapTupleIsValid(vardata->statsTuple) &&
		statistic_proc_security_check(vardata, opproc->fn_oid) &&
		get_attstatsslot(&sslot, vardata->statsTuple,
						 STATISTIC_KIND_HISTOGRAM, InvalidOid,
						 ATTSTATSSLOT_VALUES))
	{
		*hist_size = sslot.nvalues;
		if (sslot.nvalues >= min_hist_size)
		{
			LOCAL_FCINFO(fcinfo, 2);
			int			nmatch = 0;
			int			i;

			/*
			 * 我们“手工”调用 opproc，以免在结果为 NULL 时失败。
			 * 对于普通比较函数这种情况不会出现，但
			 * generic_restriction_selectivity 可能会被用于
			 * 能够返回 NULL 的操作符。一个附带的小好处是
			 * 无需每次都从头重新初始化 fcinfo 结构体。
			 */
			InitFunctionCallInfoData(*fcinfo, opproc, 2, collation,
									 NULL, NULL);
			fcinfo->args[0].isnull = false;
			fcinfo->args[1].isnull = false;
			/* be careful to apply operator right way 'round */
			if (varonleft)
				fcinfo->args[1].value = constval;
			else
				fcinfo->args[0].value = constval;

			for (i = n_skip; i < sslot.nvalues - n_skip; i++)
			{
				Datum		fresult;

				if (varonleft)
					fcinfo->args[0].value = sslot.values[i];
				else
					fcinfo->args[1].value = sslot.values[i];
				fcinfo->isnull = false;
				fresult = FunctionCallInvoke(fcinfo);
				if (!fcinfo->isnull && DatumGetBool(fresult))
					nmatch++;
			}
			result = ((double) nmatch) / ((double) (sslot.nvalues - 2 * n_skip));
		}
		else
			result = -1;
		free_attstatsslot(&sslot);
	}
	else
	{
		*hist_size = 0;
		result = -1;
	}

	return result;
}

/*
 *	generic_restriction_selectivity		- Selectivity for almost anything
 *
 * This function estimates selectivity for operators that we don't have any
 * special knowledge about, but are on data types that we collect standard
 * MCV and/or histogram statistics for.  (Additional assumptions are that
 * the operator is strict and immutable, or at least stable.)
 *
 * If we have "VAR OP CONST" or "CONST OP VAR", selectivity is estimated by
 * applying the operator to each element of the column's MCV and/or histogram
 * stats, and merging the results using the assumption that the histogram is
 * a reasonable random sample of the column's non-MCV population.  Note that
 * if the operator's semantics are related to the histogram ordering, this
 * might not be such a great assumption; other functions such as
 * scalarineqsel() are probably a better match in such cases.
 *
 * Otherwise, fall back to the default selectivity provided by the caller.
 */
double
generic_restriction_selectivity(PlannerInfo *root, Oid oproid, Oid collation,
								List *args, int varRelid,
								double default_selectivity)
{
	double		selec;
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;

	/*
	 * 如果表达式不是 变量 OP 某值 或 某值 OP 变量 的形式，
	 * 则放弃并返回默认估计值。
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		return default_selectivity;

	/*
	 * 如果另一侧是一个 NULL 常量，假定操作符是严格的并
	 * 返回零，即操作符永远不会返回 TRUE。
	 */
	if (IsA(other, Const) &&
		((Const *) other)->constisnull)
	{
		ReleaseVariableStats(vardata);
		return 0.0;
	}

	if (IsA(other, Const))
	{
		/* 变量正在与一个已知的非 NULL 常量进行比较 */
		Datum		constval = ((Const *) other)->constvalue;
		FmgrInfo	opproc;
		double		mcvsum;
		double		mcvsel;
		double		nullfrac;
		int			hist_size;

		fmgr_info(get_opcode(oproid), &opproc);

		/*
		 * 计算该列最常见值（MCV）的选择性。
		 */
		mcvsel = mcv_selectivity(&vardata, &opproc, collation,
								 constval, varonleft,
								 &mcvsum);

		/*
		 * 如果直方图足够大，则查看其中有多大比例与查询匹配，
		 * 并假定该比例对非 MCV 群体具有代表性。否则对非 MCV
		 * 群体使用默认选择性。
		 */
		selec = histogram_selectivity(&vardata, &opproc, collation,
									  constval, varonleft,
									  10, 1, &hist_size);
		if (selec < 0)
		{
			/* 没有，退回默认 */
			selec = default_selectivity;
		}
		else if (hist_size < 100)
		{
			/*
			 * 对于 10 到 100 之间的直方图大小，我们将直方图
			 * 与默认选择性结合起来，随着大小增大而越来越
			 * 信任直方图。
			 */
			double		hist_weight = hist_size / 100.0;

			selec = selec * hist_weight +
				default_selectivity * (1.0 - hist_weight);
		}

		/* 无论如何，不要相信极端偏小或偏大的估计值。 */
		if (selec < 0.0001)
			selec = 0.0001;
		else if (selec > 0.9999)
			selec = 0.9999;

		/* 不要忘记把 NULL 计算在内。 */
		if (HeapTupleIsValid(vardata.statsTuple))
			nullfrac = ((Form_pg_statistic) GETSTRUCT(vardata.statsTuple))->stanullfrac;
		else
			nullfrac = 0.0;

		/*
		 * 现在合并来自 MCV 与直方图计算的结果，
		 * 注意直方图只覆盖未列入 MCV 的非空值。
		 */
		selec *= 1.0 - nullfrac - mcvsum;
		selec += mcvsel;
	}
	else
	{
		/* Comparison value is not constant, so we can't do anything */
		selec = default_selectivity;
	}

	ReleaseVariableStats(vardata);

	/* 结果应在 [0,1] 范围内，但仍需确保…… */
	CLAMP_PROBABILITY(selec);

	return selec;
}

/*
 *	ineq_histogram_selectivity	- 为 scalarineqsel 检查直方图
 *
 * 确定变量的直方图群体中满足不等式条件（即
 * VAR <（或 <=、>、>=）CONST）的部分所占比例。
 * isgt 与 iseq 标志用于区分四种情况中哪一种适用。
 *
 * 虽然 opproc 可以从操作符 OID 查到，但常见的调用方
 * 也需要单独调用它，因此我们让调用方同时传入两者。
 *
 * 如果没有直方图则返回 -1（有效结果总是 >= 0）。
 *
 * 注意，该结果既忽略了最常见值（MCV，若有）也忽略了
 * NULL 条目。调用方应将此结果与针对列群体中那些
 * 部分的统计信息结合起来使用。
 *
 * 本函数被导出，以便其它一些估计函数可以使用它。
 */
double
ineq_histogram_selectivity(PlannerInfo *root,
						   VariableStatData *vardata,
						   Oid opoid, FmgrInfo *opproc, bool isgt, bool iseq,
						   Oid collation,
						   Datum constval, Oid consttype)
{
	double		hist_selec;
	AttStatsSlot sslot;

	hist_selec = -1.0;

	/*
	 * 将来某天，ANALYZE 可能会为每个关系/属性存储多个
	 * 直方图，对应于为列类型定义的多种可能的排序顺序。
	 * 目前我们只知道有一个，因此直接抓取它并看是否
	 * 与查询匹配即可。
	 *
	 * 注意我们不能用 opoid 作为查找参数；pg_statistic 中
	 * 出现的 staop 是相关的 '<' 操作符，但我们手里可能
	 * 是其它不等式操作符，例如 '>='。（即使 opoid 是
	 * '<' 操作符，它也可能是跨类型的。）因此我们必须
	 * 使用 comparison_ops_are_compatible() 来判断操作符
	 * 是否匹配。
	 */
	if (HeapTupleIsValid(vardata->statsTuple) &&
		statistic_proc_security_check(vardata, opproc->fn_oid) &&
		get_attstatsslot(&sslot, vardata->statsTuple,
						 STATISTIC_KIND_HISTOGRAM, InvalidOid,
						 ATTSTATSSLOT_VALUES))
	{
		if (sslot.nvalues > 1 &&
			sslot.stacoll == collation &&
			comparison_ops_are_compatible(sslot.staop, opoid))
		{
			/*
			 * 使用二分查找找到目标位置，即包含比较值的直方图
			 * 区间（bin）的右端——也就是比较操作符成功（isgt 时）
			 * 或失败（!isgt 时）的最左条目。
			 *
			 * 在此循环中，我们不关心操作符是否为 iseq；这一
			 * 细节将在下面处理。（反正我们也无法判断操作符
			 * 是否认为两个值相等。）
			 *
			 * 如果二分查找访问到直方图的首个或最后一个条目，
			 * 我们尝试用 get_actual_variable_range() 找到的真实列
			 * 最小值或最大值来替换该端点。这可以缓解由于自
			 * 上次 ANALYZE 以来最小/最大值发生变化而导致的
			 * 估计偏差。注意这可能实际上把之前不在直方图中的
			 * MCV 纳入直方图，但我们并不尝试对此进行纠正。
			 */
			double		histfrac;
			int			lobound = 0;	/* first possible slot to search */
			int			hibound = sslot.nvalues;	/* last+1 slot to search */
			bool		have_end = false;

			/*
			 * 如果只有两个直方图条目，我们会希望两者都是
			 * 最新值。（如果多于两个，我们最多只需更新其中
			 * 一个，因此这点在循环内处理。）
			 */
			if (sslot.nvalues == 2)
				have_end = get_actual_variable_range(root,
													 vardata,
													 sslot.staop,
													 collation,
													 &sslot.values[0],
													 &sslot.values[1]);

			while (lobound < hibound)
			{
				int			probe = (lobound + hibound) / 2;
				bool		ltcmp;

				/*
				 * 如果我们即将与直方图的首个或最后一个条目
				 * 进行比较，先尝试用真实的当前最小或最大值
				 * 替换它（除非上面已经这样做过）。
				 */
				if (probe == 0 && sslot.nvalues > 2)
					have_end = get_actual_variable_range(root,
														 vardata,
														 sslot.staop,
														 collation,
														 &sslot.values[0],
														 NULL);
				else if (probe == sslot.nvalues - 1 && sslot.nvalues > 2)
					have_end = get_actual_variable_range(root,
														 vardata,
														 sslot.staop,
														 collation,
														 NULL,
														 &sslot.values[probe]);

				ltcmp = DatumGetBool(FunctionCall2Coll(opproc,
													   collation,
													   sslot.values[probe],
													   constval));
				if (isgt)
					ltcmp = !ltcmp;
				if (ltcmp)
					lobound = probe + 1;
				else
					hibound = probe;
			}

			if (lobound <= 0)
			{
				/*
				 * 常量低于直方图下边界。更准确地说，我们发现
				 * 直方图中没有任何条目满足该不等式条件
				 * （!isgt 时），或者它们全部满足（isgt 时）。
				 * 我们估计整张表也是如此，因此把 histfrac 设为
				 * 0.0（若是 isgt，下面会翻转为 1.0）。
				 */
				histfrac = 0.0;
			}
			else if (lobound >= sslot.nvalues)
			{
				/*
				 * 相反的情况：常量高于直方图上边界。
				 */
				histfrac = 1.0;
			}
			else
			{
				/* 我们有 values[i-1] <= constant <= values[i]。 */
				int			i = lobound;
				double		eq_selec = 0;
				double		val,
							high,
							low;
				double		binfrac;

				/*
				 * 在下面需要用到的情况下，先估算 "x = constval"
				 * 的选择性。我们使用的计算方式类似于
				 * var_eq_const() 对非常见值（非 MCV）常量的处理，
				 * 即估计所有不同的非 MCV 值出现频率相同。
				 * 但乘以 "1.0 - sumcommon - nullfrac" 这一步会由
				 * 调用方完成，因此我们这里不做。也正因如此，
				 * 我们无法参照最不常见的 MCV 来钳制该估计，
				 * 否则结果会偏小。
				 *
				 * 注意：由于这实际上是假定 constval 不是 MCV，
				 * 若 constval 事实上就是 MCV，则逻辑上值得怀疑。
				 * 但我们总得对等值情况做某些修正，况且我们也
				 * 无从判断 constval 是否为 MCV，因为我们手边
				 * 没有合适的等值操作符。
				 */
				if (i == 1 || isgt == iseq)
				{
					double		otherdistinct;
					bool		isdefault;
					AttStatsSlot mcvslot;

					/* Get estimated number of distinct values */
					otherdistinct = get_variable_numdistinct(vardata,
															 &isdefault);

					/* 减去已知 MCV 的数量 */
					if (get_attstatsslot(&mcvslot, vardata->statsTuple,
										 STATISTIC_KIND_MCV, InvalidOid,
										 ATTSTATSSLOT_NUMBERS))
					{
						otherdistinct -= mcvslot.nnumbers;
						free_attstatsslot(&mcvslot);
					}

					/* 如果结果看起来不合理，则保持 eq_selec 为 0 */
					if (otherdistinct > 1)
						eq_selec = 1.0 / otherdistinct;
				}

				/*
				 * 将常量以及两个最接近的区间边界值转换到
				 * 统一的比较标尺上，并在该区间内做线性插值。
				 */
				if (convert_to_scalar(constval, consttype, collation,
									  &val,
									  sslot.values[i - 1], sslot.values[i],
									  vardata->vartype,
									  &low, &high))
				{
					if (high <= low)
					{
						/* 如果区间边界看似相同，则如此处理 */
						binfrac = 0.5;
					}
					else if (val <= low)
						binfrac = 0.0;
					else if (val >= high)
						binfrac = 1.0;
					else
					{
						binfrac = (val - low) / (high - low);

						/*
						 * 注意除法可能产生 NaN 或 Infinity 的
						 * 可能性。即便有了前面的检查，这种情况仍可能
						 * 发生，例如当 "low" 为 -Infinity 时。
						 */
						if (isnan(binfrac) ||
							binfrac < 0.0 || binfrac > 1.0)
							binfrac = 0.5;
					}
				}
				else
				{
					/*
					 * 理想情况下我们在此应报错，理由是对于给定的
					 * 操作符，除非我们能处理其操作数类型，否则不应
					 * 把 scalarXXsel 注册为其选择性函数。但当前
					 * 各种各样的代码都在调用 scalarXXsel，因此在
					 * 这个问题被修复之前，先给出一个默认估计。
					 */
					binfrac = 0.5;
				}

				/*
				 * 现在计算直方图所代表的值整体的选择性。
				 * 在常量之下有 i-1 个完整的区间，以及
				 * binfrac 个部分区间。
				 */
				histfrac = (double) (i - 1) + binfrac;
				histfrac /= (double) (sslot.nvalues - 1);

				/*
				 * 此时，histfrac 是直方图所代表的群体中满足
				 * "x <= constval" 的部分比例的估计值。颇为奇妙的是，
				 * 只要 convert_to_scalar() 给出合理的结果，无论我们
				 * 用哪个操作符进行探测，这句话都成立。如果探测
				 * 常量等于某个直方图条目，那么用 "<" 或 ">=" 探测时
				 * 我们会考虑该条目左侧的区间，用 "<=" 或 ">" 探测时
				 * 则会考虑右侧的区间；但 binfrac 在第一种情况下会
				 * 得出 1.0，在第二种情况下得出 0.0，两种情况下得到的
				 * histfrac 相同。对于位于直方图条目之间的探测常量，
				 * 用任何操作符都会找到相同的区间并得到相同的估计。
				 *
				 * 该估计对应的是 "x <= constval" 而非 "x < constval"，
				 * 原因在于 ANALYZE 构建直方图的方式：每个条目实际上
				 * 是其采样桶中最右边的值。因此，恰好为
				 * 1/(histogram_size-1) 整数倍的那些选择性值，应理解为
				 * 包含某个直方图条目及其左侧所有内容的估计。
				 *
				 * 然而，这一点对第一个直方图条目并不成立，因为
				 * 它必然是其采样桶中最左边的值。这意味着第一个
				 * 直方图区间比其余的略窄，窄的量恰好等于 eq_selec。
				 * 换句话说，我们希望把 "x <= 最左值" 估计为 eq_selec
				 * 而非零。因此，如果处理的是第一个区间（i==1），
				 * 则在对其余部分做线性调整的同时重新缩放，使这一点
				 * 成立。
				 */
				if (i == 1)
					histfrac += eq_selec * (1.0 - binfrac);

				/*
				 * 如果我们要估计的是 "<=" 或 ">"，那么 "x <= constval"
				 * 正合适；但如果估计的是 "<" 或 ">=",我们此刻
				 * 需要把估计值减去 eq_selec。
				 */
				if (isgt == iseq)
					histfrac -= eq_selec;
			}

			/*
			 * Now the estimate is finished for "<" and "<=" cases.  If we are
			 * estimating for ">" or ">=", flip it.
			 */
			hist_selec = isgt ? (1.0 - histfrac) : histfrac;

			/*
			 * The histogram boundaries are only approximate to begin with,
			 * and may well be out of date anyway.  Therefore, don't believe
			 * extremely small or large selectivity estimates --- unless we
			 * got actual current endpoint values from the table, in which
			 * case just do the usual sanity clamp.  Somewhat arbitrarily, we
			 * set the cutoff for other cases at a hundredth of the histogram
			 * resolution.
			 */
			if (have_end)
				CLAMP_PROBABILITY(hist_selec);
			else
			{
				double		cutoff = 0.01 / (double) (sslot.nvalues - 1);

				if (hist_selec < cutoff)
					hist_selec = cutoff;
				else if (hist_selec > 1.0 - cutoff)
					hist_selec = 1.0 - cutoff;
			}
		}
		else if (sslot.nvalues > 1)
		{
		/*
		 * 如果到了这里，说明我们有直方图，但它的排序方式
		 * 并非我们所期望。做一次暴力搜索，看看有多少条目
		 * 满足比较条件，并把该比例当作我们的估计。
		 * （这与 histogram_selectivity 的内层循环相同；也许
		 * 可以共享代码？）
		 */
			LOCAL_FCINFO(fcinfo, 2);
			int			nmatch = 0;

			InitFunctionCallInfoData(*fcinfo, opproc, 2, collation,
									 NULL, NULL);
			fcinfo->args[0].isnull = false;
			fcinfo->args[1].isnull = false;
			fcinfo->args[1].value = constval;
			for (int i = 0; i < sslot.nvalues; i++)
			{
				Datum		fresult;

				fcinfo->args[0].value = sslot.values[i];
				fcinfo->isnull = false;
				fresult = FunctionCallInvoke(fcinfo);
				if (!fcinfo->isnull && DatumGetBool(fresult))
					nmatch++;
			}
			hist_selec = ((double) nmatch) / ((double) sslot.nvalues);

		/*
		 * 同上，钳制到直方图分辨率的百分之一。这种情形
		 * 必定比正常情形更不可信，因此我们不应相信
		 * 精确为 0 或 1 的选择性。（也许此情形下的
		 * 钳制应更严格？）
		 */
			{
				double		cutoff = 0.01 / (double) (sslot.nvalues - 1);

				if (hist_selec < cutoff)
					hist_selec = cutoff;
				else if (hist_selec > 1.0 - cutoff)
					hist_selec = 1.0 - cutoff;
			}
		}

		free_attstatsslot(&sslot);
	}

	return hist_selec;
}

/*
 * Common wrapper function for the selectivity estimators that simply
 * invoke scalarineqsel().
 */
static Datum
scalarineqsel_wrapper(PG_FUNCTION_ARGS, bool isgt, bool iseq)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	Oid			collation = PG_GET_COLLATION();
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;
	Datum		constval;
	Oid			consttype;
	double		selec;

	/*
	 * 如果表达式不是 变量 op 某值 或 某值 op 变量 的形式，
	 * 则放弃并返回默认估计值。
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);

	/*
	 * 如果另一侧也不是常量，同样无法做任何有意义的估计。
	 */
	if (!IsA(other, Const))
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
	}

	/*
	 * If the constant is NULL, assume operator is strict and return zero, ie,
	 * operator will never return TRUE.
	 */
	if (((Const *) other)->constisnull)
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(0.0);
	}
	constval = ((Const *) other)->constvalue;
	consttype = ((Const *) other)->consttype;

	/*
	 * Force the var to be on the left to simplify logic in scalarineqsel.
	 */
	if (!varonleft)
	{
		operator = get_commutator(operator);
		if (!operator)
		{
			/* 使用默认选择性（或者我们是否应该改为报错？） */
			ReleaseVariableStats(vardata);
			PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
		}
		isgt = !isgt;
	}

	/* The rest of the work is done by scalarineqsel(). */
	selec = scalarineqsel(root, operator, isgt, iseq, collation,
						  &vardata, constval, consttype);

	ReleaseVariableStats(vardata);

	PG_RETURN_FLOAT8((float8) selec);
}

/*
 *		scalarltsel		- Selectivity of "<" for scalars.
 */
Datum
scalarltsel(PG_FUNCTION_ARGS)
{
	return scalarineqsel_wrapper(fcinfo, false, false);
}

/*
 *		scalarlesel		- 标量类型 "<=" 的选择性。
 */
Datum
scalarlesel(PG_FUNCTION_ARGS)
{
	return scalarineqsel_wrapper(fcinfo, false, true);
}

/*
 *		scalargtsel		- 标量类型 ">" 的选择性。
 */
Datum
scalargtsel(PG_FUNCTION_ARGS)
{
	return scalarineqsel_wrapper(fcinfo, true, false);
}

/*
 *		scalargesel		- 标量类型 ">=" 的选择性。
 */
Datum
scalargesel(PG_FUNCTION_ARGS)
{
	return scalarineqsel_wrapper(fcinfo, true, true);
}

/*
 *		boolvarsel		- 布尔变量的选择性。
 *
 * 实际上它可以作用于任何布尔值表达式。如果它只涉及
 * 指定关系的 Var，并且存在关于该 Var 或表达式的统计
 * 信息（后者在表达式被索引时有可能存在），那么我们会
 * 给出一个真实的估计；否则就只是默认估计。
 */
Selectivity
boolvarsel(PlannerInfo *root, Node *arg, int varRelid)
{
	VariableStatData vardata;
	double		selec;

	examine_variable(root, arg, varRelid, &vardata);
	if (HeapTupleIsValid(vardata.statsTuple))
	{
		/*
		 * 布尔变量 V 等价于子句 V = 't'，因此我们按
		 * 拥有该子句来计算选择性。
		 */
		selec = var_eq_const(&vardata, BooleanEqualOperator, InvalidOid,
							 BoolGetDatum(true), false, true, false);
	}
	else
	{
		/* Otherwise, the default estimate is 0.5 */
		selec = 0.5;
	}
	ReleaseVariableStats(vardata);
	return selec;
}

/*
 *		booltestsel		- BooleanTest 节点的选择性。
 */
Selectivity
booltestsel(PlannerInfo *root, BoolTestType booltesttype, Node *arg,
			int varRelid, JoinType jointype, SpecialJoinInfo *sjinfo)
{
	VariableStatData vardata;
	double		selec;

	examine_variable(root, arg, varRelid, &vardata);

	if (HeapTupleIsValid(vardata.statsTuple))
	{
		Form_pg_statistic stats;
		double		freq_null;
		AttStatsSlot sslot;

		stats = (Form_pg_statistic) GETSTRUCT(vardata.statsTuple);
		freq_null = stats->stanullfrac;

		if (get_attstatsslot(&sslot, vardata.statsTuple,
							 STATISTIC_KIND_MCV, InvalidOid,
							 ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS)
			&& sslot.nnumbers > 0)
		{
			double		freq_true;
			double		freq_false;

			/*
			 * Get first MCV frequency and derive frequency for true.
			 */
			if (DatumGetBool(sslot.values[0]))
				freq_true = sslot.numbers[0];
			else
				freq_true = 1.0 - sslot.numbers[0] - freq_null;

			/*
			 * 接着推导 false 的频率。然后视情况利用这些
			 * 频率推导出每种情况的频率。
			 */
			freq_false = 1.0 - freq_true - freq_null;

			switch (booltesttype)
			{
				case IS_UNKNOWN:
					/* 只选择 NULL 值 */
					selec = freq_null;
					break;
				case IS_NOT_UNKNOWN:
					/* select non-NULL values */
					selec = 1.0 - freq_null;
					break;
				case IS_TRUE:
					/* 只选择 TRUE 值 */
					selec = freq_true;
					break;
				case IS_NOT_TRUE:
					/* 选择非 TRUE 值 */
					selec = 1.0 - freq_true;
					break;
				case IS_FALSE:
					/* 只选择 FALSE 值 */
					selec = freq_false;
					break;
				case IS_NOT_FALSE:
					/* 选择非 FALSE 值 */
					selec = 1.0 - freq_false;
					break;
				default:
					elog(ERROR, "unrecognized booltesttype: %d",
						 (int) booltesttype);
					selec = 0.0;	/* 避免编译器告警 */
					break;
			}

			free_attstatsslot(&sslot);
		}
		else
		{
			/*
			 * No most-common-value info available. Still have null fraction
			 * information, so use it for IS [NOT] UNKNOWN. Otherwise adjust
			 * for null fraction and assume a 50-50 split of TRUE and FALSE.
			 */
			switch (booltesttype)
			{
				case IS_UNKNOWN:
					/* 只选择 NULL 值 */
					selec = freq_null;
					break;
				case IS_NOT_UNKNOWN:
					/* select non-NULL values */
					selec = 1.0 - freq_null;
					break;
				case IS_TRUE:
				case IS_FALSE:
					/* 假定选择了非 NULL 值的一半 */
					selec = (1.0 - freq_null) / 2.0;
					break;
				case IS_NOT_TRUE:
				case IS_NOT_FALSE:
					/* Assume we select NULLs plus half of the non-NULLs */
					/* 等价于 freq_null + (1.0 - freq_null) / 2.0 */
					selec = (freq_null + 1.0) / 2.0;
					break;
				default:
					elog(ERROR, "unrecognized booltesttype: %d",
						 (int) booltesttype);
					selec = 0.0;	/* 避免编译器告警 */
					break;
			}
		}
	}
	else
	{
		/*
		 * If we can't get variable statistics for the argument, perhaps
		 * clause_selectivity can do something with it.  We ignore the
		 * possibility of a NULL value when using clause_selectivity, and just
		 * assume the value is either TRUE or FALSE.
		 */
		switch (booltesttype)
		{
			case IS_UNKNOWN:
				selec = DEFAULT_UNK_SEL;
				break;
			case IS_NOT_UNKNOWN:
				selec = DEFAULT_NOT_UNK_SEL;
				break;
			case IS_TRUE:
			case IS_NOT_FALSE:
				selec = (double) clause_selectivity(root, arg,
													varRelid,
													jointype, sjinfo);
				break;
			case IS_FALSE:
			case IS_NOT_TRUE:
				selec = 1.0 - (double) clause_selectivity(root, arg,
														  varRelid,
														  jointype, sjinfo);
				break;
			default:
				elog(ERROR, "unrecognized booltesttype: %d",
					 (int) booltesttype);
				selec = 0.0;	/* 避免编译器告警 */
				break;
		}
	}

	ReleaseVariableStats(vardata);

	/* 结果应在 [0,1] 范围内，但仍需确保…… */
	CLAMP_PROBABILITY(selec);

	return (Selectivity) selec;
}

/*
 *		nulltestsel		- Selectivity of NullTest Node.
 */
Selectivity
nulltestsel(PlannerInfo *root, NullTestType nulltesttype, Node *arg,
			int varRelid, JoinType jointype, SpecialJoinInfo *sjinfo)
{
	VariableStatData vardata;
	double		selec;

	examine_variable(root, arg, varRelid, &vardata);

	if (HeapTupleIsValid(vardata.statsTuple))
	{
		Form_pg_statistic stats;
		double		freq_null;

		stats = (Form_pg_statistic) GETSTRUCT(vardata.statsTuple);
		freq_null = stats->stanullfrac;

		switch (nulltesttype)
		{
			case IS_NULL:

				/*
				 * Use freq_null directly.
				 */
				selec = freq_null;
				break;
			case IS_NOT_NULL:

				/*
				 * 选择非未知（非 NULL）的值。根据 freq_null
				 * 计算。
				 */
				selec = 1.0 - freq_null;
				break;
			default:
				elog(ERROR, "unrecognized nulltesttype: %d",
					 (int) nulltesttype);
				return (Selectivity) 0; /* 避免编译器告警 */
		}
	}
	else if (vardata.var && IsA(vardata.var, Var) &&
			 ((Var *) vardata.var)->varattno < 0)
	{
		/*
		 * 系统列没有统计信息，但我们知道它们
		 * 永不为 NULL。
		 */
		selec = (nulltesttype == IS_NULL) ? 0.0 : 1.0;
	}
	else
	{
		/*
		 * 没有可用的 ANALYZE 统计信息，于是做一个猜测
		 */
		switch (nulltesttype)
		{
			case IS_NULL:
				selec = DEFAULT_UNK_SEL;
				break;
			case IS_NOT_NULL:
				selec = DEFAULT_NOT_UNK_SEL;
				break;
			default:
				elog(ERROR, "unrecognized nulltesttype: %d",
					 (int) nulltesttype);
				return (Selectivity) 0; /* 避免编译器告警 */
		}
	}

	ReleaseVariableStats(vardata);

	/* 结果应在 [0,1] 范围内，但仍需确保…… */
	CLAMP_PROBABILITY(selec);

	return (Selectivity) selec;
}

/*
 * strip_array_coercion - 从数组表达式中剥离二进制兼容的重标记
 *
 * 对于数组值，解析器通常会生成 ArrayCoerceExpr 转换，
 * 但 RelabelType 似乎也有可能出现。此外，规划器
 * 目前并不急于折叠堆叠的 ArrayCoerceExpr 节点，
 * 因此我们需要准备好处理多于一层的情况。
 */
static Node *
strip_array_coercion(Node *node)
{
	for (;;)
	{
		if (node && IsA(node, ArrayCoerceExpr))
		{
			ArrayCoerceExpr *acoerce = (ArrayCoerceExpr *) node;

			/*
			 * If the per-element expression is just a RelabelType on top of
			 * CaseTestExpr, then we know it's a binary-compatible relabeling.
			 */
			if (IsA(acoerce->elemexpr, RelabelType) &&
				IsA(((RelabelType *) acoerce->elemexpr)->arg, CaseTestExpr))
				node = (Node *) acoerce->arg;
			else
				break;
		}
		else if (node && IsA(node, RelabelType))
		{
			/* 我们其实不期望出现这种情况，但也不妨处理 */
			node = (Node *) ((RelabelType *) node)->arg;
		}
		else
			break;
	}
	return node;
}

/*
 *		scalararraysel		- Selectivity of ScalarArrayOpExpr Node.
 */
Selectivity
scalararraysel(PlannerInfo *root,
			   ScalarArrayOpExpr *clause,
			   bool is_join_clause,
			   int varRelid,
			   JoinType jointype,
			   SpecialJoinInfo *sjinfo)
{
	Oid			operator = clause->opno;
	bool		useOr = clause->useOr;
	bool		isEquality = false;
	bool		isInequality = false;
	Node	   *leftop;
	Node	   *rightop;
	Oid			nominal_element_type;
	Oid			nominal_element_collation;
	TypeCacheEntry *typentry;
	RegProcedure oprsel;
	FmgrInfo	oprselproc;
	Selectivity s1;
	Selectivity s1disjoint;

	/* First, deconstruct the expression */
	Assert(list_length(clause->args) == 2);
	leftop = (Node *) linitial(clause->args);
	rightop = (Node *) lsecond(clause->args);

	/* 积极地将两侧都化简为常量 */
	leftop = estimate_expression_value(root, leftop);
	rightop = estimate_expression_value(root, rightop);

	/* 获取 rightop 的标称（经过重标记后的）元素类型 */
	nominal_element_type = get_base_element_type(exprType(rightop));
	if (!OidIsValid(nominal_element_type))
		return (Selectivity) 0.5;	/* 大概不会发生 */
	/* 同时获取标称排序规则，用于生成常量 */
	nominal_element_collation = exprCollation(rightop);

	/* look through any binary-compatible relabeling of rightop */
	rightop = strip_array_coercion(rightop);

	/*
	 * 判断该操作符是否为数组元素类型的默认等值或
	 * 不等值操作符。
	 */
	typentry = lookup_type_cache(nominal_element_type, TYPECACHE_EQ_OPR);
	if (OidIsValid(typentry->eq_opr))
	{
		if (operator == typentry->eq_opr)
			isEquality = true;
		else if (get_negator(operator) == typentry->eq_opr)
			isInequality = true;
	}

	/*
	 * 如果是等值或不等值，我们或许能把它估计为
	 * 一种数组包含（containment）的形式；例如
	 * "const = ANY(column)" 可以当作 "ARRAY[const] <@ column"
	 * 来处理。scalararraysel_containment 会尝试这种做法，
	 * 成功时返回选择性估计，否则返回 -1。
	 */
	if ((isEquality || isInequality) && !is_join_clause)
	{
		s1 = scalararraysel_containment(root, leftop, rightop,
										nominal_element_type,
										isEquality, useOr, varRelid);
		if (s1 >= 0.0)
			return s1;
	}

	/*
	 * 查找底层操作符的选择性估计器。如果没有则放弃。
	 */
	if (is_join_clause)
		oprsel = get_oprjoin(operator);
	else
		oprsel = get_oprrest(operator);
	if (!oprsel)
		return (Selectivity) 0.5;
	fmgr_info(oprsel, &oprselproc);

	/*
	 * 在上面的数组包含检查中，我们必须只在操作符是元素类型的
	 * 默认 btree 等值操作符（或其求反符）时才相信它是等值或
	 * 不等值，因为数组包含会使用的正是这些操作符。但在接下来的
	 * 处理中，我们可以放宽一点，也相信任何使用 eqsel() 或
	 * neqsel() 作为选择性估计器的操作符表现得像等值或
	 * 不等值。
	 */
	if (oprsel == F_EQSEL || oprsel == F_EQJOINSEL)
		isEquality = true;
	else if (oprsel == F_NEQSEL || oprsel == F_NEQJOINSEL)
		isInequality = true;

	/*
	 * 我们考虑三种情况：
	 *
	 * 1. rightop 是一个数组常量：拆解该数组，对数组的每个元素
	 * 应用操作符的选择性函数，并以 clausesel.c 处理
	 * AND/OR 组合相同的方式合并结果。
	 *
	 * 2. rightop 是一个 ARRAY[] 构造：对 ARRAY[] 构造的
	 * 每个元素应用操作符的选择性函数，并合并。
	 *
	 * 3. 否则，做一个猜测……
	 */
	if (rightop && IsA(rightop, Const))
	{
		Datum		arraydatum = ((Const *) rightop)->constvalue;
		bool		arrayisnull = ((Const *) rightop)->constisnull;
		ArrayType  *arrayval;
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;
		int			num_elems;
		Datum	   *elem_values;
		bool	   *elem_nulls;
		int			i;

		if (arrayisnull)		/* 若数组为 NULL，该条件不可能成立 */
			return (Selectivity) 0.0;
		arrayval = DatumGetArrayTypeP(arraydatum);
		get_typlenbyvalalign(ARR_ELEMTYPE(arrayval),
							 &elmlen, &elmbyval, &elmalign);
		deconstruct_array(arrayval,
						  ARR_ELEMTYPE(arrayval),
						  elmlen, elmbyval, elmalign,
						  &elem_values, &elem_nulls, &num_elems);

		/*
		 * 对于通用操作符，我们假设每个数组元素的成功概率
		 * 是相互独立的。但对于 "= ANY" 或 "<> ALL"，如果
		 * 数组元素是互不相同的（通常正是如此），那么这些
		 * 概率是不相交的，我们只需将它们相加即可。
		 *
		 * 如果我们真要较真，会尝试确认所有元素都互不相同，
		 * 但那代价高昂，似乎不值得花费这些周期；那无异于
		 * 惩罚写得好的查询而偏袒写得差的查询。不过，我们
		 * 确实做了一点自我保护：检查不相交假设是否会导出
		 * 一个不可能（超出范围）的概率；如果是，则退回到
		 * 正常计算。
		 */
		s1 = s1disjoint = (useOr ? 0.0 : 1.0);

		for (i = 0; i < num_elems; i++)
		{
			List	   *args;
			Selectivity s2;

			args = list_make2(leftop,
							  makeConst(nominal_element_type,
										-1,
										nominal_element_collation,
										elmlen,
										elem_values[i],
										elem_nulls[i],
										elmbyval));
			if (is_join_clause)
				s2 = DatumGetFloat8(FunctionCall5Coll(&oprselproc,
													  clause->inputcollid,
													  PointerGetDatum(root),
													  ObjectIdGetDatum(operator),
													  PointerGetDatum(args),
													  Int16GetDatum(jointype),
													  PointerGetDatum(sjinfo)));
			else
				s2 = DatumGetFloat8(FunctionCall4Coll(&oprselproc,
													  clause->inputcollid,
													  PointerGetDatum(root),
													  ObjectIdGetDatum(operator),
													  PointerGetDatum(args),
													  Int32GetDatum(varRelid)));

			if (useOr)
			{
				s1 = s1 + s2 - s1 * s2;
				if (isEquality)
					s1disjoint += s2;
			}
			else
			{
				s1 = s1 * s2;
				if (isInequality)
					s1disjoint += s2 - 1.0;
			}
		}

		/* 若在范围内则采用不相交概率估计 */
		if ((useOr ? isEquality : isInequality) &&
			s1disjoint >= 0.0 && s1disjoint <= 1.0)
			s1 = s1disjoint;
	}
	else if (rightop && IsA(rightop, ArrayExpr) &&
			 !((ArrayExpr *) rightop)->multidims)
	{
		ArrayExpr  *arrayexpr = (ArrayExpr *) rightop;
		int16		elmlen;
		bool		elmbyval;
		ListCell   *l;

		get_typlenbyval(arrayexpr->element_typeid,
						&elmlen, &elmbyval);

		/*
		 * 在这里我们也使用不相交概率的假设，尽管如果元素
		 * 不全是常量（它们不会全为常量，否则常量折叠
		 * 早就把 ArrayExpr 化简成 Const 了），那么出现
		 * 相同数组元素的概率会高不少。在这条路径上，
		 * 对 s1disjoint 估计做合理性检查至关重要。
		 */
		s1 = s1disjoint = (useOr ? 0.0 : 1.0);

		foreach(l, arrayexpr->elements)
		{
			Node	   *elem = (Node *) lfirst(l);
			List	   *args;
			Selectivity s2;

			/*
			 * 理论上，如果 elem 不是 nominal_element_type 类型，
			 * 我们应该插入一个 RelabelType，但任何操作符估计
			 * 函数似乎都不太可能真的在意……
			 */
			args = list_make2(leftop, elem);
			if (is_join_clause)
				s2 = DatumGetFloat8(FunctionCall5Coll(&oprselproc,
													  clause->inputcollid,
													  PointerGetDatum(root),
													  ObjectIdGetDatum(operator),
													  PointerGetDatum(args),
													  Int16GetDatum(jointype),
													  PointerGetDatum(sjinfo)));
			else
				s2 = DatumGetFloat8(FunctionCall4Coll(&oprselproc,
													  clause->inputcollid,
													  PointerGetDatum(root),
													  ObjectIdGetDatum(operator),
													  PointerGetDatum(args),
													  Int32GetDatum(varRelid)));

			if (useOr)
			{
				s1 = s1 + s2 - s1 * s2;
				if (isEquality)
					s1disjoint += s2;
			}
			else
			{
				s1 = s1 * s2;
				if (isInequality)
					s1disjoint += s2 - 1.0;
			}
		}

		/* 若在范围内则采用不相交概率估计 */
		if ((useOr ? isEquality : isInequality) &&
			s1disjoint >= 0.0 && s1disjoint <= 1.0)
			s1 = s1disjoint;
	}
	else
	{
		CaseTestExpr *dummyexpr;
		List	   *args;
		Selectivity s2;
		int			i;

		/*
		 * 我们需要一个虚拟的 rightop 传给操作符的选择性
		 * 例程。它几乎可以是任何看起来不像常量的东西；
		 * CaseTestExpr 是一个方便的选择。
		 */
		dummyexpr = makeNode(CaseTestExpr);
		dummyexpr->typeId = nominal_element_type;
		dummyexpr->typeMod = -1;
		dummyexpr->collation = clause->inputcollid;
		args = list_make2(leftop, dummyexpr);
		if (is_join_clause)
			s2 = DatumGetFloat8(FunctionCall5Coll(&oprselproc,
												  clause->inputcollid,
												  PointerGetDatum(root),
												  ObjectIdGetDatum(operator),
												  PointerGetDatum(args),
												  Int16GetDatum(jointype),
												  PointerGetDatum(sjinfo)));
		else
			s2 = DatumGetFloat8(FunctionCall4Coll(&oprselproc,
												  clause->inputcollid,
												  PointerGetDatum(root),
												  ObjectIdGetDatum(operator),
												  PointerGetDatum(args),
												  Int32GetDatum(varRelid)));
		s1 = useOr ? 0.0 : 1.0;

		/*
		 * 武断地假定最终数组值中有 10 个元素
		 * （另见 estimate_array_length）。这里我们不冒险
		 * 采用不相交概率的假设。
		 */
		for (i = 0; i < 10; i++)
		{
			if (useOr)
				s1 = s1 + s2 - s1 * s2;
			else
				s1 = s1 * s2;
		}
	}

	/* 结果应在 [0,1] 范围内，但仍需确保…… */
	CLAMP_PROBABILITY(s1);

	return s1;
}

/*
 * 估计某表达式所产生数组的元素个数。
 *
 * 注意：结果是整数，但我们使用 "double" 以避免
 * 溢出问题。大多数调用方反正都会把它用在 double 类型的
 * 表达式中。
 *
 * 注意：在某些代码路径中，root 可能被传入 NULL，
 * 从而导致估计稍差。
 */
double
estimate_array_length(PlannerInfo *root, Node *arrayexpr)
{
	/* look through any binary-compatible relabeling of arrayexpr */
	arrayexpr = strip_array_coercion(arrayexpr);

	if (arrayexpr && IsA(arrayexpr, Const))
	{
		Datum		arraydatum = ((Const *) arrayexpr)->constvalue;
		bool		arrayisnull = ((Const *) arrayexpr)->constisnull;
		ArrayType  *arrayval;

		if (arrayisnull)
			return 0;
		arrayval = DatumGetArrayTypeP(arraydatum);
		return ArrayGetNItems(ARR_NDIM(arrayval), ARR_DIMS(arrayval));
	}
	else if (arrayexpr && IsA(arrayexpr, ArrayExpr) &&
			 !((ArrayExpr *) arrayexpr)->multidims)
	{
		return list_length(((ArrayExpr *) arrayexpr)->elements);
	}
	else if (arrayexpr && root)
	{
		/* 看看能否找到关于它的任何统计信息 */
		VariableStatData vardata;
		AttStatsSlot sslot;
		double		nelem = 0;

		/*
		 * 对于 varno 为 0 的 Var，跳过调用 examine_variable，
		 * 因为它没有有效的关系项，会在 find_base_rel 中报错。
		 * 这种 Var 可能出现在嵌套集合操作的输出类型与
		 * 父节点期望类型不匹配时，因为 recurse_set_operations 会
		 * 用 generate_setop_tlist（varno 为 0）构建投影目标
		 * 列表，而如果所需的类型转换涉及 ArrayCoerceExpr，
		 * 我们就可能被调用到该 Var 上。
		 */
		if (IsA(arrayexpr, Var) && ((Var *) arrayexpr)->varno == 0)
			return 10;			/* 默认猜测，应与 scalararraysel 一致 */

		examine_variable(root, arrayexpr, 0, &vardata);
		if (HeapTupleIsValid(vardata.statsTuple))
		{
			/*
			 * Found stats, so use the average element count, which is stored
			 * in the last stanumbers element of the DECHIST statistics.
			 * Actually that is the average count of *distinct* elements;
			 * perhaps we should scale it up somewhat?
			 */
			if (get_attstatsslot(&sslot, vardata.statsTuple,
								 STATISTIC_KIND_DECHIST, InvalidOid,
								 ATTSTATSSLOT_NUMBERS))
			{
				if (sslot.nnumbers > 0)
					nelem = clamp_row_est(sslot.numbers[sslot.nnumbers - 1]);
				free_attstatsslot(&sslot);
			}
		}
		ReleaseVariableStats(vardata);

		if (nelem > 0)
			return nelem;
	}

	/* 否则使用默认猜测——应与 scalararraysel 一致 */
	return 10;
}

/*
 *		rowcomparesel		- Selectivity of RowCompareExpr Node.
 *
 * We estimate RowCompare selectivity by considering just the first (high
 * order) columns, which makes it equivalent to an ordinary OpExpr.  While
 * this estimate could be refined by considering additional columns, it
 * seems unlikely that we could do a lot better without multi-column
 * statistics.
 */
Selectivity
rowcomparesel(PlannerInfo *root,
			  RowCompareExpr *clause,
			  int varRelid, JoinType jointype, SpecialJoinInfo *sjinfo)
{
	Selectivity s1;
	Oid			opno = linitial_oid(clause->opnos);
	Oid			inputcollid = linitial_oid(clause->inputcollids);
	List	   *opargs;
	bool		is_join_clause;

	/* 为单一操作符构建等价的参数列表 */
	opargs = list_make2(linitial(clause->largs), linitial(clause->rargs));

	/*
	 * 判断它是否为连接子句。这应与 clausesel.c 的
	 * treat_as_join_clause() 保持一致，但我们有意只
	 * 考虑前导列而非子句的其余部分。
	 */
	if (varRelid != 0)
	{
		/*
		 * Caller is forcing restriction mode (eg, because we are examining an
		 * inner indexscan qual).
		 */
		is_join_clause = false;
	}
	else if (sjinfo == NULL)
	{
		/*
		 * 它必定是一个限制子句，因为它正在某个
		 * 扫描节点上被求值。
		 */
		is_join_clause = false;
	}
	else
	{
		/*
		 * Otherwise, it's a join if there's more than one base relation used.
		 */
		is_join_clause = (NumRelids(root, (Node *) opargs) > 1);
	}

	if (is_join_clause)
	{
		/* Estimate selectivity for a join clause. */
		s1 = join_selectivity(root, opno,
							  opargs,
							  inputcollid,
							  jointype,
							  sjinfo);
	}
	else
	{
		/* 估计限制子句的选择性。 */
		s1 = restriction_selectivity(root, opno,
									 opargs,
									 inputcollid,
									 varRelid);
	}

	return s1;
}

/*
 *		eqjoinsel		- Join selectivity of "="
 */
Datum
eqjoinsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);

#ifdef NOT_USED
	JoinType	jointype = (JoinType) PG_GETARG_INT16(3);
#endif
	SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) PG_GETARG_POINTER(4);
	Oid			collation = PG_GET_COLLATION();
	double		selec;
	double		selec_inner;
	VariableStatData vardata1;
	VariableStatData vardata2;
	double		nd1;
	double		nd2;
	bool		isdefault1;
	bool		isdefault2;
	Oid			opfuncoid;
	AttStatsSlot sslot1;
	AttStatsSlot sslot2;
	Form_pg_statistic stats1 = NULL;
	Form_pg_statistic stats2 = NULL;
	bool		have_mcvs1 = false;
	bool		have_mcvs2 = false;
	bool		get_mcv_stats;
	bool		join_is_reversed;
	RelOptInfo *inner_rel;

	get_join_variables(root, args, sjinfo,
					   &vardata1, &vardata2, &join_is_reversed);

	nd1 = get_variable_numdistinct(&vardata1, &isdefault1);
	nd2 = get_variable_numdistinct(&vardata2, &isdefault2);

	opfuncoid = get_opcode(operator);

	memset(&sslot1, 0, sizeof(sslot1));
	memset(&sslot2, 0, sizeof(sslot2));

	/*
	 * There is no use in fetching one side's MCVs if we lack MCVs for the
	 * other side, so do a quick check to verify that both stats exist.
	 */
	get_mcv_stats = (HeapTupleIsValid(vardata1.statsTuple) &&
					 HeapTupleIsValid(vardata2.statsTuple) &&
					 get_attstatsslot(&sslot1, vardata1.statsTuple,
									  STATISTIC_KIND_MCV, InvalidOid,
									  0) &&
					 get_attstatsslot(&sslot2, vardata2.statsTuple,
									  STATISTIC_KIND_MCV, InvalidOid,
									  0));

	if (HeapTupleIsValid(vardata1.statsTuple))
	{
		/* note we allow use of nullfrac regardless of security check */
		stats1 = (Form_pg_statistic) GETSTRUCT(vardata1.statsTuple);
		if (get_mcv_stats &&
			statistic_proc_security_check(&vardata1, opfuncoid))
			have_mcvs1 = get_attstatsslot(&sslot1, vardata1.statsTuple,
										  STATISTIC_KIND_MCV, InvalidOid,
										  ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS);
	}

	if (HeapTupleIsValid(vardata2.statsTuple))
	{
		/* note we allow use of nullfrac regardless of security check */
		stats2 = (Form_pg_statistic) GETSTRUCT(vardata2.statsTuple);
		if (get_mcv_stats &&
			statistic_proc_security_check(&vardata2, opfuncoid))
			have_mcvs2 = get_attstatsslot(&sslot2, vardata2.statsTuple,
										  STATISTIC_KIND_MCV, InvalidOid,
										  ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS);
	}

	/* 在所有情况下我们都需要计算内连接选择性 */
	selec_inner = eqjoinsel_inner(opfuncoid, collation,
								  &vardata1, &vardata2,
								  nd1, nd2,
								  isdefault1, isdefault2,
								  &sslot1, &sslot2,
								  stats1, stats2,
								  have_mcvs1, have_mcvs2);

	switch (sjinfo->jointype)
	{
		case JOIN_INNER:
		case JOIN_LEFT:
		case JOIN_FULL:
			selec = selec_inner;
			break;
		case JOIN_SEMI:
		case JOIN_ANTI:

			/*
			 * 查找连接的 inner 关系。min_righthand 已是足够的
			 * 信息，因为无论是 SEMI 还是 ANTI 连接，都不允许
			 * 对其 RHS 做任何重新关联，因此 righthand 永远恰好
			 * 就是那组关系。
			 */
			inner_rel = find_join_input_rel(root, sjinfo->min_righthand);

			if (!join_is_reversed)
				selec = eqjoinsel_semi(opfuncoid, collation,
									   &vardata1, &vardata2,
									   nd1, nd2,
									   isdefault1, isdefault2,
									   &sslot1, &sslot2,
									   stats1, stats2,
									   have_mcvs1, have_mcvs2,
									   inner_rel);
			else
			{
				Oid			commop = get_commutator(operator);
				Oid			commopfuncoid = OidIsValid(commop) ? get_opcode(commop) : InvalidOid;

				selec = eqjoinsel_semi(commopfuncoid, collation,
									   &vardata2, &vardata1,
									   nd2, nd1,
									   isdefault2, isdefault1,
									   &sslot2, &sslot1,
									   stats2, stats1,
									   have_mcvs2, have_mcvs1,
									   inner_rel);
			}

			/*
			 * 我们永远不应把半连接的输出估计为比相同输入关系
			 * 和连接条件下的内连接更多的行数；这显然是不可能
			 * 发生的。前者的估计为 N1 * Ssemi，而后者为
			 * N1 * N2 * Sinner，因此我们可以钳制 Ssemi <= N2 * Sinner。
			 * 这样做是值得的，因为我们在 eqjoinsel_semi 中使用的
			 * 估计规则较不可靠，尤其是在它不得不完全放弃的
			 * 情况下。
			 */
			selec = Min(selec, inner_rel->rows * selec_inner);
			break;
		default:
			/* 此处不应出现其它取值 */
			elog(ERROR, "unrecognized join type: %d",
				 (int) sjinfo->jointype);
			selec = 0;			/* 避免编译器告警 */
			break;
	}

	free_attstatsslot(&sslot1);
	free_attstatsslot(&sslot2);

	ReleaseVariableStats(vardata1);
	ReleaseVariableStats(vardata2);

	CLAMP_PROBABILITY(selec);

	PG_RETURN_FLOAT8((float8) selec);
}

/*
 * eqjoinsel_inner --- 普通内连接的 eqjoinsel
 *
 * 我们也把它用于 LEFT/FULL 外连接；目前尚不清楚
 * 在此处区分它们是否值得。
 */
static double
eqjoinsel_inner(Oid opfuncoid, Oid collation,
				VariableStatData *vardata1, VariableStatData *vardata2,
				double nd1, double nd2,
				bool isdefault1, bool isdefault2,
				AttStatsSlot *sslot1, AttStatsSlot *sslot2,
				Form_pg_statistic stats1, Form_pg_statistic stats2,
				bool have_mcvs1, bool have_mcvs2)
{
	double		selec;

	if (have_mcvs1 && have_mcvs2)
	{
		/*
		 * 两个关系都有了最常见值（MCV）列表。遍历这些列表，
		 * 看看哪些 MCV 在给定操作符下真正相互连接。这使我们
		 * 能够确定由 MCV 列表所代表的那部分关系的精确连接
		 * 选择性。我们仍须对剩余群体做估计，但在倾斜的
		 * 分布下，这能大幅提制准确性。相关动机可参见
		 * Y. Ioannidis 与 S. Christodoulakis 的论文
		 * "On the propagation of errors in the size of join results"
		 * （威斯康星大学麦迪逊分校计算机科学系技术报告
		 * 1018，1991 年 3 月，可从 ftp.cs.wisc.edu 获取）。
		 */
		LOCAL_FCINFO(fcinfo, 2);
		FmgrInfo	eqproc;
		bool	   *hasmatch1;
		bool	   *hasmatch2;
		double		nullfrac1 = stats1->stanullfrac;
		double		nullfrac2 = stats2->stanullfrac;
		double		matchprodfreq,
					matchfreq1,
					matchfreq2,
					unmatchfreq1,
					unmatchfreq2,
					otherfreq1,
					otherfreq2,
					totalsel1,
					totalsel2;
		int			i,
					nmatches;

		fmgr_info(opfuncoid, &eqproc);

		/*
		 * 通过只初始化一次 fcinfo 结构体来节省若干周期。
		 * 直接使用 FunctionCallInvoke 也能避免 eqproc 返回
		 * NULL 时失败，尽管等值函数本不应返回 NULL。
		 */
		InitFunctionCallInfoData(*fcinfo, &eqproc, 2, collation,
								 NULL, NULL);
		fcinfo->args[0].isnull = false;
		fcinfo->args[1].isnull = false;

		hasmatch1 = (bool *) palloc0(sslot1->nvalues * sizeof(bool));
		hasmatch2 = (bool *) palloc0(sslot2->nvalues * sizeof(bool));

		/*
		 * 注意我们假设每个 MCV 最多只匹配另一个 MCV 列表中的
		 * 一个成员。如果操作符并非真正的等值，可能会出现
		 * 多个匹配——但我们不去寻找它们，既是为了速度，
		 * 也因为那样数学上会无法自洽……
		 */
		matchprodfreq = 0.0;
		nmatches = 0;
		for (i = 0; i < sslot1->nvalues; i++)
		{
			int			j;

			fcinfo->args[0].value = sslot1->values[i];

			for (j = 0; j < sslot2->nvalues; j++)
			{
				Datum		fresult;

				if (hasmatch2[j])
					continue;
				fcinfo->args[1].value = sslot2->values[j];
				fcinfo->isnull = false;
				fresult = FunctionCallInvoke(fcinfo);
				if (!fcinfo->isnull && DatumGetBool(fresult))
				{
					hasmatch1[i] = hasmatch2[j] = true;
					matchprodfreq += sslot1->numbers[i] * sslot2->numbers[j];
					nmatches++;
					break;
				}
			}
		}
		CLAMP_PROBABILITY(matchprodfreq);
		/* 累加已匹配与未匹配 MCV 的频率 */
		matchfreq1 = unmatchfreq1 = 0.0;
		for (i = 0; i < sslot1->nvalues; i++)
		{
			if (hasmatch1[i])
				matchfreq1 += sslot1->numbers[i];
			else
				unmatchfreq1 += sslot1->numbers[i];
		}
		CLAMP_PROBABILITY(matchfreq1);
		CLAMP_PROBABILITY(unmatchfreq1);
		matchfreq2 = unmatchfreq2 = 0.0;
		for (i = 0; i < sslot2->nvalues; i++)
		{
			if (hasmatch2[i])
				matchfreq2 += sslot2->numbers[i];
			else
				unmatchfreq2 += sslot2->numbers[i];
		}
		CLAMP_PROBABILITY(matchfreq2);
		CLAMP_PROBABILITY(unmatchfreq2);
		pfree(hasmatch1);
		pfree(hasmatch2);

		/*
		 * 计算不在 MCV 列表中的非 NULL 值的总频率。
		 */
		otherfreq1 = 1.0 - nullfrac1 - matchfreq1 - unmatchfreq1;
		otherfreq2 = 1.0 - nullfrac2 - matchfreq2 - unmatchfreq2;
		CLAMP_PROBABILITY(otherfreq1);
		CLAMP_PROBABILITY(otherfreq2);

		/*
		 * 我们可以从关系 1 的角度估计总选择性：已知已匹配
		 * MCV 的选择性，加上假定与关系 2 的非 MCV 群体
		 * 中随机成员匹配的未匹配 MCV，再加上假定与关系 2
		 * 的未匹配 MCV 及非 MCV 值中随机成员匹配的非 MCV 值。
		 */
		totalsel1 = matchprodfreq;
		if (nd2 > sslot2->nvalues)
			totalsel1 += unmatchfreq1 * otherfreq2 / (nd2 - sslot2->nvalues);
		if (nd2 > nmatches)
			totalsel1 += otherfreq1 * (otherfreq2 + unmatchfreq2) /
				(nd2 - nmatches);
		/* Same estimate from the point of view of relation 2. */
		totalsel2 = matchprodfreq;
		if (nd1 > sslot1->nvalues)
			totalsel2 += unmatchfreq2 * otherfreq1 / (nd1 - sslot1->nvalues);
		if (nd1 > nmatches)
			totalsel2 += otherfreq2 * (otherfreq1 + unmatchfreq1) /
				(nd1 - nmatches);

		/*
		 * 取两个估计中较小的一个。这可以用与下面无统计情形
		 * 基本相同的方式来论证：在一级近似下，我们是从
		 * nd 较小的关系的角度做估计。
		 */
		selec = (totalsel1 < totalsel2) ? totalsel1 : totalsel2;
	}
	else
	{
		/*
		 * 我们并没有两侧的 MCV 列表。把连接选择性估计为
		 * MIN(1/nd1,1/nd2)*(1-nullfrac1)*(1-nullfrac2)。
		 * 如果我们假定连接操作符是严格的、且非 NULL 值
		 * 大致均匀分布，这就说得通：rel1 的某个给定非 NULL
		 * 元组将连接 rel2 的 0 行或 N2*(1-nullfrac2)/nd2 行，
		 * 因此总连接行数至多为
		 * N1*(1-nullfrac1)*N2*(1-nullfrac2)/nd2，对应的连接
		 * 选择性不超过 (1-nullfrac1)*(1-nullfrac2)/nd2。同理
		 * 它也不超过 (1-nullfrac1)*(1-nullfrac2)/nd1，因此带
		 * MIN() 的表达式是一个上界。使用 MIN() 意味着我们从
		 * nd 较小的关系的角度做估计（因为较大的 nd 决定了
		 * MIN）。有理由假定本关系中的大多数元组都会有连接
		 * 伙伴，因此该上界可能相当紧，应当直接采用。
		 *
		 * XXX 如果我们只有一侧的 MCV 列表，能否更聪明些？
		 * 似乎如果假定另一侧均匀分布，我们最终得到的答案
		 * 还是一样。
		 */
		double		nullfrac1 = stats1 ? stats1->stanullfrac : 0.0;
		double		nullfrac2 = stats2 ? stats2->stanullfrac : 0.0;

		selec = (1.0 - nullfrac1) * (1.0 - nullfrac2);
		if (nd1 > nd2)
			selec /= nd1;
		else
			selec /= nd2;
	}

	return selec;
}

/*
 * eqjoinsel_semi --- 半连接的 eqjoinsel
 *
 * （也用于反连接，后者我们应当以相同方式估计。）
 * 调用方已确保 vardata1 是 LHS 变量。
 * 与 eqjoinsel_inner 不同，我们必须应对 opfuncoid 为
 * InvalidOid 的情况。
 */
static double
eqjoinsel_semi(Oid opfuncoid, Oid collation,
			   VariableStatData *vardata1, VariableStatData *vardata2,
			   double nd1, double nd2,
			   bool isdefault1, bool isdefault2,
			   AttStatsSlot *sslot1, AttStatsSlot *sslot2,
			   Form_pg_statistic stats1, Form_pg_statistic stats2,
			   bool have_mcvs1, bool have_mcvs2,
			   RelOptInfo *inner_rel)
{
	double		selec;

	/*
	 * 我们把 nd2 钳制为不超过对 inner 关系大小的估计值。
	 * 这凭直觉有一定合理性，因为来自 inner 关系的
	 * 不同值显然不可能多于那么多。这种不对称（即我们不
	 * 对 nd1 做同样钳制）的原因在于，这是应用于 inner
	 * 关系的限制子句能够影响连接结果大小估计的唯一途径，
	 * 因为 set_joinrel_size_estimates 只会把 SEMI/ANTI 的选择性
	 * 乘以 outer 关系的大小。如果我们钳制了 nd1，就会对
	 * outer 关系限制条件的选择性重复计数。
	 *
	 * 我们既可以对连接变量所属的基础关系（如果只有一个）
	 * 施加这种钳制，也可以对当前连接的紧邻 inner 输入关系
	 * 施加。
	 *
	 * 如果我们做了钳制，可以把 nd2 当作一个非默认的估计；
	 * 它也许不算很好，但也不是凭空而来的。当 inner 关系
	 * 为空因而没有统计信息时，这一点最为有用。
	 */
	if (vardata2->rel)
	{
		if (nd2 >= vardata2->rel->rows)
		{
			nd2 = vardata2->rel->rows;
			isdefault2 = false;
		}
	}
	if (nd2 >= inner_rel->rows)
	{
		nd2 = inner_rel->rows;
		isdefault2 = false;
	}

	if (have_mcvs1 && have_mcvs2 && OidIsValid(opfuncoid))
	{
		/*
		 * 两个关系都有了最常见值（MCV）列表。遍历这些列表，
		 * 看看哪些 MCV 在给定操作符下真正相互连接。这使我们
		 * 能够确定由 MCV 列表所代表的那部分关系的精确连接
		 * 选择性。我们仍须对剩余群体做估计，但在倾斜的
		 * 分布下，这能大幅提升准确性。
		 */
		LOCAL_FCINFO(fcinfo, 2);
		FmgrInfo	eqproc;
		bool	   *hasmatch1;
		bool	   *hasmatch2;
		double		nullfrac1 = stats1->stanullfrac;
		double		matchfreq1,
					uncertainfrac,
					uncertain;
		int			i,
					nmatches,
					clamped_nvalues2;

		/*
		 * 上面的钳制可能导致 nd2 小于 sslot2->nvalues；在
		 * 这种情况下，我们假设关系中恰好前 nd2 个最常见值会
		 * 出现在连接输入中，因此只与 MCV 列表的前 nd2 个
		 * 成员比较。当然这经常是错的，但这已是我们能做的
		 * 最好猜测。
		 */
		clamped_nvalues2 = Min(sslot2->nvalues, nd2);

		fmgr_info(opfuncoid, &eqproc);

		/*
		 * 通过只初始化一次 fcinfo 结构体来节省若干周期。
		 * 直接使用 FunctionCallInvoke 也能避免 eqproc 返回
		 * NULL 时失败，尽管等值函数本不应返回 NULL。
		 */
		InitFunctionCallInfoData(*fcinfo, &eqproc, 2, collation,
								 NULL, NULL);
		fcinfo->args[0].isnull = false;
		fcinfo->args[1].isnull = false;

		hasmatch1 = (bool *) palloc0(sslot1->nvalues * sizeof(bool));
		hasmatch2 = (bool *) palloc0(clamped_nvalues2 * sizeof(bool));

		/*
		 * 注意我们假设每个 MCV 最多只匹配另一个 MCV 列表中的
		 * 一个成员。如果操作符并非真正的等值，可能会出现
		 * 多个匹配——但我们不去寻找它们，既是为了速度，
		 * 也因为那样数学上会无法自洽……
		 */
		nmatches = 0;
		for (i = 0; i < sslot1->nvalues; i++)
		{
			int			j;

			fcinfo->args[0].value = sslot1->values[i];

			for (j = 0; j < clamped_nvalues2; j++)
			{
				Datum		fresult;

				if (hasmatch2[j])
					continue;
				fcinfo->args[1].value = sslot2->values[j];
				fcinfo->isnull = false;
				fresult = FunctionCallInvoke(fcinfo);
				if (!fcinfo->isnull && DatumGetBool(fresult))
				{
					hasmatch1[i] = hasmatch2[j] = true;
					nmatches++;
					break;
				}
			}
		}
		/* 累加已匹配 MCV 的频率 */
		matchfreq1 = 0.0;
		for (i = 0; i < sslot1->nvalues; i++)
		{
			if (hasmatch1[i])
				matchfreq1 += sslot1->numbers[i];
		}
		CLAMP_PROBABILITY(matchfreq1);
		pfree(hasmatch1);
		pfree(hasmatch2);

		/*
		 * 现在我们需要估计关系 1 中至少有
		 * 一个连接伙伴的比例。我们确切知道已匹配的 MCV
		 * 一定有，这给了我们一个下界，但对其它一切就
		 * 几乎一无所知了。我们粗略的做法是：如果 nd1 <= nd2，
		 * 则假定所有非 NULL 的 rel1 行都有连接伙伴；否则
		 * 对不确定的行，假定其中 nd2/nd1 的比例有连接伙伴。
		 * 在做除法之前，我们可以从不同值计数中扣除已知
		 * 已匹配的 MCV。
		 *
		 * 尽管上述做法很粗略，但如果我们没有两侧可靠的
		 * ndistinct 值，它就完全没用了。因此，如果 nd1 或 nd2
		 * 是任意一方默认值，就放弃并假定一半的不确定行有
		 * 连接伙伴。
		 */
		if (!isdefault1 && !isdefault2)
		{
			nd1 -= nmatches;
			nd2 -= nmatches;
			if (nd1 <= nd2 || nd2 < 0)
				uncertainfrac = 1.0;
			else
				uncertainfrac = nd2 / nd1;
		}
		else
			uncertainfrac = 0.5;
		uncertain = 1.0 - matchfreq1 - nullfrac1;
		CLAMP_PROBABILITY(uncertain);
		selec = matchfreq1 + uncertainfrac * uncertain;
	}
	else
	{
		/*
		 * 没有两侧的 MCV 列表，我们只能使用关于 nd1 与
		 * nd2 的启发式方法。
		 */
		double		nullfrac1 = stats1 ? stats1->stanullfrac : 0.0;

		if (!isdefault1 && !isdefault2)
		{
			if (nd1 <= nd2 || nd2 < 0)
				selec = 1.0 - nullfrac1;
			else
				selec = (nd2 / nd1) * (1.0 - nullfrac1);
		}
		else
			selec = 0.5 * (1.0 - nullfrac1);
	}

	return selec;
}

/*
 *		neqjoinsel		- "!=" 的连接选择性。
 */
Datum
neqjoinsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	JoinType	jointype = (JoinType) PG_GETARG_INT16(3);
	SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) PG_GETARG_POINTER(4);
	Oid			collation = PG_GET_COLLATION();
	float8		result;

	if (jointype == JOIN_SEMI || jointype == JOIN_ANTI)
	{
		/*
		 * 对于半连接，如果 RHS 关系中存在多于一个的不同值，
		 * 那么每个非 NULL 的 LHS 行都必然能找到一行与之连接，
		 * 因为它只能与其中一个相等。为了稳定性，我们假定
		 * RHS 总是存在多于一个的不同值，尽管理论上我们
		 * 可以对空 RHS（选择性 = 0）和单不同值 RHS（选择性 =
		 * 与单一 RHS 值相同的 LHS 的比例）做特殊处理。
		 *
		 * 对于反连接，如果我们采用同样的假设，即 RHS 关系
		 * 中存在多于一个的不同键，那么每个非 NULL 的 LHS 行
		 * 都必然被反连接抑制。
		 *
		 * 因此无论哪种情况，选择性估计都应为 1 - nullfrac。
		 */
		VariableStatData leftvar;
		VariableStatData rightvar;
		bool		reversed;
		HeapTuple	statsTuple;
		double		nullfrac;

		get_join_variables(root, args, sjinfo, &leftvar, &rightvar, &reversed);
		statsTuple = reversed ? rightvar.statsTuple : leftvar.statsTuple;
		if (HeapTupleIsValid(statsTuple))
			nullfrac = ((Form_pg_statistic) GETSTRUCT(statsTuple))->stanullfrac;
		else
			nullfrac = 0.0;
		ReleaseVariableStats(leftvar);
		ReleaseVariableStats(rightvar);

		result = 1.0 - nullfrac;
	}
	else
	{
		/*
		 * 我们需要 1 - eqjoinsel()，其中等值操作符是与这个
		 * != 操作符相关联的那个，即它的求反符。
		 */
		Oid			eqop = get_negator(operator);

		if (eqop)
		{
			result =
				DatumGetFloat8(DirectFunctionCall5Coll(eqjoinsel,
													   collation,
													   PointerGetDatum(root),
													   ObjectIdGetDatum(eqop),
													   PointerGetDatum(args),
													   Int16GetDatum(jointype),
													   PointerGetDatum(sjinfo)));
		}
		else
		{
			/* 使用默认选择性（或者我们是否应该改为报错？） */
			result = DEFAULT_EQ_SEL;
		}
		result = 1.0 - result;
	}

	PG_RETURN_FLOAT8(result);
}

/*
 *		scalarltjoinsel - 标量类型 "<" 的连接选择性
 */
Datum
scalarltjoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
}

/*
 *		scalarlejoinsel - 标量类型 "<=" 的连接选择性
 */
Datum
scalarlejoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
}

/*
 *		scalargtjoinsel - 标量类型 ">" 的连接选择性
 */
Datum
scalargtjoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
}

/*
 *		scalargejoinsel - Join selectivity of ">=" for scalars
 */
Datum
scalargejoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
}


/*
 * mergejoinscansel			- 合并连接的扫描选择性。
 *
 * 合并连接会在任一输入流耗尽时立即停止。因此，如果我们能
 * 估计两个输入变量的取值范围，就能估计出实际会被读取的
 * 输入比例。这在使用索引扫描时会对代价产生相当大的
 * 影响。
 *
 * 此外，我们还能估计在找到第一对连接行之前需要读取多少
 * 输入，这会影响连接的启动时间。
 *
 * clause 应当是一个已知可合并连接（mergejoinable）的子句。
 * opfamily、cmptype 与 nulls_first 指定所使用的排序顺序。
 *
 * 输出为：
 *		*leftstart 被设为预期在第一对连接行被找到之前
 *		 会被扫描的左变量比例（0 到 1）。
 *		*leftend 被设为预期在连接终止之前会被扫描的
 *		 左变量比例（0 到 1）。
 *		*rightstart、*rightend 对右变量同理。
 */
void
mergejoinscansel(PlannerInfo *root, Node *clause,
				 Oid opfamily, CompareType cmptype, bool nulls_first,
				 Selectivity *leftstart, Selectivity *leftend,
				 Selectivity *rightstart, Selectivity *rightend)
{
	Node	   *left,
			   *right;
	VariableStatData leftvar,
				rightvar;
	Oid			opmethod;
	int			op_strategy;
	Oid			op_lefttype;
	Oid			op_righttype;
	Oid			opno,
				collation,
				lsortop,
				rsortop,
				lstatop,
				rstatop,
				ltop,
				leop,
				revltop,
				revleop;
	StrategyNumber ltstrat,
				lestrat,
				gtstrat,
				gestrat;
	bool		isgt;
	Datum		leftmin,
				leftmax,
				rightmin,
				rightmax;
	double		selec;

	/* 如果无法得出任何结果，则设置默认值。 */
	/* XXX 默认的 "start" 比例是否应略大于 0？ */
	*leftstart = *rightstart = 0.0;
	*leftend = *rightend = 1.0;

	/* 拆解合并子句 */
	if (!is_opclause(clause))
		return;					/* 不应发生 */
	opno = ((OpExpr *) clause)->opno;
	collation = ((OpExpr *) clause)->inputcollid;
	left = get_leftop((Expr *) clause);
	right = get_rightop((Expr *) clause);
	if (!right)
		return;					/* 不应发生 */

	/* 查找输入的统计信息 */
	examine_variable(root, left, 0, &leftvar);
	examine_variable(root, right, 0, &rightvar);

	opmethod = get_opfamily_method(opfamily);

	/* 提取操作符声明的左右数据类型 */
	get_op_opfamily_properties(opno, opfamily, false,
							   &op_strategy,
							   &op_lefttype,
							   &op_righttype);
	Assert(IndexAmTranslateStrategy(op_strategy, opmethod, opfamily, true) == COMPARE_EQ);

	/*
	 * 查找我们所需的各个操作符。如果我们没能全部找到，
	 * 很可能意味着该操作符族（opfamily）有问题，但我们
	 * 只是静默地失败。
	 *
	 * 注意：我们期望 pg_statistic 的直方图总是按 '<'
	 * 操作符排序，无论我们考虑的是哪种排序方向。
	 */
	switch (cmptype)
	{
		case COMPARE_LT:
			isgt = false;
			ltstrat = IndexAmTranslateCompareType(COMPARE_LT, opmethod, opfamily, true);
			lestrat = IndexAmTranslateCompareType(COMPARE_LE, opmethod, opfamily, true);
			if (op_lefttype == op_righttype)
			{
				/* 简单情况 */
				ltop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   ltstrat);
				leop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   lestrat);
				lsortop = ltop;
				rsortop = ltop;
				lstatop = lsortop;
				rstatop = rsortop;
				revltop = ltop;
				revleop = leop;
			}
			else
			{
				ltop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   ltstrat);
				leop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   lestrat);
				lsortop = get_opfamily_member(opfamily,
											  op_lefttype, op_lefttype,
											  ltstrat);
				rsortop = get_opfamily_member(opfamily,
											  op_righttype, op_righttype,
											  ltstrat);
				lstatop = lsortop;
				rstatop = rsortop;
				revltop = get_opfamily_member(opfamily,
											  op_righttype, op_lefttype,
											  ltstrat);
				revleop = get_opfamily_member(opfamily,
											  op_righttype, op_lefttype,
											  lestrat);
			}
			break;
		case COMPARE_GT:
			/* 降序情况 */
			isgt = true;
			ltstrat = IndexAmTranslateCompareType(COMPARE_LT, opmethod, opfamily, true);
			gtstrat = IndexAmTranslateCompareType(COMPARE_GT, opmethod, opfamily, true);
			gestrat = IndexAmTranslateCompareType(COMPARE_GE, opmethod, opfamily, true);
			if (op_lefttype == op_righttype)
			{
				/* 简单情况 */
				ltop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   gtstrat);
				leop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   gestrat);
				lsortop = ltop;
				rsortop = ltop;
				lstatop = get_opfamily_member(opfamily,
											  op_lefttype, op_lefttype,
											  ltstrat);
				rstatop = lstatop;
				revltop = ltop;
				revleop = leop;
			}
			else
			{
				ltop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   gtstrat);
				leop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   gestrat);
				lsortop = get_opfamily_member(opfamily,
											  op_lefttype, op_lefttype,
											  gtstrat);
				rsortop = get_opfamily_member(opfamily,
											  op_righttype, op_righttype,
											  gtstrat);
				lstatop = get_opfamily_member(opfamily,
											  op_lefttype, op_lefttype,
											  ltstrat);
				rstatop = get_opfamily_member(opfamily,
											  op_righttype, op_righttype,
											  ltstrat);
				revltop = get_opfamily_member(opfamily,
											  op_righttype, op_lefttype,
											  gtstrat);
				revleop = get_opfamily_member(opfamily,
											  op_righttype, op_lefttype,
											  gestrat);
			}
			break;
		default:
			goto fail;			/* 不应到达此处 */
	}

	if (!OidIsValid(lsortop) ||
		!OidIsValid(rsortop) ||
		!OidIsValid(lstatop) ||
		!OidIsValid(rstatop) ||
		!OidIsValid(ltop) ||
		!OidIsValid(leop) ||
		!OidIsValid(revltop) ||
		!OidIsValid(revleop))
		goto fail;				/* insufficient info in catalogs */

	/* 尝试获取两个输入的取值范围 */
	if (!isgt)
	{
		if (!get_variable_range(root, &leftvar, lstatop, collation,
								&leftmin, &leftmax))
			goto fail;			/* 统计中没有可用范围 */
		if (!get_variable_range(root, &rightvar, rstatop, collation,
								&rightmin, &rightmax))
			goto fail;			/* 统计中没有可用范围 */
	}
	else
	{
		/* 需要交换 max 与 min */
		if (!get_variable_range(root, &leftvar, lstatop, collation,
								&leftmax, &leftmin))
			goto fail;			/* 统计中没有可用范围 */
		if (!get_variable_range(root, &rightvar, rstatop, collation,
								&rightmax, &rightmin))
			goto fail;			/* 统计中没有可用范围 */
	}

	/*
	 * 现在，左变量中将被扫描的比例是
	 * <= 右侧最大值的部分所占的比例。但我们只相信
	 * 非默认的估计，否则就维持 1.0。
	 */
	selec = scalarineqsel(root, leop, isgt, true, collation, &leftvar,
						  rightmax, op_righttype);
	if (selec != DEFAULT_INEQ_SEL)
		*leftend = selec;

	/* 右变量同理。 */
	selec = scalarineqsel(root, revleop, isgt, true, collation, &rightvar,
						  leftmax, op_lefttype);
	if (selec != DEFAULT_INEQ_SEL)
		*rightend = selec;

	/*
	 * 两个 "end" 比例中真正小于 1.0 的只能有一个；
	 * 相信较小的那个估计，并把另一个重置为正好 1.0。
	 * 如果得到完全相等的估计（自连接时很容易出现），
	 * 则两个都不信。
	 */
	if (*leftend > *rightend)
		*leftend = 1.0;
	else if (*leftend < *rightend)
		*rightend = 1.0;
	else
		*leftend = *rightend = 1.0;

	/*
	 * 此外，在找到第一对连接行之前会被扫描的左变量
	 * 比例是 < 右侧最小值的部分所占的比例。
	 * 但我们只相信非默认的估计，否则就维持我们
	 * 自己的默认值。
	 */
	selec = scalarineqsel(root, ltop, isgt, false, collation, &leftvar,
						  rightmin, op_righttype);
	if (selec != DEFAULT_INEQ_SEL)
		*leftstart = selec;

	/* 右变量同理。 */
	selec = scalarineqsel(root, revltop, isgt, false, collation, &rightvar,
						  leftmin, op_lefttype);
	if (selec != DEFAULT_INEQ_SEL)
		*rightstart = selec;

	/*
	 * 两个 "start" 比例中真正大于零的只能有一个；
	 * 相信较大的那个估计，并把另一个重置为正好 0.0。
	 * 如果得到完全相等的估计（自连接时很容易出现），
	 * 则两个都不信。
	 */
	if (*leftstart < *rightstart)
		*leftstart = 0.0;
	else if (*leftstart > *rightstart)
		*rightstart = 0.0;
	else
		*leftstart = *rightstart = 0.0;

	/*
	 * 如果排序顺序是 nulls-first，我们也必须跳过任何
	 * NULL 值。这些不会被 scalarineqsel 计入，而无论我们
	 * 是否相信 scalarineqsel 的结果，都可以安全地把这个
	 * 比例加进来。但务必把总和钳制到 1.0！
	 */
	if (nulls_first)
	{
		Form_pg_statistic stats;

		if (HeapTupleIsValid(leftvar.statsTuple))
		{
			stats = (Form_pg_statistic) GETSTRUCT(leftvar.statsTuple);
			*leftstart += stats->stanullfrac;
			CLAMP_PROBABILITY(*leftstart);
			*leftend += stats->stanullfrac;
			CLAMP_PROBABILITY(*leftend);
		}
		if (HeapTupleIsValid(rightvar.statsTuple))
		{
			stats = (Form_pg_statistic) GETSTRUCT(rightvar.statsTuple);
			*rightstart += stats->stanullfrac;
			CLAMP_PROBABILITY(*rightstart);
			*rightend += stats->stanullfrac;
			CLAMP_PROBABILITY(*rightend);
		}
	}

	/* 不信任 start >= end，以防这种情况发生 */
	if (*leftstart >= *leftend)
	{
		*leftstart = 0.0;
		*leftend = 1.0;
	}
	if (*rightstart >= *rightend)
	{
		*rightstart = 0.0;
		*rightend = 1.0;
	}

fail:
	ReleaseVariableStats(leftvar);
	ReleaseVariableStats(rightvar);
}


/*
 *	matchingsel -- 通用匹配操作符选择性支持
 *
 * 把本函数用于满足以下条件的任何操作符：(a) 其数据类型
 * 是我们收集标准统计信息的类型；(b) 其默认估计
 * （DEFAULT_EQ_SEL 的两倍）是合理的。通常这对
 * 匹配类的操作符是合适的。
 */

Datum
matchingsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	Oid			collation = PG_GET_COLLATION();
	double		selec;

	/* 使用通用的限制选择性逻辑。 */
	selec = generic_restriction_selectivity(root, operator, collation,
											args, varRelid,
											DEFAULT_MATCHING_SEL);

	PG_RETURN_FLOAT8((float8) selec);
}

Datum
matchingjoinsel(PG_FUNCTION_ARGS)
{
	/* 暂时先放弃。 */
	PG_RETURN_FLOAT8(DEFAULT_MATCHING_SEL);
}


/*
 * estimate_num_groups 的辅助例程：向 GroupVarInfo 列表中
 * 添加一项，但仅当它不被认为与任何现有项相等时。
 */
typedef struct
{
	Node	   *var;			/* 可能是一个表达式，而不仅仅是一个 Var */
	RelOptInfo *rel;			/* 它所属的关系 */
	double		ndistinct;		/* 不同值的数量 */
	bool		isdefault;		/* 若使用了 DEFAULT_NUM_DISTINCT 则为 true */
} GroupVarInfo;

static List *
add_unique_group_var(PlannerInfo *root, List *varinfos,
					 Node *var, VariableStatData *vardata)
{
	GroupVarInfo *varinfo;
	double		ndistinct;
	bool		isdefault;
	ListCell   *lc;

	ndistinct = get_variable_numdistinct(vardata, &isdefault);

	/*
	 * var 内的 nullingrels 位可能导致同一个 var 在标有不同的
	 * nullingrels 时被重复计数。它们也可能妨碍我们把 var
	 * 匹配到扩展统计中的表达式（见 estimate_multivariate_ndistinct）。
	 * 因此先将其剥离。
	 */
	var = remove_nulling_relids(var, root->outer_join_rels, NULL);

	foreach(lc, varinfos)
	{
		varinfo = (GroupVarInfo *) lfirst(lc);

		/* 丢弃完全相同的副本 */
		if (equal(var, varinfo->var))
			return varinfos;

		/*
		 * Drop known-equal vars, but only if they belong to different
		 * relations (see comments for estimate_num_groups).  We aren't too
		 * fussy about the semantics of "equal" here.
		 */
		if (vardata->rel != varinfo->rel &&
			exprs_known_equal(root, var, varinfo->var, InvalidOid))
		{
			if (varinfo->ndistinct <= ndistinct)
			{
				/* 保留旧项，丢弃新项 */
				return varinfos;
			}
			else
			{
				/* Delete the older item */
				varinfos = foreach_delete_current(varinfos, lc);
			}
		}
	}

	varinfo = (GroupVarInfo *) palloc(sizeof(GroupVarInfo));

	varinfo->var = var;
	varinfo->rel = vardata->rel;
	varinfo->ndistinct = ndistinct;
	varinfo->isdefault = isdefault;
	varinfos = lappend(varinfos, varinfo);
	return varinfos;
}

/*
 * estimate_num_groups		- 估计分组查询中的组数
 *
 * 给定一个带有 GROUP BY 子句的查询，估计会有多少组——
 * 即 GROUP BY 表达式的不同组合的数量。
 *
 * 本例程也用于估计 DISTINCT 过滤步骤所输出的行数；
 * 那是一个同构的问题。（注意：实际上，我们只在
 * DISTINCT 之前没有分组或聚合时才将其用于 DISTINCT。）
 *
 * 输入：
 *	root - 查询
 *	groupExprs - 正在被分组的表达式列表
 *	input_rows - 估计到达 group/unique 过滤步骤的行数
 *	pgset - NULL，或一个指向分组集的 List**，用于
 *		对 groupExprs 进行过滤
 *
 * 输出：
 *	estinfo - 当以非 NULL 传入时，函数会在 "flags"
 *		字段中置位，以向调用方提供关于估计的额外
 *		信息。目前，仅当我们使用了任何默认值时，才会
 *		置位 SELFLAG_USED_DEFAULT 位。
 *
 * 由于系统中缺乏任何交叉相关性统计，对于涉及多个
 * Var 的 GROUP BY 条件，我们不可能做出真正可信的处理。
 * 但我们也应避免假设最坏情况（所有可能的交叉乘积
 * 项都实际作为组出现），因为被分组的 Var 往往高度
 * 相关。我们当前的方法如下：
 *	1.  产生布尔值的表达式被假定贡献两个组，与其内容
 *		无关，并在后续步骤中被忽略。这主要是因为像
 *		"col IS NULL" 这样的测试会严重破坏第 2 步中
 *		所用的启发式。
 *	2.  将给定的表达式化简为所用到的唯一 Var 列表。
 *		例如，GROUP BY a, a + b 被视为与 GROUP BY a, b
 *		相同。显然，不对同一个 Var 计数超过一次是正确的。
 *		把 f(x) 视为与 x 相同也是合理的：f() 不可能
 *		增加不同值的数量（除非它是易变的，而我们认为
 *		对分组而言不太可能），但它大概也不会显著
 *		减少不同值的数量。
 *		作为一种特殊情况，如果一个 GROUP BY 表达式能
 *		匹配到一个我们有统计信息的表达式索引，那么
 *		我们把整个表达式当作仅仅是一个 Var。
 *	3.  如果列表包含因等价类而已知相等的不同关系的
 *		Var，则从每个已知相等的集合中只保留一个 Var，
 *		保留估计值数量最少的那一个（因为其它 Var 的
 *		额外值不可能出现在连接行中）。我们只考虑不同
 *		关系的 Var 的原因是，如果考虑同一关系的 Var，
 *		就会在下一步中对该等值的限制选择性重复计数。
 *	4.  对于单个源关系内的 Var，我们把它们的值数量相乘，
 *		钳制到该关系的行数（若有多个 Var 则除以 10），
 *		再乘以一个基于该关系限制子句选择性的因子。当
 *		存在多个 Var 时，初始乘积可能过高（那是最坏
 *		情况），但钳制到关系行数的某个比例似乎是防止
 *		估计失控的有用启发式。（因子 10 源自
 *		Postgres 7.4 之前的做法。）我们用来根据限制
 *		选择性进行调整的乘法因子假定限制子句与分组
 *		相互独立，这也许并非一个有效的假设，但很难
 *		做得更好。
 *	5.  如果存在来自多个关系的 Var，我们对每个这样的
 *		关系重复第 4 步，并将结果相乘。
 * 注意，不包含被分组 Var 的关系以及连接子句都会被
 * 完全忽略。这样的关系不可能增加组数，而我们假定
 * 这样的子句也不会减少组数（有点不可靠，但我们
 * 没有更好的信息）。
 */
double
estimate_num_groups(PlannerInfo *root, List *groupExprs, double input_rows,
					List **pgset, EstimationInfo *estinfo)
{
	List	   *varinfos = NIL;
	double		srf_multiplier = 1.0;
	double		numdistinct;
	ListCell   *l;
	int			i;

	/* 清零 estinfo 输出参数（若非 NULL） */
	if (estinfo != NULL)
		memset(estinfo, 0, sizeof(EstimationInfo));

	/*
	 * We don't ever want to return an estimate of zero groups, as that tends
	 * to lead to division-by-zero and other unpleasantness.  The input_rows
	 * estimate is usually already at least 1, but clamp it just in case it
	 * isn't.
	 */
	input_rows = clamp_row_est(input_rows);

	/*
	 * If no grouping columns, there's exactly one group.  (This can't happen
	 * for normal cases with GROUP BY or DISTINCT, but it is possible for
	 * corner cases with set operations.)
	 */
	if (groupExprs == NIL || (pgset && *pgset == NIL))
		return 1.0;

	/*
	 * 统计由布尔分组表达式得到的组数。对于其它表达式，
	 * 找出所用到的唯一 Var，如果能找到其统计信息，则把
	 * 该表达式当作一个 Var。对每一个，记录其不同值数量
	 * 的统计估计（其表中的总数，不考虑过滤）。
	 */
	numdistinct = 1.0;

	i = 0;
	foreach(l, groupExprs)
	{
		Node	   *groupexpr = (Node *) lfirst(l);
		double		this_srf_multiplier;
		VariableStatData vardata;
		List	   *varshere;
		ListCell   *l2;

		/* is expression in this grouping set? */
		if (pgset && !list_member_int(*pgset, i++))
			continue;

		/*
		 * Set-returning functions in grouping columns are a bit problematic.
		 * The code below will effectively ignore their SRF nature and come up
		 * with a numdistinct estimate as though they were scalar functions.
		 * We compensate by scaling up the end result by the largest SRF
		 * rowcount estimate.  (This will be an overestimate if the SRF
		 * produces multiple copies of any output value, but it seems best to
		 * assume the SRF's outputs are distinct.  In any case, it's probably
		 * pointless to worry too much about this without much better
		 * estimates for SRF output rowcounts than we have today.)
		 */
		this_srf_multiplier = expression_returns_set_rows(root, groupexpr);
		if (srf_multiplier < this_srf_multiplier)
			srf_multiplier = this_srf_multiplier;

		/* 对返回布尔值的表达式短路处理 */
		if (exprType(groupexpr) == BOOLOID)
		{
			numdistinct *= 2.0;
			continue;
		}

		/*
		 * If examine_variable is able to deduce anything about the GROUP BY
		 * expression, treat it as a single variable even if it's really more
		 * complicated.
		 *
		 * XXX This has the consequence that if there's a statistics object on
		 * the expression, we don't split it into individual Vars. This
		 * affects our selection of statistics in
		 * estimate_multivariate_ndistinct, because it's probably better to
		 * use more accurate estimate for each expression and treat them as
		 * independent, than to combine estimates for the extracted variables
		 * when we don't know how that relates to the expressions.
		 */
		examine_variable(root, groupexpr, 0, &vardata);
		if (HeapTupleIsValid(vardata.statsTuple) || vardata.isunique)
		{
			varinfos = add_unique_group_var(root, varinfos,
											groupexpr, &vardata);
			ReleaseVariableStats(vardata);
			continue;
		}
		ReleaseVariableStats(vardata);

		/*
		 * Else pull out the component Vars.  Handle PlaceHolderVars by
		 * recursing into their arguments (effectively assuming that the
		 * PlaceHolderVar doesn't change the number of groups, which boils
		 * down to ignoring the possible addition of nulls to the result set).
		 */
		varshere = pull_var_clause(groupexpr,
								   PVC_RECURSE_AGGREGATES |
								   PVC_RECURSE_WINDOWFUNCS |
								   PVC_RECURSE_PLACEHOLDERS);

		/*
		 * 如果我们发现任何不含变量的 GROUP BY 项，那么
		 * 它要么是个常量（可忽略），要么包含易变函数；
		 * 在后者的情况下，我们放弃并假定每个输入行都会
		 * 产生一个不同的组。
		 */
		if (varshere == NIL)
		{
			if (contain_volatile_functions(groupexpr))
				return input_rows;
			continue;
		}

		/*
		 * Else add variables to varinfos list
		 */
		foreach(l2, varshere)
		{
			Node	   *var = (Node *) lfirst(l2);

			examine_variable(root, var, 0, &vardata);
			varinfos = add_unique_group_var(root, varinfos, var, &vardata);
			ReleaseVariableStats(vardata);
		}
	}

	/*
	 * If now no Vars, we must have an all-constant or all-boolean GROUP BY
	 * list.
	 */
	if (varinfos == NIL)
	{
		/* Apply SRF multiplier as we would do in the long path */
		numdistinct *= srf_multiplier;
		/* Round off */
		numdistinct = ceil(numdistinct);
		/* Guard against out-of-range answers */
		if (numdistinct > input_rows)
			numdistinct = input_rows;
		if (numdistinct < 1.0)
			numdistinct = 1.0;
		return numdistinct;
	}

	/*
	 * 按关系对 Var 分组，并估计整体的不同值数量（numdistinct）。
	 *
	 * 外层循环的每一次迭代中，我们处理 varinfos 中最前面的那个 Var，
	 * 以及同属一个关系的所有其他 Var。我们把这些 Var 从 newvarinfos
	 * 列表中移除，供下一次迭代使用。这是把同关系的 Var 归到一起的最简便方式。
	 */
	do
	{
		GroupVarInfo *varinfo1 = (GroupVarInfo *) linitial(varinfos);
		RelOptInfo *rel = varinfo1->rel;
		double		reldistinct = 1;
		double		relmaxndistinct = reldistinct;
		int			relvarcount = 0;
		List	   *newvarinfos = NIL;
		List	   *relvarinfos = NIL;

		/*
		 * 把 varinfos 列表分成两组——一组属于当前关系，另一组属于
		 * 其他关系上剩余的 Var。
		 */
		relvarinfos = lappend(relvarinfos, varinfo1);
		for_each_from(l, varinfos, 1)
		{
			GroupVarInfo *varinfo2 = (GroupVarInfo *) lfirst(l);

			if (varinfo2->rel == varinfo1->rel)
			{
				/* 当前关系上的 varinfo */
				relvarinfos = lappend(relvarinfos, varinfo2);
			}
			else
			{
				/* varinfo2 暂未轮到处理 */
				newvarinfos = lappend(newvarinfos, varinfo2);
			}
		}

		/*
		 * 取得该关系上各 Var 的 numdistinct 估计。我们迭代地寻找包含
		 * 最多 Var 的多变量 n-distinct；假设每个 Var 组彼此独立，便把它们
		 * 相乘。当再也找不到多变量匹配后，剩余的 relvarinfos 也被视为
		 * 相互独立，因此它们各自的 ndistinct 估计同样会被乘进来。
		 *
		 * 迭代过程中，统计我们一共应用了多少个独立的 numdistinct 值。
		 * 下面会应用一个修正因子，但仅当我们乘入了不止一个这样的值时才会使用。
		 */
		while (relvarinfos)
		{
			double		mvndistinct;

			if (estimate_multivariate_ndistinct(root, rel, &relvarinfos,
												&mvndistinct))
			{
				reldistinct *= mvndistinct;
				if (relmaxndistinct < mvndistinct)
					relmaxndistinct = mvndistinct;
				relvarcount++;
			}
			else
			{
				foreach(l, relvarinfos)
				{
					GroupVarInfo *varinfo2 = (GroupVarInfo *) lfirst(l);

					reldistinct *= varinfo2->ndistinct;
					if (relmaxndistinct < varinfo2->ndistinct)
						relmaxndistinct = varinfo2->ndistinct;
					relvarcount++;

					/*
					 * 当 varinfo2 的 isdefault 被置位时，我们最好在
					 * EstimationInfo 中设置 SELFLAG_USED_DEFAULT 标志位。
					 */
					if (estinfo != NULL && varinfo2->isdefault)
						estinfo->flags |= SELFLAG_USED_DEFAULT;
				}

				/* we're done with this relation */
				relvarinfos = NIL;
			}
		}

		/*
		 * 健全性检查——若关系为空，则不要除以零。
		 */
		Assert(IS_SIMPLE_REL(rel));
		if (rel->tuples > 0)
		{
			/*
			 * 钳制到关系的大小，若包含多个 Var 则钳制到关系大小 / 10。
			 * 引入这个修正因子是因为这些 Var 之间很可能相关，但我们并不知道
			 * 相关程度如何。不过，我们永远不应钳制到小于任一 Var 的最大
			 * ndistinct 值，因为组的数量必定至少达到那么多。
			 */
			double		clamp = rel->tuples;

			if (relvarcount > 1)
			{
				clamp *= 0.1;
				if (clamp < relmaxndistinct)
				{
					clamp = relmaxndistinct;
					/* 防御性处理：万一某个 ndistinct 取值过大 */
					if (clamp > rel->tuples)
						clamp = rel->tuples;
				}
			}
			if (reldistinct > clamp)
				reldistinct = clamp;

		/*
		 * 基于限制子句的选择性来更新估计，当 reldistinct 为零时
		 * 要防止除以零。另外，如果我们知道会返回全部行，则跳过此步骤。
		 */
		if (reldistinct > 0 && rel->rows < rel->tuples)
		{
			/*
			 * 对于一个包含 N 行、其中有 n 个不同值且呈均匀分布的表，
			 * 如果我们随机选取 p 行，那么被选中值中不同值的期望数量为
			 *
			 * n * (1 - product((N-N/n-i)/(N-i), i=0..p-1))
			 *
			 * = n * (1 - (N-N/n)! / (N-N/n-p)! * (N-p)! / N!)
			 *
			 * 参见 "Approximating block accesses in database
			 * organizations"，S. B. Yao，Communications of the ACM，
			 * 第 20 卷第 4 期，1977 年 4 月，第 260-261 页。
			 *
			 * 或者，把阶乘中的各项重新整理后，也可写成
			 *
			 * n * (1 - product((N-p-i)/(N-i), i=0..N/n-1))
			 *
			 * 在 p 大于 N/n 的常见情形下，这种形式的公式计算效率更高。
			 * 此外，正如 Dell'Era 所指出的，如果乘积中所有项的 i << N，
			 * 它可以被近似为
			 *
			 * n * (1 - ((N-p)/N)^(N/n))
			 *
			 * 参见 "Expected distinct values when selecting from a bag
			 * without replacement"，Alberto Dell'Era，
			 * http://www.adellera.it/investigations/distinct_balls/。
			 *
			 * 条件 i << N 等价于 n >> 1，因此当表中不同值的数量很大时，
			 * 这是一个很好的近似。事实证明，即便 n 很小时，这个公式
			 * 的效果也不错。
			 */
			reldistinct *=
					(1 - pow((rel->tuples - rel->rows) / rel->tuples,
							 rel->tuples / reldistinct));
			}
			reldistinct = clamp_row_est(reldistinct);

			/*
			 * 更新对总组数的估计。
			 */
			numdistinct *= reldistinct;
		}

		varinfos = newvarinfos;
	} while (varinfos != NIL);

	/* 现在把任意 SRF 带来的效果计入 */
	numdistinct *= srf_multiplier;

	/* 取整 */
	numdistinct = ceil(numdistinct);

	/* 防止结果超出合理范围 */
	if (numdistinct > input_rows)
		numdistinct = input_rows;
	if (numdistinct < 1.0)
		numdistinct = 1.0;

	return numdistinct;
}

/*
 * 当连接条件包含两个或更多子句时，尝试借助扩展统计信息来估计哈希连接
 * 内表的桶大小。
 *
 * 这种方法的主要思路是：对两列或更多列进行多变量估计所得到的不同值，
 * 会比单独对某一列估计出来的不同值产生更小的桶大小。
 *
 * 重要：把不同估计结果组合起来的方式，必须与调用方的方法保持一致。
 *
 * 返回那些没有取到任何扩展统计信息的子句列表。
 */
List *
estimate_multivariate_bucketsize(PlannerInfo *root, RelOptInfo *inner,
								 List *hashclauses,
								 Selectivity *innerbucketsize)
{
	List	   *clauses = list_copy(hashclauses);
	List	   *otherclauses = NIL;
	double		ndistinct = 1.0;

	if (list_length(hashclauses) <= 1)

		/*
		 * 对于单个子句无需处理。我们能否在这里使用单变量扩展统计？
		 */
		return hashclauses;

	while (clauses != NIL)
	{
		ListCell   *lc;
		int			relid = -1;
		List	   *varinfos = NIL;
		List	   *origin_rinfos = NIL;
		double		mvndistinct;
		List	   *origin_varinfos;
		int			group_relid = -1;
		RelOptInfo *group_rel = NULL;
		ListCell   *lc1,
				   *lc2;

		/*
		 * 找出引用同一单个基关系、并尝试用扩展统计来估计这样一组子句的
		 * 子句。为被接纳的子句创建 varinfo；如果它无法在这里被估计，则
		 * 把它推入 otherclauses，留待下一次迭代处理。
		 */
		foreach(lc, clauses)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
			Node	   *expr;
			Relids		relids;
			GroupVarInfo *varinfo;

			/*
			 * Find the inner side of the join, which we need to estimate the
			 * number of buckets.  Use outer_is_left because the
			 * clause_sides_match_join routine has called on hash clauses.
			 */
			relids = rinfo->outer_is_left ?
				rinfo->right_relids : rinfo->left_relids;
			expr = rinfo->outer_is_left ?
				get_rightop(rinfo->clause) : get_leftop(rinfo->clause);

			if (bms_get_singleton_member(relids, &relid) &&
				root->simple_rel_array[relid]->statlist != NIL)
			{
				bool		is_duplicate = false;

			/*
			 * 这个内表侧表达式只引用了一个关系。关于这个子句的
			 * 扩展统计信息有可能存在。
			 */
			if (group_relid < 0)
				{
					RangeTblEntry *rte = root->simple_rte_array[relid];

					if (!rte || (rte->relkind != RELKIND_RELATION &&
								 rte->relkind != RELKIND_MATVIEW &&
								 rte->relkind != RELKIND_FOREIGN_TABLE &&
								 rte->relkind != RELKIND_PARTITIONED_TABLE))
					{
					/* 原则上不可能存在扩展统计信息 */
					otherclauses = lappend(otherclauses, rinfo);
						clauses = foreach_delete_current(clauses, lc);
						continue;
					}

					group_relid = relid;
					group_rel = root->simple_rel_array[relid];
				}
				else if (group_relid != relid)

					/*
					 * 当前正处于分组形成阶段，我们不需要其他子句。
					 */
					continue;

		/*
		 * 我们要把新子句加入 varinfos 列表。我们本可以复用
		 * add_unique_group_var()，但有两个原因让我们没有这么做。
		 *
		 * 1) 我们必须让 origin_rinfos 列表的排列顺序与 varinfos
		 * 完全一致。
		 *
		 * 2) add_unique_group_var() 是为 estimate_num_groups() 设计的，
		 * 在那里组数越多越糟。但在估计哈希桶数量时，情况正好相反：
		 * 组数越少越糟。因此，我们没有必要移除“已知相等”的 var：
		 * 被移除的 var 可能很有价值地为多变量统计做出贡献，从而增加组数。
		 */

			/*
			 * 清除 nullingrels 以正确匹配哈希键。详情参见
			 * add_unique_group_var() 的注释。
			 */
			expr = remove_nulling_relids(expr, root->outer_join_rels, NULL);

			/*
			 * 检测并排除哈希键列表中的完全重复项（与 add_unique_group_var 的做法相同）。
			 */
			foreach(lc1, varinfos)
				{
					varinfo = (GroupVarInfo *) lfirst(lc1);

					if (!equal(expr, varinfo->var))
						continue;

					is_duplicate = true;
					break;
				}

				if (is_duplicate)
				{
				/*
				 * 跳过完全重复的项。把它们加入 otherclauses 列表同样没有意义。
				 */
				continue;
				}

			/*
			 * 初始化 GroupVarInfo。我们只用它来调用
			 * estimate_multivariate_ndistinct()，而该函数并不关心
			 * ndistinct 和 isdefault 字段。因此，跳过这两个字段。
			 */
			varinfo = (GroupVarInfo *) palloc0(sizeof(GroupVarInfo));
				varinfo->var = expr;
				varinfo->rel = root->simple_rel_array[relid];
				varinfos = lappend(varinfos, varinfo);

			/*
			 * 记住与 RestrictInfo 的关联，以备该子句估计失败时使用。
			 */
			origin_rinfos = lappend(origin_rinfos, rinfo);
			}
			else
			{
			/* 这个子句无法用扩展统计来估计 */
			otherclauses = lappend(otherclauses, rinfo);
			}

			clauses = foreach_delete_current(clauses, lc);
		}

		if (list_length(varinfos) < 2)
		{
			/*
			 * 多变量统计不适用于单字段（表达式除外），但该功能尚未实现。
			 */
			otherclauses = list_concat(otherclauses, origin_rinfos);
			list_free_deep(varinfos);
			list_free(origin_rinfos);
			continue;
		}

		Assert(group_rel != NULL);

		/* 使用扩展统计。 */
		origin_varinfos = varinfos;
		for (;;)
		{
			bool		estimated = estimate_multivariate_ndistinct(root,
																	group_rel,
																	&varinfos,
																	&mvndistinct);

			if (!estimated)
				break;

			/*
			 * 我们已得到一个估计值。按照与调用方逻辑一致的方式使用
			 * ndistinct 值（参见 final_cost_hashjoin）。
			 */
			if (ndistinct < mvndistinct)
				ndistinct = mvndistinct;
			Assert(ndistinct >= 1.0);
		}

		Assert(list_length(origin_varinfos) == list_length(origin_rinfos));

		/* 收集未能匹配的子句，放入 otherclauses。 */
		forboth(lc1, origin_varinfos, lc2, origin_rinfos)
		{
			GroupVarInfo *vinfo = lfirst(lc1);

			if (!list_member_ptr(varinfos, vinfo))
				/* 已经估计过了 */
				continue;

			/* 无法在这里估计——推入返回列表 */
			otherclauses = lappend(otherclauses, lfirst(lc2));
		}
	}

	*innerbucketsize = 1.0 / ndistinct;
	return otherclauses;
}

/*
 * 当指定表达式被用作哈希键、且给定桶数量时，估计哈希桶的统计信息。
 *
 * 这里尝试确定两个值：
 *
 * 1. 表达式中最常见值的频率（如果无法获取，则向 *mcv_freq 返回零）。
 *
 * 2. “桶大小比例”，即桶中的平均条目数除以关系中的元组总数。
 *
 * XXX 这其实相当不靠谱，因为我们实际上假设了在施加限制子句之后，
 * 哈希键的分布与底层关系中的分布相同。然而，我们远没有聪明到能够
 * 推算出限制子句会如何改变分布，所以目前只能将就。
 *
 * 我们被传入执行器将为给定输入关系使用的桶数量。如果数据是完美分布的，
 * 即每个可用桶都放入相同数量的元组，那么桶大小比例应为 1/nbuckets。
 * 但这种理想状态只会在以下情况下出现：(a) 至少有 nbuckets 个不同数据值，
 * 并且 (b) 数据的分布不那么倾斜。否则，桶的占用就会不均匀。如果连接中的
 * 另一个关系具有与本关系相似的键分布，那么负载最重的桶恰好就是被探测得
 * 最频繁的桶。因此，出于代价估算目的，“平均”桶大小其实应当取接近
 * “最坏情况”桶大小的值。我们试图这样来估计：如果不同数据值太少，
 * 就调整该比例，然后再按最常见值的频率与平均频率之比进行放大。
 *
 * 如果没有可用的统计信息，则使用默认值 0.1 作为估计。当内表很大时，
 * 这会相当强烈地抑制哈希连接的使用，而这正是我们想要的。除非我们知道
 * 内表分布很均匀（或者其它方案明显更糟），否则我们不想用哈希连接。
 *
 * 调用方还应当检查 mcv_freq 不要大到让那个最常见值本身就需要一个
 * 大到不切实际的桶。在哈希连接中，执行器可以在桶过大时拆分它们，
 * 但对于一个包含大量同一值重复项的桶来说，这显然无济于事。
 */
void
estimate_hash_bucket_stats(PlannerInfo *root, Node *hashkey, double nbuckets,
						   Selectivity *mcv_freq,
						   Selectivity *bucketsize_frac)
{
	VariableStatData vardata;
	double		estfract,
				ndistinct,
				stanullfrac,
				avgfreq;
	bool		isdefault;
	AttStatsSlot sslot;

	examine_variable(root, hashkey, 0, &vardata);

	/* 如果可用，则查找最常见值的频率 */
	*mcv_freq = 0.0;

	if (HeapTupleIsValid(vardata.statsTuple))
	{
		if (get_attstatsslot(&sslot, vardata.statsTuple,
							 STATISTIC_KIND_MCV, InvalidOid,
							 ATTSTATSSLOT_NUMBERS))
		{
		/*
		 * 第一个 MCV 统计量对应最常见的值。
		 */
		if (sslot.nnumbers > 0)
				*mcv_freq = sslot.numbers[0];
			free_attstatsslot(&sslot);
		}
	}

	/* Get number of distinct values */
	ndistinct = get_variable_numdistinct(&vardata, &isdefault);

	/*
	 * If ndistinct isn't real, punt.  We normally return 0.1, but if the
	 * mcv_freq is known to be even higher than that, use it instead.
	 */
	if (isdefault)
	{
		*bucketsize_frac = (Selectivity) Max(0.1, *mcv_freq);
		ReleaseVariableStats(vardata);
		return;
	}

	/* Get fraction that are null */
	if (HeapTupleIsValid(vardata.statsTuple))
	{
		Form_pg_statistic stats;

		stats = (Form_pg_statistic) GETSTRUCT(vardata.statsTuple);
		stanullfrac = stats->stanullfrac;
	}
	else
		stanullfrac = 0.0;

	/* 计算原始关系中所有不同数据值的平均频率 */
	avgfreq = (1.0 - stanullfrac) / ndistinct;

	/*
	 * 调整 ndistinct 以考虑限制子句的影响。注意，我们这里假设数据分布被
	 * 限制子句均匀地改变了！
	 *
	 * XXX 可能有更好的办法，但代价高得多：乘以那些引用了目标 Var 的关系
	 * 限制子句的选择性。
	 */
	if (vardata.rel && vardata.rel->tuples > 0)
	{
		ndistinct *= vardata.rel->rows / vardata.rel->tuples;
		ndistinct = clamp_row_est(ndistinct);
	}

	/*
	 * 桶大小比例的初始估计：只要桶数量小于期望的不同值数量，就是
	 * 1/nbuckets；否则为 1/ndistinct。
	 */
	if (ndistinct > nbuckets)
		estfract = 1.0 / nbuckets;
	else
		estfract = 1.0 / ndistinct;

	/*
	 * 把估计的桶大小向上调整，以考虑倾斜分布的影响。
	 */
	if (avgfreq > 0.0 && *mcv_freq > avgfreq)
		estfract *= *mcv_freq / avgfreq;

	/*
	 * 把桶大小钳制到合理范围（上面的调整很容易产生超出范围的结果）。
	 * 我们把下界设得比零略高一点，因为零并不是一个很合理的结果。
	 */
	if (estfract < 1.0e-6)
		estfract = 1.0e-6;
	else if (estfract > 1.0)
		estfract = 1.0;

	*bucketsize_frac = (Selectivity) estfract;

	ReleaseVariableStats(vardata);
}

/*
 * estimate_hashagg_tablesize
 *	  根据 agg_costs、路径宽度和组数，估计哈希聚合的哈希表所需的字节数。
 *
 * 我们把结果以 "double" 形式返回，以避免在乘以 dNumGroups 时
 * 可能出现溢出问题。
 *
 * XXX 鉴于现在的 hashagg 已知道从哈希表中省略不需要的列，这里可能高估了
 * 大小。另外对于混合模式的 grouping sets，不在哈希集合中的分组列也会被
 * 计入，即便 hashagg 并不会存储它们。这会成为问题吗？
 */
double
estimate_hashagg_tablesize(PlannerInfo *root, Path *path,
						   const AggClauseCosts *agg_costs, double dNumGroups)
{
	Size		hashentrysize;

	hashentrysize = hash_agg_entry_size(list_length(root->aggtransinfos),
										path->pathtarget->width,
										agg_costs->transitionSpace);

	/*
	 * 注意，这里忽略了哈希表的填充因子和增长策略的影响。考虑到默认的
	 * 填充因子相对较高，这样做应该是没问题的。在这里很难有意义地把
	 * “体积翻倍”式的增长策略也纳入考虑。
	 */
	return hashentrysize * dNumGroups;
}


/*-------------------------------------------------------------------------
 *
 * 辅助例程
 *
 *-------------------------------------------------------------------------
 */

/*
 * 为给定的 GroupVarInfo 列表寻找匹配程度最高的 ndistinct 扩展统计信息。
 *
 * 调用方必须确保给定的 GroupVarInfo 全部属于 'rel'，并且 GroupVarInfo
 * 列表中不包含任何重复的 Var 或表达式。
 *
 * 当找到匹配了 1 个以上给定 GroupVarInfo 的统计信息时，*ndistinct 参数
 * 会按 ndistinct 估计值被设置，同时会构建一个新的列表，其中匹配的
 * GroupVarInfo 已被移除，并通过 *varinfos 参数在返回 true 之前输出。
 * 当找不到匹配的统计信息时，返回 false，并且 *varinfos 和 *ndistinct
 * 参数保持不变。
 */
static bool
estimate_multivariate_ndistinct(PlannerInfo *root, RelOptInfo *rel,
								List **varinfos, double *ndistinct)
{
	ListCell   *lc;
	int			nmatches_vars;
	int			nmatches_exprs;
	Oid			statOid = InvalidOid;
	MVNDistinct *stats;
	StatisticExtInfo *matched_info = NULL;
	RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);

	/* 如果表没有扩展统计信息，则立即退出 */
	if (!rel->statlist)
		return false;

	/* 寻找匹配最多 Var 的 ndistinct 统计对象 */
	nmatches_vars = 0;			/* 我们要求至少匹配两个 */
	nmatches_exprs = 0;
	foreach(lc, rel->statlist)
	{
		ListCell   *lc2;
		StatisticExtInfo *info = (StatisticExtInfo *) lfirst(lc);
		int			nshared_vars = 0;
		int			nshared_exprs = 0;

		/* 跳过其它种类的统计 */
		if (info->kind != STATS_EXT_NDISTINCT)
			continue;

		/* 跳过 stxdinherit 值不匹配的统计 */
		if (info->inherit != rte->inh)
			continue;

		/*
		 * 确定有多少个表达式（以及未匹配表达式中的变量）能够匹配。
		 * 我们随后会用这些数值来挑选与子句匹配程度最高的统计对象。
		 */
		foreach(lc2, *varinfos)
		{
			ListCell   *lc3;
			GroupVarInfo *varinfo = (GroupVarInfo *) lfirst(lc2);
			AttrNumber	attnum;

			Assert(varinfo->rel == rel);

			/* 简单 Var，直接在统计键中查找 */
			if (IsA(varinfo->var, Var))
			{
				attnum = ((Var *) varinfo->var)->varattno;

				/*
				 * 忽略系统属性——我们不支持对它们做统计，因此无法匹配
				 * （而且由于这些值是负数，匹配也会失败）。
				 */
				if (!AttrNumberIsForUserDefinedAttr(attnum))
					continue;

				if (bms_is_member(attnum, info->keys))
					nshared_vars++;

				continue;
			}

			/* 表达式——看它是否在统计对象中 */
			foreach(lc3, info->exprs)
			{
				Node	   *expr = (Node *) lfirst(lc3);

				if (equal(varinfo->var, expr))
				{
					nshared_exprs++;
					break;
				}
			}
		}

		/*
		 * ndistinct 扩展统计包含的是针对统计所定义列的成对（至少）
		 * 组合的估计，而绝不是单字段的估计。这里我们要求至少匹配两列，
		 * 否则就跳过。
		 */
		if (nshared_vars + nshared_exprs < 2)
			continue;

		/*
		 * 检查这些统计是否比之前最佳匹配更好；如果是，则记下这个
		 * StatisticExtInfo。
		 *
		 * statlist 是按 statOid 排序的，因此即便有多组统计匹配程度相同，
		 * 我们选为最佳匹配的 StatisticExtInfo 也是确定性的。
		 */
		if ((nshared_exprs > nmatches_exprs) ||
			(((nshared_exprs == nmatches_exprs)) && (nshared_vars > nmatches_vars)))
		{
			statOid = info->statOid;
			nmatches_vars = nshared_vars;
			nmatches_exprs = nshared_exprs;
			matched_info = info;
		}
	}

	/* 没有匹配？ */
	if (statOid == InvalidOid)
		return false;

	Assert(nmatches_vars + nmatches_exprs > 1);

	stats = statext_ndistinct_load(statOid, rte->inh);

	/*
	 * 如果找到了匹配，则在其中搜索那个具体的匹配项（必定存在），并
	 * 构造输出值。
	 */
	if (stats)
	{
		int			i;
		List	   *newlist = NIL;
		MVNDistinctItem *item = NULL;
		ListCell   *lc2;
		Bitmapset  *matched = NULL;
		AttrNumber	attnum_offset;

		/*
		 * attnum 需要偏移多少？如果没有表达式，则无需偏移。否则，把
		 * 偏移量设得足够大，使得最小的那个（等于表达式的个数）被移到 1。
		 */
		if (matched_info->exprs)
			attnum_offset = (list_length(matched_info->exprs) + 1);
		else
			attnum_offset = 0;

		/* 看看实际匹配了哪些 */
		foreach(lc2, *varinfos)
		{
			ListCell   *lc3;
			int			idx;
			bool		found = false;

			GroupVarInfo *varinfo = (GroupVarInfo *) lfirst(lc2);

			/*
			 * 处理简单的 Var 表达式，方法是直接把它与键匹配。如果存在一个
			 * 匹配的表达式，我们稍后会尝试匹配它。
			 */
			if (IsA(varinfo->var, Var))
			{
				AttrNumber	attnum = ((Var *) varinfo->var)->varattno;

				/*
				 * 忽略系统属性上的表达式。不能依赖 bms 检查来处理负值。
				 */
				if (!AttrNumberIsForUserDefinedAttr(attnum))
					continue;

				/* 该变量是否落在统计对象的覆盖范围内？ */
				if (!bms_is_member(attnum, matched_info->keys))
					continue;

				attnum = attnum + attnum_offset;

				/* 确保偏移量足够 */
				Assert(AttrNumberIsForUserDefinedAttr(attnum));

				matched = bms_add_member(matched, attnum);

				found = true;
			}

			/*
			 * XXX 也许我们应该允许即便找到了匹配表达式的属性，也去搜索这些
			 * 表达式？那样可以处理像 "(a)" 这样平凡的表达式，但似乎相当
			 * 没有用处。
			 */
			if (found)
				continue;

			/* 表达式——看它是否在统计对象中 */
			idx = 0;
			foreach(lc3, matched_info->exprs)
			{
				Node	   *expr = (Node *) lfirst(lc3);

				if (equal(varinfo->var, expr))
				{
					AttrNumber	attnum = -(idx + 1);

					attnum = attnum + attnum_offset;

					/* 确保偏移量足够 */
					Assert(AttrNumberIsForUserDefinedAttr(attnum));

					matched = bms_add_member(matched, attnum);

					/* 应该只存在一个匹配的表达式 */
					break;
				}

				idx++;
			}
		}

		/* Find the specific item that exactly matches the combination */
		for (i = 0; i < stats->nitems; i++)
		{
			int			j;
			MVNDistinctItem *tmpitem = &stats->items[i];

			if (tmpitem->nattributes != bms_num_members(matched))
				continue;

			/* 假设它就是正确的项 */
			item = tmpitem;

			/* 检查该项的所有属性/表达式是否都符合匹配 */
			for (j = 0; j < tmpitem->nattributes; j++)
			{
				AttrNumber	attnum = tmpitem->attributes[j];

				/*
				 * 鉴于我们上面构造 matched 位图的方式，我们可以直接用同样的
				 * 方式偏移所有 attnum。
				 */
				attnum = attnum + attnum_offset;

				if (!bms_is_member(attnum, matched))
				{
					/* 不，不是这一项 */
					item = NULL;
					break;
				}
			}

			/*
			 * 如果该项包含了所有被匹配的属性，我们就知道它是正确的那个——
			 * 不可能有更好的了。
			 */
			if (item)
				break;
		}

		/*
		 * 确保我们找到了一个项。必定存在一个，因为 ndistinct 统计包含了
		 * 属性的所有组合。
		 */
		if (!item)
			elog(ERROR, "corrupt MVNDistinct entry");

		/* 构造输出的 varinfo 列表，只保留未被匹配的项 */
		foreach(lc, *varinfos)
		{
			GroupVarInfo *varinfo = (GroupVarInfo *) lfirst(lc);
			ListCell   *lc3;
			bool		found = false;

			/*
			 * 让我们先看看普通变量，因为这是最常见的情况，而且检查开销很低。
			 * 我们只要拿到 attnum 并用（带偏移的）matched 位图检查即可。
			 */
			if (IsA(varinfo->var, Var))
			{
				AttrNumber	attnum = ((Var *) varinfo->var)->varattno;

				/*
				 * 如果是系统属性，我们就完成了。我们不支持对系统属性做扩展
				 * 统计，所以它显然没有被匹配。只需保留这个表达式并继续。
				 */
				if (!AttrNumberIsForUserDefinedAttr(attnum))
				{
					newlist = lappend(newlist, varinfo);
					continue;
				}

				/* 应用与上面相同的偏移 */
				attnum += attnum_offset;

				/* 如果它没有被匹配，则保留这个 varinfo */
				if (!bms_is_member(attnum, matched))
					newlist = lappend(newlist, varinfo);

				/* 循环剩余部分处理复杂表达式。 */
				continue;
			}

			/*
			 * 处理复杂表达式，而不仅仅是简单的 Var。
			 *
			 * 首先，我们搜索一个表达式的精确匹配。如果找到了，就可以直接
			 * 丢弃整个 GroupVarInfo，连同我们从其中提取出的所有变量。
			 *
			 * 否则，我们检查各个变量的个体情况，并尝试将其与项中的变量匹配。
			 */
			foreach(lc3, matched_info->exprs)
			{
				Node	   *expr = (Node *) lfirst(lc3);

				if (equal(varinfo->var, expr))
				{
					found = true;
					break;
				}
			}

			/* 找到精确匹配，跳过 */
			if (found)
				continue;

			newlist = lappend(newlist, varinfo);
		}

		*varinfos = newlist;
		*ndistinct = item->ndistinct;
		return true;
	}

	return false;
}

/*
 * convert_to_scalar
 *	  把指定类型的非 NULL 值转换为 scalarineqsel() 所需的比较标尺。
 *	  成功时返回 "true"。
 *
 * XXX 这个例程是个 hack：理想情况下我们应该从 pg_type 中查找转换子例程。
 *
 * 所有数值类型都被简单地转换为等价的 "double" 值。（超出 "double"
 * 范围的 NUMERIC 值会被钳制到 +/- HUGE_VAL。）
 *
 * 字符串类型由 convert_string_to_scalar() 转换，详见下文的说明。这个
 * 例程之所以一次处理三个值而不是一个，是因为对字符串来说我们需要这样。
 *
 * bytea 类型与字符串的差异足以让它必须被单独对待。
 *
 * 几种表示绝对时间的类型都被转换为 Timestamp（它实际上是一个 int64），
 * 然后我们把它提升为 double。注意，即便对于 Timestamp 的“特殊”值，
 * 这也能给出正确结果，因为这些特殊值是被特意选成能够正确比较的；
 * 参见 timestamp_cmp。
 *
 * 几种表示相对时间（interval）的类型都被转换为以秒为单位的度量。
 */
static bool
convert_to_scalar(Datum value, Oid valuetypid, Oid collid, double *scaledvalue,
				  Datum lobound, Datum hibound, Oid boundstypid,
				  double *scaledlobound, double *scaledhibound)
{
	bool		failure = false;

	/*
	 * valuetypid 和 boundstypid 都应当与我们所调用操作符的声明输入类型
	 * 完全匹配。不过，扩展可能会尝试把 scalarineqsel 用作其输入类型
	 * 我们在这里不处理的那些操作符的估计函数；在这种情况下，我们希望
	 * 返回 false 而不是失败。无论如何，我们都不能假设 valuetypid 和
	 * boundstypid 是相同的。
	 *
	 * XXX 我们插值于其间的直方图，可能属于一个仅仅与声明类型二进制兼容的
	 * 列。本质上我们是在假设：二进制兼容类型的语义足够相似，以至于我们
	 * 可以用一种类型的操作符生成的直方图去估计另一种类型的选择性。这在某些
	 * 情况下是彻头彻尾错误的——尤其是有符号与无符号解释的差异可能让我们
	 * 出错。但在大多数情况下它足够有用，所以我们仍然这样做了。应该考虑
	 * 更严谨的做法。
	 */
	switch (valuetypid)
	{
			/*
			 * 内建数值类型
			 */
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
		case OIDOID:
		case REGPROCOID:
		case REGPROCEDUREOID:
		case REGOPEROID:
		case REGOPERATOROID:
		case REGCLASSOID:
		case REGTYPEOID:
		case REGCOLLATIONOID:
		case REGCONFIGOID:
		case REGDICTIONARYOID:
		case REGROLEOID:
		case REGNAMESPACEOID:
			*scaledvalue = convert_numeric_to_scalar(value, valuetypid,
													 &failure);
			*scaledlobound = convert_numeric_to_scalar(lobound, boundstypid,
													   &failure);
			*scaledhibound = convert_numeric_to_scalar(hibound, boundstypid,
													   &failure);
			return !failure;

			/*
			 * 内建字符串类型
			 */
		case CHAROID:
		case BPCHAROID:
		case VARCHAROID:
		case TEXTOID:
		case NAMEOID:
			{
				char	   *valstr = convert_string_datum(value, valuetypid,
														  collid, &failure);
				char	   *lostr = convert_string_datum(lobound, boundstypid,
														 collid, &failure);
				char	   *histr = convert_string_datum(hibound, boundstypid,
														 collid, &failure);

				/*
				 * 如果任意值不是字符串类型，则退出。我们可能会泄漏其它
				 * 值的已转换字符串，但这不值得专门处理。
				 */
				if (failure)
					return false;

				convert_string_to_scalar(valstr, scaledvalue,
										 lostr, scaledlobound,
										 histr, scaledhibound);
				pfree(valstr);
				pfree(lostr);
				pfree(histr);
				return true;
			}

			/*
			 * 内建 bytea 类型
			 */
		case BYTEAOID:
			{
				/* 我们只支持 bytea 与 bytea 的比较 */
				if (boundstypid != BYTEAOID)
					return false;
				convert_bytea_to_scalar(value, scaledvalue,
										lobound, scaledlobound,
										hibound, scaledhibound);
				return true;
			}

			/*
			 * 内建时间类型
			 */
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case DATEOID:
		case INTERVALOID:
		case TIMEOID:
		case TIMETZOID:
			*scaledvalue = convert_timevalue_to_scalar(value, valuetypid,
													   &failure);
			*scaledlobound = convert_timevalue_to_scalar(lobound, boundstypid,
														 &failure);
			*scaledhibound = convert_timevalue_to_scalar(hibound, boundstypid,
														 &failure);
			return !failure;

			/*
			 * Built-in network types
			 */
		case INETOID:
		case CIDROID:
		case MACADDROID:
		case MACADDR8OID:
			*scaledvalue = convert_network_to_scalar(value, valuetypid,
													 &failure);
			*scaledlobound = convert_network_to_scalar(lobound, boundstypid,
													   &failure);
			*scaledhibound = convert_network_to_scalar(hibound, boundstypid,
													   &failure);
			return !failure;
	}
	/* Don't know how to convert */
	*scaledvalue = *scaledlobound = *scaledhibound = 0;
	return false;
}

/*
 * 为任意数值数据类型完成 convert_to_scalar() 的工作。
 *
 * 失败时（例如不支持的 typid），把 *failure 置为 true；否则该变量不变。
 */
static double
convert_numeric_to_scalar(Datum value, Oid typid, bool *failure)
{
	switch (typid)
	{
		case BOOLOID:
			return (double) DatumGetBool(value);
		case INT2OID:
			return (double) DatumGetInt16(value);
		case INT4OID:
			return (double) DatumGetInt32(value);
		case INT8OID:
			return (double) DatumGetInt64(value);
		case FLOAT4OID:
			return (double) DatumGetFloat4(value);
		case FLOAT8OID:
			return (double) DatumGetFloat8(value);
		case NUMERICOID:
			/* 注意：超出范围的值会被钳制到 +-HUGE_VAL */
			return (double)
				DatumGetFloat8(DirectFunctionCall1(numeric_float8_no_overflow,
												   value));
		case OIDOID:
		case REGPROCOID:
		case REGPROCEDUREOID:
		case REGOPEROID:
		case REGOPERATOROID:
		case REGCLASSOID:
		case REGTYPEOID:
		case REGCOLLATIONOID:
		case REGCONFIGOID:
		case REGDICTIONARYOID:
		case REGROLEOID:
		case REGNAMESPACEOID:
			/* 我们可以把 OID 当作整数来处理…… */
			return (double) DatumGetObjectId(value);
	}

	*failure = true;
	return 0;
}

/*
 * 为任意字符串数据类型完成 convert_to_scalar() 的工作。
 *
 * 字符串类型被转换到从 0 到 1 的标尺上，我们把字符串的字节看作小数位。
 *
 * 不过我们不希望基数取 256，因为那往往会生成过高的选择性估计；
 * 很少有数据库会在每个位置上都出现全部 256 种可能的字节值。相反，我们
 * 把边界中出现的最小和最大字节值作为每个字节的估计范围，并在做一些
 * 修正之后使用，以处理我们大概不会以那种方式看到完整范围这一事实。
 *
 * 另外一项改进是：在计算缩放值之前，我们先丢弃这三个字符串的任何公共
 * 前缀。这让我们在遇到很窄的数据范围时能够“放大”。一个例子是电话号码
 * 数据库，其中所有值都以相同的区号开头。（实际上，边界会是相邻的
 * 直方图分箱边界值，所以这比你想象的更可能发生。）
 */
static void
convert_string_to_scalar(char *value,
						 double *scaledvalue,
						 char *lobound,
						 double *scaledlobound,
						 char *hibound,
						 double *scaledhibound)
{
	int			rangelo,
				rangehi;
	char	   *sptr;

	rangelo = rangehi = (unsigned char) hibound[0];
	for (sptr = lobound; *sptr; sptr++)
	{
		if (rangelo > (unsigned char) *sptr)
			rangelo = (unsigned char) *sptr;
		if (rangehi < (unsigned char) *sptr)
			rangehi = (unsigned char) *sptr;
	}
	for (sptr = hibound; *sptr; sptr++)
	{
		if (rangelo > (unsigned char) *sptr)
			rangelo = (unsigned char) *sptr;
		if (rangehi < (unsigned char) *sptr)
			rangehi = (unsigned char) *sptr;
	}
	/* 如果范围包含任何大写 ASCII 字符，则让它包含全部 */
	if (rangelo <= 'Z' && rangehi >= 'A')
	{
		if (rangelo > 'A')
			rangelo = 'A';
		if (rangehi < 'Z')
			rangehi = 'Z';
	}
	/* 小写同理 */
	if (rangelo <= 'z' && rangehi >= 'a')
	{
		if (rangelo > 'a')
			rangelo = 'a';
		if (rangehi < 'z')
			rangehi = 'z';
	}
	/* 数字同理 */
	if (rangelo <= '9' && rangehi >= '0')
	{
		if (rangelo > '0')
			rangelo = '0';
		if (rangehi < '9')
			rangehi = '9';
	}

	/*
	 * 如果范围包含的字符少于 10 个，就假设我们掌握的数据不足，并让它
	 * 包含常规的 ASCII 字符集。
	 */
	if (rangehi - rangelo < 9)
	{
		rangelo = ' ';
		rangehi = 127;
	}

	/*
	 * 现在去掉这三个字符串的任何公共前缀。
	 */
	while (*lobound)
	{
		if (*lobound != *hibound || *lobound != *value)
			break;
		lobound++, hibound++, value++;
	}

	/*
	 * 现在可以进行转换了。
	 */
	*scaledvalue = convert_one_string_to_scalar(value, rangelo, rangehi);
	*scaledlobound = convert_one_string_to_scalar(lobound, rangelo, rangehi);
	*scaledhibound = convert_one_string_to_scalar(hibound, rangelo, rangehi);
}

static double
convert_one_string_to_scalar(char *value, int rangelo, int rangehi)
{
	int			slen = strlen(value);
	double		num,
				denom,
				base;

	if (slen <= 0)
		return 0.0;				/* 空字符串的标量值为 0 */

	/*
	 * 从字符串中考虑超过一打的字节似乎意义不大。由于基数至少为 10，这
	 * 会给我们至少 12 位十进制的名义精度，而这肯定远比这种估计技术本身
	 * 能达到的精度更高（尤其是在非 C 区域设置下）。此外，即便使用最大
	 * 可能的基数 256，这也能保证 denom 不会增长到超过 256^13 = 2.03e31，
	 * 这在任何已知的机器上都不会溢出。
	 */
	if (slen > 12)
		slen = 12;

	/* 把起始字符转换为小数 */
	base = rangehi - rangelo + 1;
	num = 0.0;
	denom = base;
	while (slen-- > 0)
	{
		int			ch = (unsigned char) *value++;

		if (ch < rangelo)
			ch = rangelo - 1;
		else if (ch > rangehi)
			ch = rangehi + 1;
		num += ((double) (ch - rangelo)) / denom;
		denom *= base;
	}

	return num;
}

/*
 * 把一个字符串类型的 Datum 转换为一个由 palloc 分配、以 null 结尾的字符串。
 *
 * 失败时（例如不支持的 typid），把 *failure 置为 true；否则该变量不变。
 * （失败时我们会返回 NULL。）
 *
 * 当使用非 C 区域设置时，我们必须先让字符串经过 pg_strxfrm() 处理，
 * 以生成正确的区域相关结果。
 */
static char *
convert_string_datum(Datum value, Oid typid, Oid collid, bool *failure)
{
	char	   *val;
	pg_locale_t mylocale;

	switch (typid)
	{
		case CHAROID:
			val = (char *) palloc(2);
			val[0] = DatumGetChar(value);
			val[1] = '\0';
			break;
		case BPCHAROID:
		case VARCHAROID:
		case TEXTOID:
			val = TextDatumGetCString(value);
			break;
		case NAMEOID:
			{
				NameData   *nm = (NameData *) DatumGetPointer(value);

				val = pstrdup(NameStr(*nm));
				break;
			}
		default:
			*failure = true;
			return NULL;
	}

	mylocale = pg_newlocale_from_collation(collid);

	if (!mylocale->collate_is_c)
	{
		char	   *xfrmstr;
		size_t		xfrmlen;
		size_t		xfrmlen2 PG_USED_FOR_ASSERTS_ONLY;

		/*
		 * XXX: 我们可以猜测一个合适的输出缓冲区大小，只有当猜测过小时
		 * 才调用两次 pg_strxfrm()。
		 *
		 * XXX: strxfrm 在 Win32 上不支持 UTF-8 编码，它可能返回伪造的数据
		 * 或设置一个错误。除非它导致崩溃，否则这其实不是问题，因为它只会
		 * 造成估计误差而非致命错误。
		 *
		 * XXX: 我们没有检查 pg_strxfrm_enabled()。在某些平台和某些情况下，
		 * libc 的 strxfrm() 可能返回错误结果，但这只会导致估计误差。
		 */
		xfrmlen = pg_strxfrm(NULL, val, 0, mylocale);
#ifdef WIN32

		/*
		 * 在 Windows 上，strxfrm 发生错误时会返回 INT_MAX。与其尝试分配
		 * 这么大一块内存（并失败），不如就像处在 C 区域设置下那样直接返回
		 * 原始的、未修改的字符串。
		 */
		if (xfrmlen == INT_MAX)
			return val;
#endif
		xfrmstr = (char *) palloc(xfrmlen + 1);
		xfrmlen2 = pg_strxfrm(xfrmstr, val, xfrmlen + 1, mylocale);

		/*
		 * Some systems (e.g., glibc) can return a smaller value from the
		 * second call than the first; thus the Assert must be <= not ==.
		 */
		Assert(xfrmlen2 <= xfrmlen);
		pfree(val);
		val = xfrmstr;
	}

	return val;
}

/*
 * 为任意 bytea 数据类型完成 convert_to_scalar() 的工作。
 *
 * 与 convert_string_to_scalar 非常相似，只是我们不能假定以 null 结尾，
 * 因此要显式传递长度。
 *
 * 另外，关于字符“正常”取值范围的假设已被去除——目前固定使用 0..255 的
 * 数据范围。（也许将来我们会把关于实际字节数据范围的信息加入
 * pg_statistic。）
 */
static void
convert_bytea_to_scalar(Datum value,
						double *scaledvalue,
						Datum lobound,
						double *scaledlobound,
						Datum hibound,
						double *scaledhibound)
{
	bytea	   *valuep = DatumGetByteaPP(value);
	bytea	   *loboundp = DatumGetByteaPP(lobound);
	bytea	   *hiboundp = DatumGetByteaPP(hibound);
	int			rangelo,
				rangehi,
				valuelen = VARSIZE_ANY_EXHDR(valuep),
				loboundlen = VARSIZE_ANY_EXHDR(loboundp),
				hiboundlen = VARSIZE_ANY_EXHDR(hiboundp),
				i,
				minlen;
	unsigned char *valstr = (unsigned char *) VARDATA_ANY(valuep);
	unsigned char *lostr = (unsigned char *) VARDATA_ANY(loboundp);
	unsigned char *histr = (unsigned char *) VARDATA_ANY(hiboundp);

	/*
	 * 假设 bytea 数据在所有字节值上均匀分布。
	 */
	rangelo = 0;
	rangehi = 255;

	/*
	 * 现在去掉这三个字符串的任何公共前缀。
	 */
	minlen = Min(Min(valuelen, loboundlen), hiboundlen);
	for (i = 0; i < minlen; i++)
	{
		if (*lostr != *histr || *lostr != *valstr)
			break;
		lostr++, histr++, valstr++;
		loboundlen--, hiboundlen--, valuelen--;
	}

	/*
	 * 现在可以进行转换了。
	 */
	*scaledvalue = convert_one_bytea_to_scalar(valstr, valuelen, rangelo, rangehi);
	*scaledlobound = convert_one_bytea_to_scalar(lostr, loboundlen, rangelo, rangehi);
	*scaledhibound = convert_one_bytea_to_scalar(histr, hiboundlen, rangelo, rangehi);
}

static double
convert_one_bytea_to_scalar(unsigned char *value, int valuelen,
							int rangelo, int rangehi)
{
	double		num,
				denom,
				base;

	if (valuelen <= 0)
		return 0.0;				/* 空 bytea 的标量值为 0 */

	/*
	 * 由于基数为 256，没必要考虑超过大约 10 个字符（即便这么多似乎也
	 * 有些多余）
	 */
	if (valuelen > 10)
		valuelen = 10;

	/* 把起始字符转换为小数 */
	base = rangehi - rangelo + 1;
	num = 0.0;
	denom = base;
	while (valuelen-- > 0)
	{
		int			ch = *value++;

		if (ch < rangelo)
			ch = rangelo - 1;
		else if (ch > rangehi)
			ch = rangehi + 1;
		num += ((double) (ch - rangelo)) / denom;
		denom *= base;
	}

	return num;
}

/*
 * 为任意时间值数据类型完成 convert_to_scalar() 的工作。
 *
 * 失败时（例如不支持的 typid），把 *failure 置为 true；否则该变量不变。
 */
static double
convert_timevalue_to_scalar(Datum value, Oid typid, bool *failure)
{
	switch (typid)
	{
		case TIMESTAMPOID:
			return DatumGetTimestamp(value);
		case TIMESTAMPTZOID:
			return DatumGetTimestampTz(value);
		case DATEOID:
			return date2timestamp_no_overflow(DatumGetDateADT(value));
		case INTERVALOID:
			{
				Interval   *interval = DatumGetIntervalP(value);

				/*
				 * 把 Interval 的月份部分按假设的平均月长 365.25/12.0 天
				 * 转换为天。不够精确，但对我们的用途来说已经足够好了。
				 *
				 * 这对无限 interval 同样有效——它只是把所有字段都设为
				 * INT_MIN/INT_MAX，因此会产生比任何有限 interval 更小/更
				 * 大的结果。
				 */
				return interval->time + interval->day * (double) USECS_PER_DAY +
					interval->month * ((DAYS_PER_YEAR / (double) MONTHS_PER_YEAR) * USECS_PER_DAY);
			}
		case TIMEOID:
			return DatumGetTimeADT(value);
		case TIMETZOID:
			{
				TimeTzADT  *timetz = DatumGetTimeTzADTP(value);

				/* 使用相当于 GMT 的时间 */
				return (double) (timetz->time + (timetz->zone * 1000000.0));
			}
	}

	*failure = true;
	return 0;
}


/*
 * get_restriction_variable
 *		检查一个限制子句的参数，看它是否形如
 *		（变量 op 伪常量）或（伪常量 op 变量），
 *		其中“变量”可以是一个 Var，也可以是某个单关系上的、由 Var 构成的
 *		表达式。如果是，则提取关于该变量的信息，并指出它在哪一侧，以及
 *		另一个参数是什么。
 *
 * 输入：
 *	root: 规划器信息
 *	args: 子句参数列表
 *	varRelid: 参见限制选择性函数的规范说明
 *
 * 输出：（仅在返回 true 时才有效）
 *	*vardata: 取得关于变量的信息（参见 examine_variable）
 *	*other: 取得子句的另一参数，积极地化简为一个常量
 *	*varonleft: 若变量在左侧则置 true，在右侧则置 false
 *
 * 如果识别出一个变量则返回 true，否则返回 false。
 *
 * 注意：如果子句两侧都有 Var，我们必须失败，因为调用方期望另一侧
 * 表现得像个伪常量。
 */
bool
get_restriction_variable(PlannerInfo *root, List *args, int varRelid,
						 VariableStatData *vardata, Node **other,
						 bool *varonleft)
{
	Node	   *left,
			   *right;
	VariableStatData rdata;

	/* 若不是二元操作符子句则失败（大概不该发生） */
	if (list_length(args) != 2)
		return false;

	left = (Node *) linitial(args);
	right = (Node *) lsecond(args);

	/*
	 * 检查两侧。注意，当 varRelid 非零时，其它关系的 Var 会被当作伪常量。
	 */
	examine_variable(root, left, varRelid, vardata);
	examine_variable(root, right, varRelid, &rdata);

	/*
	 * 如果一侧是变量而另一侧不是，我们就成功了。
	 */
	if (vardata->rel && rdata.rel == NULL)
	{
		*varonleft = true;
		*other = estimate_expression_value(root, rdata.var);
		/* 这里假定无需调用 ReleaseVariableStats(rdata) */
		return true;
	}

	if (vardata->rel == NULL && rdata.rel)
	{
		*varonleft = false;
		*other = estimate_expression_value(root, vardata->var);
		/* 这里假定无需调用 ReleaseVariableStats(*vardata) */
		*vardata = rdata;
		return true;
	}

	/* 糟糕，子句结构不对（大概是 var op var） */
	ReleaseVariableStats(*vardata);
	ReleaseVariableStats(rdata);

	return false;
}

/*
 * get_join_variables
 *		对连接子句的每一侧调用 examine_variable()。同时，尝试判断该连接子句
 *		与 SpecialJoinInfo 相比，方向是相同还是相反。
 *
 * 如果连接子句形如 "lhs_var OP rhs_var"，我们视其为“正常”；若形如
 * "rhs_var OP lhs_var"，则视为“相反”。在无法确定而情况复杂时，我们
 * 默认假设它是正常的。
 */
void
get_join_variables(PlannerInfo *root, List *args, SpecialJoinInfo *sjinfo,
				   VariableStatData *vardata1, VariableStatData *vardata2,
				   bool *join_is_reversed)
{
	Node	   *left,
			   *right;

	if (list_length(args) != 2)
		elog(ERROR, "join operator should take two arguments");

	left = (Node *) linitial(args);
	right = (Node *) lsecond(args);

	examine_variable(root, left, 0, vardata1);
	examine_variable(root, right, 0, vardata2);

	if (vardata1->rel &&
		bms_is_subset(vardata1->rel->relids, sjinfo->syn_righthand))
		*join_is_reversed = true;	/* var1 在右侧 (RHS) */
	else if (vardata2->rel &&
			 bms_is_subset(vardata2->rel->relids, sjinfo->syn_lefthand))
		*join_is_reversed = true;	/* var2 在左侧 (LHS) */
	else
		*join_is_reversed = false;
}

/* statext_expressions_load 会复制该元组，所以这里只需 pfree 它即可。 */
static void
ReleaseDummy(HeapTuple tuple)
{
	pfree(tuple);
}

/*
 * examine_variable
 *		尝试查找关于某个表达式的统计信息。填充一个 VariableStatData
 *		结构来描述该表达式。
 *
 * 输入：
 *	root: 规划器信息
 *	node: 待检查的表达式树
 *	varRelid: 参见限制选择性函数的规范说明
 *
 * 输出：*vardata 按如下方式填充：
 *	var: 输入表达式（如果它本身是或包含变量，则去掉任何 phv 或二进制
 *		relabeling；否则保持不变）
 *	rel: 包含该变量的关系的 RelOptInfo；如果表达式不含任何 Var 则为 NULL
 *		（注意：这可能指向一个子查询的 RelOptInfo，而非当前查询中的）。
 *	statsTuple: 该变量的 pg_statistic 项，若存在则为其；否则为 NULL。
 *	freefunc: 用于释放 statsTuple 的函数指针。
 *	vartype: 表达式的暴露类型；这应当始终与我们正在估计的操作符的声明
 *		输入类型相匹配。
 *	atttype, atttypmod: “var”表达式的实际类型/ typmod。这通常与变量参数
 *		的暴露类型相同，但在二进制兼容类型的情况下可能不同。
 *	isunique: 如果我们能把该 var 匹配到唯一索引、单列 DISTINCT 或 GROUP-BY
 *		子句，则为 true，这意味着它的取值在本查询中是唯一的。（注意：这只
 *		应出于统计目的而信任，因为我们既不检查 indimmediate，也不验证所
 *		用的相等定义是否完全相同。）
 *	acl_ok: 如果当前用户拥有读取 pg_statistic 项底层列的所有表行的权限，
 *		则为 true。statistic_proc_security_check() 会查阅它。
 *
 * 调用方负责在退出前调用 ReleaseVariableStats()。
 */
void
examine_variable(PlannerInfo *root, Node *node, int varRelid,
				 VariableStatData *vardata)
{
	Node	   *basenode;
	Relids		varnos;
	Relids		basevarnos;
	RelOptInfo *onerel;

	/* 确保不会在 vardata 中返回悬空指针 */
	MemSet(vardata, 0, sizeof(VariableStatData));

	/* 保存表达式的暴露类型 */
	vardata->vartype = exprType(node);

	/*
	 * 就查找统计信息而言，PlaceHolderVar 是透明的；它们不会改变底层表达式
	 * 的取值分布。然而，它们可能掩盖结构，使我们无法识别出与基列、索引
	 * 表达式或扩展统计的匹配。所以先把它们剥离出去。
	 */
	basenode = strip_all_phvs_deep(root, node);

	/*
	 * 查看任何二进制兼容的 relabeling 内部。我们需要在这里处理嵌套的
	 * RelabelType 节点，因为前面剥离 PlaceHolderVar 后，可能把独立的
	 * RelabelType 节点变为了相邻。
	 */
	while (IsA(basenode, RelabelType))
		basenode = (Node *) ((RelabelType *) basenode)->arg;

	/* 简单 Var 的快速路径 */
	if (IsA(basenode, Var) &&
		(varRelid == 0 || varRelid == ((Var *) basenode)->varno))
	{
		Var		   *var = (Var *) basenode;

		/* 设置除统计元组以外的其它结果字段 */
		vardata->var = basenode;	/* 返回去掉 phv 和 relabeling 的 Var */
		vardata->rel = find_base_rel(root, var->varno);
		vardata->atttype = var->vartype;
		vardata->atttypmod = var->vartypmod;
		vardata->isunique = has_unique_index(vardata->rel, var->varattno);

		/* 尝试定位一些统计信息 */
		examine_simple_variable(root, var, vardata);

		return;
	}

	/*
	 * 好，这是一个更复杂的表达式。确定变量的归属。注意，当 varRelid
	 * 非零时，只有该关系的 Var 才被视为“真正的” Var。
	 */
	varnos = pull_varnos(root, basenode);
	basevarnos = bms_difference(varnos, root->outer_join_rels);

	onerel = NULL;

	if (bms_is_empty(basevarnos))
	{
		/* 完全没有 Var……必定是伪常量子句 */
	}
	else
	{
		int			relid;

		/* 检查表达式是否位于单个基关系的 Var 中 */
		if (bms_get_singleton_member(basevarnos, &relid))
		{
			if (varRelid == 0 || varRelid == relid)
			{
				onerel = find_base_rel(root, relid);
				vardata->rel = onerel;
				node = basenode;	/* 去掉任何 phv 或 relabeling */
			}
			/* 否则把它当作常量 */
		}
		else
		{
			/* varnos 包含多个 relid */
			if (varRelid == 0)
			{
				/* 把它当作连接关系的一个变量 */
				vardata->rel = find_join_rel(root, varnos);
				node = basenode;	/* 去掉任何 phv 或 relabeling */
			}
			else if (bms_is_member(varRelid, varnos))
			{
				/* 忽略属于其它关系的 Var */
				vardata->rel = find_base_rel(root, varRelid);
				node = basenode;	/* 去掉任何 phv 或 relabeling */
				/* 注意：这里没有表达式索引搜索的意义 */
			}
			/* 否则把它当作常量 */
		}
	}

	bms_free(basevarnos);

	vardata->var = node;
	vardata->atttype = exprType(node);
	vardata->atttypmod = exprTypmod(node);

	if (onerel)
	{
	/*
	 * 我们有一个由单关系 Var 构成的表达式。尝试把它与表达式索引列匹配，
	 * 希望能够找到一些统计信息。
	 *
	 * 注意，我们考虑所有索引列，包括 INCLUDE 列，因为这类列上可能有
	 * 统计信息。但对唯一性的检查需要更谨慎。
	 *
	 * XXX 有可能存在多个匹配、但对应不同索引操作符族的情况；如果是这样，
	 * 我们需要挑选一个与我们正在估计的操作符匹配的。以后再修（FIXME）。
	 */
	ListCell   *ilist;
		ListCell   *slist;

		/*
		 * 表达式中的 nullingrels 位可能会妨碍我们把它与表达式索引列或
		 * 扩展统计中的表达式匹配。所以先把它们剥离出去。
		 */
		if (bms_overlap(varnos, root->outer_join_rels))
			node = remove_nulling_relids(node, root->outer_join_rels, NULL);

		foreach(ilist, onerel->indexlist)
		{
			IndexOptInfo *index = (IndexOptInfo *) lfirst(ilist);
			ListCell   *indexpr_item;
			int			pos;

		indexpr_item = list_head(index->indexprs);
		if (indexpr_item == NULL)
			continue;		/* 这里没有表达式…… */

			for (pos = 0; pos < index->ncolumns; pos++)
			{
				if (index->indexkeys[pos] == 0)
				{
					Node	   *indexkey;

					if (indexpr_item == NULL)
						elog(ERROR, "too few entries in indexprs list");
					indexkey = (Node *) lfirst(indexpr_item);
					if (indexkey && IsA(indexkey, RelabelType))
						indexkey = (Node *) ((RelabelType *) indexkey)->arg;
					if (equal(node, indexkey))
					{
					/*
					 * 找到了匹配……它是唯一索引吗？这里的检查应当与
					 * has_unique_index() 一致。
					 */
					if (index->unique &&
							index->nkeycolumns == 1 &&
							pos == 0 &&
							(index->indpred == NIL || index->predOK))
							vardata->isunique = true;

					/*
					 * 它有统计信息吗？我们只考虑非部分索引的统计，因为
					 * 部分索引大概不能反映整个关系的统计；上面关于唯一性的
					 * 检查是我们从部分索引获取的唯一信息。
					 *
					 * 不过，索引统计钩子必须自己决定如何处理部分索引。
					 */
					if (get_index_stats_hook &&
						(*get_index_stats_hook) (root, index->indexoid,
												 pos + 1, vardata))
					{
						/*
						 * 钩子接管了获取统计元组的职责。如果它确实提供了
						 * 元组，那它最好也提供一个 freefunc。
						 */
						if (HeapTupleIsValid(vardata->statsTuple) &&
							!vardata->freefunc)
								elog(ERROR, "no function provided to release variable stats with");
						}
						else if (index->indpred == NIL)
						{
							vardata->statsTuple =
								SearchSysCache3(STATRELATTINH,
												ObjectIdGetDatum(index->indexoid),
												Int16GetDatum(pos + 1),
												BoolGetDatum(false));
							vardata->freefunc = ReleaseSysCache;

							if (HeapTupleIsValid(vardata->statsTuple))
							{
							/*
							 * 测试用户是否有权限访问该索引表的所有行。
							 *
							 * 为简单起见，我们要求整张表都是可查询的，而
							 * 不是去判断该索引依赖于哪些列。
							 *
							 * 注意，对于继承子表，权限是在继承的根父表上检查
							 * 的，而父表上整表可查询的权限并不能完全保证用户
							 * 能读到子表的所有列。但实际上，允许访问表达式
							 * 索引的统计信息不太可能造成任何值得关注的安全
							 * 违规，因此我们仍然允许。更多说明参见
							 * examine_simple_variable() 中的类似代码。
							 */
							vardata->acl_ok =
									all_rows_selectable(root,
														index->rel->relid,
														NULL);
							}
						else
						{
							/* 抑制后续对 leakproofness 的检查 */
							vardata->acl_ok = true;
						}
						}
						if (vardata->statsTuple)
							break;
					}
					indexpr_item = lnext(index->indexprs, indexpr_item);
				}
			}
			if (vardata->statsTuple)
				break;
		}

		/*
		 * 在扩展统计中搜索包含匹配表达式的那一个。可能存在多个，所以这里
		 * 只取第一个。将来，我们或许会考虑统计目标（并挑选最精确的统计），
		 * 以及其它一些参数。
		 */
		foreach(slist, onerel->statlist)
		{
			StatisticExtInfo *info = (StatisticExtInfo *) lfirst(slist);
			RangeTblEntry *rte = planner_rt_fetch(onerel->relid, root);
			ListCell   *expr_item;
			int			pos;

			/*
			 * 一旦已经为该表达式找到统计信息（无论是来自扩展统计，还是来自
			 * 前面循环中的索引），就停止。
			 */
			if (vardata->statsTuple)
				break;

			/* 跳过没有逐表达式统计的统计对象 */
			if (info->kind != STATS_EXT_EXPRESSIONS)
				continue;

			/* 跳过 stxdinherit 值不匹配的统计 */
			if (info->inherit != rte->inh)
				continue;

			pos = 0;
			foreach(expr_item, info->exprs)
			{
				Node	   *expr = (Node *) lfirst(expr_item);

				Assert(expr);

			/* 比较前先去掉 RelabelType */
			if (expr && IsA(expr, RelabelType))
				expr = (Node *) ((RelabelType *) expr)->arg;

			/* 找到匹配，看看能否提取出 pg_statistic 行 */
			if (equal(node, expr))
				{
				/*
				 * XXX 不确定是否应该把这个元组缓存到某处。目前我们只是
				 * 每次都新建一份副本。
				 */
				vardata->statsTuple =
						statext_expressions_load(info->statOid, rte->inh, pos);

					/* Nothing to release if no data found */
					if (vardata->statsTuple != NULL)
					{
						vardata->freefunc = ReleaseDummy;
					}

					/*
					 * 测试用户是否有权限访问该表的所有行。
					 *
					 * 为简单起见，我们要求整张表都是可查询的，而不是去判断
					 * 统计对象依赖于哪些列。
					 *
					 * 注意，对于继承子表，权限是在继承的根父表上检查的，而
					 * 父表上整表可查询的权限并不能完全保证用户能读到子表的
					 * 所有列。但实际上，允许访问表达式统计信息不太可能造成
					 * 任何值得关注的安全违规，因此我们仍然允许。更多说明
					 * 参见 examine_simple_variable() 中的类似代码。
					 */
					vardata->acl_ok = all_rows_selectable(root,
														  onerel->relid,
														  NULL);

					break;
				}

				pos++;
			}
		}
	}

	bms_free(varnos);
}

/*
 * strip_all_phvs_deep
 *		深度剥离一个表达式中的所有 PlaceHolderVar。

 * 作为一种性能优化，我们首先用一个轻量的 walker 检查是否存在任何
 * PlaceHolderVar。只有在确实找到了 PlaceHolderVar 时，才会调用开销较大的
 * mutator，从而在没有 PlaceHolderVar 的常见情况下避免不必要的内存分配
 * 和树拷贝。
 */
static Node *
strip_all_phvs_deep(PlannerInfo *root, Node *node)
{
	/* 如果任何地方都没有 PHV，我们就无需费力 */
	if (root->glob->lastPHId == 0)
		return node;

	if (!contain_placeholder_walker(node, NULL))
		return node;
	return strip_all_phvs_mutator(node, NULL);
}

/*
 * contain_placeholder_walker
 *		轻量 walker，用于检查一个表达式是否包含任何 PlaceHolderVar
 */
static bool
contain_placeholder_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, PlaceHolderVar))
		return true;

	return expression_tree_walker(node, contain_placeholder_walker, context);
}

/*
 * strip_all_phvs_mutator
 *		深度剥离所有 PlaceHolderVar 的 mutator
 */
static Node *
strip_all_phvs_mutator(Node *node, void *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, PlaceHolderVar))
	{
		/* 剥离它，并递归处理它包含的表达式 */
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		return strip_all_phvs_mutator((Node *) phv->phexpr, context);
	}

	return expression_tree_mutator(node, strip_all_phvs_mutator, context);
}

/*
 * examine_simple_variable
 *		为 examine_variable 处理一个简单的 Var
 *
 * 把它拆成子例程，是为了让我们能够递归处理引用了子查询（无论是
 * FROM 中的子 SELECT，还是 CTE 风格）的 Var。
 *
 * 除了统计元组外，*vardata 的所有字段我们都已经填好了。
 */
static void
examine_simple_variable(PlannerInfo *root, Var *var,
						VariableStatData *vardata)
{
	RangeTblEntry *rte = root->simple_rte_array[var->varno];

	Assert(IsA(rte, RangeTblEntry));

	if (get_relation_stats_hook &&
		(*get_relation_stats_hook) (root, rte, var->varattno, vardata))
	{
		/*
		 * 钩子接管了获取统计元组的职责。如果它确实提供了元组，那它最好
		 * 也提供了一个 freefunc。
		 */
		if (HeapTupleIsValid(vardata->statsTuple) &&
			!vardata->freefunc)
			elog(ERROR, "no function provided to release variable stats with");
	}
	else if (rte->rtekind == RTE_RELATION)
	{
		/*
		 * 普通表，或继承 appendrel 的父表，因此在 pg_statistic 中查找该列
		 */
		vardata->statsTuple = SearchSysCache3(STATRELATTINH,
											  ObjectIdGetDatum(rte->relid),
											  Int16GetDatum(var->varattno),
											  BoolGetDatum(rte->inh));
		vardata->freefunc = ReleaseSysCache;

		if (HeapTupleIsValid(vardata->statsTuple))
		{
			/*
			 * 测试用户是否有权限读取该列的所有行。
			 *
			 * 这要求用户具备相应的 SELECT 权限，且不存在来自 security
			 * barrier 视图或 RLS 策略的 securityQual。若不满足，则我们
			 * 只允许把 pg_statistic 数据传给 leakproof 函数，否则这些函数
			 * 可能会泄露用户无权查看的数据——参见
			 * statistic_proc_security_check()。
			 */
			vardata->acl_ok =
				all_rows_selectable(root, var->varno,
									bms_make_singleton(var->varattno - FirstLowInvalidHeapAttributeNumber));
		}
		else
		{
			/* 抑制后续任何可能的 leakproofness 检查 */
			vardata->acl_ok = true;
		}
	}
	else if ((rte->rtekind == RTE_SUBQUERY && !rte->inh) ||
			 (rte->rtekind == RTE_CTE && !rte->self_reference))
	{
		/*
		 * 普通子查询（不是被转换为 appendrel 的那种），或非递归 CTE。
		 * 这两种情况下，我们都可尝试找出该 Var 在子查询内引用的是什么。
		 * 对于 appendrel 和递归 CTE 的情况我们跳过，因为即便找到了列
		 * 统计，大概也并不十分相关。
		 */
		PlannerInfo *subroot;
		Query	   *subquery;
		List	   *subtlist;
		TargetEntry *ste;

		/*
		 * 如果是 whole-row var 而不是普通的列引用，则放弃。
		 */
		if (var->varattno == InvalidAttrNumber)
			return;

		/*
		 * 否则，找到子查询的规划器 subroot。
		 */
		if (rte->rtekind == RTE_SUBQUERY)
		{
			RelOptInfo *rel;

			/*
			 * 获取子查询的 RelOptInfo。注意，我们不会改变 vardata 中返回的
			 * rel，因为调用方期望它是调用方查询层级的 rel。由于我们可能
			 * 已经在递归中了，因此也不能使用那个 rel 指针，而必须重新查找
			 * 该 Var 的 rel。
			 */
			rel = find_base_rel(root, var->varno);

			subroot = rel->subroot;
		}
		else
		{
			/* CTE 的情况更棘手 */
			PlannerInfo *cteroot;
			Index		levelsup;
			int			ndx;
			int			plan_id;
			ListCell   *lc;

			/*
			 * 找到被引用的 CTE，并定位之前为它创建的 subroot。
			 */
			levelsup = rte->ctelevelsup;
			cteroot = root;
			while (levelsup-- > 0)
			{
				cteroot = cteroot->parent_root;
				if (!cteroot)	/* 不应发生 */
					elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
			}

			/*
			 * 注意：如果我们仍在规划 CTE（即，这是来自另一个 CTE 的侧向
			 * 引用），cte_plan_ids 可能比 cteList 更短。所以我们这里不能用
			 * forboth。
			 */
			ndx = 0;
			foreach(lc, cteroot->parse->cteList)
			{
				CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

				if (strcmp(cte->ctename, rte->ctename) == 0)
					break;
				ndx++;
			}
			if (lc == NULL)		/* 不应发生 */
				elog(ERROR, "could not find CTE \"%s\"", rte->ctename);
			if (ndx >= list_length(cteroot->cte_plan_ids))
				elog(ERROR, "could not find plan for CTE \"%s\"", rte->ctename);
			plan_id = list_nth_int(cteroot->cte_plan_ids, ndx);
			if (plan_id <= 0)
				elog(ERROR, "no plan was made for CTE \"%s\"", rte->ctename);
			subroot = list_nth(root->glob->subroots, plan_id - 1);
		}

		/* 如果子查询尚未被规划，我们只能放弃 */
		if (subroot == NULL)
			return;
		Assert(IsA(subroot, PlannerInfo));

		/*
		 * 我们必须使用被规划器改动过的子查询 parsetree，而不是来自 RTE 的
		 * 原始版本，因为我们需要一个能引用 subroot 的活动 RelOptInfo 的
		 * Var。例如，如果规划过程中发生了任何子查询上拉，目标列表中的 Var
		 * 可能已被替换，而我们需要看到那些替换后的表达式。
		 */
		subquery = subroot->parse;
		Assert(IsA(subquery, Query));

		/*
		 * 如果子查询使用了集合操作或分组集，则放弃，因为它们会把底层列的
		 * 统计搅得面目全非。（集合操作尤其讨厌；如果我们硬来，只会返回
		 * 只与最左侧子查询相关的统计……）DISTINCT 同样成问题，但我们稍后
		 * 再检查它，因为即便在有 DISTINCT 的情况下也仍有可能了解到一些信息。
		 */
		if (subquery->setOperations ||
			subquery->groupingSets)
			return;

		/* 获取上层 Var 所引用的子查询输出表达式 */
		if (subquery->returningList)
			subtlist = subquery->returningList;
		else
			subtlist = subquery->targetList;
		ste = get_tle_by_resno(subtlist, var->varattno);
		if (ste == NULL || ste->resjunk)
			elog(ERROR, "subquery %s does not have attribute %d",
				 rte->eref->aliasname, var->varattno);
		var = (Var *) ste->expr;

		/*
		 * 如果子查询使用了 DISTINCT，我们就无法利用该变量的任何统计……
		 * 但如果它是唯一的 DISTINCT 列，我们则有权认为它是唯一的。我们
		 * 这样测试，是为了让它在涉及 DISTINCT ON 的情况下也能生效。
		 */
		if (subquery->distinctClause)
		{
			if (list_length(subquery->distinctClause) == 1 &&
				targetIsInSortList(ste, InvalidOid, subquery->distinctClause))
				vardata->isunique = true;
			/* 无法继续深入 */
			return;
		}

		/* 与 DISTINCT 子句同样的思路，对 GROUP-BY 同样适用 */
		if (subquery->groupClause)
		{
			if (list_length(subquery->groupClause) == 1 &&
				targetIsInSortList(ste, InvalidOid, subquery->groupClause))
				vardata->isunique = true;
			/* 无法继续深入 */
			return;
		}

		/*
		 * 如果子查询源自带有 security_barrier 属性的视图，我们就不能查看
		 * 该变量的统计信息，不过注意到 DISTINCT 子句的存在似乎并无大碍。
		 * 所以到此为止。
		 *
		 * 这个限制可能比必要的更严格；对于选择性估计函数（它本身就是 C
		 * 函数，因而无论如何都无所不能）来说，查看统计信息当然没问题。
		 * 但许多选择性估计函数会很乐意*调用操作符函数*来试图得出一个好的
		 * 估计——这就不行了。所以目前，我们不深入去挖掘统计信息。
		 */
		if (rte->security_barrier)
			return;

		/* 只能处理子查询查询层级上的简单 Var */
		if (var && IsA(var, Var) &&
			var->varlevelsup == 0)
		{
			/*
			 * 好，递归进入子查询。注意，此时 vardata->isunique 的原始设置
			 * （必然为 false）保持不变。这正是我们想要的，因为即便底层列是
			 * 唯一的，子查询也可能以某种方式连接了其它表，从而产生重复。
			 */
			examine_simple_variable(subroot, var, vardata);
		}
	}
	else
	{
		/*
		 * 否则，该 Var 来自 FUNCTION 或 VALUES 类型的 RTE。（这里不会看到
		 * RTE_JOIN，因为连接别名 Var 已经被展平了。）对于函数的输出我们
		 * 能做的非常有限，但也许将来会对 VALUES 变得更聪明些。
		 */
	}
}

/*
 * all_rows_selectable
 *		测试用户是否有权限从一个给定关系中选取所有行。
 *
 * 输入：
 *	root: 规划器信息
 *	varno: 该关系的索引（假定为 RTE_RELATION）
 *	varattnos: 需要权限的属性集合；若需要整表访问则为 NULL
 *
 * 如果用户具备所需的 SELECT 权限，并且不存在来自 security barrier 视图
 * 或 RLS 策略的 securityQual，则返回 true。
 *
 * 注意，如果该关系是继承子关系，securityQual 和访问权限是针对继承根父表
 * （即查询中实际提及的关系）检查的——参见 expand_single_inheritance_child()
 * 中的注释，了解为何必须这样做。
 *
 * 如果 varattnos 非 NULL，其属性号应当按 FirstLowInvalidHeapAttributeNumber
 * 偏移，以便能够检查系统属性。如果 varattnos 为 NULL，则只检查表级 SELECT
 * 权限，而不检查任何列级权限。
 *
 * 注意：如果关系是经由视图访问的，这个函数实际测试的是视图所有者是否
 * 有权限从该关系选取数据。为了确保当前用户也有权限，还需要检查当前用户
 * 是否有权限从视图选取数据，那是在规划器启动时完成的——参见
 * subquery_planner()。
 *
 * 此函数被导出，以便其它估计函数可以使用它。
 */
bool
all_rows_selectable(PlannerInfo *root, Index varno, Bitmapset *varattnos)
{
	RelOptInfo *rel = find_base_rel_noerr(root, varno);
	RangeTblEntry *rte = planner_rt_fetch(varno, root);
	Oid			userid;
	int			varattno;

	Assert(rte->rtekind == RTE_RELATION);

	/*
	 * Determine the user ID to use for privilege checks (either the current
	 * user or the view owner, if we're accessing the table via a view).
	 *
	 * Normally the relation will have an associated RelOptInfo from which we
	 * can find the userid, but it might not if it's a RETURNING Var for an
	 * INSERT target relation.  In that case use the RTEPermissionInfo
	 * associated with the RTE.
	 *
	 * If we navigate up to a parent relation, we keep using the same userid,
	 * since it's the same in all relations of a given inheritance tree.
	 */
	if (rel)
		userid = rel->userid;
	else
	{
		RTEPermissionInfo *perminfo;

		perminfo = getRTEPermissionInfo(root->parse->rteperminfos, rte);
		userid = perminfo->checkAsUser;
	}
	if (!OidIsValid(userid))
		userid = GetUserId();

	/*
	 * 权限和 securityQual 必须在查询中实际提及的表上检查，所以如果这是
	 * 一个继承子表，就需要向上找到继承根父表。如果用户能在根父表上读到
	 * 整张表或所需的列，那么他们也能从子表读到数据。对于逐列检查，我们
	 * 必须弄清楚子关系的各属性对应根父表的哪些属性。
	 */
	if (root->append_rel_array != NULL)
	{
		AppendRelInfo *appinfo;

		appinfo = root->append_rel_array[varno];

		/*
		 * 分区被映射到它们的直接父表而非根父表，因此必须准备向上遍历多个
		 * AppendRelInfo。但如果遇到一个不是 RTE_RELATION 的父表则停止——
		 * 那是一个被展平的 UNION ALL 子查询，而非继承父表。
		 */
		while (appinfo &&
			   planner_rt_fetch(appinfo->parent_relid,
								root)->rtekind == RTE_RELATION)
		{
			Bitmapset  *parent_varattnos = NULL;

			/*
			 * 对于每个子表属性，找到对应的父表属性。在极少数情况下，该属性
			 * 可能只属于子表本地，此时我们只能接受无法访问这一列的现实。
			 */
			varattno = -1;
			while ((varattno = bms_next_member(varattnos, varattno)) >= 0)
			{
				AttrNumber	attno;
				AttrNumber	parent_attno;

				attno = varattno + FirstLowInvalidHeapAttributeNumber;

				if (attno == InvalidAttrNumber)
				{
					/*
					 * whole-row 引用，因此必须把子表的每一列都映射到父表。
					 */
					for (attno = 1; attno <= appinfo->num_child_cols; attno++)
					{
						parent_attno = appinfo->parent_colnos[attno - 1];
						if (parent_attno == 0)
							return false;	/* attr is local to child */
						parent_varattnos =
							bms_add_member(parent_varattnos,
										   parent_attno - FirstLowInvalidHeapAttributeNumber);
					}
				}
				else
				{
						if (attno < 0)
						{
							/* 系统属性号在所有表中都相同 */
							parent_attno = attno;
						}
						else
						{
							if (attno > appinfo->num_child_cols)
								return false;	/* 安全性检查 */
							parent_attno = appinfo->parent_colnos[attno - 1];
							if (parent_attno == 0)
								return false;	/* 属性只属于子表本地 */
						}
					parent_varattnos =
						bms_add_member(parent_varattnos,
									   parent_attno - FirstLowInvalidHeapAttributeNumber);
				}
			}

		/* 如果父表本身也是子表，则继续向上 */
		varno = appinfo->parent_relid;
		varattnos = parent_varattnos;
		appinfo = root->append_rel_array[varno];
	}

	/* 在这个父关系上执行访问检查 */
	rte = planner_rt_fetch(varno, root);
		Assert(rte->rtekind == RTE_RELATION);
	}

	/*
	 * 要让所有行都可读，就不能存在来自 security barrier 视图或 RLS 策略的
	 * securityQual。
	 */
	if (rte->securityQuals != NIL)
		return false;

	/*
	 * 检查表级 SELECT 权限。
	 *
	 * 如果 varattnos 非 NULL，这就足以访问所有被请求的属性，即便是子表
	 * 也不例外，因为我们已经验证过所有必需的子表列都有对应的父表列。
	 *
	 * 如果 varattnos 为 NULL（请求了整表访问），这并不必然保证用户能读到
	 * 子表的所有列，但我们仍然允许（参见 examine_variable() 中的注释），
	 * 并且不必再检查任何列级权限。
	 */
	if (pg_class_aclcheck(rte->relid, userid, ACL_SELECT) == ACLCHECK_OK)
		return true;

	if (varattnos == NULL)
		return false;			/* 请求了整表访问 */

	/*
	 * 没有表级 SELECT 权限，因此检查逐列权限。
	 */
	varattno = -1;
	while ((varattno = bms_next_member(varattnos, varattno)) >= 0)
	{
		AttrNumber	attno = varattno + FirstLowInvalidHeapAttributeNumber;

		if (attno == InvalidAttrNumber)
		{
			/* whole-row 引用，因此必须能访问所有列 */
			if (pg_attribute_aclcheck_all(rte->relid, userid, ACL_SELECT,
										  ACLMASK_ALL) != ACLCHECK_OK)
				return false;
		}
		else
		{
			if (pg_attribute_aclcheck(rte->relid, attno, userid,
									  ACL_SELECT) != ACLCHECK_OK)
				return false;
		}
	}

	/* 如果能执行到这里，说明已具备所有必需的列权限 */
	return true;
}

/*
 * examine_indexcol_variable
 *		尝试查找关于某个索引列/表达式的统计信息。填充一个
 *		VariableStatData 结构来描述该列。
 *
 * 输入：
 *	root: 规划器信息
 *	index: 我们感兴趣的列所属的索引
 *	indexcol: 从 0 开始的索引列编号（下标为 index->indexkeys[]）
 *
 * 输出：*vardata 按如下方式填充：
 *	var: 输入表达式（如果它本身是或包含变量，则去掉任何二进制 relabeling；
 *		否则类型保持不变）
 *	rel: 包含该变量的表关系的 RelOptInfo。
 *	statsTuple: 该变量的 pg_statistic 项，若存在则为其；否则为 NULL。
 *	freefunc: 用于释放 statsTuple 的函数指针。
 *
 * 调用方负责在退出前调用 ReleaseVariableStats()。
 */
static void
examine_indexcol_variable(PlannerInfo *root, IndexOptInfo *index,
						  int indexcol, VariableStatData *vardata)
{
	AttrNumber	colnum;
	Oid			relid;

	if (index->indexkeys[indexcol] != 0)
	{
		/* 简单变量——查看底层表的统计 */
		RangeTblEntry *rte = planner_rt_fetch(index->rel->relid, root);

		Assert(rte->rtekind == RTE_RELATION);
		relid = rte->relid;
		Assert(relid != InvalidOid);
		colnum = index->indexkeys[indexcol];
		vardata->rel = index->rel;

		if (get_relation_stats_hook &&
			(*get_relation_stats_hook) (root, rte, colnum, vardata))
		{
			/*
			 * 钩子接管了获取统计元组的职责。如果它确实提供了元组，那它最好
			 * 也提供了一个 freefunc。
			 */
			if (HeapTupleIsValid(vardata->statsTuple) &&
				!vardata->freefunc)
				elog(ERROR, "no function provided to release variable stats with");
		}
		else
		{
			vardata->statsTuple = SearchSysCache3(STATRELATTINH,
												  ObjectIdGetDatum(relid),
												  Int16GetDatum(colnum),
												  BoolGetDatum(rte->inh));
			vardata->freefunc = ReleaseSysCache;
		}
	}
	else
	{
		/* 表达式——也许索引本身带有统计 */
		relid = index->indexoid;
		colnum = indexcol + 1;

		if (get_index_stats_hook &&
			(*get_index_stats_hook) (root, relid, colnum, vardata))
		{
			/*
			 * 钩子接管了获取统计元组的职责。如果它确实提供了元组，那它最好
			 * 也提供了一个 freefunc。
			 */
			if (HeapTupleIsValid(vardata->statsTuple) &&
				!vardata->freefunc)
				elog(ERROR, "no function provided to release variable stats with");
		}
		else
		{
			vardata->statsTuple = SearchSysCache3(STATRELATTINH,
												  ObjectIdGetDatum(relid),
												  Int16GetDatum(colnum),
												  BoolGetDatum(false));
			vardata->freefunc = ReleaseSysCache;
		}
	}
}

/*
 * 检查是否允许调用 func_oid，并传入 vardata 中的一些 pg_statistic 数据。
 * 如果满足以下任一条件，我们就允许： (1) 用户对 pg_statistic 数据底层的
 * 表或列拥有 SELECT 权限，且不存在来自 security barrier 视图或 RLS 策略的
 * securityQual；或者 (2) 该函数被标记为 leakproof。
 */
bool
statistic_proc_security_check(VariableStatData *vardata, Oid func_oid)
{
	if (vardata->acl_ok)
		return true;			/* 拥有 SELECT 权限且无 securityQual */

	if (!OidIsValid(func_oid))
		return false;

	if (get_func_leakproof(func_oid))
		return true;

	ereport(DEBUG2,
			(errmsg_internal("not using statistics because function \"%s\" is not leakproof",
							 get_func_name(func_oid))));
	return false;
}

/*
 * get_variable_numdistinct
 *	  估计一个变量的不同值数量。
 *
 * vardata: examine_variable 的结果
 * *isdefault: 如果结果是默认值而非基于有意义的数据得出，则置为 true。
 *
 * 注意：要小心产生一个正整数结果，因为调用方可能会把结果与精确的整数
 * 计数比较，或者用它做除法。
 */
double
get_variable_numdistinct(VariableStatData *vardata, bool *isdefault)
{
	double		stadistinct;
	double		stanullfrac = 0.0;
	double		ntuples;

	*isdefault = false;

	/*
	 * 确定要使用的 stadistinct 值。有些情况下，即便没有 pg_statistic 项，
	 * 我们也能得到估计值，或者能得到比 pg_statistic 中更好的值。如果能在
	 * 找到的同时也取一下 stanullfrac（否则，由于缺乏更好的判断而假定没有
	 * 空值）。
	 */
	if (HeapTupleIsValid(vardata->statsTuple))
	{
		/* 使用 pg_statistic 项 */
		Form_pg_statistic stats;

		stats = (Form_pg_statistic) GETSTRUCT(vardata->statsTuple);
		stadistinct = stats->stadistinct;
		stanullfrac = stats->stanullfrac;
	}
	else if (vardata->vartype == BOOLOID)
	{
		/*
		 * 对布尔列做特殊处理：大概有两个不同值。
		 *
		 * 还有其它数据类型需要我们专门接线估算吗？
		 */
		stadistinct = 2.0;
	}
	else if (vardata->rel && vardata->rel->rtekind == RTE_VALUES)
	{
		/*
		 * 如果该 Var 代表 VALUES 类型 RTE 的一列，就假定它是唯一的。这当然
		 * 可能非常不准确，但在写得良好的查询中，它大体上应该是成立的。我们
		 * 可以考虑检查 VALUES 的内容来获得一些真实统计；但那只在所有条目
		 * 都是常量时才有效，而且无论如何代价都相当高。
		 */
		stadistinct = -1.0;		/* 唯一（且全非空） */
	}
	else
	{
		/*
		 * 我们不保存系统列的统计，但在某些情况下我们仍然能推断出不同值
		 * 数量。
		 */
		if (vardata->var && IsA(vardata->var, Var))
		{
			switch (((Var *) vardata->var)->varattno)
			{
				case SelfItemPointerAttributeNumber:
					stadistinct = -1.0; /* unique (and all non null) */
					break;
				case TableOidAttributeNumber:
					stadistinct = 1.0;	/* only 1 value */
					break;
				default:
					stadistinct = 0.0;	/* means "unknown" */
					break;
			}
		}
		else
			stadistinct = 0.0;	/* 表示“未知” */

		/*
		 * XXX 考虑对表达式使用 estimate_num_groups？
		 */
	}

	/*
	 * 如果该变量存在唯一索引、DISTINCT 或 GROUP-BY 子句，就假定它是唯一的，
	 * 而不管 pg_statistic 怎么说；统计可能已经过时，或者我们可能找到了一个
	 * 部分唯一索引，能证明该 var 在本查询中是唯一的。不过，我们最好还是
	 * 仍然相信空值比例统计。
	 */
	if (vardata->isunique)
		stadistinct = -1.0 * (1.0 - stanullfrac);

	/*
	 * 如果我们有一个绝对估计值，就使用它。
	 */
	if (stadistinct > 0.0)
		return clamp_row_est(stadistinct);

	/*
	 * 否则我们需要取得关系的大小；如果取不到就放弃。
	 */
	if (vardata->rel == NULL)
	{
		*isdefault = true;
		return DEFAULT_NUM_DISTINCT;
	}
	ntuples = vardata->rel->tuples;
	if (ntuples <= 0.0)
	{
		*isdefault = true;
		return DEFAULT_NUM_DISTINCT;
	}

	/*
	 * 如果我们有一个相对估计值，就使用它。
	 */
	if (stadistinct < 0.0)
		return clamp_row_est(-stadistinct * ntuples);

	/*
	 * 在没有任何数据时，如果表很小，则估计 ndistinct = ntuples，否则使用
	 * 默认值。我们用 DEFAULT_NUM_DISTINCT 作为“小”的临界值，这样行为就
	 * 不会出现不连续。
	 */
	if (ntuples < DEFAULT_NUM_DISTINCT)
		return clamp_row_est(ntuples);

	*isdefault = true;
	return DEFAULT_NUM_DISTINCT;
}

/*
 * get_variable_range
 *		估计指定变量的最小值和最大值。成功时把值存入 *min 和 *max，
 *		并返回 true。如果没有可用数据，则返回 false。
 *
 * sortop 是要使用的 "<" 比较操作符。一般来说应当用 "<" 而非 ">"，因为
 * pg_statistic 中大概只会存在前者。排序规则（collation）也必须指定。
 */
static bool
get_variable_range(PlannerInfo *root, VariableStatData *vardata,
				   Oid sortop, Oid collation,
				   Datum *min, Datum *max)
{
	Datum		tmin = 0;
	Datum		tmax = 0;
	bool		have_data = false;
	int16		typLen;
	bool		typByVal;
	Oid			opfuncoid;
	FmgrInfo	opproc;
	AttStatsSlot sslot;

	/*
	 * XXX 用索引探测相对廉价地取得列的实际最小值和最大值，这一点非常诱人。
	 * 然而，由于这个函数在连接规划期间会被调用很多次，那样可能会对规划
	 * 速度产生不良影响。在启用它之前还需要更多调查。
	 */
#ifdef NOT_USED
	if (get_actual_variable_range(root, vardata, sortop, collation, min, max))
		return true;
#endif

	if (!HeapTupleIsValid(vardata->statsTuple))
	{
		/* 没有可用统计，因此返回默认结果 */
		return false;
	}

	/*
	 * 如果我们无法把 sortop 应用到统计数据上，就直接失败。原则上，如果有
	 * 直方图而没有 MCV，我们可以不应用 sortop 就返回直方图端点……但那样
	 * 大概不值得尝试，因为调用方想用这些端点做的任何事情大概率也会通不过
	 * 安全检查。
	 */
	if (!statistic_proc_security_check(vardata,
									   (opfuncoid = get_opcode(sortop))))
		return false;

	opproc.fn_oid = InvalidOid; /* 标记为尚未查找 */

	get_typlenbyval(vardata->atttype, &typLen, &typByVal);

	/*
	 * 如果存在一个采用我们所需排序方式的直方图，就取它的第一个和最后一个
	 * 值。
	 */
	if (get_attstatsslot(&sslot, vardata->statsTuple,
						 STATISTIC_KIND_HISTOGRAM, sortop,
						 ATTSTATSSLOT_VALUES))
	{
		if (sslot.stacoll == collation && sslot.nvalues > 0)
		{
			tmin = datumCopy(sslot.values[0], typByVal, typLen);
			tmax = datumCopy(sslot.values[sslot.nvalues - 1], typByVal, typLen);
			have_data = true;
		}
		free_attstatsslot(&sslot);
	}

	/*
	 * 否则，如果存在一个采用其它排序方式的直方图，就扫描它，并按照我们
	 * 所需的排序方式取得最小值和最大值。这当然可能找不到按我们的排序方式
	 * 真正处于极值位置的值，但总比忽略可用的数据要好。
	 */
	if (!have_data &&
		get_attstatsslot(&sslot, vardata->statsTuple,
						 STATISTIC_KIND_HISTOGRAM, InvalidOid,
						 ATTSTATSSLOT_VALUES))
	{
		get_stats_slot_range(&sslot, opfuncoid, &opproc,
							 collation, typLen, typByVal,
							 &tmin, &tmax, &have_data);
		free_attstatsslot(&sslot);
	}

	/*
	 * 如果我们有最常见值（MCV）信息，就查找极端的 MCV。即便我们也拥有
	 * 直方图，这仍然是必要的，因为直方图不包含 MCV。然而，如果我们*只有*
	 * MCV 而没有直方图，就不该草率地认定它是对数据的完整表示。只有在 MCV
	 * 代表了整张表（在舍入误差范围内）时才继续。
	 */
	if (get_attstatsslot(&sslot, vardata->statsTuple,
						 STATISTIC_KIND_MCV, InvalidOid,
						 have_data ? ATTSTATSSLOT_VALUES :
						 (ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS)))
	{
		bool		use_mcvs = have_data;

		if (!have_data)
		{
			double		sumcommon = 0.0;
			double		nullfrac;
			int			i;

			for (i = 0; i < sslot.nnumbers; i++)
				sumcommon += sslot.numbers[i];
			nullfrac = ((Form_pg_statistic) GETSTRUCT(vardata->statsTuple))->stanullfrac;
			if (sumcommon + nullfrac > 0.99999)
				use_mcvs = true;
		}

		if (use_mcvs)
			get_stats_slot_range(&sslot, opfuncoid, &opproc,
								 collation, typLen, typByVal,
								 &tmin, &tmax, &have_data);
		free_attstatsslot(&sslot);
	}

	*min = tmin;
	*max = tmax;
	return have_data;
}

/*
 * get_stats_slot_range: 扫描 sslot 以取得最小值/最大值
 *
 * get_variable_range 的子例程：根据在统计数组中找到的内容更新
 * min/max/have_data。
 */
static void
get_stats_slot_range(AttStatsSlot *sslot, Oid opfuncoid, FmgrInfo *opproc,
					 Oid collation, int16 typLen, bool typByVal,
					 Datum *min, Datum *max, bool *p_have_data)
{
	Datum		tmin = *min;
	Datum		tmax = *max;
	bool		have_data = *p_have_data;
	bool		found_tmin = false;
	bool		found_tmax = false;

	/* 如果我们还没查找过比较函数，则查找它 */
	if (opproc->fn_oid != opfuncoid)
		fmgr_info(opfuncoid, opproc);

	/* 扫描该 slot 的所有值 */
	for (int i = 0; i < sslot->nvalues; i++)
	{
		if (!have_data)
		{
			tmin = tmax = sslot->values[i];
			found_tmin = found_tmax = true;
			*p_have_data = have_data = true;
			continue;
		}
		if (DatumGetBool(FunctionCall2Coll(opproc,
										   collation,
										   sslot->values[i], tmin)))
		{
			tmin = sslot->values[i];
			found_tmin = true;
		}
		if (DatumGetBool(FunctionCall2Coll(opproc,
										   collation,
										   tmax, sslot->values[i])))
		{
			tmax = sslot->values[i];
			found_tmax = true;
		}
	}

	/*
	 * 如果我们找到了新的极值，就复制该 slot 的值。
	 */
	if (found_tmin)
		*min = datumCopy(tmin, typByVal, typLen);
	if (found_tmax)
		*max = datumCopy(tmax, typByVal, typLen);
}


/*
 * get_actual_variable_range
 *		尝试通过寻找合适的 btree 索引并取其低值/高值，来识别指定变量当前的
 *		*实际*最小值和/或最大值。
 *		成功时把值存入 *min 和 *max，并返回 true。
 *		（如果不需要某一端，对应指针可以为 NULL。）
 *		失败时返回 false。
 *
 * sortop 是要使用的 "<" 比较操作符。
 * collation 是所需的排序规则。
 */
static bool
get_actual_variable_range(PlannerInfo *root, VariableStatData *vardata,
						  Oid sortop, Oid collation,
						  Datum *min, Datum *max)
{
	bool		have_data = false;
	RelOptInfo *rel = vardata->rel;
	RangeTblEntry *rte;
	ListCell   *lc;

	/* 没有关系或没有关系没有索引，则无望 */
	if (rel == NULL || rel->indexlist == NIL)
		return false;
	/* 既然有索引，它必定是一个普通关系 */
	rte = root->simple_rte_array[rel->relid];
	Assert(rte->rtekind == RTE_RELATION);

	/* 忽略分区表。这里的索引都不是真正的索引 */
	if (rte->relkind == RELKIND_PARTITIONED_TABLE)
		return false;

	/* 遍历索引，看是否有能匹配我们问题的 */
	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(lc);
		ScanDirection indexscandir;
		StrategyNumber strategy;

		/* 忽略非排序索引 */
		if (index->sortopfamily == NULL)
			continue;

		/*
		 * 忽略部分索引——我们只想要覆盖整个关系的统计。
		 */
		if (index->indpred != NIL)
			continue;

		/*
		 * 索引列表里可能包含由 get_relation_info 钩子插入的假设索引——
		 * 不要尝试访问它们。
		 */
		if (index->hypothetical)
			continue;

		/*
		 * get_actual_variable_endpoint 使用的是仅索引扫描机制，因此忽略
		 * 那些在第一列上无法使用它的索引。
		 */
		if (!index->canreturn[0])
			continue;

		/*
		 * 第一个索引列必须匹配所需的变量、sortop 和排序规则——不过我们
		 * 也可以使用降序索引。
		 */
		if (collation != index->indexcollations[0])
			continue;			/* 先测这个，因为它最廉价 */
		if (!match_index_to_operand(vardata->var, 0, index))
			continue;
		strategy = get_op_opfamily_strategy(sortop, index->sortopfamily[0]);
		switch (IndexAmTranslateStrategy(strategy, index->relam, index->sortopfamily[0], true))
		{
			case COMPARE_LT:
				if (index->reverse_sort[0])
					indexscandir = BackwardScanDirection;
				else
					indexscandir = ForwardScanDirection;
				break;
			case COMPARE_GT:
				if (index->reverse_sort[0])
					indexscandir = ForwardScanDirection;
				else
					indexscandir = BackwardScanDirection;
				break;
			default:
				/* 索引与 sortop 不匹配 */
				continue;
		}

		/*
		 * 找到了一个合适的索引来提取数据。设置一些数据，供
		 * get_actual_variable_endpoint 的两次调用共用。
		 */
		{
			MemoryContext tmpcontext;
			MemoryContext oldcontext;
			Relation	heapRel;
			Relation	indexRel;
			TupleTableSlot *slot;
			int16		typLen;
			bool		typByVal;
			ScanKeyData scankeys[1];

			/* 确保任何垃圾在我们完成后都能被回收 */
			tmpcontext = AllocSetContextCreate(CurrentMemoryContext,
											   "get_actual_variable_range workspace",
											   ALLOCSET_DEFAULT_SIZES);
			oldcontext = MemoryContextSwitchTo(tmpcontext);

			/*
			 * 打开表和索引，以便从中读取。我们应当已经对它们各自持有某种
			 * 类型的锁。
			 */
			heapRel = table_open(rte->relid, NoLock);
			indexRel = index_open(index->indexoid, NoLock);

			/* 构造索引扫描执行所需的一些东西 */
			slot = table_slot_create(heapRel, NULL);
			get_typlenbyval(vardata->atttype, &typLen, &typByVal);

			/* 设置一个 IS NOT NULL 扫描键，以便忽略空值 */
			ScanKeyEntryInitialize(&scankeys[0],
								   SK_ISNULL | SK_SEARCHNOTNULL,
								   1,	/* 要扫描的索引列 */
								   InvalidStrategy, /* 无策略 */
								   InvalidOid,	/* 无策略子类型 */
								   InvalidOid,	/* 无排序规则 */
								   InvalidOid,	/* 无对应 reg proc */
								   (Datum) 0);	/* 常量 */

			/* 如果请求了 min …… */
			if (min)
			{
				have_data = get_actual_variable_endpoint(heapRel,
														 indexRel,
														 indexscandir,
														 scankeys,
														 typLen,
														 typByVal,
														 slot,
														 oldcontext,
														 min);
			}
			else
			{
				/* 即便没请求 min，仍然想取 max */
				have_data = true;
			}

			/* 如果请求了 max，且我们之前没有失败…… */
			if (max && have_data)
			{
				/* 朝相反方向扫描；其余都一样 */
				have_data = get_actual_variable_endpoint(heapRel,
														 indexRel,
														 -indexscandir,
														 scankeys,
														 typLen,
														 typByVal,
														 slot,
														 oldcontext,
														 max);
			}

			/* 清理一切 */
			ExecDropSingleTupleTableSlot(slot);

			index_close(indexRel, NoLock);
			table_close(heapRel, NoLock);

			MemoryContextSwitchTo(oldcontext);
			MemoryContextDelete(tmpcontext);

			/* 大功告成 */
			break;
		}
	}

	return have_data;
}

/*
 * 从指定索引取得一个端点值（min 或 max，取决于 indexscandir）。成功时
 * 返回 true，否则返回 false。成功时，端点值会被存入 *endpointDatum（并
 * 复制到 outercontext 中）。
 *
 * scankeys 是一个单元素的扫描键数组，被设置为拒绝空值。
 * typLen/typByVal 描述索引第一列的数据类型。
 * tableslot 是一个适合存放表元组的 slot，以防我们需要探查堆。
 * （这些值是可以在本地计算的，但那样的话当 get_actual_variable_range 同时
 * 需要 min 和 max 时就得计算两次。）
 *
 * 失败发生在索引为空时，或者我们判定寻找一个合适元组耗时过长时。
 */
static bool
get_actual_variable_endpoint(Relation heapRel,
							 Relation indexRel,
							 ScanDirection indexscandir,
							 ScanKey scankeys,
							 int16 typLen,
							 bool typByVal,
							 TupleTableSlot *tableslot,
							 MemoryContext outercontext,
							 Datum *endpointDatum)
{
	bool		have_data = false;
	SnapshotData SnapshotNonVacuumable;
	IndexScanDesc index_scan;
	Buffer		vmbuffer = InvalidBuffer;
	BlockNumber last_heap_block = InvalidBlockNumber;
	int			n_visited_heap_pages = 0;
	ItemPointer tid;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	MemoryContext oldcontext;

	/*
	 * 我们对这个操作使用仅索引扫描机制。对于大体静态的表，这是有利的，
	 * 因为它避免了访问堆。对于动态数据它同样有利，只是原因不那么明显；
	 * 详见下文。
	 *
	 * 原则上，我们应当用当前活动的快照来扫描索引，那是我们对查询执行时
	 * 所会看到内容的最佳近似。但如果在运行查询前又取了新快照，那就不
	 * 精确了；并且，如果在索引的开头或结尾存在大量最近死亡或未提交的
	 * 行，这可能非常昂贵（因为我们会费劲地逐个取出并拒绝它们）。因此，
	 * 我们使用 SnapshotNonVacuumable。它会接受最近死亡的、未提交的以及
	 * 正常可见的行。另一方面，它会拒绝已知死亡（known-dead）的行，因此
	 * 当极值被删除时，它不会给出一个虚假答案（除非删除操作非常近）；
	 * 这种情况就是这里不使用 SnapshotAny 的原因。
	 *
	 * 这里关键的一点是：以 GlobalVisTestFor(heapRel) 为界限的
	 * SnapshotNonVacuumable，恰好是索引扫描用来判定索引项是否可被杀死
	 * （killable）的条件之逆（参见 heap_hot_search_buffer()）。因此，如果
	 * 快照拒绝了一个元组（更准确地说，是拒绝了 HOT 链上的所有元组），
	 * 而我们必须继续扫描越过它，我们就知道索引扫描会把那个索引项标记为
	 * 已杀死。这意味着下一次 get_actual_variable_endpoint() 调用就不必
	 * 再考虑那个索引项。通过这种方式，当这个函数在规划期间被大量使用时，
	 * 我们避免了重复劳动。
	 *
	 * 但使用 SnapshotNonVacuumable 也会带来它自身的风险。在最近创建的
	 * 索引中，某些索引项可能指向“破损的” HOT 链，其中并非所有元组版本
	 * 都包含与索引项匹配的数据。存活的元组版本当然与索引匹配，但
	 * SnapshotNonVacuumable 可能接受不匹配的最近死亡元组版本。因此，如果
	 * 我们从选中的堆元组取数据，就可能得到一个不接近索引极值、甚至可能
	 * 为 NULL 的虚假答案。我们之所以能规避这个风险，是因为我们从索引项
	 * 而非堆中取数据。
	 *
	 * 尽管如此小心，仍会有这样的情况：我们可能在索引末尾附近发现许多
	 * 不可见的元组。我们不想在这里耗费大量时间，因此一旦读取了过多堆
	 * 页面，就放弃。当我们因这个原因失败时，调用方最终会使用记录在
	 * pg_statistic 中的那个极值。
	 */
	InitNonVacuumableSnapshot(SnapshotNonVacuumable,
							  GlobalVisTestFor(heapRel));

	index_scan = index_beginscan(heapRel, indexRel,
								 &SnapshotNonVacuumable, NULL,
								 1, 0);
	/* 为仅索引扫描作好设置 */
	index_scan->xs_want_itup = true;
	index_rescan(index_scan, scankeys, 1, NULL, 0);

	/* 沿指定方向取第一个/下一个元组 */
	while ((tid = index_getnext_tid(index_scan, indexscandir)) != NULL)
	{
		BlockNumber block = ItemPointerGetBlockNumber(tid);

		if (!VM_ALL_VISIBLE(heapRel,
							block,
							&vmbuffer))
		{
			/* 糟了，我们得访问堆来检查可见性 */
			if (!index_fetch_heap(index_scan, tableslot))
			{
				/*
				 * 这个索引项没有可见元组，因此我们需要前进到下一个项。
				 * 在这样做之前，统计堆页面的访问次数，如果做得太多就放弃。
				 *
				 * 如果这是与上一个元组相同的堆页面，我们就不计一次页面
				 * 访问。这是偏保守的做法，因为其它最近访问过的页面大概也
				 * 还在缓冲区里；但对这个启发式来说已经够好了。
				 */
#define VISITED_PAGES_LIMIT 100

				if (block != last_heap_block)
				{
					last_heap_block = block;
					n_visited_heap_pages++;
					if (n_visited_heap_pages > VISITED_PAGES_LIMIT)
						break;
				}

				continue;		/* 无可不见元组，尝试下一个索引项 */
			}

			/* 我们实际上根本不需要这个堆元组 */
			ExecClearTuple(tableslot);

			/*
			 * 我们不在乎 HOT 链中是否有多于一个可见元组；只要有任何可见
			 * 的，就足够了。
			 */
		}

		/*
		 * 我们期望索引以 IndexTuple 而非 HeapTuple 的格式返回数据。
		 */
		if (!index_scan->xs_itup)
			elog(ERROR, "no data returned for index-only scan");

		/*
		 * 这里尚不支持重新检查（recheck）。
		 */
		if (index_scan->xs_recheck)
			break;

		/* 可以解构索引元组了 */
		index_deform_tuple(index_scan->xs_itup,
						   index_scan->xs_itupdesc,
						   values, isnull);

		/* 本不该得到空值，但要小心 */
		if (isnull[0])
			elog(ERROR, "found unexpected null value in index \"%s\"",
				 RelationGetRelationName(indexRel));

		/* 把索引列的值复制到调用方的上下文中 */
		oldcontext = MemoryContextSwitchTo(outercontext);
		*endpointDatum = datumCopy(values[0], typByVal, typLen);
		MemoryContextSwitchTo(oldcontext);
		have_data = true;
		break;
	}

	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);
	index_endscan(index_scan);

	return have_data;
}

/*
 * find_join_input_rel
 *		查找一个连接的输入关系。
 *
 * 我们假定该输入关系的 RelOptInfo 一定已经被构建好了。
 */
static RelOptInfo *
find_join_input_rel(PlannerInfo *root, Relids relids)
{
	RelOptInfo *rel = NULL;

	if (!bms_is_empty(relids))
	{
		int			relid;

		if (bms_get_singleton_member(relids, &relid))
			rel = find_base_rel(root, relid);
		else
			rel = find_join_rel(root, relids);
	}

	if (rel == NULL)
		elog(ERROR, "could not find RelOptInfo for given relids");

	return rel;
}


/*-------------------------------------------------------------------------
 *
 * 索引代价估算函数
 *
 *-------------------------------------------------------------------------
 */

/*
 * 从一个 IndexClause 列表中提取出实际的索引条件（以 RestrictInfo 形式）
 */
List *
get_quals_from_indexclauses(List *indexclauses)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, indexclauses)
	{
		IndexClause *iclause = lfirst_node(IndexClause, lc);
		ListCell   *lc2;

		foreach(lc2, iclause->indexquals)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);

			result = lappend(result, rinfo);
		}
	}
	return result;
}

/*
 * 计算一个索引条件表达式列表中各比较操作数求值的总代价。由于我们知道它们
 * 每次扫描只会被求值一次，因此无需区分启动代价与每行代价。
 *
 * 这既可以用于 get_quals_from_indexclauses() 的结果，也可以直接用于一个
 * indexorderbys 列表。在这两种情况下，我们都期望索引键表达式位于二元子句
 * 的左侧。
 */
Cost
index_other_operands_eval_cost(PlannerInfo *root, List *indexquals)
{
	Cost		qual_arg_cost = 0;
	ListCell   *lc;

	foreach(lc, indexquals)
	{
		Expr	   *clause = (Expr *) lfirst(lc);
		Node	   *other_operand;
		QualCost	index_qual_cost;

		/*
		 * 索引条件会带有 RestrictInfo，而 indexorderbys 不会。如果存在
		 * RestrictInfo，则看穿它。
		 */
		if (IsA(clause, RestrictInfo))
			clause = ((RestrictInfo *) clause)->clause;

		if (IsA(clause, OpExpr))
		{
			OpExpr	   *op = (OpExpr *) clause;

			other_operand = (Node *) lsecond(op->args);
		}
		else if (IsA(clause, RowCompareExpr))
		{
			RowCompareExpr *rc = (RowCompareExpr *) clause;

			other_operand = (Node *) rc->rargs;
		}
		else if (IsA(clause, ScalarArrayOpExpr))
		{
			ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;

			other_operand = (Node *) lsecond(saop->args);
		}
		else if (IsA(clause, NullTest))
		{
			other_operand = NULL;
		}
		else
		{
			elog(ERROR, "unsupported indexqual type: %d",
				 (int) nodeTag(clause));
			other_operand = NULL;	/* 避免编译器告警 */
		}

		cost_qual_eval_node(&index_qual_cost, other_operand, root);
		qual_arg_cost += index_qual_cost.startup + index_qual_cost.per_tuple;
	}
	return qual_arg_cost;
}

void
genericcostestimate(PlannerInfo *root,
					IndexPath *path,
					double loop_count,
					GenericCosts *costs)
{
	IndexOptInfo *index = path->indexinfo;
	List	   *indexQuals = get_quals_from_indexclauses(path->indexclauses);
	List	   *indexOrderBys = path->indexorderbys;
	Cost		indexStartupCost;
	Cost		indexTotalCost;
	Selectivity indexSelectivity;
	double		indexCorrelation;
	double		numIndexPages;
	double		numIndexTuples;
	double		spc_random_page_cost;
	double		num_sa_scans;
	double		num_outer_scans;
	double		num_scans;
	double		qual_op_cost;
	double		qual_arg_cost;
	List	   *selectivityQuals;
	ListCell   *l;

	/*
	 * 如果索引是部分索引，就把索引谓词与显式给出的索引条件做 AND，以得到
	 * 对索引选择性更准确的认识。
	 */
	selectivityQuals = add_predicate_to_index_quals(index, indexQuals);

	/*
	 * 如果调用方没有为 ScalarArrayOpExpr 索引扫描提供估计，就假定索引
	 * 下降次数等于该扫描所有 SAOP 子句中数组元素的不同组合数。
	 */
	num_sa_scans = costs->num_sa_scans;
	if (num_sa_scans < 1)
	{
		num_sa_scans = 1;
		foreach(l, indexQuals)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

			if (IsA(rinfo->clause, ScalarArrayOpExpr))
			{
				ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) rinfo->clause;
				double		alength = estimate_array_length(root, lsecond(saop->args));

				if (alength > 1)
					num_sa_scans *= alength;
			}
		}
	}

	/* 估计将被访问的主表元组的比例 */
	indexSelectivity = clauselist_selectivity(root, selectivityQuals,
											  index->rel->relid,
											  JOIN_INNER,
											  NULL);

	/*
	 * 如果调用方没有给我们估计值，就估计将被访问的索引元组数量。我们采用
	 * 这种看起来相当特别的方式，是为了对部分索引得到正确的答案。
	 */
	numIndexTuples = costs->numIndexTuples;
	if (numIndexTuples <= 0.0)
	{
		numIndexTuples = indexSelectivity * index->rel->tuples;

		/*
		 * 上面的计算统计了由 ScalarArrayOpExpr 节点引发的所有扫描所访问的
		 * 全部元组。我们想要的是每次索引扫描的平均值，因此要调整。这也是
		 * 一个顺手把结果取整的好地方。（如果调用方提供了元组估计，那处理
		 * 这些考量的责任就在调用方。）
		 */
		numIndexTuples = rint(numIndexTuples / num_sa_scans);
	}

	/*
	 * 无论如何，我们都可以用索引大小来限制元组数量的上界。另外，即便
	 * indexSelectivity 估计极小，也始终估计至少访问一个元组。
	 */
	if (numIndexTuples > index->tuples)
		numIndexTuples = index->tuples;
	if (numIndexTuples < 1.0)
		numIndexTuples = 1.0;

	/*
	 * 估计将被检索的索引页面数量。
	 *
	 * 我们使用朴素的方法：取索引页面总数中按比例的分数。实际上，这只统计
	 * 了叶子页面，而没有统计诸如索引元页面或上层树节点之类的任何开销。
	 *
	 * 在实践中，访问索引上层往往几乎是免费的，因为它们在负载下往往留在
	 * 缓存中；此外，所涉及的开销高度依赖索引类型。因此我们忽略这些开销，
	 * 把它留给调用方在需要时加上合适的代价。
	 */
	if (index->pages > 1 && index->tuples > 1)
		numIndexPages = ceil(numIndexTuples * index->pages / index->tuples);
	else
		numIndexPages = 1.0;

	/* 取得包含该索引的表空间的估计页面代价 */
	get_tablespace_page_costs(index->reltablespace,
							  &spc_random_page_cost,
							  NULL);

	/*
	 * 现在计算磁盘访问代价。
	 *
	 * 上面的所有计算都是按每次索引扫描来做的。然而，如果我们处于嵌套循环
	 * 的内表扫描中，就可以预期该扫描会为外表每一行重复进行（使用不同的
	 * 搜索键）。类似地，ScalarArrayOpExpr 条件会导致多次索引扫描。这就产生了
	 * 缓存效应减少所需磁盘页面读取次数的可能。我们想要在存在缓存的情况下
	 * 估计每次扫描的平均 I/O 代价。
	 *
	 * 我们使用 Mackert-Lohman 公式（详见 costsize.c）来估计发生的页面读取
	 * 总次数。虽然这不是它被设计出来的用途，但作为一个模型似乎也还合理。
	 * 注意，我们现在统计的是页面而非元组，因此取 N = T = 索引大小，就好像
	 * 每页有一个“元组”。
	 */
	num_outer_scans = loop_count;
	num_scans = num_sa_scans * num_outer_scans;

	if (num_scans > 1)
	{
		double		pages_fetched;

		/* 忽略缓存效应时的总页面读取次数 */
		pages_fetched = numIndexPages * num_scans;

		/* 使用 Mackert-Lohman 公式对缓存效应进行调整 */
		pages_fetched = index_pages_fetched(pages_fetched,
											index->pages,
											(double) index->pages,
											root);

		/*
		 * 现在计算磁盘访问总代价，然后为每次外表扫描报告一个按比例分摊的
		 * 份额。（不要为 ScalarArrayOpExpr 按比例分摊，因为它属于索引扫描
		 * 内部。）
		 */
		indexTotalCost = (pages_fetched * spc_random_page_cost)
			/ num_outer_scans;
	}
	else
	{
		/*
		 * 对于单次索引扫描，我们只需按每个被触及的页面收取
		 * spc_random_page_cost。
		 */
		indexTotalCost = numIndexPages * spc_random_page_cost;
	}

	/*
	 * CPU 代价：索引条件中的任何复杂表达式都需要在扫描开始时求值一次，
	 * 把它们化简为传给索引访问方法的运行时键（参见 nodeIndexscan.c）。我们
	 * 把每个元组的 CPU 代价建模为 cpu_index_tuple_cost 加上每个索引条件
	 * 操作符一个 cpu_operator_cost。由于 numIndexTuples 是每次扫描的数字，
	 * 我们必须乘以 num_sa_scans 才能得到 ScalarArrayOpExpr 情况下的正确结果。
	 * 类似地，这里还要加上任何索引 ORDER BY 表达式的代价。
	 *
	 * 注意：这忽略了重新检查有损（lossy）操作符可能带来的代价。不过，鉴于
	 * 这里其它种种不准确之处，检测那种可能需要的情况似乎比其价值更费事……
	 */
	qual_arg_cost = index_other_operands_eval_cost(root, indexQuals) +
		index_other_operands_eval_cost(root, indexOrderBys);
	qual_op_cost = cpu_operator_cost *
		(list_length(indexQuals) + list_length(indexOrderBys));

	indexStartupCost = qual_arg_cost;
	indexTotalCost += qual_arg_cost;
	indexTotalCost += numIndexTuples * num_sa_scans * (cpu_index_tuple_cost + qual_op_cost);

	/*
	 * Generic assumption about index correlation: there isn't any.
	 */
	indexCorrelation = 0.0;

	/*
	 * Return everything to caller.
	 */
	costs->indexStartupCost = indexStartupCost;
	costs->indexTotalCost = indexTotalCost;
	costs->indexSelectivity = indexSelectivity;
	costs->indexCorrelation = indexCorrelation;
	costs->numIndexPages = numIndexPages;
	costs->numIndexTuples = numIndexTuples;
	costs->spc_random_page_cost = spc_random_page_cost;
	costs->num_sa_scans = num_sa_scans;
}

/*
 * 如果索引是部分索引，就将其谓词加入到给定的条件列表中。
 *
 * 把索引谓词与显式给出的索引条件做 AND，能产生对索引选择性更准确的认识。
 * 然而，我们必须小心不要插入冗余子句，因为 clauselist_selectivity() 很容易
 * 被骗去计算出过低的选择性估计。我们的做法是只加入那些无法被证明已由给定
 * 索引条件所蕴含的谓词子句。这能成功处理诸如条件 “x = 42” 与部分索引
 * “WHERE x >= 40 AND x < 50” 配合使用的情形。还有很多其它情形我们检测不到
 * 冗余，导致选择性估计过低，从而使系统偏向于尽可能使用部分索引。不过这
 * 也不一定就是坏事。
 *
 * 注意，indexQuals 包含 RestrictInfo 节点，而 indpred 不包含，因此输出列表
 * 会是混合的。对于 predicate_implied_by() 和 clauselist_selectivity() 来说
 * 这没问题，但如果把结果传给其它地方就可能成问题。
 */
List *
add_predicate_to_index_quals(IndexOptInfo *index, List *indexQuals)
{
	List	   *predExtraQuals = NIL;
	ListCell   *lc;

	if (index->indpred == NIL)
		return indexQuals;

	foreach(lc, index->indpred)
	{
		Node	   *predQual = (Node *) lfirst(lc);
		List	   *oneQual = list_make1(predQual);

		if (!predicate_implied_by(oneQual, indexQuals, false))
			predExtraQuals = list_concat(predExtraQuals, oneQual);
	}
	return list_concat(predExtraQuals, indexQuals);
}

/*
 * 估计 btree 索引第一列的相关性。
 *
 * 如果我们能从 pg_statistic 得到第一列排序相关性 C 的估计，就把索引相关性
 * 估计为：单列索引取 C，多列索引取 C * 0.75。这里的思路是，多列会稀释第一
 * 列排序的重要性，但不会完全抵消它。
 *
 * 调用时，*vardata 的统计元组我们已经填好了。
 */
static double
btcost_correlation(IndexOptInfo *index, VariableStatData *vardata)
{
	Oid			sortop;
	AttStatsSlot sslot;
	double		indexCorrelation = 0;

	Assert(HeapTupleIsValid(vardata->statsTuple));

	sortop = get_opfamily_member(index->opfamily[0],
								 index->opcintype[0],
								 index->opcintype[0],
								 BTLessStrategyNumber);
	if (OidIsValid(sortop) &&
		get_attstatsslot(&sslot, vardata->statsTuple,
						 STATISTIC_KIND_CORRELATION, sortop,
						 ATTSTATSSLOT_NUMBERS))
	{
		double		varCorrelation;

		Assert(sslot.nnumbers == 1);
		varCorrelation = sslot.numbers[0];

		if (index->reverse_sort[0])
			varCorrelation = -varCorrelation;

		if (index->nkeycolumns > 1)
			indexCorrelation = varCorrelation * 0.75;
		else
			indexCorrelation = varCorrelation;

		free_attstatsslot(&sslot);
	}

	return indexCorrelation;
}

void
btcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
			   Cost *indexStartupCost, Cost *indexTotalCost,
			   Selectivity *indexSelectivity, double *indexCorrelation,
			   double *indexPages)
{
	IndexOptInfo *index = path->indexinfo;
	GenericCosts costs = {0};
	VariableStatData vardata = {0};
	double		numIndexTuples;
	Cost		descentCost;
	List	   *indexBoundQuals;
	List	   *indexSkipQuals;
	int			indexcol;
	bool		eqQualHere;
	bool		found_row_compare;
	bool		found_array;
	bool		found_is_null_op;
	bool		have_correlation = false;
	double		num_sa_scans;
	double		correlation = 0.0;
	ListCell   *lc;

	/*
	 * 对于 btree 扫描，只有前导的 '=' 条件以及紧接着下一个属性的不等式条件
	 * 才会贡献给索引选择性（这些就是决定索引扫描起止点的“边界条件”）。
	 * 额外的条件可以抑制对堆的访问，因此把它们计入 indexSelectivity 是可以
	 * 的，但它们不应计入对 numIndexTuples 的估计。所以我们必须检查给定的
	 * 索引条件，以找出哪些能算作边界条件。我们依赖于它们是按索引列顺序给出
	 * 这一事实。注意，nbtree 预处理可能会加入跳过数组（skip array），在没有
	 * 普通输入 '=' 条件时，它们可充当前导的 '=' 条件，因此在实践中*大多数*
	 * 输入条件都能充当索引边界条件（我们在此将其考虑在内）。
	 *
	 * 对于 RowCompareExpr，我们只考虑第一列，正如 rowcomparesel() 那样。
	 *
	 * 如果条件中存在 SAOP 或跳过数组，我们实际上会执行多达 N 次索引下降
	 * （而不只是一次），但底层数组键的操作符可被视为与平常表现相同。
	 */
	indexBoundQuals = NIL;
	indexSkipQuals = NIL;
	indexcol = 0;
	eqQualHere = false;
	found_row_compare = false;
	found_array = false;
	found_is_null_op = false;
	num_sa_scans = 1;
	foreach(lc, path->indexclauses)
	{
		IndexClause *iclause = lfirst_node(IndexClause, lc);
		ListCell   *lc2;

		if (indexcol < iclause->indexcol)
		{
			double		num_sa_scans_prev_cols = num_sa_scans;

			/*
			 * Beginning of a new column's quals.
			 *
			 * Skip scans use skip arrays, which are ScalarArrayOp style
			 * arrays that generate their elements procedurally and on demand.
			 * Given a multi-column index on "(a, b)", and an SQL WHERE clause
			 * "WHERE b = 42", a skip scan will effectively use an indexqual
			 * "WHERE a = ANY('{every col a value}') AND b = 42"。（显然，
			 * "a" 上的数组还必须返回 "IS NULL" 匹配，因为我们的 WHERE 子句
			 * 对 "a" 没有使用严格操作符）。
			 *
			 * 这里我们考虑 nbtree 会如何为任何缺少 '=' 条件的索引列回填跳过
			 * 数组。这维持了我们的 num_sa_scans 估计，并决定了这个新列
			 * （即 "iclause->indexcol" 列，而非之前的 "indexcol" 列）的
			 * RestrictInfo/条件能否被加入 indexBoundQuals。
			 *
			 * 我们需要处理带有不等式条件的列，此时跳过数组会从受那些条件
			 * （而非每个可能的值）约束的范围内生成值。我们一直在维护
			 * indexSkipQuals 以辅助此事；当它可能被用于此目的时，它现在会
			 * 包含前一列（即 indexcol 列）的所有条件。
			 */
			if (found_row_compare)
			{
				/*
				 * 由于 nbtree 的限制，在 RowCompare 输入条件之后无法再添加
				 * 跳过数组
				 */
				break;
			}
			if (eqQualHere)
			{
				/*
				 * 对于一个已经有 '=' 条件/相等约束的 indexcol，无需再添加
				 * 跳过数组
				 */
				indexcol++;
				indexSkipQuals = NIL;
			}
			eqQualHere = false;

			while (indexcol < iclause->indexcol)
			{
				double		ndistinct;
				bool		isdefault = true;

				found_array = true;

				/*
				 * 一个被跳过属性的 ndistinct，构成了我们对运行时其跳过数组
				 * 所使用的“数组元素”总数的估计基础。先把它查出来。
				 */
				examine_indexcol_variable(root, index, indexcol, &vardata);
				ndistinct = get_variable_numdistinct(&vardata, &isdefault);

				if (indexcol == 0)
				{
					/*
					 * 顺带估计一下前导列的相关性（避免下面重复读取变量
					 * 统计）
					 */
					if (HeapTupleIsValid(vardata.statsTuple))
						correlation = btcost_correlation(index, &vardata);
					have_correlation = true;
				}

				ReleaseVariableStats(vardata);

				/*
				 * 如果 ndistinct 是一个默认估计，就保守地假定运行时不会发生
				 * 任何跳过
				 */
				if (isdefault)
				{
					num_sa_scans = num_sa_scans_prev_cols;
					break;		/* 完成 indexBoundQuals 的构建 */
				}

				/*
				 * 把 indexcol 的 indexSkipQuals 选择性应用到 ndistinct 上
				 */
				if (indexSkipQuals != NIL)
				{
					List	   *partialSkipQuals;
					Selectivity ndistinctfrac;

					/*
					 * 如果索引是部分索引，就把索引谓词与索引边界条件做 AND，
					 * 以得到对前一 indexcol 不同值数量更准确的认识
					 */
					partialSkipQuals = add_predicate_to_index_quals(index,
																	indexSkipQuals);

					ndistinctfrac = clauselist_selectivity(root, partialSkipQuals,
														   index->rel->relid,
														   JOIN_INNER,
														   NULL);

					/*
					 * 如果 ndistinctfrac 本身就很具选择性，那么该扫描就不大
					 * 可能通过用后续条件重新定位自身而获益。不要让
					 * iclause->indexcol 的条件被加入 indexBoundQuals（那会
					 * 增加下降代价，而不会让 numIndexTuples 代价降低多少）。
					 */
					if (ndistinctfrac < DEFAULT_RANGE_INEQ_SEL)
					{
						num_sa_scans = num_sa_scans_prev_cols;
						break;	/* 完成 indexBoundQuals 的构建 */
					}

					/* 向下调整 ndistinct */
					ndistinct = rint(ndistinct * ndistinctfrac);
					ndistinct = Max(ndistinct, 1);
				}

				/*
				 * 在没有不等式条件时，通过把 -inf/+inf 也计为一个值，来反映
				 * 需要找到一个初始值。
				 *
				 * 对于可能的 next/prior key 索引探测，我们不另收额外费用；
				 * 这类探测有时被用来寻找下一个有效的跳过数组元素（在用它
				 * 定位到的元素值把扫描重定位到下一个可能含有匹配元组的位置
				 * 之前）。在这里要做得更好似乎很困难。使用跳过支持设施通常
				 * 能避免大多数 next/prior key 探测。但即便避免不了，也有相当
				 * 大的可能：大多数单独的 next/prior key 探测会定位到一个叶子
				 * 页面，其键空间与扫描的所有键（甚至低阶键）都重叠——这也
				 * 就免去了单独、额外的索引下降的需要。还要注意，这些探测
				 * 比非探测的基本索引扫描廉价得多：它们确实非常具选择性。
				 */
				if (indexSkipQuals == NIL)
					ndistinct += 1;

				/*
				 * Update num_sa_scans estimate by multiplying by ndistinct.
				 *
				 * We make the pessimistic assumption that there is no
				 * naturally occurring cross-column correlation.  This is
				 * 往往是错误的，但似乎最好还是宁可低估跳过带来的好处……
				 */
				num_sa_scans *= ndistinct;

				/*
				 * ……但当 num_sa_scans 超过索引页面总数时，撤销加入最新这一组
				 * 1 个或多个跳过数组（回退到 indexcol 之前的 num_sa_scans）。
				 * 这会造成代价上（作为 indexcol 的 ndistinct 的函数）的剧烈
				 * 不连续，但那正代表了实际的运行时代价。
				 *
				 * 注意，当每次基本索引扫描平均只跳过 1 或 2 个无关的叶子页面
				 * 时，跳过是有帮助的。跳过数组能节省 CPU 代价，因为扫描无需
				 * 对每个元组求值索引条件，这可能大大超过在 I/O 代价上节省的
				 * 部分。这个测试检验的是：num_sa_scans 是否意味着我们已经越过了
				 * “跳过能力不再能降低扫描代价（甚至条件求值的 CPU 代价）”的
				 * 临界点。
				 */
				if (index->pages < num_sa_scans)
				{
					num_sa_scans = num_sa_scans_prev_cols;
					break;		/* 完成 indexBoundQuals 的构建 */
				}

				indexcol++;
				indexSkipQuals = NIL;
			}

			/*
			 * 考虑是否需要添加跳过数组，以弥合旧索引列与新索引列之间最初的
			 * eqQualHere 缺口（或者一开始就不存在这样的缺口）。
			 *
			 * 如果最初的缺口无法被弥合，那么新列的条件（即
			 * iclause->indexcol 的条件）就不会进入 indexBoundQuals，因而
			 * 也不会影响我们最终的 numIndexTuples 估计。
			 */
			if (indexcol != iclause->indexcol)
				break;			/* 完成 indexBoundQuals 的构建 */
		}

		Assert(indexcol == iclause->indexcol);

		/* 检查与该索引子句关联的每一个索引条件 */
		foreach(lc2, iclause->indexquals)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);
			Expr	   *clause = rinfo->clause;
			Oid			clause_op = InvalidOid;
			int			op_strategy;

			if (IsA(clause, OpExpr))
			{
				OpExpr	   *op = (OpExpr *) clause;

				clause_op = op->opno;
			}
			else if (IsA(clause, RowCompareExpr))
			{
				RowCompareExpr *rc = (RowCompareExpr *) clause;

				clause_op = linitial_oid(rc->opnos);
				found_row_compare = true;
			}
			else if (IsA(clause, ScalarArrayOpExpr))
			{
				ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
				Node	   *other_operand = (Node *) lsecond(saop->args);
				double		alength = estimate_array_length(root, other_operand);

				clause_op = saop->opno;
				found_array = true;
				/* 仅按 indexBoundQuals 估计 SA 下降次数 */
				if (alength > 1)
					num_sa_scans *= alength;
			}
			else if (IsA(clause, NullTest))
			{
				NullTest   *nt = (NullTest *) clause;

				if (nt->nulltesttype == IS_NULL)
				{
					found_is_null_op = true;
					/* 就选择性/跳过扫描而言，IS NULL 类似于 = */
					eqQualHere = true;
				}
			}
			else
				elog(ERROR, "unsupported indexqual type: %d",
					 (int) nodeTag(clause));

			/* 检查是否为相等操作符 */
			if (OidIsValid(clause_op))
			{
				op_strategy = get_op_opfamily_strategy(clause_op,
													   index->opfamily[indexcol]);
				Assert(op_strategy != 0);	/* 不是操作符族的成员？？ */
				if (op_strategy == BTEqualStrategyNumber)
					eqQualHere = true;
			}

			indexBoundQuals = lappend(indexBoundQuals, rinfo);

			/*
			 * 对于使用跳过数组的扫描，我们用不等式选择性来估计索引下降
			 * 代价。如果看起来这些 RestrictInfo 会用于此，就保存本 indexcol
			 * 的它们。
			 */
			if (!eqQualHere && !found_row_compare &&
				indexcol < index->nkeycolumns - 1)
				indexSkipQuals = lappend(indexSkipQuals, rinfo);
		}
	}

	/*
	 * 如果索引是唯一的，并且我们为每个列都找到了 '=' 子句，就可以直接假定
	 * numIndexTuples = 1，并跳过昂贵的 clauselist_selectivity 计算。不过，
	 * 数组或 NullTest 总是会使这种推断失效（即便 eqQualHere 已被置位）。
	 */
	if (index->unique &&
		indexcol == index->nkeycolumns - 1 &&
		eqQualHere &&
		!found_array &&
		!found_is_null_op)
		numIndexTuples = 1.0;
	else
	{
		List	   *selectivityQuals;
		Selectivity btreeSelectivity;

		/*
		 * 如果索引是部分索引，就把索引谓词与索引边界条件做 AND，以得到对
		 * 边界条件所覆盖行数更准确的认识。
		 */
		selectivityQuals = add_predicate_to_index_quals(index, indexBoundQuals);

		btreeSelectivity = clauselist_selectivity(root, selectivityQuals,
												  index->rel->relid,
												  JOIN_INNER,
												  NULL);
		numIndexTuples = btreeSelectivity * index->rel->tuples;

		/*
		 * btree automatically combines individual array element primitive
		 * index scans whenever the tuples covered by the next set of array
		 * keys are close to tuples covered by the current set.  That puts a
		 * natural ceiling on the worst case number of descents -- there
		 * cannot possibly be more than one descent per leaf page scanned.
		 *
		 * Clamp the number of descents to at most 1/3 the number of index
		 * pages.  This avoids implausibly high estimates with low selectivity
		 * paths, where scans usually require only one or two descents.  This
		 * is most likely to help when there are several SAOP clauses, where
		 * naively accepting the total number of distinct combinations of
		 * array elements as the number of descents would frequently lead to
		 * wild overestimates.
		 *
		 * We somewhat arbitrarily don't just make the cutoff the total number
		 * of leaf pages (we make it 1/3 the total number of pages instead) to
		 * give the btree code credit for its ability to continue on the leaf
		 * level with low selectivity scans.
		 *
		 * Note: num_sa_scans includes both ScalarArrayOp array elements and
		 * skip array elements whose qual affects our numIndexTuples estimate.
		 */
		num_sa_scans = Min(num_sa_scans, ceil(index->pages * 0.3333333));
		num_sa_scans = Max(num_sa_scans, 1);

		/*
		 * As in genericcostestimate(), we have to adjust for any array quals
		 * included in indexBoundQuals, and then round to integer.
		 *
		 * It is tempting to make genericcostestimate behave as if array
		 * clauses work in almost the same way as scalar operators during
		 * btree scans, making the top-level scan look like a continuous scan
		 * (as opposed to num_sa_scans-many primitive index scans).  After
		 * all, btree scans mostly work like that at runtime.  However, such a
		 * scheme would badly bias genericcostestimate's simplistic approach
		 * to calculating numIndexPages through prorating.
		 *
		 * Stick with the approach taken by non-native SAOP scans for now.
		 * genericcostestimate will use the Mackert-Lohman formula to
		 * compensate for repeat page fetches, even though that definitely
		 * won't happen during btree scans (not for leaf pages, at least).
		 * We're usually very pessimistic about the number of primitive index
		 * scans that will be required, but it's not clear how to do better.
		 */
		numIndexTuples = rint(numIndexTuples / num_sa_scans);
	}

	/*
	 * Now do generic index cost estimation.
	 */
	costs.numIndexTuples = numIndexTuples;
	costs.num_sa_scans = num_sa_scans;

	genericcostestimate(root, path, loop_count, &costs);

	/*
	 * Add a CPU-cost component to represent the costs of initial btree
	 * descent.  We don't charge any I/O cost for touching upper btree levels,
	 * since they tend to stay in cache, but we still have to do about log2(N)
	 * comparisons to descend a btree of N leaf tuples.  We charge one
	 * cpu_operator_cost per comparison.
	 *
	 * If there are SAOP or skip array keys, charge this once per estimated
	 * index descent.  The ones after the first one are not startup cost so
	 * far as the overall plan goes, so just add them to "total" cost.
	 */
	if (index->tuples > 1)		/* avoid computing log(0) */
	{
		descentCost = ceil(log(index->tuples) / log(2.0)) * cpu_operator_cost;
		costs.indexStartupCost += descentCost;
		costs.indexTotalCost += costs.num_sa_scans * descentCost;
	}

	/*
	 * Even though we're not charging I/O cost for touching upper btree pages,
	 * it's still reasonable to charge some CPU cost per page descended
	 * through.  Moreover, if we had no such charge at all, bloated indexes
	 * would appear to have the same search cost as unbloated ones, at least
	 * in cases where only a single leaf page is expected to be visited.  This
	 * cost is somewhat arbitrarily set at 50x cpu_operator_cost per page
	 * touched.  The number of such pages is btree tree height plus one (ie,
	 * we charge for the leaf page too).  As above, charge once per estimated
	 * SAOP/skip array descent.
	 */
	descentCost = (index->tree_height + 1) * DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost;
	costs.indexStartupCost += descentCost;
	costs.indexTotalCost += costs.num_sa_scans * descentCost;

	if (!have_correlation)
	{
		examine_indexcol_variable(root, index, 0, &vardata);
		if (HeapTupleIsValid(vardata.statsTuple))
			costs.indexCorrelation = btcost_correlation(index, &vardata);
		ReleaseVariableStats(vardata);
	}
	else
	{
		/* btcost_correlation already called earlier on */
		costs.indexCorrelation = correlation;
	}

	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = costs.indexCorrelation;
	*indexPages = costs.numIndexPages;
}

void
hashcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
				 Cost *indexStartupCost, Cost *indexTotalCost,
				 Selectivity *indexSelectivity, double *indexCorrelation,
				 double *indexPages)
{
	GenericCosts costs = {0};

	genericcostestimate(root, path, loop_count, &costs);

	/*
	 * A hash index has no descent costs as such, since the index AM can go
	 * directly to the target bucket after computing the hash value.  There
	 * are a couple of other hash-specific costs that we could conceivably add
	 * here, though:
	 *
	 * Ideally we'd charge spc_random_page_cost for each page in the target
	 * bucket, not just the numIndexPages pages that genericcostestimate
	 * thought we'd visit.  However in most cases we don't know which bucket
	 * that will be.  There's no point in considering the average bucket size
	 * because the hash AM makes sure that's always one page.
	 *
	 * Likewise, we could consider charging some CPU for each index tuple in
	 * the bucket, if we knew how many there were.  But the per-tuple cost is
	 * just a hash value comparison, not a general datatype-dependent
	 * comparison, so any such charge ought to be quite a bit less than
	 * cpu_operator_cost; which makes it probably not worth worrying about.
	 *
	 * A bigger issue is that chance hash-value collisions will result in
	 * wasted probes into the heap.  We don't currently attempt to model this
	 * cost on the grounds that it's rare, but maybe it's not rare enough.
	 * (Any fix for this ought to consider the generic lossy-operator problem,
	 * though; it's not entirely hash-specific.)
	 */

	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = costs.indexCorrelation;
	*indexPages = costs.numIndexPages;
}

void
gistcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
				 Cost *indexStartupCost, Cost *indexTotalCost,
				 Selectivity *indexSelectivity, double *indexCorrelation,
				 double *indexPages)
{
	IndexOptInfo *index = path->indexinfo;
	GenericCosts costs = {0};
	Cost		descentCost;

	genericcostestimate(root, path, loop_count, &costs);

	/*
	 * We model index descent costs similarly to those for btree, but to do
	 * that we first need an idea of the tree height.  We somewhat arbitrarily
	 * assume that the fanout is 100, meaning the tree height is at most
	 * log100(index->pages).
	 *
	 * Although this computation isn't really expensive enough to require
	 * caching, we might as well use index->tree_height to cache it.
	 */
	if (index->tree_height < 0) /* unknown? */
	{
		if (index->pages > 1)	/* avoid computing log(0) */
			index->tree_height = (int) (log(index->pages) / log(100.0));
		else
			index->tree_height = 0;
	}

	/*
	 * Add a CPU-cost component to represent the costs of initial descent. We
	 * just use log(N) here not log2(N) since the branching factor isn't
	 * necessarily two anyway.  As for btree, charge once per SA scan.
	 */
	if (index->tuples > 1)		/* avoid computing log(0) */
	{
		descentCost = ceil(log(index->tuples)) * cpu_operator_cost;
		costs.indexStartupCost += descentCost;
		costs.indexTotalCost += costs.num_sa_scans * descentCost;
	}

	/*
	 * Likewise add a per-page charge, calculated the same as for btrees.
	 */
	descentCost = (index->tree_height + 1) * DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost;
	costs.indexStartupCost += descentCost;
	costs.indexTotalCost += costs.num_sa_scans * descentCost;

	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = costs.indexCorrelation;
	*indexPages = costs.numIndexPages;
}

void
spgcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
				Cost *indexStartupCost, Cost *indexTotalCost,
				Selectivity *indexSelectivity, double *indexCorrelation,
				double *indexPages)
{
	IndexOptInfo *index = path->indexinfo;
	GenericCosts costs = {0};
	Cost		descentCost;

	genericcostestimate(root, path, loop_count, &costs);

	/*
	 * We model index descent costs similarly to those for btree, but to do
	 * that we first need an idea of the tree height.  We somewhat arbitrarily
	 * assume that the fanout is 100, meaning the tree height is at most
	 * log100(index->pages).
	 *
	 * Although this computation isn't really expensive enough to require
	 * caching, we might as well use index->tree_height to cache it.
	 */
	if (index->tree_height < 0) /* unknown? */
	{
		if (index->pages > 1)	/* avoid computing log(0) */
			index->tree_height = (int) (log(index->pages) / log(100.0));
		else
			index->tree_height = 0;
	}

	/*
	 * Add a CPU-cost component to represent the costs of initial descent. We
	 * just use log(N) here not log2(N) since the branching factor isn't
	 * necessarily two anyway.  As for btree, charge once per SA scan.
	 */
	if (index->tuples > 1)		/* avoid computing log(0) */
	{
		descentCost = ceil(log(index->tuples)) * cpu_operator_cost;
		costs.indexStartupCost += descentCost;
		costs.indexTotalCost += costs.num_sa_scans * descentCost;
	}

	/*
	 * Likewise add a per-page charge, calculated the same as for btrees.
	 */
	descentCost = (index->tree_height + 1) * DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost;
	costs.indexStartupCost += descentCost;
	costs.indexTotalCost += costs.num_sa_scans * descentCost;

	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = costs.indexCorrelation;
	*indexPages = costs.numIndexPages;
}


/*
 * Support routines for gincostestimate
 */

typedef struct
{
	bool		attHasFullScan[INDEX_MAX_KEYS];
	bool		attHasNormalScan[INDEX_MAX_KEYS];
	double		partialEntries;
	double		exactEntries;
	double		searchEntries;
	double		arrayScans;
} GinQualCounts;

/*
 * Estimate the number of index terms that need to be searched for while
 * testing the given GIN query, and increment the counts in *counts
 * appropriately.  If the query is unsatisfiable, return false.
 */
static bool
gincost_pattern(IndexOptInfo *index, int indexcol,
				Oid clause_op, Datum query,
				GinQualCounts *counts)
{
	FmgrInfo	flinfo;
	Oid			extractProcOid;
	Oid			collation;
	int			strategy_op;
	Oid			lefttype,
				righttype;
	int32		nentries = 0;
	bool	   *partial_matches = NULL;
	Pointer    *extra_data = NULL;
	bool	   *nullFlags = NULL;
	int32		searchMode = GIN_SEARCH_MODE_DEFAULT;
	int32		i;

	Assert(indexcol < index->nkeycolumns);

	/*
	 * Get the operator's strategy number and declared input data types within
	 * the index opfamily.  (We don't need the latter, but we use
	 * get_op_opfamily_properties because it will throw error if it fails to
	 * find a matching pg_amop entry.)
	 */
	get_op_opfamily_properties(clause_op, index->opfamily[indexcol], false,
							   &strategy_op, &lefttype, &righttype);

	/*
	 * GIN always uses the "default" support functions, which are those with
	 * lefttype == righttype == the opclass' opcintype (see
	 * IndexSupportInitialize in relcache.c).
	 */
	extractProcOid = get_opfamily_proc(index->opfamily[indexcol],
									   index->opcintype[indexcol],
									   index->opcintype[indexcol],
									   GIN_EXTRACTQUERY_PROC);

	if (!OidIsValid(extractProcOid))
	{
		/* should not happen; throw same error as index_getprocinfo */
		elog(ERROR, "missing support function %d for attribute %d of index \"%s\"",
			 GIN_EXTRACTQUERY_PROC, indexcol + 1,
			 get_rel_name(index->indexoid));
	}

	/*
	 * Choose collation to pass to extractProc (should match initGinState).
	 */
	if (OidIsValid(index->indexcollations[indexcol]))
		collation = index->indexcollations[indexcol];
	else
		collation = DEFAULT_COLLATION_OID;

	fmgr_info(extractProcOid, &flinfo);

	set_fn_opclass_options(&flinfo, index->opclassoptions[indexcol]);

	FunctionCall7Coll(&flinfo,
					  collation,
					  query,
					  PointerGetDatum(&nentries),
					  UInt16GetDatum(strategy_op),
					  PointerGetDatum(&partial_matches),
					  PointerGetDatum(&extra_data),
					  PointerGetDatum(&nullFlags),
					  PointerGetDatum(&searchMode));

	if (nentries <= 0 && searchMode == GIN_SEARCH_MODE_DEFAULT)
	{
		/* No match is possible */
		return false;
	}

	for (i = 0; i < nentries; i++)
	{
		/*
		 * For partial match we haven't any information to estimate number of
		 * matched entries in index, so, we just estimate it as 100
		 */
		if (partial_matches && partial_matches[i])
			counts->partialEntries += 100;
		else
			counts->exactEntries++;

		counts->searchEntries++;
	}

	if (searchMode == GIN_SEARCH_MODE_DEFAULT)
	{
		counts->attHasNormalScan[indexcol] = true;
	}
	else if (searchMode == GIN_SEARCH_MODE_INCLUDE_EMPTY)
	{
		/* Treat "include empty" like an exact-match item */
		counts->attHasNormalScan[indexcol] = true;
		counts->exactEntries++;
		counts->searchEntries++;
	}
	else
	{
		/* It's GIN_SEARCH_MODE_ALL */
		counts->attHasFullScan[indexcol] = true;
	}

	return true;
}

/*
 * Estimate the number of index terms that need to be searched for while
 * testing the given GIN index clause, and increment the counts in *counts
 * appropriately.  If the query is unsatisfiable, return false.
 */
static bool
gincost_opexpr(PlannerInfo *root,
			   IndexOptInfo *index,
			   int indexcol,
			   OpExpr *clause,
			   GinQualCounts *counts)
{
	Oid			clause_op = clause->opno;
	Node	   *operand = (Node *) lsecond(clause->args);

	/* aggressively reduce to a constant, and look through relabeling */
	operand = estimate_expression_value(root, operand);

	if (IsA(operand, RelabelType))
		operand = (Node *) ((RelabelType *) operand)->arg;

	/*
	 * It's impossible to call extractQuery method for unknown operand. So
	 * unless operand is a Const we can't do much; just assume there will be
	 * one ordinary search entry from the operand at runtime.
	 */
	if (!IsA(operand, Const))
	{
		counts->exactEntries++;
		counts->searchEntries++;
		return true;
	}

	/* If Const is null, there can be no matches */
	if (((Const *) operand)->constisnull)
		return false;

	/* Otherwise, apply extractQuery and get the actual term counts */
	return gincost_pattern(index, indexcol, clause_op,
						   ((Const *) operand)->constvalue,
						   counts);
}

/*
 * Estimate the number of index terms that need to be searched for while
 * testing the given GIN index clause, and increment the counts in *counts
 * appropriately.  If the query is unsatisfiable, return false.
 *
 * A ScalarArrayOpExpr will give rise to N separate indexscans at runtime,
 * each of which involves one value from the RHS array, plus all the
 * non-array quals (if any).  To model this, we average the counts across
 * the RHS elements, and add the averages to the counts in *counts (which
 * correspond to per-indexscan costs).  We also multiply counts->arrayScans
 * by N, causing gincostestimate to scale up its estimates accordingly.
 */
static bool
gincost_scalararrayopexpr(PlannerInfo *root,
						  IndexOptInfo *index,
						  int indexcol,
						  ScalarArrayOpExpr *clause,
						  double numIndexEntries,
						  GinQualCounts *counts)
{
	Oid			clause_op = clause->opno;
	Node	   *rightop = (Node *) lsecond(clause->args);
	ArrayType  *arrayval;
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	int			numElems;
	Datum	   *elemValues;
	bool	   *elemNulls;
	GinQualCounts arraycounts;
	int			numPossible = 0;
	int			i;

	Assert(clause->useOr);

	/* aggressively reduce to a constant, and look through relabeling */
	rightop = estimate_expression_value(root, rightop);

	if (IsA(rightop, RelabelType))
		rightop = (Node *) ((RelabelType *) rightop)->arg;

	/*
	 * It's impossible to call extractQuery method for unknown operand. So
	 * unless operand is a Const we can't do much; just assume there will be
	 * one ordinary search entry from each array entry at runtime, and fall
	 * back on a probably-bad estimate of the number of array entries.
	 */
	if (!IsA(rightop, Const))
	{
		counts->exactEntries++;
		counts->searchEntries++;
		counts->arrayScans *= estimate_array_length(root, rightop);
		return true;
	}

	/* If Const is null, there can be no matches */
	if (((Const *) rightop)->constisnull)
		return false;

	/* Otherwise, extract the array elements and iterate over them */
	arrayval = DatumGetArrayTypeP(((Const *) rightop)->constvalue);
	get_typlenbyvalalign(ARR_ELEMTYPE(arrayval),
						 &elmlen, &elmbyval, &elmalign);
	deconstruct_array(arrayval,
					  ARR_ELEMTYPE(arrayval),
					  elmlen, elmbyval, elmalign,
					  &elemValues, &elemNulls, &numElems);

	memset(&arraycounts, 0, sizeof(arraycounts));

	for (i = 0; i < numElems; i++)
	{
		GinQualCounts elemcounts;

		/* NULL can't match anything, so ignore, as the executor will */
		if (elemNulls[i])
			continue;

		/* Otherwise, apply extractQuery and get the actual term counts */
		memset(&elemcounts, 0, sizeof(elemcounts));

		if (gincost_pattern(index, indexcol, clause_op, elemValues[i],
							&elemcounts))
		{
			/* We ignore array elements that are unsatisfiable patterns */
			numPossible++;

			if (elemcounts.attHasFullScan[indexcol] &&
				!elemcounts.attHasNormalScan[indexcol])
			{
				/*
				 * Full index scan will be required.  We treat this as if
				 * every key in the index had been listed in the query; is
				 * that reasonable?
				 */
				elemcounts.partialEntries = 0;
				elemcounts.exactEntries = numIndexEntries;
				elemcounts.searchEntries = numIndexEntries;
			}
			arraycounts.partialEntries += elemcounts.partialEntries;
			arraycounts.exactEntries += elemcounts.exactEntries;
			arraycounts.searchEntries += elemcounts.searchEntries;
		}
	}

	if (numPossible == 0)
	{
		/* No satisfiable patterns in the array */
		return false;
	}

	/*
	 * Now add the averages to the global counts.  This will give us an
	 * estimate of the average number of terms searched for in each indexscan,
	 * including contributions from both array and non-array quals.
	 */
	counts->partialEntries += arraycounts.partialEntries / numPossible;
	counts->exactEntries += arraycounts.exactEntries / numPossible;
	counts->searchEntries += arraycounts.searchEntries / numPossible;

	counts->arrayScans *= numPossible;

	return true;
}

/*
 * GIN has search behavior completely different from other index types
 */
void
gincostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
				Cost *indexStartupCost, Cost *indexTotalCost,
				Selectivity *indexSelectivity, double *indexCorrelation,
				double *indexPages)
{
	IndexOptInfo *index = path->indexinfo;
	List	   *indexQuals = get_quals_from_indexclauses(path->indexclauses);
	List	   *selectivityQuals;
	double		numPages = index->pages,
				numTuples = index->tuples;
	double		numEntryPages,
				numDataPages,
				numPendingPages,
				numEntries;
	GinQualCounts counts;
	bool		matchPossible;
	bool		fullIndexScan;
	double		partialScale;
	double		entryPagesFetched,
				dataPagesFetched,
				dataPagesFetchedBySel;
	double		qual_op_cost,
				qual_arg_cost,
				spc_random_page_cost,
				outer_scans;
	Cost		descentCost;
	Relation	indexRel;
	GinStatsData ginStats;
	ListCell   *lc;
	int			i;

	/*
	 * Obtain statistical information from the meta page, if possible.  Else
	 * set ginStats to zeroes, and we'll cope below.
	 */
	if (!index->hypothetical)
	{
		/* Lock should have already been obtained in plancat.c */
		indexRel = index_open(index->indexoid, NoLock);
		ginGetStats(indexRel, &ginStats);
		index_close(indexRel, NoLock);
	}
	else
	{
		memset(&ginStats, 0, sizeof(ginStats));
	}

	/*
	 * Assuming we got valid (nonzero) stats at all, nPendingPages can be
	 * trusted, but the other fields are data as of the last VACUUM.  We can
	 * scale them up to account for growth since then, but that method only
	 * goes so far; in the worst case, the stats might be for a completely
	 * empty index, and scaling them will produce pretty bogus numbers.
	 * Somewhat arbitrarily, set the cutoff for doing scaling at 4X growth; if
	 * it's grown more than that, fall back to estimating things only from the
	 * assumed-accurate index size.  But we'll trust nPendingPages in any case
	 * so long as it's not clearly insane, ie, more than the index size.
	 */
	if (ginStats.nPendingPages < numPages)
		numPendingPages = ginStats.nPendingPages;
	else
		numPendingPages = 0;

	if (numPages > 0 && ginStats.nTotalPages <= numPages &&
		ginStats.nTotalPages > numPages / 4 &&
		ginStats.nEntryPages > 0 && ginStats.nEntries > 0)
	{
		/*
		 * OK, the stats seem close enough to sane to be trusted.  But we
		 * still need to scale them by the ratio numPages / nTotalPages to
		 * account for growth since the last VACUUM.
		 */
		double		scale = numPages / ginStats.nTotalPages;

		numEntryPages = ceil(ginStats.nEntryPages * scale);
		numDataPages = ceil(ginStats.nDataPages * scale);
		numEntries = ceil(ginStats.nEntries * scale);
		/* ensure we didn't round up too much */
		numEntryPages = Min(numEntryPages, numPages - numPendingPages);
		numDataPages = Min(numDataPages,
						   numPages - numPendingPages - numEntryPages);
	}
	else
	{
		/*
		 * We might get here because it's a hypothetical index, or an index
		 * created pre-9.1 and never vacuumed since upgrading (in which case
		 * its stats would read as zeroes), or just because it's grown too
		 * much since the last VACUUM for us to put our faith in scaling.
		 *
		 * Invent some plausible internal statistics based on the index page
		 * count (and clamp that to at least 10 pages, just in case).  We
		 * estimate that 90% of the index is entry pages, and the rest is data
		 * pages.  Estimate 100 entries per entry page; this is rather bogus
		 * since it'll depend on the size of the keys, but it's more robust
		 * than trying to predict the number of entries per heap tuple.
		 */
		numPages = Max(numPages, 10);
		numEntryPages = floor((numPages - numPendingPages) * 0.90);
		numDataPages = numPages - numPendingPages - numEntryPages;
		numEntries = floor(numEntryPages * 100);
	}

	/* In an empty index, numEntries could be zero.  Avoid divide-by-zero */
	if (numEntries < 1)
		numEntries = 1;

	/*
	 * If the index is partial, AND the index predicate with the index-bound
	 * quals to produce a more accurate idea of the number of rows covered by
	 * the bound conditions.
	 */
	selectivityQuals = add_predicate_to_index_quals(index, indexQuals);

	/* Estimate the fraction of main-table tuples that will be visited */
	*indexSelectivity = clauselist_selectivity(root, selectivityQuals,
											   index->rel->relid,
											   JOIN_INNER,
											   NULL);

	/* fetch estimated page cost for tablespace containing index */
	get_tablespace_page_costs(index->reltablespace,
							  &spc_random_page_cost,
							  NULL);

	/*
	 * Generic assumption about index correlation: there isn't any.
	 */
	*indexCorrelation = 0.0;

	/*
	 * Examine quals to estimate number of search entries & partial matches
	 */
	memset(&counts, 0, sizeof(counts));
	counts.arrayScans = 1;
	matchPossible = true;

	foreach(lc, path->indexclauses)
	{
		IndexClause *iclause = lfirst_node(IndexClause, lc);
		ListCell   *lc2;

		foreach(lc2, iclause->indexquals)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);
			Expr	   *clause = rinfo->clause;

			if (IsA(clause, OpExpr))
			{
				matchPossible = gincost_opexpr(root,
											   index,
											   iclause->indexcol,
											   (OpExpr *) clause,
											   &counts);
				if (!matchPossible)
					break;
			}
			else if (IsA(clause, ScalarArrayOpExpr))
			{
				matchPossible = gincost_scalararrayopexpr(root,
														  index,
														  iclause->indexcol,
														  (ScalarArrayOpExpr *) clause,
														  numEntries,
														  &counts);
				if (!matchPossible)
					break;
			}
			else
			{
				/* shouldn't be anything else for a GIN index */
				elog(ERROR, "unsupported GIN indexqual type: %d",
					 (int) nodeTag(clause));
			}
		}
	}

	/* Fall out if there were any provably-unsatisfiable quals */
	if (!matchPossible)
	{
		*indexStartupCost = 0;
		*indexTotalCost = 0;
		*indexSelectivity = 0;
		return;
	}

	/*
	 * If attribute has a full scan and at the same time doesn't have normal
	 * scan, then we'll have to scan all non-null entries of that attribute.
	 * Currently, we don't have per-attribute statistics for GIN.  Thus, we
	 * must assume the whole GIN index has to be scanned in this case.
	 */
	fullIndexScan = false;
	for (i = 0; i < index->nkeycolumns; i++)
	{
		if (counts.attHasFullScan[i] && !counts.attHasNormalScan[i])
		{
			fullIndexScan = true;
			break;
		}
	}

	if (fullIndexScan || indexQuals == NIL)
	{
		/*
		 * Full index scan will be required.  We treat this as if every key in
		 * the index had been listed in the query; is that reasonable?
		 */
		counts.partialEntries = 0;
		counts.exactEntries = numEntries;
		counts.searchEntries = numEntries;
	}

	/* Will we have more than one iteration of a nestloop scan? */
	outer_scans = loop_count;

	/*
	 * Compute cost to begin scan, first of all, pay attention to pending
	 * list.
	 */
	entryPagesFetched = numPendingPages;

	/*
	 * Estimate number of entry pages read.  We need to do
	 * counts.searchEntries searches.  Use a power function as it should be,
	 * but tuples on leaf pages usually is much greater. Here we include all
	 * searches in entry tree, including search of first entry in partial
	 * match algorithm
	 */
	entryPagesFetched += ceil(counts.searchEntries * rint(pow(numEntryPages, 0.15)));

	/*
	 * Add an estimate of entry pages read by partial match algorithm. It's a
	 * scan over leaf pages in entry tree.  We haven't any useful stats here,
	 * so estimate it as proportion.  Because counts.partialEntries is really
	 * pretty bogus (see code above), it's possible that it is more than
	 * numEntries; clamp the proportion to ensure sanity.
	 */
	partialScale = counts.partialEntries / numEntries;
	partialScale = Min(partialScale, 1.0);

	entryPagesFetched += ceil(numEntryPages * partialScale);

	/*
	 * Partial match algorithm reads all data pages before doing actual scan,
	 * so it's a startup cost.  Again, we haven't any useful stats here, so
	 * estimate it as proportion.
	 */
	dataPagesFetched = ceil(numDataPages * partialScale);

	*indexStartupCost = 0;
	*indexTotalCost = 0;

	/*
	 * Add a CPU-cost component to represent the costs of initial entry btree
	 * descent.  We don't charge any I/O cost for touching upper btree levels,
	 * since they tend to stay in cache, but we still have to do about log2(N)
	 * comparisons to descend a btree of N leaf tuples.  We charge one
	 * cpu_operator_cost per comparison.
	 *
	 * If there are ScalarArrayOpExprs, charge this once per SA scan.  The
	 * ones after the first one are not startup cost so far as the overall
	 * plan is concerned, so add them only to "total" cost.
	 */
	if (numEntries > 1)			/* avoid computing log(0) */
	{
		descentCost = ceil(log(numEntries) / log(2.0)) * cpu_operator_cost;
		*indexStartupCost += descentCost * counts.searchEntries;
		*indexTotalCost += counts.arrayScans * descentCost * counts.searchEntries;
	}

	/*
	 * Add a cpu cost per entry-page fetched. This is not amortized over a
	 * loop.
	 */
	*indexStartupCost += entryPagesFetched * DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost;
	*indexTotalCost += entryPagesFetched * counts.arrayScans * DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost;

	/*
	 * Add a cpu cost per data-page fetched. This is also not amortized over a
	 * loop. Since those are the data pages from the partial match algorithm,
	 * charge them as startup cost.
	 */
	*indexStartupCost += DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost * dataPagesFetched;

	/*
	 * Since we add the startup cost to the total cost later on, remove the
	 * initial arrayscan from the total.
	 */
	*indexTotalCost += dataPagesFetched * (counts.arrayScans - 1) * DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost;

	/*
	 * Calculate cache effects if more than one scan due to nestloops or array
	 * quals.  The result is pro-rated per nestloop scan, but the array qual
	 * factor shouldn't be pro-rated (compare genericcostestimate).
	 */
	if (outer_scans > 1 || counts.arrayScans > 1)
	{
		entryPagesFetched *= outer_scans * counts.arrayScans;
		entryPagesFetched = index_pages_fetched(entryPagesFetched,
												(BlockNumber) numEntryPages,
												numEntryPages, root);
		entryPagesFetched /= outer_scans;
		dataPagesFetched *= outer_scans * counts.arrayScans;
		dataPagesFetched = index_pages_fetched(dataPagesFetched,
											   (BlockNumber) numDataPages,
											   numDataPages, root);
		dataPagesFetched /= outer_scans;
	}

	/*
	 * Here we use random page cost because logically-close pages could be far
	 * apart on disk.
	 */
	*indexStartupCost += (entryPagesFetched + dataPagesFetched) * spc_random_page_cost;

	/*
	 * Now compute the number of data pages fetched during the scan.
	 *
	 * We assume every entry to have the same number of items, and that there
	 * is no overlap between them. (XXX: tsvector and array opclasses collect
	 * statistics on the frequency of individual keys; it would be nice to use
	 * those here.)
	 */
	dataPagesFetched = ceil(numDataPages * counts.exactEntries / numEntries);

	/*
	 * If there is a lot of overlap among the entries, in particular if one of
	 * the entries is very frequent, the above calculation can grossly
	 * under-estimate.  As a simple cross-check, calculate a lower bound based
	 * on the overall selectivity of the quals.  At a minimum, we must read
	 * one item pointer for each matching entry.
	 *
	 * The width of each item pointer varies, based on the level of
	 * compression.  We don't have statistics on that, but an average of
	 * around 3 bytes per item is fairly typical.
	 */
	dataPagesFetchedBySel = ceil(*indexSelectivity *
								 (numTuples / (BLCKSZ / 3)));
	if (dataPagesFetchedBySel > dataPagesFetched)
		dataPagesFetched = dataPagesFetchedBySel;

	/* Add one page cpu-cost to the startup cost */
	*indexStartupCost += DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost * counts.searchEntries;

	/*
	 * Add once again a CPU-cost for those data pages, before amortizing for
	 * cache.
	 */
	*indexTotalCost += dataPagesFetched * counts.arrayScans * DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost;

	/* Account for cache effects, the same as above */
	if (outer_scans > 1 || counts.arrayScans > 1)
	{
		dataPagesFetched *= outer_scans * counts.arrayScans;
		dataPagesFetched = index_pages_fetched(dataPagesFetched,
											   (BlockNumber) numDataPages,
											   numDataPages, root);
		dataPagesFetched /= outer_scans;
	}

	/* And apply random_page_cost as the cost per page */
	*indexTotalCost += *indexStartupCost +
		dataPagesFetched * spc_random_page_cost;

	/*
	 * Add on index qual eval costs, much as in genericcostestimate. We charge
	 * cpu but we can disregard indexorderbys, since GIN doesn't support
	 * those.
	 */
	qual_arg_cost = index_other_operands_eval_cost(root, indexQuals);
	qual_op_cost = cpu_operator_cost * list_length(indexQuals);

	*indexStartupCost += qual_arg_cost;
	*indexTotalCost += qual_arg_cost;

	/*
	 * Add a cpu cost per search entry, corresponding to the actual visited
	 * entries.
	 */
	*indexTotalCost += (counts.searchEntries * counts.arrayScans) * (qual_op_cost);
	/* Now add a cpu cost per tuple in the posting lists / trees */
	*indexTotalCost += (numTuples * *indexSelectivity) * (cpu_index_tuple_cost);
	*indexPages = dataPagesFetched;
}

/*
 * BRIN has search behavior completely different from other index types
 */
void
brincostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
				 Cost *indexStartupCost, Cost *indexTotalCost,
				 Selectivity *indexSelectivity, double *indexCorrelation,
				 double *indexPages)
{
	IndexOptInfo *index = path->indexinfo;
	List	   *indexQuals = get_quals_from_indexclauses(path->indexclauses);
	double		numPages = index->pages;
	RelOptInfo *baserel = index->rel;
	RangeTblEntry *rte = planner_rt_fetch(baserel->relid, root);
	Cost		spc_seq_page_cost;
	Cost		spc_random_page_cost;
	double		qual_arg_cost;
	double		qualSelectivity;
	BrinStatsData statsData;
	double		indexRanges;
	double		minimalRanges;
	double		estimatedRanges;
	double		selec;
	Relation	indexRel;
	ListCell   *l;
	VariableStatData vardata;

	Assert(rte->rtekind == RTE_RELATION);

	/* fetch estimated page cost for the tablespace containing the index */
	get_tablespace_page_costs(index->reltablespace,
							  &spc_random_page_cost,
							  &spc_seq_page_cost);

	/*
	 * Obtain some data from the index itself, if possible.  Otherwise invent
	 * some plausible internal statistics based on the relation page count.
	 */
	if (!index->hypothetical)
	{
		/*
		 * A lock should have already been obtained on the index in plancat.c.
		 */
		indexRel = index_open(index->indexoid, NoLock);
		brinGetStats(indexRel, &statsData);
		index_close(indexRel, NoLock);

		/* work out the actual number of ranges in the index */
		indexRanges = Max(ceil((double) baserel->pages /
							   statsData.pagesPerRange), 1.0);
	}
	else
	{
		/*
		 * Assume default number of pages per range, and estimate the number
		 * of ranges based on that.
		 */
		indexRanges = Max(ceil((double) baserel->pages /
							   BRIN_DEFAULT_PAGES_PER_RANGE), 1.0);

		statsData.pagesPerRange = BRIN_DEFAULT_PAGES_PER_RANGE;
		statsData.revmapNumPages = (indexRanges / REVMAP_PAGE_MAXITEMS) + 1;
	}

	/*
	 * Compute index correlation
	 *
	 * Because we can use all index quals equally when scanning, we can use
	 * the largest correlation (in absolute value) among columns used by the
	 * query.  Start at zero, the worst possible case.  If we cannot find any
	 * correlation statistics, we will keep it as 0.
	 */
	*indexCorrelation = 0;

	foreach(l, path->indexclauses)
	{
		IndexClause *iclause = lfirst_node(IndexClause, l);
		AttrNumber	attnum = index->indexkeys[iclause->indexcol];

		/* attempt to lookup stats in relation for this index column */
		if (attnum != 0)
		{
			/* Simple variable -- look to stats for the underlying table */
			if (get_relation_stats_hook &&
				(*get_relation_stats_hook) (root, rte, attnum, &vardata))
			{
				/*
				 * The hook took control of acquiring a stats tuple.  If it
				 * did supply a tuple, it'd better have supplied a freefunc.
				 */
				if (HeapTupleIsValid(vardata.statsTuple) && !vardata.freefunc)
					elog(ERROR,
						 "no function provided to release variable stats with");
			}
			else
			{
				vardata.statsTuple =
					SearchSysCache3(STATRELATTINH,
									ObjectIdGetDatum(rte->relid),
									Int16GetDatum(attnum),
									BoolGetDatum(false));
				vardata.freefunc = ReleaseSysCache;
			}
		}
		else
		{
			/*
			 * Looks like we've found an expression column in the index. Let's
			 * see if there's any stats for it.
			 */

			/* get the attnum from the 0-based index. */
			attnum = iclause->indexcol + 1;

			if (get_index_stats_hook &&
				(*get_index_stats_hook) (root, index->indexoid, attnum, &vardata))
			{
				/*
				 * The hook took control of acquiring a stats tuple.  If it
				 * did supply a tuple, it'd better have supplied a freefunc.
				 */
				if (HeapTupleIsValid(vardata.statsTuple) &&
					!vardata.freefunc)
					elog(ERROR, "no function provided to release variable stats with");
			}
			else
			{
				vardata.statsTuple = SearchSysCache3(STATRELATTINH,
													 ObjectIdGetDatum(index->indexoid),
													 Int16GetDatum(attnum),
													 BoolGetDatum(false));
				vardata.freefunc = ReleaseSysCache;
			}
		}

		if (HeapTupleIsValid(vardata.statsTuple))
		{
			AttStatsSlot sslot;

			if (get_attstatsslot(&sslot, vardata.statsTuple,
								 STATISTIC_KIND_CORRELATION, InvalidOid,
								 ATTSTATSSLOT_NUMBERS))
			{
				double		varCorrelation = 0.0;

				if (sslot.nnumbers > 0)
					varCorrelation = fabs(sslot.numbers[0]);

				if (varCorrelation > *indexCorrelation)
					*indexCorrelation = varCorrelation;

				free_attstatsslot(&sslot);
			}
		}

		ReleaseVariableStats(vardata);
	}

	qualSelectivity = clauselist_selectivity(root, indexQuals,
											 baserel->relid,
											 JOIN_INNER, NULL);

	/*
	 * Now calculate the minimum possible ranges we could match with if all of
	 * the rows were in the perfect order in the table's heap.
	 */
	minimalRanges = ceil(indexRanges * qualSelectivity);

	/*
	 * Now estimate the number of ranges that we'll touch by using the
	 * indexCorrelation from the stats. Careful not to divide by zero (note
	 * we're using the absolute value of the correlation).
	 */
	if (*indexCorrelation < 1.0e-10)
		estimatedRanges = indexRanges;
	else
		estimatedRanges = Min(minimalRanges / *indexCorrelation, indexRanges);

	/* we expect to visit this portion of the table */
	selec = estimatedRanges / indexRanges;

	CLAMP_PROBABILITY(selec);

	*indexSelectivity = selec;

	/*
	 * Compute the index qual costs, much as in genericcostestimate, to add to
	 * the index costs.  We can disregard indexorderbys, since BRIN doesn't
	 * support those.
	 */
	qual_arg_cost = index_other_operands_eval_cost(root, indexQuals);

	/*
	 * Compute the startup cost as the cost to read the whole revmap
	 * sequentially, including the cost to execute the index quals.
	 */
	*indexStartupCost =
		spc_seq_page_cost * statsData.revmapNumPages * loop_count;
	*indexStartupCost += qual_arg_cost;

	/*
	 * To read a BRIN index there might be a bit of back and forth over
	 * regular pages, as revmap might point to them out of sequential order;
	 * calculate the total cost as reading the whole index in random order.
	 */
	*indexTotalCost = *indexStartupCost +
		spc_random_page_cost * (numPages - statsData.revmapNumPages) * loop_count;

	/*
	 * Charge a small amount per range tuple which we expect to match to. This
	 * is meant to reflect the costs of manipulating the bitmap. The BRIN scan
	 * will set a bit for each page in the range when we find a matching
	 * range, so we must multiply the charge by the number of pages in the
	 * range.
	 */
	*indexTotalCost += 0.1 * cpu_operator_cost * estimatedRanges *
		statsData.pagesPerRange;

	*indexPages = index->pages;
}
