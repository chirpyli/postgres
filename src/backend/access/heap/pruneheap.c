/*-------------------------------------------------------------------------
 *
 * pruneheap.c
 *	  堆页面剪枝（pruning）与 HOT 链管理代码
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/pruneheap.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/heapam_xlog.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "commands/vacuum.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/* heap_page_prune_and_freeze() 及其子例程使用的工作数据 */
typedef struct
{
	/*-------------------------------------------------------
	 * 传递给 heap_page_prune_and_freeze() 的参数
	 *-------------------------------------------------------
	 */

	/* 元组可见性测试，针对该关系初始化 */
	GlobalVisState *vistest;
	/* 剪枝过程中是否可将 dead 项设为 LP_UNUSED */
	bool		mark_unused_now;
	/* 是否尝试冻结元组 */
	bool		freeze;
	struct VacuumCutoffs *cutoffs;

	/*-------------------------------------------------------
	 * 描述需对页面执行的操作的字段
	 *-------------------------------------------------------
	 */
	TransactionId new_prune_xid;	/* 新的剪枝提示值 */
	TransactionId latest_xid_removed;
	int			nredirected;	/* 以下数组中的条目数量 */
	int			ndead;
	int			nunused;
	int			nfrozen;
	/* 累积待更改项索引的数组 */
	OffsetNumber redirected[MaxHeapTuplesPerPage * 2];
	OffsetNumber nowdead[MaxHeapTuplesPerPage];
	OffsetNumber nowunused[MaxHeapTuplesPerPage];
	HeapTupleFreeze frozen[MaxHeapTuplesPerPage];

	/*-------------------------------------------------------
	 * HOT 链处理的工作状态
	 *-------------------------------------------------------
	 */

	/*
	 * 'root_items' 包含全部 LP_REDIRECT 行指针以及普通非 HOT 元组的偏移量。
	 * 它们可以是独立项，也可以是 HOT 链中的第一个项。'heaponly_items' 包含
	 * 只能作为 HOT 链的一部分被移除的堆内（heap-only）元组。
	 */
	int			nroot_items;
	OffsetNumber root_items[MaxHeapTuplesPerPage];
	int			nheaponly_items;
	OffsetNumber heaponly_items[MaxHeapTuplesPerPage];

	/*
	 * processed[offnum] 为 true 表示位于 offnum 的项已经被处理过。
	 *
	 * 因为 FirstOffsetNumber 为 1，数组需要是 MaxHeapTuplesPerPage + 1 长，
	 * 否则每次访问都需要减 1。
	 */
	bool		processed[MaxHeapTuplesPerPage + 1];

	/*
	 * 出于正确性与效率的考虑，每个元组的可见性只计算一次；详见
	 * heap_page_prune_and_freeze() 中的说明。其类型为 int8[] 而非
	 * HTSV_Result[]，以便用 -1 表示尚未计算可见性（例如 LP_DEAD 项）。
	 *
	 * 因为 FirstOffsetNumber 为 1，数组需要是 MaxHeapTuplesPerPage + 1 长，
	 * 否则每次访问都需要减 1。
	 */
	int8		htsv[MaxHeapTuplesPerPage + 1];

	/*
	 * 与冻结相关的状态。
	 */
	HeapPageFreeze pagefrz;

	/*-------------------------------------------------------
	 * 关于已完成工作的信息
	 *
	 * These fields are not used by pruning itself for the most part, but are
	 * used to collect information about what was pruned and what state the
	 * page is in after pruning, for the benefit of the caller.  They are
	 * copied to the caller's PruneFreezeResult at the end.
	 * -------------------------------------------------------
	 */

	int			ndeleted;		/* 从页面中删除的元组数量 */

	/* 剪枝后存活与最近死亡的元组数量 */
	int			live_tuples;
	int			recently_dead_tuples;

	/* 该页面是否会让关系截断变得不安全 */
	bool		hastup;

	/*
	 * 剪枝完成后页面上的 LP_DEAD 项。包含已有的 LP_DEAD 项
	 */
	int			lpdead_items;	/* number of items in the array */
	OffsetNumber *deadoffsets;	/* points directly to presult->deadoffsets */

	/*
	 * all_visible 和 all_frozen 表示剪枝完成后，可见性映射（visibility map）
	 * 中该页面的全可见位和全冻结位是否可以被置位。
	 *
	 * visibility_cutoff_xid 是页面上存活元组的最新 xmin。调用方在设置 VM 位
	 * 时，可将其用作冲突边界（conflict horizon）。仅当我们冻结了某些元组且
	 * all_frozen 为 true 时，它才有效。
	 *
	 * 注意：all_visible 和 all_frozen 不包含 LP_DEAD 项。这对
	 * heap_page_prune_and_freeze() 来说很方便，可以用它们来决定是否冻结该
	 * 页面。返回给调用方的 all_visible 和 all_frozen 值会在最后被调整为包含
	 * LP_DEAD 项。
	 *
	 * 仅当 all_visible 也被置位时，all_frozen 才应被视为有效；我们不会在每次
	 * 清除 all_visible 标志时都去清除 all_frozen 标志。
	 */
	bool		all_visible;
	bool		all_frozen;
	TransactionId visibility_cutoff_xid;
} PruneState;

/* 本地函数 */
static HTSV_Result heap_prune_satisfies_vacuum(PruneState *prstate,
											   HeapTuple tup,
											   Buffer buffer);
static inline HTSV_Result htsv_get_valid_status(int status);
static void heap_prune_chain(Page page, BlockNumber blockno, OffsetNumber maxoff,
							 OffsetNumber rootoffnum, PruneState *prstate);
static void heap_prune_record_prunable(PruneState *prstate, TransactionId xid);
static void heap_prune_record_redirect(PruneState *prstate,
									   OffsetNumber offnum, OffsetNumber rdoffnum,
									   bool was_normal);
static void heap_prune_record_dead(PruneState *prstate, OffsetNumber offnum,
								   bool was_normal);
static void heap_prune_record_dead_or_unused(PruneState *prstate, OffsetNumber offnum,
											 bool was_normal);
static void heap_prune_record_unused(PruneState *prstate, OffsetNumber offnum, bool was_normal);

static void heap_prune_record_unchanged_lp_unused(Page page, PruneState *prstate, OffsetNumber offnum);
static void heap_prune_record_unchanged_lp_normal(Page page, PruneState *prstate, OffsetNumber offnum);
static void heap_prune_record_unchanged_lp_dead(Page page, PruneState *prstate, OffsetNumber offnum);
static void heap_prune_record_unchanged_lp_redirect(PruneState *prstate, OffsetNumber offnum);

static void page_verify_redirects(Page page);


/*
 * 对指定页面进行可选的剪枝，并修复碎片。
 *
 * 这是一个“顺手而为”的函数。只有当页面根据启发式判断看起来像是剪枝的候选，
 * 并且我们能在不阻塞的情况下获取 buffer cleanup 锁时，才会执行清理工作。
 *
 * 注意：该函数的调用非常频繁。如果剪枝没有任何意义，就必须尽快退出。
 *
 * 调用方必须持有该 buffer 的 pin，并且*不能*持有其上的锁。
 */
void
heap_page_prune_opt(Relation relation, Buffer buffer)
{
	Page		page = BufferGetPage(buffer);
	TransactionId prune_xid;
	GlobalVisState *vistest;
	Size		minfree;

	/*
	 * 在恢复（recovery）模式下无法写 WAL，因此尝试清理该页面没有意义。
	 * 主库很可能很快会发出一条清理用的 WAL 记录，所以这样做并没有什么损失。
	 */
	if (RecoveryInProgress())
		return;

	/*
	 * 先检查是否有可能存在需要剪枝的内容；如果没有 prune_xid（即没有
	 * 更新/删除操作留下潜在的 dead 元组），去确定合适的边界就是浪费。
	 */
	prune_xid = ((PageHeader) page)->pd_prune_xid;
	if (!TransactionIdIsValid(prune_xid))
		return;

	/*
	 * 检查 prune_xid 是否表明可能存在可被清理的 dead 行。
	 */
	vistest = GlobalVisTestFor(relation);

	if (!GlobalVisTestIsRemovableXid(vistest, prune_xid))
		return;

	/*
	 * 当先前的 UPDATE 未能在页面上为新的元组版本找到足够空间，或者空闲空间
	 * 低于关系的填充因子（fill-factor）目标（但不低于 10%）时，我们进行剪枝。
	 *
	 * 这里检查空闲空间是有疑问的，因为我们没有持有 buffer 上的任何锁；在最
	 * 坏情况下我们可能得到一个错误的答案。不过它不太可能*严重*错误，因为读取
	 * pd_lower 或 pd_upper 大概都是原子的。避免获取锁，似乎比在毕竟只是
	 * 启发式估计的情况下偶尔得到错误答案更为重要。
	 */
	minfree = RelationGetTargetPageFreeSpace(relation,
											 HEAP_DEFAULT_FILLFACTOR);
	minfree = Max(minfree, BLCKSZ / 10);

	if (PageIsFull(page) || PageGetHeapFreeSpace(page) < minfree)
	{
		/* 尝试获取独占的 buffer 锁 */
		if (!ConditionalLockBufferForCleanup(buffer))
			return;

		/*
		 * 既然我们已经持有了 buffer 锁，就获取关于页面空闲空间的准确信息，
		 * 并重新检查关于是否剪枝的启发式判断。
		 */
		if (PageIsFull(page) || PageGetHeapFreeSpace(page) < minfree)
		{
			OffsetNumber dummy_off_loc;
			PruneFreezeResult presult;

			/*
			 * 目前，无论关系是否有索引，都将 mark_unused_now 传为 false，因为
			 * 在当前实现下，我们无法在“访问时剪枝”（on-access pruning）期间
			 * 安全地确定这一点。
			 */
			heap_page_prune_and_freeze(relation, buffer, vistest, 0,
									   NULL, &presult, PRUNE_ON_ACCESS, &dummy_off_loc, NULL, NULL);

			/*
			 * 向 pgstats 报告回收到的元组数量。它等于 presult.ndeleted 减去
			 * 新被设为 LP_DEAD 的项的数量。
			 *
			 * 我们这样来推导 dead 元组的数量，是为了避免完全遗忘那些被设为
			 * LP_DEAD 的项，因为它们仍需由 VACUUM 来清理。在我们的报告中，
			 * 我们只想统计刚刚变成 LP_UNUSED 的堆内（heap-only）元组，而
			 * 那些 LP_DEAD 项不算在内。
			 *
			 * VACUUM 在跟踪 ndeleted 时不必以同样的方式进行补偿，因为它会
			 * 另行将相同的 LP_DEAD 项设为 LP_UNUSED。
			 */
			if (presult.ndeleted > presult.nnewlpdead)
				pgstat_update_heap_dead_tuples(relation,
											   presult.ndeleted - presult.nnewlpdead);
		}

		/* 释放 buffer 锁 */
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

		/*
		 * 我们选择在此时不更新 FSM，以避免页面上产生的空闲空间被无关的
		 * UPDATE/INSERT 复用。这些空闲空间应由对*本*页面的 UPDATE 来复用。
		 */
	}
}


