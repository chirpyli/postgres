/*-------------------------------------------------------------------------
 *
 * bgwriter.c
 *
 * 后台写入器（bgwriter）是 Postgres 8.0 新增的功能。它尝试避免常规
 * 后端进程不得不自行写出脏共享缓冲区（它们通常只在需要释放共享缓冲区以读入
 * 另一个页面时才会这样做）。在理想情况下，所有共享缓冲区的写入都将由
 * 后台写入器进程发出。但是，如果 bgwriter 未能维持足够的干净共享缓冲区，
 * 常规后端进程仍有权自行发出写入。
 *
 * 从 Postgres 9.2 开始，bgwriter 不再处理检查点。
 *
 * 正常终止通过 SIGTERM 实现，该信号指示 bgwriter 调用 exit(0)。
 * 紧急终止通过 SIGQUIT 实现；与任何后端进程一样，bgwriter 收到 SIGQUIT
 * 后会直接 abort 并退出。
 *
 * 如果 bgwriter 意外退出，postmaster 会将其视为后端崩溃：共享内存可能
 * 已损坏，因此其余后端进程应通过 SIGQUIT 终止，然后启动恢复周期。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/bgwriter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlog.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/auxprocess.h"
#include "postmaster/bgwriter.h"
#include "postmaster/interrupt.h"
#include "storage/aio_subsys.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/condition_variable.h"
#include "storage/fd.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/smgr.h"
#include "storage/standby.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/timestamp.h"

/*
 * GUC 参数
 */
int			BgWriterDelay = 200;

/*
 * 当决定进入休眠模式时，应用于 BgWriterDelay 的倍数。
 * （也许这应该是可配置的？）
 */
#define HIBERNATE_FACTOR			50

/*
 * 备用快照写入 WAL 流的时间间隔，单位毫秒。
 */
#define LOG_SNAPSHOT_INTERVAL_MS 15000

/*
 * 上次发出 LogStandbySnapshot() 时的 LSN 和时间戳，
 * 用于避免在系统中没有其他写入活动时过于频繁或重复地执行该操作。
 */
static TimestampTz last_snapshot_ts;
static XLogRecPtr last_snapshot_lsn = InvalidXLogRecPtr;


/*
 * bgwriter 进程的主入口点
 *
 * 由 AuxiliaryProcessMain 调用，该函数已经创建了基本的执行环境，
 * 但尚未启用信号处理。
 */
