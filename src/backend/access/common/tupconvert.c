/*-------------------------------------------------------------------------
 *
 * tupconvert.c
 *	  元组转换支持。
 *
 * 这些函数在逻辑上等价、但可能列顺序不同或含有一组不同被删除列的
 * 行类型之间进行转换。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/tupconvert.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tupconvert.h"
#include "executor/tuptable.h"


/*
 * 转换建立例程具有以下通用 API：
 *
 * 建立例程使用 attmap.c 来检查给定的源元组描述符与目标元组描述符是否
 * 在逻辑上兼容。若不兼容则抛出错误。若兼容，则当它们在物理上也兼容
 * （即不需要转换）时返回 NULL，否则返回一个 TupleConversionMap，
 * 供 execute_attr_map_tuple 或 execute_attr_map_slot 执行转换。
 *
 * TupleConversionMap（如果需要的话）在调用方的内存上下文中 palloc 分配。
 * 此外，给定的元组描述符会被该映射引用，因此它们必须存活到映射
 * 不再被需要为止。
 *
 * 调用方必须提供一个合适的首要错误消息，以便在抛出兼容性错误时使用。
 * 推荐的编码实践是对该字符串使用 gettext_noop()，这样它可被翻译，
 * 但除非错误真的被抛出，否则实际上不会被翻译。
 *
 *
 * 实现说明：
 *
 * TupleConversionMap 的关键组成部分是一个 attrMap[] 数组，每个输出列
 * 对应一个条目。该条目包含对应输入列的、基于 1 的索引，或者为 0 以
 * 强制生成一个 NULL 值（用于被删除的输出列）。TupleConversionMap 还
 * 包含工作区数组。
 */


/*
 * 建立元组转换，按位置匹配输入列与输出列。
 * （被删除的列在输入和输出中均被忽略。）
 */
TupleConversionMap *
convert_tuples_by_position(TupleDesc indesc,
						   TupleDesc outdesc,
						   const char *msg)
{
	TupleConversionMap *map;
	int			n;
	AttrMap    *attrMap;

	/* 校验兼容性并准备属性编号映射 */
	attrMap = build_attrmap_by_position(indesc, outdesc, msg);

	if (attrMap == NULL)
	{
		/* 不需要运行时转换 */
		return NULL;
	}

	/* 准备映射结构 */
	map = (TupleConversionMap *) palloc(sizeof(TupleConversionMap));
	map->indesc = indesc;
	map->outdesc = outdesc;
	map->attrMap = attrMap;
	/* 为 Datum 数组预分配工作区 */
	n = outdesc->natts + 1;		/* +1 用于 NULL */
	map->outvalues = (Datum *) palloc(n * sizeof(Datum));
	map->outisnull = (bool *) palloc(n * sizeof(bool));
	n = indesc->natts + 1;		/* +1 用于 NULL */
	map->invalues = (Datum *) palloc(n * sizeof(Datum));
	map->inisnull = (bool *) palloc(n * sizeof(bool));
	map->invalues[0] = (Datum) 0;	/* 设置 NULL 条目 */
	map->inisnull[0] = true;

	return map;
}

/*
 * 建立元组转换，按名称匹配输入列与输出列。
 * （被删除的列在输入和输出中均被忽略。）这用于行类型通过继承关联
 * 的场景，因此我们期望类型和 typmod 都精确匹配。除非两个行类型都是
 * 具名的组合类型，否则错误消息会不太有帮助。
 */
TupleConversionMap *
convert_tuples_by_name(TupleDesc indesc,
					   TupleDesc outdesc)
{
	AttrMap    *attrMap;

	/* 校验兼容性并准备属性编号映射 */
	attrMap = build_attrmap_by_name_if_req(indesc, outdesc, false);

	if (attrMap == NULL)
	{
		/* 不需要运行时转换 */
		return NULL;
	}

	return convert_tuples_by_name_attrmap(indesc, outdesc, attrMap);
}

/*
 * 使用给定的 AttrMap 为输入与输出 TupleDesc 建立元组转换。
 */
TupleConversionMap *
convert_tuples_by_name_attrmap(TupleDesc indesc,
							   TupleDesc outdesc,
							   AttrMap *attrMap)
{
	int			n = outdesc->natts;
	TupleConversionMap *map;

	Assert(attrMap != NULL);

	/* 准备映射结构 */
	map = (TupleConversionMap *) palloc(sizeof(TupleConversionMap));
	map->indesc = indesc;
	map->outdesc = outdesc;
	map->attrMap = attrMap;
	/* 为 Datum 数组预分配工作区 */
	map->outvalues = (Datum *) palloc(n * sizeof(Datum));
	map->outisnull = (bool *) palloc(n * sizeof(bool));
	n = indesc->natts + 1;		/* +1 用于 NULL */
	map->invalues = (Datum *) palloc(n * sizeof(Datum));
	map->inisnull = (bool *) palloc(n * sizeof(bool));
	map->invalues[0] = (Datum) 0;	/* 设置 NULL 条目 */
	map->inisnull[0] = true;

	return map;
}

