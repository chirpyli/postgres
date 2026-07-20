/*-------------------------------------------------------------------------
 *
 * localbuf.c
 *	  本地缓冲区管理器。用于临时表的快速缓冲区管理器，
 *	  临时表从不需要 WAL 日志记录或检查点等。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/localbuf.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/parallel.h"
#include "executor/instrument.h"
#include "pgstat.h"
#include "storage/aio.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "utils/guc_hooks.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "utils/resowner.h"


/*#define LBDEBUG*/

/* 缓冲区查找哈希表的条目 */
typedef struct
{
	BufferTag	key;			/* 磁盘页面的 Tag */
	int			id;				/* 关联的本地缓冲区索引 */
} LocalBufferLookupEnt;

/* 注意：此宏仅适用于本地缓冲区，不适用于共享缓冲区！ */
#define LocalBufHdrGetBlock(bufHdr) \
	LocalBufferBlockPointers[-((bufHdr)->buf_id + 2)]

int			NLocBuffer = 0;		/* 在缓冲区初始化之前 */

BufferDesc *LocalBufferDescriptors = NULL;
Block	   *LocalBufferBlockPointers = NULL;
int32	   *LocalRefCount = NULL;

static int	nextFreeLocalBufId = 0;

static HTAB *LocalBufHash = NULL;

/* 至少被 pin 过一次的本地缓冲区数量 */
static int	NLocalPinnedBuffers = 0;


static void InitLocalBuffers(void);
static Block GetLocalBufferStorage(void);
static Buffer GetLocalVictimBuffer(void);


/*
 * PrefetchLocalBuffer -
 *	  发起对关系某个块的异步读取
 *
 * 为临时关系完成 PrefetchBuffer 的工作。
 * 如果未编译进预取功能，则什么也不做。
 */
PrefetchBufferResult
PrefetchLocalBuffer(SMgrRelation smgr, ForkNumber forkNum,
					BlockNumber blockNum)
{
	PrefetchBufferResult result = {InvalidBuffer, false};
	BufferTag	newTag;			/* 所请求块的标识 */
	LocalBufferLookupEnt *hresult;

	InitBufferTag(&newTag, &smgr->smgr_rlocator.locator, forkNum, blockNum);

	/* 如果本次会话首次请求，则初始化本地缓冲区 */
	if (LocalBufHash == NULL)
		InitLocalBuffers();

	/* 查看所需的缓冲区是否已存在 */
	hresult = (LocalBufferLookupEnt *)
		hash_search(LocalBufHash, &newTag, HASH_FIND, NULL);

	if (hresult)
	{
		/* 是的，所以无需操作 */
		result.recent_buffer = -hresult->id - 1;
	}
	else
	{
#ifdef USE_PREFETCH
		/* 不在缓冲区中，所以发起预取 */
		if ((io_direct_flags & IO_DIRECT_DATA) == 0 &&
			smgrprefetch(smgr, forkNum, blockNum, 1))
		{
			result.initiated_io = true;
		}
#endif							/* USE_PREFETCH */
	}

	return result;
}


/*
 * LocalBufferAlloc -
 *	  为给定关系的给定页面查找或创建一个本地缓冲区。
 *
 * 其 API 与 bufmgr.c 中的 BufferAlloc 类似，不同之处在于由于都是本地
 * 操作，我们无需进行任何加锁。我们只支持默认访问策略（因此 usage_count
 * 总是会被推进）。
 */
