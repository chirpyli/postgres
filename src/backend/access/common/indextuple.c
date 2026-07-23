/*-------------------------------------------------------------------------
 *
 * indextuple.c
 *	   本文件包含索引元组的访问器与修改器例程，
 *	   以及各种元组相关的实用工具函数。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/indextuple.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/detoast.h"
#include "access/heaptoast.h"
#include "access/htup_details.h"
#include "access/itup.h"
#include "access/toast_internals.h"

/*
 * 这将启用索引条目的 de-toast（解压/取出外部存储）。在 VACUUM 尚未
 * 智能到能够从头重建索引之前，这是必需的。
 */
#define TOAST_INDEX_HACK

/* ----------------------------------------------------------------
 *				  index_ 元组接口例程
 * ----------------------------------------------------------------
 */

 /* ----------------
  *		index_form_tuple
  *
  *		与 index_form_tuple_context 相同，但在 CurrentMemoryContext 中
  *		分配返回的元组。
  * ----------------
  */
IndexTuple
index_form_tuple(TupleDesc tupleDescriptor,
				 const Datum *values,
				 const bool *isnull)
{
	return index_form_tuple_context(tupleDescriptor, values, isnull,
									CurrentMemoryContext);
}

/* ----------------
 *		index_form_tuple_context
 *
 *		本函数不应泄漏任何内存；否则诸如
 *		tuplesort_putindextuplevalues() 之类的调用方会非常不满。
 *
 *		只要调用方不传入以 EXTERNAL 方式存储的值，本函数就不应
 *		执行外部表访问。
 *
 *		在提供的 'context' 中分配返回的元组。
 * ----------------
 */
IndexTuple
index_form_tuple_context(TupleDesc tupleDescriptor,
						 const Datum *values,
						 const bool *isnull,
						 MemoryContext context)
{
	char	   *tp;				/* 元组指针 */
	IndexTuple	tuple;			/* 返回的元组 */
	Size		size,
				data_size,
				hoff;
	int			i;
	unsigned short infomask = 0;
	bool		hasnull = false;
	uint16		tupmask = 0;
	int			numberOfAttributes = tupleDescriptor->natts;

#ifdef TOAST_INDEX_HACK
	Datum		untoasted_values[INDEX_MAX_KEYS] = {0};
	bool		untoasted_free[INDEX_MAX_KEYS] = {0};
#endif

	if (numberOfAttributes > INDEX_MAX_KEYS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("number of index columns (%d) exceeds limit (%d)",
						numberOfAttributes, INDEX_MAX_KEYS)));

#ifdef TOAST_INDEX_HACK
	for (i = 0; i < numberOfAttributes; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupleDescriptor, i);

		untoasted_values[i] = values[i];
		untoasted_free[i] = false;

		/* 如果值为 NULL 或不是 varlena 类型，则不做任何处理 */
		if (isnull[i] || att->attlen != -1)
			continue;

		/*
		 * 如果值以 EXTERNAL 方式存储，必须将其取出，以免我们依赖于
		 * 外部存储。将来某天应当改进这一点。
		 */
		if (VARATT_IS_EXTERNAL(DatumGetPointer(values[i])))
		{
			untoasted_values[i] =
				PointerGetDatum(detoast_external_attr((struct varlena *)
													  DatumGetPointer(values[i])));
			untoasted_free[i] = true;
		}

		/*
		 * 如果值超过了尺寸目标，且属于可压缩的数据类型，
		 * 则尝试对其进行行内（in-line）压缩。
		 */
		if (!VARATT_IS_EXTENDED(DatumGetPointer(untoasted_values[i])) &&
			VARSIZE(DatumGetPointer(untoasted_values[i])) > TOAST_INDEX_TARGET &&
			(att->attstorage == TYPSTORAGE_EXTENDED ||
			 att->attstorage == TYPSTORAGE_MAIN))
		{
			Datum		cvalue;

			cvalue = toast_compress_datum(untoasted_values[i],
										  att->attcompression);

			if (DatumGetPointer(cvalue) != NULL)
			{
				/* 压缩成功 */
				if (untoasted_free[i])
					pfree(DatumGetPointer(untoasted_values[i]));
				untoasted_values[i] = cvalue;
				untoasted_free[i] = true;
			}
		}
	}
#endif

	for (i = 0; i < numberOfAttributes; i++)
	{
		if (isnull[i])
		{
			hasnull = true;
			break;
		}
	}

	if (hasnull)
		infomask |= INDEX_NULL_MASK;

	hoff = IndexInfoFindDataOffset(infomask);
#ifdef TOAST_INDEX_HACK
	data_size = heap_compute_data_size(tupleDescriptor,
									   untoasted_values, isnull);
#else
	data_size = heap_compute_data_size(tupleDescriptor,
									   values, isnull);
