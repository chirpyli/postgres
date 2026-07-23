/*-------------------------------------------------------------------------
 *
 * attmap.c
 *	  属性映射支持。
 *
 * 本文件提供通过比较输入与输出 TupleDesc 来构建和管理属性映射的
 * 工具例程。此类映射通常被作用于继承树与分区树的 DDL 用来在逻辑上
 * 等价、但列顺序不同的行类型之间进行转换，同时考虑被删除的列。
 * tupconvert.c 中的元组转换例程也使用它们。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/attmap.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/attmap.h"
#include "utils/builtins.h"


static bool check_attrmap_match(TupleDesc indesc,
								TupleDesc outdesc,
								AttrMap *attrMap);

/*
 * make_attrmap
 *
 * 在当前内存上下文中分配一个属性映射的工具例程。
 */
AttrMap *
make_attrmap(int maplen)
{
	AttrMap    *res;

	res = (AttrMap *) palloc0(sizeof(AttrMap));
	res->maplen = maplen;
	res->attnums = (AttrNumber *) palloc0(sizeof(AttrNumber) * maplen);
	return res;
}

/*
 * free_attrmap
 *
 * 释放一个属性映射的工具例程。
 */
void
free_attrmap(AttrMap *map)
{
	pfree(map->attnums);
	pfree(map);
}

/*
 * build_attrmap_by_position
 *
 * 返回一个 palloc 分配的、用于元组转换的裸属性映射，按位置匹配输入
 * 列与输出列。被删除的列在输入和输出中均被忽略，标记为 0。这通常
 * 是 tupconvert.c 中 convert_tuples_by_position 的子例程，但也可
 * 单独使用。
 *
 * 注意：errdetail 消息将 indesc 称为“返回”的行类型，将 outdesc 称为
 * “期望”的行类型。这对当前用途没问题，但将来可能需要一般化。
 */
AttrMap *
build_attrmap_by_position(TupleDesc indesc,
						  TupleDesc outdesc,
						  const char *msg)
{
	AttrMap    *attrMap;
	int			nincols;
	int			noutcols;
	int			n;
	int			i;
	int			j;
	bool		same;

/*
 * 长度按期望行类型的属性数量计算，因为它在计数中包含了被删除的属性。
 */
	n = outdesc->natts;
	attrMap = make_attrmap(n);

	j = 0;						/* j 是下一个物理输入属性 */
	nincols = noutcols = 0;		/* 它们统计非删除的属性 */
	same = true;
	for (i = 0; i < n; i++)
	{
		Form_pg_attribute outatt = TupleDescAttr(outdesc, i);

		if (outatt->attisdropped)
			continue;			/* attrMap->attnums[i] 已经为 0 */
		noutcols++;
		for (; j < indesc->natts; j++)
		{
			Form_pg_attribute inatt = TupleDescAttr(indesc, j);

			if (inatt->attisdropped)
				continue;
			nincols++;

			/* 找到匹配的列，现在检查类型 */
			if (outatt->atttypid != inatt->atttypid ||
				(outatt->atttypmod != inatt->atttypmod && outatt->atttypmod >= 0))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg_internal("%s", _(msg)),
						 errdetail("Returned type %s does not match expected type %s in column \"%s\" (position %d).",
								   format_type_with_typemod(inatt->atttypid,
															inatt->atttypmod),
								   format_type_with_typemod(outatt->atttypid,
															outatt->atttypmod),
								   NameStr(outatt->attname),
								   noutcols)));
			attrMap->attnums[i] = (AttrNumber) (j + 1);
			j++;
			break;
		}
		if (attrMap->attnums[i] == 0)
			same = false;		/* 我们会在下方报错 */
	}

	/* 检查未使用的输入列 */
	for (; j < indesc->natts; j++)
	{
		if (TupleDescCompactAttr(indesc, j)->attisdropped)
			continue;
		nincols++;
		same = false;			/* 我们会在下方报错 */
	}

	/* 使用非删除列计数报告列数不匹配 */
	if (!same)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg_internal("%s", _(msg)),
				 errdetail("Number of returned columns (%d) does not match "
						   "expected column count (%d).",
						   nincols, noutcols)));

	/* 检查映射是否为一一匹配 */
	if (check_attrmap_match(indesc, outdesc, attrMap))
	{
		/* 不需要运行时转换 */
		free_attrmap(attrMap);
		return NULL;
	}

	return attrMap;
}

/*
 * build_attrmap_by_name
 *
 * 返回一个 palloc 分配的、用于元组转换的裸属性映射，按名称匹配输入
 * 列与输出列。（被删除的列在输入和输出中均被忽略。）这通常
 * 是 tupconvert.c 中 convert_tuples_by_name 的子例程，但也可
 * 单独使用。
 *
 * 如果 'missing_ok' 为真，那么 'outdesc' 中某个不在 'indesc' 里出现的
 * 列不会被当作错误；这种情况下该 outdesc 列对应的 AttrMap.attnums[]
 * 条目为 0。
 */
