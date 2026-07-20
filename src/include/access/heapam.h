/*-------------------------------------------------------------------------
 *
 * heapam.h
 *		POSTGRES 堆访问方法定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/heapam.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HEAPAM_H
#define HEAPAM_H

#include "access/heapam_xlog.h"
#include "access/relation.h"	/* 为向后兼容 */
#include "access/relscan.h"
#include "access/sdir.h"
#include "access/skey.h"
#include "access/table.h"		/* 为向后兼容 */
#include "access/tableam.h"
#include "nodes/lockoptions.h"
#include "nodes/primnodes.h"
#include "storage/bufpage.h"
#include "storage/dsm.h"
#include "storage/lockdefs.h"
#include "storage/read_stream.h"
#include "storage/shm_toc.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"


/* heap_insert 的 "options" 标志位 */
#define HEAP_INSERT_SKIP_FSM	TABLE_INSERT_SKIP_FSM
#define HEAP_INSERT_FROZEN		TABLE_INSERT_FROZEN
#define HEAP_INSERT_NO_LOGICAL	TABLE_INSERT_NO_LOGICAL
#define HEAP_INSERT_SPECULATIVE 0x0010

/* heap_page_prune_and_freeze 的 "options" 标志位 */
#define HEAP_PAGE_PRUNE_MARK_UNUSED_NOW		(1 << 0)
#define HEAP_PAGE_PRUNE_FREEZE				(1 << 1)

typedef struct BulkInsertStateData *BulkInsertState;
struct TupleTableSlot;
struct VacuumCutoffs;

#define MaxLockTupleMode	LockTupleExclusive

/*
 * 堆表扫描的描述符。
 */
typedef struct HeapScanDescData
{
	TableScanDescData rs_base;	/* 描述符中与访问方法无关的部分 */

	/* 在 initscan 时设置的状态 */
	BlockNumber rs_nblocks;		/* 关系中的总块数 */
	BlockNumber rs_startblock;	/* 起始块编号 */
	BlockNumber rs_numblocks;	/* 要扫描的最大块数 */
	/* rs_numblocks 通常为 InvalidBlockNumber，表示 "扫描整个关系" */

	/* 扫描的当前状态 */
	bool		rs_inited;		/* false = 扫描尚未初始化 */
	OffsetNumber rs_coffset;	/* 非逐页模式下的当前偏移量 */
	BlockNumber rs_cblock;		/* 扫描中的当前块编号（若有） */
	Buffer		rs_cbuf;		/* 扫描中的当前缓冲区（若有） */
	/* 注意：若 rs_cbuf 不是 InvalidBuffer，则我们持有该缓冲区的 pin */

	BufferAccessStrategy rs_strategy;	/* 用于读取的访问策略 */

	HeapTupleData rs_ctup;		/* 扫描中的当前元组（若有） */

	/* 用于以流方式读取的扫描 */
	ReadStream *rs_read_stream;

	/*
	 * 用于顺序扫描和 TID 范围扫描以流式读取。读取
	 * 流在扫描开始时分配，并在重新扫描或
	 * 扫描方向改变时重置。每次请求新页面时
	 * 都会保存扫描方向。如果扫描方向从一页
	 * 到下一页发生改变，读取流会释放之前所有
	 * 已 pin 的缓冲区并重置预取块。
	 */
	ScanDirection rs_dir;
	BlockNumber rs_prefetch_block;

	/*
	 * 用于并行扫描存储页分配数据。非
	 * 并行扫描时为 NULL。
	 */
	ParallelBlockTableScanWorkerData *rs_parallelworkerdata;

	/* 这些字段仅用于逐页模式和位图扫描 */
	uint32		rs_cindex;		/* 当前元组在 vistuples 中的索引 */
	uint32		rs_ntuples;		/* 页面上可见元组的数量 */
	OffsetNumber rs_vistuples[MaxHeapTuplesPerPage];	/* 它们的偏移量 */
} HeapScanDescData;
typedef struct HeapScanDescData *HeapScanDesc;

typedef struct BitmapHeapScanDescData
{
	HeapScanDescData rs_heap_base;

	/* 不保存任何数据 */
}			BitmapHeapScanDescData;
typedef struct BitmapHeapScanDescData *BitmapHeapScanDesc;

