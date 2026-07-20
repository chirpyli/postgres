/*-------------------------------------------------------------------------
 *
 * bufmgr.c
 *	  缓冲区管理器接口例程
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/bufmgr.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * 主要入口点：
 *
 * ReadBuffer() —— 查找或创建持有请求页面的缓冲区，并对其加 pin，
 *		使得在本进程使用它期间，任何其他进程都无法销毁它。
 *
 * StartReadBuffer() —— 同上，但将等待步骤独立出来
 * StartReadBuffers() —— 多数据块版本
 * WaitReadBuffers() —— 上述调用的第二步
 *
 * ReleaseBuffer() —— 解除缓冲区的 pin
 *
 * MarkBufferDirty() —— 将已加 pin 的缓冲区内容标记为“脏”。
 *		磁盘写入会延迟到缓冲区被替换或检查点时进行。
 *
 * 另见以下文件：
 *		freelist.c —— 为缓冲区替换选择牺牲者（victim）
 *		buf_table.c —— 管理缓冲区查找表
 */
#include "postgres.h"

#include <sys/file.h>
#include <unistd.h>

#include "access/tableam.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#ifdef USE_ASSERT_CHECKING
#include "catalog/pg_tablespace_d.h"
#endif
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "executor/instrument.h"
#include "lib/binaryheap.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "postmaster/bgwriter.h"
#include "storage/aio.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/read_stream.h"
#include "storage/smgr.h"
#include "storage/standby.h"
#include "utils/memdebug.h"
#include "utils/ps_status.h"
#include "utils/rel.h"
#include "utils/resowner.h"
#include "utils/timestamp.h"


/* 注意：这两个宏只能用于共享缓冲区，不能用于本地缓冲区！ */
#define BufHdrGetBlock(bufHdr)	((Block) (BufferBlocks + ((Size) (bufHdr)->buf_id) * BLCKSZ))
#define BufferGetLSN(bufHdr)	(PageGetLSN(BufHdrGetBlock(bufHdr)))

/* 注意：这个宏只能用于本地缓冲区，不能用于共享缓冲区！ */
#define LocalBufHdrGetBlock(bufHdr) \
	LocalBufferBlockPointers[-((bufHdr)->buf_id + 2)]

/* SyncOneBuffer 返回值中的标志位 */
#define BUF_WRITTEN				0x01
#define BUF_REUSABLE			0x02

#define RELS_BSEARCH_THRESHOLD		20

/*
 * 当缓冲区大小（以数据块数量计）超过该阈值时，我们会扫描整个缓冲池，
 * 以移除正在被删除的关系的所有页面对应的缓冲区。对于大小低于该阈值的关系，
 * 我们通过在 BufMapping 表中进行查找来定位缓冲区。
 */
#define BUF_DROP_FULL_SCAN_THRESHOLD		(uint64) (NBuffers / 32)

typedef struct PrivateRefCountEntry
{
	Buffer		buffer;
	int32		refcount;
} PrivateRefCountEntry;

/* 64 字节，约等于常见系统上一条缓存行的大小 */
#define REFCOUNT_ARRAY_ENTRIES 8

/*
 * 某个特定表空间待做检查点的缓冲区状态，由 BufferSync 内部使用。
 */
typedef struct CkptTsStatus
{
	/* 表空间的 oid */
	Oid			tsId;

	/*
	 * 该表空间的检查点进度。为了使各表空间之间的进度可比较，每个表空间的
	 * 进度被度量为介于 0 和待检查点页面总数之间的一个数值。在该表空间中
	 * 每完成一个页面的检查点，就使该表空间的进度增加 progress_slice。
	 */
	float8		progress;
	float8		progress_slice;

	/* 该表空间中待检查点的页面数量 */
	int			num_to_scan;
	/* 该表空间中已处理的页面数量 */
	int			num_scanned;

	/* 该表空间在 CkptBufferIds 中的当前偏移量 */
	int			index;
} CkptTsStatus;

/*
 * 用于对 SMgrRelation 进行排序的数组类型
 *
 * FlushRelationsAllBuffers 与 DropRelationsAllBuffers 共用同一个比较函数。
 * 指向该结构体和 RelFileLocator 的指针必须兼容。
 */
typedef struct SMgrSortArray
{
	RelFileLocator rlocator;	/* 必须是第一个成员 */
	SMgrRelation srel;
} SMgrSortArray;

/* GUC 变量 */
bool		zero_damaged_pages = false;
int			bgwriter_lru_maxpages = 100;
double		bgwriter_lru_multiplier = 2.0;
bool		track_io_timing = false;

/*
 * PrefetchBuffer 调用方应当尽量领先于其 ReadBuffer 调用的缓冲区数量。
 * 零表示“永不预取”。该值仅用于那些未设置 effective_io_concurrency
 * 参数的表空间中的缓冲区。
 */
int			effective_io_concurrency = DEFAULT_EFFECTIVE_IO_CONCURRENCY;

/*
 * 类似于 effective_io_concurrency，但用于维护代码路径；由于它们代表许多
 * 会话工作，因此可能受益于更高的设置。若表空间设置了同名参数，则会被覆盖。
 */
int			maintenance_io_concurrency = DEFAULT_MAINTENANCE_IO_CONCURRENCY;

/*
 * 单次 I/O 操作中应处理的块数上限。StartReadBuffers() 的调用方应当遵守它，
 * 其他直接调用 smgr API 的操作也应如此。它取底层 GUC 参数
 * io_combine_limit_guc 与 io_max_combine_limit 中的较小值。
 */
int			io_combine_limit = DEFAULT_IO_COMBINE_LIMIT;
int			io_combine_limit_guc = DEFAULT_IO_COMBINE_LIMIT;
int			io_max_combine_limit = DEFAULT_IO_COMBINE_LIMIT;

/*
 * 关于对写入的缓冲区触发内核回写的 GUC 变量；依赖于操作系统的默认值
 * 通过 GUC 机制设置。
 */
int			checkpoint_flush_after = DEFAULT_CHECKPOINT_FLUSH_AFTER;
int			bgwriter_flush_after = DEFAULT_BGWRITER_FLUSH_AFTER;
int			backend_flush_after = DEFAULT_BACKEND_FLUSH_AFTER;

/* LockBufferForCleanup 的本地状态 */
static BufferDesc *PinCountWaitBuf = NULL;

/*
 * 后端私有的引用计数管理：
 *
 * 每个缓冲区还有一个私有引用计数，用于记录当前进程对该缓冲区加 pin 的次数。
 * 这样一来，若某个后端对同一个缓冲区多次加 pin，共享引用计数只需修改一次。
 * 它也用于检查在事务结束时以及退出时是否仍有缓冲区处于加 pin 状态。
 *
 *
 * 为了避免——如我们曾经那样——需要一个包含 NBuffers 个条目的数组来跟踪
 * 本地缓冲区，我们使用一个顺序查找的小数组（PrivateRefCountArray）和一个
 * 溢出哈希表（PrivateRefCountHash）来跟踪后端的本地 pin。
 *
 * 只要同时处于加 pin 状态的缓冲区数量不超过 REFCOUNT_ARRAY_ENTRIES，
 * 所有引用计数都保存在数组中；超过之后，新数组条目会将旧条目挤入哈希表。
 * 这样一来，频繁使用的条目就不会“卡”在哈希表中，而让不常用的条目塞满数组。
 *
 * 注意，在大多数场景下，处于加 pin 状态的缓冲区数量不会超过
 * REFCOUNT_ARRAY_ENTRIES。
 *
 *
 * 要将一个缓冲区登记到引用计数跟踪机制中，首先使用
 * ReservePrivateRefCountEntry() 预留一个空闲条目，之后必要时再用
 * NewPrivateRefCountEntry() 填充它。这种拆分使我们可以避免在
 * NewPrivateRefCountEntry() 中进行内存分配，这有时很重要，因为在某些场景下
 * 它是在持有自旋锁的情况下被调用的……
 */
static struct PrivateRefCountEntry PrivateRefCountArray[REFCOUNT_ARRAY_ENTRIES];
static HTAB *PrivateRefCountHash = NULL;
static int32 PrivateRefCountOverflowed = 0;
static uint32 PrivateRefCountClock = 0;
static PrivateRefCountEntry *ReservedRefCountEntry = NULL;

static uint32 MaxProportionalPins;

static void ReservePrivateRefCountEntry(void);
static PrivateRefCountEntry *NewPrivateRefCountEntry(Buffer buffer);
static PrivateRefCountEntry *GetPrivateRefCountEntry(Buffer buffer, bool do_move);
static inline int32 GetPrivateRefCount(Buffer buffer);
static void ForgetPrivateRefCountEntry(PrivateRefCountEntry *ref);

/* ResourceOwner 回调函数，用于持有进行中的 I/O 和缓冲区 pin */
static void ResOwnerReleaseBufferIO(Datum res);
static char *ResOwnerPrintBufferIO(Datum res);
static void ResOwnerReleaseBufferPin(Datum res);
static char *ResOwnerPrintBufferPin(Datum res);

const ResourceOwnerDesc buffer_io_resowner_desc =
{
	.name = "buffer io",
	.release_phase = RESOURCE_RELEASE_BEFORE_LOCKS,
	.release_priority = RELEASE_PRIO_BUFFER_IOS,
	.ReleaseResource = ResOwnerReleaseBufferIO,
	.DebugPrint = ResOwnerPrintBufferIO
};

const ResourceOwnerDesc buffer_pin_resowner_desc =
{
	.name = "buffer pin",
	.release_phase = RESOURCE_RELEASE_BEFORE_LOCKS,
	.release_priority = RELEASE_PRIO_BUFFER_PINS,
	.ReleaseResource = ResOwnerReleaseBufferPin,
	.DebugPrint = ResOwnerPrintBufferPin
};

/*
 * 确保 PrivateRefCountArray 有足够的空间再存储一个条目。必须在使用
 * NewPrivateRefCountEntry() 填充新条目之前调用它——但完全可以不使用
 * 预留的条目。
 */
static void
ReservePrivateRefCountEntry(void)
{
	/* 已经预留（或已释放），无需操作 */
	if (ReservedRefCountEntry != NULL)
		return;

	/*
	 * 首先在数组中查找空闲条目，在大多数情况下这就足够了。
	 */
	{
		int			i;

		for (i = 0; i < REFCOUNT_ARRAY_ENTRIES; i++)
		{
			PrivateRefCountEntry *res;

			res = &PrivateRefCountArray[i];

			if (res->buffer == InvalidBuffer)
			{
				ReservedRefCountEntry = res;
				return;
			}
		}
	}

	/*
	 * 没有找到。所有数组条目都已满。将一个数组条目移入哈希表。
	 */
	{
		/*
		 * 将数组中当前 clock 位置处的条目移入哈希表。使用该槽位。
		 */
		PrivateRefCountEntry *hashent;
		bool		found;

		/* 选择牺牲者槽位 */
		ReservedRefCountEntry =
			&PrivateRefCountArray[PrivateRefCountClock++ % REFCOUNT_ARRAY_ENTRIES];

		/* 它应当已被使用，否则我们就不应该走到这里。 */
		Assert(ReservedRefCountEntry->buffer != InvalidBuffer);

		/* 将牺牲的数组条目登记到哈希表中 */
		hashent = hash_search(PrivateRefCountHash,
							  &(ReservedRefCountEntry->buffer),
							  HASH_ENTER,
							  &found);
		Assert(!found);
		hashent->refcount = ReservedRefCountEntry->refcount;

		/* 清空现在空闲的数组槽位 */
		ReservedRefCountEntry->buffer = InvalidBuffer;
		ReservedRefCountEntry->refcount = 0;

		PrivateRefCountOverflowed++;
	}
}

/*
 * 填充一个先前已预留的引用计数条目。
 */
static PrivateRefCountEntry *
NewPrivateRefCountEntry(Buffer buffer)
{
	PrivateRefCountEntry *res;

	/* 仅允许在已做出预留后调用 */
	Assert(ReservedRefCountEntry != NULL);

	/* 使用掉预留的条目 */
	res = ReservedRefCountEntry;
	ReservedRefCountEntry = NULL;

	/* 并填充它 */
	res->buffer = buffer;
	res->refcount = 0;

	return res;
}

/*
 * 返回传入缓冲区的 PrivateRefCount 条目。
 *
 * 若缓冲区没有引用计数条目，则返回 NULL。否则，如果 do_move 为真，
 * 且条目位于哈希表中，则通过将它移入数组来优化其频繁访问。
 */
static PrivateRefCountEntry *
GetPrivateRefCountEntry(Buffer buffer, bool do_move)
{
	PrivateRefCountEntry *res;
	int			i;

	Assert(BufferIsValid(buffer));
	Assert(!BufferIsLocal(buffer));

	/*
	 * 首先在数组中查找引用，在大多数情况下这就足够了。
	 */
	for (i = 0; i < REFCOUNT_ARRAY_ENTRIES; i++)
	{
		res = &PrivateRefCountArray[i];

		if (res->buffer == buffer)
			return res;
	}

	/*
	 * 走到这里，我们知道如果该缓冲区已处于加 pin 状态，它并不在数组中。
	 *
	 * 仅当我们之前已经溢出到哈希表时，才在哈希表中查找该缓冲区。
	 */
	if (PrivateRefCountOverflowed == 0)
		return NULL;

	res = hash_search(PrivateRefCountHash, &buffer, HASH_FIND, NULL);

	if (res == NULL)
		return NULL;
	else if (!do_move)
	{
		/* 调用方不希望我们将哈希条目移入数组 */
		return res;
	}
	else
	{
		/* 将缓冲区从哈希表移入空闲的数组槽位 */
		bool		found;
		PrivateRefCountEntry *free;

		/* 确保有一个空闲的数组槽位 */
		ReservePrivateRefCountEntry();

		/* 使用掉预留的槽位 */
		Assert(ReservedRefCountEntry != NULL);
		free = ReservedRefCountEntry;
		ReservedRefCountEntry = NULL;
		Assert(free->buffer == InvalidBuffer);

		/* 并填充它 */
		free->buffer = buffer;
		free->refcount = res->refcount;

		/* 从哈希表中删除 */
		hash_search(PrivateRefCountHash, &buffer, HASH_REMOVE, &found);
		Assert(found);
		Assert(PrivateRefCountOverflowed > 0);
		PrivateRefCountOverflowed--;

		return free;
	}
}

/*
 * 返回传入的缓冲区被此后端加 pin 的次数。
 *
 * Only works for shared memory buffers!
 */
static inline int32
GetPrivateRefCount(Buffer buffer)
{
	PrivateRefCountEntry *ref;

	Assert(BufferIsValid(buffer));
	Assert(!BufferIsLocal(buffer));

	/*
	 * Not moving the entry - that's ok for the current users, but we might
	 * want to change this one day.
	 */
	ref = GetPrivateRefCountEntry(buffer, false);

	if (ref == NULL)
		return 0;
	return ref->refcount;
}

/*
 * 释放用于跟踪缓冲区引用计数的资源，该缓冲区我们已经不再持有其 pin，
 * 也不打算立即再次加 pin。
 */
static void
ForgetPrivateRefCountEntry(PrivateRefCountEntry *ref)
{
	Assert(ref->refcount == 0);

	if (ref >= &PrivateRefCountArray[0] &&
		ref < &PrivateRefCountArray[REFCOUNT_ARRAY_ENTRIES])
	{
		ref->buffer = InvalidBuffer;

		/*
		 * 将刚刚使用的条目标记为已预留——在许多场景下，这让我们
		 * 无需再去数组/哈希表中搜索空闲条目。
		 */
		ReservedRefCountEntry = ref;
	}
	else
	{
		bool		found;
		Buffer		buffer = ref->buffer;

		hash_search(PrivateRefCountHash, &buffer, HASH_REMOVE, &found);
		Assert(found);
		Assert(PrivateRefCountOverflowed > 0);
		PrivateRefCountOverflowed--;
	}
}

/*
 * BufferIsPinned
 *		当且仅当缓冲区被加 pin 时为真（同时检查缓冲区编号是否有效）。
 *
 *		注意：我们在此检查的是 *本* 后端是否持有该缓冲区的 pin。
 *		我们不关心其他后端是否持有。
 */
#define BufferIsPinned(bufnum) \
( \
	!BufferIsValid(bufnum) ? \
		false \
	: \
		BufferIsLocal(bufnum) ? \
			(LocalRefCount[-(bufnum) - 1] > 0) \
		: \
	(GetPrivateRefCount(bufnum) > 0) \
)


static Buffer ReadBuffer_common(Relation rel,
								SMgrRelation smgr, char smgr_persistence,
								ForkNumber forkNum, BlockNumber blockNum,
								ReadBufferMode mode, BufferAccessStrategy strategy);
static BlockNumber ExtendBufferedRelCommon(BufferManagerRelation bmr,
										   ForkNumber fork,
										   BufferAccessStrategy strategy,
										   uint32 flags,
										   uint32 extend_by,
										   BlockNumber extend_upto,
										   Buffer *buffers,
										   uint32 *extended_by);
static BlockNumber ExtendBufferedRelShared(BufferManagerRelation bmr,
										   ForkNumber fork,
										   BufferAccessStrategy strategy,
										   uint32 flags,
										   uint32 extend_by,
										   BlockNumber extend_upto,
										   Buffer *buffers,
										   uint32 *extended_by);
static bool PinBuffer(BufferDesc *buf, BufferAccessStrategy strategy);
static void PinBuffer_Locked(BufferDesc *buf);
static void UnpinBuffer(BufferDesc *buf);
static void UnpinBufferNoOwner(BufferDesc *buf);
static void BufferSync(int flags);
static uint32 WaitBufHdrUnlocked(BufferDesc *buf);
static int	SyncOneBuffer(int buf_id, bool skip_recently_used,
						  WritebackContext *wb_context);
static void WaitIO(BufferDesc *buf);
static void AbortBufferIO(Buffer buffer);
static void shared_buffer_write_error_callback(void *arg);
static void local_buffer_write_error_callback(void *arg);
static inline BufferDesc *BufferAlloc(SMgrRelation smgr,
									  char relpersistence,
									  ForkNumber forkNum,
									  BlockNumber blockNum,
									  BufferAccessStrategy strategy,
									  bool *foundPtr, IOContext io_context);
static bool AsyncReadBuffers(ReadBuffersOperation *operation, int *nblocks_progress);
static void CheckReadBuffersOperation(ReadBuffersOperation *operation, bool is_complete);
static Buffer GetVictimBuffer(BufferAccessStrategy strategy, IOContext io_context);
static void FlushBuffer(BufferDesc *buf, SMgrRelation reln,
						IOObject io_object, IOContext io_context);
static void FindAndDropRelationBuffers(RelFileLocator rlocator,
									   ForkNumber forkNum,
									   BlockNumber nForkBlock,
									   BlockNumber firstDelBlock);
static void RelationCopyStorageUsingBuffer(RelFileLocator srclocator,
										   RelFileLocator dstlocator,
										   ForkNumber forkNum, bool permanent);
static void AtProcExit_Buffers(int code, Datum arg);
static void CheckForBufferLeaks(void);
#ifdef USE_ASSERT_CHECKING
static void AssertNotCatalogBufferLock(LWLock *lock, LWLockMode mode,
									   void *unused_context);
#endif
static int	rlocator_comparator(const void *p1, const void *p2);
static inline int buffertag_comparator(const BufferTag *ba, const BufferTag *bb);
static inline int ckpt_buforder_comparator(const CkptSortItem *a, const CkptSortItem *b);
static int	ts_ckpt_progress_comparator(Datum a, Datum b, void *arg);


/*
 * PrefetchBuffer() 针对共享缓冲区的实现。
 */
PrefetchBufferResult
PrefetchSharedBuffer(SMgrRelation smgr_reln,
					 ForkNumber forkNum,
					 BlockNumber blockNum)
{
	PrefetchBufferResult result = {InvalidBuffer, false};
	BufferTag	newTag;			/* 所请求块的标识 */
	uint32		newHash;		/* newTag 的哈希值 */
	LWLock	   *newPartitionLock;	/* 该块对应的缓冲区分区锁 */
	int			buf_id;

	Assert(BlockNumberIsValid(blockNum));

	/* 创建一个标签，以便查找缓冲区 */
	InitBufferTag(&newTag, &smgr_reln->smgr_rlocator.locator,
				  forkNum, blockNum);

	/* 确定其哈希码和分区锁 ID */
	newHash = BufTableHashCode(&newTag);
	newPartitionLock = BufMappingPartitionLock(newHash);

	/* 查看该块是否已经在缓冲池中 */
	LWLockAcquire(newPartitionLock, LW_SHARED);
	buf_id = BufTableLookup(&newTag, newHash);
	LWLockRelease(newPartitionLock);

	/* If not in buffers, initiate prefetch */
	if (buf_id < 0)
	{
#ifdef USE_PREFETCH
		/*
		 * 尝试发起一次异步读。在恢复过程中，如果关系文件不存在，
		 * 则返回 false。
		 */
		if ((io_direct_flags & IO_DIRECT_DATA) == 0 &&
			smgrprefetch(smgr_reln, forkNum, blockNum, 1))
		{
			result.initiated_io = true;
		}
#endif							/* USE_PREFETCH */
	}
	else
	{
		/*
		 * 报告该块当时所处的缓冲区。调用方或许可以借此避免一次缓冲区表
		 * 查找，但此时它并未被加 pin，因此必须重新检查！
		 */
		result.recent_buffer = buf_id + 1;
	}

	/*
	 * 如果该块 *确实* 在缓冲中，我们什么也不做。这其实并不理想：
	 * 该块可能马上就要被驱逐出去，而我们知道很快就要用到它，这就很愚蠢。
	 * 但目前唯一的简单办法是提高 usage_count，而这似乎也不是个好方案：
	 * 当调用方最终访问该块时，usage_count 会再次被提高，导致处于预取
	 * 序列中的块受到过多偏袒。真正的修复需要增加一些额外的每缓冲区状态，
	 * 但目前并不清楚问题是否严重到需要这样做。
	 */

	return result;
}

/*
 * PrefetchBuffer —— 发起对关系某个块的异步读取
 *
 * 其命名类比于 ReadBuffer，但实际上并不分配缓冲区。相反，它试图确保
 * 将来对该给定块的 ReadBuffer 调用不会被 I/O 延迟。预取是可选的。
 *
 * 有三种可能的结果：
 *
 * 1.  如果该块已被缓存，结果中会包含一个有效的缓冲区，调用方可以利用它
 * 来避免后续的缓冲区查找，但它并未被加 pin，因此调用方必须重新检查它。
 *
 * 2.  如果已经请求内核发起 I/O，则 initiated_io 成员为真。目前无法得知
 * 数据是否已被内核缓存从而实际上并未真正发起 I/O，也无法得知 I/O 何时
 * 完成，除非使用同步的 ReadBuffer()。
 *
 * 3.  否则，说明该块尚未被 PostgreSQL 缓存，并且：未定义 USE_PREFETCH
 * （此构建因缺乏内核设施而不支持预取）、启用了直接 I/O，或者底层关系文件
 * 未找到且我们正处于恢复过程中。（如果关系文件未找到且我们不在恢复过程中，
 * 则会报错。）
 */
PrefetchBufferResult
PrefetchBuffer(Relation reln, ForkNumber forkNum, BlockNumber blockNum)
{
	Assert(RelationIsValid(reln));
	Assert(BlockNumberIsValid(blockNum));

	if (RelationUsesLocalBuffers(reln))
	{
		/* 见 ReadBuffer_common 中的注释 */
		if (RELATION_IS_OTHER_TEMP(reln))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot access temporary tables of other sessions")));

		/* 转交给 localbuf.c 处理 */
		return PrefetchLocalBuffer(RelationGetSmgr(reln), forkNum, blockNum);
	}
	else
	{
		/* 转交给共享缓冲区版本处理 */
		return PrefetchSharedBuffer(RelationGetSmgr(reln), forkNum, blockNum);
	}
}

/*
 * ReadRecentBuffer —— 尝试在某个最近观察到的缓冲区中加 pin 一个块
 *
 * 与 ReadBuffer() 相比，成功时它避免了一次缓冲区映射查找。如果缓冲区
 * 有效且仍然具有期望的标签，则返回 true。在此情况下，缓冲区被加 pin，
 * 使用计数被提高。
 */
bool
ReadRecentBuffer(RelFileLocator rlocator, ForkNumber forkNum, BlockNumber blockNum,
				 Buffer recent_buffer)
{
	BufferDesc *bufHdr;
	BufferTag	tag;
	uint32		buf_state;
	bool		have_private_ref;

	Assert(BufferIsValid(recent_buffer));

	ResourceOwnerEnlarge(CurrentResourceOwner);
	ReservePrivateRefCountEntry();
	InitBufferTag(&tag, &rlocator, forkNum, blockNum);

	if (BufferIsLocal(recent_buffer))
	{
		int			b = -recent_buffer - 1;

		bufHdr = GetLocalBufferDescriptor(b);
		buf_state = pg_atomic_read_u32(&bufHdr->state);

		/* 它是否仍然有效且持有正确的标签？ */
		if ((buf_state & BM_VALID) && BufferTagsEqual(&tag, &bufHdr->tag))
		{
			PinLocalBuffer(bufHdr, true);

			pgBufferUsage.local_blks_hit++;

			return true;
		}
	}
	else
	{
		bufHdr = GetBufferDescriptor(recent_buffer - 1);
		have_private_ref = GetPrivateRefCount(recent_buffer) > 0;

		/*
		 * 我们是否已经通过私有引用将该缓冲区加 pin？如果是，那它必然是
		 * 有效的，并且可以不加锁地检查标签。如果不是，我们必须先锁住
		 * 头部，然后再检查。
		 */
		if (have_private_ref)
			buf_state = pg_atomic_read_u32(&bufHdr->state);
		else
			buf_state = LockBufHdr(bufHdr);

		if ((buf_state & BM_VALID) && BufferTagsEqual(&tag, &bufHdr->tag))
		{
			/*
			 * 现在加 pin 该缓冲区是安全的。我们不能先加 pin 再问问题，
			 * 因为如果给一个随机不匹配的缓冲区加了 pin，可能会扰乱像
			 * InvalidateBuffer() 这样的代码路径。
			 */
			if (have_private_ref)
				PinBuffer(bufHdr, NULL);	/* 提高 pin 计数 */
			else
				PinBuffer_Locked(bufHdr);	/* 首次加 pin */

			pgBufferUsage.shared_blks_hit++;

			return true;
		}

		/* 如果前面锁住了头部，现在解锁。 */
		if (!have_private_ref)
			UnlockBufHdr(bufHdr, buf_state);
	}

	return false;
}

/*
 * ReadBuffer —— ReadBufferExtended 的简写形式，用于以 RBM_NORMAL 模式
 *		和默认策略从主 fork 读取。
 */
Buffer
ReadBuffer(Relation reln, BlockNumber blockNum)
{
	return ReadBufferExtended(reln, MAIN_FORKNUM, blockNum, RBM_NORMAL, NULL);
}

/*
 * ReadBufferExtended —— 返回一个包含所请求关系指定块的缓冲区。
 *		如果请求的块号为 P_NEW，则扩展关系文件并分配一个新块。
 *		（调用方负责确保同一时刻只有一个后端尝试扩展关系！）
 *
 * 返回值：包含所读取块的缓冲区的缓冲区编号。返回的缓冲区已被加 pin。
 *		出错时不返回——而是 elog。
 *
 * 假定调用此函数时，reln 已经打开。
 *
 * 在 RBM_NORMAL 模式下，页面从磁盘读取，并对页面头进行校验。
 * 若页面头无效则抛出错误。（但请注意，全零页面被视为“有效”，
 * 见 PageIsVerified()。）
 *
 * RBM_ZERO_ON_ERROR 与普通模式类似，但如果页面头无效，则清零该页
 * 而不是抛出错误。这用于非关键数据，调用方准备自行修复错误。
 *
 * 在 RBM_ZERO_AND_LOCK 模式下，如果页面尚未在缓冲区缓存中，则将其
 * 填零而不是从磁盘读取。当调用方准备从头填充页面时很有用，因为这
 * 能节省 I/O，并避免磁盘页面存在损坏页头时产生不必要的失败。返回的
 * 页面处于锁定状态，以确保调用方有机会在页面对其他进程可见之前
 * 对其进行初始化。注意：不要用此模式读取超出关系当前物理 EOF 的页面，
 * 否则当该页被修改并写出时，很可能在 md.c 中引发问题。不过 P_NEW 是安全的。
 *
 * RBM_ZERO_AND_CLEANUP_LOCK 与 RBM_ZERO_AND_LOCK 相同，但会对页面
 * 获取一个 cleanup 强度的锁。
 *
 * RBM_NORMAL_NO_LOG 模式在此处的处理方式与 RBM_NORMAL 相同。
 *
 * 如果 strategy 不为 NULL，则使用非默认的缓冲区访问策略。
 * 详见 buffer/README。
 */
inline Buffer
ReadBufferExtended(Relation reln, ForkNumber forkNum, BlockNumber blockNum,
				   ReadBufferMode mode, BufferAccessStrategy strategy)
{
	Buffer		buf;

	/*
	 * 读取缓冲区，并更新 pgstat 计数器以反映缓存命中或缺失。
	 * 跨会话临时关系的检查由 ReadBuffer_common() 负责执行。
	 */
	buf = ReadBuffer_common(reln, RelationGetSmgr(reln), 0,
							forkNum, blockNum, mode, strategy);

	return buf;
}


/*
 * ReadBufferWithoutRelcache —— 类似于 ReadBufferExtended，但不要求
 *		关系具有 relcache 条目。
 *
 * 对于 RELPERSISTENCE_PERMANENT 关系，传入 permanent = true；
 * 对于 RELPERSISTENCE_UNLOGGED 关系，传入 permanent = false。
 * 此函数不能用于临时关系（要让它支持临时关系可能很困难，除非我们只想
 * 以自身的 ProcNumber 读取临时关系）。
 */
Buffer
ReadBufferWithoutRelcache(RelFileLocator rlocator, ForkNumber forkNum,
						  BlockNumber blockNum, ReadBufferMode mode,
						  BufferAccessStrategy strategy, bool permanent)
{
	SMgrRelation smgr = smgropen(rlocator, INVALID_PROC_NUMBER);

	return ReadBuffer_common(NULL, smgr,
							 permanent ? RELPERSISTENCE_PERMANENT : RELPERSISTENCE_UNLOGGED,
							 forkNum, blockNum,
							 mode, strategy);
}

/*
 * ExtendBufferedRelBy() 的便捷封装，扩展一个数据块。
 */
Buffer
ExtendBufferedRel(BufferManagerRelation bmr,
				  ForkNumber forkNum,
				  BufferAccessStrategy strategy,
				  uint32 flags)
{
	Buffer		buf;
	uint32		extend_by = 1;

	ExtendBufferedRelBy(bmr, forkNum, strategy, flags, extend_by,
						&buf, &extend_by);

	return buf;
}

/*
 * Extend relation by multiple blocks.
 *
 * Tries to extend the relation by extend_by blocks. Depending on the
 * availability of resources the relation may end up being extended by a
 * smaller number of pages (unless an error is thrown, always by at least one
 * page). *extended_by is updated to the number of pages the relation has been
 * extended to.
 *
 * buffers needs to be an array that is at least extend_by long. Upon
 * completion, the first extend_by array elements will point to a pinned
 * buffer.
 *
 * If EB_LOCK_FIRST is part of flags, the first returned buffer is
 * locked. This is useful for callers that want a buffer that is guaranteed to
 * be empty.
 */
BlockNumber
ExtendBufferedRelBy(BufferManagerRelation bmr,
					ForkNumber fork,
					BufferAccessStrategy strategy,
					uint32 flags,
					uint32 extend_by,
					Buffer *buffers,
					uint32 *extended_by)
{
	Assert((bmr.rel != NULL) != (bmr.smgr != NULL));
	Assert(bmr.smgr == NULL || bmr.relpersistence != 0);
	Assert(extend_by > 0);

	if (bmr.smgr == NULL)
	{
		bmr.smgr = RelationGetSmgr(bmr.rel);
		bmr.relpersistence = bmr.rel->rd_rel->relpersistence;
	}

	return ExtendBufferedRelCommon(bmr, fork, strategy, flags,
								   extend_by, InvalidBlockNumber,
								   buffers, extended_by);
}

/*
 * 扩展关系，使其至少达到 extend_to 个块的大小，返回缓冲区
 * (extend_to - 1)。
 *
 * 这对于想要写入特定页面、而不管关系当前大小的调用方很有用
 * （例如对可见性映射和崩溃恢复很有用）。
 */
