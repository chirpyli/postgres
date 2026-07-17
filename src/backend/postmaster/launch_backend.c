/*-------------------------------------------------------------------------
 *
 * launch_backend.c
 *	  用于启动后端进程及其他 postmaster 子进程的函数。
 *
 * 在 Unix 系统上，新的子进程通过 fork() 启动。它继承了
 * postmaster 中已初始化的所有全局变量和数据结构。fork 之后，子进程会关闭
 * 子进程中不需要的文件描述符，并建立检测父 postmaster 进程死亡的机制等。
 * 之后，它会根据子进程的类型调用相应的 Main 函数。
 *
 * 在 EXEC_BACKEND 模式下（用于 Windows，但也可在其他平台上启用以进行测试），
 * 子进程通过 fork() + exec()（或在 Windows 上通过 CreateProcess()）启动。
 * 它不会从 postmaster 继承状态，因此需要重新连接到共享内存、重新初始化
 * 全局变量、重新加载配置文件等，以使进程达到与 Unix 系统上 fork() 之后
 * 相同的状态。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/launch_backend.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "postmaster/autovacuum.h"
#include "postmaster/bgworker_internals.h"
#include "postmaster/bgwriter.h"
#include "postmaster/fork_process.h"
#include "postmaster/pgarch.h"
#include "postmaster/postmaster.h"
#include "postmaster/startup.h"
#include "postmaster/syslogger.h"
#include "postmaster/walsummarizer.h"
#include "postmaster/walwriter.h"
#include "replication/slotsync.h"
#include "replication/walreceiver.h"
#include "storage/dsm.h"
#include "storage/io_worker.h"
#include "storage/pg_shmem.h"
#include "tcop/backend_startup.h"
#include "utils/memutils.h"

#ifdef EXEC_BACKEND
#include "nodes/queryjumble.h"
#include "storage/pg_shmem.h"
#include "storage/spin.h"
#endif


#ifdef EXEC_BACKEND

#include "common/file_utils.h"
#include "storage/fd.h"
#include "storage/lwlock.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "tcop/tcopprot.h"
#include "utils/injection_point.h"

/* 可继承到客户端进程的套接字类型 */
#ifdef WIN32
typedef struct
{
	SOCKET		origsocket;		/* 原始套接字值；如果不是套接字则为 PGINVALID_SOCKET */
	WSAPROTOCOL_INFO wsainfo;
} InheritableSocket;
#else
typedef int InheritableSocket;
#endif

/*
 * 结构体包含所有传给 exec 启动的后端进程的变量
 */
typedef struct
{
	char		DataDir[MAXPGPATH];
#ifndef WIN32
	unsigned long UsedShmemSegID;
#else
	void	   *ShmemProtectiveRegion;
	HANDLE		UsedShmemSegID;
#endif
	void	   *UsedShmemSegAddr;
	slock_t    *ShmemLock;
#ifdef USE_INJECTION_POINTS
	struct InjectionPointsCtl *ActiveInjectionPoints;
#endif
	int			NamedLWLockTrancheRequests;
	NamedLWLockTranche *NamedLWLockTrancheArray;
	LWLockPadded *MainLWLockArray;
	slock_t    *ProcStructLock;
	PROC_HDR   *ProcGlobal;
	PGPROC	   *AuxiliaryProcs;
	PGPROC	   *PreparedXactProcs;
	volatile PMSignalData *PMSignalState;
	ProcSignalHeader *ProcSignal;
	pid_t		PostmasterPid;
	TimestampTz PgStartTime;
	TimestampTz PgReloadTime;
	pg_time_t	first_syslogger_file_time;
	bool		redirection_done;
	bool		IsBinaryUpgrade;
	bool		query_id_enabled;
	int			max_safe_fds;
	int			MaxBackends;
	int			num_pmchild_slots;
#ifdef WIN32
	HANDLE		PostmasterHandle;
	HANDLE		initial_signal_pipe;
	HANDLE		syslogPipe[2];
#else
	int			postmaster_alive_fds[2];
	int			syslogPipe[2];
#endif
	char		my_exec_path[MAXPGPATH];
	char		pkglib_path[MAXPGPATH];

	int			MyPMChildSlot;

	/*
	 * 这些字段仅由后端进程使用，但放在此处是因为传递套接字在 Windows 上
	 * 需要一些特殊处理。'client_sock' 是 postmaster_child_launch 的显式参数，
	 * 但在子进程中存储在 MyClientSocket 里。
	 */
	ClientSocket client_sock;
	InheritableSocket inh_sock;

	/*
	 * 额外的启动数据，内容取决于子进程的类型。
	 */
	size_t		startup_data_len;
	char		startup_data[FLEXIBLE_ARRAY_MEMBER];
} BackendParameters;