/*
 * 根据映射执行元组的转换。
 */
HeapTuple
execute_attr_map_tuple(HeapTuple tuple, TupleConversionMap *map)
{
	AttrMap    *attrMap = map->attrMap;
	Datum	   *invalues = map->invalues;
	bool	   *inisnull = map->inisnull;
	Datum	   *outvalues = map->outvalues;
	bool	   *outisnull = map->outisnull;
	int			i;

	/*
	 * 提取旧元组的所有值，并对数组做偏移，使得 invalues[0] 留作 NULL、
	 * invalues[1] 为第一个源属性；这与 attrMap 中的编号约定完全一致。
	 */
	heap_deform_tuple(tuple, map->indesc, invalues + 1, inisnull + 1);

	/*
	 * 转置到新元组的相应字段中。
	 */
	Assert(attrMap->maplen == map->outdesc->natts);
	for (i = 0; i < attrMap->maplen; i++)
	{
		int			j = attrMap->attnums[i];

		outvalues[i] = invalues[j];
		outisnull[i] = inisnull[j];
	}

	/*
	 * 现在构造新元组。
	 */
	return heap_form_tuple(map->outdesc, outvalues, outisnull);
}

/*
 * 根据映射执行元组槽的转换。
 */
TupleTableSlot *
execute_attr_map_slot(AttrMap *attrMap,
					  TupleTableSlot *in_slot,
					  TupleTableSlot *out_slot)
{
	Datum	   *invalues;
	bool	   *inisnull;
	Datum	   *outvalues;
	bool	   *outisnull;
	int			outnatts;
	int			i;

	/* 合理性检查 */
	Assert(in_slot->tts_tupleDescriptor != NULL &&
		   out_slot->tts_tupleDescriptor != NULL);
	Assert(in_slot->tts_values != NULL && out_slot->tts_values != NULL);

	outnatts = out_slot->tts_tupleDescriptor->natts;

	/* 提取输入槽的所有值。 */
	slot_getallattrs(in_slot);

	/* 在做映射之前，先清空输出槽中的任何旧内容 */
	ExecClearTuple(out_slot);

	invalues = in_slot->tts_values;
	inisnull = in_slot->tts_isnull;
	outvalues = out_slot->tts_values;
	outisnull = out_slot->tts_isnull;

	/* 转置到输出槽的相应字段中。 */
	for (i = 0; i < outnatts; i++)
	{
		int			j = attrMap->attnums[i] - 1;

		/* attrMap->attnums[i] == 0 表示它是一个 NULL datum。 */
		if (j == -1)
		{
			outvalues[i] = (Datum) 0;
			outisnull[i] = true;
		}
		else
		{
			outvalues[i] = invalues[j];
			outisnull[i] = inisnull[j];
		}
	}

	ExecStoreVirtualTuple(out_slot);

	return out_slot;
}

/*
 * 根据映射执行列位图的转换。
 *
 * 输入与输出位图都按 FirstLowInvalidHeapAttributeNumber 做了偏移，
 * 以便容纳系统列，类似于 RangeTblEntry 中的列位图。
 */
Bitmapset *
execute_attr_map_cols(AttrMap *attrMap, Bitmapset *in_cols)
{
	Bitmapset  *out_cols;
	int			out_attnum;

	/* 针对常见平凡情况的快速路径 */
	if (in_cols == NULL)
		return NULL;

	/*
	 * 对于每个输出列，检查它对应哪个输入列。
	 */
	out_cols = NULL;

	for (out_attnum = FirstLowInvalidHeapAttributeNumber;
		 out_attnum <= attrMap->maplen;
		 out_attnum++)
	{
		int			in_attnum;

		if (out_attnum < 0)
		{
			/* 系统列。无需映射。 */
			in_attnum = out_attnum;
		}
		else if (out_attnum == 0)
			continue;
		else
		{
			/* 普通用户列 */
			in_attnum = attrMap->attnums[out_attnum - 1];

			if (in_attnum == 0)
				continue;
		}

		if (bms_is_member(in_attnum - FirstLowInvalidHeapAttributeNumber, in_cols))
			out_cols = bms_add_member(out_cols, out_attnum - FirstLowInvalidHeapAttributeNumber);
	}

	return out_cols;
}

/*
 * 释放一个 TupleConversionMap 结构。
 */
void
free_conversion_map(TupleConversionMap *map)
{
	/* indesc 和 outdesc 不属于我们，不应由我们释放 */
	free_attrmap(map->attrMap);
	pfree(map->invalues);
	pfree(map->inisnull);
	pfree(map->outvalues);
	pfree(map->outisnull);
	pfree(map);
}
