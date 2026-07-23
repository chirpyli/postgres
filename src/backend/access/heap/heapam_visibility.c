/*-------------------------------------------------------------------------
 *
 * heapam_visibility.c
 *	  堆（heap）中存储的元组的可见性规则。
 *
 * NOTE: 所有 HeapTupleSatisfies 系列函数都会在发现插入或删除该元组的事务
 * 已经提交或中止（并且设置 hint 位是安全的）时，更新元组的 "hint" 状态位。
 * 如果 hint 位发生变化，会对传入的缓冲区调用 MarkBufferDirtyHint。
 * 调用者不仅必须持有 pin，还至少必须持有包含该元组的缓冲区的
 * 共享缓冲区内容锁。
 *
 * NOTE: 使用非 MVCC 快照时，必须先检查
 * TransactionIdIsInProgress（查看 PGPROC 数组），再检查
 * TransactionIdDidCommit（查看 pg_xact）。否则会出现竞态条件：
 * 我们可能会认为一个刚刚提交的事务崩溃了，因为没有任何测试能通过。
 * xact.c 会小心地在 PGPROC 数组中清除 MyProc->xid 之前，
 * 先把提交/中止信息记录到 pg_xact 中。这修复了上述问题，但也意味着
 * 存在一个窗口期，期间 TransactionIdIsInProgress 和 TransactionIdDidCommit
 * 都会返回 true。如果我们只检查 TransactionIdDidCommit，可能会认为一个元组
 * 已提交，而稍后的 GetSnapshotData 调用仍认为其所属事务处于进行中，
 * 从而导致应用层面的不一致。结论是：在所有代码路径中都必须先检查
 * TransactionIdIsInProgress，只有少数情况例外——即我们查看的是自身主事务的
 * 子事务，因此不可能存在竞态条件。
 *
 * 这里不能使用 TransactionIdDidAbort，因为它不会把崩溃时正在进行的事务
 * 视为已中止。我们通过排除法来确定事务是否已中止/崩溃。
 *
 * 使用 MVCC 快照时，我们依赖 XidInMVCCSnapshot 而不是
 * TransactionIdIsInProgress，但逻辑相同：在确认事务不再进行之前，
 * 不要检查 pg_xact。
 *
 *
 * 可见性函数总结：
 *
 *	 HeapTupleSatisfiesMVCC()
 *		  对提供的快照可见，排除当前命令
 *	 HeapTupleSatisfiesUpdate()
 *		  对瞬时快照可见，带有用户提供的命令计数器和更复杂的结果
 *	 HeapTupleSatisfiesSelf()
 *		  对瞬时快照和当前命令可见
 *	 HeapTupleSatisfiesDirty()
 *		  类似 HeapTupleSatisfiesSelf()，但包含进行中的事务
 *	 HeapTupleSatisfiesVacuum()
 *		  对任何正在运行的事务可见，供 VACUUM 使用
 *	 HeapTupleSatisfiesNonVacuumable()
 *		  HeapTupleSatisfiesVacuum 的快照风格 API
 *	 HeapTupleSatisfiesToast()
 *		  可见，除非属于被中断的 vacuum，用于 TOAST
 *	 HeapTupleSatisfiesAny()
 *		  所有元组都可见
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/heapam_visibility.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"


/*
 * SetHintBits()
 *
 * 在适当的时机，为元组设置提交/中止 hint 位。
 *
 * 仅当我们确定事务的提交记录保证在缓冲区之前被刷新到磁盘，或者该表是
 * 临时表或未记录表（崩溃后反正会被抹掉）时，设置事务已提交的 hint 位才是
 * 安全的。我们无法在此处修改页面的 LSN，因为我们可能只持有缓冲区的共享锁，
 * 因此只有当缓冲区的 LSN 已经比提交 LSN 更新时，我们才能用 LSN 来做互斥；
 * 否则我们只能暂且不设置 hint 位，留待将来重新检查该元组时再处理。
 *
 * 当我们把事务标记为已中止时，总是可以设置 hint 位。（heapam.c 中有些代码
 * 依赖这一点！）
 *
 * 此外，如果我们在清理 HEAP_MOVED_IN 或 HEAP_MOVED_OFF 条目，那么也总是可以
 * 设置 hint 位，因为 pre-9.0 的 VACUUM FULL 总是使用同步提交，并且不会移动
 * 那些之前没有被设置 hint 的元组。（这一点本子函数并不知道，而是由它的调用者
 * 负责。）注意：老式的 VACUUM FULL 已经不存在了，但只要我们还支持从 pre-9.0
 * 数据库原地升级，就不得不保留本模块对 MOVED_OFF/MOVED_IN 标志位的支持。
 *
 * 普通提交可能是异步的，因此对于这类情况，我们需要获取事务的 LSN，然后检查
 * 它是否已经被刷新。
 *
 * 调用者应当把要检查的事务的 XID 作为 xid 传入；如果不需要检查，则传入
 * InvalidTransactionId。
 */
static inline void
SetHintBits(HeapTupleHeader tuple, Buffer buffer,
			uint16 infomask, TransactionId xid)
{
	if (TransactionIdIsValid(xid))
	{
		/* 注意：此处的 xid 必须是已提交的！ */
		XLogRecPtr	commitLSN = TransactionIdGetCommitLSN(xid);

		if (BufferIsPermanent(buffer) && XLogNeedsFlush(commitLSN) &&
			BufferGetLSNAtomic(buffer) < commitLSN)
		{
			/* 尚未刷新且没有 LSN 互斥，因此不设置 hint */
			return;
		}
	}

	tuple->t_infomask |= infomask;
	MarkBufferDirtyHint(buffer, true);
}

/*
 * HeapTupleSetHintBits --- SetHintBits() 的导出版本
 *
 * 必须独立出来，原因在于 C99 对如何实现内联函数有着糟糕（brain-dead）的规定。
 */
void
HeapTupleSetHintBits(HeapTupleHeader tuple, Buffer buffer,
					 uint16 infomask, TransactionId xid)
{
	SetHintBits(tuple, buffer, infomask, xid);
}


/*
 * HeapTupleSatisfiesSelf
 *		当且仅当堆元组对"自身"有效时返回真。
 *
 * 预期行为参见 SNAPSHOT_MVCC 的定义。
 *
 * 注意：
 *		假定堆元组是有效的。
 *
 * "自身"的满足条件如下：
 *
 * ((Xmin == my-transaction &&				该行由当前事务更新，且
 *		(Xmax is null						它未被删除
 *		 [|| Xmax != my-transaction)])			[或它已被另一个事务删除]
 * ||
 *
 * (Xmin is committed &&						该行由已提交的事务修改，且
 *		(Xmax is null ||					该行尚未被删除，或
 *			(Xmax != my-transaction &&		该行已被另一个事务删除
 *			 Xmax is not committed)))		但该事务尚未被提交
 */