#define SizeOfBackendParameters(startup_data_len) (offsetof(BackendParameters, startup_data) + startup_data_len)

static void read_backend_variables(char *id, void **startup_data, size_t *startup_data_len);
static void restore_backend_variables(BackendParameters *param);

static bool save_backend_variables(BackendParameters *param, int child_slot,
								   ClientSocket *client_sock,
#ifdef WIN32
								   HANDLE childProcess, pid_t childPid,
#endif
								   const void *startup_data, size_t startup_data_len);

static pid_t internal_forkexec(const char *child_kind, int child_slot,
							   const void *startup_data, size_t startup_data_len,
							   ClientSocket *client_sock);

#endif							/* EXEC_BACKEND */

/*
 * 启动不同种类子进程所需的信息。
 */
typedef struct
{
	const char *name;
	void		(*main_fn) (const void *startup_data, size_t startup_data_len);
	bool		shmem_attach;
} child_process_kind;

static child_process_kind child_process_kinds[] = {
	[B_INVALID] = {"invalid", NULL, false},

	[B_BACKEND] = {"backend", BackendMain, true},
	[B_DEAD_END_BACKEND] = {"dead-end backend", BackendMain, true},
	[B_AUTOVAC_LAUNCHER] = {"autovacuum launcher", AutoVacLauncherMain, true},
	[B_AUTOVAC_WORKER] = {"autovacuum worker", AutoVacWorkerMain, true},
	[B_BG_WORKER] = {"bgworker", BackgroundWorkerMain, true},

	/*
	 * WAL 发送者最初作为普通后端进程启动，并在为复制完成客户端认证后
	 * 改变其类型。我们在此列出它是为了供 PostmasterChildName() 使用，
	 * 但不能直接启动它们。
	 */
	[B_WAL_SENDER] = {"wal sender", NULL, true},
	[B_SLOTSYNC_WORKER] = {"slot sync worker", ReplSlotSyncWorkerMain, true},

	[B_STANDALONE_BACKEND] = {"standalone backend", NULL, false},

	[B_ARCHIVER] = {"archiver", PgArchiverMain, true},
	[B_BG_WRITER] = {"bgwriter", BackgroundWriterMain, true},
	[B_CHECKPOINTER] = {"checkpointer", CheckpointerMain, true},
	[B_IO_WORKER] = {"io_worker", IoWorkerMain, true},
	[B_STARTUP] = {"startup", StartupProcessMain, true},
	[B_WAL_RECEIVER] = {"wal_receiver", WalReceiverMain, true},
	[B_WAL_SUMMARIZER] = {"wal_summarizer", WalSummarizerMain, true},
	[B_WAL_WRITER] = {"wal_writer", WalWriterMain, true},

	[B_LOGGER] = {"syslogger", SysLoggerMain, false},
};

const char *
PostmasterChildName(BackendType child_type)
{
	return child_process_kinds[child_type].name;
}

/*
 * 启动一个新的 postmaster 子进程。
 *
 * 无论是否使用 EXEC_BACKEND，子进程都会被恢复到大致相同的状态：如果合适，
 * 它会连接到共享内存，并且我们从 postmaster 继承的、在子进程中不需要的
 * fd 及其他资源都已经被关闭。
 *
 * 'child_slot' 是为该子进程保留的 PMChildFlags 数组下标。'startup_data'
 * 是传给子进程的一段可选的连续数据。
 */