/*
 * 通过索引从堆中获取数据的描述符。
 */
typedef struct IndexFetchHeapData
{
	IndexFetchTableData xs_base;	/* 描述符中与访问方法无关的部分 */

	Buffer		xs_cbuf;		/* 扫描中的当前堆缓冲区（若有） */
	/* 注意：若 xs_cbuf 不是 InvalidBuffer，则我们持有该缓冲区的 pin */
} IndexFetchHeapData;

/* HeapTupleSatisfiesVacuum 的结果码 */
typedef enum
{
	HEAPTUPLE_DEAD,				/* 元组已死亡且可删除 */
	HEAPTUPLE_LIVE,				/* 元组有效（已提交，无删除者） */
	HEAPTUPLE_RECENTLY_DEAD,	/* 元组已死亡，但暂不可删除 */
	HEAPTUPLE_INSERT_IN_PROGRESS,	/* 插入事务仍在进行中 */
	HEAPTUPLE_DELETE_IN_PROGRESS,	/* 删除事务仍在进行中 */
} HTSV_Result;

/*
 * heap_prepare_freeze_tuple 可能要求 heap_freeze_execute_prepared
 * 使用 pg_xact 检查任一元组待冻结的 xmin 和/或 xmax 状态
 */
#define		HEAP_FREEZE_CHECK_XMIN_COMMITTED	0x01
#define		HEAP_FREEZE_CHECK_XMAX_ABORTED		0x02

/* 描述如何冻结元组的 heap_prepare_freeze_tuple 状态 */
typedef struct HeapTupleFreeze
{
	/* 描述如何处理元组的字段 */
	TransactionId xmax;
	uint16		t_infomask2;
	uint16		t_infomask;
	uint8		frzflags;

	/* xmin/xmax 检查标志 */
	uint8		checkflags;
	/* 元组的页内偏移量 */
	OffsetNumber offset;
} HeapTupleFreeze;

/*
 * VACUUM 用来跟踪在给定堆页面上冻结所有
 * 符合条件元组细节的状态。
 *
 * VACUUM 通过调用 heap_prepare_freeze_tuple
 * （每个带存储的元组都会单独调用）为每个页面准备冻结计划。此页级冻结
 * 状态在每次调用时更新，最终决定
 * 是否需要冻结该页面。
 *
 * 除了是否继续冻结这个基本问题外，该
 * 状态还跟踪整个表中现存最旧的 XID/MXID，以便
 * 后续推进 pg_class 中的 relfrozenxid/relminmxid 值。
 * 每次 heap_prepare_freeze_tuple 调用都会按需回推 NewRelfrozenXid 和/或
 * NewRelminMxid，以避免最终得到不安全的 pg_class 值。
 * VACUUM 结束后残留的任何未冻结 XID 或 MXID _必须_
 * 其值 >= pg_class 中最终的 relfrozenxid/relminmxid 值。这
 * 包括作为任意元组 xmax 的 MultiXact 成员而残留的 XID。
 *
 * 当所有元组检查完毕后未设置 'freeze_required' 标志时，
 * 最终是否冻结由 vacuumlazy.c 决定。它可以根据
 * 其认为合适的标准触发冻结。然而，建议
 * vacuumlazy.c 在冻结无法使目标页面随后在可见性映射中
 * 被标记为全冻结时，避免过早冻结。
 */