/*
 * 对指定页面进行剪枝、修复碎片，并可能冻结元组。
 *
 * 调用方必须持有该页面的 pin 和 buffer cleanup 锁。注意我们不会代表调用方
 * 更新页面的 FSM 信息。调用方可能还需要考虑由于我们对行指针数组的截断而导致
 * 的数组长度缩减。
 *
 * 如果设置了 HEAP_PRUNE_FREEZE 选项，我们也会在需要时冻结元组，以便推进
 * relfrozenxid / relminmxid，或者在当前这样做被认为对整体系统性能有利时
 * 冻结。冻结时需要使用 'cutoffs'、'presult'、'new_relfrozen_xid' 和
 * 'new_relmin_mxid' 这几个参数。当设置了 HEAP_PRUNE_FREEZE 选项时，我们还会
 * 在退出时设置 presult->all_visible 和 presult->all_frozen，以指示是否可以
 * 设置 VM 位。当未设置 HEAP_PRUNE_FREEZE 选项时，它们总被设为 false，因为目前
 * 只有同时进行冻结的调用方才需要这些信息。
 *
 * vistest 用于区分元组是 DEAD 还是 RECENTLY_DEAD（见
 * heap_prune_satisfies_vacuum）。
 *
 * options:
 *   MARK_UNUSED_NOW 表示 dead 项可以在剪枝过程中被设为 LP_UNUSED。
 *
 *   FREEZE 表示我们同时会冻结元组，并向调用方返回 'all_visible'、'all_frozen'
 *   标志。
 *
 * cutoffs 包含由 VACUUM 在开始对关系进行清理时建立的冻结截止点。若设置了
 * HEAP_PRUNE_FREEZE 选项则为必需。cutoffs->OldestXmin 也用于判断 dead 元组是
 * HEAPTUPLE_RECENTLY_DEAD 还是 HEAPTUPLE_DEAD。
 *
 * presult 包含调用方所需的输出参数，例如被移除的元组数量以及剪枝完成后页面上
 * dead 项的偏移量。heap_page_prune_and_freeze() 负责对其进行初始化。所有调用方
 * 都需要提供。
 *
 * reason 表示执行剪枝的原因。它被包含在 WAL 记录中，用于调试与分析，除此之外
 * 没有别的作用。
 *
 * off_loc 是调用方在错误回调中需要用到的偏移位置。
 *
 * new_relfrozen_xid 和 new_relmin_mxid 在设置了 HEAP_PRUNE_FREEZE 选项时必须由
 * 调用方提供。在进入时，它们包含目前为止在关系上看到的最旧 XID 与多事务
 * XID。它们会被更新为剪枝完成后页面上存在的最旧值。在处理完整个关系后，
 * VACUUM 可以使用这些值作为该关系新的 relfrozenxid/relminmxid。
 */
