/*-------------------------------------------------------------------------
 *
 * scankey.c
 *	  扫描键（scan key）支持代码
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/scankey.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/skey.h"
#include "catalog/pg_collation.h"


/*
 * ScanKeyEntryInitialize
 *		使用给定的全部字段值来初始化一个扫描键条目。
 *		目标过程由 OID 指定（但如果设置了 SK_SEARCHNULL 或
 *		SK_SEARCHNOTNULL，则可以为无效值）。
 *
 * 注意：调用时的 CurrentMemoryContext 应当与该 ScanKey 本身具有
 * 同样长的生命周期，因为 ScanKey 的 FmgrInfo 记录所附加的任何
 * 附属信息都将使用它。
 */
void
ScanKeyEntryInitialize(ScanKey entry,
					   int flags,
					   AttrNumber attributeNumber,
					   StrategyNumber strategy,
					   Oid subtype,
					   Oid collation,
					   RegProcedure procedure,
					   Datum argument)
{
	entry->sk_flags = flags;
	entry->sk_attno = attributeNumber;
	entry->sk_strategy = strategy;
	entry->sk_subtype = subtype;
	entry->sk_collation = collation;
	entry->sk_argument = argument;
	if (RegProcedureIsValid(procedure))
	{
		fmgr_info(procedure, &entry->sk_func);
	}
	else
	{
		Assert(flags & (SK_SEARCHNULL | SK_SEARCHNOTNULL));
		MemSet(&entry->sk_func, 0, sizeof(entry->sk_func));
	}
}

/*
 * ScanKeyInit
 *		ScanKeyEntryInitialize 的简写版本：flags 与 subtype 假定为零
 *		（即通常的取值），collation 则采用默认值。
 *
 * 这是在系统目录中进行硬编码查找时推荐的版本。它无法处理 NULL 参数、
 * 一元运算符或非默认运算符，但对于大多数硬编码查找而言，我们并不需要
 * 这些特性。
 *
 * 我们总是将 collation 设为 C_COLLATION_OID。这对于系统目录中所有
 * 支持排序规则的列来说都是正确的取值，而对于其他列类型则会被忽略，
 * 因此不值得去更精细地设置它。
 *
 * 注意：调用时的 CurrentMemoryContext 应当与该 ScanKey 本身具有
 * 同样长的生命周期，因为 ScanKey 的 FmgrInfo 记录所附加的任何
 * 附属信息都将使用它。
 */
void
ScanKeyInit(ScanKey entry,
			AttrNumber attributeNumber,
			StrategyNumber strategy,
			RegProcedure procedure,
			Datum argument)
{
	entry->sk_flags = 0;
	entry->sk_attno = attributeNumber;
	entry->sk_strategy = strategy;
	entry->sk_subtype = InvalidOid;
	entry->sk_collation = C_COLLATION_OID;
	entry->sk_argument = argument;
	fmgr_info(procedure, &entry->sk_func);
}

/*
 * ScanKeyEntryInitializeWithInfo
 *		使用已经完成的 FmgrInfo 函数查找记录来初始化一个扫描键条目。
 *
 * 注意：调用时的 CurrentMemoryContext 应当与该 ScanKey 本身具有
 * 同样长的生命周期，因为 ScanKey 的 FmgrInfo 记录所附加的任何
 * 附属信息都将使用它。
 */
void
ScanKeyEntryInitializeWithInfo(ScanKey entry,
							   int flags,
							   AttrNumber attributeNumber,
							   StrategyNumber strategy,
							   Oid subtype,
							   Oid collation,
							   FmgrInfo *finfo,
							   Datum argument)
{
	entry->sk_flags = flags;
	entry->sk_attno = attributeNumber;
	entry->sk_strategy = strategy;
	entry->sk_subtype = subtype;
	entry->sk_collation = collation;
	entry->sk_argument = argument;
	fmgr_info_copy(&entry->sk_func, finfo, CurrentMemoryContext);
}
