/*-------------------------------------------------------------------------
 *
 * itemid.h
 *	  POSTGRES 标准缓冲区页项标识符/行指针定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/itemid.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ITEMID_H
#define ITEMID_H

/*
 * 缓冲区页上的行指针。有关行指针使用方式的说明，请参见缓冲区页定义和注释。
 *
 * 在某些情况下，行指针处于"正在使用"状态但页面上没有关联的存储空间。
 * 按照惯例，任何没有存储空间的行指针的 lp_len 都设为 0，与其 lp_flags 状态无关。
 */
typedef struct ItemIdData
{
	unsigned	lp_off:15,		/* 到元组的偏移量（从页起始位置算起） */
				lp_flags:2,		/* 行指针状态，见下文 */
				lp_len:15;		/* 元组的字节长度 */
} ItemIdData;

typedef ItemIdData *ItemId;

/*
 * lp_flags 具有以下可能的取值。UNUSED 行指针可立即重用，其他状态则不可。
 */
#define LP_UNUSED		0		/* 未使用（始终应有 lp_len=0） */
#define LP_NORMAL		1		/* 已使用（始终应有 lp_len>0） */
#define LP_REDIRECT		2		/* HOT 重定向（应有 lp_len=0） */
#define LP_DEAD			3		/* 已死，可能有也可能没有存储空间 */

/*
 * 当项偏移量和长度不实际存储在 ItemIdData 中时，用这些类型表示它们。
 */
typedef uint16 ItemOffset;
typedef uint16 ItemLength;


/* ----------------
 *		辅助宏
 * ----------------
 */

/*
 *		ItemIdGetLength
 */
#define ItemIdGetLength(itemId) \
   ((itemId)->lp_len)

/*
 *		ItemIdGetOffset
 */
#define ItemIdGetOffset(itemId) \
   ((itemId)->lp_off)

/*
 *		ItemIdGetFlags
 */
#define ItemIdGetFlags(itemId) \
   ((itemId)->lp_flags)

/*
 *		ItemIdGetRedirect
 * 在 REDIRECT 指针中，lp_off 保存下一个行指针的偏移号
 */
#define ItemIdGetRedirect(itemId) \
   ((itemId)->lp_off)

/*
 * ItemIdIsValid
 *		项标识符有效时返回真。
 *		这是一个相当弱的测试，可能仅适用于断言。
 */
#define ItemIdIsValid(itemId)	PointerIsValid(itemId)

/*
 * ItemIdIsUsed
 *		项标识符处于使用状态时返回真。
 */
#define ItemIdIsUsed(itemId) \
	((itemId)->lp_flags != LP_UNUSED)

/*
 * ItemIdIsNormal
 *		项标识符处于 NORMAL 状态时返回真。
 */
#define ItemIdIsNormal(itemId) \
	((itemId)->lp_flags == LP_NORMAL)

/*
 * ItemIdIsRedirected
 *		项标识符处于 REDIRECT 状态时返回真。
 */
#define ItemIdIsRedirected(itemId) \
	((itemId)->lp_flags == LP_REDIRECT)

/*
 * ItemIdIsDead
 *		项标识符处于 DEAD 状态时返回真。
 */
#define ItemIdIsDead(itemId) \
	((itemId)->lp_flags == LP_DEAD)

/*
 * ItemIdHasStorage
 *		项标识符有关联存储空间时返回真。
 */
#define ItemIdHasStorage(itemId) \
	((itemId)->lp_len != 0)

/*
 * ItemIdSetUnused
 *		将项标识符设为 UNUSED，不带存储空间。
 *		注意避免对 itemId 的多次求值！
 */
#define ItemIdSetUnused(itemId) \
( \
	(itemId)->lp_flags = LP_UNUSED, \
	(itemId)->lp_off = 0, \
	(itemId)->lp_len = 0 \
)

/*
 * ItemIdSetNormal
 *		将项标识符设为 NORMAL，并指定存储空间。
 *		注意避免对 itemId 的多次求值！
 */
#define ItemIdSetNormal(itemId, off, len) \
( \
	(itemId)->lp_flags = LP_NORMAL, \
	(itemId)->lp_off = (off), \
	(itemId)->lp_len = (len) \
)

/*
 * ItemIdSetRedirect
 *		将项标识符设为 REDIRECT，并指定链接。
 *		注意避免对 itemId 的多次求值！
 */
#define ItemIdSetRedirect(itemId, link) \
( \
	(itemId)->lp_flags = LP_REDIRECT, \
	(itemId)->lp_off = (link), \
	(itemId)->lp_len = 0 \
)

/*
 * ItemIdSetDead
 *		将项标识符设为 DEAD，不带存储空间。
 *		注意避免对 itemId 的多次求值！
 */
#define ItemIdSetDead(itemId) \
( \
	(itemId)->lp_flags = LP_DEAD, \
	(itemId)->lp_off = 0, \
	(itemId)->lp_len = 0 \
)

/*
 * ItemIdMarkDead
 *		将项标识符设为 DEAD，但保留其现有存储空间。
 *
 * 注意：在索引中，这被当作提示位（hint-bit）机制使用；
 * 我们信任多个处理器可以并行执行此操作并得到相同的结果。
 */
#define ItemIdMarkDead(itemId) \
( \
	(itemId)->lp_flags = LP_DEAD \
)

#endif							/* ITEMID_H */