pid_t
postmaster_child_launch(BackendType child_type, int child_slot,
						void *startup_data, size_t startup_data_len,
						ClientSocket *client_sock)
{
	pid_t		pid;

	Assert(IsPostmasterEnvironment && !IsUnderPostmaster);

	/* 记录 postmaster 发起进程创建的时间，用于日志 */
	if (IsExternalConnectionBackend(child_type))
		((BackendStartupData *) startup_data)->fork_started = GetCurrentTimestamp();

#ifdef EXEC_BACKEND
	pid = internal_forkexec(child_process_kinds[child_type].name, child_slot,
							startup_data, startup_data_len, client_sock);
	/* 子进程将进入 SubPostmasterMain */
#else							/* !EXEC_BACKEND */
	pid = fork_process();
	if (pid == 0)				/* 子进程 */
	{
		/* 记录并传递可能用于日志的计时信息 */
		if (IsExternalConnectionBackend(child_type))
		{
			conn_timing.socket_create =
				((BackendStartupData *) startup_data)->socket_created;
			conn_timing.fork_start =
				((BackendStartupData *) startup_data)->fork_started;
			conn_timing.fork_end = GetCurrentTimestamp();
		}

		/* 关闭 postmaster 的套接字 */
		ClosePostmasterPorts(child_type == B_LOGGER);

		/* 与 postmaster 脱离 */
		InitPostmasterChild();

		/* 如果不需要则断开共享内存。 */
		if (!child_process_kinds[child_type].shmem_attach)
		{
			dsm_detach_all();
			PGSharedMemoryDetach();
		}

		/*
		 * 以 TopMemoryContext 进入 Main 函数。启动数据是在 PostmasterContext
		 * 中分配的，因此我们还不能在此处释放它。Main 函数会在处理完启动
		 * 数据之后释放它。
		 */
		MemoryContextSwitchTo(TopMemoryContext);

		MyPMChildSlot = child_slot;
		if (client_sock)
		{
			MyClientSocket = palloc(sizeof(ClientSocket));
			memcpy(MyClientSocket, client_sock, sizeof(ClientSocket));
		}

		/*
		 * 运行相应的 Main 函数
		 */
		child_process_kinds[child_type].main_fn(startup_data, startup_data_len);
		pg_unreachable();		/* main_fn 永远不会返回 */
	}
#endif							/* EXEC_BACKEND */
	return pid;
}

#ifdef EXEC_BACKEND
#ifndef WIN32

/*
 * internal_forkexec 非 Win32 平台实现
 *
 * - 将后端变量写入参数文件
 * - 先 fork()，然后 exec() 子进程
 */
static pid_t
internal_forkexec(const char *child_kind, int child_slot,
				  const void *startup_data, size_t startup_data_len, ClientSocket *client_sock)
{
	static unsigned long tmpBackendFileNum = 0;
	pid_t		pid;
	char		tmpfilename[MAXPGPATH];
	size_t		paramsz;
	BackendParameters *param;
	FILE	   *fp;
	char	   *argv[4];
	char		forkav[MAXPGPATH];

	/*
	 * 使用 palloc0 以确保填充字节被初始化，防止 Valgrind 抱怨向文件写入
	 * 未初始化的字节。这不是性能关键路径，而且 win32 实现也会将填充字节
	 * 初始化为零，因此即使不使用 Valgrind 也这样做。
	 */
	paramsz = SizeOfBackendParameters(startup_data_len);
	param = palloc0(paramsz);
	if (!save_backend_variables(param, child_slot, client_sock, startup_data, startup_data_len))
	{
		pfree(param);
		return -1;				/* 日志由 save_backend_variables 输出 */
	}

	/* 计算临时文件名 */
	snprintf(tmpfilename, MAXPGPATH, "%s/%s.backend_var.%d.%lu",
			 PG_TEMP_FILES_DIR, PG_TEMP_FILE_PREFIX,
			 MyProcPid, ++tmpBackendFileNum);

	/* 打开文件 */
	fp = AllocateFile(tmpfilename, PG_BINARY_W);
	if (!fp)
	{
		/*
		 * 与 OpenTemporaryFileInTablespace 中一样，尝试创建临时文件目录，
		 * 并忽略错误。
		 */
		(void) MakePGDirectory(PG_TEMP_FILES_DIR);

		fp = AllocateFile(tmpfilename, PG_BINARY_W);
		if (!fp)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not create file \"%s\": %m",
							tmpfilename)));
			pfree(param);
			return -1;
		}
	}

	if (fwrite(param, paramsz, 1, fp) != 1)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", tmpfilename)));
		FreeFile(fp);
		pfree(param);
		return -1;
	}
	pfree(param);

	/* 释放文件 */
	if (FreeFile(fp))
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", tmpfilename)));
		return -1;
	}

	/* 正确设置 argv */
	argv[0] = "postgres";
	snprintf(forkav, MAXPGPATH, "--forkchild=%s", child_kind);
	argv[1] = forkav;
	/* 在 --forkchild 参数之后插入临时文件名 */
	argv[2] = tmpfilename;
	argv[3] = NULL;

	/* 在子进程中执行 execv */
	if ((pid = fork_process()) == 0)
	{
		if (execv(postgres_exec_path, argv) < 0)
		{
			ereport(LOG,
					(errmsg("could not execute server process \"%s\": %m",
							postgres_exec_path)));
			/* 这里已在子进程中，无法返回 */
			exit(1);
		}
	}

	return pid;					/* 父进程返回 pid；fork 失败时返回 -1 */
}
#else							/* WIN32 */

