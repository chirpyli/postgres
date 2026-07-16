/*-------------------------------------------------------------------------
 *
 * rel.h
 *	  POSTGRES 关系描述符（亦称 relcache 条目）的定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/rel.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REL_H
#define REL_H

#include "access/tupdesc.h"
#include "access/xlog.h"
#include "catalog/catalog.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_publication.h"
#include "nodes/bitmapset.h"
#include "partitioning/partdefs.h"
#include "rewrite/prs2lock.h"
#include "storage/block.h"
#include "storage/relfilelocator.h"
#include "storage/smgr.h"
#include "utils/relcache.h"
#include "utils/reltrigger.h"


/*
 * LockRelId 和 LockInfo 本应属于 lmgr.h，但在此处声明更为方便，
 * 这样我们就可以在 Relation 中拥有一个 LockInfoData 字段。
 */

typedef struct LockRelId
{
	Oid			relId;			/* 关系标识符 */
	Oid			dbId;			/* 数据库标识符 */
} LockRelId;

typedef struct LockInfoData
{
	LockRelId	lockRelId;
} LockInfoData;

typedef LockInfoData *LockInfo;

/*
 * 以下是关系缓存条目的内容。
 */

typedef struct RelationData
{
	RelFileLocator rd_locator;	/* 关系的物理标识符 */
	SMgrRelation rd_smgr;		/* 缓存的文件句柄，或 NULL */
	int			rd_refcnt;		/* 引用计数 */
	ProcNumber	rd_backend;		/* 所属后端的进程号，如果是临时关系 */
	bool		rd_islocaltemp; /* 关系是本会话的临时关系 */
	bool		rd_isnailed;	/* 关系在缓存中被钉住 */
	bool		rd_isvalid;		/* relcache 条目有效 */
	bool		rd_indexvalid;	/* rd_indexlist 是否有效？（也包括 rd_pkindex 和
								 * rd_replidindex） */
	bool		rd_statvalid;	/* rd_statlist 是否有效？ */

	/*----------
	 * rd_createSubid 是关系所存活到的最高子事务的 ID；如果关系或其存储
	 * 是在当前顶层事务之前创建的，则为 0。（IndexStmt.oldNumber 会导致
	 * 一个具有旧 rd_locator 的新关系的情况。）rd_firstRelfilelocatorSubid
	 * 是 rd_locator 变更所存活到的最高子事务的 ID；如果 rd_locator 与当前
	 * 顶层事务开始时的值相同，则为 0。（回滚 rd_firstRelfilelocatorSubid
	 * 所指的子事务会将 rd_locator 恢复到当前顶层事务开始时的值；回滚任何
	 * 更低层的子事务则不会。）它们的准确性对 RelationNeedsWAL() 至关重要。
	 *
	 * rd_newRelfilelocatorSubid 是最近一次 relfilenumber 变更所存活到
	 * 的最高子事务的 ID；如果当前事务中没有变更（或我们已忘记变更它），
	 * 则为 0。该字段在非 0 时是准确的，但当关系在单个事务中有多个新的
	 * relfilenumber，且其中一个出现在随后被中止的子事务中时，它可能为 0，例如：
	 *		BEGIN;
	 *		TRUNCATE t;
	 *		SAVEPOINT save;
	 *		TRUNCATE t;
	 *		ROLLBACK TO save;
	 *		-- rd_newRelfilelocatorSubid 现在已被遗忘
	 *
	 * 如果所有 rd_*Subid 字段都为 0，则它们对于 relcache.c 之外是只读的。
	 * 那些通过更新 pg_class.reltablespace 和/或 pg_class.relfilenode 来触发
	 * rd_locator 变更的文件会调用 RelationAssumeNewRelfilelocator() 来更新
	 * rd_*Subid。
	 *
	 * rd_droppedSubid 是关系的删除所存活到的最高子事务的 ID。在 relcache.c
	 * 之外可见的条目中，此值始终为 0。
	 */
	SubTransactionId rd_createSubid;	/* 关系在当前事务中创建 */
	SubTransactionId rd_newRelfilelocatorSubid; /* 将 rd_locator 改为当前值的最高子事务 */
	SubTransactionId rd_firstRelfilelocatorSubid;	/* 将 rd_locator 改为任意值的最高子事务 */
	SubTransactionId rd_droppedSubid;	/* 删除时设置了另一个 Subid */

	Form_pg_class rd_rel;		/* RELATION 元组 */
	TupleDesc	rd_att;			/* 元组描述符 */
	Oid			rd_id;			/* 关系的对象 ID */
	LockInfoData rd_lockInfo;	/* 锁管理器用于锁定关系的信息 */
	RuleLock   *rd_rules;		/* 重写规则 */
	MemoryContext rd_rulescxt;	/* rd_rules 的私有内存上下文（如果有） */
	TriggerDesc *trigdesc;		/* 触发器信息；若关系没有则为 NULL */
	/* 此处使用 "struct" 以避免需要包含 rowsecurity.h： */
	struct RowSecurityDesc *rd_rsdesc;	/* 行级安全策略，或 NULL */

	/* 由 RelationGetFKeyList 管理的数据： */
	List	   *rd_fkeylist;	/* ForeignKeyCacheInfo 列表（见下文） */
	bool		rd_fkeyvalid;	/* 若列表已计算则为 true */

	/* 由 RelationGetPartitionKey 管理的数据： */
	PartitionKey rd_partkey;	/* 分区键，或 NULL */
	MemoryContext rd_partkeycxt;	/* rd_partkey 的私有上下文（如果有） */

	/* 由 RelationGetPartitionDesc 管理的数据： */
	PartitionDesc rd_partdesc;	/* 分区描述符，或 NULL */
	MemoryContext rd_pdcxt;		/* rd_partdesc 的私有上下文（如果有） */

	/* 同上，针对省略了已分离分区的 partdesc */
	PartitionDesc rd_partdesc_nodetached;	/* 不含已分离分区的 partdesc */
	MemoryContext rd_pddcxt;	/* 用于 rd_partdesc_nodetached（如果有） */

	/*
	 * 在 rd_partdesc_nodetached 中被排除的分区的 pg_inherits.xmin。
	 * 它告知该 partdesc 未来的使用者：如果该值对于活动快照不处于进行中，
	 * 则该 partdesc 可用，否则他们必须重新构建一个新的。（这与
	 * find_inheritance_children_extended 的行为一致）。
	 */
	TransactionId rd_partdesc_nodetached_xmin;

	/* 由 RelationGetPartitionQual 管理的数据： */
	List	   *rd_partcheck;	/* 分区 CHECK 约束条件 */
	bool		rd_partcheckvalid;	/* 若列表已计算则为 true */
	MemoryContext rd_partcheckcxt;	/* rd_partcheck 的私有上下文（如果有） */

	/* 由 RelationGetIndexList 管理的数据： */
	List	   *rd_indexlist;	/* 关系上索引的 OID 列表 */
	Oid			rd_pkindex;		/* （可延迟的？）主键的 OID（如果有） */
	bool		rd_ispkdeferrable;	/* rd_pkindex 是否为可延迟的主键？ */
	Oid			rd_replidindex; /* 复制标识索引的 OID（如果有） */

	/* 由 RelationGetStatExtList 管理的数据： */
	List	   *rd_statlist;	/* 扩展统计信息的 OID 列表 */

	/* 由 RelationGetIndexAttrBitmap 管理的数据： */
	bool		rd_attrsvalid;	/* 属性的位图是否有效？ */
	Bitmapset  *rd_keyattr;		/* 可被外键引用的列 */
	Bitmapset  *rd_pkattr;		/* 主键中包含的列 */
	Bitmapset  *rd_idattr;		/* 包含在复制标识索引中的列 */
	Bitmapset  *rd_hotblockingattr; /* 阻止 HOT 更新的列 */
	Bitmapset  *rd_summarizedattr;	/* 被汇总索引索引的列 */

	PublicationDesc *rd_pubdesc;	/* 发布描述符，或 NULL */

	/*
	 * 无论何时 rd_rel 被加载到 relcache 条目中，都会设置 rd_options。
	 * 注意，你不能通过 rd_rel 来查找此数据。NULL 表示"使用默认值"。
	 */
	bytea	   *rd_options;		/* 已解析的 pg_class.reloptions */

	/*
	 * 此关系对应的处理函数的 OID。对于索引，这是一个返回 IndexAmRoutine
	 * 的函数；对于类表关系，这是一个返回 TableAmRoutine 的函数。它被单独
	 * 存储于 rd_indam、rd_tableam 之外，因为其查找需要 syscache 访问；
	 * 但在 relcache 引导期间，我们需要能够在没有 syscache 查找的情况下
	 * 初始化 rd_tableam。
	 */
	Oid			rd_amhandler;	/* 索引访问方法的处理函数 OID */

	/*
	 * 表访问方法。
	 */
	const struct TableAmRoutine *rd_tableam;

	/* 这些字段仅对索引关系为非 NULL： */
	Form_pg_index rd_index;		/* 描述此索引的 pg_index 元组 */
	/* 此处使用 "struct" 以避免需要包含 htup.h： */
	struct HeapTupleData *rd_indextuple;	/* 整个 pg_index 元组 */

	/*
	 * 索引访问支持信息（仅用于索引关系）
	 *
	 * 注意：仅缓存每个操作类的默认支持过程，即那些 lefttype 和 righttype
	 * 等于该操作类的 opcintype 的过程。这些数组以支持函数编号为索引，
	 * 在上述限制下，该编号已足以作为标识符。
	 */
	MemoryContext rd_indexcxt;	/* 此数据的私有内存上下文 */
	/* 此处使用 "struct" 以避免需要包含 amapi.h： */
	struct IndexAmRoutine *rd_indam;	/* 索引访问方法的 API 结构体 */
	Oid		   *rd_opfamily;	/* 每个索引列的操作族 OID */
	Oid		   *rd_opcintype;	/* 操作类声明的输入数据类型 OID */
	RegProcedure *rd_support;	/* 支持过程的 OID */
	struct FmgrInfo *rd_supportinfo;	/* 支持过程的查找信息 */
	int16	   *rd_indoption;	/* 每列的特定于访问方法的标志 */
	List	   *rd_indexprs;	/* 索引表达式树（如果有） */
	List	   *rd_indpred;		/* 索引谓词树（如果有） */
	Oid		   *rd_exclops;		/* 排除操作符的 OID（如果有） */
	Oid		   *rd_exclprocs;	/* 排除操作符对应过程的 OID（如果有） */
	uint16	   *rd_exclstrats;	/* 排除操作符的策略号（如果有） */
	Oid		   *rd_indcollation;	/* 索引排序规则 OID */
	bytea	  **rd_opcoptions;	/* 已解析的特定于操作类的选项 */

	/*
	 * rd_amcache 可供索引和表访问方法缓存关于此关系的私有数据。它必须
	 * 仅仅是一个缓存，因为它可能在任何时候被重置（特别是，它会因关系
	 * 的 relcache 失效消息而被重置）。如果使用它，它必须指向一个在
	 * CacheMemoryContext 中 palloc 出来的单一内存块，或者在索引关系的情况下
	 * 指向 rd_indexcxt 中的内存块。一次 relcache 重置会包含释放该内存块
	 * 并将 rd_amcache 置为 NULL。
	 */
	void	   *rd_amcache;		/* 可供索引/表访问方法使用 */

	/*
	 * 外部表支持
	 *
	 * rd_fdwroutine 必须指向一个在 CacheMemoryContext 中 palloc 出来的单一
	 * 内存块。在一次 relcache 重置时，它会被释放并重置为 NULL。
	 */

	/* 此处使用 "struct" 以避免需要包含 fdwapi.h： */
	struct FdwRoutine *rd_fdwroutine;	/* 缓存的函数指针，或 NULL */

	/*
	 * 用于 CLUSTER、重写 ALTER TABLE 等操作的临时方案：当写入一个表的新
	 * 版本时，我们需要让插入其中的任何 toast 指针具有现存 toast 表的 OID，
	 * 而不是临时 toast 表的 OID。如果 rd_toastoid 不是 InvalidOid，它就是
	 * 要放入插入到此关系中的 toast 指针的 OID。（注意，它设置在主堆的新
	 * 版本上，而不是 toast 表本身上。）这也会使 toast_save_datum() 尝试
	 * 保留 toast 值的 OID。
	 */
	Oid			rd_toastoid;	/* 真实 TOAST 表的 OID，或 InvalidOid */

	bool		pgstat_enabled; /* 是否应统计关系统计信息 */
	/* 此处使用 "struct" 以避免需要包含 pgstat.h： */
	struct PgStat_TableStatus *pgstat_info; /* 统计信息收集区域 */
} RelationData;


