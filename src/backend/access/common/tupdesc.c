/*-------------------------------------------------------------------------
 *
 * tupdesc.c
 *	  POSTGRES 元组描述符支持代码
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/tupdesc.c
 *
 * 注意
 *	  部分执行器工具代码（如 "ExecTypeFromTL"）应被移动到此处。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/toast_compression.h"
#include "access/tupdesc_details.h"
#include "catalog/catalog.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/resowner.h"
#include "utils/syscache.h"

/* 用于持有 tupledesc 引用的 ResourceOwner 回调  */
static void ResOwnerReleaseTupleDesc(Datum res);
static char *ResOwnerPrintTupleDesc(Datum res);

static const ResourceOwnerDesc tupdesc_resowner_desc =
{
	.name = "tupdesc reference",
	.release_phase = RESOURCE_RELEASE_AFTER_LOCKS,
	.release_priority = RELEASE_PRIO_TUPDESC_REFS,
	.ReleaseResource = ResOwnerReleaseTupleDesc,
	.DebugPrint = ResOwnerPrintTupleDesc
};

/* ResourceOwnerRemember/Forget 之上的便捷封装 */
static inline void
ResourceOwnerRememberTupleDesc(ResourceOwner owner, TupleDesc tupdesc)
{
	ResourceOwnerRemember(owner, PointerGetDatum(tupdesc), &tupdesc_resowner_desc);
}

static inline void
ResourceOwnerForgetTupleDesc(ResourceOwner owner, TupleDesc tupdesc)
{
	ResourceOwnerForget(owner, PointerGetDatum(tupdesc), &tupdesc_resowner_desc);
}

/*
 * populate_compact_attribute_internal
 *		populate_compact_attribute() 的辅助函数
 */
static inline void
populate_compact_attribute_internal(Form_pg_attribute src,
									CompactAttribute *dst)
{
	memset(dst, 0, sizeof(CompactAttribute));

	dst->attcacheoff = -1;
	dst->attlen = src->attlen;

	dst->attbyval = src->attbyval;
	dst->attispackable = (src->attstorage != TYPSTORAGE_PLAIN);
	dst->atthasmissing = src->atthasmissing;
	dst->attisdropped = src->attisdropped;
	dst->attgenerated = (src->attgenerated != '\0');

	/*
	 * 为这一列指定可空性状态。假设存在一个约束，此时我们并不知道
	 * not-null 约束是否有效，因此我们将其设为 UNKNOWN，除非该表是
	 * 系统目录表，这种情况下我们知道它是有效的。
	 */
	dst->attnullability = !src->attnotnull ? ATTNULLABLE_UNRESTRICTED :
		IsCatalogRelationOid(src->attrelid) ? ATTNULLABLE_VALID :
		ATTNULLABLE_UNKNOWN;

	switch (src->attalign)
	{
		case TYPALIGN_INT:
			dst->attalignby = ALIGNOF_INT;
			break;
		case TYPALIGN_CHAR:
			dst->attalignby = sizeof(char);
			break;
		case TYPALIGN_DOUBLE:
			dst->attalignby = ALIGNOF_DOUBLE;
			break;
		case TYPALIGN_SHORT:
			dst->attalignby = ALIGNOF_SHORT;
			break;
		default:
			dst->attalignby = 0;
			elog(ERROR, "invalid attalign value: %c", src->attalign);
			break;
	}
}

/*
 * populate_compact_attribute
 *		用给定属性编号对应的 Form_pg_attribute 填充相应的
 *		CompactAttribute 元素。只要 TupleDesc 中的某个 Form_pg_attribute
 *		被修改，就必须调用本函数。
 */
void
populate_compact_attribute(TupleDesc tupdesc, int attnum)
{
	Form_pg_attribute src = TupleDescAttr(tupdesc, attnum);
	CompactAttribute *dst;

	/*
	 * 不要使用 TupleDescCompactAttr，以避免在开启断言的构建中出现无限递归。
	 */
	dst = &tupdesc->compact_attrs[attnum];

	populate_compact_attribute_internal(src, dst);
}

/*
 * verify_compact_attribute
 *		在开启断言的构建中，我们校验 CompactAttribute 是否被正确填充。
 *		这有助于发现类似 ALTER TABLE 那样修改了 FormData_pg_attribute
 *		却忘记调用 populate_compact_attribute() 的代码缺陷。
 *
 * 本函数在 TupleDescCompactAttr() 中被使用，但在此处声明以便能够访问
 * populate_compact_attribute_internal()。
 */
