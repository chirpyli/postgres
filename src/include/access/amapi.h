/*-------------------------------------------------------------------------
 *
 * amapi.h
 *	  Postgres 索引访问方法的 API。
 *
 * Copyright (c) 2015-2025, PostgreSQL Global Development Group
 *
 * src/include/access/amapi.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AMAPI_H
#define AMAPI_H

#include "access/cmptype.h"
#include "access/genam.h"
#include "access/stratnum.h"

/*
 * 我们不希望在此处包含规划器头文件，因为索引 AM 的大部分实现
 * 并不关心那些数据结构。为了在声明 amcostestimate_function 时仍能
 * 使用，这里采用结构体的前向引用。
 */
struct PlannerInfo;
struct IndexPath;

/* 类似地，本文件不应依赖 execnodes.h。 */
struct IndexInfo;


/*
 * amproperty API 所用的属性。本列表涵盖核心代码已知的属性，
 * 但索引 AM 也可以通过匹配字符串属性名来定义自己的属性。
 */
typedef enum IndexAMProperty
{
	AMPROP_UNKNOWN = 0,			/* 核心代码未知的任意属性 */
	AMPROP_ASC,					/* 列属性 */
	AMPROP_DESC,
	AMPROP_NULLS_FIRST,
	AMPROP_NULLS_LAST,
	AMPROP_ORDERABLE,
	AMPROP_DISTANCE_ORDERABLE,
	AMPROP_RETURNABLE,
	AMPROP_SEARCH_ARRAY,
	AMPROP_SEARCH_NULLS,
	AMPROP_CLUSTERABLE,			/* 索引属性 */
	AMPROP_INDEX_SCAN,
	AMPROP_BITMAP_SCAN,
	AMPROP_BACKWARD_SCAN,
	AMPROP_CAN_ORDER,			/* AM 属性 */
	AMPROP_CAN_UNIQUE,
	AMPROP_CAN_MULTI_COL,
	AMPROP_CAN_EXCLUDE,
	AMPROP_CAN_INCLUDE,
} IndexAMProperty;

/*
 * 在构建或向 opclass / opfamily 添加成员时，我们使用该结构体类型的
 * 列表来同时跟踪操作符和支持函数。amadjustmembers 函数会接收这些
 * 结构体列表，并允许修改其中的 "ref" 字段。
 *
 * "ref" 字段定义了 pg_amop 或 pg_amproc 项应当如何依赖于相关联的
 * 对象（即应使用哪种依赖类型，以及依赖哪个 opclass 或 opfamily）。
 *
 * 若 ref_is_hard 为真，则该项对操作符或支持函数具有 NORMAL 依赖，
 * 并对 opclass 或 opfamily 具有 INTERNAL 依赖。这会在操作符或支持函数
 * 被删除时强制删除对应的 opclass 或 opfamily，且需要 CASCADE 选项
 * 才能完成。同时也不允许执行 ALTER OPERATOR FAMILY DROP。对于 opclass
 * 不可或缺的对象，这是正确的行为。
 *
 * 若 ref_is_hard 为假，则该项对操作符或支持函数具有 AUTO 依赖，
 * 同时对 opclass 或 opfamily 也具有 AUTO 依赖。这允许执行
 * ALTER OPERATOR FAMILY DROP，并且会在操作符或支持函数被删除时
 * 自动发生。对于非必需的（“松散”）对象，这是正确的行为。
 *
 * 我们还会针对 lefttype/righttype 建立依赖，其强度与对操作符或
 * 支持函数的依赖相同，除非这些依赖与对操作符或支持函数的依赖
 * 是冗余的。
 */
typedef struct OpFamilyMember
{
	bool		is_func;		/* is this an operator, or support func? */
	Oid			object;			/* operator or support func's OID */
	int			number;			/* strategy or support func number */
	Oid			lefttype;		/* lefttype */
	Oid			righttype;		/* righttype */
	Oid			sortfamily;		/* ordering operator's sort opfamily, or 0 */
	bool		ref_is_hard;	/* hard or soft dependency? */
	bool		ref_is_family;	/* is dependency on opclass or opfamily? */
	Oid			refobjid;		/* OID of opclass or opfamily */
} OpFamilyMember;


/*
 * 回调函数签名 —— 更多信息请参阅 indexam.sgml。
 */

/* 将 AM 特定的策略转换为通用操作符类型 */
typedef CompareType (*amtranslate_strategy_function) (StrategyNumber strategy, Oid opfamily);

/* 将通用操作符类型转换为 AM 特定的策略 */
typedef StrategyNumber (*amtranslate_cmptype_function) (CompareType cmptype, Oid opfamily);