Buffer
ExtendBufferedRelTo(BufferManagerRelation bmr,
					ForkNumber fork,
					BufferAccessStrategy strategy,
					uint32 flags,
					BlockNumber extend_to,
					ReadBufferMode mode)
{
	BlockNumber current_size;
	uint32		extended_by = 0;
	Buffer		buffer = InvalidBuffer;
	Buffer		buffers[64];

	Assert((bmr.rel != NULL) != (bmr.smgr != NULL));
	Assert(bmr.smgr == NULL || bmr.relpersistence != 0);
	Assert(extend_to != InvalidBlockNumber && extend_to > 0);

	if (bmr.smgr == NULL)
	{
		bmr.smgr = RelationGetSmgr(bmr.rel);
		bmr.relpersistence = bmr.rel->rd_rel->relpersistence;
	}

	/*
	 * 如果需要，在文件不存在时创建它。如果
	 * smgr_cached_nblocks[fork] 为正数，则它必然存在，无需调用
	 * smgrexists。
	 */
	if ((flags & EB_CREATE_FORK_IF_NEEDED) &&
		(bmr.smgr->smgr_cached_nblocks[fork] == 0 ||
		 bmr.smgr->smgr_cached_nblocks[fork] == InvalidBlockNumber) &&
		!smgrexists(bmr.smgr, fork))
	{
		LockRelationForExtension(bmr.rel, ExclusiveLock);

		/* 重新检查，fork 可能已被并发创建 */
		if (!smgrexists(bmr.smgr, fork))
			smgrcreate(bmr.smgr, fork, flags & EB_PERFORMING_RECOVERY);

		UnlockRelationForExtension(bmr.rel, ExclusiveLock);
	}

	/*
	 * 如果请求了，则使大小缓存失效，以便 smgrnblocks 向内核查询。
	 */
	if (flags & EB_CLEAR_SIZE_CACHE)
		bmr.smgr->smgr_cached_nblocks[fork] = InvalidBlockNumber;

	/*
	 * 估算需要扩展多少页面。这样可以避免获取过多不必要的牺牲缓冲区。
	 */
	current_size = smgrnblocks(bmr.smgr, fork);

	/*
	 * 由于还没有其他进程能够查看页面内容，因此排他锁与 cleanup 强度锁
	 * 之间没有区别。注意，在向 ReadBuffer_common() 回退（读取缓冲区以
	 * 应对并发的关系扩展）时，我们传入的是原始模式。
	 */
	if (mode == RBM_ZERO_AND_LOCK || mode == RBM_ZERO_AND_CLEANUP_LOCK)
		flags |= EB_LOCK_TARGET;

	while (current_size < extend_to)
	{
		uint32		num_pages = lengthof(buffers);
		BlockNumber first_block;

		if ((uint64) current_size + num_pages > extend_to)
			num_pages = extend_to - current_size;

		first_block = ExtendBufferedRelCommon(bmr, fork, strategy, flags,
											  num_pages, extend_to,
											  buffers, &extended_by);

		current_size = first_block + extended_by;
		Assert(num_pages != 0 || current_size >= extend_to);

		for (uint32 i = 0; i < extended_by; i++)
		{
			if (first_block + i != extend_to - 1)
				ReleaseBuffer(buffers[i]);
			else
				buffer = buffers[i];
		}
	}

	/*
	 * 有可能另一个后端并发地扩展了关系。这种情况下就读取缓冲区。
	 *
	 * XXX：是否应该通过一个标志位来控制？
	 */
	if (buffer == InvalidBuffer)
	{
		Assert(extended_by == 0);
		buffer = ReadBuffer_common(bmr.rel, bmr.smgr, bmr.relpersistence,
								   fork, extend_to - 1, mode, strategy);
	}

	return buffer;
}

/*
 * 锁定并可选地清零一个缓冲区，作为 RBM_ZERO_AND_LOCK 或
 * RBM_ZERO_AND_CLEANUP_LOCK 实现的一部分。缓冲区必须已经被加 pin。
 * 如果缓冲区尚未有效，则将其清零并置为有效。
 */
static void
ZeroAndLockBuffer(Buffer buffer, ReadBufferMode mode, bool already_valid)
{
	BufferDesc *bufHdr;
	bool		need_to_zero;
	bool		isLocalBuf = BufferIsLocal(buffer);

	Assert(mode == RBM_ZERO_AND_LOCK || mode == RBM_ZERO_AND_CLEANUP_LOCK);

	if (already_valid)
	{
		/*
		 * 如果调用方已经知道缓冲区有效，我们可以跳过一些头部交互。
		 * 调用方只是想锁住缓冲区。
		 */
		need_to_zero = false;
	}
	else if (isLocalBuf)
	{
		/* 非共享缓冲区的简单情形。 */
		bufHdr = GetLocalBufferDescriptor(-buffer - 1);
		need_to_zero = StartLocalBufferIO(bufHdr, true, false);
	}
	else
	{
		/*
		 * 获取 BM_IO_IN_PROGRESS，或者发现 BM_VALID 已被并发设置。
		 * 即使我们并没有进行 I/O，这也能确保我们不会清零别人已加 pin 的
		 * 页面。仅靠排他内容锁是不够的，因为读者在确定某个元组可见后
		 * 是允许释放内容锁的（见 README 中的缓冲区访问规则）。
		 */
		bufHdr = GetBufferDescriptor(buffer - 1);
		need_to_zero = StartBufferIO(bufHdr, true, false);
	}

	if (need_to_zero)
	{
		memset(BufferGetPage(buffer), 0, BLCKSZ);

		/*
		 * 在将页面标记为有效之前获取缓冲区内容锁，以确保没有其他后端
		 * 在调用方有机会初始化该页面之前看到清零后的页面。
		 *
		 * 由于还没有其他进程能够查看页面内容，因此排他锁与 cleanup 强度锁
		 * 之间没有区别。（注意，此处不能使用 LockBuffer() 或
		 * LockBufferForCleanup()，因为它们会断言缓冲区已经有效。）
		 */
		if (!isLocalBuf)
			LWLockAcquire(BufferDescriptorGetContentLock(bufHdr), LW_EXCLUSIVE);

		/* 设置 BM_VALID，终止 I/O，并唤醒所有等待者 */
		if (isLocalBuf)
			TerminateLocalBufferIO(bufHdr, false, BM_VALID, false);
		else
			TerminateBufferIO(bufHdr, false, BM_VALID, true, false);
	}
	else if (!isLocalBuf)
	{
		/*
		 * 缓冲区有效，因此我们无法将其清零。但调用方仍然期望返回的
		 * 页面处于锁定状态。
		 */
		if (mode == RBM_ZERO_AND_LOCK)
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		else
			LockBufferForCleanup(buffer);
	}
}

/*
 * 为给定块加 pin 一个缓冲区。如果块已经存在，则将 *foundPtr 设为 true；
 * 如果需要更多工作来读取或清零它，则设为 false。
 */
static pg_attribute_always_inline Buffer
PinBufferForBlock(Relation rel,
				  SMgrRelation smgr,
				  char persistence,
				  ForkNumber forkNum,
				  BlockNumber blockNum,
				  BufferAccessStrategy strategy,
				  bool *foundPtr)
{
	BufferDesc *bufHdr;
	IOContext	io_context;
	IOObject	io_object;

	Assert(blockNum != P_NEW);

	/* 持久性应当在此之前已经设置好 */
	Assert((persistence == RELPERSISTENCE_TEMP ||
			persistence == RELPERSISTENCE_PERMANENT ||
			persistence == RELPERSISTENCE_UNLOGGED));

	if (persistence == RELPERSISTENCE_TEMP)
	{
		io_context = IOCONTEXT_NORMAL;
		io_object = IOOBJECT_TEMP_RELATION;
	}
	else
	{
		io_context = IOContextForStrategy(strategy);
		io_object = IOOBJECT_RELATION;
	}

	TRACE_POSTGRESQL_BUFFER_READ_START(forkNum, blockNum,
									   smgr->smgr_rlocator.locator.spcOid,
									   smgr->smgr_rlocator.locator.dbOid,
									   smgr->smgr_rlocator.locator.relNumber,
									   smgr->smgr_rlocator.backend);

	if (persistence == RELPERSISTENCE_TEMP)
	{
		bufHdr = LocalBufferAlloc(smgr, forkNum, blockNum, foundPtr);
		if (*foundPtr)
			pgBufferUsage.local_blks_hit++;
	}
	else
	{
		bufHdr = BufferAlloc(smgr, persistence, forkNum, blockNum,
							 strategy, foundPtr, io_context);
		if (*foundPtr)
			pgBufferUsage.shared_blks_hit++;
	}
	if (rel)
	{
		/*
		 * 虽然 pgBufferUsage 的“read”计数器只有在到达 WaitReadBuffers()
		 * 时才会增加（因此命中、以及被清零而非读取的缓冲区不计入），
		 * 但每关系的统计信息总是会将它们计入。
		 */
		pgstat_count_buffer_read(rel);
		if (*foundPtr)
			pgstat_count_buffer_hit(rel);
	}
	if (*foundPtr)
	{
		pgstat_count_io_op(io_object, io_context, IOOP_HIT, 1, 0);
		if (VacuumCostActive)
			VacuumCostBalance += VacuumCostPageHit;

		TRACE_POSTGRESQL_BUFFER_READ_DONE(forkNum, blockNum,
										  smgr->smgr_rlocator.locator.spcOid,
										  smgr->smgr_rlocator.locator.dbOid,
										  smgr->smgr_rlocator.locator.relNumber,
										  smgr->smgr_rlocator.backend,
										  true);
	}

	return BufferDescriptorGetBuffer(bufHdr);
}

/*
 * ReadBuffer_common —— 所有 ReadBuffer 变体的公共逻辑
 *
 * smgr 是必需的，rel 是可选的，除非使用 P_NEW。
 */
