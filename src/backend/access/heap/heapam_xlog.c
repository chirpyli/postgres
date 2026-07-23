/*-------------------------------------------------------------------------
 *
 * heapam_xlog.c
 *	  堆访问方法（heap access method）的 WAL 重放逻辑。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/heapam_xlog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/bufmask.h"
#include "access/heapam.h"
#include "access/visibilitymap.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "storage/freespace.h"
#include "storage/standby.h"


/*
 * Replay XLOG_HEAP2_PRUNE_* 记录。
 */
static void
heap_xlog_prune_freeze(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	char	   *maindataptr = XLogRecGetData(record);
	xl_heap_prune xlrec;
	Buffer		buffer;
	RelFileLocator rlocator;
	BlockNumber blkno;
	XLogRedoAction action;

	XLogRecGetBlockTag(record, 0, &rlocator, NULL, &blkno);
	memcpy(&xlrec, maindataptr, SizeOfHeapPrune);
	maindataptr += SizeOfHeapPrune;

	/*
	 * 我们将根据 XLHP_CLEANUP_LOCK 标志是否设置，来使用普通排他锁或 cleanup
	 * 锁。对于普通排他锁，我们最好不要执行任何需要移动已有元组数据的操作。
	 */
	Assert((xlrec.flags & XLHP_CLEANUP_LOCK) != 0 ||
		   (xlrec.flags & (XLHP_HAS_REDIRECTIONS | XLHP_HAS_DEAD_ITEMS)) == 0);

	/*
	 * 我们即将删除和/或冻结元组。在 Hot Standby 模式下，要确保没有正在运行的
	 * 查询仍然能看到这些被删除的元组，也没有查询仍然认为被冻结的 xid 处于
	 * 运行状态。冲突边界 XID 紧跟在 xl_heap_prune 之后。
	 */
	if ((xlrec.flags & XLHP_HAS_CONFLICT_HORIZON) != 0)
	{
		TransactionId snapshot_conflict_horizon;

		/* memcpy() 是因为 snapshot_conflict_horizon 是未对齐存储的 */
		memcpy(&snapshot_conflict_horizon, maindataptr, sizeof(TransactionId));
		maindataptr += sizeof(TransactionId);

		if (InHotStandby)
			ResolveRecoveryConflictWithSnapshot(snapshot_conflict_horizon,
												(xlrec.flags & XLHP_IS_CATALOG_REL) != 0,
												rlocator);
	}

	/*
	 * 如果我们有全页镜像，就恢复它，然后就完成了。
	 */
	action = XLogReadBufferForRedoExtended(record, 0, RBM_NORMAL,
										   (xlrec.flags & XLHP_CLEANUP_LOCK) != 0,
										   &buffer);
	if (action == BLK_NEEDS_REDO)
	{
		Page		page = (Page) BufferGetPage(buffer);
		OffsetNumber *redirected;
		OffsetNumber *nowdead;
		OffsetNumber *nowunused;
		int			nredirected;
		int			ndead;
		int			nunused;
		int			nplans;
		Size		datalen;
		xlhp_freeze_plan *plans;
		OffsetNumber *frz_offsets;
		char	   *dataptr = XLogRecGetBlockData(record, 0, &datalen);

		heap_xlog_deserialize_prune_and_freeze(dataptr, xlrec.flags,
											   &nplans, &plans, &frz_offsets,
											   &nredirected, &redirected,
											   &ndead, &nowdead,
											   &nunused, &nowunused);

		/*
		 * 根据记录更新所有行指针，并在需要时修复碎片。
		 */
		if (nredirected > 0 || ndead > 0 || nunused > 0)
			heap_page_prune_execute(buffer,
									(xlrec.flags & XLHP_CLEANUP_LOCK) == 0,
									redirected, nredirected,
									nowdead, ndead,
									nowunused, nunused);

		/* 冻结元组 */
		for (int p = 0; p < nplans; p++)
		{
			HeapTupleFreeze frz;

			/*
			 * 将冻结计划在 WAL 记录中的表示形式转换为
			 * heap_execute_freeze_tuple 所使用的逐元组格式
			 */
			frz.xmax = plans[p].xmax;
			frz.t_infomask2 = plans[p].t_infomask2;
			frz.t_infomask = plans[p].t_infomask;
			frz.frzflags = plans[p].frzflags;
			frz.offset = InvalidOffsetNumber;	/* 未使用，但保持整洁 */

			for (int i = 0; i < plans[p].ntuples; i++)
			{
				OffsetNumber offset = *(frz_offsets++);
				ItemId		lp;
				HeapTupleHeader tuple;

				lp = PageGetItemId(page, offset);
				tuple = (HeapTupleHeader) PageGetItem(page, lp);
				heap_execute_freeze_tuple(tuple, &frz);
			}
		}

		/* 应该没有更多数据了 */
		Assert((char *) frz_offsets == dataptr + datalen);

		/*
		 * 注意：我们不必费心更新页面的可清理提示。最差情况下，这会导致
		 * 不久后多进行一次清理周期。
		 */

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}

	/*
	 * 如果我们释放了任何空间或行指针，就更新空闲空间映射。
	 *
	 * 无论是否应用了全页镜像，都要这样做，因为 FSM 数据本来也不在页面中。
	 */
	if (BufferIsValid(buffer))
	{
		if (xlrec.flags & (XLHP_HAS_REDIRECTIONS |
						   XLHP_HAS_DEAD_ITEMS |
						   XLHP_HAS_NOW_UNUSED_ITEMS))
		{
			Size		freespace = PageGetHeapFreeSpace(BufferGetPage(buffer));

			UnlockReleaseBuffer(buffer);

			XLogRecordPageWithFreeSpace(rlocator, blkno, freespace);
		}
		else
			UnlockReleaseBuffer(buffer);
	}
}