#endif
	size = hoff + data_size;
	size = MAXALIGN(size);		/* 保守起见 */

	tp = (char *) MemoryContextAllocZero(context, size);
	tuple = (IndexTuple) tp;

	heap_fill_tuple(tupleDescriptor,
#ifdef TOAST_INDEX_HACK
					untoasted_values,
#else
					values,
#endif
					isnull,
					(char *) tp + hoff,
					data_size,
					&tupmask,
					(hasnull ? (bits8 *) tp + sizeof(IndexTupleData) : NULL));

#ifdef TOAST_INDEX_HACK
	for (i = 0; i < numberOfAttributes; i++)
	{
		if (untoasted_free[i])
			pfree(DatumGetPointer(untoasted_values[i]));
	}
#endif

	/*
	 * 我们之所以这样做，是因为 heap_fill_tuple 想要初始化一个用于
	 * HeapTuple 的 "tupmask"，而我们需要的是索引元组的 infomask。
	 * 唯一相关的信息是 "是否含有可变长度属性" 这一字段。上面我们
	 * 已经设置了 hasnull 位。
	 */
	if (tupmask & HEAP_HASVARWIDTH)
		infomask |= INDEX_VAR_MASK;

	/* 同时断言我们已经去除了外部（external）属性 */
#ifdef TOAST_INDEX_HACK
	Assert((tupmask & HEAP_HASEXTERNAL) == 0);
#endif

	/*
	 * 这里我们确保该尺寸能够放入 t_info 中为其预留的字段内。
	 */
	if ((size & INDEX_SIZE_MASK) != size)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("index row requires %zu bytes, maximum size is %zu",
						size, (Size) INDEX_SIZE_MASK)));

	infomask |= size;

	/*
	 * 初始化元数据
	 */
	tuple->t_info = infomask;
	return tuple;
}

/* ----------------
 *		nocache_index_getattr
 *
 *		本函数由 index_getattr() 宏调用，且仅在我们无法使用 cacheoffset
 *		且该值不为 null 的情况下调用。
 *
 *		本函数会在属性描述符中缓存各属性的偏移量。
 *
 *		另一种加速方式是将偏移量与元组一起缓存，但那样似乎更困难，
 *		除非你愿意承受实际把这些偏移量写入发送到磁盘的元组中所带来的
 *		存储开销。真恶心。
 *
 *		本方案会比那种方式稍慢一些，但对于命中大量元组的查询应当表现
 *		良好。一旦你缓存过一次偏移量，使用相同属性描述符检查所有其他
 *		元组的速度都会快得多。 -cim 5/4/91
 * ----------------
 */
