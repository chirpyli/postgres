/*-------------------------------------------------------------------------
 *
 * bufmgr.h
 *	  POSTGRES 缓冲区管理器定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/bufmgr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUFMGR_H
#define BUFMGR_H

#include "port/pg_iovec.h"
#include "storage/aio_types.h"
#include "storage/block.h"
#include "storage/buf.h"
#include "storage/bufpage.h"
#include "storage/relfilelocator.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"

typedef void *Block;

/*
 * GetAccessStrategy() 可能的参数。
 *
 * 如果新增一种 BufferAccessStrategyType，还需新增一个 IOContext，
 * 以便使用该策略的 I/O 统计被跟踪。
 */
typedef enum BufferAccessStrategyType
{
	BAS_NORMAL,					/* 普通随机访问 */
	BAS_BULKREAD,				/* 大型只读扫描（允许 hint bit 更新） */
	BAS_BULKWRITE,				/* 大型多块写入（例如 COPY IN） */
	BAS_VACUUM,					/* VACUUM */
} BufferAccessStrategyType;

/* ReadBufferExtended() 可能的模式 */
typedef enum
{
	RBM_NORMAL,					/* 普通读取 */
	RBM_ZERO_AND_LOCK,			/* 不从磁盘读取，由调用者初始化，
								 * 同时锁定页面 */
	RBM_ZERO_AND_CLEANUP_LOCK,	/* 类似于 RBM_ZERO_AND_LOCK，但以
								 * "cleanup" 模式锁定页面 */
	RBM_ZERO_ON_ERROR,			/* 读取，但出错时返回全零页面 */
	RBM_NORMAL_NO_LOG,			/* 在 WAL 重放期间不将页面记录为无效；
								 * 其他与普通 RBM_NORMAL 相同 */
} ReadBufferMode;

/*
 * PrefetchBuffer() 返回的类型。
 */
typedef struct PrefetchBufferResult
{
	Buffer		recent_buffer;	/* 若有效，则为命中（需重新检查！） */
	bool		initiated_io;	/* 若为真，则为未命中并触发异步 I/O */
} PrefetchBufferResult;

/*
 * 影响 ExtendBufferedRel* 行为的标志位
 */
typedef enum ExtendBufferedFlags
{
	/*
	 * 不获取扩展锁。这仅在关系非共享、持有访问排他锁，或当前为启动进程时才是安全的。
	 */
	EB_SKIP_EXTENSION_LOCK = (1 << 0),

	/* 该扩展是否属于恢复过程的一部分？ */
	EB_PERFORMING_RECOVERY = (1 << 1),

	/*
	 * 如果 fork 当前不存在，是否应当创建它？这很可能只对关系 fork 有意义。
	 */
	EB_CREATE_FORK_IF_NEEDED = (1 << 2),

	/* 第一个（也可能是唯一的）返回的缓冲区是否应当加锁返回？ */
	EB_LOCK_FIRST = (1 << 3),

	/* 是否应当清除 smgr 的大小缓存？ */
	EB_CLEAR_SIZE_CACHE = (1 << 4),

	/* 以下为内部标志 */
	EB_LOCK_TARGET = (1 << 5),
}			ExtendBufferedFlags;

/*
 * 某些函数通过 relation 或 smgr + relpersistence 来标识关系。通过下面的
 * BMR_REL()/BMR_SMGR() 宏使用。这使我们能对相关恢复和正常操作使用同一个函数。
 */
typedef struct BufferManagerRelation
{
	Relation	rel;
	struct SMgrRelationData *smgr;
	char		relpersistence;
} BufferManagerRelation;

#define BMR_REL(p_rel) ((BufferManagerRelation){.rel = p_rel})
#define BMR_SMGR(p_smgr, p_relpersistence) ((BufferManagerRelation){.smgr = p_smgr, .relpersistence = p_relpersistence})

/* 读取失败时清零页面。 */
#define READ_BUFFERS_ZERO_ON_ERROR (1 << 0)
/* 若需要 I/O 则调用 smgrprefetch()。 */
#define READ_BUFFERS_ISSUE_ADVICE (1 << 1)
/* 不将页面因校验和失败而视为无效。 */
#define READ_BUFFERS_IGNORE_CHECKSUM_FAILURES (1 << 2)
/* I/O 将立即被等待 */
#define READ_BUFFERS_SYNCHRONOUSLY (1 << 3)


