/*-------------------------------------------------------------------------
 *
 * sync.c
 *	  文件同步管理代码。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/sync/sync.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>

#include "access/clog.h"
#include "access/commit_ts.h"
#include "access/multixact.h"
#include "access/xlog.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "portability/instr_time.h"
#include "postmaster/bgwriter.h"
#include "storage/fd.h"
#include "storage/latch.h"
#include "storage/md.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

/*
 * 在某些场景下（目前是独立后端进程和 checkpointer），我们会跟踪挂起的
 * fsync 操作：我们需要记住自上次检查点以来写入的所有关系段，
 * 以便在完成下一次检查点之前将它们 fsync 到磁盘。这个哈希表记录了
 * 挂起的操作。我们使用哈希表主要是作为一种合并重复请求的便利方式。
 *
 * 我们使用类似的机制来记住不再需要、可以在下一次检查点之后删除的文件，
 * 但这里使用链表而非哈希表，因为我们不期望会有重复的请求。
 *
 * 这些机制仅用于非临时关系；我们永远不会 fsync 临时关系，也不
 * 需要推迟它们的删除（见 mdunlink 中的注释）。
 *
 * （常规后端进程不在本地跟踪挂起的操作，而是将它们转发给 checkpointer。）
 */
typedef uint16 CycleCtr;		/* 可以是任意方便的整数大小 */

typedef struct
{
	FileTag		tag;			/* 标识处理程序和文件 */
	CycleCtr	cycle_ctr;		/* 最旧请求的 sync_cycle_ctr */
	bool		canceled;		/* 如果我们"最近"取消过则为 true */
} PendingFsyncEntry;

typedef struct
{
	FileTag		tag;			/* 标识处理程序和文件 */
	CycleCtr	cycle_ctr;		/* 发出请求时的 checkpoint_cycle_ctr */
	bool		canceled;		/* 如果请求已被取消则为 true */
} PendingUnlinkEntry;

static HTAB *pendingOps = NULL;
static List *pendingUnlinks = NIL;
static MemoryContext pendingOpsCxt; /* 上述结构所用的内存上下文 */

static CycleCtr sync_cycle_ctr = 0;
static CycleCtr checkpoint_cycle_ctr = 0;

/* 调用 AbsorbSyncRequests 的间隔 */
#define FSYNCS_PER_ABSORB		10
#define UNLINKS_PER_ABSORB		10

/*
 * 用于处理 sync 和 unlink 请求的函数指针。
 */
typedef struct SyncOps
{
	int			(*sync_syncfiletag) (const FileTag *ftag, char *path);
	int			(*sync_unlinkfiletag) (const FileTag *ftag, char *path);
	bool		(*sync_filetagmatches) (const FileTag *ftag,
										const FileTag *candidate);
} SyncOps;

/*
 * 这些索引必须与 SyncRequestHandler 枚举的取值相对应。
 */
static const SyncOps syncsw[] = {
	/* 磁盘 */
	[SYNC_HANDLER_MD] = {
		.sync_syncfiletag = mdsyncfiletag,
		.sync_unlinkfiletag = mdunlinkfiletag,
		.sync_filetagmatches = mdfiletagmatches
	},
	/* pg_xact */
	[SYNC_HANDLER_CLOG] = {
		.sync_syncfiletag = clogsyncfiletag
	},
	/* pg_commit_ts */
	[SYNC_HANDLER_COMMIT_TS] = {
		.sync_syncfiletag = committssyncfiletag
	},
	/* pg_multixact/offsets */
	[SYNC_HANDLER_MULTIXACT_OFFSET] = {
		.sync_syncfiletag = multixactoffsetssyncfiletag
	},
	/* pg_multixact/members */
	[SYNC_HANDLER_MULTIXACT_MEMBER] = {
		.sync_syncfiletag = multixactmemberssyncfiletag
	}
};

/*
 * 初始化用于文件同步跟踪的数据结构。
 */
