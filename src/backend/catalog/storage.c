/*-------------------------------------------------------------------------
 *
 * storage.c
 *	  用于创建和销毁关系物理存储的代码
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/storage.c
 *
 * NOTES
 *	  这些代码的一部分曾经位于 storage/smgr/smgr.c 中，并且
 *	  其函数名仍然反映了这一点。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/visibilitymap.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bulk_write.h"
#include "storage/freespace.h"
#include "storage/proc.h"
#include "storage/smgr.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/* GUC 变量 */
int			wal_skip_threshold = 2048;	/* 单位：千字节 */

/*
 * 我们维护一个在当前事务中被创建或删除的所有关系（以 RelFileLocator
 * 值表示）的列表。当创建一个关系时，我们会立即创建其物理文件，但
 * 会记住它，以便在当前事务被中止时能够再次删除该文件。相反，删除请求
 * 不会立即执行，而只是被记录到列表中。当且仅当事务提交时，我们才会
 * 删除物理文件。
 *
 * 为了处理子事务，每个条目都以其事务嵌套级别进行了标记。在子事务提交
 * 时，我们将该子事务的条目重新指派给父级的嵌套级别。在子事务中止时，
 * 我们可以立即对当前嵌套级别的所有条目执行中止时的动作。
 *
 * 注意：该列表保存在 TopMemoryContext 中，以确保它不会提前消失。将其
 * 保存在 TopTransactionContext 中可能也没问题，但我比较谨慎。
 */

typedef struct PendingRelDelete
{
	RelFileLocator rlocator;	/* 可能需要被删除的关系 */
	ProcNumber	procNumber;		/* 如果不是临时关系则为 INVALID_PROC_NUMBER */
	bool		atCommit;		/* T=提交时删除；F=中止时删除 */
	int			nestLevel;		/* 请求的 xact 嵌套级别 */
	struct PendingRelDelete *next;	/* 链表链接 */
} PendingRelDelete;

typedef struct PendingRelSync
{
	RelFileLocator rlocator;
	bool		is_truncated;	/* 该文件是否经历过截断？ */
} PendingRelSync;

static PendingRelDelete *pendingDeletes = NULL; /* 链表的头指针 */
static HTAB *pendingSyncHash = NULL;


/*
 * AddPendingSync
 *		将一个提交时 fsync 排入队列。
 */