AttrMap *
build_attrmap_by_name(TupleDesc indesc,
					  TupleDesc outdesc,
					  bool missing_ok)
{
	AttrMap    *attrMap;
	int			outnatts;
	int			innatts;
	int			i;
	int			nextindesc = -1;

	outnatts = outdesc->natts;
	innatts = indesc->natts;

	attrMap = make_attrmap(outnatts);
	for (i = 0; i < outnatts; i++)
	{
		Form_pg_attribute outatt = TupleDescAttr(outdesc, i);
		char	   *attname;
		Oid			atttypid;
		int32		atttypmod;
		int			j;

		if (outatt->attisdropped)
			continue;			/* attrMap->attnums[i] 已经为 0 */
		attname = NameStr(outatt->attname);
		atttypid = outatt->atttypid;
		atttypmod = outatt->atttypmod;

		/*
		 * 现在在 indesc 中搜索同名的属性。分区表很可能其属性顺序与
		 * 分区保持一致，因此下面的搜索针对这种情况做了优化。有可能
		 * 其中一个关系的列被删除了而另一个没有，因此我们使用
		 * 'nextindesc' 计数器来记录搜索的起始点。如果内层循环遇到
		 * 被删除的列，它必须跳过它们，但应当让 'nextindesc' 停留在
		 * 正确的位置以便下一次外层循环使用。
		 */
		for (j = 0; j < innatts; j++)
		{
			Form_pg_attribute inatt;

			nextindesc++;
			if (nextindesc >= innatts)
				nextindesc = 0;

			inatt = TupleDescAttr(indesc, nextindesc);
			if (inatt->attisdropped)
				continue;
			if (strcmp(attname, NameStr(inatt->attname)) == 0)
			{
				/* 找到了，检查类型 */
				if (atttypid != inatt->atttypid || atttypmod != inatt->atttypmod)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("could not convert row type"),
							 errdetail("Attribute \"%s\" of type %s does not match corresponding attribute of type %s.",
									   attname,
									   format_type_be(outdesc->tdtypeid),
									   format_type_be(indesc->tdtypeid))));
				attrMap->attnums[i] = inatt->attnum;
				break;
			}
		}
		if (attrMap->attnums[i] == 0 && !missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("could not convert row type"),
					 errdetail("Attribute \"%s\" of type %s does not exist in type %s.",
							   attname,
							   format_type_be(outdesc->tdtypeid),
							   format_type_be(indesc->tdtypeid))));
	}
	return attrMap;
}

/*
 * build_attrmap_by_name_if_req
 *
 * 返回由 build_attrmap_by_name 创建的映射，如果不需要转换则返回 NULL。
 * 这是 tupconvert.c 中 convert_tuples_by_name() 及其它函数使用的便捷
 * 例程，但也可单独使用。
 */
AttrMap *
build_attrmap_by_name_if_req(TupleDesc indesc,
							 TupleDesc outdesc,
							 bool missing_ok)
{
	AttrMap    *attrMap;

	/* 校验兼容性并准备属性编号映射 */
	attrMap = build_attrmap_by_name(indesc, outdesc, missing_ok);

	/* 检查映射是否为一一匹配 */
	if (check_attrmap_match(indesc, outdesc, attrMap))
	{
		/* 不需要运行时转换 */
		free_attrmap(attrMap);
		return NULL;
	}

	return attrMap;
}

/*
 * check_attrmap_match
 *
 * 检查映射是否为一一匹配，若是则我们不需要做元组转换，
 * 属性映射也就没有必要了。
 */
static bool
check_attrmap_match(TupleDesc indesc,
					TupleDesc outdesc,
					AttrMap *attrMap)
{
	int			i;

	/* 若属性编号不同则不匹配 */
	if (indesc->natts != outdesc->natts)
		return false;

	for (i = 0; i < attrMap->maplen; i++)
	{
		CompactAttribute *inatt = TupleDescCompactAttr(indesc, i);
		CompactAttribute *outatt;

		/*
		 * 如果输入列含有缺失属性，我们就需要进行转换。
		 */
		if (inatt->atthasmissing)
			return false;

		if (attrMap->attnums[i] == (i + 1))
			continue;

		outatt = TupleDescCompactAttr(outdesc, i);

		/*
		 * 如果它是一个被删除的列，且对应的输入列也被删除了，那么我们
		 * 就不需要转换。不过，attlen 和 attalignby 必须一致。
		 */
		if (attrMap->attnums[i] == 0 &&
			inatt->attisdropped &&
			inatt->attlen == outatt->attlen &&
			inatt->attalignby == outatt->attalignby)
			continue;

		return false;
	}

	return true;
}
