/*-------------------------------------------------------------------------
 *
 * smgr.c
 *	  存储管理器切换的对外接口例程。
 *
 * 所有针对关系的文件系统操作都经由这些例程进行分发。
 * SMgrRelation 表示已打开、可供读写的物理磁盘关系文件。
 *
 * 当一个关系首次通过关系缓存被访问时，对应的 SMgrRelation 项会通过
 * 调用 smgropen() 打开，该引用会被保存在关系缓存项中。
 *
 * 不经过关系缓存的访问会直接打开 SMgrRelation。这包括从缓冲区缓存
 * 刷出缓冲区，以及诸如检查点进程或启动进程中的 WAL 重做等辅助进程
 * 内的所有访问。
 *
 * 像 CREATE、DROP、ALTER TABLE 这样的操作也会持有独立于关系缓存的
 * SMgrRelation 引用。它们需要在更新关系缓存之前先准备好物理文件。
 *
 * 后端中维护着一个哈希表，保存所有的 SMgrRelation 项。如果对同一个
 * 关系定位器（rel locator）调用两次 smgropen()，会得到同一个
 * SMgrRelation 的引用。该引用在事务结束之前一直有效。这使得对同一个
 * 关系的重复访问更高效，并且允许在 SMgrRelation 项中缓存关系大小等
 * 信息。
 *
 * 在事务结束时，所有未被固定的（unpinned）SMgrRelation 项会被移除。
 * 一个 SMgrRelation 可能持有底层文件的内核文件系统描述符，如果文件
 * 被删除，我们希望尽快关闭这些描述符。由 relcache 持有的 SMgrRelation
 * 引用会被固定（pinned），以防止它们被关闭。
 *
 * 还有另一种提前关闭文件描述符的机制：
 * PROCSIGNAL_BARRIER_SMGRRELEASE。它是一个立即关闭所有文件描述符的
 * 请求。后端收到该信号后，会关闭 SMgrRelation 持有的所有已打开文件
 * 描述符，但由于它可能发生在事务执行过程中，我们不能销毁 SMgrRelation
 * 对象本身，因为可能有正在使用的指针指向它们。参见
 * smgrrelease() 和 smgrreleaseall()。
 *
 * 注意：本文件中的大多数函数都需要在持有中断的情况下执行，否则中断
 * 处理（例如由于 < ERROR 级别的 elog/ereport）可能触发 procsignal 处理，
 * 进而触发 smgrreleaseall()。相关代码大多不可重入。将
 * HOLD_INTERRUPTS()/RESUME_INTERRUPTS() 放在这里，而不是在可能时下沉到
 * md.c，似乎更好：一方面，每个 smgr 实现都可能存在此问题；另一方面，
 * smgr.c 自身也有相当一部分代码受到影响。最终我们或许想要一个更有
 * 针对性的方案，例如允许网络化的 smgr 实现被中断，但要使其可行还需
 * 解决许多更复杂的问题（例如 smgr.c 常常在中断已被持有的情况下被调用）。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/smgr/smgr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlogutils.h"
#include "lib/ilist.h"
#include "miscadmin.h"
#include "storage/aio.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/md.h"
#include "storage/smgr.h"
#include "utils/hsearch.h"
#include "utils/inval.h"


/*
 * 这个由函数指针组成的结构体定义了 smgr.c 与各个存储管理器模块之间的
 * API。注意 smgr 子函数通常应通过 elog(ERROR) 来报告问题。例外情况是
 * smgr_unlink 应使用 elog(WARNING) 而不是报错，因为我们通常在提交后/
 * 中止后的清理阶段解除关系链接，此时再报错已经太晚了。此外，在引导
 * （bootstrap）和/或 WAL 恢复期间，通常会被视为错误的各种情况应被允许
 * ——详见 md.c 中的注释。
 */
