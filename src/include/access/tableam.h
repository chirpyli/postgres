/*-------------------------------------------------------------------------
 *
 * tableam.h
 *	  POSTGRES 表访问方法的定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/tableam.h
 *
 * 说明
 *		更高层的文档说明请参阅 tableam.sgml。
 *
 *-------------------------------------------------------------------------
 */
#ifndef TABLEAM_H
#define TABLEAM_H

#include "access/relscan.h"
#include "access/sdir.h"
#include "access/xact.h"
#include "executor/tuptable.h"
#include "storage/read_stream.h"
#include "utils/rel.h"
#include "utils/snapshot.h"


#define DEFAULT_TABLE_ACCESS_METHOD	"heap"

/* GUC 参数 */
extern PGDLLIMPORT char *default_table_access_method;
extern PGDLLIMPORT bool synchronize_seqscans;


struct BulkInsertStateData;
struct IndexInfo;
struct SampleScanState;
struct VacuumParams;
struct ValidateIndexState;

/*
 * scan_begin 回调中 flags 参数的位掩码取值。
 */
typedef enum ScanOptions
{
	/* 可指定其中一个 SO_TYPE_* */
	SO_TYPE_SEQSCAN = 1 << 0,
	SO_TYPE_BITMAPSCAN = 1 << 1,
	SO_TYPE_SAMPLESCAN = 1 << 2,
	SO_TYPE_TIDSCAN = 1 << 3,
	SO_TYPE_TIDRANGESCAN = 1 << 4,
	SO_TYPE_ANALYZE = 1 << 5,

	/* 可指定多个 SO_ALLOW_* */
	/* 允许或禁止使用访问策略 */
	SO_ALLOW_STRAT = 1 << 6,
	/* 是否向 syncscan 逻辑上报位置？ */
	SO_ALLOW_SYNC = 1 << 7,
	/* 是否逐页验证可见性？ */
	SO_ALLOW_PAGEMODE = 1 << 8,

	/* 扫描结束时是否注销快照？ */
	SO_TEMP_SNAPSHOT = 1 << 9,
}			ScanOptions;

/*
 * table_{update,delete,lock_tuple} 以及表 AM 内部可见性例程的结果码。
 */
typedef enum TM_Result
{
	/*
	 * 表示操作成功（即执行了 update/delete、获取到了锁）。
	 */
	TM_Ok,

	/* 受影响的元组对相应快照不可见 */
	TM_Invisible,

	/* 受影响的元组已被当前后端修改过 */
	TM_SelfModified,

	/*
	 * 受影响的元组已被另一个事务更新。这也包括元组被移动到另一个分区的情况。
	 */
	TM_Updated,

	/* 受影响的元组已被另一个事务删除 */
	TM_Deleted,

	/*
	 * 受影响的元组当前正被另一个会话修改。只有当 table_(update/delete/lock_tuple)
	 * 被指示不等待时才会返回此值。
	 */
	TM_BeingModified,

	/* 无法获取锁，操作被跳过。仅由 lock_tuple 使用 */
	TM_WouldBlock,
} TM_Result;

/*
 * table_update(..., update_indexes*..) 的结果码。
 * 用于决定需要更新哪些索引。
 */
typedef enum TU_UpdateIndexes
{
	/* 没有索引列被更新（包括元组的 TID 寻址） */
	TU_None,

	/* 某个非汇总型索引列被更新，或 TID 已改变 */
	TU_All,

	/* 只有汇总型列被更新，TID 未改变 */
	TU_Summarizing,
} TU_UpdateIndexes;

/*
 * 当 table_tuple_update、table_tuple_delete 或 table_tuple_lock 因为目标元组
 * 已经过期而失败时，它们会填充这个结构体，向调用者说明发生了什么。当这些函数
 * 成功时，不应依赖该结构体的内容，但 `traversed` 例外，它在成功和失败两种情况下都
 * 可能被设置。
 *
 * ctid 是目标的 ctid 链接：如果目标被删除，它与目标的 TID 相同；如果目标被更新，
 * 则它是替换元组的存储位置。
 *
 * xmax 是使该元组过时的那个事务的 XID。如果调用者想要访问替换元组，必须先核对
 * 它与本值一致，才能确信替换元组确实匹配。如果目标是 !LP_NORMAL（预期只出现在
 * 从 syscache 取出的 TID 上），本值为 InvalidTransactionId。
 *
 * cmax 是使该元组过时的命令的 CID，但仅当失败码为 TM_SelfModified（即当前事务中
 * 的某次操作使该元组过时）时才有意义；否则 cmax 为零。（之所以做此限制，是因为
 * HeapTupleHeaderGetCmax 对由其他事务过时的元组不适用。）
 *
 * traversed 表示为了尝试锁定目标元组而是否跟随了一条更新链。
 * （成功和失败两种情况下都可能被设置。）
 */
typedef struct TM_FailureData
{
	ItemPointerData ctid;
	TransactionId xmax;
	CommandId	cmax;
	bool		traversed;
} TM_FailureData;

/*
 * 调用 table_index_delete_tuples() 时使用的状态。
 *
 * 表示由表 TID 引用、由索引 AM 从索引元组中取出的表元组的状态。状态包含删除操作的
 * 高层参数，外加两个可变的、由 palloc() 分配的数组，用于记录各个表元组的状态信息。
 * 概念上这两个数组可以理解为一个单一数组。使用两个数组可以让 TM_IndexDelete
 * 结构体保持较小的体积，从而加快对第一个数组（deltids 数组）的排序。
 *
 * 一些索引 AM 调用者执行简单的索引元组删除（指定 bottomup = false），并且只包含
 * 已知死亡的 deltids。这些已知死亡的条目都直接被标记为 knowndeletable = true
 * （通常它们是来自 LP_DEAD 标记的索引元组的 TID），但这并非硬性要求。
 *
 * 指定 bottomup = true 的调用者是"自底向上索引删除"调用者。对于这类调用者，表 AM
 * 需要考虑的问题更为微妙，因为它们要求表 AM 执行极具投机性的工作，而且可能只期望
 * 表 AM 检查所有条目中的一小部分。调用者不允许为任何条目指定 knowndeletable = true，
 * 因为一切都高度投机。自底向上调用者向表 AM 提供上下文与提示——详见下文关于索引 AM
 * 与表 AM 在自底向上索引删除期间应如何协作的注释。
 *
 * 简单的索引删除调用者也可能要求表 AM 执行投机性工作。这有点类似自底向上删除，
 * 但差别也不小。表 AM 只有在顺带执行时几乎零成本的情况下，才会为简单删除调用者
 * 执行投机性工作（同时始终执行那些使 knowndeletable/LP_DEAD 索引元组能够在索引 AM
 * 内被删除所需的操作）。这正是简单索引删除调用者可以提前指定 knowndeletable = false
 * 的真实原因（其含义是"顺带成本低廉时，检查我是否能够删除对应的索引元组"）。索引 AM
 * 只应为那些 TID 指向表 AM 本来就要访问的表块的索引元组包含"额外"条目（针对基于块的
 * 表 AM 而言）。表 AM 并不被强制要求检查这些"额外" TID，但在实践中，基于块的 AM 总是
 * 能够做到这一点。
 *
 * deltids/status 数组的最终内容，对于要求表 AM 执行投机性工作的调用者（即任何条目
 * 提前将 knowndeletable 置为 false 的情况）来说是有意义的。这些索引 AM 调用者自然
 * 需要查阅最终状态，以判断哪些索引元组实际上可被删除。
 *
 * 索引 AM 可以通过设置 idxoffnum（和/或依赖每个条目能够用 tid 唯一标识）来记录哪个
 * 索引元组对应哪个 deltid，这在数组最终内容需要被解释时非常重要——数组在表 AM 处理
 * 之后可能会从初始大小收缩，和/或条目的顺序发生变化（表 AM 可能因自身原因对 deltids
 * 数组排序）。自底向上调用者可能会发现，从表 AM 返回时最终的 ndeltids 为 0，在这种
 * 情况下没有任何索引元组可以被删除。简单删除调用者可以信赖：它们已知可删除的条目
 * 都会以可删除的形式出现在最终数组中。
 */
typedef struct TM_IndexDelete
{
	ItemPointerData tid;		/* 来自索引元组的表 TID */
	int16		id;				/* 在 TM_IndexStatus 数组中的偏移 */
} TM_IndexDelete;

typedef struct TM_IndexStatus
{
	OffsetNumber idxoffnum;		/* 索引 AM 的页内偏移号 */
	bool		knowndeletable; /* 当前是否已知可删除？ */

	/* 以下为自底向上索引删除专用字段 */
	bool		promising;		/* 有希望的（重复）索引元组？ */
	int16		freespace;		/* 删除后释放的索引空间 */
} TM_IndexStatus;

/*
 * 索引 AM 与表 AM 的协作是自底向上索引删除设计的核心。索引 AM 通过将一些条目标记为
 * "有希望"，向表 AM 提供去哪里查找的提示。索引 AM 是对那些被强烈怀疑为 UPDATE
 * （在逻辑上没有修改被索引值）遗留下来旧版本的重复索引元组做此标记的。索引 AM 可能会
 * 发现，仅当这些条目被认为在近期受到过此类 UPDATE 影响时才将其标记为"有希望"会更有帮助。
 *
 * 自底向上索引删除一开始会撒下大网，通常是包含目标索引页上的所有 TID。检查事务状态
 * 信息的开销由表 AM 负责。表 AM 处于主导地位，但需要索引 AM 的悉心引导。索引 AM 要求
 * 达到 bottomupfreespace 目标，而表 AM 通过累加已知可删除条目的逐条目 freespace 值
 * 来衡量朝向该目标的进度。（所有 !bottomup 调用者都可以直接将这些空间相关字段置零。）
 */