/*
 * ForeignKeyCacheInfo
 *		relcache 可以缓存的关于外键约束的信息
 *
 * 这基本上就是 pg_constraint 中相关列的一个映像。我们把它做成 Node 的
 * 子类，以便可以在其列表上使用 copyObject()，但我们也确保它是一个
 * 没有子结构的"扁平"对象，这样 list_free_deep() 就足以释放这样的列表。
 * 每个外键列的数组可以是定长的，因为我们允许外键约束中最多有
 * INDEX_MAX_KEYS 列。
 *
 * 目前，我们主要缓存对规划器感兴趣的字段，但这些字段的集合已经为其他
 * 用途增加了约束 OID。
 */
typedef struct ForeignKeyCacheInfo
{
	pg_node_attr(no_equal, no_read, no_query_jumble)

	NodeTag		type;
	/* 约束自身的 OID */
	Oid			conoid;
	/* 受外键约束的关系 */
	Oid			conrelid;
	/* 被外键引用的关系 */
	Oid			confrelid;
	/* 外键中的列数 */
	int			nkeys;

	/* 是否强制实施？ */
	bool		conenforced;

	/*
	 * 这些数组各自具有 nkeys 个有效条目：
	 */
	/* 引用表中的列 */
	AttrNumber	conkey[INDEX_MAX_KEYS] pg_node_attr(array_size(nkeys));
	/* 被引用表中的列 */
	AttrNumber	confkey[INDEX_MAX_KEYS] pg_node_attr(array_size(nkeys));
	/* 主键 = 外键 操作符 OID */
	Oid			conpfeqop[INDEX_MAX_KEYS] pg_node_attr(array_size(nkeys));
} ForeignKeyCacheInfo;