struct ReadBuffersOperation
{
	/* 以下成员应由调用者设置。 */
	Relation	rel;			/* 可选 */
	struct SMgrRelationData *smgr;
	char		persistence;
	ForkNumber	forknum;
	BufferAccessStrategy strategy;

	/*
	 * 以下私有成员是 StartReadBuffers() 与 WaitReadBuffers() 之间通信用的私有状态，
	 * 仅在确实需要读取时才初始化，不应被修改。
	 */
	Buffer	   *buffers;
	BlockNumber blocknum;
	int			flags;
	int16		nblocks;
	int16		nblocks_done;
	PgAioWaitRef io_wref;
	PgAioReturn io_return;
};

typedef struct ReadBuffersOperation ReadBuffersOperation;

/* 前向声明，以避免在此处暴露 buf_internals.h */
struct WritebackContext;

/* 前向声明，以避免在此处包含 smgr.h */
struct SMgrRelationData;

/* 在 globals.c 中……这与 miscadmin.h 重复 */
extern PGDLLIMPORT int NBuffers;

/* 在 bufmgr.c 中 */
extern PGDLLIMPORT bool zero_damaged_pages;
extern PGDLLIMPORT int bgwriter_lru_maxpages;
extern PGDLLIMPORT double bgwriter_lru_multiplier;
extern PGDLLIMPORT bool track_io_timing;

#define DEFAULT_EFFECTIVE_IO_CONCURRENCY 16
#define DEFAULT_MAINTENANCE_IO_CONCURRENCY 16
extern PGDLLIMPORT int effective_io_concurrency;
extern PGDLLIMPORT int maintenance_io_concurrency;

#define MAX_IO_COMBINE_LIMIT PG_IOV_MAX
#define DEFAULT_IO_COMBINE_LIMIT Min(MAX_IO_COMBINE_LIMIT, (128 * 1024) / BLCKSZ)
extern PGDLLIMPORT int io_combine_limit;	/* 以下两个 GUC 中的较小值 */
extern PGDLLIMPORT int io_combine_limit_guc;
extern PGDLLIMPORT int io_max_combine_limit;

extern PGDLLIMPORT int checkpoint_flush_after;
extern PGDLLIMPORT int backend_flush_after;
extern PGDLLIMPORT int bgwriter_flush_after;

extern PGDLLIMPORT const PgAioHandleCallbacks aio_shared_buffer_readv_cb;
extern PGDLLIMPORT const PgAioHandleCallbacks aio_local_buffer_readv_cb;

/* 在 buf_init.c 中 */
extern PGDLLIMPORT char *BufferBlocks;

/* 在 localbuf.c 中 */
extern PGDLLIMPORT int NLocBuffer;
extern PGDLLIMPORT Block *LocalBufferBlockPointers;
extern PGDLLIMPORT int32 *LocalRefCount;

/* effective_io_concurrency 的上限 */
#define MAX_IO_CONCURRENCY 1000

/* ReadBuffer() 的特殊块号 */
#define P_NEW	InvalidBlockNumber	/* 扩展文件以获得新页面 */

/*
 * 缓冲区内容锁模式（LockBuffer() 的 mode 参数）
 */
#define BUFFER_LOCK_UNLOCK		0
#define BUFFER_LOCK_SHARE		1
#define BUFFER_LOCK_EXCLUSIVE	2


/*
 * bufmgr.c 中函数的原型
 */
extern PrefetchBufferResult PrefetchSharedBuffer(struct SMgrRelationData *smgr_reln,
												 ForkNumber forkNum,
												 BlockNumber blockNum);
extern PrefetchBufferResult PrefetchBuffer(Relation reln, ForkNumber forkNum,
										   BlockNumber blockNum);
extern bool ReadRecentBuffer(RelFileLocator rlocator, ForkNumber forkNum,
							 BlockNumber blockNum, Buffer recent_buffer);
extern Buffer ReadBuffer(Relation reln, BlockNumber blockNum);
extern Buffer ReadBufferExtended(Relation reln, ForkNumber forkNum,
								 BlockNumber blockNum, ReadBufferMode mode,
								 BufferAccessStrategy strategy);