typedef struct TM_IndexDeleteOp
{
	Relation	irel;			/* 目标索引关系 */
	BlockNumber iblknum;		/* 索引块号（用于错误报告） */
	bool		bottomup;		/* 是否为自底向上（而非简单）删除？ */
	int			bottomupfreespace;	/* 自底向上删除的空间目标 */

	/* 以下为可变的逐 TID 信息（由索引 AM 初始化条目） */
	int			ndeltids;		/* 当前 deltids/status 元素个数 */
	TM_IndexDelete *deltids;
	TM_IndexStatus *status;
} TM_IndexDeleteOp;

/* table_tuple_insert 的 "options" 标志位 */
/* TABLE_INSERT_SKIP_WAL 曾经是 0x0001；现在由 RelationNeedsWAL() 控制 */
#define TABLE_INSERT_SKIP_FSM		0x0002
#define TABLE_INSERT_FROZEN			0x0004
#define TABLE_INSERT_NO_LOGICAL		0x0008

/* table_tuple_lock 的标志位 */
/* 若锁模式不冲突，则跟随更新仍在进行的元组 */
#define TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS	(1 << 0)
/* 跟随更新链并锁定元组的最新版本 */
#define TUPLE_LOCK_FLAG_FIND_LAST_VERSION		(1 << 1)


/* table_index_build_scan 回调函数的类型定义 */
typedef void (*IndexBuildCallback) (Relation index,
									ItemPointer tid,
									Datum *values,
									bool *isnull,
									bool tupleIsAlive,
									void *state);

/*
 * 表 AM 的 API 结构体。注意它必须以"服务进程生命周期"的方式分配，通常作为 static
 * const 结构体，然后由 FormData_pg_am.amhandler 返回。
 *
 * 在大多数情况下，直接调用这些回调是不恰当的，应当改用 table_* 包装函数。
 *
 * GetTableAmRoutine() 会断言所需的回调都已被填充，新增回调时不要忘记更新它。
 */