static bool
HeapTupleSatisfiesSelf(HeapTuple htup, Snapshot snapshot, Buffer buffer)
{
	HeapTupleHeader tuple = htup->t_data;

	Assert(ItemPointerIsValid(&htup->t_self));
	Assert(htup->t_tableOid != InvalidOid);

	if (!HeapTupleHeaderXminCommitted(tuple))
	{
		if (HeapTupleHeaderXminInvalid(tuple))
			return false;

		/* 用于 pre-9.0 二进制升级 */
		if (tuple->t_infomask & HEAP_MOVED_OFF)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (TransactionIdIsCurrentTransactionId(xvac))
				return false;
			if (!TransactionIdIsInProgress(xvac))
			{
				if (TransactionIdDidCommit(xvac))
				{
					SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
								InvalidTransactionId);
					return false;
				}
				SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
							InvalidTransactionId);
			}
		}
		/* 用于 pre-9.0 二进制升级 */
		else if (tuple->t_infomask & HEAP_MOVED_IN)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (!TransactionIdIsCurrentTransactionId(xvac))
			{
				if (TransactionIdIsInProgress(xvac))
					return false;
				if (TransactionIdDidCommit(xvac))
					SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
								InvalidTransactionId);
				else
				{
					SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
								InvalidTransactionId);
					return false;
				}
			}
		}
		else if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmin(tuple)))
		{
			if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid 无效 */
				return true;

			if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))	/* 不是删除者 */
				return true;

			if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
			{
				TransactionId xmax;

				xmax = HeapTupleGetUpdateXid(tuple);

				/* 不是 LOCKED_ONLY，所以必然存在 xmax */
				Assert(TransactionIdIsValid(xmax));

				/* 更新的子事务必然已中止 */
				if (!TransactionIdIsCurrentTransactionId(xmax))
					return true;
				else
					return false;
			}

			if (!TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmax(tuple)))
			{
				/* 删除的子事务必然已中止 */
				SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
							InvalidTransactionId);
				return true;
			}

			return false;
		}
		else if (TransactionIdIsInProgress(HeapTupleHeaderGetRawXmin(tuple)))
			return false;
		else if (TransactionIdDidCommit(HeapTupleHeaderGetRawXmin(tuple)))
			SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
						HeapTupleHeaderGetRawXmin(tuple));
		else
		{
			/* 它必然已中止或崩溃 */
			SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
						InvalidTransactionId);
			return false;
		}
	}

	/* 到了这里，插入事务已提交 */

	if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid 无效或已中止 */
		return true;

	if (tuple->t_infomask & HEAP_XMAX_COMMITTED)
	{
		if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
			return true;
		return false;			/* 被其他事务更新 */
	}

	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		TransactionId xmax;

		if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
			return true;

		xmax = HeapTupleGetUpdateXid(tuple);

		/* 不是 LOCKED_ONLY，所以必然存在 xmax */
		Assert(TransactionIdIsValid(xmax));

		if (TransactionIdIsCurrentTransactionId(xmax))
			return false;
		if (TransactionIdIsInProgress(xmax))
			return true;
		if (TransactionIdDidCommit(xmax))
			return false;
		/* 它必然已中止或崩溃 */
		return true;
	}

	if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmax(tuple)))
	{
		if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
			return true;
		return false;
	}

	if (TransactionIdIsInProgress(HeapTupleHeaderGetRawXmax(tuple)))
		return true;

	if (!TransactionIdDidCommit(HeapTupleHeaderGetRawXmax(tuple)))
	{
		/* 它必然已中止或崩溃 */
		SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
					InvalidTransactionId);
		return true;
	}

	/* xmax 事务已提交 */

	if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
	{
		SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
					InvalidTransactionId);
		return true;
	}

	SetHintBits(tuple, buffer, HEAP_XMAX_COMMITTED,
				HeapTupleHeaderGetRawXmax(tuple));
	return false;
}

/*
 * HeapTupleSatisfiesAny
 *		虚拟的"satisfies"例程：任何元组都满足 SnapshotAny。
 */
static bool
HeapTupleSatisfiesAny(HeapTuple htup, Snapshot snapshot, Buffer buffer)
{
	return true;
}

/*
 * HeapTupleSatisfiesToast
 *		当且仅当堆元组作为 TOAST 行有效时返回真。
 *
 * 预期行为参见 SNAPSHOT_TOAST 的定义。
 *
 * 这是一个简化版本，只检查 VACUUM 的移动条件。它适用于 TOAST 的使用场景，
 * 因为 TOAST 实在不想自己做时间资格（time qual）检查；如果你能看到包含
 * TOAST 引用的主表行，你就应该能看到被 TOAST 化的值。然而，对 TOAST 表进行
 * 的 vacuum 独立于主表，万一这种 vacuum 中途失败，我们最好还是做这些检查。
 *
 * 除此之外，这也意味着你不能对 TOAST 表中的行执行 UPDATE。
 */
static bool
HeapTupleSatisfiesToast(HeapTuple htup, Snapshot snapshot,
						Buffer buffer)
{
	HeapTupleHeader tuple = htup->t_data;

	Assert(ItemPointerIsValid(&htup->t_self));
	Assert(htup->t_tableOid != InvalidOid);

	if (!HeapTupleHeaderXminCommitted(tuple))
	{
		if (HeapTupleHeaderXminInvalid(tuple))
			return false;

		/* 用于 pre-9.0 二进制升级 */
		if (tuple->t_infomask & HEAP_MOVED_OFF)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (TransactionIdIsCurrentTransactionId(xvac))
				return false;
			if (!TransactionIdIsInProgress(xvac))
			{
				if (TransactionIdDidCommit(xvac))
				{
					SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
								InvalidTransactionId);
					return false;
				}
				SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
							InvalidTransactionId);
			}
		}
		/* 用于 pre-9.0 二进制升级 */
		else if (tuple->t_infomask & HEAP_MOVED_IN)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (!TransactionIdIsCurrentTransactionId(xvac))
			{
				if (TransactionIdIsInProgress(xvac))
					return false;
				if (TransactionIdDidCommit(xvac))
					SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
								InvalidTransactionId);
				else
				{
					SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
								InvalidTransactionId);
					return false;
				}
			}
		}

		/*
		 * 一个无效的 Xmin 可能由推测插入（speculative insertion）留下，
		 * 该插入通过超级删除（super-deleting）元组而被取消。这也适用于
		 * 推测插入过程中创建的 TOAST 元组。
		 */
		else if (!TransactionIdIsValid(HeapTupleHeaderGetXmin(tuple)))
			return false;
	}

	/* 否则假定该元组对 TOAST 有效。 */
	return true;
}

/*
 * HeapTupleSatisfiesUpdate
 *
 *	本函数返回的详细结果码比本文件中大多数函数都要多，因为 UPDATE 需要
 *	了解的不仅仅是"它是否可见？"。它同时也允许传入用户提供的 CommandId，
 *	而不是依赖 CurrentCommandId。
 *
 *	可能返回的结果码有：
 *
 *	TM_Invisible：扫描开始时该元组根本不存在，例如它是由更晚的 CommandId 创建的。
 *
 *	TM_Ok：元组有效且可见，因此可以更新。
 *
 *	TM_SelfModified：元组由当前事务在当前的扫描开始之后更新。
 *
 *	TM_Updated：元组由已提交的事务更新（包括元组被移动到另一个分区的情况）。
 *
 *	TM_Deleted：元组由已提交的事务删除。
 *
 *	TM_BeingModified：元组正被一个进行中的、非当前事务的事务更新。
 *	（注意：这也包括元组被 MultiXact 共享锁定的情况，即使该 MultiXact 包含
 *	当前事务。想要区分这种情形的调用者必须自行检测。）
 */
