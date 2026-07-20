/*-------------------------------------------------------------------------
 *
 * buf_table.c
 *	  将 BufferTag 映射到缓冲区索引的例程。
 *
 * 注意：本文件中的例程不自行加锁。调用者必须
 * 持有相应 BufMappingLock 分区上的合适锁，详见
 * 各函数的注释。我们不能在这些函数内部加锁，因为
 * 大多数情况下调用者需要在释放锁之前调整
 * 缓冲区头部内容（参见 README 中的说明）。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/buf_table.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/buf_internals.h"

/* 缓冲区查找哈希表的条目 */
typedef struct
{
	BufferTag	key;			/* 磁盘页面的 Tag */
	int			id;				/* 关联的缓冲区 ID */
} BufferLookupEnt;

static HTAB *SharedBufHash;


/*
 * 估算映射哈希表所需的空间。
 *		size 是所需的哈希表大小（可能大于 NBuffers）。
 */
Size
BufTableShmemSize(int size)
{
	return hash_estimate_size(size, sizeof(BufferLookupEnt));
}

/*
 * 初始化用于映射缓冲区的共享内存哈希表。
 *		size 是所需的哈希表大小（可能大于 NBuffers）。
 */
void
InitBufTable(int size)
{
	HASHCTL		info;

	/* 假设此时还不需要加锁 */

	/* BufferTag 映射到 Buffer */
	info.keysize = sizeof(BufferTag);
	info.entrysize = sizeof(BufferLookupEnt);
	info.num_partitions = NUM_BUFFER_PARTITIONS;

	SharedBufHash = ShmemInitHash("Shared Buffer Lookup Table",
								  size, size,
								  &info,
								  HASH_ELEM | HASH_BLOBS | HASH_PARTITION);
}

/*
 * BufTableHashCode
 *		计算与 BufferTag 关联的哈希码。
 *
 * 该哈希码必须与 tag 一起传递给查找/插入/删除例程。
 * 我们采用这种设计是因为调用者需要知道哈希码以确定
 * 要锁定哪个缓冲区分区，而我们不想多次计算哈希值
 * （hash_any 有些慢）。
 */
uint32
BufTableHashCode(BufferTag *tagPtr)
{
	return get_hash_value(SharedBufHash, tagPtr);
}

/*
 * BufTableLookup
 *		查找给定的 BufferTag；返回缓冲区 ID，如果未找到则返回 -1。
 *
 * 调用者必须至少持有 tag 所属分区上 BufMappingLock 的共享锁。
 */
int
BufTableLookup(BufferTag *tagPtr, uint32 hashcode)
{
	BufferLookupEnt *result;

	result = (BufferLookupEnt *)
		hash_search_with_hash_value(SharedBufHash,
									tagPtr,
									hashcode,
									HASH_FIND,
									NULL);

	if (!result)
		return -1;

	return result->id;
}

/*
 * BufTableInsert
 *		为给定的 tag 和缓冲区 ID 插入一条哈希表条目，
 *		除非该 tag 的条目已存在。
 *
 * 插入成功返回 -1。如果已有冲突条目存在，
 * 则返回该条目中的缓冲区 ID。
 *
 * 调用者必须持有 tag 所属分区上 BufMappingLock 的排他锁。
 */
int
BufTableInsert(BufferTag *tagPtr, uint32 hashcode, int buf_id)
{
	BufferLookupEnt *result;
	bool		found;

	Assert(buf_id >= 0);		/* -1 保留给不在表中的情况 */
	Assert(tagPtr->blockNum != P_NEW);	/* 无效的 tag */

	result = (BufferLookupEnt *)
		hash_search_with_hash_value(SharedBufHash,
									tagPtr,
									hashcode,
									HASH_ENTER,
									&found);

	if (found)					/* 发现表中已有条目 */
		return result->id;

	result->id = buf_id;

	return -1;
}

/*
 * BufTableDelete
 *		删除给定 tag 的哈希表条目（必须存在）。
 *
 * 调用者必须持有 tag 所属分区上 BufMappingLock 的排他锁。
 */
void
BufTableDelete(BufferTag *tagPtr, uint32 hashcode)
{
	BufferLookupEnt *result;

	result = (BufferLookupEnt *)
		hash_search_with_hash_value(SharedBufHash,
									tagPtr,
									hashcode,
									HASH_REMOVE,
									NULL);

	if (!result)				/* 不应发生 */
		elog(ERROR, "shared buffer hash table corrupted");
}
