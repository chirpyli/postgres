/*-------------------------------------------------------------------------
 *
 * heapam_handler.c
 *	  heap 表访问方法相关代码
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/heapam_handler.c
 *
 *
 * NOTES
 *	  本文件将 heapam.c 等底层例程与 tableam 抽象层连接起来。
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/heaptoast.h"
#include "access/multixact.h"
#include "access/rewriteheap.h"
#include "access/syncscan.h"
#include "access/tableam.h"
#include "access/tsmapi.h"
#include "access/visibilitymap.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/index.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "commands/progress.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/rel.h"

static void reform_and_rewrite_tuple(HeapTuple tuple,
									 Relation OldHeap, Relation NewHeap,
									 Datum *values, bool *isnull, RewriteState rwstate);

static bool SampleHeapTupleVisible(TableScanDesc scan, Buffer buffer,
								   HeapTuple tuple,
								   OffsetNumber tupoffset);

static BlockNumber heapam_scan_get_blocks_done(HeapScanDesc hscan);

static bool BitmapHeapScanNextBlock(TableScanDesc scan,
									bool *recheck,
									uint64 *lossy_pages, uint64 *exact_pages);


/* ------------------------------------------------------------------------
 * 与 slot 相关的堆 AM 回调
 * ------------------------------------------------------------------------
 */

static const TupleTableSlotOps *
heapam_slot_callbacks(Relation relation)
{
	return &TTSOpsBufferHeapTuple;
}


/* ------------------------------------------------------------------------
 * heap AM 中与索引扫描相关的回调
 * ------------------------------------------------------------------------
 */

static IndexFetchTableData *
heapam_index_fetch_begin(Relation rel)
{
	IndexFetchHeapData *hscan = palloc0(sizeof(IndexFetchHeapData));

	hscan->xs_base.rel = rel;
	hscan->xs_cbuf = InvalidBuffer;

	return &hscan->xs_base;
}

static void
heapam_index_fetch_reset(IndexFetchTableData *scan)
{
	IndexFetchHeapData *hscan = (IndexFetchHeapData *) scan;

	if (BufferIsValid(hscan->xs_cbuf))
	{
		ReleaseBuffer(hscan->xs_cbuf);
		hscan->xs_cbuf = InvalidBuffer;
	}
}

static void
heapam_index_fetch_end(IndexFetchTableData *scan)
{
	IndexFetchHeapData *hscan = (IndexFetchHeapData *) scan;

	heapam_index_fetch_reset(scan);

	pfree(hscan);
}

static bool
heapam_index_fetch_tuple(struct IndexFetchTableData *scan,
						 ItemPointer tid,
						 Snapshot snapshot,
						 TupleTableSlot *slot,
						 bool *call_again, bool *all_dead)
{
	IndexFetchHeapData *hscan = (IndexFetchHeapData *) scan;
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	bool		got_heap_tuple;

	Assert(TTS_IS_BUFFERTUPLE(slot));

	/* 如果正处于 HOT 链中间，可以跳过缓冲区切换逻辑。 */
	if (!*call_again)
	{
		/* 如果还没有正确的缓冲区，则切换到正确的缓冲区 */
		Buffer		prev_buf = hscan->xs_cbuf;

		hscan->xs_cbuf = ReleaseAndReadBuffer(hscan->xs_cbuf,
											  hscan->xs_base.rel,
											  ItemPointerGetBlockNumber(tid));

		/*
		 * 仅当我们之前不在这个页面上时才进行页面剪枝
		 */
		if (prev_buf != hscan->xs_cbuf)
			heap_page_prune_opt(hscan->xs_base.rel, hscan->xs_cbuf);
	}

	/* 获取缓冲区的共享锁，以便检查可见性 */
	LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_SHARE);
	got_heap_tuple = heap_hot_search_buffer(tid,
											hscan->xs_base.rel,
											hscan->xs_cbuf,
											snapshot,
											&bslot->base.tupdata,
											all_dead,
											!*call_again);
	bslot->base.tupdata.t_self = *tid;
	LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_UNLOCK);

	if (got_heap_tuple)
	{
		/*
		 * 只有在非 MVCC 快照下，HOT 链中才可能有多个成员可见。
		 */
		*call_again = !IsMVCCSnapshot(snapshot);

		slot->tts_tableOid = RelationGetRelid(scan->rel);
		ExecStoreBufferHeapTuple(&bslot->base.tupdata, slot, hscan->xs_cbuf);
	}
	else
	{
		/* 已经到达 HOT 链的末尾。 */
		*call_again = false;
	}

	return got_heap_tuple;
}


/* ------------------------------------------------------------------------
 * heap AM 中针对单个元组的非修改操作回调
 * ------------------------------------------------------------------------
 */

static bool
heapam_fetch_row_version(Relation relation,
						 ItemPointer tid,
						 Snapshot snapshot,
						 TupleTableSlot *slot)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	Buffer		buffer;

	Assert(TTS_IS_BUFFERTUPLE(slot));

	bslot->base.tupdata.t_self = *tid;
	if (heap_fetch(relation, snapshot, &bslot->base.tupdata, &buffer, false))
	{
		/* 存入 slot 中，并转移已有的 pin */
		ExecStorePinnedBufferHeapTuple(&bslot->base.tupdata, slot, buffer);
		slot->tts_tableOid = RelationGetRelid(relation);

		return true;
	}

	return false;
}

static bool
heapam_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	HeapScanDesc hscan = (HeapScanDesc) scan;

	return ItemPointerIsValid(tid) &&
		ItemPointerGetBlockNumber(tid) < hscan->rs_nblocks;
}

static bool
heapam_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
								Snapshot snapshot)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	bool		res;

	Assert(TTS_IS_BUFFERTUPLE(slot));
	Assert(BufferIsValid(bslot->buffer));

	/*
	 * 调用 HeapTupleSatisfiesVisibility 需要持有缓冲区的 pin 和锁。
	 * 调用方应当已经持有 pin，但不持有锁。
	 */
	LockBuffer(bslot->buffer, BUFFER_LOCK_SHARE);
	res = HeapTupleSatisfiesVisibility(bslot->base.tuple, snapshot,
									   bslot->buffer);
	LockBuffer(bslot->buffer, BUFFER_LOCK_UNLOCK);

	return res;
}


/* ----------------------------------------------------------------------------
 *  heap AM 中用于操作物理元组的函数。
 * ----------------------------------------------------------------------------
 */

static void
heapam_tuple_insert(Relation relation, TupleTableSlot *slot, CommandId cid,
					int options, BulkInsertState bistate)
{
	bool		shouldFree = true;
	HeapTuple	tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);

	/* 用表的 oid 更新元组 */
	slot->tts_tableOid = RelationGetRelid(relation);
	tuple->t_tableOid = slot->tts_tableOid;

	/* 执行插入，并复制得到的 ItemPointer */
	heap_insert(relation, tuple, cid, options, bistate);
	ItemPointerCopy(&tuple->t_self, &slot->tts_tid);

	if (shouldFree)
		pfree(tuple);
}

static void
heapam_tuple_insert_speculative(Relation relation, TupleTableSlot *slot,
								CommandId cid, int options,
								BulkInsertState bistate, uint32 specToken)
{
	bool		shouldFree = true;
	HeapTuple	tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);

	/* 用表的 oid 更新元组 */
	slot->tts_tableOid = RelationGetRelid(relation);
	tuple->t_tableOid = slot->tts_tableOid;

	HeapTupleHeaderSetSpeculativeToken(tuple->t_data, specToken);
	options |= HEAP_INSERT_SPECULATIVE;

	/* 执行插入，并复制结果 ItemPointer */
	heap_insert(relation, tuple, cid, options, bistate);
	ItemPointerCopy(&tuple->t_self, &slot->tts_tid);

	if (shouldFree)
		pfree(tuple);
}

static void
heapam_tuple_complete_speculative(Relation relation, TupleTableSlot *slot,
								  uint32 specToken, bool succeeded)
{
	bool		shouldFree = true;
	HeapTuple	tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);

	/* 相应地调整元组的状态 */
	if (succeeded)
		heap_finish_speculative(relation, &slot->tts_tid);
	else
		heap_abort_speculative(relation, &slot->tts_tid);

	if (shouldFree)
		pfree(tuple);
}

static TM_Result
heapam_tuple_delete(Relation relation, ItemPointer tid, CommandId cid,
					Snapshot snapshot, Snapshot crosscheck, bool wait,
					TM_FailureData *tmfd, bool changingPart)
{
	/*
	 * 目前索引元组的删除在 vacuum 时处理；如果存储层自身会清理死亡元组，
	 * 那么此刻也应当调用索引元组的删除。
	 */
	return heap_delete(relation, tid, cid, crosscheck, wait, tmfd, changingPart);
}


static TM_Result
heapam_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
					CommandId cid, Snapshot snapshot, Snapshot crosscheck,
					bool wait, TM_FailureData *tmfd,
					LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{
	bool		shouldFree = true;
	HeapTuple	tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);
	TM_Result	result;

	/* 用表的 oid 更新元组 */
	slot->tts_tableOid = RelationGetRelid(relation);
	tuple->t_tableOid = slot->tts_tableOid;

	result = heap_update(relation, otid, tuple, cid, crosscheck, wait,
						 tmfd, lockmode, update_indexes);
	ItemPointerCopy(&tuple->t_self, &slot->tts_tid);

	/*
	 * 决定是否需要为元组建立新的索引项
	 *
	 * 注意：heap_update 会把新元组的 tid（位置）通过 t_self 字段返回。
	 *
	 * 如果更新不是 HOT 更新，我们必须更新所有索引；如果是 HOT 更新，
	 * 则可能更新了汇总列，因此我们只能更新汇总索引，或者完全不更新。
	 */
	if (result != TM_Ok)
	{
		Assert(*update_indexes == TU_None);
		*update_indexes = TU_None;
	}
	else if (!HeapTupleIsHeapOnly(tuple))
		Assert(*update_indexes == TU_All);
	else
		Assert((*update_indexes == TU_Summarizing) ||
			   (*update_indexes == TU_None));

	if (shouldFree)
		pfree(tuple);

	return result;
}

static TM_Result
heapam_tuple_lock(Relation relation, ItemPointer tid, Snapshot snapshot,
				  TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
				  LockWaitPolicy wait_policy, uint8 flags,
				  TM_FailureData *tmfd)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	TM_Result	result;
	Buffer		buffer;
	HeapTuple	tuple = &bslot->base.tupdata;
	bool		follow_updates;

	follow_updates = (flags & TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS) != 0;
	tmfd->traversed = false;

	Assert(TTS_IS_BUFFERTUPLE(slot));

