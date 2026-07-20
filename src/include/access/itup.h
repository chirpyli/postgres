/*-------------------------------------------------------------------------
 *
 * itup.h
 *	  POSTGRES 索引元组定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/itup.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ITUP_H
#define ITUP_H

#include "access/tupdesc.h"
#include "access/tupmacs.h"
#include "storage/bufpage.h"
#include "storage/itemptr.h"

/*
 * 索引元组头部结构
 *
 * 所有索引元组都以 IndexTupleData 开头。如果 HasNulls 位被设置，
 * 则其后紧随 IndexAttributeBitMapData。索引属性值随其后，
 * 从 MAXALIGN 边界开始。
 *
 * 注意：为位图分配的空间不随属性数量而变化；这是因为我们没有空间
 * 在头部存储属性数量。鉴于 MAXALIGN 约束，对于通常的 INDEX_MAX_KEYS 值，
 * 无论如何也节省不了空间。
 */

typedef struct IndexTupleData
{
	ItemPointerData t_tid;		/* 指向堆元组的引用 TID */

	/* ---------------
	 * t_info 按以下方式布局：
	 *
	 * 第 15 位（高位）：是否有 NULL
	 * 第 14 位：是否有变宽属性
	 * 第 13 位：由索引访问方法定义的语义
	 * 第 12-0 位：元组大小
	 * ---------------
	 */

	unsigned short t_info;		/* 关于元组的各种信息 */

} IndexTupleData;				/* 结构体末尾后还有更多数据 */

typedef IndexTupleData *IndexTuple;

typedef struct IndexAttributeBitMapData
{
	bits8		bits[(INDEX_MAX_KEYS + 8 - 1) / 8];
}			IndexAttributeBitMapData;

typedef IndexAttributeBitMapData * IndexAttributeBitMap;

/*
 * t_info 操作宏
 */
#define INDEX_SIZE_MASK 0x1FFF
#define INDEX_AM_RESERVED_BIT 0x2000	/* 保留给索引访问方法特定
										 * 用途 */
#define INDEX_VAR_MASK	0x4000
#define INDEX_NULL_MASK 0x8000

static inline Size
IndexTupleSize(const IndexTupleData *itup)
{
	return (itup->t_info & INDEX_SIZE_MASK);
}

static inline bool
IndexTupleHasNulls(const IndexTupleData *itup)
{
	return itup->t_info & INDEX_NULL_MASK;
}

static inline bool
IndexTupleHasVarwidths(const IndexTupleData *itup)
{
	return itup->t_info & INDEX_VAR_MASK;
}


/* indextuple.c 中的例程 */
extern IndexTuple index_form_tuple(TupleDesc tupleDescriptor,
								   const Datum *values, const bool *isnull);
extern IndexTuple index_form_tuple_context(TupleDesc tupleDescriptor,
										   const Datum *values, const bool *isnull,
										   MemoryContext context);
extern Datum nocache_index_getattr(IndexTuple tup, int attnum,
								   TupleDesc tupleDesc);
extern void index_deform_tuple(IndexTuple tup, TupleDesc tupleDescriptor,
							   Datum *values, bool *isnull);
extern void index_deform_tuple_internal(TupleDesc tupleDescriptor,
										Datum *values, bool *isnull,
										char *tp, bits8 *bp, int hasnulls);
extern IndexTuple CopyIndexTuple(IndexTuple source);
extern IndexTuple index_truncate_tuple(TupleDesc sourceDescriptor,
									   IndexTuple source, int leavenatts);


/*
 * 以 infomask 为参数（主要是因为该函数需要在 index_form_tuple 时可用，
 * 以便分配足够的空间）。
 */
static inline Size
IndexInfoFindDataOffset(unsigned short t_info)
{
	if (!(t_info & INDEX_NULL_MASK))
		return MAXALIGN(sizeof(IndexTupleData));
	else
		return MAXALIGN(sizeof(IndexTupleData) + sizeof(IndexAttributeBitMapData));
}

#ifndef FRONTEND

/* ----------------
 *		index_getattr
 *
 *		此函数被频繁调用，因此我们将可缓存的查找和 NULL 查找
 *		做了宏内联优化，其余情况调用 nocache_index_getattr()。
 *
 * ----------------
 */
static inline Datum
index_getattr(IndexTuple tup, int attnum, TupleDesc tupleDesc, bool *isnull)
{
	Assert(PointerIsValid(isnull));
	Assert(attnum > 0);

	*isnull = false;

	if (!IndexTupleHasNulls(tup))
	{
		CompactAttribute *attr = TupleDescCompactAttr(tupleDesc, attnum - 1);

		if (attr->attcacheoff >= 0)
		{
			return fetchatt(attr,
							(char *) tup + IndexInfoFindDataOffset(tup->t_info) +
							attr->attcacheoff);
		}
		else
			return nocache_index_getattr(tup, attnum, tupleDesc);
	}
	else
	{
		if (att_isnull(attnum - 1, (bits8 *) tup + sizeof(IndexTupleData)))
		{
			*isnull = true;
			return (Datum) NULL;
		}
		else
			return nocache_index_getattr(tup, attnum, tupleDesc);
	}
}

#endif

/*
 * MaxIndexTuplesPerPage 是单个索引页所能容纳元组数量的上界。
 * 索引元组必须包含数据或 NULL 位图，因此我们可以安全地假设它至少比裸的
 * IndexTupleData 结构体大 1 字节。分母的得出是因为每个元组必须
 * MAXALIGN 对齐，并且必须有相关联的行指针。
 *
 * 为了与索引类型无关，这不考虑页面上的任何特殊空间，因此是保守的。
 *
 * 注意：在 btree 的非叶子页面中，第一个元组没有键（隐式地视为负无穷），
 * 因此打破了"至少大 1 字节"的假设。在这样的页面上，N 个元组可能比这里
 * 估计的少占用一个 MAXALIGN 单元的空间，看似允许比估计多一个元组。
 * 但这样的页面总是至少有 MAXALIGN 的特殊空间，因此我们是安全的。
 */
#define MaxIndexTuplesPerPage	\
	((int) ((BLCKSZ - SizeOfPageHeaderData) / \
			(MAXALIGN(sizeof(IndexTupleData) + 1) + sizeof(ItemIdData))))

#endif							/* ITUP_H */
