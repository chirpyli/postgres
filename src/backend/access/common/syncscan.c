/*-------------------------------------------------------------------------
 *
 * syncscan.c
 *		扫描同步支持
 *
 * 当多个后端在同一张表上执行顺序扫描时，我们会尝试让它们保持同步，
 * 以减少所需的总体 I/O。目标是每个页面只被读入共享缓冲区缓存一次，
 * 并让所有参与该共享扫描的后端在该页面从缓存中淘汰之前就处理完它。
 *
 * 由于在一组执行顺序扫描（seqscan）的后端中，"领导者（leader）"
 * 必须等待 I/O，而"跟随者（followers）"则不需要，因此一旦能让这些后端
 * 在同一时刻检查表中大致相同的部分，就会产生强烈的自同步效果。所以
 * 真正需要的，只是让一个新启动顺序扫描的后端在离其他后端正在读取的
 * 位置较近的地方开始。我们可以以循环的方式扫描表：从块 X 一直读到
 * 末尾，再从块 0 读到 X-1，从而确保我们访问了所有的行，同时仍能参与
 * 这个公共的扫描。
 *
 * 为了实现这一点，我们跟踪每张表的扫描位置，并让新的扫描在紧邻之前
 * 扫描所在的位置附近开始。我们不会在之后尝试做任何额外的同步来让这些
 * 扫描保持在一起；某些扫描的进度可能远慢于其他扫描，例如当结果需要
 * 通过缓慢的网络传输给客户端时，而我们希望这类查询不会拖慢其他查询。
 *
 * 在任意时刻，实际存在的、针对不同表的大型顺序扫描只能有少数几个。
 * 因此我们只需将扫描位置保存在一个小的 LRU 列表中，并在每次需要查找
 * 或更新一个扫描位置时遍历该列表。整个机制只应用于超过某个阈值大小的
 * 表（但这并不属于本模块的职责范围）。
 *
 * 接口例程
 *		ss_get_location		- 返回某个关系的当前扫描位置
 *		ss_report_location	- 更新当前扫描位置
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/common/syncscan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/syncscan.h"
#include "miscadmin.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/rel.h"


/* GUC 变量 */
#ifdef TRACE_SYNCSCAN
bool		trace_syncscan = false;
#endif


/*
 * LRU 列表的大小。
 *
 * 注意：代码假定 SYNC_SCAN_NELEM > 1。
 *
 * XXX: 什么值比较合适？它应当足够大，以容纳同时被扫描的大型表的
 * 最大数量。但该值越大，就意味着在开始一次新扫描时需要遍历更多的
 * LRU 列表项。
 */
#define SYNC_SCAN_NELEM 20

/*
 * 报告当前扫描位置之间的间隔，以页面为单位。
 *
 * 注意：它应当小于我们用于批量读取的环（ring）大小（参见
 * buffer/freelist.c）。否则，一个加入其他扫描的扫描可能会从已经不在
 * 缓冲区缓存中的页面开始。这一点有些模糊；无论如何都不能保证新扫描
 * 会在该页面离开缓冲区缓存之前就将其读入，而另一方面该页面极有可能
 * 仍然在操作系统缓存中。
 */
#define SYNC_SCAN_REPORT_INTERVAL (128 * 1024 / BLCKSZ)


/*
 * 扫描位置结构本质上是一个带有头指针和尾指针的双向链表 LRU，但被
 * 设计为在固定大小的共享内存中保存一个固定的最大数量的元素。
 */
typedef struct ss_scan_location_t
{
	RelFileLocator relfilelocator;	/* 一个关系的标识 */
	BlockNumber location;		/* 该关系中上次报告的扫描位置 */
} ss_scan_location_t;

typedef struct ss_lru_item_t
{
	struct ss_lru_item_t *prev;
	struct ss_lru_item_t *next;
	ss_scan_location_t location;
} ss_lru_item_t;

typedef struct ss_scan_locations_t
{
	ss_lru_item_t *head;
	ss_lru_item_t *tail;
	ss_lru_item_t items[FLEXIBLE_ARRAY_MEMBER]; /* SYNC_SCAN_NELEM 个元素 */
} ss_scan_locations_t;

#define SizeOfScanLocations(N) \
	(offsetof(ss_scan_locations_t, items) + (N) * sizeof(ss_lru_item_t))

/* 指向共享内存中结构体的指针 */
static ss_scan_locations_t *scan_locations;

/* 内部函数的原型声明 */
static BlockNumber ss_search(RelFileLocator relfilelocator,
							 BlockNumber location, bool set);


/*
 * SyncScanShmemSize --- 报告所需的共享内存空间大小
 */
Size
SyncScanShmemSize(void)
{
	return SizeOfScanLocations(SYNC_SCAN_NELEM);
}

/*
 * SyncScanShmemInit --- 初始化本模块的共享内存
 */
