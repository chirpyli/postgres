/*-------------------------------------------------------------------------
 *
 * bulk_write.c
 *	  高效且可靠地填充一个新关系
 *
 * 这里假设在加载关系期间没有其他后端会访问它，因此我们可以走一些捷径。
 * 批量写操作启动时所指定的 fork 中已有的页面不会被修改，除非被显式写入。
 * 不要将经由常规缓冲管理器的操作与批量加载接口混用！
 *
 * 我们绕过缓冲管理器以避免加锁开销，直接调用 smgrextend()。一个缺点是
 * 这些页面在构建完成后首次使用时需要重新读入共享缓冲区。对于大关系来说，
 * 这通常是一个不错的权衡；而对于小关系，其开销相对于最初创建关系而言
 * 也并不十分显著。
 *
 * 这些页面会在需要时写入 WAL。为了节省 WAL 头部的开销，我们会将若干页面
 * 合并到一条记录中写入 WAL。
 *
 * 一个棘手之处在于：因为我们绕过了缓冲管理器，所以需要自己把关系注册到
 * 下一次检查点进行 fsync，并确保即使有检查点并发发生，关系也能被我们或
 * 检查点进程正确地 fsync。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/smgr/bulk_write.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xloginsert.h"
#include "access/xlogrecord.h"
#include "storage/bufpage.h"
#include "storage/bulk_write.h"
#include "storage/proc.h"
#include "storage/smgr.h"
#include "utils/rel.h"

#define MAX_PENDING_WRITES XLR_MAX_BLOCK_ID

static const PGIOAlignedBlock zero_buffer = {0};	/* 大小等于 BLCKSZ */

typedef struct PendingWrite
{
	BulkWriteBuffer buf;
	BlockNumber blkno;
	bool		page_std;
} PendingWrite;

/*
 * 单个关系 fork 的批量写入器状态。
 */
struct BulkWriteState
{
	/* 我们要写入的目标关系的相关信息 */
	SMgrRelation smgr;
	ForkNumber	forknum;
	bool		use_wal;

	/* 我们将若干次写入排队，并批量地写入 WAL */
	int			npending;
	PendingWrite pending_writes[MAX_PENDING_WRITES];

	/* 关系当前的大小 */
	BlockNumber relsize;

	/* 批量操作启动时刻的 RedoRecPtr */
	XLogRecPtr	start_RedoRecPtr;

	MemoryContext memcxt;
};

static void smgr_bulk_flush(BulkWriteState *bulkstate);

/*
 * 在某个关系 fork 上启动批量写操作。
 */
BulkWriteState *
smgr_bulk_start_rel(Relation rel, ForkNumber forknum)
{
	return smgr_bulk_start_smgr(RelationGetSmgr(rel),
								forknum,
								RelationNeedsWAL(rel) || forknum == INIT_FORKNUM);
}

/*
 * 在某个关系 fork 上启动批量写操作。
 *
 * 这与 smgr_bulk_start_rel 类似，但可以在没有 relcache 条目的情况下使用。
 */
BulkWriteState *
smgr_bulk_start_smgr(SMgrRelation smgr, ForkNumber forknum, bool use_wal)
{
	BulkWriteState *state;

	state = palloc(sizeof(BulkWriteState));
	state->smgr = smgr;
	state->forknum = forknum;
	state->use_wal = use_wal;

	state->npending = 0;
	state->relsize = smgrnblocks(smgr, forknum);

	state->start_RedoRecPtr = GetRedoRecPtr();

	/*
	 * 记录内存上下文。后续我们将用它来分配所有的缓冲区。
	 */
	state->memcxt = CurrentMemoryContext;

	return state;
}

/*
 * 结束批量写操作。
 *
 * 这会将其余所有待写入的页面写入 WAL 并刷新到磁盘，并在需要时 fsync 关系。
 */