tuple_lock_retry:
	tuple->t_self = *tid;
	result = heap_lock_tuple(relation, tuple, cid, mode, wait_policy,
							 follow_updates, &buffer, tmfd);

	if (result == TM_Updated &&
		(flags & TUPLE_LOCK_FLAG_FIND_LAST_VERSION))
	{
		/* 在重新检查时不应遇到推测性元组 */
		Assert(!HeapTupleHeaderIsSpeculative(tuple->t_data));

		ReleaseBuffer(buffer);

		if (!ItemPointerEquals(&tmfd->ctid, &tuple->t_self))
		{
			SnapshotData SnapshotDirty;
			TransactionId priorXmax;

		/* 元组被更新了，因此查看更新后的版本 */
			*tid = tmfd->ctid;
		/* 更新后的行其 xmin 应当与本 xmax 相匹配 */
			priorXmax = tmfd->xmax;

		/* 标记链中靠后的某个元组正在被锁定 */
			tmfd->traversed = true;

		/*
		 * 获取目标元组
		 *
		 * 在此循环以处理已更新或繁忙的元组
		 */
			InitDirtySnapshot(SnapshotDirty);
			for (;;)
			{
				if (ItemPointerIndicatesMovedPartitions(tid))
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("tuple to be locked was already moved to another partition due to concurrent update")));

				tuple->t_self = *tid;
				if (heap_fetch(relation, &SnapshotDirty, tuple, &buffer, true))
				{
					/*
					 * 如果 xmin 不是我们期望的值，那么该 slot 必定已被回收
					 * 并重新用于一个不相关的元组。这意味着该行的最新版本
					 * 已被删除，因此我们无需处理。（在持有缓冲区 content 锁
					 * 的情况下检查 xmin 应当是安全的。我们假设读取一个
					 * TransactionId 是原子的，而 Xmin 在已有元组中不会改变，
					 * 除非变为 invalid 或 frozen，而这两者都无法与 priorXmax
					 * 匹配。）
					 */
					if (!TransactionIdEquals(HeapTupleHeaderGetXmin(tuple->t_data),
											 priorXmax))
					{
						ReleaseBuffer(buffer);
						return TM_Deleted;
					}

		/* 否则 xmin 不应是脏的…… */
					if (TransactionIdIsValid(SnapshotDirty.xmin))
						ereport(ERROR,
								(errcode(ERRCODE_DATA_CORRUPTED),
								 errmsg_internal("t_xmin %u is uncommitted in tuple (%u,%u) to be updated in table \"%s\"",
												 SnapshotDirty.xmin,
												 ItemPointerGetBlockNumber(&tuple->t_self),
												 ItemPointerGetOffsetNumber(&tuple->t_self),
												 RelationGetRelationName(relation))));

					/*
					 * 如果元组正在被其他事务更新，那么我们必须等待其
					 * 提交/中止，否则只能失败退出。
					 */
					if (TransactionIdIsValid(SnapshotDirty.xmax))
					{
						ReleaseBuffer(buffer);
						switch (wait_policy)
						{
							case LockWaitBlock:
								XactLockTableWait(SnapshotDirty.xmax,
												  relation, &tuple->t_self,
												  XLTW_FetchUpdated);
								break;
							case LockWaitSkip:
								if (!ConditionalXactLockTableWait(SnapshotDirty.xmax, false))
								/* 跳过而非等待 */
									return TM_WouldBlock;
								break;
							case LockWaitError:
								if (!ConditionalXactLockTableWait(SnapshotDirty.xmax, log_lock_failures))
									ereport(ERROR,
											(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
											 errmsg("could not obtain lock on row in relation \"%s\"",
													RelationGetRelationName(relation))));
								break;
						}
      continue;	/* 回到循环开头以重复 heap_fetch */
					}

					/*
					 * 如果元组是由我们自己的事务插入的，我们必须将 cmin 与
					 * cid 进行比较：cmin >= 当前 CID 意味着我们的命令无法
					 * 看到该元组，因此我们应当忽略它。否则 heap_lock_tuple()
					 * 会抛出错误，任何后续尝试更新或删除该元组的操作也
					 * 会如此。（我们不需要检查 cmax，因为
					 * HeapTupleSatisfiesDirty 会认为被我们自己事务删除的
					 * 元组已死亡，与 cmax 无关。）我们刚刚已经确认
					 * priorXmax == xmin，因此可以直接测试该变量，而无需
					 * 再次调用 HeapTupleHeaderGetXmin。
					 */
					if (TransactionIdIsCurrentTransactionId(priorXmax) &&
						HeapTupleHeaderGetCmin(tuple->t_data) >= cid)
					{
						tmfd->xmax = priorXmax;

						/*
						 * Cmin 是那个有问题的值，因此将其保存起来。参见上文。
						 */
						tmfd->cmax = HeapTupleHeaderGetCmin(tuple->t_data);
						ReleaseBuffer(buffer);
						return TM_SelfModified;
					}

					/*
					 * 这是一个存活的元组，因此再次尝试锁定它。
					 */
					ReleaseBuffer(buffer);
					goto tuple_lock_retry;
				}

					/*
					 * 如果被引用的 slot 实际上是空的，那么该行的最新版本必定
					 * 已被删除，因此我们无需处理。
					 */
				if (tuple->t_data == NULL)
				{
					Assert(!BufferIsValid(buffer));
					return TM_Deleted;
				}

					/*
					 * 同上，如果 xmin 不是我们期望的值，则不做任何处理。
					 */
				if (!TransactionIdEquals(HeapTupleHeaderGetXmin(tuple->t_data),
										 priorXmax))
				{
					ReleaseBuffer(buffer);
					return TM_Deleted;
				}

				/*
				 * 如果执行到这里，说明元组已被找到但未能通过
				 * SnapshotDirty 的检查。假定 xmin 要么是已提交的事务，
				 * 要么是我们自己的事务（如果我们要修改该元组，这应当
				 * 必然成立），那么这必定意味着该行已被某个已提交事务
				 * 或我们自己的事务更新或删除。如果它被删除了，我们可以
				 * 忽略它；如果它被更新了，则沿链找到下一个版本并重复
				 * 整个过程。
				 *
				 * 同上，在持有缓冲区 content 锁的情况下检查 xmax 和 t_ctid
				 * 应当是安全的，因为它们不会发生变化。不过我们最好还是
				 * 持有一个缓冲区 pin。
				 */
				if (ItemPointerEquals(&tuple->t_self, &tuple->t_data->t_ctid))
				{
					/* 已被删除，因此忽略它 */
					ReleaseBuffer(buffer);
					return TM_Deleted;
				}

				/* 已被更新，因此查看更新后的行 */
				*tid = tuple->t_data->t_ctid;
				/* 更新后的行其 xmin 应当与本 xmax 相匹配 */
				priorXmax = HeapTupleHeaderGetUpdateXid(tuple->t_data);
				ReleaseBuffer(buffer);
				/* 回到循环开头，获取链中的下一个元组 */
			}
		}
		else
		{
		/* 元组已被删除，因此放弃 */
			return TM_Deleted;
		}
	}

	slot->tts_tableOid = RelationGetRelid(relation);
	tuple->t_tableOid = slot->tts_tableOid;

	/* 存入 slot 中，并转移已有的 pin */
	ExecStorePinnedBufferHeapTuple(tuple, slot, buffer);

	return result;
}


/* ------------------------------------------------------------------------
 * heap AM 中与 DDL 相关的回调。
 * ------------------------------------------------------------------------
 */

static void
heapam_relation_set_new_filelocator(Relation rel,
									const RelFileLocator *newrlocator,
									char persistence,
									TransactionId *freezeXid,
									MultiXactId *minmulti)
{
	SMgrRelation srel;

	/*
	 * 初始化为可能向表中写入元组的最小 XID。我们知道没有比 RecentXmin
	 * 更老的事务仍在运行，因此用它即可。
	 */
	*freezeXid = RecentXmin;

	/*
	 * 类似地，将最小 Multixact 初始化为可能存储在表元组中的第一个值。
	 * 正在运行的事务可能会复用其本地缓存中的值，因此我们要谨慎地
	 * 考虑所有当前正在运行的多事务。
	 *
	 * XXX 这一点还可以进一步细化，但值得这么麻烦吗？
	 */
	*minmulti = GetOldestMultiXactId();

	srel = RelationCreateStorage(*newrlocator, persistence, true);

	/*
	 * 如果需要，为未日志记录（unlogged）的表建立一个 init fork，
	 * 以便重启时能够正确地重新初始化。
	 */
	if (persistence == RELPERSISTENCE_UNLOGGED)
	{
		Assert(rel->rd_rel->relkind == RELKIND_RELATION ||
			   rel->rd_rel->relkind == RELKIND_TOASTVALUE);
		smgrcreate(srel, INIT_FORKNUM, false);
		log_smgrcreate(newrlocator, INIT_FORKNUM);
	}

	smgrclose(srel);
}

static void
heapam_relation_nontransactional_truncate(Relation rel)
{
	RelationTruncate(rel, 0);
}

static void
heapam_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
	SMgrRelation dstrel;

	/*
	 * 由于我们直接复制文件而不查看共享缓冲区，最好先将被源关系
	 * 占用在共享缓冲区中的任何页面刷出。我们假定在持有该关系的
	 * 排他锁期间不会再有新的修改。
	 */
	FlushRelationBuffers(rel);

	/*
	 * 创建并复制该关系的所有 fork，并安排对旧物理文件执行 unlink。
	 *
	 * 注意：任何 relfilenumber 值的冲突都会在
	 * RelationCreateStorage() 中被捕获。
	 */
	dstrel = RelationCreateStorage(*newrlocator, rel->rd_rel->relpersistence, true);

	/* 复制主 fork */
	RelationCopyStorage(RelationGetSmgr(rel), dstrel, MAIN_FORKNUM,
						rel->rd_rel->relpersistence);

	/* 复制那些已存在的额外 fork */
	for (ForkNumber forkNum = MAIN_FORKNUM + 1;
		 forkNum <= MAX_FORKNUM; forkNum++)
	{
		if (smgrexists(RelationGetSmgr(rel), forkNum))
		{
			smgrcreate(dstrel, forkNum, false);

			/*
			 * 如果关系是持久化的，或者是未日志记录关系的 init fork，
			 * 则需要对创建操作写 WAL 日志。
			 */
			if (RelationIsPermanent(rel) ||
				(rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED &&
				 forkNum == INIT_FORKNUM))
				log_smgrcreate(newrlocator, forkNum);
			RelationCopyStorage(RelationGetSmgr(rel), dstrel, forkNum,
								rel->rd_rel->relpersistence);
		}
	}


	/* 删除旧关系，并关闭新关系 */
	RelationDropStorage(rel);
	smgrclose(dstrel);
}