static void
AddPendingSync(const RelFileLocator *rlocator)
{
	PendingRelSync *pending;
	bool		found;

	/* 如果尚未创建哈希表则创建 */
	if (!pendingSyncHash)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(RelFileLocator);
		ctl.entrysize = sizeof(PendingRelSync);
		ctl.hcxt = TopTransactionContext;
		pendingSyncHash = hash_create("pending sync hash", 16, &ctl,
									  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	pending = hash_search(pendingSyncHash, rlocator, HASH_ENTER, &found);
	Assert(!found);
	pending->is_truncated = false;
}

/*
 * RelationCreateStorage
 *		为关系创建物理存储。
 *
 * 为关系创建底层的磁盘文件存储。这只会创建主分支（main fork）；
 * 其他分支由各需要的模块按需惰性创建。
 *
 * 本函数是事务性的。该创建过程会被写入 WAL 日志，如果事务随后中止，
 * 该存储将被销毁。如果调用方不希望存储在中止时被销毁，可以传入
 * register_delete = false。
 */
SMgrRelation
RelationCreateStorage(RelFileLocator rlocator, char relpersistence,
					  bool register_delete)
{
	SMgrRelation srel;
	ProcNumber	procNumber;
	bool		needs_wal;

	Assert(!IsInParallelMode());	/* 无法更新 pendingSyncHash */

	switch (relpersistence)
	{
		case RELPERSISTENCE_TEMP:
			procNumber = ProcNumberForTempRelations();
			needs_wal = false;
			break;
		case RELPERSISTENCE_UNLOGGED:
			procNumber = INVALID_PROC_NUMBER;
			needs_wal = false;
			break;
		case RELPERSISTENCE_PERMANENT:
			procNumber = INVALID_PROC_NUMBER;
			needs_wal = true;
			break;
		default:
			elog(ERROR, "invalid relpersistence: %c", relpersistence);
			return NULL;		/* 仅为安抚编译器 */
	}

	srel = smgropen(rlocator, procNumber);
	smgrcreate(srel, MAIN_FORKNUM, false);

	if (needs_wal)
		log_smgrcreate(&srel->smgr_rlocator.locator, MAIN_FORKNUM);

	/*
	 * 如果我们被要求这样做，则将该关系加入中止时需要删除的事物的列表。
	 */
	if (register_delete)
	{
		PendingRelDelete *pending;

		pending = (PendingRelDelete *)
			MemoryContextAlloc(TopMemoryContext, sizeof(PendingRelDelete));
		pending->rlocator = rlocator;
		pending->procNumber = procNumber;
		pending->atCommit = false;	/* 中止时删除 */
		pending->nestLevel = GetCurrentTransactionNestLevel();
		pending->next = pendingDeletes;
		pendingDeletes = pending;
	}

	if (relpersistence == RELPERSISTENCE_PERMANENT && !XLogIsNeeded())
	{
		Assert(procNumber == INVALID_PROC_NUMBER);
		AddPendingSync(&rlocator);
	}

	return srel;
}

/*
 * 向 WAL 执行 XLOG_SMGR_CREATE 记录的 XLogInsert 操作。
 */
void
log_smgrcreate(const RelFileLocator *rlocator, ForkNumber forkNum)
{
	xl_smgr_create xlrec;

	/*
	 * 写入一条报告文件创建的 XLOG 条目。
	 */
	xlrec.rlocator = *rlocator;
	xlrec.forkNum = forkNum;

	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	XLogInsert(RM_SMGR_ID, XLOG_SMGR_CREATE | XLR_SPECIAL_REL_UPDATE);
}

/*
 * RelationDropStorage
 *		安排事务提交时解除物理存储的链接（unlink）。
 */
void
RelationDropStorage(Relation rel)
{
	PendingRelDelete *pending;

	/* 将该关系加入提交时需要删除的事物的列表 */
	pending = (PendingRelDelete *)
		MemoryContextAlloc(TopMemoryContext, sizeof(PendingRelDelete));
	pending->rlocator = rel->rd_locator;
	pending->procNumber = rel->rd_backend;
	pending->atCommit = true;	/* 提交时删除 */
	pending->nestLevel = GetCurrentTransactionNestLevel();
	pending->next = pendingDeletes;
	pendingDeletes = pending;

	/*
	 * 注意：如果该关系是在本事务中创建的，那么现在它会以两种形式出现在
	 * 待删除列表中，一次是 atCommit 为 true，一次是 atCommit 为 false。
	 * 因此，无论哪种情况，它都会在该事务结束时被物理删除（而另一个条目
	 * 会被 smgrDoPendingDeletes 忽略，因此不会发生错误）。我们也可以改为
	 * 移除已有的列表条目并立即删除物理文件，但目前我保持逻辑简单。
	 */

	RelationCloseSmgr(rel);
}

/*
 * RelationPreserveStorage
 *		将一个关系标记为最终不需要删除。
 *
 * 我们需要这个函数，是因为关系映射的更改是与整个事务的提交分开提交的，
 * 因此在映射更新完成之后，事务仍然可能中止。当一个新物理关系被装入
 * 映射时，它会被安排为中止时删除，那样我们就会删除它，从而陷入麻烦。
 * 关系映射器通过在提交时告知我们不要删除这类关系来修复此问题。
 *
 * 在 ALTER TABLE 期间，我们也用这个函数来复用一个索引的旧构建版本，
 * 这次是移除提交时删除的条目。
 *
 * 如果该关系不在那些被安排删除的关系之中，则本函数为空操作。
 */
void
RelationPreserveStorage(RelFileLocator rlocator, bool atCommit)
{
	PendingRelDelete *pending;
	PendingRelDelete *prev;
	PendingRelDelete *next;

	prev = NULL;
	for (pending = pendingDeletes; pending != NULL; pending = next)
	{
		next = pending->next;
		if (RelFileLocatorEquals(rlocator, pending->rlocator)
			&& pending->atCommit == atCommit)
		{
		/* 解除链接并删除列表条目 */
		if (prev)
			prev->next = next;
		else
			pendingDeletes = next;
		pfree(pending);
		/* prev 不变 */
	}
	else
	{
		/* 不相关的条目，不要碰它 */
			prev = pending;
		}
	}
}

/*
 * RelationTruncate
 *		将关系物理截断到指定的块数。
 *
 * 这包括丢弃那些将被丢弃的块所对应的所有缓冲区。
 */
void
RelationTruncate(Relation rel, BlockNumber nblocks)
{
	bool		fsm;
	bool		vm;
	bool		need_fsm_vacuum = false;
	ForkNumber	forks[MAX_FORKNUM];
	BlockNumber old_blocks[MAX_FORKNUM];
	BlockNumber blocks[MAX_FORKNUM];
	int			nforks = 0;
	SMgrRelation reln;

	/*
	 * 确保 smgr_targblock 等没有指向新末尾之后的位置。
	 * （注意：在这个循环之后不要依赖这个 reln 指针。）
	 */
	reln = RelationGetSmgr(rel);
	reln->smgr_targblock = InvalidBlockNumber;
	for (int i = 0; i <= MAX_FORKNUM; ++i)
		reln->smgr_cached_nblocks[i] = InvalidBlockNumber;

	/* 准备截断关系的主分支（MAIN fork） */
	forks[nforks] = MAIN_FORKNUM;
	old_blocks[nforks] = smgrnblocks(reln, MAIN_FORKNUM);
	blocks[nforks] = nblocks;
	nforks++;

	/* 准备截断空闲空间映射（FSM）（如果存在的话） */
	fsm = smgrexists(RelationGetSmgr(rel), FSM_FORKNUM);
	if (fsm)
	{
		blocks[nforks] = FreeSpaceMapPrepareTruncateRel(rel, nblocks);
		if (BlockNumberIsValid(blocks[nforks]))
		{
			forks[nforks] = FSM_FORKNUM;
			old_blocks[nforks] = smgrnblocks(reln, FSM_FORKNUM);
			nforks++;
			need_fsm_vacuum = true;
		}
	}

	/* 如果存在的话，也准备截断可见性映射（visibility map） */
	vm = smgrexists(RelationGetSmgr(rel), VISIBILITYMAP_FORKNUM);
	if (vm)
	{
		blocks[nforks] = visibilitymap_prepare_truncate(rel, nblocks);
		if (BlockNumberIsValid(blocks[nforks]))
		{
			forks[nforks] = VISIBILITYMAP_FORKNUM;
			old_blocks[nforks] = smgrnblocks(reln, VISIBILITYMAP_FORKNUM);
			nforks++;
		}
	}

	RelationPreTruncate(rel);

	/*
	 * 接下来的代码会以两种独立的方式与并发的检查点发生交互。
	 *
	 * 首先，截断操作可能会丢弃那些本该由检查点刷出的缓冲区。如果确实如此，
	 * 那么文件必须在检查点记录写入之前真正在磁盘上被截断。否则，如果从重
	 * 放从该检查点开始，那些待截断的块可能仍然存在于磁盘上，但其内容比
	 * 预期的更旧，这可能导致重放失败。这些块在磁盘上完全不存在是可以的，
	 * 但它们具有错误的内容则不行。因此，我们需要在执行这段代码时设置
	 * DELAY_CHKPT_COMPLETE。
	 *
	 * 其次，下面调用的 smgrtruncate() 又会转而调用 RegisterSyncRequest()。
	 * 我们需要该调用所创建的同步请求在检查点完成之前被处理。CheckPointGuts()
	 * 会调用 ProcessSyncRequests()，但如果我们是在那之后才注册我们的同步
	 * 请求，那么截断的 WAL 记录最终可能会排在检查点记录之前，而真正的同步
	 * 直到下一个检查点才发生。为了防止这种情况，我们需要在这里设置
	 * DELAY_CHKPT_START。这样，如果 XLOG_SMGR_TRUNCATE 排在并发检查点的
	 * 重做指针之前，我们就能保证相应的同步请求会在检查点完成之前被处理。
	 */
	Assert((MyProc->delayChkptFlags & (DELAY_CHKPT_START | DELAY_CHKPT_COMPLETE)) == 0);
	MyProc->delayChkptFlags |= DELAY_CHKPT_START | DELAY_CHKPT_COMPLETE;

	/*
	 * 我们先把截断操作写入 WAL 日志，然后在一个临界区中执行截断。截断会丢弃
	 * 缓冲区（即使是脏的），然后截断磁盘文件。所有这些工作都需要在锁释放
	 * 之前完成，否则磁盘上那些缺少最近修改的旧版本页面将再次变得可访问。
	 * 如果我们发生 panic，会在崩溃恢复中重试整个操作，但即便如此我们也不能
	 * 放弃，因为我们不希望备库的关系统计大小出现分歧，从而破坏下游的重放或
	 * 可见性不变式。临界区还会抑制中断。
	 *
	 * （如果修改这段代码，另请参见 visibilitymap.c。）
	 */
	START_CRIT_SECTION();

	if (RelationNeedsWAL(rel))
	{
		/*
		 * 写入一条报告文件截断的 XLOG 条目。
		 */
		XLogRecPtr	lsn;
		xl_smgr_truncate xlrec;

		xlrec.blkno = nblocks;
		xlrec.rlocator = rel->rd_locator;
		xlrec.flags = SMGR_TRUNCATE_ALL;

		XLogBeginInsert();
		XLogRegisterData(&xlrec, sizeof(xlrec));

		lsn = XLogInsert(RM_SMGR_ID,
						 XLOG_SMGR_TRUNCATE | XLR_SPECIAL_REL_UPDATE);

		/*
		 * 强制刷出，因为否则主关系的截断可能会比 WAL 记录以及 FSM 或可见性
		 * 映射的截断更早落到磁盘上。如果我们在这个窗口期间崩溃，就会留下一个
		 * 已被截断的堆，但 FSM 或可见性映射中仍然包含那些已不存在的堆页面的
		 * 条目，并且备库也永远不会重放这次截断。
		 */
		XLogFlush(lsn);
	}

	/*
	 * 这会首先从缓冲池中移除那些在截断完成后不应再存在的缓冲区，
	 * 然后截断磁盘上相应的文件。
	 */
	smgrtruncate(RelationGetSmgr(rel), forks, nforks, old_blocks, blocks);

	END_CRIT_SECTION();

	/* 我们已经完成了所有关键工作，因此现在检查点可以正常进行了。 */
	MyProc->delayChkptFlags &= ~(DELAY_CHKPT_START | DELAY_CHKPT_COMPLETE);

	/*
	 * 更新上层的 FSM 页面以反映这次截断。这一点很重要，因为刚刚被截断的
	 * 页面很可能被标记为全部空闲，从而会被优先选中。
	 *
	 * 注意：推迟检查点直到这一步完成是没有意义的。因为 FSM 不会被写入
	 * WAL 日志，我们无论如何都必须准备好应对崩溃后可能发生损坏的情况。
	 */
	if (need_fsm_vacuum)
		FreeSpaceMapVacuumRange(rel, nblocks, InvalidBlockNumber);
}

/*
 * RelationPreTruncate
 *		在物理截断之前执行与访问方法无关的工作。
 *
 * 如果某个访问方法的 relation_nontransactional_truncate 不调用
 * RelationTruncate()，那么它必须在缩小表大小之前调用本函数。
 */
void
RelationPreTruncate(Relation rel)
{
	PendingRelSync *pending;

	if (!pendingSyncHash)
		return;

	pending = hash_search(pendingSyncHash,
						  &(RelationGetSmgr(rel)->smgr_rlocator.locator),
						  HASH_FIND, NULL);
	if (pending)
		pending->is_truncated = true;
}

/*
 * 逐块地复制一个分支的数据。
 *
 * 注意，这要求共享缓冲区中没有脏数据。如果可能存在脏数据，调用方需要
 * 使用例如 FlushRelationBuffers(rel) 将它们刷出。
 *
 * 另请注意，本函数经常通过诸如
 *		RelationCopyStorage(RelationGetSmgr(rel), ...);
 * 这样的形式被调用；这之所以安全，仅仅是因为我们在这里只执行 smgr 和
 * WAL 操作。如果我们调用了任何其他东西，一次 relcache 刷新就可能使我们的
 * SMgrRelation 参数变成一个悬空指针。
 */
void
RelationCopyStorage(SMgrRelation src, SMgrRelation dst,
					ForkNumber forkNum, char relpersistence)
{
	bool		use_wal;
	bool		copying_initfork;
	BlockNumber nblocks;
	BlockNumber blkno;
	BulkWriteState *bulkstate;

	/*
	 * 未日志记录（unlogged）关系的初始化分支在很多方面都不得不被当作普通
	 * 关系一样对待：其修改需要写入 WAL 日志，并且需要同步到磁盘。
	 */
	copying_initfork = relpersistence == RELPERSISTENCE_UNLOGGED &&
		forkNum == INIT_FORKNUM;

	/*
	 * 当且仅当启用了 WAL 归档/流复制、并且它是一个永久关系时，我们才需要
	 * 将复制的数据写入 WAL 日志。这与
	 * "RelationNeedsWAL(rel) || copying_initfork" 的结果相同，因为我们知道
	 * 当前操作创建了新的关系存储。
	 */
	use_wal = XLogIsNeeded() &&
		(relpersistence == RELPERSISTENCE_PERMANENT || copying_initfork);

	bulkstate = smgr_bulk_start_smgr(dst, forkNum, use_wal);

	nblocks = smgrnblocks(src, forkNum);

	for (blkno = 0; blkno < nblocks; blkno++)
	{
		BulkWriteBuffer buf;
		int			piv_flags;
		bool		checksum_failure;
		bool		verified;

		/* 如果在复制数据期间收到了取消信号，则退出 */
		CHECK_FOR_INTERRUPTS();

		buf = smgr_bulk_get_buf(bulkstate);
		smgrread(src, forkNum, blkno, (Page) buf);

		piv_flags = PIV_LOG_WARNING;
		if (ignore_checksum_failure)
			piv_flags |= PIV_IGNORE_CHECKSUM_FAILURE;
		verified = PageIsVerified((Page) buf, blkno, piv_flags,
								  &checksum_failure);
		if (checksum_failure)
		{
			RelFileLocatorBackend rloc = src->smgr_rlocator;

			pgstat_prepare_report_checksum_failure(rloc.locator.dbOid);
			pgstat_report_checksum_failures_in_db(rloc.locator.dbOid, 1);
		}

		if (!verified)
		{
			/*
			 * 出于谨慎起见，在调用 ereport 机制之前先捕获文件路径。这可以防止
			 * 因例如一个 errcontext 回调而导致 relcache 刷新的可能性。
			 * （errcontext 回调本不应冒这种风险，但众所周知人们有时会忘记这条
			 * 规则。）
			 */
			RelPathStr	relpath = relpathbackend(src->smgr_rlocator.locator,
												 src->smgr_rlocator.backend,
												 forkNum);

			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid page in block %u of relation \"%s\"",
							blkno, relpath.str)));
		}

		/*
		 * 将该页排入队列，以便写入 WAL 日志并写出。遗憾的是我们不知道这是
		 * 哪种类型的页面，因此必须记录整页，包括任何未使用的空间。
		 */
		smgr_bulk_write(bulkstate, blkno, buf, false);
	}
	smgr_bulk_finish(bulkstate);
}