extern Buffer ReadBufferWithoutRelcache(RelFileLocator rlocator,
										ForkNumber forkNum, BlockNumber blockNum,
										ReadBufferMode mode, BufferAccessStrategy strategy,
										bool permanent);

extern bool StartReadBuffer(ReadBuffersOperation *operation,
							Buffer *buffer,
							BlockNumber blocknum,
							int flags);
extern bool StartReadBuffers(ReadBuffersOperation *operation,
							 Buffer *buffers,
							 BlockNumber blockNum,
							 int *nblocks,
							 int flags);
extern void WaitReadBuffers(ReadBuffersOperation *operation);

extern void ReleaseBuffer(Buffer buffer);
extern void UnlockReleaseBuffer(Buffer buffer);
extern bool BufferIsExclusiveLocked(Buffer buffer);
extern bool BufferIsDirty(Buffer buffer);
extern void MarkBufferDirty(Buffer buffer);
extern void IncrBufferRefCount(Buffer buffer);
extern void CheckBufferIsPinnedOnce(Buffer buffer);
extern Buffer ReleaseAndReadBuffer(Buffer buffer, Relation relation,
								   BlockNumber blockNum);

extern Buffer ExtendBufferedRel(BufferManagerRelation bmr,
								ForkNumber forkNum,
								BufferAccessStrategy strategy,
								uint32 flags);
extern BlockNumber ExtendBufferedRelBy(BufferManagerRelation bmr,
									   ForkNumber fork,
									   BufferAccessStrategy strategy,
									   uint32 flags,
									   uint32 extend_by,
									   Buffer *buffers,
									   uint32 *extended_by);
extern Buffer ExtendBufferedRelTo(BufferManagerRelation bmr,
								  ForkNumber fork,
								  BufferAccessStrategy strategy,
								  uint32 flags,
								  BlockNumber extend_to,
								  ReadBufferMode mode);

extern void InitBufferManagerAccess(void);
extern void AtEOXact_Buffers(bool isCommit);
#ifdef USE_ASSERT_CHECKING
extern void AssertBufferLocksPermitCatalogRead(void);
#endif
extern char *DebugPrintBufferRefcount(Buffer buffer);
extern void CheckPointBuffers(int flags);
extern BlockNumber BufferGetBlockNumber(Buffer buffer);
extern BlockNumber RelationGetNumberOfBlocksInFork(Relation relation,
												   ForkNumber forkNum);
extern void FlushOneBuffer(Buffer buffer);
extern void FlushRelationBuffers(Relation rel);
extern void FlushRelationsAllBuffers(struct SMgrRelationData **smgrs, int nrels);
extern void CreateAndCopyRelationData(RelFileLocator src_rlocator,
									  RelFileLocator dst_rlocator,
									  bool permanent);
extern void FlushDatabaseBuffers(Oid dbid);
extern void DropRelationBuffers(struct SMgrRelationData *smgr_reln,
								ForkNumber *forkNum,
								int nforks, BlockNumber *firstDelBlock);
extern void DropRelationsAllBuffers(struct SMgrRelationData **smgr_reln,
									int nlocators);
extern void DropDatabaseBuffers(Oid dbid);

#define RelationGetNumberOfBlocks(reln) \
	RelationGetNumberOfBlocksInFork(reln, MAIN_FORKNUM)

extern bool BufferIsPermanent(Buffer buffer);
extern XLogRecPtr BufferGetLSNAtomic(Buffer buffer);
extern void BufferGetTag(Buffer buffer, RelFileLocator *rlocator,
						 ForkNumber *forknum, BlockNumber *blknum);

extern void MarkBufferDirtyHint(Buffer buffer, bool buffer_std);

extern void UnlockBuffers(void);
extern void LockBuffer(Buffer buffer, int mode);
extern bool ConditionalLockBuffer(Buffer buffer);
extern void LockBufferForCleanup(Buffer buffer);
extern bool ConditionalLockBufferForCleanup(Buffer buffer);
extern bool IsBufferCleanupOK(Buffer buffer);
extern bool HoldingBufferPinThatDelaysRecovery(void);

extern bool BgBufferSync(struct WritebackContext *wb_context);