static pg_attribute_always_inline Buffer
ReadBuffer_common(Relation rel, SMgrRelation smgr, char smgr_persistence,
				  ForkNumber forkNum,
				  BlockNumber blockNum, ReadBufferMode mode,
				  BufferAccessStrategy strategy)
{
	ReadBuffersOperation operation;
	Buffer		buffer;
	int			flags;
	char		persistence;

	/*
	 * 拒绝读取非本地的临时关系；由于我们看不到拥有该关系会话的
	 * 本地缓冲区，很可能会读到错误的数据。这是执行该检查的标准位置，
	 * 覆盖了 ReadBufferExtended() 入口点以及任何其他提供了 Relation 的调用方。
	 */
	if (rel && RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary tables of other sessions")));

	/*
	 * 向后兼容路径，大多数代码应当改用 ExtendBufferedRel()，
	 * 因为在 ExtendBufferedRel() 内部获取扩展锁的可扩展性要好得多。
	 */
	if (unlikely(blockNum == P_NEW))
	{
		uint32		flags = EB_SKIP_EXTENSION_LOCK;

		/*
		 * 由于还没有其他进程能够查看页面内容，因此排他锁与
		 * cleanup 强度锁之间没有区别。
		 */
		if (mode == RBM_ZERO_AND_LOCK || mode == RBM_ZERO_AND_CLEANUP_LOCK)
			flags |= EB_LOCK_FIRST;

		return ExtendBufferedRel(BMR_REL(rel), forkNum, strategy, flags);
	}

	if (rel)
		persistence = rel->rd_rel->relpersistence;
	else
		persistence = smgr_persistence;

	if (unlikely(mode == RBM_ZERO_AND_CLEANUP_LOCK ||
				 mode == RBM_ZERO_AND_LOCK))
	{
		bool		found;

		buffer = PinBufferForBlock(rel, smgr, persistence,
								   forkNum, blockNum, strategy, &found);
		ZeroAndLockBuffer(buffer, mode, found);
		return buffer;
	}

	/*
	 * 表明我们将立即等待。如果我们立即等待，那么实际异步执行 I/O
	 * 并没有好处，那只会徒增调度开销。
	 */
	flags = READ_BUFFERS_SYNCHRONOUSLY;
	if (mode == RBM_ZERO_ON_ERROR)
		flags |= READ_BUFFERS_ZERO_ON_ERROR;
	operation.smgr = smgr;
	operation.rel = rel;
	operation.persistence = persistence;
	operation.forknum = forkNum;
	operation.strategy = strategy;
	if (StartReadBuffer(&operation,
						&buffer,
						blockNum,
						flags))
		WaitReadBuffers(&operation);

	return buffer;
}

static pg_attribute_always_inline bool
StartReadBuffersImpl(ReadBuffersOperation *operation,
					 Buffer *buffers,
					 BlockNumber blockNum,
					 int *nblocks,
					 int flags,
					 bool allow_forwarding)
{
	int			actual_nblocks = *nblocks;
	int			maxcombine = 0;
	bool		did_start_io;

	Assert(*nblocks == 1 || allow_forwarding);
	Assert(*nblocks > 0);
	Assert(*nblocks <= MAX_IO_COMBINE_LIMIT);

	/* 见 ReadBuffer_common 中的注释 */
	if (operation->rel && RELATION_IS_OTHER_TEMP(operation->rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary tables of other sessions")));

	for (int i = 0; i < actual_nblocks; ++i)
	{
		bool		found;

		if (allow_forwarding && buffers[i] != InvalidBuffer)
		{
			BufferDesc *bufHdr;

			/*
			 * 这是一个由先前对 StartReadBuffers() 的调用加 pin 的缓冲区，
			 * 但当时无法在一次操作中处理完。该操作被拆分，调用方将一个
			 * 已经加 pin 的缓冲区传回给我们，以处理该操作的剩余部分。
			 * 它必须在期望的块号处继续。
			 */
			Assert(BufferGetBlockNumber(buffers[i]) == blockNum + i);

			/*
			 * 它可能是一个已经有效的缓冲区（命中），紧跟在先前一次 I/O
			 * （未命中）的最后一个连续块之后，标志着该 I/O 的结束；或者
			 * 是某个其他后端已经通过替我们执行 I/O 而使其变为有效的缓冲区，
			 * 这种情况下我们现在可以把它当作命中来处理。使用 relaxed 加载
			 * 来检查 BM_VALID 标志是安全的，因为我们在上一次调用中加 pin
			 * 时已经获得过它的一个新视图。
			 *
			 * 另一方面，如果我们尚未看到 BM_VALID，那它必然是一次被
			 * 上一次调用拆分的 I/O，我们需要尝试从该块开始一次新的 I/O。
			 * 我们也在与其他可能启动 I/O、甚至在此检查后将它标记为
			 * BM_VALID 的后端竞争，但这些情况由 StartBufferIO() 处理。
			 */
			if (BufferIsLocal(buffers[i]))
				bufHdr = GetLocalBufferDescriptor(-buffers[i] - 1);
			else
				bufHdr = GetBufferDescriptor(buffers[i] - 1);
			Assert(pg_atomic_read_u32(&bufHdr->state) & BM_TAG_VALID);
			found = pg_atomic_read_u32(&bufHdr->state) & BM_VALID;
		}
		else
		{
			buffers[i] = PinBufferForBlock(operation->rel,
										   operation->smgr,
										   operation->persistence,
										   operation->forknum,
										   blockNum + i,
										   operation->strategy,
										   &found);
		}

		if (found)
		{
			/*
			 * 命中了。如果它是请求范围内第一个块，我们可以立即返回它，
			 * 并报告无需调用 WaitReadBuffers()。如果 *nblocks 的初始值更大，
			 * 调用方需要再次调用以处理其余部分。
			 */
			if (i == 0)
			{
				*nblocks = 1;

#ifdef USE_ASSERT_CHECKING

				/*
				 * 初始化 ReadBuffersOperation 中足够的部分，使
				 * CheckReadBuffersOperation() 能够工作。在断言之外，当
				 * 未发起 I/O 时并不需要这样做。
				 */
				operation->buffers = buffers;
				operation->blocknum = blockNum;
				operation->nblocks = 1;
				operation->nblocks_done = 1;
				CheckReadBuffersOperation(operation, true);
#endif
				return false;
			}

			/*
			 * 否则，我们已经有一个要执行的 I/O，但这个块已经有效，
			 * 因此无法包含进来。在此处拆开 I/O。在此块之后可能还有
			 * 也可能没有更多需要 I/O 的块，我们尚未检查，但它们不可能
			 * 以相邻的方式与该块连续。我们将保持该缓冲区处于加 pin 状态，
			 * 并将其转发给下一次调用，从而避免在此处解除 pin 并在
			 * 下一次调用中重新加 pin 的需要。
			 */
			actual_nblocks = i;
			break;
		}
		else
		{
			/*
			 * 检查我们能以同一次 I/O 覆盖多少个块。例如，smgr 的
			 * 实现可能因为段边界而受到限制。
			 */
			if (i == 0 && actual_nblocks > 1)
			{
				maxcombine = smgrmaxcombine(operation->smgr,
											operation->forknum,
											blockNum);
				if (unlikely(maxcombine < actual_nblocks))
				{
					elog(DEBUG2, "limiting nblocks at %u from %u to %u",
						 blockNum, actual_nblocks, maxcombine);
					actual_nblocks = maxcombine;
				}
			}
		}
	}
	*nblocks = actual_nblocks;

	/* 填充 I/O 所需的信息。 */
	operation->buffers = buffers;
	operation->blocknum = blockNum;
	operation->flags = flags;
	operation->nblocks = actual_nblocks;
	operation->nblocks_done = 0;
	pgaio_wref_clear(&operation->io_wref);

	/*
	 * 使用 AIO 时，在后台启动 I/O。否则，如果调用方需要，则发起
	 * 预取请求。
	 *
	 * 我们在此为 IOMETHOD_SYNC 保留一条专用路径，是为了在一定程度上
	 * 降低引入 AIO 的风险。这是一项庞大的架构性变更，有很多机会
	 * 产生未预料到的性能影响。
	 *
	 * 使用 IOMETHOD_SYNC 本身就已经意味着不会真正异步地执行 I/O，
	 * 但如果没有此处的检查，我们会比以往更早地执行 I/O。最终这条
	 * IOMETHOD_SYNC 专用路径应当被移除。
	 */
	if (io_method != IOMETHOD_SYNC)
	{
		/*
		 * 尝试异步启动 I/O。如果另一个后端已经执行了该 I/O，则
		 * 有可能不需要再启动 I/O。
		 *
		 * 注意，即使启动了 I/O，它也可能无法覆盖整个请求的
		 * 范围，例如因为中间某个块已经被另一个后端读取了。这种情况下，
		 * 我们上面已经加 pin 的任何“尾部”缓冲区都会被 read_stream.c
		 * 转发给下一次对 StartReadBuffers() 的调用。
		 *
		 * 通过将 *nblocks 递减 *并且* 减少 operation->nblocks 来向
		 * 调用方发出信号。后者在此处完成，但在 WaitReadBuffers() 中
		 * 不会，因为在 WaitReadBuffers() 中我们无法再“缩短”整体的
		 * 读取大小，我们需要重试直到整体完成或失败。
		 */
		did_start_io = AsyncReadBuffers(operation, nblocks);

		operation->nblocks = *nblocks;
	}
	else
	{
		operation->flags |= READ_BUFFERS_SYNCHRONOUSLY;

		if (flags & READ_BUFFERS_ISSUE_ADVICE)
		{
			/*
			 * 理论上，我们应当仅在 PinBufferForBlock() 必须在上面分配
			 * 新缓冲区时才这样做。这样的话，如果在 WaitReadBuffers()
			 * 之前对相同的块发起了两次 StartReadBuffers() 调用，只有
			 * 第一次才会发出建议。那会是对于真正异步 I/O 更好的模拟
			 * （真正的异步 I/O 只会启动一次 I/O），但为简单起见
			 * 此处并未这样做。
			 */
			smgrprefetch(operation->smgr,
						 operation->forknum,
						 blockNum,
						 actual_nblocks);
		}

		/*
		 * 表明应当调用 WaitReadBuffers()。WaitReadBuffers() 将发起
		 * 必要的 I/O。
		 */
		did_start_io = true;
	}

	CheckReadBuffersOperation(operation, !did_start_io);

	return did_start_io;
}

/*
 * 开始读取从 blockNum 起始、延伸 *nblocks 个块的一块连续范围。
 * *nblocks 和 buffers 数组都是输入输出参数。进入时，由 *nblocks 覆盖的
 * buffers 元素必须持有 InvalidBuffer，或者持有由先前一次被拆分、现在
 * 正在继续的 StartReadBuffers() 调用所转发的缓冲区。返回时，*nblocks
 * 持有本操作接受的块数。如果它小于原始数量，说明本操作被拆分了，
 * 但直到原始请求大小为止的缓冲区元素可能持有用于继续操作的转发缓冲区。
 * 调用方要么从该调用所接受块的紧接后续块开始一次新的 I/O 并将那些
 * 缓冲区传回，要么在它选择不继续时释放它们。它不应对转发缓冲区
 * 做其他用途或假设。
 *
 * 如果返回 false，则不需要 I/O，退出时由 *nblocks 覆盖的缓冲区有效
 * 并可供访问。如果返回 true，则已经启动一次 I/O，必须在退出时由
 * *nblocks 覆盖的缓冲区被访问之前，使用同一个 operation 对象调用
 * WaitReadBuffers()。与 operation 对象一起，调用方提供的缓冲区数组必须
 * 保持有效直到 WaitReadBuffers() 被调用，任何转发缓冲区也必须为
 * 继续调用而保留，除非它们被显式释放。
 */
bool
StartReadBuffers(ReadBuffersOperation *operation,
				 Buffer *buffers,
				 BlockNumber blockNum,
				 int *nblocks,
				 int flags)
{
	return StartReadBuffersImpl(operation, buffers, blockNum, nblocks, flags,
								true /* expect forwarded buffers */ );
}

/*
 * StartReadBuffers() 的单块版本。当从另一个翻译单元调用时，这可能
 * 节省几条指令，因为它专门针对 nblocks == 1 做了特化。
 *
 * 此版本不支持“转发”缓冲区：只读一个块时无法创建它们，且 *buffer
 * 在入口处被忽略。
 */
bool
StartReadBuffer(ReadBuffersOperation *operation,
				Buffer *buffer,
				BlockNumber blocknum,
				int flags)
{
	int			nblocks = 1;
	bool		result;

	result = StartReadBuffersImpl(operation, buffer, blocknum, &nblocks, flags,
								  false /* single block, no forwarding */ );
	Assert(nblocks == 1);		/* single block can't be short */

	return result;
}

/*
 * 对 ReadBuffersOperation 执行健全性检查。
 */
static void
CheckReadBuffersOperation(ReadBuffersOperation *operation, bool is_complete)
{
#ifdef USE_ASSERT_CHECKING
	Assert(operation->nblocks_done <= operation->nblocks);
	Assert(!is_complete || operation->nblocks == operation->nblocks_done);

	for (int i = 0; i < operation->nblocks; i++)
	{
		Buffer		buffer = operation->buffers[i];
		BufferDesc *buf_hdr = BufferIsLocal(buffer) ?
			GetLocalBufferDescriptor(-buffer - 1) :
			GetBufferDescriptor(buffer - 1);

		Assert(BufferGetBlockNumber(buffer) == operation->blocknum + i);
		Assert(pg_atomic_read_u32(&buf_hdr->state) & BM_TAG_VALID);

		if (i < operation->nblocks_done)
			Assert(pg_atomic_read_u32(&buf_hdr->state) & BM_VALID);
	}
#endif
}

/* ReadBuffersCanStartIO() 的辅助函数，避免重复 */
static inline bool
ReadBuffersCanStartIOOnce(Buffer buffer, bool nowait)
{
	if (BufferIsLocal(buffer))
		return StartLocalBufferIO(GetLocalBufferDescriptor(-buffer - 1),
								  true, nowait);
	else
		return StartBufferIO(GetBufferDescriptor(buffer - 1), true, nowait);
}

/*
 * AsyncReadBuffers 的辅助函数，尝试让缓冲区准备好进行 I/O。
 */
static inline bool
ReadBuffersCanStartIO(Buffer buffer, bool nowait)
{
		/*
		 * 如果此后端当前有已暂存的 I/O，我们需要在等待发起 I/O 的
		 * 权利之前先提交挂起的 I/O，以避免潜在的死锁（以及更常见的、
		 * 给其他后端带来不必要的延迟）。
		 */
		if (!nowait && pgaio_have_staged())
	{
		if (ReadBuffersCanStartIOOnce(buffer, true))
			return true;

		/*
		 * 遗憾的是，StartBufferIO() 返回 false 时无法区分缓冲区
		 * 已经有效与 I/O 已经在进行中这两种情况。由于 I/O 已经
		 * 在进行中相当罕见，这种做法似乎没问题。
		 */
		pgaio_submit_staged();
	}

	return ReadBuffersCanStartIOOnce(buffer, nowait);
}

/*
 * WaitReadBuffers() 的辅助函数，处理一次 readv 操作的结果，
 * 必要时报错。
 */
static void
ProcessReadBuffersResult(ReadBuffersOperation *operation)
{
	PgAioReturn *aio_ret = &operation->io_return;
	PgAioResultStatus rs = aio_ret->result.status;
	int			newly_read_blocks = 0;

	Assert(pgaio_wref_valid(&operation->io_wref));
	Assert(aio_ret->result.status != PGAIO_RS_UNKNOWN);

	/*
	 * SMGR 将成功读取的块数作为 I/O 操作的结果报告。因此我们可以
	 * 直接把它加到 ->nblocks_done 上。
	 */

	if (likely(rs != PGAIO_RS_ERROR))
		newly_read_blocks = aio_ret->result.result;

	if (rs == PGAIO_RS_ERROR || rs == PGAIO_RS_WARNING)
		pgaio_result_report(aio_ret->result, &aio_ret->target_data,
							rs == PGAIO_RS_ERROR ? ERROR : WARNING);
	else if (aio_ret->result.status == PGAIO_RS_PARTIAL)
	{
		/*
		 * 我们会重试，因此只向服务器日志发一条调试消息
		 * （在生产场景下甚至可能不发）。
		 */
		pgaio_result_report(aio_ret->result, &aio_ret->target_data, DEBUG1);
		elog(DEBUG3, "partial read, will retry");
	}

	Assert(newly_read_blocks > 0);
	Assert(newly_read_blocks <= MAX_IO_COMBINE_LIMIT);

	operation->nblocks_done += newly_read_blocks;

	Assert(operation->nblocks_done <= operation->nblocks);
}

void
WaitReadBuffers(ReadBuffersOperation *operation)
{
	PgAioReturn *aio_ret = &operation->io_return;
	IOContext	io_context;
	IOObject	io_object;

	if (operation->persistence == RELPERSISTENCE_TEMP)
	{
		io_context = IOCONTEXT_NORMAL;
		io_object = IOOBJECT_TEMP_RELATION;
	}
	else
	{
		io_context = IOContextForStrategy(operation->strategy);
		io_object = IOOBJECT_RELATION;
	}

	/*
	 * 如果我们走到这里却没有发起过 I/O 操作，那必然是使用了
	 * io_method == IOMETHOD_SYNC 路径。否则调用方就不该调用
	 * WaitReadBuffers()。
	 *
	 * 在 IOMETHOD_SYNC 的情况下，我们——正如在引入 AIO 之前那样——
	 * 在 WaitReadBuffers() 中启动 I/O。这是作为下面重试逻辑的一部分
	 * 完成的，无需额外代码。
	 *
	 * 这条路径预计最终会消失。
	 */
	if (!pgaio_wref_valid(&operation->io_wref) && io_method != IOMETHOD_SYNC)
		elog(ERROR, "waiting for read operation that didn't read");

	/*
	 * 为了处理部分读取以及 IOMETHOD_SYNC，我们会重新发起 I/O 直到
	 * 完成。我们可能需要多次重试，不仅仅是因为可能遇到多次部分读取，
	 * 还因为剩余待读缓冲区中可能有一些已经被其他后端读入，
	 * 从而限制了 I/O 的大小。
	 */
	while (true)
	{
		int			ignored_nblocks_progress;

		CheckReadBuffersOperation(operation, false);

		/*
		 * 如果操作关联了 I/O，我们可能需要等待它。
		 */
		if (pgaio_wref_valid(&operation->io_wref))
		{
			/*
			 * 记录等待 I/O 完成所花费的时间。由于即使在并不需要
			 * 等待的情况下也记录等待
			 *
			 * a) 并不廉价，因为有时戳开销
			 *
			 * b) 会把一些时间报告为等待，即使我们从未等待过
			 *
			 * 因此我们先检查是否已经知道 I/O 已完成。
			 */
			if (aio_ret->result.status == PGAIO_RS_UNKNOWN &&
				!pgaio_wref_check_done(&operation->io_wref))
			{
				instr_time	io_start = pgstat_prepare_io_time(track_io_timing);

				pgaio_wref_wait(&operation->io_wref);

				/*
				 * I/O 操作本身已在早先的 AsyncReadBuffers() 中计入，
				 * 这里只记录等待时间。
				 */
				pgstat_count_io_op_time(io_object, io_context, IOOP_READ,
										io_start, 0, 0);
			}
			else
			{
				Assert(pgaio_wref_check_done(&operation->io_wref));
			}

			/*
			 * 现在我们确定 I/O 已完成。检查结果。这包括在出现
			 * 错误时报告错误。
			 */
			ProcessReadBuffersResult(operation);
		}

		/*
		 * 大多数情况下，我们已经启动的那一次 I/O 会读入所有内容。
		 * 但我们需要处理部分读取以及不再需要 I/O 的缓冲区。
		 */
		if (operation->nblocks_done == operation->nblocks)
			break;

		CHECK_FOR_INTERRUPTS();

		/*
		 * This may only complete the IO partially, either because some
		 * buffers were already valid, or because of a partial read.
		 *
		 * NB: In contrast to after the AsyncReadBuffers() call in
		 * StartReadBuffers(), we do *not* reduce
		 * ReadBuffersOperation->nblocks here, callers expect the full
		 * operation to be completed at this point (as more operations may
		 * have been queued).
		 */
		AsyncReadBuffers(operation, &ignored_nblocks_progress);
	}

	CheckReadBuffersOperation(operation, true);

	/* NB: READ_DONE tracepoint was already executed in completion callback */
}

/*
 * 为 ReadBuffersOperation 发起 I/O
 *
 * 本函数一次只启动单个 I/O。如果某个缓冲区已被并发读入，
 * I/O 的大小可能会被限制到小于待读块数。如果第一个待读缓冲区
 * 已经有效，则不会发起 I/O。
 *
 * 为了支持部分读取后的重试，前 operation->nblocks_done 个缓冲区会被跳过。
 *
 * 返回时，*nblocks_progress 会被更新以反映本次调用所影响的缓冲区数量。
 * 如果第一个缓冲区有效，则 *nblocks_progress 被设为 1，且
 * operation->nblocks_done 会递增。
 *
 * 如果发起了 I/O 则返回 true，如果不需要 I/O 则返回 false。
 */
static bool
AsyncReadBuffers(ReadBuffersOperation *operation, int *nblocks_progress)
{
	Buffer	   *buffers = &operation->buffers[0];
	int			flags = operation->flags;
	ForkNumber	forknum = operation->forknum;
	char		persistence = operation->persistence;
	int16		nblocks_done = operation->nblocks_done;
	BlockNumber blocknum = operation->blocknum + nblocks_done;
	Buffer	   *io_buffers = &operation->buffers[nblocks_done];
	int			io_buffers_len = 0;
	PgAioHandle *ioh;
	uint32		ioh_flags = 0;
	void	   *io_pages[MAX_IO_COMBINE_LIMIT];
	IOContext	io_context;
	IOObject	io_object;
	bool		did_start_io;

	/*
	 * 当此 I/O 被同步执行时（无论是因为调用方将立即阻塞等待 I/O，
	 * 还是因为使用了 IOMETHOD_SYNC），AIO 子系统都需要知道这一点。
	 */
	if (flags & READ_BUFFERS_SYNCHRONOUSLY)
		ioh_flags |= PGAIO_HF_SYNCHRONOUS;

	if (persistence == RELPERSISTENCE_TEMP)
	{
		io_context = IOCONTEXT_NORMAL;
		io_object = IOOBJECT_TEMP_RELATION;
		ioh_flags |= PGAIO_HF_REFERENCES_LOCAL;
	}
	else
	{
		io_context = IOContextForStrategy(operation->strategy);
		io_object = IOOBJECT_RELATION;
	}

	/*
	 * 如果启用了 zero_damaged_pages，则添加 READ_BUFFERS_ZERO_ON_ERROR
	 * 标志。这样做的原因是，希望 zero_damaged_pages 不是全局设置，
	 * 而是基于每个会话设置。完成回调函数可能在其他进程中运行
	 * （例如在 IO 工作进程中），其 zero_damaged_pages GUC 的值可能不同。
	 *
	 * XXX：我们最终可能应该为 zero_damaged_pages 使用不同的标志，
	 * 以便为 zero_damaged_pages 和 ZERO_ON_ERROR 报告不同的日志级别/错误码。
	 */
	if (zero_damaged_pages)
		flags |= READ_BUFFERS_ZERO_ON_ERROR;

	/*
	 * 出于与 zero_damaged_pages 相同的原因，我们需要使用此后端的
	 * ignore_checksum_failure 值。
	 */
	if (ignore_checksum_failure)
		flags |= READ_BUFFERS_IGNORE_CHECKSUM_FAILURES;


	/*
	 * 为了能够在本地完成回调中报告统计信息，我们现在就需要准备好
	 * 报告统计信息。这确保我们即使在临界区中也能安全地报告
	 * 校验和失败。
	 */
	pgstat_prepare_report_checksum_failure(operation->smgr->smgr_rlocator.locator.dbOid);

	/*
	 * 在 ReadBuffersCanStartIO() 之前获取 I/O 句柄，因为
	 * pgaio_io_acquire() 可能会阻塞，而我们不希望在设置
	 * IO_IN_PROGRESS 之后发生这种情况。
	 *
	 * 如果我们需要先等待 I/O 才能获得句柄，则先提交已经暂存的
	 * I/O，这样其他后端就无需等待。这里并不存在死锁风险，因为
	 * pgaio_io_acquire() 只需要等待已经提交的 I/O，而这不需要
	 * 额外的锁，但它仍可能引起不希望的等待。
	 *
	 * 一个附带好处是，这让我们能够测量 pgaio_io_acquire() 中的
	 * 时间，而不会对常见、非阻塞情形造成过多的计时器开销。
	 * 然而，目前 pgstats 基础设施并不真正支持这一点，因为它
	 * a) 断言一个操作不能在没有操作时拥有时间，b) 没有用于报告
	 * “累计”时间的 API。
	 */
	ioh = pgaio_io_acquire_nb(CurrentResourceOwner, &operation->io_return);
	if (unlikely(!ioh))
	{
		pgaio_submit_staged();

		ioh = pgaio_io_acquire(CurrentResourceOwner, &operation->io_return);
	}

	/*
	 * 检查我们是否能在第一个待读缓冲区上启动 I/O。
	 *
	 * 如果另一个后端已经有 I/O 在进行中，我们要等待其结果：
	 * 要么完成，要么出了问题我们将重试。
	 */
	if (!ReadBuffersCanStartIO(buffers[nblocks_done], false))
	{
		/*
		 * 别人已经完成了这个块，我们结束了。
		 *
		 * 当 I/O 必要时，->nblocks_done 在 ProcessReadBuffersResult()
		 * 中更新，但如果没有 I/O 则不会调用它。因此在此处更新。
		 */
		operation->nblocks_done += 1;
		*nblocks_progress = 1;

		pgaio_io_release(ioh);
		pgaio_wref_clear(&operation->io_wref);
		did_start_io = false;

		/*
		 * 将这作为此后端的“命中”来报告和统计，尽管它最初在
		 * PinBufferForBlock() 中必然是作为“未命中”开始的。
		 * 另一个后端会将其作为“读取”来统计。
		 */
		TRACE_POSTGRESQL_BUFFER_READ_DONE(forknum, blocknum,
										  operation->smgr->smgr_rlocator.locator.spcOid,
										  operation->smgr->smgr_rlocator.locator.dbOid,
										  operation->smgr->smgr_rlocator.locator.relNumber,
										  operation->smgr->smgr_rlocator.backend,
										  true);

		if (persistence == RELPERSISTENCE_TEMP)
			pgBufferUsage.local_blks_hit += 1;
		else
			pgBufferUsage.shared_blks_hit += 1;

		if (operation->rel)
			pgstat_count_buffer_hit(operation->rel);

		pgstat_count_io_op(io_object, io_context, IOOP_HIT, 1, 0);

		if (VacuumCostActive)
			VacuumCostBalance += VacuumCostPageHit;
	}
	else
	{
		instr_time	io_start;

		/* 我们找到了一个需要读入的缓冲区。 */
		Assert(io_buffers[0] == buffers[nblocks_done]);
		io_pages[0] = BufferGetBlock(buffers[nblocks_done]);
		io_buffers_len = 1;

		/*
		 * 我们在磁盘上相邻的多少个块可以同时散布读入其他缓冲区？
		 * 这种情况下，如果我们看到一个 I/O 已经在进行中，我们不会等待。
		 * 我们已经为头块设置了 BM_IO_IN_PROGRESS，因此我们应当
		 * 尽快推进那个 I/O。
		 */
		for (int i = nblocks_done + 1; i < operation->nblocks; i++)
		{
			if (!ReadBuffersCanStartIO(buffers[i], true))
				break;
			/* Must be consecutive block numbers. */
			Assert(BufferGetBlockNumber(buffers[i - 1]) ==
				   BufferGetBlockNumber(buffers[i]) - 1);
			Assert(io_buffers[io_buffers_len] == buffers[i]);

			io_pages[io_buffers_len++] = BufferGetBlock(buffers[i]);
		}

		/* 获取一个引用，以便在 WaitReadBuffers() 中等待 */
		pgaio_io_get_wref(ioh, &operation->io_wref);

		/* 将缓冲区列表提供给完成回调函数 */
		pgaio_io_set_handle_data_32(ioh, (uint32 *) io_buffers, io_buffers_len);

		pgaio_io_register_callbacks(ioh,
									persistence == RELPERSISTENCE_TEMP ?
									PGAIO_HCB_LOCAL_BUFFER_READV :
									PGAIO_HCB_SHARED_BUFFER_READV,
									flags);

		pgaio_io_set_flag(ioh, ioh_flags);

		/* ---
		 * 即使我们正尝试异步发起 I/O，也要在 smgrstartreadv() 中
		 * 记录时间：
		 * - 如果 io_method == IOMETHOD_SYNC，我们将总是立即执行 I/O
		 * - 该 io 方法可能不支持此 I/O（例如临时表的 worker I/O）
		 * ---
		 */
		io_start = pgstat_prepare_io_time(track_io_timing);
		smgrstartreadv(ioh, operation->smgr, forknum,
					   blocknum,
					   io_pages, io_buffers_len);
		pgstat_count_io_op_time(io_object, io_context, IOOP_READ,
								io_start, 1, io_buffers_len * BLCKSZ);

		if (persistence == RELPERSISTENCE_TEMP)
			pgBufferUsage.local_blks_read += io_buffers_len;
		else
			pgBufferUsage.shared_blks_read += io_buffers_len;

		/*
		 * 在发起 I/O 时统计 vacuum 代价，而不是在等待它之后。
		 * 否则我们可能在很短的时间跨度内发起大量 I/O，
		 * 即使代价上限很低。
		 */
		if (VacuumCostActive)
			VacuumCostBalance += VacuumCostPageMiss * io_buffers_len;

		*nblocks_progress = io_buffers_len;
		did_start_io = true;
	}

	return did_start_io;
}

/*
 * BufferAlloc —— PinBufferForBlock 的子例程。负责查找一个共享缓冲区。
 *		如果尚不存在该缓冲区，则选择一个替换牺牲者并驱逐旧页面，
 *		但并不会读入新页面。
 *
 * "strategy" 可以是一个缓冲区替换策略对象，或者为 NULL 表示
 * 使用默认策略。使用默认策略时，所选缓冲区的 usage_count 会
 * 被推进，否则可能不会（见 PinBuffer）。
 *
 * 返回的缓冲区已被加 pin，并且已经被标记为持有期望的页面。
 * 如果它本来就持有期望的页面，则 *foundPtr 被设为 true；
 * 否则 *foundPtr 被设为 false。
 *
 * io_context 作为输出参数传入，以避免在共享缓冲区命中且
 * 无需捕获 I/O 统计信息时调用 IOContextForStrategy()。
 *
 * 进入和退出时都不持有任何锁。
 */
static pg_attribute_always_inline BufferDesc *
BufferAlloc(SMgrRelation smgr, char relpersistence, ForkNumber forkNum,
			BlockNumber blockNum,
			BufferAccessStrategy strategy,
			bool *foundPtr, IOContext io_context)
{
	BufferTag	newTag;			/* identity of requested block */
	uint32		newHash;		/* hash value for newTag */
	LWLock	   *newPartitionLock;	/* buffer partition lock for it */
	int			existing_buf_id;
	Buffer		victim_buffer;
	BufferDesc *victim_buf_hdr;
	uint32		victim_buf_state;

	/* 确保我们有空间来记录缓冲区的 pin */
	ResourceOwnerEnlarge(CurrentResourceOwner);
	ReservePrivateRefCountEntry();

	/* 创建一个标签，以便查找缓冲区 */
	InitBufferTag(&newTag, &smgr->smgr_rlocator.locator, forkNum, blockNum);

	/* 确定其哈希码和分区锁 ID */
	newHash = BufTableHashCode(&newTag);
	newPartitionLock = BufMappingPartitionLock(newHash);

	/* 查看该块是否已经在缓冲池中 */
	LWLockAcquire(newPartitionLock, LW_SHARED);
	existing_buf_id = BufTableLookup(&newTag, newHash);
	if (existing_buf_id >= 0)
	{
		BufferDesc *buf;
		bool		valid;

		/*
		 * 找到了。现在加 pin 该缓冲区，这样任何人都无法将它
		 * 从缓冲池偷走，并检查正确的数据是否已经载入到缓冲区中。
		 */
		buf = GetBufferDescriptor(existing_buf_id);

		valid = PinBuffer(buf, strategy);

		/* 只要我们已加 pin，就可以释放映射锁 */
		LWLockRelease(newPartitionLock);

		*foundPtr = true;

		if (!valid)
		{
			/*
			 * 我们只会在这里出现，如果 (a) 别人仍在读取该页面，
			 * (b) 之前的读取尝试失败，或者 (c) 有人调用了
			 * StartReadBuffers() 但尚未调用 WaitReadBuffers()。
			 */
			*foundPtr = false;
		}

		return buf;
	}

	/*
	 * 在缓冲池中没找到。我们将需要初始化一个新缓冲区。
	 * 记得在干活的间隙释放映射锁。
	 */
	LWLockRelease(newPartitionLock);

	/*
	 * 获取一个牺牲缓冲区。别人也可能尝试做同样的事，我们
	 * 没有持有任何冲突的锁。如果是这样，我们将不得不在
	 * 稍后撤销我们的工作。
	 */
	victim_buffer = GetVictimBuffer(strategy, io_context);
	victim_buf_hdr = GetBufferDescriptor(victim_buffer - 1);

	/*
	 * 尝试在缓冲区的新标签下为其建立一个哈希表条目。如果
	 * 别人为该标签插入了另一个缓冲区，我们将释放所获得的
	 * 牺牲缓冲区，并使用已经插入的那个。
	 */
	LWLockAcquire(newPartitionLock, LW_EXCLUSIVE);
	existing_buf_id = BufTableInsert(&newTag, newHash, victim_buf_hdr->buf_id);
	if (existing_buf_id >= 0)
	{
		BufferDesc *existing_buf_hdr;
		bool		valid;

		/*
		 * 发生了冲突。已经有人做了我们正要做的事。
		 * 我们就当它是从一开始就在缓冲池中找到的那样处理。
		 * 首先，放弃我们原本打算使用的缓冲区。
		 *
		 * 我们本可以在释放分区锁之后再这样做，但那样我们就
		 * 不得不在获取锁之前调用 ResourceOwnerEnlarge() 和
		 * ReservePrivateRefCountEntry()，以应对这种罕见冲突。
		 */
		UnpinBuffer(victim_buf_hdr);

		/*
		 * 我们之前获取的牺牲缓冲区是干净且未使用的，让它能
		 * 被快速再次找到
		 */
		StrategyFreeBuffer(victim_buf_hdr);

		/* 其余代码应与本例程顶部的代码保持一致 */

		existing_buf_hdr = GetBufferDescriptor(existing_buf_id);

		valid = PinBuffer(existing_buf_hdr, strategy);

		/* 只要我们已加 pin，就可以释放映射锁 */
		LWLockRelease(newPartitionLock);

		*foundPtr = true;

		if (!valid)
		{
			/*
			 * 我们只会在这里出现，如果 (a) 别人仍在读取该页面，
			 * (b) 之前的读取尝试失败，或者 (c) 有人调用了
			 * StartReadBuffers() 但尚未调用 WaitReadBuffers()。
			 */
			*foundPtr = false;
		}

		return existing_buf_hdr;
	}

	/*
	 * 为了改变其标签，还需要锁住缓冲区头。
	 */
	victim_buf_state = LockBufHdr(victim_buf_hdr);

	/* 在我们持有缓冲区头锁期间做一些健全性检查 */
	Assert(BUF_STATE_GET_REFCOUNT(victim_buf_state) == 1);
	Assert(!(victim_buf_state & (BM_TAG_VALID | BM_VALID | BM_DIRTY | BM_IO_IN_PROGRESS)));

	victim_buf_hdr->tag = newTag;

	/*
	 * 确保对于必须在每次检查点写入的缓冲区设置 BM_PERMANENT。
	 * 未日志记录的缓冲区只需要在关闭检查点写入，但其“init”
	 * fork 除外，后者需要像永久关系一样被对待。
	 */
	victim_buf_state |= BM_TAG_VALID | BUF_USAGECOUNT_ONE;
	if (relpersistence == RELPERSISTENCE_PERMANENT || forkNum == INIT_FORKNUM)
		victim_buf_state |= BM_PERMANENT;

	UnlockBufHdr(victim_buf_hdr, victim_buf_state);

	LWLockRelease(newPartitionLock);

	/*
	 * 缓冲区内容当前无效。
	 */
	*foundPtr = false;

	return victim_buf_hdr;
}

/*
 * InvalidateBuffer —— 将一个共享缓冲区标记为无效，并将其归还给
 * 空闲链表。
 *
 * 进入时必须持有缓冲区头的自旋锁。我们在返回前释放它。
 * （这很合理，因为调用方必然是锁住了缓冲区，以此确认
 * 它应当被丢弃。）
 *
 * 这仅用于丢弃关系之类的场景。我们假设没有其他后端
 * 可能对使用此页面感兴趣，因此缓冲区被加 pin 的唯一原因
 * 是别人正在尝试将它写出。我们必须让他们完成之后才能
 * 回收该缓冲区。
 *
 * 在我们等待获取必要锁的过程中，该缓冲区可能被别人
 * 回收；如果是这样，不要把它弄乱。
 */
static void
InvalidateBuffer(BufferDesc *buf)
{
	BufferTag	oldTag;
	uint32		oldHash;		/* oldTag 的哈希值 */
	LWLock	   *oldPartitionLock;	/* 它对应的缓冲区分区锁 */
	uint32		oldFlags;
	uint32		buf_state;

	/* 在释放自旋锁之前保存原始的缓冲区标签 */
	oldTag = buf->tag;

	buf_state = pg_atomic_read_u32(&buf->state);
	Assert(buf_state & BM_LOCKED);
	UnlockBufHdr(buf, buf_state);

	/*
	 * 需要计算旧标签的哈希码和分区锁 ID。XXX：是否值得在
	 * BufferDesc 中保存哈希码，从而不必在此重新计算？
	 * 大概不值得。
	 */
	oldHash = BufTableHashCode(&oldTag);
	oldPartitionLock = BufMappingPartitionLock(oldHash);

retry:

	/*
	 * 获取排他映射锁，为改变缓冲区的关联做准备。
	 */
	LWLockAcquire(oldPartitionLock, LW_EXCLUSIVE);

	/* 重新锁住缓冲区头 */
	buf_state = LockBufHdr(buf);

	/* 如果我们在等待锁期间它发生了变化，则什么都不做 */
	if (!BufferTagsEqual(&buf->tag, &oldTag))
	{
		UnlockBufHdr(buf, buf_state);
		LWLockRelease(oldPartitionLock);
		return;
	}

	/*
	 * 我们假设它被加 pin 的原因是：要么我们之前在出错前
	 * 正在异步读取该页面，要么是别人正在将它刷出。
	 * 等待 I/O 完成。（如果引用计数被弄乱，这可能成为
	 * 死循环……最好能在一段时间后超时，但似乎无法保证
	 * 需要多少次循环。注意，如果对方已加 pin 该缓冲区
	 * 但尚未完成 StartBufferIO，WaitIO 会直接返回，我们
	 * 有效地在此处忙等待。）
	 */
	if (BUF_STATE_GET_REFCOUNT(buf_state) != 0)
	{
		UnlockBufHdr(buf, buf_state);
		LWLockRelease(oldPartitionLock);
		/* 安全检查：绝不应该是我们 *自己* 的 pin */
		if (GetPrivateRefCount(BufferDescriptorGetBuffer(buf)) > 0)
			elog(ERROR, "buffer is pinned in InvalidateBuffer");
		WaitIO(buf);
		goto retry;
	}

	/*
	 * 清除缓冲区的标签和标志位。我们必须这样做，以确保
	 * 对缓冲区数组的线性扫描不会认为该缓冲区有效。
	 */
	oldFlags = buf_state & BUF_FLAG_MASK;
	ClearBufferTag(&buf->tag);
	buf_state &= ~(BUF_FLAG_MASK | BUF_USAGECOUNT_MASK);
	UnlockBufHdr(buf, buf_state);

	/*
	 * 如果缓冲区在查找哈希表中，则将其移除。
	 */
	if (oldFlags & BM_TAG_VALID)
		BufTableDelete(&oldTag, oldHash);

	/*
	 * 映射锁处理完毕。
	 */
	LWLockRelease(oldPartitionLock);

	/*
	 * 将缓冲区插入到空闲缓冲区链表的头部。
	 */
	StrategyFreeBuffer(buf);
}

/*
 * GetVictimBuffer() 的辅助例程
 *
 * 需要在一个具有有效标签、已被加 pin、但并未持有缓冲区头
 * 自旋锁的缓冲区上调用。
 *
 * 如果缓冲区可以被重用则返回 true，此时该缓冲区仅被此后端
 * 加 pin 并标记为无效；否则返回 false。
 */
static bool
InvalidateVictimBuffer(BufferDesc *buf_hdr)
{
	uint32		buf_state;
	uint32		hash;
	LWLock	   *partition_lock;
	BufferTag	tag;

	Assert(GetPrivateRefCount(BufferDescriptorGetBuffer(buf_hdr)) == 1);

	/* 缓冲区已被加 pin，因此不加锁读取标签是安全的 */
	tag = buf_hdr->tag;

	hash = BufTableHashCode(&tag);
	partition_lock = BufMappingPartitionLock(hash);

	LWLockAcquire(partition_lock, LW_EXCLUSIVE);

	/* 锁住缓冲区头 */
	buf_state = LockBufHdr(buf_hdr);

	/*
	 * 我们已经加 pin 了该缓冲区，别人不应该能够并发地
	 * 清除它。
	 */
	Assert(buf_state & BM_TAG_VALID);
	Assert(BUF_STATE_GET_REFCOUNT(buf_state) > 0);
	Assert(BufferTagsEqual(&buf_hdr->tag, &tag));

	/*
	 * 如果此后有人加 pin 了该缓冲区，或者更糟，弄脏了它，
	 * 则放弃这个缓冲区：它显然正在被使用。
	 */
	if (BUF_STATE_GET_REFCOUNT(buf_state) != 1 || (buf_state & BM_DIRTY))
	{
		Assert(BUF_STATE_GET_REFCOUNT(buf_state) > 0);

		UnlockBufHdr(buf_hdr, buf_state);
		LWLockRelease(partition_lock);

		return false;
	}

	/*
	 * 清除缓冲区的标签、标志位和使用计数。这并非严格必需，
	 * 因为在对缓冲区做任何操作前都需要检查 BM_TAG_VALID/BM_VALID。
	 * 但目前这样做是有益的，因为对共享缓冲区多次线性扫描的
	 * 廉价预检查会用到标签（见例如 FlushDatabaseBuffers()）。
	 */
	ClearBufferTag(&buf_hdr->tag);
	buf_state &= ~(BUF_FLAG_MASK | BUF_USAGECOUNT_MASK);
	UnlockBufHdr(buf_hdr, buf_state);

	Assert(BUF_STATE_GET_REFCOUNT(buf_state) > 0);

	/* 最后从缓冲区映射表中删除缓冲区 */
	BufTableDelete(&tag, hash);

	LWLockRelease(partition_lock);

	Assert(!(buf_state & (BM_DIRTY | BM_VALID | BM_TAG_VALID)));
	Assert(BUF_STATE_GET_REFCOUNT(buf_state) > 0);
	Assert(BUF_STATE_GET_REFCOUNT(pg_atomic_read_u32(&buf_hdr->state)) > 0);

	return true;
}

static Buffer
GetVictimBuffer(BufferAccessStrategy strategy, IOContext io_context)
{
	BufferDesc *buf_hdr;
	Buffer		buf;
	uint32		buf_state;
	bool		from_ring;

	/*
	 * 在自旋锁尚未持有期间，确保有一个空闲的引用计数条目，
	 * 以及用于 pin 的 ResourceOwner 槽位。
	 */
	ReservePrivateRefCountEntry();
	ResourceOwnerEnlarge(CurrentResourceOwner);

	/* 如果预期的牺牲缓冲区被并发使用，则回到此处 */
again:

	/*
	 * 选择一个牺牲缓冲区。返回的缓冲区仍持有其头部自旋锁！
	 */
	buf_hdr = StrategyGetBuffer(strategy, &buf_state, &from_ring);
	buf = BufferDescriptorGetBuffer(buf_hdr);

	Assert(BUF_STATE_GET_REFCOUNT(buf_state) == 0);

	/* 加 pin 该缓冲区，然后释放缓冲区自旋锁 */
	PinBuffer_Locked(buf_hdr);

	/*
	 * 我们不应该对该缓冲区有任何其他 pin。
	 */
	CheckBufferIsPinnedOnce(buf);

	/*
	 * 如果缓冲区是脏的，尝试将它写出。这里存在一个竞态：
	 * 有人可能在我们上面释放缓冲区头锁之后弄脏它，甚至在我们
	 * 写出它期间（因为我们的共享锁无法阻止提示位的更新）。
	 * 我们将在重新锁住缓冲区头之后重新检查脏位。
	 */
	if (buf_state & BM_DIRTY)
	{
		LWLock	   *content_lock;

		Assert(buf_state & BM_TAG_VALID);
		Assert(buf_state & BM_VALID);

		/*
		 * We need a share-lock on the buffer contents to write it out (else
		 * we might write invalid data, eg because someone else is compacting
		 * 页面内容在我们写入期间）。我们必须在此使用条件获取锁
		 * 以避免死锁。即使 StrategyGetBuffer 返回该缓冲区时它
		 * 未被加 pin（因此必然未被锁住），等到我们到达此处时，
		 * 别人也可能已经加 pin 并对它加了排他锁。如果我们
		 * 无条件地获取锁，就会阻塞等待它们；如果它们随后又
		 * 阻塞等待我们，就会死锁。（这曾在实际中发生过：两个
		 * 后端同时尝试分裂 btree 索引页，而第二个恰好在
		 * 尝试分裂第一个从 StrategyGetBuffer 得到的那个页。）
		 */
		content_lock = BufferDescriptorGetContentLock(buf_hdr);
		if (!LWLockConditionalAcquire(content_lock, LW_SHARED))
		{
			/*
			 * 别人已经锁住了该缓冲区，因此放弃它并回到开头
			 * 去获取另一个。
			 */
			UnpinBuffer(buf_hdr);
			goto again;
		}

		/*
		 * 如果使用了非默认策略，并且写出该缓冲区需要 WAL 刷写，
		 * 则由策略来决定是继续写出/重用该缓冲区，还是选择
		 * 另一个牺牲者。我们需要检查页面 LSN，而这需要加锁，
		 * 因此无法在 StrategyGetBuffer 内部完成。
		 */
		if (strategy != NULL)
		{
			XLogRecPtr	lsn;

			/* 在持有缓冲区头锁期间读取 LSN */
			buf_state = LockBufHdr(buf_hdr);
			lsn = BufferGetLSN(buf_hdr);
			UnlockBufHdr(buf_hdr, buf_state);

			if (XLogNeedsFlush(lsn)
				&& StrategyRejectBuffer(strategy, buf_hdr, from_ring))
			{
				LWLockRelease(content_lock);
				UnpinBuffer(buf_hdr);
				goto again;
			}
		}

		/* 好的，执行 I/O */
		FlushBuffer(buf_hdr, NULL, IOOBJECT_RELATION, io_context);
		LWLockRelease(content_lock);

		ScheduleBufferTagForWriteback(&BackendWritebackContext, io_context,
									  &buf_hdr->tag);
	}


	if (buf_state & BM_VALID)
	{
		/*
		 * 当使用了 BufferAccessStrategy 时，从共享缓冲区被驱逐的
		 * 块会在对应上下文（例如 IOCONTEXT_BULKWRITE）中
		 * 计入 IOOP_EVICT。共享缓冲区被策略驱逐有两种情况：
		 * 1) 最初为策略环认领缓冲区时 2) 替换一个已有的
		 * 策略环缓冲区，因为它被加 pin 或正在使用中，无法重用。
		 *
		 * 从已经在策略环中的缓冲区被驱逐的块，会在对应策略
		 * 上下文中计入 IOOP_REUSE。
		 *
		 * 到了这一步，我们可以准确地统计驱逐和重用，因为
		 * 我们已经成功地认领了这个有效缓冲区。之前，我们可能
		 * 因为并发的 pin 或出错而被迫释放该缓冲区。
		 */
		pgstat_count_io_op(IOOBJECT_RELATION, io_context,
						   from_ring ? IOOP_REUSE : IOOP_EVICT, 1, 0);
	}

	/*
	 * 如果缓冲区在缓冲区映射表中有条目，则删除它。这可能
	 * 失败，因为另一个后端可能已加 pin 或弄脏了该缓冲区。
	 */
	if ((buf_state & BM_TAG_VALID) && !InvalidateVictimBuffer(buf_hdr))
	{
		UnpinBuffer(buf_hdr);
		goto again;
	}

	/* 最后一组健全性检查 */
#ifdef USE_ASSERT_CHECKING
	buf_state = pg_atomic_read_u32(&buf_hdr->state);

	Assert(BUF_STATE_GET_REFCOUNT(buf_state) == 1);
	Assert(!(buf_state & (BM_TAG_VALID | BM_VALID | BM_DIRTY)));

	CheckBufferIsPinnedOnce(buf);
#endif

	return buf;
}

/*
 * 返回此后端应当尝试一次性加 pin 的缓冲区的最大数量，
 * 以避免超过其公平份额。这是 GetAdditionalPinLimit()
 * 可能返回的最高值。注意，在缓冲池相对 max_connections
 * 非常小的系统上，它可能为 0。
 */
uint32
GetPinLimit(void)
{
	return MaxProportionalPins;
}

/*
 * 返回此后端若想保持在每后端限制之下、在已加 pin 缓冲区
 * 数量的基础上还可以额外加 pin 的缓冲区的最大数量。
 * 与 LimitAdditionalPins() 不同，本函数返回的限额可以为 0。
 */
uint32
GetAdditionalPinLimit(void)
{
	uint32		estimated_pins_held;

	/*
	 * 我们可以免费得到“溢出”pin 的数量，但不知道
	 * PrivateRefCountArray 中的 pin 数量。精确计算它的
	 * 代价似乎不值得，因此就假定为最大值。
	 */
	estimated_pins_held = PrivateRefCountOverflowed + REFCOUNT_ARRAY_ENTRIES;

	/* 此后端是否已持有超过其公平份额的 pin？ */
	if (estimated_pins_held > MaxProportionalPins)
		return 0;

	return MaxProportionalPins - estimated_pins_held;
}

/*
 * 限制批操作可以额外获取的 pin 数量，以避免耗尽
 * 可加 pin 的缓冲区。
 *
 * 总是允许额外一个 pin，假设该操作至少需要一个
 * 才能取得进展。
 */
void
LimitAdditionalPins(uint32 *additional_pins)
{
	uint32		limit;

	if (*additional_pins <= 1)
		return;

	limit = GetAdditionalPinLimit();
	limit = Max(limit, 1);
	if (limit < *additional_pins)
		*additional_pins = limit;
}

/*
 * ExtendBufferedRelBy() 与 ExtendBufferedRelTo() 之间共享的逻辑。
 * 仅为避免重复跟踪和 relpersistence 相关的逻辑。
 */
static BlockNumber
ExtendBufferedRelCommon(BufferManagerRelation bmr,
						ForkNumber fork,
						BufferAccessStrategy strategy,
						uint32 flags,
						uint32 extend_by,
						BlockNumber extend_upto,
						Buffer *buffers,
						uint32 *extended_by)
{
	BlockNumber first_block;

	TRACE_POSTGRESQL_BUFFER_EXTEND_START(fork,
										 bmr.smgr->smgr_rlocator.locator.spcOid,
										 bmr.smgr->smgr_rlocator.locator.dbOid,
										 bmr.smgr->smgr_rlocator.locator.relNumber,
										 bmr.smgr->smgr_rlocator.backend,
										 extend_by);

	if (bmr.relpersistence == RELPERSISTENCE_TEMP)
		first_block = ExtendBufferedRelLocal(bmr, fork, flags,
											 extend_by, extend_upto,
											 buffers, &extend_by);
	else
		first_block = ExtendBufferedRelShared(bmr, fork, strategy, flags,
											  extend_by, extend_upto,
											  buffers, &extend_by);
	*extended_by = extend_by;

	TRACE_POSTGRESQL_BUFFER_EXTEND_DONE(fork,
										bmr.smgr->smgr_rlocator.locator.spcOid,
										bmr.smgr->smgr_rlocator.locator.dbOid,
										bmr.smgr->smgr_rlocator.locator.relNumber,
										bmr.smgr->smgr_rlocator.backend,
										*extended_by,
										first_block);

	return first_block;
}

/*
 * ExtendBufferedRelBy() 与 ExtendBufferedRelTo() 针对共享缓冲区的
 * 实现。
 */
static BlockNumber
ExtendBufferedRelShared(BufferManagerRelation bmr,
						ForkNumber fork,
						BufferAccessStrategy strategy,
						uint32 flags,
						uint32 extend_by,
						BlockNumber extend_upto,
						Buffer *buffers,
						uint32 *extended_by)
{
	BlockNumber first_block;
	IOContext	io_context = IOContextForStrategy(strategy);
	instr_time	io_start;

	LimitAdditionalPins(&extend_by);

	/*
	 * 在不持有扩展锁的情况下获取用于扩展的牺牲缓冲区。
	 * 写出牺牲缓冲区是扩展关系中最昂贵的部分，尤其当
	 * 这需要 WAL 刷写时。清零缓冲区同样相当昂贵，
	 * 因此也要在持有扩展锁之前完成。
	 *
	 * 这些页面由我们加 pin 且无效。在我们持有 pin 期间，
	 * 其他后端无法将它们作为牺牲缓冲区获取。
	 */
	for (uint32 i = 0; i < extend_by; i++)
	{
		Block		buf_block;

		buffers[i] = GetVictimBuffer(strategy, io_context);
		buf_block = BufHdrGetBlock(GetBufferDescriptor(buffers[i] - 1));

		/* 新缓冲区已被填零 */
		MemSet(buf_block, 0, BLCKSZ);
	}

	/*
	 * 锁定关系以阻止并发扩展，除非请求不这样做。
	 *
	 * 我们对所有 fork 使用同一个扩展锁。这有些不必要地
	 * 严格，但目前 fork 的扩展并不常发生，还不值得
	 * 进行更细粒度的加锁。
	 *
	 * 注意，等到我们拿到锁时，另一个后端可能已经
	 * 扩展了该关系。
	 */
	if (!(flags & EB_SKIP_EXTENSION_LOCK))
		LockRelationForExtension(bmr.rel, ExclusiveLock);

	/*
	 * 如果请求了，则使大小缓存失效，以便 smgrnblocks 向内核查询。
	 */
	if (flags & EB_CLEAR_SIZE_CACHE)
		bmr.smgr->smgr_cached_nblocks[fork] = InvalidBlockNumber;

	first_block = smgrnblocks(bmr.smgr, fork);

	/*
	 * 既然我们已经有了准确的关系大小，检查调用方是否
	 * 只想扩展到某个特定大小为止。如果存在并发的
	 * 扩展，我们可能获取了过多缓冲区，需要释放它们。
	 */
	if (extend_upto != InvalidBlockNumber)
	{
		uint32		orig_extend_by = extend_by;

		if (first_block > extend_upto)
			extend_by = 0;
		else if ((uint64) first_block + extend_by > extend_upto)
			extend_by = extend_upto - first_block;

		for (uint32 i = extend_by; i < orig_extend_by; i++)
		{
			BufferDesc *buf_hdr = GetBufferDescriptor(buffers[i] - 1);

			/*
			 * 我们之前获取的牺牲缓冲区是干净且未使用的，
			 * 让它能被快速再次找到
			 */
			StrategyFreeBuffer(buf_hdr);
			UnpinBuffer(buf_hdr);
		}

		if (extend_by == 0)
		{
			if (!(flags & EB_SKIP_EXTENSION_LOCK))
				UnlockRelationForExtension(bmr.rel, ExclusiveLock);
			*extended_by = extend_by;
			return first_block;
		}
	}

	/* 如果关系已经达到可能的最大长度，则报错 */
	if ((uint64) first_block + extend_by >= MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cannot extend relation %s beyond %u blocks",
						relpath(bmr.smgr->smgr_rlocator, fork).str,
						MaxBlockNumber)));

	/*
	 * 将缓冲区插入缓冲区表，并标记为 IO_IN_PROGRESS。
	 *
	 * 这必须发生在我们扩展关系之前，因为一旦我们
	 * 这样做，其他后端就能开始读入那些页面。
	 */
	for (uint32 i = 0; i < extend_by; i++)
	{
		Buffer		victim_buf = buffers[i];
		BufferDesc *victim_buf_hdr = GetBufferDescriptor(victim_buf - 1);
		BufferTag	tag;
		uint32		hash;
		LWLock	   *partition_lock;
		int			existing_id;

		/* 以防我们需要在下面加 pin 一个已有的缓冲区 */
		ResourceOwnerEnlarge(CurrentResourceOwner);
		ReservePrivateRefCountEntry();

		InitBufferTag(&tag, &bmr.smgr->smgr_rlocator.locator, fork, first_block + i);
		hash = BufTableHashCode(&tag);
		partition_lock = BufMappingPartitionLock(hash);

		LWLockAcquire(partition_lock, LW_EXCLUSIVE);

		existing_id = BufTableInsert(&tag, hash, victim_buf_hdr->buf_id);

		/*
		 * 我们只会在这里出现于这样一个边角情形：我们正尝试
		 * 扩展关系，却发现一个已存在的缓冲区。这可能发生，
		 * 因为先前扩展关系的尝试失败了，并且
		 * 因为 mdread 不会抱怨读到 EOF 之外（当
		 * zero_damaged_pages 为 ON 时），所以先前读到 EOF
		 * 之外某个块的尝试可能留下了一个“有效”的填零缓冲区。
		 * 遗憾的是，我们也见过这种情形是因为有缺陷的 Linux
		 * 内核有时返回的 lseek(SEEK_END) 结果没有计入最近的
		 * 写入。在这种情况下，已存在的缓冲区会包含我们不想
		 * 覆盖的有效数据。由于合法的情况应该总是留下一个
		 * 填零的缓冲区，因此若不是 PageIsNew 就报错。
		 */
		if (existing_id >= 0)
		{
			BufferDesc *existing_hdr = GetBufferDescriptor(existing_id);
			Block		buf_block;
			bool		valid;

			/*
			 * 在释放分区锁之前加 pin 已有的缓冲区，
			 * 防止它被驱逐。
			 */
			valid = PinBuffer(existing_hdr, strategy);

			LWLockRelease(partition_lock);

			/*
			 * 我们之前获取的牺牲缓冲区是干净且未使用的，
			 * 让它能被快速再次找到
			 */
			StrategyFreeBuffer(victim_buf_hdr);
			UnpinBuffer(victim_buf_hdr);

			buffers[i] = BufferDescriptorGetBuffer(existing_hdr);
			buf_block = BufHdrGetBlock(existing_hdr);

			if (valid && !PageIsNew((Page) buf_block))
				ereport(ERROR,
						(errmsg("unexpected data beyond EOF in block %u of relation \"%s\"",
								existing_hdr->tag.blockNum,
								relpath(bmr.smgr->smgr_rlocator, fork).str),
						 errhint("This has been seen to occur with buggy kernels; consider updating your system.")));

			/*
			 * 我们 *必须* 在成功之前执行 smgr[zero]extend，
			 * 否则该页面不会被内核预留，下一次 P_NEW 调用
			 * 将决定返回同一个页面。清除 BM_VALID 位，
			 * 执行 StartBufferIO() 并继续。
			 *
			 * 使用循环以应对这样一种极小的可能：有人在我们
			 * 清除 BM_VALID 与 StartBufferIO 检查它之间
			 * 重新设置了 BM_VALID。
			 */
			do
			{
				uint32		buf_state = LockBufHdr(existing_hdr);

				buf_state &= ~BM_VALID;
				UnlockBufHdr(existing_hdr, buf_state);
			} while (!StartBufferIO(existing_hdr, true, false));
		}
		else
		{
			uint32		buf_state;

			buf_state = LockBufHdr(victim_buf_hdr);

			/* 在我们持有缓冲区头锁期间做一些健全性检查 */
			Assert(!(buf_state & (BM_VALID | BM_TAG_VALID | BM_DIRTY | BM_JUST_DIRTIED)));
			Assert(BUF_STATE_GET_REFCOUNT(buf_state) == 1);

			victim_buf_hdr->tag = tag;

			buf_state |= BM_TAG_VALID | BUF_USAGECOUNT_ONE;
			if (bmr.relpersistence == RELPERSISTENCE_PERMANENT || fork == INIT_FORKNUM)
				buf_state |= BM_PERMANENT;

			UnlockBufHdr(victim_buf_hdr, buf_state);

			LWLockRelease(partition_lock);

			/* XXX: could combine the locked operations in it with the above */
			StartBufferIO(victim_buf_hdr, true, false);
		}
	}

	io_start = pgstat_prepare_io_time(track_io_timing);

	/*
	 * 注意：如果 smgrzeroextend 失败，我们将得到已分配但未标记
	 * BM_VALID 的缓冲区。下一次关系扩展仍会选择同一个块号
	 * （因为关系在磁盘上并没有变长），因此未来尝试扩展关系时会
	 * 找到相同的缓冲区（如果它们尚未被回收）并回到这里再次
	 * 尝试 smgrzeroextend。
	 *
	 * 全零页面不需要设置校验和。
	 */
	smgrzeroextend(bmr.smgr, fork, first_block, extend_by, false);

	/*
	 * 释放文件扩展锁；现在其他人可以再次扩展关系了。
	 *
	 * 我们在这之后才清除 IO_IN_PROGRESS，因为唤醒等待的后端
	 * 可能会花费可观的时间。
	 */
	if (!(flags & EB_SKIP_EXTENSION_LOCK))
		UnlockRelationForExtension(bmr.rel, ExclusiveLock);

	pgstat_count_io_op_time(IOOBJECT_RELATION, io_context, IOOP_EXTEND,
							io_start, 1, extend_by * BLCKSZ);

	/* 设置 BM_VALID，终止 IO，并唤醒所有等待者 */
	for (uint32 i = 0; i < extend_by; i++)
	{
		Buffer		buf = buffers[i];
		BufferDesc *buf_hdr = GetBufferDescriptor(buf - 1);
		bool		lock = false;

		if (flags & EB_LOCK_FIRST && i == 0)
			lock = true;
		else if (flags & EB_LOCK_TARGET)
		{
			Assert(extend_upto != InvalidBlockNumber);
			if (first_block + i + 1 == extend_upto)
				lock = true;
		}

		if (lock)
			LWLockAcquire(BufferDescriptorGetContentLock(buf_hdr), LW_EXCLUSIVE);

		TerminateBufferIO(buf_hdr, false, BM_VALID, true, false);
	}

	pgBufferUsage.shared_blks_written += extend_by;

	*extended_by = extend_by;

	return first_block;
}

/*
 * BufferIsExclusiveLocked
 *
 *		检查缓冲区是否被排他锁定。
 *
 * 缓冲区必须已被 pin。
 */
bool
BufferIsExclusiveLocked(Buffer buffer)
{
	BufferDesc *bufHdr;

	Assert(BufferIsPinned(buffer));

	if (BufferIsLocal(buffer))
	{
		/* 本地缓冲区不维护内容锁。 */
		return true;
	}
	else
	{
		bufHdr = GetBufferDescriptor(buffer - 1);
		return LWLockHeldByMeInMode(BufferDescriptorGetContentLock(bufHdr),
									LW_EXCLUSIVE);
	}
}

/*
 * BufferIsDirty
 *
 *		检查缓冲区是否已被修改（dirty）。
 *
 * 缓冲区必须已被 pin 并持有排他锁。（若不持有排他锁，
 * 返回值在返回前可能就已经失效了。）
 */
bool
BufferIsDirty(Buffer buffer)
{
	BufferDesc *bufHdr;

	Assert(BufferIsPinned(buffer));

	if (BufferIsLocal(buffer))
	{
		int			bufid = -buffer - 1;

		bufHdr = GetLocalBufferDescriptor(bufid);
		/* 本地缓冲区不维护内容锁。 */
	}
	else
	{
		bufHdr = GetBufferDescriptor(buffer - 1);
		Assert(LWLockHeldByMeInMode(BufferDescriptorGetContentLock(bufHdr),
									LW_EXCLUSIVE));
	}

	return pg_atomic_read_u32(&bufHdr->state) & BM_DIRTY;
}

/*
 * MarkBufferDirty
 *
 *		将缓冲区内容标记为脏（实际写入稍后发生）。
 *
 * 缓冲区必须已被 pin 并持有排他锁。（如果调用者不持有
 * 排他锁，那么可能有人正在写入该缓冲区，
 * 导致磁盘上写入错误数据的风险。）
 */
void
MarkBufferDirty(Buffer buffer)
{
	BufferDesc *bufHdr;
	uint32		buf_state;
	uint32		old_buf_state;

	if (!BufferIsValid(buffer))
		elog(ERROR, "bad buffer ID: %d", buffer);

	if (BufferIsLocal(buffer))
	{
		MarkLocalBufferDirty(buffer);
		return;
	}

	bufHdr = GetBufferDescriptor(buffer - 1);

	Assert(BufferIsPinned(buffer));
	Assert(LWLockHeldByMeInMode(BufferDescriptorGetContentLock(bufHdr),
								LW_EXCLUSIVE));

	old_buf_state = pg_atomic_read_u32(&bufHdr->state);
	for (;;)
	{
		if (old_buf_state & BM_LOCKED)
			old_buf_state = WaitBufHdrUnlocked(bufHdr);

		buf_state = old_buf_state;

		Assert(BUF_STATE_GET_REFCOUNT(buf_state) > 0);
		buf_state |= BM_DIRTY | BM_JUST_DIRTIED;

		if (pg_atomic_compare_exchange_u32(&bufHdr->state, &old_buf_state,
										   buf_state))
			break;
	}

	/*
	 * 如果缓冲区原先不脏，则计入 vacuum 统计。
	 */
	if (!(old_buf_state & BM_DIRTY))
	{
		pgBufferUsage.shared_blks_dirtied++;
		if (VacuumCostActive)
			VacuumCostBalance += VacuumCostPageDirty;
	}
}

/*
 * ReleaseAndReadBuffer —— 合并 ReleaseBuffer() 与 ReadBuffer()
 *
 * 过去，与分别调用这两个例程相比，这可以省去一次
 * 获取/释放 BufMgrLock 的循环。如今它主要只是一个
 * 便利函数。不过，如果传入的缓冲区有效且已经包含了
 * 所需的块，我们就原样返回它；与完全释放再重新获取相比，
 * 这确实能省去相当多的工作。
 *
 * 注意：可以传入 buffer == InvalidBuffer，表示实际上没有
 * 旧缓冲区需要释放。这种情况与 ReadBuffer 相同，
 * 但可以为调用者省去一些判断。
 */
Buffer
ReleaseAndReadBuffer(Buffer buffer,
					 Relation relation,
					 BlockNumber blockNum)
{
	ForkNumber	forkNum = MAIN_FORKNUM;
	BufferDesc *bufHdr;

	if (BufferIsValid(buffer))
	{
		Assert(BufferIsPinned(buffer));
		if (BufferIsLocal(buffer))
		{
			bufHdr = GetLocalBufferDescriptor(-buffer - 1);
			if (bufHdr->tag.blockNum == blockNum &&
				BufTagMatchesRelFileLocator(&bufHdr->tag, &relation->rd_locator) &&
				BufTagGetForkNum(&bufHdr->tag) == forkNum)
				return buffer;
			UnpinLocalBuffer(buffer);
		}
		else
		{
			bufHdr = GetBufferDescriptor(buffer - 1);
			/* 我们已经 pin 住了，因此可以不用自旋锁检查 tag */
			if (bufHdr->tag.blockNum == blockNum &&
				BufTagMatchesRelFileLocator(&bufHdr->tag, &relation->rd_locator) &&
				BufTagGetForkNum(&bufHdr->tag) == forkNum)
				return buffer;
			UnpinBuffer(bufHdr);
		}
	}

	return ReadBuffer(relation, blockNum);
}

/*
 * PinBuffer —— 使缓冲区不被替换。
 *
 * 对于默认访问策略，缓冲区在首次被 pin 时其 usage_count 会递增；
 * 对于其他策略，我们只需确保 usage_count 不为零。（后者的
 * 用意是我们不希望同步的堆扫描抬高计数，但又需要它非零，
 * 以阻止其他后端从我们的环中窃取缓冲区。只要我们遍历环的速度
 * 快于全局时钟扫描的循环，我们环中的缓冲区就不会被其他后端
 * 选为替换牺牲品。）
 *
 * 这只应作用于共享缓冲区，绝不作用于本地缓冲区。
 *
 * 由于缓冲区的 pin/unpin 非常频繁，pin 缓冲区时不获取
 * 缓冲区头锁，而是在 CAS 操作循环中更新状态变量。
 * 理想情况下只需一次 CAS。
 *
 * 注意，ResourceOwnerEnlarge() 和 ReservePrivateRefCountEntry()
 * 必须已经执行过了。
 *
 * 如果缓冲区为 BM_VALID 则返回 true，否则返回 false。这一安排
 * 让某些调用者可以避免额外的自旋锁循环。
 */
static bool
PinBuffer(BufferDesc *buf, BufferAccessStrategy strategy)
{
	Buffer		b = BufferDescriptorGetBuffer(buf);
	bool		result;
	PrivateRefCountEntry *ref;

	Assert(!BufferIsLocal(b));
	Assert(ReservedRefCountEntry != NULL);

	ref = GetPrivateRefCountEntry(b, true);

	if (ref == NULL)
	{
		uint32		buf_state;
		uint32		old_buf_state;

		ref = NewPrivateRefCountEntry(b);

		old_buf_state = pg_atomic_read_u32(&buf->state);
		for (;;)
		{
			if (old_buf_state & BM_LOCKED)
				old_buf_state = WaitBufHdrUnlocked(buf);

			buf_state = old_buf_state;

		/* 增加引用计数 */
		buf_state += BUF_REFCOUNT_ONE;

		if (strategy == NULL)
		{
			/* 默认情况：除非已达上限，否则增加 usagecount。 */
				if (BUF_STATE_GET_USAGECOUNT(buf_state) < BM_MAX_USAGE_COUNT)
					buf_state += BUF_USAGECOUNT_ONE;
			}
			else
			{
			/*
			 * 环中的缓冲区不应将其他缓冲区从池中驱逐。
			 * 因此我们不会让 usagecount 超过 1。
			 */
				if (BUF_STATE_GET_USAGECOUNT(buf_state) == 0)
					buf_state += BUF_USAGECOUNT_ONE;
			}

			if (pg_atomic_compare_exchange_u32(&buf->state, &old_buf_state,
											   buf_state))
			{
				result = (buf_state & BM_VALID) != 0;

			/*
			 * 为简化起见，假设我们获取缓冲区 pin 是用于
			 * Valgrind 缓冲区客户端检查的（即使在 !result 情况下）。
			 * 不安全可访问的缓冲区通常不会保证被标记
			 * 为未定义或不可访问。
			 */
				VALGRIND_MAKE_MEM_DEFINED(BufHdrGetBlock(buf), BLCKSZ);
				break;
			}
		}
	}
	else
	{
		/*
		 * 如果我们之前 pin 过该缓冲区，它很可能是有效的，但
		 * 如果已经调用了 StartReadBuffers() 而尚未调用
		 * WaitReadBuffers()，它可能就还无效。我们会通过不加锁
		 * 地加载标志来检查。这存在竞争，但返回 false 也
		 * 没有问题：当 WaitReadBuffers() 调用 StartBufferIO()
		 * 时，它会发现缓冲区现在有效了。
		 *
		 * 注意：我们在这里故意避免 Valgrind 客户端请求。
		 * 各个访问方法可以选择在我们的客户端请求之上叠加
		 * 缓冲区页客户端请求，以强制缓冲区只能在加锁
		 * （且已 pin）状态下访问。这里缓冲区页可能合法地
		 * 不可访问，我们不能去干涉这一点。
		 */
		result = (pg_atomic_read_u32(&buf->state) & BM_VALID) != 0;
	}

	ref->refcount++;
	Assert(ref->refcount > 0);
	ResourceOwnerRememberBuffer(CurrentResourceOwner, b);
	return result;
}

/*
 * PinBuffer_Locked —— 同上，但调用者已经锁住了缓冲区头。
 * 自旋锁在返回前被释放。
 *
 * 由于此函数在持有自旋锁的情况下被调用，调用者必须先调用
 * ReservePrivateRefCountEntry() 和
 * ResourceOwnerEnlarge(CurrentResourceOwner)；
 *
 * 目前，本函数的所有调用者都不想修改缓冲区的
 * usage_count，因此不需要 strategy 参数。
 * 我们也不做 BM_VALID 测试（调用者可以自行检查）。
 *
 * 此外，所有调用者只在已知该缓冲区不可能已被本后端
 * 预先 pin 的情况下才使用本函数。这让我们能跳过
 * 搜索私有 refcount 数组与哈希，是一种便利，
 * 因为自旋锁仍然持有。
 *
 * 注意：使用本例程经常是强制性的，而不仅仅是节省一次
 * 自旋锁加锁/解锁循环的优化，因为我们需要在缓冲区
 * 状态在我们之下改变之前把它 pin 住。
 */
static void
PinBuffer_Locked(BufferDesc *buf)
{
	Buffer		b;
	PrivateRefCountEntry *ref;
	uint32		buf_state;

	/*
	 * 如前所述，我们预期不会有任何预先存在的 pin。这让我们能在
	 * 释放自旋锁之后再去操作 PrivateRefCount
	 */
	Assert(GetPrivateRefCountEntry(BufferDescriptorGetBuffer(buf), false) == NULL);

	/*
	 * 缓冲区不可能有预先存在的 pin，因此将其页面向 Valgrind
	 * 标记为已定义（这与 PinBuffer() 中后端尚未持有
	 * 缓冲区 pin 的情况类似）
	 */
	VALGRIND_MAKE_MEM_DEFINED(BufHdrGetBlock(buf), BLCKSZ);

	/*
	 * 由于我们持有缓冲区自旋锁，可以在一次操作中更新
	 * 缓冲区状态并释放锁。
	 */
	buf_state = pg_atomic_read_u32(&buf->state);
	Assert(buf_state & BM_LOCKED);
	buf_state += BUF_REFCOUNT_ONE;
	UnlockBufHdr(buf, buf_state);

	b = BufferDescriptorGetBuffer(buf);

	ref = NewPrivateRefCountEntry(b);
	ref->refcount++;

	ResourceOwnerRememberBuffer(CurrentResourceOwner, b);
}

/*
 * 用于唤醒另一个正在等待清理锁（cleanup lock）通过
 * BM_PIN_COUNT_WAITER 被释放的后端。
 *
 * 参见 LockBufferForCleanup()。
 *
 * 预期在刚释放一个缓冲区 pin 之后调用（是在 BufferDesc 上
 * 释放，而不仅仅减小该缓冲区的后端本地 pincount）。
 */
static void
WakePinCountWaiter(BufferDesc *buf)
{
	/*
	 * 获取缓冲区头锁，重新检查是否确实有等待者。另一个
	 * 后端可能已经 unpin 了这个缓冲区，并已经唤醒了
	 * 等待者。
	 *
	 * 在我们上面 unpin 之后，该缓冲区被替换并无危险，
	 * 因为它正被等待者 pin 住。如果等待者因本后端唤醒
	 * 之外的原因停止等待，它会清除 BM_PIN_COUNT_WAITER。
	 */
	uint32		buf_state = LockBufHdr(buf);

	if ((buf_state & BM_PIN_COUNT_WAITER) &&
		BUF_STATE_GET_REFCOUNT(buf_state) == 1)
	{
		/* 我们刚刚释放了除等待者之外的最后一个 pin */
		int			wait_backend_pgprocno = buf->wait_backend_pgprocno;

		buf_state &= ~BM_PIN_COUNT_WAITER;
		UnlockBufHdr(buf, buf_state);
		ProcSendSignal(wait_backend_pgprocno);
	}
	else
		UnlockBufHdr(buf, buf_state);
}

/*
 * UnpinBuffer —— 使缓冲区可被替换。
 *
 * 这只应作用于共享缓冲区，绝不作用于本地缓冲区。它
 * 总是会调整 CurrentResourceOwner。
 */
static void
UnpinBuffer(BufferDesc *buf)
{
	Buffer		b = BufferDescriptorGetBuffer(buf);

	ResourceOwnerForgetBuffer(CurrentResourceOwner, b);
	UnpinBufferNoOwner(buf);
}

static void
UnpinBufferNoOwner(BufferDesc *buf)
{
	PrivateRefCountEntry *ref;
	Buffer		b = BufferDescriptorGetBuffer(buf);

	Assert(!BufferIsLocal(b));

	/* 不加移动，因为反正可能很快就要删除它 */
	ref = GetPrivateRefCountEntry(b, false);
	Assert(ref != NULL);
	Assert(ref->refcount > 0);
	ref->refcount--;
	if (ref->refcount == 0)
	{
		uint32		buf_state;
		uint32		old_buf_state;

		/*
		 * 将缓冲区标记为对 Valgrind 不可访问。
		 *
		 * 注意，缓冲区可能已经在该访问方法代码中
		 * 被标记为不可访问，该代码强制缓冲区只能在
		 * 持有缓冲区锁时才能访问。
		 */
		VALGRIND_MAKE_MEM_NOACCESS(BufHdrGetBlock(buf), BLCKSZ);

		/* 最好别还持有缓冲区内容锁 */
		Assert(!LWLockHeldByMe(BufferDescriptorGetContentLock(buf)));

		/*
		 * 递减共享引用计数。
		 *
		 * 由于缓冲区自旋锁持有者可以仅用写操作更新状态，
		 * 这里使用原子递减并不安全；因此使用 CAS 循环。
		 */
		old_buf_state = pg_atomic_read_u32(&buf->state);
		for (;;)
		{
			if (old_buf_state & BM_LOCKED)
				old_buf_state = WaitBufHdrUnlocked(buf);

			buf_state = old_buf_state;

			buf_state -= BUF_REFCOUNT_ONE;

			if (pg_atomic_compare_exchange_u32(&buf->state, &old_buf_state,
											   buf_state))
				break;
		}

		/* 支持 LockBufferForCleanup() */
		if (buf_state & BM_PIN_COUNT_WAITER)
			WakePinCountWaiter(buf);

		ForgetPrivateRefCountEntry(ref);
	}
}

#define ST_SORT sort_checkpoint_bufferids
#define ST_ELEMENT_TYPE CkptSortItem
#define ST_COMPARE(a, b) ckpt_buforder_comparator(a, b)
#define ST_SCOPE static
#define ST_DEFINE
#include "lib/sort_template.h"

/*
 * BufferSync —— 将池中所有的脏缓冲区写出。
 *
 * 在检查点时被调用，以写出所有脏的共享缓冲区。
 * 应传入检查点请求标志。若设置了 CHECKPOINT_IMMEDIATE，
 * 我们关闭写入之间的延迟；若设置了 CHECKPOINT_IS_SHUTDOWN、
 * CHECKPOINT_END_OF_RECOVERY 或 CHECKPOINT_FLUSH_ALL，我们会
 * 写出即便未记录日志（unlogged）的缓冲区，否则这些会被跳过。
 * 其余标志目前在此处没有效果。
 */
static void
BufferSync(int flags)
{
	uint32		buf_state;
	int			buf_id;
	int			num_to_scan;
	int			num_spaces;
	int			num_processed;
	int			num_written;
	CkptTsStatus *per_ts_stat = NULL;
	Oid			last_tsid;
	binaryheap *ts_heap;
	int			i;
	int			mask = BM_DIRTY;
	WritebackContext wb_context;

	/*
	 * 除非这是一次关闭检查点，或者我们被显式告知，否则
	 * 我们只写出永久的脏缓冲区。但在关闭或恢复结束时，
	 * 我们写出所有脏缓冲区。
	 */
	if (!((flags & (CHECKPOINT_IS_SHUTDOWN | CHECKPOINT_END_OF_RECOVERY |
					CHECKPOINT_FLUSH_ALL))))
		mask |= BM_PERMANENT;

	/*
	 * 遍历所有缓冲区，将需要写出的那些标记为
	 * BM_CHECKPOINT_NEEDED。在遍历过程中计数（num_to_scan），
	 * 以便估计需要做多少工作。
	 *
	 * 这让我们只写出检查点开始时就已经是脏的那些页，
	 * 而不写出在检查点进行期间才变脏的页。任何带有
	 * BM_CHECKPOINT_NEEDED 的页一旦被写出——无论是由本函数
	 * 后面写出，还是由普通后端或 bgwriter 清理扫描写出——
	 * 该标志都会被清除。在此之后变脏的任何缓冲区都不会
	 * 设置该标志。
	 *
	 * 注意，如果我们未能写出某些缓冲区，可能会留下仍被设置
	 * BM_CHECKPOINT_NEEDED 的缓冲区。这没关系，因为任何这样的
	 * 缓冲区在下一次检查点尝试时也肯定需要被写出。
	 */
	num_to_scan = 0;
	for (buf_id = 0; buf_id < NBuffers; buf_id++)
	{
		BufferDesc *bufHdr = GetBufferDescriptor(buf_id);

		/*
		 * 检查 BM_DIRTY 只需缓冲区头自旋锁即可，
		 * 参见 SyncOneBuffer 中的注释。
		 */
		buf_state = LockBufHdr(bufHdr);

		if ((buf_state & mask) == mask)
		{
			CkptSortItem *item;

			buf_state |= BM_CHECKPOINT_NEEDED;

			item = &CkptBufferIds[num_to_scan++];
			item->buf_id = buf_id;
			item->tsId = bufHdr->tag.spcOid;
			item->relNumber = BufTagGetRelNumber(&bufHdr->tag);
			item->forkNum = BufTagGetForkNum(&bufHdr->tag);
			item->blockNum = bufHdr->tag.blockNum;
		}

		UnlockBufHdr(bufHdr, buf_state);

		/* 若 NBuffers 很大，检查 barrier 事件。 */
		if (ProcSignalBarrierPending)
			ProcessProcSignalBarrier();
	}

	if (num_to_scan == 0)
		return;					/* nothing to do */

	WritebackContextInit(&wb_context, &checkpoint_flush_after);

	TRACE_POSTGRESQL_BUFFER_SYNC_START(NBuffers, num_to_scan);

	/*
	 * 对需要写出的缓冲区排序，以降低随机 IO 的可能性。
	 * 排序对于实现各表空间之间写操作的均衡也很重要。
	 * 若不均衡写操作，我们可能会逐个表空间地写入，
	 * 从而可能使底层系统过载。
	 */
	sort_checkpoint_bufferids(CkptBufferIds, num_to_scan);

	num_spaces = 0;

	/*
	 * 为每个有缓冲区需要刷出的表空间分配进度状态。
	 * 这要求待刷出数组已经过排序。
	 */
	last_tsid = InvalidOid;
	for (i = 0; i < num_to_scan; i++)
	{
		CkptTsStatus *s;
		Oid			cur_tsid;

		cur_tsid = CkptBufferIds[i].tsId;

		/*
		 * 每发现一个新的表空间，就扩充每个表空间
		 * 状态结构体的数组。
		 */
		if (last_tsid == InvalidOid || last_tsid != cur_tsid)
		{
			Size		sz;

			num_spaces++;

			/*
			 * 不值得在这里加上按 2 的幂次增长的
			 * 逻辑——即使有几百个表空间也应该没问题。
			 */
			sz = sizeof(CkptTsStatus) * num_spaces;

			if (per_ts_stat == NULL)
				per_ts_stat = (CkptTsStatus *) palloc(sz);
			else
				per_ts_stat = (CkptTsStatus *) repalloc(per_ts_stat, sz);

			s = &per_ts_stat[num_spaces - 1];
			memset(s, 0, sizeof(*s));
			s->tsId = cur_tsid;

			/*
			 * 本表空间中的第一个缓冲区。由于 CkptBufferIds
			 * 是按表空间排序的，本表空间中所有的
			 * （s->num_to_scan 个）缓冲区都会跟在后面。
			 */
			s->index = i;

			/*
			 * progress_slice 将在我们得知每个表空间有多少缓冲区，
			 * 即本循环结束后才确定。
			 */

			last_tsid = cur_tsid;
		}
		else
		{
			s = &per_ts_stat[num_spaces - 1];
		}

		s->num_to_scan++;

		/* 检查 barrier 事件。 */
		if (ProcSignalBarrierPending)
			ProcessProcSignalBarrier();
	}

	Assert(num_spaces > 0);

	/*
	 * 在各表空间的写进度之上构建一个最小堆，
	 * 并计算出单个被处理缓冲区占总进度的多大比例。
	 */
	ts_heap = binaryheap_allocate(num_spaces,
								  ts_ckpt_progress_comparator,
								  NULL);

	for (i = 0; i < num_spaces; i++)
	{
		CkptTsStatus *ts_stat = &per_ts_stat[i];

		ts_stat->progress_slice = (float8) num_to_scan / ts_stat->num_to_scan;

		binaryheap_add_unordered(ts_heap, PointerGetDatum(ts_stat));
	}

	binaryheap_build(ts_heap);

	/*
	 * 遍历待做检查点的缓冲区，写出那些（仍然）被标记了
	 * BM_CHECKPOINT_NEEDED 的。写操作在各表空间之间均衡；
	 * 否则排序会导致一次只有一个表空间接收写入，
	 * 从而低效地使用硬件。
	 */
	num_processed = 0;
	num_written = 0;
	while (!binaryheap_empty(ts_heap))
	{
		BufferDesc *bufHdr = NULL;
		CkptTsStatus *ts_stat = (CkptTsStatus *)
			DatumGetPointer(binaryheap_first(ts_heap));

		buf_id = CkptBufferIds[ts_stat->index].buf_id;
		Assert(buf_id != -1);

		bufHdr = GetBufferDescriptor(buf_id);

		num_processed++;

		/*
		 * 这里我们不需要获取锁，因为我们只看一个比特位。
		 * 也许在我们检查之后、别人立刻写掉了缓冲区并清除了
		 * 标志，但那没关系，因为 SyncOneBuffer 那时将什么也不做。
		 * 然而，还存在一个更进一步的竞争条件：在我们这里检查该比特位
		 * 的时刻与 SyncOneBuffer 获取锁的时刻之间，别人不仅写掉了
		 * 缓冲区，还可能用另一个页替换了它并弄脏。在那种不太可能的
		 * 情况下，SyncOneBuffer 会写出本不需要写的缓冲区。
		 * 不过，为此做防护似乎并不值得。
		 */
		if (pg_atomic_read_u32(&bufHdr->state) & BM_CHECKPOINT_NEEDED)
		{
			if (SyncOneBuffer(buf_id, false, &wb_context) & BUF_WRITTEN)
			{
				TRACE_POSTGRESQL_BUFFER_SYNC_WRITTEN(buf_id);
				PendingCheckpointerStats.buffers_written++;
				num_written++;
			}
		}

		/*
		 * 独立于缓冲区是否真正需要刷出地度量进度
		 * ——否则写操作会变得不均衡。
		 */
		ts_stat->progress += ts_stat->progress_slice;
		ts_stat->num_scanned++;
		ts_stat->index++;

		/* 该表空间的所有缓冲区都已处理完了吗？ */
		if (ts_stat->num_scanned == ts_stat->num_to_scan)
		{
			binaryheap_remove_first(ts_heap);
		}
		else
		{
			/* 用新的进度更新堆 */
			binaryheap_replace_first(ts_heap, PointerGetDatum(ts_stat));
		}

	/*
	 * 睡眠以限制我们的 I/O 速率。
	 *
	 * （即使没有睡眠，这也会检查 barrier 事件。）
	 */
		CheckpointWriteDelay(flags, (double) num_processed / num_to_scan);
	}

	/*
	 * 发出所有待处理的刷出。只有检查点进程会调用 BufferSync()，
	 * 因此 IOContext 将始终是 IOCONTEXT_NORMAL。
	 */
	IssuePendingWritebacks(&wb_context, IOCONTEXT_NORMAL);

	pfree(per_ts_stat);
	per_ts_stat = NULL;
	binaryheap_free(ts_heap);

	/*
	 * 更新检查点统计。如上所述，这不包括其他后端
	 * 或 bgwriter 扫描写出的缓冲区。
	 */
	CheckpointStats.ckpt_bufs_written += num_written;

	TRACE_POSTGRESQL_BUFFER_SYNC_DONE(NBuffers, num_written, num_to_scan);
}

/*
 * BgBufferSync -- Write out some dirty buffers in the pool.
 *
 * This is called periodically by the background writer process.
 *
 * 如果 bgwriter 进程适合进入低功耗休眠模式，则返回 true。
 * （当策略时钟扫描被“套圈”且最近没有发生缓冲区分配，
 * 或者当 bgwriter 通过将 bgwriter_lru_maxpages 设为 0
 * 而被实际上禁用时，会发生这种情况。）
 */
bool
BgBufferSync(WritebackContext *wb_context)
{
	/* 从 freelist.c 获取的信息 */
	int			strategy_buf_id;
	uint32		strategy_passes;
	uint32		recent_alloc;

	/*
	 * 在多次调用之间保存的信息，以便我们确定策略点的
	 * 前进速率，并避免扫描已经被清理过的缓冲区。
	 */
	static bool saved_info_valid = false;
	static int	prev_strategy_buf_id;
	static uint32 prev_strategy_passes;
	static int	next_to_clean;
	static uint32 next_passes;

	/* 分配速率与干净缓冲区密度的移动平均 */
	static float smoothed_alloc = 0;
	static float smoothed_density = 10.0;

	/* 这些以后或许可调，但目前不行 */
	float		smoothing_samples = 16;
	float		scan_whole_pool_milliseconds = 120000.0;

	/* 用于计算我们向前扫描多远 */
	long		strategy_delta;
	int			bufs_to_lap;
	int			bufs_ahead;
	float		scans_per_alloc;
	int			reusable_buffers_est;
	int			upcoming_alloc_est;
	int			min_scan_buffers;

	/* 扫描循环本身所需的变量 */
	int			num_to_scan;
	int			num_written;
	int			reusable_buffers;

	/* 用于最终 smoothed_density 更新的变量 */
	long		new_strategy_delta;
	uint32		new_recent_alloc;

	/*
	 * 找出 freelist 时钟扫描当前的位置，以及自我们上次
	 * 调用以来发生了多少次缓冲区分配。
	 */
	strategy_buf_id = StrategySyncStart(&strategy_passes, &recent_alloc);

	/* 向 pgstat 报告缓冲区分配计数 */
	PendingBgWriterStats.buf_alloc += recent_alloc;

	/*
	 * 如果我们没有运行 LRU 扫描，做完统计工作后就停止。
	 * 我们将保存的状态标记为无效，这样如果以后重新开启
	 * LRU 扫描，我们还能正常恢复。
	 */
	if (bgwriter_lru_maxpages <= 0)
	{
		saved_info_valid = false;
		return true;
	}

	/*
	 * 计算 strategy_delta = 自上次以来时钟扫描已经扫描了多少
	 * 个缓冲区。如果是第一次，假设为 0。然后看我们是
	 * 否仍领先于时钟扫描，如果是，我们还能扫描多少个缓冲区
	 * 才会追上它并被“套圈”。注意：xxx_passes 比较中那些
	 * 看起来奇怪的写法是为了在 passes 计数回绕时
	 * 避免错误行为。
	 */
	if (saved_info_valid)
	{
		int32		passes_delta = strategy_passes - prev_strategy_passes;

		strategy_delta = strategy_buf_id - prev_strategy_buf_id;
		strategy_delta += (long) passes_delta * NBuffers;

		Assert(strategy_delta >= 0);

		if ((int32) (next_passes - strategy_passes) > 0)
		{
			/* 我们领先策略点一个 pass */
			bufs_to_lap = strategy_buf_id - next_to_clean;
#ifdef BGW_DEBUG
			elog(DEBUG2, "bgwriter ahead: bgw %u-%u strategy %u-%u delta=%ld lap=%d",
				 next_passes, next_to_clean,
				 strategy_passes, strategy_buf_id,
				 strategy_delta, bufs_to_lap);
#endif
		}
		else if (next_passes == strategy_passes &&
				 next_to_clean >= strategy_buf_id)
		{
			/* 在同一 pass 上，但领先或至少没有落后 */
			bufs_to_lap = NBuffers - (next_to_clean - strategy_buf_id);
#ifdef BGW_DEBUG
			elog(DEBUG2, "bgwriter ahead: bgw %u-%u strategy %u-%u delta=%ld lap=%d",
				 next_passes, next_to_clean,
				 strategy_passes, strategy_buf_id,
				 strategy_delta, bufs_to_lap);
#endif
		}
		else
		{
			/*
			 * 我们落后了，因此向前跳到策略点，并从那里
			 * 开始清理。
			 */
#ifdef BGW_DEBUG
			elog(DEBUG2, "bgwriter behind: bgw %u-%u strategy %u-%u delta=%ld",
				 next_passes, next_to_clean,
				 strategy_passes, strategy_buf_id,
				 strategy_delta);
#endif
			next_to_clean = strategy_buf_id;
			next_passes = strategy_passes;
			bufs_to_lap = NBuffers;
		}
	}
	else
	{
		/*
		 * 在启动时或 LRU 扫描被关闭后做初始化。
		 * 总是从策略点开始。
		 */
#ifdef BGW_DEBUG
		elog(DEBUG2, "bgwriter initializing: strategy %u-%u",
			 strategy_passes, strategy_buf_id);
#endif
		strategy_delta = 0;
		next_to_clean = strategy_buf_id;
		next_passes = strategy_passes;
		bufs_to_lap = NBuffers;
	}

	/* 更新保存的信息以备下次使用 */
	prev_strategy_buf_id = strategy_buf_id;
	prev_strategy_passes = strategy_passes;
	saved_info_valid = true;

	/*
	 * 计算每次新分配需要扫描多少个缓冲区，即
	 * 可复用缓冲区密度的倒数，并跟踪它的移动平均。
	 *
	 * 如果策略点没有移动，我们就不更新密度估计
	 */
	if (strategy_delta > 0 && recent_alloc > 0)
	{
		scans_per_alloc = (float) strategy_delta / (float) recent_alloc;
		smoothed_density += (scans_per_alloc - smoothed_density) /
			smoothing_samples;
	}

	/*
	 * 基于平滑后的密度估计，估计在当前的策略点与我们
	 * 已经向前扫描到的位置之间，有多少个可复用缓冲区。
	 */
	bufs_ahead = NBuffers - bufs_to_lap;
	reusable_buffers_est = (float) bufs_ahead / smoothed_density;

	/*
	 * 跟踪最近缓冲区分配的移动平均。这里，我们想要的
	 * 不是真正的平均，而是快速上升、缓慢下降的行为：
	 * 我们立即跟随任何增长。
	 */
	if (smoothed_alloc <= (float) recent_alloc)
		smoothed_alloc = recent_alloc;
	else
		smoothed_alloc += ((float) recent_alloc - smoothed_alloc) /
			smoothing_samples;

	/* 用一个 GUC 放大估计值，以允许更激进的调优。 */
	upcoming_alloc_est = (int) (smoothed_alloc * bgwriter_lru_multiplier);

	/*
	 * 如果 recent_alloc 在多个周期里一直为零，smoothed_alloc
	 * 最终会下溢到零，而这种下溢在某些平台上会产生
	 * 恼人的内核警告。一旦 upcoming_alloc_est 已经变成
	 * 零，再跟踪越来越小的 smoothed_alloc 值就没有意义了，
	 * 因此直接把它重置为精确的零以避免这种症状。
	 * 只要 recent_alloc 一增加，它就会重新升回来。
	 */
	if (upcoming_alloc_est == 0)
		smoothed_alloc = 0;

	/*
	 * 即使在很少或没有缓冲区分配活动的情况下，我们也想
	 * 在缓冲区缓存中取得少量进展，以便在空闲期过后
	 * 尽可能多的可复用缓冲区是干净的。
	 *
	 * (scan_whole_pool_milliseconds / BgWriterDelay) computes how many times
	 * BGW 将在 scan_whole_pool 时间内被调用；将
	 * 缓冲区池切分为那么多段。
	 */
	min_scan_buffers = (int) (NBuffers / (scan_whole_pool_milliseconds / BgWriterDelay));

	if (upcoming_alloc_est < (min_scan_buffers + reusable_buffers_est))
	{
#ifdef BGW_DEBUG
		elog(DEBUG2, "bgwriter: alloc_est=%d too small, using min=%d + reusable_est=%d",
			 upcoming_alloc_est, min_scan_buffers, reusable_buffers_est);
#endif
		upcoming_alloc_est = min_scan_buffers + reusable_buffers_est;
	}

	/*
	 * 现在写出脏的可复用缓冲区，从 next_to_clean 点
	 * 向前工作，直到我们套圈了策略扫描，或者清理了
	 * 足够多的缓冲区以匹配我们对下一周期分配需求的
	 * 估计，或者达到了 bgwriter_lru_maxpages 上限。
	 */

	num_to_scan = bufs_to_lap;
	num_written = 0;
	reusable_buffers = reusable_buffers_est;

	/* 执行 LRU 扫描 */
	while (num_to_scan > 0 && reusable_buffers < upcoming_alloc_est)
	{
		int			sync_state = SyncOneBuffer(next_to_clean, true,
											   wb_context);

		if (++next_to_clean >= NBuffers)
		{
			next_to_clean = 0;
			next_passes++;
		}
		num_to_scan--;

		if (sync_state & BUF_WRITTEN)
		{
			reusable_buffers++;
			if (++num_written >= bgwriter_lru_maxpages)
			{
				PendingBgWriterStats.maxwritten_clean++;
				break;
			}
		}
		else if (sync_state & BUF_REUSABLE)
			reusable_buffers++;
	}

	PendingBgWriterStats.buf_written_clean += num_written;

#ifdef BGW_DEBUG
	elog(DEBUG1, "bgwriter: recent_alloc=%u smoothed=%.2f delta=%ld ahead=%d density=%.2f reusable_est=%d upcoming_est=%d scanned=%d wrote=%d reusable=%d",
		 recent_alloc, smoothed_alloc, strategy_delta, bufs_ahead,
		 smoothed_density, reusable_buffers_est, upcoming_alloc_est,
		 bufs_to_lap - num_to_scan,
		 num_written,
		 reusable_buffers - reusable_buffers_est);
#endif

	/*
	 * 将上面的扫描视为一次新的分配扫描。刻画它的密度
	 * 并据此更新平滑后的密度。在策略与后台写进程
	 * 都做了一些有用扫描的情况下，这有效地将移动平均
	 * 的周期减半，这是有益的，因为在密度估计上
	 * 较长的记忆并不那么理想。
	 */
	new_strategy_delta = bufs_to_lap - num_to_scan;
	new_recent_alloc = reusable_buffers - reusable_buffers_est;
	if (new_strategy_delta > 0 && new_recent_alloc > 0)
	{
		scans_per_alloc = (float) new_strategy_delta / (float) new_recent_alloc;
		smoothed_density += (scans_per_alloc - smoothed_density) /
			smoothing_samples;

#ifdef BGW_DEBUG
		elog(DEBUG2, "bgwriter: cleaner density alloc=%u scan=%ld density=%.2f new smoothed=%.2f",
			 new_recent_alloc, new_strategy_delta,
			 scans_per_alloc, smoothed_density);
#endif
	}

	/* 如果可以休眠则返回 true */
	return (bufs_to_lap == 0 && recent_alloc == 0);
}

/*
 * SyncOneBuffer —— 在同步期间处理单个缓冲区。
 *
 * 如果 skip_recently_used 为 true，我们不会写出当前
 * 被 pin 的缓冲区，也不会写出最近被使用的缓冲区，
 * 因为它们不是替换候选者。
 *
 * 返回一个位掩码，包含以下标志位：
 *	BUF_WRITTEN：我们写出了缓冲区。
 *	BUF_REUSABLE：缓冲区可用于替换，即它的 pin 计数为 0
 *		且 usage 计数为 0。
 *
 * （如果 FlushBuffer 在锁住缓冲区后发现它是干净的，
 * BUF_WRITTEN 可能会被错误地设置，但我们并不太在意。）
 */
static int
SyncOneBuffer(int buf_id, bool skip_recently_used, WritebackContext *wb_context)
{
	BufferDesc *bufHdr = GetBufferDescriptor(buf_id);
	int			result = 0;
	uint32		buf_state;
	BufferTag	tag;

	/* 确保我们能处理这个 pin */
	ReservePrivateRefCountEntry();
	ResourceOwnerEnlarge(CurrentResourceOwner);

	/*
	 * 检查缓冲区是否需要写出。
	 *
	 * 只要我们是在访问方法中、在通过 XLogInsert() 记录
	 * 改动*之前*标记页面为脏的，就可以在不需要获取
	 * 缓冲区内容锁的情况下做这个检查：如果有人在我们
	 * 检查之后才标记缓冲区为脏，我们不担心，因为我们的
	 * checkpoint.redo 指向即将发生的改动对应的日志记录
	 * 之前，因此我们并无义务写出这样的脏缓冲区。
	 */
	buf_state = LockBufHdr(bufHdr);

	if (BUF_STATE_GET_REFCOUNT(buf_state) == 0 &&
		BUF_STATE_GET_USAGECOUNT(buf_state) == 0)
	{
		result |= BUF_REUSABLE;
	}
		else if (skip_recently_used)
	{
		/* 调用者告诉我们不要写出最近使用过的缓冲区 */
		UnlockBufHdr(bufHdr, buf_state);
		return result;
	}

	if (!(buf_state & BM_VALID) || !(buf_state & BM_DIRTY))
	{
		/* 它是干净的，所以无事可做 */
		UnlockBufHdr(bufHdr, buf_state);
		return result;
	}

	/*
	 * pin 它、共享锁它、写出它。（如果到我们锁住它时
	 * 缓冲区已经干净，FlushBuffer 将什么也不做。）
	 */
	PinBuffer_Locked(bufHdr);
	LWLockAcquire(BufferDescriptorGetContentLock(bufHdr), LW_SHARED);

	FlushBuffer(bufHdr, NULL, IOOBJECT_RELATION, IOCONTEXT_NORMAL);

	LWLockRelease(BufferDescriptorGetContentLock(bufHdr));

	tag = bufHdr->tag;

	UnpinBuffer(bufHdr);

	/*
	 * SyncOneBuffer() 只由 checkpointer 和 bgwriter 调用，
	 * 因此 IOContext 将始终是 IOCONTEXT_NORMAL。
	 */
	ScheduleBufferTagForWriteback(wb_context, IOCONTEXT_NORMAL, &tag);

	return result | BUF_WRITTEN;
}

/*
 *		AtEOXact_Buffers —— 在事务结束时做清理。
 *
 *		自 PostgreSQL 8.0 起，缓冲区 pin 应由
 *		ResourceOwner 机制释放。本例程只是一个调试用的
 *		交叉检查，确认没有遗留的 pin。
 */
void
AtEOXact_Buffers(bool isCommit)
{
	CheckForBufferLeaks();

	AtEOXact_LocalBuffers(isCommit);

	Assert(PrivateRefCountOverflowed == 0);
}

/*
 * 初始化对共享缓冲区池的访问
 *
 * 在后端启动时（无论是独立运行还是运行于 postmaster
 * 之下）被调用。它为本后端访问已经存在的缓冲区池
 * 做好准备。
 */
void
InitBufferManagerAccess(void)
{
	HASHCTL		hash_ctl;

	/*
	 * 基于 shared_buffers 和可能的最大连接数，对每
	 * 个后端应持有的 pin 数量给出一个建议性上限。
	 * 这非常保守，但在非玩具尺寸的 shared_buffers 下
	 * 应该能允许相当多的 pin。LimitAdditionalPins() 和
	 * GetAdditionalPinLimit() 可用于检查剩余的余额。
	 */
	MaxProportionalPins = NBuffers / (MaxBackends + NUM_AUXILIARY_PROCS);

	memset(&PrivateRefCountArray, 0, sizeof(PrivateRefCountArray));

	hash_ctl.keysize = sizeof(int32);
	hash_ctl.entrysize = sizeof(PrivateRefCountEntry);

	PrivateRefCountHash = hash_create("PrivateRefCount", 100, &hash_ctl,
									  HASH_ELEM | HASH_BLOBS);

	/*
	 * AtProcExit_Buffers 需要 LWLock 访问，因此必须在
	 * 后端关闭的相应阶段被调用。
	 */
	Assert(MyProc != NULL);
	on_shmem_exit(AtProcExit_Buffers, 0);
}

/*
 * 在后端退出期间，确保我们已释放所有共享缓冲区锁，
 * 并断言我们没有遗留的 pin。
 */
static void
AtProcExit_Buffers(int code, Datum arg)
{
	UnlockBuffers();

	CheckForBufferLeaks();

	/* localbuf.c 也需要一个机会 */
	AtProcExit_LocalBuffers();
}

/*
 *		CheckForBufferLeaks —— 确保本后端没有持有任何缓冲区 pin
 *
 *		自 PostgreSQL 8.0 起，缓冲区 pin 应由
 *		ResourceOwner 机制释放。本例程只是一个调试用的
 *		交叉检查，确认没有遗留的 pin。
 */
static void
CheckForBufferLeaks(void)
{
#ifdef USE_ASSERT_CHECKING
	int			RefCountErrors = 0;
	PrivateRefCountEntry *res;
	int			i;
	char	   *s;

	/* 检查数组 */
	for (i = 0; i < REFCOUNT_ARRAY_ENTRIES; i++)
	{
		res = &PrivateRefCountArray[i];

		if (res->buffer != InvalidBuffer)
		{
			s = DebugPrintBufferRefcount(res->buffer);
			elog(WARNING, "buffer refcount leak: %s", s);
			pfree(s);

			RefCountErrors++;
		}
	}

	/* 必要时搜索哈希 */
	if (PrivateRefCountOverflowed)
	{
		HASH_SEQ_STATUS hstat;

		hash_seq_init(&hstat, PrivateRefCountHash);
		while ((res = (PrivateRefCountEntry *) hash_seq_search(&hstat)) != NULL)
		{
			s = DebugPrintBufferRefcount(res->buffer);
			elog(WARNING, "buffer refcount leak: %s", s);
			pfree(s);
			RefCountErrors++;
		}
	}

	Assert(RefCountErrors == 0);
#endif
}

#ifdef USE_ASSERT_CHECKING
/*
 * 检查被排他锁定的系统表缓冲区。这是
 * AssertCouldGetRelation() 的核心。
 *
 * 如果系统表扫描读取了被排他锁定的缓冲区，后端会在
 * LWLock 上自死锁。主要威胁来自 relcache 所用系统表
 * 的被排他锁定的缓冲区，因为对任何系统表的 catcache
 * 搜索都可能构建该系统表的 relcache 项。我们没有
 * relcache 使用了哪些系统表的清单，因此只检查
 * 大多数系统表的缓冲区。
 *
 * 在持有排他缓冲区锁时最好尽量减少等待，因此
 * 最好能将此检查扩展到不局限于系统表。然而，
 * bttextcmp() 会访问 pg_collation，而非核心的操作类
 * 可能类似地读取表。只要依赖图中没有环，这就是无死锁的：
 * 修改表 A 可能导致某个操作类读取表 B，但它绝不能
 * 导致读取表 A。
 */
void
AssertBufferLocksPermitCatalogRead(void)
{
	ForEachLWLockHeldByMe(AssertNotCatalogBufferLock, NULL);
}

static void
AssertNotCatalogBufferLock(LWLock *lock, LWLockMode mode,
						   void *unused_context)
{
	BufferDesc *bufHdr;
	BufferTag	tag;
	Oid			relid;

	if (mode != LW_EXCLUSIVE)
		return;

	if (!((BufferDescPadded *) lock > BufferDescriptors &&
		  (BufferDescPadded *) lock < BufferDescriptors + NBuffers))
		return;					/* not a buffer lock */

	bufHdr = (BufferDesc *)
		((char *) lock - offsetof(BufferDesc, content_lock));
	tag = bufHdr->tag;

	/*
	 * 这个 relNumber==relid 的假设在系统表经历
	 * VACUUM FULL 或类似操作之前都成立。在执行了
	 * 这样的命令之后，relNumber 会落入普通的
	 * （非系统表）范围，而我们将失去检测对该系统表
	 * 危险访问的能力。调用 RelidByRelfilenumber() 可以
	 * 弥补这一缺口，但 RelidByRelfilenumber() 可能会
	 * 与一个已持有的锁发生死锁。
	 */
	relid = tag.relNumber;

	if (IsCatalogTextUniqueIndexOid(relid)) /* 参见被调函数处的注释 */
		return;

	Assert(!IsCatalogRelationOid(relid));
}
#endif


/*
 * 当缓冲区被意外 pin 时，发出警告的辅助例程
 */
char *
DebugPrintBufferRefcount(Buffer buffer)
{
	BufferDesc *buf;
	int32		loccount;
	char	   *result;
	ProcNumber	backend;
	uint32		buf_state;

	Assert(BufferIsValid(buffer));
	if (BufferIsLocal(buffer))
	{
		buf = GetLocalBufferDescriptor(-buffer - 1);
		loccount = LocalRefCount[-buffer - 1];
		backend = MyProcNumber;
	}
	else
	{
		buf = GetBufferDescriptor(buffer - 1);
		loccount = GetPrivateRefCount(buffer);
		backend = INVALID_PROC_NUMBER;
	}

	/* 理论上我们这里应该锁住 bufhdr */
	buf_state = pg_atomic_read_u32(&buf->state);

	result = psprintf("[%03d] (rel=%s, blockNum=%u, flags=0x%x, refcount=%u %d)",
					  buffer,
					  relpathbackend(BufTagGetRelFileLocator(&buf->tag), backend,
									 BufTagGetForkNum(&buf->tag)).str,
					  buf->tag.blockNum, buf_state & BUF_FLAG_MASK,
					  BUF_STATE_GET_REFCOUNT(buf_state), loccount);
	return result;
}

/*
 * CheckPointBuffers
 *
 * 在检查点时刻，将缓冲区池中所有脏块刷写到磁盘。
 *
 * 注意：临时关系不参与检查点，因此不需要被刷写。
 */
void
CheckPointBuffers(int flags)
{
	BufferSync(flags);
}

/*
 * BufferGetBlockNumber
 *		返回与某个缓冲区关联的那个块号。
 *
 * 注意：
 *		假设缓冲区是有效且已被 pin 的，否则该
 *		值可能立刻就过时了……
 */
BlockNumber
BufferGetBlockNumber(Buffer buffer)
{
	BufferDesc *bufHdr;

	Assert(BufferIsPinned(buffer));

	if (BufferIsLocal(buffer))
		bufHdr = GetLocalBufferDescriptor(-buffer - 1);
	else
		bufHdr = GetBufferDescriptor(buffer - 1);

	/* 已被 pin，因此可以在不持自旋锁的情况下读取 tag */
	return bufHdr->tag.blockNum;
}

/*
 * BufferGetTag
 *		返回与某个缓冲区关联的 relfilelocator、fork 号
 *		和块号。
 */
void
BufferGetTag(Buffer buffer, RelFileLocator *rlocator, ForkNumber *forknum,
			 BlockNumber *blknum)
{
	BufferDesc *bufHdr;

	/* 执行与 BufferGetBlockNumber 相同的检查。 */
	Assert(BufferIsPinned(buffer));

	if (BufferIsLocal(buffer))
		bufHdr = GetLocalBufferDescriptor(-buffer - 1);
	else
		bufHdr = GetBufferDescriptor(buffer - 1);

	/* 已被 pin，因此可以在不持自旋锁的情况下读取 tag */
	*rlocator = BufTagGetRelFileLocator(&bufHdr->tag);
	*forknum = BufTagGetForkNum(&bufHdr->tag);
	*blknum = bufHdr->tag.blockNum;
}

/*
 * FlushBuffer
 *		物理地写出一个共享缓冲区。
 *
 * 注意：这实际上只是把缓冲区内容交给内核；真正的
 * 写盘要等到内核愿意时才发生。从我们的角度看这没问题，
 * 因为我们可以从 WAL 重做这些改动。不过，在我们能
 * 做 WAL 检查点之前，需要通过 fsync 强制把这些改动
 * 落到磁盘。
 *
 * 调用者必须持有缓冲区的 pin，并且已经对缓冲区内容
 * 加了共享锁。（注意：共享锁并不能阻止对缓冲区中
 * hint 位的更新，因此页面可能在写入过程中改变，但我们
 * 假设这不会使写出的数据失效。）
 *
 * 如果调用者持有缓冲区关系的 smgr 引用，请把它作为
 * 第二个参数传入。否则传入 NULL。
 */
static void
FlushBuffer(BufferDesc *buf, SMgrRelation reln, IOObject io_object,
			IOContext io_context)
{
	XLogRecPtr	recptr;
	ErrorContextCallback errcallback;
	instr_time	io_start;
	Block		bufBlock;
	char	   *bufToWrite;
	uint32		buf_state;

	/*
	 * 尝试启动一次 I/O 操作。如果 StartBufferIO 返回
	 * false，说明在我们之前已经有别人冲刷了该缓冲区，
	 * 因此我们无需做任何事。
	 */
	if (!StartBufferIO(buf, false, false))
		return;

	/* 为 ereport() 设置错误回溯支持 */
	errcallback.callback = shared_buffer_write_error_callback;
	errcallback.arg = buf;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* 为缓冲区查找 smgr 关系 */
	if (reln == NULL)
		reln = smgropen(BufTagGetRelFileLocator(&buf->tag), INVALID_PROC_NUMBER);

	TRACE_POSTGRESQL_BUFFER_FLUSH_START(BufTagGetForkNum(&buf->tag),
										buf->tag.blockNum,
										reln->smgr_rlocator.locator.spcOid,
										reln->smgr_rlocator.locator.dbOid,
										reln->smgr_rlocator.locator.relNumber);

	buf_state = LockBufHdr(buf);

	/*
	 * 在持有头锁的情况下执行 PageGetLSN，因为我们
	 * 在所有情况下都没有排他地锁住缓冲区。
	 */
	recptr = BufferGetLSN(buf);

	/* 检查在冲刷期间块内容是否发生变化。- vadim 01/17/97 */
	buf_state &= ~BM_JUST_DIRTIED;
	UnlockBufHdr(buf, buf_state);

	/*
	 * 强制 XLOG 刷新到缓冲区的 LSN 处。这就实现了基本的
	 * WAL 规则：日志更新必须在其所描述的数据文件改动
	 * 之前落到磁盘。
	 *
	 * 然而，这条规则不适用于未记录日志（unlogged）的关系，
	 * 它们在崩溃后反正也会丢失。大多数未记录日志关系
	 * 的页面不带有 LSN，因为我们从不为此发出 WAL 记录，
	 * 因此冲刷到缓冲区 LSN 处会毫无用处，但也无害。
	 * 不过，GiST 索引在内部使用 LSN 来跟踪页分裂，
	 * 因此未记录日志的 GiST 页带有由
	 * GetFakeLSNForUnloggedRel 生成的“伪造”LSN。
	 * 伪造的 LSN 计数器超过 WAL 插入点的可能性极小，
	 * 但并非不可能；而一旦真的发生，试图冲刷 WAL 到
	 * 该位置将会失败，并带来灾难性的系统级后果。
	 * 为确保这不会发生，如果缓冲区不是永久的，就跳过
	 * 这次冲刷。
	 */
	if (buf_state & BM_PERMANENT)
		XLogFlush(recptr);

	/*
	 * 现在可以安全地把缓冲区写到磁盘了。注意，在我们
	 * 忙于冲刷日志期间，不应有其他人能够写出它，
	 * 因为我们通过设置 BM_IO_IN_PROGRESS 位获得了
	 * 执行 I/O 的排他权利。
	 */
	bufBlock = BufHdrGetBlock(buf);

	/*
	 * 如果需要，更新页面校验和。由于我们只有缓冲区的
	 * 共享锁，其他进程可能正在更新其中的 hint 位，
	 * 因此如果要做校验和计算，必须把页面复制到
	 * 私有存储中。
	 */
	bufToWrite = PageSetChecksumCopy((Page) bufBlock, buf->tag.blockNum);

	io_start = pgstat_prepare_io_time(track_io_timing);

	/*
	 * bufToWrite 要么是共享缓冲区，要么是它的副本，
	 * 视情况而定。
	 */
	smgrwrite(reln,
			  BufTagGetForkNum(&buf->tag),
			  buf->tag.blockNum,
			  bufToWrite,
			  false);

	/*
	 * 当使用了策略时，只有对已经位于策略环中的脏
	 * 缓冲区的冲刷，才会被计为策略写
	 * （IOCONTEXT [BULKREAD|BULKWRITE|VACUUM]
	 * IOOP_WRITE），用于 IO 统计跟踪。
	 *
	 * 如果一个最初加入环的共享缓冲区在被使用前
	 * 必须被冲刷，这会被计为一次 IOCONTEXT_NORMAL
	 * IOOP_WRITE。
	 *
	 * 如果一个共享缓冲区是因为当前策略缓冲区被 pin 或
	 * 占用、或因为所有策略缓冲区都脏且被拒绝
	 * （仅针对 BAS_BULKREAD 操作）而后来才加入环，
	 * 且需要冲刷，这会被计为一次 IOCONTEXT_NORMAL
	 * IOOP_WRITE（from_ring 将为 false）。
	 *
	 * 当没有使用策略时，这次写只能是脏共享缓冲区
	 * 的一次“常规”写（IOCONTEXT_NORMAL IOOP_WRITE）。
	 */
	pgstat_count_io_op_time(IOOBJECT_RELATION, io_context,
							IOOP_WRITE, io_start, 1, BLCKSZ);

	pgBufferUsage.shared_blks_written++;

	/*
	 * 将缓冲区标记为干净（除非 BM_JUST_DIRTIED 已被设置），
	 * 并结束 BM_IO_IN_PROGRESS 状态。
	 */
	TerminateBufferIO(buf, true, 0, true, false);

	TRACE_POSTGRESQL_BUFFER_FLUSH_DONE(BufTagGetForkNum(&buf->tag),
									   buf->tag.blockNum,
									   reln->smgr_rlocator.locator.spcOid,
									   reln->smgr_rlocator.locator.dbOid,
									   reln->smgr_rlocator.locator.relNumber);

	/* 弹出错误上下文栈 */
	error_context_stack = errcallback.previous;
}

/*
 * RelationGetNumberOfBlocksInFork
 *		确定指定关系 fork 中当前的页数。
 *
 * 注意，结果的准确性取决于关系存储的细节。对于
 * 内建 AM 它是准确的，但对于外部 AM 可能并不准确。
 */
BlockNumber
RelationGetNumberOfBlocksInFork(Relation relation, ForkNumber forkNum)
{
	if (RELKIND_HAS_TABLE_AM(relation->rd_rel->relkind))
	{
		/*
		 * 并非每个表 AM 都使用 BLCKSZ 宽度的定长块。
		 * 因此 tableam 返回的是字节数——但就本例程的
		 * 用途而言，我们要的是块数。因此要除以块大小
		 * 并向上取整。
		 */
		uint64		szbytes;

		szbytes = table_relation_size(relation, forkNum);

		return (szbytes + (BLCKSZ - 1)) / BLCKSZ;
	}
	else if (RELKIND_HAS_STORAGE(relation->rd_rel->relkind))
	{
		return smgrnblocks(RelationGetSmgr(relation), forkNum);
	}
	else
		Assert(false);

	return 0;					/* keep compiler quiet */
}

/*
 * BufferIsPermanent
 *		确定缓冲区在崩溃后是否可能仍然存在。
 *		调用者必须持有缓冲区 pin。
 */
bool
BufferIsPermanent(Buffer buffer)
{
	BufferDesc *bufHdr;

	/* 本地缓冲区只用于临时关系。 */
	if (BufferIsLocal(buffer))
		return false;

	/* 确保我们拿到的是真实的缓冲区，并且持有它的 pin。 */
	Assert(BufferIsValid(buffer));
	Assert(BufferIsPinned(buffer));

	/*
	 * BM_PERMANENT 在我们持有缓冲区 pin 期间无法被改变，
	 * 因此我们无需费心去获取缓冲区头自旋锁。即使
	 * 别人在我们这样做时改变了缓冲区头状态，该状态也是
	 * 原子地改变的，所以我们读到的要么是旧值要么是
	 * 新值，而不会是随机垃圾。
	 */
	bufHdr = GetBufferDescriptor(buffer - 1);
	return (pg_atomic_read_u32(&bufHdr->state) & BM_PERMANENT) != 0;
}

/*
 * BufferGetLSNAtomic
 *		使用缓冲区头锁，原子地取回缓冲区的 LSN。
 *		对于那些可能没有持有缓冲区排他锁的调用者
 *		而言，这是必要的。
 */
XLogRecPtr
BufferGetLSNAtomic(Buffer buffer)
{
	char	   *page = BufferGetPage(buffer);
	BufferDesc *bufHdr;
	XLogRecPtr	lsn;
	uint32		buf_state;

	/*
	 * 如果为了正确性我们不需要加锁，就走快速路径退出。
	 */
	if (!XLogHintBitIsNeeded() || BufferIsLocal(buffer))
		return PageGetLSN(page);

	/* 确保我们拿到的是真实的缓冲区，并且持有它的 pin。 */
	Assert(BufferIsValid(buffer));
	Assert(BufferIsPinned(buffer));

	bufHdr = GetBufferDescriptor(buffer - 1);
	buf_state = LockBufHdr(bufHdr);
	lsn = PageGetLSN(page);
	UnlockBufHdr(bufHdr, buf_state);

	return lsn;
}

/* ---------------------------------------------------------------------
 *		DropRelationBuffers
 *
 *		本函数从缓冲区池中移除指定关系各 fork 中
 *		块号 >= firstDelBlock 的所有页面。
 *		（特别地，当 firstDelBlock = 0 时，所有页面都被移除。）
 *		脏页面只是简单地被丢弃，而不必先写出它们。
 *		因此，这不可回滚，应极其谨慎地使用！
 *
 *		目前，这只从 smgr.c 在底层文件即将被删除或
 *		截断时调用（截断情形需要 firstDelBlock）。
 *		受影响页面中的数据反正稍后就会被删除，
 *		因此没有写出它的必要。由高层代码负责确保
 *		删除或截断不会丢失以后可能需要的任何数据。
 *		也由高层代码负责确保没有其他进程可能正试图
 *		把该关系的更多页面载入缓冲区。
 * --------------------------------------------------------------------
 */
void
DropRelationBuffers(SMgrRelation smgr_reln, ForkNumber *forkNum,
					int nforks, BlockNumber *firstDelBlock)
{
	int			i;
	int			j;
	RelFileLocatorBackend rlocator;
	BlockNumber nForkBlock[MAX_FORKNUM];
	uint64		nBlocksToInvalidate = 0;

	rlocator = smgr_reln->smgr_rlocator;

	/* 如果是本地关系，那就是 localbuf.c 的事了。 */
	if (RelFileLocatorBackendIsTemp(rlocator))
	{
		if (rlocator.backend == MyProcNumber)
		{
			for (j = 0; j < nforks; j++)
				DropRelationLocalBuffers(rlocator.locator, forkNum[j],
										 firstDelBlock[j]);
		}
		return;
	}

	/*
	 * 要从缓冲区池中移除指定关系各 fork 的所有页面，
	 * 我们需要扫描整个缓冲区池，但如果我们已知
	 * 关系每个 fork 的精确大小，就可以通过从
	 * BufMapping 表查找缓冲区来优化。需要精确大小
	 * 是为了确保我们不会给正在被丢弃的关系留下任何
	 * 缓冲区，否则后台写进程或检查点进程在冲刷
	 * 对应不存在文件的缓冲区时会导致 PANIC 错误。
	 *
	 * 为了知道精确大小，我们依赖于在恢复期间由我们
	 * 为每个 fork 缓存的大小，这把优化限制在了
	 * 恢复期间和备机上，但一旦我们有了关系大小的
	 * 共享缓存，就可以轻易地扩展它。
	 *
	 * 在恢复期间，我们缓存第一次 lseek(SEEK_END)
	 * 返回的值，而后续的写入会使缓存值保持最新。
	 * 参见 smgrextend。由于有缺陷的 Linux 内核
	 * 可能没有计入最近的写入，第一次 lseek 的值
	 * 可能小于文件中实际存在的块数。但这应该没事，
	 * 因为在那个文件大小之后必定不会有任何缓冲区。
	 */
	for (i = 0; i < nforks; i++)
	{
		/* 获取关系某个 fork 的块数 */
		nForkBlock[i] = smgrnblocks_cached(smgr_reln, forkNum[i]);

		if (nForkBlock[i] == InvalidBlockNumber)
		{
			nBlocksToInvalidate = InvalidBlockNumber;
			break;
		}

		/* 计算要失效的块数 */
		nBlocksToInvalidate += (nForkBlock[i] - firstDelBlock[i]);
	}

	/*
	 * 只有当要失效的块的总数低于
	 * BUF_DROP_FULL_SCAN_THRESHOLD 时，
	 * 我们才应用这一优化。
	 */
	if (BlockNumberIsValid(nBlocksToInvalidate) &&
		nBlocksToInvalidate < BUF_DROP_FULL_SCAN_THRESHOLD)
	{
		for (j = 0; j < nforks; j++)
			FindAndDropRelationBuffers(rlocator.locator, forkNum[j],
									   nForkBlock[j], firstDelBlock[j]);
		return;
	}

	for (i = 0; i < NBuffers; i++)
	{
		BufferDesc *bufHdr = GetBufferDescriptor(i);
		uint32		buf_state;

		/*
		 * 我们可以先在尝试锁住缓冲区之前预检查缓冲区
		 * tag，从而让这一过程稍快一些；这在典型情况下
		 * 能省去大量锁的获取。这应当是安全的，因为
		 * 调用者必须持有关系的 AccessExclusiveLock，
		 * 或有其他理由确信没有人正在把该关系的新页面
		 * 载入缓冲区池。（否则我们很可能完全漏掉
		 * 这些页面。）因此，虽然 tag 在我们查看时
		 * 可能正在改变，但它不可能正改变*为*我们所
		 * 关心的某个值，只可能改变*离开*那样的值。
		 * 所以不可能出现假阴性，而假阳性是安全的，
		 * 因为我们会在获取缓冲区锁之后重新检查。
		 *
		 * 我们也可以检查 forkNum 和 blockNum 以及
		 * rlocator，但这样做带来的增量收益似乎很小。
		 */
		if (!BufTagMatchesRelFileLocator(&bufHdr->tag, &rlocator.locator))
			continue;

		buf_state = LockBufHdr(bufHdr);

		for (j = 0; j < nforks; j++)
		{
			if (BufTagMatchesRelFileLocator(&bufHdr->tag, &rlocator.locator) &&
				BufTagGetForkNum(&bufHdr->tag) == forkNum[j] &&
				bufHdr->tag.blockNum >= firstDelBlock[j])
			{
				InvalidateBuffer(bufHdr);	/* 释放自旋锁 */
				break;
			}
		}
		if (j >= nforks)
			UnlockBufHdr(bufHdr, buf_state);
	}
}

/* ---------------------------------------------------------------------
 *		DropRelationsAllBuffers
 *
 *		本函数从缓冲区池中移除指定关系的所有 fork
 *		的所有页面。它等价于对每个关系每个 fork
 *		各调用一次 DropRelationBuffers，且 firstDelBlock = 0。
 *		--------------------------------------------------------------------
 */
void
DropRelationsAllBuffers(SMgrRelation *smgr_reln, int nlocators)
{
	int			i;
	int			n = 0;
	SMgrRelation *rels;
	BlockNumber (*block)[MAX_FORKNUM + 1];
	uint64		nBlocksToInvalidate = 0;
	RelFileLocator *locators;
	bool		cached = true;
	bool		use_bsearch;

	if (nlocators == 0)
		return;

	rels = palloc(sizeof(SMgrRelation) * nlocators);	/* 非本地关系 */

	/* 如果是本地关系，那就是 localbuf.c 的事了。 */
	for (i = 0; i < nlocators; i++)
	{
		if (RelFileLocatorBackendIsTemp(smgr_reln[i]->smgr_rlocator))
		{
			if (smgr_reln[i]->smgr_rlocator.backend == MyProcNumber)
				DropRelationAllLocalBuffers(smgr_reln[i]->smgr_rlocator.locator);
		}
		else
			rels[n++] = smgr_reln[i];
	}

	/*
	 * If there are no non-local relations, then we're done. Release the
	 * memory and return.
	 */
	if (n == 0)
	{
		pfree(rels);
		return;
	}

	/*
	 * 这用于记住所有关系各 fork 的块数。
	 */
	block = (BlockNumber (*)[MAX_FORKNUM + 1])
		palloc(sizeof(BlockNumber) * n * (MAX_FORKNUM + 1));

	/*
	 * 如果我们已知给定关系各 fork 的精确大小，
	 * 就可以避免扫描整个缓冲区池。参见 DropRelationBuffers。
	 */
	for (i = 0; i < n && cached; i++)
	{
		for (int j = 0; j <= MAX_FORKNUM; j++)
		{
			/* 获取关系某个 fork 的块数。 */
			block[i][j] = smgrnblocks_cached(rels[i], j);

			/* 我们只需考虑存在的那些关系 fork。 */
			if (block[i][j] == InvalidBlockNumber)
			{
				if (!smgrexists(rels[i], j))
					continue;
				cached = false;
				break;
			}

			/* 计算要失效的块的总数 */
			nBlocksToInvalidate += block[i][j];
		}
	}

	/*
	 * 只有当要失效的块的总数低于
	 * BUF_DROP_FULL_SCAN_THRESHOLD 时，
	 * 我们才应用这一优化。
	 */
	if (cached && nBlocksToInvalidate < BUF_DROP_FULL_SCAN_THRESHOLD)
	{
		for (i = 0; i < n; i++)
		{
			for (int j = 0; j <= MAX_FORKNUM; j++)
			{
				/* 忽略不存在的关系 fork */
				if (!BlockNumberIsValid(block[i][j]))
					continue;

				/* 丢弃某个特定关系 fork 的所有缓冲区 */
				FindAndDropRelationBuffers(rels[i]->smgr_rlocator.locator,
										   j, block[i][j], 0);
			}
		}

		pfree(block);
		pfree(rels);
		return;
	}

	pfree(block);
	locators = palloc(sizeof(RelFileLocator) * n);	/* 非本地关系 */
	for (i = 0; i < n; i++)
		locators[i] = rels[i]->smgr_rlocator.locator;

	/*
	 * 对于数量很少的关系，直接简单地遍历一遍即可，
	 * 以省去 bsearch 的开销。所使用的阈值与其说是
	 * 精确确定的值，不如说是一种猜测，因为它
	 * 取决于许多因素（CPU 与 RAM 速度、共享缓冲区
	 * 数量等）。
	 */
	use_bsearch = n > RELS_BSEARCH_THRESHOLD;

	/* 必要时对 rlocator 列表排序 */
	if (use_bsearch)
		qsort(locators, n, sizeof(RelFileLocator), rlocator_comparator);

	for (i = 0; i < NBuffers; i++)
	{
		RelFileLocator *rlocator = NULL;
		BufferDesc *bufHdr = GetBufferDescriptor(i);
		uint32		buf_state;

		/*
		 * 与 DropRelationBuffers 中一样，不加锁的
		 * 预检查应当是安全的，并能节省一些周期。
		 */

		if (!use_bsearch)
		{
			int			j;

			for (j = 0; j < n; j++)
			{
				if (BufTagMatchesRelFileLocator(&bufHdr->tag, &locators[j]))
				{
					rlocator = &locators[j];
					break;
				}
			}
		}
		else
		{
			RelFileLocator locator;

			locator = BufTagGetRelFileLocator(&bufHdr->tag);
			rlocator = bsearch(&locator,
							   locators, n, sizeof(RelFileLocator),
							   rlocator_comparator);
		}

		/* 缓冲区不属于任何给定的 relfilelocator；跳过它 */
		if (rlocator == NULL)
			continue;

		buf_state = LockBufHdr(bufHdr);
		if (BufTagMatchesRelFileLocator(&bufHdr->tag, rlocator))
			InvalidateBuffer(bufHdr);	/* releases spinlock */
		else
			UnlockBufHdr(bufHdr, buf_state);
	}

	pfree(locators);
	pfree(rels);
}

/* ---------------------------------------------------------------------
 *		FindAndDropRelationBuffers
 *
 *		本函数在 BufMapping 表中查找，并从缓冲区池中
 *		移除指定关系 fork 中块号 >= firstDelBlock 的
 *		所有页面。（特别地，当 firstDelBlock = 0 时，
 *		所有页面都被移除。）
 * --------------------------------------------------------------------
 */
static void
FindAndDropRelationBuffers(RelFileLocator rlocator, ForkNumber forkNum,
						   BlockNumber nForkBlock,
						   BlockNumber firstDelBlock)
{
	BlockNumber curBlock;

	for (curBlock = firstDelBlock; curBlock < nForkBlock; curBlock++)
	{
		uint32		bufHash;	/* tag 的哈希值 */
		BufferTag	bufTag;		/* 所请求块的标识 */
		LWLock	   *bufPartitionLock;	/* 它对应的缓冲区分区锁 */
		int			buf_id;
		BufferDesc *bufHdr;
		uint32		buf_state;

		/* 创建一个 tag，以便查找缓冲区 */
		InitBufferTag(&bufTag, &rlocator, forkNum, curBlock);

		/* 确定它的哈希码与分区锁 ID */
		bufHash = BufTableHashCode(&bufTag);
		bufPartitionLock = BufMappingPartitionLock(bufHash);

		/* 检查它是否在缓冲区池中。如果不在，就什么也不做。 */
		LWLockAcquire(bufPartitionLock, LW_SHARED);
		buf_id = BufTableLookup(&bufTag, bufHash);
		LWLockRelease(bufPartitionLock);

		if (buf_id < 0)
			continue;

		bufHdr = GetBufferDescriptor(buf_id);

		/*
		 * 我们需要锁住缓冲区头并重新检查缓冲区是否
		 * 仍然关联着同一个块，因为在释放 BufMapping
		 * 表上的锁之后，可能有其他后端为另一个关系
		 * 载入块而把它驱逐掉。
		 */
		buf_state = LockBufHdr(bufHdr);

		if (BufTagMatchesRelFileLocator(&bufHdr->tag, &rlocator) &&
			BufTagGetForkNum(&bufHdr->tag) == forkNum &&
			bufHdr->tag.blockNum >= firstDelBlock)
			InvalidateBuffer(bufHdr);	/* 释放自旋锁 */
		else
			UnlockBufHdr(bufHdr, buf_state);
	}
}

/* ---------------------------------------------------------------------
 *		DropDatabaseBuffers
 *
 *		本函数移除缓冲区缓存中属于某个特定数据库的
 *		所有缓冲区。脏页面只是简单地被丢弃，而不必先
 *		写出它们。当我们销毁一个数据库时，为了避免
 *		在目录树已不存在时还试图把数据刷到磁盘，会
 *		使用它。其实现与 DropRelationBuffers() 颇为
 *		相似，后者用于销毁单个关系。
 * --------------------------------------------------------------------
 */
void
DropDatabaseBuffers(Oid dbid)
{
	int			i;

	/*
	 * 我们无需考虑本地缓冲区，因为按假设，目标
	 * 数据库不是我们自己的。
	 */

	for (i = 0; i < NBuffers; i++)
	{
		BufferDesc *bufHdr = GetBufferDescriptor(i);
		uint32		buf_state;

		/*
		 * 与 DropRelationBuffers 中一样，不加锁的
		 * 预检查应当是安全的，并能节省一些周期。
		 */
		if (bufHdr->tag.dbOid != dbid)
			continue;

		buf_state = LockBufHdr(bufHdr);
		if (bufHdr->tag.dbOid == dbid)
			InvalidateBuffer(bufHdr);	/* releases spinlock */
		else
			UnlockBufHdr(bufHdr, buf_state);
	}
}

/* ---------------------------------------------------------------------
 *		FlushRelationBuffers
 *
 *		本函数将一个关系的所有脏页写出到磁盘
 *		（更准确地说，是写出到内核磁盘缓冲区），
 *		确保内核拥有该关系的最新视图。
 *
 *		通常，调用者应当持有目标关系的
 *		AccessExclusiveLock，以确保没有其他后端正忙于
 *		弄脏该关系更多的块；其效果在锁被释放后
 *		不能指望会持续。
 *
 *		XXX 目前它顺序扫描缓冲区池，应当改成更
 *		巧妙的搜索方式。本例程不会被用于任何
 *		对性能关键的代码路径，因此不值得为
 *		加速它而给常规路径增加额外开销。
 * --------------------------------------------------------------------
 */
void
FlushRelationBuffers(Relation rel)
{
	int			i;
	BufferDesc *bufHdr;
	SMgrRelation srel = RelationGetSmgr(rel);

	if (RelationUsesLocalBuffers(rel))
	{
		for (i = 0; i < NLocBuffer; i++)
		{
			uint32		buf_state;

			bufHdr = GetLocalBufferDescriptor(i);
			if (BufTagMatchesRelFileLocator(&bufHdr->tag, &rel->rd_locator) &&
				((buf_state = pg_atomic_read_u32(&bufHdr->state)) &
				 (BM_VALID | BM_DIRTY)) == (BM_VALID | BM_DIRTY))
			{
				ErrorContextCallback errcallback;

				/* 为 ereport() 设置错误回溯支持 */
				errcallback.callback = local_buffer_write_error_callback;
				errcallback.arg = bufHdr;
				errcallback.previous = error_context_stack;
				error_context_stack = &errcallback;

				/* 确保我们能处理这个 pin */
				ReservePrivateRefCountEntry();
				ResourceOwnerEnlarge(CurrentResourceOwner);

				/*
				 * pin/unpin 主要是为了能让 valgrind 工作，
				 * 但这看起来也是该做的正确之事。
				 */
				PinLocalBuffer(bufHdr, false);


				FlushLocalBuffer(bufHdr, srel);

				UnpinLocalBuffer(BufferDescriptorGetBuffer(bufHdr));

				/* 弹出错误上下文栈 */
				error_context_stack = errcallback.previous;
			}
		}

		return;
	}

	for (i = 0; i < NBuffers; i++)
	{
		uint32		buf_state;

		bufHdr = GetBufferDescriptor(i);

		/*
		 * 与 DropRelationBuffers 中一样，不加锁的
		 * 预检查应当是安全的，并能节省一些周期。
		 */
		if (!BufTagMatchesRelFileLocator(&bufHdr->tag, &rel->rd_locator))
			continue;

	/* 确保我们能处理这个 pin */
	ReservePrivateRefCountEntry();
		ResourceOwnerEnlarge(CurrentResourceOwner);

		buf_state = LockBufHdr(bufHdr);
		if (BufTagMatchesRelFileLocator(&bufHdr->tag, &rel->rd_locator) &&
			(buf_state & (BM_VALID | BM_DIRTY)) == (BM_VALID | BM_DIRTY))
		{
			PinBuffer_Locked(bufHdr);
			LWLockAcquire(BufferDescriptorGetContentLock(bufHdr), LW_SHARED);
			FlushBuffer(bufHdr, srel, IOOBJECT_RELATION, IOCONTEXT_NORMAL);
			LWLockRelease(BufferDescriptorGetContentLock(bufHdr));
			UnpinBuffer(bufHdr);
		}
		else
			UnlockBufHdr(bufHdr, buf_state);
	}
}

/* ---------------------------------------------------------------------
 *		FlushRelationsAllBuffers
 *
 *		本函数将缓冲区池中指定 smgr 关系的所有
 *		fork 的所有页面刷出。它等价于对每个
 *		关系各调用一次 FlushRelationBuffers。假定这些
 *		关系不使用本地缓冲区。
 * --------------------------------------------------------------------
 */
void
FlushRelationsAllBuffers(SMgrRelation *smgrs, int nrels)
{
	int			i;
	SMgrSortArray *srels;
	bool		use_bsearch;

	if (nrels == 0)
		return;

	/* 为 qsort 填充数组 */
	srels = palloc(sizeof(SMgrSortArray) * nrels);

	for (i = 0; i < nrels; i++)
	{
		Assert(!RelFileLocatorBackendIsTemp(smgrs[i]->smgr_rlocator));

		srels[i].rlocator = smgrs[i]->smgr_rlocator.locator;
		srels[i].srel = smgrs[i];
	}

	/*
	 * 对于数量很少、需要同步的关系，省去 bsearch 的
	 * 开销。详见 DropRelationsAllBuffers。
	 */
	use_bsearch = nrels > RELS_BSEARCH_THRESHOLD;

	/* 必要时对 SMgrRelation 列表排序 */
	if (use_bsearch)
		qsort(srels, nrels, sizeof(SMgrSortArray), rlocator_comparator);

	for (i = 0; i < NBuffers; i++)
	{
		SMgrSortArray *srelent = NULL;
		BufferDesc *bufHdr = GetBufferDescriptor(i);
		uint32		buf_state;

		/*
		 * 与 DropRelationBuffers 中一样，不加锁的
		 * 预检查应当是安全的，并能节省一些周期。
		 */

		if (!use_bsearch)
		{
			int			j;

			for (j = 0; j < nrels; j++)
			{
				if (BufTagMatchesRelFileLocator(&bufHdr->tag, &srels[j].rlocator))
				{
					srelent = &srels[j];
					break;
				}
			}
		}
		else
		{
			RelFileLocator rlocator;

			rlocator = BufTagGetRelFileLocator(&bufHdr->tag);
			srelent = bsearch(&rlocator,
							  srels, nrels, sizeof(SMgrSortArray),
							  rlocator_comparator);
		}

		/* buffer doesn't belong to any of the given relfilelocators; skip it */
		if (srelent == NULL)
			continue;

	/* 确保我们能处理这个 pin */
	ReservePrivateRefCountEntry();
		ResourceOwnerEnlarge(CurrentResourceOwner);

		buf_state = LockBufHdr(bufHdr);
		if (BufTagMatchesRelFileLocator(&bufHdr->tag, &srelent->rlocator) &&
			(buf_state & (BM_VALID | BM_DIRTY)) == (BM_VALID | BM_DIRTY))
		{
			PinBuffer_Locked(bufHdr);
			LWLockAcquire(BufferDescriptorGetContentLock(bufHdr), LW_SHARED);
			FlushBuffer(bufHdr, srelent->srel, IOOBJECT_RELATION, IOCONTEXT_NORMAL);
			LWLockRelease(BufferDescriptorGetContentLock(bufHdr));
			UnpinBuffer(bufHdr);
		}
		else
			UnlockBufHdr(bufHdr, buf_state);
	}

	pfree(srels);
}

/* ---------------------------------------------------------------------
 *		RelationCopyStorageUsingBuffer
 *
 *		使用 bufmgr 复制 fork 的数据。与 RelationCopyStorage
 *		相同，但此处改用 bufmgr 的 API 来复制，
 *		而非使用 smgrread 和 smgrextend。
 *
 *		关于 'permanent' 参数的细节，参见
 *		CreateAndCopyRelationData() 上方的注释。
 * --------------------------------------------------------------------
 */
static void
RelationCopyStorageUsingBuffer(RelFileLocator srclocator,
							   RelFileLocator dstlocator,
							   ForkNumber forkNum, bool permanent)
{
	Buffer		srcBuf;
	Buffer		dstBuf;
	Page		srcPage;
	Page		dstPage;
	bool		use_wal;
	BlockNumber nblocks;
	BlockNumber blkno;
	PGIOAlignedBlock buf;
	BufferAccessStrategy bstrategy_src;
	BufferAccessStrategy bstrategy_dst;
	BlockRangeReadStreamPrivate p;
	ReadStream *src_stream;
	SMgrRelation src_smgr;

	/*
	 * 一般来说，只要 wal_level > 'minimal' 我们就想写 WAL，
	 * 但在复制未记录日志关系的任意 fork（init fork 除外）
	 * 时可以跳过。
	 */
	use_wal = XLogIsNeeded() && (permanent || forkNum == INIT_FORKNUM);

	/* 获取源关系的块数。 */
	nblocks = smgrnblocks(smgropen(srclocator, INVALID_PROC_NUMBER),
						  forkNum);

	/* 没有可复制的内容；直接返回。 */
	if (nblocks == 0)
		return;

	/*
	 * 在开始逐块复制之前，先把目标关系批量扩展到与
	 * 源关系相同的大小。
	 */
	memset(buf.data, 0, BLCKSZ);
	smgrextend(smgropen(dstlocator, INVALID_PROC_NUMBER), forkNum, nblocks - 1,
			   buf.data, true);

	/* 这是一次批量操作，因此使用缓冲区访问策略。 */
	bstrategy_src = GetAccessStrategy(BAS_BULKREAD);
	bstrategy_dst = GetAccessStrategy(BAS_BULKWRITE);

	/* 初始化流读取 */
	p.current_blocknum = 0;
	p.last_exclusive = nblocks;
	src_smgr = smgropen(srclocator, INVALID_PROC_NUMBER);

	/*
	 * 使用批量模式是安全的，因为 block_range_read_stream_cb
	 * 不获取任何锁。
	 */
	src_stream = read_stream_begin_smgr_relation(READ_STREAM_FULL |
												 READ_STREAM_USE_BATCHING,
												 bstrategy_src,
												 src_smgr,
												 permanent ? RELPERSISTENCE_PERMANENT : RELPERSISTENCE_UNLOGGED,
												 forkNum,
												 block_range_read_stream_cb,
												 &p,
												 0);

	/* 遍历源关系文件的每一个块。 */
	for (blkno = 0; blkno < nblocks; blkno++)
	{
		CHECK_FOR_INTERRUPTS();

		/* 从源关系读取块。 */
		srcBuf = read_stream_next_buffer(src_stream, NULL);
		LockBuffer(srcBuf, BUFFER_LOCK_SHARE);
		srcPage = BufferGetPage(srcBuf);

		dstBuf = ReadBufferWithoutRelcache(dstlocator, forkNum,
										   BufferGetBlockNumber(srcBuf),
										   RBM_ZERO_AND_LOCK, bstrategy_dst,
										   permanent);
		dstPage = BufferGetPage(dstBuf);

		START_CRIT_SECTION();

		/* 将页面数据从源复制到目标。 */
		memcpy(dstPage, srcPage, BLCKSZ);
		MarkBufferDirty(dstBuf);

		/* 把复制的页面记入 WAL 日志。 */
		if (use_wal)
			log_newpage_buffer(dstBuf, true);

		END_CRIT_SECTION();

		UnlockReleaseBuffer(dstBuf);
		UnlockReleaseBuffer(srcBuf);
	}
	Assert(read_stream_next_buffer(src_stream, NULL) == InvalidBuffer);
	read_stream_end(src_stream);

	FreeAccessStrategy(bstrategy_src);
	FreeAccessStrategy(bstrategy_dst);
}

/* ---------------------------------------------------------------------
 *		CreateAndCopyRelationData
 *
 *		创建目标关系存储，并从源关系复制所有 fork
 *		到目标关系。
 *
 *		对于永久关系，将 permanent 传 true；
 *		对于未记录日志的关系，传 false。
 *		目前该 API 不支持临时关系。
 * --------------------------------------------------------------------
 */
void
CreateAndCopyRelationData(RelFileLocator src_rlocator,
						  RelFileLocator dst_rlocator, bool permanent)
{
	char		relpersistence;
	SMgrRelation src_rel;
	SMgrRelation dst_rel;

	/* 设置关系的持久性。 */
	relpersistence = permanent ?
		RELPERSISTENCE_PERMANENT : RELPERSISTENCE_UNLOGGED;

	src_rel = smgropen(src_rlocator, INVALID_PROC_NUMBER);
	dst_rel = smgropen(dst_rlocator, INVALID_PROC_NUMBER);

	/*
	 * 创建并复制该关系的所有 fork。在创建数据库期间，
	 * 我们有一个独立的清理机制来删除完整的数据库
	 * 目录。因此，每个单独的关系无需被登记
	 * 以便清理。
	 */
	RelationCreateStorage(dst_rlocator, relpersistence, false);

	/* 复制主 fork。 */
	RelationCopyStorageUsingBuffer(src_rlocator, dst_rlocator, MAIN_FORKNUM,
								   permanent);

	/* 复制那些存在的额外 fork */
	for (ForkNumber forkNum = MAIN_FORKNUM + 1;
		 forkNum <= MAX_FORKNUM; forkNum++)
	{
		if (smgrexists(src_rel, forkNum))
		{
			smgrcreate(dst_rel, forkNum, false);

			/*
			 * 如果关系是持久化的，或者这是未记录日志
			 * 关系的 init fork，则把创建记入 WAL 日志。
			 */
			if (permanent || forkNum == INIT_FORKNUM)
				log_smgrcreate(&dst_rlocator, forkNum);

			/* 逐块复制一个 fork 的数据。 */
			RelationCopyStorageUsingBuffer(src_rlocator, dst_rlocator, forkNum,
										   permanent);
		}
	}
}

/* ---------------------------------------------------------------------
 *		FlushDatabaseBuffers
 *
 *		本函数将一个数据库的所有脏页写出到磁盘
 *		（更准确地说，是写出到内核磁盘缓冲区），
 *		确保内核拥有该数据库的最新视图。
 *
 *		通常，调用者应当持有适当的锁，以确保没有
 *		其他后端在目标数据库中活跃；否则可能会有
 *		更多页面被弄脏。
 *
 *		注意，我们不考虑刷新任何临时关系的页面。
 *		假定这些页面并不重要。
 * --------------------------------------------------------------------
 */
void
FlushDatabaseBuffers(Oid dbid)
{
	int			i;
	BufferDesc *bufHdr;

	for (i = 0; i < NBuffers; i++)
	{
		uint32		buf_state;

		bufHdr = GetBufferDescriptor(i);

		/*
		 * 与 DropRelationBuffers 中一样，不加锁的
		 * 预检查应当是安全的，并能节省一些周期。
		 */
		if (bufHdr->tag.dbOid != dbid)
			continue;

	/* 确保我们能处理这个 pin */
	ReservePrivateRefCountEntry();
		ResourceOwnerEnlarge(CurrentResourceOwner);

		buf_state = LockBufHdr(bufHdr);
		if (bufHdr->tag.dbOid == dbid &&
			(buf_state & (BM_VALID | BM_DIRTY)) == (BM_VALID | BM_DIRTY))
		{
			PinBuffer_Locked(bufHdr);
			LWLockAcquire(BufferDescriptorGetContentLock(bufHdr), LW_SHARED);
			FlushBuffer(bufHdr, NULL, IOOBJECT_RELATION, IOCONTEXT_NORMAL);
			LWLockRelease(BufferDescriptorGetContentLock(bufHdr));
			UnpinBuffer(bufHdr);
		}
		else
			UnlockBufHdr(bufHdr, buf_state);
	}
}

/*
 * 将先前已被（共享或排他）锁定且被 pin 的缓冲区
 * 刷写到 OS。
 */
void
FlushOneBuffer(Buffer buffer)
{
	BufferDesc *bufHdr;

	/* 目前不需要，但没有不支持它的根本理由 */
	Assert(!BufferIsLocal(buffer));

	Assert(BufferIsPinned(buffer));

	bufHdr = GetBufferDescriptor(buffer - 1);

	Assert(LWLockHeldByMe(BufferDescriptorGetContentLock(bufHdr)));

	FlushBuffer(bufHdr, NULL, IOOBJECT_RELATION, IOCONTEXT_NORMAL);
}

/*
 * ReleaseBuffer —— 释放对缓冲区的 pin
 */
void
ReleaseBuffer(Buffer buffer)
{
	if (!BufferIsValid(buffer))
		elog(ERROR, "bad buffer ID: %d", buffer);

	if (BufferIsLocal(buffer))
		UnpinLocalBuffer(buffer);
	else
		UnpinBuffer(GetBufferDescriptor(buffer - 1));
}

/*
 * UnlockReleaseBuffer —— 释放缓冲区的内容锁与 pin
 *
 * 这只是一个常见组合的简写。
 */
void
UnlockReleaseBuffer(Buffer buffer)
{
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buffer);
}