void
verify_compact_attribute(TupleDesc tupdesc, int attnum)
{
#ifdef USE_ASSERT_CHECKING
	CompactAttribute cattr;
	Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum);
	CompactAttribute tmp;

	/*
	 * 制作 TupleDesc 的 CompactAttribute 的一个临时副本。这可能是共享的
	 * TupleDesc，其 attcacheoff 可能被另一个后端进程修改。
	 */
	memcpy(&cattr, &tupdesc->compact_attrs[attnum], sizeof(CompactAttribute));

	/*
	 * 用对应的 Form_pg_attribute 填充这个临时 CompactAttribute
	 */
	populate_compact_attribute_internal(attr, &tmp);

	/*
	 * 让 attcacheoff 保持一致，因为它已被 populate_compact_attribute_internal
	 * 重置为 -1。attnullability 同理。
	 */
	tmp.attcacheoff = cattr.attcacheoff;
	tmp.attnullability = cattr.attnullability;

	/* 校验新填充的 CompactAttribute 与 TupleDesc 中的相匹配 */
	Assert(memcmp(&tmp, &cattr, sizeof(CompactAttribute)) == 0);
#endif
}

/*
 * CreateTemplateTupleDesc
 *		本函数分配一个空的元组描述符结构。
 *
 * 元组类型 ID 信息初始时被设为匿名记录类型；调用方如有需要可覆盖它。
 */
TupleDesc
CreateTemplateTupleDesc(int natts)
{
	TupleDesc	desc;

	/*
	 * 合理性检查
	 */
	Assert(natts >= 0);

	/*
	 * 为元组描述符、CompactAttribute 数组以及一个 FormData_pg_attribute
	 * 数组分配足够的内存。
	 *
	 * 注意：FormData_pg_attribute 数组的步长是
	 * sizeof(FormData_pg_attribute)，因为我们把数组元素声明为
	 * FormData_pg_attribute 只是为了方便记法。不过，我们只保证每个条目
	 * 的前 ATTRIBUTE_FIXED_PART_SIZE 字节是有效的；大多数复制 tupdesc
	 * 条目的代码也只复制这么多。原则上由于尾部填充它可能更少，但就
	 * pg_attribute 当前的定义而言可能并没有任何填充。
	 */
	desc = (TupleDesc) palloc(offsetof(struct TupleDescData, compact_attrs) +
							  natts * sizeof(CompactAttribute) +
							  natts * sizeof(FormData_pg_attribute));

	/*
	 * 初始化 tupdesc 的其他字段。
	 */
	desc->natts = natts;
	desc->constr = NULL;
	desc->tdtypeid = RECORDOID;
	desc->tdtypmod = -1;
	desc->tdrefcount = -1;		/* 假定不进行引用计数 */

	return desc;
}

/*
 * CreateTupleDesc
 *		本函数通过复制给定的 Form_pg_attribute 数组来分配一个新的 TupleDesc。
 *
 * 元组类型 ID 信息初始时被设为匿名记录类型；调用方如有需要可覆盖它。
 */
TupleDesc
CreateTupleDesc(int natts, Form_pg_attribute *attrs)
{
	TupleDesc	desc;
	int			i;

	desc = CreateTemplateTupleDesc(natts);

	for (i = 0; i < natts; ++i)
	{
		memcpy(TupleDescAttr(desc, i), attrs[i], ATTRIBUTE_FIXED_PART_SIZE);
		populate_compact_attribute(desc, i);
	}
	return desc;
}

/*
 * CreateTupleDescCopy
 *		本函数通过从已有 TupleDesc 复制来创建一个新的 TupleDesc。
 *
 * !!! 约束和默认值不会被复制 !!!
 */
TupleDesc
CreateTupleDescCopy(TupleDesc tupdesc)
{
	TupleDesc	desc;
	int			i;

	desc = CreateTemplateTupleDesc(tupdesc->natts);

	/* 扁平复制属性数组 */
	memcpy(TupleDescAttr(desc, 0),
		   TupleDescAttr(tupdesc, 0),
		   desc->natts * sizeof(FormData_pg_attribute));

	/*
	 * 由于我们不会复制约束和默认值，因此清除与之相关的字段。
	 */
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		att->attnotnull = false;
		att->atthasdef = false;
		att->atthasmissing = false;
		att->attidentity = '\0';
		att->attgenerated = '\0';

		populate_compact_attribute(desc, i);
	}

	/* 我们也可以复制元组类型标识 */
	desc->tdtypeid = tupdesc->tdtypeid;
	desc->tdtypmod = tupdesc->tdtypmod;

	return desc;
}

