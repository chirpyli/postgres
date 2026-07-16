/*-------------------------------------------------------------------------
 *
 * pmchild.c
 *	  用于跟踪 postmaster 子进程的函数。
 *
 * Postmaster 跟踪所有子进程，以便当某个进程退出时，它能知道该进程
 * 是什么类型，并据此进行清理。每个子进程都会从一个固定大小的
 * 结构体池（pool）中分配一个 PMChild 结构体。池的大小由配置允许
 * 多少个工作进程和后端连接的各种参数决定，即 autovacuum_worker_slots、
 * max_worker_processes、max_wal_senders 以及 max_connections。
 *
 * 死端（dead-end）后端的处理方式略有不同。死端后端的数量没有限制，
 * 并且它们不需要唯一的 ID，因此它们的 PMChild 结构体是动态分配的，
 * 而不是从池中获取。
 *
 * 本文件中的结构体和函数仅供 postmaster 进程私有使用。但需要注意，
 * 在共享内存中有一个由 pmsignal.c 管理的数组，与之保持对应（mirror）。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/pmchild.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "postmaster/autovacuum.h"
#include "postmaster/postmaster.h"
#include "replication/walsender.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"

/*
 * 针对不同类型子进程的空闲链表（freelist）。我们为每种类型维护独立的
 * 池，这样例如大量启动普通后端进程时，也不会妨碍 autovacuum 或
 * 辅助进程的启动。
 */
typedef struct PMChildPool
{
	int			size;			/* 为此类进程预留的 PMChild 槽位数量 */
	int			first_slotno;	/* 属于本池的第一个槽位编号 */
	dlist_head	freelist;		/* 当前未使用的 PMChild 条目 */
} PMChildPool;

static PMChildPool pmchild_pools[BACKEND_NUM_TYPES];
NON_EXEC_STATIC int num_pmchild_slots = 0;

/*
 * 活跃子进程列表。其中包含死端子进程。
 */
dlist_head	ActiveChildList;

/*
 * MaxLivePostmasterChildren
 *
 * 返回当前可以处于活跃状态的 postmaster 子进程数量。
 * 它包含除死端子进程以外的所有子进程。这样可以让共享内存中的
 * 数组（PMChildFlags）拥有固定的最大大小。
 */
int
MaxLivePostmasterChildren(void)
{
	if (num_pmchild_slots == 0)
		elog(ERROR, "PM child array not initialized yet");
	return num_pmchild_slots;
}

/*
 * 在 postmaster 启动时进行初始化
 *
 * 注意：在崩溃重启时不会调用本函数。我们依赖 PMChild 条目在重启
 * 过程中保持有效。这一点很重要，因为 syslogger 会存活于崩溃重启
 * 过程之中，因此我们不能使其 PMChild 槽位失效。
 */
void
InitPostmasterChildSlots(void)
{
	int			slotno;
	PMChild    *slots;

	/*
	 * 这里允许的（潜在）连接数多于实际可拥有的后端数，因为有些连接
	 * 可能仍在认证中；它们可能认证失败，或者在认证周期完成前就有
	 * 既有后端退出。真正的 MaxConnections 限制会在新后端尝试加入
	 * PGPROC 数组时强制执行。
	 *
	 * WAL 发送者最初以普通后端的形式启动，因此它们共享同一个池。
	 */
	pmchild_pools[B_BACKEND].size = 2 * (MaxConnections + max_wal_senders);

	pmchild_pools[B_AUTOVAC_WORKER].size = autovacuum_worker_slots;
	pmchild_pools[B_BG_WORKER].size = max_worker_processes;
	pmchild_pools[B_IO_WORKER].size = MAX_IO_WORKERS;

	/*
	 * 这些进程每种同时只能运行一个。它们各自拥有仅包含一个条目的池。
	 */
	pmchild_pools[B_AUTOVAC_LAUNCHER].size = 1;
	pmchild_pools[B_SLOTSYNC_WORKER].size = 1;
	pmchild_pools[B_ARCHIVER].size = 1;
	pmchild_pools[B_BG_WRITER].size = 1;
	pmchild_pools[B_CHECKPOINTER].size = 1;
	pmchild_pools[B_STARTUP].size = 1;
	pmchild_pools[B_WAL_RECEIVER].size = 1;
	pmchild_pools[B_WAL_SUMMARIZER].size = 1;
	pmchild_pools[B_WAL_WRITER].size = 1;
	pmchild_pools[B_LOGGER].size = 1;

	/* 其余的 pmchild_pools 保持为零大小（未分配） */

	/* 统计槽位的总数 */
	num_pmchild_slots = 0;
	for (int i = 0; i < BACKEND_NUM_TYPES; i++)
		num_pmchild_slots += pmchild_pools[i].size;

	/* 初始化这些槽位 */
	slots = palloc(num_pmchild_slots * sizeof(PMChild));
	slotno = 0;
	for (int btype = 0; btype < BACKEND_NUM_TYPES; btype++)
	{
		pmchild_pools[btype].first_slotno = slotno + 1;
		dlist_init(&pmchild_pools[btype].freelist);

		for (int j = 0; j < pmchild_pools[btype].size; j++)
		{
			slots[slotno].pid = 0;
			slots[slotno].child_slot = slotno + 1;
			slots[slotno].bkend_type = B_INVALID;
			slots[slotno].rw = NULL;
			slots[slotno].bgworker_notify = false;
			dlist_push_tail(&pmchild_pools[btype].freelist, &slots[slotno].elem);
			slotno++;
		}
	}
	Assert(slotno == num_pmchild_slots);

	/* 初始化其他结构 */
	dlist_init(&ActiveChildList);
}