void
smgr_bulk_finish(BulkWriteState *bulkstate)
{
	/* 将剩余的页面写入 WAL 并刷新 */
	smgr_bulk_flush(bulkstate);

	/*
	 * 如有必要，对关系进行 fsync，或将其注册到下一次检查点。
	 */
	if (SmgrIsTemp(bulkstate->smgr))
	{
		/* 临时关系永远不需要 fsync */
	}
	else if (!bulkstate->use_wal)
	{
		/*----------
		 * 这可能是未记录日志（unlogged）的关系，也可能是永久关系，但我们因
		 * wal_level=minimal 而跳过了 WAL 写入：
		 *
		 * A) 未记录日志的关系
		 *
		 *    未记录日志的关系在崩溃后会消失，但它们在干净关闭时仍需要被
		 *    fsync。调用 smgrregistersync() 即可，它能确保检查点进程会在
		 *    关闭检查点将其刷新。（在下次在线检查点也会刷新它，但这并非
		 *    严格必要。）
		 *
		 *    注意，就我们的目的而言，未记录日志关系的 init fork 不被视作
		 *    未记录日志，而是当作普通的永久关系处理。调用方会为 init fork
		 *    传入 use_wal=true。
		 *
		 * B) 永久关系，因 wal_level=minimal 而跳过了 WAL 写入
		 *
		 *    这是一个新关系，我们在写入时没有将页面写入 WAL，但这些页面
		 *    在提交前需要被 fsync。
		 *
		 *    不过我们不需要在这里完成此事。fsync() 会在提交时由
		 *    smgrDoPendingSyncs() 完成 (*)。
		 *
		 *    (*) 如果关系非常小，smgrDoPendingSyncs() 可能决定在提交时把
		 *    整个关系写入 WAL，而不是去 fsync 它，但无论如何这都是
		 *    smgrDoPendingSyncs() 的职责。
		 *
		 * 这里我们无法区分这两种情况，因此保守地假定它是未记录日志的关系。
		 * 而 wal_level=minimal 的永久关系其实无需任何操作，见上文。
		 */
		smgrregistersync(bulkstate->smgr, bulkstate->forknum);
	}
	else
	{
		/*
		 * 永久关系，正常地写入了 WAL。
		 *
		 * 我们已经将所有页面写入了 WAL，因此在崩溃时会从 WAL 重放。然而，
		 * 在我们写出这些页面时，传入了 skipFsync=true，以避免把所有的写操作
		 * 都向检查点进程注册的额外开销。现在把整个关系注册一次。
		 *
		 * 这个思路有一个漏洞：如果在我们写页面的过程中发生了检查点，它已经
		 * 错过了对我们检查点启动之前所写页面的 fsync。之后若发生崩溃，会
		 * 从检查点开始重放 WAL，因此不会重放我们较早的 WAL 记录。所以如果
		 * 检查点在批量写之后才启动，现在就对文件进行 fsync。
		 */

		/*
		 * 防止检查点在 GetRedoRecPtr() 与 smgrregistersync() 调用之间启动。
		 */
		Assert((MyProc->delayChkptFlags & DELAY_CHKPT_START) == 0);
		MyProc->delayChkptFlags |= DELAY_CHKPT_START;

		if (bulkstate->start_RedoRecPtr != GetRedoRecPtr())
		{
			/*
			 * 发生了检查点，而它并不知晓我们的写入，因此由我们自己
			 * 对关系进行 fsync()。
			 */
			MyProc->delayChkptFlags &= ~DELAY_CHKPT_START;
			smgrimmedsync(bulkstate->smgr, bulkstate->forknum);
			elog(DEBUG1, "flushed relation because a checkpoint occurred concurrently");
		}
		else
		{
			smgrregistersync(bulkstate->smgr, bulkstate->forknum);
			MyProc->delayChkptFlags &= ~DELAY_CHKPT_START;
		}
	}
}

static int
buffer_cmp(const void *a, const void *b)
{
	const PendingWrite *bufa = (const PendingWrite *) a;
	const PendingWrite *bufb = (const PendingWrite *) b;

	/* 我们不应看到同一个块的重复写入 */
	Assert(bufa->blkno != bufb->blkno);
	if (bufa->blkno > bufb->blkno)
		return 1;
	else
		return -1;
}