typedef struct TableAmRoutine
{
	/* 本字段必须设置为 T_TableAmRoutine */
	NodeTag		type;


	/* ------------------------------------------------------------------------
	 * 与 slot 相关的回调。
	 * ------------------------------------------------------------------------
	 */

	/*
	 * 返回适合存储本 AM 元组的 slot 实现。
	 */
	const TupleTableSlotOps *(*slot_callbacks) (Relation rel);


	/* ------------------------------------------------------------------------
	 * 表扫描回调。
	 * ------------------------------------------------------------------------
	 */

	/*
	 * 开启对 `rel` 的扫描。该回调必须返回一个 TableScanDesc，它通常会被嵌入到
	 * 一个更大的、AM 特定的结构体中。
	 *
	 * 如果 nkeys != 0，结果需要用这些扫描键进行过滤。
	 *
	 * 如果 pscan 不为 NULL，它已经由 parallelscan_initialize() 初始化过，并且
	 * 必须对应同一关系。该参数仅在来自 table_beginscan_parallel() 时才会被设置。
	 *
	 * `flags` 是一个位掩码，用于指示扫描的类型（ScanOptions 的 SO_TYPE_*，
	 * 目前只能指定其中一个）、控制扫描行为的选项（ScanOptions 的 SO_ALLOW_*，
	 * 可指定多个，AM 可以忽略其不支持的选项），以及快照是否需要在 scan_end 时被
	 * 释放（ScanOptions 的 SO_TEMP_SNAPSHOT）。
	 */
	TableScanDesc (*scan_begin) (Relation rel,
								 Snapshot snapshot,
								 int nkeys, struct ScanKeyData *key,
								 ParallelTableScanDesc pscan,
								 uint32 flags);

	/*
	 * 释放资源并解除扫描分配。如果 TableScanDesc.temp_snap 为真，则需要注销
	 * TableScanDesc.rs_snapshot。
	 */
	void		(*scan_end) (TableScanDesc scan);

	/*
	 * 重启关系扫描。如果 set_params 为 true，则应考虑 allow_{strat,
	 * sync, pagemode}（见 scan_begin）的变更。
	 */
	void		(*scan_rescan) (TableScanDesc scan, struct ScanKeyData *key,
								bool set_params, bool allow_strat,
								bool allow_sync, bool allow_pagemode);

	/*
	 * 从 `scan` 中取回下一个元组，存入 slot。
	 */
	bool		(*scan_getnextslot) (TableScanDesc scan,
									 ScanDirection direction,
									 TupleTableSlot *slot);

	/*-----------
	 * 可选函数，用于提供对 ItemPointer 范围的扫描。实现者必须同时提供这两个
	 * 函数，或者两个都不提供。
	 *
	 * scan_set_tidrange 的实现者必须自行处理任意取值的 ItemPointer。也就是说，
	 * 它们必须能够处理以下每一种情况：
	 *
	 * 1) mintid 或 maxtid 超出了表的末尾；并且
	 * 2) mintid 大于 maxtid；并且
	 * 3) mintid 或 maxtid 的项偏移超出了 AM 所允许的最大偏移。
	 *
	 * 实现者可以假定：scan_set_tidrange 总是在 scan_getnextslot_tidrange 之前
	 * 被调用，或者在 scan_rescan 之后、任何后续对 scan_getnextslot_tidrange
	 * 的调用之前被调用。
	 */
	void		(*scan_set_tidrange) (TableScanDesc scan,
									  ItemPointer mintid,
									  ItemPointer maxtid);

	/*
	 * 从 `scan` 中取回下一个处于 scan_set_tidrange 所定义 TID 范围内的元组。
	 */
	bool		(*scan_getnextslot_tidrange) (TableScanDesc scan,
											  ScanDirection direction,
											  TupleTableSlot *slot);

	/* ------------------------------------------------------------------------
	 * 与并行表扫描相关的函数。
	 * ------------------------------------------------------------------------
	 */

	/*
	 * 估计对本关系进行并行扫描所需的共享内存大小。快照无需计入。
	 */
	Size		(*parallelscan_estimate) (Relation rel);

	/*
	 * 为本关系的并行扫描初始化 ParallelTableScanDesc。`pscan` 的大小将依据本
	 * 关系对应的 parallelscan_estimate() 结果。
	 */
	Size		(*parallelscan_initialize) (Relation rel,
											ParallelTableScanDesc pscan);

	/*
	 * 为一次新的扫描重新初始化 `pscan`。`rel` 将与 parallelscan_initialize
	 * 初始化 `pscan` 时所用的是同一个关系。
	 */
	void		(*parallelscan_reinitialize) (Relation rel,
											  ParallelTableScanDesc pscan);


	/* ------------------------------------------------------------------------
	 * 索引扫描回调
	 * ------------------------------------------------------------------------
	 */

	/*
	 * 为从关系中取回元组做准备，这在为索引扫描取回元组时是必需的。该回调必须
	 * 返回一个 IndexFetchTableData，AM 通常会把它嵌入到一个带有额外信息的更大
	 * 结构体中。
	 *
	 * 随后即可通过 index_fetch_tuple 取回索引扫描所需的元组。
	 */
	struct IndexFetchTableData *(*index_fetch_begin) (Relation rel);

	/*
	 * 重置索引取回。通常这会释放保存在 IndexFetchTableData 中的跨索引取回资源。
	 */
	void		(*index_fetch_reset) (struct IndexFetchTableData *data);

	/*
	 * 释放资源并解除索引取回分配。
	 */
	void		(*index_fetch_end) (struct IndexFetchTableData *data);

	/*
	 * 依据 `snapshot` 完成可见性测试后，将 `tid` 处的元组取回到 `slot`。如果
	 * 找到元组且通过了可见性测试，返回 true，否则返回 false。
	 *
	 * 注意，对于那些在索引列未改变时不强制更新索引的 AM，即使 tid 指向的是
	 * 元组的旧版本，也需要返回对快照可见的当前/正确版本元组。
	 *
	 * 对某个 tid 第一次调用 index_fetch_tuple 时，*call_again 为 false。如果
	 * 可能还存在另一个匹配该 tid 的元组，index_fetch_tuple 需要将 *call_again
	 * 置为 true，以此通知调用者应当再次调用 index_fetch_tuple 处理同一个 tid。
	 *
	 * 如果 all_dead 不为 NULL，当且仅当确定没有任何后端还需要看到该元组时，
	 * index_fetch_tuple 才应将 *all_dead 置为 true。索引 AM 可以利用这一点，
	 * 在将来的搜索中避免返回该 tid。
	 */
	bool		(*index_fetch_tuple) (struct IndexFetchTableData *scan,
									  ItemPointer tid,
									  Snapshot snapshot,
									  TupleTableSlot *slot,
									  bool *call_again, bool *all_dead);


	/* ------------------------------------------------------------------------
	 * 针对单个元组的非修改型操作回调
	 * ------------------------------------------------------------------------
	 */

	/*
	 * 依据 `snapshot` 完成可见性测试后，将 `tid` 处的元组取回到 `slot`。如果
	 * 找到元组且通过了可见性测试，返回 true，否则返回 false。
	 */
	bool		(*tuple_fetch_row_version) (Relation rel,
											ItemPointer tid,
											Snapshot snapshot,
											TupleTableSlot *slot);

	/*
	 * tid 对本关系的扫描是否有效。
	 */
	bool		(*tuple_tid_valid) (TableScanDesc scan,
									ItemPointer tid);

	/*
	 * 通过将 `tid` 更新为指向最新版本，返回 `tid` 处元组的最新版本。
	 */
	void		(*tuple_get_latest_tid) (TableScanDesc scan,
										 ItemPointer tid);

	/*
	 * `slot` 中的元组是否满足 `snapshot`？slot 必须是该 AM 适用的类型。
	 */
	bool		(*tuple_satisfies_snapshot) (Relation rel,
											 TupleTableSlot *slot,
											 Snapshot snapshot);

	/* 参见 table_index_delete_tuples() */
	TransactionId (*index_delete_tuples) (Relation rel,
										  TM_IndexDeleteOp *delstate);


	/* ------------------------------------------------------------------------
	 * 物理元组的修改操作。
	 * ------------------------------------------------------------------------
	 */

	/* 参数说明参见 table_tuple_insert() */
	void		(*tuple_insert) (Relation rel, TupleTableSlot *slot,
								 CommandId cid, int options,
								 struct BulkInsertStateData *bistate);

	/* 参数说明参见 table_tuple_insert_speculative() */
	void		(*tuple_insert_speculative) (Relation rel,
											 TupleTableSlot *slot,
											 CommandId cid,
											 int options,
											 struct BulkInsertStateData *bistate,
											 uint32 specToken);

	/* 参数说明参见 table_tuple_complete_speculative() */
	void		(*tuple_complete_speculative) (Relation rel,
											   TupleTableSlot *slot,
											   uint32 specToken,
											   bool succeeded);

	/* 参数说明参见 table_multi_insert() */
	void		(*multi_insert) (Relation rel, TupleTableSlot **slots, int nslots,
								 CommandId cid, int options, struct BulkInsertStateData *bistate);

	/* 参数说明参见 table_tuple_delete() */
	TM_Result	(*tuple_delete) (Relation rel,
								 ItemPointer tid,
								 CommandId cid,
								 Snapshot snapshot,
								 Snapshot crosscheck,
								 bool wait,
								 TM_FailureData *tmfd,
								 bool changingPart);

	/* 参数说明参见 table_tuple_update() */
	TM_Result	(*tuple_update) (Relation rel,
								 ItemPointer otid,
								 TupleTableSlot *slot,
								 CommandId cid,
								 Snapshot snapshot,
								 Snapshot crosscheck,
								 bool wait,
								 TM_FailureData *tmfd,
								 LockTupleMode *lockmode,
								 TU_UpdateIndexes *update_indexes);

	/* 参数说明参见 table_tuple_lock() */
	TM_Result	(*tuple_lock) (Relation rel,
							   ItemPointer tid,
							   Snapshot snapshot,
							   TupleTableSlot *slot,
							   CommandId cid,
							   LockTupleMode mode,
							   LockWaitPolicy wait_policy,
							   uint8 flags,
							   TM_FailureData *tmfd);

	/*
	 * 执行完成经由 tuple_insert 与 multi_insert（并指定了 BulkInsertState）
	 * 所做插入所必需的操作。树内访问方法已不再使用本回调。
	 *
	 * 通常，tuple_insert 与 multi_insert 的调用者会直接传入所有适用于它们的
	 * 标志位，而每个 AM 需要自行判断其中哪些对它有意义，然后仅在 finish_bulk_insert
	 * 中对那些标志位采取行动，忽略其余标志位。
	 *
	 * 可选回调。
	 */
	void		(*finish_bulk_insert) (Relation rel, int options);


	/* ------------------------------------------------------------------------
	 * 与 DDL 相关的功能。
	 * ------------------------------------------------------------------------
	 */

	/*
	 * 本回调需要为 `rel` 创建新的关系存储，并采用与 `persistence` 相适应的
	 * 持久化行为。
	 *
	 * 注意，只能依赖由 RelationBuildLocalRelation() 填充的那部分 relcache，
	 * 并且关系的目录项要么尚未存在（新关系），要么仍指向旧的 relfilelocator。
	 *
	 * 作为输出，*freezeXid 与 *minmulti 必须设置为适用于
	 * pg_class.{relfrozenxid, relminmxid} 的值。对于那些不需要填充这些字段的
	 * AM，可分别将其设为 InvalidTransactionId 与 InvalidMultiXactId。
	 *
	 * 另见 table_relation_set_new_filelocator()。
	 */
	void		(*relation_set_new_filelocator) (Relation rel,
												 const RelFileLocator *newrlocator,
												 char persistence,
												 TransactionId *freezeXid,
												 MultiXactId *minmulti);

	/*
	 * 本回调需要从 `rel` 当前的 relfilelocator 中移除所有内容。无需做任何
	 * 事务性行为的处理。通常这可以通过将底层存储截断到最小尺寸来实现。
	 *
	 * 另见 table_relation_nontransactional_truncate()。
	 */
	void		(*relation_nontransactional_truncate) (Relation rel);

	/*
	 * 参见 table_relation_copy_data()。
	 *
	 * 这通常可以通过直接复制底层存储来实现，除非存储内部还引用了表空间。
	 */
	void		(*relation_copy_data) (Relation rel,
									   const RelFileLocator *newrlocator);

	/* 参见 table_relation_copy_for_cluster() */
	void		(*relation_copy_for_cluster) (Relation OldTable,
											  Relation NewTable,
											  Relation OldIndex,
											  bool use_sort,
											  TransactionId OldestXmin,
											  TransactionId *xid_cutoff,
											  MultiXactId *multi_cutoff,
											  double *num_tuples,
											  double *tups_vacuumed,
											  double *tups_recently_dead);

	/*
	 * 响应作用于该关系的 VACUUM 命令。VACUUM 可能由用户触发，也可能由
	 * autovacuum 触发。AM 具体执行哪些操作，很大程度上取决于各个 AM 自身。
	 *
	 * 进入本回调时，事务已经建立，并且关系已被施加 ShareUpdateExclusive 锁。
	 *
	 * 注意，无论是 VACUUM FULL（及 CLUSTER），还是 ANALYZE，都不会经过本例程，
	 * 即便（对 ANALYZE 而言）它是同一个 VACUUM 命令的一部分。
	 *
	 * 未来可能还需要一个独立的回调来与 autovacuum 的调度进行整合。
	 */
	void		(*relation_vacuum) (Relation rel,
									struct VacuumParams *params,
									BufferAccessStrategy bstrategy);

	/*
	 * 为分析 `scan` 的 `blockno` 块做准备。扫描已通过 table_beginscan_analyze()
	 * 启动（参见 table_scan_analyze_next_tuple()）。

	 *
	 * 该回调可能会获取诸如锁之类的资源，这些资源会一直持有到
	 * table_scan_analyze_next_tuple() 返回 false 为止。例如，在块上的所有元组都
	 * 已被 scan_analyze_next_tuple 分析完毕之前，持有某个锁是有意义的。

	 *
	 * 如果该块不适合采样，例如它是一个不可能包含元组的元页面，该回调可以返回 false。
	 *
	 *
	 * XXX：这明显主要面向基于块的 AM。目前尚不清楚对非基于块的 AM 而言一个良好的
	 * 接口应该是什么样，因此暂时还没有这样的接口。
	 *
	 */
	bool		(*scan_analyze_next_block) (TableScanDesc scan,
											ReadStream *stream);

	/*
	 * 参见 table_scan_analyze_next_tuple()。
	 *
	 * 并非每个 AM 都对"死亡行"有有意义的概念，在这种情况下可以不递增 *deadrows，
	 * 但要注意这可能会影响 autovacuum 的调度（见 relation_vacuum 回调的注释）。

	 */
	bool		(*scan_analyze_next_tuple) (TableScanDesc scan,
											TransactionId OldestXmin,
											double *liverows,
											double *deadrows,
											TupleTableSlot *slot);

	/* 参数说明参见 table_index_build_range_scan */
	double		(*index_build_range_scan) (Relation table_rel,
										   Relation index_rel,
										   struct IndexInfo *index_info,
										   bool allow_sync,
										   bool anyvisible,
										   bool progress,
										   BlockNumber start_blockno,
										   BlockNumber numblocks,
										   IndexBuildCallback callback,
										   void *callback_state,
										   TableScanDesc scan);

	/* 参数说明参见 table_index_validate_scan */
	void		(*index_validate_scan) (Relation table_rel,
										Relation index_rel,
										struct IndexInfo *index_info,
										Snapshot snapshot,
										struct ValidateIndexState *state);


	/* ------------------------------------------------------------------------
	 * 其他函数。
	 * ------------------------------------------------------------------------
	 */

	/*
	 * 参见 table_relation_size()。
	 *
	 * 注意，目前少数调用者会使用 MAIN_FORKNUM 的大小来推断可能相关的块范围
	 * （brin、analyze）。我们很可能需要在某个时候为这些场景修订该接口。
	 */
	uint64		(*relation_size) (Relation rel, ForkNumber forkNumber);


	/*
	 * 如果关系需要一个 TOAST 表，本回调应返回 true；否则返回 false。它可以在
	 * 做决定前查看关系的元组描述符，但如果它使用其他方式存储大值（或根本不支持），
	 * 也可以直接返回 false。
	 */
	bool		(*relation_needs_toast_table) (Relation rel);

	/*
	 * 本回调应返回用于为本 AM 实现 TOAST 表的表 AM 的 OID。如果
	 * relation_needs_toast_table 回调始终返回 false，则本回调并非必需。

	 */
	Oid			(*relation_toast_am) (Relation rel);

	/*
	 * 当对由本 AM 实现的 TOAST 表中存储的值进行去 TOAST 时，会调用本回调。
	 * 详见 table_relation_fetch_toast_slice()。
	 */
	void		(*relation_fetch_toast_slice) (Relation toastrel, Oid valueid,
											   int32 attrsize,
											   int32 sliceoffset,
											   int32 slicelength,
											   struct varlena *result);


	/* ------------------------------------------------------------------------
	 * 规划器相关函数。
	 * ------------------------------------------------------------------------
	 */

	/*
	 * 参见 table_relation_estimate_size()。
	 *
	 * 尽管是面向块的，但对于内部不使用块的 AM 来说，转换成可用的表示形式
	 * 应该也不算太难。
	 *
	 * 它与 relation_size 回调的不同之处在于：它返回用于规划目的的大小估计
	 * （包括关系大小和元组数），而非返回一个当前正确的估计值。

	 */
	void		(*relation_estimate_size) (Relation rel, int32 *attr_widths,
										   BlockNumber *pages, double *tuples,
										   double *allvisfrac);


	/* ------------------------------------------------------------------------
	 * 执行器相关函数。
	 * ------------------------------------------------------------------------
	 */

	/*
	 * 将位图表扫描的下一个元组取入 `slot`，若找到可见元组则返回 true，
	 * 否则返回 false。
	 *
	 * 如果位图对所选中页面是有损（lossy）的，则递增 `lossy_pages`；
	 * 否则递增 `exact_pages`。这些信息会被记录，用于在 EXPLAIN ANALYZE 输出中展示。

	 *
	 * 从位图预取额外数据的职责留给表 AM。
	 *
	 * 这是一个可选回调。
	 */
	bool		(*scan_bitmap_next_tuple) (TableScanDesc scan,
										   TupleTableSlot *slot,
										   bool *recheck,
										   uint64 *lossy_pages,
										   uint64 *exact_pages);

	/*
	 * 为从采样扫描的下一个块中取回元组做准备。如果采样扫描已结束则返回 false，
	 * 否则返回 true。`scan` 是通过 table_beginscan_sampling() 启动的。

	 *
	 * 通常，它会首先通过调用 TsmRoutine 的 NextSampleBlock() 回调（若不为 NULL）
	 * 来确定目标块；否则会对所有块执行顺序扫描。随后通常会读入并对该块加 pin。


	 *
	 * 由于 TsmRoutine 接口是基于块的，需要向 NextSampleBlock() 传入一个块。
	 * 如果这对某个 AM 不合适，它需要在内部完成内部表示与基于块的表示之间的映射。

	 *
	 * 注意，持有容易引发死锁的资源（例如 lwlocks）直到 scan_sample_next_tuple()
	 * 耗尽该块上的元组为止，这是不可接受的——该元组很可能被返回给上层查询节点，
	 * 而下一次调用可能要很久之后才会发生。持有 buffer pin 之类的资源显然是没问题的。


	 *
	 * 目前，实现该接口是必需的，因为没有其他方式（与位图扫描等不同）来实现采样扫描。
	 * 如果无法实现，AM 可以报错。

	 */
	bool		(*scan_sample_next_block) (TableScanDesc scan,
										   struct SampleScanState *scanstate);

	/*
	 * 本回调仅在 scan_sample_next_block 返回 true 之后才会被调用，它应使用
	 * TsmRoutine 的 NextSampleTuple() 回调，确定从该选中块返回下一个元组。
	 *
	 * 该回调需要执行可见性检查，并且只返回可见的元组。这显然可能意味着要多次调用
	 * NextSampleTuple()。

	 *
	 * TsmRoutine 接口假定给定页面上存在最大偏移，因此如果某个 AM 不适用这一点，
	 * 它需要以某种方式模拟这一假定。
	 */
	bool		(*scan_sample_next_tuple) (TableScanDesc scan,
										   struct SampleScanState *scanstate,
										   TupleTableSlot *slot);

} TableAmRoutine;