TM_Result
HeapTupleSatisfiesUpdate(HeapTuple htup, CommandId curcid,
						 Buffer buffer)
{
	HeapTupleHeader tuple = htup->t_data;

	Assert(ItemPointerIsValid(&htup->t_self));
	Assert(htup->t_tableOid != InvalidOid);

	if (!HeapTupleHeaderXminCommitted(tuple))
	{
		if (HeapTupleHeaderXminInvalid(tuple))
			return TM_Invisible;

		/* 用于 pre-9.0 二进制升级 */
		if (tuple->t_infomask & HEAP_MOVED_OFF)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (TransactionIdIsCurrentTransactionId(xvac))
				return TM_Invisible;
			if (!TransactionIdIsInProgress(xvac))
			{
				if (TransactionIdDidCommit(xvac))
				{
					SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
								InvalidTransactionId);
					return TM_Invisible;
				}
				SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
							InvalidTransactionId);
			}
		}
		/* 用于 pre-9.0 二进制升级 */
		else if (tuple->t_infomask & HEAP_MOVED_IN)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (!TransactionIdIsCurrentTransactionId(xvac))
			{
				if (TransactionIdIsInProgress(xvac))
					return TM_Invisible;
				if (TransactionIdDidCommit(xvac))
					SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
								InvalidTransactionId);
				else
				{
					SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
								InvalidTransactionId);
					return TM_Invisible;
				}
			}
		}
		else if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmin(tuple)))
		{
			if (HeapTupleHeaderGetCmin(tuple) >= curcid)
				return TM_Invisible;	/* 扫描开始之后才插入 */

			if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid 无效 */
				return TM_Ok;

			if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
			{
				TransactionId xmax;

				xmax = HeapTupleHeaderGetRawXmax(tuple);

				/*
				 * 这里要小心：尽管这个元组是由我们自己的事务创建的，但它可能
				 * 被其他事务锁定——如果我们在更新它时，其原始版本正处于
				 * key-share 锁定状态的话。
				 */

				if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
				{
					if (MultiXactIdIsRunning(xmax, true))
						return TM_BeingModified;
					else
						return TM_Ok;
				}

				/*
				 * 如果锁持有者已经不存在，那么这个 Xmax 中就没有任何值得关注的
				 * 东西剩下了；否则，就将该元组报告为被锁定/已更新。
				 */
				if (!TransactionIdIsInProgress(xmax))
					return TM_Ok;
				return TM_BeingModified;
			}

			if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
			{
				TransactionId xmax;

				xmax = HeapTupleGetUpdateXid(tuple);

				/* 不是 LOCKED_ONLY，所以必然存在 xmax */
				Assert(TransactionIdIsValid(xmax));

				/* 删除的子事务必然已中止 */
				if (!TransactionIdIsCurrentTransactionId(xmax))
				{
					if (MultiXactIdIsRunning(HeapTupleHeaderGetRawXmax(tuple),
											 false))
						return TM_BeingModified;
					return TM_Ok;
				}
				else
				{
					if (HeapTupleHeaderGetCmax(tuple) >= curcid)
						return TM_SelfModified; /* 扫描开始之后才更新 */
					else
						return TM_Invisible;	/* 扫描开始之前已更新 */
				}
			}

			if (!TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmax(tuple)))
			{
				/* 删除的子事务必然已中止 */
				SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
							InvalidTransactionId);
				return TM_Ok;
			}

			if (HeapTupleHeaderGetCmax(tuple) >= curcid)
				return TM_SelfModified; /* 扫描开始之后才更新 */
			else
				return TM_Invisible;	/* 扫描开始之前已更新 */
		}
		else if (TransactionIdIsInProgress(HeapTupleHeaderGetRawXmin(tuple)))
			return TM_Invisible;
		else if (TransactionIdDidCommit(HeapTupleHeaderGetRawXmin(tuple)))
			SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
						HeapTupleHeaderGetRawXmin(tuple));
		else
		{
			/* 它必然已中止或崩溃 */
			SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
						InvalidTransactionId);
			return TM_Invisible;
		}
	}

	/* 到了这里，插入事务已提交 */

	if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid 无效或已中止 */
		return TM_Ok;

	if (tuple->t_infomask & HEAP_XMAX_COMMITTED)
	{
		if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
			return TM_Ok;
		if (!ItemPointerEquals(&htup->t_self, &tuple->t_ctid))
			return TM_Updated;	/* 被其他事务更新 */
		else
			return TM_Deleted;	/* 被其他事务删除 */
	}

	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		TransactionId xmax;

		if (HEAP_LOCKED_UPGRADED(tuple->t_infomask))
			return TM_Ok;

		if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
		{
			if (MultiXactIdIsRunning(HeapTupleHeaderGetRawXmax(tuple), true))
				return TM_BeingModified;

			SetHintBits(tuple, buffer, HEAP_XMAX_INVALID, InvalidTransactionId);
			return TM_Ok;
		}

		xmax = HeapTupleGetUpdateXid(tuple);
		if (!TransactionIdIsValid(xmax))
		{
			if (MultiXactIdIsRunning(HeapTupleHeaderGetRawXmax(tuple), false))
				return TM_BeingModified;
		}

		/* 不是 LOCKED_ONLY，所以必然存在 xmax */
		Assert(TransactionIdIsValid(xmax));

		if (TransactionIdIsCurrentTransactionId(xmax))
		{
			if (HeapTupleHeaderGetCmax(tuple) >= curcid)
				return TM_SelfModified; /* 扫描开始之后才更新 */
			else
				return TM_Invisible;	/* 扫描开始之前已更新 */
		}

		if (MultiXactIdIsRunning(HeapTupleHeaderGetRawXmax(tuple), false))
			return TM_BeingModified;

		if (TransactionIdDidCommit(xmax))
		{
			if (!ItemPointerEquals(&htup->t_self, &tuple->t_ctid))
				return TM_Updated;
			else
				return TM_Deleted;
		}

		/*
		 * By here, the update in the Xmax is either aborted or crashed, but
		 * what about the other members?
		 */

		if (!MultiXactIdIsRunning(HeapTupleHeaderGetRawXmax(tuple), false))
		{
			/*
			 * There's no member, even just a locker, alive anymore, so we can
			 * mark the Xmax as invalid.
			 */
			SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
						InvalidTransactionId);
			return TM_Ok;
		}
		else
		{
			/* 有锁持有者正在运行 */
			return TM_BeingModified;
		}
	}

	if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmax(tuple)))
	{
		if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
			return TM_BeingModified;
		if (HeapTupleHeaderGetCmax(tuple) >= curcid)
			return TM_SelfModified; /* 扫描开始之后才更新 */
		else
			return TM_Invisible;	/* 扫描开始之前已更新 */
	}

	if (TransactionIdIsInProgress(HeapTupleHeaderGetRawXmax(tuple)))
		return TM_BeingModified;

	if (!TransactionIdDidCommit(HeapTupleHeaderGetRawXmax(tuple)))
	{
		/* 它必然已中止或崩溃 */
		SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
					InvalidTransactionId);
		return TM_Ok;
	}

	/* xmax 事务已提交 */

	if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
	{
		SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
					InvalidTransactionId);
		return TM_Ok;
	}

	SetHintBits(tuple, buffer, HEAP_XMAX_COMMITTED,
				HeapTupleHeaderGetRawXmax(tuple));
	if (!ItemPointerEquals(&htup->t_self, &tuple->t_ctid))
		return TM_Updated;		/* 被其他事务更新 */
	else
		return TM_Deleted;		/* 被其他事务删除 */
}

