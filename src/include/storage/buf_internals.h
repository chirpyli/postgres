/*-------------------------------------------------------------------------
 *
 * buf_internals.h
 *	  缓冲区管理器和缓冲区替换策略的内部定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/buf_internals.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUFMGR_INTERNALS_H
#define BUFMGR_INTERNALS_H

#include "pgstat.h"
#include "port/atomics.h"
#include "storage/aio_types.h"
#include "storage/buf.h"
#include "storage/bufmgr.h"
#include "storage/condition_variable.h"
#include "storage/lwlock.h"
#include "storage/procnumber.h"
#include "storage/shmem.h"
#include "storage/smgr.h"
#include "storage/spin.h"
#include "utils/relcache.h"
#include "utils/resowner.h"

/*
 * 缓冲区状态是一个 32 位变量，组合了以下数据。
 *
 * - 18 位 refcount
 * - 4 位 usage count
 * - 10 位标志位
 *
 * 将这些值组合在一起，可以在不锁定缓冲区头部的情况下，通过 CAS 循环
 * 一并修改它们来完成某些操作。
 *
 * 缓冲区状态各组成部分的定义如下。
 */
#define BUF_REFCOUNT_BITS 18
#define BUF_USAGECOUNT_BITS 4
#define BUF_FLAG_BITS 10

StaticAssertDecl(BUF_REFCOUNT_BITS + BUF_USAGECOUNT_BITS + BUF_FLAG_BITS == 32,
				 "缓冲区状态空间的各部分之和必须等于 32");

#define BUF_REFCOUNT_ONE 1
#define BUF_REFCOUNT_MASK ((1U << BUF_REFCOUNT_BITS) - 1)
#define BUF_USAGECOUNT_MASK (((1U << BUF_USAGECOUNT_BITS) - 1) << (BUF_REFCOUNT_BITS))
#define BUF_USAGECOUNT_ONE (1U << BUF_REFCOUNT_BITS)
#define BUF_USAGECOUNT_SHIFT BUF_REFCOUNT_BITS
#define BUF_FLAG_MASK (((1U << BUF_FLAG_BITS) - 1) << (BUF_REFCOUNT_BITS + BUF_USAGECOUNT_BITS))

/* 从缓冲区状态获取 refcount 和 usagecount */
#define BUF_STATE_GET_REFCOUNT(state) ((state) & BUF_REFCOUNT_MASK)
#define BUF_STATE_GET_USAGECOUNT(state) (((state) & BUF_USAGECOUNT_MASK) >> BUF_USAGECOUNT_SHIFT)

/*
 * 缓冲区描述符的标志位
 *
 * 注意：BM_TAG_VALID 本质上意味着缓冲区哈希表中存在与缓冲区 tag 关联的条目。
 */
#define BM_LOCKED				(1U << 22)	/* 缓冲区头部已锁定 */
#define BM_DIRTY				(1U << 23)	/* 数据需要写入 */
#define BM_VALID				(1U << 24)	/* 数据有效 */
#define BM_TAG_VALID			(1U << 25)	/* 已分配 tag */
#define BM_IO_IN_PROGRESS		(1U << 26)	/* 正在读取或写入 */
#define BM_IO_ERROR				(1U << 27)	/* 先前的 I/O 失败 */
#define BM_JUST_DIRTIED			(1U << 28)	/* 写操作开始后变脏 */
#define BM_PIN_COUNT_WAITER		(1U << 29)	/* 有独占 pin 的等待者 */
#define BM_CHECKPOINT_NEEDED	(1U << 30)	/* 必须为检查点写入 */
#define BM_PERMANENT			(1U << 31)	/* 永久缓冲区（非 unlogged 或 init fork） */
/*
 * usage_count 的最大允许值代表了时钟扫描（clock-sweep）缓冲区管理算法的
 * 准确性与速度之间的权衡。较大的值（接近 NBuffers）会近似 LRU 语义。
 * 但找到空闲缓冲区可能需要多达 BM_MAX_USAGE_COUNT+1 次完整的时钟扫描循环，
 * 因此实践中我们不希望该值过大。
 */
#define BM_MAX_USAGE_COUNT	5