/*
 * StdRdOptions
 *		堆的 rd_options 的标准内容。
 *
 * RelationGetFillFactor() 和 RelationGetTargetPageFreeSpace() 只能应用于
 * 使用此格式或其超集作为私有选项数据的关系。
 */
 /* 与 autovacuum 相关的重选项。 */
typedef struct AutoVacOpts
{
	bool		enabled;
	int			vacuum_threshold;
	int			vacuum_max_threshold;
	int			vacuum_ins_threshold;
	int			analyze_threshold;
	int			vacuum_cost_limit;
	int			freeze_min_age;
	int			freeze_max_age;
	int			freeze_table_age;
	int			multixact_freeze_min_age;
	int			multixact_freeze_max_age;
	int			multixact_freeze_table_age;
	int			log_min_duration;
	float8		vacuum_cost_delay;
	float8		vacuum_scale_factor;
	float8		vacuum_ins_scale_factor;
	float8		analyze_scale_factor;
} AutoVacOpts;

/* StdRdOptions->vacuum_index_cleanup 的取值 */
typedef enum StdRdOptIndexCleanup
{
	STDRD_OPTION_VACUUM_INDEX_CLEANUP_AUTO = 0,
	STDRD_OPTION_VACUUM_INDEX_CLEANUP_OFF,
	STDRD_OPTION_VACUUM_INDEX_CLEANUP_ON,
} StdRdOptIndexCleanup;

