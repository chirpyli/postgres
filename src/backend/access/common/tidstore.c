/*-------------------------------------------------------------------------
 *
 * tidstore.c
 *		TID (ItemPointerData) 存储实现。
 *
 * TidStore 是一种用于存放 TID（ItemPointerData）的内存数据结构。
 * 在内部，它使用基数树（radix tree）作为 TID 的存储。键是 BlockNumber，
 * 值是偏移量的位图，即 BlocktableEntry。
 *
 * TidStore 可以通过 TidStoreCreateShared() 在并行的多个工作进程之间共享。
 * 其他后端可以通过 TidStoreAttach() 附加到这个共享的 TidStore 上。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/common/tidstore.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tidstore.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "storage/lwlock.h"
#include "utils/dsa.h"


#define WORDNUM(x)	((x) / BITS_PER_BITMAPWORD)
#define BITNUM(x)	((x) % BITS_PER_BITMAPWORD)

/* 一个页面中活跃的字（word）数量： */
#define WORDS_PER_PAGE(n) ((n) / BITS_PER_BITMAPWORD + 1)

/* 我们可以在 BlocktableEntry 头部中存储的偏移量数量 */
#define NUM_FULL_OFFSETS ((sizeof(uintptr_t) - sizeof(uint8) - sizeof(int8)) / sizeof(OffsetNumber))

/*
 * 此名称与 tidbitmap.c 中的 PagetableEntry 相似，
 * 因为两者具有类似的功能。
 */
typedef struct BlocktableEntry
{
	struct
	{
#ifndef WORDS_BIGENDIAN
		/*
		 * 我们需要放置这个成员的位置，以便为背后的基数树预留空间，
		 * 使得当结构体 'header' 被存储在指针或 DSA 指针内部时，基数树
		 * 能够标记最低位。
		 */
		uint8		flags;

		int8		nwords;
#endif

		/*
		 * 我们可以在这里存储少量的偏移量，以避免因稀疏位图而浪费空间。
		 */
		OffsetNumber full_offsets[NUM_FULL_OFFSETS];

#ifdef WORDS_BIGENDIAN
		int8		nwords;
		uint8		flags;
#endif
	}			header;

	/*
	 * 我们预期这里不会存在任何填充空间，但为了谨慎起见，创建新条目的
	 * 代码应当把直到 'words' 之前的空间全部清零。
	 */

	bitmapword	words[FLEXIBLE_ARRAY_MEMBER];
} BlocktableEntry;

/*
 * 'nwords' 的类型限制了 'words' 数组中字（word）的最大数量。这里计算
 * 的是我们实际可以在位图中存储的最大偏移量。在实践中，它几乎总是与
 * MaxOffsetNumber 相同。
 */
#define MAX_OFFSET_IN_BITMAP Min(BITS_PER_BITMAPWORD * PG_INT8_MAX - 1, MaxOffsetNumber)

#define MaxBlocktableEntrySize \
	offsetof(BlocktableEntry, words) + \
		(sizeof(bitmapword) * WORDS_PER_PAGE(MAX_OFFSET_IN_BITMAP))

#define RT_PREFIX local_ts
#define RT_SCOPE static
#define RT_DECLARE
#define RT_DEFINE
#define RT_VALUE_TYPE BlocktableEntry
#define RT_VARLEN_VALUE_SIZE(page) \
	(offsetof(BlocktableEntry, words) + \
	sizeof(bitmapword) * (page)->header.nwords)
#define RT_RUNTIME_EMBEDDABLE_VALUE
#include "lib/radixtree.h"

#define RT_PREFIX shared_ts
#define RT_SHMEM
#define RT_SCOPE static
#define RT_DECLARE
#define RT_DEFINE
#define RT_VALUE_TYPE BlocktableEntry
#define RT_VARLEN_VALUE_SIZE(page) \
	(offsetof(BlocktableEntry, words) + \
	sizeof(bitmapword) * (page)->header.nwords)
#define RT_RUNTIME_EMBEDDABLE_VALUE
#include "lib/radixtree.h"

/* 一个 TidStore 的每后端（per-backend）状态 */
struct TidStore
{
	/*
	 * 在使用本地内存时用于基数树（radix tree）的 MemoryContext，
	 * 在使用共享内存时为 NULL
	 */
	MemoryContext rt_context;

	/* TID 的存储。根据 TidStoreIsShared() 的结果选用其中之一 */
	union
	{
		local_ts_radix_tree *local;
		shared_ts_radix_tree *shared;
	}			tree;

	/* 如果使用共享内存，则为 TidStore 使用的 DSA 区域 */
	dsa_area   *area;
};
#define TidStoreIsShared(ts) ((ts)->area != NULL)