BufferDesc *
LocalBufferAlloc(SMgrRelation smgr, ForkNumber forkNum, BlockNumber blockNum,
				 bool *foundPtr)
{
	BufferTag	newTag;			/* 所请求块的标识 */
	LocalBufferLookupEnt *hresult;
	BufferDesc *bufHdr;
	Buffer		victim_buffer;
	int			bufid;
	bool		found;

	InitBufferTag(&newTag, &smgr->smgr_rlocator.locator, forkNum, blockNum);

	/* 如果本次会话首次请求，则初始化本地缓冲区 */
	if (LocalBufHash == NULL)
		InitLocalBuffers();

	ResourceOwnerEnlarge(CurrentResourceOwner);

	/* 查看所需的缓冲区是否已存在 */
	hresult = (LocalBufferLookupEnt *)
		hash_search(LocalBufHash, &newTag, HASH_FIND, NULL);

	if (hresult)
	{
		bufid = hresult->id;
		bufHdr = GetLocalBufferDescriptor(bufid);
		Assert(BufferTagsEqual(&bufHdr->tag, &newTag));

		*foundPtr = PinLocalBuffer(bufHdr, true);
	}
	else
	{
		uint32		buf_state;

		victim_buffer = GetLocalVictimBuffer();
		bufid = -victim_buffer - 1;
		bufHdr = GetLocalBufferDescriptor(bufid);

		hresult = (LocalBufferLookupEnt *)
			hash_search(LocalBufHash, &newTag, HASH_ENTER, &found);
		if (found)				/* 不应发生 */
			elog(ERROR, "local buffer hash table corrupted");
		hresult->id = bufid;

		/*
		 * 现在它完全属于我们了。
		 */
		bufHdr->tag = newTag;

		buf_state = pg_atomic_read_u32(&bufHdr->state);
		buf_state &= ~(BUF_FLAG_MASK | BUF_USAGECOUNT_MASK);
		buf_state |= BM_TAG_VALID | BUF_USAGECOUNT_ONE;
		pg_atomic_unlocked_write_u32(&bufHdr->state, buf_state);

		*foundPtr = false;
	}

	return bufHdr;
}

/*
 * 类似 FlushBuffer()，但用于本地缓冲区。
 */
void
FlushLocalBuffer(BufferDesc *bufHdr, SMgrRelation reln)
{
	instr_time	io_start;
	Page		localpage = (char *) LocalBufHdrGetBlock(bufHdr);

	Assert(LocalRefCount[-BufferDescriptorGetBuffer(bufHdr) - 1] > 0);

	/*
 * 尝试启动一次 I/O 操作。目前 StartLocalBufferIO 没有理由返回
 * false，因此若出现这种情况则报错。
	 */
	if (!StartLocalBufferIO(bufHdr, false, false))
		elog(ERROR, "failed to start write IO on local buffer");

	/* 查找缓冲区的 smgr 关系 */
	if (reln == NULL)
		reln = smgropen(BufTagGetRelFileLocator(&bufHdr->tag),
						MyProcNumber);

	PageSetChecksumInplace(localpage, bufHdr->tag.blockNum);

	io_start = pgstat_prepare_io_time(track_io_timing);

	/* 写入... */
	smgrwrite(reln,
			  BufTagGetForkNum(&bufHdr->tag),
			  bufHdr->tag.blockNum,
			  localpage,
			  false);

	/* 临时表的 I/O 不使用缓冲区访问策略 */
	pgstat_count_io_op_time(IOOBJECT_TEMP_RELATION, IOCONTEXT_NORMAL,
							IOOP_WRITE, io_start, 1, BLCKSZ);

	/* 标记为非脏 */
	TerminateLocalBufferIO(bufHdr, true, 0, false);

	pgBufferUsage.local_blks_written++;
}

static Buffer
GetLocalVictimBuffer(void)
{
	int			victim_bufid;
	int			trycounter;
	BufferDesc *bufHdr;

	ResourceOwnerEnlarge(CurrentResourceOwner);

	/*
	 * 需要获取一个新的缓冲区。我们使用时钟扫描算法（本质上
	 * 与 freelist.c 现在所做的相同……）
	 */
	trycounter = NLocBuffer;
	for (;;)
	{
		victim_bufid = nextFreeLocalBufId;

		if (++nextFreeLocalBufId >= NLocBuffer)
			nextFreeLocalBufId = 0;

		bufHdr = GetLocalBufferDescriptor(victim_bufid);

		if (LocalRefCount[victim_bufid] == 0)
		{
			uint32		buf_state = pg_atomic_read_u32(&bufHdr->state);

			if (BUF_STATE_GET_USAGECOUNT(buf_state) > 0)
			{
				buf_state -= BUF_USAGECOUNT_ONE;
				pg_atomic_unlocked_write_u32(&bufHdr->state, buf_state);
				trycounter = NLocBuffer;
			}
			else if (BUF_STATE_GET_REFCOUNT(buf_state) > 0)
			{
				/*
				 * 如果后端为这个缓冲区发起了 AIO 随后又出错退出，
				 * 就可能到达这里。
				 */
			}
			else
			{
				/* 找到一个可用的缓冲区 */
				PinLocalBuffer(bufHdr, false);
				break;
			}
		}
		else if (--trycounter == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
					 errmsg("no empty local buffer available")));
	}

	/*
	 * 惰性内存分配：在首次使用某个缓冲区时再分配空间。
	 */
	if (LocalBufHdrGetBlock(bufHdr) == NULL)
	{
		/* 设置供 BufferGetBlock() 宏使用的指针 */
		LocalBufHdrGetBlock(bufHdr) = GetLocalBufferStorage();
	}

	/*
	 * 这个缓冲区没有被引用，但它可能仍然是脏的。如果是这样，
	 * 在重用之前要先把它写出！
	 */
	if (pg_atomic_read_u32(&bufHdr->state) & BM_DIRTY)
		FlushLocalBuffer(bufHdr, NULL);

	/*
	 * 从哈希表中移除被淘汰的缓冲区并标记为无效。
	 */
	if (pg_atomic_read_u32(&bufHdr->state) & BM_TAG_VALID)
	{
		InvalidateLocalBuffer(bufHdr, false);

		pgstat_count_io_op(IOOBJECT_TEMP_RELATION, IOCONTEXT_NORMAL, IOOP_EVICT, 1, 0);
	}

	return BufferDescriptorGetBuffer(bufHdr);
}