/*
 * HeapTupleSatisfiesDirty
 *		当且仅当堆元组有效（包含进行中事务所产生的影响）时返回真。
 *
 * 预期行为参见 SNAPSHOT_DIRTY 的定义。
 *
 * 就当前事务以及已提交/已中止事务的影响而言，它本质上与
 * HeapTupleSatisfiesSelf 类似。不过，我们还会包含其他仍在进行中的事务所
 * 产生的影响。
 *
 * 一个特殊的技巧是：传入的 snapshot 结构体被当作输出参数使用，用来返回
 * 影响该元组的并发事务的 xid。如果元组的 xmin 是另一个仍在进行中的事务，
 * 则将 snapshot->xmin 设为该元组的 xmin；否则，若元组的 xmin 是已提交有效、
 * 已提交死亡或是我自己的事务，则设为 InvalidTransactionId。snapshot->xmax
 * 与元组的 xmax 同理。如果元组是推测插入（speculatively）的，即插入者可能
 * 在不中止整个事务的情况下回退该插入，那么相关的 token 也会在
 * snapshot->speculativeToken 中返回。
 */
static bool
HeapTupleSatisfiesDirty(HeapTuple htup, Snapshot snapshot,
						Buffer buffer)
{
	HeapTupleHeader tuple = htup->t_data;

	Assert(ItemPointerIsValid(&htup->t_self));
	Assert(htup->t_tableOid != InvalidOid);

	snapshot->xmin = snapshot->xmax = InvalidTransactionId;
	snapshot->speculativeToken = 0;

	if (!HeapTupleHeaderXminCommitted(tuple))
	{
		if (HeapTupleHeaderXminInvalid(tuple))
			return false;

		/* 用于 pre-9.0 二进制升级 */
		if (tuple->t_infomask & HEAP_MOVED_OFF)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (TransactionIdIsCurrentTransactionId(xvac))
				return false;
			if (!TransactionIdIsInProgress(xvac))
			{
				if (TransactionIdDidCommit(xvac))
				{
					SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
								InvalidTransactionId);
					return false;
				}
				SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
							InvalidTransactionId);
			}
		}
		/* 用于 pre-9.0 二进制升级 */
		else if (tuple->t_infomask & HEAP_MOVED_IN)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (!TransactionIdIsCurrentTransactionId(xvac))
			{
				if (TransactionIdIsInProgress(xvac))
					return false;
				if (TransactionIdDidCommit(xvac))
					SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
								InvalidTransactionId);
				else
				{
					SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
								InvalidTransactionId);
					return false;
				}
			}
		}
		else if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmin(tuple)))
		{
			if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid 无效 */
				return true;

			if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))	/* 不是删除者 */
				return true;

			if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
			{
				TransactionId xmax;

				xmax = HeapTupleGetUpdateXid(tuple);

				/* 不是 LOCKED_ONLY，所以必然存在 xmax */
				Assert(TransactionIdIsValid(xmax));

				/* 更新的子事务必然已中止 */
				if (!TransactionIdIsCurrentTransactionId(xmax))
					return true;
				else
					return false;
			}

			if (!TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmax(tuple)))
			{
				/* 删除的子事务必然已中止 */
				SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
							InvalidTransactionId);
				return true;
			}

			return false;
		}
		else if (TransactionIdIsInProgress(HeapTupleHeaderGetRawXmin(tuple)))
		{
			/*
			 * Return the speculative token to caller.  Caller can worry about
			 * xmax, since it requires a conclusively locked row version, and
			 * a concurrent update to this tuple is a conflict of its
			 * purposes.
			 */
			if (HeapTupleHeaderIsSpeculative(tuple))
			{
				snapshot->speculativeToken =
					HeapTupleHeaderGetSpeculativeToken(tuple);

				Assert(snapshot->speculativeToken != 0);
			}

			snapshot->xmin = HeapTupleHeaderGetRawXmin(tuple);
			/* XXX 我们是不是应该继续向下去检查 xmax？ */
			return true;		/* 由其他事务插入 */
		}
		else if (TransactionIdDidCommit(HeapTupleHeaderGetRawXmin(tuple)))
			SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
						HeapTupleHeaderGetRawXmin(tuple));
		else
		{
			/* 它必然已中止或崩溃 */
			SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
						InvalidTransactionId);
			return false;
		}
	}

	/* 到了这里，插入事务已提交 */

	if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid 无效或已中止 */
		return true;

	if (tuple->t_infomask & HEAP_XMAX_COMMITTED)
	{
		if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
			return true;
		return false;			/* 被其他事务更新 */
	}

	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		TransactionId xmax;

		if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
			return true;

		xmax = HeapTupleGetUpdateXid(tuple);

		/* 不是 LOCKED_ONLY，所以必然存在 xmax */
		Assert(TransactionIdIsValid(xmax));

		if (TransactionIdIsCurrentTransactionId(xmax))
			return false;
		if (TransactionIdIsInProgress(xmax))
		{
			snapshot->xmax = xmax;
			return true;
		}
		if (TransactionIdDidCommit(xmax))
			return false;
		/* 它必然已中止或崩溃 */
		return true;
	}

	if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmax(tuple)))
	{
		if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
			return true;
		return false;
	}

	if (TransactionIdIsInProgress(HeapTupleHeaderGetRawXmax(tuple)))
	{
		if (!HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
			snapshot->xmax = HeapTupleHeaderGetRawXmax(tuple);
		return true;
	}

	if (!TransactionIdDidCommit(HeapTupleHeaderGetRawXmax(tuple)))
	{
		/* 它必然已中止或崩溃 */
		SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
					InvalidTransactionId);
		return true;
	}

	/* xmax 事务已提交 */

	if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
	{
		SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
					InvalidTransactionId);
		return true;
	}

	SetHintBits(tuple, buffer, HEAP_XMAX_COMMITTED,
				HeapTupleHeaderGetRawXmax(tuple));
	return false;				/* 被其他事务更新 */
}

/*
 * HeapTupleSatisfiesMVCC
 *		当且仅当堆元组对给定的 MVCC 快照有效时返回真。
 *
 * 预期行为参见 SNAPSHOT_MVCC 的定义。
 *
 * 注意，在这里，如果根据我们的快照，插入/删除该元组的事务仍在进行中，那么
 * 我们不会更新元组状态的 hint 位，即便它实际上现在已经提交或中止了。这是
 * 有意为之的。检查事务的真实状态需要访问高争用的共享数据结构，从而产生我们
 * 宁可避免的争用，并且无论如何它也不会改变可见性检查的结果。hint 位会被第一个
 * 拥有足够新、能够看到插入/删除事务已经完成的快照的访问者更新。与此同时，
 * 不设置 hint 位带来的代价基本上是：每次 HeapTupleSatisfiesMVCC 调用除了
 * 需要执行 XidInMVCCSnapshot 之外（但后者无论如何都得执行），还要执行
 * TransactionIdIsCurrentTransactionId。在旧的实现中，我们试图尽快设置 hint 位，
 * 结果改为在每次调用中都执行 TransactionIdIsInProgress——但在插入/删除事务
 * 仍在进行期间这毫无作用——这反而消耗了更多 CPU 周期，并增加了对 ProcArrayLock
 * 的争用。
 */