/* TidStore 的迭代器 */
struct TidStoreIter
{
	TidStore   *ts;

	/* 基数树的迭代器。根据 TidStoreIsShared() 的结果选用其中之一 */
	union
	{
		shared_ts_iter *shared;
		local_ts_iter *local;
	}			tree_iter;

	/* 返回给调用方的输出 */
	TidStoreIterResult output;
};

/*
 * 创建一个 TidStore。该 TidStore 会存活在本调用时刻的 CurrentMemoryContext
 * 中。由基数树支撑的 TID 存储，会存活在它的子内存上下文 rt_context 中。
 *
 * "max_bytes" 并非一个在内部强制执行的限制；它仅被用作一个提示，来
 * 限制用于 TID 存储的内存上下文的块大小上限。这可以减少因过度分配而
 * 导致的空间浪费。如果调用方想要监控内存使用情况，它必须将自身的
 * 限制与 TidStoreMemoryUsage() 所报告的值进行比较。
 */
TidStore *
TidStoreCreateLocal(size_t max_bytes, bool insert_only)
{
	TidStore   *ts;
	size_t		initBlockSize = ALLOCSET_DEFAULT_INITSIZE;
	size_t		minContextSize = ALLOCSET_DEFAULT_MINSIZE;
	size_t		maxBlockSize = ALLOCSET_DEFAULT_MAXSIZE;

	ts = palloc0(sizeof(TidStore));

	/* 选择 maxBlockSize，使其不大于 max_bytes 的 1/16 */
	while (16 * maxBlockSize > max_bytes)
		maxBlockSize >>= 1;

	if (maxBlockSize < ALLOCSET_DEFAULT_INITSIZE)
		maxBlockSize = ALLOCSET_DEFAULT_INITSIZE;

	/* 为 TID 存储创建一个内存上下文 */
	if (insert_only)
	{
		ts->rt_context = BumpContextCreate(CurrentMemoryContext,
										   "TID storage",
										   minContextSize,
										   initBlockSize,
										   maxBlockSize);
	}
	else
	{
		ts->rt_context = AllocSetContextCreate(CurrentMemoryContext,
											   "TID storage",
											   minContextSize,
											   initBlockSize,
											   maxBlockSize);
	}

	ts->tree.local = local_ts_create(ts->rt_context);

	return ts;
}

/*
 * 与 TidStoreCreateLocal() 类似，但会在 DSA 区域上创建一个共享的
 * TidStore。
 *
 * 返回的对象被分配在后端本地的内存中。
 */
TidStore *
TidStoreCreateShared(size_t max_bytes, int tranche_id)
{
	TidStore   *ts;
	dsa_area   *area;
	size_t		dsa_init_size = DSA_DEFAULT_INIT_SEGMENT_SIZE;
	size_t		dsa_max_size = DSA_MAX_SEGMENT_SIZE;

	ts = palloc0(sizeof(TidStore));

	/*
	 * 选择初始和最大的 DSA 段大小，使其不长于 max_bytes 的 1/8。
	 */
	while (8 * dsa_max_size > max_bytes)
		dsa_max_size >>= 1;

	if (dsa_max_size < DSA_MIN_SEGMENT_SIZE)
		dsa_max_size = DSA_MIN_SEGMENT_SIZE;

	if (dsa_init_size > dsa_max_size)
		dsa_init_size = dsa_max_size;

	area = dsa_create_ext(tranche_id, dsa_init_size, dsa_max_size);
	ts->tree.shared = shared_ts_create(area, tranche_id);
	ts->area = area;

	return ts;
}

/*
 * 附加到共享的 TidStore。'area_handle' 是创建该 TidStore 的 DSA 句柄。
 * 'handle' 是由 TidStoreGetHandle() 返回的 dsa_pointer。返回的对象使用
 * CurrentMemoryContext 分配在后端本地内存中。
 */
TidStore *
TidStoreAttach(dsa_handle area_handle, dsa_pointer handle)
{
	TidStore   *ts;
	dsa_area   *area;

	Assert(area_handle != DSA_HANDLE_INVALID);
	Assert(DsaPointerIsValid(handle));

	/* 创建每后端（per-backend）状态 */
	ts = palloc0(sizeof(TidStore));

	area = dsa_attach(area_handle);

	/* 找到共享的基数树 */
	ts->tree.shared = shared_ts_attach(area, handle);
	ts->area = area;

	return ts;
}

/*
 * 从一个 TidStore 分离（detach）。这同时也会从基数树分离，并释放
 * 后端本地的资源。
 */
void
TidStoreDetach(TidStore *ts)
{
	Assert(TidStoreIsShared(ts));

	shared_ts_detach(ts->tree.shared);
	dsa_detach(ts->area);

	pfree(ts);
}