/* 参见 GetPinLimit() */
uint32
GetLocalPinLimit(void)
{
	/* 每个后端都有自己独立的临时缓冲区，并且可以将它们全部 pin 住。 */
	return num_temp_buffers;
}

/* 参见 GetAdditionalPinLimit() */
uint32
GetAdditionalLocalPinLimit(void)
{
	Assert(NLocalPinnedBuffers <= num_temp_buffers);
	return num_temp_buffers - NLocalPinnedBuffers;
}

/* 参见 LimitAdditionalPins() */
void
LimitAdditionalLocalPins(uint32 *additional_pins)
{
	uint32		max_pins;

	if (*additional_pins <= 1)
		return;

	/*
 * 与 LimitAdditionalPins() 不同，其他后端在这里不起作用。我们最多
 * 总共允许 NLocBuffer 个 pin，但它可能尚未初始化，因此读取
 * num_temp_buffers。
	 */
	max_pins = (num_temp_buffers - NLocalPinnedBuffers);

	if (*additional_pins >= max_pins)
		*additional_pins = max_pins;
}

/*
 * ExtendBufferedRelBy() 和 ExtendBufferedRelTo() 针对临时缓冲区的实现。
 */
BlockNumber
ExtendBufferedRelLocal(BufferManagerRelation bmr,
					   ForkNumber fork,
					   uint32 flags,
					   uint32 extend_by,
					   BlockNumber extend_upto,
					   Buffer *buffers,
					   uint32 *extended_by)
{
	BlockNumber first_block;
	instr_time	io_start;

	/* 如果本次会话首次请求，则初始化本地缓冲区 */
	if (LocalBufHash == NULL)
		InitLocalBuffers();

	LimitAdditionalLocalPins(&extend_by);

	for (uint32 i = 0; i < extend_by; i++)
	{
		BufferDesc *buf_hdr;
		Block		buf_block;

		buffers[i] = GetLocalVictimBuffer();
		buf_hdr = GetLocalBufferDescriptor(-buffers[i] - 1);
		buf_block = LocalBufHdrGetBlock(buf_hdr);

		/* 新缓冲区被零填充 */
		MemSet(buf_block, 0, BLCKSZ);
	}

	first_block = smgrnblocks(bmr.smgr, fork);

	if (extend_upto != InvalidBlockNumber)
	{
		/*
 * 与共享关系不同，没有任何东西能并发地改变关系的大小。因此我们
 * 不会最终发现其实不需要做任何事。
		 */
		Assert(first_block <= extend_upto);

		Assert((uint64) first_block + extend_by <= extend_upto);
	}

	/* 如果关系已经达到可能的最大长度则失败 */
	if ((uint64) first_block + extend_by >= MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cannot extend relation %s beyond %u blocks",
						relpath(bmr.smgr->smgr_rlocator, fork).str,
						MaxBlockNumber)));

	for (uint32 i = 0; i < extend_by; i++)
	{
		int			victim_buf_id;
		BufferDesc *victim_buf_hdr;
		BufferTag	tag;
		LocalBufferLookupEnt *hresult;
		bool		found;

		victim_buf_id = -buffers[i] - 1;
		victim_buf_hdr = GetLocalBufferDescriptor(victim_buf_id);

		/* 以防下面需要 pin 一个已存在的缓冲区 */
		ResourceOwnerEnlarge(CurrentResourceOwner);

		InitBufferTag(&tag, &bmr.smgr->smgr_rlocator.locator, fork, first_block + i);

		hresult = (LocalBufferLookupEnt *)
			hash_search(LocalBufHash, &tag, HASH_ENTER, &found);
		if (found)
		{
			BufferDesc *existing_hdr;
			uint32		buf_state;

			UnpinLocalBuffer(BufferDescriptorGetBuffer(victim_buf_hdr));

			existing_hdr = GetLocalBufferDescriptor(hresult->id);
			PinLocalBuffer(existing_hdr, false);
			buffers[i] = BufferDescriptorGetBuffer(existing_hdr);

			/*
			 * 清除 BM_VALID 位，调用 StartLocalBufferIO() 然后继续。
			 */
			buf_state = pg_atomic_read_u32(&existing_hdr->state);
			Assert(buf_state & BM_TAG_VALID);
			Assert(!(buf_state & BM_DIRTY));
			buf_state &= ~BM_VALID;
			pg_atomic_unlocked_write_u32(&existing_hdr->state, buf_state);

			/* 本地缓冲区无需循环 */
			StartLocalBufferIO(existing_hdr, true, false);
		}
		else
		{
			uint32		buf_state = pg_atomic_read_u32(&victim_buf_hdr->state);

			Assert(!(buf_state & (BM_VALID | BM_TAG_VALID | BM_DIRTY | BM_JUST_DIRTIED)));

			victim_buf_hdr->tag = tag;

			buf_state |= BM_TAG_VALID | BUF_USAGECOUNT_ONE;

			pg_atomic_unlocked_write_u32(&victim_buf_hdr->state, buf_state);

			hresult->id = victim_buf_id;

			StartLocalBufferIO(victim_buf_hdr, true, false);
		}
	}

	io_start = pgstat_prepare_io_time(track_io_timing);

	/* 实际扩展关系 */
	smgrzeroextend(bmr.smgr, fork, first_block, extend_by, false);

	pgstat_count_io_op_time(IOOBJECT_TEMP_RELATION, IOCONTEXT_NORMAL, IOOP_EXTEND,
							io_start, 1, extend_by * BLCKSZ);

	for (uint32 i = 0; i < extend_by; i++)
	{
		Buffer		buf = buffers[i];
		BufferDesc *buf_hdr;
		uint32		buf_state;

		buf_hdr = GetLocalBufferDescriptor(-buf - 1);

		buf_state = pg_atomic_read_u32(&buf_hdr->state);
		buf_state |= BM_VALID;
		pg_atomic_unlocked_write_u32(&buf_hdr->state, buf_state);
	}

	*extended_by = extend_by;

	pgBufferUsage.local_blks_written += extend_by;

	return first_block;
}