Datum
nocache_index_getattr(IndexTuple tup,
					  int attnum,
					  TupleDesc tupleDesc)
{
	char	   *tp;				/* 指向元组数据部分的指针 */
	bits8	   *bp = NULL;		/* 指向元组中 null 位图的指针 */
	bool		slow = false;	/* 我们是否必须逐个遍历属性？ */
	int			data_off;		/* 元组数据偏移量 */
	int			off;			/* 数据内的当前偏移量 */

	/* ----------------
	 *	 三种情况：
	 *
	 *	 1: 没有 null，也没有可变长度属性。
	 *	 2: 在目标属性之后存在一个 null 或可变长度属性。
	 *	 3: 在目标属性之前存在 null 或可变长度属性。
	 * ----------------
	 */

	data_off = IndexInfoFindDataOffset(tup->t_info);

	attnum--;

	if (IndexTupleHasNulls(tup))
	{
		/*
		 * 元组中某处存在一个 null
		 *
		 * 检查所需的属性是否为 null
		 */

		/* XXX 此处 "假定" t_bits 紧跟在固定长度的元组头部之后！ */
		bp = (bits8 *) ((char *) tup + sizeof(IndexTupleData));

		/*
		 * 现在检查前面是否有任何位为 null……
		 */
		{
			int			byte = attnum >> 3;
			int			finalbit = attnum & 0x07;

			/* 检查最后一个字节中处于最终位 "之前" 是否存在 null */
			if ((~bp[byte]) & ((1 << finalbit) - 1))
				slow = true;
			else
			{
				/* 检查任何 "更早" 的字节中是否存在 null */
				int			i;

				for (i = 0; i < byte; i++)
				{
					if (bp[i] != 0xFF)
					{
						slow = true;
						break;
					}
				}
			}
		}
	}

	tp = (char *) tup + data_off;

	if (!slow)
	{
		CompactAttribute *att;

		/*
		 * 如果执行到这里，说明直到（并包括）目标属性为止都没有 null。
		 * 如果我们有缓存的偏移量，就可以直接使用它。
		 */
		att = TupleDescCompactAttr(tupleDesc, attnum);
		if (att->attcacheoff >= 0)
			return fetchatt(att, tp + att->attcacheoff);

		/*
		 * 否则，检查直到（并包括）目标属性为止是否存在非固定长度属性。
		 * 如果没有，则可以廉价且安全地初始化这些属性的缓存偏移量。
		 */
		if (IndexTupleHasVarwidths(tup))
		{
			int			j;

			for (j = 0; j <= attnum; j++)
			{
				if (TupleDescCompactAttr(tupleDesc, j)->attlen <= 0)
				{
					slow = true;
					break;
				}
			}
		}
	}

	if (!slow)
	{
		int			natts = tupleDesc->natts;
		int			j = 1;

		/*
		 * 如果执行到这里，说明该元组直到（并包括）目标属性为止没有
		 * null 也没有可变长度属性，因此我们可以使用缓存的偏移量……
		 * 只是我们还没有它，否则就不会来到这里。由于计算固定长度列的
		 * 偏移量代价很低，我们借此机会初始化 *所有* 前导固定长度列的
		 * 缓存偏移量，以期避免将来再次进入此例程。
		 */
		TupleDescCompactAttr(tupleDesc, 0)->attcacheoff = 0;

		/* 之前我们可能已经在慢速路径中设置了一些偏移量 */
		while (j < natts && TupleDescCompactAttr(tupleDesc, j)->attcacheoff > 0)
			j++;

		off = TupleDescCompactAttr(tupleDesc, j - 1)->attcacheoff +
			TupleDescCompactAttr(tupleDesc, j - 1)->attlen;

		for (; j < natts; j++)
		{
			CompactAttribute *att = TupleDescCompactAttr(tupleDesc, j);

			if (att->attlen <= 0)
				break;

			off = att_nominal_alignby(off, att->attalignby);

			att->attcacheoff = off;

			off += att->attlen;
		}

		Assert(j > attnum);

		off = TupleDescCompactAttr(tupleDesc, attnum)->attcacheoff;
	}
	else
	{
		bool		usecache = true;
		int			i;

		/*
		 * 现在我们知道必须 小心地 遍历元组。但我们仍然可能为下一次
		 * 缓存一些偏移量。
		 *
		 * 注意 - 这个循环有点微妙。对于每个非 null 属性，我们必须先
		 * 计入该属性之前的对齐填充，然后再根据其长度跳过该属性。null
		 * 既不占用存储空间，也没有对齐填充。在遇到 null 或可变长度
		 * 属性之前，我们都可以使用/设置 attcacheoff。
		 */
		off = 0;
		for (i = 0;; i++)		/* 循环出口在 "break" 处 */
		{
			CompactAttribute *att = TupleDescCompactAttr(tupleDesc, i);

			if (IndexTupleHasNulls(tup) && att_isnull(i, bp))
			{
				usecache = false;
				continue;		/* 这个不可能是目标属性 */
			}

			/* 如果我们已知道下一个偏移量，就可以跳过其余部分 */
			if (usecache && att->attcacheoff >= 0)
				off = att->attcacheoff;
			else if (att->attlen == -1)
			{
				/*
				 * 只有当偏移量本身已经适当对齐（从而在任何情况下都不会
				 * 有填充字节）时，我们才能为 varlena 属性缓存该偏移量：
				 * 这样该偏移量对于对齐或未对齐的值都是有效的。
				 */
				if (usecache &&
					off == att_nominal_alignby(off, att->attalignby))
					att->attcacheoff = off;
				else
				{
					off = att_pointer_alignby(off, att->attalignby, -1,
											  tp + off);
					usecache = false;
				}
			}
			else
			{
				/* 不是 varlena，因此可以安全地使用 att_nominal_alignby */
				off = att_nominal_alignby(off, att->attalignby);

				if (usecache)
					att->attcacheoff = off;
			}

			if (i == attnum)
				break;

			off = att_addlength_pointer(off, att->attlen, tp + off);

			if (usecache && att->attlen <= 0)
				usecache = false;
		}
	}

	return fetchatt(TupleDescCompactAttr(tupleDesc, attnum), tp + off);
}

/*
 * 将一个索引元组转换为 Datum/isnull 数组。
 *
 * 调用方必须为输出数组分配足够的存储空间。
 *（INDEX_MAX_KEYS 个条目应当足够。）
 *
 * 本函数与 heap_deform_tuple() 几乎相同，但用于 IndexTuple。
 * 一个区别是该元组绝不应含有任何缺失（missing）列。
 */
void
index_deform_tuple(IndexTuple tup, TupleDesc tupleDescriptor,
				   Datum *values, bool *isnull)
{
	char	   *tp;				/* 指向元组数据的指针 */
	bits8	   *bp;				/* 指向元组中 null 位图的指针 */

	/* XXX 此处 "假定" t_bits 紧跟在固定长度的元组头部之后！ */
	bp = (bits8 *) ((char *) tup + sizeof(IndexTupleData));

	tp = (char *) tup + IndexInfoFindDataOffset(tup->t_info);

	index_deform_tuple_internal(tupleDescriptor, values, isnull,
								tp, bp, IndexTupleHasNulls(tup));
}