StaticAssertDecl(BM_MAX_USAGE_COUNT < (1 << BUF_USAGECOUNT_BITS),
				 "BM_MAX_USAGE_COUNT 无法放入 BUF_USAGECOUNT_BITS 位");
StaticAssertDecl(MAX_BACKENDS_BITS <= BUF_REFCOUNT_BITS,
				 "MAX_BACKENDS_BITS 必须 <= BUF_REFCOUNT_BITS");

/*
 * Buffer tag 标识缓冲区中包含的是哪个磁盘块。
 *
 * 注意：BufferTag 数据必须足以确定块要写到哪里，而无需引用 pg_class
 * 或 pg_tablespace 条目。刷新缓冲区的后端甚至可能不认为该关系已经可见
 * （其事务可能早于创建该关系的事务启动）。无论如何，存储管理器必须
 * 能够处理这种情况。
 *
 * 注意：如果结构体中包含任何填充字节，则必须修正 InitBufferTag 将其清零，
 * 因为该结构体被用作哈希键。
 */
typedef struct buftag
{
	Oid			spcOid;			/* 表空间 oid */
	Oid			dbOid;			/* 数据库 oid */
	RelFileNumber relNumber;	/* 关系文件号 */
	ForkNumber	forkNum;		/* fork 号 */
	BlockNumber blockNum;		/* 相对于关系起始的块号 */
} BufferTag;

static inline RelFileNumber
BufTagGetRelNumber(const BufferTag *tag)
{
	return tag->relNumber;
}

static inline ForkNumber
BufTagGetForkNum(const BufferTag *tag)
{
	return tag->forkNum;
}

static inline void
BufTagSetRelForkDetails(BufferTag *tag, RelFileNumber relnumber,
						ForkNumber forknum)
{
	tag->relNumber = relnumber;
	tag->forkNum = forknum;
}

static inline RelFileLocator
BufTagGetRelFileLocator(const BufferTag *tag)
{
	RelFileLocator rlocator;

	rlocator.spcOid = tag->spcOid;
	rlocator.dbOid = tag->dbOid;
	rlocator.relNumber = BufTagGetRelNumber(tag);

	return rlocator;
}

static inline void
ClearBufferTag(BufferTag *tag)
{
	tag->spcOid = InvalidOid;
	tag->dbOid = InvalidOid;
	BufTagSetRelForkDetails(tag, InvalidRelFileNumber, InvalidForkNumber);
	tag->blockNum = InvalidBlockNumber;
}

static inline void
InitBufferTag(BufferTag *tag, const RelFileLocator *rlocator,
			  ForkNumber forkNum, BlockNumber blockNum)
{
	tag->spcOid = rlocator->spcOid;
	tag->dbOid = rlocator->dbOid;
	BufTagSetRelForkDetails(tag, rlocator->relNumber, forkNum);
	tag->blockNum = blockNum;
}

static inline bool
BufferTagsEqual(const BufferTag *tag1, const BufferTag *tag2)
{
	return (tag1->spcOid == tag2->spcOid) &&
		(tag1->dbOid == tag2->dbOid) &&
		(tag1->relNumber == tag2->relNumber) &&
		(tag1->blockNum == tag2->blockNum) &&
		(tag1->forkNum == tag2->forkNum);
}

static inline bool
BufTagMatchesRelFileLocator(const BufferTag *tag,
							const RelFileLocator *rlocator)
{
	return (tag->spcOid == rlocator->spcOid) &&
		(tag->dbOid == rlocator->dbOid) &&
		(BufTagGetRelNumber(tag) == rlocator->relNumber);
}


/*
 * 共享缓冲区映射表被分区以减少争用。
 * 要确定给定 tag 需要哪个分区锁，先用 BufTableHashCode() 计算 tag 的哈希码，
 * 再调用 BufMappingPartitionLock()。
 * 注意：NUM_BUFFER_PARTITIONS 必须是 2 的幂！
 */
static inline uint32
BufTableHashPartition(uint32 hashcode)
{
	return hashcode % NUM_BUFFER_PARTITIONS;
}

static inline LWLock *
BufMappingPartitionLock(uint32 hashcode)
{
	return &MainLWLockArray[BUFFER_MAPPING_LWLOCK_OFFSET +
							BufTableHashPartition(hashcode)].lock;
}