/*
 * CreateTupleDescTruncatedCopy
 *		本函数创建一个仅包含已有 TupleDesc 前 'natts' 个属性的新 TupleDesc。
 *
 * !!! 约束和默认值不会被复制 !!!
 */
TupleDesc
CreateTupleDescTruncatedCopy(TupleDesc tupdesc, int natts)
{
	TupleDesc	desc;
	int			i;

	Assert(natts <= tupdesc->natts);

	desc = CreateTemplateTupleDesc(natts);

	/* 扁平复制属性数组 */
	memcpy(TupleDescAttr(desc, 0),
		   TupleDescAttr(tupdesc, 0),
		   desc->natts * sizeof(FormData_pg_attribute));

	/*
	 * 由于我们不会复制约束和默认值，因此清除与之相关的字段。
	 */
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		att->attnotnull = false;
		att->atthasdef = false;
		att->atthasmissing = false;
		att->attidentity = '\0';
		att->attgenerated = '\0';

		populate_compact_attribute(desc, i);
	}

	/* 我们也可以复制元组类型标识 */
	desc->tdtypeid = tupdesc->tdtypeid;
	desc->tdtypmod = tupdesc->tdtypmod;

	return desc;
}

/*
 * CreateTupleDescCopyConstr
 *		本函数通过从已有 TupleDesc（连同其约束和默认值）复制来创建新的 TupleDesc。
 */
TupleDesc
CreateTupleDescCopyConstr(TupleDesc tupdesc)
{
	TupleDesc	desc;
	TupleConstr *constr = tupdesc->constr;
	int			i;

	desc = CreateTemplateTupleDesc(tupdesc->natts);

	/* 扁平复制属性数组 */
	memcpy(TupleDescAttr(desc, 0),
		   TupleDescAttr(tupdesc, 0),
		   desc->natts * sizeof(FormData_pg_attribute));

	for (i = 0; i < desc->natts; i++)
	{
		populate_compact_attribute(desc, i);

		TupleDescCompactAttr(desc, i)->attnullability =
			TupleDescCompactAttr(tupdesc, i)->attnullability;
	}

	/* 如有约束结构则复制它 */
	if (constr)
	{
		TupleConstr *cpy = (TupleConstr *) palloc0(sizeof(TupleConstr));

		cpy->has_not_null = constr->has_not_null;
		cpy->has_generated_stored = constr->has_generated_stored;
		cpy->has_generated_virtual = constr->has_generated_virtual;

		if ((cpy->num_defval = constr->num_defval) > 0)
		{
			cpy->defval = (AttrDefault *) palloc(cpy->num_defval * sizeof(AttrDefault));
			memcpy(cpy->defval, constr->defval, cpy->num_defval * sizeof(AttrDefault));
			for (i = cpy->num_defval - 1; i >= 0; i--)
				cpy->defval[i].adbin = pstrdup(constr->defval[i].adbin);
		}

		if (constr->missing)
		{
			cpy->missing = (AttrMissing *) palloc(tupdesc->natts * sizeof(AttrMissing));
			memcpy(cpy->missing, constr->missing, tupdesc->natts * sizeof(AttrMissing));
			for (i = tupdesc->natts - 1; i >= 0; i--)
			{
				if (constr->missing[i].am_present)
				{
					CompactAttribute *attr = TupleDescCompactAttr(tupdesc, i);

					cpy->missing[i].am_value = datumCopy(constr->missing[i].am_value,
														 attr->attbyval,
														 attr->attlen);
				}
			}
		}

		if ((cpy->num_check = constr->num_check) > 0)
		{
			cpy->check = (ConstrCheck *) palloc(cpy->num_check * sizeof(ConstrCheck));
			memcpy(cpy->check, constr->check, cpy->num_check * sizeof(ConstrCheck));
			for (i = cpy->num_check - 1; i >= 0; i--)
			{
				cpy->check[i].ccname = pstrdup(constr->check[i].ccname);
				cpy->check[i].ccbin = pstrdup(constr->check[i].ccbin);
				cpy->check[i].ccenforced = constr->check[i].ccenforced;
				cpy->check[i].ccvalid = constr->check[i].ccvalid;
				cpy->check[i].ccnoinherit = constr->check[i].ccnoinherit;
			}
		}

		desc->constr = cpy;
	}

	/* 我们也可以复制元组类型标识 */
	desc->tdtypeid = tupdesc->tdtypeid;
	desc->tdtypmod = tupdesc->tdtypmod;

	return desc;
}

/*
 * TupleDescCopy
 *		将元组描述符复制到调用方提供的内存中。
 *		该内存可以是映射到任意地址的共享内存，且必须足以容纳
 *		TupleDescSize(src) 字节。
 *
 * !!! 约束和默认值不会被复制 !!!
 */