typedef struct HeapPageFreeze
{
	/* heap_prepare_freeze_tuple 调用者是否需要冻结该页面？ */
	bool		freeze_required;

	/*
	 * "Freeze" 版本的 NewRelfrozenXid/NewRelminMxid 跟踪器。
	 *
	 * 当 heap_freeze_execute_prepared 冻结时，或某页
	 * 没有任何冻结计划时使用的跟踪器。按定义，vacuumlazy.c
	 * 冻结任何页面都是合法的。这甚至包括
	 * 一开始就没有带存储元组的页面。这样，
	 * 来自 heap_prepare_freeze_tuple 的 'totally_frozen' 结果
	 * 在任何情况下都可用相同方式处理，即便无需执行任何
	 * 冻结计划来 "冻结页面"。只有 "freeze" 路径需要考虑
	 * 在此方案下将页面标记为全冻结的需求。
	 *
	 * 冻结页面时，我们通常冻结所有 XIDs < OldestXmin，仅
	 * 留下（若有）不符合冻结条件的 XID。因此
	 * 你可能会疑惑为何需要这些跟踪器；为何 VACUUM
	 * 冻结的 _任何_ 页面 _都会_ 残留
	 * 使顶层 NewRelfrozenXid/NewRelminMxid 跟踪器回退的 XID/MXID？
	 *
	 * 使用不过度规定 MultiXact 受影响方式的
	 * "冻结页面" 定义是有用的。heap_prepare_freeze_tuple
	 * 通常倾向于立即移除 Multis，但在
	 * 惰性处理可让 VACUUM 避免分配新 Multi 的情况下会使用惰性方式。
	 * "freeze the page" 跟踪器正是为了实现这种灵活性。
	 */
	TransactionId FreezePageRelfrozenXid;
	MultiXactId FreezePageRelminMxid;

	/*
	 * "No freeze" 版本的 NewRelfrozenXid/NewRelminMxid 跟踪器。
	 *
	 * 这些跟踪器的维护方式与 VACUUM 扫描
	 * 未加 cleanup 锁的页面时使用的跟踪器相同。两条代码路径
	 * 基于同一总体思路（在本次 VACUUM 期间为该页面
	 * 做更少的工作，代价是不得不接受较旧的最终值）。
	 */
	TransactionId NoFreezePageRelfrozenXid;
	MultiXactId NoFreezePageRelminMxid;

} HeapPageFreeze;

/*
 * heap_page_prune_and_freeze() 返回的每页状态
 */
typedef struct PruneFreezeResult
{
	int			ndeleted;		/* 从页面删除的元组数 */
	int			nnewlpdead;		/* 新出现的 LP_DEAD 项数量 */
	int			nfrozen;		/* 冻结的元组数 */

	/* 剪枝后页面上有效及最近死亡的元组数 */
	int			live_tuples;
	int			recently_dead_tuples;

	/*
	 * all_visible 和 all_frozen 表示剪枝后
	 * 可见性映射中的 all-visible 和 all-frozen 位是否可设置。
	 *
	 * vm_conflict_horizon 是页面上有效元组最新的 xmin。调用者
	 * 可在设置 VM 位时将其作为冲突边界。它
	 * 仅在冻结了部分元组（nfrozen > 0）且 all_frozen 为
	 * true 时有效。
	 *
	 * 这些字段仅在设置了 HEAP_PRUNE_FREEZE 选项时才会被设置。
	 */
	bool		all_visible;
	bool		all_frozen;
	TransactionId vm_conflict_horizon;

	/*
	 * 页面是否会使关系截断不安全。即便页面
	 * 包含 LP_DEAD 项，此字段也会设为 'true'。VACUUM 会在尝试
	 * 截断前将其移除。
	 */
	bool		hastup;

	/*
	 * 剪枝后页面上的 LP_DEAD 项。包括已有的 LP_DEAD
	 * 项。
	 */
	int			lpdead_items;
	OffsetNumber deadoffsets[MaxHeapTuplesPerPage];
} PruneFreezeResult;

/* heap_page_prune_and_freeze() 的 'reason' 码 */
typedef enum
{
	PRUNE_ON_ACCESS,			/* 访问时剪枝 */
	PRUNE_VACUUM_SCAN,			/* VACUUM 第一次堆扫描 */
	PRUNE_VACUUM_CLEANUP,		/* VACUUM 第二次堆扫描 */
} PruneReason;

/* ----------------
 *		堆访问方法的函数原型
 *
 * heap_create、heap_create_with_catalog 和 heap_drop_with_catalog
 * 声明在 catalog/heap.h 中
 * ----------------
 */


/*
 * HeapScanIsValid
 *		当且仅当堆扫描有效时为 true。
 */
#define HeapScanIsValid(scan) PointerIsValid(scan)

extern TableScanDesc heap_beginscan(Relation relation, Snapshot snapshot,
									int nkeys, ScanKey key,
									ParallelTableScanDesc parallel_scan,
									uint32 flags);
extern void heap_setscanlimits(TableScanDesc sscan, BlockNumber startBlk,
							   BlockNumber numBlks);