extern uint32 GetPinLimit(void);
extern uint32 GetLocalPinLimit(void);
extern uint32 GetAdditionalPinLimit(void);
extern uint32 GetAdditionalLocalPinLimit(void);
extern void LimitAdditionalPins(uint32 *additional_pins);
extern void LimitAdditionalLocalPins(uint32 *additional_pins);

extern bool EvictUnpinnedBuffer(Buffer buf, bool *buffer_flushed);
extern void EvictAllUnpinnedBuffers(int32 *buffers_evicted,
									int32 *buffers_flushed,
									int32 *buffers_skipped);
extern void EvictRelUnpinnedBuffers(Relation rel,
									int32 *buffers_evicted,
									int32 *buffers_flushed,
									int32 *buffers_skipped);

/* 在 buf_init.c 中 */
extern void BufferManagerShmemInit(void);
extern Size BufferManagerShmemSize(void);

/* 在 localbuf.c 中 */
extern void AtProcExit_LocalBuffers(void);

/* 在 freelist.c 中 */

extern BufferAccessStrategy GetAccessStrategy(BufferAccessStrategyType btype);
extern BufferAccessStrategy GetAccessStrategyWithSize(BufferAccessStrategyType btype,
													  int ring_size_kb);
extern int	GetAccessStrategyBufferCount(BufferAccessStrategy strategy);
extern int	GetAccessStrategyPinLimit(BufferAccessStrategy strategy);

extern void FreeAccessStrategy(BufferAccessStrategy strategy);


/* 内联函数 */

/*
 * 虽然这个头文件名义上仅用于后端，但某些前端程序（如 pg_waldump）也会包含它。
 * 对于即使函数未被使用也会发出 static inline 函数的编译器，这会导致未满足的
 * 外部引用；因此用 #ifndef FRONTEND 将这些函数隐藏起来。
 */

#ifndef FRONTEND

/*
 * BufferIsValid
 *		给定的缓冲区号合法（无论共享缓冲区还是本地缓冲区）时返回真。
 *
 * 注意：在很长一段时间里，它的定义与 BufferIsPinned 相同，也就是说如果
 * 你没有持有该缓冲区的 pin，它就会返回 False。我认为这是错误的，只会掩盖
 * 逻辑错误。代码应当始终知道自己是否持有缓冲区引用，而与 pin 状态无关。
 *
 * 注意：在更长的另一段时间里，它并不完全等同于 BufferIsInvalid() 宏的取反，
 * 因为它还会做合理性检查以验证缓冲区号在范围内。这个宏最初很可能只打算用于
 * 断言中，但后来使用范围扩大了很多，即便在非断言构建中执行这些检查的开销
 * 也可能相当可观。因此，我们现在已将这些范围检查降级为宏内部的断言。
 */
static inline bool
BufferIsValid(Buffer bufnum)
{
	Assert(bufnum <= NBuffers);
	Assert(bufnum >= -NLocBuffer);

	return bufnum != InvalidBuffer;
}

/*
 * BufferGetBlock
 *		返回与缓冲区关联的磁盘页映像的引用。
 *
 * 注意：
 *		假定缓冲区合法。
 */
static inline Block
BufferGetBlock(Buffer buffer)
{
	Assert(BufferIsValid(buffer));

	if (BufferIsLocal(buffer))
		return LocalBufferBlockPointers[-buffer - 1];
	else
		return (Block) (BufferBlocks + ((Size) (buffer - 1)) * BLCKSZ);
}

/*
 * BufferGetPageSize
 *		返回缓冲区内的页大小。
 *
 * 注意：
 *		假定缓冲区合法。
 *
 *		缓冲区可以是一个原始磁盘块，无需包含合法的（已格式化的）磁盘页。
 */
/* XXX 应从缓冲区描述符中获取 */
static inline Size
BufferGetPageSize(Buffer buffer)
{
	Assert(BufferIsValid(buffer));
	return (Size) BLCKSZ;
}

/*
 * BufferGetPage
 *		返回与缓冲区关联的页面。
 */
static inline Page
BufferGetPage(Buffer buffer)
{
	return (Page) BufferGetBlock(buffer);
}

#endif							/* FRONTEND */

#endif							/* BUFMGR_H */