void
heap_page_prune_and_freeze(Relation relation, Buffer buffer,
						   GlobalVisState *vistest,
						   int options,
						   struct VacuumCutoffs *cutoffs,
						   PruneFreezeResult *presult,
						   PruneReason reason,
						   OffsetNumber *off_loc,
						   TransactionId *new_relfrozen_xid,
						   MultiXactId *new_relmin_mxid)
{
	Page		page = BufferGetPage(buffer);
	BlockNumber blockno = BufferGetBlockNumber(buffer);
	OffsetNumber offnum,
				maxoff;
	PruneState	prstate;
	HeapTupleData tup;
	bool		do_freeze;
	bool		do_prune;
	bool		do_hint;
	bool		hint_bit_fpi;
	int64		fpi_before = pgWalUsage.wal_fpi;

	/* 将参数复制到 prstate */
	prstate.vistest = vistest;
	prstate.mark_unused_now = (options & HEAP_PAGE_PRUNE_MARK_UNUSED_NOW) != 0;
	prstate.freeze = (options & HEAP_PAGE_PRUNE_FREEZE) != 0;
	prstate.cutoffs = cutoffs;

	/*
	 * 我们的策略是：扫描页面，列出需要更改的项，然后在临界区内应用这些更改。
	 * 这样可以让尽可能多的逻辑脱离临界区，同时也能够保证 WAL 重放与正常情况
	 * 表现一致。
	 *
	 * 首先，将新的 pd_prune_xid 值初始化为零（表示没有可剪枝的元组）。如果
	 * 我们发现任何可能很快变得可剪枝的元组，就会把相关的最旧 XID 保存到
	 * new_prune_xid 中。同时初始化其余的工作状态。
	 */
	prstate.new_prune_xid = InvalidTransactionId;
	prstate.latest_xid_removed = InvalidTransactionId;
	prstate.nredirected = prstate.ndead = prstate.nunused = prstate.nfrozen = 0;
	prstate.nroot_items = 0;
	prstate.nheaponly_items = 0;

	/* 初始化页面冻结工作状态 */
	prstate.pagefrz.freeze_required = false;
	if (prstate.freeze)
	{
		Assert(new_relfrozen_xid && new_relmin_mxid);
		prstate.pagefrz.FreezePageRelfrozenXid = *new_relfrozen_xid;
		prstate.pagefrz.NoFreezePageRelfrozenXid = *new_relfrozen_xid;
		prstate.pagefrz.FreezePageRelminMxid = *new_relmin_mxid;
		prstate.pagefrz.NoFreezePageRelminMxid = *new_relmin_mxid;
	}
	else
	{
		Assert(new_relfrozen_xid == NULL && new_relmin_mxid == NULL);
		prstate.pagefrz.FreezePageRelminMxid = InvalidMultiXactId;
		prstate.pagefrz.NoFreezePageRelminMxid = InvalidMultiXactId;
		prstate.pagefrz.FreezePageRelfrozenXid = InvalidTransactionId;
		prstate.pagefrz.NoFreezePageRelfrozenXid = InvalidTransactionId;
	}

	prstate.ndeleted = 0;
	prstate.live_tuples = 0;
	prstate.recently_dead_tuples = 0;
	prstate.hastup = false;
	prstate.lpdead_items = 0;
	prstate.deadoffsets = presult->deadoffsets;

	/*
	 * 调用方在我们完成之后可能会更新 VM。我们可以跟踪页面在剪枝与冻结之后
	 * 是否会是全可见和全冻结的，以帮助调用方完成这项工作。
	 *
	 * 目前，只有 VACUUM 会设置 VM 位。为了节省开销，只有在调用方需要时才做
	 * 这个记账工作。目前，这与 HEAP_PAGE_PRUNE_FREEZE 绑定在一起，但如果你
	 * 想在不冻结的情况下更新 VM 位，或者在不设置 VM 位的情况下冻结，它也可以
	 * 是一个独立的标志。
	 *
	 * 除了告诉调用方是否可以设置 VM 位之外，我们也把 'all_visible' 和
	 * 'all_frozen' 用于我们自己的决策。如果整个页面将变为冻结状态，我们会考虑
	 * 顺手冻结元组。如果页面上存在对所有人不可见的元组，或者存在尚不可移除的
	 * dead 元组，我们就无法冻结整个页面。不过，那些会在清理结束时被移除的
	 * dead 元组，不应妨碍我们顺手进行冻结。因此，当看到 LP_DEAD 项时，我们
	 * 不会清除 all_visible。我们会在函数末尾把值返回给调用方时对此进行修正，
	 * 以免调用方错误地设置了 VM 位。
	 */
	if (prstate.freeze)
	{
		prstate.all_visible = true;
		prstate.all_frozen = true;
	}
	else
	{
		/*
		 * 初始化为 false 可以让我们跳过在
		 * heap_prune_record_unchanged_lp_normal() 中更新它们的工作。
		 */
		prstate.all_visible = false;
		prstate.all_frozen = false;
	}

	/*
	 * 可见性截止 xid 是页面上存活元组的最新 xmin。在通常情况下，它会被设为
	 * 冲突边界（conflict horizon），调用方可以用它来更新 VM。如果在冻结与
	 * 剪枝结束时页面是全冻结的，那么备库（standby）上任何正在运行的事务都
	 * 不可能不把页面上的元组视为全可见，因此冲突边界保持为
	 * InvalidTransactionId。
	 */
	prstate.visibility_cutoff_xid = InvalidTransactionId;

	maxoff = PageGetMaxOffsetNumber(page);
	tup.t_tableOid = RelationGetRelid(relation);

	/*
	 * 对所有元组确定 HTSV，并将它们排队，以待作为 HOT 链的根或堆内项来处理。
	 *
	 * 每个元组只确定一次 HTSV，这是为了保证正确性，以应对两次运行 HTSV
	 * 可能得到不同结果的情况。例如，如果另一个被检查的项导致
	 * GlobalVisTestIsRemovableFullXid() 更新了边界，RECENTLY_DEAD 可能变为
	 * DEAD；或者，如果插入事务中止，INSERT_IN_PROGRESS 可能变为 DEAD。
	 *
	 * 这对性能也有好处。最常见的情况是，页面内的元组存储在递减的偏移量处
	 * （而项存储在递增的偏移量处）。当处理一个页面上的所有元组时，这会导致
	 * 以递减的偏移量、且步长可变地读取页面内的内存。这对 CPU 预取器来说很难
	 * 处理。以逆序处理项（从而以递增顺序处理元组）能显著提高预取效率 /
	 * 减少缓存未命中次数。
	 */
	for (offnum = maxoff;
		 offnum >= FirstOffsetNumber;
		 offnum = OffsetNumberPrev(offnum))
	{
		ItemId		itemid = PageGetItemId(page, offnum);
		HeapTupleHeader htup;

		/*
		 * 设置偏移号，以便在处理此元组时发生的任何错误中一并显示它。
		 */
		*off_loc = offnum;

		prstate.processed[offnum] = false;
		prstate.htsv[offnum] = -1;

		/* 如果 slot 中不包含元组，无事可做 */
		if (!ItemIdIsUsed(itemid))
		{
			heap_prune_record_unchanged_lp_unused(page, &prstate, offnum);
			continue;
		}

		if (ItemIdIsDead(itemid))
		{
			/*
			 * 如果调用方将 mark_unused_now 设为 true，我们现在就可以把 dead
			 * 行指针设为 LP_UNUSED。
			 */
			if (unlikely(prstate.mark_unused_now))
				heap_prune_record_unused(&prstate, offnum, false);
			else
				heap_prune_record_unchanged_lp_dead(page, &prstate, offnum);
			continue;
		}

		if (ItemIdIsRedirected(itemid))
		{
			/* 这是一个 HOT 链的起点 */
			prstate.root_items[prstate.nroot_items++] = offnum;
			continue;
		}

		Assert(ItemIdIsNormal(itemid));

		/*
		 * 获取元组的可见性状态，并将其排队等待处理。
		 */
		htup = (HeapTupleHeader) PageGetItem(page, itemid);
		tup.t_data = htup;
		tup.t_len = ItemIdGetLength(itemid);
		ItemPointerSet(&tup.t_self, blockno, offnum);

		prstate.htsv[offnum] = heap_prune_satisfies_vacuum(&prstate, &tup,
														   buffer);

		if (!HeapTupleHeaderIsHeapOnly(htup))
			prstate.root_items[prstate.nroot_items++] = offnum;
		else
			prstate.heaponly_items[prstate.nheaponly_items++] = offnum;
	}

	/*
	 * 如果启用了校验和（checksums），heap_prune_satisfies_vacuum() 可能已经
	 * 导致发出了一个 FPI（全页镜像）。
	 */
	hint_bit_fpi = fpi_before != pgWalUsage.wal_fpi;

	/*
	 * 处理 HOT 链。
	 *
	 * 我们是从 'maxoff' 开始把项加入数组的，因此通过逆序处理该数组，我们就
	 * 以递增的偏移量顺序来处理这些项。这个顺序对正确性没有影响，但一些快速的
	 * 微基准测试表明这样更快。（早期的 PostgreSQL 版本不是使用 root_items
	 * 数组，而是扫描页面上的所有项，它们同样是以递增偏移量顺序进行的。）
	 */
	for (int i = prstate.nroot_items - 1; i >= 0; i--)
	{
		offnum = prstate.root_items[i];

		/* 忽略作为更早的链的一部分已经被处理过的项 */
		if (prstate.processed[offnum])
			continue;

		/* see preceding loop */
		*off_loc = offnum;

		/* 处理该项或该项组成的链 */
		heap_prune_chain(page, blockno, maxoff, offnum, &prstate);
	}

	/*
	 * 处理任何尚未作为 HOT 链的一部分被处理过的堆内元组。
	 */
	for (int i = prstate.nheaponly_items - 1; i >= 0; i--)
	{
		offnum = prstate.heaponly_items[i];

		if (prstate.processed[offnum])
			continue;

		/* see preceding loop */
		*off_loc = offnum;

		/*
		 * 如果元组是 DEAD 且不与任何其它元组形成链，就将其标记为 unused。
		 * （如果它确实成链，我们只能作为剪枝其所在链的一部分来移除它。）
		 *
		 * 我们主要需要这样做来处理被中止的 HOT 更新，即 XMIN_INVALID 的
		 * 堆内元组。这些元组可能不被任何链所链接，因为其父元组可能在任何
		 * 剪枝发生之前就被重新更新了。因此我们必须能够将它们与链剪枝分开
		 * 单独回收。（注意，HeapTupleHeaderIsHotUpdated 永远不会为
		 * XMIN_INVALID 元组返回 true，所以即使被中止的事务中存在连续的
		 * 更新，这段代码也能正常工作。）
		 */
		if (prstate.htsv[offnum] == HEAPTUPLE_DEAD)
		{
			ItemId		itemid = PageGetItemId(page, offnum);
			HeapTupleHeader htup = (HeapTupleHeader) PageGetItem(page, itemid);

			if (likely(!HeapTupleHeaderIsHotUpdated(htup)))
			{
				HeapTupleHeaderAdvanceConflictHorizon(htup,
													  &prstate.latest_xid_removed);
				heap_prune_record_unused(&prstate, offnum, true);
			}
			else
			{
				/*
				 * This tuple should've been processed and removed as part of
				 * a HOT chain, so something's wrong.  To preserve evidence,
				 * we don't dare to remove it.  We cannot leave behind a DEAD
				 * tuple either, because that will cause VACUUM to error out.
				 * Throwing an error with a distinct error message seems like
				 * the least bad option.
				 */
				elog(ERROR, "dead heap-only tuple (%u, %d) is not linked to from any HOT chain",
					 blockno, offnum);
			}
		}
		else
			heap_prune_record_unchanged_lp_normal(page, &prstate, offnum);
	}

	/* 我们现在应该已经对每个元组恰好处理过一次 */
#ifdef USE_ASSERT_CHECKING
	for (offnum = FirstOffsetNumber;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		*off_loc = offnum;

		Assert(prstate.processed[offnum]);
	}
#endif

	/* 处理完给定页面后，清除偏移信息。 */
	*off_loc = InvalidOffsetNumber;

	do_prune = prstate.nredirected > 0 ||
		prstate.ndead > 0 ||
		prstate.nunused > 0;

	/*
	 * 即使我们没有剪枝任何内容，如果我们为 pd_prune_xid 字段找到了新值，或者
	 * 页面被标记为已满，我们也会更新提示位（hint bit）。
	 */
	do_hint = ((PageHeader) page)->pd_prune_xid != prstate.new_prune_xid ||
		PageIsFull(page);

	/*
	 * 根据我们准备的冻结计划，决定是否要继续进行冻结。
	 */
	do_freeze = false;
	if (prstate.freeze)
	{
		if (prstate.pagefrz.freeze_required)
		{
			/*
			 * heap_prepare_freeze_tuple 指示存在至少一个来自 FreezeLimit/
			 * MultiXactCutoff 之前的 XID/MXID。必须冻结以推进
			 * relfrozenxid/relminmxid。
			 */
			do_freeze = true;
		}
		else
		{
			/*
			 * 如果我们反正要生成 FPI，并且这样做意味着我们可以在之后将页面设为
			 * 全冻结（可能要等到 VACUUM 的最后一次堆扫描才会发生），则顺手
			 * 冻结该页面。
			 *
			 * XXX：以前，我们通过对比剪枝前后的 pgWalUsage.wal_fpi 来判断剪枝
			 * 是否发出了 FPI。自从冻结与剪枝记录被合并后，这个启发式方法就
			 * 无法再使用了。顺手冻结的启发式方法必须改进；不过眼下，先尝试
			 * 近似还原旧的逻辑。
			 */
			if (prstate.all_visible && prstate.all_frozen && prstate.nfrozen > 0)
			{
				/*
				 * 冻结会使页面变为全冻结。是否已经发出了 FPI，或者反正会发出？
				 */
				if (RelationNeedsWAL(relation))
				{
					if (hint_bit_fpi)
						do_freeze = true;
					else if (do_prune)
					{
						if (XLogCheckBufferNeedsBackup(buffer))
							do_freeze = true;
					}
					else if (do_hint)
					{
						if (XLogHintBitIsNeeded() && XLogCheckBufferNeedsBackup(buffer))
							do_freeze = true;
					}
				}
			}
		}
	}

	if (do_freeze)
	{
		/*
		 * 在进入临界区之前，验证我们将要冻结的元组。
		 */
		heap_pre_freeze_checks(buffer, prstate.frozen, prstate.nfrozen);
	}
	else if (prstate.nfrozen > 0)
	{
		/*
		 * 页面上包含一些尚未冻结的元组，而我们选择现在不冻结它们。那样的话
		 * 页面就不会是全冻结的。
		 */
		Assert(!prstate.pagefrz.freeze_required);

		prstate.all_frozen = false;
		prstate.nfrozen = 0;	/* avoid miscounts in instrumentation */
	}
	else
	{
		/*
		 * 我们没有要执行的冻结计划。不过，页面可能已经是全冻结的了（也许仅在
		 * 剪枝之后）。这样的页面可以被我们的调用方在 VM 中标记为全冻结，即使
		 * 它没有任何元组是在此处新冻结的。
		 */
	}

	/* 应用更改时发生的任何错误都是致命的 */
	START_CRIT_SECTION();

	if (do_hint)
	{
		/*
		 * 将页面的 pd_prune_xid 字段更新为零，或者更新为任何即将可剪枝元组的
		 * 最旧 XID。
		 */
		((PageHeader) page)->pd_prune_xid = prstate.new_prune_xid;

		/*
		 * 同时清除“页面已满”标志，因为在页面发生其它变化之前，重复进行
		 * 剪枝/碎片整理过程没有意义。
		 */
		PageClearFull(page);

		/*
		 * 如果这就是我们要对该页面做的全部事情，那么这是一个不写 WAL 日志的
		 * 提示（hint）。如果我们打算冻结或剪枝该页面，我们会在下面把 buffer
		 * 标记为脏。
		 */
		if (!do_freeze && !do_prune)
			MarkBufferDirtyHint(buffer, true);
	}

	if (do_prune || do_freeze)
	{
		/* 应用计划中的项更改，并修复页面碎片。 */
		if (do_prune)
		{
			heap_page_prune_execute(buffer, false,
									prstate.redirected, prstate.nredirected,
									prstate.nowdead, prstate.ndead,
									prstate.nowunused, prstate.nunused);
		}

		if (do_freeze)
			heap_freeze_prepared_tuples(buffer, prstate.frozen, prstate.nfrozen);

		MarkBufferDirty(buffer);

		/*
		 * 发出一条 XLOG_HEAP2_PRUNE_FREEZE 的 WAL 记录，记录我们所做的工作
		 */
		if (RelationNeedsWAL(relation))
		{
			/*
			 * 整条记录的 snapshotConflictHorizon 应该取所有可能修改中计算出的
			 * 所有边界里最保守的一个。如果这条记录要剪枝元组，那么备库上任何
			 * 比本记录将剪枝掉的最新被移除元组的 youngest xmax 更老的事务都会
			 * 发生冲突。如果这条记录要冻结元组，那么备库上任何 xid 比本记录
			 * 将冻结的最新元组更老的事务都会发生冲突。
			 */
			TransactionId frz_conflict_horizon = InvalidTransactionId;
			TransactionId conflict_xid;

			/*
			 * 当整个页面在我们完成后有资格在 VM 中变为全冻结时，我们可以使用
			 * visibility_cutoff_xid 作为冲突的截止点。否则，我们通过从
			 * OldestXmin 回退一步来生成一个保守的截止点。
			 */
			if (do_freeze)
			{
				if (prstate.all_visible && prstate.all_frozen)
					frz_conflict_horizon = prstate.visibility_cutoff_xid;
				else
				{
					/* 在启用 hot_standby_feedback 时，避免误报冲突 */
					frz_conflict_horizon = prstate.cutoffs->OldestXmin;
					TransactionIdRetreat(frz_conflict_horizon);
				}
			}

			if (TransactionIdFollows(frz_conflict_horizon, prstate.latest_xid_removed))
				conflict_xid = frz_conflict_horizon;
			else
				conflict_xid = prstate.latest_xid_removed;

			log_heap_prune_and_freeze(relation, buffer,
									  conflict_xid,
									  true, reason,
									  prstate.frozen, prstate.nfrozen,
									  prstate.redirected, prstate.nredirected,
									  prstate.nowdead, prstate.ndead,
									  prstate.nowunused, prstate.nunused);
		}
	}

	END_CRIT_SECTION();

	/* Copy information back for caller */
	presult->ndeleted = prstate.ndeleted;
	presult->nnewlpdead = prstate.ndead;
	presult->nfrozen = prstate.nfrozen;
	presult->live_tuples = prstate.live_tuples;
	presult->recently_dead_tuples = prstate.recently_dead_tuples;

	/*
	 * 早先为了方便，我们在 all_visible 中忽略了 LP_DEAD 项，以使是否冻结页面的
	 * 决定不受 LP_DEAD 项短期存在的影响。这些 LP_DEAD 项实际上被假定为正在
	 * 形成中的 LP_UNUSED 项。只要当前正在进行的 VACUUM 完成了它，究竟是哪一次
	 * 堆扫描（初次扫描还是最后一次扫描）最终将页面设为全冻结都无关紧要。
	 *
	 * 既然冻结已经确定下来，如果页面上存在任何 LP_DEAD 项，就取消 all_visible。
	 * 它需要反映页面当前的状态，正如调用方所期望的那样。
	 */
	if (prstate.all_visible && prstate.lpdead_items == 0)
	{
		presult->all_visible = prstate.all_visible;
		presult->all_frozen = prstate.all_frozen;
	}
	else
	{
		presult->all_visible = false;
		presult->all_frozen = false;
	}

	presult->hastup = prstate.hastup;

	/*
	 * 对于计划更新可见性映射的调用方，该记录的冲突边界必须是页面上最新的 xmin。
	 * 不过，如果页面被完全冻结，就不会有冲突，vm_conflict_horizon 应当保持为
	 * InvalidTransactionId。这也包括我们刚刚冻结了所有元组的情况；剪枝-冻结
	 * 记录中已经包含了冲突 XID，所以调用方不再需要它。
	 */
	if (presult->all_frozen)
		presult->vm_conflict_horizon = InvalidTransactionId;
	else
		presult->vm_conflict_horizon = prstate.visibility_cutoff_xid;

	presult->lpdead_items = prstate.lpdead_items;
	/* presult->deadoffsets 数组已经被填充 */

	if (prstate.freeze)
	{
		if (presult->nfrozen > 0)
		{
			*new_relfrozen_xid = prstate.pagefrz.FreezePageRelfrozenXid;
			*new_relmin_mxid = prstate.pagefrz.FreezePageRelminMxid;
		}
		else
		{
			*new_relfrozen_xid = prstate.pagefrz.NoFreezePageRelfrozenXid;
			*new_relmin_mxid = prstate.pagefrz.NoFreezePageRelminMxid;
		}
	}
}