typedef struct StdRdOptions
{
	int32		vl_len_;		/* varlena 头（不要直接修改！） */
	int			fillfactor;		/* 页面填充因子，以百分比表示 (0..100) */
	int			toast_tuple_target; /* 元组 toast 的目标值 */
	AutoVacOpts autovacuum;		/* 与 autovacuum 相关的选项 */
	bool		user_catalog_table; /* 用作额外的系统目录关系 */
	int			parallel_workers;	/* 并行工作进程的最大数量 */
	StdRdOptIndexCleanup vacuum_index_cleanup;	/* 控制索引清理 */
	bool		vacuum_truncate;	/* 允许 vacuum 截断关系 */
	bool		vacuum_truncate_set;	/* vacuum_truncate 是否已设置 */

	/*
	 * vacuum 可以主动扫描但未能冻结的关系页面所占的比例。
	 * 如果禁用则为 0，如果未指定则为 -1。
	 */
	double		vacuum_max_eager_freeze_failure_rate;
} StdRdOptions;

#define HEAP_MIN_FILLFACTOR			10
#define HEAP_DEFAULT_FILLFACTOR		100

/*
 * RelationGetToastTupleTarget
 *		返回关系的 toast_tuple_target。注意参数会被多次求值！
 */
#define RelationGetToastTupleTarget(relation, defaulttarg) \
	((relation)->rd_options ? \
	 ((StdRdOptions *) (relation)->rd_options)->toast_tuple_target : (defaulttarg))