/* 构建新索引 */
typedef IndexBuildResult *(*ambuild_function) (Relation heapRelation,
											   Relation indexRelation,
											   struct IndexInfo *indexInfo);

/* 构建空索引 */
typedef void (*ambuildempty_function) (Relation indexRelation);

/* 插入该元组 */
typedef bool (*aminsert_function) (Relation indexRelation,
								   Datum *values,
								   bool *isnull,
								   ItemPointer heap_tid,
								   Relation heapRelation,
								   IndexUniqueCheck checkUnique,
								   bool indexUnchanged,
								   struct IndexInfo *indexInfo);

/* 插入后的清理 */
typedef void (*aminsertcleanup_function) (Relation indexRelation,
										  struct IndexInfo *indexInfo);

/* 批量删除 */
typedef IndexBulkDeleteResult *(*ambulkdelete_function) (IndexVacuumInfo *info,
														 IndexBulkDeleteResult *stats,
														 IndexBulkDeleteCallback callback,
														 void *callback_state);

/* VACUUM 后的清理 */
typedef IndexBulkDeleteResult *(*amvacuumcleanup_function) (IndexVacuumInfo *info,
															IndexBulkDeleteResult *stats);

/* 索引扫描能否返回 IndexTuple？ */
typedef bool (*amcanreturn_function) (Relation indexRelation, int attno);

/* 估算索引扫描的代价 */
typedef void (*amcostestimate_function) (struct PlannerInfo *root,
										 struct IndexPath *path,
										 double loop_count,
										 Cost *indexStartupCost,
										 Cost *indexTotalCost,
										 Selectivity *indexSelectivity,
										 double *indexCorrelation,
										 double *indexPages);

/* 估算树状结构的索引高度
 *
 * XXX 这里只是计算一个稍后供 amcostestimate 使用的值。如有需要传递更多
 * 值，可对此 API 进行扩展。
 */
typedef int (*amgettreeheight_function) (Relation rel);

/* 解析索引的 reloptions */
typedef bytea *(*amoptions_function) (Datum reloptions,
									  bool validate);

/* 报告 AM、索引或索引列的属性 */
typedef bool (*amproperty_function) (Oid index_oid, int attno,
									 IndexAMProperty prop, const char *propname,
									 bool *res, bool *isnull);

/* 进度报告中所用阶段的名称 */
typedef char *(*ambuildphasename_function) (int64 phasenum);

/* 校验本 AM 的 opclass 定义 */
typedef bool (*amvalidate_function) (Oid opclassoid);

/* 校验待加入 opclass/family 的操作符与支持函数 */
typedef void (*amadjustmembers_function) (Oid opfamilyoid,
										  Oid opclassoid,
										  List *operators,
										  List *functions);

/* 准备索引扫描 */
typedef IndexScanDesc (*ambeginscan_function) (Relation indexRelation,
											   int nkeys,
											   int norderbys);

/* （重新）启动索引扫描 */
typedef void (*amrescan_function) (IndexScanDesc scan,
								   ScanKey keys,
								   int nkeys,
								   ScanKey orderbys,
								   int norderbys);

/* 下一个有效元组 */
typedef bool (*amgettuple_function) (IndexScanDesc scan,
									 ScanDirection direction);

/* 取回所有有效元组 */
typedef int64 (*amgetbitmap_function) (IndexScanDesc scan,
									   TIDBitmap *tbm);

/* 结束索引扫描 */
typedef void (*amendscan_function) (IndexScanDesc scan);

/* 标记当前扫描位置 */
typedef void (*ammarkpos_function) (IndexScanDesc scan);

/* 恢复已标记的扫描位置 */
typedef void (*amrestrpos_function) (IndexScanDesc scan);

/*
 * 回调函数签名 —— 用于并行索引扫描。
 */

/* 估算并行扫描描述符的大小 */
typedef Size (*amestimateparallelscan_function) (Relation indexRelation,
												 int nkeys, int norderbys);

/* 准备并行索引扫描 */
typedef void (*aminitparallelscan_function) (void *target);

/* （重新）启动并行索引扫描 */
typedef void (*amparallelrescan_function) (IndexScanDesc scan);

/*
 * 索引 AM 的 API 结构。注意：此结构必须存放在单个 palloc
 * 分配的内存块中。
 */
