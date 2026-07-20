/*-------------------------------------------------------------------------
 *
 * fsmpage.c
 *	  routines to search and manipulate one FSM page.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/freespace/fsmpage.c
 *
 * NOTES:
 *
 *	本文件中的公共函数构成了一个 API，用于隐藏 FSM 页面的内部结构。
 *	这使得 freespace.c 可以将每个 FSM 页面视为一个含有 SlotsPerPage 个
 *	"槽位"（slot）的黑盒。fsm_set_avail() 和 fsm_get_avail() 让你可以
 *	获取/设置某个槽位的值，而 fsm_search_avail() 则让你搜索值 >= X 的槽位。
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/bufmgr.h"
#include "storage/fsm_internals.h"

/* 在页面内部遍历树结构的宏。根节点索引为零。 */
#define leftchild(x)	(2 * (x) + 1)
#define rightchild(x)	(2 * (x) + 2)
#define parentof(x)		(((x) - 1) / 2)

/*
 * 查找 x 的右邻居，在同一层内环绕。
 */
static int
rightneighbor(int x)
{
	/*
	 * 向右移动。这可能会环绕，跨入下一层最左边的节点。
	 */
	x++;

	/*
	 * 检查我们是否跨入了下一层最左边的节点，若是则予以修正。
	 * 每一层最左边的节点编号为 x = 2^level - 1，因此可以通过判断
	 * (x + 1) 是否是 2 的幂来确认，这里使用了一个标准的补码算术技巧。
	 */
	if (((x + 1) & x) == 0)
		x = parentof(x);

	return x;
}

/*
 * 设置页面上某个槽位的值。若页面被修改则返回 true。
 *
 * 调用者必须持有该页面的排他锁。
 */
bool
fsm_set_avail(Page page, int slot, uint8 value)
{
	int			nodeno = NonLeafNodesPerPage + slot;
	FSMPage		fsmpage = (FSMPage) PageGetContents(page);
	uint8		oldvalue;

	Assert(slot < LeafNodesPerPage);

	oldvalue = fsmpage->fp_nodes[nodeno];

	/* 如果值没有变化，就无需做任何操作 */
	if (oldvalue == value && value <= fsmpage->fp_nodes[0])
		return false;

	fsmpage->fp_nodes[nodeno] = value;

	/*
	 * 向上传播，直到抵达根节点或者一个不需要更新的节点为止。
	 */
	do
	{
		uint8		newvalue = 0;
		int			lchild;
		int			rchild;

		nodeno = parentof(nodeno);
		lchild = leftchild(nodeno);
		rchild = lchild + 1;

		newvalue = fsmpage->fp_nodes[lchild];
		if (rchild < NodesPerPage)
			newvalue = Max(newvalue,
						   fsmpage->fp_nodes[rchild]);

		oldvalue = fsmpage->fp_nodes[nodeno];
		if (oldvalue == newvalue)
			break;

		fsmpage->fp_nodes[nodeno] = newvalue;
	} while (nodeno > 0);

	/*
	 * 完整性检查：如果新值（仍然）高于顶层的值，说明树已损坏。
	 * 若是如此，则重建。
	 */
	if (value > fsmpage->fp_nodes[0])
		fsm_rebuild_page(page);

	return true;
}

/*
 * 返回页面上给定槽位的值。
 *
 * 由于这只是对单个字节的只读访问，因此无需对页面加锁。
 */
uint8
fsm_get_avail(Page page, int slot)
{
	FSMPage		fsmpage = (FSMPage) PageGetContents(page);

	Assert(slot < LeafNodesPerPage);

	return fsmpage->fp_nodes[NonLeafNodesPerPage + slot];
}

/*
 * 返回页面根节点处的值。
 *
 * 由于这只是对单个字节的只读访问，因此无需对页面加锁。
 */
uint8
fsm_get_max_avail(Page page)
{
	FSMPage		fsmpage = (FSMPage) PageGetContents(page);

	return fsmpage->fp_nodes[0];
}