/*
 * MarkLocalBufferDirty -
 *	  标记本地缓冲区为脏
 */
void
MarkLocalBufferDirty(Buffer buffer)
{
	int			bufid;
	BufferDesc *bufHdr;
	uint32		buf_state;

	Assert(BufferIsLocal(buffer));

#ifdef LBDEBUG
	fprintf(stderr, "LB DIRTY %d\n", buffer);
#endif

	bufid = -buffer - 1;

	Assert(LocalRefCount[bufid] > 0);

	bufHdr = GetLocalBufferDescriptor(bufid);

	buf_state = pg_atomic_read_u32(&bufHdr->state);

	if (!(buf_state & BM_DIRTY))
		pgBufferUsage.local_blks_dirtied++;

	buf_state |= BM_DIRTY;

	pg_atomic_unlocked_write_u32(&bufHdr->state, buf_state);
}

/*
 * 类似 StartBufferIO，但用于本地缓冲区
 */
bool
StartLocalBufferIO(BufferDesc *bufHdr, bool forInput, bool nowait)
{
	uint32		buf_state;

	/*
 * 使用 AIO 时，缓冲区可能已有正在进行的 I/O，例如对同一关系进行两次
 * 扫描时。此时要么等待另一个 I/O 完成，要么返回 false。
	 */
	if (pgaio_wref_valid(&bufHdr->io_wref))
	{
		PgAioWaitRef iow = bufHdr->io_wref;

		if (nowait)
			return false;

		pgaio_wref_wait(&iow);
	}

	/* 一旦到达这里，该缓冲区上肯定没有正在进行的 I/O */

	/* 检查是否其他人已经完成该 I/O */
	buf_state = pg_atomic_read_u32(&bufHdr->state);
	if (forInput ? (buf_state & BM_VALID) : !(buf_state & BM_DIRTY))
	{
		return false;
	}

	/* BM_IO_IN_PROGRESS 当前不用于本地缓冲区 */

	/* 本地缓冲区不使用 resowner 跟踪 I/O */

	return true;
}