static inline LWLock *
BufMappingPartitionLockByIndex(uint32 index)
{
	return &MainLWLockArray[BUFFER_MAPPING_LWLOCK_OFFSET + index].lock;
}

/*
 *	BufferDesc —— 单个共享缓冲区的共享描述符/状态数据。
 *
 * 注意：检查或修改 tag、state 或 wait_backend_pgprocno 字段时，必须持有
 * 缓冲区头部锁（BM_LOCKED 标志）。一般来说，缓冲区头部锁是一个自旋锁，
 * 与标志位、refcount 和 usagecount 一起组合进单个原子变量中。这种布局使
 * 我们能在一次原子操作中完成某些操作，而无需真正获取和释放自旋锁；例如
 * 增加或减少 refcount。buf_id 字段在初始化后永不改变，因此不需要加锁。
 * freeNext 由 buffer_strategy_lock 而非缓冲区头部锁保护。LWLock 自身会
 * 处理好自己。缓冲区头部锁*不*用于控制对缓冲区中数据的访问！
 *
 * 假定在持有缓冲区头部锁期间不会有人更改 state 字段。因此缓冲区头部锁的持有者
 * 可以在一次写入中完成对 state 变量的复杂更新，同时释放锁（清除 BM_LOCKED 标志）。
 * 另一方面，在持有缓冲区头部锁的情况下更新 state 被限制为 CAS 操作，以确保
 * BM_LOCKED 标志未被设置。原子性的自增/自减、OR/AND 等位操作是不允许的。
 *
 * 一个例外是：如果缓冲区已被我们 pin 住，则其 tag 不可能在背后改变，因此
 * 我们可以不锁定缓冲区头部就检查 tag。此外，有些地方我们会对标志位做一次性的
 * 读取而不费心去锁定缓冲区头部；这通常出现在不期望被测标志位发生变化的情况下。
 *
 * 如果另一个后端 pin 住了缓冲区，我们就无法从磁盘页中物理移除其中的项。因此，
 * 一个后端可能需要等待所有其他 pin 消失。这是通过将自己的 pgprocno 存入
 * wait_backend_pgprocno 并设置 BM_PIN_COUNT_WAITER 标志位来通知的。目前，
 * 每个缓冲区只能有一个这样的等待者。
 *
 * 本地缓冲区头部也使用同一个结构体，但其中的锁不会被使用，也并非所有标志位
 * 都有意义。为了避免不必要的开销，对 state 字段的操作应当不使用真正的原子操作
 * （即仅使用 pg_atomic_read_u32() 和 pg_atomic_unlocked_write_u32()）。
 *
 * 在添加或重排成员时，小心不要增大该结构体的大小。将其控制在 64 字节（最常见的
 * CPU 缓存行大小）以下对性能相当重要。
 *
 * 每个缓冲区的 I/O 条件变量目前保存在该结构体之外的一个独立数组中。它们本可以
 * 移入此处，在常见系统上仍能容纳在该限制内，但目前尚未这样做。
 */
typedef struct BufferDesc
{
	BufferTag	tag;			/* 缓冲区中所包含页面的 ID */
	int			buf_id;			/* 缓冲区的索引号（从 0 开始） */

	/* tag 的状态，包含标志位、refcount 和 usagecount */
	pg_atomic_uint32 state;

	int			wait_backend_pgprocno;	/* pin 计数等待者所在的后端 */
	int			freeNext;		/* 空闲链表中的链接 */

	PgAioWaitRef io_wref;		/* 当 AIO 进行中时设置 */
	LWLock		content_lock;	/* 用于锁定对缓冲区内容的访问 */
} BufferDesc;

/*
 * 实践证明，如果缓冲区头部按缓存行对齐，对它们的并发访问会更高效。因此我们
 * 强制 BufferDescriptors 数组的起始位置位于缓存行边界，并强制每个元素大小为
 * 一个缓存行。
 *
 * XXX：由于这主要在高度并发的工作负载中有意义，而如今这类负载几乎都在 64 位
 * 系统上，且空间浪费在 32 位系统上会更明显，因此我们没有在那些系统上强制
 * 步长为缓存行大小。如果有人做了实际的性能测试，我们可以重新评估。
 *
 * 注意，本地缓冲区描述符不会被强制对齐——因为不存在对其的并发访问，对齐不太可能
 * 带来收益。
 *
 * 这里使用 64 字节的缓存行大小，因为这是最常见的尺寸。设得更大只会浪费内存。
 * 即使在具有 32 或 128 字节缓存行大小的平台上运行，对齐到边界并避免伪共享也是有益的。
 */