extern void heap_prepare_pagescan(TableScanDesc sscan);
extern void heap_rescan(TableScanDesc sscan, ScanKey key, bool set_params,
						bool allow_strat, bool allow_sync, bool allow_pagemode);
extern void heap_endscan(TableScanDesc sscan);
extern HeapTuple heap_getnext(TableScanDesc sscan, ScanDirection direction);
extern bool heap_getnextslot(TableScanDesc sscan,
							 ScanDirection direction, struct TupleTableSlot *slot);
extern void heap_set_tidrange(TableScanDesc sscan, ItemPointer mintid,
							  ItemPointer maxtid);
extern bool heap_getnextslot_tidrange(TableScanDesc sscan,
									  ScanDirection direction,
									  TupleTableSlot *slot);
extern bool heap_fetch(Relation relation, Snapshot snapshot,
					   HeapTuple tuple, Buffer *userbuf, bool keep_buf);
extern bool heap_hot_search_buffer(ItemPointer tid, Relation relation,
								   Buffer buffer, Snapshot snapshot, HeapTuple heapTuple,
								   bool *all_dead, bool first_call);

extern void heap_get_latest_tid(TableScanDesc sscan, ItemPointer tid);

extern BulkInsertState GetBulkInsertState(void);
extern void FreeBulkInsertState(BulkInsertState);
extern void ReleaseBulkInsertStatePin(BulkInsertState bistate);

extern void heap_insert(Relation relation, HeapTuple tup, CommandId cid,
						int options, BulkInsertState bistate);
extern void heap_multi_insert(Relation relation, struct TupleTableSlot **slots,
							  int ntuples, CommandId cid, int options,
							  BulkInsertState bistate);
extern TM_Result heap_delete(Relation relation, ItemPointer tid,
							 CommandId cid, Snapshot crosscheck, bool wait,
							 struct TM_FailureData *tmfd, bool changingPart);
extern void heap_finish_speculative(Relation relation, ItemPointer tid);
extern void heap_abort_speculative(Relation relation, ItemPointer tid);
extern TM_Result heap_update(Relation relation, ItemPointer otid,
							 HeapTuple newtup,
							 CommandId cid, Snapshot crosscheck, bool wait,
							 struct TM_FailureData *tmfd, LockTupleMode *lockmode,
							 TU_UpdateIndexes *update_indexes);
extern TM_Result heap_lock_tuple(Relation relation, HeapTuple tuple,
								 CommandId cid, LockTupleMode mode, LockWaitPolicy wait_policy,
								 bool follow_updates,
								 Buffer *buffer, struct TM_FailureData *tmfd);

extern bool heap_inplace_lock(Relation relation,
							  HeapTuple oldtup_ptr, Buffer buffer,
							  void (*release_callback) (void *), void *arg);
extern void heap_inplace_update_and_unlock(Relation relation,
										   HeapTuple oldtup, HeapTuple tuple,
										   Buffer buffer);
extern void heap_inplace_unlock(Relation relation,
								HeapTuple oldtup, Buffer buffer);
extern bool heap_prepare_freeze_tuple(HeapTupleHeader tuple,
									  const struct VacuumCutoffs *cutoffs,
									  HeapPageFreeze *pagefrz,
									  HeapTupleFreeze *frz, bool *totally_frozen);

extern void heap_pre_freeze_checks(Buffer buffer,
								   HeapTupleFreeze *tuples, int ntuples);
extern void heap_freeze_prepared_tuples(Buffer buffer,
										HeapTupleFreeze *tuples, int ntuples);
extern bool heap_freeze_tuple(HeapTupleHeader tuple,
							  TransactionId relfrozenxid, TransactionId relminmxid,
							  TransactionId FreezeLimit, TransactionId MultiXactCutoff);
extern bool heap_tuple_should_freeze(HeapTupleHeader tuple,
									 const struct VacuumCutoffs *cutoffs,
									 TransactionId *NoFreezePageRelfrozenXid,
									 MultiXactId *NoFreezePageRelminMxid);
extern bool heap_tuple_needs_eventual_freeze(HeapTupleHeader tuple);

extern void simple_heap_insert(Relation relation, HeapTuple tup);
extern void simple_heap_delete(Relation relation, ItemPointer tid);
extern void simple_heap_update(Relation relation, ItemPointer otid,
							   HeapTuple tup, TU_UpdateIndexes *update_indexes);