static void
heapam_relation_copy_for_cluster(Relation OldHeap, Relation NewHeap,
								 Relation OldIndex, bool use_sort,
								 TransactionId OldestXmin,
								 TransactionId *xid_cutoff,
								 MultiXactId *multi_cutoff,
								 double *num_tuples,
								 double *tups_vacuumed,
								 double *tups_recently_dead)
{
	RewriteState rwstate;
	IndexScanDesc indexScan;
	TableScanDesc tableScan;
	HeapScanDesc heapScan;
	bool		is_system_catalog;
	Tuplesortstate *tuplesort;
	TupleDesc	oldTupDesc = RelationGetDescr(OldHeap);
	TupleDesc	newTupDesc = RelationGetDescr(NewHeap);
	TupleTableSlot *slot;
	int			natts;
	Datum	   *values;
	bool	   *isnull;
	BufferHeapTupleTableSlot *hslot;
	BlockNumber prev_cblock = InvalidBlockNumber;

	/* 记住它是否是系统目录 */
	is_system_catalog = IsSystemRelation(OldHeap);

	/*
	 * 有效的 smgr_targblock 意味着已经有东西写入了该关系。
	 * 这可能是无害的，但本函数并未为此做好准备。
	 */
	Assert(RelationGetTargetBlock(NewHeap) == InvalidBlockNumber);

	/* 预分配 values/isnull 数组 */
	natts = newTupDesc->natts;
	values = (Datum *) palloc(natts * sizeof(Datum));
	isnull = (bool *) palloc(natts * sizeof(bool));

	/* 初始化重写操作 */
	rwstate = begin_heap_rewrite(OldHeap, NewHeap, OldestXmin, *xid_cutoff,
								 *multi_cutoff);


	/* 若需要则设置排序 */
	if (use_sort)
		tuplesort = tuplesort_begin_cluster(oldTupDesc, OldIndex,
											maintenance_work_mem,
											NULL, TUPLESORT_NONE);
	else
		tuplesort = NULL;

	/*
	 * 准备扫描 OldHeap。为了确保能看到那些仍需要复制的最近死亡元组，
	 * 我们使用 SnapshotAny 进行扫描，并用 HeapTupleSatisfiesVacuum
	 * 来做可见性判断。
	 */
	if (OldIndex != NULL && !use_sort)
	{
		const int	ci_index[] = {
			PROGRESS_CLUSTER_PHASE,
			PROGRESS_CLUSTER_INDEX_RELID
		};
		int64		ci_val[2];

		/* 将阶段和 OIDOldIndex 设置到对应的列中 */
		ci_val[0] = PROGRESS_CLUSTER_PHASE_INDEX_SCAN_HEAP;
		ci_val[1] = RelationGetRelid(OldIndex);
		pgstat_progress_update_multi_param(2, ci_index, ci_val);

		tableScan = NULL;
		heapScan = NULL;
		indexScan = index_beginscan(OldHeap, OldIndex, SnapshotAny, NULL, 0, 0);
		index_rescan(indexScan, NULL, 0, NULL, 0);
	}
	else
	{
		/* 在扫描-排序模式以及 VACUUM FULL 中，设置扫描阶段 */
		pgstat_progress_update_param(PROGRESS_CLUSTER_PHASE,
									 PROGRESS_CLUSTER_PHASE_SEQ_SCAN_HEAP);

		tableScan = table_beginscan(OldHeap, SnapshotAny, 0, (ScanKey) NULL);
		heapScan = (HeapScanDesc) tableScan;
		indexScan = NULL;

		/* 设置堆的总块数 */
		pgstat_progress_update_param(PROGRESS_CLUSTER_TOTAL_HEAP_BLKS,
									 heapScan->rs_nblocks);
	}

	slot = table_slot_create(OldHeap, NULL);
	hslot = (BufferHeapTupleTableSlot *) slot;

	/*
	 * 遍历 OldHeap，可以按 OldIndex 的顺序，也可以顺序遍历；将每个元组
	 * 复制进 NewHeap，或者临时放入 tuplesort 模块。注意我们无需对死亡
	 * 元组进行排序（它们反正也不会进入新表）。
	 */
	for (;;)
	{
		HeapTuple	tuple;
		Buffer		buf;
		bool		isdead;

		CHECK_FOR_INTERRUPTS();

		if (indexScan != NULL)
		{
			if (!index_getnext_slot(indexScan, ForwardScanDirection, slot))
				break;

			/* 由于没有使用任何扫描键，应当永远不需要重新检查 */
			if (indexScan->xs_recheck)
				elog(ERROR, "CLUSTER does not support lossy index conditions");
		}
		else
		{
			if (!table_scan_getnextslot(tableScan, ForwardScanDirection, slot))
			{
				/*
				 * 如果扫描的最后若干页是空的，我们就会在
				 * heap_blks_scanned != heap_blks_total 的情况下进入下一阶段。
				 * 为了让表扫描阶段结束后 heap_blks_scanned 等于
				 * heap_blks_total，这里在表扫描完成时手动将该参数
				 * 更新为正确的值。
				 */
				pgstat_progress_update_param(PROGRESS_CLUSTER_HEAP_BLKS_SCANNED,
											 heapScan->rs_nblocks);
				break;
			}

			/*
			 * 在扫描-排序模式以及 VACUUM FULL 中，设置已扫描的堆块数
			 *
			 * 注意 heapScan 可能从某个偏移处开始并环绕，即
			 * rs_startblock 可能大于 0，而 rs_cblock 最终可能以小于
			 * rs_startblock 的块号结束。为了避免向用户展示这种环绕，
			 * 我们用 rs_startblock 对 rs_cblock 做偏移（对 rs_nblocks
			 * 取模）。
			 */
			if (prev_cblock != heapScan->rs_cblock)
			{
				pgstat_progress_update_param(PROGRESS_CLUSTER_HEAP_BLKS_SCANNED,
											 (heapScan->rs_cblock +
											  heapScan->rs_nblocks -
											  heapScan->rs_startblock
											  ) % heapScan->rs_nblocks + 1);
				prev_cblock = heapScan->rs_cblock;
			}
		}

		tuple = ExecFetchSlotHeapTuple(slot, false, NULL);
		buf = hslot->buffer;

		LockBuffer(buf, BUFFER_LOCK_SHARE);

		switch (HeapTupleSatisfiesVacuum(tuple, OldestXmin, buf))
		{
			case HEAPTUPLE_DEAD:
				/* 必定是死亡元组 */
				isdead = true;
				break;
			case HEAPTUPLE_RECENTLY_DEAD:
				*tups_recently_dead += 1;
				/* 继续执行下一个 case */
			case HEAPTUPLE_LIVE:
				/* 存活或最近死亡，必须复制它 */
				isdead = false;
				break;
			case HEAPTUPLE_INSERT_IN_PROGRESS:

				/*
				 * 由于我们持有该关系的排他锁，通常只有在它是在我们自己
				 * 事务中较早插入的情况下才会看到这种状态。不过在系统目录
				 * 中也可能发生，因为我们往往会在提交前就释放写锁。如果以上
				 * 两种情况都不满足，则给出警告；但无论如何我们最好都复制它。
				 */
				if (!is_system_catalog &&
					!TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmin(tuple->t_data)))
					elog(WARNING, "concurrent insert in progress within table \"%s\"",
						 RelationGetRelationName(OldHeap));
				/* 当作存活处理 */
				isdead = false;
				break;
			case HEAPTUPLE_DELETE_IN_PROGRESS:

				/*
				 * 与 INSERT_IN_PROGRESS 情况类似。
				 */
				if (!is_system_catalog &&
					!TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetUpdateXid(tuple->t_data)))
					elog(WARNING, "concurrent delete in progress within table \"%s\"",
						 RelationGetRelationName(OldHeap));
				/* 当作最近死亡处理 */
				*tups_recently_dead += 1;
				isdead = false;
				break;
			default:
				elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
    isdead = false; /* 避免编译器告警 */
				break;
		}

		LockBuffer(buf, BUFFER_LOCK_UNLOCK);

		if (isdead)
		{
			*tups_vacuumed += 1;
			/* 堆重写模块仍然需要看到它…… */
			if (rewrite_heap_dead_tuple(rwstate, tuple))
			{
				/* A previous recently-dead tuple is now known dead */
				*tups_vacuumed += 1;
				*tups_recently_dead -= 1;
			}
			continue;
		}

		*num_tuples += 1;
		if (tuplesort != NULL)
		{
			tuplesort_putheaptuple(tuplesort, tuple);

			/*
			 * In scan-and-sort mode, report increase in number of tuples
			 * scanned
			 */
			pgstat_progress_update_param(PROGRESS_CLUSTER_HEAP_TUPLES_SCANNED,
										 *num_tuples);
		}
		else
		{
			const int	ct_index[] = {
				PROGRESS_CLUSTER_HEAP_TUPLES_SCANNED,
				PROGRESS_CLUSTER_HEAP_TUPLES_WRITTEN
			};
			int64		ct_val[2];

			reform_and_rewrite_tuple(tuple, OldHeap, NewHeap,
									 values, isnull, rwstate);

			/*
			 * In indexscan mode and also VACUUM FULL, report increase in
			 * number of tuples scanned and written
			 */
			ct_val[0] = *num_tuples;
			ct_val[1] = *num_tuples;
			pgstat_progress_update_multi_param(2, ct_index, ct_val);
		}
	}

	if (indexScan != NULL)
		index_endscan(indexScan);
	if (tableScan != NULL)
		table_endscan(tableScan);
	if (slot)
		ExecDropSingleTupleTableSlot(slot);

	/*
	 * In scan-and-sort mode, complete the sort, then read out all live tuples
	 * from the tuplestore and write them to the new relation.
	 */
	if (tuplesort != NULL)
	{
		double		n_tuples = 0;

		/* 上报：我们当前正在对元组排序 */
		pgstat_progress_update_param(PROGRESS_CLUSTER_PHASE,
									 PROGRESS_CLUSTER_PHASE_SORT_TUPLES);

		tuplesort_performsort(tuplesort);

		/* 上报：我们当前正在写入新的堆 */
		pgstat_progress_update_param(PROGRESS_CLUSTER_PHASE,
									 PROGRESS_CLUSTER_PHASE_WRITE_NEW_HEAP);

		for (;;)
		{
			HeapTuple	tuple;

			CHECK_FOR_INTERRUPTS();

			tuple = tuplesort_getheaptuple(tuplesort, true);
			if (tuple == NULL)
				break;

			n_tuples += 1;
			reform_and_rewrite_tuple(tuple,
									 OldHeap, NewHeap,
									 values, isnull,
									 rwstate);
			/* 上报 n_tuples */
			pgstat_progress_update_param(PROGRESS_CLUSTER_HEAP_TUPLES_WRITTEN,
										 n_tuples);
		}

		tuplesort_end(tuplesort);
	}

	/* 写出所有剩余元组，并在需要时执行 fsync */
	end_heap_rewrite(rwstate);

	/* 清理 */
	pfree(values);
	pfree(isnull);
}

/*
 * 准备分析读取流中的下一个块。若流已耗尽则返回 false，否则返回 true。
 * 扫描必须以 SO_TYPE_ANALYZE 选项启动。
 *
 * 本例程会持有堆页面上的缓冲区 pin 与锁。它们会一直保持，直到
 * heapam_scan_analyze_next_tuple() 返回 false，即直到堆页面上的所有
 * 项都被分析完毕。
 */