typedef struct f_smgr
{
	void		(*smgr_init) (void);	/* 可以为 NULL */
	void		(*smgr_shutdown) (void);	/* 可以为 NULL */
	void		(*smgr_open) (SMgrRelation reln);
	void		(*smgr_close) (SMgrRelation reln, ForkNumber forknum);
	void		(*smgr_create) (SMgrRelation reln, ForkNumber forknum,
								bool isRedo);
	bool		(*smgr_exists) (SMgrRelation reln, ForkNumber forknum);
	void		(*smgr_unlink) (RelFileLocatorBackend rlocator, ForkNumber forknum,
								bool isRedo);
	void		(*smgr_extend) (SMgrRelation reln, ForkNumber forknum,
								BlockNumber blocknum, const void *buffer, bool skipFsync);
	void		(*smgr_zeroextend) (SMgrRelation reln, ForkNumber forknum,
									BlockNumber blocknum, int nblocks, bool skipFsync);
	bool		(*smgr_prefetch) (SMgrRelation reln, ForkNumber forknum,
								  BlockNumber blocknum, int nblocks);
	uint32		(*smgr_maxcombine) (SMgrRelation reln, ForkNumber forknum,
									BlockNumber blocknum);
	void		(*smgr_readv) (SMgrRelation reln, ForkNumber forknum,
							   BlockNumber blocknum,
							   void **buffers, BlockNumber nblocks);
	void		(*smgr_startreadv) (PgAioHandle *ioh,
									SMgrRelation reln, ForkNumber forknum,
									BlockNumber blocknum,
									void **buffers, BlockNumber nblocks);
	void		(*smgr_writev) (SMgrRelation reln, ForkNumber forknum,
								BlockNumber blocknum,
								const void **buffers, BlockNumber nblocks,
								bool skipFsync);
	void		(*smgr_writeback) (SMgrRelation reln, ForkNumber forknum,
								   BlockNumber blocknum, BlockNumber nblocks);
	BlockNumber (*smgr_nblocks) (SMgrRelation reln, ForkNumber forknum);
	void		(*smgr_truncate) (SMgrRelation reln, ForkNumber forknum,
								  BlockNumber old_blocks, BlockNumber nblocks);
	void		(*smgr_immedsync) (SMgrRelation reln, ForkNumber forknum);
	void		(*smgr_registersync) (SMgrRelation reln, ForkNumber forknum);
	int			(*smgr_fd) (SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum, uint32 *off);
} f_smgr;

static const f_smgr smgrsw[] = {
	/* 磁盘 */
	{
		.smgr_init = mdinit,
		.smgr_shutdown = NULL,
		.smgr_open = mdopen,
		.smgr_close = mdclose,
		.smgr_create = mdcreate,
		.smgr_exists = mdexists,
		.smgr_unlink = mdunlink,
		.smgr_extend = mdextend,
		.smgr_zeroextend = mdzeroextend,
		.smgr_prefetch = mdprefetch,
		.smgr_maxcombine = mdmaxcombine,
		.smgr_readv = mdreadv,
		.smgr_startreadv = mdstartreadv,
		.smgr_writev = mdwritev,
		.smgr_writeback = mdwriteback,
		.smgr_nblocks = mdnblocks,
		.smgr_truncate = mdtruncate,
		.smgr_immedsync = mdimmedsync,
		.smgr_registersync = mdregistersync,
		.smgr_fd = mdfd,
	}
};

static const int NSmgr = lengthof(smgrsw);

/*
 * 每个后端都有一个哈希表，用于保存所有现存的 SMgrRelation 对象。
 * 此外，所有“未固定”（unpinned）的 SMgrRelation 对象会被串成一个链表。
 */
static HTAB *SMgrRelationHash = NULL;

static dlist_head unpinned_relns;

/* 本地函数原型 */
static void smgrshutdown(int code, Datum arg);
static void smgrdestroy(SMgrRelation reln);

static void smgr_aio_reopen(PgAioHandle *ioh);
static char *smgr_aio_describe_identity(const PgAioTargetData *sd);


const PgAioTargetInfo aio_smgr_target_info = {
	.name = "smgr",
	.reopen = smgr_aio_reopen,
	.describe_identity = smgr_aio_describe_identity,
};


/*
 * smgrinit()、smgrshutdown() -- 初始化或关闭存储管理器。
 *
 * 注意：smgrinit 在后端启动期间（普通或独立模式）被调用，而*不是*在
 * postmaster 启动期间。因此，在这里创建或在 smgrshutdown 中销毁的
 * 任何资源都是后端局部（backend-local）的。
 */
void
smgrinit(void)
{
	int			i;

	HOLD_INTERRUPTS();

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_init)
			smgrsw[i].smgr_init();
	}

	RESUME_INTERRUPTS();

	/* 注册关闭处理函数 */
	on_proc_exit(smgrshutdown, 0);
}

/*
 * 后端关闭期间用于 smgr 清理的 on_proc_exit 钩子
 */
static void
smgrshutdown(int code, Datum arg)
{
	int			i;

	HOLD_INTERRUPTS();

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_shutdown)
			smgrsw[i].smgr_shutdown();
	}

	RESUME_INTERRUPTS();
}

/*
 * smgropen() -- 返回一个 SMgrRelation 对象，必要时创建它。
 *
 * 在 17 之前的 PostgreSQL 版本中，该函数返回一个生命周期未定义的对象。
 * 但现在，该对象在事务的整个生命周期内保持有效，直到调用
 * AtEOXact_SMgr() 为止，这让调用者更容易判断自己能持有指向返回对象的
 * 指针多久。如果在事务之外调用该函数，对象会一直有效，直到调用
 * smgrdestroy() 或 smgrdestroyall()。使用 smgr 但不使用事务的后台进程
 * 通常每个检查点周期执行一次。
 *
 * 该函数并不会真正去打开底层文件。
 */