/* ----------------------------------------------------------------------------
 * Slot 相关函数。
 * ----------------------------------------------------------------------------
 */

/*
 * 返回适合持有该关系相应类型元组的 slot 回调。适用于普通表、视图、
 * 外部表以及分区表。

 */
extern const TupleTableSlotOps *table_slot_callbacks(Relation relation);

/*
 * 使用 table_slot_callbacks() 返回的回调创建一个 slot，并将其注册到 *reglist 上。

 */
extern TupleTableSlot *table_slot_create(Relation relation, List **reglist);


/* ----------------------------------------------------------------------------
 * 表扫描函数。
 * ----------------------------------------------------------------------------
 */

/*
 * 开启对 `rel` 的扫描。返回的元组都通过 `snapshot` 的可见性测试，并且
 * 如果 nkeys != 0，结果还会被这些扫描键过滤。
 */
static inline TableScanDesc
table_beginscan(Relation rel, Snapshot snapshot,
				int nkeys, struct ScanKeyData *key)
{
	uint32		flags = SO_TYPE_SEQSCAN |
		SO_ALLOW_STRAT | SO_ALLOW_SYNC | SO_ALLOW_PAGEMODE;

	return rel->rd_tableam->scan_begin(rel, snapshot, nkeys, key, NULL, flags);
}

/*
 * 类似 table_beginscan()，但用于扫描系统目录。它会自动使用适合扫描系统目录
 * 关系的快照。
 */
extern TableScanDesc table_beginscan_catalog(Relation relation, int nkeys,
											 struct ScanKeyData *key);

/*
 * 类似 table_beginscan()，但 table_beginscan_strat() 提供了一个扩展 API，
 * 让调用者控制是否可以使用非默认的缓冲区访问策略，以及是否可以选择 syncscan
 * （这可能导致扫描不从 0 号块开始）。在普通的 table_beginscan 中，这两个选项
 * 默认都为 true（即扫描可能不从 0 号块开始）。
 */
static inline TableScanDesc
table_beginscan_strat(Relation rel, Snapshot snapshot,
					  int nkeys, struct ScanKeyData *key,
					  bool allow_strat, bool allow_sync)
{
	uint32		flags = SO_TYPE_SEQSCAN | SO_ALLOW_PAGEMODE;

	if (allow_strat)
		flags |= SO_ALLOW_STRAT;
	if (allow_sync)
		flags |= SO_ALLOW_SYNC;

	return rel->rd_tableam->scan_begin(rel, snapshot, nkeys, key, NULL, flags);
}

/*
 * table_beginscan_bm 是设置位图堆扫描所用 TableScanDesc 的另一种入口点。
 * 尽管这种扫描技术与标准的顺序扫描大不相同，但两者之间仍有足够的共性，
 * 使得复用同一数据结构是值得的。

 */
static inline TableScanDesc
table_beginscan_bm(Relation rel, Snapshot snapshot,
				   int nkeys, struct ScanKeyData *key)
{
	uint32		flags = SO_TYPE_BITMAPSCAN | SO_ALLOW_PAGEMODE;

	return rel->rd_tableam->scan_begin(rel, snapshot, nkeys, key,
									   NULL, flags);
}

/*
 * table_beginscan_sampling 是设置 TABLESAMPLE 扫描所用 TableScanDesc 的
 * 另一种入口点。与位图扫描类似，尽管行为大不相同，但复用同一数据结构是
 * 值得的。除 table_beginscan_strat 提供的选项外，本次调用还可控制是否使用
 * 页面模式可见性检查。
 */
static inline TableScanDesc
table_beginscan_sampling(Relation rel, Snapshot snapshot,
						 int nkeys, struct ScanKeyData *key,
						 bool allow_strat, bool allow_sync,
						 bool allow_pagemode)
{
	uint32		flags = SO_TYPE_SAMPLESCAN;

	if (allow_strat)
		flags |= SO_ALLOW_STRAT;
	if (allow_sync)
		flags |= SO_ALLOW_SYNC;
	if (allow_pagemode)
		flags |= SO_ALLOW_PAGEMODE;

	return rel->rd_tableam->scan_begin(rel, snapshot, nkeys, key, NULL, flags);
}

/*
 * table_beginscan_tid 是设置 Tid 扫描所用 TableScanDesc 的另一种入口点。
 * 与位图扫描类似，尽管行为大不相同，但复用同一数据结构是值得的。
 */
static inline TableScanDesc
table_beginscan_tid(Relation rel, Snapshot snapshot)
{
	uint32		flags = SO_TYPE_TIDSCAN;

	return rel->rd_tableam->scan_begin(rel, snapshot, 0, NULL, NULL, flags);
}

/*
 * table_beginscan_analyze 是设置 ANALYZE 扫描所用 TableScanDesc 的另一种
 * 入口点。与位图扫描类似，尽管行为大不相同，但复用同一数据结构是值得的。
 */
static inline TableScanDesc
table_beginscan_analyze(Relation rel)
{
	uint32		flags = SO_TYPE_ANALYZE;

	return rel->rd_tableam->scan_begin(rel, NULL, 0, NULL, NULL, flags);
}

/*
 * 结束关系扫描。
 */
static inline void
table_endscan(TableScanDesc scan)
{
	scan->rs_rd->rd_tableam->scan_end(scan);
}

/*
 * 重启关系扫描。
 */
static inline void
table_rescan(TableScanDesc scan,
			 struct ScanKeyData *key)
{
	scan->rs_rd->rd_tableam->scan_rescan(scan, key, false, false, false, false);
}

/*
 * 在修改参数之后重启关系扫描。
 *
 * 本次调用允许在开启一次全新扫描之前，修改缓冲区策略、syncscan 以及
 * pagemode 选项。注意，尽管 syncscan 的实际使用可能发生变化（实际上相当于
 * 启用或禁用上报）。
 */
static inline void
table_rescan_set_params(TableScanDesc scan, struct ScanKeyData *key,
						bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	scan->rs_rd->rd_tableam->scan_rescan(scan, key, true,
										 allow_strat, allow_sync,
										 allow_pagemode);
}

/*
 * 从 `scan` 取回下一个元组，存入 slot。
 */
