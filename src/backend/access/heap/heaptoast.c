/*-------------------------------------------------------------------------
 *
 * heaptoast.c
 *	  堆特有的定义，用于变长属性的外部存储与压缩存储
 *	  （变长属性的存储）。
 *
 * Copyright (c) 2000-2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/heaptoast.c
 *
 *
 * INTERFACE ROUTINES
 *		heap_toast_insert_or_update -
 *			尝试通过压缩或移出属性，使给定的元组能放入单个页面
 *			（即移出属性）
 *
 *		heap_toast_delete -
 *			当元组被删除时回收其 toast 存储
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/detoast.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/heaptoast.h"
#include "access/toast_helper.h"
#include "access/toast_internals.h"
#include "utils/fmgroids.h"


/* ----------
 * heap_toast_delete -
 *
 *	在 DELETE 时对 toast 项进行级联删除
 * ----------
 */
void
heap_toast_delete(Relation rel, HeapTuple oldtup, bool is_speculative)
{
	TupleDesc	tupleDesc;
	Datum		toast_values[MaxHeapAttributeNumber];
	bool		toast_isnull[MaxHeapAttributeNumber];

	/*
	 * 我们只应该被调用于普通关系或物化视图的元组——对 toast 关系递归是糟糕的。
	 */
	Assert(rel->rd_rel->relkind == RELKIND_RELATION ||
		   rel->rd_rel->relkind == RELKIND_MATVIEW);

	/*
	 * 获取元组描述符并将元组拆分为各个字段。
	 *
	 * 注意：在这里使用 heap_deform_tuple() 还是仅对 varlena 列使用
	 * heap_getattr() 是有争议的。如果 varlena 列很少而非 varlena 列很多，
	 * 后者可能更优。然而，heap_deform_tuple 的代价仅为 O(N)，而使用
	 * heap_getattr 的方式在存在很多 varlena 列时代价为 O(N^2)，因此选择
	 * 线性代价似乎更稳妥。（顺便一提，除非至少存在一个 varlena 列，否则
	 * 我们根本不会走到这里。）
	 */
	tupleDesc = rel->rd_att;

	Assert(tupleDesc->natts <= MaxHeapAttributeNumber);
	heap_deform_tuple(oldtup, tupleDesc, toast_values, toast_isnull);

	/* 执行真正的工作。 */
	toast_delete_external(rel, toast_values, toast_isnull, is_speculative);
}


/* ----------
 * heap_toast_insert_or_update -
 *
 *	删除不再使用的 toast 项并创建新的 toast 项，以
 *	使新元组能放入 INSERT 或 UPDATE 中
 *
 * 输入：
 *	newtup：要插入的候选新元组
 *	oldtup：用于 UPDATE 的旧行版本；INSERT 时为 NULL
 *	options：传递给 heap_insert()（用于 toast 行）的选项
 * 结果：
 *	若无需 toast 则返回 newtup，否则返回一个 palloc 分配的、被修改过的元组，
 *	该元组才是实际应当被存储的内容
 *
 * 注意：newtup 与 oldtup 都不会被修改。这与本例程 8.1 版本之前的
 * API 不同。
 * ----------
 */
