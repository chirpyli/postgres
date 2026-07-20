/*-------------------------------------------------------------------------
 *
 * freelist.c
 *	  管理缓冲池替换策略的例程。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/freelist.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "pgstat.h"
#include "port/atomics.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/proc.h"

#define INT_ACCESS_ONCE(var)	((int)(*((volatile int *)&(var))))


/*
 * 共享的 freelist 控制信息。
 */
typedef struct
{
	/* 自旋锁：保护下面的值 */
	slock_t		buffer_strategy_lock;

	/*
	 * 时钟扫描指针：下一个要考虑夺取的缓冲区的索引。注意
	 * 这不是一个具体缓冲区 —— 我们只会增加该值。因此，
	 * 要得到一个实际缓冲区，需要对其取 NBuffers 的模。
	 */
	pg_atomic_uint32 nextVictimBuffer;

	int			firstFreeBuffer;	/* 未使用缓冲区链表的表头 */
	int			lastFreeBuffer; /* 未使用缓冲区链表的表尾 */

	/*
	 * 注意：当 firstFreeBuffer 为 -1（即链表为空）时，
	 * lastFreeBuffer 未定义。
	 */

	/*
	 * 统计信息。这些计数器应足够宽，以免在单个
	 * bgwriter 周期内溢出。
	 */
	uint32		completePasses; /* 时钟扫描完成的圈数 */
	pg_atomic_uint32 numBufferAllocs;	/* 自上次重置以来分配的缓冲区数 */

	/*
	 * 有活动时需通知的后台工作进程，若没有则为 -1。
	 * 参见 StrategyNotifyBgWriter。
	 */
	int			bgwprocno;
} BufferStrategyControl;

/* 指向共享状态的指针 */
static BufferStrategyControl *StrategyControl = NULL;

/*
 * 用于管理可重用的共享缓冲区环的私有（非共享）状态。
 * 这是目前唯一一种 BufferAccessStrategy 对象，但将来
 * 我们可能会有其他种类。
 */
typedef struct BufferAccessStrategyData
{
	/* 整体策略类型 */
	BufferAccessStrategyType btype;
	/* buffers[] 数组的元素个数 */
	int			nbuffers;

	/*
	 * 环中"当前"槽位的索引，即最近一次由
	 * GetBufferFromRing 返回的槽位。
	 */
	int			current;

	/*
	 * 缓冲区编号数组。InvalidBuffer（即零）表示我们
	 * 尚未为该环槽位选择缓冲区。为便于分配，它与
	 * 结构体的固定字段一起被 palloc 分配。
	 */
	Buffer		buffers[FLEXIBLE_ARRAY_MEMBER];
}			BufferAccessStrategyData;


/* 内部函数的原型 */
static BufferDesc *GetBufferFromRing(BufferAccessStrategy strategy,
									 uint32 *buf_state);
static void AddBufferToRing(BufferAccessStrategy strategy,
							BufferDesc *buf);

/*
 * ClockSweepTick - StrategyGetBuffer() 的辅助例程。
 *
 * 将时钟指针向前移动一个缓冲区（相对于当前位置），并返回
 * 指针下当前缓冲区的 id。
 */
static inline uint32
ClockSweepTick(void)
{
	uint32		victim;

	/*
	 * 原子地将指针向前移动一个缓冲区 —— 如果有多个进程
	 * 同时这样做，可能导致返回的缓冲区顺序与表面上
	 * 略有出入。
	 */
	victim =
		pg_atomic_fetch_add_u32(&StrategyControl->nextVictimBuffer, 1);

	if (victim >= NBuffers)
	{
		uint32		originalVictim = victim;

		/* 始终将我们在 BufferDescriptors 中查找的内容取模 */
		victim = victim % NBuffers;

		/*
		 * 如果我们是刚刚导致回绕的那个进程，则强制在
		 * 持有自旋锁的情况下递增 completePasses。我们
		 * 需要自旋锁，以便 StrategySyncStart() 能返回一个
		 * 由 nextVictimBuffer 和 completePasses 组成的
		 * 一致值。
		 */
		if (victim == 0)
		{
			uint32		expected;
			uint32		wrapped;
			bool		success = false;

			expected = originalVictim + 1;

			while (!success)
			{
				/*
				 * 在递增 completePasses 时获取自旋锁。这
				 * 允许其他读取者以一致的方式读取 nextVictimBuffer 和
				 * completePasses，而这是 StrategySyncStart() 所
				 * 要求的。理论上延迟递增可能导致 nextVictimBuffers
				 * 溢出，但那极不可能发生，且不会特别有害。
				 */
				SpinLockAcquire(&StrategyControl->buffer_strategy_lock);

				wrapped = expected % NBuffers;

				success = pg_atomic_compare_exchange_u32(&StrategyControl->nextVictimBuffer,
														 &expected, wrapped);
				if (success)
					StrategyControl->completePasses++;
				SpinLockRelease(&StrategyControl->buffer_strategy_lock);
			}
		}
	}
	return victim;
}