/*
 * internal_forkexec 的 Win32 实现
 *
 * - 使用 CreateProcess() 以挂起状态启动后端
 * - 将后端变量写入参数文件
 *	- 在此过程中，复制新进程继承所需的句柄和套接字
 * - 一旦后端参数文件写完，就恢复新进程的执行。
 */
static pid_t
internal_forkexec(const char *child_kind, int child_slot,
				  const void *startup_data, size_t startup_data_len, ClientSocket *client_sock)
{
	int			retry_count = 0;
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	char		cmdLine[MAXPGPATH * 2];
	HANDLE		paramHandle;
	BackendParameters *param;
	SECURITY_ATTRIBUTES sa;
	size_t		paramsz;
	char		paramHandleStr[32];
	int			l;

	paramsz = SizeOfBackendParameters(startup_data_len);

	/* 如果需要重试，从这里继续 */
retry:

	/* 建立用于传递参数的共享内存 */
	ZeroMemory(&sa, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	paramHandle = CreateFileMapping(INVALID_HANDLE_VALUE,
									&sa,
									PAGE_READWRITE,
									0,
									paramsz,
									NULL);
	if (paramHandle == INVALID_HANDLE_VALUE)
	{
		ereport(LOG,
				(errmsg("could not create backend parameter file mapping: error code %lu",
						GetLastError())));
		return -1;
	}
	param = MapViewOfFile(paramHandle, FILE_MAP_WRITE, 0, 0, paramsz);
	if (!param)
	{
		ereport(LOG,
				(errmsg("could not map backend parameter memory: error code %lu",
						GetLastError())));
		CloseHandle(paramHandle);
		return -1;
	}

	/* 构造命令行 */
#ifdef _WIN64
	sprintf(paramHandleStr, "%llu", (LONG_PTR) paramHandle);
#else
	sprintf(paramHandleStr, "%lu", (DWORD) paramHandle);
#endif
	l = snprintf(cmdLine, sizeof(cmdLine) - 1, "\"%s\" --forkchild=\"%s\" %s",
				 postgres_exec_path, child_kind, paramHandleStr);
	if (l >= sizeof(cmdLine))
	{
		ereport(LOG,
				(errmsg("subprocess command line too long")));
		UnmapViewOfFile(param);
		CloseHandle(paramHandle);
		return -1;
	}

	memset(&pi, 0, sizeof(pi));
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);

	/*
	 * 以挂起状态创建子进程。待我们写出参数文件之后，再将其恢复运行。
	 */
	if (!CreateProcess(NULL, cmdLine, NULL, NULL, TRUE, CREATE_SUSPENDED,
					   NULL, NULL, &si, &pi))
	{
		ereport(LOG,
				(errmsg("CreateProcess() call failed: %m (error code %lu)",
						GetLastError())));
		UnmapViewOfFile(param);
		CloseHandle(paramHandle);
		return -1;
	}

	if (!save_backend_variables(param, child_slot, client_sock,
								pi.hProcess, pi.dwProcessId,
								startup_data, startup_data_len))
	{
		/*
		 * 日志由 save_backend_variables 输出，但我们必须清理
		 * 这个半成品进程留下的烂摊子
		 */
		if (!TerminateProcess(pi.hProcess, 255))
			ereport(LOG,
					(errmsg_internal("could not terminate unstarted process: error code %lu",
									 GetLastError())));
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		UnmapViewOfFile(param);
		CloseHandle(paramHandle);
		return -1;				/* 日志由 save_backend_variables 输出 */
	}

	/* 释放现在已经继承给后端的参数共享内存 */
	if (!UnmapViewOfFile(param))
		ereport(LOG,
				(errmsg("could not unmap view of backend parameter file: error code %lu",
						GetLastError())));
	if (!CloseHandle(paramHandle))
		ereport(LOG,
				(errmsg("could not close handle to backend parameter file: error code %lu",
						GetLastError())));

	/*
	 * 在恢复子进程之前，预留我们主共享内存段所使用的内存区域。通常这应当
	 * 成功，但如果 ASLR 处于活动状态，则有时可能因栈或堆已被映射到该范围
	 * 而失败。在这种情况下，直接终止该进程并重试。
	 */
	if (!pgwin32_ReserveSharedMemoryRegion(pi.hProcess))
	{
		/* pgwin32_ReserveSharedMemoryRegion 已经记录了日志 */
		if (!TerminateProcess(pi.hProcess, 255))
			ereport(LOG,
					(errmsg_internal("could not terminate process that failed to reserve memory: error code %lu",
									 GetLastError())));
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		if (++retry_count < 100)
			goto retry;
		ereport(LOG,
				(errmsg("giving up after too many tries to reserve shared memory"),
				 errhint("This might be caused by ASLR or antivirus software.")));
		return -1;
	}

	/*
	 * 既然后端变量已经写出，我们启动子线程，使其在我们设置其余父进程
	 * 状态的同时开始初始化。
	 */
	if (ResumeThread(pi.hThread) == -1)
	{
		if (!TerminateProcess(pi.hProcess, 255))
		{
			ereport(LOG,
					(errmsg_internal("could not terminate unstartable process: error code %lu",
									 GetLastError())));
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
			return -1;
		}
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		ereport(LOG,
				(errmsg_internal("could not resume thread of unstarted process: error code %lu",
								 GetLastError())));
		return -1;
	}

	/* 在子进程死亡时设置通知 */
	pgwin32_register_deadchild_callback(pi.hProcess, pi.dwProcessId);

	/* 不要关闭 pi.hProcess，它现在由 deadchild 回调持有 */

	CloseHandle(pi.hThread);

	return pi.dwProcessId;
}
#endif							/* WIN32 */