void
TupleDescCopy(TupleDesc dst, TupleDesc src)
{
	int			i;

	/* 扁平复制头部和属性数组 */
	memcpy(dst, src, TupleDescSize(src));

	/*
	 * 由于我们不会复制约束和默认值，因此清除与之相关的字段。
	 */
	for (i = 0; i < dst->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(dst, i);

		att->attnotnull = false;
		att->atthasdef = false;
		att->atthasmissing = false;
		att->attidentity = '\0';
		att->attgenerated = '\0';

		populate_compact_attribute(dst, i);
	}
	dst->constr = NULL;

	/*
	 * 此外，假定目标描述符不进行引用计数。（无论如何，复制源端的
	 * 引用计数值都是错误的。）
	 */
	dst->tdrefcount = -1;
}

/*
 * TupleDescCopyEntry
 *		本函数将单个属性结构从一个元组描述符复制到另一个。
 *
 * !!! 约束和默认值不会被复制 !!!
 */
void
TupleDescCopyEntry(TupleDesc dst, AttrNumber dstAttno,
				   TupleDesc src, AttrNumber srcAttno)
{
	Form_pg_attribute dstAtt = TupleDescAttr(dst, dstAttno - 1);
	Form_pg_attribute srcAtt = TupleDescAttr(src, srcAttno - 1);

	/*
	 * 合理性检查
	 */
	Assert(PointerIsValid(src));
	Assert(PointerIsValid(dst));
	Assert(srcAttno >= 1);
	Assert(srcAttno <= src->natts);
	Assert(dstAttno >= 1);
	Assert(dstAttno <= dst->natts);

	memcpy(dstAtt, srcAtt, ATTRIBUTE_FIXED_PART_SIZE);

	dstAtt->attnum = dstAttno;

	/* 由于我们不会复制约束和默认值，因此清除这些字段 */
	dstAtt->attnotnull = false;
	dstAtt->atthasdef = false;
	dstAtt->atthasmissing = false;
	dstAtt->attidentity = '\0';
	dstAtt->attgenerated = '\0';

	populate_compact_attribute(dst, dstAttno - 1);
}

/*
 * 释放一个 TupleDesc 及其所有子结构
 */
void
FreeTupleDesc(TupleDesc tupdesc)
{
	int			i;

	/*
	 * 或许此处应断言 tdrefcount == 0，以禁止对未引用计数的 tupdesc
	 * 进行显式释放？
	 */
	Assert(tupdesc->tdrefcount <= 0);

	if (tupdesc->constr)
	{
		if (tupdesc->constr->num_defval > 0)
		{
			AttrDefault *attrdef = tupdesc->constr->defval;

			for (i = tupdesc->constr->num_defval - 1; i >= 0; i--)
				pfree(attrdef[i].adbin);
			pfree(attrdef);
		}
		if (tupdesc->constr->missing)
		{
			AttrMissing *attrmiss = tupdesc->constr->missing;

			for (i = tupdesc->natts - 1; i >= 0; i--)
			{
				if (attrmiss[i].am_present
					&& !TupleDescAttr(tupdesc, i)->attbyval)
					pfree(DatumGetPointer(attrmiss[i].am_value));
			}
			pfree(attrmiss);
		}
		if (tupdesc->constr->num_check > 0)
		{
			ConstrCheck *check = tupdesc->constr->check;

			for (i = tupdesc->constr->num_check - 1; i >= 0; i--)
			{
				pfree(check[i].ccname);
				pfree(check[i].ccbin);
			}
			pfree(check);
		}
		pfree(tupdesc->constr);
	}

	pfree(tupdesc);
}

/*
 * 增加 tupdesc 的引用计数，并将该引用记录到 CurrentResourceOwner 中。
 *
 * 不要将其用于未进行引用计数的 tupdesc。（对状态不明的 tupdesc 请使用
 * PinTupleDesc 宏。）
 */
void
IncrTupleDescRefCount(TupleDesc tupdesc)
{
	Assert(tupdesc->tdrefcount >= 0);

	ResourceOwnerEnlarge(CurrentResourceOwner);
	tupdesc->tdrefcount++;
	ResourceOwnerRememberTupleDesc(CurrentResourceOwner, tupdesc);
}

/*
 * 减少 tupdesc 的引用计数，从 CurrentResourceOwner 中移除对应的引用，
 * 并在不再有引用剩余时释放该 tupdesc。
 *
 * 不要将其用于未进行引用计数的 tupdesc。（对状态不明的 tupdesc 请使用
 * ReleaseTupleDesc 宏。）
 */