/*
 * 搜索类别至少为 minvalue 的槽位。
 * 返回槽位编号，若未找到则返回 -1。
 *
 * 调用者必须至少持有该页面的共享锁，而本函数在需要更新页面时，
 * 可以先解锁再以排他模式重新加锁。如果调用者已经持有排他锁，
 * 则将 exclusive_lock_held 设为 true，以避免做多余的工作。
 *
 * 如果 advancenext 为 false，fp_next_slot 会被设为指向所返回的槽位；
 * 如果为 true，则指向所返回槽位的下一个槽位。
 */
int
fsm_search_avail(Buffer buf, uint8 minvalue, bool advancenext,
				 bool exclusive_lock_held)
{
	Page		page = BufferGetPage(buf);
	FSMPage		fsmpage = (FSMPage) PageGetContents(page);
	int			nodeno;
	int			target;
	uint16		slot;

restart:

	/*
	 * 先检查根节点，如果没有叶子节点具有足够的空闲空间，则快速退出。
	 */
	if (fsmpage->fp_nodes[0] < minvalue)
		return -1;

	/*
	 * 使用 fp_next_slot 作为搜索起点。它只是一个提示，因此需要检查其
	 * 取值是否合理。（当上一次调用返回页面上最后一个槽位时，这也处理了
	 * 环绕的情况。）
	 */
	target = fsmpage->fp_next_slot;
	if (target < 0 || target >= LeafNodesPerPage)
		target = 0;
	target += NonLeafNodesPerPage;

	/*----------
	 * 从目标槽位开始搜索。每一步，先向右移动一个节点，再向上爬到父节点。
	 * 当我们抵达一个拥有足够空闲空间的节点时停止（必然会如此，因为根节点
	 * 具有足够的空间）。
	 *
	 * 其核心思想是逐步扩展我们的"搜索三角形"，即当前节点所覆盖的所有
	 * 节点，并确保我们从起点开始一直向右搜索。第一步时，只检查目标槽位。
	 * 当我们从左孩子向上移动到父节点时，我们是在把该父节点的右子树加入
	 * 搜索三角形。当我们从右孩子先向右再向上移动时，我们是在丢弃当前的
	 * 搜索三角形（我们已知其中不含任何合适的页面），转而查看其右侧、尺寸
	 * 大一号的三角形。因此我们从最初的起点出发永远不会向左看，且每一步
	 * 搜索三角形的尺寸都会翻倍，从而保证搜索 N 个页面只需 log2(N) 的工作量。
	 *
	 * "向右移动"操作在碰到树右边界时会环绕，因此即便我们从靠近右侧的位置
	 * 开始，行为依然是良好的。还需注意，这种"先向右再向上"的行为确保了我们
	 * 不会落在叶子层右侧那些缺失的节点上。
	 *
	 * 举例来说，考虑下面这棵树：
	 *
	 *		   7
	 *	   7	   6
	 *	 5	 7	 6	 5
	 *	4 5 5 7 2 6 5 2
	 *				T
	 *
	 * 假设目标节点就是字母 T 所指示的节点，并且我们在搜索值大于等于 6 的
	 * 节点。搜索从 T 开始。第一次迭代时，我们向右移动，再向上到父节点，
	 * 到达最右侧的 5。第二次迭代时，我们向右移动（环绕），再向上爬，到达
	 * 第三层的 7。7 满足我们的搜索条件，于是我们沿着全是 7 的路径向下
	 * 走到最底层。这实际上就是起点（允许环绕）之后第一个合适的页面。
	 *----------
	 */
	nodeno = target;
	while (nodeno > 0)
	{
		if (fsmpage->fp_nodes[nodeno] >= minvalue)
			break;

		/*
		 * 向右移动，必要时在同一层内环绕，然后向上爬。
		 */
		nodeno = parentof(rightneighbor(nodeno));
	}

	/*
	 * 我们现在位于树中间某处一个具有足够空闲空间的节点上。向下走到最底层，
	 * 沿一条具有足够空闲空间的路径下行，如果存在选择则优先向左移动。
	 */
	while (nodeno < NonLeafNodesPerPage)
	{
		int			childnodeno = leftchild(nodeno);

		if (childnodeno < NodesPerPage &&
			fsmpage->fp_nodes[childnodeno] >= minvalue)
		{
			nodeno = childnodeno;
			continue;
		}
		childnodeno++;			/* 指向右孩子 */
		if (childnodeno < NodesPerPage &&
			fsmpage->fp_nodes[childnodeno] >= minvalue)
		{
			nodeno = childnodeno;
		}
		else
		{
			/*
			 * 哎呀。父节点承诺过左孩子或右孩子中至少有一个拥有足够的空间，
			 * 但两者实际上都没有。这可能发生在出现"撕裂页"（torn page）的
			 * 情况下，也就是说，如果我们之前在把页面写入磁盘时崩溃了，只有
			 * 页面的一部分成功写入了磁盘。
			 *
			 * 修复损坏并重新开始。
			 */
			RelFileLocator rlocator;
			ForkNumber	forknum;
			BlockNumber blknum;

			BufferGetTag(buf, &rlocator, &forknum, &blknum);
			elog(DEBUG1, "fixing corrupt FSM block %u, relation %u/%u/%u",
				 blknum, rlocator.spcOid, rlocator.dbOid, rlocator.relNumber);

		/* 确保我们持有一个排他锁 */
		if (!exclusive_lock_held)
			{
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
				LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
				exclusive_lock_held = true;
			}
			fsm_rebuild_page(page);
			MarkBufferDirtyHint(buf, false);
			goto restart;
		}
	}

	/* 我们现在位于最底层，处于一个具有足够空间的节点上。 */
	slot = nodeno - NonLeafNodesPerPage;

	/*
	 * 更新下一个目标指针。注意，即便我们只持有共享锁，也会做这件事，
	 * 其理由是：使用共享锁、偶尔得到一个错乱的下一个指针，总比承受
	 * 排他锁带来的并发代价要好。
	 *
	 * 环绕情况在本函数开头已经处理。
	 */
	fsmpage->fp_next_slot = slot + (advancenext ? 1 : 0);

	return slot;
}