/*
 * 完成所有待处理的写入。
 */
static void
smgr_bulk_flush(BulkWriteState *bulkstate)
{
	int			npending = bulkstate->npending;
	PendingWrite *pending_writes = bulkstate->pending_writes;

	if (npending == 0)
		return;

	if (npending > 1)
		qsort(pending_writes, npending, sizeof(PendingWrite), buffer_cmp);

	if (bulkstate->use_wal)
	{
		BlockNumber blknos[MAX_PENDING_WRITES];
		Page		pages[MAX_PENDING_WRITES];
		bool		page_std = true;

		for (int i = 0; i < npending; i++)
		{
			blknos[i] = pending_writes[i].blkno;
			pages[i] = pending_writes[i].buf->data;

			/*
			 * 如果其中任一页面使用了 !page_std，我们就把它们全部按
			 * 非标准页面记录。这有些浪费，但实际上标准与非标准页面布局
			 * 混合的情况很罕见，所有内建的访问方法都不会这样做。
			 */
			if (!pending_writes[i].page_std)
				page_std = false;
		}
		log_newpages(&bulkstate->smgr->smgr_rlocator.locator, bulkstate->forknum,
					 npending, blknos, pages, page_std);
	}

	for (int i = 0; i < npending; i++)
	{
		BlockNumber blkno = pending_writes[i].blkno;
		Page		page = pending_writes[i].buf->data;

		PageSetChecksumInplace(page, blkno);

		if (blkno >= bulkstate->relsize)
		{
			/*
			 * 如果我们必须以非顺序的方式写页面，就用零把中间的空间填充满，
			 * 直到回过头来覆盖。在标准的 Unix 文件系统上，这在逻辑上并非
			 * 必要（未写入的空间读出来本来就是零），但这有助于避免产生
			 * 文件碎片。不过这些填充用的空页面不会被写入 WAL。
			 */
			while (blkno > bulkstate->relsize)
			{
				/* 全零页面不设校验和 */
				smgrextend(bulkstate->smgr, bulkstate->forknum,
						   bulkstate->relsize,
						   &zero_buffer,
						   true);
				bulkstate->relsize++;
			}

			smgrextend(bulkstate->smgr, bulkstate->forknum, blkno, page, true);
			bulkstate->relsize++;
		}
		else
			smgrwrite(bulkstate->smgr, bulkstate->forknum, blkno, page, true);
		pfree(page);
	}

	bulkstate->npending = 0;
}

/*
 * 将 'buf' 的写入排入队列。
 *
 * 注意：这会接管 'buf' 的所有权！
 *
 * 在一次批量写操作中，你只能写入某个给定块一次。
 */
void
smgr_bulk_write(BulkWriteState *bulkstate, BlockNumber blocknum, BulkWriteBuffer buf, bool page_std)
{
	PendingWrite *w;

	w = &bulkstate->pending_writes[bulkstate->npending++];
	w->buf = buf;
	w->blkno = blocknum;
	w->page_std = page_std;

	if (bulkstate->npending == MAX_PENDING_WRITES)
		smgr_bulk_flush(bulkstate);
}

/*
 * 分配一个新的缓冲区，之后可通过 smgr_bulk_write() 写入。
 *
 * 没有用于释放缓冲区的函数。当你把它传给 smgr_bulk_write() 时，它会接管
 * 其所有权，并在不再需要时将其释放。
 *
 * 目前这只是简单地用 palloc 实现，但将来也可能改用环形缓冲区或更大的
 * 内存块来实现，因此不要依赖当前的实现方式。
 */
BulkWriteBuffer
smgr_bulk_get_buf(BulkWriteState *bulkstate)
{
	return MemoryContextAllocAligned(bulkstate->memcxt, BLCKSZ, PG_IO_ALIGN_SIZE, 0);
}