void
BackgroundWriterMain(const void *startup_data, size_t startup_data_len)
{
	sigjmp_buf	local_sigjmp_buf;
	MemoryContext bgwriter_context;
	bool		prev_hibernate;
	WritebackContext wb_context;

	Assert(startup_data_len == 0);

	MyBackendType = B_BG_WRITER;
	AuxiliaryProcessMainCommon();

	/*
	 * 正确地接收或忽略可能发送给我们的信号。
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	/* SIGQUIT 处理函数已由 InitPostmasterChild 设置 */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SIG_IGN);

	/*
	 * 重置一些 postmaster 接受但此处不接受的信号
	 */
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * 我们刚刚启动，假定已经有一个关闭或恢复结束时的快照。
	 */
	last_snapshot_ts = GetCurrentTimestamp();

	/*
	 * 创建一个内存上下文，所有工作都在其中进行。这样做的目的是在错误恢复
	 * 过程中可以重置该上下文，从而避免可能的内存泄漏。以前这段代码直接在
	 * TopMemoryContext 中运行，但重置那个上下文将是非常糟糕的做法。
	 */
	bgwriter_context = AllocSetContextCreate(TopMemoryContext,
											 "Background Writer",
											 ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(bgwriter_context);

	WritebackContextInit(&wb_context, &bgwriter_flush_after);

	/*
	 * 如果遇到异常，处理会在此处恢复执行。
	 *
	 * 你可能会疑惑为什么不用 PG_TRY 构造包裹一个无限循环。
	 * 原因在于这里是异常栈的底部，因此使用 PG_TRY 的话，在 CATCH 部分
	 * 将完全没有异常处理器。而通过让最外层的 setjmp 始终保持活跃，
	 * 我们至少在错误恢复过程中还有从错误中恢复的机会。
	 * （如果因此陷入无限循环，它很快会因 elog.c 内部状态栈溢出而停止。）
	 *
	 * 注意我们使用了 sigsetjmp(..., 1)，以便当 longjmp 到此处时，
	 * 当前的信号掩码（即 BlockSig）会被恢复。因此，在完成错误恢复之前，
	 * SIGQUIT 之外的信号将被阻塞。这似乎会使 HOLD_INTERRUPTS()
	 * 调用显得多余，但实际上并非如此，因为 InterruptPending 可能已经
	 * 被设置了。
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* 由于没有使用 PG_TRY，必须手动重置错误栈 */
		error_context_stack = NULL;

		/* 清理期间阻止中断 */
		HOLD_INTERRUPTS();

		/* 将错误报告到服务器日志 */
		EmitErrorReport();

		/*
		 * 这些操作实际上只是 AbortTransaction() 的一个最小子集。
		 * bgwriter 中需要关注的资源并不多，但我们确实拥有 LWLocks、
		 * 缓冲区和临时文件。
		 */
		LWLockReleaseAll();
		ConditionVariableCancelSleep();
		pgaio_error_cleanup();
		UnlockBuffers();
		ReleaseAuxProcessResources(false);
		AtEOXact_Buffers(false);
		AtEOXact_SMgr();
		AtEOXact_Files(false);
		AtEOXact_HashTables(false);

		/*
		 * 现在返回到正常的顶层上下文，并为下次使用清理 ErrorContext。
		 */
		MemoryContextSwitchTo(bgwriter_context);
		FlushErrorState();

		/* 刷新顶层上下文中泄漏的任何数据 */
		MemoryContextReset(bgwriter_context);

		/* 重新初始化以避免重复错误导致问题 */
		WritebackContextInit(&wb_context, &bgwriter_flush_after);

		/* 现在可以再次允许中断 */
		RESUME_INTERRUPTS();

		/*
		 * 在任何错误后至少休眠 1 秒。写入错误可能会重复出现，
		 * 我们不希望以最快的速度塞满错误日志。
		 */
		pg_usleep(1000000L);

		/* 在此处报告等待结束，此时不再有进一步等待的可能 */
		pgstat_report_wait_end();
	}

	/* 现在可以处理 ereport(ERROR) 了 */
	PG_exception_stack = &local_sigjmp_buf;

	/*
	 * 解除信号阻塞（postmaster fork 我们时信号是被阻塞的）
	 */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * 在任何错误后重置休眠状态。
	 */
	prev_hibernate = false;

	/*
	 * 永久循环
	 */
	for (;;)
	{
		bool		can_hibernate;
		int			rc;

		/* 清除任何已挂起的唤醒事件 */
		ResetLatch(MyLatch);

		ProcessMainLoopInterrupts();

		/*
		 * 执行一轮脏缓冲区写入。
		 */
		can_hibernate = BgBufferSync(&wb_context);

		/* 将挂起的统计信息上报到累积统计系统 */
		pgstat_report_bgwriter();
		pgstat_report_wal(true);

		if (FirstCallSinceLastCheckpoint())
		{
		/*
		 * 在任何检查点之后，释放所有 smgr 对象。否则对于已删除的关系，
		 * 我们将永远不会释放它们，因为 bgwriter 不处理共享失效消息，
		 * 也不调用 AtEOXact_SMgr()。
		 */
			smgrdestroyall();
		}

		/*
		 * 每隔一段时间记录一个新的 xl_running_xacts，以便复制可以更快地
		 * 进入一致状态（考虑子事务溢出快照）并更频繁地清理资源（锁、
		 * KnownXids* 等）。这样做的成本相对较低，因此每 15 秒
		 * （LOG_SNAPSHOT_INTERVAL_MS）4 次/分钟似乎是可以接受的。
		 *
		 * 我们假定写入 xl_running_xacts 的间隔显著大于 BgWriterDelay，
		 * 因此不会使整体超时处理变得复杂，而是假定即使在休眠模式激活时，
		 * 我们也足够频繁地被调用。严格满足 LOG_SNAPSHOT_INTERVAL_MS
		 * 并不是那么重要。为了确保在空闲系统上不会不必要地唤醒磁盘，
		 * 我们会检查自上次记录运行事务以来是否有任何 WAL 插入。
		 *
		 * 我们在 bgwriter 中进行这一日志记录，因为它是唯一一个定期运行
		 * 并始终返回主循环的进程。例如，Checkpointer 在活跃时几乎不
		 * 在其主循环中，因此很难定期记录。
		 */
		if (XLogStandbyInfoActive() && !RecoveryInProgress())
		{
			TimestampTz timeout = 0;
			TimestampTz now = GetCurrentTimestamp();

			timeout = TimestampTzPlusMilliseconds(last_snapshot_ts,
												  LOG_SNAPSHOT_INTERVAL_MS);

			/*
			 * 仅在经过了足够长的时间并且自上次快照以来已插入了
			 * 有意义记录时才记录。必须使用 <= 而不是 < 进行比较，
			 * 因为 GetLastImportantRecPtr() 指向记录的起始位置，
			 * 而 last_snapshot_lsn 指向记录的刚刚结束之后。
			 */
			if (now >= timeout &&
				last_snapshot_lsn <= GetLastImportantRecPtr())
			{
				last_snapshot_lsn = LogStandbySnapshot();
				last_snapshot_ts = now;
			}
		}

		/*
		 * 休眠直到我们被信号唤醒或 BgWriterDelay 超时。
		 *
		 * 注意：BgBufferSync() 中的反馈控制循环期望我们每隔
		 * BgWriterDelay 毫秒就调用它一次。虽然精确到这一点对于
		 * 正确性来说并不关键，但如果我们偏离得太远，反馈循环可能
		 * 会出现异常行为。因此，避免让该进程负载过多的
		 * 锁存器事件，这些事件在正常运行期间可能频繁发生。
		 */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   BgWriterDelay /* ms */ , WAIT_EVENT_BGWRITER_MAIN);

		/*
		 * 如果没有锁存器事件且 BgBufferSync 报告没有任何活动，
		 * 则在"休眠"模式下延长睡眠时间，睡眠时长远远超过
		 * bgwriter_delay 指定的值。减少唤醒次数可以节省电力。
		 * 当后端进程重新开始使用缓冲区时，它会通过设置我们的锁存器
		 * 来唤醒我们。由于额外的睡眠仅在没有任何缓冲区分配发生时
		 * 才持续，这不应该严重扭曲 BgBufferSync 控制循环的行为；
		 * 本质上，控制循环会认为系统级的空闲间隔不存在。
		 *
		 * 这里存在一个竞态条件：后端可能在 BgBufferSync 看到分配计数为零
		 * 之后、在我们调用 StrategyNotifyBgWriter 之前分配一个缓冲区。
		 * 虽然我们是否休眠并不是什么关键问题，但我们通过仅在
		 * BgBufferSync 连续两个周期都报告无活动时才进入休眠来降低
		 * 这种可能性。另外，我们通过不永久休眠来缓解任何因错过唤醒
		 * 而可能产生的后果。
		 */
		if (rc == WL_TIMEOUT && can_hibernate && prev_hibernate)
		{
			/* 请求在下一次缓冲区分配时通知我们 */
			StrategyNotifyBgWriter(MyProcNumber);
			/* 休眠 ... */
			(void) WaitLatch(MyLatch,
							 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							 BgWriterDelay * HIBERNATE_FACTOR,
							 WAIT_EVENT_BGWRITER_HIBERNATE);
			/* 重置通知请求，以防我们是因超时醒来的 */
			StrategyNotifyBgWriter(-1);
		}

		prev_hibernate = can_hibernate;
	}
}