/*
 * 将一个索引元组转换为 Datum/isnull 数组，
 * 且不假定索引元组头部具有任何特定布局。
 *
 * 调用方必须提供指向数据区的指针、指向 nulls 位图的指针
 *（当 !hasnulls 时可以为 NULL），以及 hasnulls 标志。
 */
void
index_deform_tuple_internal(TupleDesc tupleDescriptor,
							Datum *values, bool *isnull,
							char *tp, bits8 *bp, int hasnulls)
{
	int			natts = tupleDescriptor->natts; /* 要提取的属性数量 */
	int			attnum;
	int			off = 0;		/* 元组数据中的偏移量 */
	bool		slow = false;	/* 我们能否使用/设置 attcacheoff？ */

	/* 使用断言保护那些分配固定大小数组的调用方 */
	Assert(natts <= INDEX_MAX_KEYS);

	for (attnum = 0; attnum < natts; attnum++)
	{
		CompactAttribute *thisatt = TupleDescCompactAttr(tupleDescriptor, attnum);

		if (hasnulls && att_isnull(attnum, bp))
		{
			values[attnum] = (Datum) 0;
			isnull[attnum] = true;
			slow = true;		/* 不能再使用 attcacheoff 了 */
			continue;
		}

		isnull[attnum] = false;

		if (!slow && thisatt->attcacheoff >= 0)
			off = thisatt->attcacheoff;
		else if (thisatt->attlen == -1)
		{
			/*
			 * 只有当偏移量本身已经适当对齐（从而在任何情况下都不会有
			 * 填充字节）时，我们才能为 varlena 属性缓存该偏移量：这样
			 * 该偏移量对于对齐或未对齐的值都是有效的。
			 */
			if (!slow &&
				off == att_nominal_alignby(off, thisatt->attalignby))
				thisatt->attcacheoff = off;
			else
			{
				off = att_pointer_alignby(off, thisatt->attalignby, -1,
										  tp + off);
				slow = true;
			}
		}
		else
		{
			/* 不是 varlena，因此可以安全地使用 att_nominal_alignby */
			off = att_nominal_alignby(off, thisatt->attalignby);

			if (!slow)
				thisatt->attcacheoff = off;
		}

		values[attnum] = fetchatt(thisatt, tp + off);

		off = att_addlength_pointer(off, thisatt->attlen, tp + off);

		if (thisatt->attlen <= 0)
			slow = true;		/* 不能再使用 attcacheoff 了 */
	}
}

/*
 * 创建一个索引元组的 palloc 副本。
 */
IndexTuple
CopyIndexTuple(IndexTuple source)
{
	IndexTuple	result;
	Size		size;

	size = IndexTupleSize(source);
	result = (IndexTuple) palloc(size);
	memcpy(result, source, size);
	return result;
}

/*
 * 创建一个索引元组的 palloc 副本，仅保留前 leavenatts 个属性。
 *
 * 截断（truncation）能够保证生成的索引元组不会比原始元组更大。
 * 使用原始元组描述符来处理该 IndexTuple 是安全的，但调用方必须
 * 避免实际访问返回元组中被截断的属性！在实践中这意味着调用
 * index_getattr() 时必须特别小心，并且被截断的元组只应由处于
 * 调用方直接控制之下的代码访问。
 *
 * 在持有缓冲区锁的情况下调用本函数是安全的，因为它从不执行外部表
 * 访问。如果将来索引元组有可能包含 EXTERNAL TOAST 值，那么这一点
 * 就必须重新审视。
 */
IndexTuple
index_truncate_tuple(TupleDesc sourceDescriptor, IndexTuple source,
					 int leavenatts)
{
	TupleDesc	truncdesc;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	IndexTuple	truncated;

	Assert(leavenatts <= sourceDescriptor->natts);

	/* 简单情况：实际上并不需要截断 */
	if (leavenatts == sourceDescriptor->natts)
		return CopyIndexTuple(source);

	/* 创建临时的截断元组描述符 */
	truncdesc = CreateTupleDescTruncatedCopy(sourceDescriptor, leavenatts);

	/* 解构，并构造一个属性更少的元组副本 */
	index_deform_tuple(source, truncdesc, values, isnull);
	truncated = index_form_tuple(truncdesc, values, isnull);
	truncated->t_tid = source->t_tid;
	Assert(IndexTupleSize(truncated) <= IndexTupleSize(source));

	/*
	 * 这里不会泄漏内存，TupleDescCopy() 不会分配任何内部结构，因此
	 * 普通的 pfree() 就应当能清理所有已分配的内存
	 */
	pfree(truncdesc);

	return truncated;
}