/*
 * have_free_buffer —— 无锁检查缓冲池中是否存在空闲缓冲区。
 *
 * 如果结果为真，一旦空闲缓冲区被其他操作移出，结果就会
 * 过时，因此严格需要使用空闲缓冲区的调用者不应调用本函数。
 */
bool
have_free_buffer(void)
{
	if (StrategyControl->firstFreeBuffer >= 0)
		return true;
	else
		return false;
}

/*
 * StrategyGetBuffer
 *
 *	由 bufmgr 调用，以获取在 BufferAlloc() 中使用的下一个
 *	候选缓冲区。BufferAlloc() 唯一的硬性要求是所选择的
 *	缓冲区当前不能被人 pin 住。
 *
 *	strategy 是一个 BufferAccessStrategy 对象，若为 NULL
 *	则使用默认策略。
 *
 *	为确保在我们之前没有别人能 pin 住该缓冲区，我们必须在
 *	仍持有缓冲区头部自旋锁的情况下返回该缓冲区。
 */
BufferDesc *
StrategyGetBuffer(BufferAccessStrategy strategy, uint32 *buf_state, bool *from_ring)
{
	BufferDesc *buf;
	int			bgwprocno;
	int			trycounter;
	uint32		local_buf_state;	/* 避免重复（解）引用 */

	*from_ring = false;

	/*
	 * 如果给定了策略对象，看它是否能选择一个缓冲区。
	 * 我们假设策略对象不需要 buffer_strategy_lock。
	 */
	if (strategy != NULL)
	{
		buf = GetBufferFromRing(strategy, buf_state);
		if (buf != NULL)
		{
			*from_ring = true;
			return buf;
		}
	}

	/*
	 * 如果需要，我们要唤醒 bgwriter。由于我们不想为此依赖
	 * 自旋锁，所以强制从共享内存读取一次，然后基于该值
	 * 设置 latch。我们需要费此周折，是因为否则 bgwprocno
	 * 可能在检查期间或之后被重置，因为编译器可能只是
	 * 从内存中重新读取。
	 *
	 * 如果 bgwriter 在不恰当的时刻退出，这可能会设置错误
	 * 进程的 latch。但由于 PGPROC->procLatch 永远不会被
	 * 释放，最坏的后果只是我们设置了某个任意进程的 latch。
	 */
	bgwprocno = INT_ACCESS_ONCE(StrategyControl->bgwprocno);
	if (bgwprocno != -1)
	{
		/* 先重置 bgwprocno，再设置 latch */
		StrategyControl->bgwprocno = -1;

		/*
		 * 这里不获取 ProcArrayLock，这有点别扭。但实际上
		 * 没关系，因为 procLatch 永远不会被释放，所以我们
		 * 最多只是可能设置了错误进程（或没有进程）的 latch。
		 */
		SetLatch(&ProcGlobal->allProcs[bgwprocno].procLatch);
	}

	/*
	 * 我们统计缓冲区分配请求数，以便 bgwriter 能够估算
	 * 缓冲区消耗速度。注意，由策略对象回收的缓冲区
	 * 故意不在此处计数。
	 */
	pg_atomic_fetch_add_u32(&StrategyControl->numBufferAllocs, 1);

	/*
	 * 首先不加锁检查 freelist 中是否有缓冲区。由于我们
	 * 在每次 StrategyGetBuffer() 调用中并不要求自旋锁，
	 * 在此处获取它会有点可惜 —— 在大多数情况下是
	 * 无用的。这显然留下了一个竞态：缓冲区被放入
	 * freelist 但我们尚未看到该存储 —— 不过这相当
	 * 无害，它只会在下次缓冲区获取时被使用。
	 *
	 * 如果 freelist 上有缓冲区，则获取自旋锁以弹出一个
	 * 缓冲区。然后检查该缓冲区是否可用，如果不可用
	 * 则重复。
	 *
	 * 注意，freeNext 字段被认为受 buffer_strategy_lock 保护，
	 * 而非各个缓冲区自旋锁保护，因此在不持有自旋锁的
	 * 情况下操作它们是没问题的。
	 */
	if (StrategyControl->firstFreeBuffer >= 0)
	{
		while (true)
		{
			/* 获取自旋锁以从 freelist 中移除元素 */
			SpinLockAcquire(&StrategyControl->buffer_strategy_lock);

			if (StrategyControl->firstFreeBuffer < 0)
			{
				SpinLockRelease(&StrategyControl->buffer_strategy_lock);
				break;
			}

			buf = GetBufferDescriptor(StrategyControl->firstFreeBuffer);
			Assert(buf->freeNext != FREENEXT_NOT_IN_LIST);

			/* 无条件地从 freelist 中移除缓冲区 */
			StrategyControl->firstFreeBuffer = buf->freeNext;
			buf->freeNext = FREENEXT_NOT_IN_LIST;

			/*
			 * 释放锁，以便在我们检查此缓冲区时，别人
			 * 可以访问 freelist。
			 */
			SpinLockRelease(&StrategyControl->buffer_strategy_lock);

			/*
			 * 如果缓冲区被 pin 住或 usage_count 非零，我们
			 * 无法使用它；丢弃它并重试。（这只可能在 VACUUM
			 * 把一个有效缓冲区放入 freelist、然后在我们拿到它
			 * 之前被别人使用的情况下发生。从 8.3 起这大概
			 * 完全不可能了，但我们最好还是检查一下。）
			 */
			local_buf_state = LockBufHdr(buf);
			if (BUF_STATE_GET_REFCOUNT(local_buf_state) == 0
				&& BUF_STATE_GET_USAGECOUNT(local_buf_state) == 0)
			{
				if (strategy != NULL)
					AddBufferToRing(strategy, buf);
				*buf_state = local_buf_state;
				return buf;
			}
			UnlockBufHdr(buf, local_buf_state);
		}
	}

	/* freelist 上什么都没有，因此运行"时钟扫描"算法 */
	trycounter = NBuffers;
	for (;;)
	{
		buf = GetBufferDescriptor(ClockSweepTick());

		/*
		 * 如果缓冲区被 pin 住或 usage_count 非零，我们无法
		 * 使用它；递减 usage_count（除非被 pin 住）并继续
		 * 扫描。
		 */
		local_buf_state = LockBufHdr(buf);

		if (BUF_STATE_GET_REFCOUNT(local_buf_state) == 0)
		{
			if (BUF_STATE_GET_USAGECOUNT(local_buf_state) != 0)
			{
				local_buf_state -= BUF_USAGECOUNT_ONE;

				trycounter = NBuffers;
			}
			else
			{
				/* 找到了一个可用的缓冲区 */
				if (strategy != NULL)
					AddBufferToRing(strategy, buf);
				*buf_state = local_buf_state;
				return buf;
			}
		}
		else if (--trycounter == 0)
		{
			/*
			 * 我们已经扫描了所有缓冲区而没有做出任何状态改变，
			 * 因此所有缓冲区都被 pin 住（或者在我们查看时是
			 * 如此）。我们或许盼望有人最终会释放一个，但
			 * 报错可能比冒着陷入无限循环的风险更好。
			 */
			UnlockBufHdr(buf, local_buf_state);
			elog(ERROR, "no unpinned buffers available");
		}
		UnlockBufHdr(buf, local_buf_state);
	}
}