/*
 * Replay XLOG_HEAP2_VISIBLE 记录。
 *
 * 这里至关重要的完整性要求是：我们绝不能出现可见性映射位被设置、而页面级的
 * PD_ALL_VISIBLE 位被清除的情况。如果发生这种情况，后续对该页面的修改将无法
 * 清除可见性映射位。
 */
static void
heap_xlog_visible(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_visible *xlrec = (xl_heap_visible *) XLogRecGetData(record);
	Buffer		vmbuffer = InvalidBuffer;
	Buffer		buffer;
	Page		page;
	RelFileLocator rlocator;
	BlockNumber blkno;
	XLogRedoAction action;

	Assert((xlrec->flags & VISIBILITYMAP_XLOG_VALID_BITS) == xlrec->flags);

	XLogRecGetBlockTag(record, 1, &rlocator, NULL, &blkno);

	/*
	 * 如果有任何 Hot Standby 事务正在运行，且其 xmin 边界足够旧、以至于该页面
	 * 对它们并非全可见，那么它们可能会错误地认为仅索引扫描可以跳过堆读取。
	 *
	 * 注意：在此处抛出某种“软”冲突，强制任何正在进行的仅索引扫描去执行堆
	 * 读取，可能比直接杀死事务更好。
	 */
	if (InHotStandby)
		ResolveRecoveryConflictWithSnapshot(xlrec->snapshotConflictHorizon,
											xlrec->flags & VISIBILITYMAP_XLOG_CATALOG_REL,
											rlocator);

	/*
	 * 读取堆页面（如果它仍然存在）。如果在恢复过程中堆文件后来被删除或截断，
	 * 我们就不需要更新该页面，但最好仍然更新可见性映射。
	 */
	action = XLogReadBufferForRedo(record, 1, &buffer);
	if (action == BLK_NEEDS_REDO)
	{
		/*
		 * 在设置可见性映射位时，我们不会提升堆页面的 LSN（除非启用了校验和或
		 * wal_hint_bits，在这种情况下我们必须提升）。这使我们面临页面撕裂的
		 * 风险，但由于我们根本不会以任何方式检查现有页面内容，所以我们并不在意。
		 */
		page = BufferGetPage(buffer);

		PageSetAllVisible(page);

		if (XLogHintBitIsNeeded())
			PageSetLSN(page, lsn);

		MarkBufferDirty(buffer);
	}
	else if (action == BLK_RESTORED)
	{
		/*
		 * 如果堆块已被备份，我们已经恢复了它，没有更多事情要做。（这只可能在
		 * 启用了校验和或 wal_log_hints 时发生。）
		 */
	}

	if (BufferIsValid(buffer))
	{
		Size		space = PageGetFreeSpace(BufferGetPage(buffer));

		UnlockReleaseBuffer(buffer);

		/*
		 * 由于 FSM 没有被 WAL 记录，并且只是启发式地更新，它在备机上很容易变得
		 * 陈旧。如果备机后来被提升为主机并运行 VACUUM，它会跳过对变为全可见
		 * （或全冻结，取决于 VACUUM 模式）的页面的各空闲空间数值的更新，而当
		 * FreeSpaceMapVacuum 将过于乐观的空闲空间值传播到上层 FSM 时，这会成为
		 * 问题；后续的插入者试图使用这些页面，结果却发现它们不可用。当存在大量
		 * 此类页面时，这会导致长时间的停顿。
		 *
		 * 通过更新 FSM 中关于正在变为全可见或全冻结页面的认识，来预防这些问题。
		 *
		 * 无论是否应用了全页镜像，都要这样做，因为 FSM 数据本来也不在页面中。
		 */
		if (xlrec->flags & VISIBILITYMAP_VALID_BITS)
			XLogRecordPageWithFreeSpace(rlocator, blkno, space);
	}

	/*
	 * 即使由于 LSN 互斥机制而跳过了堆页面更新，更新可见性映射仍然是安全的。
	 * 任何清除可见性映射位的 WAL 记录都会在检查页面 LSN 之前执行清除，因此
	 * 任何需要被清除的位仍然会被清除。
	 */
	if (XLogReadBufferForRedoExtended(record, 0, RBM_ZERO_ON_ERROR, false,
									  &vmbuffer) == BLK_NEEDS_REDO)
	{
		Page		vmpage = BufferGetPage(vmbuffer);
		Relation	reln;
		uint8		vmbits;

		/* 如果页面是以全零形式读入的，则初始化它 */
		if (PageIsNew(vmpage))
			PageInit(vmpage, BLCKSZ, 0);

		/* 移除 VISIBILITYMAP_XLOG_* */
		vmbits = xlrec->flags & VISIBILITYMAP_VALID_BITS;

		/*
		 * XLogReadBufferForRedoExtended 已经锁定了缓冲区。但 visibilitymap_set
		 * 会自行处理加锁。
		 */
		LockBuffer(vmbuffer, BUFFER_LOCK_UNLOCK);

		reln = CreateFakeRelcacheEntry(rlocator);
		visibilitymap_pin(reln, blkno, &vmbuffer);

		visibilitymap_set(reln, blkno, InvalidBuffer, lsn, vmbuffer,
						  xlrec->snapshotConflictHorizon, vmbits);

		ReleaseBuffer(vmbuffer);
		FreeFakeRelcacheEntry(reln);
	}
	else if (BufferIsValid(vmbuffer))
		UnlockReleaseBuffer(vmbuffer);
}

/*
 * 给定一个来自 XLog 记录的 "infobits" 字段，为记录所涉及的元组在指定的
 * infomask 和 infomask2 中设置正确的位。
 *
 * （这是 compute_infobits 的逆操作）。
 */