/*
 * 类似 TerminateBufferIO，但用于本地缓冲区
 */
void
TerminateLocalBufferIO(BufferDesc *bufHdr, bool clear_dirty, uint32 set_flag_bits,
					   bool release_aio)
{
	/* 只需调整标志位 */
	uint32		buf_state = pg_atomic_read_u32(&bufHdr->state);

	/* BM_IO_IN_PROGRESS 当前不用于本地缓冲区 */

	/* 清除之前的错误，如果本次 I/O 失败，会被再次标记 */
	buf_state &= ~BM_IO_ERROR;

	if (clear_dirty)
		buf_state &= ~BM_DIRTY;

	if (release_aio)
	{
		/* 释放 I/O 子系统持有的 pin，另见 buffer_stage_common() */
		Assert(BUF_STATE_GET_REFCOUNT(buf_state) > 0);
		buf_state -= BUF_REFCOUNT_ONE;
		pgaio_wref_clear(&bufHdr->io_wref);
	}

	buf_state |= set_flag_bits;
	pg_atomic_unlocked_write_u32(&bufHdr->state, buf_state);

	/* 本地缓冲区不使用 resowner 跟踪 I/O */

	/* 本地缓冲区不使用 I/O 条件变量，因为没有其他进程能看到该缓冲区 */

	/* 本地缓冲区不使用 BM_PIN_COUNT_WAITER，因此无需唤醒 */
}

/*
 * InvalidateLocalBuffer -- 标记一个本地缓冲区为无效。
 *
 * 如果 check_unreferenced 为 true，当缓冲区仍被 pin 住时报错。
 * 当调用 InvalidateLocalBuffer() 是为了改变某个缓冲区的标识（而非仅仅丢弃该
 * 缓冲区）时，传入 false 是合适的。
 * 缓冲区。
 *
 * 另见 InvalidateBuffer()。
 */
void
InvalidateLocalBuffer(BufferDesc *bufHdr, bool check_unreferenced)
{
	Buffer		buffer = BufferDescriptorGetBuffer(bufHdr);
	int			bufid = -buffer - 1;
	uint32		buf_state;
	LocalBufferLookupEnt *hresult;

	/*
	 * 有可能我们在例如中止创建表的事务之前，已经在该缓冲区上启动了 I/O。
	 * 在移除 / 重用该缓冲区之前，我们需要等待该 I/O
	 * 完成。
	 */
	if (pgaio_wref_valid(&bufHdr->io_wref))
	{
		PgAioWaitRef iow = bufHdr->io_wref;

		pgaio_wref_wait(&iow);
		Assert(!pgaio_wref_valid(&bufHdr->io_wref));
	}

	buf_state = pg_atomic_read_u32(&bufHdr->state);

	/*
	 * 我们不仅要测试 LocalRefCount[bufid]，还要测试 BufferDesc 本身，
	 * 因为后者被用来表示 AIO 子系统持有的 pin。
	 * 如果在发起 AIO 之后查询出错退出，就可能出现这种情况。
	 */
	if (check_unreferenced &&
		(LocalRefCount[bufid] != 0 || BUF_STATE_GET_REFCOUNT(buf_state) != 0))
		elog(ERROR, "block %u of %s is still referenced (local %d)",
			 bufHdr->tag.blockNum,
			 relpathbackend(BufTagGetRelFileLocator(&bufHdr->tag),
							MyProcNumber,
							BufTagGetForkNum(&bufHdr->tag)).str,
			 LocalRefCount[bufid]);

	/* 从哈希表中移除条目 */
	hresult = (LocalBufferLookupEnt *)
		hash_search(LocalBufHash, &bufHdr->tag, HASH_REMOVE, NULL);
	if (!hresult)				/* 不应发生 */
		elog(ERROR, "local buffer hash table corrupted");
	/* 标记缓冲区无效 */
	ClearBufferTag(&bufHdr->tag);
	buf_state &= ~BUF_FLAG_MASK;
	buf_state &= ~BUF_USAGECOUNT_MASK;
	pg_atomic_unlocked_write_u32(&bufHdr->state, buf_state);
}