void
DecrTupleDescRefCount(TupleDesc tupdesc)
{
	Assert(tupdesc->tdrefcount > 0);

	ResourceOwnerForgetTupleDesc(CurrentResourceOwner, tupdesc);
	if (--tupdesc->tdrefcount == 0)
		FreeTupleDesc(tupdesc);
}

/*
 * 比较两个 TupleDesc 结构在逻辑上是否相等
 */
bool
equalTupleDescs(TupleDesc tupdesc1, TupleDesc tupdesc2)
{
	int			i,
				n;

	if (tupdesc1->natts != tupdesc2->natts)
		return false;
	if (tupdesc1->tdtypeid != tupdesc2->tdtypeid)
		return false;

	/* 不检查 tdtypmod 和 tdrefcount */

	for (i = 0; i < tupdesc1->natts; i++)
	{
		Form_pg_attribute attr1 = TupleDescAttr(tupdesc1, i);
		Form_pg_attribute attr2 = TupleDescAttr(tupdesc2, i);

		/*
		 * 我们不需要在这里检查每一个字段：可以忽略 attrelid 和 attnum
		 * （它们仅用于将行放入 attrs 数组中）。看起来我们似乎可以省去
		 * 对 attlen/attbyval/attalign 的检查，因为它们是由 atttypid 派生
		 * 出来的；但对于被删除的列，我们必须检查它们（因为所有被删除
		 * 列的 atttypid 都为零），而且一般来说始终检查它们似乎更安全。
		 *
		 * 我们有意忽略 atthasmissing，因为它在 tupdesc 中并不十分重要，
		 * tupdesc 中并不包含 attmissingval 字段。
		 */
		if (strcmp(NameStr(attr1->attname), NameStr(attr2->attname)) != 0)
			return false;
		if (attr1->atttypid != attr2->atttypid)
			return false;
		if (attr1->attlen != attr2->attlen)
			return false;
		if (attr1->attndims != attr2->attndims)
			return false;
		if (attr1->atttypmod != attr2->atttypmod)
			return false;
		if (attr1->attbyval != attr2->attbyval)
			return false;
		if (attr1->attalign != attr2->attalign)
			return false;
		if (attr1->attstorage != attr2->attstorage)
			return false;
		if (attr1->attcompression != attr2->attcompression)
			return false;
		if (attr1->attnotnull != attr2->attnotnull)
			return false;

		/*
		 * 当列带有 not-null 约束时，我们还需要考虑其有效性方面，这一点
		 * 仅体现在 CompactAttribute->attnullability 中，因此要对其进行校验。
		 */
		if (attr1->attnotnull)
		{
			CompactAttribute *cattr1 = TupleDescCompactAttr(tupdesc1, i);
			CompactAttribute *cattr2 = TupleDescCompactAttr(tupdesc2, i);

			Assert(cattr1->attnullability != ATTNULLABLE_UNKNOWN);
			Assert((cattr1->attnullability == ATTNULLABLE_UNKNOWN) ==
				   (cattr2->attnullability == ATTNULLABLE_UNKNOWN));

			if (cattr1->attnullability != cattr2->attnullability)
				return false;
		}
		if (attr1->atthasdef != attr2->atthasdef)
			return false;
		if (attr1->attidentity != attr2->attidentity)
			return false;
		if (attr1->attgenerated != attr2->attgenerated)
			return false;
		if (attr1->attisdropped != attr2->attisdropped)
			return false;
		if (attr1->attislocal != attr2->attislocal)
			return false;
		if (attr1->attinhcount != attr2->attinhcount)
			return false;
		if (attr1->attcollation != attr2->attcollation)
			return false;
		/* 变长字段根本不存在…… */
	}

	if (tupdesc1->constr != NULL)
	{
		TupleConstr *constr1 = tupdesc1->constr;
		TupleConstr *constr2 = tupdesc2->constr;

		if (constr2 == NULL)
			return false;
		if (constr1->has_not_null != constr2->has_not_null)
			return false;
		if (constr1->has_generated_stored != constr2->has_generated_stored)
			return false;
		if (constr1->has_generated_virtual != constr2->has_generated_virtual)
			return false;
		n = constr1->num_defval;
		if (n != (int) constr2->num_defval)
			return false;
		/* 此处我们假设两个 AttrDefault 数组都按 adnum 顺序排列 */
		for (i = 0; i < n; i++)
		{
			AttrDefault *defval1 = constr1->defval + i;
			AttrDefault *defval2 = constr2->defval + i;

			if (defval1->adnum != defval2->adnum)
				return false;
			if (strcmp(defval1->adbin, defval2->adbin) != 0)
				return false;
		}
		if (constr1->missing)
		{
			if (!constr2->missing)
				return false;
			for (i = 0; i < tupdesc1->natts; i++)
			{
				AttrMissing *missval1 = constr1->missing + i;
				AttrMissing *missval2 = constr2->missing + i;

				if (missval1->am_present != missval2->am_present)
					return false;
				if (missval1->am_present)
				{
					CompactAttribute *missatt1 = TupleDescCompactAttr(tupdesc1, i);

					if (!datumIsEqual(missval1->am_value, missval2->am_value,
									  missatt1->attbyval, missatt1->attlen))
						return false;
				}
			}
		}
		else if (constr2->missing)
			return false;
		n = constr1->num_check;
		if (n != (int) constr2->num_check)
			return false;

		/*
		 * 类似地，这里依赖 ConstrCheck 条目按名称排序。如果存在重复的名称，
		 * 比较结果将不确定，但这种情况不应发生。
		 */
		for (i = 0; i < n; i++)
		{
			ConstrCheck *check1 = constr1->check + i;
			ConstrCheck *check2 = constr2->check + i;

			if (!(strcmp(check1->ccname, check2->ccname) == 0 &&
				  strcmp(check1->ccbin, check2->ccbin) == 0 &&
				  check1->ccenforced == check2->ccenforced &&
				  check1->ccvalid == check2->ccvalid &&
				  check1->ccnoinherit == check2->ccnoinherit))
				return false;
		}
	}
	else if (tupdesc2->constr != NULL)
		return false;
	return true;
}

