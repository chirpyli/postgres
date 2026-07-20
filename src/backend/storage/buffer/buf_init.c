/*-------------------------------------------------------------------------
 *
 * buf_init.c
 *	  缓冲区管理器初始化例程
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/buf_init.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/aio.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"

BufferDescPadded *BufferDescriptors;
char	   *BufferBlocks;
ConditionVariableMinimallyPadded *BufferIOCVArray;
WritebackContext BackendWritebackContext;
CkptSortItem *CkptBufferIds;


/*
 * 数据结构：
 *		缓冲区存在于 freelist（空闲链表）和一个查找数据结构中。
 *
 *
 * 缓冲区查找：
 *		有两点重要说明。首先，缓冲区必须在 IO 开始之前就可用于查找。
 *		否则，第二个试图读取该缓冲区的进程会分配它自己的副本，
 *		从而导致缓冲池出现不一致。
 *
 * 缓冲区替换：
 *		见 freelist.c。缓冲区在使用过程中（无论是数据管理器使用，
 *		还是处于 IO 期间）都不能被替换。
 *
 *
 * 同步/加锁：
 *
 * IO_IN_PROGRESS —— 这是缓冲区描述符中的一个标志位。
 *		必须在发起 IO 时设置，并在 IO 结束时清除。它的作用是确保一个进程
 *		不会在另一个进程正将缓冲区换入（fault in）时就开始使用该缓冲区。
 *		见 WaitIO 及相关例程。
 *
 * refcount —— 统计持有某缓冲区 pin 的进程数量。
 *		缓冲区在 IO 期间以及 BufferAlloc() 调用之后会立即被 pin 住。
 *		pin 必须在事务结束前释放。出于效率考虑，若单个后端对同一个缓冲区
 *		多次 pin，共享引用计数不会重复增加。详见 bufmgr.c 中的
 *		PrivateRefCount 机制。
 */


/*
 * 初始化共享缓冲池
 *
 * 在共享内存初始化期间会被调用一次（无论是在 postmaster 中，
 * 还是在独立后端中）。
 */
void
BufferManagerShmemInit(void)
{
	bool		foundBufs,
				foundDescs,
				foundIOCV,
				foundBufCkpt;

	/* 将描述符对齐到缓存行边界。 */
	BufferDescriptors = (BufferDescPadded *)
		ShmemInitStruct("Buffer Descriptors",
						NBuffers * sizeof(BufferDescPadded),
						&foundDescs);

	/* 将缓冲池对齐到 IO 页大小边界。 */
	BufferBlocks = (char *)
		TYPEALIGN(PG_IO_ALIGN_SIZE,
				  ShmemInitStruct("Buffer Blocks",
								  NBuffers * (Size) BLCKSZ + PG_IO_ALIGN_SIZE,
								  &foundBufs));

	/* 将条件变量对齐到缓存行边界。 */
	BufferIOCVArray = (ConditionVariableMinimallyPadded *)
		ShmemInitStruct("Buffer IO Condition Variables",
						NBuffers * sizeof(ConditionVariableMinimallyPadded),
						&foundIOCV);

	/*
	 * 用于对即将做检查点的缓冲区 id 进行排序的数组位于共享内存中，
	 * 以避免在运行时分配大量内存。因为该分配可能发生在检查点进行过程中，
	 * 或检查点进程重启时，此时内存分配失败将非常麻烦。
	 */
	CkptBufferIds = (CkptSortItem *)
		ShmemInitStruct("Checkpoint BufferIds",
						NBuffers * sizeof(CkptSortItem), &foundBufCkpt);

	if (foundDescs || foundBufs || foundIOCV || foundBufCkpt)
	{
		/* 要么全部找到，要么一个都找不到 */
		Assert(foundDescs && foundBufs && foundIOCV && foundBufCkpt);
		/* 注意：此分支仅在 EXEC_BACKEND 情况下才会走到 */
	}
	else
	{
		int			i;

		/*
		 * 初始化所有缓冲区头。
		 */
		for (i = 0; i < NBuffers; i++)
		{
			BufferDesc *buf = GetBufferDescriptor(i);

			ClearBufferTag(&buf->tag);

			pg_atomic_init_u32(&buf->state, 0);
			buf->wait_backend_pgprocno = INVALID_PROC_NUMBER;

			buf->buf_id = i;

			pgaio_wref_clear(&buf->io_wref);

			/*
			 * 初始时将所有缓冲区作为未使用状态链接在一起。
			 * 该链表的后续管理由 freelist.c 负责。
			 */
			buf->freeNext = i + 1;

			LWLockInitialize(BufferDescriptorGetContentLock(buf),
							 LWTRANCHE_BUFFER_CONTENT);

			ConditionVariableInit(BufferDescriptorGetIOCV(buf));
		}

		/* 修正链表的最后一个结点 */
		GetBufferDescriptor(NBuffers - 1)->freeNext = FREENEXT_END_OF_LIST;
	}

	/* 初始化其他共享缓冲管理相关内容 */
	StrategyInitialize(!foundDescs);

	/* 初始化每后端文件刷写上下文 */
	WritebackContextInit(&BackendWritebackContext,
						 &backend_flush_after);
}

/*
 * BufferManagerShmemSize
 *
 * 计算缓冲池所需的共享内存大小，包括数据页、缓冲区描述符、哈希表等。
 */
Size
BufferManagerShmemSize(void)
{
	Size		size = 0;

	/* 缓冲区描述符的大小 */
	size = add_size(size, mul_size(NBuffers, sizeof(BufferDescPadded)));
	/* 用于对齐缓冲区描述符 */
	size = add_size(size, PG_CACHE_LINE_SIZE);

	/* 数据页大小，外加对齐填充 */
	size = add_size(size, PG_IO_ALIGN_SIZE);
	size = add_size(size, mul_size(NBuffers, BLCKSZ));

	/* freelist.c 所管理内容的大小 */
	size = add_size(size, StrategyShmemSize());

	/* I/O 条件变量的大小 */
	size = add_size(size, mul_size(NBuffers,
								   sizeof(ConditionVariableMinimallyPadded)));
	/* 用于对齐上述结构 */
	size = add_size(size, PG_CACHE_LINE_SIZE);

	/* bufmgr.c 中检查点排序数组的大小 */
	size = add_size(size, mul_size(NBuffers, sizeof(CkptSortItem)));

	return size;
}