/*
 * RelationGetFillFactor
 *		返回关系的填充因子（fillfactor）。注意参数会被多次求值！
 */
#define RelationGetFillFactor(relation, defaultff) \
	((relation)->rd_options ? \
	 ((StdRdOptions *) (relation)->rd_options)->fillfactor : (defaultff))

/*
 * RelationGetTargetPageUsage
 *		返回关系每页期望的空间使用量（以字节为单位）。
 */
#define RelationGetTargetPageUsage(relation, defaultff) \
	(BLCKSZ * RelationGetFillFactor(relation, defaultff) / 100)

/*
 * RelationGetTargetPageFreeSpace
 *		返回关系每页期望的空闲空间（以字节为单位）。
 */
#define RelationGetTargetPageFreeSpace(relation, defaultff) \
	(BLCKSZ * (100 - RelationGetFillFactor(relation, defaultff)) / 100)

/*
 * RelationIsUsedAsCatalogTable
 *		返回从逻辑解码的角度看，该关系是否应被视为系统目录表。
 *		注意参数会被多次求值！
 */
#define RelationIsUsedAsCatalogTable(relation)	\
	((relation)->rd_options && \
	 ((relation)->rd_rel->relkind == RELKIND_RELATION || \
	  (relation)->rd_rel->relkind == RELKIND_MATVIEW) ? \
	 ((StdRdOptions *) (relation)->rd_options)->user_catalog_table : false)

/*
 * RelationGetParallelWorkers
 *		返回关系的 parallel_workers 重选项设置。
 *		注意参数会被多次求值！
 */
#define RelationGetParallelWorkers(relation, defaultpw) \
	((relation)->rd_options ? \
	 ((StdRdOptions *) (relation)->rd_options)->parallel_workers : (defaultpw))