/*
 * equalRowTypes
 *
 * 本函数判断两个元组描述符是否具有相等的行类型。它仅检查 pg_attribute 中
 * 适用于行类型的那些字段，而忽略那些定义物理行存储或表列元数据的字段。
 *
 * 具体来说，它检查：
 *
 * - 属性数量相同
 * - 组合类型 ID 相同（但两者也可能都为零）
 * - 对应的属性（按顺序）具有相同的名称、类型、typmod 和排序规则
 *
 * 本函数用于判断两个记录类型是否兼容、函数返回的行类型是否相同，以及
 * 其他类似场景。
 *
 * （XXX 关于这里是否应检查 attndims 曾有讨论，但目前决定不检查。）
 *
 * 注意：我们有意不检查 tdtypmod 字段。这样 typcache.c 就能使用本例程
 * 来判断一个已缓存的记录类型是否匹配所请求的类型。
 */
bool
equalRowTypes(TupleDesc tupdesc1, TupleDesc tupdesc2)
{
	if (tupdesc1->natts != tupdesc2->natts)
		return false;
	if (tupdesc1->tdtypeid != tupdesc2->tdtypeid)
		return false;

	for (int i = 0; i < tupdesc1->natts; i++)
	{
		Form_pg_attribute attr1 = TupleDescAttr(tupdesc1, i);
		Form_pg_attribute attr2 = TupleDescAttr(tupdesc2, i);

		if (strcmp(NameStr(attr1->attname), NameStr(attr2->attname)) != 0)
			return false;
		if (attr1->atttypid != attr2->atttypid)
			return false;
		if (attr1->atttypmod != attr2->atttypmod)
			return false;
		if (attr1->attcollation != attr2->attcollation)
			return false;

		/* 从表派生的记录类型可能包含被删除的字段。 */
		if (attr1->attisdropped != attr2->attisdropped)
			return false;
	}

	return true;
}

/*
 * hashRowType
 *
 * 如果两个元组描述符会被 equalRowTypes() 判定为相等，那么根据本函数
 * 它们的哈希值也将相等。
 */
uint32
hashRowType(TupleDesc desc)
{
	uint32		s;
	int			i;

	s = hash_combine(0, hash_uint32(desc->natts));
	s = hash_combine(s, hash_uint32(desc->tdtypeid));
	for (i = 0; i < desc->natts; ++i)
		s = hash_combine(s, hash_uint32(TupleDescAttr(desc, i)->atttypid));

	return s;
}

/*
 * TupleDescInitEntry
 *		本函数在先前已分配的元组描述符中初始化单个属性结构。
 *
 * 如果 attributeName 为 NULL，attname 字段会被设为空字符串
 * （这适用于我们不知道或不需要该字段名称的场景）。此外，一些调用方
 * 使用本函数来修改已有 tupdesc 中与数据类型相关的字段；它们传入
 * attributeName = NameStr(att->attname) 以表示不应修改 attname 字段。
 *
 * 注意，attcollation 会被设为指定数据类型的默认值。如果需要非默认的
 * 排序规则，请在之后使用 TupleDescInitEntryCollation 插入。
 */
