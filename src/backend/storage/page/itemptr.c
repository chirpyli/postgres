/*-------------------------------------------------------------------------
 *
 * itemptr.c
 *	  POSTGRES 磁盘项指针代码。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/page/itemptr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/itemptr.h"


/*
 * 我们确实希望 ItemPointerData 精确地为 6 字节。
 */
StaticAssertDecl(sizeof(ItemPointerData) == 3 * sizeof(uint16),
				 "ItemPointerData struct is improperly padded");

/*
 * ItemPointerEquals
 *	如果两个项指针指向同一个项则返回 true，
 *	 否则返回 false。
 *
 * 注意：
 *	断言两个磁盘项指针都是有效的！
 */
bool
ItemPointerEquals(ItemPointer pointer1, ItemPointer pointer2)
{
	if (ItemPointerGetBlockNumber(pointer1) ==
		ItemPointerGetBlockNumber(pointer2) &&
		ItemPointerGetOffsetNumber(pointer1) ==
		ItemPointerGetOffsetNumber(pointer2))
		return true;
	else
		return false;
}

/*
 * ItemPointerCompare
 *		项指针的通用 btree 风格比较。
 */
int32
ItemPointerCompare(ItemPointer arg1, ItemPointer arg2)
{
	/*
	 * 使用 ItemPointerGet{Offset,Block}NumberNoCheck 以避免断言
	 * ip_posid != 0，对于用户提供的 TID 来说，这个条件可能不成立。
	 */
	BlockNumber b1 = ItemPointerGetBlockNumberNoCheck(arg1);
	BlockNumber b2 = ItemPointerGetBlockNumberNoCheck(arg2);

	if (b1 < b2)
		return -1;
	else if (b1 > b2)
		return 1;
	else if (ItemPointerGetOffsetNumberNoCheck(arg1) <
			 ItemPointerGetOffsetNumberNoCheck(arg2))
		return -1;
	else if (ItemPointerGetOffsetNumberNoCheck(arg1) >
			 ItemPointerGetOffsetNumberNoCheck(arg2))
		return 1;
	else
		return 0;
}

/*
 * ItemPointerInc
 *		将 'pointer' 递增 1，仅关注 ItemPointer 类型自身的范围限制，
 *		而不考虑 MaxOffsetNumber 和 FirstOffsetNumber。
 *		这可能导致 'pointer' 变为 !OffsetNumberIsValid。
 *
 * 如果指针已经是 ItemPointer 类型范围所允许的最大可能值，
 * 则不做任何操作。
 */
void
ItemPointerInc(ItemPointer pointer)
{
	BlockNumber blk = ItemPointerGetBlockNumberNoCheck(pointer);
	OffsetNumber off = ItemPointerGetOffsetNumberNoCheck(pointer);

	if (off == PG_UINT16_MAX)
	{
		if (blk != InvalidBlockNumber)
		{
			off = 0;
			blk++;
		}
	}
	else
		off++;

	ItemPointerSet(pointer, blk, off);
}

/*
 * ItemPointerDec
 *		将 'pointer' 递减 1，仅关注 ItemPointer 类型自身的范围限制，
 *		而不考虑 MaxOffsetNumber 和 FirstOffsetNumber。
 *		这可能导致 'pointer' 变为 !OffsetNumberIsValid。
 *
 * 如果指针已经是 ItemPointer 类型范围所允许的最小可能值，
 * 则不做任何操作。这依赖于 FirstOffsetNumber 为 1 而非 0。
 */
void
ItemPointerDec(ItemPointer pointer)
{
	BlockNumber blk = ItemPointerGetBlockNumberNoCheck(pointer);
	OffsetNumber off = ItemPointerGetOffsetNumberNoCheck(pointer);

	if (off == 0)
	{
		if (blk != 0)
		{
			off = PG_UINT16_MAX;
			blk--;
		}
	}
	else
		off--;

	ItemPointerSet(pointer, blk, off);
}