/* ViewOptions->check_option 的取值 */
typedef enum ViewOptCheckOption
{
	VIEW_OPTION_CHECK_OPTION_NOT_SET,
	VIEW_OPTION_CHECK_OPTION_LOCAL,
	VIEW_OPTION_CHECK_OPTION_CASCADED,
} ViewOptCheckOption;

/*
 * ViewOptions
 *		视图的 rd_options 的内容
 */
typedef struct ViewOptions
{
	int32		vl_len_;		/* varlena 头（不要直接修改！） */
	bool		security_barrier;
	bool		security_invoker;
	ViewOptCheckOption check_option;
} ViewOptions;

/*
 * RelationIsSecurityView
 *		返回该关系是否为安全视图。注意参数会被多次求值！
 */
#define RelationIsSecurityView(relation)									\
	(AssertMacro(relation->rd_rel->relkind == RELKIND_VIEW),				\
	 (relation)->rd_options ?												\
	  ((ViewOptions *) (relation)->rd_options)->security_barrier : false)

/*
 * RelationHasSecurityInvoker
 *		如果该关系设置了 security_invoker 属性，则返回 true。
 *		注意参数会被多次求值！
 */
#define RelationHasSecurityInvoker(relation)								\
	(AssertMacro(relation->rd_rel->relkind == RELKIND_VIEW),				\
	 (relation)->rd_options ?												\
	  ((ViewOptions *) (relation)->rd_options)->security_invoker : false)

/*
 * RelationHasCheckOption
 *		如果该关系是定义了 local 或 cascaded check option 的视图，
 *		则返回 true。注意参数会被多次求值！
 */
#define RelationHasCheckOption(relation)									\
	(AssertMacro(relation->rd_rel->relkind == RELKIND_VIEW),				\
	 (relation)->rd_options &&												\
	 ((ViewOptions *) (relation)->rd_options)->check_option !=				\
	 VIEW_OPTION_CHECK_OPTION_NOT_SET)

/*
 * RelationHasLocalCheckOption
 *		如果该关系是定义了 local check option 的视图，则返回 true。
 *		注意参数会被多次求值！
 */
#define RelationHasLocalCheckOption(relation)								\
	(AssertMacro(relation->rd_rel->relkind == RELKIND_VIEW),				\
	 (relation)->rd_options &&												\
	 ((ViewOptions *) (relation)->rd_options)->check_option ==				\
	 VIEW_OPTION_CHECK_OPTION_LOCAL)

/*
 * RelationHasCascadedCheckOption
 *		如果该关系是定义了 cascaded check option 的视图，则返回 true。
 *		注意参数会被多次求值！
 */
#define RelationHasCascadedCheckOption(relation)							\
	(AssertMacro(relation->rd_rel->relkind == RELKIND_VIEW),				\
	 (relation)->rd_options &&												\
	 ((ViewOptions *) (relation)->rd_options)->check_option ==				\
	  VIEW_OPTION_CHECK_OPTION_CASCADED)

/*
 * RelationIsValid
 *		当且仅当关系描述符有效时为 true。
 */
#define RelationIsValid(relation) PointerIsValid(relation)

#define InvalidRelation ((Relation) NULL)

/*
 * RelationHasReferenceCountZero
 *		当且仅当关系的引用计数为零时为 true。
 *
 * 注意：
 *		假定关系描述符有效。
 */
#define RelationHasReferenceCountZero(relation) \
		((bool)((relation)->rd_refcnt == 0))

/*
 * RelationGetForm
 *		返回关系的 pg_class 元组。
 *
 * 注意：
 *		假定关系描述符有效。
 */
#define RelationGetForm(relation) ((relation)->rd_rel)

/*
 * RelationGetRelid
 *		返回关系的 OID
 */
#define RelationGetRelid(relation) ((relation)->rd_id)

/*
 * RelationGetNumberOfAttributes
 *		返回关系中属性的总数。
 */
#define RelationGetNumberOfAttributes(relation) ((relation)->rd_rel->relnatts)