/*
 * DropRelationLocalBuffers
 *		此函数从缓冲区池中移除指定关系中所有块号 >= firstDelBlock
 *		的页面。
 *		（特别地，当 firstDelBlock = 0 时，移除所有页面。）
 *		脏页会直接被丢弃，而不会费心先将它们写出。
 *		因此，这是不可回滚
 *		的，所以务必极其谨慎地使用！
 *
 *		更多说明参见 bufmgr.c 中的 DropRelationBuffers。
 */
void
DropRelationLocalBuffers(RelFileLocator rlocator, ForkNumber forkNum,
						 BlockNumber firstDelBlock)
{
	int			i;

	for (i = 0; i < NLocBuffer; i++)
	{
		BufferDesc *bufHdr = GetLocalBufferDescriptor(i);
		uint32		buf_state;

		buf_state = pg_atomic_read_u32(&bufHdr->state);

		if ((buf_state & BM_TAG_VALID) &&
			BufTagMatchesRelFileLocator(&bufHdr->tag, &rlocator) &&
			BufTagGetForkNum(&bufHdr->tag) == forkNum &&
			bufHdr->tag.blockNum >= firstDelBlock)
		{
			InvalidateLocalBuffer(bufHdr, true);
		}
	}
}

/*
 * DropRelationAllLocalBuffers
 *		此函数从缓冲区池中移除指定关系所有分支（fork）的所有页面。
 *		（针对指定关系）。
 *
 *		更多说明参见 bufmgr.c 中的 DropRelationsAllBuffers。
 */
void
DropRelationAllLocalBuffers(RelFileLocator rlocator)
{
	int			i;

	for (i = 0; i < NLocBuffer; i++)
	{
		BufferDesc *bufHdr = GetLocalBufferDescriptor(i);
		uint32		buf_state;

		buf_state = pg_atomic_read_u32(&bufHdr->state);

		if ((buf_state & BM_TAG_VALID) &&
			BufTagMatchesRelFileLocator(&bufHdr->tag, &rlocator))
		{
			InvalidateLocalBuffer(bufHdr, true);
		}
	}
}

/*
 * InitLocalBuffers -
 *	  初始化本地缓冲区缓存。由于大多数查询（尤其是多用户的查询）不涉及
 *	  本地缓冲区，我们将缓冲区的实际内存分配推迟到需要时再进行；此处
 *	  仅创建缓冲区头。
 */
static void
InitLocalBuffers(void)
{
	int			nbufs = num_temp_buffers;
	HASHCTL		info;
	int			i;

	/*
	 * 并行工作进程无法访问临时表中的数据，因为它们看不到其领导者（leader）
	 * 的本地缓冲区。
	 * 这里是一个方便且低成本的兜底检查点。注意我们并不想
	 * 阻止并行工作进程访问临时表的目录元数据，因此在更高层级做检查是
	 * 不合适的。
	 */
	if (IsParallelWorker())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot access temporary tables during a parallel operation")));

	/* 分配并清零缓冲区头及辅助数组 */
	LocalBufferDescriptors = (BufferDesc *) calloc(nbufs, sizeof(BufferDesc));
	LocalBufferBlockPointers = (Block *) calloc(nbufs, sizeof(Block));
	LocalRefCount = (int32 *) calloc(nbufs, sizeof(int32));
	if (!LocalBufferDescriptors || !LocalBufferBlockPointers || !LocalRefCount)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	nextFreeLocalBufId = 0;

	/* 初始化需要以非零值开始的字段 */
	for (i = 0; i < nbufs; i++)
	{
		BufferDesc *buf = GetLocalBufferDescriptor(i);

		/*
		 * 用负数表示本地缓冲区。这有点棘手：共享缓冲区
		 * 从 0 开始，我们必须从 -2 开始。（注意 BufferDescriptorGetBuffer 例程会给
		 * buf_id 加 1，因此我们的第一个缓冲区 id
		 * 是 -1。）
		 */
		buf->buf_id = -i - 2;

		pgaio_wref_clear(&buf->io_wref);

		/*
		 * 故意不初始化缓冲区的原子变量（除了上面将底层内存清零之外）。
		 * 这样在没有原子操作支持平台上，
		 * 如果有人（重新）为本地缓冲区
		 * 引入了原子操作，我们就会得到错误提示。
		 */
	}

	/* 创建查找哈希表 */
	info.keysize = sizeof(BufferTag);
	info.entrysize = sizeof(LocalBufferLookupEnt);

	LocalBufHash = hash_create("Local Buffer Lookup Table",
							   nbufs,
							   &info,
							   HASH_ELEM | HASH_BLOBS);

	if (!LocalBufHash)
		elog(ERROR, "could not initialize local buffer hash table");

	/* 初始化完成，标记缓冲区已分配 */
	NLocBuffer = nbufs;
}