/*
 * 执行堆剪枝的可见性检查。
 */
static HTSV_Result
heap_prune_satisfies_vacuum(PruneState *prstate, HeapTuple tup, Buffer buffer)
{
	HTSV_Result res;
	TransactionId dead_after;

	res = HeapTupleSatisfiesVacuumHorizon(tup, buffer, &dead_after);

	if (res != HEAPTUPLE_RECENTLY_DEAD)
		return res;

	/*
	 * 对于 VACUUM，我们必须确保剪枝掉 xmax 早于 OldestXmin 的元组——OldestXmin
	 * 是在开始清理该关系时确定的可见性截止点。OldestXmin 用于冻结判断，而
	 * 我们无法冻结 dead 元组的 xmax。
	 */
	if (prstate->cutoffs &&
		TransactionIdIsValid(prstate->cutoffs->OldestXmin) &&
		NormalTransactionIdPrecedes(dead_after, prstate->cutoffs->OldestXmin))
		return HEAPTUPLE_DEAD;

	/*
	 * 判断与所提供的 GlobalVisState 相比，该元组是否被视为 dead。访问时剪枝
	 * 不会提供 VacuumCutoffs。而对于 vacuum，即使元组的 xmax 不比 OldestXmin
	 * 更旧，如果 GlobalVisState 自开始清理该关系以来已经更新，
	 * GlobalVisTestIsRemovableXid() 仍可能判定该行为 dead。
	 */
	if (GlobalVisTestIsRemovableXid(prstate->vistest, dead_after))
		return HEAPTUPLE_DEAD;

	return res;
}


/*
 * 剪枝只计算一次元组可见性，并将结果保存在一个 int8 数组中。详见 PruneState.htsv。
 * 这个辅助函数的作用，是防止去检查那些尚未计算出的可见性状态数组成员。
 */
static inline HTSV_Result
htsv_get_valid_status(int status)
{
	Assert(status >= HEAPTUPLE_DEAD &&
		   status <= HEAPTUPLE_DELETE_IN_PROGRESS);
	return (HTSV_Result) status;
}

/*
 * 剪枝指定的行指针，或剪枝从该指针起始的 HOT 链。
 *
 * 元组的可见性信息由 prstate->htsv 提供。
 *
 * 如果该项是一个被索引引用的元组（即不是堆内元组），则通过移除 HOT 链开头
 * 所有的 DEAD 元组来剪枝该 HOT 链。我们也会剪枝位于 DEAD 元组之前的任何
 * RECENTLY_DEAD 元组。这样做是可以的，因为一个位于 DEAD 元组之前的 RECENTLY_DEAD
 * 元组实际上已经是 DEAD 了，只是我们的可见性测试太粗略，无法检测到它。
 *
 * 剪枝绝不能留下一个仍然拥有元组存储的 DEAD 元组。VACUUM 没有准备好处理那种
 * 情况。
 *
 * 根行指针会被重定向到最后一个 DEAD 元组紧邻其后的那个元组。如果链中的全部
 * 元组都是 DEAD，根行指针会被标记为 LP_DEAD。（这也包括 DEAD 普通元组的情况，
 * 我们将其视为长度为 1 的链。）
 *
 * 我们在这里并不真正修改页面。我们只是往 prstate 的数组中添加记录，表示将要
 * 进行的更改。要被重定向的项被加入 redirected[] 数组（每次重定向占两个条目）；
 * 要被设为 LP_DEAD 状态的项被加入 nowdead[]；要被设为 LP_UNUSED 状态的项被加入
 * nowunused[]。我们会基于页面在应用这些更改之后的样子，来进行存活元组、可见性
 * 等方面的记账。所有这些记账都在 heap_prune_record_*() 子例程中完成。分工上，
 * heap_prune_chain() 决定每个元组的命运，即它将被移除、被重定向还是保持不变，
 * 而 heap_prune_record_*() 子例程则根据该结果更新 PruneState。
 */