/*
 * IndexRelationGetNumberOfAttributes
 *		返回索引中属性的数量。
 */
#define IndexRelationGetNumberOfAttributes(relation) \
		((relation)->rd_index->indnatts)

/*
 * IndexRelationGetNumberOfKeyAttributes
 *		返回索引中键属性的数量。
 */
#define IndexRelationGetNumberOfKeyAttributes(relation) \
		((relation)->rd_index->indnkeyatts)

/*
 * RelationGetDescr
 *		返回关系的元组描述符。
 */
#define RelationGetDescr(relation) ((relation)->rd_att)

/*
 * RelationGetRelationName
 *		返回关系的名称。
 *
 * 注意，名称仅在其所属的模式（namespace）内唯一。
 */
#define RelationGetRelationName(relation) \
	(NameStr((relation)->rd_rel->relname))

/*
 * RelationGetNamespace
 *		返回关系的命名空间 OID。
 */
#define RelationGetNamespace(relation) \
	((relation)->rd_rel->relnamespace)

/*
 * RelationIsMapped
 *		如果关系使用 relfilenumber 映射，则为 true。注意参数会被多次求值！
 */
#define RelationIsMapped(relation) \
	(RELKIND_HAS_STORAGE((relation)->rd_rel->relkind) && \
	 ((relation)->rd_rel->relfilenode == InvalidRelFileNumber))

#ifndef FRONTEND
/*
 * RelationGetSmgr
 *		返回关系的 smgr 文件句柄，如有需要则打开它。
 *
 * 极少有代码被授权直接访问 rel->rd_smgr。应改用此函数来获取其值。
 */
static inline SMgrRelation
RelationGetSmgr(Relation rel)
{
	if (unlikely(rel->rd_smgr == NULL))
	{
		rel->rd_smgr = smgropen(rel->rd_locator, rel->rd_backend);
		smgrpin(rel->rd_smgr);
	}
	return rel->rd_smgr;
}

/*
 * RelationCloseSmgr
 *		在 smgr 层面关闭关系（如果尚未关闭的话）。
 */
static inline void
RelationCloseSmgr(Relation relation)
{
	if (relation->rd_smgr != NULL)
	{
		smgrunpin(relation->rd_smgr);
		smgrclose(relation->rd_smgr);
		relation->rd_smgr = NULL;
	}
}
#endif							/* !FRONTEND */

/*
 * RelationGetTargetBlock
 *		获取关系当前的插入目标块。
 *
 * 如果没有当前目标块，则返回 InvalidBlockNumber。注意，目标块状态会在
 * 任何 smgr 层面的失效时被丢弃，因此如果 smgr 句柄当前未打开，则无需
 * 重新打开它。
 */
#define RelationGetTargetBlock(relation) \
	( (relation)->rd_smgr != NULL ? (relation)->rd_smgr->smgr_targblock : InvalidBlockNumber )

/*
 * RelationSetTargetBlock
 *		设置关系当前的插入目标块。
 */
#define RelationSetTargetBlock(relation, targblock) \
	do { \
		RelationGetSmgr(relation)->smgr_targblock = (targblock); \
	} while (0)

/*
 * RelationIsPermanent
 *		如果关系是永久的，则为 true。
 */
#define RelationIsPermanent(relation) \
	((relation)->rd_rel->relpersistence == RELPERSISTENCE_PERMANENT)

/*
 * RelationNeedsWAL
 *		如果关系需要 WAL，则为 true。
 *
 * 如果 wal_level = minimal，并且该关系是在当前事务中创建或截断的，
 * 则返回 false。参见 src/backend/access/transam/README 中的
 * "Skipping WAL for New RelFileLocator"。
 */
#define RelationNeedsWAL(relation)										\
	(RelationIsPermanent(relation) && (XLogIsNeeded() ||				\
	  (relation->rd_createSubid == InvalidSubTransactionId &&			\
	   relation->rd_firstRelfilelocatorSubid == InvalidSubTransactionId)))