/*
 * RelFileLocatorSkippingWAL
 *		检查一个 BM_PERMANENT 的 relfilelocator 是否正在跳过 WAL。
 *
 * 对某些关系的修改不能写入 WAL；详见
 * src/backend/access/transam/README 中的 "Skipping WAL for New
 * RelFileLocator"。虽然从 Relation 可以高效地得知这一点，但本函数
 * 是为那些无法访问 Relation 的代码路径准备的。
 */
bool
RelFileLocatorSkippingWAL(RelFileLocator rlocator)
{
	if (!pendingSyncHash ||
		hash_search(pendingSyncHash, &rlocator, HASH_FIND, NULL) == NULL)
		return false;

	return true;
}

/*
 * EstimatePendingSyncsSpace
 *		估计将同步操作传递给并行工作进程所需的空间。
 */
Size
EstimatePendingSyncsSpace(void)
{
	long		entries;

	entries = pendingSyncHash ? hash_get_num_entries(pendingSyncHash) : 0;
	return mul_size(1 + entries, sizeof(RelFileLocator));
}

/*
 * SerializePendingSyncs
 *		为并行工作进程序列化同步操作。
 */
void
SerializePendingSyncs(Size maxSize, char *startAddress)
{
	HTAB	   *tmphash;
	HASHCTL		ctl;
	HASH_SEQ_STATUS scan;
	PendingRelSync *sync;
	PendingRelDelete *delete;
	RelFileLocator *src;
	RelFileLocator *dest = (RelFileLocator *) startAddress;

	if (!pendingSyncHash)
		goto terminate;

	/* 创建临时哈希表以收集活跃的 relfilelocator */
	ctl.keysize = sizeof(RelFileLocator);
	ctl.entrysize = sizeof(RelFileLocator);
	ctl.hcxt = CurrentMemoryContext;
	tmphash = hash_create("tmp relfilelocators",
						  hash_get_num_entries(pendingSyncHash), &ctl,
						  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/* 从待同步项中收集所有 rlocator */
	hash_seq_init(&scan, pendingSyncHash);
	while ((sync = (PendingRelSync *) hash_seq_search(&scan)))
		(void) hash_search(tmphash, &sync->rlocator, HASH_ENTER, NULL);

	/* 移除已删除的 rnode */
	for (delete = pendingDeletes; delete != NULL; delete = delete->next)
		if (delete->atCommit)
			(void) hash_search(tmphash, &delete->rlocator,
							   HASH_REMOVE, NULL);

	hash_seq_init(&scan, tmphash);
	while ((src = (RelFileLocator *) hash_seq_search(&scan)))
		*dest++ = *src;

	hash_destroy(tmphash);

terminate:
	MemSet(dest, 0, sizeof(RelFileLocator));
}

/*
 * RestorePendingSyncs
 *		在并行工作进程内恢复同步操作。
 *
 * RelationNeedsWAL() 和 RelFileLocatorSkippingWAL() 必须向并行工作进程
 * 提供正确的答案。只有 smgrDoPendingSyncs() 会在事务结束时读取
 * is_truncated 字段。因此，不要恢复它。
 */
void
RestorePendingSyncs(char *startAddress)
{
	RelFileLocator *rlocator;

	Assert(pendingSyncHash == NULL);
	for (rlocator = (RelFileLocator *) startAddress; rlocator->relNumber != 0;
		 rlocator++)
		AddPendingSync(rlocator);
}

/*
 *	smgrDoPendingDeletes() -- 在事务结束时处理关系的删除。
 *
 * 在回滚子事务时也会运行本函数；我们希望立即清理一个失败了的子事务。
 *
 * 注意：有可能我们被要求移除一个在任何分支中都没有物理存储的关系。
 * 特别是，有可能我们正在清理一个旧的临时关系，而 RemovePgTempFiles
 * 已经回收了其物理存储。
 */
void
smgrDoPendingDeletes(bool isCommit)
{
	int			nestLevel = GetCurrentTransactionNestLevel();
	PendingRelDelete *pending;
	PendingRelDelete *prev;
	PendingRelDelete *next;
	int			nrels = 0,
				maxrels = 0;
	SMgrRelation *srels = NULL;

	prev = NULL;
	for (pending = pendingDeletes; pending != NULL; pending = next)
	{
		next = pending->next;
		if (pending->nestLevel < nestLevel)
		{
			/* 外层级别的条目不应被立即处理 */
			prev = pending;
		}
		else
		{
			/* 先解除列表条目的链接，这样在失败时我们不会重试 */
			if (prev)
				prev->next = next;
			else
				pendingDeletes = next;
			/* 如果需要则执行删除 */
			if (pending->atCommit == isCommit)
			{
				SMgrRelation srel;

				srel = smgropen(pending->rlocator, pending->procNumber);

				/* 分配初始数组，如果需要则扩展它 */
				if (maxrels == 0)
				{
					maxrels = 8;
					srels = palloc(sizeof(SMgrRelation) * maxrels);
				}
				else if (maxrels <= nrels)
				{
					maxrels *= 2;
					srels = repalloc(srels, sizeof(SMgrRelation) * maxrels);
				}

				srels[nrels++] = srel;
			}
			/* 必须显式地释放列表条目 */
			pfree(pending);
			/* prev 不变 */
		}
	}

	if (nrels > 0)
	{
		smgrdounlinkall(srels, nrels, false);

		for (int i = 0; i < nrels; i++)
			smgrclose(srels[i]);

		pfree(srels);
	}
}

/*
 *	smgrDoPendingSyncs() -- 在事务结束时处理关系的同步。
 */
void
smgrDoPendingSyncs(bool isCommit, bool isParallelWorker)
{
	PendingRelDelete *pending;
	int			nrels = 0,
				maxrels = 0;
	SMgrRelation *srels = NULL;
	HASH_SEQ_STATUS scan;
	PendingRelSync *pendingsync;

	Assert(GetCurrentTransactionNestLevel() == 1);

	if (!pendingSyncHash)
		return;					/* 没有需要同步的关系 */

	/* 中止 -- 直接丢弃所有待同步项 */
	if (!isCommit)
	{
		pendingSyncHash = NULL;
		return;
	}

	AssertPendingSyncs_RelationCache();

	/* 并行工作进程 -- 直接丢弃所有待同步项 */
	if (isParallelWorker)
	{
		pendingSyncHash = NULL;
		return;
	}

	/* 跳过那些 smgrDoPendingDeletes() 将要删除的节点。 */
	for (pending = pendingDeletes; pending != NULL; pending = pending->next)
		if (pending->atCommit)
			(void) hash_search(pendingSyncHash, &pending->rlocator,
							   HASH_REMOVE, NULL);

	hash_seq_init(&scan, pendingSyncHash);
	while ((pendingsync = (PendingRelSync *) hash_seq_search(&scan)))
	{
		ForkNumber	fork;
		BlockNumber nblocks[MAX_FORKNUM + 1];
		uint64		total_blocks = 0;
		SMgrRelation srel;

		srel = smgropen(pendingsync->rlocator, INVALID_PROC_NUMBER);

		/*
		 * 对于较小的关系，我们会发出 newpage 的 WAL 记录。
		 *
		 * 较小的 WAL 记录有机会与其他后端的 WAL 记录一起被刷出。对于那些
		 * 小于某个阈值（由 GUC wal_skip_threshold 定义）的文件，我们会发出
		 * WAL 记录而不是执行同步，以期获得更快的提交。
		 */
		if (!pendingsync->is_truncated)
		{
			for (fork = 0; fork <= MAX_FORKNUM; fork++)
			{
				if (smgrexists(srel, fork))
				{
					BlockNumber n = smgrnblocks(srel, fork);

				/* 对于未日志记录的关系，我们不应走到这里 */
				Assert(fork != INIT_FORKNUM);
					nblocks[fork] = n;
					total_blocks += n;
				}
				else
					nblocks[fork] = InvalidBlockNumber;
			}
		}

		/*
		 * 同步文件，或者为其内容发出 WAL 记录。
		 *
		 * 尽管当文件足够小时我们会发出 WAL 记录，但如果该文件经历过截断，
		 * 则无论其大小如何都要执行文件同步。这是因为，如果在过去一个更长的
		 * 文件已经被刷出的情况下，我们省略了文件的同步写出而改为发出 WAL，
		 * 那么在崩溃恢复之后该文件后面可能会跟有残留的垃圾块。你可能会认为，
		 * 如果当前的主分支比以往任何时候都长，我们就可以选择 WAL；但也存在
		 * 主分支比以往更长、而 FSM 分支却变短的情况。
		 */
		if (pendingsync->is_truncated ||
			total_blocks >= wal_skip_threshold * (uint64) 1024 / BLCKSZ)
		{
			/* 分配初始数组，如果需要则扩展它 */
			if (maxrels == 0)
			{
				maxrels = 8;
				srels = palloc(sizeof(SMgrRelation) * maxrels);
			}
			else if (maxrels <= nrels)
			{
				maxrels *= 2;
				srels = repalloc(srels, sizeof(SMgrRelation) * maxrels);
			}

			srels[nrels++] = srel;
		}
		else
		{
			/* 为所有块发出 WAL 记录。文件足够小。 */
			for (fork = 0; fork <= MAX_FORKNUM; fork++)
			{
				int			n = nblocks[fork];
				Relation	rel;

				if (!BlockNumberIsValid(n))
					continue;

				/*
				 * 为整个文件发出 WAL。遗憾的是我们不知道这是哪种类型的页面，
				 * 因此必须记录整页，包括任何未使用的空间。ReadBufferExtended()
				 * 会计入一些 pgstat 事件；遗憾的是，我们丢弃了它们。
				 */
				rel = CreateFakeRelcacheEntry(srel->smgr_rlocator.locator);
				log_newpage_range(rel, fork, 0, n, false);
				FreeFakeRelcacheEntry(rel);
			}
		}
	}

	pendingSyncHash = NULL;

	if (nrels > 0)
	{
		smgrdosyncall(srels, nrels);
		pfree(srels);
	}
}

/*
 * smgrGetPendingDeletes() -- 获取一个待删除的非临时关系的列表。
 *
 * 返回值是被安排终止的关系的个数。*ptr 会被设置为指向一个
 * 新分配的 RelFileLocator 数组。如果没有待删除的关系，*ptr 会被设置为 NULL。
 *
 * 返回的列表只包含非临时关系。这样做是可以的，因为该列表只在临时关系
 * 无关紧要的上下文中使用：我们要么正在写入两阶段状态文件（而触碰过
 * 临时表的事务无法被准备），要么正在写入 xlog（并且无论如何，如果
 * 我们重启，所有临时文件都会被清除，因此不需要重做也去做这件事）。
 *
 * 注意，该列表不包含任何由上层事务安排终止的关系。
 */
int
smgrGetPendingDeletes(bool forCommit, RelFileLocator **ptr)
{
	int			nestLevel = GetCurrentTransactionNestLevel();
	int			nrels;
	RelFileLocator *rptr;
	PendingRelDelete *pending;

	nrels = 0;
	for (pending = pendingDeletes; pending != NULL; pending = pending->next)
	{
		if (pending->nestLevel >= nestLevel && pending->atCommit == forCommit
			&& pending->procNumber == INVALID_PROC_NUMBER)
			nrels++;
	}
	if (nrels == 0)
	{
		*ptr = NULL;
		return 0;
	}
	rptr = (RelFileLocator *) palloc(nrels * sizeof(RelFileLocator));
	*ptr = rptr;
	for (pending = pendingDeletes; pending != NULL; pending = pending->next)
	{
		if (pending->nestLevel >= nestLevel && pending->atCommit == forCommit
			&& pending->procNumber == INVALID_PROC_NUMBER)
		{
			*rptr = pending->rlocator;
			rptr++;
		}
	}
	return nrels;
}

/*
 *	PostPrepare_smgr -- 在一次成功的 PREPARE 之后进行清理
 *
 * 我们在这里要做的是丢弃关于待删除关系的、位于内存中的状态。这些状态
 * 全部已经被记录到了 2PC 状态文件中，因此 smgr 不再需要为此操心。
 */
void
PostPrepare_smgr(void)
{
	PendingRelDelete *pending;
	PendingRelDelete *next;

	for (pending = pendingDeletes; pending != NULL; pending = next)
	{
		next = pending->next;
		pendingDeletes = next;
		/* 必须显式地释放列表条目 */
		pfree(pending);
	}
}


/*
 * AtSubCommit_smgr() --- 处理子事务的提交。
 *
 * 将待删除列表中的所有条目重新指派给父事务。
 */
void
AtSubCommit_smgr(void)
{
	int			nestLevel = GetCurrentTransactionNestLevel();
	PendingRelDelete *pending;

	for (pending = pendingDeletes; pending != NULL; pending = pending->next)
	{
		if (pending->nestLevel >= nestLevel)
			pending->nestLevel = nestLevel - 1;
	}
}

/*
 * AtSubAbort_smgr() --- 处理子事务的中止。
 *
 * 删除已创建的关系，并忘掉已删除的关系。我们可以立即执行这些操作，
 * 因为我们知道这个子事务不会提交。
 */
void
AtSubAbort_smgr(void)
{
	smgrDoPendingDeletes(false);
}

void
smgr_redo(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	/* smgr 记录中不使用备份块 */
	Assert(!XLogRecHasAnyBlockRefs(record));

	if (info == XLOG_SMGR_CREATE)
	{
		xl_smgr_create *xlrec = (xl_smgr_create *) XLogRecGetData(record);
		SMgrRelation reln;

		reln = smgropen(xlrec->rlocator, INVALID_PROC_NUMBER);
		smgrcreate(reln, xlrec->forkNum, true);
	}
	else if (info == XLOG_SMGR_TRUNCATE)
	{
		xl_smgr_truncate *xlrec = (xl_smgr_truncate *) XLogRecGetData(record);
		SMgrRelation reln;
		Relation	rel;
		ForkNumber	forks[MAX_FORKNUM];
		BlockNumber blocks[MAX_FORKNUM];
		BlockNumber old_blocks[MAX_FORKNUM];
		int			nforks = 0;
		bool		need_fsm_vacuum = false;

		reln = smgropen(xlrec->rlocator, INVALID_PROC_NUMBER);

		/*
		 * 如果该关系不存在，则强制创建它（这暗示它在 WAL 序列中更靠后的
		 * 位置被删除了）。与 XLogReadBufferForRedo 中一样，我们倾向于
		 * 重新创建该关系，并尽可能好地重放日志，直到看到删除操作。
		 */
		smgrcreate(reln, MAIN_FORKNUM, true);

		/*
		 * 在执行截断之前，先更新最小恢复点以覆盖这条 WAL 记录。一旦关系被
		 * 截断，就再也没有回头路了。缓冲区管理器对关系文件的常规更新强制
		 * 执行“WAL 优先”规则，从而确保最小恢复点总是在数据文件中相应的
		 * 修改被刷到磁盘之前被更新。我们在这里必须手动做同样的事情。
		 *
		 * 在截断之前做这件事意味着，如果截断由于某种原因失败，即使在重启
		 * 之后你也无法启动系统，直到你修复底层状况使截断能够成功为止。
		 * 作为替代方案，我们也可以在截断之后更新最小恢复点，但那样就会
		 * 留下一个“WAL 优先”规则可能被违反的小窗口。
		 */
		XLogFlush(lsn);

		/* 准备截断主分支（MAIN fork） */
		if ((xlrec->flags & SMGR_TRUNCATE_HEAP) != 0)
		{
			forks[nforks] = MAIN_FORKNUM;
			old_blocks[nforks] = smgrnblocks(reln, MAIN_FORKNUM);
			blocks[nforks] = xlrec->blkno;
			nforks++;

			/* 同时告知 xlogutils.c */
			XLogTruncateRelation(xlrec->rlocator, MAIN_FORKNUM, xlrec->blkno);
		}

		/* 也准备截断 FSM 和 VM */
		rel = CreateFakeRelcacheEntry(xlrec->rlocator);

		if ((xlrec->flags & SMGR_TRUNCATE_FSM) != 0 &&
			smgrexists(reln, FSM_FORKNUM))
		{
			blocks[nforks] = FreeSpaceMapPrepareTruncateRel(rel, xlrec->blkno);
			if (BlockNumberIsValid(blocks[nforks]))
			{
				forks[nforks] = FSM_FORKNUM;
				old_blocks[nforks] = smgrnblocks(reln, FSM_FORKNUM);
				nforks++;
				need_fsm_vacuum = true;
			}
		}
		if ((xlrec->flags & SMGR_TRUNCATE_VM) != 0 &&
			smgrexists(reln, VISIBILITYMAP_FORKNUM))
		{
			blocks[nforks] = visibilitymap_prepare_truncate(rel, xlrec->blkno);
			if (BlockNumberIsValid(blocks[nforks]))
			{
				forks[nforks] = VISIBILITYMAP_FORKNUM;
				old_blocks[nforks] = smgrnblocks(reln, VISIBILITYMAP_FORKNUM);
				nforks++;
			}
		}

		/* 执行真正的工作以截断关系的各个分支 */
		if (nforks > 0)
		{
			START_CRIT_SECTION();
			smgrtruncate(reln, forks, nforks, old_blocks, blocks);
			END_CRIT_SECTION();
		}

		/*
		 * 更新上层的 FSM 页面以反映这次截断。这一点很重要，因为刚刚被截断的
		 * 页面很可能被标记为全部空闲，从而会被优先选中。
		 */
		if (need_fsm_vacuum)
			FreeSpaceMapVacuumRange(rel, xlrec->blkno,
									InvalidBlockNumber);

		FreeFakeRelcacheEntry(rel);
	}
	else
		elog(PANIC, "smgr_redo: unknown op code %u", info);
}