void
TupleDescInitEntry(TupleDesc desc,
				   AttrNumber attributeNumber,
				   const char *attributeName,
				   Oid oidtypeid,
				   int32 typmod,
				   int attdim)
{
	HeapTuple	tuple;
	Form_pg_type typeForm;
	Form_pg_attribute att;

	/*
	 * 合理性检查
	 */
	Assert(PointerIsValid(desc));
	Assert(attributeNumber >= 1);
	Assert(attributeNumber <= desc->natts);
	Assert(attdim >= 0);
	Assert(attdim <= PG_INT16_MAX);

	/*
	 * 初始化属性字段
	 */
	att = TupleDescAttr(desc, attributeNumber - 1);

	att->attrelid = 0;			/* 虚拟值 */

	/*
	 * 注意：attributeName 可以为 NULL，因为规划器并不总是在目标列表中
	 * 填入有效的 resname 值，尤其是针对 resjunk 属性。此外，如果调用方
	 * 想要复用旧的 attname，则不做任何改动。
	 */
	if (attributeName == NULL)
		MemSet(NameStr(att->attname), 0, NAMEDATALEN);
	else if (attributeName != NameStr(att->attname))
		namestrcpy(&(att->attname), attributeName);

	att->atttypmod = typmod;

	att->attnum = attributeNumber;
	att->attndims = attdim;

	att->attnotnull = false;
	att->atthasdef = false;
	att->atthasmissing = false;
	att->attidentity = '\0';
	att->attgenerated = '\0';
	att->attisdropped = false;
	att->attislocal = true;
	att->attinhcount = 0;
	/* 变长字段在 tupdesc 中并不存在 */

	tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(oidtypeid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for type %u", oidtypeid);
	typeForm = (Form_pg_type) GETSTRUCT(tuple);

	att->atttypid = oidtypeid;
	att->attlen = typeForm->typlen;
	att->attbyval = typeForm->typbyval;
	att->attalign = typeForm->typalign;
	att->attstorage = typeForm->typstorage;
	att->attcompression = InvalidCompressionMethod;
	att->attcollation = typeForm->typcollation;

	populate_compact_attribute(desc, attributeNumber - 1);

	ReleaseSysCache(tuple);
}

/*
 * TupleDescInitBuiltinEntry
 *		在无需访问目录的情况下初始化元组描述符。仅支持有限范围的
 *		内建类型。
 */
void
TupleDescInitBuiltinEntry(TupleDesc desc,
						  AttrNumber attributeNumber,
						  const char *attributeName,
						  Oid oidtypeid,
						  int32 typmod,
						  int attdim)
{
	Form_pg_attribute att;

	/* 合理性检查 */
	Assert(PointerIsValid(desc));
	Assert(attributeNumber >= 1);
	Assert(attributeNumber <= desc->natts);
	Assert(attdim >= 0);
	Assert(attdim <= PG_INT16_MAX);

	/* 初始化属性字段 */
	att = TupleDescAttr(desc, attributeNumber - 1);
	att->attrelid = 0;			/* 虚拟值 */

	/* 与 TupleDescInitEntry 不同，这里要求提供属性名称 */
	Assert(attributeName != NULL);
	namestrcpy(&(att->attname), attributeName);

	att->atttypmod = typmod;

	att->attnum = attributeNumber;
	att->attndims = attdim;

	att->attnotnull = false;
	att->atthasdef = false;
	att->atthasmissing = false;
	att->attidentity = '\0';
	att->attgenerated = '\0';
	att->attisdropped = false;
	att->attislocal = true;
	att->attinhcount = 0;
	/* 变长字段在 tupdesc 中并不存在 */

	att->atttypid = oidtypeid;

	/*
	 * 我们这里的目的是支持足够少的类型，使基本的内建命令无需访问目录
	 * 也能工作——例如，这样即使在不连接数据库的后端进程中也能执行
	 * 某些操作。
	 */
	switch (oidtypeid)
	{
		case TEXTOID:
		case TEXTARRAYOID:
			att->attlen = -1;
			att->attbyval = false;
			att->attalign = TYPALIGN_INT;
			att->attstorage = TYPSTORAGE_EXTENDED;
			att->attcompression = InvalidCompressionMethod;
			att->attcollation = DEFAULT_COLLATION_OID;
			break;

		case BOOLOID:
			att->attlen = 1;
			att->attbyval = true;
			att->attalign = TYPALIGN_CHAR;
			att->attstorage = TYPSTORAGE_PLAIN;
			att->attcompression = InvalidCompressionMethod;
			att->attcollation = InvalidOid;
			break;

		case INT4OID:
			att->attlen = 4;
			att->attbyval = true;
			att->attalign = TYPALIGN_INT;
			att->attstorage = TYPSTORAGE_PLAIN;
			att->attcompression = InvalidCompressionMethod;
			att->attcollation = InvalidOid;
			break;

		case INT8OID:
			att->attlen = 8;
			att->attbyval = FLOAT8PASSBYVAL;
			att->attalign = TYPALIGN_DOUBLE;
			att->attstorage = TYPSTORAGE_PLAIN;
			att->attcompression = InvalidCompressionMethod;
			att->attcollation = InvalidOid;
			break;

		case OIDOID:
			att->attlen = 4;
			att->attbyval = true;
			att->attalign = TYPALIGN_INT;
			att->attstorage = TYPSTORAGE_PLAIN;
			att->attcompression = InvalidCompressionMethod;
			att->attcollation = InvalidOid;
			break;

		default:
			elog(ERROR, "unsupported type %u", oidtypeid);
	}

	populate_compact_attribute(desc, attributeNumber - 1);
}

/*
 * TupleDescInitEntryCollation
 *
 * 为先前已初始化的元组描述符条目指定一个非默认的排序规则。
 */
void
TupleDescInitEntryCollation(TupleDesc desc,
							AttrNumber attributeNumber,
							Oid collationid)
{
	/*
	 * 合理性检查
	 */
	Assert(PointerIsValid(desc));
	Assert(attributeNumber >= 1);
	Assert(attributeNumber <= desc->natts);

	TupleDescAttr(desc, attributeNumber - 1)->attcollation = collationid;
}

/*
 * BuildDescFromLists
 *
 * 根据给定的列名列表（String 节点形式）、列类型 OID 列表、typmod 列表
 * 以及排序规则 OID 列表构建一个 TupleDesc。
 *
 * 不会生成任何约束。
 *
 * 本函数用于返回 RECORD 的函数。
 */
TupleDesc
BuildDescFromLists(const List *names, const List *types, const List *typmods, const List *collations)
{
	int			natts;
	AttrNumber	attnum;
	ListCell   *l1;
	ListCell   *l2;
	ListCell   *l3;
	ListCell   *l4;
	TupleDesc	desc;

	natts = list_length(names);
	Assert(natts == list_length(types));
	Assert(natts == list_length(typmods));
	Assert(natts == list_length(collations));

	/*
	 * 分配一个新的元组描述符
	 */
	desc = CreateTemplateTupleDesc(natts);

	attnum = 0;
	forfour(l1, names, l2, types, l3, typmods, l4, collations)
	{
		char	   *attname = strVal(lfirst(l1));
		Oid			atttypid = lfirst_oid(l2);
		int32		atttypmod = lfirst_int(l3);
		Oid			attcollation = lfirst_oid(l4);

		attnum++;

		TupleDescInitEntry(desc, attnum, attname, atttypid, atttypmod, 0);
		TupleDescInitEntryCollation(desc, attnum, attcollation);
	}

	return desc;
}

/*
 * 获取给定属性编号的默认表达式（若无则为 NULL）。
 */
Node *
TupleDescGetDefault(TupleDesc tupdesc, AttrNumber attnum)
{
	Node	   *result = NULL;

	if (tupdesc->constr)
	{
		AttrDefault *attrdef = tupdesc->constr->defval;

		for (int i = 0; i < tupdesc->constr->num_defval; i++)
		{
			if (attrdef[i].adnum == attnum)
			{
				result = stringToNode(attrdef[i].adbin);
				break;
			}
		}
	}

	return result;
}

/* ResourceOwner 回调 */
static void
ResOwnerReleaseTupleDesc(Datum res)
{
	TupleDesc	tupdesc = (TupleDesc) DatumGetPointer(res);

	/* 类似于 DecrTupleDescRefCount，但不调用 ResourceOwnerForget() */
	Assert(tupdesc->tdrefcount > 0);
	if (--tupdesc->tdrefcount == 0)
		FreeTupleDesc(tupdesc);
}

static char *
ResOwnerPrintTupleDesc(Datum res)
{
	TupleDesc	tupdesc = (TupleDesc) DatumGetPointer(res);

	return psprintf("TupleDesc %p (%u,%d)",
					tupdesc, tupdesc->tdtypeid, tupdesc->tdtypmod);
}