/*
 * IncrBufferRefCount
 *		对我们*已经*至少 pin 过一次的缓冲区，
 *		递增其 pin 计数。
 *
 *		本函数不能用于我们并未持有 pin 的缓冲区，
 *		因为它不改变共享缓冲区状态。
 */
void
IncrBufferRefCount(Buffer buffer)
{
	Assert(BufferIsPinned(buffer));
	ResourceOwnerEnlarge(CurrentResourceOwner);
	if (BufferIsLocal(buffer))
		LocalRefCount[-buffer - 1]++;
	else
	{
		PrivateRefCountEntry *ref;

		ref = GetPrivateRefCountEntry(buffer, true);
		Assert(ref != NULL);
		ref->refcount++;
	}
	ResourceOwnerRememberBuffer(CurrentResourceOwner, buffer);
}

/*
 * MarkBufferDirtyHint
 *
 *	标记缓冲区为脏，用于非关键的改动。
 *
 * 这与 MarkBufferDirty 本质上是相同的，区别在于：
 *
 * 1. 调用者不写 WAL；因此如果启用了校验和，
 *	  我们可能需要写一条 XLOG_FPI_FOR_HINT 的 WAL
 *	  记录来防范页面撕裂。
 * 2. 调用者可能只持有共享锁而非缓冲区内容锁的
 *	  排他锁。
 * 3. 本函数不保证缓冲区总是被标记脏（由于竞争
 *	  条件），因此不能用于重要的改动。
 */