HeapTuple
heap_toast_insert_or_update(Relation rel, HeapTuple newtup, HeapTuple oldtup,
							int options)
{
	HeapTuple	result_tuple;
	TupleDesc	tupleDesc;
	int			numAttrs;

	Size		maxDataLen;
	Size		hoff;

	bool		toast_isnull[MaxHeapAttributeNumber];
	bool		toast_oldisnull[MaxHeapAttributeNumber];
	Datum		toast_values[MaxHeapAttributeNumber];
	Datum		toast_oldvalues[MaxHeapAttributeNumber];
	ToastAttrInfo toast_attr[MaxHeapAttributeNumber];
	ToastTupleContext ttc;

	/*
	 * 忽略 INSERT_SPECULATIVE 选项。推测性的插入/超级删除只是正常地
	 * 插入/删除 toast 值。在这里处理似乎最简单，而不是（可能）在多个
	 * 调用方中分别处理。
	 */
	options &= ~HEAP_INSERT_SPECULATIVE;

	/*
	 * 我们只应该被调用于普通关系或物化视图的元组——对 toast 关系递归是糟糕的。
	 */
	Assert(rel->rd_rel->relkind == RELKIND_RELATION ||
		   rel->rd_rel->relkind == RELKIND_MATVIEW);

	/*
	 * 获取元组描述符并将元组（们）拆分为各个字段。
	 */
	tupleDesc = rel->rd_att;
	numAttrs = tupleDesc->natts;

	Assert(numAttrs <= MaxHeapAttributeNumber);
	heap_deform_tuple(newtup, tupleDesc, toast_values, toast_isnull);
	if (oldtup != NULL)
		heap_deform_tuple(oldtup, tupleDesc, toast_oldvalues, toast_oldisnull);

	/* ----------
	 * 为 toast 做准备
	 * ----------
	 */
	ttc.ttc_rel = rel;
	ttc.ttc_values = toast_values;
	ttc.ttc_isnull = toast_isnull;
	if (oldtup == NULL)
	{
		ttc.ttc_oldvalues = NULL;
		ttc.ttc_oldisnull = NULL;
	}
	else
	{
		ttc.ttc_oldvalues = toast_oldvalues;
		ttc.ttc_oldisnull = toast_oldisnull;
	}
	ttc.ttc_attr = toast_attr;
	toast_tuple_init(&ttc);

/* ----------
 * 压缩和/或外部存储，直到数据能放入目标长度
 *
 *	1：对 attstorage 为 EXTENDED 的属性进行内联压缩，并对很大的
 *	   attstorage 为 EXTENDED 或 EXTERNAL 的大属性立即外部存储
 *	2：将 attstorage 为 EXTENDED 或 EXTERNAL 的属性外部存储
 *	3：对 attstorage 为 MAIN 的属性进行内联压缩
 *	4：将 attstorage 为 MAIN 的属性外部存储
 * ----------
 */

	/* 计算头部开销——这应当与 heap_form_tuple() 保持一致 */
	hoff = SizeofHeapTupleHeader;
	if ((ttc.ttc_flags & TOAST_HAS_NULLS) != 0)
		hoff += BITMAPLEN(numAttrs);
	hoff = MAXALIGN(hoff);
	/* 现在转换为对元组数据大小的限制 */
	maxDataLen = RelationGetToastTupleTarget(rel, TOAST_TUPLE_TARGET) - hoff;

	/*
	 * 查找具有 attstorage EXTENDED 的属性进行压缩。同时找出具有
	 * attstorage EXTENDED 或 EXTERNAL 的大属性，并将其外部存储。
	 */
	while (heap_compute_data_size(tupleDesc,
								  toast_values, toast_isnull) > maxDataLen)
	{
		int			biggest_attno;

		biggest_attno = toast_tuple_find_biggest_attribute(&ttc, true, false);
		if (biggest_attno < 0)
			break;

		/*
		 * 尝试就地压缩它（如果其 attstorage 为 EXTENDED）
		 */
		if (TupleDescAttr(tupleDesc, biggest_attno)->attstorage == TYPSTORAGE_EXTENDED)
			toast_tuple_try_compression(&ttc, biggest_attno);
		else
		{
		/*
		 * 其 attstorage 为 EXTERNAL，在后续的压缩轮次中忽略
		 */
			toast_attr[biggest_attno].tai_colflags |= TOASTCOL_INCOMPRESSIBLE;
		}

		/*
		 * 如果该值单独就超过了 maxDataLen（压缩后若有），则尽可能立即将其
		 * 推入 toast 表。这避免了在常见的“一个长字段加若干短字段”情况下，
		 * 对其他字段进行无用的压缩。
		 *
		 * XXX：也许阈值应该小于 maxDataLen？
		 */
		if (toast_attr[biggest_attno].tai_size > maxDataLen &&
			rel->rd_rel->reltoastrelid != InvalidOid)
			toast_tuple_externalize(&ttc, biggest_attno, options);
	}

	/*
	 * 其次，我们查找仍然是内联的、attstorage 为 EXTENDED 或 EXTERNAL 的属性，
	 * 并将它们外部化。但如果不存在可推入的 toast 表，则跳过此步。
	 */
	while (heap_compute_data_size(tupleDesc,
								  toast_values, toast_isnull) > maxDataLen &&
		   rel->rd_rel->reltoastrelid != InvalidOid)
	{
		int			biggest_attno;

		biggest_attno = toast_tuple_find_biggest_attribute(&ttc, false, false);
		if (biggest_attno < 0)
			break;
		toast_tuple_externalize(&ttc, biggest_attno, options);
	}

	/*
	 * 第 3 轮——这次我们将 storage 为 MAIN 的属性纳入压缩
	 */
	while (heap_compute_data_size(tupleDesc,
								  toast_values, toast_isnull) > maxDataLen)
	{
		int			biggest_attno;

		biggest_attno = toast_tuple_find_biggest_attribute(&ttc, true, true);
		if (biggest_attno < 0)
			break;

		toast_tuple_try_compression(&ttc, biggest_attno);
	}

	/*
	 * 最后，我们将 MAIN 类型的属性外部存储。此时我们增大目标元组大小，
	 * 使得 MAIN 属性除非确实必要，否则不会被外部存储。
	 */
	maxDataLen = TOAST_TUPLE_TARGET_MAIN - hoff;

	while (heap_compute_data_size(tupleDesc,
								  toast_values, toast_isnull) > maxDataLen &&
		   rel->rd_rel->reltoastrelid != InvalidOid)
	{
		int			biggest_attno;

		biggest_attno = toast_tuple_find_biggest_attribute(&ttc, false, true);
		if (biggest_attno < 0)
			break;

		toast_tuple_externalize(&ttc, biggest_attno, options);
	}

	/*
	 * 如果我们对任一值进行了 toast，就需要用变更后的值构建一个新的堆元组。
	 */
	if ((ttc.ttc_flags & TOAST_NEEDS_CHANGE) != 0)
	{
		HeapTupleHeader olddata = newtup->t_data;
		HeapTupleHeader new_data;
		int32		new_header_len;
		int32		new_data_len;
		int32		new_tuple_len;

		/*
		 * 计算元组的新大小。
		 *
		 * 注意：我们过去在此处假设旧元组的 t_hoff 必然等于 new_header_len
		 * 的值，但那是不正确的。旧元组可能拥有比当前更小的 natts，如果自其
		 * 被存储以来发生过 ALTER TABLE ADD COLUMN；而这会导致对空值位图大小的
		 * 不同结论，甚至关于是否根本需要空值位图的不同结论。
		 */
		new_header_len = SizeofHeapTupleHeader;
		if ((ttc.ttc_flags & TOAST_HAS_NULLS) != 0)
			new_header_len += BITMAPLEN(numAttrs);
		new_header_len = MAXALIGN(new_header_len);
		new_data_len = heap_compute_data_size(tupleDesc,
											  toast_values, toast_isnull);
		new_tuple_len = new_header_len + new_data_len;

		/*
		 * 分配并清零所需的空间，并填充 HeapTupleData 字段。
		 */
		result_tuple = (HeapTuple) palloc0(HEAPTUPLESIZE + new_tuple_len);
		result_tuple->t_len = new_tuple_len;
		result_tuple->t_self = newtup->t_self;
		result_tuple->t_tableOid = newtup->t_tableOid;
		new_data = (HeapTupleHeader) ((char *) result_tuple + HEAPTUPLESIZE);
		result_tuple->t_data = new_data;

		/*
		 * 复制现有元组头，但调整 natts 和 t_hoff。
		 */
		memcpy(new_data, olddata, SizeofHeapTupleHeader);
		HeapTupleHeaderSetNatts(new_data, numAttrs);
		new_data->t_hoff = new_header_len;

		/* 复制数据，并在需要时填充空值位图 */
		heap_fill_tuple(tupleDesc,
						toast_values,
						toast_isnull,
						(char *) new_data + new_header_len,
						new_data_len,
						&(new_data->t_infomask),
						((ttc.ttc_flags & TOAST_HAS_NULLS) != 0) ?
						new_data->t_bits : NULL);
	}
	else
		result_tuple = newtup;

	toast_tuple_cleanup(&ttc);

	return result_tuple;
}