/*
 * StrategyFreeBuffer：将一个缓冲区放入 freelist。
 */
void
StrategyFreeBuffer(BufferDesc *buf)
{
	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);

	/*
	 * 可能有人要求我们放入 freelist 的东西已经在其中了；
	 * 如果发生这种情况，不要搞乱链表。
	 */
	if (buf->freeNext == FREENEXT_NOT_IN_LIST)
	{
		buf->freeNext = StrategyControl->firstFreeBuffer;
		if (buf->freeNext < 0)
			StrategyControl->lastFreeBuffer = buf->buf_id;
		StrategyControl->firstFreeBuffer = buf->buf_id;
	}

	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
}

/*
 * StrategySyncStart —— 告诉 BgBufferSync 从哪里开始同步。
 *
 * 返回最先同步的最佳缓冲区的索引。BgBufferSync() 将从
 * 那里开始绕缓冲区数组循环。
 *
 * 此外，如果传入非 NULL 指针，我们会返回已完成的圈数
 * （实际上即 nextVictimBuffer 的高位）以及最近的
 * 缓冲区分配计数。分配计数在被读取后会被重置。
 */
int
StrategySyncStart(uint32 *complete_passes, uint32 *num_buf_alloc)
{
	uint32		nextVictimBuffer;
	int			result;

	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
	nextVictimBuffer = pg_atomic_read_u32(&StrategyControl->nextVictimBuffer);
	result = nextVictimBuffer % NBuffers;

	if (complete_passes)
	{
		*complete_passes = StrategyControl->completePasses;

		/*
		 * 额外加上在 completePasses 被递增之前发生的
		 * 回绕次数。参见 ClockSweepTick()。
		 */
		*complete_passes += nextVictimBuffer / NBuffers;
	}

	if (num_buf_alloc)
	{
		*num_buf_alloc = pg_atomic_exchange_u32(&StrategyControl->numBufferAllocs, 0);
	}
	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
	return result;
}