/*
 * XXX: 我们可以有一个稍微高效一点的 PinLocalBuffer() 版本，它不支持调整
 * usagecount——但到目前为止，似乎不值得为此费劲。
 *
 * 注意：在这之前必须先完成 ResourceOwnerEnlarge()。
 */
bool
PinLocalBuffer(BufferDesc *buf_hdr, bool adjust_usagecount)
{
	uint32		buf_state;
	Buffer		buffer = BufferDescriptorGetBuffer(buf_hdr);
	int			bufid = -buffer - 1;

	buf_state = pg_atomic_read_u32(&buf_hdr->state);

	if (LocalRefCount[bufid] == 0)
	{
		NLocalPinnedBuffers++;
		buf_state += BUF_REFCOUNT_ONE;
		if (adjust_usagecount &&
			BUF_STATE_GET_USAGECOUNT(buf_state) < BM_MAX_USAGE_COUNT)
		{
			buf_state += BUF_USAGECOUNT_ONE;
		}
		pg_atomic_unlocked_write_u32(&buf_hdr->state, buf_state);

		/*
		 * 参见 PinBuffer() 中的注释。
		 *
		 * 如果缓冲区尚未分配，它会在 GetLocalBufferStorage() 中被标记为
		 * 已定义。
		 */
		if (LocalBufHdrGetBlock(buf_hdr) != NULL)
			VALGRIND_MAKE_MEM_DEFINED(LocalBufHdrGetBlock(buf_hdr), BLCKSZ);
	}
	LocalRefCount[bufid]++;
	ResourceOwnerRememberBuffer(CurrentResourceOwner,
								BufferDescriptorGetBuffer(buf_hdr));

	return buf_state & BM_VALID;
}

void
UnpinLocalBuffer(Buffer buffer)
{
	UnpinLocalBufferNoOwner(buffer);
	ResourceOwnerForgetBuffer(CurrentResourceOwner, buffer);
}

void
UnpinLocalBufferNoOwner(Buffer buffer)
{
	int			buffid = -buffer - 1;

	Assert(BufferIsLocal(buffer));
	Assert(LocalRefCount[buffid] > 0);
	Assert(NLocalPinnedBuffers > 0);

	if (--LocalRefCount[buffid] == 0)
	{
		BufferDesc *buf_hdr = GetLocalBufferDescriptor(buffid);
		uint32		buf_state;

		NLocalPinnedBuffers--;

		buf_state = pg_atomic_read_u32(&buf_hdr->state);
		Assert(BUF_STATE_GET_REFCOUNT(buf_state) > 0);
		buf_state -= BUF_REFCOUNT_ONE;
		pg_atomic_unlocked_write_u32(&buf_hdr->state, buf_state);

		/* 参见 UnpinBufferNoOwner 中的注释 */
		VALGRIND_MAKE_MEM_NOACCESS(LocalBufHdrGetBlock(buf_hdr), BLCKSZ);
	}
}

/*
 * temp_buffers 的 GUC check_hook
 */
bool
check_temp_buffers(int *newval, void **extra, GucSource source)
{
	/*
	 * 一旦本地缓冲区已经初始化，再修改这个值就太晚了。
	 * 不过，如果这次只是一次测试调用，则允许修改。
	 */
	if (source != PGC_S_TEST && NLocBuffer && NLocBuffer != *newval)
	{
		GUC_check_errdetail("\"temp_buffers\" cannot be changed after any temporary tables have been accessed in the session.");
		return false;
	}
	return true;
}