static void
heap_prune_chain(Page page, BlockNumber blockno, OffsetNumber maxoff,
				 OffsetNumber rootoffnum, PruneState *prstate)
{
	TransactionId priorXmax = InvalidTransactionId;
	ItemId		rootlp;
	OffsetNumber offnum;
	OffsetNumber chainitems[MaxHeapTuplesPerPage];

	/*
	 * 在遍历完 HOT 链之后，ndeadchain 是 chainitems 中最后一个 dead 项之后第一个
	 * 存活后继项的索引。
	 */
	int			ndeadchain = 0,
				nchain = 0;

	rootlp = PageGetItemId(page, rootoffnum);

	/* 从根元组开始 */
	offnum = rootoffnum;

	/* 当还未到达链的末尾时 */
	for (;;)
	{
		HeapTupleHeader htup;
		ItemId		lp;

		/* 合理性检查（纯粹出于谨慎） */
		if (offnum < FirstOffsetNumber)
			break;

		/*
		 * 当行指针数组被截断时，可能会出现偏移量超出了页面行指针数组末尾的情况
		 * （原始项一定是 unused 的）
		 */
		if (offnum > maxoff)
			break;

		/* 如果项已经被处理过，停止——它一定不属于同一条链 */
		if (prstate->processed[offnum])
			break;

		lp = PageGetItemId(page, offnum);

		/*
		 * 未使用的项显然不属于这条链。同样地，dead 行指针也不能属于这条链。
		 * 这两种情况都已经被标记为已处理。
		 */
		Assert(ItemIdIsUsed(lp));
		Assert(!ItemIdIsDead(lp));

		/*
		 * 如果我们正在看的是被重定向的根行指针，就跳到链中的第一个普通元组。
		 * 如果我们在别处发现了一个重定向项，则停止——它一定不属于同一条链。
		 */
		if (ItemIdIsRedirected(lp))
		{
			if (nchain > 0)
				break;			/* not at start of chain */
			chainitems[nchain++] = offnum;
			offnum = ItemIdGetRedirect(rootlp);
			continue;
		}

		Assert(ItemIdIsNormal(lp));

		htup = (HeapTupleHeader) PageGetItem(page, lp);

		/*
		 * Check the tuple XMIN against prior XMAX, if any
		 */
		if (TransactionIdIsValid(priorXmax) &&
			!TransactionIdEquals(HeapTupleHeaderGetXmin(htup), priorXmax))
			break;

		/*
		 * OK, this tuple is indeed a member of the chain.
		 */
		chainitems[nchain++] = offnum;

		switch (htsv_get_valid_status(prstate->htsv[offnum]))
		{
			case HEAPTUPLE_DEAD:

				/* Remember the last DEAD tuple seen */
				ndeadchain = nchain;
				HeapTupleHeaderAdvanceConflictHorizon(htup,
													  &prstate->latest_xid_removed);
				/* 前进到链的下一个成员 */
				break;

			case HEAPTUPLE_RECENTLY_DEAD:

			/*
			 * 即使我们在移除 RECENTLY_DEAD 元组，也无需推进冲突边界。这是因为
			 * 我们只有在 RECENTLY_DEAD 元组位于 DEAD 元组之前时才会移除它们，
			 * 而由于 DEAD 元组在链中更靠后，它一定是由比 RECENTLY_DEAD 元组
			 * 更新的事务插入的。对于那个 DEAD 元组，我们已经推进过冲突边界了。
			 */

			/*
			 * 越过 RECENTLY_DEAD 元组，以防它们后面还有 DEAD 元组。我们必须
			 * 确保不会漏掉任何 DEAD 元组，因为剪枝之后仍然拥有元组存储的 DEAD
			 * 元组会让 VACUUM 感到困惑。
			 */
				break;

			case HEAPTUPLE_DELETE_IN_PROGRESS:
			case HEAPTUPLE_LIVE:
			case HEAPTUPLE_INSERT_IN_PROGRESS:
				goto process_chain;

			default:
				elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
				goto process_chain;
		}

		/*
		 * 如果元组不是 HOT 更新的，那么我们就到达了这条 HOT 更新链的末尾。
		 */
		if (!HeapTupleHeaderIsHotUpdated(htup))
			goto process_chain;

		/* HOT 意味着它不可能被移动到不同的分区 */
		Assert(!HeapTupleHeaderIndicatesMovedPartitions(htup));

		/*
		 * 前进到链的下一个成员。
		 */
		Assert(ItemPointerGetBlockNumber(&htup->t_ctid) == blockno);
		offnum = ItemPointerGetOffsetNumber(&htup->t_ctid);
		priorXmax = HeapTupleHeaderGetUpdateXid(htup);
	}

	if (ItemIdIsRedirected(rootlp) && nchain < 2)
	{
		/*
		 * 我们发现了一个重定向项，但它指向的后续项无效。如果
		 * heap_page_prune_and_freeze() 中的循环导致我们在访问重定向项之前
		 * 先访问了它的 dead 后继项，就可能出现这种情况。如果调用方有指示，
		 * 我们可以把重定向项设为 LP_DEAD 状态或 LP_UNUSED 来进行清理。
		 */
		heap_prune_record_dead_or_unused(prstate, rootoffnum, false);
		return;
	}

process_chain:

	if (ndeadchain == 0)
	{
		/*
		 * 没有发现 DEAD 元组，因此这条链完全由普通的、未更改的元组组成。
		 * 保持原样不动。
		 */
		int			i = 0;

		if (ItemIdIsRedirected(rootlp))
		{
			heap_prune_record_unchanged_lp_redirect(prstate, rootoffnum);
			i++;
		}
		for (; i < nchain; i++)
			heap_prune_record_unchanged_lp_normal(page, prstate, chainitems[i]);
	}
	else if (ndeadchain == nchain)
	{
		/*
		 * 整条链都已死亡。将根行指针标记为 LP_DEAD，并彻底移除链中的其它元组。
		 */
		heap_prune_record_dead_or_unused(prstate, rootoffnum, ItemIdIsNormal(rootlp));
		for (int i = 1; i < nchain; i++)
			heap_prune_record_unused(prstate, chainitems[i], true);
	}
	else
	{
		/*
		 * 我们在链中发现了一个 DEAD 元组。将根行指针重定向到第一个非 DEAD 元组，
		 * 并把我们能够从中移除的每个中间项标记为 unused。
		 */
		heap_prune_record_redirect(prstate, rootoffnum, chainitems[ndeadchain],
								   ItemIdIsNormal(rootlp));
		for (int i = 1; i < ndeadchain; i++)
			heap_prune_record_unused(prstate, chainitems[i], true);

		/* 链中其余的元组都是普通、未更改的元组 */
		for (int i = ndeadchain; i < nchain; i++)
			heap_prune_record_unchanged_lp_normal(page, prstate, chainitems[i]);
	}
}

/* 记录最早的“很快可剪枝”XID */
static void
heap_prune_record_prunable(PruneState *prstate, TransactionId xid)
{
	/*
	 * 这应该与 PageSetPrunable 宏完全一致。我们还不能把数据直接写入页头，
	 * 因此先更新工作状态。
	 */
	Assert(TransactionIdIsNormal(xid));
	if (!TransactionIdIsValid(prstate->new_prune_xid) ||
		TransactionIdPrecedes(xid, prstate->new_prune_xid))
		prstate->new_prune_xid = xid;
}

/* 记录将要被重定向的行指针 */
static void
heap_prune_record_redirect(PruneState *prstate,
						   OffsetNumber offnum, OffsetNumber rdoffnum,
						   bool was_normal)
{
	Assert(!prstate->processed[offnum]);
	prstate->processed[offnum] = true;

	/*
	 * 不要在这里标记重定向目标。它需要作为未更改的元组被单独计数。
	 */

	Assert(prstate->nredirected < MaxHeapTuplesPerPage);
	prstate->redirected[prstate->nredirected * 2] = offnum;
	prstate->redirected[prstate->nredirected * 2 + 1] = rdoffnum;

	prstate->nredirected++;

	/*
	 * 如果根项原本是一个普通元组，那么我们正在删除它，所以把它计入结果中。
	 * 但是把重定向项改为其它状态（即使是改成 DEAD 状态）则不计入。
	 */
	if (was_normal)
		prstate->ndeleted++;

	prstate->hastup = true;
}

/* 记录将要被标记为 dead 的行指针 */
static void
heap_prune_record_dead(PruneState *prstate, OffsetNumber offnum,
					   bool was_normal)
{
	Assert(!prstate->processed[offnum]);
	prstate->processed[offnum] = true;

	Assert(prstate->ndead < MaxHeapTuplesPerPage);
	prstate->nowdead[prstate->ndead] = offnum;
	prstate->ndead++;

	/*
	 * 故意把取消 all_visible 的操作推迟到剪枝过程的稍后进行。可移除的 dead
	 * 元组不应妨碍冻结该页面。
	 */

	/* 记录供 vacuum 使用的 dead 偏移 */
	prstate->deadoffsets[prstate->lpdead_items++] = offnum;

	/*
	 * 如果根项原本是一个普通元组，那么我们正在删除它，所以把它计入结果中。
	 * 但是把重定向项改为其它状态（即使是改成 DEAD 状态）则不计入。
	 */
	if (was_normal)
		prstate->ndeleted++;
}

/*
 * 根据调用方是否将 mark_unused_now 设为 true，记录一个行指针应当被标记为
 * LP_DEAD 还是 LP_UNUSED。还有其它一些情况我们会把行指针标记为 LP_UNUSED，
 * 但如果 mark_unused_now 为 true，我们就不会把行指针标记为 LP_DEAD。
 */
static void
heap_prune_record_dead_or_unused(PruneState *prstate, OffsetNumber offnum,
								 bool was_normal)
{
	/*
	 * 如果调用方将 mark_unused_now 设为 true，我们就可以在剪枝过程中移除 dead
	 * 元组，而不必把它们的行指针标记为 dead。把该元组的行指针设为 LP_UNUSED。
	 * 我们暗示这种情况出现的可能性较小。
	 */
	if (unlikely(prstate->mark_unused_now))
		heap_prune_record_unused(prstate, offnum, was_normal);
	else
		heap_prune_record_dead(prstate, offnum, was_normal);
}