#define BUFFERDESC_PAD_TO_SIZE	(SIZEOF_VOID_P == 8 ? 64 : 1)

typedef union BufferDescPadded
{
	BufferDesc	bufferdesc;
	char		pad[BUFFERDESC_PAD_TO_SIZE];
} BufferDescPadded;

/*
 * PendingWriteback 和 WritebackContext 结构体用于保存待向操作系统发出的
 * 刷新请求相关信息。
 */
typedef struct PendingWriteback
{
	/* 此处可以存储不同类型的待处理刷新请求 */
	BufferTag	tag;
} PendingWriteback;

/* 结构体在 bufmgr.h 中前向声明 */
typedef struct WritebackContext
{
	/* 指向可合并的最大写回请求数的指针 */
	int		   *max_pending;

	/* 当前待处理的写回请求数 */
	int			nr_pending;

	/* 待处理的请求 */
	PendingWriteback pending_writebacks[WRITEBACK_MAX_PENDING_FLUSHES];
} WritebackContext;

/* 在 buf_init.c 中 */
extern PGDLLIMPORT BufferDescPadded *BufferDescriptors;
extern PGDLLIMPORT ConditionVariableMinimallyPadded *BufferIOCVArray;
extern PGDLLIMPORT WritebackContext BackendWritebackContext;

/* 在 localbuf.c 中 */
extern PGDLLIMPORT BufferDesc *LocalBufferDescriptors;


static inline BufferDesc *
GetBufferDescriptor(uint32 id)
{
	return &(BufferDescriptors[id]).bufferdesc;
}

static inline BufferDesc *
GetLocalBufferDescriptor(uint32 id)
{
	return &LocalBufferDescriptors[id];
}

static inline Buffer
BufferDescriptorGetBuffer(const BufferDesc *bdesc)
{
	return (Buffer) (bdesc->buf_id + 1);
}

static inline ConditionVariable *
BufferDescriptorGetIOCV(const BufferDesc *bdesc)
{
	return &(BufferIOCVArray[bdesc->buf_id]).cv;
}

static inline LWLock *
BufferDescriptorGetContentLock(const BufferDesc *bdesc)
{
	return (LWLock *) (&bdesc->content_lock);
}

/*
 * freeNext 字段要么是下一个空闲链表条目的索引，
 * 要么是以下特殊值之一：
 */
#define FREENEXT_END_OF_LIST	(-1)
#define FREENEXT_NOT_IN_LIST	(-2)

/*
 * 用于获取/释放共享缓冲区头部自旋锁的函数。
 * 切勿将其用于本地缓冲区！
 */
extern uint32 LockBufHdr(BufferDesc *desc);

static inline void
UnlockBufHdr(BufferDesc *desc, uint32 buf_state)
{
	pg_write_barrier();
	pg_atomic_write_u32(&desc->state, buf_state & (~BM_LOCKED));
}

/* 在 bufmgr.c 中 */

/*
 * 在检查点上按文件对缓冲区进行排序的结构体。
 *
 * 该结构体在共享内存中为每个缓冲区分配，因此应尽量保持最小。
 */
typedef struct CkptSortItem
{
	Oid			tsId;
	RelFileNumber relNumber;
	ForkNumber	forkNum;
	BlockNumber blockNum;
	int			buf_id;
} CkptSortItem;

extern PGDLLIMPORT CkptSortItem *CkptBufferIds;

/* 持有缓冲区 I/O 和 pin 的 ResourceOwner 回调 */
extern PGDLLIMPORT const ResourceOwnerDesc buffer_io_resowner_desc;
extern PGDLLIMPORT const ResourceOwnerDesc buffer_pin_resowner_desc;