void
InitSync(void)
{
	/*
	 * 如果需要，创建挂起操作哈希表。目前的情况是：当我们处于独立模式
	 * （未运行在 postmaster 之下）或是 checkpointer 辅助进程时需要它。
	 */
	if (!IsUnderPostmaster || AmCheckpointerProcess())
	{
		HASHCTL		hash_ctl;

		/*
		 * XXX：checkpointer 在吸收 fsync 请求时需要向挂起操作表中添加条目。
		 * 这是在临界区（critical section）中完成的，通常这是不允许的，
		 * 但我们做了一个例外。这意味着理论上存在在吸收 fsync 请求时
		 * 耗尽内存的可能性，从而导致 PANIC。幸运的是该哈希表很小，
		 * 因此在实践中不太可能发生这种情况。
		 */
		pendingOpsCxt = AllocSetContextCreate(TopMemoryContext,
											  "Pending ops context",
											  ALLOCSET_DEFAULT_SIZES);
		MemoryContextAllowInCriticalSection(pendingOpsCxt, true);

		hash_ctl.keysize = sizeof(FileTag);
		hash_ctl.entrysize = sizeof(PendingFsyncEntry);
		hash_ctl.hcxt = pendingOpsCxt;
		pendingOps = hash_create("Pending Ops Table",
								 100L,
								 &hash_ctl,
								 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		pendingUnlinks = NIL;
	}
}

/*
 * SyncPreCheckpoint() -- 执行检查点之前的工作
 *
 * 为了区分在本次检查点开始之前到达的 unlink 请求与在检查点期间到达的
 * 请求，我们使用一个类似于 fsync 请求所用的周期计数器。该周期计数器
 * 在此处递增。
 *
 * 必须在确定检查点的 REDO 点*之前*调用它。这能确保我们不会过早
 * 删除文件。由于这会调用执行内存分配的 AbsorbSyncRequests()，
 * 因此它不能在临界区中调用。
 *
 * 注意：我们不能在此处做任何依赖于"检查点将会完成"这一假设的操作。
 */
void
SyncPreCheckpoint(void)
{
	/*
	 * 诸如 DROP TABLESPACE 这样的操作假定下一次检查点会处理所有最近
	 * 转发的 unlink 请求，但如果这些请求没有在递增周期计数器之前被吸收，
	 * 它们就要等到未来的检查点才会被处理。下面的吸收操作确保任何在检查点
	 * 开始之前转发的 unlink 请求都将在当前检查点中被处理。
	 */
	AbsorbSyncRequests();

	/*
	 * 在此点之后到达的任何 unlink 请求将被分配下一个周期计数器，
	 * 并且要到下一次检查点才会被 unlink。
	 */
	checkpoint_cycle_ctr++;
}

/*
 * SyncPostCheckpoint() -- 执行检查点之后的工作
 *
 * 删除任何现在可以安全删除的遗留文件。
 */
void
SyncPostCheckpoint(void)
{
	int			absorb_counter;
	ListCell   *lc;

	absorb_counter = UNLINKS_PER_ABSORB;
	foreach(lc, pendingUnlinks)
	{
		PendingUnlinkEntry *entry = (PendingUnlinkEntry *) lfirst(lc);
		char		path[MAXPGPATH];

		/* 跳过任何已取消的条目 */
		if (entry->canceled)
			continue;

		/*
		 * 新条目会被追加到末尾，因此如果条目是新的，说明我们已经
		 * 到达了旧条目的末尾。
		 *
		 * 注意：如果恰好有连续若干个检查点失败，我们可能会因
		 * cycle_ctr 回绕而在此处被误导。然而，唯一的后果是我们将
		 * unlink 推迟一个额外的检查点，这是完全可以接受的。
		 */
		if (entry->cycle_ctr == checkpoint_cycle_ctr)
			break;

		/* 取消链接（删除）该文件 */
		if (syncsw[entry->tag.handler].sync_unlinkfiletag(&entry->tag,
														  path) < 0)
		{
			/*
			 * 这里存在一个竞态条件：当我们处理挂起的 unlink 请求时，
			 * 数据库可能恰好正在被删除。如果 DROP DATABASE 在我们之前
			 * 删除了该文件，我们在这里就会得到 ENOENT。rmtree() 也必须
			 * 忽略 ENOENT 错误，以应对我们先删除了该文件的可能性。
			 */
			if (errno != ENOENT)
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\": %m", path)));
		}

		/* 将该列表条目标记为已取消，以防万一 */
		entry->canceled = true;

		/*
		 * 与 ProcessSyncRequests 中一样，当要删除大量文件时，我们不希望
		 * 长时间停止吸收 fsync 请求。我们在此循环位置安全地调用
		 * AbsorbSyncRequests() 是可行的。
		 */
		if (--absorb_counter <= 0)
		{
			AbsorbSyncRequests();
			absorb_counter = UNLINKS_PER_ABSORB;
		}
	}

	/*
	 * 如果我们到达了列表末尾，可以直接删除整个列表
	 * （记得要 pfree 所有 PendingUnlinkEntry 对象）。否则，
	 * 我们必须保留 "lc" 处及其之后的条目。
	 */
	if (lc == NULL)
	{
		list_free_deep(pendingUnlinks);
		pendingUnlinks = NIL;
	}
	else
	{
		int			ntodelete = list_cell_number(pendingUnlinks, lc);

		for (int i = 0; i < ntodelete; i++)
			pfree(list_nth(pendingUnlinks, i));

		pendingUnlinks = list_delete_first_n(pendingUnlinks, ntodelete);
	}
}