static bool
heapam_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream)
{
	HeapScanDesc hscan = (HeapScanDesc) scan;

	/*
	 * We must maintain a pin on the target page's buffer to ensure that
	 * concurrent activity - e.g. HOT pruning - doesn't delete tuples out from
	 * under us.  It comes from the stream already pinned.   We also choose to
	 * hold sharelock on the buffer throughout --- we could release and
	 * re-acquire sharelock for each tuple, but since we aren't doing much
	 * work per tuple, the extra lock traffic is probably better avoided.
	 */
	hscan->rs_cbuf = read_stream_next_buffer(stream, NULL);
	if (!BufferIsValid(hscan->rs_cbuf))
		return false;

	LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_SHARE);

	hscan->rs_cblock = BufferGetBlockNumber(hscan->rs_cbuf);
	hscan->rs_cindex = FirstOffsetNumber;
	return true;
}

static bool
heapam_scan_analyze_next_tuple(TableScanDesc scan, TransactionId OldestXmin,
							   double *liverows, double *deadrows,
							   TupleTableSlot *slot)
{
	HeapScanDesc hscan = (HeapScanDesc) scan;
	Page		targpage;
	OffsetNumber maxoffset;
	BufferHeapTupleTableSlot *hslot;

	Assert(TTS_IS_BUFFERTUPLE(slot));

	hslot = (BufferHeapTupleTableSlot *) slot;
	targpage = BufferGetPage(hscan->rs_cbuf);
	maxoffset = PageGetMaxOffsetNumber(targpage);

	/* 在所选页面的所有元组上执行内层循环 */
	for (; hscan->rs_cindex <= maxoffset; hscan->rs_cindex++)
	{
		ItemId		itemid;
		HeapTuple	targtuple = &hslot->base.tupdata;
		bool		sample_it = false;

		itemid = PageGetItemId(targpage, hscan->rs_cindex);

		/*
		 * 我们忽略未使用和重定向的行指针。DEAD 行指针应被计为死亡，
		 * 因为我们需要 vacuum 来清除它们。注意此规则与
		 * heap_page_prune_and_freeze() 的计数方式一致。
		 */
		if (!ItemIdIsNormal(itemid))
		{
			if (ItemIdIsDead(itemid))
				*deadrows += 1;
			continue;
		}

		ItemPointerSet(&targtuple->t_self, hscan->rs_cblock, hscan->rs_cindex);

		targtuple->t_tableOid = RelationGetRelid(scan->rs_rd);
		targtuple->t_data = (HeapTupleHeader) PageGetItem(targpage, itemid);
		targtuple->t_len = ItemIdGetLength(itemid);

		switch (HeapTupleSatisfiesVacuum(targtuple, OldestXmin,
										 hscan->rs_cbuf))
		{
			case HEAPTUPLE_LIVE:
				sample_it = true;
				*liverows += 1;
				break;

			case HEAPTUPLE_DEAD:
			case HEAPTUPLE_RECENTLY_DEAD:
				/* 统计死亡和最近死亡的行 */
				*deadrows += 1;
				break;

			case HEAPTUPLE_INSERT_IN_PROGRESS:

			/*
			 * 插入进行中的行不被计数。我们假设当插入事务提交或中止时，
			 * 它会发送一条统计消息来累加正确的计数。这只有在
			 * 该事务在我们完成表分析之后结束时才正确；如果顺序相反，
			 * 它的统计更新会被我们的覆盖。不过，只有当该事务运行得
			 * 足够久以插入大量元组时误差才会很大，因此假设它会在我们
			 * 之后结束是更稳妥的选择。
			 *
			 * 一个特殊情况是插入事务可能就是我们自己。这种情况下，
			 * 我们应该对该行计数并采样，以便支持那些在一个事务中
			 * 既载入表又分析表的用户。（pgstat_report_analyze 必须
			 * 调整我们上报给累积统计系统的数字，才能让结果正确。）
			 */
				if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmin(targtuple->t_data)))
				{
					sample_it = true;
					*liverows += 1;
				}
				break;

			case HEAPTUPLE_DELETE_IN_PROGRESS:

				/*
				 * We count and sample delete-in-progress rows the same as
				 * live ones, so that the stats counters come out right if the
				 * deleting transaction commits after us, per the same
				 * reasoning given above.
				 *
				 * If the delete was done by our own transaction, however, we
				 * must count the row as dead to make pgstat_report_analyze's
				 * stats adjustments come out right.  (Note: this works out
				 * properly when the row was both inserted and deleted in our
				 * xact.)
				 *
				 * The net effect of these choices is that we act as though an
				 * IN_PROGRESS transaction hasn't happened yet, except if it
				 * is our own transaction, which we assume has happened.
				 *
				 * This approach ensures that we behave sanely if we see both
				 * the pre-image and post-image rows for a row being updated
				 * by a concurrent transaction: we will sample the pre-image
				 * but not the post-image.  We also get sane results if the
				 * concurrent transaction never commits.
				 */
				if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetUpdateXid(targtuple->t_data)))
					*deadrows += 1;
				else
				{
					sample_it = true;
					*liverows += 1;
				}
				break;

			default:
				elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
				break;
		}

		if (sample_it)
		{
			ExecStoreBufferHeapTuple(targtuple, slot, hscan->rs_cbuf);
			hscan->rs_cindex++;

		/* 注意此处我们保持缓冲区加锁状态！ */
			return true;
		}
	}

	/* 现在释放页面上的锁和 pin */
	UnlockReleaseBuffer(hscan->rs_cbuf);
	hscan->rs_cbuf = InvalidBuffer;

	/* 同时防止旧 slot 内容在页面上持有 pin */
	ExecClearTuple(slot);

	return false;
}

