/*-------------------------------------------------------------------------
 *
 * indexfsm.c
 *	  POSTGRES free space map for quickly finding free pages in relations
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/freespace/indexfsm.c
 *
 *
 * NOTES:
 *
 *	这与 freespace.c 中用于堆的 FSM 类似，但不同之处在于：我们不跟踪
 *	页面上的空闲空间大小，而只跟踪页面是完全空闲还是在使用中。我们
 *	使用与堆相同的 FSM 实现，用 0 表示已使用的页面，用 (BLCKSZ - 1)
 *	表示未使用的页面。
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/freespace.h"
#include "storage/indexfsm.h"

/*
 * 导出例程
 */

/*
 * GetFreeIndexPage - 从 FSM 中返回一个空闲页面
 *
 * 作为副作用，该页面会在 FSM 中被标记为已使用。
 */
BlockNumber
GetFreeIndexPage(Relation rel)
{
	BlockNumber blkno = GetPageWithFreeSpace(rel, BLCKSZ / 2);

	if (blkno != InvalidBlockNumber)
		RecordUsedIndexPage(rel, blkno);

	return blkno;
}

/*
 * RecordFreeIndexPage - 在 FSM 中将一个页面标记为空闲
 */
void
RecordFreeIndexPage(Relation rel, BlockNumber freeBlock)
{
	RecordPageWithFreeSpace(rel, freeBlock, BLCKSZ - 1);
}


/*
 * RecordUsedIndexPage - 在 FSM 中将一个页面标记为已使用
 */
void
RecordUsedIndexPage(Relation rel, BlockNumber usedBlock)
{
	RecordPageWithFreeSpace(rel, usedBlock, 0);
}

/*
 * IndexFreeSpaceMapVacuum - 扫描并修复 FSM 中的任何不一致
 */
void
IndexFreeSpaceMapVacuum(Relation rel)
{
	FreeSpaceMapVacuum(rel);
}