void
MarkBufferDirtyHint(Buffer buffer, bool buffer_std)
{
	BufferDesc *bufHdr;
	Page		page = BufferGetPage(buffer);

	if (!BufferIsValid(buffer))
		elog(ERROR, "bad buffer ID: %d", buffer);

	if (BufferIsLocal(buffer))
	{
		MarkLocalBufferDirty(buffer);
		return;
	}

	bufHdr = GetBufferDescriptor(buffer - 1);

	Assert(GetPrivateRefCount(buffer) > 0);
	/* 此处，共享锁或排他锁都可以 */
	Assert(LWLockHeldByMe(BufferDescriptorGetContentLock(bufHdr)));

	/*
	 * 如果我们正在做某个事务提交后、增删了许多
	 * 元组的首次扫描，本例程可能会在同一页面上
	 * 被调用很多次。因此，如果缓冲区已经脏了，
	 * 我们要尽可能快。我们通过在不获取自旋锁
	 * 的情况下判断——如果状态位看起来已经设置。
	 * 由于这个测试是不加锁的，我们可能会没能注意到
	 * 标志刚刚被清除，并因此没能重置它们，
	 * 这源于内存排序问题。但既然本函数只打算
	 * 用于那些即使没写出数据也无害的情况，
	 * 这其实并无大碍。
	 */
	if ((pg_atomic_read_u32(&bufHdr->state) & (BM_DIRTY | BM_JUST_DIRTIED)) !=
		(BM_DIRTY | BM_JUST_DIRTIED))
	{
		XLogRecPtr	lsn = InvalidXLogRecPtr;
		bool		dirtied = false;
		bool		delayChkptFlags = false;
		uint32		buf_state;

		/*
		 * 如果我们需要保护 hint 位更新免受页面撕裂，
		 * 就把页面的整页镜像记入 WAL 日志。这个整页
		 * 镜像只有在 hint 位更新是该页自上一次
		 * 检查点以来的首次改动时才是必要的。
		 *
		 * 我们这里不检查 full_page_writes，因为那段逻辑
		 * 已经包含在调用 XLogInsert() 中，毕竟该值
		 * 是动态变化的。
		 */
		if (XLogHintBitIsNeeded() &&
			(pg_atomic_read_u32(&bufHdr->state) & BM_PERMANENT))
		{
			/*
			 * 如果由于某个 relfilelocator 特定的条件，
			 * 或是处于恢复中，我们必须不写 WAL，那就
			 * 不要把页面弄脏。我们可以设置 hint 位，
			 * 只是因此不能把页面弄脏，所以当我们
			 * 驱逐该页面或关闭时，这个 hint 位就丢失了。
			 *
			 * 更长的讨论参见
			 * src/backend/storage/page/README。
			 */
			if (RecoveryInProgress() ||
				RelFileLocatorSkippingWAL(BufTagGetRelFileLocator(&bufHdr->tag)))
				return;

			/*
			 * 如果块已经因为我们的某次改动或之前设置的
			 * hint 位而变脏，那我们就不需要写整页镜像。
			 * 注意，积极地清理因设置 hint 位而变脏的
			 * 块会增加调用频率。而批量设置 hint 位
			 * 则会降低调用频率……
			 *
			 * 我们必须先发出 WAL 记录，再标记缓冲区为脏。
			 * 否则我们可能会在写 WAL 之前就写出页面。
			 * 那会造成竞争条件，因为检查点可能发生在
			 * 写 WAL 记录与标记缓冲区脏之间。我们用一个
			 * 虽然笨拙但已在事务提交期间用来防止竞争条件的
			 * 办法来解决它。基本上，我们只是阻止检查点
			 * WAL 记录被写出，直到我们已把缓冲区标记脏。
			 * 我们在标记脏之前不会启动检查点刷写，因此
			 * 我们的检查点必须把改动成功刷到磁盘，否则
			 * 检查点永远不会被写出，于是崩溃恢复会修复它。
			 *
			 * 我们可能在没有 xid 的情况下进入这里，所以
			 * CreateCheckPoint 等待虚拟事务而非完整事务
			 * id 这一点是至关重要的。
			 */
			Assert((MyProc->delayChkptFlags & DELAY_CHKPT_START) == 0);
			MyProc->delayChkptFlags |= DELAY_CHKPT_START;
			delayChkptFlags = true;
			lsn = XLogSaveBufferForHint(buffer, buffer_std);
		}

		buf_state = LockBufHdr(bufHdr);

		Assert(BUF_STATE_GET_REFCOUNT(buf_state) > 0);

		if (!(buf_state & BM_DIRTY))
		{
			dirtied = true;		/* 意为“将因本动作而被弄脏” */

			/*
			 * 如果我们写了一个备份块，就设置页面 LSN。我们只
			 * 持有共享锁时本不该设置它，但只要以某种
			 * 方式把它串行化就无妨。我们选择在持有
			 * 缓冲区头锁时设置 LSN，这会使得任何只持有
			 * 共享锁的 LSN 读取者，在使用 PageGetLSN()
			 * 之前也要先获取缓冲区头锁，这一点在
			 * BufferGetLSNAtomic() 中被强制要求。
			 *
			 * 如果启用了校验和，你也许以为我们应该在这里
			 * 重置校验和。那会在本检查点周期内稍后
			 * 写出该页面时发生。
			 */
			if (!XLogRecPtrIsInvalid(lsn))
				PageSetLSN(page, lsn);
		}

		buf_state |= BM_DIRTY | BM_JUST_DIRTIED;
		UnlockBufHdr(bufHdr, buf_state);

		if (delayChkptFlags)
			MyProc->delayChkptFlags &= ~DELAY_CHKPT_START;

		if (dirtied)
		{
			pgBufferUsage.shared_blks_dirtied++;
			if (VacuumCostActive)
				VacuumCostBalance += VacuumCostPageDirty;
		}
	}
}