/*
 *	ProcessSyncRequests() -- 处理排队的 fsync 请求。
 */
void
ProcessSyncRequests(void)
{
	static bool sync_in_progress = false;

	HASH_SEQ_STATUS hstat;
	PendingFsyncEntry *entry;
	int			absorb_counter;

	/* 关于同步耗时的统计 */
	int			processed = 0;
	instr_time	sync_start,
				sync_end,
				sync_diff;
	uint64		elapsed;
	uint64		longest = 0;
	uint64		total_elapsed = 0;

	/*
	 * 仅在检查点期间才会调用本函数，而检查点应当只发生在已创建
	 * pendingOps 的进程中。
	 */
	if (!pendingOps)
		elog(ERROR, "cannot sync without a pendingOps table");

	/*
	 * 如果我们处于 checkpointer 中，本次同步最好包含后端进程到此为止
	 * 排队的所有 fsync 请求。可能发生的最紧迫竞态条件是：一个必须为检查点
	 * 写入并 fsync 的缓冲区，可能在刚被 BufferSync() 访问之前被某个后端
	 * 进程刷出。我们知道后端进程会在清除缓冲区的脏位之前排队一个 fsync
	 * 请求，因此只要我们在完成 BufferSync() 之后执行一次 Absorb 就
	 * 是安全的。
	 */
	AbsorbSyncRequests();

	/*
	 * 为了避免过度的 fsync（最坏情况下，可能导致一个永不终止的检查点），
	 * 我们希望忽略在此点之后进入哈希表的 fsync 请求 —— 它们应在下一次
	 * 被处理。我们使用 sync_cycle_ctr 来区分新旧条目：新条目的 cycle_ctr
	 * 将等于递增后的 sync_cycle_ctr 值。
	 *
	 * 在正常情况下，此时表中存在的所有条目其 cycle_ctr 都恰好等于
	 * 当前的（即将变为旧的）sync_cycle_ctr 值。然而，如果我们在 fsync
	 * 循环的中途失败，那么当我们回到这里再次尝试时，可能仍残留有
	 * 较旧的 cycle_ctr 值。反复的检查点失败最终会使计数器回绕，以至于
	 * 一个旧条目可能看起来像是新的，导致我们跳过它，从而可能使一个
	 * 本不该成功的检查点得以成功。为了防止回绕，每当上一次的
	 * ProcessSyncRequests() 未能完成时，就遍历该表并强制设置
	 * cycle_ctr = sync_cycle_ctr。
	 *
	 * 不要试图将此循环与主循环合并，因为问题恰恰在于那个循环可能在
	 * 访问完所有条目之前就失败。从性能角度看这也无所谓，因为在
	 * 正常运行的系统中永远不会走到这条路径。
	 */
	if (sync_in_progress)
	{
		/* 先前的尝试失败，因此更新任何过期的 cycle_ctr 值 */
		hash_seq_init(&hstat, pendingOps);
		while ((entry = (PendingFsyncEntry *) hash_seq_search(&hstat)) != NULL)
		{
			entry->cycle_ctr = sync_cycle_ctr;
		}
	}

	/* 递增计数器，以使新的哈希表条目可区分 */
	sync_cycle_ctr++;

	/* 设置标志，以便在未能到达循环末尾时检测到失败 */
	sync_in_progress = true;

	/* 现在扫描哈希表，查找要处理的 fsync 请求 */
	absorb_counter = FSYNCS_PER_ABSORB;
	hash_seq_init(&hstat, pendingOps);
	while ((entry = (PendingFsyncEntry *) hash_seq_search(&hstat)) != NULL)
	{
		int			failures;

		/*
		 * 如果条目是新的，则本次不处理它；它是新的。
		 * 注意："continue" 会跳过循环底部的哈希删除调用。
		 */
		if (entry->cycle_ctr == sync_cycle_ctr)
			continue;

		/* 否则断言我们没有漏掉它 */
		Assert((CycleCtr) (entry->cycle_ctr + 1) == sync_cycle_ctr);

		/*
		 * 如果 fsync 已关闭，那么我们根本不必费心打开文件。
		 * （我们将检查延迟到此处，以便运行时动态切换 fsync 的行为是合理的。）
		 */
		if (enableFsync)
		{
			/*
			 * 如果在 checkpointer 中，我们希望时不时地吸收挂起的请求，
			 * 以防止 fsync 请求队列溢出。新添加的条目是否会被
			 * hash_seq_search 访问到是不确定的，但我们并不关心，
			 * 因为我们本来也不需要处理它们。
			 */
			if (--absorb_counter <= 0)
			{
				AbsorbSyncRequests();
				absorb_counter = FSYNCS_PER_ABSORB;
			}

			/*
			 * fsync 表中可能包含这样的请求：它们要 fsync 的段在我们处理
			 * 到它们时已经被删除（unlink）。与其仅仅寄希望于可以忽略
			 * ENOENT（或 Windows 上的 EACCES）错误，我们在出错时的做法是
			 * 吸收挂起的请求然后重试。由于 mdunlink() 在实际 unlink 之前
			 * 会排队一条"取消"消息，因此如果是这种情况，吸收之后该 fsync
			 * 请求保证会被标记为已取消。DROP DATABASE 同样必须在开始删除
			 * 之前通知我们忘掉 fsync 请求。
			 */
			for (failures = 0; !entry->canceled; failures++)
			{
				char		path[MAXPGPATH];

				INSTR_TIME_SET_CURRENT(sync_start);
				if (syncsw[entry->tag.handler].sync_syncfiletag(&entry->tag,
																path) == 0)
				{
					/* 成功；更新关于同步耗时的统计 */
					INSTR_TIME_SET_CURRENT(sync_end);
					sync_diff = sync_end;
					INSTR_TIME_SUBTRACT(sync_diff, sync_start);
					elapsed = INSTR_TIME_GET_MICROSEC(sync_diff);
					if (elapsed > longest)
						longest = elapsed;
					total_elapsed += elapsed;
					processed++;

					if (log_checkpoints)
						elog(DEBUG1, "checkpoint sync: number=%d file=%s time=%.3f ms",
							 processed,
							 path,
							 (double) elapsed / 1000);

					break;		/* out of retry loop */
				}

				/*
				 * 自 fsync 请求被登记以来，该关系可能已被删除或截断。
				 * 因此允许 ENOENT，但前提是我们之前没有在这个文件上失败过。
				 */
				if (!FILE_POSSIBLY_DELETED(errno) || failures > 0)
					ereport(data_sync_elevel(ERROR),
							(errcode_for_file_access(),
							 errmsg("could not fsync file \"%s\": %m",
									path)));
				else
					ereport(DEBUG1,
							(errcode_for_file_access(),
							 errmsg_internal("could not fsync file \"%s\" but retrying: %m",
											 path)));

				/*
				 * 吸收到达的请求，并检查是否有针对此关系 fork 的取消
				 * 消息到达。
				 */
				AbsorbSyncRequests();
				absorb_counter = FSYNCS_PER_ABSORB; /* 顺便重置也无妨... */
			}					/* 重试循环结束 */
		}

		/* 本条目处理完毕，将其移除 */
		if (hash_search(pendingOps, &entry->tag, HASH_REMOVE, NULL) == NULL)
			elog(ERROR, "pendingOps corrupted");
	}							/* 哈希表条目循环结束 */

	/* 返回同步性能指标，供检查点结束时报告 */
	CheckpointStats.ckpt_sync_rels = processed;
	CheckpointStats.ckpt_longest_sync = longest;
	CheckpointStats.ckpt_agg_sync_time = total_elapsed;

	/* 标记 ProcessSyncRequests 成功完成 */
	sync_in_progress = false;
}