/*
 * 将所有编号 >= nslots 的槽位的可用空间清零。
 * 若页面被修改则返回 true。
 */
bool
fsm_truncate_avail(Page page, int nslots)
{
	FSMPage		fsmpage = (FSMPage) PageGetContents(page);
	uint8	   *ptr;
	bool		changed = false;

	Assert(nslots >= 0 && nslots < LeafNodesPerPage);

	/* 清空所有被截断的叶子节点 */
	ptr = &fsmpage->fp_nodes[NonLeafNodesPerPage + nslots];
	for (; ptr < &fsmpage->fp_nodes[NodesPerPage]; ptr++)
	{
		if (*ptr != 0)
			changed = true;
		*ptr = 0;
	}

	/* 修复上层节点。 */
	if (changed)
		fsm_rebuild_page(page);

	return changed;
}

/*
 * 重建一个页面的上层结构。若页面被修改则返回 true。
 */
bool
fsm_rebuild_page(Page page)
{
	FSMPage		fsmpage = (FSMPage) PageGetContents(page);
	bool		changed = false;
	int			nodeno;

	/*
	 * 从最低的非叶子层、最后一个节点开始，逆向进行，遍历所有层的所有
	 * 非叶子节点，一直到达根节点。
	 */
	for (nodeno = NonLeafNodesPerPage - 1; nodeno >= 0; nodeno--)
	{
		int			lchild = leftchild(nodeno);
		int			rchild = lchild + 1;
		uint8		newvalue = 0;

		/* 我们最先检查的几个节点可能只有零个或一个孩子。 */
		if (lchild < NodesPerPage)
			newvalue = fsmpage->fp_nodes[lchild];

		if (rchild < NodesPerPage)
			newvalue = Max(newvalue,
						   fsmpage->fp_nodes[rchild]);

		if (fsmpage->fp_nodes[nodeno] != newvalue)
		{
			fsmpage->fp_nodes[nodeno] = newvalue;
			changed = true;
		}
	}

	return changed;
}