static bool
HeapTupleSatisfiesMVCC(HeapTuple htup, Snapshot snapshot,
					   Buffer buffer)
{
	HeapTupleHeader tuple = htup->t_data;

	/*
	 * 断言调用者已经注册了该快照。本函数本身并不关心注册与否，但一般来说
	 * 你不应该去使用未注册的快照，因为它可能在使用期间被置为无效；而这里
	 * 正是一个方便地检查这一点的地方。
	 */
	Assert(snapshot->regd_count > 0 || snapshot->active_count > 0);

	Assert(ItemPointerIsValid(&htup->t_self));
	Assert(htup->t_tableOid != InvalidOid);

	if (!HeapTupleHeaderXminCommitted(tuple))
	{
		if (HeapTupleHeaderXminInvalid(tuple))
			return false;

		/* 用于 pre-9.0 二进制升级 */
		if (tuple->t_infomask & HEAP_MOVED_OFF)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (TransactionIdIsCurrentTransactionId(xvac))
				return false;
			if (!XidInMVCCSnapshot(xvac, snapshot))
			{
				if (TransactionIdDidCommit(xvac))
				{
					SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
								InvalidTransactionId);
					return false;
				}
				SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
							InvalidTransactionId);
			}
		}
		/* 用于 pre-9.0 二进制升级 */
		else if (tuple->t_infomask & HEAP_MOVED_IN)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (!TransactionIdIsCurrentTransactionId(xvac))
			{
				if (XidInMVCCSnapshot(xvac, snapshot))
					return false;
				if (TransactionIdDidCommit(xvac))
					SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
								InvalidTransactionId);
				else
				{
					SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
								InvalidTransactionId);
					return false;
				}
			}
		}
		else if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmin(tuple)))
		{
			if (HeapTupleHeaderGetCmin(tuple) >= snapshot->curcid)
				return false;	/* 扫描开始之后才插入 */

			if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid 无效 */
				return true;

			if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))	/* 不是删除者 */
				return true;

			if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
			{
				TransactionId xmax;

				xmax = HeapTupleGetUpdateXid(tuple);

				/* 不是 LOCKED_ONLY，所以必然存在 xmax */
				Assert(TransactionIdIsValid(xmax));

				/* 更新的子事务必然已中止 */
				if (!TransactionIdIsCurrentTransactionId(xmax))
					return true;
				else if (HeapTupleHeaderGetCmax(tuple) >= snapshot->curcid)
					return true;	/* 扫描开始之后才更新 */
				else
					return false;	/* 扫描开始之前已更新 */
			}

			if (!TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmax(tuple)))
			{
				/* 删除的子事务必然已中止 */
				SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
							InvalidTransactionId);
				return true;
			}

			if (HeapTupleHeaderGetCmax(tuple) >= snapshot->curcid)
				return true;	/* 扫描开始之后才删除 */
			else
				return false;	/* deleted before scan started */
		}
		else if (XidInMVCCSnapshot(HeapTupleHeaderGetRawXmin(tuple), snapshot))
			return false;
		else if (TransactionIdDidCommit(HeapTupleHeaderGetRawXmin(tuple)))
			SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
						HeapTupleHeaderGetRawXmin(tuple));
		else
		{
			/* 它必然已中止或崩溃 */
			SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
						InvalidTransactionId);
			return false;
		}
	}
	else
	{
		/* xmin 已提交，但按照我们的快照来看可能还不算 */
		if (!HeapTupleHeaderXminFrozen(tuple) &&
			XidInMVCCSnapshot(HeapTupleHeaderGetRawXmin(tuple), snapshot))
			return false;		/* 视为仍在进行中 */
	}

	/* 到了这里，插入事务已提交 */

	if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid 无效或已中止 */
		return true;

	if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
		return true;

	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		TransactionId xmax;

		/* 上面已经检查过了 */
		Assert(!HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask));

		xmax = HeapTupleGetUpdateXid(tuple);

		/* 不是 LOCKED_ONLY，所以必然存在 xmax */
		Assert(TransactionIdIsValid(xmax));

		if (TransactionIdIsCurrentTransactionId(xmax))
		{
			if (HeapTupleHeaderGetCmax(tuple) >= snapshot->curcid)
				return true;	/* 扫描开始之后才删除 */
			else
				return false;	/* deleted before scan started */
		}
		if (XidInMVCCSnapshot(xmax, snapshot))
			return true;
		if (TransactionIdDidCommit(xmax))
			return false;		/* updating transaction committed */
		/* 它必然已中止或崩溃 */
		return true;
	}

	if (!(tuple->t_infomask & HEAP_XMAX_COMMITTED))
	{
		if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmax(tuple)))
		{
			if (HeapTupleHeaderGetCmax(tuple) >= snapshot->curcid)
				return true;	/* 扫描开始之后才删除 */
			else
				return false;	/* deleted before scan started */
		}

		if (XidInMVCCSnapshot(HeapTupleHeaderGetRawXmax(tuple), snapshot))
			return true;

		if (!TransactionIdDidCommit(HeapTupleHeaderGetRawXmax(tuple)))
		{
			/* 它必然已中止或崩溃 */
			SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
						InvalidTransactionId);
			return true;
		}

		/* xmax 事务已提交 */
		SetHintBits(tuple, buffer, HEAP_XMAX_COMMITTED,
					HeapTupleHeaderGetRawXmax(tuple));
	}
	else
	{
		/* xmax 已提交，但按照我们的快照来看可能还不算 */
		if (XidInMVCCSnapshot(HeapTupleHeaderGetRawXmax(tuple), snapshot))
			return true;		/* 视为仍在进行中 */
	}

	/* xmax 事务已提交 */

	return false;
}


/*
 * HeapTupleSatisfiesVacuum
 *
 *	为 VACUUM 的目的判定元组的状态。在这里，我们主要想知道的是，一个元组
 *	是否对*任何*正在运行的事务潜在可见。如果是，VACUUM 暂时还不能将其移除。
 *
 * OldestXmin 是一个截止 XID（从 GetOldestNonRemovableTransactionId() 获得）。
 * 被 XID >= OldestXmin 删除的元组被视为"最近死亡（recently dead）"；它们
 * 可能仍对某些打开的事务可见，因此即使我们看到删除事务已经提交，也不能
 * 移除它们。
 */
HTSV_Result
HeapTupleSatisfiesVacuum(HeapTuple htup, TransactionId OldestXmin,
						 Buffer buffer)
{
	TransactionId dead_after = InvalidTransactionId;
	HTSV_Result res;

	res = HeapTupleSatisfiesVacuumHorizon(htup, buffer, &dead_after);

	if (res == HEAPTUPLE_RECENTLY_DEAD)
	{
		Assert(TransactionIdIsValid(dead_after));

		if (TransactionIdPrecedes(dead_after, OldestXmin))
			res = HEAPTUPLE_DEAD;
	}
	else
		Assert(!TransactionIdIsValid(dead_after));

	return res;
}

/*
 * HeapTupleSatisfiesVacuum 及类似例程的实际工作函数。
 *
 * 与 HeapTupleSatisfiesVacuum 不同，本例程在遇到一个仍可能对某些后端可见的
 * 元组时，会把需要与可见性边界（horizon）做比较的 xid 存入 *dead_after，
 * 并返回 HEAPTUPLE_RECENTLY_DEAD。调用者随后可以自行与边界做比较。例如，
 * 在与不同的边界进行比较时，这就很有用。
 *
 * 注意：这里仍可能返回 HEAPTUPLE_DEAD，例如当插入事务中止时。
 */