/*
 * 释放共享缓冲区的内容锁。
 *
 * 用于错误之后的清理。
 *
 * 目前，我们可以预期 lwlock.c 的 LWLockReleaseAll()
 * 已经负责释放了缓冲区内容锁本身；在这里
 * 我们唯一需要处理的是清除任何正在进行中的
 * PIN_COUNT 请求。
 */
void
UnlockBuffers(void)
{
	BufferDesc *buf = PinCountWaitBuf;

	if (buf)
	{
		uint32		buf_state;

		buf_state = LockBufHdr(buf);

		/*
		 * 如果标志位没有设置，也不要抱怨；它可能
		 * 已经被重置，但我们是在收到信号之前
		 * 就得到了 cancel/die 中断。
		 */
		if ((buf_state & BM_PIN_COUNT_WAITER) != 0 &&
			buf->wait_backend_pgprocno == MyProcNumber)
			buf_state &= ~BM_PIN_COUNT_WAITER;

		UnlockBufHdr(buf, buf_state);

		PinCountWaitBuf = NULL;
	}
}

/*
 * Acquire or release the content_lock for the buffer.
 */
void
LockBuffer(Buffer buffer, int mode)
{
	BufferDesc *buf;

	Assert(BufferIsPinned(buffer));
	if (BufferIsLocal(buffer))
		return;					/* local buffers need no lock */

	buf = GetBufferDescriptor(buffer - 1);

	if (mode == BUFFER_LOCK_UNLOCK)
		LWLockRelease(BufferDescriptorGetContentLock(buf));
	else if (mode == BUFFER_LOCK_SHARE)
		LWLockAcquire(BufferDescriptorGetContentLock(buf), LW_SHARED);
	else if (mode == BUFFER_LOCK_EXCLUSIVE)
		LWLockAcquire(BufferDescriptorGetContentLock(buf), LW_EXCLUSIVE);
	else
		elog(ERROR, "unrecognized buffer lock mode: %d", mode);
}