static inline bool
table_scan_getnextslot(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot)
{
	slot->tts_tableOid = RelationGetRelid(sscan->rs_rd);

	/* 我们不期望实际使用 NoMovementScanDirection 进行扫描 */
	Assert(direction == ForwardScanDirection ||
		   direction == BackwardScanDirection);

	/*
	 * 我们不期望对 table_scan_getnextslot 的直接调用传入对系统目录或普通表
	 * 有效的 CheckXidAlive。详见这些变量声明所在的 xact.c 中的详细注释。
	 */
	if (unlikely(TransactionIdIsValid(CheckXidAlive) && !bsysscan))
		elog(ERROR, "unexpected table_scan_getnextslot call during logical decoding");

	return sscan->rs_rd->rd_tableam->scan_getnextslot(sscan, direction, slot);
}

/* ----------------------------------------------------------------------------
 * TID 范围扫描相关函数。
 * ----------------------------------------------------------------------------
 */

/*
 * table_beginscan_tidrange 是设置 TID 范围扫描所用 TableScanDesc 的入口点。
 */
static inline TableScanDesc
table_beginscan_tidrange(Relation rel, Snapshot snapshot,
						 ItemPointer mintid,
						 ItemPointer maxtid)
{
	TableScanDesc sscan;
	uint32		flags = SO_TYPE_TIDRANGESCAN | SO_ALLOW_PAGEMODE;

	sscan = rel->rd_tableam->scan_begin(rel, snapshot, 0, NULL, NULL, flags);

	/* 设置要扫描的 TID 范围 */
	sscan->rs_rd->rd_tableam->scan_set_tidrange(sscan, mintid, maxtid);

	return sscan;
}

/*
 * table_rescan_tidrange 会重置扫描位置，并为由 table_beginscan_tidrange
 * 创建的 TableScanDesc 设置要扫描的最小和最大 TID 范围。
 */
static inline void
table_rescan_tidrange(TableScanDesc sscan, ItemPointer mintid,
					  ItemPointer maxtid)
{
	/* 确保使用了 table_beginscan_tidrange() */
	Assert((sscan->rs_flags & SO_TYPE_TIDRANGESCAN) != 0);

	sscan->rs_rd->rd_tableam->scan_rescan(sscan, NULL, false, false, false, false);
	sscan->rs_rd->rd_tableam->scan_set_tidrange(sscan, mintid, maxtid);
}

/*
 * 为 table_beginscan_tidrange() 创建的 TID 范围扫描，从 `sscan` 取回下一个
 * 元组。将元组存入 `slot` 并返回 true；如果该范围内已没有更多元组，则返回 false。
 */
static inline bool
table_scan_getnextslot_tidrange(TableScanDesc sscan, ScanDirection direction,
								TupleTableSlot *slot)
{
	/* 确保使用了 table_beginscan_tidrange() */
	Assert((sscan->rs_flags & SO_TYPE_TIDRANGESCAN) != 0);

	/* 我们不期望实际使用 NoMovementScanDirection 进行扫描 */
	Assert(direction == ForwardScanDirection ||
		   direction == BackwardScanDirection);

	return sscan->rs_rd->rd_tableam->scan_getnextslot_tidrange(sscan,
															   direction,
															   slot);
}


/* ----------------------------------------------------------------------------
 * 并行表扫描相关函数。
 * ----------------------------------------------------------------------------
 */

/*
 * 估计对本关系进行并行扫描所需的共享内存大小。
 */
extern Size table_parallelscan_estimate(Relation rel, Snapshot snapshot);

/*
 * 为本关系的并行扫描初始化 ParallelTableScanDesc。`pscan` 的大小必须依据同名关系
 * 对应的 parallelscan_estimate() 结果来确定。只需在领导者进程中调用一次；
 * 之后各个工作进程通过 table_beginscan_parallel 挂载。

 */
extern void table_parallelscan_initialize(Relation rel,
										  ParallelTableScanDesc pscan,
										  Snapshot snapshot);

/*
 * 开始一次并行扫描。`pscan` 必须已经由同名关系的
 * table_parallelscan_initialize() 初始化过。该初始化并不需要在本后端中完成。

 * 调用者必须对该关系持有合适的锁。
 */
extern TableScanDesc table_beginscan_parallel(Relation relation,
											  ParallelTableScanDesc pscan);

/*
 * 重启一次并行扫描。在领导者进程中调用本函数。调用者负责确保在调用本函数前，
 * 所有工作进程都已结束本次扫描。
 */
static inline void
table_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
	rel->rd_tableam->parallelscan_reinitialize(rel, pscan);
}


/* ----------------------------------------------------------------------------
 * 索引扫描相关函数。
 * ----------------------------------------------------------------------------
 */

/*
 * 为从关系中取回元组做准备，这在通过索引扫描取回元组时是必需的。
 *
 * 随后即可通过 table_index_fetch_tuple() 取回索引扫描所需的元组。
 */
static inline IndexFetchTableData *
table_index_fetch_begin(Relation rel)
{
	return rel->rd_tableam->index_fetch_begin(rel);
}

/*
 * 重置索引取回。通常这会释放保存在索引取回结构中的跨索引取回资源。
 */
static inline void
table_index_fetch_reset(struct IndexFetchTableData *scan)
{
	scan->rel->rd_tableam->index_fetch_reset(scan);
}

/*
 * 释放资源并解除索引取回分配。
 */
static inline void
table_index_fetch_end(struct IndexFetchTableData *scan)
{
	scan->rel->rd_tableam->index_fetch_end(scan);
}

/*
 * 作为索引扫描的一部分，依据 `snapshot` 完成可见性测试后，将 `tid` 处的元组
 * 取回到 `slot`。如果找到元组且通过了可见性测试，返回 true，否则返回 false。
 * 注意，当我们返回 true 时，*tid 可能会被修改（见后文关于通过单个索引项
 * 可到达的多个行版本的相关说明）。
 *
 * 对某个 tid 第一次调用 table_index_fetch_tuple() 时，*call_again 必须为 false。
 * 如果可能还存在另一个匹配该 tid 的元组，*call_again 会被置为 true，以此通知
 * 调用者应当再次调用 table_index_fetch_tuple() 处理同一个 tid。
 *
 * 如果 all_dead 不为 NULL，当且仅当确定没有任何后端还需要看到该元组时，
 * table_index_fetch_tuple() 才会将 *all_dead 置为 true。索引 AM 可以利用这一点，
 * 在将来的搜索中避免返回该 tid。
 *
 * 本函数与 table_tuple_fetch_row_version() 的区别在于：如果 AM 支持存储通过单个
 * 索引项可到达的多个行版本（如堆的 HOT），本函数会返回该行当前可见的版本。
 * 而在索引项到表元组的查找之外，通常需要的就是 table_tuple_fetch_row_version()，
 * 因为它会在 `tid` 处精确求值元组。
 */
static inline bool
table_index_fetch_tuple(struct IndexFetchTableData *scan,
						ItemPointer tid,
						Snapshot snapshot,
						TupleTableSlot *slot,
						bool *call_again, bool *all_dead)
{
	/*
	 * 我们不期望对 table_index_fetch_tuple 的直接调用传入对系统目录或普通表
	 * 有效的 CheckXidAlive。详见这些变量声明所在的 xact.c 中的详细注释。
	 */
	if (unlikely(TransactionIdIsValid(CheckXidAlive) && !bsysscan))
		elog(ERROR, "unexpected table_index_fetch_tuple call during logical decoding");

	return scan->rel->rd_tableam->index_fetch_tuple(scan, tid, snapshot,
													slot, call_again,
													all_dead);
}

/*
 * 这是 table_index_fetch_tuple() 的便捷封装，用于返回是否存在与某个索引项
 * 对应的表元组项。它很可能仅用于核实唯一索引中是否存在冲突。
 */
extern bool table_index_fetch_tuple_check(Relation rel,
										  ItemPointer tid,
										  Snapshot snapshot,
										  bool *all_dead);


/* ------------------------------------------------------------------------
 * 针对单个元组的非修改型操作函数
 * ------------------------------------------------------------------------
 */


/*
 * 依据 `snapshot` 完成可见性测试后，将 `tid` 处的元组取回到 `slot`。
 * 如果找到元组且通过了可见性测试，返回 true，否则返回 false。

 *
 * 关于这些函数之间的差异，请参见 table_index_fetch_tuple 的注释。在索引项到
 * 表元组的查找之外使用本函数是正确的。

 */
static inline bool
table_tuple_fetch_row_version(Relation rel,
							  ItemPointer tid,
							  Snapshot snapshot,
							  TupleTableSlot *slot)
{
	/*
	 * 我们不期望对 table_tuple_fetch_row_version 的直接调用传入对系统目录或普通表
	 * 有效的 CheckXidAlive。详见这些变量声明所在的 xact.c 中的详细注释。
	 */
	if (unlikely(TransactionIdIsValid(CheckXidAlive) && !bsysscan))
		elog(ERROR, "unexpected table_tuple_fetch_row_version call during logical decoding");

	return rel->rd_tableam->tuple_fetch_row_version(rel, tid, snapshot, slot);
}

/*
 * 校验 `tid` 是否可能是一个有效的元组标识符。这并不要求被指向的行必须存在或
 * 可见，只是要求用该 tid 调用（例如通过 table_tuple_get_latest_tid() 或
 * table_tuple_fetch_row_version()）时不应报错。


 *
 * `scan` needs to have been started via table_beginscan().
 */
static inline bool
table_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	return scan->rs_rd->rd_tableam->tuple_tid_valid(scan, tid);
}

/*
 * 通过将 `tid` 更新为指向最新版本，返回 `tid` 处元组的最新版本。
 */
extern void table_tuple_get_latest_tid(TableScanDesc scan, ItemPointer tid);

/*
 * 若 slot 中的元组满足 `snapshot`，则返回 true。
 *
 * 这要求 slot 中的元组有效，且与本 AM 的类型相符。
 *
 * 某些 AM 可能会作为副作用修改元组底层的数据。若是如此，它们应将相关缓冲区
 * 标记为脏。
 */