HTSV_Result
HeapTupleSatisfiesVacuumHorizon(HeapTuple htup, Buffer buffer, TransactionId *dead_after)
{
	HeapTupleHeader tuple = htup->t_data;

	Assert(ItemPointerIsValid(&htup->t_self));
	Assert(htup->t_tableOid != InvalidOid);
	Assert(dead_after != NULL);

	*dead_after = InvalidTransactionId;

	/*
	 * 插入事务是否已经提交？
	 *
	 * 如果插入事务中止了，那么该元组从未对任何其它事务可见，因此我们可以
	 * 立即将其删除。
	 */
	if (!HeapTupleHeaderXminCommitted(tuple))
	{
		if (HeapTupleHeaderXminInvalid(tuple))
			return HEAPTUPLE_DEAD;
		/* 用于 pre-9.0 二进制升级 */
		else if (tuple->t_infomask & HEAP_MOVED_OFF)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (TransactionIdIsCurrentTransactionId(xvac))
				return HEAPTUPLE_DELETE_IN_PROGRESS;
			if (TransactionIdIsInProgress(xvac))
				return HEAPTUPLE_DELETE_IN_PROGRESS;
			if (TransactionIdDidCommit(xvac))
			{
				SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
							InvalidTransactionId);
				return HEAPTUPLE_DEAD;
			}
			SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
						InvalidTransactionId);
		}
		/* 用于 pre-9.0 二进制升级 */
		else if (tuple->t_infomask & HEAP_MOVED_IN)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (TransactionIdIsCurrentTransactionId(xvac))
				return HEAPTUPLE_INSERT_IN_PROGRESS;
			if (TransactionIdIsInProgress(xvac))
				return HEAPTUPLE_INSERT_IN_PROGRESS;
			if (TransactionIdDidCommit(xvac))
				SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
							InvalidTransactionId);
			else
			{
				SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
							InvalidTransactionId);
				return HEAPTUPLE_DEAD;
			}
		}
		else if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmin(tuple)))
		{
			if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid 无效 */
				return HEAPTUPLE_INSERT_IN_PROGRESS;
		/* 只是被锁定？为了性能，先只做 infomask 检查 */
		if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask) ||
			HeapTupleHeaderIsOnlyLocked(tuple))
			return HEAPTUPLE_INSERT_IN_PROGRESS;
		/* 插入后又被同一个事务删除 */
			if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetUpdateXid(tuple)))
				return HEAPTUPLE_DELETE_IN_PROGRESS;
			/* 删除的子事务必然已中止 */
			return HEAPTUPLE_INSERT_IN_PROGRESS;
		}
		else if (TransactionIdIsInProgress(HeapTupleHeaderGetRawXmin(tuple)))
		{
			/*
			 * 在这里，通过查看 xmax 是有可能区分 INSERT/DELETE 是否正在进行中的——
			 * 但这对大多数调用者似乎没有好处，甚至对某些调用者有害。我们宁愿让
			 * 调用者去查看/等待 xmin，而不是 xmax。返回 INSERT_IN_PROGRESS 始终是
			 * 正确的，因为从其他后端的视角来看，这正是正在发生的事情。
			 */
			return HEAPTUPLE_INSERT_IN_PROGRESS;
		}
		else if (TransactionIdDidCommit(HeapTupleHeaderGetRawXmin(tuple)))
			SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
						HeapTupleHeaderGetRawXmin(tuple));
		else
		{
			/*
			 * 既非进行中，也非已提交，因此要么是已中止，要么是崩溃了
			 */
			SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
						InvalidTransactionId);
			return HEAPTUPLE_DEAD;
		}

		/*
		 * At this point the xmin is known committed, but we might not have
		 * been able to set the hint bit yet; so we can no longer Assert that
		 * it's set.
		 */
	}

	/*
	 * 好了，插入者已经提交，所以它在某些时刻是有效的。那么删除事务又如何呢？
	 */
	if (tuple->t_infomask & HEAP_XMAX_INVALID)
		return HEAPTUPLE_LIVE;

	if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
	{
		/*
		 * "Deleting" xact really only locked it, so the tuple is live in any
		 * case.  However, we should make sure that either XMAX_COMMITTED or
		 * XMAX_INVALID gets set once the xact is gone, to reduce the costs of
		 * examining the tuple for future xacts.
		 */
		if (!(tuple->t_infomask & HEAP_XMAX_COMMITTED))
		{
			if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
			{
			/*
			 * 如果这是一个 pre-pg_upgrade 的元组，那么该 multixact 不可能还在
			 * 运行；否则就需要检查。
			 */
				if (!HEAP_LOCKED_UPGRADED(tuple->t_infomask) &&
					MultiXactIdIsRunning(HeapTupleHeaderGetRawXmax(tuple),
										 true))
					return HEAPTUPLE_LIVE;
				SetHintBits(tuple, buffer, HEAP_XMAX_INVALID, InvalidTransactionId);
			}
			else
			{
				if (TransactionIdIsInProgress(HeapTupleHeaderGetRawXmax(tuple)))
					return HEAPTUPLE_LIVE;
				SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
							InvalidTransactionId);
			}
		}

		/*
		 * 我们其实并不关心 xmax 是提交了、中止了还是崩溃了。我们知道 xmax
		 * 确实锁定了该元组，但它并没有、也永远不会真正去更新它。
		 */

		return HEAPTUPLE_LIVE;
	}

	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		TransactionId xmax = HeapTupleGetUpdateXid(tuple);

		/* 上面已经检查过了 */
		Assert(!HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask));

		/* 不是 LOCKED_ONLY，所以必然存在 xmax */
		Assert(TransactionIdIsValid(xmax));

		if (TransactionIdIsInProgress(xmax))
			return HEAPTUPLE_DELETE_IN_PROGRESS;
		else if (TransactionIdDidCommit(xmax))
		{
			/*
			 * 由于锁持有者的存在，该 multixact 可能仍在运行。无论如何在低于
			 * xid 边界时都需要允许被修剪——否则我们可能会得到一个元组，其更新者
			 * 由于边界的缘故必须被移除，却没有被修剪掉。修剪该元组并不是问题，
			 * 因为任何剩余的锁持有者也会出现在更新的元组版本中。
			 */
			*dead_after = xmax;
			return HEAPTUPLE_RECENTLY_DEAD;
		}
		else if (!MultiXactIdIsRunning(HeapTupleHeaderGetRawXmax(tuple), false))
		{
			/*
			 * 既非进行中，也非已提交，因此要么是已中止，要么是崩溃了。
			 * 将 Xmax 标记为无效。
			 */
			SetHintBits(tuple, buffer, HEAP_XMAX_INVALID, InvalidTransactionId);
		}

		return HEAPTUPLE_LIVE;
	}

	if (!(tuple->t_infomask & HEAP_XMAX_COMMITTED))
	{
		if (TransactionIdIsInProgress(HeapTupleHeaderGetRawXmax(tuple)))
			return HEAPTUPLE_DELETE_IN_PROGRESS;
		else if (TransactionIdDidCommit(HeapTupleHeaderGetRawXmax(tuple)))
			SetHintBits(tuple, buffer, HEAP_XMAX_COMMITTED,
						HeapTupleHeaderGetRawXmax(tuple));
		else
		{
			/*
			 * 既非进行中，也非已提交，因此要么是已中止，要么是崩溃了
			 */
			SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
						InvalidTransactionId);
			return HEAPTUPLE_LIVE;
		}

		/*
		 * At this point the xmax is known committed, but we might not have
		 * been able to set the hint bit yet; so we can no longer Assert that
		 * it's set.
		 */
	}

	/*
	 * 删除者已经提交，允许调用者检查它是否"足够新"，以至于某些打开的事务
	 * 仍可能看到该元组。
	 */
	*dead_after = HeapTupleHeaderGetRawXmax(tuple);
	return HEAPTUPLE_RECENTLY_DEAD;
}