/*
 * RelationUsesLocalBuffers
 *		如果关系的页面存储在本地缓冲区中，则为 true。
 */
#define RelationUsesLocalBuffers(relation) \
	((relation)->rd_rel->relpersistence == RELPERSISTENCE_TEMP)

/*
 * RELATION_IS_LOCAL
 *		如果一个关系是临时的，或者是在当前事务中新创建的，则可以假定
 *		它仅对当前后端可访问。这通常用于决定我们可以跳过获取锁。
 *
 * 注意参数会被多次求值
 */
#define RELATION_IS_LOCAL(relation) \
	((relation)->rd_islocaltemp || \
	 (relation)->rd_createSubid != InvalidSubTransactionId)

/*
 * RELATION_IS_OTHER_TEMP
 *		测试一个属于其他会话的临时关系。
 *
 * 通过任何方式读取另一个会话的临时表数据都不会正确工作：
 * 拥有该数据所属会话将数据保存在其私有的本地缓冲区池中，
 * 而我们无法访问它。现有的缓冲区管理器入口点
 *（ReadBuffer_common()、StartReadBuffersImpl()、read_stream_begin_impl()
 * 和 PrefetchBuffer()）已经强制执行此规则；任何新的缓冲区访问入口点
 * 也必须如此。命令级代码（TRUNCATE、ALTER TABLE、VACUUM、CLUSTER、
 * REINDEX 等）还利用此宏来生成特定于命令的错误消息。
 *
 * 注意参数会被多次求值
 */
#define RELATION_IS_OTHER_TEMP(relation) \
	((relation)->rd_rel->relpersistence == RELPERSISTENCE_TEMP && \
	 !(relation)->rd_islocaltemp)


/*
 * RelationIsScannable
 *		目前仅对未被其查询填充（populated）的物化视图为 false。
 *		这一点以后可能会变得更复杂，因此使用一个看起来像函数的宏。
 */
#define RelationIsScannable(relation) ((relation)->rd_rel->relispopulated)

/*
 * RelationIsPopulated
 *		目前，我们在物理上并不区分物化视图的 "populated"（已填充）和
 *		"scannable"（可扫描）属性，但这一点以后可能会改变。
 *		因此，在代码测试中应使用这些宏中恰当的一个。
 */
#define RelationIsPopulated(relation) ((relation)->rd_rel->relispopulated)

/*
 * RelationIsAccessibleInLogicalDecoding
 *		如果我们需要记录足够的信息以便通过解码快照进行访问，则为 true。
 */
#define RelationIsAccessibleInLogicalDecoding(relation) \
	(XLogLogicalInfoActive() && \
	 RelationNeedsWAL(relation) && \
	 (IsCatalogRelation(relation) || RelationIsUsedAsCatalogTable(relation)))

/*
 * RelationIsLogicallyLogged
 *		如果我们需要记录足够的信息以便从 WAL 流中提取数据，则为 true。
 *
 * 我们不会为未日志记录（unlogged）表记录信息（因为它们本来就不写 WAL 日志），
 * 也不会为外部表记录信息（因为它们同样不写 WAL 日志），也不会为系统表
 * 记录信息（它们的内容难以理解，而且为此让解码变得稍微复杂却收益甚微）。
 * 注意，我们*会*为用户定义的系统目录表记录信息，因为它们对用户而言
 * 大概是有意义的……
 */
#define RelationIsLogicallyLogged(relation) \
	(XLogLogicalInfoActive() && \
	 RelationNeedsWAL(relation) && \
	 (relation)->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&	\
	 !IsCatalogRelation(relation))

/* utils/cache/relcache.c 中的例程 */
extern void RelationIncrementReferenceCount(Relation rel);
extern void RelationDecrementReferenceCount(Relation rel);

#endif							/* REL_H */