static void
fix_infomask_from_infobits(uint8 infobits, uint16 *infomask, uint16 *infomask2)
{
	*infomask &= ~(HEAP_XMAX_IS_MULTI | HEAP_XMAX_LOCK_ONLY |
				   HEAP_XMAX_KEYSHR_LOCK | HEAP_XMAX_EXCL_LOCK);
	*infomask2 &= ~HEAP_KEYS_UPDATED;

	if (infobits & XLHL_XMAX_IS_MULTI)
		*infomask |= HEAP_XMAX_IS_MULTI;
	if (infobits & XLHL_XMAX_LOCK_ONLY)
		*infomask |= HEAP_XMAX_LOCK_ONLY;
	if (infobits & XLHL_XMAX_EXCL_LOCK)
		*infomask |= HEAP_XMAX_EXCL_LOCK;
	/* 注意：这里没有考虑 HEAP_XMAX_SHR_LOCK */
	if (infobits & XLHL_XMAX_KEYSHR_LOCK)
		*infomask |= HEAP_XMAX_KEYSHR_LOCK;

	if (infobits & XLHL_KEYS_UPDATED)
		*infomask2 |= HEAP_KEYS_UPDATED;
}

/*
 * Replay XLOG_HEAP_DELETE 记录。
 */
static void
heap_xlog_delete(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_delete *xlrec = (xl_heap_delete *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	ItemId		lp = NULL;
	HeapTupleHeader htup;
	BlockNumber blkno;
	RelFileLocator target_locator;
	ItemPointerData target_tid;

	XLogRecGetBlockTag(record, 0, &target_locator, NULL, &blkno);
	ItemPointerSetBlockNumber(&target_tid, blkno);
	ItemPointerSetOffsetNumber(&target_tid, xlrec->offnum);

	/*
	 * 即使堆页面已经是最新的，可见性映射可能仍然需要被修复。
	 */
	if (xlrec->flags & XLH_DELETE_ALL_VISIBLE_CLEARED)
	{
		Relation	reln = CreateFakeRelcacheEntry(target_locator);
		Buffer		vmbuffer = InvalidBuffer;

		visibilitymap_pin(reln, blkno, &vmbuffer);
		visibilitymap_clear(reln, blkno, vmbuffer, VISIBILITYMAP_VALID_BITS);
		ReleaseBuffer(vmbuffer);
		FreeFakeRelcacheEntry(reln);
	}

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(buffer);

		if (PageGetMaxOffsetNumber(page) >= xlrec->offnum)
			lp = PageGetItemId(page, xlrec->offnum);

		if (PageGetMaxOffsetNumber(page) < xlrec->offnum || !ItemIdIsNormal(lp))
			elog(PANIC, "invalid lp");

		htup = (HeapTupleHeader) PageGetItem(page, lp);

		htup->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
		htup->t_infomask2 &= ~HEAP_KEYS_UPDATED;
		HeapTupleHeaderClearHotUpdated(htup);
		fix_infomask_from_infobits(xlrec->infobits_set,
								   &htup->t_infomask, &htup->t_infomask2);
		if (!(xlrec->flags & XLH_DELETE_IS_SUPER))
			HeapTupleHeaderSetXmax(htup, xlrec->xmax);
		else
			HeapTupleHeaderSetXmin(htup, InvalidTransactionId);
		HeapTupleHeaderSetCmax(htup, FirstCommandId, false);

		/* 将页面标记为清理的候选 */
		PageSetPrunable(page, XLogRecGetXid(record));

		if (xlrec->flags & XLH_DELETE_ALL_VISIBLE_CLEARED)
			PageClearAllVisible(page);

		/* 确保 t_ctid 被正确设置 */
		if (xlrec->flags & XLH_DELETE_IS_PARTITION_MOVE)
			HeapTupleHeaderSetMovedPartitions(htup);
		else
			htup->t_ctid = target_tid;
		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

/*
 * Replay XLOG_HEAP_INSERT 记录。
 */
static void
heap_xlog_insert(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_insert *xlrec = (xl_heap_insert *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	union
	{
		HeapTupleHeaderData hdr;
		char		data[MaxHeapTupleSize];
	}			tbuf;
	HeapTupleHeader htup;
	xl_heap_header xlhdr;
	uint32		newlen;
	Size		freespace = 0;
	RelFileLocator target_locator;
	BlockNumber blkno;
	ItemPointerData target_tid;
	XLogRedoAction action;

	XLogRecGetBlockTag(record, 0, &target_locator, NULL, &blkno);
	ItemPointerSetBlockNumber(&target_tid, blkno);
	ItemPointerSetOffsetNumber(&target_tid, xlrec->offnum);

	/* heap_insert() 代码路径中不会冻结 */
	Assert(!(xlrec->flags & XLH_INSERT_ALL_FROZEN_SET));

	/*
	 * 即使堆页面已经是最新的，可见性映射可能仍然需要被修复。
	 */
	if (xlrec->flags & XLH_INSERT_ALL_VISIBLE_CLEARED)
	{
		Relation	reln = CreateFakeRelcacheEntry(target_locator);
		Buffer		vmbuffer = InvalidBuffer;

		visibilitymap_pin(reln, blkno, &vmbuffer);
		visibilitymap_clear(reln, blkno, vmbuffer, VISIBILITYMAP_VALID_BITS);
		ReleaseBuffer(vmbuffer);
		FreeFakeRelcacheEntry(reln);
	}

	/*
	 * 如果我们插入的是页面上第一个也是唯一一个元组，就从头重新初始化该页面。
	 */
	if (XLogRecGetInfo(record) & XLOG_HEAP_INIT_PAGE)
	{
		buffer = XLogInitBufferForRedo(record, 0);
		page = BufferGetPage(buffer);
		PageInit(page, BufferGetPageSize(buffer), 0);
		action = BLK_NEEDS_REDO;
	}
	else
		action = XLogReadBufferForRedo(record, 0, &buffer);
	if (action == BLK_NEEDS_REDO)
	{
		Size		datalen;
		char	   *data;

		page = BufferGetPage(buffer);

		if (PageGetMaxOffsetNumber(page) + 1 < xlrec->offnum)
			elog(PANIC, "invalid max offset number");

		data = XLogRecGetBlockData(record, 0, &datalen);

		newlen = datalen - SizeOfHeapHeader;
		Assert(datalen > SizeOfHeapHeader && newlen <= MaxHeapTupleSize);
		memcpy(&xlhdr, data, SizeOfHeapHeader);
		data += SizeOfHeapHeader;

		htup = &tbuf.hdr;
		MemSet(htup, 0, SizeofHeapTupleHeader);
		/* PG73FORMAT: 获取 bitmap [+ padding] [+ oid] + data */
		memcpy((char *) htup + SizeofHeapTupleHeader,
			   data,
			   newlen);
		newlen += SizeofHeapTupleHeader;
		htup->t_infomask2 = xlhdr.t_infomask2;
		htup->t_infomask = xlhdr.t_infomask;
		htup->t_hoff = xlhdr.t_hoff;
		HeapTupleHeaderSetXmin(htup, XLogRecGetXid(record));
		HeapTupleHeaderSetCmin(htup, FirstCommandId);
		htup->t_ctid = target_tid;

		if (PageAddItem(page, (Item) htup, newlen, xlrec->offnum,
						true, true) == InvalidOffsetNumber)
			elog(PANIC, "failed to add tuple");

		freespace = PageGetHeapFreeSpace(page); /* 用于更新下面的 FSM */

		PageSetLSN(page, lsn);

		if (xlrec->flags & XLH_INSERT_ALL_VISIBLE_CLEARED)
			PageClearAllVisible(page);

		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	/*
	 * 如果页面的空闲空间很少，也要更新 FSM。我们随意地将“很少”定义为低于
	 * 20%。在不了解表填充因子的情况下，我们做不到更好。
	 *
	 * XXX: 如果页面是从全页镜像恢复的，就不要这样做。我们在那种情况下不费心
	 * 更新 FSM，反正它也不需要完全准确。
	 */
	if (action == BLK_NEEDS_REDO && freespace < BLCKSZ / 5)
		XLogRecordPageWithFreeSpace(target_locator, blkno, freespace);
}

/*
 * Replay XLOG_HEAP2_MULTI_INSERT 记录。
 */
static void
heap_xlog_multi_insert(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_multi_insert *xlrec;
	RelFileLocator rlocator;
	BlockNumber blkno;
	Buffer		buffer;
	Page		page;
	union
	{
		HeapTupleHeaderData hdr;
		char		data[MaxHeapTupleSize];
	}			tbuf;
	HeapTupleHeader htup;
	uint32		newlen;
	Size		freespace = 0;
	int			i;
	bool		isinit = (XLogRecGetInfo(record) & XLOG_HEAP_INIT_PAGE) != 0;
	XLogRedoAction action;

	/*
	 * 插入操作不会覆盖 MVCC 数据，因此不需要冲突处理。
	 */
	xlrec = (xl_heap_multi_insert *) XLogRecGetData(record);

	XLogRecGetBlockTag(record, 0, &rlocator, NULL, &blkno);

	/* 检查互斥的标志没有同时被设置 */
	Assert(!((xlrec->flags & XLH_INSERT_ALL_VISIBLE_CLEARED) &&
			 (xlrec->flags & XLH_INSERT_ALL_FROZEN_SET)));

	/*
	 * 即使堆页面已经是最新的，可见性映射可能仍然需要被修复。
	 */
	if (xlrec->flags & XLH_INSERT_ALL_VISIBLE_CLEARED)
	{
		Relation	reln = CreateFakeRelcacheEntry(rlocator);
		Buffer		vmbuffer = InvalidBuffer;

		visibilitymap_pin(reln, blkno, &vmbuffer);
		visibilitymap_clear(reln, blkno, vmbuffer, VISIBILITYMAP_VALID_BITS);
		ReleaseBuffer(vmbuffer);
		FreeFakeRelcacheEntry(reln);
	}

	if (isinit)
	{
		buffer = XLogInitBufferForRedo(record, 0);
		page = BufferGetPage(buffer);
		PageInit(page, BufferGetPageSize(buffer), 0);
		action = BLK_NEEDS_REDO;
	}
	else
		action = XLogReadBufferForRedo(record, 0, &buffer);
	if (action == BLK_NEEDS_REDO)
	{
		char	   *tupdata;
		char	   *endptr;
		Size		len;

		/* 元组以块数据的形式存储 */
		tupdata = XLogRecGetBlockData(record, 0, &len);
		endptr = tupdata + len;

		page = (Page) BufferGetPage(buffer);

		for (i = 0; i < xlrec->ntuples; i++)
		{
			OffsetNumber offnum;
			xl_multi_insert_tuple *xlhdr;

			/*
			 * 如果我们正在重新初始化页面，元组会按从 FirstOffsetNumber 开始的顺序
			 * 存储。否则，WAL 记录中会有一个偏移量数组，元组紧随其后。
			 */
			if (isinit)
				offnum = FirstOffsetNumber + i;
			else
				offnum = xlrec->offsets[i];
			if (PageGetMaxOffsetNumber(page) + 1 < offnum)
				elog(PANIC, "invalid max offset number");

			xlhdr = (xl_multi_insert_tuple *) SHORTALIGN(tupdata);
			tupdata = ((char *) xlhdr) + SizeOfMultiInsertTuple;

			newlen = xlhdr->datalen;
			Assert(newlen <= MaxHeapTupleSize);
			htup = &tbuf.hdr;
			MemSet(htup, 0, SizeofHeapTupleHeader);
			/* PG73FORMAT: 获取 bitmap [+ padding] [+ oid] + data */
			memcpy((char *) htup + SizeofHeapTupleHeader,
				   tupdata,
				   newlen);
			tupdata += newlen;

			newlen += SizeofHeapTupleHeader;
			htup->t_infomask2 = xlhdr->t_infomask2;
			htup->t_infomask = xlhdr->t_infomask;
			htup->t_hoff = xlhdr->t_hoff;
			HeapTupleHeaderSetXmin(htup, XLogRecGetXid(record));
			HeapTupleHeaderSetCmin(htup, FirstCommandId);
			ItemPointerSetBlockNumber(&htup->t_ctid, blkno);
			ItemPointerSetOffsetNumber(&htup->t_ctid, offnum);

			offnum = PageAddItem(page, (Item) htup, newlen, offnum, true, true);
			if (offnum == InvalidOffsetNumber)
				elog(PANIC, "failed to add tuple");
		}
		if (tupdata != endptr)
			elog(PANIC, "total tuple length mismatch");

		freespace = PageGetHeapFreeSpace(page); /* 用于更新下面的 FSM */

		PageSetLSN(page, lsn);

		if (xlrec->flags & XLH_INSERT_ALL_VISIBLE_CLEARED)
			PageClearAllVisible(page);

		/* XLH_INSERT_ALL_FROZEN_SET 意味着所有元组都是可见的 */
		if (xlrec->flags & XLH_INSERT_ALL_FROZEN_SET)
			PageSetAllVisible(page);

		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	/*
	 * 如果页面的空闲空间很少，也要更新 FSM。我们随意地将“很少”定义为低于
	 * 20%。在不了解表填充因子的情况下，我们做不到更好。
	 *
	 * XXX: 如果页面是从全页镜像恢复的，就不要这样做。我们在那种情况下不费心
	 * 更新 FSM，反正它也不需要完全准确。
	 */
	if (action == BLK_NEEDS_REDO && freespace < BLCKSZ / 5)
		XLogRecordPageWithFreeSpace(rlocator, blkno, freespace);
}

/*
 * Replay XLOG_HEAP_UPDATE 和 XLOG_HEAP_HOT_UPDATE 记录。
 */
static void
heap_xlog_update(XLogReaderState *record, bool hot_update)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_update *xlrec = (xl_heap_update *) XLogRecGetData(record);
	RelFileLocator rlocator;
	BlockNumber oldblk;
	BlockNumber newblk;
	ItemPointerData newtid;
	Buffer		obuffer,
				nbuffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleData oldtup;
	HeapTupleHeader htup;
	uint16		prefixlen = 0,
				suffixlen = 0;
	char	   *newp;
	union
	{
		HeapTupleHeaderData hdr;
		char		data[MaxHeapTupleSize];
	}			tbuf;
	xl_heap_header xlhdr;
	uint32		newlen;
	Size		freespace = 0;
	XLogRedoAction oldaction;
	XLogRedoAction newaction;

	/* 初始化以保持编译器安静 */
	oldtup.t_data = NULL;
	oldtup.t_len = 0;

	XLogRecGetBlockTag(record, 0, &rlocator, NULL, &newblk);
	if (XLogRecGetBlockTagExtended(record, 1, NULL, NULL, &oldblk, NULL))
	{
		/* HOT 更新绝不会跨页面进行 */
		Assert(!hot_update);
	}
	else
		oldblk = newblk;

	ItemPointerSet(&newtid, newblk, xlrec->new_offnum);

	/*
	 * 即使堆页面已经是最新的，可见性映射可能仍然需要被修复。
	 */
	if (xlrec->flags & XLH_UPDATE_OLD_ALL_VISIBLE_CLEARED)
	{
		Relation	reln = CreateFakeRelcacheEntry(rlocator);
		Buffer		vmbuffer = InvalidBuffer;

		visibilitymap_pin(reln, oldblk, &vmbuffer);
		visibilitymap_clear(reln, oldblk, vmbuffer, VISIBILITYMAP_VALID_BITS);
		ReleaseBuffer(vmbuffer);
		FreeFakeRelcacheEntry(reln);
	}

	/*
	 * 在正常操作中，按页号顺序锁定两个页面非常重要，以避免与其他反向进行的
	 * 更新操作发生可能的死锁。然而，在 WAL 重放期间不可能有其他更新发生，所以
	 * 我们不必担心这一点。但我们的确需要担心：不能向 Hot Standby 查询暴露
	 * 不一致的状态——因此在将新元组添加到新页面之前，不能解锁原始页面。
	 */

	/* 处理旧的元组版本 */
	oldaction = XLogReadBufferForRedo(record, (oldblk == newblk) ? 0 : 1,
									  &obuffer);
	if (oldaction == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(obuffer);
		offnum = xlrec->old_offnum;
		if (PageGetMaxOffsetNumber(page) >= offnum)
			lp = PageGetItemId(page, offnum);

		if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsNormal(lp))
			elog(PANIC, "invalid lp");

		htup = (HeapTupleHeader) PageGetItem(page, lp);

		oldtup.t_data = htup;
		oldtup.t_len = ItemIdGetLength(lp);

		htup->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
		htup->t_infomask2 &= ~HEAP_KEYS_UPDATED;
		if (hot_update)
			HeapTupleHeaderSetHotUpdated(htup);
		else
			HeapTupleHeaderClearHotUpdated(htup);
		fix_infomask_from_infobits(xlrec->old_infobits_set, &htup->t_infomask,
								   &htup->t_infomask2);
		HeapTupleHeaderSetXmax(htup, xlrec->old_xmax);
		HeapTupleHeaderSetCmax(htup, FirstCommandId, false);
		/* 在 t_ctid 中设置前向链链接 */
		htup->t_ctid = newtid;

		/* 将页面标记为清理的候选 */
		PageSetPrunable(page, XLogRecGetXid(record));

		if (xlrec->flags & XLH_UPDATE_OLD_ALL_VISIBLE_CLEARED)
			PageClearAllVisible(page);

		PageSetLSN(page, lsn);
		MarkBufferDirty(obuffer);
	}

	/*
	 * 读取新元组要插入的页面（如果与旧页面不同的话）。
	 */
	if (oldblk == newblk)
	{
		nbuffer = obuffer;
		newaction = oldaction;
	}
	else if (XLogRecGetInfo(record) & XLOG_HEAP_INIT_PAGE)
	{
		nbuffer = XLogInitBufferForRedo(record, 0);
		page = (Page) BufferGetPage(nbuffer);
		PageInit(page, BufferGetPageSize(nbuffer), 0);
		newaction = BLK_NEEDS_REDO;
	}
	else
		newaction = XLogReadBufferForRedo(record, 0, &nbuffer);

	/*
	 * 即使堆页面已经是最新的，可见性映射可能仍然需要被修复。
	 */
	if (xlrec->flags & XLH_UPDATE_NEW_ALL_VISIBLE_CLEARED)
	{
		Relation	reln = CreateFakeRelcacheEntry(rlocator);
		Buffer		vmbuffer = InvalidBuffer;

		visibilitymap_pin(reln, newblk, &vmbuffer);
		visibilitymap_clear(reln, newblk, vmbuffer, VISIBILITYMAP_VALID_BITS);
		ReleaseBuffer(vmbuffer);
		FreeFakeRelcacheEntry(reln);
	}

	/* 处理新元组 */
	if (newaction == BLK_NEEDS_REDO)
	{
		char	   *recdata;
		char	   *recdata_end;
		Size		datalen;
		Size		tuplen;

		recdata = XLogRecGetBlockData(record, 0, &datalen);
		recdata_end = recdata + datalen;

		page = BufferGetPage(nbuffer);

		offnum = xlrec->new_offnum;
		if (PageGetMaxOffsetNumber(page) + 1 < offnum)
			elog(PANIC, "invalid max offset number");

		if (xlrec->flags & XLH_UPDATE_PREFIX_FROM_OLD)
		{
			Assert(newblk == oldblk);
			memcpy(&prefixlen, recdata, sizeof(uint16));
			recdata += sizeof(uint16);
		}
		if (xlrec->flags & XLH_UPDATE_SUFFIX_FROM_OLD)
		{
			Assert(newblk == oldblk);
			memcpy(&suffixlen, recdata, sizeof(uint16));
			recdata += sizeof(uint16);
		}

		memcpy(&xlhdr, recdata, SizeOfHeapHeader);
		recdata += SizeOfHeapHeader;

		tuplen = recdata_end - recdata;
		Assert(tuplen <= MaxHeapTupleSize);

		htup = &tbuf.hdr;
		MemSet(htup, 0, SizeofHeapTupleHeader);

			/*
			 * 利用旧元组的前缀和/或后缀，以及 WAL 记录中存储的数据，重建新元组。
			 */
		newp = (char *) htup + SizeofHeapTupleHeader;
		if (prefixlen > 0)
		{
			int			len;

			/* 从 WAL 记录复制 bitmap [+ padding] [+ oid] */
			len = xlhdr.t_hoff - SizeofHeapTupleHeader;
			memcpy(newp, recdata, len);
			recdata += len;
			newp += len;

			/* 从旧元组复制前缀 */
			memcpy(newp, (char *) oldtup.t_data + oldtup.t_data->t_hoff, prefixlen);
			newp += prefixlen;

			/* 从 WAL 记录复制新元组数据 */
			len = tuplen - (xlhdr.t_hoff - SizeofHeapTupleHeader);
			memcpy(newp, recdata, len);
			recdata += len;
			newp += len;
		}
		else
		{
			/*
			 * 一次性地从记录中复制 bitmap [+ padding] [+ oid] + data
			 */
			memcpy(newp, recdata, tuplen);
			recdata += tuplen;
			newp += tuplen;
		}
		Assert(recdata == recdata_end);

		/* 从旧元组复制后缀 */
		if (suffixlen > 0)
			memcpy(newp, (char *) oldtup.t_data + oldtup.t_len - suffixlen, suffixlen);

		newlen = SizeofHeapTupleHeader + tuplen + prefixlen + suffixlen;
		htup->t_infomask2 = xlhdr.t_infomask2;
		htup->t_infomask = xlhdr.t_infomask;
		htup->t_hoff = xlhdr.t_hoff;

		HeapTupleHeaderSetXmin(htup, XLogRecGetXid(record));
		HeapTupleHeaderSetCmin(htup, FirstCommandId);
		HeapTupleHeaderSetXmax(htup, xlrec->new_xmax);
		/* 确保 t_ctid 中没有前向链链接 */
		htup->t_ctid = newtid;

		offnum = PageAddItem(page, (Item) htup, newlen, offnum, true, true);
		if (offnum == InvalidOffsetNumber)
			elog(PANIC, "failed to add tuple");

		if (xlrec->flags & XLH_UPDATE_NEW_ALL_VISIBLE_CLEARED)
			PageClearAllVisible(page);

		freespace = PageGetHeapFreeSpace(page); /* 用于更新下面的 FSM */

		PageSetLSN(page, lsn);
		MarkBufferDirty(nbuffer);
	}

	if (BufferIsValid(nbuffer) && nbuffer != obuffer)
		UnlockReleaseBuffer(nbuffer);
	if (BufferIsValid(obuffer))
		UnlockReleaseBuffer(obuffer);

	/*
	 * 如果新页面的空闲空间很少，也要更新 FSM。我们随意地将“很少”定义为低于
	 * 20%。在不了解表填充因子的情况下，我们做不到更好。
	 *
	 * 然而，不要在 HOT 更新时更新 FSM，因为在崩溃恢复后，旧元组或新元组之一
	 * 必然已死且可被清理。假设新元组与旧元组大小大致相同，在清理之后，页面的
	 * 空闲空间将大致与更新前一样多。
	 *
	 * XXX: 如果页面是从全页镜像恢复的，就不要这样做。我们在那种情况下不费心
	 * 更新 FSM，反正它也不需要完全准确。
	 */
	if (newaction == BLK_NEEDS_REDO && !hot_update && freespace < BLCKSZ / 5)
		XLogRecordPageWithFreeSpace(rlocator, newblk, freespace);
}

/*
 * Replay XLOG_HEAP_CONFIRM 记录。
 */
static void
heap_xlog_confirm(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_confirm *xlrec = (xl_heap_confirm *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleHeader htup;

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(buffer);

		offnum = xlrec->offnum;
		if (PageGetMaxOffsetNumber(page) >= offnum)
			lp = PageGetItemId(page, offnum);

		if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsNormal(lp))
			elog(PANIC, "invalid lp");

		htup = (HeapTupleHeader) PageGetItem(page, lp);

		/*
		 * 将元组确认为实际已插入
		 */
		ItemPointerSet(&htup->t_ctid, BufferGetBlockNumber(buffer), offnum);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

/*
 * Replay XLOG_HEAP_LOCK 记录。
 */
static void
heap_xlog_lock(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_lock *xlrec = (xl_heap_lock *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleHeader htup;

	/*
	 * 即使堆页面已经是最新的，可见性映射可能仍然需要被修复。
	 */
	if (xlrec->flags & XLH_LOCK_ALL_FROZEN_CLEARED)
	{
		RelFileLocator rlocator;
		Buffer		vmbuffer = InvalidBuffer;
		BlockNumber block;
		Relation	reln;

		XLogRecGetBlockTag(record, 0, &rlocator, NULL, &block);
		reln = CreateFakeRelcacheEntry(rlocator);

		visibilitymap_pin(reln, block, &vmbuffer);
		visibilitymap_clear(reln, block, vmbuffer, VISIBILITYMAP_ALL_FROZEN);

		ReleaseBuffer(vmbuffer);
		FreeFakeRelcacheEntry(reln);
	}

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		page = (Page) BufferGetPage(buffer);

		offnum = xlrec->offnum;
		if (PageGetMaxOffsetNumber(page) >= offnum)
			lp = PageGetItemId(page, offnum);

		if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsNormal(lp))
			elog(PANIC, "invalid lp");

		htup = (HeapTupleHeader) PageGetItem(page, lp);

		htup->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
		htup->t_infomask2 &= ~HEAP_KEYS_UPDATED;
		fix_infomask_from_infobits(xlrec->infobits_set, &htup->t_infomask,
								   &htup->t_infomask2);

		/*
		 * 清除相关的更新标志，但仅在修改后的 infomask 表明没有更新时才清除。
		 */
		if (HEAP_XMAX_IS_LOCKED_ONLY(htup->t_infomask))
		{
			HeapTupleHeaderClearHotUpdated(htup);
			/* 确保 t_ctid 中没有前向链链接 */
			ItemPointerSet(&htup->t_ctid,
						   BufferGetBlockNumber(buffer),
						   offnum);
		}
		HeapTupleHeaderSetXmax(htup, xlrec->xmax);
		HeapTupleHeaderSetCmax(htup, FirstCommandId, false);
		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

/*
 * Replay XLOG_HEAP2_LOCK_UPDATED 记录。
 */
static void
heap_xlog_lock_updated(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_lock_updated *xlrec;
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleHeader htup;

	xlrec = (xl_heap_lock_updated *) XLogRecGetData(record);

	/*
	 * 即使堆页面已经是最新的，可见性映射可能仍然需要被修复。
	 */
	if (xlrec->flags & XLH_LOCK_ALL_FROZEN_CLEARED)
	{
		RelFileLocator rlocator;
		Buffer		vmbuffer = InvalidBuffer;
		BlockNumber block;
		Relation	reln;

		XLogRecGetBlockTag(record, 0, &rlocator, NULL, &block);
		reln = CreateFakeRelcacheEntry(rlocator);

		visibilitymap_pin(reln, block, &vmbuffer);
		visibilitymap_clear(reln, block, vmbuffer, VISIBILITYMAP_ALL_FROZEN);

		ReleaseBuffer(vmbuffer);
		FreeFakeRelcacheEntry(reln);
	}

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(buffer);

		offnum = xlrec->offnum;
		if (PageGetMaxOffsetNumber(page) >= offnum)
			lp = PageGetItemId(page, offnum);

		if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsNormal(lp))
			elog(PANIC, "invalid lp");

		htup = (HeapTupleHeader) PageGetItem(page, lp);

		htup->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
		htup->t_infomask2 &= ~HEAP_KEYS_UPDATED;
		fix_infomask_from_infobits(xlrec->infobits_set, &htup->t_infomask,
								   &htup->t_infomask2);
		HeapTupleHeaderSetXmax(htup, xlrec->xmax);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

/*
 * Replay XLOG_HEAP_INPLACE 记录。
 */
static void
heap_xlog_inplace(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_inplace *xlrec = (xl_heap_inplace *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleHeader htup;
	uint32		oldlen;
	Size		newlen;

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		char	   *newtup = XLogRecGetBlockData(record, 0, &newlen);

		page = BufferGetPage(buffer);

		offnum = xlrec->offnum;
		if (PageGetMaxOffsetNumber(page) >= offnum)
			lp = PageGetItemId(page, offnum);

		if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsNormal(lp))
			elog(PANIC, "invalid lp");

		htup = (HeapTupleHeader) PageGetItem(page, lp);

		oldlen = ItemIdGetLength(lp) - htup->t_hoff;
		if (oldlen != newlen)
			elog(PANIC, "wrong tuple length");

		memcpy((char *) htup + htup->t_hoff, newtup, newlen);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	ProcessCommittedInvalidationMessages(xlrec->msgs,
										 xlrec->nmsgs,
										 xlrec->relcacheInitFileInval,
										 xlrec->dbId,
										 xlrec->tsId);
}

void
heap_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	/*
	 * 这些操作不会覆盖 MVCC 数据，因此不需要冲突处理。heap2 资源管理器中的
	 * 那些操作则需要。
	 */

	switch (info & XLOG_HEAP_OPMASK)
	{
		case XLOG_HEAP_INSERT:
			heap_xlog_insert(record);
			break;
		case XLOG_HEAP_DELETE:
			heap_xlog_delete(record);
			break;
		case XLOG_HEAP_UPDATE:
			heap_xlog_update(record, false);
			break;
		case XLOG_HEAP_TRUNCATE:

		/*
		 * TRUNCATE 是一个空操作，因为其动作已经作为 SMGR WAL 记录被记录了。
		 * TRUNCATE WAL 记录仅用于逻辑解码。
		 */
			break;
		case XLOG_HEAP_HOT_UPDATE:
			heap_xlog_update(record, true);
			break;
		case XLOG_HEAP_CONFIRM:
			heap_xlog_confirm(record);
			break;
		case XLOG_HEAP_LOCK:
			heap_xlog_lock(record);
			break;
		case XLOG_HEAP_INPLACE:
			heap_xlog_inplace(record);
			break;
		default:
			elog(PANIC, "heap_redo: unknown op code %u", info);
	}
}

void
heap2_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info & XLOG_HEAP_OPMASK)
	{
		case XLOG_HEAP2_PRUNE_ON_ACCESS:
		case XLOG_HEAP2_PRUNE_VACUUM_SCAN:
		case XLOG_HEAP2_PRUNE_VACUUM_CLEANUP:
			heap_xlog_prune_freeze(record);
			break;
		case XLOG_HEAP2_VISIBLE:
			heap_xlog_visible(record);
			break;
		case XLOG_HEAP2_MULTI_INSERT:
			heap_xlog_multi_insert(record);
			break;
		case XLOG_HEAP2_LOCK_UPDATED:
			heap_xlog_lock_updated(record);
			break;
		case XLOG_HEAP2_NEW_CID:

		/*
		 * 在真正的重放时无需做任何事，它仅在逻辑解码期间使用。
		 */
			break;
		case XLOG_HEAP2_REWRITE:
			heap_xlog_logical_rewrite(record);
			break;
		default:
			elog(PANIC, "heap2_redo: unknown op code %u", info);
	}
}

/*
 * 在对堆页面执行一致性检查之前，对页面进行掩码处理。
 */
void
heap_mask(char *pagedata, BlockNumber blkno)
{
	Page		page = (Page) pagedata;
	OffsetNumber off;

	mask_page_lsn_and_checksum(page);

	mask_page_hint_bits(page);
	mask_unused_space(page);

	for (off = 1; off <= PageGetMaxOffsetNumber(page); off++)
	{
		ItemId		iid = PageGetItemId(page, off);
		char	   *page_item;

		page_item = (char *) (page + ItemIdGetOffset(iid));

		if (ItemIdIsNormal(iid))
		{
			HeapTupleHeader page_htup = (HeapTupleHeader) page_item;

			/*
			 * 如果元组的 xmin 尚未冻结，我们应该忽略 hint 位上的差异，因为
			 * 它们可以在不写 WAL 的情况下被设置。
			 */
			if (!HeapTupleHeaderXminFrozen(page_htup))
				page_htup->t_infomask &= ~HEAP_XACT_MASK;
			else
			{
				/* 我们仍然需要掩码掉 xmax 的 hint 位。 */
				page_htup->t_infomask &= ~HEAP_XMAX_INVALID;
				page_htup->t_infomask &= ~HEAP_XMAX_COMMITTED;
			}

			/*
			 * 在重放期间，我们将 Command Id 设置为 FirstCommandId。因此，对它也
			 * 进行掩码。详见 heap_xlog_insert()。
			 */
			page_htup->t_choice.t_heap.t_field3.t_cid = MASK_MARKER;

			/*
			 * 对于一个推测性元组（speculative tuple），heap_insert() 不会在调用者
			 * 传入的堆元组本身中设置 ctid，而是让 ctid 字段包含一个推测性令牌值
			 * ——一个每个后端单调递增的标识符。此外，它在任何情况下都不会将 ctid
			 * 写入 WAL。
			 *
			 * 在 redo 期间，heap_xlog_insert() 将 t_ctid 设置为当前块号和自身偏移
			 * 号。它并不关心主库上任何推测性插入。因此，我们将 t_ctid 设置为当前
			 * 块号和自身偏移号，以忽略任何不一致。
			 */
			if (HeapTupleHeaderIsSpeculative(page_htup))
				ItemPointerSet(&page_htup->t_ctid, blkno, off);

			/*
			 * 注意：不忽略因元组移动（即 HeapTupleHeaderIndicatesMovedPartitions）
			 * 而导致的 ctid 变更，因为这是需要在主库和备库之间保持同步的重要信息，
			 * 因此会被写入 WAL。
			 */
		}

			/*
			 * 当项的长度不是 MAXALIGN 对齐时，忽略元组之后的任何填充字节。
			 */
		if (ItemIdHasStorage(iid))
		{
			int			len = ItemIdGetLength(iid);
			int			padlen = MAXALIGN(len) - len;

			if (padlen > 0)
				memset(page_item + len, MASK_MARKER, padlen);
		}
	}
}