/* ----------
 * toast_flatten_tuple -
 *
 *	将一个元组“拍平”，使其不包含任何行外（out-of-line）的 toasted 字段。
 *	（这并不会消除已压缩的或短头部的 datum。）
 *
 *	注意：我们期望调用者已经检查过 HeapTupleHasExternal(tup)，
 *	因此无需短路路径。
 * ----------
 */
HeapTuple
toast_flatten_tuple(HeapTuple tup, TupleDesc tupleDesc)
{
	HeapTuple	new_tuple;
	int			numAttrs = tupleDesc->natts;
	int			i;
	Datum		toast_values[MaxTupleAttributeNumber];
	bool		toast_isnull[MaxTupleAttributeNumber];
	bool		toast_free[MaxTupleAttributeNumber];

	/*
	 * 将元组拆分为各个字段。
	 */
	Assert(numAttrs <= MaxTupleAttributeNumber);
	heap_deform_tuple(tup, tupleDesc, toast_values, toast_isnull);

	memset(toast_free, 0, numAttrs * sizeof(bool));

	for (i = 0; i < numAttrs; i++)
	{
		/*
		 * 查看非空的 varlena 属性
		 */
		if (!toast_isnull[i] && TupleDescCompactAttr(tupleDesc, i)->attlen == -1)
		{
			struct varlena *new_value;

			new_value = (struct varlena *) DatumGetPointer(toast_values[i]);
			if (VARATT_IS_EXTERNAL(new_value))
			{
				new_value = detoast_external_attr(new_value);
				toast_values[i] = PointerGetDatum(new_value);
				toast_free[i] = true;
			}
		}
	}

	/*
	 * 构造重新配置后的元组。
	 */
	new_tuple = heap_form_tuple(tupleDesc, toast_values, toast_isnull);

	/*
	 * 务必复制元组的标识字段。我们也特意复制可见性信息，以防有人在 syscache
	 * 条目中查看这些字段。
	 */
	new_tuple->t_self = tup->t_self;
	new_tuple->t_tableOid = tup->t_tableOid;

	new_tuple->t_data->t_choice = tup->t_data->t_choice;
	new_tuple->t_data->t_ctid = tup->t_data->t_ctid;
	new_tuple->t_data->t_infomask &= ~HEAP_XACT_MASK;
	new_tuple->t_data->t_infomask |=
		tup->t_data->t_infomask & HEAP_XACT_MASK;
	new_tuple->t_data->t_infomask2 &= ~HEAP2_XACT_MASK;
	new_tuple->t_data->t_infomask2 |=
		tup->t_data->t_infomask2 & HEAP2_XACT_MASK;

	/*
	 * 释放已分配的临时值
	 */
	for (i = 0; i < numAttrs; i++)
		if (toast_free[i])
			pfree(DatumGetPointer(toast_values[i]));

	return new_tuple;
}