/* 记录将要被标记为 unused 的行指针 */
static void
heap_prune_record_unused(PruneState *prstate, OffsetNumber offnum, bool was_normal)
{
	Assert(!prstate->processed[offnum]);
	prstate->processed[offnum] = true;

	Assert(prstate->nunused < MaxHeapTuplesPerPage);
	prstate->nowunused[prstate->nunused] = offnum;
	prstate->nunused++;

	/*
	 * 如果根项原本是一个普通元组，那么我们正在删除它，所以把它计入结果中。
	 * 但是把重定向项改为其它状态（即使是改成 DEAD 状态）则不计入。
	 */
	if (was_normal)
		prstate->ndeleted++;
}

/*
 * Record an unused line pointer that is left unchanged.
 */
static void
heap_prune_record_unchanged_lp_unused(Page page, PruneState *prstate, OffsetNumber offnum)
{
	Assert(!prstate->processed[offnum]);
	prstate->processed[offnum] = true;
}

/*
 * Record line pointer that is left unchanged.  We consider freezing it, and
 * update bookkeeping of tuple counts and page visibility.
 */
static void
heap_prune_record_unchanged_lp_normal(Page page, PruneState *prstate, OffsetNumber offnum)
{
	HeapTupleHeader htup;

	Assert(!prstate->processed[offnum]);
	prstate->processed[offnum] = true;

	prstate->hastup = true;		/* 页面非空 */

	/*
	 * The criteria for counting a tuple as live in this block need to match
	 * what analyze.c's acquire_sample_rows() does, otherwise VACUUM and
	 * ANALYZE may produce wildly different reltuples values, e.g. when there
	 * are many recently-dead tuples.
	 *
	 * The logic here is a bit simpler than acquire_sample_rows(), as VACUUM
	 * can't run inside a transaction block, which makes some cases impossible
	 * (e.g. in-progress insert from the same transaction).
	 *
	 * HEAPTUPLE_DEAD are handled by the other heap_prune_record_*()
	 * subroutines.  They don't count dead items like acquire_sample_rows()
	 * does, because we assume that all dead items will become LP_UNUSED
	 * before VACUUM finishes.  This difference is only superficial.  VACUUM
	 * effectively agrees with ANALYZE about DEAD items, in the end.  VACUUM
	 * won't remember LP_DEAD items, but only because they're not supposed to
	 * be left behind when it is done. (Cases where we bypass index vacuuming
	 * will violate this optimistic assumption, but the overall impact of that
	 * should be negligible.)
	 */
	htup = (HeapTupleHeader) PageGetItem(page, PageGetItemId(page, offnum));

	switch (prstate->htsv[offnum])
	{
		case HEAPTUPLE_LIVE:

			/*
			 * Count it as live.  Not only is this natural, but it's also what
			 * acquire_sample_rows() does.
			 */
			prstate->live_tuples++;

			/*
			 * Is the tuple definitely visible to all transactions?
			 *
			 * NB: Like with per-tuple hint bits, we can't set the
			 * PD_ALL_VISIBLE flag if the inserter committed asynchronously.
			 * See SetHintBits for more info.  Check that the tuple is hinted
			 * xmin-committed because of that.
			 */
			if (prstate->all_visible)
			{
				TransactionId xmin;

				if (!HeapTupleHeaderXminCommitted(htup))
				{
					prstate->all_visible = false;
					break;
				}

				/*
				 * The inserter definitely committed.  But is it old enough
				 * that everyone sees it as committed?  A FrozenTransactionId
				 * is seen as committed to everyone.  Otherwise, we check if
				 * there is a snapshot that considers this xid to still be
				 * running, and if so, we don't consider the page all-visible.
				 */
				xmin = HeapTupleHeaderGetXmin(htup);

				/*
				 * For now always use prstate->cutoffs for this test, because
				 * we only update 'all_visible' when freezing is requested. We
				 * could use GlobalVisTestIsRemovableXid instead, if a
				 * non-freezing caller wanted to set the VM bit.
				 */
				Assert(prstate->cutoffs);
				if (!TransactionIdPrecedes(xmin, prstate->cutoffs->OldestXmin))
				{
					prstate->all_visible = false;
					break;
				}

				/* Track newest xmin on page. */
				if (TransactionIdFollows(xmin, prstate->visibility_cutoff_xid) &&
					TransactionIdIsNormal(xmin))
					prstate->visibility_cutoff_xid = xmin;
			}
			break;

		case HEAPTUPLE_RECENTLY_DEAD:
			prstate->recently_dead_tuples++;
			prstate->all_visible = false;

			/*
			 * This tuple will soon become DEAD.  Update the hint field so
			 * that the page is reconsidered for pruning in future.
			 */
			heap_prune_record_prunable(prstate,
									   HeapTupleHeaderGetUpdateXid(htup));
			break;

		case HEAPTUPLE_INSERT_IN_PROGRESS:

			/*
			 * We do not count these rows as live, because we expect the
			 * inserting transaction to update the counters at commit, and we
			 * assume that will happen only after we report our results.  This
			 * assumption is a bit shaky, but it is what acquire_sample_rows()
			 * does, so be consistent.
			 */
			prstate->all_visible = false;

			/*
			 * If we wanted to optimize for aborts, we might consider marking
			 * the page prunable when we see INSERT_IN_PROGRESS.  But we
			 * don't.  See related decisions about when to mark the page
			 * prunable in heapam.c.
			 */
			break;

		case HEAPTUPLE_DELETE_IN_PROGRESS:

			/*
			 * This an expected case during concurrent vacuum.  Count such
			 * rows as live.  As above, we assume the deleting transaction
			 * will commit and update the counters after we report.
			 */
			prstate->live_tuples++;
			prstate->all_visible = false;

			/*
			 * This tuple may soon become DEAD.  Update the hint field so that
			 * the page is reconsidered for pruning in future.
			 */
			heap_prune_record_prunable(prstate,
									   HeapTupleHeaderGetUpdateXid(htup));
			break;

		default:

			/*
			 * DEAD tuples should've been passed to heap_prune_record_dead()
			 * or heap_prune_record_unused() instead.
			 */
			elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result %d",
				 prstate->htsv[offnum]);
			break;
	}

	/* Consider freezing any normal tuples which will not be removed */
	if (prstate->freeze)
	{
		bool		totally_frozen;

		if ((heap_prepare_freeze_tuple(htup,
									   prstate->cutoffs,
									   &prstate->pagefrz,
									   &prstate->frozen[prstate->nfrozen],
									   &totally_frozen)))
		{
			/* Save prepared freeze plan for later */
			prstate->frozen[prstate->nfrozen++].offset = offnum;
		}

		/*
		 * If any tuple isn't either totally frozen already or eligible to
		 * become totally frozen (according to its freeze plan), then the page
		 * definitely cannot be set all-frozen in the visibility map later on.
		 */
		if (!totally_frozen)
			prstate->all_frozen = false;
	}
}


/*
 * Record line pointer that was already LP_DEAD and is left unchanged.
 */
static void
heap_prune_record_unchanged_lp_dead(Page page, PruneState *prstate, OffsetNumber offnum)
{
	Assert(!prstate->processed[offnum]);
	prstate->processed[offnum] = true;

	/*
	 * Deliberately don't set hastup for LP_DEAD items.  We make the soft
	 * assumption that any LP_DEAD items encountered here will become
	 * LP_UNUSED later on, before count_nondeletable_pages is reached.  If we
	 * don't make this assumption then rel truncation will only happen every
	 * other VACUUM, at most.  Besides, VACUUM must treat
	 * hastup/nonempty_pages as provisional no matter how LP_DEAD items are
	 * handled (handled here, or handled later on).
	 *
	 * Similarly, don't unset all_visible until later, at the end of
	 * heap_page_prune_and_freeze().  This will allow us to attempt to freeze
	 * the page after pruning.  As long as we unset it before updating the
	 * visibility map, this will be correct.
	 */

	/* 记录供 vacuum 使用的 dead 偏移 */
	prstate->deadoffsets[prstate->lpdead_items++] = offnum;
}

/*
 * Record LP_REDIRECT that is left unchanged.
 */
static void
heap_prune_record_unchanged_lp_redirect(PruneState *prstate, OffsetNumber offnum)
{
	/*
	 * A redirect line pointer doesn't count as a live tuple.
	 *
	 * If we leave a redirect line pointer in place, there will be another
	 * tuple on the page that it points to.  We will do the bookkeeping for
	 * that separately.  So we have nothing to do here, except remember that
	 * we processed this item.
	 */
	Assert(!prstate->processed[offnum]);
	prstate->processed[offnum] = true;
}

/*
 * Perform the actual page changes needed by heap_page_prune_and_freeze().
 *
 * If 'lp_truncate_only' is set, we are merely marking LP_DEAD line pointers
 * as unused, not redirecting or removing anything else.  The
 * PageRepairFragmentation() call is skipped in that case.
 *
 * If 'lp_truncate_only' is not set, the caller must hold a cleanup lock on
 * the buffer.  If it is set, an ordinary exclusive lock suffices.
 */