/*
 * RememberSyncRequest() -- 来自 sync 请求 checkpointer 侧的回调
 *
 * 我们将 fsync 请求塞入本地哈希表，以便在 checkpointer 的下一次检查点
 * 期间执行。不过 UNLINK 请求会进入一个独立的链表，因为它们是分开
 * 处理的。
 *
 * 关于所支持的 sync 请求类型，详见 sync.h。
 */
void
RememberSyncRequest(const FileTag *ftag, SyncRequestType type)
{
	Assert(pendingOps);

	if (type == SYNC_FORGET_REQUEST)
	{
		PendingFsyncEntry *entry;

		/* 取消先前登记的请求 */
		entry = (PendingFsyncEntry *) hash_search(pendingOps,
												  ftag,
												  HASH_FIND,
												  NULL);
		if (entry != NULL)
			entry->canceled = true;
	}
	else if (type == SYNC_FILTER_REQUEST)
	{
		HASH_SEQ_STATUS hstat;
		PendingFsyncEntry *pfe;
		ListCell   *cell;

		/* 取消匹配的 fsync 请求 */
		hash_seq_init(&hstat, pendingOps);
		while ((pfe = (PendingFsyncEntry *) hash_seq_search(&hstat)) != NULL)
		{
			if (pfe->tag.handler == ftag->handler &&
				syncsw[ftag->handler].sync_filetagmatches(ftag, &pfe->tag))
				pfe->canceled = true;
		}

		/* 取消匹配的 unlink 请求 */
		foreach(cell, pendingUnlinks)
		{
			PendingUnlinkEntry *pue = (PendingUnlinkEntry *) lfirst(cell);

			if (pue->tag.handler == ftag->handler &&
				syncsw[ftag->handler].sync_filetagmatches(ftag, &pue->tag))
				pue->canceled = true;
		}
	}
	else if (type == SYNC_UNLINK_REQUEST)
	{
		/* unlink 请求：将其放入链表 */
		MemoryContext oldcxt = MemoryContextSwitchTo(pendingOpsCxt);
		PendingUnlinkEntry *entry;

		entry = palloc(sizeof(PendingUnlinkEntry));
		entry->tag = *ftag;
		entry->cycle_ctr = checkpoint_cycle_ctr;
		entry->canceled = false;

		pendingUnlinks = lappend(pendingUnlinks, entry);

		MemoryContextSwitchTo(oldcxt);
	}
	else
	{
		/* 常规情况：登记一个 fsync 此段的请求 */
		MemoryContext oldcxt = MemoryContextSwitchTo(pendingOpsCxt);
		PendingFsyncEntry *entry;
		bool		found;

		Assert(type == SYNC_REQUEST);

		entry = (PendingFsyncEntry *) hash_search(pendingOps,
												  ftag,
												  HASH_ENTER,
												  &found);
		/* 如果是新条目，或之前被取消过，则初始化它 */
		if (!found || entry->canceled)
		{
			entry->cycle_ctr = sync_cycle_ctr;
			entry->canceled = false;
		}

		/*
		 * 注意：如果条目已存在，我们故意不改变 cycle_ctr。cycle_ctr 必须
		 * 代表该条目中可能存在的、最旧的 fsync 请求。
		 */

		MemoryContextSwitchTo(oldcxt);
	}
}

