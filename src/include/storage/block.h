/*-------------------------------------------------------------------------
 *
 * block.h
 *	  POSTGRES 磁盘块定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/block.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BLOCK_H
#define BLOCK_H

/*
 * BlockNumber：
 *
 * 每个数据文件（堆或索引）被划分为 postgres 磁盘块（可将其视为 I/O 的最小单位——
 * 一个 postgres 缓冲区恰好包含一个磁盘块）。块按顺序编号，从 0 到 0xFFFFFFFE。
 *
 * InvalidBlockNumber 与 bufmgr.h 中的 P_NEW 含义相同。
 *
 * 访问方法、缓冲区管理器和存储管理器几乎是唯一应当直接访问磁盘块的代码。
 */
typedef uint32 BlockNumber;

#define InvalidBlockNumber		((BlockNumber) 0xFFFFFFFF)

#define MaxBlockNumber			((BlockNumber) 0xFFFFFFFE)

/*
 * BlockId：
 *
 * 这是 BlockNumber 的存储类型。换句话说，此类型用于磁盘存储结构（例如 HeapTupleData 中），
 * 而 BlockNumber 是进行计算时使用的类型（例如在访问方法代码中）。
 *
 * 设置独立类型似乎没有别的理由，唯一的好处是 BlockId 可以 SHORTALIGN 对齐（因此包含
 * BlockId 的结构体，如 ItemPointerData，也可以 SHORTALIGN 对齐）。这一点对于减少每个页面
 * 中的行指针（ItemIdData）数组以及每个堆或索引元组头部所需的空间非常重要，因此没有充分理由
 * 不应轻易改变这一设计。
 */
typedef struct BlockIdData
{
	uint16		bi_hi;
	uint16		bi_lo;
} BlockIdData;

typedef BlockIdData *BlockId;	/* 块标识符 */

/* ----------------
 *		辅助函数
 * ----------------
 */

/*
 * BlockNumberIsValid
 *		blockNumber 合法时返回真。
 */
static inline bool
BlockNumberIsValid(BlockNumber blockNumber)
{
	return blockNumber != InvalidBlockNumber;
}

/*
 * BlockIdSet
 *		将块标识符设为指定的值。
 */
static inline void
BlockIdSet(BlockIdData *blockId, BlockNumber blockNumber)
{
	blockId->bi_hi = blockNumber >> 16;
	blockId->bi_lo = blockNumber & 0xffff;
}

/*
 * BlockIdEquals
 *		检查块号是否相等。
 */
static inline bool
BlockIdEquals(const BlockIdData *blockId1, const BlockIdData *blockId2)
{
	return (blockId1->bi_hi == blockId2->bi_hi &&
			blockId1->bi_lo == blockId2->bi_lo);
}

/*
 * BlockIdGetBlockNumber
 *		从块标识符中检索块号。
 */
static inline BlockNumber
BlockIdGetBlockNumber(const BlockIdData *blockId)
{
	return (((BlockNumber) blockId->bi_hi) << 16) | ((BlockNumber) blockId->bi_lo);
}

#endif							/* BLOCK_H */