/*
 * 为指定类型的 postmaster 子进程分配一个 PMChild 条目。
 *
 * 该条目从对应类型的正确池中获取。
 *
 * 返回结构体中的 pmchild->child_slot 在所有活跃子进程中是唯一的。
 */
PMChild *
AssignPostmasterChildSlot(BackendType btype)
{
	dlist_head *freelist;
	PMChild    *pmchild;

	if (pmchild_pools[btype].size == 0)
		elog(ERROR, "cannot allocate a PMChild slot for backend type %d", btype);

	freelist = &pmchild_pools[btype].freelist;
	if (dlist_is_empty(freelist))
		return NULL;

	pmchild = dlist_container(PMChild, elem, dlist_pop_head_node(freelist));
	pmchild->pid = 0;
	pmchild->bkend_type = btype;
	pmchild->rw = NULL;
	pmchild->bgworker_notify = true;

	/*
	 * 每个条目的 pmchild->child_slot 在槽位数组分配时就已经初始化。
	 * 这里做一个健全性检查。
	 */
	if (!(pmchild->child_slot >= pmchild_pools[btype].first_slotno &&
		  pmchild->child_slot < pmchild_pools[btype].first_slotno + pmchild_pools[btype].size))
	{
		elog(ERROR, "pmchild freelist for backend type %d is corrupt",
			 pmchild->bkend_type);
	}

	dlist_push_head(&ActiveChildList, &pmchild->elem);

	/* 更新共享内存数组中的状态 */
	MarkPostmasterChildSlotAssigned(pmchild->child_slot);

	elog(DEBUG2, "assigned pm child slot %d for %s",
		 pmchild->child_slot, PostmasterChildName(btype));

	return pmchild;
}

/*
 * 为死端后端分配一个 PMChild 结构体。死端子进程不会被分配
 * child_slot 编号。该结构体通过 palloc 分配；若内存不足则返回 NULL。
 */
PMChild *
AllocDeadEndChild(void)
{
	PMChild    *pmchild;

	elog(DEBUG2, "allocating dead-end child");

	pmchild = (PMChild *) palloc_extended(sizeof(PMChild), MCXT_ALLOC_NO_OOM);
	if (pmchild)
	{
		pmchild->pid = 0;
		pmchild->child_slot = 0;
		pmchild->bkend_type = B_DEAD_END_BACKEND;
		pmchild->rw = NULL;
		pmchild->bgworker_notify = false;

		dlist_push_head(&ActiveChildList, &pmchild->elem);
	}

	return pmchild;
}

/*
 * 在子进程退出后，释放其 PMChild 槽位。
 *
 * 如果子进程已干净地从共享内存中分离，则返回 true，否则返回 false
 * （参见 MarkPostmasterChildSlotUnassigned）。
 */
bool
ReleasePostmasterChildSlot(PMChild *pmchild)
{
	dlist_delete(&pmchild->elem);
	if (pmchild->bkend_type == B_DEAD_END_BACKEND)
	{
		elog(DEBUG2, "releasing dead-end backend");
		pfree(pmchild);
		return true;
	}
	else
	{
		PMChildPool *pool;

		elog(DEBUG2, "releasing pm child slot %d", pmchild->child_slot);

		/* WAL 发送者最初以普通后端的形式启动，并共享同一池 */
		if (pmchild->bkend_type == B_WAL_SENDER)
			pool = &pmchild_pools[B_BACKEND];
		else
			pool = &pmchild_pools[pmchild->bkend_type];

		/* 健全性检查：确保将条目归还到正确的池 */
		if (!(pmchild->child_slot >= pool->first_slotno &&
			  pmchild->child_slot < pool->first_slotno + pool->size))
		{
			elog(ERROR, "pmchild freelist for backend type %d is corrupt",
				 pmchild->bkend_type);
		}

		dlist_push_head(&pool->freelist, &pmchild->elem);
		return MarkPostmasterChildSlotUnassigned(pmchild->child_slot);
	}
}

/*
 * 通过 PID 查找正在运行的子进程的 PMChild 条目。
 */
PMChild *
FindPostmasterChildByPid(int pid)
{
	dlist_iter	iter;

	dlist_foreach(iter, &ActiveChildList)
	{
		PMChild    *bp = dlist_container(PMChild, elem, iter.cur);

		if (bp->pid == pid)
			return bp;
	}
	return NULL;
}