static double
heapam_index_build_range_scan(Relation heapRelation,
							  Relation indexRelation,
							  IndexInfo *indexInfo,
							  bool allow_sync,
							  bool anyvisible,
							  bool progress,
							  BlockNumber start_blockno,
							  BlockNumber numblocks,
							  IndexBuildCallback callback,
							  void *callback_state,
							  TableScanDesc scan)
{
	HeapScanDesc hscan;
	bool		is_system_catalog;
	bool		checking_uniqueness;
	HeapTuple	heapTuple;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	double		reltuples;
	ExprState  *predicate;
	TupleTableSlot *slot;
	EState	   *estate;
	ExprContext *econtext;
	Snapshot	snapshot;
	bool		need_unregister_snapshot = false;
	TransactionId OldestXmin;
	BlockNumber previous_blkno = InvalidBlockNumber;
	BlockNumber root_blkno = InvalidBlockNumber;
	OffsetNumber root_offsets[MaxHeapTuplesPerPage];

	/*
	 * 健全性检查
	 */
	Assert(OidIsValid(indexRelation->rd_rel->relam));

	/* 记录它是否为系统目录表 */
	is_system_catalog = IsSystemRelation(heapRelation);

	/* 检查我们是否正在校验唯一性 / 排他属性 */
	checking_uniqueness = (indexInfo->ii_Unique ||
						   indexInfo->ii_ExclusionOps != NULL);

	/*
	 * "Any visible" mode is not compatible with uniqueness checks; make sure
	 * only one of those is requested.
	 */
	Assert(!(anyvisible && checking_uniqueness));

	/*
	 * Need an EState for evaluation of index expressions and partial-index
	 * predicates.  Also a slot to hold the current tuple.
	 */
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = table_slot_create(heapRelation, NULL);

	/* 将 econtext 的扫描元组设置为正在测试的元组 */
	econtext->ecxt_scantuple = slot;

	/* 若有谓词，设置其执行状态。 */
	predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);

	/*
	 * 准备对基关系进行扫描。在普通的索引构建中，我们使用
	 * SnapshotAny，因为我们必须检索所有元组并自行进行时间
	 * 资格检查（因为我们必须索引 RECENTLY_DEAD 元组）。而在
	 * 并发构建或引导（bootstrap）期间，我们会获取一个普通的
	 * MVCC 快照，并索引其中所有存活的元组。
	 */
	OldestXmin = InvalidTransactionId;

	/* 此处可以忽略 lazy VACUUM */
	if (!IsBootstrapProcessingMode() && !indexInfo->ii_Concurrent)
		OldestXmin = GetOldestNonRemovableTransactionId(heapRelation);

	if (!scan)
	{
		/*
		 * 串行索引构建。
		 *
		 * 这种情况下必须由我们自己开始堆扫描。我们也可能需要
		 * 注册一个生命周期由我们直接控制的快照。
		 */
		if (!TransactionIdIsValid(OldestXmin))
		{
			snapshot = RegisterSnapshot(GetTransactionSnapshot());
			need_unregister_snapshot = true;
		}
		else
			snapshot = SnapshotAny;

  scan = table_beginscan_strat(heapRelation,	/* relation 关系 */
          snapshot,	/* snapshot 快照 */
          0, /* number of keys 键的数量 */
          NULL,	/* scan key 扫描键 */
          true,	/* buffer access strategy OK 允许缓冲区访问策略 */
          allow_sync);	/* syncscan OK? 允许 syncscan？ */
	}
	else
	{
		/*
		 * 并行索引构建。
		 *
		 * 并行情况下从不注册/注销自己的快照。快照取自并行堆扫描，
		 * 是 SnapshotAny 或 MVCC 快照，依据与串行情形的相同标准。
		 */
		Assert(!IsBootstrapProcessingMode());
		Assert(allow_sync);
		snapshot = scan->rs_snapshot;
	}

	hscan = (HeapScanDesc) scan;

	/*
	 * 如果使用 SnapshotAny，则必须已调用过
	 * GetOldestNonRemovableTransactionId()。对于 MVCC 快照则不应调用。
	 * （这一点在并行构建时尤其值得检查，因为支持并行构建的 ambuild
	 * 例程必须自行处理好这些细节。）
	 */
	Assert(snapshot == SnapshotAny || IsMVCCSnapshot(snapshot));
	Assert(snapshot == SnapshotAny ? TransactionIdIsValid(OldestXmin) :
		   !TransactionIdIsValid(OldestXmin));
	Assert(snapshot == SnapshotAny || !anyvisible);

	/* 发布待扫描的块数量 */
	if (progress)
	{
		BlockNumber nblocks;

		if (hscan->rs_base.rs_parallel != NULL)
		{
			ParallelBlockTableScanDesc pbscan;

			pbscan = (ParallelBlockTableScanDesc) hscan->rs_base.rs_parallel;
			nblocks = pbscan->phs_nblocks;
		}
		else
			nblocks = hscan->rs_nblocks;

		pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_TOTAL,
									 nblocks);
	}

	/* 设置扫描的起止端点 */
	if (!allow_sync)
		heap_setscanlimits(scan, start_blockno, numblocks);
	else
	{
		/* syncscan 只能在整个关系上请求 */
		Assert(start_blockno == 0);
		Assert(numblocks == InvalidBlockNumber);
	}

	reltuples = 0;

	/*
	 * 扫描基关系中的所有元组。
	 */
	while ((heapTuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		bool		tupleIsAlive;

		CHECK_FOR_INTERRUPTS();

		/* 若被要求，则上报扫描进度。 */
		if (progress)
		{
			BlockNumber blocks_done = heapam_scan_get_blocks_done(hscan);

			if (blocks_done != previous_blkno)
			{
				pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE,
											 blocks_done);
				previous_blkno = blocks_done;
			}
		}

		/*
		 * 当处理已更新元组的 HOT 链时，我们希望索引存活元组（若有）的
		 * 值，但在该链的 root 元组 TID 之下建立索引。这种做法是必要的，
		 * 以保留堆中的 HOT 链结构。因此我们需要能为 HOT 链中的每个元组
		 * 找到其 root 项偏移量。当首次到达关系的新页面时，调用
		 * heap_get_root_tuples() 来构建该页面上 root 项偏移量的映射。
		 *
		 * 跨缓冲区加锁/解锁使用此信息看似不安全。然而，我们对表持有
		 * ShareLock，因此不会发生普通的插入/更新/删除；并且我们在访问
		 * 页面期间持续持有该缓冲区的 pin，因此也不会发生剪枝操作。
		 *
		 * 在仅对表持有 ShareUpdateExclusiveLock 的情况下，可能会出现一些
		 * 我们在初次读取页面时尚不知道的 HOT 元组。为处理这种情况，当某
		 * HOT 元组指向一个我们未知其信息的 root 项时，我们会重新获取
		 * root 偏移量列表。
		 *
		 * 此外，尽管我们在扫描页面期间对元组存活性的判断可能改变（由于
		 * 并发事务的提交/中止），但链 root 的位置不会变，因此该信息无需
		 * 在等待另一事务后重建。
		 *
		 * 注意这里隐含的假设：每个 HOT 链中最多只有一个存活元组——否则
		 * 我们可能创建多个指向同一 root 元组的索引项。
		 */
		if (hscan->rs_cblock != root_blkno)
		{
			Page		page = BufferGetPage(hscan->rs_cbuf);

			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_SHARE);
			heap_get_root_tuples(page, root_offsets);
			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			root_blkno = hscan->rs_cblock;
		}

		if (snapshot == SnapshotAny)
		{
			/* 自行进行时间资格检查 */
			bool		indexIt;
			TransactionId xwait;

	recheck:

			/*
			 * 我们本可以不必在此处锁定缓冲区，因为调用方应持有该关系的
			 * ShareLock，但为了稳妥起见还是加上。（即便存在 HOT 剪枝，
			 * 此说明仍然成立：我们在缓冲区上的 pin 会阻止剪枝发生。）
			 */
			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_SHARE);

			/*
			 * 本代码块中将元组计为存活的判定标准，必须与 analyze.c 中
			 * heapam_scan_analyze_next_tuple() 的做法一致，否则 CREATE INDEX
			 * 与 ANALYZE 可能产生差异极大的 reltuples 值，例如当存在大量
			 * 最近死亡元组时。
			 */
			switch (HeapTupleSatisfiesVacuum(heapTuple, OldestXmin,
											 hscan->rs_cbuf))
			{
				case HEAPTUPLE_DEAD:
					/* 确定已死亡，可以忽略它 */
					indexIt = false;
					tupleIsAlive = false;
					break;
				case HEAPTUPLE_LIVE:
					/* 普通情形，对其建立索引并执行唯一性检查 */
					indexIt = true;
					tupleIsAlive = true;
					/* 同时将其计为存活 */
					reltuples += 1;
					break;
				case HEAPTUPLE_RECENTLY_DEAD:

					/*
					 * 如果元组是最近删除的，我们无论如何都必须索引它，
					 * 以保留 MVCC 语义。（在我们完成索引构建后，既有的
					 * 事务可能尝试使用该索引，并且可能需要看到这样的元组。）
					 *
					 * 然而，如果它是 HOT 更新的，则我们必须只索引位于
					 * HOT 链末端存活的元组。由于这破坏了既有快照的语义，
					 * 需将该索引标记为对它们不可用。
					 *
					 * 即使我们索引了最近死亡元组，也不将其计入 reltuples；
					 * 参见 heapam_scan_analyze_next_tuple()。
					 */
					if (HeapTupleIsHotUpdated(heapTuple))
					{
						indexIt = false;
						/* 将索引标记为对旧快照不安全 */
						indexInfo->ii_BrokenHotChain = true;
					}
					else
						indexIt = true;
					/* 无论如何，将该元组排除在唯一性检查之外 */
					tupleIsAlive = false;
					break;
				case HEAPTUPLE_INSERT_IN_PROGRESS:

					/*
					 * In "anyvisible" mode, this tuple is visible and we
					 * don't need any further checks.
					 */
					if (anyvisible)
					{
						indexIt = true;
						tupleIsAlive = true;
						reltuples += 1;
						break;
					}

					/*
					 * 由于调用方应持有 ShareLock 或更高级别的锁，通常只有
					 * 在本事务中更早插入时才可能看到此情形。不过在系统目录
					 * 中也可能发生，因为我们在提交前往往会先释放写锁。
					 * 若两种情况都不满足，则给出告警。
					 */
					xwait = HeapTupleHeaderGetXmin(heapTuple->t_data);
					if (!TransactionIdIsCurrentTransactionId(xwait))
					{
						if (!is_system_catalog)
							elog(WARNING, "concurrent insert in progress within table \"%s\"",
								 RelationGetRelationName(heapRelation));

						/*
						 * 若我们正在进行唯一性检查，索引这样的元组可能
						 * 导致虚假的唯一性失败。此时我们等待插入事务
						 * 完成后再重新检查。
						 */
						if (checking_uniqueness)
						{
							/*
							 * 等待前必须先释放缓冲区上的锁
							 */
							LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);
							XactLockTableWait(xwait, heapRelation,
											  &heapTuple->t_self,
											  XLTW_InsertIndexUnique);
							CHECK_FOR_INTERRUPTS();
							goto recheck;
						}
					}
					else
					{
						/*
						 * 为与 heapam_scan_analyze_next_tuple() 保持一致，
						 * 仅当 INSERT_IN_PROGRESS 元组由本事务插入时才将其
						 * 计为存活。
						 */
						reltuples += 1;
					}

					/*
					 * 我们必须索引这样的元组，因为如果索引构建提交，
					 * 它们就是有效的。
					 */
					indexIt = true;
					tupleIsAlive = true;
					break;
				case HEAPTUPLE_DELETE_IN_PROGRESS:

					/*
					 * 与 INSERT_IN_PROGRESS 情形类似，除非是本事务自己的删除
					 * 或是系统目录，否则这不应发生；但在 anyvisible 模式下，
					 * 该元组是可见的。
					 */
					if (anyvisible)
					{
						indexIt = true;
						tupleIsAlive = false;
						reltuples += 1;
						break;
					}

					xwait = HeapTupleHeaderGetUpdateXid(heapTuple->t_data);
					if (!TransactionIdIsCurrentTransactionId(xwait))
					{
						if (!is_system_catalog)
							elog(WARNING, "concurrent delete in progress within table \"%s\"",
								 RelationGetRelationName(heapRelation));

						/*
						 * 若我们正在进行唯一性检查，假设该元组已死亡可能
						 * 导致遗漏唯一性冲突。此时我们等待删除事务完成
						 * 后再重新检查。
						 *
						 * 此外，如果它是 HOT 更新的元组，我们不应索引它，
						 * 而应索引 HOT 链末端存活的元组。然而，删除事务
						 * 可能中止，最终使该元组保持存活，这种情况下仍须
						 * 对其建立索引。要知道该如何处理，唯一办法就是等待
						 * 删除事务完成后再重新检查。
						 */
						if (checking_uniqueness ||
							HeapTupleIsHotUpdated(heapTuple))
						{
							/*
							 * 等待前必须先释放缓冲区上的锁
							 */
							LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);
							XactLockTableWait(xwait, heapRelation,
											  &heapTuple->t_self,
											  XLTW_InsertIndexUnique);
							CHECK_FOR_INTERRUPTS();
							goto recheck;
						}

						/*
						 * 否则对其建立索引但不检查唯一性，与 RECENTLY_DEAD
						 * 元组的处理相同。
						 */
						indexIt = true;

						/*
						 * 若 DELETE_IN_PROGRESS 元组不是由当前事务删除的，
						 * 则将其计为存活。这正是
						 * heapam_scan_analyze_next_tuple() 的做法，我们希望
						 * 行为保持一致。
						 */
						reltuples += 1;
					}
					else if (HeapTupleIsHotUpdated(heapTuple))
					{
						/*
						 * 这是一个由本事务删除的 HOT 更新元组。我们可以假定
						 * 该删除会提交（否则索引内容也无所谓），因此按与
						 * RECENTLY_DEAD 的 HOT 更新元组相同的方式处理。
						 */
						indexIt = false;
						/* 将索引标记为对旧快照不安全 */
						indexInfo->ii_BrokenHotChain = true;
					}
					else
					{
						/*
						 * 这是一个由本事务删除的普通元组。对其建立索引，但
						 * 不检查唯一性，也不计入 reltuples，与 RECENTLY_DEAD
						 * 元组的处理相同。
						 */
						indexIt = true;
					}
					/* 无论如何，将该元组排除在唯一性检查之外 */
					tupleIsAlive = false;
					break;
				default:
					elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
     indexIt = tupleIsAlive = false; /* 避免编译器告警 */
					break;
			}

			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			if (!indexIt)
				continue;
		}
		else
		{
			/* heap_getnext 已进行了时间资格检查 */
			tupleIsAlive = true;
			reltuples += 1;
		}

		MemoryContextReset(econtext->ecxt_per_tuple_memory);

		/* 为谓词或表达式求值做准备 */
		ExecStoreBufferHeapTuple(heapTuple, slot, hscan->rs_cbuf);

		/*
		 * 在部分索引中，丢弃不满足谓词的元组。
		 */
		if (predicate != NULL)
		{
			if (!ExecQual(predicate, econtext))
				continue;
		}

		/*
		 * 针对当前堆元组，提取本索引所用的全部属性，并记录其中哪些
		 * 为空。这同时会完成所需表达式的求值。
		 */
		FormIndexDatum(indexInfo,
					   slot,
					   estate,
					   values,
					   isnull);

		/*
		 * 你可能会以为我们应该在这里直接构建索引元组，但某些索引
		 * AM 希望先对数据做进一步处理。因此这里改为传递 values[] 和
		 * isnull[] 数组。
		 */

		if (HeapTupleIsHeapOnly(heapTuple))
		{
			/*
			 * 对于 heap-only 元组，将其 TID 伪装成 root 元组的 TID。
			 * 相关讨论见 src/backend/access/heap/README.HOT。
			 */
			ItemPointerData tid;
			OffsetNumber offnum;

			offnum = ItemPointerGetOffsetNumber(&heapTuple->t_self);

			/*
			 * 如果某 HOT 元组指向一个我们未知其信息的 root，则重新
			 * 获取 root 项。若仍然失败，则将其报告为损坏。
			 */
			if (root_offsets[offnum - 1] == InvalidOffsetNumber)
			{
				Page		page = BufferGetPage(hscan->rs_cbuf);

				LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_SHARE);
				heap_get_root_tuples(page, root_offsets);
				LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);
			}

			if (!OffsetNumberIsValid(root_offsets[offnum - 1]))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg_internal("failed to find parent tuple for heap-only tuple at (%u,%u) in table \"%s\"",
										 ItemPointerGetBlockNumber(&heapTuple->t_self),
										 offnum,
										 RelationGetRelationName(heapRelation))));

			ItemPointerSet(&tid, ItemPointerGetBlockNumber(&heapTuple->t_self),
						   root_offsets[offnum - 1]);

			/* 调用 AM 的回调例程来处理该元组 */
			callback(indexRelation, &tid, values, isnull, tupleIsAlive,
					 callback_state);
		}
		else
		{
			/* 调用 AM 的回调例程来处理该元组 */
			callback(indexRelation, &heapTuple->t_self, values, isnull,
					 tupleIsAlive, callback_state);
		}
	}

	/* 最后一次上报扫描进度。 */
	if (progress)
	{
		BlockNumber blks_done;

		if (hscan->rs_base.rs_parallel != NULL)
		{
			ParallelBlockTableScanDesc pbscan;

			pbscan = (ParallelBlockTableScanDesc) hscan->rs_base.rs_parallel;
			blks_done = pbscan->phs_nblocks;
		}
		else
			blks_done = hscan->rs_nblocks;

		pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE,
									 blks_done);
	}

	table_endscan(scan);

	/* 若快照由我们设置并注册，现在可以丢弃它了 */
	if (need_unregister_snapshot)
		UnregisterSnapshot(snapshot);

	ExecDropSingleTupleTableSlot(slot);

	FreeExecutorState(estate);

	/* 这些指针可能原本指向已不存在的 estate */
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NULL;

	return reltuples;
}