void
SyncScanShmemInit(void)
{
	int			i;
	bool		found;

	scan_locations = (ss_scan_locations_t *)
		ShmemInitStruct("Sync Scan Locations List",
						SizeOfScanLocations(SYNC_SCAN_NELEM),
						&found);

	if (!IsUnderPostmaster)
	{
		/* 初始化共享内存区域 */
		Assert(!found);

		scan_locations->head = &scan_locations->items[0];
		scan_locations->tail = &scan_locations->items[SYNC_SCAN_NELEM - 1];

		for (i = 0; i < SYNC_SCAN_NELEM; i++)
		{
			ss_lru_item_t *item = &scan_locations->items[i];

			/*
			 * 用无效值初始化所有的槽位。随着扫描被启动，这些无效条目
			 * 会从 LRU 列表上脱落，并被真实的条目所取代。
			 */
			item->location.relfilelocator.spcOid = InvalidOid;
			item->location.relfilelocator.dbOid = InvalidOid;
			item->location.relfilelocator.relNumber = InvalidRelFileNumber;
			item->location.location = InvalidBlockNumber;

			item->prev = (i > 0) ?
				(&scan_locations->items[i - 1]) : NULL;
			item->next = (i < SYNC_SCAN_NELEM - 1) ?
				(&scan_locations->items[i + 1]) : NULL;
		}
	}
	else
		Assert(found);
}

/*
 * ss_search --- 在 scan_locations 结构中查找一个带有给定
 *		relfilelocator 的条目。
 *
 * 如果 "set" 为真，则该位置会被更新为给定的位置。如果找不到带有
 * 给定 relfilelocator 的条目，则即使 "set" 为假，也会在列表头部用
 * 给定的位置创建一个条目。
 *
 * 在任何情况下，返回可能经过更新之后的位置。
 *
 * 调用方负责确保已经获取了针对该共享数据结构的适当锁。
 */
static BlockNumber
ss_search(RelFileLocator relfilelocator, BlockNumber location, bool set)
{
	ss_lru_item_t *item;

	item = scan_locations->head;
	for (;;)
	{
		bool		match;

		match = RelFileLocatorEquals(item->location.relfilelocator,
									 relfilelocator);

		if (match || item->next == NULL)
		{
			/*
			 * 如果我们到达了列表末尾且没有找到匹配项，则接管
			 * 最后一个条目
			 */
			if (!match)
			{
				item->location.relfilelocator = relfilelocator;
				item->location.location = location;
			}
			else if (set)
				item->location.location = location;

			/* 将该条目移动到 LRU 列表的前端 */
			if (item != scan_locations->head)
			{
				/* 解除链接 */
				if (item == scan_locations->tail)
					scan_locations->tail = item->prev;
				item->prev->next = item->next;
				if (item->next)
					item->next->prev = item->prev;

				/* 重新链接 */
				item->prev = NULL;
				item->next = scan_locations->head;
				scan_locations->head->prev = item;
				scan_locations->head = item;
			}

			return item->location.location;
		}

		item = item->next;
	}

	/* 不会到达此处 */
}

/*
 * ss_get_location --- 获取扫描的最优起始位置
 *
 * 返回该关系上一次顺序扫描上次报告的位置，如果找不到有效的位置
 * 则返回 0。
 *
 * 我们期望调用方刚刚执行过 RelationGetNumberOfBlocks()，因此该数值
 * 是被传入的，而无需再次计算。保证返回的结果小于 relnblocks（前提是
 * relnblocks > 0）。
 */
BlockNumber
ss_get_location(Relation rel, BlockNumber relnblocks)
{
	BlockNumber startloc;

	LWLockAcquire(SyncScanLock, LW_EXCLUSIVE);
	startloc = ss_search(rel->rd_locator, 0, false);
	LWLockRelease(SyncScanLock);

	/*
	 * 如果该位置对于本次扫描而言不是一个有效的块号，则从 0 开始。
	 *
	 * 例如，如果在保存该位置之后有 VACUUM 截断了表，就可能发生这种情况。
	 */
	if (startloc >= relnblocks)
		startloc = 0;

#ifdef TRACE_SYNCSCAN
	if (trace_syncscan)
		elog(LOG,
			 "SYNC_SCAN: start \"%s\" (size %u) at %u",
			 RelationGetRelationName(rel), relnblocks, startloc);
#endif

	return startloc;
}

/*
 * ss_report_location --- 更新当前扫描位置
 *
 * 将一个形如 (relfilelocator, blocknumber) 的条目写入共享的 Sync Scan
 * 状态中，覆盖同一 relfilelocator 已有的任何条目。
 */
void
ss_report_location(Relation rel, BlockNumber location)
{
#ifdef TRACE_SYNCSCAN
	if (trace_syncscan)
	{
		if ((location % 1024) == 0)
			elog(LOG,
				 "SYNC_SCAN: scanning \"%s\" at %u",
				 RelationGetRelationName(rel), location);
	}
#endif

	/*
	 * 为了减少锁竞争，只在每 N 个页面时报告一次扫描进度。出于同样的
	 * 原因，如果锁不能立即获取，也不要阻塞等待。漏掉几次更新并不要紧，
	 * 它只意味着一个想要加入该组扫描的新扫描会从扫描头部稍靠后的
	 * 位置开始。但愿这些页面仍然在操作系统缓存中，扫描能很快追上。
	 */
	if ((location % SYNC_SCAN_REPORT_INTERVAL) == 0)
	{
		if (LWLockConditionalAcquire(SyncScanLock, LW_EXCLUSIVE))
		{
			(void) ss_search(rel->rd_locator, location, true);
			LWLockRelease(SyncScanLock);
		}
#ifdef TRACE_SYNCSCAN
		else if (trace_syncscan)
			elog(LOG,
				 "SYNC_SCAN: missed update for \"%s\" at %u",
				 RelationGetRelationName(rel), location);
#endif
	}
}
