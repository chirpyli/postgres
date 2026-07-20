/*-------------------------------------------------------------------------
 *
 * htup.h
 *	  POSTGRES 堆元组定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/htup.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HTUP_H
#define HTUP_H

#include "storage/itemptr.h"

/* htup_details.h 中定义的结构体的类型定义和前向声明 */

typedef struct HeapTupleHeaderData HeapTupleHeaderData;

typedef HeapTupleHeaderData *HeapTupleHeader;

typedef struct MinimalTupleData MinimalTupleData;

typedef MinimalTupleData *MinimalTuple;


/*
 * HeapTupleData 是一个指向元组的内存数据结构。
 *
 * 该数据结构有以下几种使用方式：
 *
 * * 指向磁盘缓冲区中的元组：t_data 直接指向缓冲区内部
 *	 （代码应该持有该缓冲区的 pin，但这并未反映在 HeapTupleData 本身中）。
 *
 * * 指向空：t_data 为 NULL。这在某些函数中用作失败指示。
 *
 * * 属于 palloc 分配的元组的一部分：HeapTupleData 自身和元组
 *	 形成单一的 palloc 分配块。t_data 指向紧接 HeapTupleData 结构体之后
 *	 的内存位置（偏移量为 HEAPTUPLESIZE）。
 *	 这是 heap_form_tuple 及相关例程的输出格式。
 *
 * * 单独分配的元组：t_data 指向一个 palloc 分配的块，
 *	 该块不与 HeapTupleData 相邻。（这种情况已被弃用，因为很难与
 *	 情况 #1 区分。应仅在有限的上下文中使用，
 *	 即代码明确知道情况 #1 不会发生的地方。）
 *
 * * 单独分配的 minimal tuple：t_data 指向 MinimalTuple 起始位置之前
 *	 MINIMAL_TUPLE_OFFSET 字节处。与前一种情况一样，无法通过检查区分
 *	 这种情况与情况 #1；设置或销毁此表示形式的代码
 *	 必须清楚自己在做什么。
 *
 * t_len 应始终有效，指向空的情况除外。
 * 如果 HeapTupleData 指向磁盘缓冲区，或表示磁盘上元组的副本，
 * 则 t_self 和 t_tableOid 应有效。对于人工构造的元组，
 * 应显式将其设置为无效。
 */
typedef struct HeapTupleData
{
	uint32		t_len;			/* *t_data 的长度 */
	ItemPointerData t_self;		/* SelfItemPointer */
	Oid			t_tableOid;		/* 元组所属的表 */
#define FIELDNO_HEAPTUPLEDATA_DATA 3
	HeapTupleHeader t_data;		/* -> 元组头和元组数据 */
} HeapTupleData;

typedef HeapTupleData *HeapTuple;

#define HEAPTUPLESIZE	MAXALIGN(sizeof(HeapTupleData))

/*
 * 用于 HeapTuple 指针的访问宏。
 */
#define HeapTupleIsValid(tuple) PointerIsValid(tuple)

/* HeapTupleHeader 函数，实现在 utils/time/combocid.c 中 */
extern CommandId HeapTupleHeaderGetCmin(const HeapTupleHeaderData *tup);
extern CommandId HeapTupleHeaderGetCmax(const HeapTupleHeaderData *tup);
extern void HeapTupleHeaderAdjustCmax(const HeapTupleHeaderData *tup,
									  CommandId *cmax, bool *iscombo);

/* heapam.c 中 HeapTupleHeader 访问器的原型声明 */
extern TransactionId HeapTupleGetUpdateXid(const HeapTupleHeaderData *tup);

#endif							/* HTUP_H */