static void
heapam_index_validate_scan(Relation heapRelation,
						   Relation indexRelation,
						   IndexInfo *indexInfo,
						   Snapshot snapshot,
						   ValidateIndexState *state)
{
	TableScanDesc scan;
	HeapScanDesc hscan;
	HeapTuple	heapTuple;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	ExprState  *predicate;
	TupleTableSlot *slot;
	EState	   *estate;
	ExprContext *econtext;
	BlockNumber root_blkno = InvalidBlockNumber;
	OffsetNumber root_offsets[MaxHeapTuplesPerPage];
	bool		in_index[MaxHeapTuplesPerPage];
	BlockNumber previous_blkno = InvalidBlockNumber;

	/* 用于归并的状态变量 */
	ItemPointer indexcursor = NULL;
	ItemPointerData decoded;
	bool		tuplesort_empty = false;

	/*
	 * 健全性检查
	 */
	Assert(OidIsValid(indexRelation->rd_rel->relam));

	/*
	 * 需要 EState 用于计算索引表达式和部分索引谓词。
	 * 还需要一个 slot 来保存当前元组。
	 */
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = MakeSingleTupleTableSlot(RelationGetDescr(heapRelation),
									&TTSOpsHeapTuple);

	/* 将 econtext 的扫描元组设置为正在测试的元组 */
	econtext->ecxt_scantuple = slot;

	/* 若有谓词，设置其执行状态。 */
	predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);

	/*
	 * 准备对基关系进行扫描。我们只需要那些满足传入的参考快照的元组。
	 * 这里必须禁用 syncscan，因为我们必须从 0 号块开始向前读取，
	 * 以与排好序的 TID 相匹配，这一点至关重要。
	 */
 scan = table_beginscan_strat(heapRelation,	/* relation 关系 */
         snapshot,	/* snapshot 快照 */
         0, /* number of keys 键的数量 */
         NULL,	/* scan key 扫描键 */
         true,	/* buffer access strategy OK 允许缓冲区访问策略 */
         false);	/* syncscan not OK 不允许 syncscan */
	hscan = (HeapScanDesc) scan;

	pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_TOTAL,
								 hscan->rs_nblocks);

	/*
	 * 扫描所有与快照匹配的元组。
	 */
	while ((heapTuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		ItemPointer heapcursor = &heapTuple->t_self;
		ItemPointerData rootTuple;
		OffsetNumber root_offnum;

		CHECK_FOR_INTERRUPTS();

		state->htups += 1;

		if ((previous_blkno == InvalidBlockNumber) ||
			(hscan->rs_cblock != previous_blkno))
		{
			pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE,
										 hscan->rs_cblock);
			previous_blkno = hscan->rs_cblock;
		}

		/*
		 * 正如 table_index_build_scan 中的注释所述，我们应该在 heap-only
		 * 元组的 root 元组 TID 之下建立索引；因此当我们前进到新的堆页面时，
		 * 需要构建该页面上 root 项偏移量的映射。
		 *
		 * 这会使与 tuplesort 输出的归并变得复杂：我们会按偏移量顺序访问
		 * 存活元组，但我们需要拿来与索引内容比较的 root 偏移量可能以不同
		 * 的顺序排列。因此我们可能需要在 tuplesort 输出中"回看"，但仅限于
		 * 当前页面内。我们通过维护一个 bool 数组 in_index[] 来实现，它记录
		 * 当前页面上所有已被越过（passed-over）的 tuplesort 输出 TID。
		 * 当前进到新的堆页面时，我们在此处清空该数组。
		 */
		if (hscan->rs_cblock != root_blkno)
		{
			Page		page = BufferGetPage(hscan->rs_cbuf);

			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_SHARE);
			heap_get_root_tuples(page, root_offsets);
			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			memset(in_index, 0, sizeof(in_index));

			root_blkno = hscan->rs_cblock;
		}

		/* 将实际元组 TID 转换为 root TID */
		rootTuple = *heapcursor;
		root_offnum = ItemPointerGetOffsetNumber(heapcursor);

		if (HeapTupleIsHeapOnly(heapTuple))
		{
			root_offnum = root_offsets[root_offnum - 1];
			if (!OffsetNumberIsValid(root_offnum))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg_internal("failed to find parent tuple for heap-only tuple at (%u,%u) in table \"%s\"",
										 ItemPointerGetBlockNumber(heapcursor),
										 ItemPointerGetOffsetNumber(heapcursor),
										 RelationGetRelationName(heapRelation))));
			ItemPointerSetOffsetNumber(&rootTuple, root_offnum);
		}

		/*
		 * 通过跳过索引元组，直到找到或越过当前 root 元组，完成"归并"。
		 */
		while (!tuplesort_empty &&
			   (!indexcursor ||
				ItemPointerCompare(indexcursor, &rootTuple) < 0))
		{
			Datum		ts_val;
			bool		ts_isnull;

			if (indexcursor)
			{
				/*
				 * 记录当前堆页面上此前已看到的索引项
				 */
				if (ItemPointerGetBlockNumber(indexcursor) == root_blkno)
					in_index[ItemPointerGetOffsetNumber(indexcursor) - 1] = true;
			}

			tuplesort_empty = !tuplesort_getdatum(state->tuplesort, true,
												  false, &ts_val, &ts_isnull,
												  NULL);
			Assert(tuplesort_empty || !ts_isnull);
			if (!tuplesort_empty)
			{
				itemptr_decode(&decoded, DatumGetInt64(ts_val));
				indexcursor = &decoded;
			}
			else
			{
				/* 保持整洁 */
				indexcursor = NULL;
			}
		}

		/*
		 * If the tuplesort has overshot *and* we didn't see a match earlier,
		 * then this tuple is missing from the index, so insert it.
		 */
		if ((tuplesort_empty ||
			 ItemPointerCompare(indexcursor, &rootTuple) > 0) &&
			!in_index[root_offnum - 1])
		{
			MemoryContextReset(econtext->ecxt_per_tuple_memory);

			/* 为谓词或表达式求值做准备 */
			ExecStoreHeapTuple(heapTuple, slot, false);

			/*
			 * 在部分索引中，丢弃不满足谓词的元组。
			 */
			if (predicate != NULL)
			{
				if (!ExecQual(predicate, econtext))
					continue;
			}

			/*
			 * 针对当前堆元组，提取本索引所用的全部属性，并记录其中哪些
			 * 为空。这同时会完成所需表达式的求值。
			 */
			FormIndexDatum(indexInfo,
						   slot,
						   estate,
						   values,
						   isnull);

			/*
			 * 你可能会以为我们应该在这里直接构建索引元组，但某些索引
			 * AM 希望先对数据做进一步处理。因此这里改为传递 values[] 和
			 * isnull[] 数组。
			 */

			/*
			 * 如果元组已经提交死亡，你可能会以为我们可以跳过唯一性检查，
			 * 但在存在 HOT 的情况下这已不再成立，因为这次插入实际上是针对
			 * 整个 HOT 链的唯一性检查的代理。也就是说，我们这里的元组可能
			 * 已经因为早已被 HOT 更新而为死亡，而若是如此，执行该更新的事务
			 * 并不会认为它应当插入索引项。索引 AM 会检查整个 HOT 链，并在
			 * 存在冲突时正确地检测出来。
			 */

			index_insert(indexRelation,
						 values,
						 isnull,
						 &rootTuple,
						 heapRelation,
						 indexInfo->ii_Unique ?
						 UNIQUE_CHECK_YES : UNIQUE_CHECK_NO,
						 false,
						 indexInfo);

			state->tups_inserted += 1;
		}
	}

	table_endscan(scan);

	ExecDropSingleTupleTableSlot(slot);

	FreeExecutorState(estate);

	/* 这些指针可能原本指向已不存在的 estate */
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NULL;
}

/*
 * 返回自启动以来本扫描已读取的块数量。这主要用于进度上报，
 * 而不追求完全精确：在并行扫描中，工作进程可能正在并发读取
 * 比我们上报位置更靠前的块。
 */
static BlockNumber
heapam_scan_get_blocks_done(HeapScanDesc hscan)
{
	ParallelBlockTableScanDesc bpscan = NULL;
	BlockNumber startblock;
	BlockNumber blocks_done;

	if (hscan->rs_base.rs_parallel != NULL)
	{
		bpscan = (ParallelBlockTableScanDesc) hscan->rs_base.rs_parallel;
		startblock = bpscan->phs_startblock;
	}
	else
		startblock = hscan->rs_startblock;

	/*
	 * 若 startblock 不为零，可能已经绕回到关系末尾。
	 */
	if (hscan->rs_cblock > startblock)
		blocks_done = hscan->rs_cblock - startblock;
	else
	{
		BlockNumber nblocks;

		nblocks = bpscan != NULL ? bpscan->phs_nblocks : hscan->rs_nblocks;
		blocks_done = nblocks - startblock +
			hscan->rs_cblock;
	}

	return blocks_done;
}