/* 对 ResourceOwnerRemember/Forget 的便捷封装 */
static inline void
ResourceOwnerRememberBuffer(ResourceOwner owner, Buffer buffer)
{
	ResourceOwnerRemember(owner, Int32GetDatum(buffer), &buffer_pin_resowner_desc);
}
static inline void
ResourceOwnerForgetBuffer(ResourceOwner owner, Buffer buffer)
{
	ResourceOwnerForget(owner, Int32GetDatum(buffer), &buffer_pin_resowner_desc);
}
static inline void
ResourceOwnerRememberBufferIO(ResourceOwner owner, Buffer buffer)
{
	ResourceOwnerRemember(owner, Int32GetDatum(buffer), &buffer_io_resowner_desc);
}
static inline void
ResourceOwnerForgetBufferIO(ResourceOwner owner, Buffer buffer)
{
	ResourceOwnerForget(owner, Int32GetDatum(buffer), &buffer_io_resowner_desc);
}

/*
 * 内部缓冲区管理例程
 */
/* bufmgr.c */
extern void WritebackContextInit(WritebackContext *context, int *max_pending);
extern void IssuePendingWritebacks(WritebackContext *wb_context, IOContext io_context);
extern void ScheduleBufferTagForWriteback(WritebackContext *wb_context,
										  IOContext io_context, BufferTag *tag);

/* 仅为便于编写测试 */
extern bool StartBufferIO(BufferDesc *buf, bool forInput, bool nowait);
extern void TerminateBufferIO(BufferDesc *buf, bool clear_dirty, uint32 set_flag_bits,
							  bool forget_owner, bool release_aio);


/* freelist.c */
extern IOContext IOContextForStrategy(BufferAccessStrategy strategy);
extern BufferDesc *StrategyGetBuffer(BufferAccessStrategy strategy,
									 uint32 *buf_state, bool *from_ring);
extern void StrategyFreeBuffer(BufferDesc *buf);
extern bool StrategyRejectBuffer(BufferAccessStrategy strategy,
								 BufferDesc *buf, bool from_ring);

extern int	StrategySyncStart(uint32 *complete_passes, uint32 *num_buf_alloc);
extern void StrategyNotifyBgWriter(int bgwprocno);

extern Size StrategyShmemSize(void);
extern void StrategyInitialize(bool init);
extern bool have_free_buffer(void);

/* buf_table.c */
extern Size BufTableShmemSize(int size);
extern void InitBufTable(int size);
extern uint32 BufTableHashCode(BufferTag *tagPtr);
extern int	BufTableLookup(BufferTag *tagPtr, uint32 hashcode);
extern int	BufTableInsert(BufferTag *tagPtr, uint32 hashcode, int buf_id);
extern void BufTableDelete(BufferTag *tagPtr, uint32 hashcode);

/* localbuf.c */
extern bool PinLocalBuffer(BufferDesc *buf_hdr, bool adjust_usagecount);
extern void UnpinLocalBuffer(Buffer buffer);
extern void UnpinLocalBufferNoOwner(Buffer buffer);
extern PrefetchBufferResult PrefetchLocalBuffer(SMgrRelation smgr,
												ForkNumber forkNum,
												BlockNumber blockNum);
extern BufferDesc *LocalBufferAlloc(SMgrRelation smgr, ForkNumber forkNum,
									BlockNumber blockNum, bool *foundPtr);
extern BlockNumber ExtendBufferedRelLocal(BufferManagerRelation bmr,
										  ForkNumber fork,
										  uint32 flags,
										  uint32 extend_by,
										  BlockNumber extend_upto,
										  Buffer *buffers,
										  uint32 *extended_by);
extern void MarkLocalBufferDirty(Buffer buffer);
extern void TerminateLocalBufferIO(BufferDesc *bufHdr, bool clear_dirty,
								   uint32 set_flag_bits, bool release_aio);
extern bool StartLocalBufferIO(BufferDesc *bufHdr, bool forInput, bool nowait);
extern void FlushLocalBuffer(BufferDesc *bufHdr, SMgrRelation reln);
extern void InvalidateLocalBuffer(BufferDesc *bufHdr, bool check_unreferenced);
extern void DropRelationLocalBuffers(RelFileLocator rlocator,
									 ForkNumber forkNum,
									 BlockNumber firstDelBlock);
extern void DropRelationAllLocalBuffers(RelFileLocator rlocator);
extern void AtEOXact_LocalBuffers(bool isCommit);

#endif							/* BUFMGR_INTERNALS_H */