/*
 * 锁支持函数。
 *
 * 对于共享的 TidStore，我们可以使用基数树的锁，因为需要保护的数据
 * 只有那棵共享的基数树。
 */

void
TidStoreLockExclusive(TidStore *ts)
{
	if (TidStoreIsShared(ts))
		shared_ts_lock_exclusive(ts->tree.shared);
}

void
TidStoreLockShare(TidStore *ts)
{
	if (TidStoreIsShared(ts))
		shared_ts_lock_share(ts->tree.shared);
}

void
TidStoreUnlock(TidStore *ts)
{
	if (TidStoreIsShared(ts))
		shared_ts_unlock(ts->tree.shared);
}

/*
 * 销毁一个 TidStore，释放所有内存。
 *
 * 注意：调用方必须确保，在调用本函数之前，没有任何其他后端会尝试访问
 * 该 TidStore。其他后端必须显式调用 TidStoreDetach() 来释放与该 TidStore
 * 相关联的后端本地内存。调用 TidStoreDestroy() 的那个后端则不得再调用
 * TidStoreDetach()。
 */
void
TidStoreDestroy(TidStore *ts)
{
	/* 销毁底层的基数树 */
	if (TidStoreIsShared(ts))
	{
		shared_ts_free(ts->tree.shared);
		dsa_detach(ts->area);
	}
	else
	{
		local_ts_free(ts->tree.local);
		MemoryContextDelete(ts->rt_context);
	}

	pfree(ts);
}

/*
 * 为给定的块和偏移量数组创建或替换一个条目。
 *
 * 注意：本函数是为 vacuum 的堆扫描阶段而设计并优化的，因此有一些
 * 限制：
 *
 * - 偏移号 "offsets" 必须按升序排列。
 * - 如果块号已存在，则该条目会被替换 —— 无法向条目中添加或从中
 *   移除偏移量。
 */
void
TidStoreSetBlockOffsets(TidStore *ts, BlockNumber blkno, OffsetNumber *offsets,
						int num_offsets)
{
	union
	{
		char		data[MaxBlocktableEntrySize];
		BlocktableEntry force_align_entry;
	}			data;
	BlocktableEntry *page = (BlocktableEntry *) data.data;
	bitmapword	word;
	int			wordnum;
	int			next_word_threshold;
	int			idx = 0;

	Assert(num_offsets > 0);

	/* 检查给定的偏移号是否有序 */
	for (int i = 1; i < num_offsets; i++)
		Assert(offsets[i] > offsets[i - 1]);

	memset(page, 0, offsetof(BlocktableEntry, words));

	if (num_offsets <= NUM_FULL_OFFSETS)
	{
		for (int i = 0; i < num_offsets; i++)
		{
			OffsetNumber off = offsets[i];

			/* 安全检查，确保不会超出位数组的边界 */
			if (off == InvalidOffsetNumber || off > MAX_OFFSET_IN_BITMAP)
				elog(ERROR, "tuple offset out of range: %u", off);

			page->header.full_offsets[i] = off;
		}

		page->header.nwords = 0;
	}
	else
	{
		for (wordnum = 0, next_word_threshold = BITS_PER_BITMAPWORD;
			 wordnum <= WORDNUM(offsets[num_offsets - 1]);
			 wordnum++, next_word_threshold += BITS_PER_BITMAPWORD)
		{
			word = 0;

			while (idx < num_offsets)
			{
				OffsetNumber off = offsets[idx];

				/* 安全检查，确保不会超出位数组的边界 */
				if (off == InvalidOffsetNumber || off > MAX_OFFSET_IN_BITMAP)
					elog(ERROR, "tuple offset out of range: %u", off);

				if (off >= next_word_threshold)
					break;

				word |= ((bitmapword) 1 << BITNUM(off));
				idx++;
			}

			/* 为本 wordnum 写出偏移量位图 */
			page->words[wordnum] = word;
		}

		page->header.nwords = wordnum;
		Assert(page->header.nwords == WORDS_PER_PAGE(offsets[num_offsets - 1]));
	}

	if (TidStoreIsShared(ts))
		shared_ts_set(ts->tree.shared, blkno, page);
	else
		local_ts_set(ts->tree.local, blkno, page);
}