static inline bool
table_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
							   Snapshot snapshot)
{
	return rel->rd_tableam->tuple_satisfies_snapshot(rel, slot, snapshot);
}

/*
 * 根据索引元组对应的表 TID，判断哪些索引元组可以安全删除。
 *
 * 判断来自索引 AM 调用者 TM_IndexDeleteOp 状态中的哪些条目指向可被 vacuum 的
 * 表元组。那些被 tableam 判定为可被 vacuum 的条目，对索引 AM 而言自然可以安全
 * 删除，因此会被直接标记为可删除。完整细节参见 TM_IndexDelete 与 TM_IndexDeleteOp
 * 上方的注释。
 *
 * 返回一个 snapshotConflictHorizon 事务 ID，调用者将其放入索引删除的 WAL 记录中。
 * 在 Hot Standby 模式下重放该 WAL 记录时可能会用到它——备机上可能因此需要为
 * 该索引删除操作引发一次恢复冲突。
 */
static inline TransactionId
table_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	return rel->rd_tableam->index_delete_tuples(rel, delstate);
}


/* ----------------------------------------------------------------------------
 * 针对物理元组的修改操作函数。
 * ----------------------------------------------------------------------------
 */

/*
 * 将一个 slot 中的元组插入到表 AM 例程中。
 *
 * options 位掩码允许调用者指定可能改变 AM 行为的选项。AM 会忽略它不支持的选项。
 *
 * 若指定了 TABLE_INSERT_SKIP_FSM 选项，AM 可以不复用关系中的空闲空间。当我们
 * 知道关系是新建的、且不包含多少有用的空闲空间时，这可以节省一些开销。
 * TABLE_INSERT_SKIP_FSM 通常会被直接传给 RelationGetBufferForTuple。详情参见
 * 该方法。
 *
 * TABLE_INSERT_FROZEN 只应指定给插入到当前子事务期间创建的、且没有既有快照或
 * 既有游标打开的关系存储中的插入。这会使行被冻结，属于违反 MVCC 的行为，需要
 * 由用户显式选择。
 *
 * TABLE_INSERT_NO_LOGICAL 强制禁用为该元组输出逻辑解码信息。这仅应在表重写期间
 * 使用，因为此时 RelationIsLogicallyLogged(relation) 对新关系尚不准确。
 *
 * 注意，如果元组需要任何行外数据，这些选项中的大多数在插入堆的 TOAST 表时也会
 * 被应用。
 *
 * BulkInsertState 对象（若有；bistate 为 NULL 时采用默认行为）也只是透传给
 * RelationGetBufferForTuple。如果提供了 `bistate`，则需要调用
 * table_finish_bulk_insert()。
 *
 * 返回时，slot 的 tts_tid 与 tts_tableOid 会被更新以反映本次插入。但请注意，
 * 对 slot 内字段的任何 TOAST 处理不会反映到 slot 的内容中。
 */
static inline void
table_tuple_insert(Relation rel, TupleTableSlot *slot, CommandId cid,
				   int options, struct BulkInsertStateData *bistate)
{
	rel->rd_tableam->tuple_insert(rel, slot, cid, options,
								  bistate);
}

/*
 * 执行"投机插入"。这些插入之后可以被回退，而不必中止整个事务。其他会话可以
 * 等待该投机插入被确认（变成一个普通元组），或被中止（如同它从未存在过）。
 * 被投机插入的元组表现为短时间的"值锁"，用于实现 INSERT .. ON CONFLICT。
 *
 * 执行了投机插入的事务要么必须中止，要么必须通过
 * table_tuple_complete_speculative(succeeded = ...) 来结束该投机插入。
 */
static inline void
table_tuple_insert_speculative(Relation rel, TupleTableSlot *slot,
							   CommandId cid, int options,
							   struct BulkInsertStateData *bistate,
							   uint32 specToken)
{
	rel->rd_tableam->tuple_insert_speculative(rel, slot, cid, options,
											  bistate, specToken);
}

/*
 * 结束在同一事务中启动的"投机插入"。若 succeeded 为 true，则元组被完整插入；
 * 若为 false，则被移除。
 */
static inline void
table_tuple_complete_speculative(Relation rel, TupleTableSlot *slot,
								 uint32 specToken, bool succeeded)
{
	rel->rd_tableam->tuple_complete_speculative(rel, slot, specToken,
												succeeded);
}

/*
 * 向表中插入多个元组。
 *
 * 这与 table_tuple_insert() 类似，但它在一次操作中插入多个元组。这通常比在循环
 * 中反复调用 table_tuple_insert() 更快，例如 AM 可以减少 WAL 日志与页面锁的开销。
 *
 * 除了以 `nslots` 个元组作为输入、并以 TupleTableSlot 数组 `slots` 传入外，
 * table_multi_insert() 的参数与 table_tuple_insert() 相同。
 *
 * 注意：这会向当前内存上下文中泄漏内存。若有问题，可在调用前创建一个临时上下文。
 */
static inline void
table_multi_insert(Relation rel, TupleTableSlot **slots, int nslots,
				   CommandId cid, int options, struct BulkInsertStateData *bistate)
{
	rel->rd_tableam->multi_insert(rel, slots, nslots,
								  cid, options, bistate);
}

/*
 * 删除一个元组。
 *
 * 注意：除非已准备好处理并发更新情况，否则不要直接调用本函数。应使用
 * simple_table_tuple_delete 代替。
 *
 * 输入参数：
 *	relation - 要修改的表（调用者必须持有合适的锁）
 *	tid - 待删除元组的 TID
 *	cid - 删除命令 ID（用于可见性测试，成功时存入 cmax）
 *	crosscheck - 若不为 InvalidSnapshot，则还需针对它检查元组
 *	wait - 若为 true，则等待任何冲突的更新提交/中止
 * 输出参数：
 *	tmfd - 在失败情况下被填充（见下文）
 *	changingPart - 当且仅当元组因分区键被更新而被移动到另一个分区表时为 true，
 *		否则为 false。
 *
 * 正常的成功返回值是 TM_Ok，表示我们确实删除了它。失败返回码为 TM_SelfModified、
 * TM_Updated 和 TM_BeingModified（最后一个仅在 wait == false 时才可能出现）。
 *
 * 在失败情况下，本例程会用元组的 t_ctid、t_xmax 以及（在可能时）t_cmax 填充
 * *tmfd。更多信息参见 TM_FailureData 结构体的注释。
 */
static inline TM_Result
table_tuple_delete(Relation rel, ItemPointer tid, CommandId cid,
				   Snapshot snapshot, Snapshot crosscheck, bool wait,
				   TM_FailureData *tmfd, bool changingPart)
{
	return rel->rd_tableam->tuple_delete(rel, tid, cid,
										 snapshot, crosscheck,
										 wait, tmfd, changingPart);
}

/*
 * 更新一个元组。
 *
 * 注意：除非已准备好处理并发更新情况，否则不要直接调用本函数。应使用
 * simple_table_tuple_update 代替。
 *
 * 输入参数：
 *	relation - 要修改的表（调用者必须持有合适的锁）
 *	otid - 待替换旧元组的 TID
 *	slot - 新构造的待存储元组数据
 *	cid - 更新命令 ID（用于可见性测试，成功时存入 cmax/cmin）
 *	crosscheck - 若不为 InvalidSnapshot，则还需针对它检查旧元组
 *	wait - 若为 true，则等待任何冲突的更新提交/中止
 * 输出参数：
 *	tmfd - 在失败情况下被填充（见下文）
 *	lockmode - 被填充为在元组上获取的锁模式
 *	update_indexes - 在成功情况下，若需要为本元组建立新的索引项则被设置；
 *		参见 TU_UpdateIndexes
 *
 * 正常的成功返回值是 TM_Ok，表示我们确实更新了它。失败返回码为 TM_SelfModified、
 * TM_Updated 和 TM_BeingModified（最后一个仅在 wait == false 时才可能出现）。
 *
 * 成功时，slot 的 tts_tid 与 tts_tableOid 会被更新以匹配新存储的元组；尤其是
 * slot->tts_tid 被设为新元组插入处的 TID，且仅当执行了 HOT 更新时，其
 * HEAP_ONLY_TUPLE 标志才会被设置。但是，新元组数据中的任何 TOAST 变更都不会
 * 反映到 *newtup 中。
 *
 * 在失败情况下，本例程会用元组的 t_ctid、t_xmax 以及（在可能时）t_cmax 填充
 * *tmfd。更多信息参见 TM_FailureData 结构体的注释。
 */
static inline TM_Result
table_tuple_update(Relation rel, ItemPointer otid, TupleTableSlot *slot,
				   CommandId cid, Snapshot snapshot, Snapshot crosscheck,
				   bool wait, TM_FailureData *tmfd, LockTupleMode *lockmode,
				   TU_UpdateIndexes *update_indexes)
{
	return rel->rd_tableam->tuple_update(rel, otid, slot,
										 cid, snapshot, crosscheck,
										 wait, tmfd,
										 lockmode, update_indexes);
}