typedef struct IndexAmRoutine
{
	NodeTag		type;

	/*
	 * 我们可以通过它遍历/搜索此 AM 的策略（操作符）总数。
	 * 若 AM 没有固定的策略分配集合，则为 0。
	 */
	uint16		amstrategies;
	/* 此 AM 使用的支持函数总数 */
	uint16		amsupport;
	/* opclass 选项支持函数号，或 0 */
	uint16		amoptsprocnum;
	/* AM 是否支持按索引列的值排序（ORDER BY）？ */
	bool		amcanorder;
	/* AM 是否支持按索引列上某操作符的结果排序（ORDER BY）？ */
	bool		amcanorderbyop;
	/* AM 是否支持使用与 hash AM 一致的 API 进行哈希？ */
	bool		amcanhash;
	/* 同一 opfamily 内的操作符是否具有一致的相等语义？ */
	bool		amconsistentequality;
	/* 同一 opfamily 内的操作符是否具有一致的排序语义？ */
	bool		amconsistentordering;
	/* AM 是否支持反向扫描？ */
	bool		amcanbackward;
	/* AM 是否支持 UNIQUE 索引？ */
	bool		amcanunique;
	/* AM 是否支持多列索引？ */
	bool		amcanmulticol;
	/* AM 是否要求扫描须对第一个索引列施加约束？ */
	bool		amoptionalkey;
	/* AM 是否处理 ScalarArrayOpExpr 条件？ */
	bool		amsearcharray;
	/* AM 是否处理 IS NULL/IS NOT NULL 条件？ */
	bool		amsearchnulls;
	/* 索引存储的数据类型能否与列的数据类型不同？ */
	bool		amstorage;
	/* 此类型的索引能否被聚簇？ */
	bool		amclusterable;
	/* AM 是否处理谓词锁？ */
	bool		ampredlocks;
	/* AM 是否支持并行扫描？ */
	bool		amcanparallel;
	/* AM 是否支持并行构建？ */
	bool		amcanbuildparallel;
	/* AM 是否支持通过 INCLUDE 子句包含的列？ */
	bool		amcaninclude;
	/* AM 是否使用 maintenance_work_mem？ */
	bool		amusemaintenanceworkmem;
	/* AM 是否仅以块粒度存储元组信息？ */
	bool		amsummarizing;
	/* 并行 VACUUM 标志的按位或。标志定义见 vacuum.h。 */
	uint8		amparallelvacuumoptions;
	/* 索引中存储的数据类型，若为可变类型则为 InvalidOid */
	Oid			amkeytype;

	/*
	 * 如果你在上述或下列列表中新增属性，那么它们通常也应通过
	 * property API 暴露出来（参见文件顶部的 IndexAMProperty，
	 * 以及 utils/adt/amutils.c）。
	 */

	/* 接口函数 */
	ambuild_function ambuild;
	ambuildempty_function ambuildempty;
	aminsert_function aminsert;
	aminsertcleanup_function aminsertcleanup;	/* 可为 NULL */
	ambulkdelete_function ambulkdelete;
	amvacuumcleanup_function amvacuumcleanup;
	amcanreturn_function amcanreturn;	/* 可为 NULL */
	amcostestimate_function amcostestimate;
	amgettreeheight_function amgettreeheight;	/* 可为 NULL */
	amoptions_function amoptions;
	amproperty_function amproperty; /* 可为 NULL */
	ambuildphasename_function ambuildphasename; /* 可为 NULL */
	amvalidate_function amvalidate;
	amadjustmembers_function amadjustmembers;	/* 可为 NULL */
	ambeginscan_function ambeginscan;
	amrescan_function amrescan;
	amgettuple_function amgettuple; /* 可为 NULL */
	amgetbitmap_function amgetbitmap;	/* 可为 NULL */
	amendscan_function amendscan;
	ammarkpos_function ammarkpos;	/* 可为 NULL */
	amrestrpos_function amrestrpos; /* 可为 NULL */

	/* 支持并行索引扫描的接口函数 */
	amestimateparallelscan_function amestimateparallelscan; /* 可为 NULL */
	aminitparallelscan_function aminitparallelscan; /* 可为 NULL */
	amparallelrescan_function amparallelrescan; /* 可为 NULL */

	/* 支持规划的接口函数 */
	amtranslate_strategy_function amtranslatestrategy;	/* 可为 NULL */
	amtranslate_cmptype_function amtranslatecmptype;	/* 可为 NULL */
} IndexAmRoutine;


/* access/index/amapi.c 中的函数 */
extern IndexAmRoutine *GetIndexAmRoutine(Oid amhandler);
extern IndexAmRoutine *GetIndexAmRoutineByAmId(Oid amoid, bool noerror);
extern CompareType IndexAmTranslateStrategy(StrategyNumber strategy, Oid amoid, Oid opfamily, bool missing_ok);
extern StrategyNumber IndexAmTranslateCompareType(CompareType cmptype, Oid amoid, Oid opfamily, bool missing_ok);

#endif							/* AMAPI_H */