/*
 * 获取缓冲区的内容锁，但仅当我们无需等待时。
 *
 * 这假定调用者想要 BUFFER_LOCK_EXCLUSIVE 模式。
 */
bool
ConditionalLockBuffer(Buffer buffer)
{
	BufferDesc *buf;

	Assert(BufferIsPinned(buffer));
	if (BufferIsLocal(buffer))
		return true;			/* act as though we got it */

	buf = GetBufferDescriptor(buffer - 1);

	return LWLockConditionalAcquire(BufferDescriptorGetContentLock(buf),
									LW_EXCLUSIVE);
}

/*
 * 验证本后端恰好 pin 了缓冲区一次。
 *
 * 注意：与 BufferIsPinned() 中一样，我们在这里
 * 检查的是*本*后端持有该缓冲区的 pin。
 * 我们并不关心其他某个后端是否持有。
 */
void
CheckBufferIsPinnedOnce(Buffer buffer)
{
	if (BufferIsLocal(buffer))
	{
		if (LocalRefCount[-buffer - 1] != 1)
			elog(ERROR, "incorrect local pin count: %d",
				 LocalRefCount[-buffer - 1]);
	}
	else
	{
		if (GetPrivateRefCount(buffer) != 1)
			elog(ERROR, "incorrect local pin count: %d",
				 GetPrivateRefCount(buffer));
	}
}

/*
 * LockBufferForCleanup —— 锁定一个缓冲区，为删除其中的
 * 项做准备
 *
 * 只有当调用者 (a) 持有该缓冲区的排他锁，并且
 * (b) 观察到没有其他后端持有该缓冲区的 pin 时，
 * 才能从磁盘页中删除项。如果存在 pin，那么另
 * 一个后端可能持有指向该缓冲区的指针（例如，
 * 一个堆扫描对某个项的引用——更多细节见
 * README）。不过，如果在清理开始之后才加上
 * pin 则没关系；新到达的后端在我们释放
 * 排他锁之前将无法查看该页面。
 *
 * 为了实现这一协议，想要删除者必须先 pin 缓冲区，
 * 然后调用 LockBufferForCleanup()。它与
 * LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE) 类似，
 * 区别在于它会循环，直到成功观察到 pin 计数为 1。
 */
void
LockBufferForCleanup(Buffer buffer)
{
	BufferDesc *bufHdr;
	TimestampTz waitStart = 0;
	bool		waiting = false;
	bool		logged_recovery_conflict = false;

	Assert(BufferIsPinned(buffer));
	Assert(PinCountWaitBuf == NULL);

	CheckBufferIsPinnedOnce(buffer);

	/*
	 * 我们目前还无需担心正在进行中的 AIO 持有
	 * pin，因为到目前为止我们只支持通过 AIO 做
	 * 读取，而本函数只能在缓冲区有效（即没有
	 * 读取在途）时才能被调用。
	 */

	/* 没有其他人需要等待 */
	if (BufferIsLocal(buffer))
		return;

	bufHdr = GetBufferDescriptor(buffer - 1);

	for (;;)
	{
		uint32		buf_state;

		/* 尝试获取锁 */
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		buf_state = LockBufHdr(bufHdr);

		Assert(BUF_STATE_GET_REFCOUNT(buf_state) > 0);
		if (BUF_STATE_GET_REFCOUNT(buf_state) == 1)
		{
			/* 成功获取了 pincount 为 1 的排他锁 */
			UnlockBufHdr(bufHdr, buf_state);

			/*
			 * 如果缓冲区 pin 上的恢复冲突已经解决，
			 * 但启动进程等待它的时间超过了
			 * deadlock_timeout，则发出日志消息。
			 */
			if (logged_recovery_conflict)
				LogRecoveryConflict(PROCSIG_RECOVERY_CONFLICT_BUFFERPIN,
									waitStart, GetCurrentTimestamp(),
									NULL, false);

			if (waiting)
			{
				/* 重置 ps 显示，移除我们加上的后缀 */
				set_ps_display_remove_suffix();
				waiting = false;
			}
			return;
		}
		/* 失败，因此把我自己标记为正在等待 pincount 1 */
		if (buf_state & BM_PIN_COUNT_WAITER)
		{
			UnlockBufHdr(bufHdr, buf_state);
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			elog(ERROR, "multiple backends attempting to wait for pincount 1");
		}
		bufHdr->wait_backend_pgprocno = MyProcNumber;
		PinCountWaitBuf = bufHdr;
		buf_state |= BM_PIN_COUNT_WAITER;
		UnlockBufHdr(bufHdr, buf_state);
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

		/* 等待被 UnpinBuffer() 发信号唤醒 */
		if (InHotStandby)
		{
			if (!waiting)
			{
				/* 调整进程标题，表明它正在等待 */
				set_ps_display_suffix("waiting");
				waiting = true;
			}

			/*
			 * 如果启动进程因为缓冲区 pin 上的恢复冲突
			 * 等待的时间超过了 deadlock_timeout，则发出
			 * 日志消息。
			 *
			 * 如果是第一次循环则跳过，因为这种情况下
			 * 启动进程尚未开始等待。因此，等待开始的
			 * 时间戳是在这段逻辑之后才设置的。
			 */
			if (waitStart != 0 && !logged_recovery_conflict)
			{
				TimestampTz now = GetCurrentTimestamp();

				if (TimestampDifferenceExceeds(waitStart, now,
											   DeadlockTimeout))
				{
					LogRecoveryConflict(PROCSIG_RECOVERY_CONFLICT_BUFFERPIN,
										waitStart, now, NULL, true);
					logged_recovery_conflict = true;
				}
			}

			/*
			 * 如果启用了日志且是第一次循环，则设置
			 * 等待开始的时间戳。
			 */
			if (log_recovery_conflict_waits && waitStart == 0)
				waitStart = GetCurrentTimestamp();

			/* 发布启动进程所等待的 bufid */
			SetStartupBufferPinWaitBufId(buffer - 1);
			/* 设置闹钟，然后等待被 UnpinBuffer() 发信号唤醒 */
			ResolveRecoveryConflictWithBufferPin();
			/* 重置已发布的 bufid */
			SetStartupBufferPinWaitBufId(-1);
		}
		else
			ProcWaitForSignal(WAIT_EVENT_BUFFER_PIN);

		/*
		 * 移除把我们标记为等待者的标志。通常这已经不再
		 * 被设置，但 ProcWaitForSignal() 也可能因其他
		 * 信号而返回。我们只在自己是等待者时才
		 * 重置该标志，因为理论上另一个后端可能
		 * 已经开始了等待。以目前的使用方式这不可能
		 * 发生（因为有表级锁），但小心为上。
		 */
		buf_state = LockBufHdr(bufHdr);
		if ((buf_state & BM_PIN_COUNT_WAITER) != 0 &&
			bufHdr->wait_backend_pgprocno == MyProcNumber)
			buf_state &= ~BM_PIN_COUNT_WAITER;
		UnlockBufHdr(bufHdr, buf_state);

		PinCountWaitBuf = NULL;
		/* 回到循环开头再试一次 */
	}
}

/*
 * 当启动进程请求取消所有正阻塞它的 pin 持有者时，
 * 由 ProcessRecoveryConflictInterrupts() 调用本函数做检查。
 */
bool
HoldingBufferPinThatDelaysRecovery(void)
{
	int			bufid = GetStartupBufferPinWaitBufId();

	/*
	 * 如果我们被缓慢地唤醒，那么有可能启动进程
	 * 在我们到达这里之前就已经被其他后端唤醒了。
	 * 也有可能我们是因为多次中断或在不当时刻的
	 * 中断而到达这里的，因此要确保如果 bufid
	 * 没有被设置，我们就什么也不做。
	 */
	if (bufid < 0)
		return false;

	if (GetPrivateRefCount(bufid + 1) > 0)
		return true;

	return false;
}

/*
 * ConditionalLockBufferForCleanup —— 同上，但不等待以获取锁
 *
 * 我们不会循环，而只是检查一次，看 pin 计数
 * 是否 OK。如果不 OK，则在未持有任何锁的情况下
 * 返回 false。
 */
bool
ConditionalLockBufferForCleanup(Buffer buffer)
{
	BufferDesc *bufHdr;
	uint32		buf_state,
				refcount;

	Assert(BufferIsValid(buffer));

	/* 参见 LockBufferForCleanup() 中有关 AIO 的注释 */

	if (BufferIsLocal(buffer))
	{
		refcount = LocalRefCount[-buffer - 1];
		/* 应当恰好有一个 pin */
		Assert(refcount > 0);
		if (refcount != 1)
			return false;
		/* 没有其他人需要等待 */
		return true;
	}

	/* 应当恰好有一个本地 pin */
	refcount = GetPrivateRefCount(buffer);
	Assert(refcount);
	if (refcount != 1)
		return false;

	/* 尝试获取锁 */
	if (!ConditionalLockBuffer(buffer))
		return false;

	bufHdr = GetBufferDescriptor(buffer - 1);
	buf_state = LockBufHdr(bufHdr);
	refcount = BUF_STATE_GET_REFCOUNT(buf_state);

	Assert(refcount > 0);
	if (refcount == 1)
	{
		/* 成功获取了 pincount 为 1 的排他锁 */
		UnlockBufHdr(bufHdr, buf_state);
		return true;
	}

	/* 失败，因此释放锁 */
	UnlockBufHdr(bufHdr, buf_state);
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	return false;
}

/*
 * IsBufferCleanupOK —— 同上，但我们已经持有锁
 *
 * 检查对我们已锁住的缓冲区执行清理是否 OK。
 * 如果我们观察到 pin 计数为 1，那么我们的
 * 排他锁恰好就是清理锁，于是我们就可以进行
 * 任何原本在寻求清理锁时本可允许的操作。
 */
bool
IsBufferCleanupOK(Buffer buffer)
{
	BufferDesc *bufHdr;
	uint32		buf_state;

	Assert(BufferIsValid(buffer));

	/* 参见 LockBufferForCleanup() 中有关 AIO 的注释 */

	if (BufferIsLocal(buffer))
	{
		/* 应当恰好有一个 pin */
		if (LocalRefCount[-buffer - 1] != 1)
			return false;
		/* 没有其他人需要等待 */
		return true;
	}

	/* 应当恰好有一个本地 pin */
	if (GetPrivateRefCount(buffer) != 1)
		return false;

	bufHdr = GetBufferDescriptor(buffer - 1);

	/* 调用者必须持有缓冲区的排他锁 */
	Assert(LWLockHeldByMeInMode(BufferDescriptorGetContentLock(bufHdr),
								LW_EXCLUSIVE));

	buf_state = LockBufHdr(bufHdr);

	Assert(BUF_STATE_GET_REFCOUNT(buf_state) > 0);
	if (BUF_STATE_GET_REFCOUNT(buf_state) == 1)
	{
		/* pincount 没问题。 */
		UnlockBufHdr(bufHdr, buf_state);
		return true;
	}

	UnlockBufHdr(bufHdr, buf_state);
	return false;
}


/*
 *	缓冲区 I/O 处理相关的函数
 *
 *	还要注意的是，这些只用于共享缓冲区，
 *	而非本地缓冲区。
 */

/*
 * WaitIO —— 阻塞，直到 'buf' 上的 IO_IN_PROGRESS
 * 标志被清除。
 */
static void
WaitIO(BufferDesc *buf)
{
	ConditionVariable *cv = BufferDescriptorGetIOCV(buf);

	ConditionVariablePrepareToSleep(cv);
	for (;;)
	{
		uint32		buf_state;
		PgAioWaitRef iow;

		/*
		 * 此处未必需要获取自旋锁来检查这个标志，
		 * 但既然这个测试对正确性至关重要，我们
		 * 还是稳妥为上。
		 */
		buf_state = LockBufHdr(buf);

		/*
		 * 在持有自旋锁期间复制等待引用。这能防止
		 * 另一个后端中并发的 TerminateBufferIO()
		 * 在读它的同时清除 wref。
		 */
		iow = buf->io_wref;
		UnlockBufHdr(buf, buf_state);

		/* 没有正在进行的 IO，我们无需等待 */
		if (!(buf_state & BM_IO_IN_PROGRESS))
			break;

		/*
		 * 该缓冲区有正在进行的异步 IO，等待它完成。
		 */
		if (pgaio_wref_valid(&iow))
		{
			pgaio_wref_wait(&iow);

			/*
			 * AIO 子系统在内部使用条件变量，因此可能把
			 * 本后端从 BufferDesc 的 CV 中移除。虽然
			 * 这不会造成正确性问题（第一次 CV 睡眠
			 * 在未曾注册时只是立即返回），但既然
			 * 我们在函数开头就注意地避免，这里也值得
			 * 避免不必要的循环迭代。
			 */
			ConditionVariablePrepareToSleep(cv);
			continue;
		}

		/* 在 BufferDesc->cv 上等待，例如等待并发的同步 IO */
		ConditionVariableSleep(cv, WAIT_EVENT_BUFFER_IO);
	}
	ConditionVariableCancelSleep();
}

/*
 * StartBufferIO：在本缓冲区上开始 I/O
 *	（假设）
 *	我的进程没有在本缓冲区上执行任何 IO
 *	缓冲区已被 Pinned
 *
 * 在某些场景下，多个后端可能并发地尝试
 * 同一个 I/O 操作。如果已有别的后端在本缓冲区上
 * 开始了 I/O，我们将使用 WaitIO() 等待它完成。
 *
 * 输入操作只会对非 BM_VALID 的缓冲区尝试，
 * 而输出操作只会对 BM_VALID 且 BM_DIRTY 的
 * 缓冲区尝试，因此我们总是能判断工作是否
 * 已经完成。
 *
 * 如果我们成功地把缓冲区标记为 I/O 忙，则返回 true；
 * 如果别人已经完成了工作，则返回 false。
 *
 * 如果 nowait 为 true，那么我们不等待另一个后端
 * 的 I/O 完成。这种情况下，false 表示 I/O 要么
 * 已经完成，要么仍在进行中。这对于想知道自己能否
 * 把该 I/O 作为更大操作一部分来执行、而又不想
 * 等待答案或区分原因的调用者很有用。
 */
bool
StartBufferIO(BufferDesc *buf, bool forInput, bool nowait)
{
	uint32		buf_state;

	ResourceOwnerEnlarge(CurrentResourceOwner);

	for (;;)
	{
		buf_state = LockBufHdr(buf);

		if (!(buf_state & BM_IO_IN_PROGRESS))
			break;
		UnlockBufHdr(buf, buf_state);
		if (nowait)
			return false;
		WaitIO(buf);
	}

	/* 一旦到达这里，该缓冲区上肯定没有任何进行中的 I/O */

	/* 检查是否别人已经完成了 I/O */
	if (forInput ? (buf_state & BM_VALID) : !(buf_state & BM_DIRTY))
	{
		UnlockBufHdr(buf, buf_state);
		return false;
	}

	buf_state |= BM_IO_IN_PROGRESS;
	UnlockBufHdr(buf, buf_state);

	ResourceOwnerRememberBufferIO(CurrentResourceOwner,
								  BufferDescriptorGetBuffer(buf));

	return true;
}

/*
 * TerminateBufferIO：释放我们正在其上做 I/O 的缓冲区
 *	（假设）
 *	我的进程正在为该缓冲区执行 IO
 *	缓冲区的 BM_IO_IN_PROGRESS 位已设置
 *	缓冲区已被 Pinned
 *
 * 如果 clear_dirty 为 true 且 BM_JUST_DIRTIED 未设置，
 * 我们清除缓冲区的 BM_DIRTY 标志。在终止一次
 * 成功的写操作时这样做是合适的。对 BM_JUST_DIRTIED
 * 的检查是必要的，以避免在我们写期间缓冲区被
 * 重新弄脏时把它标记成干净。
 *
 * set_flag_bits 会被 OR 进缓冲区的标志中。在失败
 * 情况下它必须包含 BM_IO_ERROR。在成功完成时
 * 它可以是 0，或者如果是刚读入页面则为 BM_VALID。
 *
 * 如果 forget_owner 为 true，我们将该缓冲区的 I/O
 * 从当前资源拥有者处释放。（forget_owner=false
 * 用于资源拥有者自身正被释放时）
 */
void
TerminateBufferIO(BufferDesc *buf, bool clear_dirty, uint32 set_flag_bits,
				  bool forget_owner, bool release_aio)
{
	uint32		buf_state;

	buf_state = LockBufHdr(buf);

	Assert(buf_state & BM_IO_IN_PROGRESS);
	buf_state &= ~BM_IO_IN_PROGRESS;

	/* 清除先前的错误；如果这次 IO 失败，它会被再次标记 */
	buf_state &= ~BM_IO_ERROR;

	if (clear_dirty && !(buf_state & BM_JUST_DIRTIED))
		buf_state &= ~(BM_DIRTY | BM_CHECKPOINT_NEEDED);

	if (release_aio)
	{
		/* 由 AIO 子系统释放所有权 */
		Assert(BUF_STATE_GET_REFCOUNT(buf_state) > 0);
		buf_state -= BUF_REFCOUNT_ONE;
		pgaio_wref_clear(&buf->io_wref);
	}

	buf_state |= set_flag_bits;
	UnlockBufHdr(buf, buf_state);

	if (forget_owner)
		ResourceOwnerForgetBufferIO(CurrentResourceOwner,
									BufferDescriptorGetBuffer(buf));

	ConditionVariableBroadcast(BufferDescriptorGetIOCV(buf));

	/*
	 * 支持 LockBufferForCleanup()
	 *
	 * 我们可能刚刚释放了除等待者之外的最后一个
	 * pin。在大多数情况下，本后端还持有该缓冲区
	 * 的另一次 pin。但如果，例如，本后端正在
	 * 完成另一个后端发出的 IO，那么也许是时候
	 * 唤醒等待者了。
	 */
	if (release_aio && (buf_state & BM_PIN_COUNT_WAITER))
		WakePinCountWaiter(buf);
}

/*
 * AbortBufferIO：在错误之后清理活跃的缓冲区 I/O。
 *
 *	我们可能曾持有的所有 LWLocks 都已经被
 *	释放，但我们尚未释放缓冲区 pin，因此
 *	缓冲区仍是被 pin 的。
 *
 *	如果 I/O 曾在进行中，我们总是设置
 *	BM_IO_ERROR，即便该错误条件可能与 I/O 无关。
 *
 *  注意：这不会把缓冲区 I/O 从资源拥有者处
 *  移除。当我们正在释放整个资源拥有者时这
 *  是正确的，但在其他上下文中使用本函数时要当心。
 */
static void
AbortBufferIO(Buffer buffer)
{
	BufferDesc *buf_hdr = GetBufferDescriptor(buffer - 1);
	uint32		buf_state;

	buf_state = LockBufHdr(buf_hdr);
	Assert(buf_state & (BM_IO_IN_PROGRESS | BM_TAG_VALID));

	if (!(buf_state & BM_VALID))
	{
		Assert(!(buf_state & BM_DIRTY));
		UnlockBufHdr(buf_hdr, buf_state);
	}
	else
	{
		Assert(buf_state & BM_DIRTY);
		UnlockBufHdr(buf_hdr, buf_state);

		/* 如果这不是第一次失败，则发出通知…… */
		if (buf_state & BM_IO_ERROR)
		{
			/* 缓冲区已被 pin，因此我们可以不加自旋锁读取 tag */
			ereport(WARNING,
					(errcode(ERRCODE_IO_ERROR),
					 errmsg("could not write block %u of %s",
							buf_hdr->tag.blockNum,
							relpathperm(BufTagGetRelFileLocator(&buf_hdr->tag),
										BufTagGetForkNum(&buf_hdr->tag)).str),
					 errdetail("Multiple failures --- write error might be permanent.")));
		}
	}

	TerminateBufferIO(buf_hdr, false, BM_IO_ERROR, false, false);
}

/*
 * 在共享缓冲区写操作期间发生错误时使用的
 * 错误上下文回调。
 */
static void
shared_buffer_write_error_callback(void *arg)
{
	BufferDesc *bufHdr = (BufferDesc *) arg;

	/* 缓冲区已被 pin，因此我们可以不加自旋锁读取 tag */
	if (bufHdr != NULL)
		errcontext("writing block %u of relation \"%s\"",
				   bufHdr->tag.blockNum,
				   relpathperm(BufTagGetRelFileLocator(&bufHdr->tag),
							   BufTagGetForkNum(&bufHdr->tag)).str);
}

/*
 * 在本地缓冲区写操作期间发生错误时使用的
 * 错误上下文回调。
 */
static void
local_buffer_write_error_callback(void *arg)
{
	BufferDesc *bufHdr = (BufferDesc *) arg;

	if (bufHdr != NULL)
		errcontext("writing block %u of relation \"%s\"",
				   bufHdr->tag.blockNum,
				   relpathbackend(BufTagGetRelFileLocator(&bufHdr->tag),
								  MyProcNumber,
								  BufTagGetForkNum(&bufHdr->tag)).str);
}

/*
 * RelFileLocator 的 qsort/bsearch 比较器；
 * 参见 RelFileLocatorEquals。
 */
static int
rlocator_comparator(const void *p1, const void *p2)
{
	RelFileLocator n1 = *(const RelFileLocator *) p1;
	RelFileLocator n2 = *(const RelFileLocator *) p2;

	if (n1.relNumber < n2.relNumber)
		return -1;
	else if (n1.relNumber > n2.relNumber)
		return 1;

	if (n1.dbOid < n2.dbOid)
		return -1;
	else if (n1.dbOid > n2.dbOid)
		return 1;

	if (n1.spcOid < n2.spcOid)
		return -1;
	else if (n1.spcOid > n2.spcOid)
		return 1;
	else
		return 0;
}

/*
 * 锁住缓冲区头——在缓冲区状态中设置 BM_LOCKED。
 */
uint32
LockBufHdr(BufferDesc *desc)
{
	SpinDelayStatus delayStatus;
	uint32		old_buf_state;

	Assert(!BufferIsLocal(BufferDescriptorGetBuffer(desc)));

	init_local_spin_delay(&delayStatus);

	while (true)
	{
		/* 设置 BM_LOCKED 标志 */
		old_buf_state = pg_atomic_fetch_or_u32(&desc->state, BM_LOCKED);
		/* 如果之前没被设置，我们就 OK */
		if (!(old_buf_state & BM_LOCKED))
			break;
		perform_spin_delay(&delayStatus);
	}
	finish_spin_delay(&delayStatus);
	return old_buf_state | BM_LOCKED;
}

/*
 * 等待直到 BM_LOCKED 标志不再被设置，并返回
 * 那一刻缓冲区的状态。
 *
 * 显然，在返回值时缓冲区可能已经被锁住，
 * 因此这主要对 CAS 风格的循环有用。
 */
static uint32
WaitBufHdrUnlocked(BufferDesc *buf)
{
	SpinDelayStatus delayStatus;
	uint32		buf_state;

	init_local_spin_delay(&delayStatus);

	buf_state = pg_atomic_read_u32(&buf->state);

	while (buf_state & BM_LOCKED)
	{
		perform_spin_delay(&delayStatus);
		buf_state = pg_atomic_read_u32(&buf->state);
	}

	finish_spin_delay(&delayStatus);

	return buf_state;
}

/*
 * BufferTag 比较器。
 */
static inline int
buffertag_comparator(const BufferTag *ba, const BufferTag *bb)
{
	int			ret;
	RelFileLocator rlocatora;
	RelFileLocator rlocatorb;

	rlocatora = BufTagGetRelFileLocator(ba);
	rlocatorb = BufTagGetRelFileLocator(bb);

	ret = rlocator_comparator(&rlocatora, &rlocatorb);

	if (ret != 0)
		return ret;

	if (BufTagGetForkNum(ba) < BufTagGetForkNum(bb))
		return -1;
	if (BufTagGetForkNum(ba) > BufTagGetForkNum(bb))
		return 1;

	if (ba->blockNum < bb->blockNum)
		return -1;
	if (ba->blockNum > bb->blockNum)
		return 1;

	return 0;
}

/*
 * 决定检查点中写出顺序的比较器。
 *
 * 表空间必须最先比较，这很重要，
 * 各表空间之间写操作的均衡逻辑正依赖于此。
 */
static inline int
ckpt_buforder_comparator(const CkptSortItem *a, const CkptSortItem *b)
{
	/* 比较表空间 */
	if (a->tsId < b->tsId)
		return -1;
	else if (a->tsId > b->tsId)
		return 1;
	/* 比较关系 */
	if (a->relNumber < b->relNumber)
		return -1;
	else if (a->relNumber > b->relNumber)
		return 1;
	/* 比较 fork */
	else if (a->forkNum < b->forkNum)
		return -1;
	else if (a->forkNum > b->forkNum)
		return 1;
	/* 比较块号 */
	else if (a->blockNum < b->blockNum)
		return -1;
	else if (a->blockNum > b->blockNum)
		return 1;
	/* 相等的页面 ID 不太可能，但并非不可能 */
	return 0;
}

/*
 * 用于各表空间检查点完成进度的最小堆的
 * 比较器。
 */
static int
ts_ckpt_progress_comparator(Datum a, Datum b, void *arg)
{
	CkptTsStatus *sa = (CkptTsStatus *) a;
	CkptTsStatus *sb = (CkptTsStatus *) b;

	/* 我们想要一个最小堆，因此当 a < b 时返回 1 */
	if (sa->progress < sb->progress)
		return 1;
	else if (sa->progress == sb->progress)
		return 0;
	else
		return -1;
}

/*
 * 初始化一个写回（writeback）上下文，丢弃
 * 可能存在的先前状态。
 *
 * *max_pending 是一个指针而非直接的值，这样
 * 合并上限就能被 GUC 机制轻易地修改，
 * 调用代码也无需检查当前配置。值为 0
 * 意味着不会执行任何写回控制。
 */
void
WritebackContextInit(WritebackContext *context, int *max_pending)
{
	Assert(*max_pending <= WRITEBACK_MAX_PENDING_FLUSHES);

	context->max_pending = max_pending;
	context->nr_pending = 0;
}

/*
 * 把缓冲区加入待处理写回请求的列表。
 */
void
ScheduleBufferTagForWriteback(WritebackContext *wb_context, IOContext io_context,
							  BufferTag *tag)
{
	PendingWriteback *pending;

	/*
	 * As pg_flush_data() doesn't do anything with fsync disabled, there's no
	 * 这种情况下就没有跟踪的必要。
	 */
	if (io_direct_flags & IO_DIRECT_DATA ||
		!enableFsync)
		return;

	/*
	 * 把缓冲区加入待处理写回数组，除非写回控制
	 * 被禁用。
	 */
	if (*wb_context->max_pending > 0)
	{
		Assert(*wb_context->max_pending <= WRITEBACK_MAX_PENDING_FLUSHES);

		pending = &wb_context->pending_writebacks[wb_context->nr_pending++];

		pending->tag = *tag;
	}

	/*
	 * 如果超过了写回上限，就执行待处理的刷写。这
	 * 也包含了这样一种情况：之前已经加入
	 * 了一项，但现在控制已被禁用。
	 */
	if (wb_context->nr_pending >= *wb_context->max_pending)
		IssuePendingWritebacks(wb_context, io_context);
}

#define ST_SORT sort_pending_writebacks
#define ST_ELEMENT_TYPE PendingWriteback
#define ST_COMPARE(a, b) buffertag_comparator(&a->tag, &b->tag)
#define ST_SCOPE static
#define ST_DEFINE
#include "lib/sort_template.h"

/*
 * 把先前通过 ScheduleBufferTagForWriteback 登记的所有
 * 待处理写回请求，发往 OS。
 *
 * 因为本函数只用于改善 OS 的 IO 调度，
 * 我们尽量不报错——它只是一个提示。
 */
void
IssuePendingWritebacks(WritebackContext *wb_context, IOContext io_context)
{
	instr_time	io_start;
	int			i;

	if (wb_context->nr_pending == 0)
		return;

	/*
	 * 按顺序执行写操作能让它们快很多，并且可以把
	 * 针对连续块的写回请求合并为更大的写回。
	 */
	sort_pending_writebacks(wb_context->pending_writebacks,
							wb_context->nr_pending);

	io_start = pgstat_prepare_io_time(track_io_timing);

	/*
	 * 合并相邻的写，但不做其他事。为此我们遍历
	 * 现在已排序的待处理刷写数组，并向前查找
	 * 所有相邻的（或相同的）写。
	 */
	for (i = 0; i < wb_context->nr_pending; i++)
	{
		PendingWriteback *cur;
		PendingWriteback *next;
		SMgrRelation reln;
		int			ahead;
		BufferTag	tag;
		RelFileLocator currlocator;
		Size		nblocks = 1;

		cur = &wb_context->pending_writebacks[i];
		tag = cur->tag;
		currlocator = BufTagGetRelFileLocator(&tag);

		/*
		 * 向前查看后续的写回请求，看它们能否
		 * 与当前的这一个合并。
		 */
		for (ahead = 0; i + ahead + 1 < wb_context->nr_pending; ahead++)
		{

			next = &wb_context->pending_writebacks[i + ahead + 1];

			/* 不同的文件，停止 */
			if (!RelFileLocatorEquals(currlocator,
									 BufTagGetRelFileLocator(&next->tag)) ||
				BufTagGetForkNum(&cur->tag) != BufTagGetForkNum(&next->tag))
				break;

			/* 好，块被排队了两次，跳过 */
			if (cur->tag.blockNum == next->tag.blockNum)
				continue;

			/* 只合并连续的写 */
			if (cur->tag.blockNum + 1 != next->tag.blockNum)
				break;

			nblocks++;
			cur = next;
		}

		i += ahead;

	/* 最后告诉内核把数据写往存储 */
	reln = smgropen(currlocator, INVALID_PROC_NUMBER);
	smgrwriteback(reln, BufTagGetForkNum(&tag), tag.blockNum, nblocks);
}

/*
 * 假定写回请求只针对含有永久关系
 * 块的缓冲区发出。
 */
pgstat_count_io_op_time(IOOBJECT_RELATION, io_context,
						IOOP_WRITEBACK, io_start, wb_context->nr_pending, 0);

wb_context->nr_pending = 0;
}