extern TransactionId heap_index_delete_tuples(Relation rel,
											  TM_IndexDeleteOp *delstate);

/* 位于 heap/pruneheap.c */
struct GlobalVisState;
extern void heap_page_prune_opt(Relation relation, Buffer buffer);
extern void heap_page_prune_and_freeze(Relation relation, Buffer buffer,
									   struct GlobalVisState *vistest,
									   int options,
									   struct VacuumCutoffs *cutoffs,
									   PruneFreezeResult *presult,
									   PruneReason reason,
									   OffsetNumber *off_loc,
									   TransactionId *new_relfrozen_xid,
									   MultiXactId *new_relmin_mxid);
extern void heap_page_prune_execute(Buffer buffer, bool lp_truncate_only,
									OffsetNumber *redirected, int nredirected,
									OffsetNumber *nowdead, int ndead,
									OffsetNumber *nowunused, int nunused);
extern void heap_get_root_tuples(Page page, OffsetNumber *root_offsets);
extern void log_heap_prune_and_freeze(Relation relation, Buffer buffer,
									  TransactionId conflict_xid,
									  bool cleanup_lock,
									  PruneReason reason,
									  HeapTupleFreeze *frozen, int nfrozen,
									  OffsetNumber *redirected, int nredirected,
									  OffsetNumber *dead, int ndead,
									  OffsetNumber *unused, int nunused);

/* 位于 heap/vacuumlazy.c */
struct VacuumParams;
extern void heap_vacuum_rel(Relation rel,
							struct VacuumParams *params, BufferAccessStrategy bstrategy);

/* 位于 heap/heapam_visibility.c */
extern bool HeapTupleSatisfiesVisibility(HeapTuple htup, Snapshot snapshot,
										 Buffer buffer);
extern TM_Result HeapTupleSatisfiesUpdate(HeapTuple htup, CommandId curcid,
										  Buffer buffer);
extern HTSV_Result HeapTupleSatisfiesVacuum(HeapTuple htup, TransactionId OldestXmin,
											Buffer buffer);
extern HTSV_Result HeapTupleSatisfiesVacuumHorizon(HeapTuple htup, Buffer buffer,
												   TransactionId *dead_after);
extern void HeapTupleSetHintBits(HeapTupleHeader tuple, Buffer buffer,
								 uint16 infomask, TransactionId xid);
extern bool HeapTupleHeaderIsOnlyLocked(HeapTupleHeader tuple);
extern bool HeapTupleIsSurelyDead(HeapTuple htup,
								  struct GlobalVisState *vistest);

/*
 * 为避免泄露过多关于 reorderbuffer 实现的
 * 细节，此函数实现在 reorderbuffer.c 而非 heapam_visibility.c 中
 */
struct HTAB;
extern bool ResolveCminCmaxDuringDecoding(struct HTAB *tuplecid_data,
										  Snapshot snapshot,
										  HeapTuple htup,
										  Buffer buffer,
										  CommandId *cmin, CommandId *cmax);
extern void HeapCheckForSerializableConflictOut(bool visible, Relation relation, HeapTuple tuple,
												Buffer buffer, Snapshot snapshot);

/*
 * heap_execute_freeze_tuple
 *		使用调用者提供的冻结计划执行元组的预处理冻结。
 *
 * 调用者负责确保没有其他后端能访问
 * 该元组底层的存储，方式可以是持有包含该元组的缓冲区上的
 * 排他锁（lazy VACUUM 采用的方式），或使其位于
 * 私有存储中（CLUSTER 及其相关操作采用的方式）。
 */
static inline void
heap_execute_freeze_tuple(HeapTupleHeader tuple, HeapTupleFreeze *frz)
{
	HeapTupleHeaderSetXmax(tuple, frz->xmax);

	if (frz->frzflags & XLH_FREEZE_XVAC)
		HeapTupleHeaderSetXvac(tuple, FrozenTransactionId);

	if (frz->frzflags & XLH_INVALID_XVAC)
		HeapTupleHeaderSetXvac(tuple, InvalidTransactionId);

	tuple->t_infomask = frz->t_infomask;
	tuple->t_infomask2 = frz->t_infomask2;
}

#endif							/* HEAPAM_H */