/*
 * 以指定模式锁定一个元组。
 *
 * 输入参数：
 *	relation: 包含元组的表（调用者必须持有合适的锁）
 *	tid: 待锁定元组的 TID（若跟随了更新链则会被更新）
 *	snapshot: 用于可见性判断的快照
 *	cid: 当前命令 ID（用于可见性测试，锁定成功时存入元组的 cmax）
 *	mode: 期望的锁模式
 *	wait_policy: 当无法立即获取元组锁时的处理策略
 *	flags:
 *		若指定 TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS，则跟随更新链，
 *		在锁模式不冲突的情况下也锁定后代元组。
 *		若指定 TUPLE_LOCK_FLAG_FIND_LAST_VERSION，则跟随更新链并锁定最新版本。
 *
 * 输出参数：
 *	*slot: 包含目标元组
 *	*tmfd: 在失败情况下被填充（见下文）
 *
 * 函数返回值可能是：
 *	TM_Ok: 成功获取锁
 *	TM_Invisible: 锁获取失败，因为元组对我们从来不可见
 *	TM_SelfModified: 锁获取失败，因为元组被自身更新
 *	TM_Updated: 锁获取失败，因为元组被其他事务更新
 *	TM_Deleted: 锁获取失败，因为元组被其他事务删除
 *	TM_WouldBlock: 无法获取锁，且 wait_policy 为 skip
 *
 * 在除 TM_Invisible 与 TM_Deleted 之外的失败情况下，本例程会用元组的 t_ctid、
 * t_xmax 以及（在可能时）t_cmax 填充 *tmfd。此外，无论是成功还是失败，只要
 * 跟随了更新链，tmfd->traversed 都会被设置。更多信息参见 TM_FailureData
 * 结构体的注释。
 */
static inline TM_Result
table_tuple_lock(Relation rel, ItemPointer tid, Snapshot snapshot,
				 TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
				 LockWaitPolicy wait_policy, uint8 flags,
				 TM_FailureData *tmfd)
{
	return rel->rd_tableam->tuple_lock(rel, tid, snapshot, slot,
									   cid, mode, wait_policy,
									   flags, tmfd);
}

/*
 * 执行完成经由 tuple_insert 与 multi_insert（并指定了 BulkInsertState）
 * 所做插入所必需的操作。
 */
static inline void
table_finish_bulk_insert(Relation rel, int options)
{
	/* 可选回调 */
	if (rel->rd_tableam && rel->rd_tableam->finish_bulk_insert)
		rel->rd_tableam->finish_bulk_insert(rel, options);
}


/* ------------------------------------------------------------------------
 * 与 DDL 相关的功能。
 * ------------------------------------------------------------------------
 */

/*
 * 为 `rel` 在 `newrlocator` 处创建存储，持久化行为由 `persistence` 指定。
 *
 * 本函数既在关系创建期间使用，也在各种 DDL 操作中使用，以创建可从头填充的新
 * 关系存储。当为既有 relfilelocator 创建新存储时，应在 relcache 项被更新之前
 * 调用本函数。
 *
 * *freezeXid 与 *minmulti 会被设为 pg_class.{relfrozenxid, relminmxid} 必须
 * 被设为的事务 ID / 多事务地平线。
 */
static inline void
table_relation_set_new_filelocator(Relation rel,
								   const RelFileLocator *newrlocator,
								   char persistence,
								   TransactionId *freezeXid,
								   MultiXactId *minmulti)
{
	rel->rd_tableam->relation_set_new_filelocator(rel, newrlocator,
												  persistence, freezeXid,
												  minmulti);
}

/*
 * 以非事务性的方式，从 `rel` 中移除所有表内容。非事务性意味着无需支持回滚。
 * 这通常只用于对在当前事务中创建的关系存储执行截断。
 */
static inline void
table_relation_nontransactional_truncate(Relation rel)
{
	rel->rd_tableam->relation_nontransactional_truncate(rel);
}

/*
 * 将 `rel` 中的数据复制到新的 relfilelocator `newrlocator`。在本函数被调用前，
 * 新的 relfilelocator 可能尚未关联任何存储。本函数只应被用于更改关系表空间等
 * 底层操作。
 */
static inline void
table_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
	rel->rd_tableam->relation_copy_data(rel, newrlocator);
}

/*
 * 作为 CLUSTER 或 VACUUM FULL 的一部分，将 `OldTable` 中的数据复制到 `NewTable`。
 *
 * 附加输入参数：
 * - use_sort - 若为 true，则表内容按 `OldIndex` 适当排序；若为 false 且 OldIndex
 *   不是 InvalidOid，则按该索引的顺序复制数据；若为 false 且 OldIndex 为
 *   InvalidOid，则不执行排序
 * - OldIndex - 参见 use_sort
 * - OldestXmin - 由 vacuum_get_cutoffs() 计算，即便关系的 AM 并不需要它
 * - *xid_cutoff - 同上
 * - *multi_cutoff - 同上
 *
 * 输出参数：
 * - *xid_cutoff - 关系新的 relfrozenxid 值，可能为无效值
 * - *multi_cutoff - 关系新的 relminmxid 值，可能为无效值
 * - *tups_vacuumed - 用于日志记录的统计信息（若 AM 适用）
 * - *tups_recently_dead - 用于日志记录的统计信息（若 AM 适用）
 */
static inline void
table_relation_copy_for_cluster(Relation OldTable, Relation NewTable,
								Relation OldIndex,
								bool use_sort,
								TransactionId OldestXmin,
								TransactionId *xid_cutoff,
								MultiXactId *multi_cutoff,
								double *num_tuples,
								double *tups_vacuumed,
								double *tups_recently_dead)
{
	OldTable->rd_tableam->relation_copy_for_cluster(OldTable, NewTable, OldIndex,
													use_sort, OldestXmin,
													xid_cutoff, multi_cutoff,
													num_tuples, tups_vacuumed,
													tups_recently_dead);
}

/*
 * 对该关系执行 VACUUM。VACUUM 可能由用户触发，也可能由 autovacuum 触发。AM
 * 具体执行哪些操作，很大程度上取决于各个 AM 自身。
 *
 * 进入本函数时，事务已经建立，并且该表已被施加 ShareUpdateExclusive 锁。
 *
 * 注意，无论是 VACUUM FULL（及 CLUSTER），还是 ANALYZE，都不会经过本例程，
 * 即便（对 ANALYZE 而言）它是同一个 VACUUM 命令的一部分。
 */
static inline void
table_relation_vacuum(Relation rel, struct VacuumParams *params,
					  BufferAccessStrategy bstrategy)
{
	rel->rd_tableam->relation_vacuum(rel, params, bstrategy);
}

/*
 * 为分析读取流中的下一个块做准备。该扫描必须已经通过 table_beginscan_analyze()
 * 启动。注意，本例程可能会获取诸如锁之类的资源，这些资源会一直持有到
 * table_scan_analyze_next_tuple() 返回 false 为止。
 *
 * 如果该块不适合采样，则返回 false，否则返回 true。
 */
static inline bool
table_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream)
{
	return scan->rs_rd->rd_tableam->scan_analyze_next_block(scan, stream);
}

/*
 * 遍历由 table_scan_analyze_next_block()（必须已返回 true，且本例程此前对该块
 * 尚未返回过 false）所选中块内的元组。如果找到适合采样的元组，返回 true 并将
 * 该元组存入 `slot`。
 *
 * *liverows 与 *deadrows 会根据遇到的元组相应递增。
 */
static inline bool
table_scan_analyze_next_tuple(TableScanDesc scan, TransactionId OldestXmin,
							  double *liverows, double *deadrows,
							  TupleTableSlot *slot)
{
	return scan->rs_rd->rd_tableam->scan_analyze_next_tuple(scan, OldestXmin,
															liverows, deadrows,
															slot);
}

/*
 * table_index_build_scan - 扫描表以找出需要被索引的元组
 *
 * 本函数在访问方法特定的索引构建过程完成自身所需的准备工作之后被回调。父表
 * 关系会被扫描以找出应当进入索引的元组。每个这样的元组都会被传给该 AM 的回调
 * 例程，由它做正确的事情将该元组加入新索引。本函数返回后，AM 的索引构建过程
 * 会执行它所需的任何清理工作。
 *
 * 返回活元组的总数。这用于更新 pg_class 的统计信息。（无法在此处完成更新有些
 * 恼人，但我们希望将该更新与其他更新合并；参见 index_update_stats。）注意，
 * 索引 AM 自身必须记录索引元组的数量；我们在此不记录，因为 AM 可能出于自身
 * 原因（例如无法存储 NULL）拒绝其中某些元组。
 *
 * 若 progress 为真，则在开始扫描时更新 PROGRESS_SCAN_BLOCKS_TOTAL 计数器，并
 * 在扫描过程中持续更新 PROGRESS_SCAN_BLOCKS_DONE。
 *
 * 一个副作用是：如果我们检测到任何可能损坏的 HOT 链，会将 indexInfo->
 * ii_BrokenHotChain 设为 true。目前，只要 HOT 链中存在任何 RECENTLY_DEAD 或
 * DELETE_IN_PROGRESS 项，我们就会设置它，而不会费力去检测它们是否真的与链尾
 * 不兼容。这真正有意义的场景只是堆 AM，未来或许需要为其他 AM 做一般化处理。
 */
static inline double
table_index_build_scan(Relation table_rel,
					   Relation index_rel,
					   struct IndexInfo *index_info,
					   bool allow_sync,
					   bool progress,
					   IndexBuildCallback callback,
					   void *callback_state,
					   TableScanDesc scan)
{
	return table_rel->rd_tableam->index_build_range_scan(table_rel,
														 index_rel,
														 index_info,
														 allow_sync,
														 false,
														 progress,
														 0,
														 InvalidBlockNumber,
														 callback,
														 callback_state,
														 scan);
}

/*
 * 与 table_index_build_scan() 类似，但只扫描给定数量的块，而非整张表。通过将
 * numblocks 传为 InvalidBlockNumber 可以表示扫描到关系末尾。注意，在请求
 * syncscan 时无法限制扫描范围。
 *
 * 当请求 "anyvisible" 模式时，所有对任何事务可见的元组都会被索引并计为活元组，
 * 包括那些由仍在进行中的事务插入或删除的元组。
 */