/*
 * SubPostmasterMain -- 让经过 fork/exec 的进程进入等同于在 Unix 上直接
 *			fork 后的状态，然后分派到相应的处理逻辑。
 *
 * 前两个命令行参数预期为 "--forkchild=<name>"，其中 <name> 表示我们要
 * 成为哪种 postmaster 子进程，以及一个变量文件名，我们可以读取该文件
 * 来加载在 Unix 上本应由 fork() 继承的数据。
 */
void
SubPostmasterMain(int argc, char *argv[])
{
	void	   *startup_data;
	size_t		startup_data_len;
	char	   *child_kind;
	BackendType child_type;
	bool		found = false;
	TimestampTz fork_end;

	/* 在 EXEC_BACKEND 情况下，我们不会继承这些设置 */
	IsPostmasterEnvironment = true;
	whereToSendOutput = DestNone;

	/*
	 * 记录进程创建结束的时间，用于日志。我们不包含从共享内存复制数据
	 * 以及初始化后端所花费的时间。
	 */
	fork_end = GetCurrentTimestamp();

	/* 初始化必要的子系统（以确保 elog() 行为正常） */
	InitializeGUCOptions();

	/* 检查我们是否拿到了合适的参数 */
	if (argc != 3)
		elog(FATAL, "invalid subpostmaster invocation");

	/* 在 child_process_kinds 中查找对应条目 */
	if (strncmp(argv[1], "--forkchild=", 12) != 0)
		elog(FATAL, "invalid subpostmaster invocation (--forkchild argument missing)");
	child_kind = argv[1] + 12;
	found = false;
	for (int idx = 0; idx < lengthof(child_process_kinds); idx++)
	{
		if (strcmp(child_process_kinds[idx].name, child_kind) == 0)
		{
			child_type = (BackendType) idx;
			found = true;
			break;
		}
	}
	if (!found)
		elog(ERROR, "unknown child kind %s", child_kind);

	/* 读入变量文件 */
	read_backend_variables(argv[2], &startup_data, &startup_data_len);

	/* 关闭 postmaster 的套接字（一旦知晓就关闭） */
	ClosePostmasterPorts(child_type == B_LOGGER);

	/* 作为 postmaster 子进程进行初始化 */
	InitPostmasterChild();

	/*
	 * 如果合适，则以物理方式重新连接到共享内存段。我们希望在继续之前
	 * 完成这一步，以确保我们能连接到 postmaster 使用的相同地址。另一方面，
	 * 如果我们选择不重新连接，则可能还需要做其他清理工作。
	 *
	 * 如果在 Linux 上测试 EXEC_BACKEND，应在启动 postmaster 之前以 root
	 * 身份运行以下命令：
	 *
	 * sysctl -w kernel.randomize_va_space=0
	 *
	 * 这样可以避免由于随机化的栈和代码地址导致子进程的内存映射与父进程
	 * 不同，从而使有时无法在期望的地址上连接共享内存。完成后请将该设置
	 * 恢复为原来的值（通常为 '1' 或 '2'）。
	 */
	if (child_process_kinds[child_type].shmem_attach)
		PGSharedMemoryReAttach();
	else
		PGSharedMemoryNoReAttach();

	/* 读入其余的 GUC 变量 */
	read_nondefault_variables();

	/* 记录并传递 log_connections 可能需要的计时信息 */
	if (IsExternalConnectionBackend(child_type))
	{
		conn_timing.socket_create =
			((BackendStartupData *) startup_data)->socket_created;
		conn_timing.fork_start =
			((BackendStartupData *) startup_data)->fork_started;
		conn_timing.fork_end = fork_end;
	}

	/*
	 * 检查数据目录看起来是否合法，这也会检查数据目录的权限，并更新我们
	 * 之后创建文件时使用的 umask 以及文件/组相关变量。注意：这确实应该在
	 * 我们创建任何文件或目录之前完成。
	 */
	checkDataDir();

	/*
	 * （重新）读取控制文件，因为它包含配置信息。postmaster 已经读取过，
	 * 但本进程对此并不知情。
	 */
	LocalProcessControlFile(false);

	/*
	 * 重新加载所有由 postmaster 预加载的库。由于本进程是通过 exec 启动的，
	 * 那些库并没有跟随我们过来；但为了让行为与非 EXEC_BACKEND 模式一致，
	 * 我们应该把它们加载到所有子进程中。
	 */
	process_shared_preload_libraries();

	/* 恢复基本的共享内存指针 */
	if (UsedShmemSegAddr != NULL)
		InitShmemAccess(UsedShmemSegAddr);

	/*
	 * 运行相应的 Main 函数
	 */
	child_process_kinds[child_type].main_fn(startup_data, startup_data_len);
	pg_unreachable();			/* main_fn 永远不会返回 */
}