/* ----------
 * toast_flatten_tuple_to_datum -
 *
 *	将一个包含行外 toasted 字段的元组“拍平”为一个 Datum。
 *	结果总是在当前内存上下文中通过 palloc 分配。
 *
 *	我们有一个通用规则：容器类型（行、数组、范围等）的 Datum 不得包含任何
 *	外部 TOAST 指针。如果没有这条规则，我们在准备存储元组时就必须查看每个
 *	Datum 的内部，而这代价高昂，并且无法干净地扩展到新的容器类型。
 *
 *	然而，我们不想规定以 HeapTuple 表示的元组不能包含 toasted 字段，因此当
 *	这样的 HeapTuple 被转换为 Datum 时，应当调用本例程。
 *
 *	顺带地，我们也会解压任何已压缩的字段。这对于正确性并非必要，但它反映了
 *	一个预期：如果对整个元组（而非单个字段）进行压缩，压缩会更有效。不过我们
 *	并不想仅仅为了去掉压缩字段就去解构并重建元组。因此，调用者通常只会在发现
 *	元组至少存在一个外部字段时才会调用本例程。
 *
 *	另一方面，内联的短头部 varlena 字段会被保持原样。如果我们在这里将它们
 *	“去 toast”，它们随后又会在 heap_fill_tuple 中被改回短头部格式。
 * ----------
 */