SMgrRelation
smgropen(RelFileLocator rlocator, ProcNumber backend)
{
	RelFileLocatorBackend brlocator;
	SMgrRelation reln;
	bool		found;

	Assert(RelFileNumberIsValid(rlocator.relNumber));

	HOLD_INTERRUPTS();

	if (SMgrRelationHash == NULL)
	{
		/* 首次调用：初始化哈希表 */
		HASHCTL		ctl;

		ctl.keysize = sizeof(RelFileLocatorBackend);
		ctl.entrysize = sizeof(SMgrRelationData);
		SMgrRelationHash = hash_create("smgr relation table", 400,
									   &ctl, HASH_ELEM | HASH_BLOBS);
		dlist_init(&unpinned_relns);
	}

	/* 查找或创建一个项 */
	brlocator.locator = rlocator;
	brlocator.backend = backend;
	reln = (SMgrRelation) hash_search(SMgrRelationHash,
									  &brlocator,
									  HASH_ENTER, &found);

	/* 如果之前不存在则进行初始化 */
	if (!found)
	{
		/* hash_search 已经填好了查找键 */
		reln->smgr_targblock = InvalidBlockNumber;
		for (int i = 0; i <= MAX_FORKNUM; ++i)
			reln->smgr_cached_nblocks[i] = InvalidBlockNumber;
		reln->smgr_which = 0;	/* 目前只有 md.c 一种实现 */

		/* 尚未被固定 */
		reln->pincount = 0;
		dlist_push_tail(&unpinned_relns, &reln->node);

		/* implementation-specific initialization */
		smgrsw[reln->smgr_which].smgr_open(reln);
	}

	RESUME_INTERRUPTS();

	return reln;
}

/*
 * smgrpin() -- 防止 SMgrRelation 对象在事务结束时被销毁
 */
void
smgrpin(SMgrRelation reln)
{
	if (reln->pincount == 0)
		dlist_delete(&reln->node);
	reln->pincount++;
}

/*
 * smgrunpin() -- 允许 SMgrRelation 对象在事务结束时被销毁
 *
 * 该对象仍然有效，但如果没有其他 pin，它会被移到未固定链表中，
 * 由 AtEOXact_SMgr() 将其销毁。
 */
void
smgrunpin(SMgrRelation reln)
{
	Assert(reln->pincount > 0);
	reln->pincount--;
	if (reln->pincount == 0)
		dlist_push_tail(&unpinned_relns, &reln->node);
}

/*
 * smgrdestroy() -- 删除一个 SMgrRelation 对象。
 */
static void
smgrdestroy(SMgrRelation reln)
{
	ForkNumber	forknum;

	Assert(reln->pincount == 0);

	HOLD_INTERRUPTS();

	for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
		smgrsw[reln->smgr_which].smgr_close(reln, forknum);

	dlist_delete(&reln->node);

	if (hash_search(SMgrRelationHash,
					&(reln->smgr_rlocator),
					HASH_REMOVE, NULL) == NULL)
		elog(ERROR, "SMgrRelation hashtable corrupted");

	RESUME_INTERRUPTS();
}

/*
 * smgrrelease() -- 释放该对象使用的所有资源。
 *
 * 该对象仍然保持有效。
 */
void
smgrrelease(SMgrRelation reln)
{
	HOLD_INTERRUPTS();

	for (ForkNumber forknum = 0; forknum <= MAX_FORKNUM; forknum++)
	{
		smgrsw[reln->smgr_which].smgr_close(reln, forknum);
		reln->smgr_cached_nblocks[forknum] = InvalidBlockNumber;
	}
	reln->smgr_targblock = InvalidBlockNumber;

	RESUME_INTERRUPTS();
}

/*
 * smgrclose() -- 关闭一个 SMgrRelation 对象。
 *
 * 本次调用之后不应再使用该 SMgrRelation 引用。然而，由于我们没有跟踪
 * smgropen() 返回的引用，无法确定是否还有其他引用仍指向同一个对象，
 * 因此暂时还不能移除该 SMgrRelation 对象。所以目前它只是 smgrrelease()
 * 的同义函数。
 */
void
smgrclose(SMgrRelation reln)
{
	smgrrelease(reln);
}

/*
 * smgrdestroyall() -- 释放所有未固定对象使用的资源。
 *
 * 调用前必须确认：除了通过 smgrpin() 固定的那些之外，没有任何指向
 * SMgrRelation 的指针。
 */
void
smgrdestroyall(void)
{
	dlist_mutable_iter iter;

	/* 在 dlist_foreach_modify() 中接受中断似乎不安全 */
	HOLD_INTERRUPTS();

	/*
	 * 清除所有未固定的 SMgrRelation。我们依赖 smgrdestroy() 将每一项
	 * 从链表中移除。
	 */
	dlist_foreach_modify(iter, &unpinned_relns)
	{
		SMgrRelation rel = dlist_container(SMgrRelationData, node,
										   iter.cur);

		smgrdestroy(rel);
	}

	RESUME_INTERRUPTS();
}