/*
 * StrategyNotifyBgWriter —— 设置或清除分配通知 latch。
 *
 * 如果 bgwprocno 不是 -1，下一次 StrategyGetBuffer 调用
 * 将设置该 latch。传入 -1 可在通知发生前清除待定通知。
 * 此特性由 bgwriter 进程用于从休眠中唤醒自身，
 * 不适合其他人使用。
 */
void
StrategyNotifyBgWriter(int bgwprocno)
{
	/*
	 * 我们获取 buffer_strategy_lock 只是为了确保该存储
	 * 对 StrategyGetBuffer 表现为原子操作。bgwriter 应当
	 * 相当不频繁地调用本函数，因此这样保险不会带来
	 * 性能损失。
	 */
	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
	StrategyControl->bgwprocno = bgwprocno;
	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
}


/*
 * StrategyShmemSize
 *
 * 估算 freelist 相关结构所使用的共享内存大小。
 *
 * 注意：由于某些历史原因，缓冲区查找哈希表的大小
 * 也在此处确定。
 */
Size
StrategyShmemSize(void)
{
	Size		size = 0;

	/* 查找哈希表的大小……参见 StrategyInitialize 中的注释 */
	size = add_size(size, BufTableShmemSize(NBuffers + NUM_BUFFER_PARTITIONS));

	/* 共享替换策略控制块的大小 */
	size = add_size(size, MAXALIGN(sizeof(BufferStrategyControl)));

	return size;
}

/*
 * StrategyInitialize —— 初始化缓冲缓存替换策略。
 *
 * 前提：所有缓冲区已经构建成一条链表。
 *		仅由 postmaster 调用，且只在初始化期间。
 */