Datum
toast_flatten_tuple_to_datum(HeapTupleHeader tup,
							 uint32 tup_len,
							 TupleDesc tupleDesc)
{
	HeapTupleHeader new_data;
	int32		new_header_len;
	int32		new_data_len;
	int32		new_tuple_len;
	HeapTupleData tmptup;
	int			numAttrs = tupleDesc->natts;
	int			i;
	bool		has_nulls = false;
	Datum		toast_values[MaxTupleAttributeNumber];
	bool		toast_isnull[MaxTupleAttributeNumber];
	bool		toast_free[MaxTupleAttributeNumber];

	/* 构建一个临时的 HeapTuple 控制结构 */
	tmptup.t_len = tup_len;
	ItemPointerSetInvalid(&(tmptup.t_self));
	tmptup.t_tableOid = InvalidOid;
	tmptup.t_data = tup;

	/*
	 * 将元组拆分为各个字段。
	 */
	Assert(numAttrs <= MaxTupleAttributeNumber);
	heap_deform_tuple(&tmptup, tupleDesc, toast_values, toast_isnull);

	memset(toast_free, 0, numAttrs * sizeof(bool));

	for (i = 0; i < numAttrs; i++)
	{
		/*
		 * 查看非空的 varlena 属性
		 */
		if (toast_isnull[i])
			has_nulls = true;
		else if (TupleDescCompactAttr(tupleDesc, i)->attlen == -1)
		{
			struct varlena *new_value;

			new_value = (struct varlena *) DatumGetPointer(toast_values[i]);
			if (VARATT_IS_EXTERNAL(new_value) ||
				VARATT_IS_COMPRESSED(new_value))
			{
				new_value = detoast_attr(new_value);
				toast_values[i] = PointerGetDatum(new_value);
				toast_free[i] = true;
			}
		}
	}

	/*
	 * 计算元组的新大小。
	 *
	 * 这应当与 heap_toast_insert_or_update 中的重建代码保持一致。
	 */
	new_header_len = SizeofHeapTupleHeader;
	if (has_nulls)
		new_header_len += BITMAPLEN(numAttrs);
	new_header_len = MAXALIGN(new_header_len);
	new_data_len = heap_compute_data_size(tupleDesc,
										  toast_values, toast_isnull);
	new_tuple_len = new_header_len + new_data_len;

	new_data = (HeapTupleHeader) palloc0(new_tuple_len);

	/*
	 * 复制现有元组头，但调整 natts 和 t_hoff。
	 */
	memcpy(new_data, tup, SizeofHeapTupleHeader);
	HeapTupleHeaderSetNatts(new_data, numAttrs);
	new_data->t_hoff = new_header_len;

	/* 正确设置复合 Datum 头部字段 */
	HeapTupleHeaderSetDatumLength(new_data, new_tuple_len);
	HeapTupleHeaderSetTypeId(new_data, tupleDesc->tdtypeid);
	HeapTupleHeaderSetTypMod(new_data, tupleDesc->tdtypmod);

	/* 复制数据，并在需要时填充空值位图 */
	heap_fill_tuple(tupleDesc,
					toast_values,
					toast_isnull,
					(char *) new_data + new_header_len,
					new_data_len,
					&(new_data->t_infomask),
					has_nulls ? new_data->t_bits : NULL);

	/*
	 * 释放已分配的临时值
	 */
	for (i = 0; i < numAttrs; i++)
		if (toast_free[i])
			pfree(DatumGetPointer(toast_values[i]));

	return PointerGetDatum(new_data);
}


/* ----------
 * toast_build_flattened_tuple -
 *
 *	构建一个不包含任何行外 toasted 字段的元组。
 *	（这并不会消除已压缩的或短头部的 datum。）
 *
 *	这本质上与 heap_form_tuple 类似，区别在于它会
 *	事先展开任何外部数据指针。
 *
 *	是否最好在同时解压内联的已压缩 datum 并不十分明确。
 *	目前我们不会这样做。
 * ----------
 */