/*
 * smgrreleaseall() -- 释放所有对象使用的资源。
 */
void
smgrreleaseall(void)
{
	HASH_SEQ_STATUS status;
	SMgrRelation reln;

	/* 如果哈希表尚未建立则无事可做 */
	if (SMgrRelationHash == NULL)
		return;

	/* 迭代期间接受中断似乎不安全 */
	HOLD_INTERRUPTS();

	hash_seq_init(&status, SMgrRelationHash);

	while ((reln = (SMgrRelation) hash_seq_search(&status)) != NULL)
	{
		smgrrelease(reln);
	}

	RESUME_INTERRUPTS();
}

/*
 * smgrreleaserellocator() -- 释放给定 RelFileLocator 的资源（如果它已打开）。
 *
 * 其效果与 smgrrelease(smgropen(rlocator)) 相同，但当对应项不存在时，
 * 可以避免无谓地创建哈希表项又随即丢弃它。
 */
void
smgrreleaserellocator(RelFileLocatorBackend rlocator)
{
	SMgrRelation reln;

	/* 如果哈希表尚未建立则无事可做 */
	if (SMgrRelationHash == NULL)
		return;

	reln = (SMgrRelation) hash_search(SMgrRelationHash,
									  &rlocator,
									  HASH_FIND, NULL);
	if (reln != NULL)
		smgrrelease(reln);
}

/*
 * smgrexists() -- 某个 fork 的底层文件是否存在？
 */
bool
smgrexists(SMgrRelation reln, ForkNumber forknum)
{
	bool		ret;

	HOLD_INTERRUPTS();
	ret = smgrsw[reln->smgr_which].smgr_exists(reln, forknum);
	RESUME_INTERRUPTS();

	return ret;
}

/*
 * smgrcreate() -- 创建一个新的关系。
 *
 * 给定一个已创建（但大概尚未使用）的 SMgrRelation，使该 fork 的
 * 底层磁盘文件或其他存储被创建出来。
 */
void
smgrcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo)
{
	HOLD_INTERRUPTS();
	smgrsw[reln->smgr_which].smgr_create(reln, forknum, isRedo);
	RESUME_INTERRUPTS();
}

/*
 * smgrdosyncall() -- 立即将所有给定关系所有 fork 同步到存储。
 *
 * 所有给定关系的所有 fork 都会被同步到存储中。
 *
 * 这等价于对每个 smgr 关系调用 FlushRelationBuffers()，再对每个关系的
 * 所有 fork 调用 smgrimmedsync()，但速度要快得多，因此应尽可能优先使用。
 */
void
smgrdosyncall(SMgrRelation *rels, int nrels)
{
	int			i = 0;
	ForkNumber	forknum;

	if (nrels == 0)
		return;

	FlushRelationsAllBuffers(rels, nrels);

	HOLD_INTERRUPTS();

	/*
	 * 同步物理文件。
	 */
	for (i = 0; i < nrels; i++)
	{
		int			which = rels[i]->smgr_which;

		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
		{
			if (smgrsw[which].smgr_exists(rels[i], forknum))
				smgrsw[which].smgr_immedsync(rels[i], forknum);
		}
	}

	RESUME_INTERRUPTS();
}

/*
 * smgrdounlinkall() -- 立即解除所有给定关系所有 fork 的链接
 *
 * 所有给定关系的所有 fork 都会从存储中被移除。这不应在事务操作期间
 * 使用，因为它无法被回滚。
 *
 * 如果 isRedo 为 true，底层文件已经不存在也是允许的。
 */
void
smgrdounlinkall(SMgrRelation *rels, int nrels, bool isRedo)
{
	int			i = 0;
	RelFileLocatorBackend *rlocators;
	ForkNumber	forknum;

	if (nrels == 0)
		return;

	/*
	 * 在 DropRelationBuffers() 与解除底层文件链接之间处理中断是不安全的。
	 * 这或许应当是一个临界区，但目前还没做到。
	 */
	HOLD_INTERRUPTS();

	/*
	 * 丢弃这些关系剩余的所有缓冲区。bufmgr 会直接丢弃它们，
	 * 而不费心写出其内容。
	 */
	DropRelationsAllBuffers(rels, nrels);

	/*
	 * 创建一个数组，包含所有要删除的关系，并顺便在 smgr 层关闭
	 * 每个关系的各个 fork
	 */
	rlocators = palloc(sizeof(RelFileLocatorBackend) * nrels);
	for (i = 0; i < nrels; i++)
	{
		RelFileLocatorBackend rlocator = rels[i]->smgr_rlocator;
		int			which = rels[i]->smgr_which;

		rlocators[i] = rlocator;

		/* 在 smgr 层关闭各 fork */
		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
			smgrsw[which].smgr_close(rels[i], forknum);
	}

	/*
	 * 发送一条共享失效（shared-inval）消息，强制其他后端关闭它们可能
	 * 持有的、指向这些关系的悬空 smgr 引用。我们应在开始真正解除链接
	 * 之前执行此操作，以防在中间某步失败。注意这些 sinval 消息最终也会
	 * 回到本后端，从而提供一个兜底，确保我们也关闭了自己的 smgr 关系。
	 */
	for (i = 0; i < nrels; i++)
		CacheInvalidateSmgr(rlocators[i]);

	/*
	 * 删除物理文件。
	 *
	 * 注意：smgr_unlink 必须把删除失败视为 WARNING 而非 ERROR，因为我们
	 * 已经决定提交或中止当前事务。
	 */

	for (i = 0; i < nrels; i++)
	{
		int			which = rels[i]->smgr_which;

		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
			smgrsw[which].smgr_unlink(rlocators[i], forknum, isRedo);
	}

	pfree(rlocators);

	RESUME_INTERRUPTS();
}