/*
 * HeapTupleSatisfiesNonVacuumable
 *
 *	如果元组可能对某些事务可见，则返回真；如果它对所有人都确定已死（即
 *	可被 vacuum）则返回假。
 *
 *	预期行为参见 SNAPSHOT_NON_VACUUMABLE 的定义。
 *
 *	这是 HeapTupleSatisfiesVacuum 的一个接口，可通过 HeapTupleSatisfiesSnapshot
 *	调用，从而可以借助 Snapshot 来使用。snapshot->vistest 必须已经用要使用的
 *	边界（horizon）设置好。
 */
static bool
HeapTupleSatisfiesNonVacuumable(HeapTuple htup, Snapshot snapshot,
								Buffer buffer)
{
	TransactionId dead_after = InvalidTransactionId;
	HTSV_Result res;

	res = HeapTupleSatisfiesVacuumHorizon(htup, buffer, &dead_after);

	if (res == HEAPTUPLE_RECENTLY_DEAD)
	{
		Assert(TransactionIdIsValid(dead_after));

		if (GlobalVisTestIsRemovableXid(snapshot->vistest, dead_after))
			res = HEAPTUPLE_DEAD;
	}
	else
		Assert(!TransactionIdIsValid(dead_after));

	return res != HEAPTUPLE_DEAD;
}


/*
 * HeapTupleIsSurelyDead
 *
 *	廉价地判断一个元组是否对所有旁观者确定已死。
 *	我们有时会用本函数代替 HeapTupleSatisfiesVacuum，当该元组刚刚被另一个
 *	可见性例程（通常是 HeapTupleSatisfiesMVCC）测试过，因此任何可以设置的
 *	hint 位应该都已经被设置了。我们假设如果没有设置任何 hint 位，那么 xmin
 *	或 xmax 事务仍在进行中。因此本函数比 HeapTupleSatisfiesVacuum 更快，因为
 *	我们既不查询 procarray，也不查询 CLOG。
 *	在存疑时返回 false 是可以的，但我们只有在元组确实可以被移除时才必须
 *	返回 true。
 */
bool
HeapTupleIsSurelyDead(HeapTuple htup, GlobalVisState *vistest)
{
	HeapTupleHeader tuple = htup->t_data;

	Assert(ItemPointerIsValid(&htup->t_self));
	Assert(htup->t_tableOid != InvalidOid);

	/*
	 * 如果插入事务被标记为无效，那么它已中止，该元组确定已死。如果它既未被
	 * 标记为已提交，也未被标记为无效，那么我们假定它还存活（因为这里假定
	 * 所有相关的 hint 位都在刚刚被设置了）。
	 */
	if (!HeapTupleHeaderXminCommitted(tuple))
		return HeapTupleHeaderXminInvalid(tuple);

	/*
	 * 如果插入事务提交了，但任何删除事务中止了，那么该元组仍然存活。
	 */
	if (tuple->t_infomask & HEAP_XMAX_INVALID)
		return false;

	/*
	 * 如果 XMAX 只是一个锁，那么该元组仍然存活。
	 */
	if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
		return false;

	/*
	 * 如果 Xmax 是一个 MultiXact，它可能是死的也可能是活的，但我们不去检查
	 * pg_multixact 就无法知道。
	 */
	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
		return false;

	/* If deleter isn't known to have committed, assume it's still running. */
	if (!(tuple->t_infomask & HEAP_XMAX_COMMITTED))
		return false;

	/* 删除者已提交，因此如果 XID 足够旧，元组就是死的。 */
	return GlobalVisTestIsRemovableXid(vistest,
									   HeapTupleHeaderGetRawXmax(tuple));
}

/*
 * 该元组是否真的只是被锁定了？也就是说，它是否没有被更新？
 *
 * 如果锁持有者不是一个 multi，那么只检查 infomask 位就很容易判断；否则我们
 * 需要确认更新事务没有中止。
 *
 * 本函数放在这里，是因为它遵循本文件开头所列出的同样的可见性规则。
 */
bool
HeapTupleHeaderIsOnlyLocked(HeapTupleHeader tuple)
{
	TransactionId xmax;

	/* if there's no valid Xmax, then there's obviously no update either */
	if (tuple->t_infomask & HEAP_XMAX_INVALID)
		return true;

	if (tuple->t_infomask & HEAP_XMAX_LOCK_ONLY)
		return true;

	/* invalid xmax means no update */
	if (!TransactionIdIsValid(HeapTupleHeaderGetRawXmax(tuple)))
		return true;

	/*
	 * if HEAP_XMAX_LOCK_ONLY is not set and not a multi, then this must
	 * necessarily have been updated
	 */
	if (!(tuple->t_infomask & HEAP_XMAX_IS_MULTI))
		return false;

	/* ……但如果是 multi，那么更新 Xid 也许已经中止了。 */
	xmax = HeapTupleGetUpdateXid(tuple);

	/* 不是 LOCKED_ONLY，所以必然存在 xmax */
	Assert(TransactionIdIsValid(xmax));

	if (TransactionIdIsCurrentTransactionId(xmax))
		return false;
	if (TransactionIdIsInProgress(xmax))
		return false;
	if (TransactionIdDidCommit(xmax))
		return false;

	/*
	 * not current, not in progress, not committed -- must have aborted or
	 * crashed
	 */
	return true;
}

/*
 * 检查事务 id 'xid' 是否位于预先排序的数组 'xip' 中。
 */
static bool
TransactionIdInArray(TransactionId xid, TransactionId *xip, Size num)
{
	return num > 0 &&
		bsearch(&xid, xip, num, sizeof(TransactionId), xidComparator) != NULL;
}

/*
 * 本函数所遵循的语义，请参见 HeapTupleSatisfiesMVCC 的注释。
 *
 * 只能用于来自系统目录（catalog）表的元组！
 *
 * 目前我们不需要支持 HEAP_MOVED_(IN|OFF)，因为我们只支持读取那些不可能在
 * 更旧的版本中创建的目录页面。
 *
 * 我们在这里不设置任何 hint 位，因为这样做似乎不太可能带来好处——那些位
 * 应该已经被正常的访问设置过了；而且这样做似乎也太危险，因为在时间旅行
 * （timetravel）期间设置它们所涉及的语义，比仅仅处理"当前"时要复杂得多。
 */