/* 如果给定的 TID 存在于 TidStore 中，则返回 true */
bool
TidStoreIsMember(TidStore *ts, ItemPointer tid)
{
	int			wordnum;
	int			bitnum;
	BlocktableEntry *page;
	BlockNumber blk = ItemPointerGetBlockNumber(tid);
	OffsetNumber off = ItemPointerGetOffsetNumber(tid);

	if (TidStoreIsShared(ts))
		page = shared_ts_find(ts->tree.shared, blk);
	else
		page = local_ts_find(ts->tree.local, blk);

	/* 该 blk 没有对应的条目 */
	if (page == NULL)
		return false;

	if (page->header.nwords == 0)
	{
		/* 偏移量存放在头部中 */
		for (int i = 0; i < NUM_FULL_OFFSETS; i++)
		{
			if (page->header.full_offsets[i] == off)
				return true;
		}
		return false;
	}
	else
	{
		wordnum = WORDNUM(off);
		bitnum = BITNUM(off);

		/* 该 off 没有对应的位图 */
		if (wordnum >= page->header.nwords)
			return false;

		return (page->words[wordnum] & ((bitmapword) 1 << bitnum)) != 0;
	}
}

/*
 * 准备对 TidStore 进行遍历（iterate）。
 *
 * TidStoreIter 结构体被创建在调用方的内存上下文中，并将在
 * TidStoreEndIterate 中被释放。
 *
 * 在迭代完成之前，由调用方负责持有 TidStore 上的锁。
 */
TidStoreIter *
TidStoreBeginIterate(TidStore *ts)
{
	TidStoreIter *iter;

	iter = palloc0(sizeof(TidStoreIter));
	iter->ts = ts;

	if (TidStoreIsShared(ts))
		iter->tree_iter.shared = shared_ts_begin_iterate(ts->tree.shared);
	else
		iter->tree_iter.local = local_ts_begin_iterate(ts->tree.local);

	return iter;
}


/*
 * 返回一个结果，其中包含下一个块号，并可用于通过调用
 * TidStoreGetBlockOffsets() 来获取偏移量集合。该结果是可复制的。
 */
TidStoreIterResult *
TidStoreIterateNext(TidStoreIter *iter)
{
	uint64		key;
	BlocktableEntry *page;

	if (TidStoreIsShared(iter->ts))
		page = shared_ts_iterate_next(iter->tree_iter.shared, &key);
	else
		page = local_ts_iterate_next(iter->tree_iter.local, &key);

	if (page == NULL)
		return NULL;

	iter->output.blkno = key;
	iter->output.internal_page = page;

	return &(iter->output);
}

/*
 * 结束对 TidStore 的遍历（iteration）。
 *
 * 由调用方负责释放任何持有的锁。
 */
void
TidStoreEndIterate(TidStoreIter *iter)
{
	if (TidStoreIsShared(iter->ts))
		shared_ts_end_iterate(iter->tree_iter.shared);
	else
		local_ts_end_iterate(iter->tree_iter.local);

	pfree(iter);
}

/*
 * 返回 TidStore 的内存使用量。
 */
size_t
TidStoreMemoryUsage(TidStore *ts)
{
	if (TidStoreIsShared(ts))
		return shared_ts_memory_usage(ts->tree.shared);
	else
		return local_ts_memory_usage(ts->tree.local);
}

/*
 * 返回 TidStore 所在的 DSA 区域。
 */
dsa_area *
TidStoreGetDSA(TidStore *ts)
{
	Assert(TidStoreIsShared(ts));

	return ts->area;
}

dsa_pointer
TidStoreGetHandle(TidStore *ts)
{
	Assert(TidStoreIsShared(ts));

	return (dsa_pointer) shared_ts_get_handle(ts->tree.shared);
}

/*
 * 给定一个由 TidStoreIterateNext() 返回的 TidStoreIterResult，提取其中的
 * 偏移号。如果数量 <= max_offsets，则返回填入的偏移量个数；否则，在
 * 给定空间中尽可能多地填入，并返回所需缓冲区的大小。
 */
int
TidStoreGetBlockOffsets(TidStoreIterResult *result,
						OffsetNumber *offsets,
						int max_offsets)
{
	BlocktableEntry *page = result->internal_page;
	int			num_offsets = 0;
	int			wordnum;

	if (page->header.nwords == 0)
	{
		/* 偏移量存放在头部中 */
		for (int i = 0; i < NUM_FULL_OFFSETS; i++)
		{
			if (page->header.full_offsets[i] != InvalidOffsetNumber)
			{
				if (num_offsets < max_offsets)
					offsets[num_offsets] = page->header.full_offsets[i];
				num_offsets++;
			}
		}
	}
	else
	{
		for (wordnum = 0; wordnum < page->header.nwords; wordnum++)
		{
			bitmapword	w = page->words[wordnum];
			int			off = wordnum * BITS_PER_BITMAPWORD;

			while (w != 0)
			{
				if (w & 1)
				{
					if (num_offsets < max_offsets)
						offsets[num_offsets] = (OffsetNumber) off;
					num_offsets++;
				}
				off++;
				w >>= 1;
			}
		}
	}

	return num_offsets;
}