HeapTuple
toast_build_flattened_tuple(TupleDesc tupleDesc,
							Datum *values,
							bool *isnull)
{
	HeapTuple	new_tuple;
	int			numAttrs = tupleDesc->natts;
	int			num_to_free;
	int			i;
	Datum		new_values[MaxTupleAttributeNumber];
	Pointer		freeable_values[MaxTupleAttributeNumber];

	/*
	 * 我们可以将调用者的 isnull 数组直接传给 heap_form_tuple，但我们可能
	 * 需要修改 values 数组。
	 */
	Assert(numAttrs <= MaxTupleAttributeNumber);
	memcpy(new_values, values, numAttrs * sizeof(Datum));

	num_to_free = 0;
	for (i = 0; i < numAttrs; i++)
	{
		/*
		 * 查看非空的 varlena 属性
		 */
		if (!isnull[i] && TupleDescCompactAttr(tupleDesc, i)->attlen == -1)
		{
			struct varlena *new_value;

			new_value = (struct varlena *) DatumGetPointer(new_values[i]);
			if (VARATT_IS_EXTERNAL(new_value))
			{
				new_value = detoast_external_attr(new_value);
				new_values[i] = PointerGetDatum(new_value);
				freeable_values[num_to_free++] = (Pointer) new_value;
			}
		}
	}

	/*
	 * 构造重新配置后的元组。
	 */
	new_tuple = heap_form_tuple(tupleDesc, new_values, isnull);

	/*
	 * 释放已分配的临时值
	 */
	for (i = 0; i < num_to_free; i++)
		pfree(freeable_values[i]);

	return new_tuple;
}

/*
 * 从堆表中获取一个 TOAST 切片。
 *
 * toastrel：从中获取块的 relation。
 * valueid：标识正在获取其块的 TOAST 值。
 * attrsize：TOAST 值的总大小。
 * sliceoffset：TOAST 值内部、用于获取的字节偏移量。
 * slicelength：要从 TOAST 值中获取的字节数。
 * result：应当将结果写入其中的 varlena。
 */
void
heap_fetch_toast_slice(Relation toastrel, Oid valueid, int32 attrsize,
					   int32 sliceoffset, int32 slicelength,
					   struct varlena *result)
{
	Relation   *toastidxs;
	ScanKeyData toastkey[3];
	TupleDesc	toasttupDesc = toastrel->rd_att;
	int			nscankeys;
	SysScanDesc toastscan;
	HeapTuple	ttup;
	int32		expectedchunk;
	int32		totalchunks = ((attrsize - 1) / TOAST_MAX_CHUNK_SIZE) + 1;
	int			startchunk;
	int			endchunk;
	int			num_indexes;
	int			validIndex;

	/* 查找 toast 关系的有效索引 */
	validIndex = toast_open_indexes(toastrel,
									AccessShareLock,
									&toastidxs,
									&num_indexes);

	startchunk = sliceoffset / TOAST_MAX_CHUNK_SIZE;
	endchunk = (sliceoffset + slicelength - 1) / TOAST_MAX_CHUNK_SIZE;
	Assert(endchunk <= totalchunks);

	/* 设置用于从索引获取的扫描键。 */
	ScanKeyInit(&toastkey[0],
				(AttrNumber) 1,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(valueid));

	/*
	 * 如果获取所有块，则没有额外条件。否则，对单个块使用相等条件，
	 * 对其他情况使用范围条件。
	 */
	if (startchunk == 0 && endchunk == totalchunks - 1)
		nscankeys = 1;
	else if (startchunk == endchunk)
	{
		ScanKeyInit(&toastkey[1],
					(AttrNumber) 2,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(startchunk));
		nscankeys = 2;
	}
	else
	{
		ScanKeyInit(&toastkey[1],
					(AttrNumber) 2,
					BTGreaterEqualStrategyNumber, F_INT4GE,
					Int32GetDatum(startchunk));
		ScanKeyInit(&toastkey[2],
					(AttrNumber) 2,
					BTLessEqualStrategyNumber, F_INT4LE,
					Int32GetDatum(endchunk));
		nscankeys = 3;
	}