void
StrategyInitialize(bool init)
{
	bool		found;

	/*
	 * 初始化共享缓冲区查找哈希表。
	 *
	 * 由于我们无法容忍查找表条目耗尽，我们必须确保
	 * 在此处指定足够大的表大小。最大稳态使用量当然是
	 * NBuffers 个条目，但 BufferAlloc() 会在删除旧条目前
	 * 尝试插入新条目。原则上这可能同时在每个分区中
	 * 发生，因此我们可能需要多达 NBuffers + NUM_BUFFER_PARTITIONS
	 * 个条目。
	 */
	InitBufTable(NBuffers + NUM_BUFFER_PARTITIONS);

	/*
	 * 获取或创建共享策略控制块
	 */
	StrategyControl = (BufferStrategyControl *)
		ShmemInitStruct("Buffer Strategy Status",
						sizeof(BufferStrategyControl),
						&found);

	if (!found)
	{
		/*
		 * 只做一次，通常在 postmaster 中
		 */
		Assert(init);

		SpinLockInit(&StrategyControl->buffer_strategy_lock);

		/*
		 * 为我们的策略抓取整个空闲缓冲区链表。我们
		 * 假设它之前已由 BufferManagerShmemInit() 设置好。
		 */
		StrategyControl->firstFreeBuffer = 0;
		StrategyControl->lastFreeBuffer = NBuffers - 1;

		/* 初始化时钟扫描指针 */
		pg_atomic_init_u32(&StrategyControl->nextVictimBuffer, 0);

		/* 清除统计信息 */
		StrategyControl->completePasses = 0;
		pg_atomic_init_u32(&StrategyControl->numBufferAllocs, 0);

		/* 没有待定通知 */
		StrategyControl->bgwprocno = -1;
	}
	else
		Assert(!init);
}


/* ----------------------------------------------------------------
 *				后端私有缓冲区环管理
 * ----------------------------------------------------------------
 */


/*
 * GetAccessStrategy —— 创建一个 BufferAccessStrategy 对象。
 *
 * 该对象分配在当前内存上下文中。
 */
BufferAccessStrategy
GetAccessStrategy(BufferAccessStrategyType btype)
{
	int			ring_size_kb;

	/*
	 * 选择要使用的环大小。理由参见 buffer/README。
	 *
	 * 注意：如果你更改 BAS_BULKREAD 的环大小，另请参阅
	 * access/heap/syncscan.c 中的 SYNC_SCAN_REPORT_INTERVAL。
	 */
	switch (btype)
	{
		case BAS_NORMAL:
			/* 如果有人要 NORMAL，就给他一个"默认"对象 */
			return NULL;

		case BAS_BULKREAD:
			{
				int			ring_max_kb;

				/*
				 * 环必须始终足够大，以便在将缓冲区提供给策略
				 * 的使用者和该缓冲区被重用之间留出一定的
				 * 时间间隔。否则即使没有并发活动，使用者的
				 * pin 也会阻止缓冲区被重用。
				 *
				 * 我们还需要确保环始终足够大以容纳
				 * SYNC_SCAN_REPORT_INTERVAL，如上所述。
				 *
				 * 因此我们先从一个最小尺寸开始，再在适当时
				 * 进一步增大尺寸。
				 */
				ring_size_kb = 256;

				/*
				 * 如果我们不被允许 pin 足够多的缓冲区，更大的
				 * 环就没有意义。但我们绝不会限制到小于上面的
				 * 最小尺寸。
				 */
				ring_max_kb = GetPinLimit() * (BLCKSZ / 1024);
				ring_max_kb = Max(ring_size_kb, ring_max_kb);

				/*
				 * 我们希望环额外拥有容纳配置 IO 并发度的
				 * 空间。在被读入期间，缓冲区显然还不能被
				 * 重用。
				 *
				 * 每个 IO 最大可达 io_combine_limit 个块，而我们希望
				 * 启动最多 effective_io_concurrency 个 IO。
				 *
				 * 注意 effective_io_concurrency 可能为 0，这会
				 * 禁用 AIO。
				 */
				ring_size_kb += (BLCKSZ / 1024) *
					io_combine_limit * effective_io_concurrency;

				if (ring_size_kb > ring_max_kb)
					ring_size_kb = ring_max_kb;
				break;
			}
		case BAS_BULKWRITE:
			ring_size_kb = 16 * 1024;
			break;
		case BAS_VACUUM:
			ring_size_kb = 2048;
			break;

		default:
			elog(ERROR, "unrecognized buffer access strategy: %d",
				 (int) btype);
			return NULL;		/* 避免编译器告警 */
	}

	return GetAccessStrategyWithSize(btype, ring_size_kb);
}