static inline double
table_index_build_range_scan(Relation table_rel,
							 Relation index_rel,
							 struct IndexInfo *index_info,
							 bool allow_sync,
							 bool anyvisible,
							 bool progress,
							 BlockNumber start_blockno,
							 BlockNumber numblocks,
							 IndexBuildCallback callback,
							 void *callback_state,
							 TableScanDesc scan)
{
	return table_rel->rd_tableam->index_build_range_scan(table_rel,
														 index_rel,
														 index_info,
														 allow_sync,
														 anyvisible,
														 progress,
														 start_blockno,
														 numblocks,
														 callback,
														 callback_state,
														 scan);
}

/*
 * table_index_validate_scan - 并发索引构建所需的第二次表扫描
 *
 * 解释参见 validate_index()。
 */
static inline void
table_index_validate_scan(Relation table_rel,
						  Relation index_rel,
						  struct IndexInfo *index_info,
						  Snapshot snapshot,
						  struct ValidateIndexState *state)
{
	table_rel->rd_tableam->index_validate_scan(table_rel,
											   index_rel,
											   index_info,
											   snapshot,
											   state);
}


/* ----------------------------------------------------------------------------
 * 其他功能
 * ----------------------------------------------------------------------------
 */

/*
 * 返回 `rel` 当前的大小（以字节为单位）。若 `forkNumber` 为 InvalidForkNumber，
 * 则返回关系的整体大小，否则返回所指分叉的大小。
 *
 * 注意，对某些 AM 而言，整体大小可能并不等于各个分叉大小之和，例如因为 AM 的
 * 存储在结构上并不能整齐地映射到内建类型的分叉。
 */
static inline uint64
table_relation_size(Relation rel, ForkNumber forkNumber)
{
	return rel->rd_tableam->relation_size(rel, forkNumber);
}

/*
 * table_relation_needs_toast_table - 该关系是否需要一个 TOAST 表？
 */
static inline bool
table_relation_needs_toast_table(Relation rel)
{
	return rel->rd_tableam->relation_needs_toast_table(rel);
}

/*
 * 返回应当用于为本关系实现 TOAST 表的 AM 的 OID。
 */
static inline Oid
table_relation_toast_am(Relation rel)
{
	return rel->rd_tableam->relation_toast_am(rel);
}

/*
 * 从 TOAST 表中取回 TOAST 值的全部或部分内容。
 *
 * 如果本 AM 从不用于实现 TOAST 表，则不需要此回调。但是，如果有被 TOAST 的值
 * 存储在本类型的表中，则你将需要此回调。
 *
 * toastrel 是存储被 TOAST 值的那个关系。
 *
 * valueid 标识要取回的是哪个 TOAST 值。对于堆而言，它对应于 chunk_id 列中
 * 存储的值。
 *
 * attrsize 是要取回的 TOAST 值的总大小。
 *
 * sliceoffset 是要取回的首字节在 TOAST 值中的偏移。
 *
 * slicelength 是应从 TOAST 值中取回的字节数。
 *
 * result 是由调用者分配的空间，取回的字节应存入其中。
 */
static inline void
table_relation_fetch_toast_slice(Relation toastrel, Oid valueid,
								 int32 attrsize, int32 sliceoffset,
								 int32 slicelength, struct varlena *result)
{
	toastrel->rd_tableam->relation_fetch_toast_slice(toastrel, valueid,
													 attrsize,
													 sliceoffset, slicelength,
													 result);
}


/* ----------------------------------------------------------------------------
 * 规划器相关功能
 * ----------------------------------------------------------------------------
 */

/*
 * 估算关系的当前大小，作为 estimate_rel_size() 的 AM 特定底层实现。参数的
 * 含义参见该函数。
 */
static inline void
table_relation_estimate_size(Relation rel, int32 *attr_widths,
							 BlockNumber *pages, double *tuples,
							 double *allvisfrac)
{
	rel->rd_tableam->relation_estimate_size(rel, attr_widths, pages, tuples,
											allvisfrac);
}


/* ----------------------------------------------------------------------------
 * 执行器相关功能
 * ----------------------------------------------------------------------------
 */

/*
 * 作为位图表扫描的一部分取回 / 检查 / 返回元组。`scan` 必须已经通过
 * table_beginscan_bm() 启动。将位图表扫描的下一个元组取入 `slot`，若找到可见
 * 元组则返回 true，否则返回 false。
 *
 * `recheck` 由表 AM 设置，用于指示 `slot` 中的元组是否需要被重新检查。来自有损
 * 页面的元组总是需要重新检查，但某些非有损页面的元组也可能需要重新检查。
 *
 * 若块在 bitmap 中的表示是有损的，则递增 `lossy_pages`；否则递增 `exact_pages`。
 */
static inline bool
table_scan_bitmap_next_tuple(TableScanDesc scan,
							 TupleTableSlot *slot,
							 bool *recheck,
							 uint64 *lossy_pages,
							 uint64 *exact_pages)
{
	/*
	 * 我们不期望对 table_scan_bitmap_next_tuple 的直接调用传入对系统目录或普通表
	 * 有效的 CheckXidAlive。详见这些变量声明所在的 xact.c 中的详细注释。
	 */
	if (unlikely(TransactionIdIsValid(CheckXidAlive) && !bsysscan))
		elog(ERROR, "unexpected table_scan_bitmap_next_tuple call during logical decoding");

	return scan->rs_rd->rd_tableam->scan_bitmap_next_tuple(scan,
														   slot,
														   recheck,
														   lossy_pages,
														   exact_pages);
}

/*
 * 为从采样扫描的下一个块中取回元组做准备。如果采样扫描已结束则返回 false，
 * 否则返回 true。`scan` 必须已经通过 table_beginscan_sampling() 启动。
 *
 * 必要时会调用 TsmRoutine 的 NextSampleBlock() 回调（即 NextSampleBlock 不为
 * NULL），否则会对底层关系执行顺序扫描。
 */
static inline bool
table_scan_sample_next_block(TableScanDesc scan,
							 struct SampleScanState *scanstate)
{
	/*
	 * 我们不期望对 table_scan_sample_next_block 的直接调用传入对系统目录或普通表
	 * 有效的 CheckXidAlive。详见这些变量声明所在的 xact.c 中的详细注释。
	 */
	if (unlikely(TransactionIdIsValid(CheckXidAlive) && !bsysscan))
		elog(ERROR, "unexpected table_scan_sample_next_block call during logical decoding");
	return scan->rs_rd->rd_tableam->scan_sample_next_block(scan, scanstate);
}

/*
 * 将下一个采样元组取入 `slot`，若找到可见元组则返回 true，否则返回 false。
 * table_scan_sample_next_block() 必须事先已选中一个块（即返回了 true），且
 * 对同一块此前的 table_scan_sample_next_tuple() 不能返回过 false。
 *
 * 本函数会调用 TsmRoutine 的 NextSampleTuple() 回调。
 */
static inline bool
table_scan_sample_next_tuple(TableScanDesc scan,
							 struct SampleScanState *scanstate,
							 TupleTableSlot *slot)
{
	/*
	 * 我们不期望对 table_scan_sample_next_tuple 的直接调用传入对系统目录或普通表
	 * 有效的 CheckXidAlive。详见这些变量声明所在的 xact.c 中的详细注释。
	 */
	if (unlikely(TransactionIdIsValid(CheckXidAlive) && !bsysscan))
		elog(ERROR, "unexpected table_scan_sample_next_tuple call during logical decoding");
	return scan->rs_rd->rd_tableam->scan_sample_next_tuple(scan, scanstate,
														   slot);
}


/* ----------------------------------------------------------------------------
 * 简化修改操作的辅助函数。
 * ----------------------------------------------------------------------------
 */

extern void simple_table_tuple_insert(Relation rel, TupleTableSlot *slot);
extern void simple_table_tuple_delete(Relation rel, ItemPointer tid,
									  Snapshot snapshot);
extern void simple_table_tuple_update(Relation rel, ItemPointer otid,
									  TupleTableSlot *slot, Snapshot snapshot,
									  TU_UpdateIndexes *update_indexes);


/* ----------------------------------------------------------------------------
 * 为面向块访问的 AM 实现并行扫描的辅助函数。
 * ----------------------------------------------------------------------------
 */

extern Size table_block_parallelscan_estimate(Relation rel);
extern Size table_block_parallelscan_initialize(Relation rel,
												ParallelTableScanDesc pscan);
extern void table_block_parallelscan_reinitialize(Relation rel,
												  ParallelTableScanDesc pscan);
extern BlockNumber table_block_parallelscan_nextpage(Relation rel,
													 ParallelBlockTableScanWorker pbscanwork,
													 ParallelBlockTableScanDesc pbscan);
extern void table_block_parallelscan_startblock_init(Relation rel,
													 ParallelBlockTableScanWorker pbscanwork,
													 ParallelBlockTableScanDesc pbscan);


/* ----------------------------------------------------------------------------
 * 为面向块访问的 AM 实现关系大小估算的辅助函数。
 * ----------------------------------------------------------------------------
 */

extern uint64 table_block_relation_size(Relation rel, ForkNumber forkNumber);
extern void table_block_relation_estimate_size(Relation rel,
											   int32 *attr_widths,
											   BlockNumber *pages,
											   double *tuples,
											   double *allvisfrac,
											   Size overhead_bytes_per_tuple,
											   Size usable_bytes_per_page);

/* ----------------------------------------------------------------------------
 * tableamapi.c 中的函数
 * ----------------------------------------------------------------------------
 */

extern const TableAmRoutine *GetTableAmRoutine(Oid amhandler);

/* ----------------------------------------------------------------------------
 * heapam_handler.c 中的函数
 * ----------------------------------------------------------------------------
 */

extern const TableAmRoutine *GetHeapamTableAmRoutine(void);

#endif							/* TABLEAM_H */