static bool
HeapTupleSatisfiesHistoricMVCC(HeapTuple htup, Snapshot snapshot,
							   Buffer buffer)
{
	HeapTupleHeader tuple = htup->t_data;
	TransactionId xmin = HeapTupleHeaderGetXmin(tuple);
	TransactionId xmax = HeapTupleHeaderGetRawXmax(tuple);

	Assert(ItemPointerIsValid(&htup->t_self));
	Assert(htup->t_tableOid != InvalidOid);

	/* inserting transaction aborted */
	if (HeapTupleHeaderXminInvalid(tuple))
	{
		Assert(!TransactionIdDidCommit(xmin));
		return false;
	}
	/* 检查它是否我们的某个 txid，顶层事务也在其中 */
	else if (TransactionIdInArray(xmin, snapshot->subxip, snapshot->subxcnt))
	{
		bool		resolved;
		CommandId	cmin = HeapTupleHeaderGetRawCommandId(tuple);
		CommandId	cmax = InvalidCommandId;

		/*
		 * another transaction might have (tried to) delete this tuple or
		 * cmin/cmax was stored in a combo CID. So we need to lookup the
		 * actual values externally.
		 */
		resolved = ResolveCminCmaxDuringDecoding(HistoricSnapshotGetTupleCids(), snapshot,
												 htup, buffer,
												 &cmin, &cmax);

		/*
		 * 如果我们还没有把 combo CID 解析为 cmin/cmax，这意味着我们
		 * 还没有解码这个 combo CID。这意味着 cmin 肯定在未来，而我们本不应该
		 * 看到这个元组。
		 *
		 * XXX 这仅适用于对进行中事务的解码。在常规的逻辑解码中，我们只在
		 * 提交时执行这段代码，到那时我们应该已经看到了所有相关的 combo CID。
		 * 因此理想情况下我们应该在这种情况下报错，但实际上这不会发生。如果
		 * 我们对此过于担心，可以在 ResolveCminCmaxDuringDecoding 内部加上一个
		 * elog。
		 *
		 * XXX 对于流式（streaming）的情况，我们可以跟踪所分配的最大 combo CID，
		 * 并以此为基础报错（当无法解析低于该观察到的最大值的 combo CID 时）。
		 */
		if (!resolved)
			return false;

		Assert(cmin != InvalidCommandId);

		if (cmin >= snapshot->curcid)
			return false;		/* 扫描开始之后才插入 */
		/* 继续向下 */
	}
		/* 在我们的 xmin 边界之前提交。执行一次普通的可见性检查。 */
	else if (TransactionIdPrecedes(xmin, snapshot->xmin))
	{
		Assert(!(HeapTupleHeaderXminCommitted(tuple) &&
				 !TransactionIdDidCommit(xmin)));

		/* 先检查 hint 位，之后再查阅 clog */
		if (!HeapTupleHeaderXminCommitted(tuple) &&
			!TransactionIdDidCommit(xmin))
			return false;
		/* 继续向下 */
	}
	/* 超出了我们的 xmax 边界，即不可见 */
	else if (TransactionIdFollowsOrEquals(xmin, snapshot->xmax))
	{
		return false;
	}
	/* 检查它是否是 [xmin, xmax) 中的一个已提交事务 */
	else if (TransactionIdInArray(xmin, snapshot->xip, snapshot->xcnt))
	{
		/* 继续向下 */
	}

	/*
	 * 以上都不是，即位于 [xmin, xmax) 之间，但还没有提交。也就是不可见。
	 */
	else
	{
		return false;
	}

	/* 到了这里，我们知道 xmin 是可见的，接下来检查 xmax */

	/* xid 无效或已中止 */
	if (tuple->t_infomask & HEAP_XMAX_INVALID)
		return true;
	/* 被锁定的元组总是可见的 */
	else if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
		return true;

	/*
	 * 如果我们正在查看用户表，或者有人对系统表执行了 SELECT ... FOR
	 * SHARE/UPDATE，那么在这里就可能看到 multis。
	 */
	else if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		xmax = HeapTupleGetUpdateXid(tuple);
	}

	/* 检查它是否我们的某个 txid，顶层事务也在其中 */
	if (TransactionIdInArray(xmax, snapshot->subxip, snapshot->subxcnt))
	{
		bool		resolved;
		CommandId	cmin;
		CommandId	cmax = HeapTupleHeaderGetRawCommandId(tuple);

		/* 查找实际的 cmin/cmax 值 */
		resolved = ResolveCminCmaxDuringDecoding(HistoricSnapshotGetTupleCids(), snapshot,
												 htup, buffer,
												 &cmin, &cmax);

		/*
		 * 如果我们还没有把 combo CID 解析为 cmin/cmax，这意味着我们
		 * 还没有解码这个 combo CID。这意味着 cmax 肯定在未来，而我们仍然应该
		 * 看到这个元组。
		 *
		 * XXX 这仅适用于对进行中事务的解码。在常规的逻辑解码中，我们只在
		 * 提交时执行这段代码，到那时我们应该已经看到了所有相关的 combo CID。
		 * 因此理想情况下我们应该在这种情况下报错，但实际上这不会发生。如果
		 * 我们对此过于担心，可以在 ResolveCminCmaxDuringDecoding 内部加上一个
		 * elog。
		 *
		 * XXX 对于流式（streaming）的情况，我们可以跟踪所分配的最大 combo CID，
		 * 并以此为基础报错（当无法解析低于该观察到的最大值的 combo CID 时）。
		 */
		if (!resolved || cmax == InvalidCommandId)
			return true;

		if (cmax >= snapshot->curcid)
			return true;		/* 扫描开始之后才删除 */
		else
			return false;		/* deleted before scan started */
	}
	/* 低于 xmin 边界，正常的事务状态有效 */
	else if (TransactionIdPrecedes(xmax, snapshot->xmin))
	{
		Assert(!(tuple->t_infomask & HEAP_XMAX_COMMITTED &&
				 !TransactionIdDidCommit(xmax)));

		/* 先检查 hint 位 */
		if (tuple->t_infomask & HEAP_XMAX_COMMITTED)
			return false;

		/* 检查 clog */
		return !TransactionIdDidCommit(xmax);
	}
	/* 高于 xmax 边界，我们不可能看到删除事务 */
	else if (TransactionIdFollowsOrEquals(xmax, snapshot->xmax))
		return true;
	/* xmax 位于 [xmin, xmax) 之间，检查已知已提交的数组 */
	else if (TransactionIdInArray(xmax, snapshot->xip, snapshot->xcnt))
		return false;
	/* xmax 位于 [xmin, xmax) 之间，但已知尚未提交 */
	else
		return true;
}

/*
 * HeapTupleSatisfiesVisibility
 *		当且仅当堆元组满足时间资格（time qual）时返回真。
 *
 * 注意：
 *	假定堆元组是有效的，且缓冲区至少被共享锁锁定。
 *
 *	HeapTuple 的 t_infomask 中的 hint 位可能会作为副作用被更新；
 *	如果是这样，所指示的缓冲区会被标记为脏。
 */
bool
HeapTupleSatisfiesVisibility(HeapTuple htup, Snapshot snapshot, Buffer buffer)
{
	switch (snapshot->snapshot_type)
	{
		case SNAPSHOT_MVCC:
			return HeapTupleSatisfiesMVCC(htup, snapshot, buffer);
		case SNAPSHOT_SELF:
			return HeapTupleSatisfiesSelf(htup, snapshot, buffer);
		case SNAPSHOT_ANY:
			return HeapTupleSatisfiesAny(htup, snapshot, buffer);
		case SNAPSHOT_TOAST:
			return HeapTupleSatisfiesToast(htup, snapshot, buffer);
		case SNAPSHOT_DIRTY:
			return HeapTupleSatisfiesDirty(htup, snapshot, buffer);
		case SNAPSHOT_HISTORIC_MVCC:
			return HeapTupleSatisfiesHistoricMVCC(htup, snapshot, buffer);
		case SNAPSHOT_NON_VACUUMABLE:
			return HeapTupleSatisfiesNonVacuumable(htup, snapshot, buffer);
	}

	return false;				/* 让编译器安静 */
}