#ifndef WIN32
#define write_inheritable_socket(dest, src, childpid) ((*(dest) = (src)), true)
#define read_inheritable_socket(dest, src) (*(dest) = *(src))
#else
static bool write_duplicated_handle(HANDLE *dest, HANDLE src, HANDLE child);
static bool write_inheritable_socket(InheritableSocket *dest, SOCKET src,
									 pid_t childPid);
static void read_inheritable_socket(SOCKET *dest, InheritableSocket *src);
#endif


/* 将关键的后端变量保存到 BackendParameters 结构体中 */
static bool
save_backend_variables(BackendParameters *param,
					   int child_slot, ClientSocket *client_sock,
#ifdef WIN32
					   HANDLE childProcess, pid_t childPid,
#endif
					   const void *startup_data, size_t startup_data_len)
{
	if (client_sock)
		memcpy(&param->client_sock, client_sock, sizeof(ClientSocket));
	else
		memset(&param->client_sock, 0, sizeof(ClientSocket));
	if (!write_inheritable_socket(&param->inh_sock,
								  client_sock ? client_sock->sock : PGINVALID_SOCKET,
								  childPid))
		return false;

	strlcpy(param->DataDir, DataDir, MAXPGPATH);

	param->MyPMChildSlot = child_slot;

#ifdef WIN32
	param->ShmemProtectiveRegion = ShmemProtectiveRegion;
#endif
	param->UsedShmemSegID = UsedShmemSegID;
	param->UsedShmemSegAddr = UsedShmemSegAddr;

	param->ShmemLock = ShmemLock;

#ifdef USE_INJECTION_POINTS
	param->ActiveInjectionPoints = ActiveInjectionPoints;
#endif

	param->NamedLWLockTrancheRequests = NamedLWLockTrancheRequests;
	param->NamedLWLockTrancheArray = NamedLWLockTrancheArray;
	param->MainLWLockArray = MainLWLockArray;
	param->ProcStructLock = ProcStructLock;
	param->ProcGlobal = ProcGlobal;
	param->AuxiliaryProcs = AuxiliaryProcs;
	param->PreparedXactProcs = PreparedXactProcs;
	param->PMSignalState = PMSignalState;
	param->ProcSignal = ProcSignal;

	param->PostmasterPid = PostmasterPid;
	param->PgStartTime = PgStartTime;
	param->PgReloadTime = PgReloadTime;
	param->first_syslogger_file_time = first_syslogger_file_time;

	param->redirection_done = redirection_done;
	param->IsBinaryUpgrade = IsBinaryUpgrade;
	param->query_id_enabled = query_id_enabled;
	param->max_safe_fds = max_safe_fds;

	param->MaxBackends = MaxBackends;
	param->num_pmchild_slots = num_pmchild_slots;

#ifdef WIN32
	param->PostmasterHandle = PostmasterHandle;
	if (!write_duplicated_handle(&param->initial_signal_pipe,
								 pgwin32_create_signal_listener(childPid),
								 childProcess))
		return false;
#else
	memcpy(&param->postmaster_alive_fds, &postmaster_alive_fds,
		   sizeof(postmaster_alive_fds));
#endif

	memcpy(&param->syslogPipe, &syslogPipe, sizeof(syslogPipe));

	strlcpy(param->my_exec_path, my_exec_path, MAXPGPATH);

	strlcpy(param->pkglib_path, pkglib_path, MAXPGPATH);

	param->startup_data_len = startup_data_len;
	if (startup_data_len > 0)
		memcpy(param->startup_data, startup_data, startup_data_len);

	return true;
}