void
heap_page_prune_execute(Buffer buffer, bool lp_truncate_only,
						OffsetNumber *redirected, int nredirected,
						OffsetNumber *nowdead, int ndead,
						OffsetNumber *nowunused, int nunused)
{
	Page		page = (Page) BufferGetPage(buffer);
	OffsetNumber *offnum;
	HeapTupleHeader htup PG_USED_FOR_ASSERTS_ONLY;

	/* Shouldn't be called unless there's something to do */
	Assert(nredirected > 0 || ndead > 0 || nunused > 0);

	/* If 'lp_truncate_only', we can only remove already-dead line pointers */
	Assert(!lp_truncate_only || (nredirected == 0 && ndead == 0));

	/* Update all redirected line pointers */
	offnum = redirected;
	for (int i = 0; i < nredirected; i++)
	{
		OffsetNumber fromoff = *offnum++;
		OffsetNumber tooff = *offnum++;
		ItemId		fromlp = PageGetItemId(page, fromoff);
		ItemId		tolp PG_USED_FOR_ASSERTS_ONLY;

#ifdef USE_ASSERT_CHECKING

		/*
		 * Any existing item that we set as an LP_REDIRECT (any 'from' item)
		 * must be the first item from a HOT chain.  If the item has tuple
		 * storage then it can't be a heap-only tuple.  Otherwise we are just
		 * maintaining an existing LP_REDIRECT from an existing HOT chain that
		 * has been pruned at least once before now.
		 */
		if (!ItemIdIsRedirected(fromlp))
		{
			Assert(ItemIdHasStorage(fromlp) && ItemIdIsNormal(fromlp));

			htup = (HeapTupleHeader) PageGetItem(page, fromlp);
			Assert(!HeapTupleHeaderIsHeapOnly(htup));
		}
		else
		{
			/* We shouldn't need to redundantly set the redirect */
			Assert(ItemIdGetRedirect(fromlp) != tooff);
		}

		/*
		 * The item that we're about to set as an LP_REDIRECT (the 'from'
		 * item) will point to an existing item (the 'to' item) that is
		 * already a heap-only tuple.  There can be at most one LP_REDIRECT
		 * item per HOT chain.
		 *
		 * We need to keep around an LP_REDIRECT item (after original
		 * non-heap-only root tuple gets pruned away) so that it's always
		 * possible for VACUUM to easily figure out what TID to delete from
		 * indexes when an entire HOT chain becomes dead.  A heap-only tuple
		 * can never become LP_DEAD; an LP_REDIRECT item or a regular heap
		 * tuple can.
		 *
		 * This check may miss problems, e.g. the target of a redirect could
		 * be marked as unused subsequently. The page_verify_redirects() check
		 * below will catch such problems.
		 */
		tolp = PageGetItemId(page, tooff);
		Assert(ItemIdHasStorage(tolp) && ItemIdIsNormal(tolp));
		htup = (HeapTupleHeader) PageGetItem(page, tolp);
		Assert(HeapTupleHeaderIsHeapOnly(htup));
#endif

		ItemIdSetRedirect(fromlp, tooff);
	}

	/* Update all now-dead line pointers */
	offnum = nowdead;
	for (int i = 0; i < ndead; i++)
	{
		OffsetNumber off = *offnum++;
		ItemId		lp = PageGetItemId(page, off);

#ifdef USE_ASSERT_CHECKING

		/*
		 * An LP_DEAD line pointer must be left behind when the original item
		 * (which is dead to everybody) could still be referenced by a TID in
		 * an index.  This should never be necessary with any individual
		 * heap-only tuple item, though. (It's not clear how much of a problem
		 * that would be, but there is no reason to allow it.)
		 */
		if (ItemIdHasStorage(lp))
		{
			Assert(ItemIdIsNormal(lp));
			htup = (HeapTupleHeader) PageGetItem(page, lp);
			Assert(!HeapTupleHeaderIsHeapOnly(htup));
		}
		else
		{
			/* Whole HOT chain becomes dead */
			Assert(ItemIdIsRedirected(lp));
		}
#endif

		ItemIdSetDead(lp);
	}

	/* Update all now-unused line pointers */
	offnum = nowunused;
	for (int i = 0; i < nunused; i++)
	{
		OffsetNumber off = *offnum++;
		ItemId		lp = PageGetItemId(page, off);

#ifdef USE_ASSERT_CHECKING

		if (lp_truncate_only)
		{
			/* Setting LP_DEAD to LP_UNUSED in vacuum's second pass */
			Assert(ItemIdIsDead(lp) && !ItemIdHasStorage(lp));
		}
		else
		{
			/*
			 * When heap_page_prune_and_freeze() was called, mark_unused_now
			 * may have been passed as true, which allows would-be LP_DEAD
			 * items to be made LP_UNUSED instead.  This is only possible if
			 * the relation has no indexes.  If there are any dead items, then
			 * mark_unused_now was not true and every item being marked
			 * LP_UNUSED must refer to a heap-only tuple.
			 */
			if (ndead > 0)
			{
				Assert(ItemIdHasStorage(lp) && ItemIdIsNormal(lp));
				htup = (HeapTupleHeader) PageGetItem(page, lp);
				Assert(HeapTupleHeaderIsHeapOnly(htup));
			}
			else
				Assert(ItemIdIsUsed(lp));
		}

#endif

		ItemIdSetUnused(lp);
	}

	if (lp_truncate_only)
		PageTruncateLinePointerArray(page);
	else
	{
		/*
		 * Finally, repair any fragmentation, and update the page's hint bit
		 * about whether it has free pointers.
		 */
		PageRepairFragmentation(page);

		/*
		 * Now that the page has been modified, assert that redirect items
		 * still point to valid targets.
		 */
		page_verify_redirects(page);
	}
}


/*
 * If built with assertions, verify that all LP_REDIRECT items point to a
 * valid item.
 *
 * One way that bugs related to HOT pruning show is redirect items pointing to
 * removed tuples. It's not trivial to reliably check that marking an item
 * unused will not orphan a redirect item during heap_prune_chain() /
 * heap_page_prune_execute(), so we additionally check the whole page after
 * pruning. Without this check such bugs would typically only cause asserts
 * later, potentially well after the corruption has been introduced.
 *
 * Also check comments in heap_page_prune_execute()'s redirection loop.
 */
static void
page_verify_redirects(Page page)
{
#ifdef USE_ASSERT_CHECKING
	OffsetNumber offnum;
	OffsetNumber maxoff;

	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemid = PageGetItemId(page, offnum);
		OffsetNumber targoff;
		ItemId		targitem;
		HeapTupleHeader htup;

		if (!ItemIdIsRedirected(itemid))
			continue;

		targoff = ItemIdGetRedirect(itemid);
		targitem = PageGetItemId(page, targoff);

		Assert(ItemIdIsUsed(targitem));
		Assert(ItemIdIsNormal(targitem));
		Assert(ItemIdHasStorage(targitem));
		htup = (HeapTupleHeader) PageGetItem(page, targitem);
		Assert(HeapTupleHeaderIsHeapOnly(htup));
	}
#endif
}


/*
 * For all items in this page, find their respective root line pointers.
 * If item k is part of a HOT-chain with root at item j, then we set
 * root_offsets[k - 1] = j.
 *
 * The passed-in root_offsets array must have MaxHeapTuplesPerPage entries.
 * Unused entries are filled with InvalidOffsetNumber (zero).
 *
 * The function must be called with at least share lock on the buffer, to
 * prevent concurrent prune operations.
 *
 * Note: The information collected here is valid only as long as the caller
 * holds a pin on the buffer. Once pin is released, a tuple might be pruned
 * and reused by a completely unrelated tuple.
 */
void
heap_get_root_tuples(Page page, OffsetNumber *root_offsets)
{
	OffsetNumber offnum,
				maxoff;

	MemSet(root_offsets, InvalidOffsetNumber,
		   MaxHeapTuplesPerPage * sizeof(OffsetNumber));

	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber; offnum <= maxoff; offnum = OffsetNumberNext(offnum))
	{
		ItemId		lp = PageGetItemId(page, offnum);
		HeapTupleHeader htup;
		OffsetNumber nextoffnum;
		TransactionId priorXmax;

		/* skip unused and dead items */
		if (!ItemIdIsUsed(lp) || ItemIdIsDead(lp))
			continue;

		if (ItemIdIsNormal(lp))
		{
			htup = (HeapTupleHeader) PageGetItem(page, lp);

			/*
			 * Check if this tuple is part of a HOT-chain rooted at some other
			 * tuple. If so, skip it for now; we'll process it when we find
			 * its root.
			 */
			if (HeapTupleHeaderIsHeapOnly(htup))
				continue;

			/*
			 * This is either a plain tuple or the root of a HOT-chain.
			 * Remember it in the mapping.
			 */
			root_offsets[offnum - 1] = offnum;

			/* If it's not the start of a HOT-chain, we're done with it */
			if (!HeapTupleHeaderIsHotUpdated(htup))
				continue;

			/* Set up to scan the HOT-chain */
			nextoffnum = ItemPointerGetOffsetNumber(&htup->t_ctid);
			priorXmax = HeapTupleHeaderGetUpdateXid(htup);
		}
		else
		{
			/* Must be a redirect item. We do not set its root_offsets entry */
			Assert(ItemIdIsRedirected(lp));
			/* Set up to scan the HOT-chain */
			nextoffnum = ItemIdGetRedirect(lp);
			priorXmax = InvalidTransactionId;
		}

		/*
		 * Now follow the HOT-chain and collect other tuples in the chain.
		 *
		 * Note: Even though this is a nested loop, the complexity of the
		 * function is O(N) because a tuple in the page should be visited not
		 * more than twice, once in the outer loop and once in HOT-chain
		 * chases.
		 */
		for (;;)
		{
			/* 合理性检查（纯粹出于谨慎） */
			if (offnum < FirstOffsetNumber)
				break;

			/*
			 * An offset past the end of page's line pointer array is possible
			 * when the array was truncated
			 */
			if (offnum > maxoff)
				break;

			lp = PageGetItemId(page, nextoffnum);

			/* Check for broken chains */
			if (!ItemIdIsNormal(lp))
				break;

			htup = (HeapTupleHeader) PageGetItem(page, lp);

			if (TransactionIdIsValid(priorXmax) &&
				!TransactionIdEquals(priorXmax, HeapTupleHeaderGetXmin(htup)))
				break;

			/* Remember the root line pointer for this item */
			root_offsets[nextoffnum - 1] = offnum;

			/* Advance to next chain member, if any */
			if (!HeapTupleHeaderIsHotUpdated(htup))
				break;

			/* HOT 意味着它不可能被移动到不同的分区 */
			Assert(!HeapTupleHeaderIndicatesMovedPartitions(htup));

			nextoffnum = ItemPointerGetOffsetNumber(&htup->t_ctid);
			priorXmax = HeapTupleHeaderGetUpdateXid(htup);
		}
	}
}