	/* 准备扫描 */
	toastscan = systable_beginscan_ordered(toastrel, toastidxs[validIndex],
										   get_toast_snapshot(), nscankeys, toastkey);

	/*
	 * 通过索引读取各个块
	 *
	 * 索引建立在 (valueid, chunkidx) 上，因此它们会按顺序返回。
	 */
	expectedchunk = startchunk;
	while ((ttup = systable_getnext_ordered(toastscan, ForwardScanDirection)) != NULL)
	{
		int32		curchunk;
		Pointer		chunk;
		bool		isnull;
		char	   *chunkdata;
		int32		chunksize;
		int32		expected_size;
		int32		chcpystrt;
		int32		chcpyend;

		/*
		 * 得到一个块，提取其序号与数据
		 */
		curchunk = DatumGetInt32(fastgetattr(ttup, 2, toasttupDesc, &isnull));
		Assert(!isnull);
		chunk = DatumGetPointer(fastgetattr(ttup, 3, toasttupDesc, &isnull));
		Assert(!isnull);
		if (!VARATT_IS_EXTENDED(chunk))
		{
			chunksize = VARSIZE(chunk) - VARHDRSZ;
			chunkdata = VARDATA(chunk);
		}
		else if (VARATT_IS_SHORT(chunk))
		{
			/* 可能由于 heap_form_tuple 的运作而发生 */
			chunksize = VARSIZE_SHORT(chunk) - VARHDRSZ_SHORT;
			chunkdata = VARDATA_SHORT(chunk);
		}
		else
		{
			/* 不应发生 */
			elog(ERROR, "found toasted toast chunk for toast value %u in %s",
				 valueid, RelationGetRelationName(toastrel));
			chunksize = 0;		/* 让编译器安静 */
			chunkdata = NULL;
		}

		/*
		 * 对我们找到的数据做一些检查
		 */
		if (curchunk != expectedchunk)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("unexpected chunk number %d (expected %d) for toast value %u in %s",
									 curchunk, expectedchunk, valueid,
									 RelationGetRelationName(toastrel))));
		if (curchunk > endchunk)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("unexpected chunk number %d (out of range %d..%d) for toast value %u in %s",
									 curchunk,
									 startchunk, endchunk, valueid,
									 RelationGetRelationName(toastrel))));
		expected_size = curchunk < totalchunks - 1 ? TOAST_MAX_CHUNK_SIZE
			: attrsize - ((totalchunks - 1) * TOAST_MAX_CHUNK_SIZE);
		if (chunksize != expected_size)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("unexpected chunk size %d (expected %d) in chunk %d of %d for toast value %u in %s",
									 chunksize, expected_size,
									 curchunk, totalchunks, valueid,
									 RelationGetRelationName(toastrel))));

		/*
		 * 将数据复制到结果中的正确位置
		 */
		chcpystrt = 0;
		chcpyend = chunksize - 1;
		if (curchunk == startchunk)
			chcpystrt = sliceoffset % TOAST_MAX_CHUNK_SIZE;
		if (curchunk == endchunk)
			chcpyend = (sliceoffset + slicelength - 1) % TOAST_MAX_CHUNK_SIZE;

		memcpy(VARDATA(result) +
			   curchunk * TOAST_MAX_CHUNK_SIZE - sliceoffset + chcpystrt,
			   chunkdata + chcpystrt,
			   (chcpyend - chcpystrt) + 1);

		expectedchunk++;
	}

	/*
	 * 最终检查我们是否成功获取了该 datum
	 */
	if (expectedchunk != (endchunk + 1))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("missing chunk number %d for toast value %u in %s",
								 expectedchunk, valueid,
								 RelationGetRelationName(toastrel))));

	/* 结束扫描并关闭索引。 */
	systable_endscan_ordered(toastscan);
	toast_close_indexes(toastidxs, num_indexes, AccessShareLock);
}