/*
 * smgrextend() -- 向文件追加一个新块。
 *
 * 其语义与 smgrwrite() 几乎相同：在指定位置写入。但它是用于扩展关系
 * 的场景（即 blocknum 位于当前 EOF 处或其之后）。注意我们假设在
 * 当前 EOF 之后写入一个块，会使中间的文件空间被填为零。
 */
void
smgrextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		   const void *buffer, bool skipFsync)
{
	HOLD_INTERRUPTS();

	smgrsw[reln->smgr_which].smgr_extend(reln, forknum, blocknum,
										 buffer, skipFsync);

	/*
	 * 通常我们期望这会使 nblocks 增加 1，但如果缓存的值与预期不符，
	 * 则将其置为无效，以便下次调用时向内核查询。
	 */
	if (reln->smgr_cached_nblocks[forknum] == blocknum)
		reln->smgr_cached_nblocks[forknum] = blocknum + 1;
	else
		reln->smgr_cached_nblocks[forknum] = InvalidBlockNumber;

	RESUME_INTERRUPTS();
}

/*
 * smgrzeroextend() -- 向文件追加新的全零块。
 *
 * 与 smgrextend() 类似，不同之处在于关系可以一次性扩展多个块，
 * 且新增的块会被填为零。
 */
void
smgrzeroextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			   int nblocks, bool skipFsync)
{
	HOLD_INTERRUPTS();

	smgrsw[reln->smgr_which].smgr_zeroextend(reln, forknum, blocknum,
											 nblocks, skipFsync);

	/*
	 * 通常我们期望这会使 fork 大小增加 nblocks，但如果缓存的值与预期
	 * 不符，则将其置为无效，以便下次调用时向内核查询。
	 */
	if (reln->smgr_cached_nblocks[forknum] == blocknum)
		reln->smgr_cached_nblocks[forknum] = blocknum + nblocks;
	else
		reln->smgr_cached_nblocks[forknum] = InvalidBlockNumber;

	RESUME_INTERRUPTS();
}

/*
 * smgrprefetch() -- 发起对关系中指定块的异步读取。
 *
 * 仅在恢复期间，它可能返回 false 来表明某个文件不存在
 * （大概它已被后续的 WAL 记录删除）。
 */
bool
smgrprefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			 int nblocks)
{
	bool		ret;

	HOLD_INTERRUPTS();
	ret = smgrsw[reln->smgr_which].smgr_prefetch(reln, forknum, blocknum, nblocks);
	RESUME_INTERRUPTS();

	return ret;
}

/*
 * smgrmaxcombine() -- 返回以 blocknum 起始的 IO 最多可以合并的
 *				 总块数。
 *
 * 返回值包含 blocknum 本身的那个 IO。
 */
uint32
smgrmaxcombine(SMgrRelation reln, ForkNumber forknum,
			   BlockNumber blocknum)
{
	uint32		ret;

	HOLD_INTERRUPTS();
	ret = smgrsw[reln->smgr_which].smgr_maxcombine(reln, forknum, blocknum);
	RESUME_INTERRUPTS();

	return ret;
}

/*
 * smgrreadv() -- 从关系中读取一个特定的块范围到所提供的缓冲区中。
 *
 * 该例程由缓冲区管理器调用，以便在共享缓冲区缓存中实例化页面。
 * 所有存储管理器都以 POSTGRES 期望的格式返回页面。
 *
 * 如果打算读取多个块，调用者需要使用 smgrmaxcombine() 来检查有多少
 * 块可以合并到一次 IO 中。
 */
void
smgrreadv(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		  void **buffers, BlockNumber nblocks)
{
	HOLD_INTERRUPTS();
	smgrsw[reln->smgr_which].smgr_readv(reln, forknum, blocknum, buffers,
										nblocks);
	RESUME_INTERRUPTS();
}