/*
 * GetLocalBufferStorage - 为本地缓冲区分配内存
 *
 * 这个函数的思路是聚合我们对存储的请求，这样内存管理器就不会看到
 * 大量相对较小的请求。由于本地缓冲区一旦在特定进程中创建就永远不会归还，
 * 因此没有必要用单独管理的块来增加内存管理器的负担。
 */
static Block
GetLocalBufferStorage(void)
{
	static char *cur_block = NULL;
	static int	next_buf_in_block = 0;
	static int	num_bufs_in_block = 0;
	static int	total_bufs_allocated = 0;
	static MemoryContext LocalBufferContext = NULL;

	char	   *this_buf;

	Assert(total_bufs_allocated < NLocBuffer);

	if (next_buf_in_block >= num_bufs_in_block)
	{
		/* 需要向内存管理器发起新请求 */
		int			num_bufs;

		/*
		 * 我们将本地缓冲区分配在一个独立的上下文中，
		 * 这样在 MemoryContextStats 输出中就能方便地识别出它们占用的空间。
		 * 在首次使用时创建该上下文。
		 */
		if (LocalBufferContext == NULL)
			LocalBufferContext =
				AllocSetContextCreate(TopMemoryContext,
									  "LocalBufferContext",
									  ALLOCSET_DEFAULT_SIZES);

		/* 初始请求 16 个缓冲区，后续每次翻倍 */
		num_bufs = Max(num_bufs_in_block * 2, 16);
		/* 但不超过剩余所需的本地缓冲区数量 */
		num_bufs = Min(num_bufs, NLocBuffer - total_bufs_allocated);
		/* 同时不要超过 MaxAllocSize */
		num_bufs = Min(num_bufs, MaxAllocSize / BLCKSZ);

		/* 缓冲区应 I/O 对齐 */
		cur_block = (char *)
			TYPEALIGN(PG_IO_ALIGN_SIZE,
					  MemoryContextAlloc(LocalBufferContext,
										 num_bufs * BLCKSZ + PG_IO_ALIGN_SIZE));
		next_buf_in_block = 0;
		num_bufs_in_block = num_bufs;
	}

	/* 在当前内存块中分配下一个缓冲区 */
	this_buf = cur_block + next_buf_in_block * BLCKSZ;
	next_buf_in_block++;
	total_bufs_allocated++;

	/*
	 * 调用方的 PinLocalBuffer() 对于 Valgrind 更新来说执行得太早，
	 * 因此在这里完成。这个块实际上是未定义的，但我们希望与
	 * 不需要分配内存的常规情况保持一致。
	 * 这在 method_io_uring.c 填充块时特别需要，因为
	 * Valgrind 无法识别 io_uring 读取会导致未定义内存变为已定义。
	 */
	VALGRIND_MAKE_MEM_DEFINED(this_buf, BLCKSZ);

	return (Block) this_buf;
}

/*
 * CheckForLocalBufferLeaks - 确保此后端未持有任何本地缓冲区 pin
 *
 * 类似 CheckForBufferLeaks()，但用于本地缓冲区。
 */
static void
CheckForLocalBufferLeaks(void)
{
#ifdef USE_ASSERT_CHECKING
	if (LocalRefCount)
	{
		int			RefCountErrors = 0;
		int			i;

		for (i = 0; i < NLocBuffer; i++)
		{
			if (LocalRefCount[i] != 0)
			{
				Buffer		b = -i - 1;
				char	   *s;

				s = DebugPrintBufferRefcount(b);
				elog(WARNING, "local buffer refcount leak: %s", s);
				pfree(s);

				RefCountErrors++;
			}
		}
		Assert(RefCountErrors == 0);
	}
#endif
}

/*
 * AtEOXact_LocalBuffers - 事务结束时清理
 *
 * 类似 AtEOXact_Buffers，但用于本地缓冲区。
 */
void
AtEOXact_LocalBuffers(bool isCommit)
{
	CheckForLocalBufferLeaks();
}

/*
 * AtProcExit_LocalBuffers - 确保后端退出时已释放所有 pin。
 *
 * 类似 AtProcExit_Buffers，但用于本地缓冲区。
 */
void
AtProcExit_LocalBuffers(void)
{
	/*
	 * 我们不应该持有任何残留的 pin；如果持有且未启用断言，
	 * 稍后在尝试删除临时关系时会在 DropRelationBuffers 中失败。
	 */
	CheckForLocalBufferLeaks();
}