#ifdef WIN32
/*
 * 为在子进程中使用而复制一个句柄，并将该句柄在子进程中的实例写入
 * 参数文件。
 */
static bool
write_duplicated_handle(HANDLE *dest, HANDLE src, HANDLE childProcess)
{
	HANDLE		hChild = INVALID_HANDLE_VALUE;

	if (!DuplicateHandle(GetCurrentProcess(),
						 src,
						 childProcess,
						 &hChild,
						 0,
						 TRUE,
						 DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS))
	{
		ereport(LOG,
				(errmsg_internal("could not duplicate handle to be written to backend parameter file: error code %lu",
								 GetLastError())));
		return false;
	}

	*dest = hChild;
	return true;
}

/*
 * 为在子进程中使用而复制一个套接字，并将结果结构体写入参数文件。
 * 这是必需的，因为 Windows 上非常常见的一些 LSP（分层服务提供者，
 * 如杀毒软件、防火墙、下载管理器等）会破坏直接的套接字继承。
 */
static bool
write_inheritable_socket(InheritableSocket *dest, SOCKET src, pid_t childpid)
{
	dest->origsocket = src;
	if (src != 0 && src != PGINVALID_SOCKET)
	{
		/* 实际的套接字 */
		if (WSADuplicateSocket(src, childpid, &dest->wsainfo) != 0)
		{
			ereport(LOG,
					(errmsg("could not duplicate socket %d for use in backend: error code %d",
							(int) src, WSAGetLastError())));
			return false;
		}
	}
	return true;
}

/*
 * 读回复制的套接字结构体，并获取套接字描述符。
 */
static void
read_inheritable_socket(SOCKET *dest, InheritableSocket *src)
{
	SOCKET		s;

	if (src->origsocket == PGINVALID_SOCKET || src->origsocket == 0)
	{
		/* 不是真正的套接字！ */
		*dest = src->origsocket;
	}
	else
	{
		/* 实际的套接字，因此从结构体创建 */
		s = WSASocket(FROM_PROTOCOL_INFO,
					  FROM_PROTOCOL_INFO,
					  FROM_PROTOCOL_INFO,
					  &src->wsainfo,
					  0,
					  0);
		if (s == INVALID_SOCKET)
		{
			write_stderr("could not create inherited socket: error code %d\n",
						 WSAGetLastError());
			exit(1);
		}
		*dest = s;

		/*
		 * 为确保不会得到对同一套接字的两个引用，关闭原来的那个。
		 * （当继承确实生效时就会发生这种情况。）
		 */
		closesocket(src->origsocket);
	}
}
#endif