/*
 * smgrstartreadv() -- smgrreadv() 的异步版本
 *
 * 使用该 IO 句柄 `ioh` 启动一次异步 readv IO。除 `ioh` 之外，所有参数
 * 与 smgrreadv() 相同。
 *
 * smgr 之上的完成回调会收到结果，即成功读取的块数（如果读取[部分]
 * 成功的话；未成功读取的块对应的缓冲区可能带有未指定的修改，最多可达
 * 全部 nblocks）。这维持了 smgr 以块而非字节为操作层级的抽象。
 *
 * 与 smgrreadv() 相比，调用者需要承担更多责任：
 * - 部分读取需要由调用者重新发起针对未读块的 IO 来处理
 * - smgr 会以 ereport(LOG_SERVER_ONLY) 报告某些问题，但上层负责调用
 *   pgaio_result_report() 将该消息反映给用户（如果 IO 结果为
 *   PGAIO_RS_WARNING），或中止（子）事务（如果 IO 结果为 PGAIO_RS_ERROR）。
 * - 在 Valgrind 下，"buffers" 内存的状态可能变为也可能不变为 DEFINED，
 *   取决于 io_method 和并发活动。
 */
void
smgrstartreadv(PgAioHandle *ioh,
			   SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			   void **buffers, BlockNumber nblocks)
{
	HOLD_INTERRUPTS();
	smgrsw[reln->smgr_which].smgr_startreadv(ioh,
											 reln, forknum, blocknum, buffers,
											 nblocks);
	RESUME_INTERRUPTS();
}

/*
 * smgrwritev() -- 写出所提供的缓冲区。
 *
 * 仅用于更新关系中已存在的块（即当前 EOF 之前的那些）。要扩展关系，
 * 请使用 smgrextend()。
 *
 * 这不是同步写——返回时块不一定已落盘，只是被转交给内核。不过，会
 * 采取措施在下次检查点之前 fsync 该写入。
 *
 * 注意：确保在下次检查点 fsync 的机制，依赖于存在某种阻止并发检查点
 * “抢跑”在写入之前的因素。一种方式是对缓冲区加锁；缓冲区管理器的
 * 写入即受此保护。bulk_write.c 中的批量写入设施会检查 redo 指针，并在
 * 发生检查点时调用 smgrimmedsync()；这依赖于没有其他后端能并发修改
 * 该页面的事实。
 *
 * skipFsync 表示调用者将通过其他途径来 fsync 该关系，因此我们无需
 * 费心。临时关系也不需要 fsync。
 *
 * 如果打算读取多个块，调用者需要使用 smgrmaxcombine() 来检查有多少
 * 块可以合并到一次 IO 中。
 */
void
smgrwritev(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		   const void **buffers, BlockNumber nblocks, bool skipFsync)
{
	HOLD_INTERRUPTS();
	smgrsw[reln->smgr_which].smgr_writev(reln, forknum, blocknum,
										 buffers, nblocks, skipFsync);
	RESUME_INTERRUPTS();
}

/*
 * smgrwriteback() -- 触发内核对已提供的块范围执行回写。
 */
void
smgrwriteback(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			  BlockNumber nblocks)
{
	HOLD_INTERRUPTS();
	smgrsw[reln->smgr_which].smgr_writeback(reln, forknum, blocknum,
											nblocks);
	RESUME_INTERRUPTS();
}

/*
 * smgrnblocks() -- 计算所提供关系中的块数。
 */
BlockNumber
smgrnblocks(SMgrRelation reln, ForkNumber forknum)
{
	BlockNumber result;

	/* 如果拿到块数的缓存值，则直接返回。 */
	result = smgrnblocks_cached(reln, forknum);
	if (result != InvalidBlockNumber)
		return result;

	HOLD_INTERRUPTS();

	result = smgrsw[reln->smgr_which].smgr_nblocks(reln, forknum);

	reln->smgr_cached_nblocks[forknum] = result;

	RESUME_INTERRUPTS();

	return result;
}

/*
 * smgrnblocks_cached() -- 获取所提供关系中缓存的块数。
 *
 * 当不在恢复状态，或关系 fork 大小未被缓存时，返回 InvalidBlockNumber。
 */
BlockNumber
smgrnblocks_cached(SMgrRelation reln, ForkNumber forknum)
{
	/*
	 * 目前，由于缺乏针对文件大小变化的共享失效机制，该函数在恢复期间
	 * 才会使用缓存值。其他位置的代码会读取 smgr_cached_nblocks 并
	 * 处理其中的过期数据。
	 */
	if (InRecovery && reln->smgr_cached_nblocks[forknum] != InvalidBlockNumber)
		return reln->smgr_cached_nblocks[forknum];

	return InvalidBlockNumber;
}

