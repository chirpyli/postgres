/*-------------------------------------------------------------------------
 *
 * heaptoast.h
 *		针对变长属性外部存储与压缩存储的堆相关定义。
 *
 * Copyright (c) 2000-2025, PostgreSQL Global Development Group
 *
 * src/include/access/heaptoast.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HEAPTOAST_H
#define HEAPTOAST_H

#include "access/htup_details.h"
#include "storage/lockdefs.h"
#include "utils/relcache.h"

/*
 * 找出每页有 N 个元组时，单个元组的最大尺寸。
 */
#define MaximumBytesPerTuple(tuplesPerPage) \
	MAXALIGN_DOWN((BLCKSZ - \
				   MAXALIGN(SizeOfPageHeaderData + (tuplesPerPage) * sizeof(ItemIdData))) \
				  / (tuplesPerPage))

/*
 * 这些符号控制 toaster 的激活。如果元组大于
 * TOAST_TUPLE_THRESHOLD，我们会尝试通过压缩可压缩字段、
 * 并将 EXTENDED 和 EXTERNAL 数据移出行外，将其压缩到不超过
 * TOAST_TUPLE_TARGET 字节。
 *
 * 这两个数值不必相同（尽管目前相同）。TARGET 超过
 * THRESHOLD 没有意义，但将其设得更小可能有益。
 *
 * 目前我们让这两个值都等于 TOAST_TUPLES_PER_PAGE 个元组
 * 能放入一个堆页面时的最大元组尺寸。
 *
 * XXX 虽然这些可以在不重新 initdb 的情况下修改，但在随意更改前
 * 需要仔细考虑 toasting.c 中的 needs_toast_table()。另见
 * large_object.h 中的 LOBLKSIZE，它*无法*在不重新 initdb 的情况下修改。
 */
#define TOAST_TUPLES_PER_PAGE	4

#define TOAST_TUPLE_THRESHOLD	MaximumBytesPerTuple(TOAST_TUPLES_PER_PAGE)

#define TOAST_TUPLE_TARGET		TOAST_TUPLE_THRESHOLD

/*
 * 代码也会考虑将 MAIN 数据移出行外，但仅在前述步骤
 * 都未达到目标元组尺寸时才作为最后手段。在此阶段我们
 * 使用不同的目标尺寸，目前等于能放入一个堆页面的最大元组。
 * 这是合理的，因为用户已要求我们尽可能将数据保留在行内。
 */
#define TOAST_TUPLES_PER_PAGE_MAIN	1

#define TOAST_TUPLE_TARGET_MAIN MaximumBytesPerTuple(TOAST_TUPLES_PER_PAGE_MAIN)

/*
 * 如果索引值大于 TOAST_INDEX_TARGET，我们会尝试压缩它
 * （不过无法将其移出行外）。注意，为简化 index_form_tuple()，
 * 此数值是按 datum 而非按元组计算的。
 */
#define TOAST_INDEX_TARGET		(MaxHeapTupleSize / 16)

/*
 * 当我们在行外存储超大 datum 时，会将其拆分为每块最多
 * TOAST_MAX_CHUNK_SIZE 个数据字节的分块。该数值*必须*足够小，
 * 使得完整的 toast 表元组（包括 ID、序列字段及所有额外开销）
 * 能放入一个页面。这里的代码基于"希望在一个页面上放下
 * EXTERN_TUPLES_PER_PAGE 个最大尺寸的元组"这一考量来设定该尺寸。
 *
 * 注意：修改 TOAST_MAX_CHUNK_SIZE 需要重新 initdb。
 */
#define EXTERN_TUPLES_PER_PAGE	4	/* 只需调整此项 */

#define EXTERN_TUPLE_MAX_SIZE	MaximumBytesPerTuple(EXTERN_TUPLES_PER_PAGE)

#define TOAST_MAX_CHUNK_SIZE	\
	(EXTERN_TUPLE_MAX_SIZE -							\
	 MAXALIGN(SizeofHeapTupleHeader) -					\
	 sizeof(Oid) -										\
	 sizeof(int32) -									\
	 VARHDRSZ)

/* ----------
 * heap_toast_insert_or_update -
 *
 *		由 heap_insert() 和 heap_update() 调用。
 * ----------
 */
extern HeapTuple heap_toast_insert_or_update(Relation rel, HeapTuple newtup,
											 HeapTuple oldtup, int options);

/* ----------
 * heap_toast_delete -
 *
 *		由 heap_delete() 调用。
 * ----------
 */
extern void heap_toast_delete(Relation rel, HeapTuple oldtup,
							  bool is_speculative);

/* ----------
 * toast_flatten_tuple -
 *
 *		将一个元组"扁平化"，使其不包含行外 toast 字段。
 *		（这不会消除压缩或短头 datum。）
 * ----------
 */
extern HeapTuple toast_flatten_tuple(HeapTuple tup, TupleDesc tupleDesc);

/* ----------
 * toast_flatten_tuple_to_datum -
 *
 *		将包含行外 toast 字段的元组"扁平化"为一个 Datum。
 * ----------
 */
extern Datum toast_flatten_tuple_to_datum(HeapTupleHeader tup,
										  uint32 tup_len,
										  TupleDesc tupleDesc);

/* ----------
 * toast_build_flattened_tuple -
 *
 *		构建一个不包含行外 toast 字段的元组。
 *		（这不会消除压缩或短头 datum。）
 * ----------
 */
extern HeapTuple toast_build_flattened_tuple(TupleDesc tupleDesc,
											 Datum *values,
											 bool *isnull);

/* ----------
 * heap_fetch_toast_slice
 *
 *		从存储在堆表中的 toast 值中获取一个分片。
 * ----------
 */
extern void heap_fetch_toast_slice(Relation toastrel, Oid valueid,
								   int32 attrsize, int32 sliceoffset,
								   int32 slicelength, struct varlena *result);

#endif							/* HEAPTOAST_H */