static void
read_backend_variables(char *id, void **startup_data, size_t *startup_data_len)
{
	BackendParameters param;

#ifndef WIN32
	/* 非 Win32 实现从文件读取 */
	FILE	   *fp;

	/* 打开文件 */
	fp = AllocateFile(id, PG_BINARY_R);
	if (!fp)
	{
		write_stderr("could not open backend variables file \"%s\": %m\n", id);
		exit(1);
	}

	if (fread(&param, sizeof(param), 1, fp) != 1)
	{
		write_stderr("could not read from backend variables file \"%s\": %m\n", id);
		exit(1);
	}

	/* 读取启动数据 */
	*startup_data_len = param.startup_data_len;
	if (param.startup_data_len > 0)
	{
		*startup_data = palloc(*startup_data_len);
		if (fread(*startup_data, *startup_data_len, 1, fp) != 1)
		{
			write_stderr("could not read startup data from backend variables file \"%s\": %m\n",
						 id);
			exit(1);
		}
	}
	else
		*startup_data = NULL;

	/* 释放文件 */
	FreeFile(fp);
	if (unlink(id) != 0)
	{
		write_stderr("could not remove file \"%s\": %m\n", id);
		exit(1);
	}
#else
	/* Win32 版本使用映射文件 */
	HANDLE		paramHandle;
	BackendParameters *paramp;

#ifdef _WIN64
	paramHandle = (HANDLE) _atoi64(id);
#else
	paramHandle = (HANDLE) atol(id);
#endif
	paramp = MapViewOfFile(paramHandle, FILE_MAP_READ, 0, 0, 0);
	if (!paramp)
	{
		write_stderr("could not map view of backend variables: error code %lu\n",
					 GetLastError());
		exit(1);
	}

	memcpy(&param, paramp, sizeof(BackendParameters));

	/* 读取启动数据 */
	*startup_data_len = param.startup_data_len;
	if (param.startup_data_len > 0)
	{
		*startup_data = palloc(paramp->startup_data_len);
		memcpy(*startup_data, paramp->startup_data, param.startup_data_len);
	}
	else
		*startup_data = NULL;

	if (!UnmapViewOfFile(paramp))
	{
		write_stderr("could not unmap view of backend variables: error code %lu\n",
					 GetLastError());
		exit(1);
	}

	if (!CloseHandle(paramHandle))
	{
		write_stderr("could not close handle to backend parameter variables: error code %lu\n",
					 GetLastError());
		exit(1);
	}
#endif

	restore_backend_variables(&param);
}

/* 从 BackendParameters 结构体中恢复关键的后端变量 */
static void
restore_backend_variables(BackendParameters *param)
{
	if (param->client_sock.sock != PGINVALID_SOCKET)
	{
		MyClientSocket = MemoryContextAlloc(TopMemoryContext, sizeof(ClientSocket));
		memcpy(MyClientSocket, &param->client_sock, sizeof(ClientSocket));
		read_inheritable_socket(&MyClientSocket->sock, &param->inh_sock);
	}

	SetDataDir(param->DataDir);

	MyPMChildSlot = param->MyPMChildSlot;

#ifdef WIN32
	ShmemProtectiveRegion = param->ShmemProtectiveRegion;
#endif
	UsedShmemSegID = param->UsedShmemSegID;
	UsedShmemSegAddr = param->UsedShmemSegAddr;

	ShmemLock = param->ShmemLock;

#ifdef USE_INJECTION_POINTS
	ActiveInjectionPoints = param->ActiveInjectionPoints;
#endif

	NamedLWLockTrancheRequests = param->NamedLWLockTrancheRequests;
	NamedLWLockTrancheArray = param->NamedLWLockTrancheArray;
	MainLWLockArray = param->MainLWLockArray;
	ProcStructLock = param->ProcStructLock;
	ProcGlobal = param->ProcGlobal;
	AuxiliaryProcs = param->AuxiliaryProcs;
	PreparedXactProcs = param->PreparedXactProcs;
	PMSignalState = param->PMSignalState;
	ProcSignal = param->ProcSignal;

	PostmasterPid = param->PostmasterPid;
	PgStartTime = param->PgStartTime;
	PgReloadTime = param->PgReloadTime;
	first_syslogger_file_time = param->first_syslogger_file_time;

	redirection_done = param->redirection_done;
	IsBinaryUpgrade = param->IsBinaryUpgrade;
	query_id_enabled = param->query_id_enabled;
	max_safe_fds = param->max_safe_fds;

	MaxBackends = param->MaxBackends;
	num_pmchild_slots = param->num_pmchild_slots;

#ifdef WIN32
	PostmasterHandle = param->PostmasterHandle;
	pgwin32_initial_signal_pipe = param->initial_signal_pipe;
#else
	memcpy(&postmaster_alive_fds, &param->postmaster_alive_fds,
		   sizeof(postmaster_alive_fds));
#endif

	memcpy(&syslogPipe, &param->syslogPipe, sizeof(syslogPipe));

	strlcpy(my_exec_path, param->my_exec_path, MAXPGPATH);

	strlcpy(pkglib_path, param->pkglib_path, MAXPGPATH);

	/*
	 * 我们需要恢复 fd.c 对外打开的 FD 的计数；为避免混乱，请确保在恢复
	 * max_safe_fds 之后再执行此操作。（注意：BackendInitialize 会为
	 * (*client_sock)->sock 处理此事。）
	 */
#ifndef WIN32
	if (postmaster_alive_fds[0] >= 0)
		ReserveExternalFD();
	if (postmaster_alive_fds[1] >= 0)
		ReserveExternalFD();
#endif
}

#endif							/* EXEC_BACKEND */