/* ------------------------------------------------------------------------
 * 堆 AM 的杂项回调
 * ------------------------------------------------------------------------
 */

/*
 * 检查该表是否需要 TOAST 表。仅当满足以下条件时才需要：(1) 存在
 * 任意可 TOAST 的属性；并且 (2) 元组的最大长度可能超过
 * TOAST_TUPLE_THRESHOLD。（我们不想为类似 "f1 varchar(20)" 这样的
 * 东西创建 TOAST 表。）
 */
static bool
heapam_relation_needs_toast_table(Relation rel)
{
	int32		data_length = 0;
	bool		maxlength_unknown = false;
	bool		has_toastable_attrs = false;
	TupleDesc	tupdesc = rel->rd_att;
	int32		tuple_length;
	int			i;

	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;
		if (att->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
			continue;
		data_length = att_align_nominal(data_length, att->attalign);
		if (att->attlen > 0)
		{
			/* 定长类型永不可 TOAST */
			data_length += att->attlen;
		}
		else
		{
			int32		maxlen = type_maximum_size(att->atttypid,
												   att->atttypmod);

			if (maxlen < 0)
				maxlength_unknown = true;
			else
				data_length += maxlen;
			if (att->attstorage != TYPSTORAGE_PLAIN)
				has_toastable_attrs = true;
		}
	}
	if (!has_toastable_attrs)
  return false;			/* 没有可 TOAST 的内容？ */
	if (maxlength_unknown)
  return true;			/* 存在任意不限长度的属性？ */
	tuple_length = MAXALIGN(SizeofHeapTupleHeader +
							BITMAPLEN(tupdesc->natts)) +
		MAXALIGN(data_length);
	return (tuple_length > TOAST_TUPLE_THRESHOLD);
}

/*
 * 堆关系的 TOAST 表本身就是普通的堆关系。
 */
static Oid
heapam_relation_toast_am(Relation rel)
{
	return rel->rd_rel->relam;
}


/* ------------------------------------------------------------------------
 * Planner related callbacks for the heap AM
 * ------------------------------------------------------------------------
 */

#define HEAP_OVERHEAD_BYTES_PER_TUPLE \
	(MAXALIGN(SizeofHeapTupleHeader) + sizeof(ItemIdData))
#define HEAP_USABLE_BYTES_PER_PAGE \
	(BLCKSZ - SizeOfPageHeaderData)

static void
heapam_estimate_rel_size(Relation rel, int32 *attr_widths,
						 BlockNumber *pages, double *tuples,
						 double *allvisfrac)
{
	table_block_relation_estimate_size(rel, attr_widths, pages,
									   tuples, allvisfrac,
									   HEAP_OVERHEAD_BYTES_PER_TUPLE,
									   HEAP_USABLE_BYTES_PER_PAGE);
}


/* ------------------------------------------------------------------------
 * Executor related callbacks for the heap AM
 * ------------------------------------------------------------------------
 */

static bool
heapam_scan_bitmap_next_tuple(TableScanDesc scan,
							  TupleTableSlot *slot,
							  bool *recheck,
							  uint64 *lossy_pages,
							  uint64 *exact_pages)
{
	BitmapHeapScanDesc bscan = (BitmapHeapScanDesc) scan;
	HeapScanDesc hscan = (HeapScanDesc) bscan;
	OffsetNumber targoffset;
	Page		page;
	ItemId		lp;

	/*
	 * Out of range?  If so, nothing more to look at on this page
	 */
	while (hscan->rs_cindex >= hscan->rs_ntuples)
	{
		/*
		 * 若位图已耗尽且我们无需再扫描其他块，则返回 false。
		 */
		if (!BitmapHeapScanNextBlock(scan, recheck, lossy_pages, exact_pages))
			return false;
	}

	targoffset = hscan->rs_vistuples[hscan->rs_cindex];
	page = BufferGetPage(hscan->rs_cbuf);
	lp = PageGetItemId(page, targoffset);
	Assert(ItemIdIsNormal(lp));

	hscan->rs_ctup.t_data = (HeapTupleHeader) PageGetItem(page, lp);
	hscan->rs_ctup.t_len = ItemIdGetLength(lp);
	hscan->rs_ctup.t_tableOid = scan->rs_rd->rd_id;
	ItemPointerSet(&hscan->rs_ctup.t_self, hscan->rs_cblock, targoffset);

	pgstat_count_heap_fetch(scan->rs_rd);

	/*
	 * 将结果 slot 设置为指向该元组。注意该 slot 会获取缓冲区上的 pin。
	 */
	ExecStoreBufferHeapTuple(&hscan->rs_ctup,
							 slot,
							 hscan->rs_cbuf);

	hscan->rs_cindex++;

	return true;
}

static bool
heapam_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate)
{
	HeapScanDesc hscan = (HeapScanDesc) scan;
	TsmRoutine *tsm = scanstate->tsmroutine;
	BlockNumber blockno;

	/* 若关系为空则立即返回 false */
	if (hscan->rs_nblocks == 0)
		return false;

	/* 释放先前的扫描缓冲区（若存在） */
	if (BufferIsValid(hscan->rs_cbuf))
	{
		ReleaseBuffer(hscan->rs_cbuf);
		hscan->rs_cbuf = InvalidBuffer;
	}

	if (tsm->NextSampleBlock)
		blockno = tsm->NextSampleBlock(scanstate, hscan->rs_nblocks);
	else
	{
		/* 顺序扫描表 */

		if (hscan->rs_cblock == InvalidBlockNumber)
		{
			Assert(!hscan->rs_inited);
			blockno = hscan->rs_startblock;
		}
		else
		{
			Assert(hscan->rs_inited);

			blockno = hscan->rs_cblock + 1;

			if (blockno >= hscan->rs_nblocks)
			{
				/* 绕回到关系开头，可能并非从 0 开始 */
				blockno = 0;
			}

		/*
		 * 为同步目的上报我们的新扫描位置。
		 *
		 * 注意：我们在检查扫描是否结束之前执行此操作，以便位置提示的
		 * 最终状态回到关系的开头。这并非严格必要，但如果不这样做，
		 * 多次运行同一查询时起始位置每次都会略微向后偏移，容易令人困惑。
		 * 不过一般而言，我们并不保证任何特定的顺序。
		 */
			if (scan->rs_flags & SO_ALLOW_SYNC)
				ss_report_location(scan->rs_rd, blockno);

			if (blockno == hscan->rs_startblock)
			{
				blockno = InvalidBlockNumber;
			}
		}
	}

	hscan->rs_cblock = blockno;

	if (!BlockNumberIsValid(blockno))
	{
		hscan->rs_inited = false;
		return false;
	}

	Assert(hscan->rs_cblock < hscan->rs_nblocks);

	/*
	 * Be sure to check for interrupts at least once per page.  Checks at
	 * higher code levels won't be able to stop a sample scan that encounters
	 * many pages' worth of consecutive dead tuples.
	 */
	CHECK_FOR_INTERRUPTS();

	/* 使用选定的策略读取页面 */
	hscan->rs_cbuf = ReadBufferExtended(hscan->rs_base.rs_rd, MAIN_FORKNUM,
										blockno, RBM_NORMAL, hscan->rs_strategy);

	/* 在 pagemode 下，剪枝页面并确定可见元组偏移量 */
	if (hscan->rs_base.rs_flags & SO_ALLOW_PAGEMODE)
		heap_prepare_pagescan(scan);

	hscan->rs_inited = true;
	return true;
}

static bool
heapam_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate,
							  TupleTableSlot *slot)
{
	HeapScanDesc hscan = (HeapScanDesc) scan;
	TsmRoutine *tsm = scanstate->tsmroutine;
	BlockNumber blockno = hscan->rs_cblock;
	bool		pagemode = (scan->rs_flags & SO_ALLOW_PAGEMODE) != 0;

	Page		page;
	bool		all_visible;
	OffsetNumber maxoffset;

	/*
	 * 当不使用 pagemode 时，我们必须在元组可见性检查期间锁定缓冲区。
	 */
	if (!pagemode)
		LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_SHARE);

	page = (Page) BufferGetPage(hscan->rs_cbuf);
	all_visible = PageIsAllVisible(page) &&
		!scan->rs_snapshot->takenDuringRecovery;
	maxoffset = PageGetMaxOffsetNumber(page);

	for (;;)
	{
		OffsetNumber tupoffset;

		CHECK_FOR_INTERRUPTS();

		/* 向表采样方法询问本页面上应检查哪些元组。 */
		tupoffset = tsm->NextSampleTuple(scanstate,
										 blockno,
										 maxoffset);

		if (OffsetNumberIsValid(tupoffset))
		{
			ItemId		itemid;
			bool		visible;
			HeapTuple	tuple = &(hscan->rs_ctup);

			/* 跳过无效的元组指针。 */
			itemid = PageGetItemId(page, tupoffset);
			if (!ItemIdIsNormal(itemid))
				continue;

			tuple->t_data = (HeapTupleHeader) PageGetItem(page, itemid);
			tuple->t_len = ItemIdGetLength(itemid);
			ItemPointerSet(&(tuple->t_self), blockno, tupoffset);


			if (all_visible)
				visible = true;
			else
				visible = SampleHeapTupleVisible(scan, hscan->rs_cbuf,
												 tuple, tupoffset);

			/* 在 pagemode 下，heap_prepare_pagescan 已为我们完成此工作 */
			if (!pagemode)
				HeapCheckForSerializableConflictOut(visible, scan->rs_rd, tuple,
													hscan->rs_cbuf, scan->rs_snapshot);

			/* 尝试同一页面上的下一个元组。 */
			if (!visible)
				continue;

			/* 找到可见元组，将其返回。 */
			if (!pagemode)
				LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			ExecStoreBufferHeapTuple(tuple, slot, hscan->rs_cbuf);

			/* 将成功获取的元组计为堆获取 */
			pgstat_count_heap_getnext(scan->rs_rd);

			return true;
		}
		else
		{
			/*
			 * 若执行到这里，说明本页面上的项已耗尽，是时候前进到下一页了。
			 */
			if (!pagemode)
				LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			ExecClearTuple(slot);
			return false;
		}
	}

	Assert(0);
}


/* ----------------------------------------------------------------------------
 *  Helper functions for the above.
 * ----------------------------------------------------------------------------
 */

/*
 * 重建并改写给定的元组
 *
 * 我们不能简单地原样复制该元组，原因有几点：
 *
 * 1. 我们希望挤出所有已删除列的值，既能节省空间，又能确保不会
 * 出现任何边界情况导致的失败。（例如，新表可能没有 TOAST 表，
 * 因而无法存储已删除列的任何大值。）
 *
 * 2. 该元组甚至可能对于新表是非法的；目前已知这只会在 ALTER TABLE
 * SET WITHOUT OIDS 的副作用下发生。
 *
 * 因此，我们必须从构成元组的各个 Datum 中重建该元组。
 */
