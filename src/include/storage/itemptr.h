/*-------------------------------------------------------------------------
 *
 * itemptr.h
 *	  POSTGRES 磁盘项指针（item pointer）定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/itemptr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ITEMPTR_H
#define ITEMPTR_H

#include "storage/block.h"
#include "storage/off.h"

/*
 * ItemPointer：
 *
 * 这是一个指向已知文件磁盘页中某一项的指针（例如，从索引指向其父表的交叉链接）。
 * ip_blkid 告诉我们哪个块，ip_posid 告诉我们想要 linp（ItemIdData）数组中的哪个条目。
 *
 * 注意：由于每个元组头部和磁盘上的索引元组头部中都有项指针，因此务必不要浪费空间
 * 用于结构体填充字节。该结构体设计为六字节长（包含三个 int16 字段），但某些编译器
 * 会将其填充到八字节，除非强制对齐。我们在可能的情况下施加了适当的约束。如果无法让
 * 你的编译器配合，将会浪费大量空间。
 */
typedef struct ItemPointerData
{
	BlockIdData ip_blkid;
	OffsetNumber ip_posid;
}

/* 如果编译器支持 packed 和 aligned pragma, 则使用它们 */
#if defined(pg_attribute_packed) && defined(pg_attribute_aligned)
			pg_attribute_packed()
			pg_attribute_aligned(2)
#endif
ItemPointerData;

typedef ItemPointerData *ItemPointer;

/* ----------------
 *		堆元组（t_ctid）中使用的特殊值
 * ----------------
 */

/*
 * 如果堆元组持有投机插入令牌而非真实的 TID，则 ip_posid 设为 SpecTokenOffsetNumber，
 * 令牌存储在 ip_blkid 中。SpecTokenOffsetNumber 必须大于 MaxOffsetNumber，
 * 以便与常规项指针中的合法偏移量区分开来。
 */
#define SpecTokenOffsetNumber		0xfffe

/*
 * 当元组通过 UPDATE 移动到其他分区时，旧元组版本的 t_ctid 会被设置为这个魔数。
 */
#define MovedPartitionsOffsetNumber 0xfffd
#define MovedPartitionsBlockNumber	InvalidBlockNumber


/* ----------------
 *		辅助函数
 * ----------------
 */

/*
 * ItemPointerIsValid
 *		磁盘项指针不为 NULL 时返回真。
 */
static inline bool
ItemPointerIsValid(const ItemPointerData *pointer)
{
	return PointerIsValid(pointer) && pointer->ip_posid != 0;
}

/*
 * ItemPointerGetBlockNumberNoCheck
 *		返回磁盘项指针的块号。
 */
static inline BlockNumber
ItemPointerGetBlockNumberNoCheck(const ItemPointerData *pointer)
{
	return BlockIdGetBlockNumber(&pointer->ip_blkid);
}

/*
 * ItemPointerGetBlockNumber
 *		同上，但会验证项指针看起来是否合法。
 */
static inline BlockNumber
ItemPointerGetBlockNumber(const ItemPointerData *pointer)
{
	Assert(ItemPointerIsValid(pointer));
	return ItemPointerGetBlockNumberNoCheck(pointer);
}

/*
 * ItemPointerGetOffsetNumberNoCheck
 *		返回磁盘项指针的偏移号。
 */
static inline OffsetNumber
ItemPointerGetOffsetNumberNoCheck(const ItemPointerData *pointer)
{
	return pointer->ip_posid;
}

/*
 * ItemPointerGetOffsetNumber
 *		同上，但会验证项指针看起来是否合法。
 */
static inline OffsetNumber
ItemPointerGetOffsetNumber(const ItemPointerData *pointer)
{
	Assert(ItemPointerIsValid(pointer));
	return ItemPointerGetOffsetNumberNoCheck(pointer);
}

/*
 * ItemPointerSet
 *		将磁盘项指针设为指定的块和偏移。
 */
static inline void
ItemPointerSet(ItemPointerData *pointer, BlockNumber blockNumber, OffsetNumber offNum)
{
	Assert(PointerIsValid(pointer));
	BlockIdSet(&pointer->ip_blkid, blockNumber);
	pointer->ip_posid = offNum;
}

/*
 * ItemPointerSetBlockNumber
 *		将磁盘项指针设为指定的块。
 */
static inline void
ItemPointerSetBlockNumber(ItemPointerData *pointer, BlockNumber blockNumber)
{
	Assert(PointerIsValid(pointer));
	BlockIdSet(&pointer->ip_blkid, blockNumber);
}

/*
 * ItemPointerSetOffsetNumber
 *		将磁盘项指针设为指定的偏移。
 */
static inline void
ItemPointerSetOffsetNumber(ItemPointerData *pointer, OffsetNumber offsetNumber)
{
	Assert(PointerIsValid(pointer));
	pointer->ip_posid = offsetNumber;
}

/*
 * ItemPointerCopy
 *		将一个磁盘项指针的内容复制到另一个磁盘项指针。
 *
 * 如果 ItemPointer 中将来出现填充字节，则需要以不同方式处理，因为它被用作哈希键。
 */
static inline void
ItemPointerCopy(const ItemPointerData *fromPointer, ItemPointerData *toPointer)
{
	Assert(PointerIsValid(toPointer));
	Assert(PointerIsValid(fromPointer));
	*toPointer = *fromPointer;
}

/*
 * ItemPointerSetInvalid
 *		将磁盘项指针设为无效。
 */
static inline void
ItemPointerSetInvalid(ItemPointerData *pointer)
{
	Assert(PointerIsValid(pointer));
	BlockIdSet(&pointer->ip_blkid, InvalidBlockNumber);
	pointer->ip_posid = InvalidOffsetNumber;
}

/*
 * ItemPointerIndicatesMovedPartitions
 *		块号表明元组已移动到其他分区时返回真。
 */
static inline bool
ItemPointerIndicatesMovedPartitions(const ItemPointerData *pointer)
{
	return
		ItemPointerGetOffsetNumber(pointer) == MovedPartitionsOffsetNumber &&
		ItemPointerGetBlockNumberNoCheck(pointer) == MovedPartitionsBlockNumber;
}

/*
 * ItemPointerSetMovedPartitions
 *		指示项指针引用的项已移动到其他分区。
 */
static inline void
ItemPointerSetMovedPartitions(ItemPointerData *pointer)
{
	ItemPointerSet(pointer, MovedPartitionsBlockNumber, MovedPartitionsOffsetNumber);
}

/* ----------------
 *		外部函数声明
 * ----------------
 */

extern bool ItemPointerEquals(ItemPointer pointer1, ItemPointer pointer2);
extern int32 ItemPointerCompare(ItemPointer arg1, ItemPointer arg2);
extern void ItemPointerInc(ItemPointer pointer);
extern void ItemPointerDec(ItemPointer pointer);

/* ----------------
 *		Datum 转换函数
 * ----------------
 */

static inline ItemPointer
DatumGetItemPointer(Datum X)
{
	return (ItemPointer) DatumGetPointer(X);
}

static inline Datum
ItemPointerGetDatum(const ItemPointerData *X)
{
	return PointerGetDatum(X);
}

#define PG_GETARG_ITEMPOINTER(n) DatumGetItemPointer(PG_GETARG_DATUM(n))
#define PG_RETURN_ITEMPOINTER(x) return ItemPointerGetDatum(x)

#endif							/* ITEMPTR_H */