/*
 * GetAccessStrategyWithSize —— 创建一个缓冲区数等于传入
 *		大小的 BufferAccessStrategy 对象。
 *
 * 如果给定的环大小为 0，则不会创建 BufferAccessStrategy，
 * 函数返回 NULL。ring_size_kb 不得为负数。
 */
BufferAccessStrategy
GetAccessStrategyWithSize(BufferAccessStrategyType btype, int ring_size_kb)
{
	int			ring_buffers;
	BufferAccessStrategy strategy;

	Assert(ring_size_kb >= 0);

	/* 算出 ring_size_kb 是多少个缓冲区 */
	ring_buffers = ring_size_kb / (BLCKSZ / 1024);

	/* 0 表示无限制，因此不需要 BufferAccessStrategy */
	if (ring_buffers == 0)
		return NULL;

	/* 上限为 shared_buffers 的 1/8 */
	ring_buffers = Min(NBuffers / 8, ring_buffers);

	/* NBuffers 绝不应小于 16，因此这不应发生 */
	Assert(ring_buffers > 0);

	/* 分配对象并将所有元素初始化为零 */
	strategy = (BufferAccessStrategy)
		palloc0(offsetof(BufferAccessStrategyData, buffers) +
				ring_buffers * sizeof(Buffer));

	/* 设置并非初始为零的字段 */
	strategy->btype = btype;
	strategy->nbuffers = ring_buffers;

	return strategy;
}

/*
 * GetAccessStrategyBufferCount —— 环中缓冲区数的访问器。
 *
 * 输入为 NULL 时返回 0，以匹配 GetAccessStrategyWithSize()
 * 在大小为 0 时返回 NULL 的行为。
 */
int
GetAccessStrategyBufferCount(BufferAccessStrategy strategy)
{
	if (strategy == NULL)
		return 0;

	return strategy->nbuffers;
}

/*
 * GetAccessStrategyPinLimit —— 获取应被 pin 的缓冲区数的上限。
 *
 * 当 pin 额外的缓冲区以进行预读时，基于环的策略的
 * 使用者面临在预读时一次性 pin 住环中过多部分的
 * 危险。对于某些策略，这意味着从环中"逃逸"；对于
 * 其他策略，这意味着因关联的 WAL 刷写而非常频繁地
 * 将脏数据强制写入磁盘。由于外部代码对此一无所知，
 * 我们允许各个策略类型暴露一个在决定一次性 pin 的
 * 最大缓冲区数时应应用的钳制值。
 *
 * 调用者应将此数值与其他相关限制合并后取最小值。
 */
int
GetAccessStrategyPinLimit(BufferAccessStrategy strategy)
{
	if (strategy == NULL)
		return NBuffers;

	switch (strategy->btype)
	{
		case BAS_BULKREAD:

			/*
			 * 由于 BAS_BULKREAD 使用 StrategyRejectBuffer()，
			 * 脏缓冲区不应成为问题，调用者可自由一次性
			 * pin 住整个环。
			 */
			return strategy->nbuffers;

		default:

			/*
			 * 告诉调用者不要 pin 超过环中一半的缓冲区。
			 * 这是在预读距离和推迟回写及关联 WAL 流量
			 * 之间的一种权衡。
			 */
			return strategy->nbuffers / 2;
	}
}

/*
 * FreeAccessStrategy —— 释放一个 BufferAccessStrategy 对象。
 *
 * 目前简单的 pfree 就够了，但我们希望调用者不要
 * 对 BufferAccessStrategy 的表示做那么多假设。
 */
void
FreeAccessStrategy(BufferAccessStrategy strategy)
{
	/* 如果以"默认"策略调用，不要崩溃 */
	if (strategy != NULL)
		pfree(strategy);
}

/*
 * GetBufferFromRing —— 从环中返回一个缓冲区，若环为空/
 *		不可用则返回 NULL。
 *
 * 返回的缓冲区上持有其 bufhdr 自旋锁。
 */