static void
reform_and_rewrite_tuple(HeapTuple tuple,
						 Relation OldHeap, Relation NewHeap,
						 Datum *values, bool *isnull, RewriteState rwstate)
{
	TupleDesc	oldTupDesc = RelationGetDescr(OldHeap);
	TupleDesc	newTupDesc = RelationGetDescr(NewHeap);
	HeapTuple	copiedTuple;
	int			i;

	heap_deform_tuple(tuple, oldTupDesc, values, isnull);

	/* 务必将任何已删除列置空 */
	for (i = 0; i < newTupDesc->natts; i++)
	{
		if (TupleDescCompactAttr(newTupDesc, i)->attisdropped)
			isnull[i] = true;
	}

	copiedTuple = heap_form_tuple(newTupDesc, values, isnull);

	/* 其余工作由堆重写模块完成 */
	rewrite_heap_tuple(rwstate, tuple, copiedTuple);

	heap_freetuple(copiedTuple);
}

/*
 * 检查元组的可见性。
 */
static bool
SampleHeapTupleVisible(TableScanDesc scan, Buffer buffer,
					   HeapTuple tuple,
					   OffsetNumber tupoffset)
{
	HeapScanDesc hscan = (HeapScanDesc) scan;

	if (scan->rs_flags & SO_ALLOW_PAGEMODE)
	{
		uint32		start = 0,
					end = hscan->rs_ntuples;

		/*
		 * 在 pageatatime 模式下，heap_prepare_pagescan() 已经完成了可见性
		 * 检查，因此只需查看它留在 rs_vistuples[] 中的信息。
		 *
		 * 我们在已知有序的数组上使用二分查找。注意：如果我们强制要求
		 * NextSampleTuple 按递增顺序选择元组，本可省去一些开销，但不确定
		 * 由此带来的收益是否足以证明该限制是合理的。
		 */
		while (start < end)
		{
			uint32		mid = start + (end - start) / 2;
			OffsetNumber curoffset = hscan->rs_vistuples[mid];

			if (tupoffset == curoffset)
				return true;
			else if (tupoffset < curoffset)
				end = mid;
			else
				start = mid + 1;
		}

		return false;
	}
	else
	{
		/* 否则，我们必须单独检查该元组。 */
		return HeapTupleSatisfiesVisibility(tuple, scan->rs_snapshot,
											buffer);
	}
}

/*
 * 辅助函数：获取位图堆扫描的下一个块。当获取到下一块并将其保存到
 * 扫描描述符中时返回 true；当位图或关系已耗尽时返回 false。
 */
static bool
BitmapHeapScanNextBlock(TableScanDesc scan,
						bool *recheck,
						uint64 *lossy_pages, uint64 *exact_pages)
{
	BitmapHeapScanDesc bscan = (BitmapHeapScanDesc) scan;
	HeapScanDesc hscan = (HeapScanDesc) bscan;
	BlockNumber block;
	void	   *per_buffer_data;
	Buffer		buffer;
	Snapshot	snapshot;
	int			ntup;
	TBMIterateResult *tbmres;
	OffsetNumber offsets[TBM_MAX_TUPLES_PER_PAGE];
	int			noffsets = -1;

	Assert(scan->rs_flags & SO_TYPE_BITMAPSCAN);
	Assert(hscan->rs_read_stream);

	hscan->rs_cindex = 0;
	hscan->rs_ntuples = 0;

	/* 释放包含上一块的缓冲区。 */
	if (BufferIsValid(hscan->rs_cbuf))
	{
		ReleaseBuffer(hscan->rs_cbuf);
		hscan->rs_cbuf = InvalidBuffer;
	}

	hscan->rs_cbuf = read_stream_next_buffer(hscan->rs_read_stream,
											 &per_buffer_data);

	if (BufferIsInvalid(hscan->rs_cbuf))
	{
		/* 位图已耗尽 */
		return false;
	}

	Assert(per_buffer_data);

	tbmres = per_buffer_data;

	Assert(BlockNumberIsValid(tbmres->blockno));
	Assert(BufferGetBlockNumber(hscan->rs_cbuf) == tbmres->blockno);

	/* 精确页面需要提取其元组偏移量。 */
	if (!tbmres->lossy)
		noffsets = tbm_extract_page_tuple(tbmres, offsets,
										  TBM_MAX_TUPLES_PER_PAGE);

	*recheck = tbmres->recheck;

	block = hscan->rs_cblock = tbmres->blockno;
	buffer = hscan->rs_cbuf;
	snapshot = scan->rs_snapshot;

	ntup = 0;

	/*
	 * 在可能的情况下，对整页进行剪枝并修复碎片。
	 */
	heap_page_prune_opt(scan->rs_rd, buffer);

	/*
	 * 在检查元组可见性期间，我们必须持有缓冲区内容的共享锁。
	 * 但在此之后，只要我们还持有缓冲区的 pin，已被判定为可见的元组
	 * 就保证是有效的。
	 */
	LockBuffer(buffer, BUFFER_LOCK_SHARE);

	/*
	 * 对于有损和无损这两种情况，我们需要采用不同的策略。
	 */
	if (!tbmres->lossy)
	{
		/*
		 * 位图是无损的，因此我们只需查看 tbmres 中列出的偏移量；但我们必须
		 * 跟随从每个这样的偏移量开始的任意 HOT 链。
		 */
		int			curslot;

		/* 到此时我们必须已经提取了元组偏移量 */
		Assert(noffsets > -1);

		for (curslot = 0; curslot < noffsets; curslot++)
		{
			OffsetNumber offnum = offsets[curslot];
			ItemPointerData tid;
			HeapTupleData heapTuple;

			ItemPointerSet(&tid, block, offnum);
			if (heap_hot_search_buffer(&tid, scan->rs_rd, buffer, snapshot,
									   &heapTuple, NULL, true))
				hscan->rs_vistuples[ntup++] = ItemPointerGetOffsetNumber(&tid);
		}
	}
	else
	{
		/*
		 * 位图是有损的，因此我们必须检查页面上的每个行指针。但我们
		 * 可以忽略 HOT 链，因为无论如何我们都会检查每个元组。
		 */
		Page		page = BufferGetPage(buffer);
		OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
		OffsetNumber offnum;

		for (offnum = FirstOffsetNumber; offnum <= maxoff; offnum = OffsetNumberNext(offnum))
		{
			ItemId		lp;
			HeapTupleData loctup;
			bool		valid;

			lp = PageGetItemId(page, offnum);
			if (!ItemIdIsNormal(lp))
				continue;
			loctup.t_data = (HeapTupleHeader) PageGetItem(page, lp);
			loctup.t_len = ItemIdGetLength(lp);
			loctup.t_tableOid = scan->rs_rd->rd_id;
			ItemPointerSet(&loctup.t_self, block, offnum);
			valid = HeapTupleSatisfiesVisibility(&loctup, snapshot, buffer);
			if (valid)
			{
				hscan->rs_vistuples[ntup++] = offnum;
				PredicateLockTID(scan->rs_rd, &loctup.t_self, snapshot,
								 HeapTupleHeaderGetXmin(loctup.t_data));
			}
			HeapCheckForSerializableConflictOut(valid, scan->rs_rd, &loctup,
												buffer, snapshot);
		}
	}

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	Assert(ntup <= MaxHeapTuplesPerPage);
	hscan->rs_ntuples = ntup;

	if (tbmres->lossy)
		(*lossy_pages)++;
	else
		(*exact_pages)++;

	/*
	 * 返回 true 表示找到了一个有效块且位图尚未耗尽。如果本页面上没有
	 * 可见元组，hscan->rs_ntuples 将为 0，而 heapam_scan_bitmap_next_tuple()
	 * 会返回 false，从而把控制权交回本函数以推进到 Bitmap 中的下一个块。
	 */
	return true;
}

/* ------------------------------------------------------------------------
 * 堆表访问方法的定义。
 * ------------------------------------------------------------------------
 */

static const TableAmRoutine heapam_methods = {
	.type = T_TableAmRoutine,

	.slot_callbacks = heapam_slot_callbacks,

	.scan_begin = heap_beginscan,
	.scan_end = heap_endscan,
	.scan_rescan = heap_rescan,
	.scan_getnextslot = heap_getnextslot,

	.scan_set_tidrange = heap_set_tidrange,
	.scan_getnextslot_tidrange = heap_getnextslot_tidrange,

	.parallelscan_estimate = table_block_parallelscan_estimate,
	.parallelscan_initialize = table_block_parallelscan_initialize,
	.parallelscan_reinitialize = table_block_parallelscan_reinitialize,

	.index_fetch_begin = heapam_index_fetch_begin,
	.index_fetch_reset = heapam_index_fetch_reset,
	.index_fetch_end = heapam_index_fetch_end,
	.index_fetch_tuple = heapam_index_fetch_tuple,

	.tuple_insert = heapam_tuple_insert,
	.tuple_insert_speculative = heapam_tuple_insert_speculative,
	.tuple_complete_speculative = heapam_tuple_complete_speculative,
	.multi_insert = heap_multi_insert,
	.tuple_delete = heapam_tuple_delete,
	.tuple_update = heapam_tuple_update,
	.tuple_lock = heapam_tuple_lock,

	.tuple_fetch_row_version = heapam_fetch_row_version,
	.tuple_get_latest_tid = heap_get_latest_tid,
	.tuple_tid_valid = heapam_tuple_tid_valid,
	.tuple_satisfies_snapshot = heapam_tuple_satisfies_snapshot,
	.index_delete_tuples = heap_index_delete_tuples,

	.relation_set_new_filelocator = heapam_relation_set_new_filelocator,
	.relation_nontransactional_truncate = heapam_relation_nontransactional_truncate,
	.relation_copy_data = heapam_relation_copy_data,
	.relation_copy_for_cluster = heapam_relation_copy_for_cluster,
	.relation_vacuum = heap_vacuum_rel,
	.scan_analyze_next_block = heapam_scan_analyze_next_block,
	.scan_analyze_next_tuple = heapam_scan_analyze_next_tuple,
	.index_build_range_scan = heapam_index_build_range_scan,
	.index_validate_scan = heapam_index_validate_scan,

	.relation_size = table_block_relation_size,
	.relation_needs_toast_table = heapam_relation_needs_toast_table,
	.relation_toast_am = heapam_relation_toast_am,
	.relation_fetch_toast_slice = heap_fetch_toast_slice,

	.relation_estimate_size = heapam_estimate_rel_size,

	.scan_bitmap_next_tuple = heapam_scan_bitmap_next_tuple,
	.scan_sample_next_block = heapam_scan_sample_next_block,
	.scan_sample_next_tuple = heapam_scan_sample_next_tuple
};


const TableAmRoutine *
GetHeapamTableAmRoutine(void)
{
	return &heapam_methods;
}

Datum
heap_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&heapam_methods);
}
