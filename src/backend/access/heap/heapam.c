/*-------------------------------------------------------------------------
 *
 * heapam.c
 *    堆访问方法代码
 *
 * 版权所有 (c) 1996-2025, PostgreSQL 全球开发组
 * 版权所有 (c) 1994, 加利福尼亚大学董事会
 *
 *
 * 标识
 *	  src/backend/access/heap/heapam.c
 *
 *
 * 接口例程
 *		heap_beginscan	- 开始关系扫描
 *		heap_rescan		- 重启关系扫描
 *		heap_endscan	- 结束关系扫描
 *		heap_getnext	- 获取扫描中的下一条元组
 *		heap_fetch		- 根据给定 tid 获取元组
 *		heap_insert		- 向关系中插入元组
 *		heap_multi_insert - 向关系中插入多条元组
 *		heap_delete		- 从关系中删除元组
 *		heap_update		- 用另一个元组替换关系中的元组
 *
 * 注释
 *	  本文件包含 heap_ 系列例程，实现了所有 POSTGRES
 *	  关系使用的 POSTGRES 堆访问方法。
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/heaptoast.h"
#include "access/hio.h"
#include "access/multixact.h"
#include "access/subtrans.h"
#include "access/syncscan.h"
#include "access/valid.h"
#include "access/visibilitymap.h"
#include "access/xloginsert.h"
#include "catalog/pg_database.h"
#include "catalog/pg_database_d.h"
#include "commands/vacuum.h"
#include "pgstat.h"
#include "port/pg_bitutils.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "utils/datum.h"
#include "utils/injection_point.h"
#include "utils/inval.h"
#include "utils/spccache.h"
#include "utils/syscache.h"


static HeapTuple heap_prepare_insert(Relation relation, HeapTuple tup,
									 TransactionId xid, CommandId cid, int options);
static XLogRecPtr log_heap_update(Relation reln, Buffer oldbuf,
								  Buffer newbuf, HeapTuple oldtup,
								  HeapTuple newtup, HeapTuple old_key_tuple,
								  bool all_visible_cleared, bool new_all_visible_cleared);
#ifdef USE_ASSERT_CHECKING
static void check_lock_if_inplace_updateable_rel(Relation relation,
												 ItemPointer otid,
												 HeapTuple newtup);
static void check_inplace_rel_lock(HeapTuple oldtup);
#endif
static Bitmapset *HeapDetermineColumnsInfo(Relation relation,
										   Bitmapset *interesting_cols,
										   Bitmapset *external_cols,
										   HeapTuple oldtup, HeapTuple newtup,
										   bool *has_external);
static bool heap_acquire_tuplock(Relation relation, ItemPointer tid,
								 LockTupleMode mode, LockWaitPolicy wait_policy,
								 bool *have_tuple_lock);
static inline BlockNumber heapgettup_advance_block(HeapScanDesc scan,
												   BlockNumber block,
												   ScanDirection dir);
static pg_noinline BlockNumber heapgettup_initial_block(HeapScanDesc scan,
														ScanDirection dir);
static void compute_new_xmax_infomask(TransactionId xmax, uint16 old_infomask,
									  uint16 old_infomask2, TransactionId add_to_xmax,
									  LockTupleMode mode, bool is_update,
									  TransactionId *result_xmax, uint16 *result_infomask,
									  uint16 *result_infomask2);
static TM_Result heap_lock_updated_tuple(Relation rel,
										 uint16 prior_infomask,
										 TransactionId prior_rawxmax,
										 const ItemPointerData *prior_ctid,
										 TransactionId xid,
										 LockTupleMode mode);
static void GetMultiXactIdHintBits(MultiXactId multi, uint16 *new_infomask,
								   uint16 *new_infomask2);
static TransactionId MultiXactIdGetUpdateXid(TransactionId xmax,
											 uint16 t_infomask);
static bool DoesMultiXactIdConflict(MultiXactId multi, uint16 infomask,
									LockTupleMode lockmode, bool *current_is_member);
static void MultiXactIdWait(MultiXactId multi, MultiXactStatus status, uint16 infomask,
							Relation rel, ItemPointer ctid, XLTW_Oper oper,
							int *remaining);
static bool ConditionalMultiXactIdWait(MultiXactId multi, MultiXactStatus status,
									   uint16 infomask, Relation rel, int *remaining,
									   bool logLockFailure);
static void index_delete_sort(TM_IndexDeleteOp *delstate);
static int	bottomup_sort_and_shrink(TM_IndexDeleteOp *delstate);
static XLogRecPtr log_heap_new_cid(Relation relation, HeapTuple tup);
static HeapTuple ExtractReplicaIdentity(Relation relation, HeapTuple tp, bool key_required,
										bool *copy);


/*
 * 每种元组锁模式都有一个对应的重量级锁，以及一个或两个对应的
 * MultiXactStatus（一个仅用于锁定元组，另一个用于更新元组）。
 * 此表（及下面的宏）帮助我们确定任何特定元组锁强度应使用的
 * 重量级锁模式和 MultiXactStatus 值。
 *
 * 这些都和 InplaceUpdateTupleLock（ExclusiveLock 的别名）配合使用。
 *
 * 不要直接查看 lockstatus/updstatus！请使用 get_mxact_status_for_lock。
 */
static const struct
{
	LOCKMODE	hwlock;
	int			lockstatus;
	int			updstatus;
}

			tupleLockExtraInfo[MaxLockTupleMode + 1] =
{
	{							/* LockTupleKeyShare（键共享锁） */
		AccessShareLock,
		MultiXactStatusForKeyShare,
		-1						/* KeyShare 不允许更新元组 */
	},
	{							/* LockTupleShare（共享锁） */
		RowShareLock,
		MultiXactStatusForShare,
		-1						/* Share 不允许更新元组 */
	},
	{							/* LockTupleNoKeyExclusive（无键排他锁） */
		ExclusiveLock,
		MultiXactStatusForNoKeyUpdate,
		MultiXactStatusNoKeyUpdate
	},
	{							/* LockTupleExclusive（排他锁） */
		AccessExclusiveLock,
		MultiXactStatusForUpdate,
		MultiXactStatusUpdate
	}
};

/* 根据给定的 MultiXactStatus 获取 LOCKMODE */
#define LOCKMODE_from_mxstatus(status) \
			(tupleLockExtraInfo[TUPLOCK_from_mxstatus((status))].hwlock)

/*
 * 使用 LockTupleMode 强度值获取元组上的重量级锁。
 * 这比让每个调用者将其转换为 lock.h 的 LOCKMODE 更易读。
 */
#define LockTupleTuplock(rel, tup, mode) \
	LockTuple((rel), (tup), tupleLockExtraInfo[mode].hwlock)
#define UnlockTupleTuplock(rel, tup, mode) \
	UnlockTuple((rel), (tup), tupleLockExtraInfo[mode].hwlock)
#define ConditionalLockTupleTuplock(rel, tup, mode, log) \
	ConditionalLockTuple((rel), (tup), tupleLockExtraInfo[mode].hwlock, (log))

#ifdef USE_PREFETCH
/*
 * heap_index_delete_tuples 和 index_delete_prefetch_buffer 使用此结构
 * 来协调预取活动
 */
typedef struct
{
	BlockNumber cur_hblkno;
	int			next_item;
	int			ndeltids;
	TM_IndexDelete *deltids;
} IndexDeletePrefetchState;
#endif

/* heap_index_delete_tuples 自底向上索引删除的成本常量 */
#define BOTTOMUP_MAX_NBLOCKS			6
#define BOTTOMUP_TOLERANCE_NBLOCKS		3

/*
 * heap_index_delete_tuples 在确定必须访问哪些堆块以帮助其
 * 自底向上索引删除调用者时使用此结构
 */
typedef struct IndexDeleteCounts
{
	int16		npromisingtids; /* 组中"有前景的" TID 数量 */
	int16		ntids;			/* 组中 TID 数量 */
	int16		ifirsttid;		/* 组中第一个 deltid 的偏移量 */
} IndexDeleteCounts;

/*
 * 此表将每个特定的 MultiXactStatus 值映射到其对应的元组锁强度值。
 */
static const int MultiXactStatusLock[MaxMultiXactStatus + 1] =
{
	LockTupleKeyShare,			/* ForKeyShare（用于键共享） */
	LockTupleShare,				/* ForShare（用于共享） */
	LockTupleNoKeyExclusive,	/* ForNoKeyUpdate（用于无键更新） */
	LockTupleExclusive,			/* ForUpdate（用于更新） */
	LockTupleNoKeyExclusive,	/* NoKeyUpdate（无键更新） */
	LockTupleExclusive			/* Update（更新） */
};

/* 根据给定的 MultiXactStatus 获取 LockTupleMode */
#define TUPLOCK_from_mxstatus(status) \
			(MultiXactStatusLock[(status)])

/*
 * 如果可能需要 TOAST 访问，检查我们是否有有效的快照。
 */
static inline void
AssertHasSnapshotForToast(Relation rel)
{
#ifdef USE_ASSERT_CHECKING

	/* bootstrap 模式特别会破坏此规则 */
	if (!IsNormalProcessingMode())
		return;

	/* 如果关系没有 TOAST 表，那就可以了 */
	if (!OidIsValid(rel->rd_rel->reltoastrelid))
		return;

	Assert(HaveRegisteredOrActiveSnapshot());

#endif							/* USE_ASSERT_CHECKING */
}

/* ----------------------------------------------------------------
 *						 堆支持例程
 * ----------------------------------------------------------------
 */

/*
 * 并行顺序扫描的流式读取 API 回调。返回调用者希望从读取流中获取的
 * 下一个块，当扫描完成时返回 InvalidBlockNumber。
 */
static BlockNumber
heap_scan_stream_read_next_parallel(ReadStream *stream,
									void *callback_private_data,
									void *per_buffer_data)
{
	HeapScanDesc scan = (HeapScanDesc) callback_private_data;

	Assert(ScanDirectionIsForward(scan->rs_dir));
	Assert(scan->rs_base.rs_parallel);

	if (unlikely(!scan->rs_inited))
	{
		/* 并行扫描 */
		table_block_parallelscan_startblock_init(scan->rs_base.rs_rd,
												 scan->rs_parallelworkerdata,
												 (ParallelBlockTableScanDesc) scan->rs_base.rs_parallel);

		/* 如果没有更多块，可能返回 InvalidBlockNumber */
		scan->rs_prefetch_block = table_block_parallelscan_nextpage(scan->rs_base.rs_rd,
																	scan->rs_parallelworkerdata,
																	(ParallelBlockTableScanDesc) scan->rs_base.rs_parallel);
		scan->rs_inited = true;
	}
	else
	{
		scan->rs_prefetch_block = table_block_parallelscan_nextpage(scan->rs_base.rs_rd,
																	scan->rs_parallelworkerdata, (ParallelBlockTableScanDesc)
																	scan->rs_base.rs_parallel);
	}

	return scan->rs_prefetch_block;
}

/*
 * 串行顺序扫描和 TID 范围扫描的流式读取 API 回调。
 * 返回调用者希望从读取流中获取的下一个块，当扫描完成时返回 InvalidBlockNumber。
 */
static BlockNumber
heap_scan_stream_read_next_serial(ReadStream *stream,
								  void *callback_private_data,
								  void *per_buffer_data)
{
	HeapScanDesc scan = (HeapScanDesc) callback_private_data;

	if (unlikely(!scan->rs_inited))
	{
		scan->rs_prefetch_block = heapgettup_initial_block(scan, scan->rs_dir);
		scan->rs_inited = true;
	}
	else
		scan->rs_prefetch_block = heapgettup_advance_block(scan,
														   scan->rs_prefetch_block,
														   scan->rs_dir);

	return scan->rs_prefetch_block;
}

/*
 * 位图堆扫描的读取流 API 回调。
 * 返回调用者希望从读取流中获取的下一个块，当扫描完成时返回 InvalidBlockNumber。
 */
static BlockNumber
bitmapheap_stream_read_next(ReadStream *pgsr, void *private_data,
							void *per_buffer_data)
{
	TBMIterateResult *tbmres = per_buffer_data;
	BitmapHeapScanDesc bscan = (BitmapHeapScanDesc) private_data;
	HeapScanDesc hscan = (HeapScanDesc) bscan;
	TableScanDesc sscan = &hscan->rs_base;

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		/* 位图中没有更多条目 */
		if (!tbm_iterate(&sscan->st.rs_tbmiterator, tbmres))
			return InvalidBlockNumber;

		/*
		 * 忽略超出我们认为的关系末尾之外的任何声明条目。关系可能在扫描开始后
		 * 被扩展了（我们只持有 AccessShareLock，可能是来自此后端的插入）。
		 * 但在 SERIALIZABLE 隔离级别下我们不采用此优化，因为我们需要检查
		 * 索引可到达的所有不可见元组。
		 */
		if (!IsolationIsSerializable() &&
			tbmres->blockno >= hscan->rs_nblocks)
			continue;

		return tbmres->blockno;
	}

	/* 不可到达 */
	Assert(false);
}

/* ----------------
 *		initscan - heap_beginscan 和 heap_rescan 的公共扫描代码
 * ----------------
 */
static void
initscan(HeapScanDesc scan, ScanKey key, bool keep_startblock)
{
	ParallelBlockTableScanDesc bpscan = NULL;
	bool		allow_strat;
	bool		allow_sync;

	/*
	 * 确定需要扫描的块数。
	 *
	 * 在扫描开始时做一次就足够了，因为扫描进行期间添加的任何元组
	 * 对我的快照来说都是不可见的。
	 * （使用非 MVCC 快照时并非如此。但我们无论如何也无法保证返回
	 * 扫描开始后添加的元组，因为它们可能进入我们已经扫描过的页面。
	 * 要保证非 MVCC 快照的结果一致性，调用者必须持有更高级别的锁，
	 * 以确保相关的元组不会改变。）
	 */
	if (scan->rs_base.rs_parallel != NULL)
	{
		bpscan = (ParallelBlockTableScanDesc) scan->rs_base.rs_parallel;
		scan->rs_nblocks = bpscan->phs_nblocks;
	}
	else
		scan->rs_nblocks = RelationGetNumberOfBlocks(scan->rs_base.rs_rd);

	/*
	 * 如果表相对于 NBuffers 较大，则使用批量读取访问策略并启用同步扫描
	 * （参见 syncscan.c）。虽然这些功能的阈值可以不同，但我们让它们相同，
	 * 这样只有两种行为需要调优，而不是四种。
	 * （然而，有些调用者需要能够独立于表大小来禁用其中一种或两种行为；
	 * 此外还有一个 GUC 变量可以禁用同步扫描。）
	 *
	 * 注意 table_block_parallelscan_initialize 有非常相似的测试；
	 * 如果你修改此处，考虑同时修改那里。
	 */
	if (!RelationUsesLocalBuffers(scan->rs_base.rs_rd) &&
		scan->rs_nblocks > NBuffers / 4)
	{
		allow_strat = (scan->rs_base.rs_flags & SO_ALLOW_STRAT) != 0;
		allow_sync = (scan->rs_base.rs_flags & SO_ALLOW_SYNC) != 0;
	}
	else
		allow_strat = allow_sync = false;

	if (allow_strat)
	{
		/* 重新扫描时保留之前的策略对象。 */
		if (scan->rs_strategy == NULL)
			scan->rs_strategy = GetAccessStrategy(BAS_BULKREAD);
	}
	else
	{
		if (scan->rs_strategy != NULL)
			FreeAccessStrategy(scan->rs_strategy);
		scan->rs_strategy = NULL;
	}

	if (scan->rs_base.rs_parallel != NULL)
	{
		/* 对于并行扫描，遵循 ParallelTableScanDesc 的设置。 */
		if (scan->rs_base.rs_parallel->phs_syncscan)
			scan->rs_base.rs_flags |= SO_ALLOW_SYNC;
		else
			scan->rs_base.rs_flags &= ~SO_ALLOW_SYNC;
	}
	else if (keep_startblock)
	{
		/*
		 * 重新扫描时，我们希望保留之前的 startblock 设置，
		 * 这样回退游标不会产生意外结果。但需要重置活动的 syncscan 设置。
		 */
		if (allow_sync && synchronize_seqscans)
			scan->rs_base.rs_flags |= SO_ALLOW_SYNC;
		else
			scan->rs_base.rs_flags &= ~SO_ALLOW_SYNC;
	}
	else if (allow_sync && synchronize_seqscans)
	{
		scan->rs_base.rs_flags |= SO_ALLOW_SYNC;
		scan->rs_startblock = ss_get_location(scan->rs_base.rs_rd, scan->rs_nblocks);
	}
	else
	{
		scan->rs_base.rs_flags &= ~SO_ALLOW_SYNC;
		scan->rs_startblock = 0;
	}

	scan->rs_numblocks = InvalidBlockNumber;
	scan->rs_inited = false;
	scan->rs_ctup.t_data = NULL;
	ItemPointerSetInvalid(&scan->rs_ctup.t_self);
	scan->rs_cbuf = InvalidBuffer;
	scan->rs_cblock = InvalidBlockNumber;
	scan->rs_ntuples = 0;
	scan->rs_cindex = 0;

	/*
	 * 初始化为 ForwardScanDirection，因为这是最常见的方向，
	 * 且堆扫描通常在反向扫描之前先正向扫描（例如游标）。
	 */
	scan->rs_dir = ForwardScanDirection;
	scan->rs_prefetch_block = InvalidBlockNumber;

	/* 当未 rs_inited 时，逐页扫描字段始终无效 */

	/*
	 * 如果合适，复制扫描键
	 */
	if (key != NULL && scan->rs_base.rs_nkeys > 0)
		memcpy(scan->rs_base.rs_key, key, scan->rs_base.rs_nkeys * sizeof(ScanKeyData));

	/*
	 * 目前，我们仅对顺序堆扫描有统计计数器（但对于位图扫描，
	 * 底层的位图索引扫描会被计数；对于采样扫描，我们更新元组获取的统计信息）。
	 */
	if (scan->rs_base.rs_flags & SO_TYPE_SEQSCAN)
		pgstat_count_heap_scan(scan->rs_base.rs_rd);
}

/*
 * heap_setscanlimits - 限制堆扫描的范围
 *
 * startBlk 是起始页面
 * numBlks 是要扫描的页面数（InvalidBlockNumber 表示"全部"）
 */
void
heap_setscanlimits(TableScanDesc sscan, BlockNumber startBlk, BlockNumber numBlks)
{
	HeapScanDesc scan = (HeapScanDesc) sscan;

	Assert(!scan->rs_inited);	/* 否则为时已晚，无法修改 */
	/* 否则 rs_startblock 是有效的 */
	Assert(!(scan->rs_base.rs_flags & SO_ALLOW_SYNC));

	/* 检查 startBlk 是否有效（但允许零块的情况...） */
	Assert(startBlk == 0 || startBlk < scan->rs_nblocks);

	scan->rs_startblock = startBlk;
	scan->rs_numblocks = numBlks;
}

/*
 * heap_prepare_pagescan() 的逐元组循环。提取出来以便可以多次调用，
 * 并使用 all_visible、check_serializable 的常量参数。
 */
pg_attribute_always_inline
static int
page_collect_tuples(HeapScanDesc scan, Snapshot snapshot,
					Page page, Buffer buffer,
					BlockNumber block, int lines,
					bool all_visible, bool check_serializable)
{
	int			ntup = 0;
	OffsetNumber lineoff;

	for (lineoff = FirstOffsetNumber; lineoff <= lines; lineoff++)
	{
		ItemId		lpp = PageGetItemId(page, lineoff);
		HeapTupleData loctup;
		bool		valid;

		if (!ItemIdIsNormal(lpp))
			continue;

		loctup.t_data = (HeapTupleHeader) PageGetItem(page, lpp);
		loctup.t_len = ItemIdGetLength(lpp);
		loctup.t_tableOid = RelationGetRelid(scan->rs_base.rs_rd);
		ItemPointerSet(&(loctup.t_self), block, lineoff);

		if (all_visible)
			valid = true;
		else
			valid = HeapTupleSatisfiesVisibility(&loctup, snapshot, buffer);

		if (check_serializable)
			HeapCheckForSerializableConflictOut(valid, scan->rs_base.rs_rd,
												&loctup, buffer, snapshot);

		if (valid)
		{
			scan->rs_vistuples[ntup] = lineoff;
			ntup++;
		}
	}

	Assert(ntup <= MaxHeapTuplesPerPage);

	return ntup;
}

/*
 * heap_prepare_pagescan - 准备当前扫描页面以在分页模式下扫描
 *
 * 准备工作目前包括：1. 修剪扫描的 rs_cbuf 页面，2. 用可见元组的
 * OffsetNumber 填充 rs_vistuples[] 数组。
 */
void
heap_prepare_pagescan(TableScanDesc sscan)
{
	HeapScanDesc scan = (HeapScanDesc) sscan;
	Buffer		buffer = scan->rs_cbuf;
	BlockNumber block = scan->rs_cblock;
	Snapshot	snapshot;
	Page		page;
	int			lines;
	bool		all_visible;
	bool		check_serializable;

	Assert(BufferGetBlockNumber(buffer) == block);

	/* 确保在非分页模式下不会意外被使用 */
	Assert(scan->rs_base.rs_flags & SO_ALLOW_PAGEMODE);
	snapshot = scan->rs_base.rs_snapshot;

	/*
	 * 如果可能，修剪并修复整个页面的碎片。
	 */
	heap_page_prune_opt(scan->rs_base.rs_rd, buffer);

	/*
	 * 在检查元组可见性时，我们必须持有缓冲区内容的共享锁。
	 * 但之后，只要我们持有缓冲区 pin，我们发现为可见的元组就保证是好的。
	 */
	LockBuffer(buffer, BUFFER_LOCK_SHARE);

	page = BufferGetPage(buffer);
	lines = PageGetMaxOffsetNumber(page);

	/*
	 * 如果全可见标志指示页面上所有元组对所有人都可见，
	 * 我们可以跳过逐元组的可见性测试。
	 *
	 * 注意：在热备模式下，一个在主机上对所有事务可见的元组，
	 * 在备机上的只读事务中可能仍然不可见。我们通过跟踪可见元组的
	 * 最小 xmin 作为分界 XID 来部分处理此问题，同时在主机上标记
	 * 页面为全可见并通过 WAL 记录该操作以及可见性映射 SET 操作。
	 * 在热备中，我们等待（或中止）所有可能无法看到页面上一个或多个
	 * 元组的事务。这就是索引仅扫描在热备中正常工作的原理。
	 * 索引仅扫描和堆扫描之间的一个关键区别是，索引仅扫描完全依赖
	 * 可见性映射，而堆扫描查看页面级别的 PD_ALL_VISIBLE 标志。
	 * 我们不确定页面级别的标志是否可以以相同方式信任，因为它可能
	 * 在没有显式 WAL 日志记录的情况下传播，例如通过整页写入。
	 * 在我们能够毫无疑问地证明这一点之前，让我们对每个元组进行
	 * 可见性检查。
	 */
	all_visible = PageIsAllVisible(page) && !snapshot->takenDuringRecovery;
	check_serializable =
		CheckForSerializableConflictOutNeeded(scan->rs_base.rs_rd, snapshot);

	/*
	 * 我们使用常量参数调用 page_collect_tuples()，以便编译器对常量参数
	 * 进行常量折叠。需要在多个编译器上使用常量参数（而非变量）的单独调用
	 * 来实际执行常量折叠。
	 */
	if (likely(all_visible))
	{
		if (likely(!check_serializable))
			scan->rs_ntuples = page_collect_tuples(scan, snapshot, page, buffer,
												   block, lines, true, false);
		else
			scan->rs_ntuples = page_collect_tuples(scan, snapshot, page, buffer,
												   block, lines, true, true);
	}
	else
	{
		if (likely(!check_serializable))
			scan->rs_ntuples = page_collect_tuples(scan, snapshot, page, buffer,
												   block, lines, false, false);
		else
			scan->rs_ntuples = page_collect_tuples(scan, snapshot, page, buffer,
												   block, lines, false, true);
	}

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
}

/*
 * heap_fetch_next_buffer - 从 MAIN_FORKNUM 读取并固定下一个块。
 *
 * 从读取流中读取扫描关系的下一个块，并将其保存在扫描描述符中。
 * 它已经被固定。
 */
static inline void
heap_fetch_next_buffer(HeapScanDesc scan, ScanDirection dir)
{
	Assert(scan->rs_read_stream);

	/* 释放先前的扫描缓冲区（如果有的话） */
	if (BufferIsValid(scan->rs_cbuf))
	{
		ReleaseBuffer(scan->rs_cbuf);
		scan->rs_cbuf = InvalidBuffer;
	}

	/*
	 * 确保每个页面至少检查一次中断。在更高代码级别的检查将无法
	 * 阻止遇到大量连续死元组页面的顺序扫描。
	 */
	CHECK_FOR_INTERRUPTS();

	/*
	 * 如果扫描方向正在改变，将预取块重置为当前块。否则，在按新的、
	 * 正确的扫描方向预取块之前，我们会错误地再次预取预取块和当前块
	 * 之间的块。
	 */
	if (unlikely(scan->rs_dir != dir))
	{
		scan->rs_prefetch_block = scan->rs_cblock;
		read_stream_reset(scan->rs_read_stream);
	}

	scan->rs_dir = dir;

	scan->rs_cbuf = read_stream_next_buffer(scan->rs_read_stream, NULL);
	if (BufferIsValid(scan->rs_cbuf))
		scan->rs_cblock = BufferGetBlockNumber(scan->rs_cbuf);
}

/*
 * heapgettup_initial_block - 返回第一个要扫描的 BlockNumber
 *
 * 当没有块需要扫描时，返回 InvalidBlockNumber。这可能发生在空表
 * 以及并行扫描中，当并行工作进程在我们可以获得第一页之前就获取了所有页面时。
 */
static pg_noinline BlockNumber
heapgettup_initial_block(HeapScanDesc scan, ScanDirection dir)
{
	Assert(!scan->rs_inited);
	Assert(scan->rs_base.rs_parallel == NULL);

	/* 当没有页面需要扫描时，返回 InvalidBlockNumber */
	if (scan->rs_nblocks == 0 || scan->rs_numblocks == 0)
		return InvalidBlockNumber;

	if (ScanDirectionIsForward(dir))
	{
		return scan->rs_startblock;
	}
	else
	{
		/*
		 * 在反向扫描中禁用到 syncscan 逻辑的报告；其他人不太可能
		 * 同时做同样的事情，更可能的是我们只会搞乱正向扫描器。
		 */
		scan->rs_base.rs_flags &= ~SO_ALLOW_SYNC;

		/*
		 * 从扫描的最后一页开始。如果 rs_numblocks 被 heap_setscanlimits()
		 * 调整过，确保将其纳入考虑。
		 */
		if (scan->rs_numblocks != InvalidBlockNumber)
			return (scan->rs_startblock + scan->rs_numblocks - 1) % scan->rs_nblocks;

		if (scan->rs_startblock > 0)
			return scan->rs_startblock - 1;

		return scan->rs_nblocks - 1;
	}
}


/*
 * heapgettup_start_page - heapgettup() 的辅助函数
 *
 * 基于 scan->rs_cbuf 返回下一个要扫描的页面，并设置 *linesleft
 * 为该页面上的元组数量。同时设置 *lineoff 为第一个要扫描的偏移量，
 * 正向扫描获取第一个偏移量，反向扫描获取页面上的最后一个偏移量。
 */
static Page
heapgettup_start_page(HeapScanDesc scan, ScanDirection dir, int *linesleft,
					  OffsetNumber *lineoff)
{
	Page		page;

	Assert(scan->rs_inited);
	Assert(BufferIsValid(scan->rs_cbuf));

	/* 调用者负责在需要时确保缓冲区已锁定 */
	page = BufferGetPage(scan->rs_cbuf);

	*linesleft = PageGetMaxOffsetNumber(page) - FirstOffsetNumber + 1;

	if (ScanDirectionIsForward(dir))
		*lineoff = FirstOffsetNumber;
	else
		*lineoff = (OffsetNumber) (*linesleft);

	/* lineoff 现在引用物理上上一个或下一个 tid */
	return page;
}


/*
 * heapgettup_continue_page - heapgettup() 的辅助函数
 *
 * 基于 scan->rs_cbuf 返回下一个要扫描的页面，并设置 *linesleft
 * 为该页面上剩余要扫描的元组数量。同时根据 'dir' 中的 ScanDirection
 * 设置 *lineoff 为下一个要扫描的偏移量。
 */
static inline Page
heapgettup_continue_page(HeapScanDesc scan, ScanDirection dir, int *linesleft,
						 OffsetNumber *lineoff)
{
	Page		page;

	Assert(scan->rs_inited);
	Assert(BufferIsValid(scan->rs_cbuf));

	/* 调用者负责在需要时确保缓冲区已锁定 */
	page = BufferGetPage(scan->rs_cbuf);

	if (ScanDirectionIsForward(dir))
	{
		*lineoff = OffsetNumberNext(scan->rs_coffset);
		*linesleft = PageGetMaxOffsetNumber(page) - (*lineoff) + 1;
	}
	else
	{
		/*
		 * 当使用非 MVCC 快照时，上次扫描返回的元组可能已被清理，
		 * 因此我们必须重新建立 lineoff <= PageGetMaxOffsetNumber(page) 的不变量
		 */
		*lineoff = Min(PageGetMaxOffsetNumber(page), OffsetNumberPrev(scan->rs_coffset));
		*linesleft = *lineoff;
	}

	/* lineoff 现在引用物理上上一个或下一个 tid */
	return page;
}

/*
 * heapgettup_advance_block - heap_fetch_next_buffer() 的辅助函数
 *
 * 给定当前块号、扫描方向以及扫描描述符中的各种信息，计算下一个要扫描的
 * BlockNumber 并返回。如果没有更多块需要扫描，返回 InvalidBlockNumber
 * 以向调用者表明此事实。
 *
 * 不应调用此函数来确定初始块号——仅用于后续块。
 *
 * 当 heap_setscanlimits() 施加了限制时，此函数也会调整 rs_numblocks。
 */
static inline BlockNumber
heapgettup_advance_block(HeapScanDesc scan, BlockNumber block, ScanDirection dir)
{
	Assert(scan->rs_base.rs_parallel == NULL);

	if (likely(ScanDirectionIsForward(dir)))
	{
		block++;

		/* 回绕到堆的起始位置 */
		if (block >= scan->rs_nblocks)
			block = 0;

		/*
		 * 报告新的扫描位置以用于同步目的。但反向移动时不这样做，
		 * 那只会搞乱其他正向移动的扫描器。
		 *
		 * 注意：我们在检查扫描结束之前执行此操作，以便位置提示的最终状态
		 * 回到关系的起始位置。这不是严格必需的，但否则当你多次运行相同查询时，
		 * 每次调用的起始位置都会稍微向后偏移，这令人困惑。
		 * 但一般情况下我们不保证任何特定的顺序。
		 */
		if (scan->rs_base.rs_flags & SO_ALLOW_SYNC)
			ss_report_location(scan->rs_base.rs_rd, block);

		/* 如果回到了我们开始的位置，就完成了 */
		if (block == scan->rs_startblock)
			return InvalidBlockNumber;

		/* 检查是否达到了 heap_setscanlimits() 施加的限制 */
		if (scan->rs_numblocks != InvalidBlockNumber)
		{
			if (--scan->rs_numblocks == 0)
				return InvalidBlockNumber;
		}

		return block;
	}
	else
	{
		/* 如果最后一个块是起始位置，就完成了 */
		if (block == scan->rs_startblock)
			return InvalidBlockNumber;

		/* 检查是否达到了 heap_setscanlimits() 施加的限制 */
		if (scan->rs_numblocks != InvalidBlockNumber)
		{
			if (--scan->rs_numblocks == 0)
				return InvalidBlockNumber;
		}

		/* 当最后一页是第 0 页时，回绕到堆的末尾 */
		if (block == 0)
			block = scan->rs_nblocks;

		block--;

		return block;
	}
}

/* ----------------
 *		heapgettup - 获取下一条堆元组
 *
 *		如果尚未初始化，则初始化扫描；然后按照 "dir" 指示的方向前进到下一条
 *		元组；在 scan->rs_ctup 中返回下一条元组，如果没有更多元组，则设置
 *		scan->rs_ctup.t_data = NULL。
 *
 * 注意：尽管 nkeys/key 保存在扫描描述符中，但仍单独传递的原因是调用者
 * 可能不希望我们检查扫描键。
 *
 * 注意：当我们在任一方向上超出扫描末尾时，会重置 rs_inited。
 * 这意味着以相同扫描方向的进一步请求将重新开始扫描，这有点奇怪，
 * 但以相反扫描方向的请求将在正确方向上开始全新的扫描。
 * 后者是游标的必需行为，而前者在 Postgres 中通常是未定义行为，
 * 所以我们不太关心。
 * ----------------
 */
static void
heapgettup(HeapScanDesc scan,
		   ScanDirection dir,
		   int nkeys,
		   ScanKey key)
{
	HeapTuple	tuple = &(scan->rs_ctup);
	Page		page;
	OffsetNumber lineoff;
	int			linesleft;

	if (likely(scan->rs_inited))
	{
		/* 从之前返回的页面/元组继续 */
		LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);
		page = heapgettup_continue_page(scan, dir, &linesleft, &lineoff);
		goto continue_page;
	}

	/*
	 * 推进扫描，直到找到一个符合条件的元组或没有更多可扫描的内容
	 */
	while (true)
	{
		heap_fetch_next_buffer(scan, dir);

		/* 是否没有更多块可扫描？ */
		if (!BufferIsValid(scan->rs_cbuf))
			break;

		Assert(BufferGetBlockNumber(scan->rs_cbuf) == scan->rs_cblock);

		LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);
		page = heapgettup_start_page(scan, dir, &linesleft, &lineoff);
continue_page:

		/*
		 * 仅在还有行剩余时继续扫描页面。
		 *
		 * 注意，这可以防止我们访问超出 PageGetMaxOffsetNumber() 的行指针；
		 * 既适用于恢复表扫描时的正向扫描，也适用于开始扫描新页面时。
		 */
		for (; linesleft > 0; linesleft--, lineoff += dir)
		{
			bool		visible;
			ItemId		lpp = PageGetItemId(page, lineoff);

			if (!ItemIdIsNormal(lpp))
				continue;

			tuple->t_data = (HeapTupleHeader) PageGetItem(page, lpp);
			tuple->t_len = ItemIdGetLength(lpp);
			ItemPointerSet(&(tuple->t_self), scan->rs_cblock, lineoff);

			visible = HeapTupleSatisfiesVisibility(tuple,
												   scan->rs_base.rs_snapshot,
												   scan->rs_cbuf);

			HeapCheckForSerializableConflictOut(visible, scan->rs_base.rs_rd,
												tuple, scan->rs_cbuf,
												scan->rs_base.rs_snapshot);

			/* 跳过对此快照不可见的元组 */
			if (!visible)
				continue;

			/* 跳过任何不匹配扫描键的元组 */
			if (key != NULL &&
				!HeapKeyTest(tuple, RelationGetDescr(scan->rs_base.rs_rd),
							 nkeys, key))
				continue;

			LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
			scan->rs_coffset = lineoff;
			return;
		}

		/*
		 * 如果到达这里，意味着我们已经穷尽了此页面上的所有项，
		 * 是时候移动到下一页了。
		 */
		LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
	}

	/* 扫描结束 */
	if (BufferIsValid(scan->rs_cbuf))
		ReleaseBuffer(scan->rs_cbuf);

	scan->rs_cbuf = InvalidBuffer;
	scan->rs_cblock = InvalidBlockNumber;
	scan->rs_prefetch_block = InvalidBlockNumber;
	tuple->t_data = NULL;
	scan->rs_inited = false;
}

/* ----------------
 *		heapgettup_pagemode - 在逐页模式下获取下一条堆元组
 *
 *		与 heapgettup 相同的 API，但在逐页模式下使用
 *
 * 内部逻辑也与 heapgettup 大致相同，但有一些区别：我们不获取缓冲区内容锁
 * （那只需要在 heap_prepare_pagescan 内部发生），而且我们只遍历
 * rs_vistuples[] 中列出的元组，而不是页面上的所有元组。
 * 注意 lineindex 是从 0 开始的，而 heapgettup 中相应的循环变量 lineoff
 * 是从 1 开始的。
 * ----------------
 */
static void
heapgettup_pagemode(HeapScanDesc scan,
					ScanDirection dir,
					int nkeys,
					ScanKey key)
{
	HeapTuple	tuple = &(scan->rs_ctup);
	Page		page;
	uint32		lineindex;
	uint32		linesleft;

	if (likely(scan->rs_inited))
	{
		/* 从之前返回的页面/元组继续 */
		page = BufferGetPage(scan->rs_cbuf);

		lineindex = scan->rs_cindex + dir;
		if (ScanDirectionIsForward(dir))
			linesleft = scan->rs_ntuples - lineindex;
		else
			linesleft = scan->rs_cindex;
		/* lineindex 现在引用下一个或上一个可见 tid */

		goto continue_page;
	}

	/*
	 * 推进扫描，直到找到一个符合条件的元组或没有更多可扫描的内容
	 */
	while (true)
	{
		heap_fetch_next_buffer(scan, dir);

		/* 是否没有更多块可扫描？ */
		if (!BufferIsValid(scan->rs_cbuf))
			break;

		Assert(BufferGetBlockNumber(scan->rs_cbuf) == scan->rs_cblock);

		/* 修剪页面并确定可见元组的偏移量 */
		heap_prepare_pagescan((TableScanDesc) scan);
		page = BufferGetPage(scan->rs_cbuf);
		linesleft = scan->rs_ntuples;
		lineindex = ScanDirectionIsForward(dir) ? 0 : linesleft - 1;

		/* 块对所有元组都一样，在循环外设置一次 */
		ItemPointerSetBlockNumber(&tuple->t_self, scan->rs_cblock);

		/* lineindex 现在引用下一个或上一个可见 tid */
continue_page:

		for (; linesleft > 0; linesleft--, lineindex += dir)
		{
			ItemId		lpp;
			OffsetNumber lineoff;

			Assert(lineindex < scan->rs_ntuples);
			lineoff = scan->rs_vistuples[lineindex];
			lpp = PageGetItemId(page, lineoff);
			Assert(ItemIdIsNormal(lpp));

			tuple->t_data = (HeapTupleHeader) PageGetItem(page, lpp);
			tuple->t_len = ItemIdGetLength(lpp);
			ItemPointerSetOffsetNumber(&tuple->t_self, lineoff);

			/* 跳过任何不匹配扫描键的元组 */
			if (key != NULL &&
				!HeapKeyTest(tuple, RelationGetDescr(scan->rs_base.rs_rd),
							 nkeys, key))
				continue;

			scan->rs_cindex = lineindex;
			return;
		}
	}

	/* 扫描结束 */
	if (BufferIsValid(scan->rs_cbuf))
		ReleaseBuffer(scan->rs_cbuf);
	scan->rs_cbuf = InvalidBuffer;
	scan->rs_cblock = InvalidBlockNumber;
	scan->rs_prefetch_block = InvalidBlockNumber;
	tuple->t_data = NULL;
	scan->rs_inited = false;
}


/* ----------------------------------------------------------------
 *					 堆访问方法接口
 * ----------------------------------------------------------------
 */


TableScanDesc
heap_beginscan(Relation relation, Snapshot snapshot,
			   int nkeys, ScanKey key,
			   ParallelTableScanDesc parallel_scan,
			   uint32 flags)
{
	HeapScanDesc scan;

	/*
	 * 在扫描关系时增加关系引用计数
	 *
	 * 这只是为了真正确保在扫描持有指向 relcache 条目的指针时，
	 * 该条目不会消失。调用者无论如何都应该保持关系打开，
	 * 所以在所有正常场景下这都是多余的...
	 */
	RelationIncrementReferenceCount(relation);

	/*
	 * 分配并初始化扫描描述符
	 */
	if (flags & SO_TYPE_BITMAPSCAN)
	{
		BitmapHeapScanDesc bscan = palloc(sizeof(BitmapHeapScanDescData));

		/*
		 * 位图堆扫描没有任何普通堆扫描没有的字段，
		 * 因此此处不需要特殊初始化。
		 */
		scan = (HeapScanDesc) bscan;
	}
	else
		scan = (HeapScanDesc) palloc(sizeof(HeapScanDescData));

	scan->rs_base.rs_rd = relation;
	scan->rs_base.rs_snapshot = snapshot;
	scan->rs_base.rs_nkeys = nkeys;
	scan->rs_base.rs_flags = flags;
	scan->rs_base.rs_parallel = parallel_scan;
	scan->rs_strategy = NULL;	/* 在 initscan 中设置 */
	scan->rs_cbuf = InvalidBuffer;

	/*
	 * 如果快照不是 MVCC 安全的，则禁用逐页模式。
	 */
	if (!(snapshot && IsMVCCSnapshot(snapshot)))
		scan->rs_base.rs_flags &= ~SO_ALLOW_PAGEMODE;

	/*
	 * 对于可序列化事务中的顺序扫描和采样扫描，在整个关系上获取谓词锁。
	 * 这不仅需要锁定所有匹配的元组，还需要与表中的新插入冲突。在索引扫描中，
	 * 我们在覆盖扫描条件中指定范围的索引页面上获取页面锁，但在堆扫描中，
	 * 没有更细粒度的东西可以锁定。位图扫描则有不同，我们已经扫描了索引并锁定
	 * 了覆盖谓词的索引页面。但在那种情况下，我们仍然必须锁定任何匹配的堆元组。
	 * 对于采样扫描，我们可以将锁定优化为至少页面级别的粒度，
	 * 但需要为此添加逐元组锁定。
	 */
	if (scan->rs_base.rs_flags & (SO_TYPE_SEQSCAN | SO_TYPE_SAMPLESCAN))
	{
		/*
		 * 确保能够可靠地注意到缺失的快照，即使隔离模式意味着不执行
		 * 谓词锁定（因此此处不使用快照）。
		 */
		Assert(snapshot);
		PredicateLockRelation(relation, snapshot);
	}

	/* 我们只需要设置一次 */
	scan->rs_ctup.t_tableOid = RelationGetRelid(relation);

	/*
	 * 在进行并行扫描时，为并行工作进程分配跟踪页面分配的内存。
	 */
	if (parallel_scan != NULL)
		scan->rs_parallelworkerdata = palloc(sizeof(ParallelBlockTableScanWorkerData));
	else
		scan->rs_parallelworkerdata = NULL;

	/*
	 * 我们在这里而不是在 initscan() 中做这个，因为 heap_rescan 也会调用
	 * initscan()，而我们不想再次分配内存
	 */
	if (nkeys > 0)
		scan->rs_base.rs_key = (ScanKey) palloc(sizeof(ScanKeyData) * nkeys);
	else
		scan->rs_base.rs_key = NULL;

	initscan(scan, key, false);

	scan->rs_read_stream = NULL;

	/*
	 * 为顺序扫描和 TID 范围扫描设置读取流。这应该在 initscan() 之后进行，
	 * 因为 initscan() 会分配传递给读取流 API 的 BufferAccessStrategy 对象。
	 */
	if (scan->rs_base.rs_flags & SO_TYPE_SEQSCAN ||
		scan->rs_base.rs_flags & SO_TYPE_TIDRANGESCAN)
	{
		ReadStreamBlockNumberCB cb;

		if (scan->rs_base.rs_parallel)
			cb = heap_scan_stream_read_next_parallel;
		else
			cb = heap_scan_stream_read_next_serial;

		/* ---
		 * 使用批处理模式是安全的，因为 `cb` 获取的锁永远不会在等待 IO 时持有：
		 * - 非并行情况下使用 SyncScanLock
		 * - 并行情况下，仅使用 spinlock 和 atomics
		 * ---
		 */
		scan->rs_read_stream = read_stream_begin_relation(READ_STREAM_SEQUENTIAL |
														  READ_STREAM_USE_BATCHING,
														  scan->rs_strategy,
														  scan->rs_base.rs_rd,
														  MAIN_FORKNUM,
														  cb,
														  scan,
														  0);
	}
	else if (scan->rs_base.rs_flags & SO_TYPE_BITMAPSCAN)
	{
		scan->rs_read_stream = read_stream_begin_relation(READ_STREAM_DEFAULT |
														  READ_STREAM_USE_BATCHING,
														  scan->rs_strategy,
														  scan->rs_base.rs_rd,
														  MAIN_FORKNUM,
														  bitmapheap_stream_read_next,
														  scan,
														  sizeof(TBMIterateResult));
	}


	return (TableScanDesc) scan;
}

void
heap_rescan(TableScanDesc sscan, ScanKey key, bool set_params,
			bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	HeapScanDesc scan = (HeapScanDesc) sscan;

	if (set_params)
	{
		if (allow_strat)
			scan->rs_base.rs_flags |= SO_ALLOW_STRAT;
		else
			scan->rs_base.rs_flags &= ~SO_ALLOW_STRAT;

		if (allow_sync)
			scan->rs_base.rs_flags |= SO_ALLOW_SYNC;
		else
			scan->rs_base.rs_flags &= ~SO_ALLOW_SYNC;

		if (allow_pagemode && scan->rs_base.rs_snapshot &&
			IsMVCCSnapshot(scan->rs_base.rs_snapshot))
			scan->rs_base.rs_flags |= SO_ALLOW_PAGEMODE;
		else
			scan->rs_base.rs_flags &= ~SO_ALLOW_PAGEMODE;
	}

	/*
	 * 释放扫描缓冲区的 pin
	 */
	if (BufferIsValid(scan->rs_cbuf))
	{
		ReleaseBuffer(scan->rs_cbuf);
		scan->rs_cbuf = InvalidBuffer;
	}

	/*
	 * SO_TYPE_BITMAPSCAN 会在此处被清理，但它相对于普通堆扫描
	 * 不持有任何额外数据
	 */

	/*
	 * 重新扫描时重置读取流。这必须在 initscan() 之前完成，
	 * 因为 read_stream_reset() 引用的某些状态会在 initscan() 中被重置。
	 */
	if (scan->rs_read_stream)
		read_stream_reset(scan->rs_read_stream);

	/*
	 * 重新初始化扫描描述符
	 */
	initscan(scan, key, true);
}

void
heap_endscan(TableScanDesc sscan)
{
	HeapScanDesc scan = (HeapScanDesc) sscan;

	/* 注意：不需要任何锁定操作 */

	/*
	 * 释放扫描缓冲区的 pin
	 */
	if (BufferIsValid(scan->rs_cbuf))
		ReleaseBuffer(scan->rs_cbuf);

	/*
	 * 必须在释放 BufferAccessStrategy 之前释放读取流。
	 */
	if (scan->rs_read_stream)
		read_stream_end(scan->rs_read_stream);

	/*
	 * 减少关系引用计数并释放扫描描述符存储
	 */
	RelationDecrementReferenceCount(scan->rs_base.rs_rd);

	if (scan->rs_base.rs_key)
		pfree(scan->rs_base.rs_key);

	if (scan->rs_strategy != NULL)
		FreeAccessStrategy(scan->rs_strategy);

	if (scan->rs_parallelworkerdata != NULL)
		pfree(scan->rs_parallelworkerdata);

	if (scan->rs_base.rs_flags & SO_TEMP_SNAPSHOT)
		UnregisterSnapshot(scan->rs_base.rs_snapshot);

	pfree(scan);
}

HeapTuple
heap_getnext(TableScanDesc sscan, ScanDirection direction)
{
	HeapScanDesc scan = (HeapScanDesc) sscan;

	/*
	 * 此函数仍然被广泛直接使用，不经过表 AM，因此添加安全检查。
	 * 我们可能稍后将其降级为断言。检查 AM 例程而非 AM oid 的原因是，
	 * 这允许编写回归测试来创建另一个重用堆处理器的 AM。
	 */
	if (unlikely(sscan->rs_rd->rd_tableam != GetHeapamTableAmRoutine()))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg_internal("only heap AM is supported")));

	/*
	 * 我们不期望直接调用 heap_getnext 时系统表或普通表的 CheckXidAlive 有效。
	 * 有关声明这些变量的详细注释，请参见 xact.c。通常我们在 tableam 级别 API
	 * 有此类检查，但此函数从许多地方被调用，因此我们需要在这里确保。
	 */
	if (unlikely(TransactionIdIsValid(CheckXidAlive) && !bsysscan))
		elog(ERROR, "unexpected heap_getnext call during logical decoding");

	/* 注意：不需要任何锁定操作 */

	if (scan->rs_base.rs_flags & SO_ALLOW_PAGEMODE)
		heapgettup_pagemode(scan, direction,
							scan->rs_base.rs_nkeys, scan->rs_base.rs_key);
	else
		heapgettup(scan, direction,
				   scan->rs_base.rs_nkeys, scan->rs_base.rs_key);

	if (scan->rs_ctup.t_data == NULL)
		return NULL;

	/*
	 * 如果到达这里，说明我们有一条新的当前扫描元组，因此指向正确的返回缓冲区
	 * 并返回元组。
	 */

	pgstat_count_heap_getnext(scan->rs_base.rs_rd);

	return &scan->rs_ctup;
}

bool
heap_getnextslot(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot)
{
	HeapScanDesc scan = (HeapScanDesc) sscan;

	/* 注意：不需要任何锁定操作 */

	if (sscan->rs_flags & SO_ALLOW_PAGEMODE)
		heapgettup_pagemode(scan, direction, sscan->rs_nkeys, sscan->rs_key);
	else
		heapgettup(scan, direction, sscan->rs_nkeys, sscan->rs_key);

	if (scan->rs_ctup.t_data == NULL)
	{
		ExecClearTuple(slot);
		return false;
	}

	/*
	 * 如果到达这里，说明我们有一条新的当前扫描元组，因此指向正确的返回缓冲区
	 * 并返回元组。
	 */

	pgstat_count_heap_getnext(scan->rs_base.rs_rd);

	ExecStoreBufferHeapTuple(&scan->rs_ctup, slot,
							 scan->rs_cbuf);
	return true;
}

void
heap_set_tidrange(TableScanDesc sscan, ItemPointer mintid,
				  ItemPointer maxtid)
{
	HeapScanDesc scan = (HeapScanDesc) sscan;
	BlockNumber startBlk;
	BlockNumber numBlks;
	ItemPointerData highestItem;
	ItemPointerData lowestItem;

	/*
	 * 对于没有任何页面的关系，我们可以简单地不设置 TID 范围。
	 * 没有元组需要扫描，因此也不会有超出给定 TID 范围的元组。
	 */
	if (scan->rs_nblocks == 0)
		return;

	/*
	 * 设置一些 ItemPointer，分别指向堆中第一个和最后一个可能的元组。
	 */
	ItemPointerSet(&highestItem, scan->rs_nblocks - 1, MaxOffsetNumber);
	ItemPointerSet(&lowestItem, 0, FirstOffsetNumber);

	/*
	 * 如果给定的最大 TID 低于关系中可能的最高 TID，则将范围限制到该值，
	 * 否则我们扫描到关系的末尾。
	 */
	if (ItemPointerCompare(maxtid, &highestItem) < 0)
		ItemPointerCopy(maxtid, &highestItem);

	/*
	 * 如果给定的最小 TID 高于关系中可能的最低 TID，则将范围限制为仅扫描
	 * 高于该值的 TID。
	 */
	if (ItemPointerCompare(mintid, &lowestItem) > 0)
		ItemPointerCopy(mintid, &lowestItem);

	/*
	 * 检查空范围并防止下面 numBlks 计算可能产生的负结果。
	 */
	if (ItemPointerCompare(&highestItem, &lowestItem) < 0)
	{
		/* 设置一个空块范围进行扫描 */
		heap_setscanlimits(sscan, 0, 0);
		return;
	}

	/*
	 * 计算第一个块和我们必须扫描的块数。我们可以在这里更激进一些，
	 * 通过检查 lowestItem 的偏移是否高于 MaxOffsetNumber，执行更多验证
	 * 来尝试进一步缩小扫描块的范围。在这种情况下，我们可以将 startBlk
	 * 加一。同样，如果 highestItem 的偏移为 0，我们可以少扫描一个块。
	 * 然而，目前这样的优化似乎不值得费心。
	 */
	startBlk = ItemPointerGetBlockNumberNoCheck(&lowestItem);

	numBlks = ItemPointerGetBlockNumberNoCheck(&highestItem) -
		ItemPointerGetBlockNumberNoCheck(&lowestItem) + 1;

	/* 设置起始块和要扫描的块数 */
	heap_setscanlimits(sscan, startBlk, numBlks);

	/* 最后，在 sscan 中设置 TID 范围 */
	ItemPointerCopy(&lowestItem, &sscan->st.tidrange.rs_mintid);
	ItemPointerCopy(&highestItem, &sscan->st.tidrange.rs_maxtid);
}

bool
heap_getnextslot_tidrange(TableScanDesc sscan, ScanDirection direction,
						  TupleTableSlot *slot)
{
	HeapScanDesc scan = (HeapScanDesc) sscan;
	ItemPointer mintid = &sscan->st.tidrange.rs_mintid;
	ItemPointer maxtid = &sscan->st.tidrange.rs_maxtid;

	/* 注意：无需操作锁 */
	for (;;)
	{
		if (sscan->rs_flags & SO_ALLOW_PAGEMODE)
			heapgettup_pagemode(scan, direction, sscan->rs_nkeys, sscan->rs_key);
		else
			heapgettup(scan, direction, sscan->rs_nkeys, sscan->rs_key);

		if (scan->rs_ctup.t_data == NULL)
		{
			ExecClearTuple(slot);
			return false;
		}

		/*
		 * heap_set_tidrange 将使用 heap_setscanlimits 将我们扫描的页面范围
		 * 限制在可能包含我们要扫描的 TID 范围的页面上。这里我们必须过滤掉
		 * 这些页面中超出该范围的任何元组。
		 */
		if (ItemPointerCompare(&scan->rs_ctup.t_self, mintid) < 0)
		{
			ExecClearTuple(slot);

			/*
			 * 反向扫描时，TID 将按降序排列。此方向上的后续元组将更低，
			 * 因此我们可以直接返回 false 来指示不再有更多元组。
			 */
			if (ScanDirectionIsBackward(direction))
				return false;

			continue;
		}

		/*
		 * 同样对于最后一页，我们必须过滤掉大于 maxtid 的 TID。
		 */
		if (ItemPointerCompare(&scan->rs_ctup.t_self, maxtid) > 0)
		{
			ExecClearTuple(slot);

			/*
			 * 正向扫描时，TID 将按升序排列。此方向上的后续元组将更高，
			 * 因此我们可以直接返回 false 来指示不再有更多元组。
			 */
			if (ScanDirectionIsForward(direction))
				return false;
			continue;
		}

		break;
	}

	/*
	 * 如果到达这里，说明我们有一条新的当前扫描元组，因此指向正确的返回缓冲区
	 * 并返回元组。
	 */
	pgstat_count_heap_getnext(scan->rs_base.rs_rd);

	ExecStoreBufferHeapTuple(&scan->rs_ctup, slot, scan->rs_cbuf);
	return true;
}

/*
 *	heap_fetch		- 根据给定的 tid 获取元组
 *
 * 进入时，tuple->t_self 是要获取的 TID。我们固定持有该元组的缓冲区，
 * 填充 *tuple 的其余字段，并根据指定的快照检查元组。
 *
 * 如果成功（元组找到并通过快照时间限定检查），则 *userbuf 设置为持有该元组
 * 的缓冲区并返回 true。调用者在使用完元组后必须解除缓冲区的 pin。
 *
 * 如果未找到元组（即项号引用了一个已删除的槽位），则 tuple->t_data 设置为 NULL，
 * *userbuf 设置为 InvalidBuffer，并返回 false。
 *
 * 如果找到了元组但未通过时间限定检查，则行为取决于 keep_buf 参数。
 * 如果 keep_buf 为 false，结果与未找到元组的情况相同。如果 keep_buf 为 true，
 * 则 tuple->t_data 和 *userbuf 以成功情况返回，调用者仍必须解除缓冲区的 pin；
 * 但返回 false。
 *
 * heap_fetch 不跟踪 HOT 链：只会获取请求的精确 TID。
 *
 * 我们在无效块号上 ereport() 但在无效项号上返回 false，这有些矛盾。
 * 不过有几个原因。一是调用者可以相对容易地检查块号的有效性，但无法在
 * 不自己读取页面的情况下检查项号。另一个原因是，当我们跟随 t_ctid 链接时，
 * 可以合理地确信页面号是有效的（因为 VACUUM 不会在未先杀死引用元组的情况下
 * 截断目标页面），但项号很可能无效。
 */
bool
heap_fetch(Relation relation,
		   Snapshot snapshot,
		   HeapTuple tuple,
		   Buffer *userbuf,
		   bool keep_buf)
{
	ItemPointer tid = &(tuple->t_self);
	ItemId		lp;
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	bool		valid;

	/*
	 * 获取并 pin 关系的适当页面。
	 */
	buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));

	/*
	 * 需要在缓冲区上持有共享锁以检查元组提交状态。
	 */
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buffer);

	/*
	 * 最好检查 offnum 是否超出范围，以防获取 TID 后进行了 VACUUM。
	 */
	offnum = ItemPointerGetOffsetNumber(tid);
	if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		*userbuf = InvalidBuffer;
		tuple->t_data = NULL;
		return false;
	}

	/*
	 * 获取与请求的 tid 对应的行指针
	 */
	lp = PageGetItemId(page, offnum);

	/*
	 * 必须检查已删除的元组。
	 */
	if (!ItemIdIsNormal(lp))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		*userbuf = InvalidBuffer;
		tuple->t_data = NULL;
		return false;
	}

	/*
	 * 填充 *tuple 字段
	 */
	tuple->t_data = (HeapTupleHeader) PageGetItem(page, lp);
	tuple->t_len = ItemIdGetLength(lp);
	tuple->t_tableOid = RelationGetRelid(relation);

	/*
	 * 检查元组可见性，然后释放锁
	 */
	valid = HeapTupleSatisfiesVisibility(tuple, snapshot, buffer);

	if (valid)
		PredicateLockTID(relation, &(tuple->t_self), snapshot,
						 HeapTupleHeaderGetXmin(tuple->t_data));

	HeapCheckForSerializableConflictOut(valid, relation, tuple, buffer, snapshot);

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	if (valid)
	{
		/*
		 * 所有检查通过，因此将元组作为有效返回。调用者现在负责释放缓冲区。
		 */
		*userbuf = buffer;

		return true;
	}

	/* 元组未通过时间限定检查，但调用者可能仍然想查看它。 */
	if (keep_buf)
		*userbuf = buffer;
	else
	{
		ReleaseBuffer(buffer);
		*userbuf = InvalidBuffer;
		tuple->t_data = NULL;
	}

	return false;
}

/*
 *	heap_hot_search_buffer	- 在 HOT 链中搜索满足快照的元组
 *
 * 进入时，*tid 是元组的 TID（可以是简单元组，也可以是 HOT 链的根），
 * buffer 是持有此元组的缓冲区。我们搜索满足给定快照的第一个链成员。
 * 如果找到，我们更新 *tid 以引用该元组的偏移号，并返回 true。
 * 如果没有匹配，返回 false，不修改 *tid。
 *
 * heapTuple 是调用者提供的缓冲区。当找到匹配时，我们在这里返回元组，
 * 同时更新 *tid。如果未找到匹配，返回时此缓冲区的内容是未定义的。
 *
 * 如果 all_dead 不为 NULL，我们检查不可见元组是否对所有事务全局死亡；
 * 如果 HOT 链的所有成员都可清理，则 *all_dead 设置为 true，否则为 false。
 *
 * 与 heap_fetch 不同，调用者必须已经对缓冲区拥有 pin 和（至少）共享锁；
 * 在退出时它仍然被固定/锁定。
 */
bool
heap_hot_search_buffer(ItemPointer tid, Relation relation, Buffer buffer,
					   Snapshot snapshot, HeapTuple heapTuple,
					   bool *all_dead, bool first_call)
{
	Page		page = BufferGetPage(buffer);
	TransactionId prev_xmax = InvalidTransactionId;
	BlockNumber blkno;
	OffsetNumber offnum;
	bool		at_chain_start;
	bool		valid;
	bool		skip;
	GlobalVisState *vistest = NULL;

	/* 如果这不是第一次调用，之前的调用返回了一个（活跃的！）元组 */
	if (all_dead)
		*all_dead = first_call;

	blkno = ItemPointerGetBlockNumber(tid);
	offnum = ItemPointerGetOffsetNumber(tid);
	at_chain_start = first_call;
	skip = !first_call;

	/* XXX: 我们应该断言快照已被推送或注册 */
	Assert(TransactionIdIsValid(RecentXmin));
	Assert(BufferGetBlockNumber(buffer) == blkno);

	/* 扫描 HOT 链的可能的多个成员 */
	for (;;)
	{
		ItemId		lp;

		/* 检查伪造的 TID */
		if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
			break;

		lp = PageGetItemId(page, offnum);

		/* 检查未使用、已死或重定向的项 */
		if (!ItemIdIsNormal(lp))
		{
			/* 我们应该只在链的开头看到重定向 */
			if (ItemIdIsRedirected(lp) && at_chain_start)
			{
				/* 跟随重定向 */
				offnum = ItemIdGetRedirect(lp);
				at_chain_start = false;
				continue;
			}
			/* 否则必定是链的末尾 */
			break;
		}

		/*
		 * 更新 heapTuple 以指向我们当前正在调查的 HOT 链元素。
		 * 正确设置 t_self 很重要，因为 SSI 检查和历史 MVCC 快照的
		 * *Satisfies 例程需要正确的 tid 来决定可见性。
		 */
		heapTuple->t_data = (HeapTupleHeader) PageGetItem(page, lp);
		heapTuple->t_len = ItemIdGetLength(lp);
		heapTuple->t_tableOid = RelationGetRelid(relation);
		ItemPointerSet(&heapTuple->t_self, blkno, offnum);

		/*
		 * 不应该在链的开头看到 HEAP_ONLY 元组。
		 */
		if (at_chain_start && HeapTupleIsHeapOnly(heapTuple))
			break;

		/*
		 * xmin 应该匹配之前的 xmax 值，否则链已损坏。
		 */
		if (TransactionIdIsValid(prev_xmax) &&
			!TransactionIdEquals(prev_xmax,
								 HeapTupleHeaderGetXmin(heapTuple->t_data)))
			break;

		/*
		 * 当 first_call 为 true（因此 skip 初始为 false）时，我们将返回
		 * 找到的第一个元组。但在后续遍历中，heapTuple 最初会指向我们上次
		 * 返回的元组。再次返回它是不正确的（并且会永远循环），
		 * 所以我们会跳过它并返回找到的下一个匹配项。
		 */
		if (!skip)
		{
			/* 如果根据快照它是可见的，我们必须返回它 */
			valid = HeapTupleSatisfiesVisibility(heapTuple, snapshot, buffer);
			HeapCheckForSerializableConflictOut(valid, relation, heapTuple,
												buffer, snapshot);

			if (valid)
			{
				ItemPointerSetOffsetNumber(tid, offnum);
				PredicateLockTID(relation, &heapTuple->t_self, snapshot,
								 HeapTupleHeaderGetXmin(heapTuple->t_data));
				if (all_dead)
					*all_dead = false;
				return true;
			}
		}
		skip = false;

		/*
		 * 如果我们看不到它，也许其他人也看不到。根据调用者的请求，
		 * 检查所有链成员是否对所有事务死亡。
		 *
		 * 注意：如果你更改此处关于什么是"死亡"的标准，请修复规划器的
		 * get_actual_variable_range() 函数以匹配。
		 */
		if (all_dead && *all_dead)
		{
			if (!vistest)
				vistest = GlobalVisTestFor(relation);

			if (!HeapTupleIsSurelyDead(heapTuple, vistest))
				*all_dead = false;
		}

		/*
		 * 检查 HOT 链是否在此元组之后继续；如果是，获取下一个 offnum
		 * 并继续循环。
		 */
		if (HeapTupleIsHotUpdated(heapTuple))
		{
			Assert(ItemPointerGetBlockNumber(&heapTuple->t_data->t_ctid) ==
				   blkno);
			offnum = ItemPointerGetOffsetNumber(&heapTuple->t_data->t_ctid);
			at_chain_start = false;
			prev_xmax = HeapTupleHeaderGetUpdateXid(heapTuple->t_data);
		}
		else
			break;				/* 链的末尾 */
	}

	return false;
}

/*
 *	heap_get_latest_tid -  修改给定元组的最新 CTID
 *
 * 实际上，这获取根据扫描快照可见的最新版本。使用 SnapshotDirty 创建扫描
 * 以获取最新的、可能未提交的版本。
 *
 * *tid 既是输入参数也是输出参数：它被更新为行的最新版本。
 * 注意，如果没有任何版本的行通过快照测试，它不会被更改。
 */
void
heap_get_latest_tid(TableScanDesc sscan,
					ItemPointer tid)
{
	Relation	relation = sscan->rs_rd;
	Snapshot	snapshot = sscan->rs_snapshot;
	ItemPointerData ctid;
	TransactionId priorXmax;

	/*
	 * table_tuple_get_latest_tid() 已验证传入的 tid 是有效的。
	 * 但假设 t_ctid 链接是有效的——表中不应有无效的链接。
	 */
	Assert(ItemPointerIsValid(tid));

	/*
	 * 循环跟踪 t_ctid 链接。在循环顶部，ctid 是我们需要检查的元组，
	 * 而 *tid 是如果 ctid 被证明是伪造的，我们将返回的 TID。
	 *
	 * 注意，我们将循环直到到达 t_ctid 链的末尾。取决于传入的快照，
	 * 最多可能有一个可见版本的行，但我们不尝试对此进行优化。
	 */
	ctid = *tid;
	priorXmax = InvalidTransactionId;	/* 无法检查第一个 XMIN */
	for (;;)
	{
		Buffer		buffer;
		Page		page;
		OffsetNumber offnum;
		ItemId		lp;
		HeapTupleData tp;
		bool		valid;

		/*
		 * 读取、pin 并锁定页面。
		 */
		buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(&ctid));
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);

		/*
		 * 检查伪造的项号。这不被视为错误条件，因为在跟随 t_ctid 链接时
		 * 可能会发生。我们只是假设先前的 tid 是 OK 的并原样返回。
		 */
		offnum = ItemPointerGetOffsetNumber(&ctid);
		if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
		{
			UnlockReleaseBuffer(buffer);
			break;
		}
		lp = PageGetItemId(page, offnum);
		if (!ItemIdIsNormal(lp))
		{
			UnlockReleaseBuffer(buffer);
			break;
		}

		/* 可以访问元组了 */
		tp.t_self = ctid;
		tp.t_data = (HeapTupleHeader) PageGetItem(page, lp);
		tp.t_len = ItemIdGetLength(lp);
		tp.t_tableOid = RelationGetRelid(relation);

		/*
		 * 在跟随 t_ctid 链接后，我们可能到达一个不相关的元组。检查 XMIN 匹配。
		 */
		if (TransactionIdIsValid(priorXmax) &&
			!TransactionIdEquals(priorXmax, HeapTupleHeaderGetXmin(tp.t_data)))
		{
			UnlockReleaseBuffer(buffer);
			break;
		}

		/*
		 * 检查元组可见性；如果可见，将其设置为新的结果候选。
		 */
		valid = HeapTupleSatisfiesVisibility(&tp, snapshot, buffer);
		HeapCheckForSerializableConflictOut(valid, relation, &tp, buffer, snapshot);
		if (valid)
			*tid = ctid;

		/*
		 * 如果存在有效的 t_ctid 链接，跟随它，否则完成。
		 */
		if ((tp.t_data->t_infomask & HEAP_XMAX_INVALID) ||
			HeapTupleHeaderIsOnlyLocked(tp.t_data) ||
			HeapTupleHeaderIndicatesMovedPartitions(tp.t_data) ||
			ItemPointerEquals(&tp.t_self, &tp.t_data->t_ctid))
		{
			UnlockReleaseBuffer(buffer);
			break;
		}

		ctid = tp.t_data->t_ctid;
		priorXmax = HeapTupleHeaderGetUpdateXid(tp.t_data);
		UnlockReleaseBuffer(buffer);
	}							/* 循环结束 */
}


/*
 * UpdateXmaxHintBits - 在 xmax 事务结束后更新元组提示位
 *
 * 这在我们等待 XMAX 事务终止后被调用。如果事务中止，我们保证在退出时
 * XMAX_INVALID 提示位会被设置。如果事务提交，我们尽可能设置 XMAX_COMMITTED
 * 提示位——但要注意，如果事务是异步提交的，这可能还不能实现。
 *
 * 注意，如果事务仅是一个锁定者，即使它提交，我们也会设置 HEAP_XMAX_INVALID。
 *
 * 因此调用者应该只关注 XMAX_INVALID。
 *
 * 注意，这不允许用于其 xmax 是多事务（multixact）的元组。
 */
static void
UpdateXmaxHintBits(HeapTupleHeader tuple, Buffer buffer, TransactionId xid)
{
	Assert(TransactionIdEquals(HeapTupleHeaderGetRawXmax(tuple), xid));
	Assert(!(tuple->t_infomask & HEAP_XMAX_IS_MULTI));

	if (!(tuple->t_infomask & (HEAP_XMAX_COMMITTED | HEAP_XMAX_INVALID)))
	{
		if (!HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask) &&
			TransactionIdDidCommit(xid))
			HeapTupleSetHintBits(tuple, buffer, HEAP_XMAX_COMMITTED,
								 xid);
		else
			HeapTupleSetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
								 InvalidTransactionId);
	}
}


/*
 * GetBulkInsertState - 为批量插入准备状态对象
 */
BulkInsertState
GetBulkInsertState(void)
{
	BulkInsertState bistate;

	bistate = (BulkInsertState) palloc(sizeof(BulkInsertStateData));
	bistate->strategy = GetAccessStrategy(BAS_BULKWRITE);
	bistate->current_buf = InvalidBuffer;
	bistate->next_free = InvalidBlockNumber;
	bistate->last_free = InvalidBlockNumber;
	bistate->already_extended_by = 0;
	return bistate;
}

/*
 * FreeBulkInsertState - 完成批量插入后进行清理
 */
void
FreeBulkInsertState(BulkInsertState bistate)
{
	if (bistate->current_buf != InvalidBuffer)
		ReleaseBuffer(bistate->current_buf);
	FreeAccessStrategy(bistate->strategy);
	pfree(bistate);
}

/*
 * ReleaseBulkInsertStatePin - 释放当前在 bistate 中持有的缓冲区
 */
void
ReleaseBulkInsertStatePin(BulkInsertState bistate)
{
	if (bistate->current_buf != InvalidBuffer)
		ReleaseBuffer(bistate->current_buf);
	bistate->current_buf = InvalidBuffer;

	/*
	 * 尽管名称如此，我们也重置批量关系扩展状态。否则我们可能会由于
	 * 在一个分区的 ->next_free 中查找空闲空间而出错，即使 ->next_free
	 * 是在扩展另一个分区时设置的。即使不出错，查看另一个分区偏移量处的
	 * 现有块也会影响效率。
	 */
	bistate->next_free = InvalidBlockNumber;
	bistate->last_free = InvalidBlockNumber;
}


/*
 *	heap_insert		- 将元组插入堆中
 *
 * 新元组被盖上当前事务 ID 和指定命令 ID 的戳记。
 *
 * 关于大多数输入标志的注释，参见 table_tuple_insert，只是本函数直接接受元组
 * 而非槽（slot）。
 *
 * 所有 TABLE_INSERT_ 选项都有对应的 HEAP_INSERT_ 选项，此外还有
 * HEAP_INSERT_SPECULATIVE，用于实现 table_tuple_insert_speculative()。
 *
 * 返回时，*tup 的头部字段已更新以匹配存储的元组；特别是 tup->t_self
 * 接收元组实际存储的 TID。但注意，元组数据中任何字段的 TOAST 处理
 * 不会反映到 *tup 中。
 */
void
heap_insert(Relation relation, HeapTuple tup, CommandId cid,
			int options, BulkInsertState bistate)
{
	TransactionId xid = GetCurrentTransactionId();
	HeapTuple	heaptup;
	Buffer		buffer;
	Buffer		vmbuffer = InvalidBuffer;
	bool		all_visible_cleared = false;

	/* 快速简单的检查，确保元组与关系的行类型匹配。 */
	Assert(HeapTupleHeaderGetNatts(tup->t_data) <=
		   RelationGetNumberOfAttributes(relation));

	AssertHasSnapshotForToast(relation);

	/*
	 * 填充元组头部字段，必要时对元组进行 TOAST 处理。
	 *
	 * 注意：从此处往下，heaptup 是我们实际打算存储到关系中的数据；
	 * tup 是调用者原始的未 TOAST 数据。
	 */
	heaptup = heap_prepare_insert(relation, tup, xid, cid, options);

	/*
	 * 查找用于插入此元组的缓冲区。如果页面是全可见的，这也会
	 * pin 所需的可见性映射页面。
	 */
	buffer = RelationGetBufferForTuple(relation, heaptup->t_len,
									   InvalidBuffer, options, bistate,
									   &vmbuffer, NULL,
									   0);

	/*
	 * 我们即将执行实际的插入——但首先检查冲突，以避免可能需要回滚
	 * 我们刚刚完成的工作。
	 *
	 * 只要在本次检查和插入对扫描可见之间没有其他进程扫描页面的可能性，
	 * 这就无需重新检查就是安全的（即，从此点到元组插入可见之间持续持有
	 * 排他缓冲区内容锁）。
	 *
	 * 对于堆插入，我们只需要检查表级别的 SSI 锁。我们的新元组不可能
	 * 与现有元组锁冲突，堆页面锁只是元组锁的合并版本；它们不像索引页面锁
	 * 那样锁定"间隙"。因此我们在调用时不需要指定缓冲区，这使得检查更快。
	 */
	CheckForSerializableConflictIn(relation, NULL, InvalidBlockNumber);

	/* 从此时到更改被记录之前，不得有 EREPORT(ERROR) */
	START_CRIT_SECTION();

	RelationPutHeapTuple(relation, buffer, heaptup,
						 (options & HEAP_INSERT_SPECULATIVE) != 0);

	if (PageIsAllVisible(BufferGetPage(buffer)))
	{
		all_visible_cleared = true;
		PageClearAllVisible(BufferGetPage(buffer));
		visibilitymap_clear(relation,
							ItemPointerGetBlockNumber(&(heaptup->t_self)),
							vmbuffer, VISIBILITYMAP_VALID_BITS);
	}

	/*
	 * XXX 我们应该在此页面上设置 PageSetPrunable 吗？
	 *
	 * 插入事务可能最终中止，从而使此元组变成 DEAD，从而可供修剪。
	 * 虽然我们不想针对中止进行优化，但如果此页面中没有其他元组被
	 * UPDATE/DELETE，则中止的元组将永远不会被修剪，直到触发下一次 vacuum。
	 *
	 * 如果你确实在此添加了 PageSetPrunable，也请在 heap_xlog_insert 中添加。
	 */

	MarkBufferDirty(buffer);

	/* XLOG 相关 */
	if (RelationNeedsWAL(relation))
	{
		xl_heap_insert xlrec;
		xl_heap_header xlhdr;
		XLogRecPtr	recptr;
		Page		page = BufferGetPage(buffer);
		uint8		info = XLOG_HEAP_INSERT;
		int			bufflags = 0;

		/*
		 * 如果这是系统表，我们需要传输组合 CID 以便正确解码，因此也记录它。
		 */
		if (RelationIsAccessibleInLogicalDecoding(relation))
			log_heap_new_cid(relation, heaptup);

		/*
		 * 如果这是页面上的单个第一个元组，我们可以重新初始化页面而不恢复
		 * 整个页面。设置标志，并隐藏来自 XLogInsert 的缓冲区引用。
		 */
		if (ItemPointerGetOffsetNumber(&(heaptup->t_self)) == FirstOffsetNumber &&
			PageGetMaxOffsetNumber(page) == FirstOffsetNumber)
		{
			info |= XLOG_HEAP_INIT_PAGE;
			bufflags |= REGBUF_WILL_INIT;
		}

		xlrec.offnum = ItemPointerGetOffsetNumber(&heaptup->t_self);
		xlrec.flags = 0;
		if (all_visible_cleared)
			xlrec.flags |= XLH_INSERT_ALL_VISIBLE_CLEARED;
		if (options & HEAP_INSERT_SPECULATIVE)
			xlrec.flags |= XLH_INSERT_IS_SPECULATIVE;
		Assert(ItemPointerGetBlockNumber(&heaptup->t_self) == BufferGetBlockNumber(buffer));

		/*
		 * 对于逻辑解码，即使我们进行整页写入，也需要元组，因此即使我们
		 * 使用整页镜像，也要确保它被包含。（XXX 我们可以改为在 FPW 中存储指针）。
		 */
		if (RelationIsLogicallyLogged(relation) &&
			!(options & HEAP_INSERT_NO_LOGICAL))
		{
			xlrec.flags |= XLH_INSERT_CONTAINS_NEW_TUPLE;
			bufflags |= REGBUF_KEEP_DATA;

			if (IsToastRelation(relation))
				xlrec.flags |= XLH_INSERT_ON_TOAST_RELATION;
		}

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfHeapInsert);

		xlhdr.t_infomask2 = heaptup->t_data->t_infomask2;
		xlhdr.t_infomask = heaptup->t_data->t_infomask;
		xlhdr.t_hoff = heaptup->t_data->t_hoff;

		/*
		 * 注意我们将 xlhdr 标记为属于缓冲区；如果 XLogInsert 决定将整个页面
		 * 写入 xlog，我们不需要在 xlog 中存储 xl_heap_header。
		 */
		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD | bufflags);
		XLogRegisterBufData(0, &xlhdr, SizeOfHeapHeader);
		/* PG73FORMAT: 写入位图 [+ 填充] [+ oid] + 数据 */
		XLogRegisterBufData(0,
							(char *) heaptup->t_data + SizeofHeapTupleHeader,
							heaptup->t_len - SizeofHeapTupleHeader);

		/* 在行级别按来源过滤效率更高 */
		XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

		recptr = XLogInsert(RM_HEAP_ID, info);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buffer);
	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);

	/*
	 * 如果元组是可缓存的，标记它以在事务中止时从缓存中失效。
	 * 注意在释放缓冲区后执行此操作是 OK 的，因为 heaptup 数据结构
	 * 全在本地内存中，不在共享缓冲区中。
	 */
	CacheInvalidateHeapTuple(relation, heaptup, NULL);

	/* 注意：推测性插入也会被计数，即使稍后中止 */
	pgstat_count_heap_insert(relation, 1);

	/*
	 * 如果 heaptup 是私有副本，释放它。不要忘记也将 t_self 复制回调用者的镜像。
	 */
	if (heaptup != tup)
	{
		tup->t_self = heaptup->t_self;
		heap_freetuple(heaptup);
	}
}

/*
 * heap_insert() 的子例程。准备一个元组进行插入。设置元组头部字段，
 * 必要时对元组进行 TOAST 处理。如果被 TOAST 处理，返回元组的 TOAST 版本，
 * 否则返回原始元组。注意，在任何情况下，头部字段也会在原始元组中设置。
 */
static HeapTuple
heap_prepare_insert(Relation relation, HeapTuple tup, TransactionId xid,
					CommandId cid, int options)
{
	/*
	 * 为了支持并行插入，我们需要确保它们可以在 worker 中安全执行。
	 * 我们已有基础设施支持一般情况下的并行插入，但插入操作产生新 CommandId
	 * 的情况除外（例如，向具有外键列的表插入数据）。
	 */
	if (IsParallelWorker())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot insert tuples in a parallel worker")));

	tup->t_data->t_infomask &= ~(HEAP_XACT_MASK);
	tup->t_data->t_infomask2 &= ~(HEAP2_XACT_MASK);
	tup->t_data->t_infomask |= HEAP_XMAX_INVALID;
	HeapTupleHeaderSetXmin(tup->t_data, xid);
	if (options & HEAP_INSERT_FROZEN)
		HeapTupleHeaderSetXminFrozen(tup->t_data);

	HeapTupleHeaderSetCmin(tup->t_data, cid);
	HeapTupleHeaderSetXmax(tup->t_data, 0); /* 为了保持干净 */
	tup->t_tableOid = RelationGetRelid(relation);

	/*
	 * 如果新元组太大无法存储，或者包含来自其他关系的已 TOAST 的外部属性，
	 * 则调用 TOAST 处理器。
	 */
	if (relation->rd_rel->relkind != RELKIND_RELATION &&
		relation->rd_rel->relkind != RELKIND_MATVIEW)
	{
		/* TOAST 表条目不应被递归 TOAST */
		Assert(!HeapTupleHasExternal(tup));
		return tup;
	}
	else if (HeapTupleHasExternal(tup) || tup->t_len > TOAST_TUPLE_THRESHOLD)
		return heap_toast_insert_or_update(relation, tup, NULL, options);
	else
		return tup;
}

/*
 * heap_multi_insert() 的辅助函数，计算插入剩余堆元组需要多少个完整页面。
 * 用于确定需要将关系扩展多少。
 */
static int
heap_multi_insert_pages(HeapTuple *heaptuples, int done, int ntuples, Size saveFreeSpace)
{
	size_t		page_avail = BLCKSZ - SizeOfPageHeaderData - saveFreeSpace;
	int			npages = 1;

	for (int i = done; i < ntuples; i++)
	{
		size_t		tup_sz = sizeof(ItemIdData) + MAXALIGN(heaptuples[i]->t_len);

		if (page_avail < tup_sz)
		{
			npages++;
			page_avail = BLCKSZ - SizeOfPageHeaderData - saveFreeSpace;
		}
		page_avail -= tup_sz;
	}

	return npages;
}

/*
 *	heap_multi_insert	- 将多个元组插入堆中
 *
 * 类似于 heap_insert()，但一次操作插入多个元组。
 * 这比在循环中调用 heap_insert() 更快，因为当多个元组可以插入到
 * 同一个页面时，我们只需写一条覆盖所有元组的 WAL 记录，
 * 并且只需要锁定/解锁页面一次。
 *
 * 注意：这会向当前内存上下文中泄漏内存。如果这是问题，可以在调用前
 * 创建一个临时上下文。
 */
void
heap_multi_insert(Relation relation, TupleTableSlot **slots, int ntuples,
				  CommandId cid, int options, BulkInsertState bistate)
{
	TransactionId xid = GetCurrentTransactionId();
	HeapTuple  *heaptuples;
	int			i;
	int			ndone;
	PGAlignedBlock scratch;
	Page		page;
	Buffer		vmbuffer = InvalidBuffer;
	bool		needwal;
	Size		saveFreeSpace;
	bool		need_tuple_data = RelationIsLogicallyLogged(relation);
	bool		need_cids = RelationIsAccessibleInLogicalDecoding(relation);
	bool		starting_with_empty_page = false;
	int			npages = 0;
	int			npages_used = 0;

	/* 当前 heap_multi_insert() 不需要（因此不支持）此选项 */
	Assert(!(options & HEAP_INSERT_NO_LOGICAL));

	AssertHasSnapshotForToast(relation);

	needwal = RelationNeedsWAL(relation);
	saveFreeSpace = RelationGetTargetPageFreeSpace(relation,
												   HEAP_DEFAULT_FILLFACTOR);

	/* 对所有槽进行 TOAST 处理并设置头部数据 */
	heaptuples = palloc(ntuples * sizeof(HeapTuple));
	for (i = 0; i < ntuples; i++)
	{
		HeapTuple	tuple;

		tuple = ExecFetchSlotHeapTuple(slots[i], true, NULL);
		slots[i]->tts_tableOid = RelationGetRelid(relation);
		tuple->t_tableOid = slots[i]->tts_tableOid;
		heaptuples[i] = heap_prepare_insert(relation, tuple, xid, cid,
											options);
	}

	/*
	 * 我们即将执行实际的插入——但首先检查冲突，以最大限度地减少需要回滚
	 * 刚刚完成的工作的可能性。
	 *
	 * 这里的检查不能明确防止序列化异常；该检查必须至少在获取每个受影响缓冲区
	 * 的排他缓冲区内容锁之后进行，并且可以在所有插入反映到缓冲区并释放这些锁
	 * 之后再进行；否则存在竞争条件。由于在下面的循环中可能会锁定和解锁多个
	 * 缓冲区，并且在循环之前锁定所有缓冲区是不可行的，因此我们必须在最后
	 * 进行最终检查。
	 *
	 * 这里的检查可以省略而不失正确性；它的存在纯粹是为了优化。
	 *
	 * 对于堆插入，我们只需要检查表级别的 SSI 锁。我们的新元组不可能与现有
	 * 元组锁冲突，堆页面锁只是元组锁的合并版本；它们不像索引页面锁那样锁定
	 * "间隙"。因此我们在调用时不需要指定缓冲区，这使得检查更快。
	 */
	CheckForSerializableConflictIn(relation, NULL, InvalidBlockNumber);

	ndone = 0;
	while (ndone < ntuples)
	{
		Buffer		buffer;
		bool		all_visible_cleared = false;
		bool		all_frozen_set = false;
		int			nthispage;

		CHECK_FOR_INTERRUPTS();

		/*
		 * 计算在最坏情况下容纳待插入元组所需的页面数。如果需要，
		 * 这将用于在 RelationGetBufferForTuple() 中确定关系需要扩展多少。
		 * 如果我们是从头填充一页，可以只更新上次的计算，但如果我们从部分填充的
		 * 页面开始，则需要从头重新计算，因为潜在地需要的页面数量会由于
		 * 元组需要适配页面、页头等因素而变化。
		 */
		if (ndone == 0 || !starting_with_empty_page)
		{
			npages = heap_multi_insert_pages(heaptuples, ndone, ntuples,
											 saveFreeSpace);
			npages_used = 0;
		}
		else
			npages_used++;

		/*
		 * 找到至少能容纳下一个元组的缓冲区。如果页面是全可见的，
		 * 这也会 pin 所需的可见性映射页面。
		 *
		 * 如果 COPY FREEZE 向空页面插入元组，也要 pin 可见性映射页面。
		 * 参见下面的 all_frozen_set。
		 */
		buffer = RelationGetBufferForTuple(relation, heaptuples[ndone]->t_len,
										   InvalidBuffer, options, bistate,
										   &vmbuffer, NULL,
										   npages - npages_used);
		page = BufferGetPage(buffer);

		starting_with_empty_page = PageGetMaxOffsetNumber(page) == 0;

		if (starting_with_empty_page && (options & HEAP_INSERT_FROZEN))
			all_frozen_set = true;

		/* 从此处开始到变更被记录之前，不得 EREPORT(ERROR) */
		START_CRIT_SECTION();

		/*
		 * RelationGetBufferForTuple 已确保第一个元组能放得下。
		 * 将其放入页面，然后放入尽可能多的其他元组。
		 */
		RelationPutHeapTuple(relation, buffer, heaptuples[ndone], false);

		/*
		 * 对于逻辑解码，我们需要组合 CID 来正确解码系统表。
		 */
		if (needwal && need_cids)
			log_heap_new_cid(relation, heaptuples[ndone]);

		for (nthispage = 1; ndone + nthispage < ntuples; nthispage++)
		{
			HeapTuple	heaptup = heaptuples[ndone + nthispage];

			if (PageGetHeapFreeSpace(page) < MAXALIGN(heaptup->t_len) + saveFreeSpace)
				break;

			RelationPutHeapTuple(relation, buffer, heaptup, false);

			/*
			 * 对于逻辑解码，我们需要组合 CID 来正确解码系统表。
			 */
			if (needwal && need_cids)
				log_heap_new_cid(relation, heaptup);
		}

		/*
		 * 如果页面是全可见的，需要清除该标志，除非我们只是在上面添加
		 * 更多冻结行。
		 *
		 * 如果我们只是向之前为空的页面添加已冻结的行，则将其标记为全可见。
		 */
		if (PageIsAllVisible(page) && !(options & HEAP_INSERT_FROZEN))
		{
			all_visible_cleared = true;
			PageClearAllVisible(page);
			visibilitymap_clear(relation,
								BufferGetBlockNumber(buffer),
								vmbuffer, VISIBILITYMAP_VALID_BITS);
		}
		else if (all_frozen_set)
			PageSetAllVisible(page);

		/*
		 * XXX 我们应该在此页面上设置 PageSetPrunable 吗？参见 heap_insert()
		 */

		MarkBufferDirty(buffer);

		/* XLOG 相关 */
		if (needwal)
		{
			XLogRecPtr	recptr;
			xl_heap_multi_insert *xlrec;
			uint8		info = XLOG_HEAP2_MULTI_INSERT;
			char	   *tupledata;
			int			totaldatalen;
			char	   *scratchptr = scratch.data;
			bool		init;
			int			bufflags = 0;

			/*
			 * 如果页面之前是空的，我们可以重新初始化页面而不恢复整个页面。
			 */
			init = starting_with_empty_page;

			/* 从暂存区分配 xl_heap_multi_insert 结构 */
			xlrec = (xl_heap_multi_insert *) scratchptr;
			scratchptr += SizeOfHeapMultiInsert;

			/*
			 * 分配偏移数组。除非我们正在重新初始化页面，在这种情况下元组
			 * 按顺序存储，从 FirstOffsetNumber 开始，我们不需要显式存储偏移。
			 */
			if (!init)
				scratchptr += nthispage * sizeof(OffsetNumber);

			/* 暂存空间的其余部分用于元组数据 */
			tupledata = scratchptr;

			/* 检查互斥标志是否未同时设置 */
			Assert(!(all_visible_cleared && all_frozen_set));

			xlrec->flags = 0;
			if (all_visible_cleared)
				xlrec->flags = XLH_INSERT_ALL_VISIBLE_CLEARED;
			if (all_frozen_set)
				xlrec->flags = XLH_INSERT_ALL_FROZEN_SET;

			xlrec->ntuples = nthispage;

			/*
			 * 为每个元组写出 xl_multi_insert_tuple 和元组数据本身。
			 */
			for (i = 0; i < nthispage; i++)
			{
				HeapTuple	heaptup = heaptuples[ndone + i];
				xl_multi_insert_tuple *tuphdr;
				int			datalen;

				if (!init)
					xlrec->offsets[i] = ItemPointerGetOffsetNumber(&heaptup->t_self);
				/* xl_multi_insert_tuple 需要两字节对齐。 */
				tuphdr = (xl_multi_insert_tuple *) SHORTALIGN(scratchptr);
				scratchptr = ((char *) tuphdr) + SizeOfMultiInsertTuple;

				tuphdr->t_infomask2 = heaptup->t_data->t_infomask2;
				tuphdr->t_infomask = heaptup->t_data->t_infomask;
				tuphdr->t_hoff = heaptup->t_data->t_hoff;

				/* 写入位图 [+ 填充] [+ oid] + 数据 */
				datalen = heaptup->t_len - SizeofHeapTupleHeader;
				memcpy(scratchptr,
					   (char *) heaptup->t_data + SizeofHeapTupleHeader,
					   datalen);
				tuphdr->datalen = datalen;
				scratchptr += datalen;
			}
			totaldatalen = scratchptr - tupledata;
			Assert((scratchptr - scratch.data) < BLCKSZ);

			if (need_tuple_data)
				xlrec->flags |= XLH_INSERT_CONTAINS_NEW_TUPLE;

			/*
			 * 标志这是本次 heap_multi_insert() 调用发出的最后一条
			 * xl_heap_multi_insert 记录。逻辑解码需要此信息以知道何时清理
			 * 临时数据。
			 */
			if (ndone + nthispage == ntuples)
				xlrec->flags |= XLH_INSERT_LAST_IN_MULTI;

			if (init)
			{
				info |= XLOG_HEAP_INIT_PAGE;
				bufflags |= REGBUF_WILL_INIT;
			}

			/*
			 * 如果我们正在执行逻辑解码，即使我们获取页面的整页镜像，
			 * 也要包含新的元组数据。
			 */
			if (need_tuple_data)
				bufflags |= REGBUF_KEEP_DATA;

			XLogBeginInsert();
			XLogRegisterData(xlrec, tupledata - scratch.data);
			XLogRegisterBuffer(0, buffer, REGBUF_STANDARD | bufflags);

			XLogRegisterBufData(0, tupledata, totaldatalen);

			/* 在行级别按来源过滤效率更高 */
			XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

			recptr = XLogInsert(RM_HEAP2_ID, info);

			PageSetLSN(page, recptr);
		}

		END_CRIT_SECTION();

		/*
		 * 如果我们冻结了页面上的所有内容，更新可见性映射。
		 * 我们已经持有 vmbuffer 的 pin。
		 */
		if (all_frozen_set)
		{
			Assert(PageIsAllVisible(page));
			Assert(visibilitymap_pin_ok(BufferGetBlockNumber(buffer), vmbuffer));

			/*
			 * 此处使用 InvalidTransactionId 是可以的——这仅在指定
			 * HEAP_INSERT_FROZEN 时使用，它有意违反可见性规则。
			 */
			visibilitymap_set(relation, BufferGetBlockNumber(buffer), buffer,
							  InvalidXLogRecPtr, vmbuffer,
							  InvalidTransactionId,
							  VISIBILITYMAP_ALL_VISIBLE | VISIBILITYMAP_ALL_FROZEN);
		}

		UnlockReleaseBuffer(buffer);
		ndone += nthispage;

		/*
		 * 注意：只在插入所有元组后才释放 vmbuffer——我们很可能会插入到
		 * 后续可能使用相同 vm 页面的堆页面中。
		 */
	}

	/* 所有元组插入完毕，因此释放最后一个 vmbuffer。 */
	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);

	/*
	 * 实际插入已完成。再次检查冲突，以确保检测到针对这些插入的
	 * 所有读写冲突。如果没有这个最终检查，堆的顺序扫描可能在"之前"检查之后
	 * 锁定了表，错失了检测冲突的一次机会，然后在新元组存在之前扫描了表，
	 * 错失了另一次检测冲突的机会。
	 *
	 * 对于堆插入，我们只需要检查表级别的 SSI 锁。我们的新元组不可能与现有
	 * 元组锁冲突，堆页面锁只是元组锁的合并版本；它们不像索引页面锁那样锁定
	 * "间隙"。因此我们在调用时不需要指定缓冲区。
	 */
	CheckForSerializableConflictIn(relation, NULL, InvalidBlockNumber);

	/*
	 * 如果元组是可缓存的，标记它们以在事务中止时从缓存中失效。
	 * 注意在释放缓冲区后执行此操作是 OK 的，因为 heaptuples 数据结构
	 * 全在本地内存中，不在共享缓冲区中。
	 */
	if (IsCatalogRelation(relation))
	{
		for (i = 0; i < ntuples; i++)
			CacheInvalidateHeapTuple(relation, heaptuples[i], NULL);
	}

	/* 将 t_self 字段复制回调用者的槽中 */
	for (i = 0; i < ntuples; i++)
		slots[i]->tts_tid = heaptuples[i]->t_self;

	pgstat_count_heap_insert(relation, ntuples);
}

/*
 *	simple_heap_insert - 插入一个元组
 *
 * 目前，此函数与 heap_insert 的不同之处仅在于提供默认命令 ID 且不允许访问
 * 加速选项。
 *
 * 在大多数修改系统表的地方，应该使用此函数而不是直接使用 heap_insert。
 */
void
simple_heap_insert(Relation relation, HeapTuple tup)
{
	heap_insert(relation, tup, GetCurrentCommandId(true), 0, NULL);
}

/*
 * 给定 infomask/infomask2，计算必须保存在 xl_heap_delete、xl_heap_update、
 * xl_heap_lock、xl_heap_lock_updated WAL 记录的 "infobits" 字段中的位。
 *
 * 参见 fix_infomask_from_infobits。
 */
static uint8
compute_infobits(uint16 infomask, uint16 infomask2)
{
	return
		((infomask & HEAP_XMAX_IS_MULTI) != 0 ? XLHL_XMAX_IS_MULTI : 0) |
		((infomask & HEAP_XMAX_LOCK_ONLY) != 0 ? XLHL_XMAX_LOCK_ONLY : 0) |
		((infomask & HEAP_XMAX_EXCL_LOCK) != 0 ? XLHL_XMAX_EXCL_LOCK : 0) |
	/* 这里忽略 HEAP_XMAX_SHR_LOCK */
		((infomask & HEAP_XMAX_KEYSHR_LOCK) != 0 ? XLHL_XMAX_KEYSHR_LOCK : 0) |
		((infomask2 & HEAP_KEYS_UPDATED) != 0 ?
		 XLHL_KEYS_UPDATED : 0);
}

/*
 * 给定元组的同一 t_infomask 的两个版本，比较它们并返回元组 Xmax 的相关状态
 * 是否已更改。这在缓冲区锁被释放并重新获取后使用：我们希望确保元组状态
 * 与我们之前检查时的状态保持一致。
 *
 * 注意 Xmax 字段本身必须单独比较。
 */
static inline bool
xmax_infomask_changed(uint16 new_infomask, uint16 old_infomask)
{
	const uint16 interesting =
		HEAP_XMAX_IS_MULTI | HEAP_XMAX_LOCK_ONLY | HEAP_LOCK_MASK;

	if ((new_infomask & interesting) != (old_infomask & interesting))
		return true;

	return false;
}

/*
 *	heap_delete - 删除一个元组
 *
 * 关于参数的解释，参见 table_tuple_delete()，只是本函数直接接受元组
 * 而非槽（slot）。
 *
 * 在失败情况下，此函数用元组的 t_ctid、t_xmax（必要时解析可能的 MultiXact）
 * 和 t_cmax（后者仅适用于 TM_SelfModified，因为我们无法从另一个事务生成的
 * 组合 CID 中获取 cmax）填充 *tmfd。
 */
TM_Result
heap_delete(Relation relation, ItemPointer tid,
			CommandId cid, Snapshot crosscheck, bool wait,
			TM_FailureData *tmfd, bool changingPart)
{
	TM_Result	result;
	TransactionId xid = GetCurrentTransactionId();
	ItemId		lp;
	HeapTupleData tp;
	Page		page;
	BlockNumber block;
	Buffer		buffer;
	Buffer		vmbuffer = InvalidBuffer;
	TransactionId new_xmax;
	uint16		new_infomask,
				new_infomask2;
	bool		have_tuple_lock = false;
	bool		iscombo;
	bool		all_visible_cleared = false;
	HeapTuple	old_key_tuple = NULL;	/* 元组的副本标识 */
	bool		old_key_copied = false;

	Assert(ItemPointerIsValid(tid));

	AssertHasSnapshotForToast(relation);

	/*
	 * 并行操作期间禁止此操作，以免它分配组合 CID。
	 * 其他工作进程可能需要该组合 CID 进行可见性检查，而我们无法将其广播给它们。
	 */
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot delete tuples during a parallel operation")));

	block = ItemPointerGetBlockNumber(tid);
	buffer = ReadBuffer(relation, block);
	page = BufferGetPage(buffer);

	/*
	 * 在锁定缓冲区之前，如果看起来必要，先 pin 可见性映射页面。
	 * 由于我们尚未获取锁，其他人可能正在更改此页面，因此我们需要在获取锁后
	 * 重新检查。
	 */
	if (PageIsAllVisible(page))
		visibilitymap_pin(relation, block, &vmbuffer);

	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
	Assert(ItemIdIsNormal(lp));

	tp.t_tableOid = RelationGetRelid(relation);
	tp.t_data = (HeapTupleHeader) PageGetItem(page, lp);
	tp.t_len = ItemIdGetLength(lp);
	tp.t_self = *tid;

l1:

	/*
	 * 如果我们没有 pin 可见性映射页面，而页面在我们锁定缓冲区期间变成了全可见，
	 * 我们将不得不解锁并重新锁定，以避免在 I/O 期间持有缓冲区锁。
	 * 这有点遗憾，但希望不会经常发生。
	 */
	if (vmbuffer == InvalidBuffer && PageIsAllVisible(page))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		visibilitymap_pin(relation, block, &vmbuffer);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	}

	result = HeapTupleSatisfiesUpdate(&tp, cid, buffer);

	if (result == TM_Invisible)
	{
		UnlockReleaseBuffer(buffer);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("attempted to delete invisible tuple")));
	}
	else if (result == TM_BeingModified && wait)
	{
		TransactionId xwait;
		uint16		infomask;

		/* 必须在解锁缓冲区之前复制状态数据 */
		xwait = HeapTupleHeaderGetRawXmax(tp.t_data);
		infomask = tp.t_data->t_infomask;

		/*
		 * 等待并发事务结束——除非是单个锁持有者且是我们自己的事务。
		 * 注意我们不关心锁持有者使用哪种锁模式，因为我们需要最强的模式。
		 *
		 * 在等待之前，我们需要获取元组锁以确立我们对元组的优先级
		 * （参见 heap_lock_tuple）。LockTuple 会在我们排到元组的下一位时
		 * 释放我们。
		 *
		 * 如果我们在下面被迫"重新开始"，我们保留元组锁；
		 * 这样安排我们可以在重新检查元组状态时保持在队列头部。
		 */
		if (infomask & HEAP_XMAX_IS_MULTI)
		{
			bool		current_is_member = false;

			if (DoesMultiXactIdConflict((MultiXactId) xwait, infomask,
										LockTupleExclusive, &current_is_member))
			{
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

				/*
				 * 如果需要，获取锁（但当我们要请求锁且已经持有时跳过；
				 * 避免死锁）。
				 */
				if (!current_is_member)
					heap_acquire_tuplock(relation, &(tp.t_self), LockTupleExclusive,
										 LockWaitBlock, &have_tuple_lock);

				/* 等待 multixact */
				MultiXactIdWait((MultiXactId) xwait, MultiXactStatusUpdate, infomask,
								relation, &(tp.t_self), XLTW_Delete,
								NULL);
				LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

				/*
				 * 如果 xwait 只是锁定了元组，那么在我们到达此点之前，
				 * 其他事务可能会更新此元组。检查 xmax 是否更改，如果更改则重新开始。
				 *
				 * 如果我们没有 pin VM 页面，且页面已变为全可见，也必须重新开始。
				 */
				if ((vmbuffer == InvalidBuffer && PageIsAllVisible(page)) ||
					xmax_infomask_changed(tp.t_data->t_infomask, infomask) ||
					!TransactionIdEquals(HeapTupleHeaderGetRawXmax(tp.t_data),
										 xwait))
					goto l1;
			}

			/*
			 * 你可能认为 multixact 在此处必然已完成，但并非如此：
			 * 它可能有存活成员，即我们自己的事务或此后端的其他子事务。
			 * 然而，在任何一种情况下我们删除元组都是合法的（后一种情况本质上
			 * 是将我们之前的共享锁升级为排他锁）。我们不费心更改磁盘上的提示位，
			 * 因为我们即将完全覆盖 xmax。
			 */
		}
		else if (!TransactionIdIsCurrentTransactionId(xwait))
		{
			/*
			 * 等待常规事务结束；但首先获取元组锁。
			 */
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			heap_acquire_tuplock(relation, &(tp.t_self), LockTupleExclusive,
								 LockWaitBlock, &have_tuple_lock);
			XactLockTableWait(xwait, relation, &(tp.t_self), XLTW_Delete);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

			/*
			 * xwait 已完成，但如果 xwait 只是锁定了元组，那么在我们到达此点之前，
			 * 其他事务可能会更新此元组。检查 xmax 是否更改，如果更改则重新开始。
			 *
			 * 如果我们没有 pin VM 页面，且页面已变为全可见，也必须重新开始。
			 */
			if ((vmbuffer == InvalidBuffer && PageIsAllVisible(page)) ||
				xmax_infomask_changed(tp.t_data->t_infomask, infomask) ||
				!TransactionIdEquals(HeapTupleHeaderGetRawXmax(tp.t_data),
									 xwait))
				goto l1;

			/* 否则检查它是提交了还是中止了 */
			UpdateXmaxHintBits(tp.t_data, buffer, xwait);
		}

		/*
		 * 如果先前的 xmax 已中止，或者它提交了但仅锁定了元组而未更新它，
		 * 我们可以覆盖。
		 */
		if ((tp.t_data->t_infomask & HEAP_XMAX_INVALID) ||
			HEAP_XMAX_IS_LOCKED_ONLY(tp.t_data->t_infomask) ||
			HeapTupleHeaderIsOnlyLocked(tp.t_data))
			result = TM_Ok;
		else if (!ItemPointerEquals(&tp.t_self, &tp.t_data->t_ctid))
			result = TM_Updated;
		else
			result = TM_Deleted;
	}

	/* 对 HeapTupleSatisfiesUpdate() 的结果和上述逻辑进行正确性检查 */
	if (result != TM_Ok)
	{
		Assert(result == TM_SelfModified ||
			   result == TM_Updated ||
			   result == TM_Deleted ||
			   result == TM_BeingModified);
		Assert(!(tp.t_data->t_infomask & HEAP_XMAX_INVALID));
		Assert(result != TM_Updated ||
			   !ItemPointerEquals(&tp.t_self, &tp.t_data->t_ctid));
	}

	if (crosscheck != InvalidSnapshot && result == TM_Ok)
	{
		/* 对事务快照模式的 RI 更新执行额外检查 */
		if (!HeapTupleSatisfiesVisibility(&tp, crosscheck, buffer))
			result = TM_Updated;
	}

	if (result != TM_Ok)
	{
		tmfd->ctid = tp.t_data->t_ctid;
		tmfd->xmax = HeapTupleHeaderGetUpdateXid(tp.t_data);
		if (result == TM_SelfModified)
			tmfd->cmax = HeapTupleHeaderGetCmax(tp.t_data);
		else
			tmfd->cmax = InvalidCommandId;
		UnlockReleaseBuffer(buffer);
		if (have_tuple_lock)
			UnlockTupleTuplock(relation, &(tp.t_self), LockTupleExclusive);
		if (vmbuffer != InvalidBuffer)
			ReleaseBuffer(vmbuffer);
		return result;
	}

	/*
	 * 我们即将执行实际的删除——但首先检查冲突，以避免可能需要回滚
	 * 刚刚完成的工作。
	 *
	 * 只要在本次检查和删除对扫描可见之间没有其他进程扫描页面的可能性，
	 * 这就无需重新检查就是安全的（即，从此点到元组删除可见之间持续持有
	 * 排他缓冲区内容锁）。
	 */
	CheckForSerializableConflictIn(relation, tid, BufferGetBlockNumber(buffer));

	/* 如有必要，将 cid 替换为组合 CID */
	HeapTupleHeaderAdjustCmax(tp.t_data, &cid, &iscombo);

	/*
	 * 在进入临界区之前计算副本标识元组，以避免在内存分配失败时触发 PANIC。
	 */
	old_key_tuple = ExtractReplicaIdentity(relation, &tp, true, &old_key_copied);

	/*
	 * 如果这是当前事务中第一个可能涉及 multixact 的操作，设置我的每后端
	 * OldestMemberMXactId 值。我们可以确定此事务永远不会成为比这更老的
	 * MultiXactId 的成员。（即使我们最终只使用自己的 TransactionId，
	 * 也必须执行此操作，因为其他后端可能立即将我们的 XID 纳入 MultiXact。）
	 */
	MultiXactIdSetOldestMember();

	compute_new_xmax_infomask(HeapTupleHeaderGetRawXmax(tp.t_data),
							  tp.t_data->t_infomask, tp.t_data->t_infomask2,
							  xid, LockTupleExclusive, true,
							  &new_xmax, &new_infomask, &new_infomask2);

	START_CRIT_SECTION();

	/*
	 * 如果此事务提交，元组迟早会变成 DEAD。设置标志，当我们的 xid
	 * 降到 OldestXmin 水平线以下时，此页面成为修剪的候选。
	 * 如果事务最终中止，后续的页面修剪将是无操作，提示位会被清除。
	 */
	PageSetPrunable(page, xid);

	if (PageIsAllVisible(page))
	{
		all_visible_cleared = true;
		PageClearAllVisible(page);
		visibilitymap_clear(relation, BufferGetBlockNumber(buffer),
							vmbuffer, VISIBILITYMAP_VALID_BITS);
	}

	/* 存储删除元组的事务的事务信息 */
	tp.t_data->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
	tp.t_data->t_infomask2 &= ~HEAP_KEYS_UPDATED;
	tp.t_data->t_infomask |= new_infomask;
	tp.t_data->t_infomask2 |= new_infomask2;
	HeapTupleHeaderClearHotUpdated(tp.t_data);
	HeapTupleHeaderSetXmax(tp.t_data, new_xmax);
	HeapTupleHeaderSetCmax(tp.t_data, cid, iscombo);
	/* 确保 t_ctid 中没有前向链链接 */
	tp.t_data->t_ctid = tp.t_self;

	/* 标记这实际上是一个移动到另一个分区的操作 */
	if (changingPart)
		HeapTupleHeaderSetMovedPartitions(tp.t_data);

	MarkBufferDirty(buffer);

	/*
	 * XLOG 相关内容
	 *
	 * 注意：heap_abort_speculative() 使用相同的 xlog 记录和重放例程。
	 */
	if (RelationNeedsWAL(relation))
	{
		xl_heap_delete xlrec;
		xl_heap_header xlhdr;
		XLogRecPtr	recptr;

		/*
		 * 对于逻辑解码，我们需要组合 CID 来正确解码系统表
		 */
		if (RelationIsAccessibleInLogicalDecoding(relation))
			log_heap_new_cid(relation, &tp);

		xlrec.flags = 0;
		if (all_visible_cleared)
			xlrec.flags |= XLH_DELETE_ALL_VISIBLE_CLEARED;
		if (changingPart)
			xlrec.flags |= XLH_DELETE_IS_PARTITION_MOVE;
		xlrec.infobits_set = compute_infobits(tp.t_data->t_infomask,
											  tp.t_data->t_infomask2);
		xlrec.offnum = ItemPointerGetOffsetNumber(&tp.t_self);
		xlrec.xmax = new_xmax;

		if (old_key_tuple != NULL)
		{
			if (relation->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
				xlrec.flags |= XLH_DELETE_CONTAINS_OLD_TUPLE;
			else
				xlrec.flags |= XLH_DELETE_CONTAINS_OLD_KEY;
		}

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfHeapDelete);

		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

		/*
		 * 记录已删除元组的副本标识（如果存在的话）
		 */
		if (old_key_tuple != NULL)
		{
			xlhdr.t_infomask2 = old_key_tuple->t_data->t_infomask2;
			xlhdr.t_infomask = old_key_tuple->t_data->t_infomask;
			xlhdr.t_hoff = old_key_tuple->t_data->t_hoff;

			XLogRegisterData(&xlhdr, SizeOfHeapHeader);
			XLogRegisterData((char *) old_key_tuple->t_data
							 + SizeofHeapTupleHeader,
							 old_key_tuple->t_len
							 - SizeofHeapTupleHeader);
		}

		/* 在行级别按来源过滤效率更高 */
		XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

		recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_DELETE);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);

	/*
	 * 如果元组有已 TOAST 的行外属性，我们也需要删除这些项。
	 * 我们必须在释放缓冲区之前执行此操作，因为我们需要查看元组的内容，
	 * 但可以先释放缓冲区的内容锁。
	 */
	if (relation->rd_rel->relkind != RELKIND_RELATION &&
		relation->rd_rel->relkind != RELKIND_MATVIEW)
	{
		/* TOAST 表条目不应被递归 TOAST */
		Assert(!HeapTupleHasExternal(&tp));
	}
	else if (HeapTupleHasExternal(&tp))
		heap_toast_delete(relation, &tp, false);

	/*
	 * 标记元组以在下一个命令边界处从系统缓存中失效。
	 * 我们必须在释放缓冲区之前执行此操作，因为我们需要查看元组的内容。
	 */
	CacheInvalidateHeapTuple(relation, &tp, NULL);

	/* 现在我们可以释放缓冲区了 */
	ReleaseBuffer(buffer);

	/*
	 * Release the lmgr tuple lock, if we had it.
	 */
	if (have_tuple_lock)
		UnlockTupleTuplock(relation, &(tp.t_self), LockTupleExclusive);

	pgstat_count_heap_delete(relation);

	if (old_key_tuple != NULL && old_key_copied)
		heap_freetuple(old_key_tuple);

	return TM_Ok;
}

/*
 *	simple_heap_delete - 删除一个元组
 *
 * 当不预期目标元组会有并发更新时（例如，因为我们对元组关联的关系持有锁），
 * 可以使用此函数删除元组。任何失败都会通过 ereport() 报告。
 */
void
simple_heap_delete(Relation relation, ItemPointer tid)
{
	TM_Result	result;
	TM_FailureData tmfd;

	result = heap_delete(relation, tid,
						 GetCurrentCommandId(true), InvalidSnapshot,
						 true /* 等待提交 */ ,
						 &tmfd, false /* 是否正在变更分区 */ );
	switch (result)
	{
		case TM_SelfModified:
			/* 元组已在当前命令中更新？ */
			elog(ERROR, "tuple already updated by self");
			break;

		case TM_Ok:
			/* 成功完成 */
			break;

		case TM_Updated:
			elog(ERROR, "tuple concurrently updated");
			break;

		case TM_Deleted:
			elog(ERROR, "tuple concurrently deleted");
			break;

		default:
			elog(ERROR, "unrecognized heap_delete status: %u", result);
			break;
	}
}

/*
 *	heap_update - 替换一个元组
 *
 * 关于参数的解释，参见 table_tuple_update()，只是本函数直接接受元组
 * 而非槽（slot）。
 *
 * 在失败情况下，此函数用元组的 t_ctid、t_xmax（必要时解析可能的 MultiXact）
 * 和 t_cmax（后者仅适用于 TM_SelfModified，因为我们无法从另一个事务生成的
 * 组合 CID 中获取 cmax）填充 *tmfd。
 */
TM_Result
heap_update(Relation relation, ItemPointer otid, HeapTuple newtup,
			CommandId cid, Snapshot crosscheck, bool wait,
			TM_FailureData *tmfd, LockTupleMode *lockmode,
			TU_UpdateIndexes *update_indexes)
{
	TM_Result	result;
	TransactionId xid = GetCurrentTransactionId();
	Bitmapset  *hot_attrs;
	Bitmapset  *sum_attrs;
	Bitmapset  *key_attrs;
	Bitmapset  *id_attrs;
	Bitmapset  *interesting_attrs;
	Bitmapset  *modified_attrs;
	ItemId		lp;
	HeapTupleData oldtup;
	HeapTuple	heaptup;
	HeapTuple	old_key_tuple = NULL;
	bool		old_key_copied = false;
	Page		page;
	BlockNumber block;
	MultiXactStatus mxact_status;
	Buffer		buffer,
				newbuf,
				vmbuffer = InvalidBuffer,
				vmbuffer_new = InvalidBuffer;
	bool		need_toast;
	Size		newtupsize,
				pagefree;
	bool		have_tuple_lock = false;
	bool		iscombo;
	bool		use_hot_update = false;
	bool		summarized_update = false;
	bool		key_intact;
	bool		all_visible_cleared = false;
	bool		all_visible_cleared_new = false;
	bool		checked_lockers;
	bool		locker_remains;
	bool		id_has_external = false;
	TransactionId xmax_new_tuple,
				xmax_old_tuple;
	uint16		infomask_old_tuple,
				infomask2_old_tuple,
				infomask_new_tuple,
				infomask2_new_tuple;

	Assert(ItemPointerIsValid(otid));

	/* 快速简单的检查，确保元组与关系的行类型匹配。 */
	Assert(HeapTupleHeaderGetNatts(newtup->t_data) <=
		   RelationGetNumberOfAttributes(relation));

	AssertHasSnapshotForToast(relation);

	/*
	 * 并行操作期间禁止此操作，以免它分配组合 CID。
	 * 其他工作进程可能需要该组合 CID 进行可见性检查，而我们无法将其广播给它们。
	 */
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot update tuples during a parallel operation")));

#ifdef USE_ASSERT_CHECKING
	check_lock_if_inplace_updateable_rel(relation, otid, newtup);
#endif

	/*
	 * 获取各种操作需要检查的属性列表。
	 *
	 * 对于 HOT 考虑而言，如果我们更新失败或必须将新元组放到不同的页面上，
	 * 这些工作就白费了。但我们必须在获取缓冲区锁之前计算此列表 ---
	 * 在最坏的情况下，如果我们正在对某个相关系统目录进行更新，
	 * 稍后再尝试获取该列表可能会导致死锁。无论如何，relcache 会缓存数据，
	 * 所以这通常相当廉价。
	 *
	 * 我们还需要副本标识（replica identity）使用的列以及被认为是表中行的
	 * "键"的列。
	 *
	 * 注意我们获取的是每个位图的副本，因此无需担心中途发生 relcache 刷新。
	 */
	hot_attrs = RelationGetIndexAttrBitmap(relation,
										   INDEX_ATTR_BITMAP_HOT_BLOCKING);
	sum_attrs = RelationGetIndexAttrBitmap(relation,
										   INDEX_ATTR_BITMAP_SUMMARIZED);
	key_attrs = RelationGetIndexAttrBitmap(relation, INDEX_ATTR_BITMAP_KEY);
	id_attrs = RelationGetIndexAttrBitmap(relation,
										  INDEX_ATTR_BITMAP_IDENTITY_KEY);
	interesting_attrs = NULL;
	interesting_attrs = bms_add_members(interesting_attrs, hot_attrs);
	interesting_attrs = bms_add_members(interesting_attrs, sum_attrs);
	interesting_attrs = bms_add_members(interesting_attrs, key_attrs);
	interesting_attrs = bms_add_members(interesting_attrs, id_attrs);

	block = ItemPointerGetBlockNumber(otid);
	INJECTION_POINT("heap_update-before-pin", NULL);
	buffer = ReadBuffer(relation, block);
	page = BufferGetPage(buffer);

	/*
	 * 在锁定缓冲区之前，如果看起来有必要，先 pin 可见性映射页。由于我们尚未
	 * 获取锁，其他人可能正在修改此页，因此我们需要在获取锁后重新检查。
	 */
	if (PageIsAllVisible(page))
		visibilitymap_pin(relation, block, &vmbuffer);

	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	lp = PageGetItemId(page, ItemPointerGetOffsetNumber(otid));

	/*
	 * 通常，缓冲区 pin 和/或快照会阻止 otid 被裁剪，从而确保此处看到 LP_NORMAL。
	 * 当 otid 来自 syscache 时，可能既无 pin 也无快照，因此可能看到其他 LP_ 状态，
	 * 每种状态都表示并发裁剪。
	 *
	 * 最准确的做法是返回 TM_Updated 失败。但是，与其他 TM_Updated 场景不同，
	 * 在 LP_UNUSED 和 LP_DEAD 情况下我们不知道后继 ctid。虽然 TM_Updated
	 * 和 TM_Deleted 之间的区别对 SQL 语句 UPDATE 和 MERGE 确实有影响，但这些
	 * SQL 语句持有快照，可确保 LP_NORMAL。因此，选择 TM_Updated 还是 TM_Deleted
	 * 仅影响错误消息的措辞。我们选择 TM_Deleted，原因有二。首先，它避免了使
	 * tmfd->ctid 有效的条件说明变得复杂。其次，它在错误日志中提供了证据表明
	 * 我们走了这个分支。
	 *
	 * 既然可能在 otid 处看到 LP_UNUSED，那么也可能看到替换了 LP_UNUSED 的
	 * 元组的 LP_NORMAL。如果该元组属于不相关的行，我们将报"重复键值违反唯一约束"
	 * 失败。XXX 如果 otid 是 newtup 行的活跃更新版本，我们将丢弃调用者从
	 * syscache 获取的该目录行版本之后、该行最新版本所发生的更改。参见
	 * syscache-update-pruned.spec。
	 */
	if (!ItemIdIsNormal(lp))
	{
		Assert(RelationSupportsSysCache(RelationGetRelid(relation)));

		UnlockReleaseBuffer(buffer);
		Assert(!have_tuple_lock);
		if (vmbuffer != InvalidBuffer)
			ReleaseBuffer(vmbuffer);
		tmfd->ctid = *otid;
		tmfd->xmax = InvalidTransactionId;
		tmfd->cmax = InvalidCommandId;
		*update_indexes = TU_None;

		bms_free(hot_attrs);
		bms_free(sum_attrs);
		bms_free(key_attrs);
		bms_free(id_attrs);
		/* modified_attrs 尚未初始化 */
		bms_free(interesting_attrs);
		return TM_Deleted;
	}

	/*
	 * 填充 oldtup 中足够的数据，以便 HeapDetermineColumnsInfo 正常工作。
	 */
	oldtup.t_tableOid = RelationGetRelid(relation);
	oldtup.t_data = (HeapTupleHeader) PageGetItem(page, lp);
	oldtup.t_len = ItemIdGetLength(lp);
	oldtup.t_self = *otid;

	/* 新元组已就绪，仅差此项： */
	newtup->t_tableOid = RelationGetRelid(relation);

	/*
	 * 确定更新修改了哪些列。此外，识别旧元组中未修改的副本标识键属性是否
	 * 有外部存储的情况。这是必需的，因为对于这类属性，扁平化后的值不会
	 * 作为新元组的一部分记录 WAL 日志，因此我们必须将其包含在 old_key_tuple
	 * 中。参见 ExtractReplicaIdentity。
	 */
	modified_attrs = HeapDetermineColumnsInfo(relation, interesting_attrs,
											  id_attrs, &oldtup,
											  newtup, &id_has_external);

	/*
	 * 如果我们没有更新任何"键"列，可以使用弱锁类型。这在我们与外键检查
	 * 同时运行时可以提高并发性。
	 *
	 * 注意，如果某列在执行更新期间被 detoast，但最终值相同，则此测试将失败，
	 * 我们将使用更強的锁。这是可以接受的；需要优化的重点是那些不修改键列的
	 * 更新，而不是那些碰巧键值相同的更新。
	 */
	if (!bms_overlap(modified_attrs, key_attrs))
	{
		*lockmode = LockTupleNoKeyExclusive;
		mxact_status = MultiXactStatusNoKeyUpdate;
		key_intact = true;

		/*
		 * 如果这是当前事务中第一个可能涉及 multixact 的操作，设置每个后台进程
		 * 的 OldestMemberMXactId。我们可以确信该事务永远不会成为任何比它更早的
		 * MultiXactId 的成员。（即使我们最终只使用自己的 TransactionId，也必须
		 * 这样做，因为其他后台进程可能随后立即将我们的 XID 合并到 MultiXact 中。）
		 */
		MultiXactIdSetOldestMember();
	}
	else
	{
		*lockmode = LockTupleExclusive;
		mxact_status = MultiXactStatusUpdate;
		key_intact = false;
	}

	/*
	 * 注意：从此处开始，请使用 oldtup 而非 otid 来引用旧元组。otid 很可能指向
	 * newtup->t_self，而我们将用新元组的位置覆盖它，因此如果继续使用 otid，
	 * 极有可能导致混淆。
	 */

l2:
	checked_lockers = false;
	locker_remains = false;
	result = HeapTupleSatisfiesUpdate(&oldtup, cid, buffer);

	/* 关于"no wait"情况，参见下文 */
	Assert(result != TM_BeingModified || wait);

	if (result == TM_Invisible)
	{
		UnlockReleaseBuffer(buffer);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("attempted to update invisible tuple")));
	}
	else if (result == TM_BeingModified && wait)
	{
		TransactionId xwait;
		uint16		infomask;
		bool		can_continue = false;

		/*
		 * XXX 注意，我们没有在这里考虑"no wait"的情况。目前这不是问题，因为
		 * 没有调用者使用该情况，但如果将来引入这样的调用者，应该修复此处。
		 * 以前也不是问题，因为此代码总是等待，但现在某些元组锁不与我们使用的
		 * 锁模式冲突，因此这种情况可能需要特殊处理。
		 *
		 * 这可能导致直接调用 heap_update 的第三方代码失败。
		 */

		/* 在解锁缓冲区之前，必须先复制状态数据 */
		xwait = HeapTupleHeaderGetRawXmax(oldtup.t_data);
		infomask = oldtup.t_data->t_infomask;

		/*
		 * 现在我们必须处理现有的锁定者。如果是多重事务（multi），则等待它；
		 * 我们可能会在其完全消失之前被唤醒（在某些情况下甚至根本不等待）；
		 * 我们需要保留它作为锁定者，除非它已完全消失。
		 *
		 * 如果不是多重事务，我们在实际等待之前需要检查等待条件。如果更新操作
		 * 与锁不冲突，我们就直接继续而不等待（但要确保保留锁）。
		 *
		 * 在等待之前，我们需要获取元组锁以建立我们对元组的优先级
		 * （参见 heap_lock_tuple）。LockTuple 会在我们成为元组的下一个等待者时
		 * 释放我们。注意，在我们确定要等待之前，不能获取元组锁；否则我们可能会
		 * 与持有元组锁并等待我们的其他事务发生竞争条件。
		 *
		 * 如果我们在下面被迫"重新开始"，我们会保留元组锁；这确保我们在重新检查
		 * 元组状态时始终排在队列的最前面。
		 */
		if (infomask & HEAP_XMAX_IS_MULTI)
		{
			TransactionId update_xact;
			int			remain;
			bool		current_is_member = false;

			if (DoesMultiXactIdConflict((MultiXactId) xwait, infomask,
										*lockmode, &current_is_member))
			{
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

				/*
				 * Acquire the lock, if necessary (but skip it when we're
				 * requesting a lock and already have one; avoids deadlock).
				 */
				if (!current_is_member)
					heap_acquire_tuplock(relation, &(oldtup.t_self), *lockmode,
										 LockWaitBlock, &have_tuple_lock);

				/* 等待 multixact */
				MultiXactIdWait((MultiXactId) xwait, mxact_status, infomask,
								relation, &oldtup.t_self, XLTW_Update,
								&remain);
				checked_lockers = true;
				locker_remains = remain != 0;
				LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

				/*
				 * 如果 xwait 刚锁定了元组，那么在我们到达此处之前，其他事务
				 * 可能已更新此元组。检查 xmax 是否变化，如果有变化则重新开始。
				 */
				if (xmax_infomask_changed(oldtup.t_data->t_infomask,
										  infomask) ||
					!TransactionIdEquals(HeapTupleHeaderGetRawXmax(oldtup.t_data),
										 xwait))
					goto l2;
			}

			/*
			 * 注意，此时 multixact 可能尚未完成。它可能有存活成员：我们自己
			 * 的事务或此后台进程的其他子事务，以及任何使用 LockTupleKeyShare
			 * 锁定元组的并发事务（如果我们只获得了 LockTupleNoKeyExclusive）。
			 * 如果是这种情况，我们必须小心地用 Xmax 中的存活成员标记已更新的元组。
			 *
			 * 注意，MultiXact 中可能还有另一个更新操作。此时，我们需要检查它
			 * 是已提交还是已中止。如果已中止，我们可以安全地再次更新；否则存在
			 * 更新冲突，我们必须在下面返回 TM_Updated 或 TM_Deleted。
			 *
			 * 在 LockTupleExclusive 情况下，我们仍然需要保留存活成员：这些包括
			 * 在此之前持有的元组锁，保留它们对于子事务中止时很重要。
			 */
			if (!HEAP_XMAX_IS_LOCKED_ONLY(oldtup.t_data->t_infomask))
				update_xact = HeapTupleGetUpdateXid(oldtup.t_data);
			else
				update_xact = InvalidTransactionId;

			/*
			 * MultiXact 中没有 UPDATE；或者它已中止。无需在此调用
			 * TransactionIdIsInProgress()，因为我们上面已经调用了 MultiXactIdWait()。
			 */
			if (!TransactionIdIsValid(update_xact) ||
				TransactionIdDidAbort(update_xact))
				can_continue = true;
		}
		else if (TransactionIdIsCurrentTransactionId(xwait))
		{
			/*
			 * 唯一的锁定者是我们自己；这里可以不必获取元组锁，但必须保留我们
			 * 的锁定信息。
			 */
			checked_lockers = true;
			locker_remains = true;
			can_continue = true;
		}
		else if (HEAP_XMAX_IS_KEYSHR_LOCKED(infomask) && key_intact)
		{
			/*
			 * 如果仅是一个键共享（key-share）锁定者，并且我们没有修改键列，
			 * 则无需等待其结束；但需要将其保留为锁定者。
			 */
			checked_lockers = true;
			locker_remains = true;
			can_continue = true;
		}
		else
		{
			/*
			 * 等待常规事务结束；但首先，获取元组锁。
			 */
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			heap_acquire_tuplock(relation, &(oldtup.t_self), *lockmode,
								 LockWaitBlock, &have_tuple_lock);
			XactLockTableWait(xwait, relation, &oldtup.t_self,
							  XLTW_Update);
			checked_lockers = true;
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

			/*
			 * xwait 已完成，但如果 xwait 刚锁定了元组，那么在我们到达此处之前，
			 * 其他事务可能已更新此元组。检查 xmax 是否变化，如果有变化则重新开始。
			 */
			if (xmax_infomask_changed(oldtup.t_data->t_infomask, infomask) ||
				!TransactionIdEquals(xwait,
									 HeapTupleHeaderGetRawXmax(oldtup.t_data)))
				goto l2;

			/* 否则检查它是已提交还是已中止 */
			UpdateXmaxHintBits(oldtup.t_data, buffer, xwait);
			if (oldtup.t_data->t_infomask & HEAP_XMAX_INVALID)
				can_continue = true;
		}

		if (can_continue)
			result = TM_Ok;
		else if (!ItemPointerEquals(&oldtup.t_self, &oldtup.t_data->t_ctid))
			result = TM_Updated;
		else
			result = TM_Deleted;
	}

	/* 对 HeapTupleSatisfiesUpdate() 的结果和上述逻辑进行健全性检查 */
	if (result != TM_Ok)
	{
		Assert(result == TM_SelfModified ||
			   result == TM_Updated ||
			   result == TM_Deleted ||
			   result == TM_BeingModified);
		Assert(!(oldtup.t_data->t_infomask & HEAP_XMAX_INVALID));
		Assert(result != TM_Updated ||
			   !ItemPointerEquals(&oldtup.t_self, &oldtup.t_data->t_ctid));
	}

	if (crosscheck != InvalidSnapshot && result == TM_Ok)
	{
		/* 为事务快照模式的 RI 更新执行额外检查 */
		if (!HeapTupleSatisfiesVisibility(&oldtup, crosscheck, buffer))
			result = TM_Updated;
	}

	if (result != TM_Ok)
	{
		tmfd->ctid = oldtup.t_data->t_ctid;
		tmfd->xmax = HeapTupleHeaderGetUpdateXid(oldtup.t_data);
		if (result == TM_SelfModified)
			tmfd->cmax = HeapTupleHeaderGetCmax(oldtup.t_data);
		else
			tmfd->cmax = InvalidCommandId;
		UnlockReleaseBuffer(buffer);
		if (have_tuple_lock)
			UnlockTupleTuplock(relation, &(oldtup.t_self), *lockmode);
		if (vmbuffer != InvalidBuffer)
			ReleaseBuffer(vmbuffer);
		*update_indexes = TU_None;

		bms_free(hot_attrs);
		bms_free(sum_attrs);
		bms_free(key_attrs);
		bms_free(id_attrs);
		bms_free(modified_attrs);
		bms_free(interesting_attrs);
		return result;
	}

	/*
	 * 如果我们没有 pin 可见性映射页，而在我们忙于锁定缓冲区期间或之后
	 * 某个解锁窗口期间该页变为完全可见，则我们必须解锁并重新锁定，
	 * 以避免在 I/O 期间持有缓冲区锁。这有点遗憾，尤其是我们现在必须重新检查
	 * 元组是否被锁定或在脚下被更新，但希望这种情况不常发生。
	 */
	if (vmbuffer == InvalidBuffer && PageIsAllVisible(page))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		visibilitymap_pin(relation, block, &vmbuffer);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		goto l2;
	}

	/* 填充事务状态数据 */

	/*
	 * 如果我们正在更新的元组已被锁定，则需要在旧元组的 Xmax 中保留锁定信息。
	 * 为此准备一个新的 Xmax 值。
	 */
	compute_new_xmax_infomask(HeapTupleHeaderGetRawXmax(oldtup.t_data),
							  oldtup.t_data->t_infomask,
							  oldtup.t_data->t_infomask2,
							  xid, *lockmode, true,
							  &xmax_old_tuple, &infomask_old_tuple,
							  &infomask2_old_tuple);

	/*
	 * 同时为新元组的副本准备 Xmax 值。如果之前没有 xmax，或者虽然有过但
	 * 所有锁定者现已消失，则使用 InvalidTransactionId；否则，从旧元组获取
	 * xmax。（在少数情况下，xmax 也可能是 InvalidTransactionId 却没有设置
	 * HEAP_XMAX_INVALID 位；这没问题。）
	 */
	if ((oldtup.t_data->t_infomask & HEAP_XMAX_INVALID) ||
		HEAP_LOCKED_UPGRADED(oldtup.t_data->t_infomask) ||
		(checked_lockers && !locker_remains))
		xmax_new_tuple = InvalidTransactionId;
	else
		xmax_new_tuple = HeapTupleHeaderGetRawXmax(oldtup.t_data);

	if (!TransactionIdIsValid(xmax_new_tuple))
	{
		infomask_new_tuple = HEAP_XMAX_INVALID;
		infomask2_new_tuple = 0;
	}
	else
	{
		/*
		 * 如果我们为新元组找到了有效的 Xmax，那么新元组上使用的 infomask 位
		 * 取决于旧元组上的内容。注意，由于我们正在进行更新，唯一可能的情况是
		 * 锁定者持有 FOR KEY SHARE 锁。
		 */
		if (oldtup.t_data->t_infomask & HEAP_XMAX_IS_MULTI)
		{
			GetMultiXactIdHintBits(xmax_new_tuple, &infomask_new_tuple,
								   &infomask2_new_tuple);
		}
		else
		{
			infomask_new_tuple = HEAP_XMAX_KEYSHR_LOCK | HEAP_XMAX_LOCK_ONLY;
			infomask2_new_tuple = 0;
		}
	}

	/*
	 * 为新元组准备 Xmin 和 Xmax 的适当初始值，以及上述计算的初始 infomask 位。
	 */
	newtup->t_data->t_infomask &= ~(HEAP_XACT_MASK);
	newtup->t_data->t_infomask2 &= ~(HEAP2_XACT_MASK);
	HeapTupleHeaderSetXmin(newtup->t_data, xid);
	HeapTupleHeaderSetCmin(newtup->t_data, cid);
	newtup->t_data->t_infomask |= HEAP_UPDATED | infomask_new_tuple;
	newtup->t_data->t_infomask2 |= infomask2_new_tuple;
	HeapTupleHeaderSetXmax(newtup->t_data, xmax_new_tuple);

	/*
	 * 如有必要，将 cid 替换为组合 CID。注意，我们已将普通 cid 放入了新元组中。
	 */
	HeapTupleHeaderAdjustCmax(oldtup.t_data, &cid, &iscombo);

	/*
	 * 如果需要激活 toaster，或者新元组无法与旧元组放在同一页面上，那么
	 * 我们在进行 TOAST 和/或表文件扩展操作时必须释放旧元组缓冲区的内容锁
	 * （但不释放 pin！）。我们必须标记旧元组以显示它已被锁定，否则其他进程
	 * 可能尝试自行更新它。
	 *
	 * 如果已经存在行外（out-of-line）TOAST 值，或者新元组超过阈值，
	 * 则需要调用 toaster。
	 */
	if (relation->rd_rel->relkind != RELKIND_RELATION &&
		relation->rd_rel->relkind != RELKIND_MATVIEW)
	{
		/* toast 表条目绝对不应被递归 toast */
		Assert(!HeapTupleHasExternal(&oldtup));
		Assert(!HeapTupleHasExternal(newtup));
		need_toast = false;
	}
	else
		need_toast = (HeapTupleHasExternal(&oldtup) ||
					  HeapTupleHasExternal(newtup) ||
					  newtup->t_len > TOAST_TUPLE_THRESHOLD);

	pagefree = PageGetHeapFreeSpace(page);

	newtupsize = MAXALIGN(newtup->t_len);

	if (need_toast || newtupsize > pagefree)
	{
		TransactionId xmax_lock_old_tuple;
		uint16		infomask_lock_old_tuple,
					infomask2_lock_old_tuple;
		bool		cleared_all_frozen = false;

		/*
		 * 为防止并发会话更新元组，在释放页级锁期间，我们必须临时将其标记
		 * 为已锁定。
		 *
		 * 为满足"任何可能出现在写入磁盘的缓冲区中的 xid"的规则，我们不得不
		 * 将此临时修改记录到 WAL 中。我们可以为此目的复用 xl_heap_lock。
		 * 如果我们在完成实际更新之前崩溃/出错，xmax 将属于一个已中止事务，
		 * 从而允许其他会话继续。
		 */

		/*
		 * 计算适合锁定元组的 xmax / infomask。这必须与用于更新的组合值分开
		 * 计算，否则可能创建出错误的 multixact。
		 */
		compute_new_xmax_infomask(HeapTupleHeaderGetRawXmax(oldtup.t_data),
								  oldtup.t_data->t_infomask,
								  oldtup.t_data->t_infomask2,
								  xid, *lockmode, false,
								  &xmax_lock_old_tuple, &infomask_lock_old_tuple,
								  &infomask2_lock_old_tuple);

		Assert(HEAP_XMAX_IS_LOCKED_ONLY(infomask_lock_old_tuple));

		START_CRIT_SECTION();

		/* 清除过时的可见性标志... */
		oldtup.t_data->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
		oldtup.t_data->t_infomask2 &= ~HEAP_KEYS_UPDATED;
		HeapTupleClearHotUpdated(&oldtup);
		/* ... 并存储更新此元组的事务信息 */
		Assert(TransactionIdIsValid(xmax_lock_old_tuple));
		HeapTupleHeaderSetXmax(oldtup.t_data, xmax_lock_old_tuple);
		oldtup.t_data->t_infomask |= infomask_lock_old_tuple;
		oldtup.t_data->t_infomask2 |= infomask2_lock_old_tuple;
		HeapTupleHeaderSetCmax(oldtup.t_data, cid, iscombo);

		/* 临时使其看起来像未被更新，但已锁定 */
		oldtup.t_data->t_ctid = oldtup.t_self;

		/*
		 * 如有需要，清除可见性映射上的 all-frozen 位。我们可以立即重置
		 * ALL_VISIBLE，但考虑到 WAL 日志开销不变，似乎不一定值得。
		 */
		if (PageIsAllVisible(page) &&
			visibilitymap_clear(relation, block, vmbuffer,
								VISIBILITYMAP_ALL_FROZEN))
			cleared_all_frozen = true;

		MarkBufferDirty(buffer);

		if (RelationNeedsWAL(relation))
		{
			xl_heap_lock xlrec;
			XLogRecPtr	recptr;

			XLogBeginInsert();
			XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

			xlrec.offnum = ItemPointerGetOffsetNumber(&oldtup.t_self);
			xlrec.xmax = xmax_lock_old_tuple;
			xlrec.infobits_set = compute_infobits(oldtup.t_data->t_infomask,
												  oldtup.t_data->t_infomask2);
			xlrec.flags =
				cleared_all_frozen ? XLH_LOCK_ALL_FROZEN_CLEARED : 0;
			XLogRegisterData(&xlrec, SizeOfHeapLock);
			recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_LOCK);
			PageSetLSN(page, recptr);
		}

		END_CRIT_SECTION();

		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

		/*
		 * 如有需要，让 toaster 完成其工作。
		 *
		 * 注意：在此之后，heaptup 是我们实际要存入关系的数据；newtup 是调用者
		 * 原始的未 toast 数据。
		 */
		if (need_toast)
		{
			/* 注意，在更新过程中我们始终使用 WAL 和 FSM */
			heaptup = heap_toast_insert_or_update(relation, newtup, &oldtup, 0);
			newtupsize = MAXALIGN(heaptup->t_len);
		}
		else
			heaptup = newtup;

		/*
		 * 现在，元组是否需要新页面？这有点棘手，因为其他进程可能在我们不注意时
		 * 向页面添加了元组。在重新获取缓冲区锁后，我们必须重新检查可用空间。
		 * 但如果之前的可用空间量仍不足，就无需费心；现在不太可能比之前有更多
		 * 空闲空间。
		 *
		 * 更重要的是，如果需要获取新页面，我们将需要同时获取旧页面和
		 * 新页面的缓冲区锁。为避免与其他尝试以相反顺序获取同一对锁的后台进程
		 * 发生死锁，我们必须保持一致的锁获取顺序。我们使用"先锁定关系中编号
		 * 较小的页面"规则。为此，我们必须在未持有旧页面锁的情况下调用
		 * RelationGetBufferForTuple，并依赖它以正确顺序获取两个页面的锁。
		 *
		 * 另一个考虑因素是：如果需要清除任一页面上的 all-visible 标志，
		 * 我们需要可见性映射页的 pin。如果调用 RelationGetBufferForTuple，
		 * 我们依赖它来获取此类 pin；但如果不调用，则必须在此处处理。因此我们
		 * 需要一个循环。
		 */
		for (;;)
		{
			if (newtupsize > pagefree)
			{
				/* 放不下，必须使用 RelationGetBufferForTuple。 */
				newbuf = RelationGetBufferForTuple(relation, heaptup->t_len,
												   buffer, 0, NULL,
												   &vmbuffer_new, &vmbuffer,
												   0);
				/* 完成。 */
				break;
			}
			/* 如果需要且尚未持有，获取 VM 页的 pin。 */
			if (vmbuffer == InvalidBuffer && PageIsAllVisible(page))
				visibilitymap_pin(relation, block, &vmbuffer);
			/* 重新获取旧元组页面的锁。 */
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
			/* 使用最新的空闲空间重新检查 */
			pagefree = PageGetHeapFreeSpace(page);
			if (newtupsize > pagefree ||
				(vmbuffer == InvalidBuffer && PageIsAllVisible(page)))
			{
				/*
				 * 糟糕，空间又不够了，或者刚刚有人设置了 all-visible 标志。
				 * 我们必须解锁并循环以避免死锁。幸好，这条路径很少被走到。
				 */
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			}
			else
			{
				/* 完成。 */
				newbuf = buffer;
				break;
			}
		}
	}
	else
	{
		/* 无需 TOAST 操作，且能放入相同页面 */
		newbuf = buffer;
		heaptup = newtup;
	}

	/*
	 * 我们即将执行实际更新 —— 先检查冲突，以避免可能需要回滚刚刚完成的工作。
	 *
	 * 只要在本次检查与更新对扫描可见之间没有其他进程扫描页面的可能性
	 * （即，从此时起直到元组更新可见为止，持续持有排他缓冲区内容锁），
	 * 此操作就是安全的，无需重新检查。
	 *
	 * 对于新元组，只需要在关系级别检查，但由于两个元组在同一关系中，
	 * 且对 oldtup 的检查将包含关系级别的检查，因此单独检查新元组没有额外好处。
	 */
	CheckForSerializableConflictIn(relation, &oldtup.t_self,
								   BufferGetBlockNumber(buffer));

	/*
	 * 此时，newbuf 和 buffer 均已 pin 且锁定，且 newbuf 有足够空间存放新元组。
	 * 如果它们是同一个缓冲区，则只持有一个 pin。
	 */

	if (newbuf == buffer)
	{
		/*
		 * Since the new tuple is going into the same page, we might be able
		 * to do a HOT update.  Check if any of the index columns have been
		 * changed.
		 */
		if (!bms_overlap(modified_attrs, hot_attrs))
		{
			use_hot_update = true;

			/*
			 * 如果索引热块（hot-blocking）中使用的列都没有被更新，我们可以应用
			 * HOT，但仍需检查是否需要更新汇总索引（summarizing indexes），
			 * 如果相关列被修改则更新这些索引；否则我们可能无法检测到
			 * BRIN minmax 索引中的值范围变化等情况。
			 */
			if (bms_overlap(modified_attrs, sum_attrs))
				summarized_update = true;
		}
	}
	else
	{
		/* 设置提示，表示旧页可以使用 prun/defrag */
		PageSetFull(page);
	}

	/*
	 * 在进入临界区之前计算副本标识元组，以避免在内存分配失败时触发 PANIC。
	 * 如果无需记录任何内容，ExtractReplicaIdentity() 将返回 NULL。
	 * 仅当副本标识键列被修改或包含外部数据时，才将 old key required 设为 true。
	 */
	old_key_tuple = ExtractReplicaIdentity(relation, &oldtup,
										   bms_overlap(modified_attrs, id_attrs) ||
										   id_has_external,
										   &old_key_copied);

	/* 从此处起直到更改记录完毕之前，禁止 EREPORT(ERROR) */
	START_CRIT_SECTION();

	/*
	 * 如果此事务提交，旧元组迟早会变为 DEAD。设置标志，表示一旦我们的 xid
	 * 低于 OldestXmin 水平线，此页面就是裁剪候选。如果事务最终中止，后续的
	 * 页面裁剪将是空操作（no-op），提示也将被清除。
	 *
	 * XXX 是否应该在 newbuf 上设置提示？如果事务中止，newbuf 中会有一个可裁剪
	 * 的元组；但目前我们选择不为中止情况优化。注意，如果此决定改变，
	 * heap_xlog_update 必须保持同步。
	 */
	PageSetPrunable(page, xid);

	if (use_hot_update)
	{
		/* 将旧元组标记为 HOT-updated */
		HeapTupleSetHotUpdated(&oldtup);
		/* 将新元组标记为 heap-only */
		HeapTupleSetHeapOnly(heaptup);
		/* 同样标记调用者的副本，以防与 heaptup 不同 */
		HeapTupleSetHeapOnly(newtup);
	}
	else
	{
		/* 确保元组正确标记为 non-HOT */
		HeapTupleClearHotUpdated(&oldtup);
		HeapTupleClearHeapOnly(heaptup);
		HeapTupleClearHeapOnly(newtup);
	}

	RelationPutHeapTuple(relation, newbuf, heaptup, false); /* 插入新元组 */


	/* 清除过时的可见性标志，可能由我们上面自己设置的... */
	oldtup.t_data->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
	oldtup.t_data->t_infomask2 &= ~HEAP_KEYS_UPDATED;
	/* ... 并存储更新此元组的事务信息 */
	Assert(TransactionIdIsValid(xmax_old_tuple));
	HeapTupleHeaderSetXmax(oldtup.t_data, xmax_old_tuple);
	oldtup.t_data->t_infomask |= infomask_old_tuple;
	oldtup.t_data->t_infomask2 |= infomask2_old_tuple;
	HeapTupleHeaderSetCmax(oldtup.t_data, cid, iscombo);

	/* 在旧元组的 t_ctid 中记录新元组的地址 */
	oldtup.t_data->t_ctid = heaptup->t_self;

	/* 清除 PD_ALL_VISIBLE 标志，重置所有可见性映射位 */
	if (PageIsAllVisible(BufferGetPage(buffer)))
	{
		all_visible_cleared = true;
		PageClearAllVisible(BufferGetPage(buffer));
		visibilitymap_clear(relation, BufferGetBlockNumber(buffer),
							vmbuffer, VISIBILITYMAP_VALID_BITS);
	}
	if (newbuf != buffer && PageIsAllVisible(BufferGetPage(newbuf)))
	{
		all_visible_cleared_new = true;
		PageClearAllVisible(BufferGetPage(newbuf));
		visibilitymap_clear(relation, BufferGetBlockNumber(newbuf),
							vmbuffer_new, VISIBILITYMAP_VALID_BITS);
	}

	if (newbuf != buffer)
		MarkBufferDirty(newbuf);
	MarkBufferDirty(buffer);

	/* XLOG 记录 */
	if (RelationNeedsWAL(relation))
	{
		XLogRecPtr	recptr;

		/*
		 * 对于逻辑解码，我们需要组合 CID 来正确解码目录。
		 */
		if (RelationIsAccessibleInLogicalDecoding(relation))
		{
			log_heap_new_cid(relation, &oldtup);
			log_heap_new_cid(relation, heaptup);
		}

		recptr = log_heap_update(relation, buffer,
								 newbuf, &oldtup, heaptup,
								 old_key_tuple,
								 all_visible_cleared,
								 all_visible_cleared_new);
		if (newbuf != buffer)
		{
			PageSetLSN(BufferGetPage(newbuf), recptr);
		}
		PageSetLSN(BufferGetPage(buffer), recptr);
	}

	END_CRIT_SECTION();

	if (newbuf != buffer)
		LockBuffer(newbuf, BUFFER_LOCK_UNLOCK);
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	/*
	 * 标记旧元组在下一次命令边界时从系统缓存中作废，同时标记新元组以备我们
	 * 中止时作废。必须在释放缓冲区之前完成此操作，因为 oldtup 在缓冲区中。
	 * （heaptup 全在本地内存中，但需要在一次 inval.c 调用中处理两个元组版本，
	 * 以避免冗余的 sinval 消息。）
	 */
	CacheInvalidateHeapTuple(relation, &oldtup, heaptup);

	/* 现在可以释放缓冲区了 */
	if (newbuf != buffer)
		ReleaseBuffer(newbuf);
	ReleaseBuffer(buffer);
	if (BufferIsValid(vmbuffer_new))
		ReleaseBuffer(vmbuffer_new);
	if (BufferIsValid(vmbuffer))
		ReleaseBuffer(vmbuffer);

	/*
	 * 释放 lmgr 元组锁（如果持有的话）。
	 */
	if (have_tuple_lock)
		UnlockTupleTuplock(relation, &(oldtup.t_self), *lockmode);

	pgstat_count_heap_update(relation, use_hot_update, newbuf != buffer);

	/*
	 * 如果 heaptup 是私有副本，则释放它。不要忘记将 t_self 复制回调用者的
	 * 镜像中。
	 */
	if (heaptup != newtup)
	{
		newtup->t_self = heaptup->t_self;
		heap_freetuple(heaptup);
	}

	/*
	 * 如果是 HOT 更新，更新可能仍需要更新汇总索引（summarized indexes），
	 * 以免遗漏这些汇总的更新而导致错误结果（例如，此更新可能改变块的
	 * minmax 范围）。
	 */
	if (use_hot_update)
	{
		if (summarized_update)
			*update_indexes = TU_Summarizing;
		else
			*update_indexes = TU_None;
	}
	else
		*update_indexes = TU_All;

	if (old_key_tuple != NULL && old_key_copied)
		heap_freetuple(old_key_tuple);

	bms_free(hot_attrs);
	bms_free(sum_attrs);
	bms_free(key_attrs);
	bms_free(id_attrs);
	bms_free(modified_attrs);
	bms_free(interesting_attrs);

	return TM_Ok;
}

#ifdef USE_ASSERT_CHECKING
/*
 * 确认在 heap_update() 期间持有充足的锁，规则来自
 * README.tuplock 的"Locking to write inplace-updated tables"章节。
 */
static void
check_lock_if_inplace_updateable_rel(Relation relation,
									 ItemPointer otid,
									 HeapTuple newtup)
{
	/* LOCKTAG_TUPLE 对任何系统目录均可接受 */
	switch (RelationGetRelid(relation))
	{
		case RelationRelationId:
		case DatabaseRelationId:
			{
				LOCKTAG		tuptag;

				SET_LOCKTAG_TUPLE(tuptag,
								  relation->rd_lockInfo.lockRelId.dbId,
								  relation->rd_lockInfo.lockRelId.relId,
								  ItemPointerGetBlockNumber(otid),
								  ItemPointerGetOffsetNumber(otid));
				if (LockHeldByMe(&tuptag, InplaceUpdateTupleLock, false))
					return;
			}
			break;
		default:
			Assert(!IsInplaceUpdateRelation(relation));
			return;
	}

	switch (RelationGetRelid(relation))
	{
		case RelationRelationId:
			{
				/* LOCKTAG_TUPLE 或 LOCKTAG_RELATION 均可 */
				Form_pg_class classForm = (Form_pg_class) GETSTRUCT(newtup);
				Oid			relid = classForm->oid;
				Oid			dbid;
				LOCKTAG		tag;

				if (IsSharedRelation(relid))
					dbid = InvalidOid;
				else
					dbid = MyDatabaseId;

				if (classForm->relkind == RELKIND_INDEX)
				{
					Relation	irel = index_open(relid, AccessShareLock);

					SET_LOCKTAG_RELATION(tag, dbid, irel->rd_index->indrelid);
					index_close(irel, AccessShareLock);
				}
				else
					SET_LOCKTAG_RELATION(tag, dbid, relid);

				if (!LockHeldByMe(&tag, ShareUpdateExclusiveLock, false) &&
					!LockHeldByMe(&tag, ShareRowExclusiveLock, true))
					elog(WARNING,
						 "missing lock for relation \"%s\" (OID %u, relkind %c) @ TID (%u,%u)",
						 NameStr(classForm->relname),
						 relid,
						 classForm->relkind,
						 ItemPointerGetBlockNumber(otid),
						 ItemPointerGetOffsetNumber(otid));
			}
			break;
		case DatabaseRelationId:
			{
				/* 需要 LOCKTAG_TUPLE */
				Form_pg_database dbForm = (Form_pg_database) GETSTRUCT(newtup);

				elog(WARNING,
					 "missing lock on database \"%s\" (OID %u) @ TID (%u,%u)",
					 NameStr(dbForm->datname),
					 dbForm->oid,
					 ItemPointerGetBlockNumber(otid),
					 ItemPointerGetOffsetNumber(otid));
			}
			break;
	}
}

/*
 * 确认持有充足的关系锁，规则来自 README.tuplock 的
 * "Locking to write inplace-updated tables"章节。
 */
static void
check_inplace_rel_lock(HeapTuple oldtup)
{
	Form_pg_class classForm = (Form_pg_class) GETSTRUCT(oldtup);
	Oid			relid = classForm->oid;
	Oid			dbid;
	LOCKTAG		tag;

	if (IsSharedRelation(relid))
		dbid = InvalidOid;
	else
		dbid = MyDatabaseId;

	if (classForm->relkind == RELKIND_INDEX)
	{
		Relation	irel = index_open(relid, AccessShareLock);

		SET_LOCKTAG_RELATION(tag, dbid, irel->rd_index->indrelid);
		index_close(irel, AccessShareLock);
	}
	else
		SET_LOCKTAG_RELATION(tag, dbid, relid);

	if (!LockHeldByMe(&tag, ShareUpdateExclusiveLock, true))
		elog(WARNING,
			 "missing lock for relation \"%s\" (OID %u, relkind %c) @ TID (%u,%u)",
			 NameStr(classForm->relname),
			 relid,
			 classForm->relkind,
			 ItemPointerGetBlockNumber(&oldtup->t_self),
			 ItemPointerGetOffsetNumber(&oldtup->t_self));
}
#endif

/*
 * 检查指定属性的值是否相同。HeapDetermineColumnsInfo 的子函数。
 */
static bool
heap_attr_equals(TupleDesc tupdesc, int attrnum, Datum value1, Datum value2,
				 bool isnull1, bool isnull2)
{
	/*
	 * 如果一个是 NULL 而另一个不是，则它们肯定不相等。
	 */
	if (isnull1 != isnull2)
		return false;

	/*
	 * 如果两者都是 NULL，则可以认为它们相等。
	 */
	if (isnull1)
		return true;

	/*
	 * 我们对两个 datum 进行简单的二进制比较。这可能过于严格，因为同一逻辑值
	 * 可能有多种二进制表示。但只要没有误判，应该没问题。使用类型特定的等值
	 * 运算符很麻烦，因为不同的操作符类可能有不同的等值概念；此外，我们不能
	 * 在持有排他缓冲区锁的情况下安全地调用用户自定义函数。
	 */
	if (attrnum <= 0)
	{
		/* 唯一允许的系统列是 OID，因此执行此操作 */
		return (DatumGetObjectId(value1) == DatumGetObjectId(value2));
	}
	else
	{
		CompactAttribute *att;

		Assert(attrnum <= tupdesc->natts);
		att = TupleDescCompactAttr(tupdesc, attrnum - 1);
		return datumIsEqual(value1, value2, att->attbyval, att->attlen);
	}
}

/*
 * 检查哪些列正在被更新。
 *
 * 给定一个已更新的元组，从列为"interesting"的集合中确定（并返回到输出的
 * bitmapset 中）发生变化的列集合。
 *
 * has_external 指示旧元组的未修改属性（来自标记为 interesting 的属性）中
 * 是否有属于 external_cols 并以外部方式存储的属性。
 */
static Bitmapset *
HeapDetermineColumnsInfo(Relation relation,
						 Bitmapset *interesting_cols,
						 Bitmapset *external_cols,
						 HeapTuple oldtup, HeapTuple newtup,
						 bool *has_external)
{
	int			attidx;
	Bitmapset  *modified = NULL;
	TupleDesc	tupdesc = RelationGetDescr(relation);

	attidx = -1;
	while ((attidx = bms_next_member(interesting_cols, attidx)) >= 0)
	{
		/* attidx 是从零开始的，attrnum 是正常的属性编号 */
		AttrNumber	attrnum = attidx + FirstLowInvalidHeapAttributeNumber;
		Datum		value1,
					value2;
		bool		isnull1,
					isnull2;

		/*
		 * 如果是全元组引用，则返回"不相等"。实际上不值得支持这种情况，
		 * 因为它只能在没有发生实际变化的更新（no-op update）中成功，
		 * 而这种场景不值得优化。
		 */
		if (attrnum == 0)
		{
			modified = bms_add_member(modified, attidx);
			continue;
		}

		/*
		 * 同样，对于除 tableOID 之外的任何系统属性，自动返回"不相等"；
		 * 我们不能期望这些在 HOT 链中一致，甚至不能期望它们在新元组中
		 * 已经被正确设置。
		 */
		if (attrnum < 0)
		{
			if (attrnum != TableOidAttributeNumber)
			{
				modified = bms_add_member(modified, attidx);
				continue;
			}
		}

		/*
		 * 提取对应的值。XXX 如果有许多索引列，这效率很低。是否应该
		 * 对每个元组调用一次 heap_deform_tuple？但这不适用于系统列...
		 */
		value1 = heap_getattr(oldtup, attrnum, tupdesc, &isnull1);
		value2 = heap_getattr(newtup, attrnum, tupdesc, &isnull2);

		if (!heap_attr_equals(tupdesc, attrnum, value1,
							  value2, isnull1, isnull2))
		{
			modified = bms_add_member(modified, attidx);
			continue;
		}

		/*
		 * 无需检查不能外部存储的属性。注意系统属性不能外部存储。
		 */
		if (attrnum < 0 || isnull1 ||
			TupleDescCompactAttr(tupdesc, attrnum - 1)->attlen != -1)
			continue;

		/*
		 * 检查旧元组的属性是否已外部存储，并且是否为 external_cols 的成员。
		 */
		if (VARATT_IS_EXTERNAL((struct varlena *) DatumGetPointer(value1)) &&
			bms_is_member(attidx, external_cols))
			*has_external = true;
	}

	return modified;
}

/*
 *	simple_heap_update - 替换一个元组
 *
 * 此函数用于在预期不会有并发更新目标元组的情况下（例如，因为我们持有
 * 与元组关联的关系锁）更新元组。任何失败都会通过 ereport() 报告。
 */
void
simple_heap_update(Relation relation, ItemPointer otid, HeapTuple tup,
				   TU_UpdateIndexes *update_indexes)
{
	TM_Result	result;
	TM_FailureData tmfd;
	LockTupleMode lockmode;

	result = heap_update(relation, otid, tup,
						 GetCurrentCommandId(true), InvalidSnapshot,
						 true /* 等待提交 */ ,
						 &tmfd, &lockmode, update_indexes);
	switch (result)
	{
		case TM_SelfModified:
			/* 元组已在当前命令中被更新？ */
			elog(ERROR, "tuple already updated by self");
			break;

		case TM_Ok:
			/* 成功完成 */
			break;

		case TM_Updated:
			elog(ERROR, "tuple concurrently updated");
			break;

		case TM_Deleted:
			elog(ERROR, "tuple concurrently deleted");
			break;

		default:
			elog(ERROR, "unrecognized heap_update status: %u", result);
			break;
	}
}


/*
 * 返回与给定元组锁定模式对应的 MultiXactStatus。
 */
static MultiXactStatus
get_mxact_status_for_lock(LockTupleMode mode, bool is_update)
{
	int			retval;

	if (is_update)
		retval = tupleLockExtraInfo[mode].updstatus;
	else
		retval = tupleLockExtraInfo[mode].lockstatus;

	if (retval == -1)
		elog(ERROR, "invalid lock tuple mode %d/%s", mode,
			 is_update ? "true" : "false");

	return (MultiXactStatus) retval;
}

/*
 *	heap_lock_tuple - 以共享或排他模式锁定一个元组
 *
 * 注意：此函数会获取一个缓冲区 pin，调用者必须释放它。
 *
 * 输入参数：
 *	relation: 包含元组的关系（调用者必须持有合适的锁）
 *	tid: 要锁定的元组的 TID
 *	cid: 当前命令 ID（用于可见性测试，并在锁定成功时存入元组的 cmax）
 *	mode: 指示需要共享锁还是排他锁
 *	wait_policy: 当元组锁不可用时如何处理
 *	follow_updates: 如果为 true，则沿更新链也锁定后代元组。
 *
 * 输出参数：
 *	*tuple: 填充了所有字段
 *	*buffer: 设置为持有元组的缓冲区（退出时已 pin 但未锁定）
 *	*tmfd: 在失败情况下填充（见下文）
 *
 * 函数返回值与 table_tuple_lock() 的返回值相同。
 *
 * 在除 TM_Invisible 之外的失败情况下，此函数用元组的 t_ctid、t_xmax
 * （必要时解析可能的 MultiXact）和 t_cmax 填充 *tmfd
 * （最后一个仅用于 TM_SelfModified，因为我们无法从其他事务生成的
 * 组合 CID 中获取 cmax）。
 * 更多信息请参见 struct TM_FailureData 的注释。
 *
 * 关于此机制的详细说明，请参见 README.tuplock。
 */
TM_Result
heap_lock_tuple(Relation relation, HeapTuple tuple,
				CommandId cid, LockTupleMode mode, LockWaitPolicy wait_policy,
				bool follow_updates,
				Buffer *buffer, TM_FailureData *tmfd)
{
	TM_Result	result;
	ItemPointer tid = &(tuple->t_self);
	ItemId		lp;
	Page		page;
	Buffer		vmbuffer = InvalidBuffer;
	BlockNumber block;
	TransactionId xid,
				xmax;
	uint16		old_infomask,
				new_infomask,
				new_infomask2;
	bool		first_time = true;
	bool		skip_tuple_lock = false;
	bool		have_tuple_lock = false;
	bool		cleared_all_frozen = false;

	*buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));
	block = ItemPointerGetBlockNumber(tid);

	/*
	 * Before locking the buffer, pin the visibility map page if it appears to
	 * be necessary.  Since we haven't got the lock yet, someone else might be
	 * in the middle of changing this, so we'll need to recheck after we have
	 * the lock.
	 */
	if (PageIsAllVisible(BufferGetPage(*buffer)))
		visibilitymap_pin(relation, block, &vmbuffer);

	LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);

	page = BufferGetPage(*buffer);
	lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
	Assert(ItemIdIsNormal(lp));

	tuple->t_data = (HeapTupleHeader) PageGetItem(page, lp);
	tuple->t_len = ItemIdGetLength(lp);
	tuple->t_tableOid = RelationGetRelid(relation);

l3:
	result = HeapTupleSatisfiesUpdate(tuple, cid, *buffer);

	if (result == TM_Invisible)
	{
		/*
		 * This is possible, but only when locking a tuple for ON CONFLICT
		 * UPDATE.  We return this value here rather than throwing an error in
		 * order to give that case the opportunity to throw a more specific
		 * error.
		 */
		result = TM_Invisible;
		goto out_locked;
	}
	else if (result == TM_BeingModified ||
			 result == TM_Updated ||
			 result == TM_Deleted)
	{
		TransactionId xwait;
		uint16		infomask;
		uint16		infomask2;
		bool		require_sleep;
		ItemPointerData t_ctid;

		/* 在解锁缓冲区之前，必须先复制状态数据 */
		xwait = HeapTupleHeaderGetRawXmax(tuple->t_data);
		infomask = tuple->t_data->t_infomask;
		infomask2 = tuple->t_data->t_infomask2;
		ItemPointerCopy(&tuple->t_data->t_ctid, &t_ctid);

		LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);

		/*
		 * 如果当前顶层事务的某个子事务已经持有与我们请求的锁同等强度或更强的锁，
		 * 那么我们实际上已经持有了所需的锁。我们*必须*在不尝试获取元组锁的
		 * 情况下成功，否则将与想要获取更强锁的任何人死锁。
		 *
		 * 注意，我们仅在第一次循环 HTSU 结果时执行此操作；在后续遍历中测试
		 * 没有意义，因为很明显我们自己的事务在第一次检查之后不可能获得新锁。
		 */
		if (first_time)
		{
			first_time = false;

			if (infomask & HEAP_XMAX_IS_MULTI)
			{
				int			i;
				int			nmembers;
				MultiXactMember *members;

				/*
				 * 这里不需要允许旧的 multixact；如果真是那样，
				 * HeapTupleSatisfiesUpdate 会返回 MayBeUpdated，我们就不会
				 * 走到这里了。
				 */
				nmembers =
					GetMultiXactIdMembers(xwait, &members, false,
										  HEAP_XMAX_IS_LOCKED_ONLY(infomask));

				for (i = 0; i < nmembers; i++)
				{
					/* 仅考虑属于我们自身事务的成员 */
					if (!TransactionIdIsCurrentTransactionId(members[i].xid))
						continue;

					if (TUPLOCK_from_mxstatus(members[i].status) >= mode)
					{
						pfree(members);
						result = TM_Ok;
						goto out_unlocked;
					}
					else
					{
						/*
						 * 禁用重量级元组锁的获取。否则，在提升较弱锁时，我们
						 * 可能会与另一个已获取重量级元组锁并等待我们的事务
						 * 完成的锁定者发生死锁。
						 *
						 * 注意，在这种情况下，如果需要，我们仍然需要等待
						 * multixact，以避免获取冲突的锁。
						 */
						skip_tuple_lock = true;
					}
				}

				if (members)
					pfree(members);
			}
			else if (TransactionIdIsCurrentTransactionId(xwait))
			{
				switch (mode)
				{
					case LockTupleKeyShare:
						Assert(HEAP_XMAX_IS_KEYSHR_LOCKED(infomask) ||
							   HEAP_XMAX_IS_SHR_LOCKED(infomask) ||
							   HEAP_XMAX_IS_EXCL_LOCKED(infomask));
						result = TM_Ok;
						goto out_unlocked;
					case LockTupleShare:
						if (HEAP_XMAX_IS_SHR_LOCKED(infomask) ||
							HEAP_XMAX_IS_EXCL_LOCKED(infomask))
						{
							result = TM_Ok;
							goto out_unlocked;
						}
						break;
					case LockTupleNoKeyExclusive:
						if (HEAP_XMAX_IS_EXCL_LOCKED(infomask))
						{
							result = TM_Ok;
							goto out_unlocked;
						}
						break;
					case LockTupleExclusive:
						if (HEAP_XMAX_IS_EXCL_LOCKED(infomask) &&
							infomask2 & HEAP_KEYS_UPDATED)
						{
							result = TM_Ok;
							goto out_unlocked;
						}
						break;
				}
			}
		}

		/*
		 * 初步假设我们需要等待锁定事务完成。我们在下面检查各种可以
		 * 关闭此等待的情况。
		 */
		require_sleep = true;
		if (mode == LockTupleKeyShare)
		{
			/*
			 * 如果我们请求的是 KeyShare，且没有更新操作正在执行，则无需等待。
			 * 即使有更新，如果键未被修改，我们仍然可以继续。
			 *
			 * 但是，如果有更新，我们需要遍历更新链，将行的未来版本也标记为
			 * 已锁定。这样，如果有人删除该未来版本，我们可以受到保护，防止
			 * 键消失。锁定未来版本可能会短暂阻塞（如果并发事务正在删除键），
			 * 或者可能返回"删除键的事务已提交"的结果。因此我们在重新锁定
			 * 缓冲区之前执行此操作；否则容易发生死锁。
			 *
			 * 注意，我们锁定的 TID 是在解锁缓冲区之前获取的。要使其在我们
			 * 不注意时发生变化，我们在重新锁定缓冲区后测试的其他属性也必须
			 * 同时变化，此时我们将重新执行上面的循环。
			 */
			if (!(infomask2 & HEAP_KEYS_UPDATED))
			{
				bool		updated;

				updated = !HEAP_XMAX_IS_LOCKED_ONLY(infomask);

				/*
				 * 如果有更新，则跟随更新链；如果无法完成则退出。
				 */
				if (follow_updates && updated &&
					!ItemPointerEquals(&tuple->t_self, &t_ctid))
				{
					TM_Result	res;

					res = heap_lock_updated_tuple(relation,
												  infomask, xwait, &t_ctid,
												  GetCurrentTransactionId(),
												  mode);
					if (res != TM_Ok)
					{
						result = res;
						/* 恢复代码期望持有缓冲区锁 */
						LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
						goto failed;
					}
				}

				LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);

				/*
				 * 确保它仍是合适的锁，否则重新开始。此外，如果在我们释放锁
				 * 之前它未被更新，但现在已更新，我们也要重新开始；原因是现在
				 * 需要跟随更新链来锁定新版本。
				 */
				if (!HeapTupleHeaderIsOnlyLocked(tuple->t_data) &&
					((tuple->t_data->t_infomask2 & HEAP_KEYS_UPDATED) ||
					 !updated))
					goto l3;

				/* 看起来没问题，可以跳过等待 */
				require_sleep = false;

				/*
				 * 注意，我们允许 Xmax 在此处变化；其他更新者/锁定者可能在我们
				 * 获取缓冲区锁之前修改了它。然而，这不是问题，因为通过刚才的
				 * 重新检查，我们确保它们仍然不与我们需要获取的锁冲突。
				 */
			}
		}
		else if (mode == LockTupleShare)
		{
			/*
			 * If we're requesting Share, we can similarly avoid sleeping if
			 * there's no update and no exclusive lock present.
			 */
			if (HEAP_XMAX_IS_LOCKED_ONLY(infomask) &&
				!HEAP_XMAX_IS_EXCL_LOCKED(infomask))
			{
				LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);

				/*
				 * Make sure it's still an appropriate lock, else start over.
				 * See above about allowing xmax to change.
				 */
				if (!HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_data->t_infomask) ||
					HEAP_XMAX_IS_EXCL_LOCKED(tuple->t_data->t_infomask))
					goto l3;
				require_sleep = false;
			}
		}
		else if (mode == LockTupleNoKeyExclusive)
		{
			/*
			 * 如果请求的是 NoKeyExclusive，我们也可以避免等待；只需确保
			 * 没有已获取的冲突锁。
			 */
			if (infomask & HEAP_XMAX_IS_MULTI)
			{
				if (!DoesMultiXactIdConflict((MultiXactId) xwait, infomask,
											 mode, NULL))
				{
					/*
					 * 不冲突，但如果 xmax 在此期间发生了变化，则重新开始。
					 */
					LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
					if (xmax_infomask_changed(tuple->t_data->t_infomask, infomask) ||
						!TransactionIdEquals(HeapTupleHeaderGetRawXmax(tuple->t_data),
											 xwait))
						goto l3;

					/* 否则，没问题 */
					require_sleep = false;
				}
			}
			else if (HEAP_XMAX_IS_KEYSHR_LOCKED(infomask))
			{
				LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);

				/* 如果 xmax 在此期间发生了变化，则重新开始 */
				if (xmax_infomask_changed(tuple->t_data->t_infomask, infomask) ||
					!TransactionIdEquals(HeapTupleHeaderGetRawXmax(tuple->t_data),
										 xwait))
					goto l3;
				/* 否则，没问题 */
				require_sleep = false;
			}
		}

		/*
		 * 作为独立于上述检查的一项检查，如果当前事务是元组的唯一锁定者，
		 * 我们也可以避免等待。注意，已持有锁的强度与此无关；这不是关于在
		 * Xmax 中记录锁（无论如何，这将在下面此优化之后完成）。
		 * 还要注意，我们持有比请求的锁更强的锁的情况已经由上面的无需操作
		 * 的处理所涵盖。
		 *
		 * 注意，我们只在此处处理非 multixact 的情况；MultiXactIdWait 能
		 * 很好地自行处理此情况。
		 */
		if (require_sleep && !(infomask & HEAP_XMAX_IS_MULTI) &&
			TransactionIdIsCurrentTransactionId(xwait))
		{
			/* ... 但如果 xmax 在此期间发生了变化，则重新开始 */
			LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
			if (xmax_infomask_changed(tuple->t_data->t_infomask, infomask) ||
				!TransactionIdEquals(HeapTupleHeaderGetRawXmax(tuple->t_data),
									 xwait))
				goto l3;
			Assert(HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_data->t_infomask));
			require_sleep = false;
		}

		/*
		 * 如有必要，是时候等待其他事务/multixact 了。
		 *
		 * 如果其他事务是已提交的更新/删除操作，那么等待不可能有任何好处：
		 * 如果需要等待，则退出并抛出错误。
		 *
		 * 到这里，我们要么已经获取了排他缓冲区锁，要么必须等待锁定事务或
		 * multixact；因此在下面我们确保在等待之后获取缓冲区锁。
		 */
		if (require_sleep && (result == TM_Updated || result == TM_Deleted))
		{
			LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
			goto failed;
		}
		else if (require_sleep)
		{
			/*
			 * 获取元组锁以建立我们对元组的优先级，或尝试失败。LockTuple 会在
			 * 我们成为元组的下一个等待者时释放我们。即使我们使用共享锁也必须
			 * 执行此操作，但如果已经持有较弱的元组锁则不需要。
			 *
			 * 如果我们在下面被迫"重新开始"，我们会保留元组锁；这确保我们在
			 * 重新检查元组状态时始终排在队列的最前面。
			 */
			if (!skip_tuple_lock &&
				!heap_acquire_tuplock(relation, tid, mode, wait_policy,
									  &have_tuple_lock))
			{
				/*
				 * 这仅在 wait_policy 为 Skip 且无法获取锁时发生。
				 */
				result = TM_WouldBlock;
				/* 恢复代码期望持有缓冲区锁 */
				LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
				goto failed;
			}

			if (infomask & HEAP_XMAX_IS_MULTI)
			{
				MultiXactStatus status = get_mxact_status_for_lock(mode, false);

				/* 我们只锁定元组，从不更新它们 */
				if (status >= MultiXactStatusNoKeyUpdate)
					elog(ERROR, "invalid lock mode in heap_lock_tuple");

				/* 等待 multixact 结束，或尝试失败 */
				switch (wait_policy)
				{
					case LockWaitBlock:
						MultiXactIdWait((MultiXactId) xwait, status, infomask,
										relation, &tuple->t_self, XLTW_Lock, NULL);
						break;
					case LockWaitSkip:
						if (!ConditionalMultiXactIdWait((MultiXactId) xwait,
														status, infomask, relation,
														NULL, false))
						{
							result = TM_WouldBlock;
							/* 恢复代码期望持有缓冲区锁 */
							LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
							goto failed;
						}
						break;
					case LockWaitError:
						if (!ConditionalMultiXactIdWait((MultiXactId) xwait,
														status, infomask, relation,
														NULL, log_lock_failures))
							ereport(ERROR,
									(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
									 errmsg("could not obtain lock on row in relation \"%s\"",
											RelationGetRelationName(relation))));

						break;
				}

				/*
				 * 当然，multixact 在此处可能尚未完成：如果请求的是轻量级锁模式，
				 * 持有轻量级锁的其他事务可能仍然存活，我们自己的事务或此后台进程
				 * 的其他子事务持有的锁也可能仍然存活。我们需要保留存活的
				 * MultiXact 成员。注意，对于后者来说这不是绝对必要的，但这样做
				 * 更简单。
				 */
			}
			else
			{
				/* 等待常规事务结束，或尝试失败 */
				switch (wait_policy)
				{
					case LockWaitBlock:
						XactLockTableWait(xwait, relation, &tuple->t_self,
										  XLTW_Lock);
						break;
					case LockWaitSkip:
						if (!ConditionalXactLockTableWait(xwait, false))
						{
							result = TM_WouldBlock;
							/* 恢复代码期望持有缓冲区锁 */
							LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
							goto failed;
						}
						break;
					case LockWaitError:
						if (!ConditionalXactLockTableWait(xwait, log_lock_failures))
							ereport(ERROR,
									(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
									 errmsg("could not obtain lock on row in relation \"%s\"",
											RelationGetRelationName(relation))));
						break;
				}
			}

			/* 如果有更新，跟随更新链 */
			if (follow_updates && !HEAP_XMAX_IS_LOCKED_ONLY(infomask) &&
				!ItemPointerEquals(&tuple->t_self, &t_ctid))
			{
				TM_Result	res;

				res = heap_lock_updated_tuple(relation,
											  infomask, xwait, &t_ctid,
											  GetCurrentTransactionId(),
											  mode);
				if (res != TM_Ok)
				{
					result = res;
					/* 恢复代码期望持有缓冲区锁 */
					LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
					goto failed;
				}
			}

			LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);

			/*
			 * xwait is done, but if xwait had just locked the tuple then some
			 * other xact could update this tuple before we get to this point.
			 * Check for xmax change, and start over if so.
			 */
			if (xmax_infomask_changed(tuple->t_data->t_infomask, infomask) ||
				!TransactionIdEquals(HeapTupleHeaderGetRawXmax(tuple->t_data),
									 xwait))
				goto l3;

			if (!(infomask & HEAP_XMAX_IS_MULTI))
			{
				/*
				 * Otherwise check if it committed or aborted.  Note we cannot
				 * be here if the tuple was only locked by somebody who didn't
				 * conflict with us; that would have been handled above.  So
				 * that transaction must necessarily be gone by now.  But
				 * don't check for this in the multixact case, because some
				 * locker transactions might still be running.
				 */
				UpdateXmaxHintBits(tuple->t_data, *buffer, xwait);
			}
		}

		/* 到这里，我们确定再次持有排他缓冲区锁 */

		/*
		 * We may lock if previous xmax aborted, or if it committed but only
		 * locked the tuple without updating it; or if we didn't have to wait
		 * at all for whatever reason.
		 */
		if (!require_sleep ||
			(tuple->t_data->t_infomask & HEAP_XMAX_INVALID) ||
			HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_data->t_infomask) ||
			HeapTupleHeaderIsOnlyLocked(tuple->t_data))
			result = TM_Ok;
		else if (!ItemPointerEquals(&tuple->t_self, &tuple->t_data->t_ctid))
			result = TM_Updated;
		else
			result = TM_Deleted;
	}

failed:
	if (result != TM_Ok)
	{
		Assert(result == TM_SelfModified || result == TM_Updated ||
			   result == TM_Deleted || result == TM_WouldBlock);

		/*
		 * When locking a tuple under LockWaitSkip semantics and we fail with
		 * TM_WouldBlock above, it's possible for concurrent transactions to
		 * release the lock and set HEAP_XMAX_INVALID in the meantime.  So
		 * this assert is slightly different from the equivalent one in
		 * heap_delete and heap_update.
		 */
		Assert((result == TM_WouldBlock) ||
			   !(tuple->t_data->t_infomask & HEAP_XMAX_INVALID));
		Assert(result != TM_Updated ||
			   !ItemPointerEquals(&tuple->t_self, &tuple->t_data->t_ctid));
		tmfd->ctid = tuple->t_data->t_ctid;
		tmfd->xmax = HeapTupleHeaderGetUpdateXid(tuple->t_data);
		if (result == TM_SelfModified)
			tmfd->cmax = HeapTupleHeaderGetCmax(tuple->t_data);
		else
			tmfd->cmax = InvalidCommandId;
		goto out_locked;
	}

	/*
	 * If we didn't pin the visibility map page and the page has become all
	 * visible while we were busy locking the buffer, or during some
	 * subsequent window during which we had it unlocked, we'll have to unlock
	 * and re-lock, to avoid holding the buffer lock across I/O.  That's a bit
	 * unfortunate, especially since we'll now have to recheck whether the
	 * tuple has been locked or updated under us, but hopefully it won't
	 * happen very often.
	 */
	if (vmbuffer == InvalidBuffer && PageIsAllVisible(page))
	{
		LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);
		visibilitymap_pin(relation, block, &vmbuffer);
		LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
		goto l3;
	}

	xmax = HeapTupleHeaderGetRawXmax(tuple->t_data);
	old_infomask = tuple->t_data->t_infomask;

	/*
	 * If this is the first possibly-multixact-able operation in the current
	 * transaction, set my per-backend OldestMemberMXactId setting. We can be
	 * certain that the transaction will never become a member of any older
	 * MultiXactIds than that.  (We have to do this even if we end up just
	 * using our own TransactionId below, since some other backend could
	 * incorporate our XID into a MultiXact immediately afterwards.)
	 */
	MultiXactIdSetOldestMember();

	/*
	 * Compute the new xmax and infomask to store into the tuple.  Note we do
	 * not modify the tuple just yet, because that would leave it in the wrong
	 * state if multixact.c elogs.
	 */
	compute_new_xmax_infomask(xmax, old_infomask, tuple->t_data->t_infomask2,
							  GetCurrentTransactionId(), mode, false,
							  &xid, &new_infomask, &new_infomask2);

	START_CRIT_SECTION();

	/*
	 * Store transaction information of xact locking the tuple.
	 *
	 * Note: Cmax is meaningless in this context, so don't set it; this avoids
	 * possibly generating a useless combo CID.  Moreover, if we're locking a
	 * previously updated tuple, it's important to preserve the Cmax.
	 *
	 * Also reset the HOT UPDATE bit, but only if there's no update; otherwise
	 * we would break the HOT chain.
	 */
	tuple->t_data->t_infomask &= ~HEAP_XMAX_BITS;
	tuple->t_data->t_infomask2 &= ~HEAP_KEYS_UPDATED;
	tuple->t_data->t_infomask |= new_infomask;
	tuple->t_data->t_infomask2 |= new_infomask2;
	if (HEAP_XMAX_IS_LOCKED_ONLY(new_infomask))
		HeapTupleHeaderClearHotUpdated(tuple->t_data);
	HeapTupleHeaderSetXmax(tuple->t_data, xid);

	/*
	 * Make sure there is no forward chain link in t_ctid.  Note that in the
	 * cases where the tuple has been updated, we must not overwrite t_ctid,
	 * because it was set by the updater.  Moreover, if the tuple has been
	 * updated, we need to follow the update chain to lock the new versions of
	 * the tuple as well.
	 */
	if (HEAP_XMAX_IS_LOCKED_ONLY(new_infomask))
		tuple->t_data->t_ctid = *tid;

	/* 如有需要，仅清除可见性映射上的 all-frozen 位 */
	if (PageIsAllVisible(page) &&
		visibilitymap_clear(relation, block, vmbuffer,
							VISIBILITYMAP_ALL_FROZEN))
		cleared_all_frozen = true;


	MarkBufferDirty(*buffer);

	/*
	 * XLOG stuff.  You might think that we don't need an XLOG record because
	 * there is no state change worth restoring after a crash.  You would be
	 * wrong however: we have just written either a TransactionId or a
	 * MultiXactId that may never have been seen on disk before, and we need
	 * to make sure that there are XLOG entries covering those ID numbers.
	 * Else the same IDs might be re-used after a crash, which would be
	 * disastrous if this page made it to disk before the crash.  Essentially
	 * we have to enforce the WAL log-before-data rule even in this case.
	 * (Also, in a PITR log-shipping or 2PC environment, we have to have XLOG
	 * entries for everything anyway.)
	 */
	if (RelationNeedsWAL(relation))
	{
		xl_heap_lock xlrec;
		XLogRecPtr	recptr;

		XLogBeginInsert();
		XLogRegisterBuffer(0, *buffer, REGBUF_STANDARD);

		xlrec.offnum = ItemPointerGetOffsetNumber(&tuple->t_self);
		xlrec.xmax = xid;
		xlrec.infobits_set = compute_infobits(new_infomask,
											  tuple->t_data->t_infomask2);
		xlrec.flags = cleared_all_frozen ? XLH_LOCK_ALL_FROZEN_CLEARED : 0;
		XLogRegisterData(&xlrec, SizeOfHeapLock);

		/* 目前不解码行锁，因此无需记录 origin */

		recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_LOCK);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	result = TM_Ok;

out_locked:
	LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);

out_unlocked:
	if (BufferIsValid(vmbuffer))
		ReleaseBuffer(vmbuffer);

	/*
	 * 不要在此处更新可见性映射。锁定元组不会改变可见性信息。
	 */

	/*
	 * 既然我们已经成功地将元组标记为已锁定，我们可以释放 lmgr 元组锁
	 * （如果持有的话）。
	 */
	if (have_tuple_lock)
		UnlockTupleTuplock(relation, tid, mode);

	return result;
}

/*
 * 获取给定元组的重量级锁，为获取其正常的基于 Xmax 的元组锁做准备。
 *
 * have_tuple_lock 既是输入也是输出参数：在输入时，指示锁是否已先前获取
 * （如果已获取，此函数不做任何操作）。如果此函数返回成功，have_tuple_lock
 * 已被翻转为 true。
 *
 * 如果无法获取锁则返回 false；这只可能在 wait_policy 为 Skip 时发生。
 */
static bool
heap_acquire_tuplock(Relation relation, ItemPointer tid, LockTupleMode mode,
					 LockWaitPolicy wait_policy, bool *have_tuple_lock)
{
	if (*have_tuple_lock)
		return true;

	switch (wait_policy)
	{
		case LockWaitBlock:
			LockTupleTuplock(relation, tid, mode);
			break;

		case LockWaitSkip:
			if (!ConditionalLockTupleTuplock(relation, tid, mode, false))
				return false;
			break;

		case LockWaitError:
			if (!ConditionalLockTupleTuplock(relation, tid, mode, log_lock_failures))
				ereport(ERROR,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("could not obtain lock on row in relation \"%s\"",
								RelationGetRelationName(relation))));
			break;
	}
	*have_tuple_lock = true;

	return true;
}

/*
 * 给定原始的 Xmax 和 infomask 集合，以及一个获取某种模式新锁的事务
 * （由 add_to_xmax 标识），计算元组上应使用的新 Xmax 和对应的 infomask 值。
 *
 * 注意，这可能有副作用，比如创建一个新的 MultiXactId。
 *
 * 大多数调用者在调用此函数之前会先调用 HeapTupleSatisfiesUpdate；
 * 如果 xmax 是一个 MultiXactId 但已不再运行，该函数会设置 HEAP_XMAX_INVALID 位。
 * 存在一个竞争条件：MultiXactId 可能在那之后已经完成，但此不常见情况
 * 要么在此处处理，要么在 MultiXactIdExpand 中处理。
 *
 * 当旧的 xmax 是常规 TransactionId 时，也存在类似的竞争条件。我们再次测试
 * TransactionIdIsInProgress 只是为了缩小窗口，但仍有可能最终创建不必要的
 * MultiXactId。幸运的是这是无害的。
 */
static void
compute_new_xmax_infomask(TransactionId xmax, uint16 old_infomask,
						  uint16 old_infomask2, TransactionId add_to_xmax,
						  LockTupleMode mode, bool is_update,
						  TransactionId *result_xmax, uint16 *result_infomask,
						  uint16 *result_infomask2)
{
	TransactionId new_xmax;
	uint16		new_infomask,
				new_infomask2;

	Assert(TransactionIdIsCurrentTransactionId(add_to_xmax));

l5:
	new_infomask = 0;
	new_infomask2 = 0;
	if (old_infomask & HEAP_XMAX_INVALID)
	{
		/*
		 * 没有先前的锁定者；我们只需插入自己的 TransactionId。
		 *
		 * 注意，此情况必须首先检查，因为下面有几个代码块会跳回此处以实现
		 * 某些优化；在这些情况下 old_infomask 可能包含其他脏位，但我们并不
		 * 真正在意。
		 */
		if (is_update)
		{
			new_xmax = add_to_xmax;
			if (mode == LockTupleExclusive)
				new_infomask2 |= HEAP_KEYS_UPDATED;
		}
		else
		{
			new_infomask |= HEAP_XMAX_LOCK_ONLY;
			switch (mode)
			{
				case LockTupleKeyShare:
					new_xmax = add_to_xmax;
					new_infomask |= HEAP_XMAX_KEYSHR_LOCK;
					break;
				case LockTupleShare:
					new_xmax = add_to_xmax;
					new_infomask |= HEAP_XMAX_SHR_LOCK;
					break;
				case LockTupleNoKeyExclusive:
					new_xmax = add_to_xmax;
					new_infomask |= HEAP_XMAX_EXCL_LOCK;
					break;
				case LockTupleExclusive:
					new_xmax = add_to_xmax;
					new_infomask |= HEAP_XMAX_EXCL_LOCK;
					new_infomask2 |= HEAP_KEYS_UPDATED;
					break;
				default:
					new_xmax = InvalidTransactionId;	/* 使编译器静默 */
					elog(ERROR, "invalid lock mode");
			}
		}
	}
	else if (old_infomask & HEAP_XMAX_IS_MULTI)
	{
		MultiXactStatus new_status;

		/*
		 * 目前不允许为 multi 设置 XMAX_COMMITTED，因此交叉检查。
		 */
		Assert(!(old_infomask & HEAP_XMAX_COMMITTED));

		/*
		 * 带有 LOCK_ONLY 但未设置任何锁位的 multixact（即 pg_upgrade 升级的
		 * 共享锁定元组）不可能仍在运行。此检查对通过 pg_upgrade 升级的数据库
		 * 至关重要；MultiXactIdIsRunning 和 MultiXactIdExpand 都假定不会传递
		 * 此类 multi。
		 */
		if (HEAP_LOCKED_UPGRADED(old_infomask))
		{
			old_infomask &= ~HEAP_XMAX_IS_MULTI;
			old_infomask |= HEAP_XMAX_INVALID;
			goto l5;
		}

		/*
		 * 如果 XMAX 已经是一个 MultiXactId，则需要扩展它以包含 add_to_xmax；
		 * 但如果所有成员都是锁定者且已全部消失，则可以去掉 IS_MULTI 位，
		 * 仅仅将 add_to_xmax 设置为唯一的锁定者/更新者。如果所有锁定者都已
		 * 消失且更新者已中止，我们也可以不需要 multi。
		 *
		 * 如果我们不做此检查，调用 GetMultiXactIdMembers 的开销将由
		 * MultiXactIdExpand 支付，因此此检查不会产生额外工作。
		 */
		if (!MultiXactIdIsRunning(xmax, HEAP_XMAX_IS_LOCKED_ONLY(old_infomask)))
		{
			if (HEAP_XMAX_IS_LOCKED_ONLY(old_infomask) ||
				!TransactionIdDidCommit(MultiXactIdGetUpdateXid(xmax,
																old_infomask)))
			{
				/*
				 * 重置这些位并重新开始；否则向下穿过以在下面创建新 multi。
				 */
				old_infomask &= ~HEAP_XMAX_IS_MULTI;
				old_infomask |= HEAP_XMAX_INVALID;
				goto l5;
			}
		}

		new_status = get_mxact_status_for_lock(mode, is_update);

		new_xmax = MultiXactIdExpand((MultiXactId) xmax, add_to_xmax,
									 new_status);
		GetMultiXactIdHintBits(new_xmax, &new_infomask, &new_infomask2);
	}
	else if (old_infomask & HEAP_XMAX_COMMITTED)
	{
		/*
		 * 这是一个已提交的更新，因此需要将其保留为元组的更新者。
		 */
		MultiXactStatus status;
		MultiXactStatus new_status;

		if (old_infomask2 & HEAP_KEYS_UPDATED)
			status = MultiXactStatusUpdate;
		else
			status = MultiXactStatusNoKeyUpdate;

		new_status = get_mxact_status_for_lock(mode, is_update);

		/*
		 * 由于它没有在运行，显然旧更新者不可能与当前更新者相同，
		 * 因此我们不需要像上面代码块中那样检查该情况。
		 */
		new_xmax = MultiXactIdCreate(xmax, status, add_to_xmax, new_status);
		GetMultiXactIdHintBits(new_xmax, &new_infomask, &new_infomask2);
	}
	else if (TransactionIdIsInProgress(xmax))
	{
		/*
		 * 如果 XMAX 是一个有效的正在进行的 TransactionId，那么我们需要
		 * 创建一个新的 MultiXactId，将旧的锁定者或更新者与我们自己的
		 * TransactionId 一起包含进去。
		 */
		MultiXactStatus new_status;
		MultiXactStatus old_status;
		LockTupleMode old_mode;

		if (HEAP_XMAX_IS_LOCKED_ONLY(old_infomask))
		{
			if (HEAP_XMAX_IS_KEYSHR_LOCKED(old_infomask))
				old_status = MultiXactStatusForKeyShare;
			else if (HEAP_XMAX_IS_SHR_LOCKED(old_infomask))
				old_status = MultiXactStatusForShare;
			else if (HEAP_XMAX_IS_EXCL_LOCKED(old_infomask))
			{
				if (old_infomask2 & HEAP_KEYS_UPDATED)
					old_status = MultiXactStatusForUpdate;
				else
					old_status = MultiXactStatusForNoKeyUpdate;
			}
			else
			{
				/*
				 * LOCK_ONLY 只有在页面由 pg_upgrade 升级时才能单独出现。
				 * 但在那种情况下，TransactionIdIsInProgress() 应该返回 false。
				 * 我们假设此时它已不再锁定。
				 */
				elog(WARNING, "LOCK_ONLY found for Xid in progress %u", xmax);
				old_infomask |= HEAP_XMAX_INVALID;
				old_infomask &= ~HEAP_XMAX_LOCK_ONLY;
				goto l5;
			}
		}
		else
		{
			/* 这是一个更新操作，但是哪种类型？ */
			if (old_infomask2 & HEAP_KEYS_UPDATED)
				old_status = MultiXactStatusUpdate;
			else
				old_status = MultiXactStatusNoKeyUpdate;
		}

		old_mode = TUPLOCK_from_mxstatus(old_status);

		/*
		 * 如果要获取的锁与现有锁属于同一个 TransactionId，则有一种可能的
		 * 优化：仅考虑两者中最强的锁作为唯一存在的锁，然后重新开始。
		 */
		if (xmax == add_to_xmax)
		{
			/*
			 * 注意，原始元组不可能已被更新：否则我们不会在此处，因为元组
			 * 将不可见，我们也无法尝试更新它。一个微妙之处在于，此代码也可
			 * 在遍历更新链以锁定元组未来版本时运行。但那时我们也不会在此处，
			 * 因为 add_to_xmax 会与原始更新者不同。
			 */
			Assert(HEAP_XMAX_IS_LOCKED_ONLY(old_infomask));

			/* 获取两者中最强的 */
			if (mode < old_mode)
				mode = old_mode;
			/* 不能触碰 is_update */

			old_infomask |= HEAP_XMAX_INVALID;
			goto l5;
		}

		/* 否则，回退到创建新的 multixact */
		new_status = get_mxact_status_for_lock(mode, is_update);
		new_xmax = MultiXactIdCreate(xmax, old_status,
									 add_to_xmax, new_status);
		GetMultiXactIdHintBits(new_xmax, &new_infomask, &new_infomask2);
	}
	else if (!HEAP_XMAX_IS_LOCKED_ONLY(old_infomask) &&
			 TransactionIdDidCommit(xmax))
	{
		/*
		 * 这是一个已提交的更新，因此需要将其保留为元组的更新者。
		 */
		MultiXactStatus status;
		MultiXactStatus new_status;

		if (old_infomask2 & HEAP_KEYS_UPDATED)
			status = MultiXactStatusUpdate;
		else
			status = MultiXactStatusNoKeyUpdate;

		new_status = get_mxact_status_for_lock(mode, is_update);

		/*
		 * 由于它没有在运行，显然旧更新者不可能与当前更新者相同，
		 * 因此我们不需要像上面代码块中那样检查该情况。
		 */
		new_xmax = MultiXactIdCreate(xmax, status, add_to_xmax, new_status);
		GetMultiXactIdHintBits(new_xmax, &new_infomask, &new_infomask2);
	}
	else
	{
		/*
		 * 当锁定/更新事务在从元组提取 infomask 时正在运行，但在
		 * TransactionIdIsInProgress 运行之前结束了，才会到达这里。
		 * 将其当作从一开始就没有锁定者来处理。
		 */
		old_infomask |= HEAP_XMAX_INVALID;
		goto l5;
	}

	*result_infomask = new_infomask;
	*result_infomask2 = new_infomask2;
	*result_xmax = new_xmax;
}

/*
 * heap_lock_updated_tuple_rec 的子函数。
 *
 * 给定由指定 xid 标识的事务持有的假设 multixact 状态，如果当前事务想要获取
 * 给定模式的锁，它是否需要等待、失败，还是可以直接继续？如果需要等待，
 * 则 *needwait 设置为 true；如果可以继续，则返回 TM_Ok。如果锁已被当前事务
 * 持有，则返回 TM_SelfModified。如果与另一个事务冲突，则返回其他
 * HeapTupleSatisfiesUpdate 返回值。
 *
 * 持有的状态被称为"假设的"，因为它可能对应的是单个 Xid 持有的锁，即不是
 * 真正的 MultiXactId；我们以这种方式表示是为了 API 的简单性。
 */
static TM_Result
test_lockmode_for_conflict(MultiXactStatus status, TransactionId xid,
						   LockTupleMode mode, HeapTuple tup,
						   bool *needwait)
{
	MultiXactStatus wantedstatus;

	*needwait = false;
	wantedstatus = get_mxact_status_for_lock(mode, false);

	/*
	 * Note: we *must* check TransactionIdIsInProgress before
	 * TransactionIdDidAbort/Commit; see comment at top of heapam_visibility.c
	 * for an explanation.
	 */
	if (TransactionIdIsCurrentTransactionId(xid))
	{
		/*
		 * The tuple has already been locked by our own transaction.  This is
		 * very rare but can happen if multiple transactions are trying to
		 * lock an ancient version of the same tuple.
		 */
		return TM_SelfModified;
	}
	else if (TransactionIdIsInProgress(xid))
	{
		/*
		 * If the locking transaction is running, what we do depends on
		 * whether the lock modes conflict: if they do, then we must wait for
		 * it to finish; otherwise we can fall through to lock this tuple
		 * version without waiting.
		 */
		if (DoLockModesConflict(LOCKMODE_from_mxstatus(status),
								LOCKMODE_from_mxstatus(wantedstatus)))
		{
			*needwait = true;
		}

		/*
		 * If we set needwait above, then this value doesn't matter;
		 * otherwise, this value signals to caller that it's okay to proceed.
		 */
		return TM_Ok;
	}
	else if (TransactionIdDidAbort(xid))
		return TM_Ok;
	else if (TransactionIdDidCommit(xid))
	{
		/*
		 * The other transaction committed.  If it was only a locker, then the
		 * lock is completely gone now and we can return success; but if it
		 * was an update, then what we do depends on whether the two lock
		 * modes conflict.  If they conflict, then we must report error to
		 * caller. But if they don't, we can fall through to allow the current
		 * transaction to lock the tuple.
		 *
		 * Note: the reason we worry about ISUPDATE here is because as soon as
		 * a transaction ends, all its locks are gone and meaningless, and
		 * thus we can ignore them; whereas its updates persist.  In the
		 * TransactionIdIsInProgress case, above, we don't need to check
		 * because we know the lock is still "alive" and thus a conflict needs
		 * always be checked.
		 */
		if (!ISUPDATE_from_mxstatus(status))
			return TM_Ok;

		if (DoLockModesConflict(LOCKMODE_from_mxstatus(status),
								LOCKMODE_from_mxstatus(wantedstatus)))
		{
			/* 真糟糕 */
			if (!ItemPointerEquals(&tup->t_self, &tup->t_data->t_ctid))
				return TM_Updated;
			else
				return TM_Deleted;
		}

		return TM_Ok;
	}

	/* 非运行中、非中止、非提交 —— 一定是崩溃了 */
	return TM_Ok;
}


/*
 * heap_lock_updated_tuple 的递归部分
 *
 * 获取 rel 中 tid 指向的元组，并将其标记为由给定的 xid 以给定的模式锁定；
 * 如果此元组已更新，则递归锁定新版本。
 */
static TM_Result
heap_lock_updated_tuple_rec(Relation rel, TransactionId priorXmax,
							const ItemPointerData *tid, TransactionId xid,
							LockTupleMode mode)
{
	TM_Result	result;
	ItemPointerData tupid;
	HeapTupleData mytup;
	Buffer		buf;
	uint16		new_infomask,
				new_infomask2,
				old_infomask,
				old_infomask2;
	TransactionId xmax,
				new_xmax;
	bool		cleared_all_frozen = false;
	bool		pinned_desired_page;
	Buffer		vmbuffer = InvalidBuffer;
	BlockNumber block;

	ItemPointerCopy(tid, &tupid);

	for (;;)
	{
		new_infomask = 0;
		new_xmax = InvalidTransactionId;
		block = ItemPointerGetBlockNumber(&tupid);
		ItemPointerCopy(&tupid, &(mytup.t_self));

		if (!heap_fetch(rel, SnapshotAny, &mytup, &buf, false))
		{
			/*
			 * 如果找不到元组的更新版本，那是因为其创建者事务中止后，
			 * 该版本被 vacuum/prune 清理掉了。因此表现为到达了链的末尾，
			 * 没有需要继续锁定的元组：向调用者返回成功。
			 */
			result = TM_Ok;
			goto out_unlocked;
		}

l4:
		CHECK_FOR_INTERRUPTS();

		/*
		 * 在锁定缓冲区之前，如果看起来有必要，先 pin 可见性映射页。由于我们
		 * 尚未获取锁，其他人可能正在修改此页，因此我们需要在获取锁后重新检查。
		 */
		if (PageIsAllVisible(BufferGetPage(buf)))
		{
			visibilitymap_pin(rel, block, &vmbuffer);
			pinned_desired_page = true;
		}
		else
			pinned_desired_page = false;

		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

		/*
		 * 如果我们没有 pin 可见性映射页，而在忙着锁定缓冲区期间该页变为
		 * 完全可见，则必须解锁并重新锁定，以避免在 I/O 期间持有缓冲区锁。
		 * 这有点遗憾，但希望不常发生。
		 *
		 * 注意：在通过此函数的一些路径中，到达此处时持有 VM 页面的 pin，
		 * 该页面可能与此页面匹配，也可能不匹配。如果此页面不是 all-visible，
		 * 我们不会使用 VM 页面，但会持有该 pin 直到函数结束。
		 */
		if (!pinned_desired_page && PageIsAllVisible(BufferGetPage(buf)))
		{
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			visibilitymap_pin(rel, block, &vmbuffer);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		}

		/*
		 * 将元组 XMIN 与 priorXMAX 比较（如果有的话）。如果到达了链的末尾，
		 * 则完成，返回成功。
		 */
		if (TransactionIdIsValid(priorXmax) &&
			!TransactionIdEquals(HeapTupleHeaderGetXmin(mytup.t_data),
								 priorXmax))
		{
			result = TM_Ok;
			goto out_locked;
		}

		/*
		 * 还要检查 Xmin：如果此元组是由已中止的（子）事务创建的，那么我们已经
		 * 锁定了链中最后一个活跃版本，因此完成，返回成功。
		 */
		if (TransactionIdDidAbort(HeapTupleHeaderGetXmin(mytup.t_data)))
		{
			result = TM_Ok;
			goto out_locked;
		}

		old_infomask = mytup.t_data->t_infomask;
		old_infomask2 = mytup.t_data->t_infomask2;
		xmax = HeapTupleHeaderGetRawXmax(mytup.t_data);

		/*
		 * 如果此元组版本已被某些并发事务更新或锁定，我们的锁模式是否与
		 * 其他事务持有的锁冲突，以及它们的状态如何，决定了我们要执行的操作。
		 */
		if (!(old_infomask & HEAP_XMAX_INVALID))
		{
			TransactionId rawxmax;
			bool		needwait;

			rawxmax = HeapTupleHeaderGetRawXmax(mytup.t_data);
			if (old_infomask & HEAP_XMAX_IS_MULTI)
			{
				int			nmembers;
				int			i;
				MultiXactMember *members;

				/*
				 * 不需要对 pg_upgrade 的元组进行测试：这仅适用于更新链中首个元组
				 * 之后的元组。链中的首个元组可能确实是 9.2 格式且经 pg_upgrade
				 * 的锁定元组，但该元组已由调用者锁定，而非由我们锁定；后续元组
				 * 不可能如此，因为调用者必然在 pg_upgrade 本身之后获取了快照。
				 */
				Assert(!HEAP_LOCKED_UPGRADED(mytup.t_data->t_infomask));

				nmembers = GetMultiXactIdMembers(rawxmax, &members, false,
												 HEAP_XMAX_IS_LOCKED_ONLY(old_infomask));
				for (i = 0; i < nmembers; i++)
				{
					result = test_lockmode_for_conflict(members[i].status,
														members[i].xid,
														mode,
														&mytup,
														&needwait);

					/*
					 * 如果此元组在我们之前的迭代中已被我们自己锁定（例如，
					 * 由于 xmax 变化，heap_lock_tuple 被迫重新启动锁定循环），
					 * 那么我们在此元组版本上已持有锁，无需执行任何操作；这也
					 * 不是错误状况。我们只需跳过此元组，继续锁定更新链中的
					 * 下一个版本。
					 */
					if (result == TM_SelfModified)
					{
						pfree(members);
						goto next;
					}

					if (needwait)
					{
						LockBuffer(buf, BUFFER_LOCK_UNLOCK);
						XactLockTableWait(members[i].xid, rel,
										  &mytup.t_self,
										  XLTW_LockUpdated);
						pfree(members);
						goto l4;
					}
					if (result != TM_Ok)
					{
						pfree(members);
						goto out_locked;
					}
				}
				if (members)
					pfree(members);
			}
			else
			{
				MultiXactStatus status;

				/*
				 * 对于非 multi 的 Xmax，首先需要使用 infomask 位来计算
				 * 对应的 MultiXactStatus。
				 */
				if (HEAP_XMAX_IS_LOCKED_ONLY(old_infomask))
				{
					if (HEAP_XMAX_IS_KEYSHR_LOCKED(old_infomask))
						status = MultiXactStatusForKeyShare;
					else if (HEAP_XMAX_IS_SHR_LOCKED(old_infomask))
						status = MultiXactStatusForShare;
					else if (HEAP_XMAX_IS_EXCL_LOCKED(old_infomask))
					{
						if (old_infomask2 & HEAP_KEYS_UPDATED)
							status = MultiXactStatusForUpdate;
						else
							status = MultiXactStatusForNoKeyUpdate;
					}
					else
					{
						/*
						 * 单独出现的 LOCK_ONLY（在旧集群中标记为共享锁定的
						 * pg_upgrade 元组）不应出现在更新链中间。
						 */
						elog(ERROR, "invalid lock status in tuple");
					}
				}
				else
				{
					/* 这是一个更新操作，但是哪种类型？ */
					if (old_infomask2 & HEAP_KEYS_UPDATED)
						status = MultiXactStatusUpdate;
					else
						status = MultiXactStatusNoKeyUpdate;
				}

				result = test_lockmode_for_conflict(status, rawxmax, mode,
													&mytup, &needwait);

				/*
				 * 如果此元组在我们之前的迭代中已被我们自己锁定（例如，由于
				 * xmax 变化，heap_lock_tuple 被迫重新启动锁定循环），那么
				 * 我们在此元组版本上已持有锁，无需执行任何操作；这也不是错误
				 * 状况。我们只需跳过此元组，继续锁定更新链中的下一个版本。
				 */
				if (result == TM_SelfModified)
					goto next;

				if (needwait)
				{
					LockBuffer(buf, BUFFER_LOCK_UNLOCK);
					XactLockTableWait(rawxmax, rel, &mytup.t_self,
									  XLTW_LockUpdated);
					goto l4;
				}
				if (result != TM_Ok)
				{
					goto out_locked;
				}
			}
		}

		/* 计算元组的新 Xmax 和 infomask 值... */
		compute_new_xmax_infomask(xmax, old_infomask, mytup.t_data->t_infomask2,
								  xid, mode, false,
								  &new_xmax, &new_infomask, &new_infomask2);

		if (PageIsAllVisible(BufferGetPage(buf)) &&
			visibilitymap_clear(rel, block, vmbuffer,
								VISIBILITYMAP_ALL_FROZEN))
			cleared_all_frozen = true;

		START_CRIT_SECTION();

		/* ... 然后设置它们 */
		HeapTupleHeaderSetXmax(mytup.t_data, new_xmax);
		mytup.t_data->t_infomask &= ~HEAP_XMAX_BITS;
		mytup.t_data->t_infomask2 &= ~HEAP_KEYS_UPDATED;
		mytup.t_data->t_infomask |= new_infomask;
		mytup.t_data->t_infomask2 |= new_infomask2;

		MarkBufferDirty(buf);

		/* XLOG 记录 */
		if (RelationNeedsWAL(rel))
		{
			xl_heap_lock_updated xlrec;
			XLogRecPtr	recptr;
			Page		page = BufferGetPage(buf);

			XLogBeginInsert();
			XLogRegisterBuffer(0, buf, REGBUF_STANDARD);

			xlrec.offnum = ItemPointerGetOffsetNumber(&mytup.t_self);
			xlrec.xmax = new_xmax;
			xlrec.infobits_set = compute_infobits(new_infomask, new_infomask2);
			xlrec.flags =
				cleared_all_frozen ? XLH_LOCK_ALL_FROZEN_CLEARED : 0;

			XLogRegisterData(&xlrec, SizeOfHeapLockUpdated);

			recptr = XLogInsert(RM_HEAP2_ID, XLOG_HEAP2_LOCK_UPDATED);

			PageSetLSN(page, recptr);
		}

		END_CRIT_SECTION();

next:
		/* 如果找到更新链的末尾，我们就完成了。 */
		if (mytup.t_data->t_infomask & HEAP_XMAX_INVALID ||
			HeapTupleHeaderIndicatesMovedPartitions(mytup.t_data) ||
			ItemPointerEquals(&mytup.t_self, &mytup.t_data->t_ctid) ||
			HeapTupleHeaderIsOnlyLocked(mytup.t_data))
		{
			result = TM_Ok;
			goto out_locked;
		}

		/* 尾递归 */
		priorXmax = HeapTupleHeaderGetUpdateXid(mytup.t_data);
		ItemPointerCopy(&(mytup.t_data->t_ctid), &tupid);
		UnlockReleaseBuffer(buf);
	}

	result = TM_Ok;

out_locked:
	UnlockReleaseBuffer(buf);

out_unlocked:
	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);

	return result;
}

/*
 * heap_lock_updated_tuple
 *		在锁定已更新元组时跟随更新链，获取更新版本上的锁（行标记）。
 *
 * 'prior_infomask'、'prior_raw_xmax' 和 'prior_ctid' 是来自初始元组的对应
 * 字段。我们将从 'prior_ctid' 指向的元组开始锁定。注意：此函数不会锁定
 * 初始元组本身。
 *
 * 此函数不检查可见性，它只是无条件地将元组标记为已锁定。如果更新链中的
 * 任何元组正在被并发删除（或更新且键被修改），则等待执行该操作的事务完成。
 *
 * 注意，当必须等待其他事务释放锁时，我们不会像 heap_lock_tuple 那样在遍历的
 * 元组上获取重量级元组锁。原因是多个事务同时遍历链的情况可能非常罕见，
 * 以至于饥饿风险不大：到达此处的前提条件之一是使用的快照早于创建此元组的
 * 更新（因为我们从元组的早期版本开始），但与此同时，此事务不能使用可重复读
 * 或串行化隔离级别，因为那将导致串行化失败。
 */
static TM_Result
heap_lock_updated_tuple(Relation rel,
						uint16 prior_infomask,
						TransactionId prior_raw_xmax,
						const ItemPointerData *prior_ctid,
						TransactionId xid, LockTupleMode mode)
{
	INJECTION_POINT("heap_lock_updated_tuple", NULL);

	/*
	 * 如果元组已移动到另一个分区（实际上相当于删除），则在此停止。
	 */
	if (!ItemPointerIndicatesMovedPartitions(prior_ctid))
	{
		TransactionId prior_xmax;

		/*
		 * 如果这是当前事务中第一个可能涉及 multixact 的操作，设置每个后台进程
		 * 的 OldestMemberMXactId。我们可以确信该事务永远不会成为任何比它更早的
		 * MultiXactId 的成员。（即使我们最终只使用自己的 TransactionId，也必须
		 * 这样做，因为其他后台进程可能随后立即将我们的 XID 合并到 MultiXact 中。）
		 */
		MultiXactIdSetOldestMember();

		prior_xmax = (prior_infomask & HEAP_XMAX_IS_MULTI) ?
			MultiXactIdGetUpdateXid(prior_raw_xmax, prior_infomask) : prior_raw_xmax;
		return heap_lock_updated_tuple_rec(rel, prior_xmax, prior_ctid, xid, mode);
	}

	/* 无需锁定 */
	return TM_Ok;
}

/*
 *	heap_finish_speculative - 将投机插入标记为成功
 *
 * 要成功完成投机插入，必须清除元组中的投机令牌（speculative token）。
 * 为此，包含投机令牌值的 t_ctid 字段被就地修改为指向元组自身，
 * 这是新插入普通元组的特征。
 *
 * 注意：在不完成或中止投机插入的情况下提交是不允许的。我们可以隐式地将
 * 已提交事务的投机元组视为已完成，但这样就必须准备处理已提交元组上的投机
 * 令牌。这不会很难——没有人会查看 xmax 无效的元组的 ctid 字段——但
 * 在完成时清除令牌的成本也不高。显式的确认 WAL 记录也使逻辑解码更简单。
 */
void
heap_finish_speculative(Relation relation, ItemPointer tid)
{
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleHeader htup;

	buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	page = (Page) BufferGetPage(buffer);

	offnum = ItemPointerGetOffsetNumber(tid);
	if (PageGetMaxOffsetNumber(page) >= offnum)
		lp = PageGetItemId(page, offnum);

	if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsNormal(lp))
		elog(ERROR, "invalid lp");

	htup = (HeapTupleHeader) PageGetItem(page, lp);

	/* 从此处起直到更改记录完毕之前，禁止 EREPORT(ERROR) */
	START_CRIT_SECTION();

	Assert(HeapTupleHeaderIsSpeculative(htup));

	MarkBufferDirty(buffer);

	/*
	 * 将投机插入令牌替换为真实的 t_ctid，像普通元组一样指向自身。
	 */
	htup->t_ctid = *tid;

	/* XLOG 记录 */
	if (RelationNeedsWAL(relation))
	{
		xl_heap_confirm xlrec;
		XLogRecPtr	recptr;

		xlrec.offnum = ItemPointerGetOffsetNumber(tid);

		XLogBeginInsert();

		/* 我们希望对此记录应用与普通插入相同的过滤 */
		XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

		XLogRegisterData(&xlrec, SizeOfHeapConfirm);
		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

		recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_CONFIRM);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buffer);
}

/*
 *	heap_abort_speculative - 杀死投机插入的元组
 *
 * 通过将 xmin 设置为无效，将在同一命令中投机插入的元组标记为已死。
 * 这使得所有事务（包括我们自己的事务）都能立即将其视为已死。特别是，
 * 这使得 HeapTupleSatisfiesDirty() 将元组视为已死，从而其他后台进程在插入
 * 重复键值时不必等待我们整个事务完成（它们只需等待我们的投机插入完成即可）。
 *
 * 杀死元组可以防止"非原则性死锁"，这是由非用户可见的相互依赖引起的死锁。
 * 根据定义，非原则性死锁不能通过用户在客户端代码中重新排序锁获取来防止，
 * 因为实现级别的锁获取不在用户的直接控制之下。如果投机插入者不采取此预防措施，
 * 在高并发下它们可能彼此死锁，这是不可接受的。
 *
 * 这与 heap_delete 有些冗余，但我们更倾向于使用具有简化需求的专用函数。
 * 注意，此函数也用于删除投机插入期间创建的 TOAST 元组。
 *
 * 此函数不影响逻辑解码，因为逻辑解码只查看确认记录。
 */
void
heap_abort_speculative(Relation relation, ItemPointer tid)
{
	TransactionId xid = GetCurrentTransactionId();
	ItemId		lp;
	HeapTupleData tp;
	Page		page;
	BlockNumber block;
	Buffer		buffer;

	Assert(ItemPointerIsValid(tid));

	block = ItemPointerGetBlockNumber(tid);
	buffer = ReadBuffer(relation, block);
	page = BufferGetPage(buffer);

	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	/*
	 * 页面不可能是 all-visible，因为我们刚刚向其中插入了数据且仍在运行。
	 */
	Assert(!PageIsAllVisible(page));

	lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
	Assert(ItemIdIsNormal(lp));

	tp.t_tableOid = RelationGetRelid(relation);
	tp.t_data = (HeapTupleHeader) PageGetItem(page, lp);
	tp.t_len = ItemIdGetLength(lp);
	tp.t_self = *tid;

	/*
	 * 健全性检查：元组确实是一个由我们插入的投机插入元组。
	 */
	if (tp.t_data->t_choice.t_heap.t_xmin != xid)
		elog(ERROR, "attempted to kill a tuple inserted by another transaction");
	if (!(IsToastRelation(relation) || HeapTupleHeaderIsSpeculative(tp.t_data)))
		elog(ERROR, "attempted to kill a non-speculative tuple");
	Assert(!HeapTupleHeaderIsHeapOnly(tp.t_data));

	/*
	 * 无需在此检查串行化冲突。也永远不需要组合 CID。无需提取副本标识，
	 * 也无需对 infomask 位做任何特殊处理。
	 */

	START_CRIT_SECTION();

	/*
	 * 元组将立即变为 DEAD。通过将 xmin 设置为 TransactionXmin 来标记此页面
	 * 为裁剪候选。虽然不是立即可裁剪的，但这是我们可以廉价确定的最老的 xid，
	 * 并且对回卷/早于表的 relfrozenxid 是安全的。为防止新关系的 relfrozenxid
	 * 比 TransactionXmin 更新的罕见情况，如果确实如此则使用 relfrozenxid
	 * （vacuum 随后不能将 relfrozenxid 移到 TransactionXmin 之后，所以此处
	 * 不存在竞争）。
	 */
	Assert(TransactionIdIsValid(TransactionXmin));
	{
		TransactionId relfrozenxid = relation->rd_rel->relfrozenxid;
		TransactionId prune_xid;

		if (TransactionIdPrecedes(TransactionXmin, relfrozenxid))
			prune_xid = relfrozenxid;
		else
			prune_xid = TransactionXmin;
		PageSetPrunable(page, prune_xid);
	}

	/* 存储删除元组的事务信息 */
	tp.t_data->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
	tp.t_data->t_infomask2 &= ~HEAP_KEYS_UPDATED;

	/*
	 * 将元组头的 xmin 设置为 InvalidTransactionId。这使元组立即对所有事务
	 * 不可见。（特别是对等待投机令牌、稍后被唤醒的事务。）
	 */
	HeapTupleHeaderSetXmin(tp.t_data, InvalidTransactionId);

	/* 同时清除投机插入令牌 */
	tp.t_data->t_ctid = tp.t_self;

	MarkBufferDirty(buffer);

	/*
	 * XLOG stuff
	 *
	 * The WAL records generated here match heap_delete().  The same recovery
	 * routines are used.
	 */
	if (RelationNeedsWAL(relation))
	{
		xl_heap_delete xlrec;
		XLogRecPtr	recptr;

		xlrec.flags = XLH_DELETE_IS_SUPER;
		xlrec.infobits_set = compute_infobits(tp.t_data->t_infomask,
											  tp.t_data->t_infomask2);
		xlrec.offnum = ItemPointerGetOffsetNumber(&tp.t_self);
		xlrec.xmax = xid;

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfHeapDelete);
		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

		/* 不记录副本标识和复制源 */

		recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_DELETE);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	if (HeapTupleHasExternal(&tp))
	{
		Assert(!IsToastRelation(relation));
		heap_toast_delete(relation, &tp, true);
	}

	/*
	 * 永远不需要标记元组作废，因为系统目录不支持投机插入。
	 */

	/* 现在可以释放缓冲区了 */
	ReleaseBuffer(buffer);

	/* 计数删除，因为我们也计数了插入 */
	pgstat_count_heap_delete(relation);
}

/*
 * heap_inplace_lock - 保护就地更新免受并发 heap_update() 的影响
 *
 * 评估元组状态是否与无键更新兼容。当前事务的行标记是没有问题的，来自任何
 * 事务的 KEY SHARE 锁也是。如果兼容，返回 true，缓冲区处于排他锁定状态，
 * 调用者必须通过调用 heap_inplace_update_and_unlock()、heap_inplace_unlock()
 * 或抛出错误来释放该锁。否则，调用 release_callback(arg)，等待阻塞事务结束，
 * 然后返回 false。
 *
 * 由于此函数用于系统目录，且 SERIALIZABLE 不涵盖 DDL，因此不保证任何特定
 * 的谓词锁定。
 *
 * 可以修改此函数使其对正在删除的元组也返回 true。所有就地更新者都持有与
 * DROP 冲突的锁。如果有显式的 "DELETE FROM pg_class" 正在进行，我们会像
 * 等待更新一样等待它。
 *
 * 就地更新字段的读取者期望对这些字段的更改是持久的。例如，
 * vac_truncate_clog() 通过目录快照从 pg_database 元组中读取 datfrozenxid。
 * 未来的快照绝不能为同一数据库 OID 返回更低的 datfrozenxid（在
 * FullTransactionIdPrecedes() 意义上更低）。我们之所以能达到这一点，是因为
 * 在持有缓冲区锁期间，元组的任何更新都无法开始。在类似
 * BEGIN;GRANT;CREATE INDEX;COMMIT 的情况下，我们正在就地更新一个仅对此事务
 * 可见的元组。ROLLBACK 是可以接受丢失就地更新的一种情况。（在 ROLLBACK 时
 * 恢复 relhasindex=false 是可以的，因为任何并发的 CREATE INDEX 会被阻塞，
 * 然后就地更新已提交的元组。）
 *
 * 原则上，我们可以通过覆盖更新链中的每个元组来避免等待。读取者的预期允许
 * 仅在以下情况下更新元组：它已中止、是链的尾部、或者我们已经更新了其 t_ctid
 * 引用的元组。因此，我们需要从尾部到头部依次覆盖元组。这意味着要么
 * (a) 在一个临界区中修改所有元组，要么 (b) 接受部分完成的可能性。
 * relfrozenxid 更新的部分完成会产生奇怪的结果：表的下一次 VACUUM 可能在
 * vacuum_get_cutoffs() 和完成之间看到表的 relfrozenxid 向前移动。
 */
bool
heap_inplace_lock(Relation relation,
				  HeapTuple oldtup_ptr, Buffer buffer,
				  void (*release_callback) (void *), void *arg)
{
	HeapTupleData oldtup = *oldtup_ptr; /* 最小化与 heap_update() 的差异 */
	TM_Result	result;
	bool		ret;

#ifdef USE_ASSERT_CHECKING
	if (RelationGetRelid(relation) == RelationRelationId)
		check_inplace_rel_lock(oldtup_ptr);
#endif

	Assert(BufferIsValid(buffer));

	/*
	 * 如有必要，注册共享缓存失效信息。在此步骤与 LockTuple() 之间，
	 * 其他会话可能会完成对此元组的就地更新。由于就地更新不改变缓存键，
	 * 这是无害的。
	 *
	 * 虽然在确认可以返回 true 后再注册失效信息很诱人，但以下障碍阻止了
	 * 这种重新排序。注册失效信息可能会触发 CatalogCacheInitializeCache()，
	 * 该函数会锁定 "buffer"。如果在我们自己的 LockBuffer() 之后执行，
	 * 将无限期挂起。因此，我们必须在 LockBuffer() 之前注册失效信息。
	 */
	CacheInvalidateHeapTupleInplace(relation, oldtup_ptr);

	LockTuple(relation, &oldtup.t_self, InplaceUpdateTupleLock);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	/*----------
	 * 像 heap_update() 那样解释 HeapTupleSatisfiesUpdate()，不同之处在于：
	 *
	 * - 无条件等待
	 * - 已在上方锁定元组，因为 inplace 无条件需要此锁
	 * - 等待后不重新检查头部：更简单地推迟到下一次迭代
	 * - 即使更新者中止也不尝试继续：同理
	 * - 不进行交叉检查
	 */
	result = HeapTupleSatisfiesUpdate(&oldtup, GetCurrentCommandId(false),
									  buffer);

	if (result == TM_Invisible)
	{
		/* 没有已知的方式会导致此情况 */
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg_internal("attempted to overwrite invisible tuple")));
	}
	else if (result == TM_SelfModified)
	{
		/*
		 * 如果表达式愚蠢到调用例如 SELECT ... FROM pg_class FOR SHARE，
		 * CREATE INDEX 可能会到达此处。其他 SQL 语句的 C 代码在同一行的
		 * heap_update() 之后，如果没有介入的 CommandCounterIncrement()，
		 * 也可能到达此处。
		 */
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("tuple to be updated was already modified by an operation triggered by the current command")));
	}
	else if (result == TM_BeingModified)
	{
		TransactionId xwait;
		uint16		infomask;

		xwait = HeapTupleHeaderGetRawXmax(oldtup.t_data);
		infomask = oldtup.t_data->t_infomask;

		if (infomask & HEAP_XMAX_IS_MULTI)
		{
			LockTupleMode lockmode = LockTupleNoKeyExclusive;
			MultiXactStatus mxact_status = MultiXactStatusNoKeyUpdate;
			int			remain;

			if (DoesMultiXactIdConflict((MultiXactId) xwait, infomask,
										lockmode, NULL))
			{
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
				release_callback(arg);
				ret = false;
				MultiXactIdWait((MultiXactId) xwait, mxact_status, infomask,
								relation, &oldtup.t_self, XLTW_Update,
								&remain);
			}
			else
				ret = true;
		}
		else if (TransactionIdIsCurrentTransactionId(xwait))
			ret = true;
		else if (HEAP_XMAX_IS_KEYSHR_LOCKED(infomask))
			ret = true;
		else
		{
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			release_callback(arg);
			ret = false;
			XactLockTableWait(xwait, relation, &oldtup.t_self,
							  XLTW_Update);
		}
	}
	else
	{
		ret = (result == TM_Ok);
		if (!ret)
		{
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			release_callback(arg);
		}
	}

	/*
	 * GetCatalogSnapshot() 依赖失效消息来知道何时获取新快照。xwait 的 COMMIT
	 * 负责发送失效消息。我们没有获取足够阻止其发送的重量级锁，因此必须获取
	 * 新快照以确保后续尝试有公平的机会。虽然在 xwait 中止时我们不需要这样做，
	 * 但不必费心优化该情况。
	 */
	if (!ret)
	{
		UnlockTuple(relation, &oldtup.t_self, InplaceUpdateTupleLock);
		ForgetInplace_Inval();
		InvalidateCatalogSnapshot();
	}
	return ret;
}

/*
 * heap_inplace_update_and_unlock - systable_inplace_update_finish 的核心
 *
 * 元组不能改变大小，因此其头部字段和空值位图（如果有）也不会改变。
 *
 * 由于我们持有 LOCKTAG_TUPLE，没有更新者持有此元组的本地副本。
 */
void
heap_inplace_update_and_unlock(Relation relation,
							   HeapTuple oldtup, HeapTuple tuple,
							   Buffer buffer)
{
	HeapTupleHeader htup = oldtup->t_data;
	uint32		oldlen;
	uint32		newlen;
	char	   *dst;
	char	   *src;
	int			nmsgs = 0;
	SharedInvalidationMessage *invalMessages = NULL;
	bool		RelcacheInitFileInval = false;

	Assert(ItemPointerEquals(&oldtup->t_self, &tuple->t_self));
	oldlen = oldtup->t_len - htup->t_hoff;
	newlen = tuple->t_len - tuple->t_data->t_hoff;
	if (oldlen != newlen || htup->t_hoff != tuple->t_data->t_hoff)
		elog(ERROR, "wrong tuple length");

	dst = (char *) htup + htup->t_hoff;
	src = (char *) tuple->t_data + tuple->t_data->t_hoff;

	/* 与 RecordTransactionCommit() 类似，仅在需要时记录 */
	if (XLogStandbyInfoActive())
		nmsgs = inplaceGetInvalidationMessages(&invalMessages,
											   &RelcacheInitFileInval);

	/*
	 * Unlink relcache init files as needed.  If unlinking, acquire
	 * RelCacheInitLock until after associated invalidations.  By doing this
	 * in advance, if we checkpoint and then crash between inplace
	 * XLogInsert() and inval, we don't rely on StartupXLOG() ->
	 * RelationCacheInitFileRemove().  That uses elevel==LOG, so replay would
	 * neglect to PANIC on EIO.
	 */
	PreInplace_Inval();

	/*----------
	 * NO EREPORT(ERROR) from here till changes are complete
	 *
	 * Our buffer lock won't stop a reader having already pinned and checked
	 * visibility for this tuple.  Hence, we write WAL first, then mutate the
	 * buffer.  Like in MarkBufferDirtyHint() or RecordTransactionCommit(),
	 * checkpoint delay makes that acceptable.  With the usual order of
	 * changes, a crash after memcpy() and before XLogInsert() could allow
	 * datfrozenxid to overtake relfrozenxid:
	 *
	 * ["D" is a VACUUM (ONLY_DATABASE_STATS)]
	 * ["R" is a VACUUM tbl]
	 * D: vac_update_datfrozenxid() -> systable_beginscan(pg_class)
	 * D: systable_getnext() returns pg_class tuple of tbl
	 * R: memcpy() into pg_class tuple of tbl
	 * D: raise pg_database.datfrozenxid, XLogInsert(), finish
	 * [crash]
	 * [recovery restores datfrozenxid w/o relfrozenxid]
	 *
	 * Mimic MarkBufferDirtyHint() subroutine XLogSaveBufferForHint().
	 * Specifically, use DELAY_CHKPT_START, and copy the buffer to the stack.
	 * The stack copy facilitates a FPI of the post-mutation block before we
	 * accept other sessions seeing it.  DELAY_CHKPT_START allows us to
	 * XLogInsert() before MarkBufferDirty().  Since XLogSaveBufferForHint()
	 * can operate under BUFFER_LOCK_SHARED, it can't avoid DELAY_CHKPT_START.
	 * This function, however, likely could avoid it with the following order
	 * of operations: MarkBufferDirty(), XLogInsert(), memcpy().  Opt to use
	 * DELAY_CHKPT_START here, too, as a way to have fewer distinct code
	 * patterns to analyze.  Inplace update isn't so frequent that it should
	 * pursue the small optimization of skipping DELAY_CHKPT_START.
	 */
	Assert((MyProc->delayChkptFlags & DELAY_CHKPT_START) == 0);
	START_CRIT_SECTION();
	MyProc->delayChkptFlags |= DELAY_CHKPT_START;

	/* XLOG 记录 */
	if (RelationNeedsWAL(relation))
	{
		xl_heap_inplace xlrec;
		PGAlignedBlock copied_buffer;
		char	   *origdata = (char *) BufferGetBlock(buffer);
		Page		page = BufferGetPage(buffer);
		uint16		lower = ((PageHeader) page)->pd_lower;
		uint16		upper = ((PageHeader) page)->pd_upper;
		uintptr_t	dst_offset_in_block;
		RelFileLocator rlocator;
		ForkNumber	forkno;
		BlockNumber blkno;
		XLogRecPtr	recptr;

		xlrec.offnum = ItemPointerGetOffsetNumber(&tuple->t_self);
		xlrec.dbId = MyDatabaseId;
		xlrec.tsId = MyDatabaseTableSpace;
		xlrec.relcacheInitFileInval = RelcacheInitFileInval;
		xlrec.nmsgs = nmsgs;

		XLogBeginInsert();
		XLogRegisterData(&xlrec, MinSizeOfHeapInplace);
		if (nmsgs != 0)
			XLogRegisterData(invalMessages,
							 nmsgs * sizeof(SharedInvalidationMessage));

		/* 注册与变更后缓冲区内容匹配的块 */
		memcpy(copied_buffer.data, origdata, lower);
		memcpy(copied_buffer.data + upper, origdata + upper, BLCKSZ - upper);
		dst_offset_in_block = dst - origdata;
		memcpy(copied_buffer.data + dst_offset_in_block, src, newlen);
		BufferGetTag(buffer, &rlocator, &forkno, &blkno);
		Assert(forkno == MAIN_FORKNUM);
		XLogRegisterBlock(0, &rlocator, forkno, blkno, copied_buffer.data,
						  REGBUF_STANDARD);
		XLogRegisterBufData(0, src, newlen);

		/* 目前不解码就地更新，不记录 origin */

		recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_INPLACE);

		PageSetLSN(page, recptr);
	}

	memcpy(dst, src, newlen);

	MarkBufferDirty(buffer);

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	/*
	 * Send invalidations to shared queue.  SearchSysCacheLocked1() assumes we
	 * do this before UnlockTuple().
	 */
	AtInplace_Inval();

	MyProc->delayChkptFlags &= ~DELAY_CHKPT_START;
	END_CRIT_SECTION();
	UnlockTuple(relation, &tuple->t_self, InplaceUpdateTupleLock);

	AcceptInvalidationMessages();	/* 对刚发送的失效消息进行本地处理 */

	/*
	 * Queue a transactional inval, for logical decoding and for third-party
	 * code that might have been relying on it since long before inplace
	 * update adopted immediate invalidation.  See README.tuplock section
	 * "Reading inplace-updated columns" for logical decoding details.
	 */
	if (!IsBootstrapProcessingMode())
		CacheInvalidateHeapTuple(relation, tuple, NULL);
}

/*
 * heap_inplace_unlock - reverse of heap_inplace_lock
 */
void
heap_inplace_unlock(Relation relation,
					HeapTuple oldtup, Buffer buffer)
{
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	UnlockTuple(relation, &oldtup->t_self, InplaceUpdateTupleLock);
	ForgetInplace_Inval();
}

#define		FRM_NOOP				0x0001
#define		FRM_INVALIDATE_XMAX		0x0002
#define		FRM_RETURN_IS_XID		0x0004
#define		FRM_RETURN_IS_MULTI		0x0008
#define		FRM_MARK_COMMITTED		0x0010

/*
 * FreezeMultiXactId
 *		确定当元组被 MultiXactId 标记时，冻结期间应采取的操作。
 *
 * "flags" 是输出值，用于告知调用者返回时应执行的操作。
 * "pagefrz" 是输入/输出值，用于管理页级别的冻结。
 *
 * 可在 "flags" 中设置的可能取值：
 * FRM_NOOP
 *		不执行任何操作 —— 保持现有 Xmax
 * FRM_INVALIDATE_XMAX
 *		将 Xmax 标记为 InvalidTransactionId 并设置 XMAX_INVALID 标志。
 * FRM_RETURN_IS_XID
 *		返回值为单个更新 Xid，用作 xmax。
 * FRM_MARK_COMMITTED
 *		可将 Xmax 标记为 HEAP_XMAX_COMMITTED
 * FRM_RETURN_IS_MULTI
 *		返回值是一个新的 MultiXactId，用作新的 Xmax。
 *		（调用者必须通过 GetMultiXactIdHintBits 获取正确的 infomask 位）
 *
 * 调用者将页面冻结的控制权委托给我们。实际上，除非指示进行 FRM_NOOP 处理，
 * 否则我们总是强制冻结调用者的页面。我们帮助调用者确保 XIDs < FreezeLimit
 * 和 MXIDs < MultiXactCutoff 的值不会被残留。我们自由选择处理每个 Multi
 * 的时机和方式，从不违反冻结的截止条件。
 *
 * 主动处理 Multi（相对于冻结 XID 的时间线）有助于将 MultiXact 成员 SLRU
 * 缓冲区未命中降到最低。对我们而言，短期内通过积极处理也能避免 SLRU
 * 缓冲区未命中，从而降低成本。
 *
 * 注意：当设置 FRM_RETURN_IS_MULTI 时，会创建 _新_ 的 MultiXactId，但仅在
 * FreezeLimit 和/或 MultiXactCutoff 截止条件让我们别无选择时才这样做。
 * 这通常可以推迟，通常足以完全避免。原则上应避免在 VACUUM 期间分配新的
 * Multi，因为只有 VACUUM 才能推进 relminmxid，所以在此处分配新的 Multi
 * 会带来特殊的风险。
 *
 * 注意：当我们未强制进行页级冻结时，调用者必须使用
 * heap_tuple_should_freeze 维护 "no freeze" NewRelfrozenXid/NewRelminMxid
 * 跟踪器。
 *
 * 注意：当我们已经强制进行页级冻结时，调用者应避免不必要地调用
 * heap_tuple_should_freeze，因为这可能会导致与我们特意通过冻结来避免的
 * 相同的 SLRU 缓冲区未命中。
 */
static TransactionId
FreezeMultiXactId(MultiXactId multi, uint16 t_infomask,
				  const struct VacuumCutoffs *cutoffs, uint16 *flags,
				  HeapPageFreeze *pagefrz)
{
	TransactionId newxmax;
	MultiXactMember *members;
	int			nmembers;
	bool		need_replace;
	int			nnewmembers;
	MultiXactMember *newmembers;
	bool		has_lockers;
	TransactionId update_xid;
	bool		update_committed;
	TransactionId FreezePageRelfrozenXid;

	*flags = 0;

	/* 我们只应在 Multis 中被调用 */
	Assert(t_infomask & HEAP_XMAX_IS_MULTI);

	if (!MultiXactIdIsValid(multi) ||
		HEAP_LOCKED_UPGRADED(t_infomask))
	{
		*flags |= FRM_INVALIDATE_XMAX;
		pagefrz->freeze_required = true;
		return InvalidTransactionId;
	}
	else if (MultiXactIdPrecedes(multi, cutoffs->relminmxid))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("found multixact %u from before relminmxid %u",
								 multi, cutoffs->relminmxid)));
	else if (MultiXactIdPrecedes(multi, cutoffs->OldestMxact))
	{
		TransactionId update_xact;

		/*
		 * 这个旧的 multi 不可能有仍在运行的成员，但还是要验证一下以防万一。
		 * 如果它只是一个锁持有者，可以不加考虑地移除；但如果它包含一个更新，
		 * 我们可能需要保留它。
		 */
		if (MultiXactIdIsRunning(multi,
								 HEAP_XMAX_IS_LOCKED_ONLY(t_infomask)))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("multixact %u from before multi freeze cutoff %u found to be still running",
									 multi, cutoffs->OldestMxact)));

		if (HEAP_XMAX_IS_LOCKED_ONLY(t_infomask))
		{
			*flags |= FRM_INVALIDATE_XMAX;
			pagefrz->freeze_required = true;
			return InvalidTransactionId;
		}

		/* 用其更新者的单个 XID 替换 multi？ */
		update_xact = MultiXactIdGetUpdateXid(multi, t_infomask);
		if (TransactionIdPrecedes(update_xact, cutoffs->relfrozenxid))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("multixact %u contains update XID %u from before relfrozenxid %u",
									 multi, update_xact,
									 cutoffs->relfrozenxid)));
		else if (TransactionIdPrecedes(update_xact, cutoffs->OldestXmin))
		{
			/*
			 * 更新者 XID 必定已经中止（否则元组会被清除掉，因为更新者 XID
			 * < OldestXmin）。直接移除 xmax。
			 */
			if (TransactionIdDidCommit(update_xact))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg_internal("multixact %u contains committed update XID %u from before removable cutoff %u",
										 multi, update_xact,
										 cutoffs->OldestXmin)));
			*flags |= FRM_INVALIDATE_XMAX;
			pagefrz->freeze_required = true;
			return InvalidTransactionId;
		}

		/* 必须将更新者 XID 保留为新 xmax */
		*flags |= FRM_RETURN_IS_XID;
		pagefrz->freeze_required = true;
		return update_xact;
	}

	/*
	 * Some member(s) of this Multi may be below FreezeLimit xid cutoff, so we
	 * need to walk the whole members array to figure out what to do, if
	 * anything.
	 */
	nmembers =
		GetMultiXactIdMembers(multi, &members, false,
							  HEAP_XMAX_IS_LOCKED_ONLY(t_infomask));
	if (nmembers <= 0)
	{
		/* 没有值得保留的 */
		*flags |= FRM_INVALIDATE_XMAX;
		pagefrz->freeze_required = true;
		return InvalidTransactionId;
	}

	/*
	 * The FRM_NOOP case is the only case where we might need to ratchet back
	 * FreezePageRelfrozenXid or FreezePageRelminMxid.  It is also the only
	 * case where our caller might ratchet back its NoFreezePageRelfrozenXid
	 * or NoFreezePageRelminMxid "no freeze" trackers to deal with a multi.
	 * FRM_NOOP handling should result in the NewRelfrozenXid/NewRelminMxid
	 * trackers managed by VACUUM being ratcheting back by xmax to the degree
	 * required to make it safe to leave xmax undisturbed, independent of
	 * whether or not page freezing is triggered somewhere else.
	 *
	 * Our policy is to force freezing in every case other than FRM_NOOP,
	 * which obviates the need to maintain either set of trackers, anywhere.
	 * Every other case will reliably execute a freeze plan for xmax that
	 * either replaces xmax with an XID/MXID >= OldestXmin/OldestMxact, or
	 * sets xmax to an InvalidTransactionId XID, rendering xmax fully frozen.
	 * (VACUUM's NewRelfrozenXid/NewRelminMxid trackers are initialized with
	 * OldestXmin/OldestMxact, so later values never need to be tracked here.)
	 */
	need_replace = false;
	FreezePageRelfrozenXid = pagefrz->FreezePageRelfrozenXid;
	for (int i = 0; i < nmembers; i++)
	{
		TransactionId xid = members[i].xid;

		Assert(!TransactionIdPrecedes(xid, cutoffs->relfrozenxid));

		if (TransactionIdPrecedes(xid, cutoffs->FreezeLimit))
		{
			/* 不能违反 FreezeLimit 后置条件 */
			need_replace = true;
			break;
		}
		if (TransactionIdPrecedes(xid, FreezePageRelfrozenXid))
			FreezePageRelfrozenXid = xid;
	}

	/* 也不能违反 MultiXactCutoff 后置条件 */
	if (!need_replace)
		need_replace = MultiXactIdPrecedes(multi, cutoffs->MultiXactCutoff);

	if (!need_replace)
	{
		/*
		 * vacuumlazy.c might ratchet back NewRelminMxid, NewRelfrozenXid, or
		 * both together to make it safe to retain this particular multi after
		 * freezing its page
		 */
		*flags |= FRM_NOOP;
		pagefrz->FreezePageRelfrozenXid = FreezePageRelfrozenXid;
		if (MultiXactIdPrecedes(multi, pagefrz->FreezePageRelminMxid))
			pagefrz->FreezePageRelminMxid = multi;
		pfree(members);
		return multi;
	}

	/*
	 * Do a more thorough second pass over the multi to figure out which
	 * member XIDs actually need to be kept.  Checking the precise status of
	 * individual members might even show that we don't need to keep anything.
	 * That is quite possible even though the Multi must be >= OldestMxact,
	 * since our second pass only keeps member XIDs when it's truly necessary;
	 * even member XIDs >= OldestXmin often won't be kept by second pass.
	 */
	nnewmembers = 0;
	newmembers = palloc(sizeof(MultiXactMember) * nmembers);
	has_lockers = false;
	update_xid = InvalidTransactionId;
	update_committed = false;

	/*
	 * Determine whether to keep each member xid, or to ignore it instead
	 */
	for (int i = 0; i < nmembers; i++)
	{
		TransactionId xid = members[i].xid;
		MultiXactStatus mstatus = members[i].status;

		Assert(!TransactionIdPrecedes(xid, cutoffs->relfrozenxid));

		if (!ISUPDATE_from_mxstatus(mstatus))
		{
			/*
			 * Locker XID (not updater XID).  We only keep lockers that are
			 * still running.
			 */
			if (TransactionIdIsCurrentTransactionId(xid) ||
				TransactionIdIsInProgress(xid))
			{
				if (TransactionIdPrecedes(xid, cutoffs->OldestXmin))
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg_internal("multixact %u contains running locker XID %u from before removable cutoff %u",
											 multi, xid,
											 cutoffs->OldestXmin)));
				newmembers[nnewmembers++] = members[i];
				has_lockers = true;
			}

			continue;
		}

		/*
		 * Updater XID (not locker XID).  Should we keep it?
		 *
		 * Since the tuple wasn't totally removed when vacuum pruned, the
		 * update Xid cannot possibly be older than OldestXmin cutoff unless
		 * the updater XID aborted.  If the updater transaction is known
		 * aborted or crashed then it's okay to ignore it, otherwise not.
		 *
		 * In any case the Multi should never contain two updaters, whatever
		 * their individual commit status.  Check for that first, in passing.
		 */
		if (TransactionIdIsValid(update_xid))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("multixact %u has two or more updating members",
									 multi),
					 errdetail_internal("First updater XID=%u second updater XID=%u.",
										update_xid, xid)));

		/*
		 * As with all tuple visibility routines, it's critical to test
		 * TransactionIdIsInProgress before TransactionIdDidCommit, because of
		 * race conditions explained in detail in heapam_visibility.c.
		 */
		if (TransactionIdIsCurrentTransactionId(xid) ||
			TransactionIdIsInProgress(xid))
			update_xid = xid;
		else if (TransactionIdDidCommit(xid))
		{
			/*
			 * The transaction committed, so we can tell caller to set
			 * HEAP_XMAX_COMMITTED.  (We can only do this because we know the
			 * transaction is not running.)
			 */
			update_committed = true;
			update_xid = xid;
		}
		else
		{
			/*
			 * Not in progress, not committed -- must be aborted or crashed;
			 * we can ignore it.
			 */
			continue;
		}

		/*
		 * We determined that updater must be kept -- add it to pending new
		 * members list
		 */
		if (TransactionIdPrecedes(xid, cutoffs->OldestXmin))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("multixact %u contains committed update XID %u from before removable cutoff %u",
									 multi, xid, cutoffs->OldestXmin)));
		newmembers[nnewmembers++] = members[i];
	}

	pfree(members);

	/*
	 * Determine what to do with caller's multi based on information gathered
	 * during our second pass
	 */
	if (nnewmembers == 0)
	{
		/* 没有值得保留的 */
		*flags |= FRM_INVALIDATE_XMAX;
		newxmax = InvalidTransactionId;
	}
	else if (TransactionIdIsValid(update_xid) && !has_lockers)
	{
		/*
		 * If there's a single member and it's an update, pass it back alone
		 * without creating a new Multi.  (XXX we could do this when there's a
		 * single remaining locker, too, but that would complicate the API too
		 * much; moreover, the case with the single updater is more
		 * interesting, because those are longer-lived.)
		 */
		Assert(nnewmembers == 1);
		*flags |= FRM_RETURN_IS_XID;
		if (update_committed)
			*flags |= FRM_MARK_COMMITTED;
		newxmax = update_xid;
	}
	else
	{
		/*
		 * Create a new multixact with the surviving members of the previous
		 * one, to set as new Xmax in the tuple
		 */
		newxmax = MultiXactIdCreateFromMembers(nnewmembers, newmembers);
		*flags |= FRM_RETURN_IS_MULTI;
	}

	pfree(newmembers);

	pagefrz->freeze_required = true;
	return newxmax;
}

/*
 * heap_prepare_freeze_tuple
 *
 * 检查元组的任何 XID 字段（xmin、xmax、xvac）是否早于 OldestXmin 和/或
 * OldestMxact 冻结截止值。如果是，则在 *frz 输出参数中设置足够的状态，
 * 使调用者可以在冻结页面时处理此元组，并返回 true。如果当前无法对元组
 * 进行任何更改，则返回 false。
 *
 * 如果调用者执行返回的冻结计划后，元组将被完全冻结（或者该元组已被
 * 之前的 VACUUM 完全冻结），则还将 *totally_frozen 设为 true。这表示
 * 没有残留的 XID 或 MultiXactId 需要未来 VACUUM 处理了。
 *
 * VACUUM 调用者必须为每个我们返回 true 的元组组装 HeapTupleFreeze
 * 冻结计划条目，然后执行冻结。调用者必须在首次调用此函数处理每个
 * 堆页之前，初始化该页整体的 pagefrz 字段。
 *
 * VACUUM 调用者决定是否冻结整个页面。我们通常会为调用者直接丢弃的页面
 * 准备冻结计划。然而，VACUUM 并不总能做出选择；当 pagefrz.freeze_required
 * 被设置时，它必须冻结，以确保不会残留任何 XIDs < FreezeLimit（以及
 * MXIDs < MultiXactCutoff）。我们协助确保 VACUUM 始终遵循该规则。
 *
 * 我们有时会在严格必要之前很久就强制冻结 xmax MultiXactId 值，
 * 仅仅是为了确保 FreezeLimit 后置条件。在代价低时主动处理
 * MultiXactId 是值得的，将其搭载在 "强制冻结" 机制上很方便。
 * 反之，当目前代价过高时，我们有时会延迟冻结 MultiXactId（但仅在
 * 不违反 FreezeLimit/MultiXactCutoff 后置条件的情况下这样做）。
 *
 * 假定调用者已通过 HeapTupleSatisfiesVacuum() 检查了元组，并确定
 * 它不是 HEAPTUPLE_DEAD（否则应该删除元组，而不是冻结它）。
 *
 * 注意：此函数有副作用：它可能分配一个新的 MultiXactId。
 * 当我们的 *frz 输出随后在 heap_execute_freeze_tuple 中处理时，
 * 该 MultiXactId 将被设置为元组的新 xmax。如果元组在共享缓冲区中，
 * 调用者最好已经持有该缓冲区的排他锁。
 */
bool
heap_prepare_freeze_tuple(HeapTupleHeader tuple,
						  const struct VacuumCutoffs *cutoffs,
						  HeapPageFreeze *pagefrz,
						  HeapTupleFreeze *frz, bool *totally_frozen)
{
	bool		xmin_already_frozen = false,
				xmax_already_frozen = false;
	bool		freeze_xmin = false,
				replace_xvac = false,
				replace_xmax = false,
				freeze_xmax = false;
	TransactionId xid;

	frz->xmax = HeapTupleHeaderGetRawXmax(tuple);
	frz->t_infomask2 = tuple->t_infomask2;
	frz->t_infomask = tuple->t_infomask;
	frz->frzflags = 0;
	frz->checkflags = 0;

	/*
	 * Process xmin, while keeping track of whether it's already frozen, or
	 * will become frozen iff our freeze plan is executed by caller (could be
	 * neither).
	 */
	xid = HeapTupleHeaderGetXmin(tuple);
	if (!TransactionIdIsNormal(xid))
		xmin_already_frozen = true;
	else
	{
		if (TransactionIdPrecedes(xid, cutoffs->relfrozenxid))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("found xmin %u from before relfrozenxid %u",
									 xid, cutoffs->relfrozenxid)));

		/* 将在下面的冻结计划中设置 freeze_xmin 标志 */
		freeze_xmin = TransactionIdPrecedes(xid, cutoffs->OldestXmin);

		/* 验证 xmin 在执行冻结计划时是否已提交 */
		if (freeze_xmin)
			frz->checkflags |= HEAP_FREEZE_CHECK_XMIN_COMMITTED;
	}

	/*
	 * Old-style VACUUM FULL is gone, but we have to process xvac for as long
	 * as we support having MOVED_OFF/MOVED_IN tuples in the database
	 */
	xid = HeapTupleHeaderGetXvac(tuple);
	if (TransactionIdIsNormal(xid))
	{
		Assert(TransactionIdPrecedesOrEquals(cutoffs->relfrozenxid, xid));
		Assert(TransactionIdPrecedes(xid, cutoffs->OldestXmin));

		/*
		 * For Xvac, we always freeze proactively.  This allows totally_frozen
		 * tracking to ignore xvac.
		 */
		replace_xvac = pagefrz->freeze_required = true;

		/* 将在下面的冻结计划中设置 replace_xvac 标志 */
	}

	/* 现在处理 xmax */
	xid = frz->xmax;
	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		/* 原始 xmax 是 MultiXactId */
		TransactionId newxmax;
		uint16		flags;

		/*
		 * We will either remove xmax completely (in the "freeze_xmax" path),
		 * process xmax by replacing it (in the "replace_xmax" path), or
		 * perform no-op xmax processing.  The only constraint is that the
		 * FreezeLimit/MultiXactCutoff postcondition must never be violated.
		 */
		newxmax = FreezeMultiXactId(xid, tuple->t_infomask, cutoffs,
									&flags, pagefrz);

		if (flags & FRM_NOOP)
		{
			/*
			 * xmax is a MultiXactId, and nothing about it changes for now.
			 * This is the only case where 'freeze_required' won't have been
			 * set for us by FreezeMultiXactId, as well as the only case where
			 * neither freeze_xmax nor replace_xmax are set (given a multi).
			 *
			 * This is a no-op, but the call to FreezeMultiXactId might have
			 * ratcheted back NewRelfrozenXid and/or NewRelminMxid trackers
			 * for us (the "freeze page" variants, specifically).  That'll
			 * make it safe for our caller to freeze the page later on, while
			 * leaving this particular xmax undisturbed.
			 *
			 * FreezeMultiXactId is _not_ responsible for the "no freeze"
			 * NewRelfrozenXid/NewRelminMxid trackers, though -- that's our
			 * job.  A call to heap_tuple_should_freeze for this same tuple
			 * will take place below if 'freeze_required' isn't set already.
			 * (This repeats work from FreezeMultiXactId, but allows "no
			 * freeze" tracker maintenance to happen in only one place.)
			 */
			Assert(!MultiXactIdPrecedes(newxmax, cutoffs->MultiXactCutoff));
			Assert(MultiXactIdIsValid(newxmax) && xid == newxmax);
		}
		else if (flags & FRM_RETURN_IS_XID)
		{
			/*
			 * xmax will become an updater Xid (original MultiXact's updater
			 * member Xid will be carried forward as a simple Xid in Xmax).
			 */
			Assert(!TransactionIdPrecedes(newxmax, cutoffs->OldestXmin));

			/*
			 * NB -- some of these transformations are only valid because we
			 * know the return Xid is a tuple updater (i.e. not merely a
			 * locker.) Also note that the only reason we don't explicitly
			 * worry about HEAP_KEYS_UPDATED is because it lives in
			 * t_infomask2 rather than t_infomask.
			 */
			frz->t_infomask &= ~HEAP_XMAX_BITS;
			frz->xmax = newxmax;
			if (flags & FRM_MARK_COMMITTED)
				frz->t_infomask |= HEAP_XMAX_COMMITTED;
			replace_xmax = true;
		}
		else if (flags & FRM_RETURN_IS_MULTI)
		{
			uint16		newbits;
			uint16		newbits2;

			/*
			 * xmax is an old MultiXactId that we have to replace with a new
			 * MultiXactId, to carry forward two or more original member XIDs.
			 */
			Assert(!MultiXactIdPrecedes(newxmax, cutoffs->OldestMxact));

			/*
			 * We can't use GetMultiXactIdHintBits directly on the new multi
			 * here; that routine initializes the masks to all zeroes, which
			 * would lose other bits we need.  Doing it this way ensures all
			 * unrelated bits remain untouched.
			 */
			frz->t_infomask &= ~HEAP_XMAX_BITS;
			frz->t_infomask2 &= ~HEAP_KEYS_UPDATED;
			GetMultiXactIdHintBits(newxmax, &newbits, &newbits2);
			frz->t_infomask |= newbits;
			frz->t_infomask2 |= newbits2;
			frz->xmax = newxmax;
			replace_xmax = true;
		}
		else
		{
			/*
			 * Freeze plan for tuple "freezes xmax" in the strictest sense:
			 * it'll leave nothing in xmax (neither an Xid nor a MultiXactId).
			 */
			Assert(flags & FRM_INVALIDATE_XMAX);
			Assert(!TransactionIdIsValid(newxmax));

			/* 将在下面的冻结计划中设置 freeze_xmax 标志 */
			freeze_xmax = true;
		}

		/* MultiXactId 处理强制冻结（FRM_NOOP 情况除外） */
		Assert(pagefrz->freeze_required || (!freeze_xmax && !replace_xmax));
	}
	else if (TransactionIdIsNormal(xid))
	{
		/* 原始 xmax 是普通 XID */
		if (TransactionIdPrecedes(xid, cutoffs->relfrozenxid))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("found xmax %u from before relfrozenxid %u",
									 xid, cutoffs->relfrozenxid)));

		/* 将在下面的冻结计划中设置 freeze_xmax 标志 */
		freeze_xmax = TransactionIdPrecedes(xid, cutoffs->OldestXmin);

		/*
		 * Verify that xmax aborted if and when freeze plan is executed,
		 * provided it's from an update. (A lock-only xmax can be removed
		 * independent of this, since the lock is released at xact end.)
		 */
		if (freeze_xmax && !HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
			frz->checkflags |= HEAP_FREEZE_CHECK_XMAX_ABORTED;
	}
	else if (!TransactionIdIsValid(xid))
	{
		/* 原始 xmax 是 InvalidTransactionId */
		Assert((tuple->t_infomask & HEAP_XMAX_IS_MULTI) == 0);
		xmax_already_frozen = true;
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("found raw xmax %u (infomask 0x%04x) not invalid and not multi",
								 xid, tuple->t_infomask)));

	if (freeze_xmin)
	{
		Assert(!xmin_already_frozen);

		frz->t_infomask |= HEAP_XMIN_FROZEN;
	}
	if (replace_xvac)
	{
		/*
		 * If a MOVED_OFF tuple is not dead, the xvac transaction must have
		 * failed; whereas a non-dead MOVED_IN tuple must mean the xvac
		 * transaction succeeded.
		 */
		Assert(pagefrz->freeze_required);
		if (tuple->t_infomask & HEAP_MOVED_OFF)
			frz->frzflags |= XLH_INVALID_XVAC;
		else
			frz->frzflags |= XLH_FREEZE_XVAC;
	}
	if (replace_xmax)
	{
		Assert(!xmax_already_frozen && !freeze_xmax);
		Assert(pagefrz->freeze_required);

		/* 已在先前的冻结计划中设置了 replace_xmax 标志 */
	}
	if (freeze_xmax)
	{
		Assert(!xmax_already_frozen && !replace_xmax);

		frz->xmax = InvalidTransactionId;

		/*
		 * The tuple might be marked either XMAX_INVALID or XMAX_COMMITTED +
		 * LOCKED.  Normalize to INVALID just to be sure no one gets confused.
		 * Also get rid of the HEAP_KEYS_UPDATED bit.
		 */
		frz->t_infomask &= ~HEAP_XMAX_BITS;
		frz->t_infomask |= HEAP_XMAX_INVALID;
		frz->t_infomask2 &= ~HEAP_HOT_UPDATED;
		frz->t_infomask2 &= ~HEAP_KEYS_UPDATED;
	}

	/*
	 * Determine if this tuple is already totally frozen, or will become
	 * totally frozen (provided caller executes freeze plans for the page)
	 */
	*totally_frozen = ((freeze_xmin || xmin_already_frozen) &&
					   (freeze_xmax || xmax_already_frozen));

	if (!pagefrz->freeze_required && !(xmin_already_frozen &&
									   xmax_already_frozen))
	{
		/*
		 * So far no previous tuple from the page made freezing mandatory.
		 * Does this tuple force caller to freeze the entire page?
		 */
		pagefrz->freeze_required =
			heap_tuple_should_freeze(tuple, cutoffs,
									 &pagefrz->NoFreezePageRelfrozenXid,
									 &pagefrz->NoFreezePageRelminMxid);
	}

	/* 告知调用者此元组是否在 *frz 中设置了可用冻结计划 */
	return freeze_xmin || replace_xvac || replace_xmax || freeze_xmax;
}

/*
 * 在执行冻结计划之前，对 xmin/xmax XID 状态执行健全性检查。
 *
 * heap_prepare_freeze_tuple 不直接执行这些检查，因为 pg_xact 查找
 * 相对昂贵。不应在每次连续 VACUUM 决定不冻结同一页面时重复执行。
 */
void
heap_pre_freeze_checks(Buffer buffer,
					   HeapTupleFreeze *tuples, int ntuples)
{
	Page		page = BufferGetPage(buffer);

	for (int i = 0; i < ntuples; i++)
	{
		HeapTupleFreeze *frz = tuples + i;
		ItemId		itemid = PageGetItemId(page, frz->offset);
		HeapTupleHeader htup;

		htup = (HeapTupleHeader) PageGetItem(page, itemid);

		/* 在此处刻意避免依赖元组提示位 */
		if (frz->checkflags & HEAP_FREEZE_CHECK_XMIN_COMMITTED)
		{
			TransactionId xmin = HeapTupleHeaderGetRawXmin(htup);

			Assert(!HeapTupleHeaderXminFrozen(htup));
			if (unlikely(!TransactionIdDidCommit(xmin)))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg_internal("uncommitted xmin %u needs to be frozen",
										 xmin)));
		}

		/*
		 * TransactionIdDidAbort 在存在因崩溃期间仍在进行的事务而残留的
		 * XID 时无法可靠工作，因此我们只能检查 xmax 是否未提交
		 */
		if (frz->checkflags & HEAP_FREEZE_CHECK_XMAX_ABORTED)
		{
			TransactionId xmax = HeapTupleHeaderGetRawXmax(htup);

			Assert(TransactionIdIsNormal(xmax));
			if (unlikely(TransactionIdDidCommit(xmax)))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg_internal("cannot freeze committed xmax %u",
										 xmax)));
		}
	}
}

/*
 * 辅助例程，代表调用者执行页面上一个或多个堆元组的冻结。
 * 调用者传递来自 heap_prepare_freeze_tuple 的元组计划数组。
 * 调用者必须为我们在每个计划中设置 'offset'。
 * 必须在临界区中调用，该临界区还需标记缓冲区为脏，并在需要时发出 WAL。
 */
void
heap_freeze_prepared_tuples(Buffer buffer, HeapTupleFreeze *tuples, int ntuples)
{
	Page		page = BufferGetPage(buffer);

	for (int i = 0; i < ntuples; i++)
	{
		HeapTupleFreeze *frz = tuples + i;
		ItemId		itemid = PageGetItemId(page, frz->offset);
		HeapTupleHeader htup;

		htup = (HeapTupleHeader) PageGetItem(page, itemid);
		heap_execute_freeze_tuple(htup, frz);
	}
}

/*
 * heap_freeze_tuple
 *		原地冻结元组，无需 WAL 日志记录。
 *
 * 适用于自行执行 WAL 日志记录的调用者，如 CLUSTER。
 */
bool
heap_freeze_tuple(HeapTupleHeader tuple,
				  TransactionId relfrozenxid, TransactionId relminmxid,
				  TransactionId FreezeLimit, TransactionId MultiXactCutoff)
{
	HeapTupleFreeze frz;
	bool		do_freeze;
	bool		totally_frozen;
	struct VacuumCutoffs cutoffs;
	HeapPageFreeze pagefrz;

	cutoffs.relfrozenxid = relfrozenxid;
	cutoffs.relminmxid = relminmxid;
	cutoffs.OldestXmin = FreezeLimit;
	cutoffs.OldestMxact = MultiXactCutoff;
	cutoffs.FreezeLimit = FreezeLimit;
	cutoffs.MultiXactCutoff = MultiXactCutoff;

	pagefrz.freeze_required = true;
	pagefrz.FreezePageRelfrozenXid = FreezeLimit;
	pagefrz.FreezePageRelminMxid = MultiXactCutoff;
	pagefrz.NoFreezePageRelfrozenXid = FreezeLimit;
	pagefrz.NoFreezePageRelminMxid = MultiXactCutoff;

	do_freeze = heap_prepare_freeze_tuple(tuple, &cutoffs,
										  &pagefrz, &frz, &totally_frozen);

	/*
	 * Note that because this is not a WAL-logged operation, we don't need to
	 * fill in the offset in the freeze record.
	 */

	if (do_freeze)
		heap_execute_freeze_tuple(tuple, &frz);
	return do_freeze;
}

/*
 * 对于给定的 MultiXactId，返回应在元组 infomask 中设置的提示位。
 *
 * 通常应对刚创建的 multixact 调用此函数，此时它位于本地缓存中，
 * 因此 GetMembers 调用很快。
 */
static void
GetMultiXactIdHintBits(MultiXactId multi, uint16 *new_infomask,
					   uint16 *new_infomask2)
{
	int			nmembers;
	MultiXactMember *members;
	int			i;
	uint16		bits = HEAP_XMAX_IS_MULTI;
	uint16		bits2 = 0;
	bool		has_update = false;
	LockTupleMode strongest = LockTupleKeyShare;

	/*
	 * 我们仅在刚创建的 multi 中使用此功能，因此它们不可能是
	 * pg_upgrade 之前的值。
	 */
	nmembers = GetMultiXactIdMembers(multi, &members, false, false);

	for (i = 0; i < nmembers; i++)
	{
		LockTupleMode mode;

		/*
		 * 记住 multixact 中任何成员持有的最强锁模式。
		 */
		mode = TUPLOCK_from_mxstatus(members[i].status);
		if (mode > strongest)
			strongest = mode;

		/* 查看还需要哪些其他位 */
		switch (members[i].status)
		{
			case MultiXactStatusForKeyShare:
			case MultiXactStatusForShare:
			case MultiXactStatusForNoKeyUpdate:
				break;

			case MultiXactStatusForUpdate:
				bits2 |= HEAP_KEYS_UPDATED;
				break;

			case MultiXactStatusNoKeyUpdate:
				has_update = true;
				break;

			case MultiXactStatusUpdate:
				bits2 |= HEAP_KEYS_UPDATED;
				has_update = true;
				break;
		}
	}

	if (strongest == LockTupleExclusive ||
		strongest == LockTupleNoKeyExclusive)
		bits |= HEAP_XMAX_EXCL_LOCK;
	else if (strongest == LockTupleShare)
		bits |= HEAP_XMAX_SHR_LOCK;
	else if (strongest == LockTupleKeyShare)
		bits |= HEAP_XMAX_KEYSHR_LOCK;

	if (!has_update)
		bits |= HEAP_XMAX_LOCK_ONLY;

	if (nmembers > 0)
		pfree(members);

	*new_infomask = bits;
	*new_infomask2 = bits2;
}

/*
 * MultiXactIdGetUpdateXid
 *
 * 给定一个 multixact Xmax 及其对应的 infomask（未设置
 * HEAP_XMAX_LOCK_ONLY 位），获取并返回更新事务的 Xid。
 *
 * 调用者应自行检查更新事务的状态（如有必要）。
 */
static TransactionId
MultiXactIdGetUpdateXid(TransactionId xmax, uint16 t_infomask)
{
	TransactionId update_xact = InvalidTransactionId;
	MultiXactMember *members;
	int			nmembers;

	Assert(!(t_infomask & HEAP_XMAX_LOCK_ONLY));
	Assert(t_infomask & HEAP_XMAX_IS_MULTI);

	/*
	 * 由于我们知道 LOCK_ONLY 位未设置，这不可能是来自 pg_upgrade 之前的 multi。
	 */
	nmembers = GetMultiXactIdMembers(xmax, &members, false, false);

	if (nmembers > 0)
	{
		int			i;

		for (i = 0; i < nmembers; i++)
		{
			/* 忽略锁定者 */
			if (!ISUPDATE_from_mxstatus(members[i].status))
				continue;

			/* 最多只能有一个更新者 */
			Assert(update_xact == InvalidTransactionId);
			update_xact = members[i].xid;
#ifndef USE_ASSERT_CHECKING

			/*
			 * 在启用断言的构建中，遍历整个数组以确保没有其他更新者。
			 */
			break;
#endif
		}

		pfree(members);
	}

	return update_xact;
}

/*
 * HeapTupleGetUpdateXid
 *		As above, but use a HeapTupleHeader
 *
 * See also HeapTupleHeaderGetUpdateXid, which can be used without previously
 * checking the hint bits.
 */
TransactionId
HeapTupleGetUpdateXid(const HeapTupleHeaderData *tup)
{
	return MultiXactIdGetUpdateXid(HeapTupleHeaderGetRawXmax(tup),
								   tup->t_infomask);
}

/*
 * 给定的 multixact 是否与当前事务获取给定强度的元组锁冲突？
 *
 * 传入的 infomask 与元组头中的给定 multixact 配对。
 *
 * 如果 current_is_member 非 NULL，且当前事务是该给定 multixact 的成员，
 * 则将其设为 'true'。
 */
static bool
DoesMultiXactIdConflict(MultiXactId multi, uint16 infomask,
						LockTupleMode lockmode, bool *current_is_member)
{
	int			nmembers;
	MultiXactMember *members;
	bool		result = false;
	LOCKMODE	wanted = tupleLockExtraInfo[lockmode].hwlock;

	if (HEAP_LOCKED_UPGRADED(infomask))
		return false;

	nmembers = GetMultiXactIdMembers(multi, &members, false,
									 HEAP_XMAX_IS_LOCKED_ONLY(infomask));
	if (nmembers >= 0)
	{
		int			i;

		for (i = 0; i < nmembers; i++)
		{
			TransactionId memxid;
			LOCKMODE	memlockmode;

			if (result && (current_is_member == NULL || *current_is_member))
				break;

			memlockmode = LOCKMODE_from_mxstatus(members[i].status);

			/* 忽略来自当前事务的成员（但跟踪其存在性） */
			memxid = members[i].xid;
			if (TransactionIdIsCurrentTransactionId(memxid))
			{
				if (current_is_member != NULL)
					*current_is_member = true;
				continue;
			}
			else if (result)
				continue;

			/* 忽略与我们要获取的锁不冲突的成员 */
			if (!DoLockModesConflict(memlockmode, wanted))
				continue;

			if (ISUPDATE_from_mxstatus(members[i].status))
			{
				/* 忽略已中止的更新者 */
				if (TransactionIdDidAbort(memxid))
					continue;
			}
			else
			{
				/* 忽略已不再进行中的仅加锁者 */
				if (!TransactionIdIsInProgress(memxid))
					continue;
			}

			/*
			 * Whatever remains are either live lockers that conflict with our
			 * wanted lock, and updaters that are not aborted.  Those conflict
			 * with what we want.  Set up to return true, but keep going to
			 * look for the current transaction among the multixact members,
			 * if needed.
			 */
			result = true;
		}
		pfree(members);
	}

	return result;
}

/*
 * Do_MultiXactIdWait
 *		以下两个函数的实际实现。
 *
 * 'multi'、'status' 和 'infomask' 指示要等待的内容（status 用于确保
 * 我们只等待冲突的成员，infomask 用于在仅锁 multi 的情况下优化
 * multixact 访问）；'nowait' 指示是否使用条件锁获取，允许调用者在
 * 锁不可用时失败。'rel'、'ctid' 和 'oper' 用于设置错误消息的上下文
 * 信息。'remaining'（若非 NULL）接收仍在运行的成员数量，包括当前
 * 事务的任何（非中止的）子事务。'logLockFailure' 指示在启用 'nowait'
 * 时锁获取失败是否记录详细信息。
 *
 * 我们通过对每个成员使用 XactLockTableWait 来休眠。但是，属于当前
 * 后端的成员*不会*被等待；这不仅毫无意义，还会导致 XactLockTableWait
 * 内部的 Assert 失败。当此函数返回时，可以确定所有*其他后端*中
 * 属于该 MultiXactId 且与请求的状态冲突的事务已经死亡（且不会有新的
 * 事务被添加，因为向已存在的 MultiXactId 添加成员是不合法的）。
 *
 * 但当我们完成休眠时，其他人可能已经更改了包含元组的 Xmax，
 * 因此调用者需要以某种方式迭代调用我们。
 *
 * 注意：当我们返回 false 时，剩余成员数量不可信赖。
 */
static bool
Do_MultiXactIdWait(MultiXactId multi, MultiXactStatus status,
				   uint16 infomask, bool nowait,
				   Relation rel, ItemPointer ctid, XLTW_Oper oper,
				   int *remaining, bool logLockFailure)
{
	bool		result = true;
	MultiXactMember *members;
	int			nmembers;
	int			remain = 0;

	/* 对于 pg_upgrade 之前的元组，完全不需要等待 */
	nmembers = HEAP_LOCKED_UPGRADED(infomask) ? -1 :
		GetMultiXactIdMembers(multi, &members, false,
							  HEAP_XMAX_IS_LOCKED_ONLY(infomask));

	if (nmembers >= 0)
	{
		int			i;

		for (i = 0; i < nmembers; i++)
		{
			TransactionId memxid = members[i].xid;
			MultiXactStatus memstatus = members[i].status;

			if (TransactionIdIsCurrentTransactionId(memxid))
			{
				remain++;
				continue;
			}

			if (!DoLockModesConflict(LOCKMODE_from_mxstatus(memstatus),
									 LOCKMODE_from_mxstatus(status)))
			{
				if (remaining && TransactionIdIsInProgress(memxid))
					remain++;
				continue;
			}

		/*
		 * 此成员与我们的 multi 冲突，因此必须休眠（若要求避免等待则返回失败）。
		 *
		 * 注意，我们不自建错误上下文回调，而是将信息传递给 XactLockTableWait。
		 * 这可能显得有些浪费，因为对 multixact 的每个成员都要设置和拆除上下文，
		 * 但实际中几乎不可察觉，且避免了代码重复。
		 */
			if (nowait)
			{
				result = ConditionalXactLockTableWait(memxid, logLockFailure);
				if (!result)
					break;
			}
			else
				XactLockTableWait(memxid, rel, ctid, oper);
		}

		pfree(members);
	}

	if (remaining)
		*remaining = remain;

	return result;
}

/*
 * MultiXactIdWait
 *		在 MultiXactId 上休眠。
 *
 * 当我们完成休眠时，其他人可能已经更改了包含元组的 Xmax，
 * 因此调用者需要以某种方式迭代调用我们。
 *
 * 我们返回（在 *remaining 中，若非 NULL）仍在运行的成员数量，
 * 包括当前事务的任何（非中止的）子事务。
 */
static void
MultiXactIdWait(MultiXactId multi, MultiXactStatus status, uint16 infomask,
				Relation rel, ItemPointer ctid, XLTW_Oper oper,
				int *remaining)
{
	(void) Do_MultiXactIdWait(multi, status, infomask, false,
							  rel, ctid, oper, remaining, false);
}

/*
 * ConditionalMultiXactIdWait
 *		同上，但仅在无需阻塞即可获取锁时才锁定。
 *
 * 当我们完成休眠时，其他人可能已经更改了包含元组的 Xmax，
 * 因此调用者需要以某种方式迭代调用我们。
 *
 * 如果 multixact 已全部消失，返回 true。如果某些事务可能仍在运行，
 * 返回 false。
 *
 * 我们返回（在 *remaining 中，若非 NULL）仍在运行的成员数量，
 * 包括当前事务的任何（非中止的）子事务。
 */
static bool
ConditionalMultiXactIdWait(MultiXactId multi, MultiXactStatus status,
						   uint16 infomask, Relation rel, int *remaining,
						   bool logLockFailure)
{
	return Do_MultiXactIdWait(multi, status, infomask, true,
							  rel, NULL, XLTW_None, remaining, logLockFailure);
}

/*
 * heap_tuple_needs_eventual_freeze
 *
 * 检查元组的任何 XID 字段（xmin、xmax、xvac）最终是否将需要冻结
 * （前提是元组未被剪枝删除）。
 */
bool
heap_tuple_needs_eventual_freeze(HeapTupleHeader tuple)
{
	TransactionId xid;

	/*
	 * 如果 xmin 是普通事务 ID，则此元组肯定未冻结。
	 */
	xid = HeapTupleHeaderGetXmin(tuple);
	if (TransactionIdIsNormal(xid))
		return true;

	/*
	 * 如果 xmax 是有效的事务或 multixact，此元组也未冻结。
	 */
	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		MultiXactId multi;

		multi = HeapTupleHeaderGetRawXmax(tuple);
		if (MultiXactIdIsValid(multi))
			return true;
	}
	else
	{
		xid = HeapTupleHeaderGetRawXmax(tuple);
		if (TransactionIdIsNormal(xid))
			return true;
	}

	if (tuple->t_infomask & HEAP_MOVED)
	{
		xid = HeapTupleHeaderGetXvac(tuple);
		if (TransactionIdIsNormal(xid))
			return true;
	}

	return false;
}

/*
 * heap_tuple_should_freeze
 *
 * 返回值指示 heap_prepare_freeze_tuple 姊妹函数是否（或是否应该）强制
 * 冻结包含调用者元组的堆页。元组头中 XIDs/MXIDs < FreezeLimit/
 * MultiXactCutoff 会触发冻结。这包括 (xmin, xmax, xvac) 字段以及
 * MultiXact 成员 XID。
 *
 * *NoFreezePageRelfrozenXid 和 *NoFreezePageRelminMxid 输入/输出参数
 * 帮助 VACUUM 跟踪关系中现存最旧的 XID/MXID。我们的工作假设是调用者
 * 不会决定冻结此元组。调用者应仅在完全承诺不冻结该元组/页面之后，
 * 才回退其自己的顶层跟踪器。
 */
bool
heap_tuple_should_freeze(HeapTupleHeader tuple,
						 const struct VacuumCutoffs *cutoffs,
						 TransactionId *NoFreezePageRelfrozenXid,
						 MultiXactId *NoFreezePageRelminMxid)
{
	TransactionId xid;
	MultiXactId multi;
	bool		freeze = false;

	/* 首先处理 xmin */
	xid = HeapTupleHeaderGetXmin(tuple);
	if (TransactionIdIsNormal(xid))
	{
		Assert(TransactionIdPrecedesOrEquals(cutoffs->relfrozenxid, xid));
		if (TransactionIdPrecedes(xid, *NoFreezePageRelfrozenXid))
			*NoFreezePageRelfrozenXid = xid;
		if (TransactionIdPrecedes(xid, cutoffs->FreezeLimit))
			freeze = true;
	}

	/* 现在处理 xmax */
	xid = InvalidTransactionId;
	multi = InvalidMultiXactId;
	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
		multi = HeapTupleHeaderGetRawXmax(tuple);
	else
		xid = HeapTupleHeaderGetRawXmax(tuple);

	if (TransactionIdIsNormal(xid))
	{
		Assert(TransactionIdPrecedesOrEquals(cutoffs->relfrozenxid, xid));
		/* xmax 是非永久 XID */
		if (TransactionIdPrecedes(xid, *NoFreezePageRelfrozenXid))
			*NoFreezePageRelfrozenXid = xid;
		if (TransactionIdPrecedes(xid, cutoffs->FreezeLimit))
			freeze = true;
	}
	else if (!MultiXactIdIsValid(multi))
	{
		/* xmax 是永久 XID 或无效的 MultiXactId/XID */
	}
	else if (HEAP_LOCKED_UPGRADED(tuple->t_infomask))
	{
		/* xmax 是 pg_upgrade 产生的 MultiXact，不能有更新者 XID */
		if (MultiXactIdPrecedes(multi, *NoFreezePageRelminMxid))
			*NoFreezePageRelminMxid = multi;
		/* heap_prepare_freeze_tuple 始终冻结 pg_upgrade 产生的 xmax */
		freeze = true;
	}
	else
	{
		/* xmax 是可能包含更新者 XID 的 MultiXactId */
		MultiXactMember *members;
		int			nmembers;

		Assert(MultiXactIdPrecedesOrEquals(cutoffs->relminmxid, multi));
		if (MultiXactIdPrecedes(multi, *NoFreezePageRelminMxid))
			*NoFreezePageRelminMxid = multi;
		if (MultiXactIdPrecedes(multi, cutoffs->MultiXactCutoff))
			freeze = true;

		/* 需要检查 mxact 中是否有任何成员是旧的 */
		nmembers = GetMultiXactIdMembers(multi, &members, false,
										 HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask));

		for (int i = 0; i < nmembers; i++)
		{
			xid = members[i].xid;
			Assert(TransactionIdPrecedesOrEquals(cutoffs->relfrozenxid, xid));
			if (TransactionIdPrecedes(xid, *NoFreezePageRelfrozenXid))
				*NoFreezePageRelfrozenXid = xid;
			if (TransactionIdPrecedes(xid, cutoffs->FreezeLimit))
				freeze = true;
		}
		if (nmembers > 0)
			pfree(members);
	}

	if (tuple->t_infomask & HEAP_MOVED)
	{
		xid = HeapTupleHeaderGetXvac(tuple);
		if (TransactionIdIsNormal(xid))
		{
			Assert(TransactionIdPrecedesOrEquals(cutoffs->relfrozenxid, xid));
			if (TransactionIdPrecedes(xid, *NoFreezePageRelfrozenXid))
				*NoFreezePageRelfrozenXid = xid;
			/* heap_prepare_freeze_tuple 强制进行 xvac 冻结 */
			freeze = true;
		}
	}

	return freeze;
}

/*
 * 为调用者维护 snapshotConflictHorizon，通过使用 'tuple' 中包含的任何
 * 已提交 XID 将其值向前推移。'tuple' 是调用者正在物理删除的过时堆元组，
 * 例如通过 HOT 剪枝或索引删除。
 *
 * 调用者必须将其值初始化为 InvalidTransactionId，这通常被解读为
 * "确定不需要恢复冲突"。最终值必须反映调用者通过其正在进行的剪枝/删除
 * 操作将物理删除（或移除 TID 引用的）所有堆元组。
 * REDO 例程在重放调用者的操作时，会将最终值（从调用者的 WAL 记录中获取）
 * 传递给 ResolveRecoveryConflictWithSnapshot()。
 */
void
HeapTupleHeaderAdvanceConflictHorizon(HeapTupleHeader tuple,
									  TransactionId *snapshotConflictHorizon)
{
	TransactionId xmin = HeapTupleHeaderGetXmin(tuple);
	TransactionId xmax = HeapTupleHeaderGetUpdateXid(tuple);
	TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

	if (tuple->t_infomask & HEAP_MOVED)
	{
		if (TransactionIdPrecedes(*snapshotConflictHorizon, xvac))
			*snapshotConflictHorizon = xvac;
	}

	/*
	 * Ignore tuples inserted by an aborted transaction or if the tuple was
	 * updated/deleted by the inserting transaction.
	 *
	 * Look for a committed hint bit, or if no xmin bit is set, check clog.
	 */
	if (HeapTupleHeaderXminCommitted(tuple) ||
		(!HeapTupleHeaderXminInvalid(tuple) && TransactionIdDidCommit(xmin)))
	{
		if (xmax != xmin &&
			TransactionIdFollows(xmax, *snapshotConflictHorizon))
			*snapshotConflictHorizon = xmax;
	}
}

#ifdef USE_PREFETCH
/*
 * heap_index_delete_tuples 的辅助函数。为 prefetch_count 个缓冲区发出
 * 预取请求。prefetch_state 跟踪所有可以预取的缓冲区以及已经预取的缓冲区；
 * 每次调用此函数都从上一次调用结束的位置继续。
 *
 * 注意：我们期望 deltids 数组按堆块分组排序，每个块的所有 TID 恰好聚集在
 * 同一组中。
 */
static void
index_delete_prefetch_buffer(Relation rel,
							 IndexDeletePrefetchState *prefetch_state,
							 int prefetch_count)
{
	BlockNumber cur_hblkno = prefetch_state->cur_hblkno;
	int			count = 0;
	int			i;
	int			ndeltids = prefetch_state->ndeltids;
	TM_IndexDelete *deltids = prefetch_state->deltids;

	for (i = prefetch_state->next_item;
		 i < ndeltids && count < prefetch_count;
		 i++)
	{
		ItemPointer htid = &deltids[i].tid;

		if (cur_hblkno == InvalidBlockNumber ||
			ItemPointerGetBlockNumber(htid) != cur_hblkno)
		{
			cur_hblkno = ItemPointerGetBlockNumber(htid);
			PrefetchBuffer(rel, MAIN_FORKNUM, cur_hblkno);
			count++;
		}
	}

	/*
	 * 保存预取位置，以便下次从该位置继续。
	 */
	prefetch_state->next_item = i;
	prefetch_state->cur_hblkno = cur_hblkno;
}
#endif

/*
 * heap_index_delete_tuples 的辅助函数。检查索引 AM 调用者索引页中涉及
 * 无效 TID 的索引损坏。
 *
 * 这是执行这些检查的理想位置。索引 AM 必须对包含我们在此检查的 TID 的
 * 索引页持有缓冲区锁，因此我们完全无需担心并发的 VACUUM。当 htid 直接
 * 指向 LP_UNUSED 项或 heap-only 元组时，我们可以确定索引已损坏——
 * 这种情况在标准索引扫描中不会出现。
 */
static inline void
index_delete_check_htid(TM_IndexDeleteOp *delstate,
						Page page, OffsetNumber maxoff,
						ItemPointer htid, TM_IndexStatus *istatus)
{
	OffsetNumber indexpagehoffnum = ItemPointerGetOffsetNumber(htid);
	ItemId		iid;

	Assert(OffsetNumberIsValid(istatus->idxoffnum));

	if (unlikely(indexpagehoffnum > maxoff))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("heap tid from index tuple (%u,%u) points past end of heap page line pointer array at offset %u of block %u in index \"%s\"",
								 ItemPointerGetBlockNumber(htid),
								 indexpagehoffnum,
								 istatus->idxoffnum, delstate->iblknum,
								 RelationGetRelationName(delstate->irel))));

	iid = PageGetItemId(page, indexpagehoffnum);
	if (unlikely(!ItemIdIsUsed(iid)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("heap tid from index tuple (%u,%u) points to unused heap page item at offset %u of block %u in index \"%s\"",
								 ItemPointerGetBlockNumber(htid),
								 indexpagehoffnum,
								 istatus->idxoffnum, delstate->iblknum,
								 RelationGetRelationName(delstate->irel))));

	if (ItemIdHasStorage(iid))
	{
		HeapTupleHeader htup;

		Assert(ItemIdIsNormal(iid));
		htup = (HeapTupleHeader) PageGetItem(page, iid);

		if (unlikely(HeapTupleHeaderIsHeapOnly(htup)))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg_internal("heap tid from index tuple (%u,%u) points to heap-only tuple at offset %u of block %u in index \"%s\"",
									 ItemPointerGetBlockNumber(htid),
									 indexpagehoffnum,
									 istatus->idxoffnum, delstate->iblknum,
									 RelationGetRelationName(delstate->irel))));
	}
}

/*
 * tableam 的 index_delete_tuples 接口的 heapam 实现。
 *
 * 此辅助函数由索引 AM 在索引元组删除期间调用。有关此处实现的接口说明
 * 及一般工作原理，请参阅 tableam 头文件注释。注意，每次调用此处的操作
 * 要么是一次简单的索引删除，要么是一次自底向上的索引删除。
 *
 * 这可能会产生相当多的 I/O，因为我们可能从单个索引块中删除数百条元组。
 * 为了在某种程度上摊还此开销，这里使用了预取，并合并对同一堆块的重复访问。
 */
TransactionId
heap_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	/* 初始假设是更早的裁剪已处理了冲突 */
	TransactionId snapshotConflictHorizon = InvalidTransactionId;
	BlockNumber blkno = InvalidBlockNumber;
	Buffer		buf = InvalidBuffer;
	Page		page = NULL;
	OffsetNumber maxoff = InvalidOffsetNumber;
	TransactionId priorXmax;
#ifdef USE_PREFETCH
	IndexDeletePrefetchState prefetch_state;
	int			prefetch_distance;
#endif
	SnapshotData SnapshotNonVacuumable;
	int			finalndeltids = 0,
				nblocksaccessed = 0;

	/* 仅在自底向上索引删除情况中使用的状态 */
	int			nblocksfavorable = 0;
	int			curtargetfreespace = delstate->bottomupfreespace,
				lastfreespace = 0,
				actualfreespace = 0;
	bool		bottomup_final_block = false;

	InitNonVacuumableSnapshot(SnapshotNonVacuumable, GlobalVisTestFor(rel));

	/* 按 TID 对调用者的 deltids 数组排序以便进一步处理 */
	index_delete_sort(delstate);

	/*
	 * Bottom-up case: resort deltids array in an order attuned to where the
	 * greatest number of promising TIDs are to be found, and determine how
	 * many blocks from the start of sorted array should be considered
	 * favorable.  This will also shrink the deltids array in order to
	 * eliminate completely unfavorable blocks up front.
	 */
	if (delstate->bottomup)
		nblocksfavorable = bottomup_sort_and_shrink(delstate);

#ifdef USE_PREFETCH
	/* 初始化预取状态。 */
	prefetch_state.cur_hblkno = InvalidBlockNumber;
	prefetch_state.next_item = 0;
	prefetch_state.ndeltids = delstate->ndeltids;
	prefetch_state.deltids = delstate->deltids;

	/*
	 * Determine the prefetch distance that we will attempt to maintain.
	 *
	 * Since the caller holds a buffer lock somewhere in rel, we'd better make
	 * sure that isn't a catalog relation before we call code that does
	 * syscache lookups, to avoid risk of deadlock.
	 */
	if (IsCatalogRelation(rel))
		prefetch_distance = maintenance_io_concurrency;
	else
		prefetch_distance =
			get_tablespace_maintenance_io_concurrency(rel->rd_rel->reltablespace);

	/* 为自底向上删除调用者限制初始预取距离 */
	if (delstate->bottomup)
	{
		Assert(nblocksfavorable >= 1);
		Assert(nblocksfavorable <= BOTTOMUP_MAX_NBLOCKS);
		prefetch_distance = Min(prefetch_distance, nblocksfavorable);
	}

	/* 开始预取。 */
	index_delete_prefetch_buffer(rel, &prefetch_state, prefetch_distance);
#endif

	/* 遍历 deltids，确定要删除哪些，检查其水平线 */
	Assert(delstate->ndeltids > 0);
	for (int i = 0; i < delstate->ndeltids; i++)
	{
		TM_IndexDelete *ideltid = &delstate->deltids[i];
		TM_IndexStatus *istatus = delstate->status + ideltid->id;
		ItemPointer htid = &ideltid->tid;
		OffsetNumber offnum;

		/*
		 * Read buffer, and perform required extra steps each time a new block
		 * is encountered.  Avoid refetching if it's the same block as the one
		 * from the last htid.
		 */
		if (blkno == InvalidBlockNumber ||
			ItemPointerGetBlockNumber(htid) != blkno)
		{
			/*
			 * Consider giving up early for bottom-up index deletion caller
			 * first. (Only prefetch next-next block afterwards, when it
			 * becomes clear that we're at least going to access the next
			 * block in line.)
			 *
			 * Sometimes the first block frees so much space for bottom-up
			 * caller that the deletion process can end without accessing any
			 * more blocks.  It is usually necessary to access 2 or 3 blocks
			 * per bottom-up deletion operation, though.
			 */
			if (delstate->bottomup)
			{
				/*
				 * We often allow caller to delete a few additional items
				 * whose entries we reached after the point that space target
				 * from caller was satisfied.  The cost of accessing the page
				 * was already paid at that point, so it made sense to finish
				 * it off.  When that happened, we finalize everything here
				 * (by finishing off the whole bottom-up deletion operation
				 * without needlessly paying the cost of accessing any more
				 * blocks).
				 */
				if (bottomup_final_block)
					break;

				/*
				 * Give up when we didn't enable our caller to free any
				 * additional space as a result of processing the page that we
				 * just finished up with.  This rule is the main way in which
				 * we keep the cost of bottom-up deletion under control.
				 */
				if (nblocksaccessed >= 1 && actualfreespace == lastfreespace)
					break;
				lastfreespace = actualfreespace;	/* 供下次使用 */

				/*
				 * Deletion operation (which is bottom-up) will definitely
				 * access the next block in line.  Prepare for that now.
				 *
				 * Decay target free space so that we don't hang on for too
				 * long with a marginal case. (Space target is only truly
				 * helpful when it allows us to recognize that we don't need
				 * to access more than 1 or 2 blocks to satisfy caller due to
				 * agreeable workload characteristics.)
				 *
				 * We are a bit more patient when we encounter contiguous
				 * blocks, though: these are treated as favorable blocks.  The
				 * decay process is only applied when the next block in line
				 * is not a favorable/contiguous block.  This is not an
				 * exception to the general rule; we still insist on finding
				 * at least one deletable item per block accessed.  See
				 * bottomup_nblocksfavorable() for full details of the theory
				 * behind favorable blocks and heap block locality in general.
				 *
				 * Note: The first block in line is always treated as a
				 * favorable block, so the earliest possible point that the
				 * decay can be applied is just before we access the second
				 * block in line.  The Assert() verifies this for us.
				 */
				Assert(nblocksaccessed > 0 || nblocksfavorable > 0);
				if (nblocksfavorable > 0)
					nblocksfavorable--;
				else
					curtargetfreespace /= 2;
			}

			/* 释放旧缓冲区 */
			if (BufferIsValid(buf))
				UnlockReleaseBuffer(buf);

			blkno = ItemPointerGetBlockNumber(htid);
			buf = ReadBuffer(rel, blkno);
			nblocksaccessed++;
			Assert(!delstate->bottomup ||
				   nblocksaccessed <= BOTTOMUP_MAX_NBLOCKS);

#ifdef USE_PREFETCH

			/*
			 * To maintain the prefetch distance, prefetch one more page for
			 * each page we read.
			 */
			index_delete_prefetch_buffer(rel, &prefetch_state, 1);
#endif

			LockBuffer(buf, BUFFER_LOCK_SHARE);

			page = BufferGetPage(buf);
			maxoff = PageGetMaxOffsetNumber(page);
		}

		/*
		 * In passing, detect index corruption involving an index page with a
		 * TID that points to a location in the heap that couldn't possibly be
		 * correct.  We only do this with actual TIDs from caller's index page
		 * (not items reached by traversing through a HOT chain).
		 */
		index_delete_check_htid(delstate, page, maxoff, htid, istatus);

		if (istatus->knowndeletable)
			Assert(!delstate->bottomup && !istatus->promising);
		else
		{
			ItemPointerData tmp = *htid;
			HeapTupleData heapTuple;

			/* 此 HOT 链中是否有任何元组不可 vacuum？ */
			if (heap_hot_search_buffer(&tmp, rel, buf, &SnapshotNonVacuumable,
									   &heapTuple, NULL, true))
				continue;		/* 无法删除条目 */

			/* 调用者将删除，因为整个 HOT 链都是可 vacuum 的 */
			istatus->knowndeletable = true;

			/* 为自底向上删除情况维护索引空闲空间信息 */
			if (delstate->bottomup)
			{
				Assert(istatus->freespace > 0);
				actualfreespace += istatus->freespace;
				if (actualfreespace >= curtargetfreespace)
					bottomup_final_block = true;
			}
		}

				/* 通过将当前值随堆元组头推进，维护整个删除操作的
			 * snapshotConflictHorizon 值。这在松散意义上基于
			 * 剪枝 HOT 链的逻辑。
			 */
		offnum = ItemPointerGetOffsetNumber(htid);
		priorXmax = InvalidTransactionId;	/* 无法检查第一个 XMIN */
		for (;;)
		{
			ItemId		lp;
			HeapTupleHeader htup;

			/* 健全性检查（纯粹偏执） */
			if (offnum < FirstOffsetNumber)
				break;

						/* 当行指针数组被截断时，可能出现超出页面行指针数组
				 * 末尾的偏移量
				 */
			if (offnum > maxoff)
				break;

			lp = PageGetItemId(page, offnum);
			if (ItemIdIsRedirected(lp))
			{
				offnum = ItemIdGetRedirect(lp);
				continue;
			}

						/* 我们经常会遇到 LP_DEAD 行指针（尤其是调用者事先标记为
				 * knowndeletable 的条目）。对于指向 LP_DEAD 项的 htid，
				 * 不会检查任何堆元组头。这是可以接受的，因为最初将行指针
				 * 置为 LP_DEAD 的那次剪枝操作，在生成其自身的
				 * snapshotConflictHorizon 值时，必然已经考虑了原始元组头。
				 *
				 * 像这样依赖 XLOG_HEAP2_PRUNE_VACUUM_SCAN 记录，与索引
				 * 清理在所有情况下采用的策略相同。索引 VACUUM 的 WAL 记录
				 * 因此甚至没有属于自己的 snapshotConflictHorizon 字段。
				 */
			if (!ItemIdIsNormal(lp))
				break;

			htup = (HeapTupleHeader) PageGetItem(page, lp);

						/* 检查元组的 XMIN 与之前的 XMAX 是否一致（若有）
				 */
			if (TransactionIdIsValid(priorXmax) &&
				!TransactionIdEquals(HeapTupleHeaderGetXmin(htup), priorXmax))
				break;

			HeapTupleHeaderAdvanceConflictHorizon(htup,
												  &snapshotConflictHorizon);

						/* 如果元组不是 HOT 更新的，则我们已到达此 HOT 链的末尾。
				 * 无需访问同一更新链中后续的元组（它们有自己的索引项）
				 * —— 直接转到索引访问方法调用者给出的下一个 htid 即可。
				 */
			if (!HeapTupleHeaderIsHotUpdated(htup))
				break;

			/* 前进到下一个 HOT 链成员 */
			Assert(ItemPointerGetBlockNumber(&htup->t_ctid) == blkno);
			offnum = ItemPointerGetOffsetNumber(&htup->t_ctid);
			priorXmax = HeapTupleHeaderGetUpdateXid(htup);
		}

		/* 允许调用者进一步/最终缩小 deltids */
		finalndeltids = i + 1;
	}

	UnlockReleaseBuffer(buf);

		/* 缩小 deltids 数组，排除末尾不可删除的条目。这
		 * 不只是一个小优化。对于自底向上调用者，最终的 deltids 数组大小
		 * 可能为零。索引访问方法被明确允许依赖
		 * ndeltids 在所有可删除条目总数为零时为零这一事实。
		 */
	Assert(finalndeltids > 0 || delstate->bottomup);
	delstate->ndeltids = finalndeltids;

	return snapshotConflictHorizon;
}

/* index_delete_sort() 专用的可内联比较函数
	 */
static inline int
index_delete_sort_cmp(TM_IndexDelete *deltid1, TM_IndexDelete *deltid2)
{
	ItemPointer tid1 = &deltid1->tid;
	ItemPointer tid2 = &deltid2->tid;

	{
		BlockNumber blk1 = ItemPointerGetBlockNumber(tid1);
		BlockNumber blk2 = ItemPointerGetBlockNumber(tid2);

		if (blk1 != blk2)
			return (blk1 < blk2) ? -1 : 1;
	}
	{
		OffsetNumber pos1 = ItemPointerGetOffsetNumber(tid1);
		OffsetNumber pos2 = ItemPointerGetOffsetNumber(tid2);

		if (pos1 != pos2)
			return (pos1 < pos2) ? -1 : 1;
	}

	Assert(false);

	return 0;
}

/* 按 TID 对 delstate 中的 deltids 数组排序。这为
	 * heap_index_delete_tuples() 的后续处理做好准备。
	 *
	 * 在某些工作负载下，此操作会成为 CPU 周期的明显消耗者，因此
	 * 我们费心做了专门化/微优化。我们在此使用希尔排序，因为它易于
	 * 专门化、编译出的指令相对较少，并且能适应预排序的输入/子集
	 * （在此处很典型）。
	 */
static void
index_delete_sort(TM_IndexDeleteOp *delstate)
{
	TM_IndexDelete *deltids = delstate->deltids;
	int			ndeltids = delstate->ndeltids;

		/* 希尔排序的间隔序列（取自 Sedgewick-Incerpi 论文）。
		 *
		 * 此实现在数组大小不超过约 4500 时速度很快。这覆盖了
		 * 所有受支持的 BLCKSZ 取值。
		 */
	const int	gaps[9] = {1968, 861, 336, 112, 48, 21, 7, 3, 1};

	/* 在修改此处任何内容之前请仔细考虑 —— 保持交换廉价 */
	StaticAssertDecl(sizeof(TM_IndexDelete) <= 8,
					 "element size exceeds 8 bytes");

	for (int g = 0; g < lengthof(gaps); g++)
	{
		for (int hi = gaps[g], i = hi; i < ndeltids; i++)
		{
			TM_IndexDelete d = deltids[i];
			int			j = i;

			while (j >= hi && index_delete_sort_cmp(&deltids[j - hi], &d) >= 0)
			{
				deltids[j] = deltids[j - hi];
				j -= hi;
			}
			deltids[j] = d;
		}
	}
}

/* 返回在一次自底向上索引删除过程中，应被视为有利/连续的块数量。
	 * 这是从队列中第一个块开始并包含该块的若干堆块。
	 *
	 * 在自底向上索引删除过程中，始终至少有一个有利块。在最坏情况下
	 * （即堆块完全随机时），队列中的第一个块（唯一的有利块）可被视为
	 * 由单个块组成的退化连续块数组。heap_index_delete_tuples() 会预期这一点。
	 *
	 * 调用者传入 blockgroups，即对 deltids 将为 heap_index_delete_tuples()
	 * 自底向上索引删除处理进行排序的最终顺序的描述。注意，deltids 实际上
	 * 尚无需排序（调用者只是把 deltids 传给我们，以便我们据此解释 blockgroups）。
	 *
	 * 你可能会认为连续块的存在无足轻重，因为一般而言，决定我们访问哪些块
	 * 的主要因素是 promising TID 的数量，而这只是来自索引访问方法的一个
	 * 固定提示。不过我们真正针对的并非一般情况 —— 实际目标是让我们的行为
	 * 适应各种各样自然出现的条件。我们所应用的大部分启发式方法的效果，
	 * 只有在经过时间、跨越许多_相关_的自底向上索引删除过程后，在总体上才能显现。
	 *
	 * 将某些块视为有利，使 heapam 能够识别并适应这样的工作负载：在自底向上
	 * 索引删除期间访问的堆块可以被连续访问，即每个新访问的块都是自底向上
	 * 删除刚处理完的块的邻居（或与之足够接近）。尽早访问更有利的块可能更划算
	 * （例如在本次过程中，而非跨一系列相关自底向上过程）。无论如何，以单个大批次
	 * 有利块形式一同出现的所有块，被_某个_自底向上过程访问，很可能只是时间问题
	 * （或进一步相关版本变动的问题）。大批次的有利块往往要么几乎持续出现，
	 * 要么一次都不出现（这完全取决于各索引的工作负载特征）。
	 *
	 * 注意，blockgroups 的排序顺序应用了二的幂分桶方案，至少为那些自然适合
	 * 由堆块局部性驱动的负载，创造了将连续块组归并到一起的机会。这不仅以显而易见的
	 * 方式增强了自底向上堆块处理的空间局部性，还实现了访问的时间局部性，因为按堆块
	 * 编号排序自然倾向于使自底向上处理顺序变得确定。
	 *
	 * 考虑以下例子，以体会时间局部性可能的重要性：有一个堆关系带多个索引，
	 * 每个索引的基数都是低到中等。它持续承受非 HOT 更新。更新是倾斜的
	 * （可能集中在主键的某一部分）。没有任何索引被 UPDATE 语句在逻辑上修改
	 * （否则自底向上索引删除根本不会触发）。自然地，每一轮新的索引元组
	 * （针对每个调用 heap_update() 的堆元组）在每个索引中都会具有相同的堆 TID。
	 * 由于这些索引基数低且从不在逻辑上被修改，自底向上删除过程中的 heapam 处理
	 * 会大致按序访问堆块。访问的时间局部性之所以出现，是因为自底向上删除过程
	 * 在任何给定时刻在各索引上的行为都非常相似。这将访问堆块所需的缓冲未命中数
	 * 保持在最小。
	 */
static int
bottomup_nblocksfavorable(IndexDeleteCounts *blockgroups, int nblockgroups,
						  TM_IndexDelete *deltids)
{
	int64		lastblock = -1;
	int			nblocksfavorable = 0;

	Assert(nblockgroups >= 1);
	Assert(nblockgroups <= BOTTOMUP_MAX_NBLOCKS);

		/* 我们容忍那些物理顺序上仅有轻微错乱的堆块。当一对几乎
		 * 连续的块恰好落入不同的桶时（可能仅仅因为分桶方案
		 * 未能完全忽略的 npromisingtids 微小差异），就会出现小的
		 * 波动。我们施加一个小的容差，从而有效地忽略这些波动。
		 * 我们使用的精确容差略显随意，但在实践中足够有效。
		 */
	for (int b = 0; b < nblockgroups; b++)
	{
		IndexDeleteCounts *group = blockgroups + b;
		TM_IndexDelete *firstdtid = deltids + group->ifirsttid;
		BlockNumber block = ItemPointerGetBlockNumber(&firstdtid->tid);

		if (lastblock != -1 &&
			((int64) block < lastblock - BOTTOMUP_TOLERANCE_NBLOCKS ||
			 (int64) block > lastblock + BOTTOMUP_TOLERANCE_NBLOCKS))
			break;

		nblocksfavorable++;
		lastblock = block;
	}

	/* 始终指示至少有一个有利块 */
	Assert(nblocksfavorable >= 1);

	return nblocksfavorable;
}

/* bottomup_sort_and_shrink() 的 qsort 比较函数
	 */
static int
bottomup_sort_and_shrink_cmp(const void *arg1, const void *arg2)
{
	const IndexDeleteCounts *group1 = (const IndexDeleteCounts *) arg1;
	const IndexDeleteCounts *group2 = (const IndexDeleteCounts *) arg2;

		/* 最重要的字段是 npromisingtids（我们将其排序顺序反转，
		 * 以便按降序排序）。
		 *
		 * 调用者应已把 npromisingtids 字段归一化为二的幂
		 * 取值（桶）。
		 */
	if (group1->npromisingtids > group2->npromisingtids)
		return -1;
	if (group1->npromisingtids < group2->npromisingtids)
		return 1;

		/* 决胜规则：ntids 降序排序。
		 *
		 * 我们不能指望 ntids 字段是二的幂取值。我们应该
		 * 表现得就像它们已经为我们向上取整了一样。
		 */
	if (group1->ntids != group2->ntids)
	{
		uint32		ntids1 = pg_nextpower2_32((uint32) group1->ntids);
		uint32		ntids2 = pg_nextpower2_32((uint32) group2->ntids);

		if (ntids1 > ntids2)
			return -1;
		if (ntids1 < ntids2)
			return 1;
	}

		/* 决胜规则：deltids 中块的偏移（块在 deltids 数组中对应
		 * 第一个 TID 的偏移）升序。
		 *
		 * 这等价于按堆块编号升序排序（在数组其余相等的
		 * 子集中）。这种方式使我们无需访问行外 TID。（我们依赖的
		 * 假设是：在形成这些来自各堆块组第一个 TID 的偏移时，
		 * deltids 数组已按堆 TID 升序排序。）
		 */
	if (group1->ifirsttid > group2->ifirsttid)
		return 1;
	if (group1->ifirsttid < group2->ifirsttid)
		return -1;

	pg_unreachable();

	return 0;
}

/* 面向自底向上删除调用者的 heap_index_delete_tuples() 辅助函数。
	 *
	 * 按自底向上删除进行有效处理所需的顺序对 deltids 数组排序。
	 * 调用我们时，该数组应已按 TID 顺序排好。排序过程把 deltids
	 * 中的堆 TID 分组为堆块组。较早/更有希望的组/块通常就是那些
	 * 已知拥有最多 "promising" TID 的。
	 *
	 * 在状态中设置 deltids 数组的新大小（ndeltids）。返回时，
	 * deltids 只会包含来自 BOTTOMUP_MAX_NBLOCKS 个最有希望堆块的 TID。
	 * 这通常意味着 deltids 会被缩小到原始大小的一小部分
	 * （我们事先就为调用者排除了许多堆块）。
	 *
	 * 返回 "有利" 块的数量。定义与完整细节见
	 * bottomup_nblocksfavorable()。
	 */
static int
bottomup_sort_and_shrink(TM_IndexDeleteOp *delstate)
{
	IndexDeleteCounts *blockgroups;
	TM_IndexDelete *reordereddeltids;
	BlockNumber curblock = InvalidBlockNumber;
	int			nblockgroups = 0;
	int			ncopied = 0;
	int			nblocksfavorable = 0;

	Assert(delstate->bottomup);
	Assert(delstate->ndeltids > 0);

	/* 计算每个堆块的 TID 数量 */
	blockgroups = palloc(sizeof(IndexDeleteCounts) * delstate->ndeltids);
	for (int i = 0; i < delstate->ndeltids; i++)
	{
		TM_IndexDelete *ideltid = &delstate->deltids[i];
		TM_IndexStatus *istatus = delstate->status + ideltid->id;
		ItemPointer htid = &ideltid->tid;
		bool		promising = istatus->promising;

		if (curblock != ItemPointerGetBlockNumber(htid))
		{
			/* 新块组 */
			nblockgroups++;

			Assert(curblock < ItemPointerGetBlockNumber(htid) ||
				   !BlockNumberIsValid(curblock));

			curblock = ItemPointerGetBlockNumber(htid);
			blockgroups[nblockgroups - 1].ifirsttid = i;
			blockgroups[nblockgroups - 1].ntids = 1;
			blockgroups[nblockgroups - 1].npromisingtids = 0;
		}
		else
		{
			blockgroups[nblockgroups - 1].ntids++;
		}

		if (promising)
			blockgroups[nblockgroups - 1].npromisingtids++;
	}

		/* 我们即将对块组进行排序，以确定访问堆块的最佳顺序。
		 * 但在那之前，先把每个块组的 promising 元组数量向上取整到
		 * 下一个二的幂，除非它很低（小于 4），此时取整到 4。当要在
		 * 两个取值都很低的块组之间做选择时，npromisingtids 噪声太大
		 * 而不可信。
		 *
		 * 此方案将堆块/块组划分到各个桶中。每个桶包含彼此
		 * _大致_ 拥有相同数量 promising TID 的块。目的是忽略
		 * promising 条目总数上相对微小的差异，从而让整个过程
		 * 可以把少许权重让给 heapam 因素（如堆块局部性）。
		 * 这其实并非权衡 —— 我们毫无损失。把 npromisingtids 值的微小
		 * 差异当作噪声以外的东西来解读是愚蠢的。
		 *
		 * 在对具有相同 npromisingtids 的块组子集排序时，我们以
		 * nhtids 作为决胜依据，但这与 npromisingtids 有同样的问题，
		 * 因此 nhtids 也采用同样的二的幂分桶方案。我们在此不对其
		 * 做同样修复的唯一原因是，排序之后我们还需要准确的 nhtids
		 * 值。我们改为动态处理 nhtids 的分桶（在排序比较函数中）。
		 *
		 * 关于堆局部性/有利块何时以及如何显著影响堆块的访问，
		 * 完整说明见 bottomup_nblocksfavorable()。
		 */
	for (int b = 0; b < nblockgroups; b++)
	{
		IndexDeleteCounts *group = blockgroups + b;

		/* 在 npromisingtids 较低时，回退到 nhtids 更好 */
		if (group->npromisingtids <= 4)
			group->npromisingtids = 4;
		else
			group->npromisingtids =
				pg_nextpower2_32((uint32) group->npromisingtids);
	}

	/* 排序组并重新排列调用者的 deltids 数组 */
	qsort(blockgroups, nblockgroups, sizeof(IndexDeleteCounts),
		  bottomup_sort_and_shrink_cmp);
	reordereddeltids = palloc(delstate->ndeltids * sizeof(TM_IndexDelete));

	nblockgroups = Min(BOTTOMUP_MAX_NBLOCKS, nblockgroups);
	/* 确定最终 deltids 开头有多少个有利块 */
	nblocksfavorable = bottomup_nblocksfavorable(blockgroups, nblockgroups,
												 delstate->deltids);

	for (int b = 0; b < nblockgroups; b++)
	{
		IndexDeleteCounts *group = blockgroups + b;
		TM_IndexDelete *firstdtid = delstate->deltids + group->ifirsttid;

		memcpy(reordereddeltids + ncopied, firstdtid,
			   sizeof(TM_IndexDelete) * group->ntids);
		ncopied += group->ntids;
	}

	/* 将最终分组和排序的 TID 复制回调用者数组的开头 */
	memcpy(delstate->deltids, reordereddeltids,
		   sizeof(TM_IndexDelete) * ncopied);
	delstate->ndeltids = ncopied;

	pfree(reordereddeltids);
	pfree(blockgroups);

	return nblocksfavorable;
}

/* 为堆可见性操作执行 XLogInsert。'block' 是被标记为全可见的块，
	 * vm_buffer 是包含相应可见性映射块的缓冲区。两者都应已被
	 * 修改并置脏。
	 *
	 * snapshotConflictHorizon 来自被标记为全可见的页面上最大的 xmin。
	 * REDO 例程用它来生成恢复冲突。
	 *
	 * 如果启用了校验和或 wal_log_hints，我们还可能生成 heap_buffer 的
	 * 整页镜像。否则，我们优化掉该 FPI（为堆缓冲区指定 REGBUF_NO_IMAGE），
	 * 这种情况下调用者*不应*更新堆页面的 LSN。
	 */
XLogRecPtr
log_heap_visible(Relation rel, Buffer heap_buffer, Buffer vm_buffer,
				 TransactionId snapshotConflictHorizon, uint8 vmflags)
{
	xl_heap_visible xlrec;
	XLogRecPtr	recptr;
	uint8		flags;

	Assert(BufferIsValid(heap_buffer));
	Assert(BufferIsValid(vm_buffer));

	xlrec.snapshotConflictHorizon = snapshotConflictHorizon;
	xlrec.flags = vmflags;
	if (RelationIsAccessibleInLogicalDecoding(rel))
		xlrec.flags |= VISIBILITYMAP_XLOG_CATALOG_REL;
	XLogBeginInsert();
	XLogRegisterData(&xlrec, SizeOfHeapVisible);

	XLogRegisterBuffer(0, vm_buffer, 0);

	flags = REGBUF_STANDARD;
	if (!XLogHintBitIsNeeded())
		flags |= REGBUF_NO_IMAGE;
	XLogRegisterBuffer(1, heap_buffer, flags);

	recptr = XLogInsert(RM_HEAP2_ID, XLOG_HEAP2_VISIBLE);

	return recptr;
}

/* 为堆更新操作执行 XLogInsert。调用者必须已经修改了缓冲区并将其置脏。
	 */
static XLogRecPtr
log_heap_update(Relation reln, Buffer oldbuf,
				Buffer newbuf, HeapTuple oldtup, HeapTuple newtup,
				HeapTuple old_key_tuple,
				bool all_visible_cleared, bool new_all_visible_cleared)
{
	xl_heap_update xlrec;
	xl_heap_header xlhdr;
	xl_heap_header xlhdr_idx;
	uint8		info;
	uint16		prefix_suffix[2];
	uint16		prefixlen = 0,
				suffixlen = 0;
	XLogRecPtr	recptr;
	Page		page = BufferGetPage(newbuf);
	bool		need_tuple_data = RelationIsLogicallyLogged(reln);
	bool		init;
	int			bufflags;

	/* 调用者不应在非 WAL 日志关系上调用我 */
	Assert(RelationNeedsWAL(reln));

	XLogBeginInsert();

	if (HeapTupleIsHeapOnly(newtup))
		info = XLOG_HEAP_HOT_UPDATE;
	else
		info = XLOG_HEAP_UPDATE;

		/* 如果新旧元组在同一页面上，我们只需记录新元组中被修改的
		 * 部分。这节省了需要写入的 WAL 量。目前，我们只是统计元组
		 * 开头和结尾处未变化的字节数。这样检查很快，并且
		 * 完美覆盖了只有一个字段被更新的常见情况。
		 *
		 * 即使新旧元组在不同页面上，我们也可以这样做，但前提是我们
		 * 不为旧页面制作整页镜像，而这很难提前知晓。此外，如果旧元组
		 * 因某种原因损坏，这会让损坏传播到新页面，因此最好避免。
		 * 基于"大多数更新倾向于在同一页面上创建新元组版本"这一普遍
		 * 假设，跨页面这样做其实收益不大。
		 *
		 * 如果我们为新手页面制作整页镜像，则跳过此处理，因为那种情况下
		 * 我们不会把新元组纳入 WAL 记录。同样，如果 wal_level='logical'
		 * 也禁用，因为逻辑解码需要能够仅从 WAL 记录中完整读取新元组。
		 */
	if (oldbuf == newbuf && !need_tuple_data &&
		!XLogCheckBufferNeedsBackup(newbuf))
	{
		char	   *oldp = (char *) oldtup->t_data + oldtup->t_data->t_hoff;
		char	   *newp = (char *) newtup->t_data + newtup->t_data->t_hoff;
		int			oldlen = oldtup->t_len - oldtup->t_data->t_hoff;
		int			newlen = newtup->t_len - newtup->t_data->t_hoff;

		/* 检查新旧元组之间的公共前缀 */
		for (prefixlen = 0; prefixlen < Min(oldlen, newlen); prefixlen++)
		{
			if (newp[prefixlen] != oldp[prefixlen])
				break;
		}

				/* 存储前缀长度需要 2 字节，因此我们需要至少节省 3 字节，
			 * 否则就没有意义。
			 */
		if (prefixlen < 3)
			prefixlen = 0;

		/* 后缀同理 */
		for (suffixlen = 0; suffixlen < Min(oldlen, newlen) - prefixlen; suffixlen++)
		{
			if (newp[newlen - suffixlen - 1] != oldp[oldlen - suffixlen - 1])
				break;
		}
		if (suffixlen < 3)
			suffixlen = 0;
	}

	/* 准备主 WAL 数据链 */
	xlrec.flags = 0;
	if (all_visible_cleared)
		xlrec.flags |= XLH_UPDATE_OLD_ALL_VISIBLE_CLEARED;
	if (new_all_visible_cleared)
		xlrec.flags |= XLH_UPDATE_NEW_ALL_VISIBLE_CLEARED;
	if (prefixlen > 0)
		xlrec.flags |= XLH_UPDATE_PREFIX_FROM_OLD;
	if (suffixlen > 0)
		xlrec.flags |= XLH_UPDATE_SUFFIX_FROM_OLD;
	if (need_tuple_data)
	{
		xlrec.flags |= XLH_UPDATE_CONTAINS_NEW_TUPLE;
		if (old_key_tuple)
		{
			if (reln->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
				xlrec.flags |= XLH_UPDATE_CONTAINS_OLD_TUPLE;
			else
				xlrec.flags |= XLH_UPDATE_CONTAINS_OLD_KEY;
		}
	}

	/* 如果新元组是页面上唯一且第一个元组... */
	if (ItemPointerGetOffsetNumber(&(newtup->t_self)) == FirstOffsetNumber &&
		PageGetMaxOffsetNumber(page) == FirstOffsetNumber)
	{
		info |= XLOG_HEAP_INIT_PAGE;
		init = true;
	}
	else
		init = false;

	/* 准备旧页面的 WAL 数据 */
	xlrec.old_offnum = ItemPointerGetOffsetNumber(&oldtup->t_self);
	xlrec.old_xmax = HeapTupleHeaderGetRawXmax(oldtup->t_data);
	xlrec.old_infobits_set = compute_infobits(oldtup->t_data->t_infomask,
											  oldtup->t_data->t_infomask2);

	/* 准备新页面的 WAL 数据 */
	xlrec.new_offnum = ItemPointerGetOffsetNumber(&newtup->t_self);
	xlrec.new_xmax = HeapTupleHeaderGetRawXmax(newtup->t_data);

	bufflags = REGBUF_STANDARD;
	if (init)
		bufflags |= REGBUF_WILL_INIT;
	if (need_tuple_data)
		bufflags |= REGBUF_KEEP_DATA;

	XLogRegisterBuffer(0, newbuf, bufflags);
	if (oldbuf != newbuf)
		XLogRegisterBuffer(1, oldbuf, REGBUF_STANDARD);

	XLogRegisterData(&xlrec, SizeOfHeapUpdate);

		/* 为新元组准备 WAL 数据。
		 */
	if (prefixlen > 0 || suffixlen > 0)
	{
		if (prefixlen > 0 && suffixlen > 0)
		{
			prefix_suffix[0] = prefixlen;
			prefix_suffix[1] = suffixlen;
			XLogRegisterBufData(0, &prefix_suffix, sizeof(uint16) * 2);
		}
		else if (prefixlen > 0)
		{
			XLogRegisterBufData(0, &prefixlen, sizeof(uint16));
		}
		else
		{
			XLogRegisterBufData(0, &suffixlen, sizeof(uint16));
		}
	}

	xlhdr.t_infomask2 = newtup->t_data->t_infomask2;
	xlhdr.t_infomask = newtup->t_data->t_infomask;
	xlhdr.t_hoff = newtup->t_data->t_hoff;
	Assert(SizeofHeapTupleHeader + prefixlen + suffixlen <= newtup->t_len);

		/* PG73FORMAT：写入位图 [+ 填充] [+ oid] + 数据
		 *
		 * 此 'data' 不含公共前缀或后缀。
		 */
	XLogRegisterBufData(0, &xlhdr, SizeOfHeapHeader);
	if (prefixlen == 0)
	{
		XLogRegisterBufData(0,
							(char *) newtup->t_data + SizeofHeapTupleHeader,
							newtup->t_len - SizeofHeapTupleHeader - suffixlen);
	}
	else
	{
				/* 必须把公共前缀之后的空值位图和数据作为两个独立的
			 * rdata 条目写入。
			 */
		/* 位图 [+ 填充] [+ oid] */
		if (newtup->t_data->t_hoff - SizeofHeapTupleHeader > 0)
		{
			XLogRegisterBufData(0,
								(char *) newtup->t_data + SizeofHeapTupleHeader,
								newtup->t_data->t_hoff - SizeofHeapTupleHeader);
		}

		/* 公共前缀之后的数据 */
		XLogRegisterBufData(0,
							(char *) newtup->t_data + newtup->t_data->t_hoff + prefixlen,
							newtup->t_len - newtup->t_data->t_hoff - prefixlen - suffixlen);
	}

	/* 需要记录元组标识 */
	if (need_tuple_data && old_key_tuple)
	{
		/* 实际上不需要这个，但解码时更方便 */
		xlhdr_idx.t_infomask2 = old_key_tuple->t_data->t_infomask2;
		xlhdr_idx.t_infomask = old_key_tuple->t_data->t_infomask;
		xlhdr_idx.t_hoff = old_key_tuple->t_data->t_hoff;

		XLogRegisterData(&xlhdr_idx, SizeOfHeapHeader);

		/* PG73FORMAT: 写入位图 [+ 填充] [+ oid] + 数据 */
		XLogRegisterData((char *) old_key_tuple->t_data + SizeofHeapTupleHeader,
						 old_key_tuple->t_len - SizeofHeapTupleHeader);
	}

	/* 在行级别按 origin 过滤效率更高 */
	XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

	recptr = XLogInsert(RM_HEAP_ID, info);

	return recptr;
}

/* 为 XLOG_HEAP2_NEW_CID 记录执行 XLogInsert
	 *
	 * 这仅用于 wal_level >= WAL_LEVEL_LOGICAL，且仅用于系统表元组。
	 */
static XLogRecPtr
log_heap_new_cid(Relation relation, HeapTuple tup)
{
	xl_heap_new_cid xlrec;

	XLogRecPtr	recptr;
	HeapTupleHeader hdr = tup->t_data;

	Assert(ItemPointerIsValid(&tup->t_self));
	Assert(tup->t_tableOid != InvalidOid);

	xlrec.top_xid = GetTopTransactionId();
	xlrec.target_locator = relation->rd_locator;
	xlrec.target_tid = tup->t_self;

		/* 如果元组在同一事务中被插入并删除，我们一定有
		 * 组合 CID，设置 cmin 和 cmax。
		 */
	if (hdr->t_infomask & HEAP_COMBOCID)
	{
		Assert(!(hdr->t_infomask & HEAP_XMAX_INVALID));
		Assert(!HeapTupleHeaderXminInvalid(hdr));
		xlrec.cmin = HeapTupleHeaderGetCmin(hdr);
		xlrec.cmax = HeapTupleHeaderGetCmax(hdr);
		xlrec.combocid = HeapTupleHeaderGetRawCommandId(hdr);
	}
	/* 没有组合 CID，因此此事务只能设置 cmin 或 cmax */
	else
	{
				/* 元组被插入。
			 *
			 * 我们需要检查是否为 LOCK ONLY，因为在 FOR KEY SHARE
			 * 更新的情况下，multixact 可能被转移到新元组上，
			 * 此时虽然元组刚刚被插入，却会存在一个 xmax。
			 */
		if (hdr->t_infomask & HEAP_XMAX_INVALID ||
			HEAP_XMAX_IS_LOCKED_ONLY(hdr->t_infomask))
		{
			xlrec.cmin = HeapTupleHeaderGetRawCommandId(hdr);
			xlrec.cmax = InvalidCommandId;
		}
		/* 来自不同事务的元组被更新或删除。 */
		else
		{
			xlrec.cmin = InvalidCommandId;
			xlrec.cmax = HeapTupleHeaderGetRawCommandId(hdr);
		}
		xlrec.combocid = InvalidCommandId;
	}

		/* 注意，我们无需在此注册缓冲区，因为此操作
		 * 不会修改页面。调用我们的插入/更新/删除操作确实修改了，
		 * 但那部分已单独写入 WAL。
		 */
	XLogBeginInsert();
	XLogRegisterData(&xlrec, SizeOfHeapNewCid);

	/* 无论 origin 如何都会被查看 */

	recptr = XLogInsert(RM_HEAP2_ID, XLOG_HEAP2_NEW_CID);

	return recptr;
}

/* 构建一个代表所配置 REPLICA IDENTITY 的堆元组，用于在
	 * UPDATE 或 DELETE 中表示旧元组。
	 *
	 * 如果无需记录标识，或没有定义合适的键，则返回 NULL。
	 *
	 * 如果有任何 replica identity 列改变了值，或其中任何列含有
	 * 外部数据，则将 key_required 传为 true。删除必须始终传 true。
	 *
	 * 如果返回的元组是修改后的副本而非传入的那个元组，
	 * 则 *copy 被设为 true。
	 */
static HeapTuple
ExtractReplicaIdentity(Relation relation, HeapTuple tp, bool key_required,
					   bool *copy)
{
	TupleDesc	desc = RelationGetDescr(relation);
	char		replident = relation->rd_rel->relreplident;
	Bitmapset  *idattrs;
	HeapTuple	key_tuple;
	bool		nulls[MaxHeapAttributeNumber];
	Datum		values[MaxHeapAttributeNumber];

	*copy = false;

	if (!RelationIsLogicallyLogged(relation))
		return NULL;

	if (replident == REPLICA_IDENTITY_NOTHING)
		return NULL;

	if (replident == REPLICA_IDENTITY_FULL)
	{
				/* 当记录整个旧元组时，它很可能会包含被 toast 的列。
			 * 若如此，强制将它们内联化。
			 */
		if (HeapTupleHasExternal(tp))
		{
			*copy = true;
			tp = toast_flatten_tuple(tp, desc);
		}
		return tp;
	}

	/* 如果不需要键且我们只记录键，则完成 */
	if (!key_required)
		return NULL;

	/* 找出 replica identity 列 */
	idattrs = RelationGetIndexAttrBitmap(relation,
										 INDEX_ATTR_BITMAP_IDENTITY_KEY);

		/* 如果没有定义 replica identity 列，则视为 !key_required。
		 * （这种情况不应从 heap_update 到达，因为后者应准确计算
		 * key_required。但 heap_delete 只是为 key_required 传入常量
		 * true，因此删除时可能走到这里。）
		 */
	if (bms_is_empty(idattrs))
		return NULL;

		/* 构建一个只包含 replica identity 列的新元组，其余位置为
		 * 空。顺带断言 replica identity 列不为空。
		 */
	heap_deform_tuple(tp, desc, values, nulls);

	for (int i = 0; i < desc->natts; i++)
	{
		if (bms_is_member(i + 1 - FirstLowInvalidHeapAttributeNumber,
						  idattrs))
			Assert(!nulls[i]);
		else
			nulls[i] = true;
	}

	key_tuple = heap_form_tuple(desc, values, nulls);
	*copy = true;

	bms_free(idattrs);

		/* 如果到此只含索引列的元组仍然含有被 toast 的列，
		 * 强制将它们内联化。这不太可能发生，因为索引列的大小有限制，
		 * 所以即便更高效，我们也没有在上面的索引列循环中
		 * 重复 toast_flatten_tuple() 的功能。
		 */
	if (HeapTupleHasExternal(key_tuple))
	{
		HeapTuple	oldtup = key_tuple;

		key_tuple = toast_flatten_tuple(oldtup, desc);
		heap_freetuple(oldtup);
	}

	return key_tuple;
}

/* HeapCheckForSerializableConflictOut
	 * 我们正在读取一个元组。如果它不可见，可能与插入者存在
	 * 读-写冲突（rw-conflict out）。否则，如果它对可见，
	 * 但已被删除，则可能与删除者存在读-写冲突。
	 *
	 * 我们将确定可能产生冲突的写入事务的顶层 xid，并请
	 * CheckForSerializableConflictOut() 检查与我们自身事务的重叠。
	 *
	 * 在 heapam.c 中只要读取过元组的地方，几乎都应调用此函数。
	 * 调用者必须至少持有缓冲区的共享锁，因为此函数可能会
	 * 在元组上设置提示位。目前没有已知的理由要从索引访问方法中
	 * 调用此函数。
	 */
void
HeapCheckForSerializableConflictOut(bool visible, Relation relation,
									HeapTuple tuple, Buffer buffer,
									Snapshot snapshot)
{
	TransactionId xid;
	HTSV_Result htsvResult;

	if (!CheckForSerializableConflictOutNeeded(relation, snapshot))
		return;

		/* 检查元组是否已被并发事务写入：要么创建出对我们不可见
		 * 的元组，要么在我们可见时将其删除。"visible" 布尔值表示
		 * 元组是否对我们可见，而 HeapTupleSatisfiesVacuum 检查它还有
		 * 什么其他情况在发生。
		 *
		 * 如果遇到一个并发插入、又恰好被并发更新（由另一事务）的
		 * 元组，将使用元组的 xmin —— 而不是更新者的 xid。
		 */
	htsvResult = HeapTupleSatisfiesVacuum(tuple, TransactionXmin, buffer);
	switch (htsvResult)
	{
		case HEAPTUPLE_LIVE:
			if (visible)
				return;
			xid = HeapTupleHeaderGetXmin(tuple->t_data);
			break;
		case HEAPTUPLE_RECENTLY_DEAD:
		case HEAPTUPLE_DELETE_IN_PROGRESS:
			if (visible)
				xid = HeapTupleHeaderGetUpdateXid(tuple->t_data);
			else
				xid = HeapTupleHeaderGetXmin(tuple->t_data);

			if (TransactionIdPrecedes(xid, TransactionXmin))
			{
				/* 这类似于 HEAPTUPLE_DEAD 情况 */
				Assert(!visible);
				return;
			}
			break;
		case HEAPTUPLE_INSERT_IN_PROGRESS:
			xid = HeapTupleHeaderGetXmin(tuple->t_data);
			break;
		case HEAPTUPLE_DEAD:
			Assert(!visible);
			return;
		default:

						/* 能到达此 default 子句的唯一途径，是向枚举类型
				 * 新增了一个值却没有在 switch 语句中加入它。
				 * 这是一个 bug，因此 elog。
				 */
			elog(ERROR, "unrecognized return value from HeapTupleSatisfiesVacuum: %u", htsvResult);

						/* 尽管已覆盖所有枚举值并在此 default 中调用了 elog，
				 * 某些编译器仍认为这是一条允许下方在未初始化情况下
				 * 使用 xid 的代码路径。消除该警告。
				 */
			xid = InvalidTransactionId;
	}

	Assert(TransactionIdIsValid(xid));
	Assert(TransactionIdFollowsOrEquals(xid, TransactionXmin));

		/* 查找顶层 xid。如果 xid 过早而不构成冲突，或
		 * 是它就是我们自己的 xid，则退出。
		 */
	if (TransactionIdEquals(xid, GetTopTransactionIdIfAny()))
		return;
	xid = SubTransGetTopmostTransaction(xid);
	if (TransactionIdPrecedes(xid, TransactionXmin))
		return;

	CheckForSerializableConflictOut(relation, xid, snapshot);
}