/*
 * Compare fields that describe actions required to freeze tuple with caller's
 * open plan.  If everything matches then the frz tuple plan is equivalent to
 * caller's plan.
 */
static inline bool
heap_log_freeze_eq(xlhp_freeze_plan *plan, HeapTupleFreeze *frz)
{
	if (plan->xmax == frz->xmax &&
		plan->t_infomask2 == frz->t_infomask2 &&
		plan->t_infomask == frz->t_infomask &&
		plan->frzflags == frz->frzflags)
		return true;

	/* Caller must call heap_log_freeze_new_plan again for frz */
	return false;
}

/*
 * Comparator used to deduplicate the freeze plans used in WAL records.
 */
static int
heap_log_freeze_cmp(const void *arg1, const void *arg2)
{
	HeapTupleFreeze *frz1 = (HeapTupleFreeze *) arg1;
	HeapTupleFreeze *frz2 = (HeapTupleFreeze *) arg2;

	if (frz1->xmax < frz2->xmax)
		return -1;
	else if (frz1->xmax > frz2->xmax)
		return 1;

	if (frz1->t_infomask2 < frz2->t_infomask2)
		return -1;
	else if (frz1->t_infomask2 > frz2->t_infomask2)
		return 1;

	if (frz1->t_infomask < frz2->t_infomask)
		return -1;
	else if (frz1->t_infomask > frz2->t_infomask)
		return 1;

	if (frz1->frzflags < frz2->frzflags)
		return -1;
	else if (frz1->frzflags > frz2->frzflags)
		return 1;

	/*
	 * heap_log_freeze_eq would consider these tuple-wise plans to be equal.
	 * (So the tuples will share a single canonical freeze plan.)
	 *
	 * We tiebreak on page offset number to keep each freeze plan's page
	 * offset number array individually sorted. (Unnecessary, but be tidy.)
	 */
	if (frz1->offset < frz2->offset)
		return -1;
	else if (frz1->offset > frz2->offset)
		return 1;

	Assert(false);
	return 0;
}

/*
 * Start new plan initialized using tuple-level actions.  At least one tuple
 * will have steps required to freeze described by caller's plan during REDO.
 */
static inline void
heap_log_freeze_new_plan(xlhp_freeze_plan *plan, HeapTupleFreeze *frz)
{
	plan->xmax = frz->xmax;
	plan->t_infomask2 = frz->t_infomask2;
	plan->t_infomask = frz->t_infomask;
	plan->frzflags = frz->frzflags;
	plan->ntuples = 1;			/* for now */
}

/*
 * Deduplicate tuple-based freeze plans so that each distinct set of
 * processing steps is only stored once in the WAL record.
 * Called during original execution of freezing (for logged relations).
 *
 * Return value is number of plans set in *plans_out for caller.  Also writes
 * an array of offset numbers into *offsets_out output argument for caller
 * (actually there is one array per freeze plan, but that's not of immediate
 * concern to our caller).
 */
static int
heap_log_freeze_plan(HeapTupleFreeze *tuples, int ntuples,
					 xlhp_freeze_plan *plans_out,
					 OffsetNumber *offsets_out)
{
	int			nplans = 0;

	/* Sort tuple-based freeze plans in the order required to deduplicate */
	qsort(tuples, ntuples, sizeof(HeapTupleFreeze), heap_log_freeze_cmp);

	for (int i = 0; i < ntuples; i++)
	{
		HeapTupleFreeze *frz = tuples + i;

		if (i == 0)
		{
			/* New canonical freeze plan starting with first tup */
			heap_log_freeze_new_plan(plans_out, frz);
			nplans++;
		}
		else if (heap_log_freeze_eq(plans_out, frz))
		{
			/* tup matches open canonical plan -- include tup in it */
			Assert(offsets_out[i - 1] < frz->offset);
			plans_out->ntuples++;
		}
		else
		{
			/* Tup doesn't match current plan -- done with it now */
			plans_out++;

			/* New canonical freeze plan starting with this tup */
			heap_log_freeze_new_plan(plans_out, frz);
			nplans++;
		}

		/*
		 * Save page offset number in dedicated buffer in passing.
		 *
		 * REDO routine relies on the record's offset numbers array grouping
		 * offset numbers by freeze plan.  The sort order within each grouping
		 * is ascending offset number order, just to keep things tidy.
		 */
		offsets_out[i] = frz->offset;
	}

	Assert(nplans > 0 && nplans <= ntuples);

	return nplans;
}

/*
 * Write an XLOG_HEAP2_PRUNE_FREEZE WAL record
 *
 * This is used for several different page maintenance operations:
 *
 * - Page pruning, in VACUUM's 1st pass or on access: Some items are
 *   redirected, some marked dead, and some removed altogether.
 *
 * - Freezing: Items are marked as 'frozen'.
 *
 * - Vacuum, 2nd pass: Items that are already LP_DEAD are marked as unused.
 *
 * They have enough commonalities that we use a single WAL record for them
 * all.
 *
 * If replaying the record requires a cleanup lock, pass cleanup_lock = true.
 * Replaying 'redirected' or 'dead' items always requires a cleanup lock, but
 * replaying 'unused' items depends on whether they were all previously marked
 * as dead.
 *
 * Note: This function scribbles on the 'frozen' array.
 *
 * Note: This is called in a critical section, so careful what you do here.
 */
void
log_heap_prune_and_freeze(Relation relation, Buffer buffer,
						  TransactionId conflict_xid,
						  bool cleanup_lock,
						  PruneReason reason,
						  HeapTupleFreeze *frozen, int nfrozen,
						  OffsetNumber *redirected, int nredirected,
						  OffsetNumber *dead, int ndead,
						  OffsetNumber *unused, int nunused)
{
	xl_heap_prune xlrec;
	XLogRecPtr	recptr;
	uint8		info;

	/* The following local variables hold data registered in the WAL record: */
	xlhp_freeze_plan plans[MaxHeapTuplesPerPage];
	xlhp_freeze_plans freeze_plans;
	xlhp_prune_items redirect_items;
	xlhp_prune_items dead_items;
	xlhp_prune_items unused_items;
	OffsetNumber frz_offsets[MaxHeapTuplesPerPage];

	xlrec.flags = 0;

	/*
	 * Prepare data for the buffer.  The arrays are not actually in the
	 * buffer, but we pretend that they are.  When XLogInsert stores a full
	 * page image, the arrays can be omitted.
	 */
	XLogBeginInsert();
	XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
	if (nfrozen > 0)
	{
		int			nplans;

		xlrec.flags |= XLHP_HAS_FREEZE_PLANS;

		/*
		 * Prepare deduplicated representation for use in the WAL record. This
		 * destructively sorts frozen tuples array in-place.
		 */
		nplans = heap_log_freeze_plan(frozen, nfrozen, plans, frz_offsets);

		freeze_plans.nplans = nplans;
		XLogRegisterBufData(0, &freeze_plans,
							offsetof(xlhp_freeze_plans, plans));
		XLogRegisterBufData(0, plans,
							sizeof(xlhp_freeze_plan) * nplans);
	}
	if (nredirected > 0)
	{
		xlrec.flags |= XLHP_HAS_REDIRECTIONS;

		redirect_items.ntargets = nredirected;
		XLogRegisterBufData(0, &redirect_items,
							offsetof(xlhp_prune_items, data));
		XLogRegisterBufData(0, redirected,
							sizeof(OffsetNumber[2]) * nredirected);
	}
	if (ndead > 0)
	{
		xlrec.flags |= XLHP_HAS_DEAD_ITEMS;

		dead_items.ntargets = ndead;
		XLogRegisterBufData(0, &dead_items,
							offsetof(xlhp_prune_items, data));
		XLogRegisterBufData(0, dead,
							sizeof(OffsetNumber) * ndead);
	}
	if (nunused > 0)
	{
		xlrec.flags |= XLHP_HAS_NOW_UNUSED_ITEMS;

		unused_items.ntargets = nunused;
		XLogRegisterBufData(0, &unused_items,
							offsetof(xlhp_prune_items, data));
		XLogRegisterBufData(0, unused,
							sizeof(OffsetNumber) * nunused);
	}
	if (nfrozen > 0)
		XLogRegisterBufData(0, frz_offsets,
							sizeof(OffsetNumber) * nfrozen);

	/*
	 * Prepare the main xl_heap_prune record.  We already set the XLHP_HAS_*
	 * flag above.
	 */
	if (RelationIsAccessibleInLogicalDecoding(relation))
		xlrec.flags |= XLHP_IS_CATALOG_REL;
	if (TransactionIdIsValid(conflict_xid))
		xlrec.flags |= XLHP_HAS_CONFLICT_HORIZON;
	if (cleanup_lock)
		xlrec.flags |= XLHP_CLEANUP_LOCK;
	else
	{
		Assert(nredirected == 0 && ndead == 0);
		/* also, any items in 'unused' must've been LP_DEAD previously */
	}
	XLogRegisterData(&xlrec, SizeOfHeapPrune);
	if (TransactionIdIsValid(conflict_xid))
		XLogRegisterData(&conflict_xid, sizeof(TransactionId));

	switch (reason)
	{
		case PRUNE_ON_ACCESS:
			info = XLOG_HEAP2_PRUNE_ON_ACCESS;
			break;
		case PRUNE_VACUUM_SCAN:
			info = XLOG_HEAP2_PRUNE_VACUUM_SCAN;
			break;
		case PRUNE_VACUUM_CLEANUP:
			info = XLOG_HEAP2_PRUNE_VACUUM_CLEANUP;
			break;
		default:
			elog(ERROR, "unrecognized prune reason: %d", (int) reason);
			break;
	}
	recptr = XLogInsert(RM_HEAP2_ID, info);

	PageSetLSN(BufferGetPage(buffer), recptr);
}