/*
 * 在本地登记 sync 请求，或将其转发给 checkpointer。
 *
 * 如果 retryOnError 为 true，当队列中没有空间时我们会持续重试。
 * 成功返回 true，没有空间则返回 false。
 */
bool
RegisterSyncRequest(const FileTag *ftag, SyncRequestType type,
					bool retryOnError)
{
	bool		ret;

	if (pendingOps != NULL)
	{
		/* 独立后端进程或启动进程：fsync 状态是本地的 */
		RememberSyncRequest(ftag, type);
		return true;
	}

	for (;;)
	{
		/*
		 * 将此事通知 checkpointer。如果在 retryOnError 模式下未能将消息
		 * 排队，我们就必须休眠并重试……这很丑陋，但希望不会经常发生。
		 *
		 * XXX：我们应该在此循环中 CHECK_FOR_INTERRUPTS 吗？在
		 * SYNC_UNLINK_REQUEST 的情况下带着错误逃脱，会让不再使用的文件
		 * 仍然留在磁盘上，这很糟糕，因此我倾向于假设 checkpointer 总会
		 * 很快清空队列。
		 */
		ret = ForwardSyncRequest(ftag, type);

		/*
		 * 如果我们成功将请求排队，或者失败且被指示不要在出错时重试，
		 * 则跳出循环。
		 */
		if (ret || (!ret && !retryOnError))
			break;

		WaitLatch(NULL, WL_EXIT_ON_PM_DEATH | WL_TIMEOUT, 10,
				  WAIT_EVENT_REGISTER_SYNC_REQUEST);
	}

	return ret;
}