static BufferDesc *
GetBufferFromRing(BufferAccessStrategy strategy, uint32 *buf_state)
{
	BufferDesc *buf;
	Buffer		bufnum;
	uint32		local_buf_state;	/* 避免重复（解）引用 */


	/* 前进到下一个环槽位 */
	if (++strategy->current >= strategy->nbuffers)
		strategy->current = 0;

	/*
	 * 如果槽位尚未被填充，告诉调用者用普通分配策略
	 * 分配一个新缓冲区。然后它将通过用新缓冲区调用
	 * AddBufferToRing 来填充此槽位。
	 */
	bufnum = strategy->buffers[strategy->current];
	if (bufnum == InvalidBuffer)
		return NULL;

	/*
	 * 如果缓冲区被 pin 住，则在任何情况下我们都无法使用它。
	 *
	 * 如果 usage_count 为 0 或 1，则该缓冲区是公平的（我们
	 * 期望为 1，因为我们自己之前对该环元素的使用会将其
	 * 留在那里，但从那时起它可能被时钟扫描递减过）。
	 * 更高的 usage_count 表示别人已触碰过该缓冲区，因此
	 * 我们不应重用它。
	 */
	buf = GetBufferDescriptor(bufnum - 1);
	local_buf_state = LockBufHdr(buf);
	if (BUF_STATE_GET_REFCOUNT(local_buf_state) == 0
		&& BUF_STATE_GET_USAGECOUNT(local_buf_state) <= 1)
	{
		*buf_state = local_buf_state;
		return buf;
	}
	UnlockBufHdr(buf, local_buf_state);

	/*
	 * 告诉调用者用普通分配策略分配一个新缓冲区。
	 * 然后它将通过 AddBufferToRing 替换此环元素。
	 */
	return NULL;
}

/*
 * AddBufferToRing —— 向缓冲区环中添加一个缓冲区。
 *
 * 调用者必须持有该缓冲区的缓冲区头部自旋锁。由于
 * 这是在被持有自旋锁的情况下调用的，它最好相当廉价。
 */
static void
AddBufferToRing(BufferAccessStrategy strategy, BufferDesc *buf)
{
	strategy->buffers[strategy->current] = BufferDescriptorGetBuffer(buf);
}

/*
 * 工具函数，返回给定 BufferAccessStrategy 的策略环的
 * IOContext。
 */
IOContext
IOContextForStrategy(BufferAccessStrategy strategy)
{
	if (!strategy)
		return IOCONTEXT_NORMAL;

	switch (strategy->btype)
	{
		case BAS_NORMAL:

			/*
			 * 目前，GetAccessStrategy() 对
			 * BufferAccessStrategyType BAS_NORMAL 返回 NULL，
			 * 因此此分支不可达。
			 */
			pg_unreachable();
			return IOCONTEXT_NORMAL;
		case BAS_BULKREAD:
			return IOCONTEXT_BULKREAD;
		case BAS_BULKWRITE:
			return IOCONTEXT_BULKWRITE;
		case BAS_VACUUM:
			return IOCONTEXT_VACUUM;
	}

	elog(ERROR, "unrecognized BufferAccessStrategyType: %d", strategy->btype);
	pg_unreachable();
}

/*
 * StrategyRejectBuffer —— 考虑拒绝一个脏缓冲区。
 *
 * 当使用非默认策略时，如果由 StrategyGetBuffer 选中的
 * 缓冲区需要写出、且那样做将需要刷写 WAL，缓冲区管理器
 * 会调用此函数。这给了我们选择另一个牺牲者的机会。
 *
 * 如果缓冲区管理器应请求一个新的牺牲者则返回 true，
 * 如果应写出并复用该缓冲区则返回 false。
 */
bool
StrategyRejectBuffer(BufferAccessStrategy strategy, BufferDesc *buf, bool from_ring)
{
	/* 我们只在 bulkread 模式下这样做 */
	if (strategy->btype != BAS_BULKREAD)
		return false;

	/* 不要干扰正常缓冲区替换策略的行为 */
	if (!from_ring ||
		strategy->buffers[strategy->current] != BufferDescriptorGetBuffer(buf))
		return false;

	/*
	 * 从环中移除该脏缓冲区；这是在所有环成员都是脏的
	 * 情况下防止无限循环所必需的。
	 */
	strategy->buffers[strategy->current] = InvalidBuffer;

	return true;
}