/*
 * smgrtruncate() -- 将所提供关系的给定 fork 截断到各自指定的块数
 *
 * 截断会立即执行，因此无法回滚。
 *
 * 调用者必须持有该关系的 AccessExclusiveLock，以确保其他后端在本函数
 * 发送 smgr 失效事件之后、再次访问该关系的任何 fork 之前收到该事件。
 * fork 的当前大小应通过 old_nblocks 提供。该函数通常应在临界区中调用，
 * 但当前大小必须在临界区之外检查，且两者之间不得调用任何与本关系
 * 相关的中断或 smgr 函数。
 */
void
smgrtruncate(SMgrRelation reln, ForkNumber *forknum, int nforks,
			 BlockNumber *old_nblocks, BlockNumber *nblocks)
{
	int			i;

	/*
	 * 丢弃即将被删除的块对应的所有缓冲区。bufmgr 会直接丢弃它们，
	 * 而不费心写出其内容。
	 */
	DropRelationBuffers(reln, forknum, nforks, nblocks);

	/*
	 * 发送一条共享失效消息，强制其他后端关闭它们可能持有的、指向本
	 * 关系的任何 smgr 引用。这很有用，因为它们可能持有指向已被删除
	 * 段的打开文件指针，和/或指向新关系末尾之后的 smgr_targblock 变量。
	 * （该失效消息也会回到本后端，导致一次大概不必要的本地 smgr 刷出。
	 * 但我们并不认为这是性能关键的路径。）与解除链接的代码一样，我们
	 * 希望确保该消息在我们开始改动磁盘上的内容之前发出。
	 */
	CacheInvalidateSmgr(reln->smgr_rlocator);

	/* 执行截断 */
	for (i = 0; i < nforks; i++)
	{
		/* 如果遇到错误，使缓存大小变为无效。 */
		reln->smgr_cached_nblocks[forknum[i]] = InvalidBlockNumber;

		smgrsw[reln->smgr_which].smgr_truncate(reln, forknum[i],
											   old_nblocks[i], nblocks[i]);

		/*
		 * 我们不妨更新本地的 smgr_cached_nblocks 值。本函数发出的 smgr
		 * 缓存失效消息会导致其他后端使它们的 smgr_cached_nblocks 副本
		 * 失效，这些本地的副本也会在下个命令边界失效。但要确保在那之前
		 * 它们不完全错误。
		 *
		 * 当一个关系被多次截断、某个副本应用了所有截断、之后又从一个
		 * 位于这些截断之前的重启点重新启动时，可能出现 nblocks > old_nblocks
		 * 的情况。磁盘上的关系将是最后一次截断后的大小。重放第一次截断
		 * 时，我们会得到 nblocks > 当前大小。这种情况下 smgr_truncate 不
		 * 做任何事，因此应将缓存大小设为旧大小而非请求的大小。
		 */
		reln->smgr_cached_nblocks[forknum[i]] =
			nblocks[i] > old_nblocks[i] ? old_nblocks[i] : nblocks[i];
	}
}

/*
 * smgrregistersync() -- 请求一个关系在下次检查点被同步
 *
 * 它可在以 skipFsync = true 调用 smgrwrite() 或 smgrextend() 之后使用，
 * 用于登记之前被跳过的 fsync。
 *
 * 注意：要当心在 smgrwrite 或 smgrextend 调用与本次调用之间，可能已经
 * 发生过检查点！这种情况下，该检查点已经错过了对本关系的 fsync，你应当
 * 改用 smgrimmedsync。大多数调用者应使用 bulk_write.c 中的批量加载设施，
 * 它会处理好这一切。
 */
void
smgrregistersync(SMgrRelation reln, ForkNumber forknum)
{
	HOLD_INTERRUPTS();
	smgrsw[reln->smgr_which].smgr_registersync(reln, forknum);
	RESUME_INTERRUPTS();
}

/*
 * smgrimmedsync() -- 强制将指定关系同步到稳定存储。
 *
 * 同步地将之前对该指定关系的所有写入强制落盘。
 *
 * 这在构建全新关系（例如新建索引）时很有用。我们不必按增量方式将索引
 * 构建步骤记录到 WAL，而只需用 smgrwrite 或 smgrextend 把已完成的索引
 * 页写入磁盘，然后在提交事务之前 fsync 完成后的索引文件。（这对于崩溃
 * 恢复来说已经足够，因为它实际上等价于对完成后的索引强制做一次检查点。
 * 但如果你希望将 WAL 日志用于 PITR 或复制目的，这就*不够*了：那种情况下
 * 我们还必须生成 WAL 记录。）
 *
 * 前面的写入应当指定 skipFsync = true，以避免重复的 fsync。
 *
 * 注意，如果存在该关系存在脏缓冲区的任何可能，你需要先执行
 * FlushRelationBuffers()；否则这次同步意义不大。
 *
 * 大多数调用者应使用 bulk_write.c 中的批量加载设施，而不是直接调用本
 * 函数。
 */
void
smgrimmedsync(SMgrRelation reln, ForkNumber forknum)
{
	HOLD_INTERRUPTS();
	smgrsw[reln->smgr_which].smgr_immedsync(reln, forknum);
	RESUME_INTERRUPTS();
}

