/*-------------------------------------------------------------------------
 *
 * skipsupport.c
 *	  B-Tree 跳跃扫描（skip scan）的支持例程。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/skipsupport.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/nbtree.h"
#include "utils/lsyscache.h"
#include "utils/skipsupport.h"

/*
 * 根据给定的操作符类（opfamily + opcintype）填充 SkipSupport。
 *
 * 成功时返回 skip support 结构体，分配在调用者的内存上下文中。
 * 否则返回 NULL，表示操作符类没有 skip support 函数。
 */
SkipSupport
PrepareSkipSupportFromOpclass(Oid opfamily, Oid opcintype, bool reverse)
{
	Oid			skipSupportFunction;
	SkipSupport sksup;

	/* 查找 skip support 函数 */
	skipSupportFunction = get_opfamily_proc(opfamily, opcintype, opcintype,
											BTSKIPSUPPORT_PROC);
	if (!OidIsValid(skipSupportFunction))
		return NULL;

	sksup = palloc(sizeof(SkipSupportData));
	OidFunctionCall1(skipSupportFunction, PointerGetDatum(sksup));

	if (reverse)
	{
		/*
		 * DESC/反向情形：交换 low_elem 与 high_elem，并交换 decrement
		 * 与 increment
		 */
		Datum		low_elem = sksup->low_elem;
		SkipSupportIncDec decrement = sksup->decrement;

		sksup->low_elem = sksup->high_elem;
		sksup->decrement = sksup->increment;

		sksup->high_elem = low_elem;
		sksup->increment = decrement;
	}

	return sksup;
}