/* 资源拥有者回调 */

static void
ResOwnerReleaseBufferIO(Datum res)
{
	Buffer		buffer = DatumGetInt32(res);

	AbortBufferIO(buffer);
}

static char *
ResOwnerPrintBufferIO(Datum res)
{
	Buffer		buffer = DatumGetInt32(res);

	return psprintf("lost track of buffer IO on buffer %d", buffer);
}

static void
ResOwnerReleaseBufferPin(Datum res)
{
	Buffer		buffer = DatumGetInt32(res);

	/* 与 ReleaseBuffer 类似，但不调用 ResourceOwnerForgetBuffer */
	if (!BufferIsValid(buffer))
		elog(ERROR, "bad buffer ID: %d", buffer);

	if (BufferIsLocal(buffer))
		UnpinLocalBufferNoOwner(buffer);
	else
		UnpinBufferNoOwner(GetBufferDescriptor(buffer - 1));
}

static char *
ResOwnerPrintBufferPin(Datum res)
{
	return DebugPrintBufferRefcount(DatumGetInt32(res));
}

/*
 * 辅助函数：驱逐一个已被获取缓冲区头锁的
 * 未 pin 缓冲区。
 */
static bool
EvictUnpinnedBufferInternal(BufferDesc *desc, bool *buffer_flushed)
{
	uint32		buf_state;
	bool		result;

	*buffer_flushed = false;

	buf_state = pg_atomic_read_u32(&(desc->state));
	Assert(buf_state & BM_LOCKED);

	if ((buf_state & BM_VALID) == 0)
	{
		UnlockBufHdr(desc, buf_state);
		return false;
	}

	/* 检查它尚未被 pin。 */
	if (BUF_STATE_GET_REFCOUNT(buf_state) > 0)
	{
		UnlockBufHdr(desc, buf_state);
		return false;
	}

	PinBuffer_Locked(desc);		/* 释放自旋锁 */

	/* 如果它脏了，尝试清理一次。 */
	if (buf_state & BM_DIRTY)
	{
		LWLockAcquire(BufferDescriptorGetContentLock(desc), LW_SHARED);
		FlushBuffer(desc, NULL, IOOBJECT_RELATION, IOCONTEXT_NORMAL);
		*buffer_flushed = true;
		LWLockRelease(BufferDescriptorGetContentLock(desc));
	}

	/* 如果它变脏或别人 pin 了它，这将返回 false。 */
	result = InvalidateVictimBuffer(desc);

	UnpinBuffer(desc);

	return result;
}

/*
 * 尝试驱逐一个共享缓冲区中的当前块。
 *
 * 本函数仅用于测试/开发目的！
 *
 * 要成功，缓冲区在入口处必须未被 pin，因此如果
 * 调用者心里想的是某个特定块，那么在本函数
 * 运行时它可能已经被子某个其他块替换掉了。
 * 它在返回时也被解除 pin，因此缓冲区可能在
 * 控制返回之前再次被占用，甚至可能是被
 * 同一个块占用。这种缺乏其他 interlocking
 * 的固有竞争性，使得本函数不适合非测试用途。
 *
 * 如果缓冲区是脏的且已经被刷出，则 *buffer_flushed
 * 被设为 true，否则为 false。不过，
 * *buffer_flushed=true 并不一定意味着是我们
 * 刷出了该缓冲区，它也可能是被别人刷出的。
 *
 * 如果缓冲区是有效的且现在已被置为无效，则返回 true。
 * 如果缓冲区不是有效的、因为 pin 无法驱逐、
 * 或缓冲区在我们尝试写出时再次变脏，则返回 false。
 */
bool
EvictUnpinnedBuffer(Buffer buf, bool *buffer_flushed)
{
	BufferDesc *desc;

	Assert(BufferIsValid(buf) && !BufferIsLocal(buf));

	/* 确保我们能够 pin 该缓冲区。 */
	ResourceOwnerEnlarge(CurrentResourceOwner);
	ReservePrivateRefCountEntry();

	desc = GetBufferDescriptor(buf - 1);
	LockBufHdr(desc);

	return EvictUnpinnedBufferInternal(desc, buffer_flushed);
}

/*
 * 尝试驱逐所有的共享缓冲区。
 *
 * 本函数仅用于测试/开发目的！参见
 * EvictUnpinnedBuffer()。
 *
 * buffers_* 参数是强制性的，表示以下各类
 * 缓冲区的计数：
 * - buffers_evicted - 被驱逐的
 * - buffers_flushed - 被刷出的
 * - buffers_skipped - 无法被驱逐的
 */
void
EvictAllUnpinnedBuffers(int32 *buffers_evicted, int32 *buffers_flushed,
						int32 *buffers_skipped)
{
	*buffers_evicted = 0;
	*buffers_skipped = 0;
	*buffers_flushed = 0;

	for (int buf = 1; buf <= NBuffers; buf++)
	{
		BufferDesc *desc = GetBufferDescriptor(buf - 1);
		uint32		buf_state;
		bool		buffer_flushed;

		CHECK_FOR_INTERRUPTS();

		buf_state = pg_atomic_read_u32(&desc->state);
		if (!(buf_state & BM_VALID))
			continue;

		ResourceOwnerEnlarge(CurrentResourceOwner);
		ReservePrivateRefCountEntry();

		LockBufHdr(desc);

		if (EvictUnpinnedBufferInternal(desc, &buffer_flushed))
			(*buffers_evicted)++;
		else
			(*buffers_skipped)++;

		if (buffer_flushed)
			(*buffers_flushed)++;
	}
}

/*
 * 尝试驱逐包含指定关系页面的所有共享缓冲区。
 *
 * 本函数仅用于测试/开发目的！参见
 * EvictUnpinnedBuffer()。
 *
 * 调用者必须至少持有关系上的 AccessShareLock 锁，
 * 以防止关系被删除。
 *
 * buffers_* 参数是强制性的，表示以下各类
 * 缓冲区的计数：
 * - buffers_evicted - 被驱逐的
 * - buffers_flushed - 被刷出的
 * - buffers_skipped - 无法被驱逐的
 */
void
EvictRelUnpinnedBuffers(Relation rel, int32 *buffers_evicted,
						int32 *buffers_flushed, int32 *buffers_skipped)
{
	Assert(!RelationUsesLocalBuffers(rel));

	*buffers_skipped = 0;
	*buffers_evicted = 0;
	*buffers_flushed = 0;

	for (int buf = 1; buf <= NBuffers; buf++)
	{
		BufferDesc *desc = GetBufferDescriptor(buf - 1);
		uint32		buf_state = pg_atomic_read_u32(&(desc->state));
		bool		buffer_flushed;

		CHECK_FOR_INTERRUPTS();

		/* 无锁预检应该是安全的，且能节省一些周期。 */
		if ((buf_state & BM_VALID) == 0 ||
			!BufTagMatchesRelFileLocator(&desc->tag, &rel->rd_locator))
			continue;

		/* 确保我们能够 pin 该缓冲区。 */
		ResourceOwnerEnlarge(CurrentResourceOwner);
		ReservePrivateRefCountEntry();

		buf_state = LockBufHdr(desc);

		/* 重新检查，没有锁的情况下可能已发生变化 */
		if ((buf_state & BM_VALID) == 0 ||
			!BufTagMatchesRelFileLocator(&desc->tag, &rel->rd_locator))
		{
			UnlockBufHdr(desc, buf_state);
			continue;
		}

		if (EvictUnpinnedBufferInternal(desc, &buffer_flushed))
			(*buffers_evicted)++;
		else
			(*buffers_skipped)++;

		if (buffer_flushed)
			(*buffers_flushed)++;
	}
}

/*
 * 本地/共享缓冲区 readv/writev 的 AIO handle staging 回调的通用实现。
 *
 * 每个 readv/writev 可以针对多个缓冲区。这些缓冲区已经
 * 通过 IO handle 完成注册。
 *
 * 为了使 IO 准备好执行（"staging"），我们需要确保目标缓冲区
 * 在 IO 进行期间处于适当的状态。为此，AIO 子系统需要
 * 拥有自己的缓冲区 pin，否则此后端中的错误可能导致该后端的
 * 缓冲区 pin 作为错误处理的一部分被释放，进而导致缓冲区
 * 在 IO 进行期间被替换。
 */
static pg_attribute_always_inline void
buffer_stage_common(PgAioHandle *ioh, bool is_write, bool is_temp)
{
	uint64	   *io_data;
	uint8		handle_data_len;
	PgAioWaitRef io_ref;
	BufferTag	first PG_USED_FOR_ASSERTS_ONLY = {0};

	io_data = pgaio_io_get_handle_data(ioh, &handle_data_len);

	pgaio_io_get_wref(ioh, &io_ref);

	/* 遍历向量化 readv/writev 所涉及的所有缓冲区 */
	for (int i = 0; i < handle_data_len; i++)
	{
		Buffer		buffer = (Buffer) io_data[i];
		BufferDesc *buf_hdr = is_temp ?
			GetLocalBufferDescriptor(-buffer - 1)
			: GetBufferDescriptor(buffer - 1);
		uint32		buf_state;

		/*
		 * 检查所有缓冲区确实是那些可以想像在一次 IO
		 * 中完成的，即它们是顺序的。这是 IO 实际执行前
		 * 最后一段有缓冲区感知能力的代码，而混淆哪些缓冲区
		 * 是 IO 的目标会很难调试，因此做额外的偏执检查
		 * 是值得的。
		 */
		if (i == 0)
			first = buf_hdr->tag;
		else
		{
			Assert(buf_hdr->tag.relNumber == first.relNumber);
			Assert(buf_hdr->tag.blockNum == first.blockNum + i);
		}

		if (is_temp)
			buf_state = pg_atomic_read_u32(&buf_hdr->state);
		else
			buf_state = LockBufHdr(buf_hdr);

		/* 验证缓冲区处于预期状态 */
		Assert(buf_state & BM_TAG_VALID);
		if (is_write)
		{
			Assert(buf_state & BM_VALID);
			Assert(buf_state & BM_DIRTY);
		}
		else
		{
			Assert(!(buf_state & BM_VALID));
			Assert(!(buf_state & BM_DIRTY));
		}

		/* 临时缓冲区不使用 BM_IO_IN_PROGRESS */
		if (!is_temp)
			Assert(buf_state & BM_IO_IN_PROGRESS);

		Assert(BUF_STATE_GET_REFCOUNT(buf_state) >= 1);

		/*
		 * 反应缓冲区现在由 AIO 子系统拥有。
		 *
		 * 对于本地缓冲区：这不能像人们最初想的那样
		 * 仅通过 LocalRefCount 来完成，因为此后端可能在 AIO
		 * 仍在进行中时出错，从而释放此后端自身持有的
		 * 所有 pin。
		 *
		 * 此 pin 在 TerminateBufferIO() 中再次释放。
		 */
		buf_state += BUF_REFCOUNT_ONE;
		buf_hdr->io_wref = io_ref;

		if (is_temp)
			pg_atomic_unlocked_write_u32(&buf_hdr->state, buf_state);
		else
			UnlockBufHdr(buf_hdr, buf_state);

		/*
		 * 确保在缓冲区被写出期间防止缓冲区修改的
		 * content lock 不会因错误而提前释放。
		 */
		if (is_write && !is_temp)
		{
			LWLock	   *content_lock;

			content_lock = BufferDescriptorGetContentLock(buf_hdr);

			Assert(LWLockHeldByMe(content_lock));

			/*
			 * 锁现在由 AIO 子系统拥有。
			 */
			LWLockDisown(content_lock);
		}

		/*
		 * 停止通过 resowner 跟踪此缓冲区 —— 现在由 AIO
		 * 系统来跟踪。
		 */
		if (!is_temp)
			ResourceOwnerForgetBufferIO(CurrentResourceOwner, buffer);
	}
}

/*
 * 解码由 buffer_readv_encode_error() 编码的 readv 错误。
 */
static inline void
buffer_readv_decode_error(PgAioResult result,
						  bool *zeroed_any,
						  bool *ignored_any,
						  uint8 *zeroed_or_error_count,
						  uint8 *checkfail_count,
						  uint8 *first_off)
{
	uint32		rem_error = result.error_data;

	/* 参见 buffer_readv_encode_error 中的 static assert */
#define READV_COUNT_BITS	7
#define READV_COUNT_MASK	((1 << READV_COUNT_BITS) - 1)

	*zeroed_any = rem_error & 1;
	rem_error >>= 1;

	*ignored_any = rem_error & 1;
	rem_error >>= 1;

	*zeroed_or_error_count = rem_error & READV_COUNT_MASK;
	rem_error >>= READV_COUNT_BITS;

	*checkfail_count = rem_error & READV_COUNT_MASK;
	rem_error >>= READV_COUNT_BITS;

	*first_off = rem_error & READV_COUNT_MASK;
	rem_error >>= READV_COUNT_BITS;
}

/*
 * 为 buffer_readv_complete() 编码错误的辅助函数。
 *
 * 错误按如下方式编码：
 * - bit 0 表示是否有页面被置零（1）或未置零（0）
 * - bit 1 表示是否有校验和失败被忽略（1）或未忽略（0）
 * - 接下来的 READV_COUNT_BITS 位表示出错或置零的页面数
 * - 接下来的 READV_COUNT_BITS 位表示校验和失败数
 * - 接下来的 READV_COUNT_BITS 位表示第一个出错或置零页面
 *   的偏移量，如果没有错误/置零，则记录第一个被忽略校验和
 *   的偏移量
 */
static inline void
buffer_readv_encode_error(PgAioResult *result,
						  bool is_temp,
						  bool zeroed_any,
						  bool ignored_any,
						  uint8 error_count,
						  uint8 zeroed_count,
						  uint8 checkfail_count,
						  uint8 first_error_off,
						  uint8 first_zeroed_off,
						  uint8 first_ignored_off)
{

	uint8		shift = 0;
	uint8		zeroed_or_error_count =
		error_count > 0 ? error_count : zeroed_count;
	uint8		first_off;

	StaticAssertStmt(PG_IOV_MAX <= 1 << READV_COUNT_BITS,
					 "PG_IOV_MAX is bigger than reserved space for error data");
	StaticAssertStmt((1 + 1 + 3 * READV_COUNT_BITS) <= PGAIO_RESULT_ERROR_BITS,
					 "PGAIO_RESULT_ERROR_BITS is insufficient for buffer_readv");

	/*
	 * 我们只有空间编码一个偏移量 —— 但幸运的是这已经足够了。
	 * 如果有错误，错误偏移量就是有意义的那个；置零缓冲区与
	 * 被忽略缓冲区的情况类似。
	 */
	if (error_count > 0)
		first_off = first_error_off;
	else if (zeroed_count > 0)
		first_off = first_zeroed_off;
	else
		first_off = first_ignored_off;

	Assert(!zeroed_any || error_count == 0);

	result->error_data = 0;

	result->error_data |= zeroed_any << shift;
	shift += 1;

	result->error_data |= ignored_any << shift;
	shift += 1;

	result->error_data |= ((uint32) zeroed_or_error_count) << shift;
	shift += READV_COUNT_BITS;

	result->error_data |= ((uint32) checkfail_count) << shift;
	shift += READV_COUNT_BITS;

	result->error_data |= ((uint32) first_off) << shift;
	shift += READV_COUNT_BITS;

	result->id = is_temp ? PGAIO_HCB_LOCAL_BUFFER_READV :
		PGAIO_HCB_SHARED_BUFFER_READV;

	if (error_count > 0)
		result->status = PGAIO_RS_ERROR;
	else
		result->status = PGAIO_RS_WARNING;

	/*
	 * 编码相当复杂，值得用解码函数
	 * 对其进行交叉校验。
	 */
#ifdef USE_ASSERT_CHECKING
	{
		bool		zeroed_any_2,
					ignored_any_2;
		uint8		zeroed_or_error_count_2,
					checkfail_count_2,
					first_off_2;

		buffer_readv_decode_error(*result,
								  &zeroed_any_2, &ignored_any_2,
								  &zeroed_or_error_count_2,
								  &checkfail_count_2,
								  &first_off_2);
		Assert(zeroed_any == zeroed_any_2);
		Assert(ignored_any == ignored_any_2);
		Assert(zeroed_or_error_count == zeroed_or_error_count_2);
		Assert(checkfail_count == checkfail_count_2);
		Assert(first_off == first_off_2);
	}
#endif

#undef READV_COUNT_BITS
#undef READV_COUNT_MASK
}

/*
 * AIO readv 完成回调的辅助函数，同时支持共享缓冲区和
 * 临时缓冲区。多页读取中的每个缓冲区都会被调用一次。
 */
static pg_attribute_always_inline void
buffer_readv_complete_one(PgAioTargetData *td, uint8 buf_off, Buffer buffer,
						  uint8 flags, bool failed, bool is_temp,
						  bool *buffer_invalid,
						  bool *failed_checksum,
						  bool *ignored_checksum,
						  bool *zeroed_buffer)
{
	BufferDesc *buf_hdr = is_temp ?
		GetLocalBufferDescriptor(-buffer - 1)
		: GetBufferDescriptor(buffer - 1);
	BufferTag	tag = buf_hdr->tag;
	char	   *bufdata = BufferGetBlock(buffer);
	uint32		set_flag_bits;
	int			piv_flags;

	/* 检查缓冲区是否处于读操作的预期状态 */
#ifdef USE_ASSERT_CHECKING
	{
		uint32		buf_state = pg_atomic_read_u32(&buf_hdr->state);

		Assert(buf_state & BM_TAG_VALID);
		Assert(!(buf_state & BM_VALID));
		/* 临时缓冲区不使用 BM_IO_IN_PROGRESS */
		if (!is_temp)
			Assert(buf_state & BM_IO_IN_PROGRESS);
		Assert(!(buf_state & BM_DIRTY));
	}
#endif

	*buffer_invalid = false;
	*failed_checksum = false;
	*ignored_checksum = false;
	*zeroed_buffer = false;

	/*
	 * 我们让 PageIsVerified() 仅记录校验和错误的消息，
	 * 因为完成回调可能在任意后端（或 IO worker）中运行。
	 * 我们将在 buffer_readv_report() 中报告校验和错误。
	 */
	piv_flags = PIV_LOG_LOG;

	/* 本地的 zero_damaged_pages 可能与定义者的不同 */
	if (flags & READ_BUFFERS_IGNORE_CHECKSUM_FAILURES)
		piv_flags |= PIV_IGNORE_CHECKSUM_FAILURE;

	/* 检查垃圾数据。 */
	if (!failed)
	{
		/*
		 * 如果缓冲区当前未被此后端 pin（例如因为我们是在
		 * 出错后完成此 IO），则缓冲区数据在缓冲区被 unpin
		 * 时已被标记为不可访问。AIO 子系统持有一个 pin，
		 * 但这并不阻止缓冲区被标记为不可访问。完成回调
		 * 也可能在另一个进程中执行。
		 */
#ifdef USE_VALGRIND
		if (!BufferIsPinned(buffer))
			VALGRIND_MAKE_MEM_DEFINED(bufdata, BLCKSZ);
#endif

		if (!PageIsVerified((Page) bufdata, tag.blockNum, piv_flags,
							failed_checksum))
		{
			if (flags & READ_BUFFERS_ZERO_ON_ERROR)
			{
				memset(bufdata, 0, BLCKSZ);
				*zeroed_buffer = true;
			}
			else
			{
				*buffer_invalid = true;
				/* 标记缓冲区为失败 */
				failed = true;
			}
		}
		else if (*failed_checksum)
			*ignored_checksum = true;

		/* 撤销上面所做的操作 */
#ifdef USE_VALGRIND
		if (!BufferIsPinned(buffer))
			VALGRIND_MAKE_MEM_NOACCESS(bufdata, BLCKSZ);
#endif

		/*
		 * 立即记录关于无效页面的消息，但仅记录到服务器日志。
		 * 之所以要立即记录，是因为这段代码可能在不同于发起请求的
		 * 后端中执行。之所以要立即记录，是因为发起者可能不会立即
		 * 处理查询结果（因为它正忙于执行查询处理的另一部分），
		 * 或者根本不会处理（例如如果它被取消或因另一个 IO 也失败
		 * 而出错）。IO 的定义者在处理 IO 结果时将发出 ERROR
		 * 或 WARNING。
		 *
		 * 为避免重复生成这些日志消息的代码，我们复用
		 * buffer_readv_report()。
		 */
		if (*buffer_invalid || *failed_checksum || *zeroed_buffer)
		{
			PgAioResult result_one = {0};

			buffer_readv_encode_error(&result_one, is_temp,
									  *zeroed_buffer,
									  *ignored_checksum,
									  *buffer_invalid,
									  *zeroed_buffer ? 1 : 0,
									  *failed_checksum ? 1 : 0,
									  buf_off, buf_off, buf_off);
			pgaio_result_report(result_one, td, LOG_SERVER_ONLY);
		}
	}

	/* 终止 I/O 并设置 BM_VALID。 */
	set_flag_bits = failed ? BM_IO_ERROR : BM_VALID;
	if (is_temp)
		TerminateLocalBufferIO(buf_hdr, false, set_flag_bits, true);
	else
		TerminateBufferIO(buf_hdr, false, set_flag_bits, false, true);

	/*
	 * 在回调中调用 BUFFER_READ_DONE tracepoint，即使回调可能
	 * 不在调用 BUFFER_READ_START 的同一个后端中执行。
	 * 另一种方案是将 tracepoint 调用推迟到更晚的阶段
	 * （例如共享缓冲区读的本地完成回调），但这似乎
	 * 更没有帮助。
	 */
	TRACE_POSTGRESQL_BUFFER_READ_DONE(tag.forkNum,
									  tag.blockNum,
									  tag.spcOid,
									  tag.dbOid,
									  tag.relNumber,
									  is_temp ? MyProcNumber : INVALID_PROC_NUMBER,
									  false);
}

/*
 * 执行单个 AIO 读的完成处理。该读操作可能涵盖
 * 多个块 / 缓冲区。
 *
 * 在共享缓冲区和本地缓冲区之间共享，以减少
 * 代码重复。
 */
static pg_attribute_always_inline PgAioResult
buffer_readv_complete(PgAioHandle *ioh, PgAioResult prior_result,
					  uint8 cb_data, bool is_temp)
{
	PgAioResult result = prior_result;
	PgAioTargetData *td = pgaio_io_get_target_data(ioh);
	uint8		first_error_off = 0;
	uint8		first_zeroed_off = 0;
	uint8		first_ignored_off = 0;
	uint8		error_count = 0;
	uint8		zeroed_count = 0;
	uint8		ignored_count = 0;
	uint8		checkfail_count = 0;
	uint64	   *io_data;
	uint8		handle_data_len;

	if (is_temp)
	{
		Assert(td->smgr.is_temp);
		Assert(pgaio_io_get_owner(ioh) == MyProcNumber);
	}
	else
		Assert(!td->smgr.is_temp);

	/*
	 * 遍历此 IO 所涉及的所有缓冲区，并对每个缓冲区
	 * 调用逐缓冲区完成函数。
	 */
	io_data = pgaio_io_get_handle_data(ioh, &handle_data_len);
	for (uint8 buf_off = 0; buf_off < handle_data_len; buf_off++)
	{
		Buffer		buf = io_data[buf_off];
		bool		failed;
		bool		failed_verification = false;
		bool		failed_checksum = false;
		bool		zeroed_buffer = false;
		bool		ignored_checksum = false;

		Assert(BufferIsValid(buf));

		/*
		 * 如果整个 I/O 在底层失败，则每个缓冲区都需要
		 * 标记为失败。在部分读取的情况下，前面的
		 * 几个缓冲区可能是正常的。
		 */
		failed =
			prior_result.status == PGAIO_RS_ERROR
			|| prior_result.result <= buf_off;

		buffer_readv_complete_one(td, buf_off, buf, cb_data, failed, is_temp,
								  &failed_verification,
								  &failed_checksum,
								  &ignored_checksum,
								  &zeroed_buffer);

		/*
		 * 跨所有页面追踪各类错误条件的数量，
		 * 因为一次 IO 中可能有多个页面
		 * 验证失败。
		 */
		if (failed_verification && !zeroed_buffer && error_count++ == 0)
			first_error_off = buf_off;
		if (zeroed_buffer && zeroed_count++ == 0)
			first_zeroed_off = buf_off;
		if (ignored_checksum && ignored_count++ == 0)
			first_ignored_off = buf_off;
		if (failed_checksum)
			checkfail_count++;
	}

	/*
	 * 如果 smgr 读取[部分]成功且页面验证对某些页面失败，
	 * 则适当调整 IO 的结果状态。
	 */
	if (prior_result.status != PGAIO_RS_ERROR &&
		(error_count > 0 || ignored_count > 0 || zeroed_count > 0))
	{
		buffer_readv_encode_error(&result, is_temp,
								  zeroed_count > 0, ignored_count > 0,
								  error_count, zeroed_count, checkfail_count,
								  first_error_off, first_zeroed_off,
								  first_ignored_off);
		pgaio_result_report(result, td, DEBUG1);
	}

	/*
	 * 对于共享关系，此报告在
	 * shared_buffer_readv_complete_local() 中完成。
	 */
	if (is_temp && checkfail_count > 0)
		pgstat_report_checksum_failures_in_db(td->smgr.rlocator.dbOid,
											  checkfail_count);

	return result;
}

/*
 * aio_shared_buffer_readv_cb 和 aio_local_buffer_readv_cb 的
 * AIO 错误报告回调。
 *
 * 错误在 buffer_readv_encode_error() /
 * buffer_readv_decode_error() 中编码/解码。
 */
static void
buffer_readv_report(PgAioResult result, const PgAioTargetData *td,
					int elevel)
{
	int			nblocks = td->smgr.nblocks;
	BlockNumber first = td->smgr.blockNum;
	BlockNumber last = first + nblocks - 1;
	ProcNumber	errProc =
		td->smgr.is_temp ? MyProcNumber : INVALID_PROC_NUMBER;
	RelPathStr	rpath =
		relpathbackend(td->smgr.rlocator, errProc, td->smgr.forkNum);
	bool		zeroed_any,
				ignored_any;
	uint8		zeroed_or_error_count,
				checkfail_count,
				first_off;
	uint8		affected_count;
	const char *msg_one,
			   *msg_mult,
			   *det_mult,
			   *hint_mult;

	buffer_readv_decode_error(result, &zeroed_any, &ignored_any,
							  &zeroed_or_error_count,
							  &checkfail_count,
							  &first_off);

	/*
	 * 将同时包含置零缓冲区和被忽略校验和的读取作为
	 * 特殊情况处理，因为它太不规则，不能以与其他情况
	 * 相同的方式发出。
	 */
	if (zeroed_any && ignored_any)
	{
		Assert(zeroed_any && ignored_any);
		Assert(nblocks > 1);	/* 同一个块不可能同时被置零和忽略 */
		Assert(result.status != PGAIO_RS_ERROR);
		affected_count = zeroed_or_error_count;

		ereport(elevel,
				errcode(ERRCODE_DATA_CORRUPTED),
				errmsg("zeroing %u page(s) and ignoring %u checksum failure(s) among blocks %u..%u of relation \"%s\"",
					   affected_count, checkfail_count, first, last, rpath.str),
				affected_count > 1 ?
				errdetail("Block %u held the first zeroed page.",
						  first + first_off) : 0,
				errhint_plural("See server log for details about the other %d invalid block.",
							   "See server log for details about the other %d invalid blocks.",
							   affected_count + checkfail_count - 1,
							   affected_count + checkfail_count - 1));
		return;
	}

	/*
	 * 其他消息高度重复。为避免重复冗长复杂的
	 * ereport()，单独收集翻译后的格式字符串，
	 * 然后执行一个通用的 ereport。
	 */
	if (result.status == PGAIO_RS_ERROR)
	{
		Assert(!zeroed_any);	/* 零化页面时不可能有无效页面 */
		affected_count = zeroed_or_error_count;
		msg_one = _("invalid page in block %u of relation \"%s\"");
		msg_mult = _("%u invalid pages among blocks %u..%u of relation \"%s\"");
		det_mult = _("Block %u held the first invalid page.");
		hint_mult = _("See server log for the other %u invalid block(s).");
	}
	else if (zeroed_any && !ignored_any)
	{
		affected_count = zeroed_or_error_count;
		msg_one = _("invalid page in block %u of relation \"%s\"; zeroing out page");
		msg_mult = _("zeroing out %u invalid pages among blocks %u..%u of relation \"%s\"");
		det_mult = _("Block %u held the first zeroed page.");
		hint_mult = _("See server log for the other %u zeroed block(s).");
	}
	else if (!zeroed_any && ignored_any)
	{
		affected_count = checkfail_count;
		msg_one = _("ignoring checksum failure in block %u of relation \"%s\"");
		msg_mult = _("ignoring %u checksum failures among blocks %u..%u of relation \"%s\"");
		det_mult = _("Block %u held the first ignored page.");
		hint_mult = _("See server log for the other %u ignored block(s).");
	}
	else
		pg_unreachable();

	ereport(elevel,
			errcode(ERRCODE_DATA_CORRUPTED),
			affected_count == 1 ?
			errmsg_internal(msg_one, first + first_off, rpath.str) :
			errmsg_internal(msg_mult, affected_count, first, last, rpath.str),
			affected_count > 1 ? errdetail_internal(det_mult, first + first_off) : 0,
			affected_count > 1 ? errhint_internal(hint_mult, affected_count - 1) : 0);
}

static void
shared_buffer_readv_stage(PgAioHandle *ioh, uint8 cb_data)
{
	buffer_stage_common(ioh, false, false);
}

static PgAioResult
shared_buffer_readv_complete(PgAioHandle *ioh, PgAioResult prior_result,
							 uint8 cb_data)
{
	return buffer_readv_complete(ioh, prior_result, cb_data, false);
}

/*
 * 我们需要一个用于共享缓冲区的后端本地完成回调，
 * 以便能够正确报告校验和错误。然而不幸的是，
 * 只有报告后端之前调用了
 * pgstat_prepare_report_checksum_failure() 才能安全完成，
 * 而这只能在启动 IO 的后端中保证。因此需要此回调。
 */
static PgAioResult
shared_buffer_readv_complete_local(PgAioHandle *ioh, PgAioResult prior_result,
								   uint8 cb_data)
{
	bool		zeroed_any,
				ignored_any;
	uint8		zeroed_or_error_count,
				checkfail_count,
				first_off;

	if (prior_result.status == PGAIO_RS_OK)
		return prior_result;

	buffer_readv_decode_error(prior_result,
							  &zeroed_any,
							  &ignored_any,
							  &zeroed_or_error_count,
							  &checkfail_count,
							  &first_off);

	if (checkfail_count)
	{
		PgAioTargetData *td = pgaio_io_get_target_data(ioh);

		pgstat_report_checksum_failures_in_db(td->smgr.rlocator.dbOid,
											  checkfail_count);
	}

	return prior_result;
}

static void
local_buffer_readv_stage(PgAioHandle *ioh, uint8 cb_data)
{
	buffer_stage_common(ioh, false, true);
}

static PgAioResult
local_buffer_readv_complete(PgAioHandle *ioh, PgAioResult prior_result,
							uint8 cb_data)
{
	return buffer_readv_complete(ioh, prior_result, cb_data, true);
}

/* readv 回调通过回调数据接收 READ_BUFFERS_* 标志 */
const PgAioHandleCallbacks aio_shared_buffer_readv_cb = {
	.stage = shared_buffer_readv_stage,
	.complete_shared = shared_buffer_readv_complete,
	/* 需要本地回调来报告校验和失败 */
	.complete_local = shared_buffer_readv_complete_local,
	.report = buffer_readv_report,
};

/* readv 回调通过回调数据接收 READ_BUFFERS_* 标志 */
const PgAioHandleCallbacks aio_local_buffer_readv_cb = {
	.stage = local_buffer_readv_stage,

	/*
	 * 请注意，与 shared_buffers 的情况相反，这里使用
	 * complete_local，因为只有发起 IO 的后端才能访问所需的
	 * 数据结构。这在 IO 完成可能被另一个后端偶然
	 * 消费的情况下非常重要。
	 */
	.complete_local = local_buffer_readv_complete,
	.report = buffer_readv_report,
};