/*
 * 返回指定块号对应的 fd，并将 *off 更新到合适的位置。
 *
 * 这仅用于 AIO 需要在与发起 IO 不同的进程中执行该 IO 的情况
 * （例如在某个 IO worker 中）。
 */
static int
smgrfd(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum, uint32 *off)
{
	int			fd;

	/*
	 * 调用者需要阻止中断被处理，否则该 FD 可能会被过早关闭。
	 */
	Assert(!INTERRUPTS_CAN_BE_PROCESSED());

	fd = smgrsw[reln->smgr_which].smgr_fd(reln, forknum, blocknum, off);

	return fd;
}

/*
 * AtEOXact_SMgr
 *
 * 该例程在事务提交或中止时调用（它并不在意是哪种）。所有未固定的
 * SMgrRelation 对象都会被销毁。
 *
 * 我们这样做是为了在两种需求之间折中：一方面希望临时 SMgrRelation 能
 * 存活一段时间（以摊还多次盲写的开销），另一方面又需要它们不能永远
 * 存活（因为我们很可能持有着底层文件的内核文件描述符，而需要确保该
 * 文件被删除时能尽快关闭它）。
 */
void
AtEOXact_SMgr(void)
{
	smgrdestroyall();
}

/*
 * 当我们被 ProcSignalBarrier 命令释放所有已打开文件时，调用本例程。
 */
bool
ProcessBarrierSmgrRelease(void)
{
	smgrreleaseall();
	return true;
}

/*
 * 将 IO 句柄的目标设为 smgr，并初始化所有相关的数据部分。
 */
void
pgaio_io_set_target_smgr(PgAioHandle *ioh,
						 SMgrRelationData *smgr,
						 ForkNumber forknum,
						 BlockNumber blocknum,
						 int nblocks,
						 bool skip_fsync)
{
	PgAioTargetData *sd = pgaio_io_get_target_data(ioh);

	pgaio_io_set_target(ioh, PGAIO_TID_SMGR);

	/* 后端由 IO 拥有者隐含指定 */
	sd->smgr.rlocator = smgr->smgr_rlocator.locator;
	sd->smgr.forkNum = forknum;
	sd->smgr.blockNum = blocknum;
	sd->smgr.nblocks = nblocks;
	sd->smgr.is_temp = SmgrIsTemp(smgr);
	/* 临时关系绝不应被 fsync */
	sd->smgr.skip_fsync = skip_fsync && !SmgrIsTemp(smgr);
}

/*
 * smgr AIO 目标的回调，用于重新打开文件（例如因为 IO 是在某个
 * worker 中执行的）。
 */
static void
smgr_aio_reopen(PgAioHandle *ioh)
{
	PgAioTargetData *sd = pgaio_io_get_target_data(ioh);
	PgAioOpData *od = pgaio_io_get_op_data(ioh);
	SMgrRelation reln;
	ProcNumber	procno;
	uint32		off;

	/*
	 * 调用者需要阻止中断被处理，否则该 FD 可能在我们要执行 IO 之前
	 * 又被关闭。
	 */
	Assert(!INTERRUPTS_CAN_BE_PROCESSED());

	if (sd->smgr.is_temp)
		procno = pgaio_io_get_owner(ioh);
	else
		procno = INVALID_PROC_NUMBER;

	reln = smgropen(sd->smgr.rlocator, procno);
	switch (pgaio_io_get_op(ioh))
	{
		case PGAIO_OP_INVALID:
			pg_unreachable();
			break;
		case PGAIO_OP_READV:
			od->read.fd = smgrfd(reln, sd->smgr.forkNum, sd->smgr.blockNum, &off);
			Assert(off == od->read.offset);
			break;
		case PGAIO_OP_WRITEV:
			od->write.fd = smgrfd(reln, sd->smgr.forkNum, sd->smgr.blockNum, &off);
			Assert(off == od->write.offset);
			break;
	}
}

/*
 * smgr AIO 目标的回调，用于描述 IO 的目标。
 */
static char *
smgr_aio_describe_identity(const PgAioTargetData *sd)
{
	RelPathStr	path;
	char	   *desc;

	path = relpathbackend(sd->smgr.rlocator,
						  sd->smgr.is_temp ?
						  MyProcNumber : INVALID_PROC_NUMBER,
						  sd->smgr.forkNum);

	if (sd->smgr.nblocks == 0)
		desc = psprintf(_("file \"%s\""), path.str);
	else if (sd->smgr.nblocks == 1)
		desc = psprintf(_("block %u in file \"%s\""),
						sd->smgr.blockNum,
						path.str);
	else
		desc = psprintf(_("blocks %u..%u in file \"%s\""),
						sd->smgr.blockNum,
						sd->smgr.blockNum + sd->smgr.nblocks - 1,
						path.str);

	return desc;
}
