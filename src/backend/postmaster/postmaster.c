/*-------------------------------------------------------------------------
 *
 * postmaster.c
 *	  本程序充当 POSTGRES 系统请求的调度中心。前端程序连接到
 *	  Postmaster，postmaster 再 fork 出一个新的后端进程来处理该
 *	  连接。
 *
 *	  postmaster 还管理系统级操作，例如启动和关闭。不过请注意，
 *	  postmaster 自身并不执行这些操作——它只是在适当的时机 fork 出
 *	  一个子进程去完成。当某个后端崩溃时，它也负责重置系统。
 *
 *	  postmaster 进程在启动期间创建共享内存和信号量池，但通常
 *	  不会自己去触碰它们。特别地，它不是后端 PGPROC 数组的成员，
 *	  因此不能参与锁管理器操作。让 postmaster 远离共享内存操作
 *	  使它更简单、更可靠。postmaster 几乎总能通过重置共享内存来
 *	  从单个后端的崩溃中恢复；如果它大量操作共享内存，就会容易
 *	  随着后端一起崩溃。
 *
 *	  当收到请求消息时，我们现在立即 fork()。子进程执行请求的
 *	  身份验证，成功后即成为后端。这使得认证代码可以用简单的
 *	  单线程风格编写（而不必像以前那样使用粗糙的“穷人的多任务”
 *	  代码）。更重要的是，它能确保 SSL 或 PAM 这类非多线程库中的
 *	  阻塞不会导致对其他客户端的拒绝服务。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/postmaster.c
 *
 * 说明
 *
 * 初始化：
 *		Postmaster 为后端设置共享内存数据结构。
 *
 * 同步：
 *		Postmaster 与后端共享内存，但应避免触碰共享内存，以免在
 *		崩溃的后端弄乱了锁或共享内存时被卡住。同样，Postmaster
 *		绝不应阻塞在前端客户端发来的消息上。
 *
 * 垃圾回收：
 *		Postmaster 在后端发生紧急退出和/或核心转储后进行清理。
 *
 * 错误报告：
 *		仅使用 write_stderr() 报告“交互式”错误（本质上就是命令行上
 *		的错误参数）。一旦 postmaster 启动，就使用 ereport()。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/param.h>
#include <netdb.h>
#include <limits.h>

#ifdef USE_BONJOUR
#include <dns_sd.h>
#endif

#ifdef USE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

#ifdef HAVE_PTHREAD_IS_THREADED_NP
#include <pthread.h>
#endif

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogrecovery.h"
#include "common/file_perm.h"
#include "common/pg_prng.h"
#include "lib/ilist.h"
#include "libpq/libpq.h"
#include "libpq/pqsignal.h"
#include "pg_getopt.h"
#include "pgstat.h"
#include "port/pg_bswap.h"
#include "postmaster/autovacuum.h"
#include "postmaster/bgworker_internals.h"
#include "postmaster/pgarch.h"
#include "postmaster/postmaster.h"
#include "postmaster/syslogger.h"
#include "postmaster/walsummarizer.h"
#include "replication/logicallauncher.h"
#include "replication/slotsync.h"
#include "replication/walsender.h"
#include "storage/aio_subsys.h"
#include "storage/fd.h"
#include "storage/io_worker.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "tcop/backend_startup.h"
#include "tcop/tcopprot.h"
#include "utils/datetime.h"
#include "utils/memutils.h"
#include "utils/pidfile.h"
#include "utils/timestamp.h"
#include "utils/varlena.h"

#ifdef EXEC_BACKEND
#include "common/file_utils.h"
#include "storage/pg_shmem.h"
#endif


/*
 * CountChildren 和 SignalChildren 接受一个位掩码参数，用以表示
 * 要计数或发信号的 BackendType。定义独立的类型与函数来操作位掩码，
 * 以避免意外地将普通 BackendType 当作位掩码传入（或反过来）。
 */
typedef struct
{
	uint32		mask;
} BackendTypeMask;

StaticAssertDecl(BACKEND_NUM_TYPES < 32, "too many backend types for uint32");

static const BackendTypeMask BTYPE_MASK_ALL = {(1 << BACKEND_NUM_TYPES) - 1};
static const BackendTypeMask BTYPE_MASK_NONE = {0};

static inline BackendTypeMask
btmask(BackendType t)
{
	BackendTypeMask mask = {.mask = 1 << t};

	return mask;
}

static inline BackendTypeMask
btmask_add_n(BackendTypeMask mask, int nargs, BackendType *t)
{
	for (int i = 0; i < nargs; i++)
		mask.mask |= 1 << t[i];
	return mask;
}

#define btmask_add(mask, ...) \
	btmask_add_n(mask, \
		lengthof(((BackendType[]){__VA_ARGS__})), \
		(BackendType[]){__VA_ARGS__} \
	)

static inline BackendTypeMask
btmask_del(BackendTypeMask mask, BackendType t)
{
	mask.mask &= ~(1 << t);
	return mask;
}

static inline BackendTypeMask
btmask_all_except_n(int nargs, BackendType *t)
{
	BackendTypeMask mask = BTYPE_MASK_ALL;

	for (int i = 0; i < nargs; i++)
		mask = btmask_del(mask, t[i]);
	return mask;
}

#define btmask_all_except(...) \
	btmask_all_except_n( \
		lengthof(((BackendType[]){__VA_ARGS__})), \
		(BackendType[]){__VA_ARGS__} \
	)

static inline bool
btmask_contains(BackendTypeMask mask, BackendType t)
{
	return (mask.mask & (1 << t)) != 0;
}


BackgroundWorker *MyBgworkerEntry = NULL;

/* 我们监听连接所用的套接字端口号 */
int			PostPortNumber = DEF_PGPORT;

/* Unix 套接字所在的目录名 */
char	   *Unix_socket_directories;

/* TCP 监听地址 */
char	   *ListenAddresses;

/*
 * SuperuserReservedConnections 是为超级用户保留的后端数量，
 * ReservedConnections 是为拥有预定义角色 pg_use_reserved_connections
 * 权限的角色保留的后端数量。它们都从 MaxConnections 后端槽位的
 * 总数中扣除，因此既非超级用户、也不拥有 pg_use_reserved_connections
 * 权限的角色可用的后端槽位数量为
 * (MaxConnections - SuperuserReservedConnections - ReservedConnections)。
 *
 * 如果剩余槽位数量小于或等于 SuperuserReservedConnections，则只有
 * 超级用户可以建立新连接。如果剩余槽位数量大于 SuperuserReservedConnections
 * 但小于或等于 (SuperuserReservedConnections + ReservedConnections)，
 * 则只有超级用户以及拥有 pg_use_reserved_connections 权限的角色可以
 * 建立新连接。注意，已经存在的超级用户和 pg_use_reserved_connections
 * 连接不计入这些限制。
 */
int			SuperuserReservedConnections;
int			ReservedConnections;

/* 我们正在监听的套接字。 */
#define MAXLISTEN	64
static int	NumListenSockets = 0;
static pgsocket *ListenSockets = NULL;

/* 更多的选项变量 */
bool		EnableSSL = false;

int			PreAuthDelay = 0;
int			AuthenticationTimeout = 60;

bool		log_hostname;		/* 用于 ps 显示和日志 */

bool		enable_bonjour = false;
char	   *bonjour_name;
bool		restart_after_crash = true;
bool		remove_temp_files_after_crash = true;

/*
 * 在致命错误（例如某个子进程崩溃）后终止子进程时，我们通常发送
 * SIGQUIT——本文件中其余大多数注释也都基于这一假设——但开发者
 * 可能更倾向于使用 SIGABRT 来收集各个子进程的核心转储。
 */
bool		send_abort_for_crash = false;
bool		send_abort_for_kill = false;

/* 特殊的子进程；未运行时为 NULL */
static PMChild *StartupPMChild = NULL,
		   *BgWriterPMChild = NULL,
		   *CheckpointerPMChild = NULL,
		   *WalWriterPMChild = NULL,
		   *WalReceiverPMChild = NULL,
		   *WalSummarizerPMChild = NULL,
		   *AutoVacLauncherPMChild = NULL,
		   *PgArchPMChild = NULL,
		   *SysLoggerPMChild = NULL,
		   *SlotSyncWorkerPMChild = NULL;

/* 启动进程的状态 */
typedef enum
{
	STARTUP_NOT_RUNNING,
	STARTUP_RUNNING,
	STARTUP_SIGNALED,			/* 我们向它发送了 SIGQUIT 或 SIGKILL */
	STARTUP_CRASHED,
} StartupStatusEnum;

static StartupStatusEnum StartupStatus = STARTUP_NOT_RUNNING;

/* 启动/关闭状态 */
#define			NoShutdown		0
#define			SmartShutdown	1
#define			FastShutdown	2
#define			ImmediateShutdown	3

static int	Shutdown = NoShutdown;

static bool FatalError = false; /* 如果从后端崩溃中恢复则为 T */

/*
 * 我们使用一个简单的状态机来控制启动、关闭以及崩溃恢复
 *（后者类似于先关闭再启动）。
 *
 * 完成 postmaster 的所有初始化工作后，我们进入 PM_STARTUP 状态并
 * 启动启动进程。启动进程首先读取控制文件并执行其他初始化步骤。
 * 在正常启动或崩溃恢复之后，启动进程以退出码 0 退出，我们切换到
 * PM_RUN 状态。不过，归档恢复的处理方式比较特殊，因为它耗时更长，
 * 而且我们希望在归档恢复期间支持热备。
 *
 * 当启动进程准备好开始归档恢复时，它会向 postmaster 发信号，我们
 * 切换到 PM_RECOVERY 状态。后台写入进程和检查点进程已经在运行
 *（因为它们在 PM_STARTUP 期间就已启动），启动进程继续应用 WAL。
 * 如果启用了热备，则在 WAL 重做到达一致性点后，启动进程再次向我们
 * 发信号，我们切换到 PM_HOT_STANDBY 状态并开始接受连接以执行
 * 只读查询。归档恢复结束时，启动进程以退出码 0 退出，我们切换到
 * PM_RUN 状态。
 *
 * 普通的子后端只能在处于 PM_RUN 或 PM_HOT_STANDBY 状态时启动
 *（connsAllowed 也可能限制启动）。在其他状态下，我们通过启动
 * “dead-end”（死胡同）子进程来处理连接请求，这些子进程只会向
 * 客户端发送一条错误消息然后退出。（我们在 ActiveChildList 中跟踪
 * 它们，以便知道它们何时全部退出；这很重要，因为它们仍然连接着
 * 共享内存，会干扰销毁 shmem 段的尝试，并可能在创建新段时导致
 * SHMALL 失败。）在 PM_WAIT_DEAD_END 状态下，我们等待所有 dead-end
 * 子进程离开系统，因此会完全停止接受连接请求，直到最后一个存在的
 * 子进程退出（希望不会等太久）。
 *
 * 注意，这个状态变量并不区分我们*为何*进入 PM_RUN 之后的状态——
 * 必须查看 Shutdown 和 FatalError 才能知道原因。FatalError 在
 * PM_RECOVERY、PM_HOT_STANDBY 或 PM_RUN 状态下、以及在
 * PM_WAIT_XLOG_SHUTDOWN 状态下永远不会为真（因为我们不会在试图从
 * 崩溃中恢复时进入这些状态）。它在 PM_STARTUP 状态下可能为真，
 * 因为我们要等到成功启动 WAL 重做后才会清除它。
 */
typedef enum
{
	PM_INIT,					/* postmaster 正在启动 */
	PM_STARTUP,					/* 等待启动子进程 */
	PM_RECOVERY,				/* 处于归档恢复模式 */
	PM_HOT_STANDBY,				/* 处于热备模式 */
	PM_RUN,						/* 正常的“数据库已启动”状态 */
	PM_STOP_BACKENDS,			/* 需要停止其余后端 */
	PM_WAIT_BACKENDS,			/* 等待活跃后端退出 */
	PM_WAIT_XLOG_SHUTDOWN,		/* 等待检查点进程执行关闭
								 * 检查点 */
	PM_WAIT_XLOG_ARCHIVAL,		/* 等待归档进程和 walsender
								 * 完成 */
	PM_WAIT_IO_WORKERS,			/* 等待 io 工作进程退出 */
	PM_WAIT_CHECKPOINTER,		/* 等待检查点进程关闭 */
	PM_WAIT_DEAD_END,			/* 等待 dead-end 子进程退出 */
	PM_NO_CHILDREN,				/* 所有重要子进程均已退出 */
} PMState;

static PMState pmState = PM_INIT;

/*
 * 在执行“智能关闭”时，我们会限制新连接，但保持 PM_RUN 或
 * PM_HOT_STANDBY 状态，直到所有客户端后端都退出。connsAllowed 是一个
 * 子状态指示器，显示当前生效的限制。只有当 pmState 为 PM_RUN 或
 * PM_HOT_STANDBY 时它才有意义。
 */
static bool connsAllowed = true;

/* 立即关闭或子进程崩溃时 SIGKILL 超时的起始时间 */
/* 为零表示超时计时未运行 */
static time_t AbortStartTime = 0;

/* 上述超时的时长 */
#define SIGKILL_CHILDREN_AFTER_SECS		5

static bool ReachedNormalRunning = false;	/* 如果已到达 PM_RUN 则为 T */

bool		ClientAuthInProgress = false;	/* 在为新的客户端进行
											 * 身份验证期间为 T */

bool		redirection_done = false;	/* stderr 是否已为 syslogger 重定向？ */

/* 收到 START_AUTOVAC_LAUNCHER 信号 */
static bool start_autovac_launcher = false;

/* 需要向 launcher 发信号以传达某种状态 */
static bool avlauncher_needs_signal = false;

/* 收到 START_WALRECEIVER 信号 */
static bool WalReceiverRequested = false;

/* 当有需要启动的工作进程时设置 */
static bool StartWorkerNeeded = true;
static bool HaveCrashedWorker = false;

/* 收到信号时设置 */
static volatile sig_atomic_t pending_pm_pmsignal;
static volatile sig_atomic_t pending_pm_child_exit;
static volatile sig_atomic_t pending_pm_reload_request;
static volatile sig_atomic_t pending_pm_shutdown_request;
static volatile sig_atomic_t pending_pm_fast_shutdown_request;
static volatile sig_atomic_t pending_pm_immediate_shutdown_request;

/* 事件多路复用对象 */
static WaitEventSet *pm_wait_set;

#ifdef USE_SSL
/* 在 SSL 已正确初始化时设置 */
bool		LoadedSSL = false;
#endif

#ifdef USE_BONJOUR
static DNSServiceRef bonjour_sdref = NULL;
#endif

/* IO 工作进程管理的状态。 */
static int	io_worker_count = 0;
static PMChild *io_worker_children[MAX_IO_WORKERS];

/*
 * postmaster.c - 函数原型声明
 */
static void CloseServerPorts(int status, Datum arg);
static void unlink_external_pid_file(int status, Datum arg);
static void getInstallationPaths(const char *argv0);
static void checkControlFile(void);
static void handle_pm_pmsignal_signal(SIGNAL_ARGS);
static void handle_pm_child_exit_signal(SIGNAL_ARGS);
static void handle_pm_reload_request_signal(SIGNAL_ARGS);
static void handle_pm_shutdown_request_signal(SIGNAL_ARGS);
static void process_pm_pmsignal(void);
static void process_pm_child_exit(void);
static void process_pm_reload_request(void);
static void process_pm_shutdown_request(void);
static void dummy_handler(SIGNAL_ARGS);
static void CleanupBackend(PMChild *bp, int exitstatus);
static void HandleChildCrash(int pid, int exitstatus, const char *procname);
static void LogChildExit(int lev, const char *procname,
						 int pid, int exitstatus);
static void PostmasterStateMachine(void);
static void UpdatePMState(PMState newState);

pg_noreturn static void ExitPostmaster(int status);
static int	ServerLoop(void);
static int	BackendStartup(ClientSocket *client_sock);
static void report_fork_failure_to_client(ClientSocket *client_sock, int errnum);
static CAC_state canAcceptConnections(BackendType backend_type);
static void signal_child(PMChild *pmchild, int signal);
static bool SignalChildren(int signal, BackendTypeMask targetMask);
static void TerminateChildren(int signal);
static int	CountChildren(BackendTypeMask targetMask);
static void LaunchMissingBackgroundProcesses(void);
static void maybe_start_bgworkers(void);
static bool maybe_reap_io_worker(int pid);
static void maybe_adjust_io_workers(void);
static bool CreateOptsFile(int argc, char *argv[], char *fullprogname);
static PMChild *StartChildProcess(BackendType type);
static void StartSysLogger(void);
static void StartAutovacuumWorker(void);
static bool StartBackgroundWorker(RegisteredBgWorker *rw);
static void InitPostmasterDeathWatchHandle(void);

#ifdef WIN32
#define WNOHANG 0				/* 被忽略，因此任何整数值都可以 */

static pid_t waitpid(pid_t pid, int *exitstatus, int options);
static void WINAPI pgwin32_deadchild_callback(PVOID lpParameter, BOOLEAN TimerOrWaitFired);

static HANDLE win32ChildQueue;

typedef struct
{
	HANDLE		waitHandle;
	HANDLE		procHandle;
	DWORD		procId;
} win32_deadchild_waitinfo;
#endif							/* WIN32 */

/* 用于检查子进程退出状态的宏 */
#define EXIT_STATUS_0(st)  ((st) == 0)
#define EXIT_STATUS_1(st)  (WIFEXITED(st) && WEXITSTATUS(st) == 1)
#define EXIT_STATUS_3(st)  (WIFEXITED(st) && WEXITSTATUS(st) == 3)

#ifndef WIN32
/*
 * 用于监控 postmaster 是否存活的管道的文件描述符。
 * 第一个是 POSTMASTER_FD_WATCH，第二个是 POSTMASTER_FD_OWN。
 */
int			postmaster_alive_fds[2] = {-1, -1};
#else
/* 在 Windows 上用于相同目的的 postmaster 进程句柄 */
HANDLE		PostmasterHandle;
#endif

/*
 * Postmaster 的主入口点
 */
void
PostmasterMain(int argc, char *argv[])
{
	int			opt;
	int			status;
	char	   *userDoption = NULL;
	bool		listen_addr_saved = false;
	char	   *output_config_variable = NULL;

	InitProcessGlobals();

	PostmasterPid = MyProcPid;

	IsPostmasterEnvironment = true;

	/*
	 * 启动我们的 win32 信号处理实现
	 */
#ifdef WIN32
	pgwin32_signal_initialize();
#endif

	/*
	 * 在检查数据目录（见 checkDataDir()）之前，我们本不应创建任何
	 * 文件或目录，但以防万一，仍将 umask 设置为最严格（仅属主）的
	 * 权限。
	 *
	 * checkDataDir() 会根据数据目录的权限重置 umask。
	 */
	umask(PG_MODE_MASK_OWNER);

	/*
	 * 默认情况下，postmaster 中的 palloc() 请求会在 PostmasterContext
	 * 中分配，这是一块可以被后端回收的空间。需要供后端使用的已分配
	 * 数据应当在 TopMemoryContext 中分配。
	 */
	PostmasterContext = AllocSetContextCreate(TopMemoryContext,
											  "Postmaster",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(PostmasterContext);

	/* 初始化安装相关文件的路径 */
	getInstallationPaths(argv[0]);

	/*
	 * 为 postmaster 进程设置信号处理函数。
	 *
	 * 注意：修改这个列表时，要检查是否会对子进程的信号处理设置产生
	 * 副作用。参见 tcop/postgres.c、bootstrap/bootstrap.c、
	 * postmaster/bgwriter.c、postmaster/walwriter.c、postmaster/autovacuum.c、
	 * postmaster/pgarch.c、postmaster/syslogger.c、postmaster/bgworker.c
	 * 以及 postmaster/checkpointer.c。
	 */
	pqinitmask();
	sigprocmask(SIG_SETMASK, &BlockSig, NULL);

	pqsignal(SIGHUP, handle_pm_reload_request_signal);
	pqsignal(SIGINT, handle_pm_shutdown_request_signal);
	pqsignal(SIGQUIT, handle_pm_shutdown_request_signal);
	pqsignal(SIGTERM, handle_pm_shutdown_request_signal);
	pqsignal(SIGALRM, SIG_IGN); /* 忽略 */
	pqsignal(SIGPIPE, SIG_IGN); /* 忽略 */
	pqsignal(SIGUSR1, handle_pm_pmsignal_signal);
	pqsignal(SIGUSR2, dummy_handler);	/* 未使用，为子进程保留 */
	pqsignal(SIGCHLD, handle_pm_child_exit_signal);

	/* 这会视平台情况配置 SIGURG。 */
	InitializeWaitEventSupport();
	InitProcessLocalLatch();

	/*
	 * Postgres 中不应有其他地方去触碰 SIGTTIN/SIGTTOU 的处理。我们在
	 * postmaster 环境下忽略这些信号，以避免子进程因向 stderr 写入而
	 * 被冻结的风险。但对于独立后端，它们的默认处理方式是合理的。
	 * 因此，所有子进程只需沿用继承下来的设置即可。
	 */
#ifdef SIGTTIN
	pqsignal(SIGTTIN, SIG_IGN); /* 忽略 */
#endif
#ifdef SIGTTOU
	pqsignal(SIGTTOU, SIG_IGN); /* 忽略 */
#endif

	/* 忽略 SIGXFSZ，使 ulimit 超限表现得像磁盘已满 */
#ifdef SIGXFSZ
	pqsignal(SIGXFSZ, SIG_IGN); /* 忽略 */
#endif

	/* 开始接收信号。 */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * 选项设置
	 */
	InitializeGUCOptions();

	opterr = 1;

	/*
	 * 解析命令行选项。注意：保持与 tcop/postgres.c（选项集不应冲突）
	 * 以及 main/main.c 中公共的 help() 函数同步。
	 */
	while ((opt = getopt(argc, argv, "B:bC:c:D:d:EeFf:h:ijk:lN:OPp:r:S:sTt:W:-:")) != -1)
	{
		switch (opt)
		{
			case 'B':
				SetConfigOption("shared_buffers", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'b':
				/* 用于二进制升级的未公开标志 */
				IsBinaryUpgrade = true;
				break;

			case 'C':
				output_config_variable = strdup(optarg);
				break;

			case '-':

				/*
				 * 如果用户放错了用于分派到子程序的、必须排在最前面的
				 * 特殊选项，则报错。parse_dispatch_option() 在找不到匹配时
				 * 会返回 DISPATCH_POSTMASTER，因此对其余任何情况都报错。
				 */
				if (parse_dispatch_option(optarg) != DISPATCH_POSTMASTER)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("--%s must be first argument", optarg)));

				/* 穿透 */
			case 'c':
				{
					char	   *name,
							   *value;

					ParseLongOption(optarg, &name, &value);
					if (!value)
					{
						if (opt == '-')
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("--%s requires a value",
											optarg)));
						else
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("-c %s requires a value",
											optarg)));
					}

					SetConfigOption(name, value, PGC_POSTMASTER, PGC_S_ARGV);
					pfree(name);
					pfree(value);
					break;
				}

			case 'D':
				userDoption = strdup(optarg);
				break;

			case 'd':
				set_debug_options(atoi(optarg), PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'E':
				SetConfigOption("log_statement", "all", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'e':
				SetConfigOption("datestyle", "euro", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'F':
				SetConfigOption("fsync", "false", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'f':
				if (!set_plan_disabling_options(optarg, PGC_POSTMASTER, PGC_S_ARGV))
				{
					write_stderr("%s: invalid argument for option -f: \"%s\"\n",
								 progname, optarg);
					ExitPostmaster(1);
				}
				break;

			case 'h':
				SetConfigOption("listen_addresses", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'i':
				SetConfigOption("listen_addresses", "*", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'j':
				/* 仅由交互式后端使用 */
				break;

			case 'k':
				SetConfigOption("unix_socket_directories", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'l':
				SetConfigOption("ssl", "true", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'N':
				SetConfigOption("max_connections", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'O':
				SetConfigOption("allow_system_table_mods", "true", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'P':
				SetConfigOption("ignore_system_indexes", "true", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'p':
				SetConfigOption("port", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'r':
				/* 仅由单用户后端使用 */
				break;

			case 'S':
				SetConfigOption("work_mem", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 's':
				SetConfigOption("log_statement_stats", "true", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'T':

				/*
				 * 这个选项过去被定义为在后端崩溃后发送 SIGSTOP，
				 * 但发送 SIGABRT 似乎更有用。
				 */
				SetConfigOption("send_abort_for_crash", "true", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 't':
				{
					const char *tmp = get_stats_option_name(optarg);

					if (tmp)
					{
						SetConfigOption(tmp, "true", PGC_POSTMASTER, PGC_S_ARGV);
					}
					else
					{
						write_stderr("%s: invalid argument for option -t: \"%s\"\n",
									 progname, optarg);
						ExitPostmaster(1);
					}
					break;
				}

			case 'W':
				SetConfigOption("post_auth_delay", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			default:
				write_stderr("Try \"%s --help\" for more information.\n",
							 progname);
				ExitPostmaster(1);
		}
	}

	/*
	 * Postmaster 不接受任何非选项的开关参数。
	 */
	if (optind < argc)
	{
		write_stderr("%s: invalid argument: \"%s\"\n",
					 progname, argv[optind]);
		write_stderr("Try \"%s --help\" for more information.\n",
					 progname);
		ExitPostmaster(1);
	}

	/*
	 * 定位正确的配置文件和数据目录，并首次读取 postgresql.conf。
	 */
	if (!SelectConfigFiles(userDoption, progname))
		ExitPostmaster(2);

	if (output_config_variable != NULL)
	{
		/*
		 * 如果这是一个运行时计算（runtime-computed）的 GUC，它尚未被
		 * 初始化，当前值没有用处。然而，对大多数 GUC 而言，这里是打印
		 * 其值的一个便利位置，因为即使服务器已经在运行，把 postmaster
		 * 启动流程执行到这一步也是安全的。对于少数几个我们目前还无法
		 * 提供有意义值的运行时计算 GUC，我们会等到 postmaster 启动的
		 * 后续阶段再打印其值。对于那些 GUC，我们将无法在正在运行的
		 * 服务器上使用 -C，但现在使用该选项会导致它们得到错误的结果。
		 */
		int			flags = GetConfigOptionFlags(output_config_variable, true);

		if ((flags & GUC_RUNTIME_COMPUTED) == 0)
		{
			/*
			 * 指定了 "-C guc"，因此打印该 GUC 的值并退出。无需额外的
			 * 权限检查，因为用户是在数据目录内进行读取。
			 */
			const char *config_val = GetConfigOption(output_config_variable,
													 false, false);

			puts(config_val ? config_val : "");
			ExitPostmaster(0);
		}

		/*
		 * 运行时计算的 GUC 会在稍后打印。当我们初始化服务器启动序列时，
		 * 屏蔽掉可能出现在生成输出中的任何日志消息。FATAL 及更严重的
		 * 消息值得显示，即便通常人们至少只会预期出现 PANIC。LOG 级别的
		 * 条目会被隐藏。
		 */
		SetConfigOption("log_min_messages", "FATAL", PGC_SUSET,
						PGC_S_OVERRIDE);
	}

	/* 验证 DataDir 看起来是否合理 */
	checkDataDir();

	/* 检查 pg_control 是否存在 */
	checkControlFile();

	/* 并将工作目录切换到该目录 */
	ChangeToDataDir();

	/*
	 * 检查 GUC 设置中是否存在无效的相互组合。
	 */
	if (SuperuserReservedConnections + ReservedConnections >= MaxConnections)
	{
		write_stderr("%s: \"superuser_reserved_connections\" (%d) plus \"reserved_connections\" (%d) must be less than \"max_connections\" (%d)\n",
					 progname,
					 SuperuserReservedConnections, ReservedConnections,
					 MaxConnections);
		ExitPostmaster(1);
	}
	if (XLogArchiveMode > ARCHIVE_MODE_OFF && wal_level == WAL_LEVEL_MINIMAL)
		ereport(ERROR,
				(errmsg("WAL archival cannot be enabled when \"wal_level\" is \"minimal\"")));
	if (max_wal_senders > 0 && wal_level == WAL_LEVEL_MINIMAL)
		ereport(ERROR,
				(errmsg("WAL streaming (\"max_wal_senders\" > 0) requires \"wal_level\" to be \"replica\" or \"logical\"")));
	if (summarize_wal && wal_level == WAL_LEVEL_MINIMAL)
		ereport(ERROR,
				(errmsg("WAL cannot be summarized when \"wal_level\" is \"minimal\"")));

	/*
	 * 其他一次性内部健全性检查可以放在这里，前提是它们很快。
	 *（把任何较慢的处理放到更靠后、postmaster.pid 创建之后。）
	 */
	if (!CheckDateTokenTables())
	{
		write_stderr("%s: invalid datetoken tables, please fix\n", progname);
		ExitPostmaster(1);
	}

	/*
	 * 既然已经处理完 postmaster 的参数，重置 getopt(3) 库，
	 * 以便它在子进程中能正常工作。
	 */
	optind = 1;
#ifdef HAVE_INT_OPTRESET
	optreset = 1;				/* 某些系统也需要这个 */
#endif

	/* 用于调试：显示 postmaster 的环境 */
	if (message_level_is_interesting(DEBUG3))
	{
#if !defined(WIN32) || defined(_MSC_VER)
		extern char **environ;
#endif
		char	  **p;
		StringInfoData si;

		initStringInfo(&si);

		appendStringInfoString(&si, "initial environment dump:");
		for (p = environ; *p; ++p)
			appendStringInfo(&si, "\n%s", *p);

		ereport(DEBUG3, errmsg_internal("%s", si.data));
		pfree(si.data);
	}

	/*
	 * 为数据目录创建锁文件。
	 *
	 * 我们希望在尝试获取输入套接字之前做这件事，因为数据目录的
	 * 互斥机制比套接字文件的互斥机制更可靠（感谢某位决定把套接字
	 * 文件放在 /tmp 的人 :-()。出于同样的原因，最好先获取 TCP
	 * 套接字，再获取 Unix 套接字。
	 *
	 * 还要注意，这会在内部设置负责移除数据目录和套接字锁文件的
	 * on_proc_exit 函数；因此它必须在打开套接字之前发生，这样在退出时
	 * 套接字锁文件会在 CloseServerPorts 运行之后才被移除。
	 */
	CreateDataDirLockFile(true);

	/*
	 * 读取控制文件（用于错误检查和配置信息）。
	 *
	 * 由于我们会校验控制文件的 CRC，这在需要运行时测试 CRC 支持指令的
	 * 机器上会产生一个有用的副作用。postmaster 会在启动时测试一次，
	 * 然后它的子进程会继承正确的函数指针，无需重复测试。
	 */
	LocalProcessControlFile(false);

	/*
	 * 注册 apply launcher。最好在任意模块有机会占用后台工作进程
	 * 槽位之前调用它。
	 */
	ApplyLauncherRegister();

	/*
	 * 处理任何应在 postmaster 启动时预加载的库
	 */
	process_shared_preload_libraries();

	/*
	 * 如果指定了，则初始化 SSL 库。
	 */
#ifdef USE_SSL
	if (EnableSSL)
	{
		(void) secure_initialize(true);
		LoadedSSL = true;
	}
#endif

	/*
	 * 既然可加载模块已经有机会修改任何 GUC，就计算 MaxBackends 并
	 * 初始化用于跟踪子进程的机制。
	 */
	InitializeMaxBackends();
	InitPostmasterChildSlots();

	/*
	 * 计算 PGPROC 快速路径锁数组的大小。
	 */
	InitializeFastPathLocks();

	/*
	 * 给预加载的库一个机会来请求额外的共享内存。
	 */
	process_shmem_requests();

	/*
	 * 既然可加载模块已经有机会请求额外的共享内存，就确定那些依赖
	 * 所需共享内存量的、运行时计算的 GUC 的值。
	 */
	InitializeShmemGUCs();

	/*
	 * 既然模块已经加载，就可以处理 wal_consistency_checking GUC 中
	 * 指定的任何自定义资源管理器了。
	 */
	InitializeWalConsistencyChecking();

	/*
	 * 如果 -C 与运行时计算的 GUC 一起指定，我们之前推迟了其值的打印，
	 * 因为当时 GUC 尚未初始化。我们在锁定数据目录之前处理大多数 GUC
	 * 的 -C，以便该选项可以在运行中的服务器上使用。然而，有少数 GUC
	 * 是运行时计算的，在锁定数据目录之前没有有意义的值，而且我们
	 * 无法在运行中的服务器上提前安全地计算它们的值。此时，这些 GUC
	 * 应该已经正确初始化，而且我们尚未建立共享内存，因此这是处理这些
	 * 特殊 GUC 的 -C 选项的好时机。
	 */
	if (output_config_variable != NULL)
	{
		const char *config_val = GetConfigOption(output_config_variable,
												 false, false);

		puts(config_val ? config_val : "");
		ExitPostmaster(0);
	}

	/*
	 * 建立共享内存和信号量。
	 *
	 * 注意：如果使用 SysV 共享内存和/或信号量，每次 postmaster 启动
	 * 通常会选择相同的 IPC 键。这有助于确保在 postmaster 崩溃并重启时
	 * 能清理掉失效的 IPC 对象。
	 */
	CreateSharedMemoryAndSemaphores();

	/*
	 * 估算可打开的文件数量。这必须在建立信号量之后进行，因为在
	 * 某些平台上信号量也计入打开的文件数。
	 */
	set_max_safe_fds();

	/*
	 * 初始化允许子进程在 postmaster 死亡时从睡眠中唤醒的管道
	 *（在 Windows 上则是进程句柄）。
	 */
	InitPostmasterDeathWatchHandle();

#ifdef WIN32

	/*
	 * 初始化用于传递已死亡子进程列表的 I/O 完成端口。
	 */
	win32ChildQueue = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
	if (win32ChildQueue == NULL)
		ereport(FATAL,
				(errmsg("could not create I/O completion port for child queue")));
#endif

#ifdef EXEC_BACKEND
	/* 写出供子进程使用的非默认 GUC 设置 */
	write_nondefault_variables(PGC_POSTMASTER);

	/*
	 * 清空用于向子进程传递参数的临时目录（见 internal_forkexec）。
	 * 我们必须在启动任何子进程之前做这件事，否则会出现竞态条件：
	 * 我们可能在子进程读取参数文件之前就把它删掉了。现在做这件事
	 * 应该是安全的，因为我们之前已经验证该数据目录中没有冲突的
	 * Postgres 进程。
	 */
	RemovePgTempFilesInDir(PG_TEMP_FILES_DIR, true, false);
#endif

	/*
	 * 强制移除用于发出备机提升请求的文件。否则，这些文件的存在会
	 * 触发过早的提升，无论用户是否希望如此。
	 *
	 * 这种移除文件的操作通常是不必要的，因为它们只可能在备机提升
	 * 的短暂期间存在。但存在一个竞态条件：如果 pg_ctl promote 在
	 * 提升过程中执行并创建了这些文件，它们可能会一直保留到服务器
	 * 作为主库启动之后。随后，如果新的备机使用从新主库取得的备份
	 * 启动，这些文件可能在服务器启动时仍然存在，必须移除它们以免
	 * 发生意外的提升。
	 *
	 * 注意，提升信号文件需要在启动进程被调用之前移除。因为在那之后，
	 * 它们就可能被 postmaster 的 SIGUSR1 信号处理函数使用了。
	 */
	RemovePromoteSignalFiles();

	/* 对 logrotate 信号文件做同样的处理 */
	RemoveLogrotateSignalFiles();

	/* 移除任何保存当前日志文件名的过期文件。 */
	if (unlink(LOG_METAINFO_DATAFILE) < 0 && errno != ENOENT)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m",
						LOG_METAINFO_DATAFILE)));

	/*
	 * 如果启用，则启动 syslogger 收集子进程
	 */
	if (Logging_collector)
		StartSysLogger();

	/*
	 * 将 whereToSendOutput 从 DestDebug（其初始状态）重置为 DestNone。
	 * 这会阻止 ereport 把日志消息发送到 stderr，除非 Log_destination
	 * 允许。我们直到 postmaster 完全启动后才这样做，因为启动失败
	 * 最好也报告到 stderr。
	 *
	 * 如果我们确实要禁用向 stderr 的日志输出，先发出一条日志消息
	 * 说明这一点，为那些可能不记得日志被配置到其他地方的用户
	 * 提供一条线索。
	 */
	if (!(Log_destination & LOG_DESTINATION_STDERR))
		ereport(LOG,
				(errmsg("ending log output to stderr"),
				 errhint("Future log output will go to log destination \"%s\".",
						 Log_destination_string)));

	whereToSendOutput = DestNone;

	/*
	 * 在日志中报告服务器启动。虽然我们可以更早发出这条消息，但
	 * 如果打算使用日志收集器，最好是在启动它之后再报告。
	 */
	ereport(LOG,
			(errmsg("starting %s", PG_VERSION_STR)));

	/*
	 * 建立输入套接字。
	 *
	 * 先设置一个 on_proc_exit 函数，负责在 postmaster 关闭时再次
	 * 关闭这些套接字。
	 */
	ListenSockets = palloc(MAXLISTEN * sizeof(pgsocket));
	on_proc_exit(CloseServerPorts, 0);

	if (ListenAddresses)
	{
		char	   *rawstring;
		List	   *elemlist;
		ListCell   *l;
		int			success = 0;

		/* 需要一份可修改的 ListenAddresses 副本 */
		rawstring = pstrdup(ListenAddresses);

		/* 将字符串解析为主机名列表 */
		if (!SplitGUCList(rawstring, ',', &elemlist))
		{
			/* 列表中存在语法错误 */
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid list syntax in parameter \"%s\"",
							"listen_addresses")));
		}

		foreach(l, elemlist)
		{
			char	   *curhost = (char *) lfirst(l);

			if (strcmp(curhost, "*") == 0)
				status = ListenServerPort(AF_UNSPEC, NULL,
										  (unsigned short) PostPortNumber,
										  NULL,
										  ListenSockets,
										  &NumListenSockets,
										  MAXLISTEN);
			else
				status = ListenServerPort(AF_UNSPEC, curhost,
										  (unsigned short) PostPortNumber,
										  NULL,
										  ListenSockets,
										  &NumListenSockets,
										  MAXLISTEN);

			if (status == STATUS_OK)
			{
				success++;
				/* 将第一个成功的主机地址记录到锁文件中 */
				if (!listen_addr_saved)
				{
					AddToDataDirLockFile(LOCK_FILE_LINE_LISTEN_ADDR, curhost);
					listen_addr_saved = true;
				}
			}
			else
				ereport(WARNING,
						(errmsg("could not create listen socket for \"%s\"",
								curhost)));
		}

		if (!success && elemlist != NIL)
			ereport(FATAL,
					(errmsg("could not create any TCP/IP sockets")));

		list_free(elemlist);
		pfree(rawstring);
	}

#ifdef USE_BONJOUR
	/* 仅当我们打开了 TCP 套接字时才注册 Bonjour */
	if (enable_bonjour && NumListenSockets > 0)
	{
		DNSServiceErrorType err;

		/*
		 * 我们将 interface_index 传为 0，这表示在所有“适用”的接口上
		 * 注册。如果我们只绑定到可用网络接口的一个子集，从 DNS-SD
		 * 文档中并不能完全确定这么做是否合适。
		 */
		err = DNSServiceRegister(&bonjour_sdref,
								 0,
								 0,
								 bonjour_name,
								 "_postgresql._tcp.",
								 NULL,
								 NULL,
								 pg_hton16(PostPortNumber),
								 0,
								 NULL,
								 NULL,
								 NULL);
		if (err != kDNSServiceErr_NoError)
			ereport(LOG,
					(errmsg("DNSServiceRegister() failed: error code %ld",
							(long) err)));

		/*
		 * 我们不去费心读取 mDNS 守护进程的回复，并且预期在 postmaster
		 * 终止时套接字关闭后，它会自动终止我们的注册。因此这里没有
		 * 更多事情要做。不过，bonjour_sdref 会被保留下来，以便 fork
		 * 出来的子进程可以关闭它们自己的套接字副本。
		 */
	}
#endif

	if (Unix_socket_directories)
	{
		char	   *rawstring;
		List	   *elemlist;
		ListCell   *l;
		int			success = 0;

		/* 需要一份可修改的 Unix_socket_directories 副本 */
		rawstring = pstrdup(Unix_socket_directories);

		/* 将字符串解析为目录列表 */
		if (!SplitDirectoriesString(rawstring, ',', &elemlist))
		{
			/* 列表中存在语法错误 */
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid list syntax in parameter \"%s\"",
							"unix_socket_directories")));
		}

		foreach(l, elemlist)
		{
			char	   *socketdir = (char *) lfirst(l);

			status = ListenServerPort(AF_UNIX, NULL,
									  (unsigned short) PostPortNumber,
									  socketdir,
									  ListenSockets,
									  &NumListenSockets,
									  MAXLISTEN);

			if (status == STATUS_OK)
			{
				success++;
				/* 将第一个成功的 Unix 套接字记录到锁文件中 */
				if (success == 1)
					AddToDataDirLockFile(LOCK_FILE_LINE_SOCKET_DIR, socketdir);
			}
			else
				ereport(WARNING,
						(errmsg("could not create Unix-domain socket in directory \"%s\"",
								socketdir)));
		}

		if (!success && elemlist != NIL)
			ereport(FATAL,
					(errmsg("could not create any Unix-domain sockets")));

		list_free_deep(elemlist);
		pfree(rawstring);
	}

	/*
	 * 检查我们确实有可用于监听的套接字
	 */
	if (NumListenSockets == 0)
		ereport(FATAL,
				(errmsg("no socket created for listening")));

	/*
	 * 如果没有有效的 TCP 端口，则为监听地址写入一个空行，表示
	 * 必须使用 Unix 套接字。注意，这行内容在还没有对应的套接字支撑
	 * 之前不会被加入锁文件。
	 */
	if (!listen_addr_saved)
		AddToDataDirLockFile(LOCK_FILE_LINE_LISTEN_ADDR, "");

	/*
	 * 记录 postmaster 选项。我们把这一步推迟到现在，以避免记录到无效的
	 * 选项（例如无法使用的端口号）。
	 */
	if (!CreateOptsFile(argc, argv, my_exec_path))
		ExitPostmaster(1);

	/*
	 * 如果被请求，写出外部 PID 文件
	 */
	if (external_pid_file)
	{
		FILE	   *fpidfile = fopen(external_pid_file, "w");

		if (fpidfile)
		{
			fprintf(fpidfile, "%d\n", MyProcPid);
			fclose(fpidfile);

			/* 让 PID 文件对所有用户可读 */
			if (chmod(external_pid_file, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH) != 0)
				write_stderr("%s: could not change permissions of external PID file \"%s\": %m\n",
							 progname, external_pid_file);
		}
		else
			write_stderr("%s: could not write external PID file \"%s\": %m\n",
						 progname, external_pid_file);

		on_proc_exit(unlink_external_pid_file, 0);
	}

	/*
	 * 移除旧的临时文件。此时该目录中不可能有其他 Postgres 进程在运行，
	 * 因此这样做是安全的。
	 */
	RemovePgTempFiles();

	/*
	 * 初始化 autovacuum 子系统（同样，还没有真正启动进程）
	 */
	autovac_init();

	/*
	 * 加载用于客户端认证的配置
	 */
	if (!load_hba())
	{
		/*
		 * 如果我们无法加载 HBA 文件，继续运行就没有意义了，
		 * 因为这种情况下没有任何办法连接到数据库。
		 */
		ereport(FATAL,
		/* 译注：%s 是一个配置文件 */
				(errmsg("could not load %s", HbaFileName)));
	}
	if (!load_ident())
	{
		/*
		 * 没有 IDENT 文件我们也可以启动，尽管这意味着你无法使用任何
		 * 需要用户名映射的认证方式。load_ident() 已经把错误的详细信息
		 * 记录到了日志中。
		 */
	}

#ifdef HAVE_PTHREAD_IS_THREADED_NP

	/*
	 * 在 macOS 上，libintl 会把 setlocale() 替换为一个在第二个参数为 ""
	 * 且所有相关环境变量都未设置或为空时调用 CFLocaleCopyCurrent() 的版本。
	 * CFLocaleCopyCurrent() 会使进程变成多线程。postmaster 会调用
	 * sigprocmask() 并调用 fork() 而不立即 exec()，这两者在一个多线程
	 * 程序中都有未定义的行为。多线程的 postmaster 在 Windows 上是正常的
	 * 情况，因为 Windows 既不提供 fork() 也不提供 sigprocmask()。目前，
	 * macOS 是唯一拥有 pthread_is_threaded_np() 的平台，因此我们无需担心
	 * 这个 HINT 在其他地方是否合适。
	 */
	if (pthread_is_threaded_np() != 0)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("postmaster became multithreaded during startup"),
				 errhint("Set the LC_ALL environment variable to a valid locale.")));
#endif

	/*
	 * 记录 postmaster 启动时间
	 */
	PgStartTime = GetCurrentTimestamp();

	/*
	 * 在 postmaster.pid 文件中报告 postmaster 状态，以便 pg_ctl 能够
	 * 看到当前正在发生什么。
	 */
	AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STARTING);

	UpdatePMState(PM_STARTUP);

	/* 确保我们在启动期间能够执行 I/O。 */
	maybe_adjust_io_workers();

	/* 启动 bgwriter 和 checkpointer，让它们能协助恢复 */
	if (CheckpointerPMChild == NULL)
		CheckpointerPMChild = StartChildProcess(B_CHECKPOINTER);
	if (BgWriterPMChild == NULL)
		BgWriterPMChild = StartChildProcess(B_BG_WRITER);

	/*
	 * 我们已经准备就绪，可以开始了……
	 */
	StartupPMChild = StartChildProcess(B_STARTUP);
	Assert(StartupPMChild != NULL);
	StartupStatus = STARTUP_RUNNING;

	/* 某些工作进程可能已被安排现在启动 */
	maybe_start_bgworkers();

	status = ServerLoop();

	/*
	 * ServerLoop 大概永远不应该返回，但如果它返回了，就关闭。
	 */
	ExitPostmaster(status != STATUS_OK);

	abort();					/* 不会到达此处 */
}


/*
 * on_proc_exit 回调，用于关闭服务器的监听套接字
 */
static void
CloseServerPorts(int status, Datum arg)
{
	int			i;

	/*
	 * 首先，显式关闭所有套接字文件描述符。过去我们只是让这事在
	 * postmaster 退出时隐式发生，但最好在移除 postmaster.pid 锁文件
	 * 之前关闭它们；否则，如果新的 postmaster 想要重用 TCP 端口号，
	 * 就会出现竞态条件。
	 */
	for (i = 0; i < NumListenSockets; i++)
	{
		if (closesocket(ListenSockets[i]) != 0)
			elog(LOG, "could not close listen socket: %m");
	}
	NumListenSockets = 0;

	/*
	 * 接下来，移除 Unix 套接字的任何文件系统条目。为了避免与接入的
	 * postmaster 发生竞态条件，这必须在关闭套接字之后、移除锁文件
	 * 之前进行。
	 */
	RemoveSocketFiles();

	/*
	 * 我们在这里不对套接字锁文件做任何处理；它们会在稍后的
	 * on_proc_exit 回调中被移除。
	 */
}

/*
 * on_proc_exit 回调，用于删除 external_pid_file
 */
static void
unlink_external_pid_file(int status, Datum arg)
{
	if (external_pid_file)
		unlink(external_pid_file);
}


/*
 * 计算并检查属于安装一部分的文件的目录路径
 *（由 postgres 可执行文件自身的位置推断得出）
 */
static void
getInstallationPaths(const char *argv0)
{
	DIR		   *pdir;

	/* 定位 postgres 可执行文件本身 */
	if (find_my_exec(argv0, my_exec_path) < 0)
		ereport(FATAL,
				(errmsg("%s: could not locate my own executable path", argv0)));

#ifdef EXEC_BACKEND
	/* 在切换工作目录之前定位可执行的后端 */
	if (find_other_exec(argv0, "postgres", PG_BACKEND_VERSIONSTR,
						postgres_exec_path) < 0)
		ereport(FATAL,
				(errmsg("%s: could not locate matching postgres executable",
						argv0)));
#endif

	/*
	 * 定位 pkglib 目录——这必须尽早设置，以防我们因 postgresql.conf
	 * 中的配置项而尝试从其中加载任何模块。
	 */
	get_pkglib_path(my_exec_path, pkglib_path);

	/*
	 * 确认那里存在一个可读的目录；否则 Postgres 安装不完整或已损坏。
	 *（这种失败的一个典型原因是 postgres 可执行文件被移动或硬链接
	 * 到了某个并非安装 lib/ 目录同级目录的位置。）
	 */
	pdir = AllocateDir(pkglib_path);
	if (pdir == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m",
						pkglib_path),
				 errhint("This may indicate an incomplete PostgreSQL installation, or that the file \"%s\" has been moved away from its proper location.",
						 my_exec_path)));
	FreeDir(pdir);

	/*
	 * 检查 share/ 目录并不值得。如果 lib/ 目录在那里，share/ 大概
	 * 也在。
	 */
}

/*
 * 检查 pg_control 是否存在于数据目录中的正确位置。
 *
 * 这里不尝试验证 pg_control 的内容。这只是一次健全性检查，
 * 看看我们是否指向一个真实的数据目录。
 */
static void
checkControlFile(void)
{
	char		path[MAXPGPATH];
	FILE	   *fp;

	snprintf(path, sizeof(path), "%s/%s", DataDir, XLOG_CONTROL_FILE);

	fp = AllocateFile(path, PG_BINARY_R);
	if (fp == NULL)
	{
		write_stderr("%s: could not find the database system\n"
					 "Expected to find it in the directory \"%s\",\n"
					 "but could not open file \"%s\": %m\n",
					 progname, DataDir, path);
		ExitPostmaster(2);
	}
	FreeFile(fp);
}

/*
 * 确定应让 ServerLoop 睡眠多久（以毫秒为单位）。
 *
 * 在正常情况下我们最多等待一分钟，以确保即使没有请求到达，
 * ServerLoop 处理的其他后台任务也能完成。然而，如果有等待启动的
 * 后台工作进程，我们实际上不睡眠，以便它们能被快速处理。其他
 * 例外情况如代码所示。
 */
static int
DetermineSleepTime(void)
{
	TimestampTz next_wakeup = 0;

	/*
	 * 正常情况：要么根本没有后台工作进程，要么我们处于关闭序列中
	 *（在此期间我们完全忽略 bgworker）。
	 */
	if (Shutdown > NoShutdown ||
		(!StartWorkerNeeded && !HaveCrashedWorker))
	{
		if (AbortStartTime != 0)
		{
			int			seconds;

			/* 终止前剩余的时间；若已过期则钳制为 0 */
			seconds = SIGKILL_CHILDREN_AFTER_SECS -
				(time(NULL) - AbortStartTime);

			return Max(seconds * 1000, 0);
		}
		else
			return 60 * 1000;
	}

	if (StartWorkerNeeded)
		return 0;

	if (HaveCrashedWorker)
	{
		dlist_mutable_iter iter;

		/*
		 * 当存在崩溃的 bgworker 时，我们睡眠足够的时间，使它们能在
		 * 请求重启时被重启。扫描列表，根据最近一次崩溃时间和请求的
		 * 重启间隔，确定所有唤醒时间中的最小值。
		 */
		dlist_foreach_modify(iter, &BackgroundWorkerList)
		{
			RegisteredBgWorker *rw;
			TimestampTz this_wakeup;

			rw = dlist_container(RegisteredBgWorker, rw_lnode, iter.cur);

			if (rw->rw_crashed_at == 0)
				continue;

			if (rw->rw_worker.bgw_restart_time == BGW_NEVER_RESTART
				|| rw->rw_terminate)
			{
				ForgetBackgroundWorker(rw);
				continue;
			}

			this_wakeup = TimestampTzPlusMilliseconds(rw->rw_crashed_at,
													  1000L * rw->rw_worker.bgw_restart_time);
			if (next_wakeup == 0 || this_wakeup < next_wakeup)
				next_wakeup = this_wakeup;
		}
	}

	if (next_wakeup != 0)
	{
		int			ms;

		/* TimestampDifferenceMilliseconds 的结果在 [0, INT_MAX] 范围内 */
		ms = (int) TimestampDifferenceMilliseconds(GetCurrentTimestamp(),
												   next_wakeup);
		return Min(60 * 1000, ms);
	}

	return 60 * 1000;
}

/*
 * 启用或禁用服务器套接字事件的通知。由于我们目前没有从已有
 * WaitEventSet 中移除事件的办法，这里直接销毁并重建整个集合。
 * 在关闭期间会调用它，以便我们能等待后端退出而不接受新连接；
 * 在崩溃重新初始化、需要重新开始监听新连接时也会调用它。
 * 该 WaitEventSet 会由 fork 出来的子进程通过 ClosePostmasterPorts()
 * 释放。
 */
static void
ConfigurePostmasterWaitSet(bool accept_connections)
{
	if (pm_wait_set)
		FreeWaitEventSet(pm_wait_set);
	pm_wait_set = NULL;

	pm_wait_set = CreateWaitEventSet(NULL,
									 accept_connections ? (1 + NumListenSockets) : 1);
	AddWaitEventToSet(pm_wait_set, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch,
					  NULL);

	if (accept_connections)
	{
		for (int i = 0; i < NumListenSockets; i++)
			AddWaitEventToSet(pm_wait_set, WL_SOCKET_ACCEPT, ListenSockets[i],
							  NULL, NULL);
	}
}

/*
 * postmaster 的主空闲循环
 */
static int
ServerLoop(void)
{
	time_t		last_lockfile_recheck_time,
				last_touch_time;
	WaitEvent	events[MAXLISTEN];
	int			nevents;

	ConfigurePostmasterWaitSet(true);
	last_lockfile_recheck_time = last_touch_time = time(NULL);

	for (;;)
	{
		time_t		now;

		nevents = WaitEventSetWait(pm_wait_set,
								   DetermineSleepTime(),
								   events,
								   lengthof(events),
								   0 /* postmaster 不投递 wait_events */ );

		/*
		 * 是信号处理函数设置的闩锁，还是我们的某个套接字上有待处理的
		 * 新连接？如果是后者，就 fork 一个子进程来处理它。
		 */
		for (int i = 0; i < nevents; i++)
		{
			if (events[i].events & WL_LATCH_SET)
				ResetLatch(MyLatch);

			/*
			 * 以下请求会被无条件处理，即使我们没有看到 WL_LATCH_SET。
			 * 这让关闭和重新加载请求获得高优先级，因为闩锁可能碰巧
			 * 出现在 events[] 中较后的位置，或者会在对 WaitEventSetWait()
			 * 的后续调用中报告。
			 */
			if (pending_pm_shutdown_request)
				process_pm_shutdown_request();
			if (pending_pm_reload_request)
				process_pm_reload_request();
			if (pending_pm_child_exit)
				process_pm_child_exit();
			if (pending_pm_pmsignal)
				process_pm_pmsignal();

			if (events[i].events & WL_SOCKET_ACCEPT)
			{
				ClientSocket s;

				if (AcceptConnection(events[i].fd, &s) == STATUS_OK)
					BackendStartup(&s);

				/* 本进程中我们不再需要这个已打开的套接字 */
				if (s.sock != PGINVALID_SOCKET)
				{
					if (closesocket(s.sock) != 0)
						elog(LOG, "could not close client socket: %m");
				}
			}
		}

		/*
		 * 如果我们在改变状态后、或因为某些进程退出而需要启动任何后台
		 * 进程，现在就启动。
		 */
		LaunchMissingBackgroundProcesses();

		/* 如果我们需要向 autovacuum launcher 发信号，现在就发 */
		if (avlauncher_needs_signal)
		{
			avlauncher_needs_signal = false;
			if (AutoVacLauncherPMChild != NULL)
				signal_child(AutoVacLauncherPMChild, SIGUSR2);
		}

#ifdef HAVE_PTHREAD_IS_THREADED_NP

		/*
		 * 在启用断言的情况下，定期检查是否出现了额外的线程。
		 * 所有构建版本都会在启动和退出时检查。
		 */
		Assert(pthread_is_threaded_np() == 0);
#endif

		/*
		 * 最后，检查是否到了该做某些事情的时候——这些事情我们不希望
		 * 每次循环都做，因为它们开销有点大。注意，这些任务的执行时间
		 * 可能有最多一分钟的误差，因为 DetermineSleepTime() 最多只会
		 * 让我们睡眠那么久；SIGKILL 超时除外，它那里有专门的特殊逻辑。
		 */
		now = time(NULL);

		/*
		 * 如果我们已经向子进程发送了 SIGQUIT 而它们迟迟不关闭，那么
		 * 现在该向它们发送 SIGKILL（或在被请求时发送 SIGABRT）了。这
		 * 通常不会发生，但在某些条件下 backend 在关闭时可能会卡住。这
		 * 是让它们摆脱卡死的最后手段。
		 *
		 * 注意在从进程崩溃中恢复期间我们也会这样做。
		 */
		if ((Shutdown >= ImmediateShutdown || FatalError) &&
			AbortStartTime != 0 &&
			(now - AbortStartTime) >= SIGKILL_CHILDREN_AFTER_SECS)
		{
			/* 之前我们对它们还算客气。现在不再客气了 */
			ereport(LOG,
			/* 译注：%s 是 SIGKILL 或 SIGABRT */
					(errmsg("issuing %s to recalcitrant children",
							send_abort_for_kill ? "SIGABRT" : "SIGKILL")));
			TerminateChildren(send_abort_for_kill ? SIGABRT : SIGKILL);
			/* 重置标志，以免我们再次发送 SIGKILL */
			AbortStartTime = 0;
		}

		/*
		 * 每分钟验证一次 postmaster.pid 是否未被删除或覆盖。如果
		 * 被删改了，我们强制关闭。这样可以避免 postmaster 和子进程在
		 * 其数据库消失后仍然滞留，并在同一位置创建新的数据库集簇时
		 * 可能引发问题。它也能提供一些保护，防止 DBA 愚蠢地删除
		 * postmaster.pid 并手动启动一个新的 postmaster。那样做很可能
		 * 导致数据损坏，但我们可以通过尽快中止来将损害降到最低。
		 */
		if (now - last_lockfile_recheck_time >= 1 * SECS_PER_MINUTE)
		{
			if (!RecheckDataDirLockFile())
			{
				ereport(LOG,
						(errmsg("performing immediate shutdown because data directory lock file is invalid")));
				kill(MyProcPid, SIGQUIT);
			}
			last_lockfile_recheck_time = now;
		}

		/*
		 * 每 58 分钟触碰一次 Unix 套接字和锁文件，以确保它们不会被
		 * 过于积极的 /tmp 清理任务删除。我们假设没有人会以小于一小时的
		 * 截止时间来运行清理程序……
		 */
		if (now - last_touch_time >= 58 * SECS_PER_MINUTE)
		{
			TouchSocketFiles();
			TouchSocketLockFiles();
			last_touch_time = now;
		}
	}
}

/*
 * canAcceptConnections —— 检查数据库状态是否允许指定类型的连接。
 * backend_type 可以是 B_BACKEND 或 B_AUTOVAC_WORKER。
 *（注意，我们还不知道一个普通的 B_BACKEND 连接是否会变成 walsender。）
 */
static CAC_state
canAcceptConnections(BackendType backend_type)
{
	CAC_state	result = CAC_OK;

	Assert(backend_type == B_BACKEND || backend_type == B_AUTOVAC_WORKER);

	/*
	 * 在启动/关闭/不一致的恢复状态下，不能启动后端。为此，我们将
	 * autovac 工作进程与用户后端同等对待。
	 */
	if (pmState != PM_RUN && pmState != PM_HOT_STANDBY)
	{
		if (Shutdown > NoShutdown)
			return CAC_SHUTDOWN;	/* 关闭待处理 */
		else if (!FatalError && pmState == PM_STARTUP)
			return CAC_STARTUP; /* 正常启动 */
		else if (!FatalError && pmState == PM_RECOVERY)
			return CAC_NOTHOTSTANDBY;	/* 尚未准备好热备 */
		else
			return CAC_RECOVERY;	/* 否则一定是崩溃恢复 */
	}

	/*
	 * “智能关闭”的限制只应用于普通连接，而不应用于 autovac 工作进程。
	 */
	if (!connsAllowed && backend_type == B_BACKEND)
		return CAC_SHUTDOWN;	/* 关闭待处理 */

	return result;
}

/*
 * ClosePostmasterPorts —— 关闭 postmaster 所有打开的套接字
 *
 * 在子进程启动时调用，以释放该子进程不需要的文件描述符。
 * 当然，postmaster 自己仍然保持着它们打开。
 *
 * 注意：我们将 am_syslogger 作为布尔值传入，因为在调用此函数时
 * 我们还不希望设置全局变量。
 */
void
ClosePostmasterPorts(bool am_syslogger)
{
	/* 释放 postmaster 的 WaitEventSet 持有的资源。 */
	if (pm_wait_set)
	{
		FreeWaitEventSetAfterFork(pm_wait_set);
		pm_wait_set = NULL;
	}

#ifndef WIN32

	/*
	 * 关闭 postmaster 死亡监视管道的写端。尽早这样做很重要，这样如果
	 * postmaster 死了，其他进程不会因为我们还保持着管道打开而以为它
	 * 仍在运行。
	 */
	if (close(postmaster_alive_fds[POSTMASTER_FD_OWN]) != 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg_internal("could not close postmaster death monitoring pipe in child process: %m")));
	postmaster_alive_fds[POSTMASTER_FD_OWN] = -1;
	/* 通知 fd.c 我们释放了一个管道文件描述符。 */
	ReleaseExternalFD();
#endif

	/*
	 * 关闭 postmaster 的监听套接字。这些不被 fd.c 跟踪，因此我们
	 * 在这里不调用 ReleaseExternalFD()。
	 *
	 * 监听套接字被标记为 FD_CLOEXEC，所以在 EXEC_BACKEND 模式下
	 * 不需要这样做。
	 */
#ifndef EXEC_BACKEND
	if (ListenSockets)
	{
		for (int i = 0; i < NumListenSockets; i++)
		{
			if (closesocket(ListenSockets[i]) != 0)
				elog(LOG, "could not close listen socket: %m");
		}
		pfree(ListenSockets);
	}
	NumListenSockets = 0;
	ListenSockets = NULL;
#endif

	/*
	 * 如果使用 syslogger，关闭管道的读端。我们同样不费心在 fd.c 中
	 * 跟踪它。
	 */
	if (!am_syslogger)
	{
#ifndef WIN32
		if (syslogPipe[0] >= 0)
			close(syslogPipe[0]);
		syslogPipe[0] = -1;
#else
		if (syslogPipe[0])
			CloseHandle(syslogPipe[0]);
		syslogPipe[0] = 0;
#endif
	}

#ifdef USE_BONJOUR
	/* 如果使用 Bonjour，关闭到 mDNS 守护进程的连接 */
	if (bonjour_sdref)
		close(DNSServiceRefSockFD(bonjour_sdref));
#endif
}


/*
 * InitProcessGlobals —— 设置 MyStartTime[stamp]、随机数种子
 *
 * 在 postmaster 和每个后端启动时都会尽早调用。
 */
void
InitProcessGlobals(void)
{
	MyStartTimestamp = GetCurrentTimestamp();
	MyStartTime = timestamptz_to_time_t(MyStartTimestamp);

	/*
	 * 在每个进程中设置不同的全局种子。我们需要不可预测的随机源，
	 * 因此如果可能，使用高质量的随机位作为种子。否则，回退到基于
	 * 时间戳和 PID 的种子。
	 */
	if (unlikely(!pg_prng_strong_seed(&pg_global_prng_state)))
	{
		uint64		rseed;

		/*
		 * 由于 PID 和时间戳的最低有效位变化更频繁，将时间戳左移，以便在
		 * 给定时间段内允许更多的种子总数。由于那样会让时间戳中只有
		 * 20 位每约 1 秒循环一次，因此也混合进一些高位比特。
		 */
		rseed = ((uint64) MyProcPid) ^
			((uint64) MyStartTimestamp << 12) ^
			((uint64) MyStartTimestamp >> 20);

		pg_prng_seed(&pg_global_prng_state, rseed);
	}

	/*
	 * 同时确保我们为 random(3) 设置了一个良好的种子。核心 Postgres
	 * 中已不推荐使用它，但扩展可能会用到。
	 */
#ifndef WIN32
	srandom(pg_prng_uint32(&pg_global_prng_state));
#endif
}

/*
 * 子进程使用 SIGUSR1 向我们通知 'pmsignal'。pg_ctl 使用 SIGUSR1
 * 请求 postmaster 检查 logrotate 和 promote 文件。
 */
static void
handle_pm_pmsignal_signal(SIGNAL_ARGS)
{
	pending_pm_pmsignal = true;
	SetLatch(MyLatch);
}

/*
 * pg_ctl 使用 SIGHUP 请求重新加载配置文件。
 */
static void
handle_pm_reload_request_signal(SIGNAL_ARGS)
{
	pending_pm_reload_request = true;
	SetLatch(MyLatch);
}

/*
 * 重新读取配置文件，并通知子进程也这样做。
 */
static void
process_pm_reload_request(void)
{
	pending_pm_reload_request = false;

	ereport(DEBUG2,
			(errmsg_internal("postmaster received reload request signal")));

	if (Shutdown <= SmartShutdown)
	{
		ereport(LOG,
				(errmsg("received SIGHUP, reloading configuration files")));
		ProcessConfigFile(PGC_SIGHUP);
		SignalChildren(SIGHUP, btmask_all_except(B_DEAD_END_BACKEND));

		/* 同时也重新加载认证配置文件 */
		if (!load_hba())
			ereport(LOG,
			/* 译注：%s 是一个配置文件 */
					(errmsg("%s was not reloaded", HbaFileName)));

		if (!load_ident())
			ereport(LOG,
					(errmsg("%s was not reloaded", IdentFileName)));

#ifdef USE_SSL
		/* 同时也重新加载 SSL 配置 */
		if (EnableSSL)
		{
			if (secure_initialize(false) == 0)
				LoadedSSL = true;
			else
				ereport(LOG,
						(errmsg("SSL configuration was not reloaded")));
		}
		else
		{
			secure_destroy();
			LoadedSSL = false;
		}
#endif

#ifdef EXEC_BACKEND
		/* 为将来的子进程更新起点文件 */
		write_nondefault_variables(PGC_SIGHUP);
#endif
	}
}

/*
 * pg_ctl 使用 SIGTERM、SIGINT 和 SIGQUIT 来请求不同类型的
 * 关闭。
 */
static void
handle_pm_shutdown_request_signal(SIGNAL_ARGS)
{
	switch (postgres_signal_arg)
	{
		case SIGTERM:
			/* 如果另外两个标志未设置，则隐含为 smart */
			pending_pm_shutdown_request = true;
			break;
		case SIGINT:
			pending_pm_fast_shutdown_request = true;
			pending_pm_shutdown_request = true;
			break;
		case SIGQUIT:
			pending_pm_immediate_shutdown_request = true;
			pending_pm_shutdown_request = true;
			break;
	}
	SetLatch(MyLatch);
}

/*
 * 处理关闭请求。
 */
static void
process_pm_shutdown_request(void)
{
	int			mode;

	ereport(DEBUG2,
			(errmsg_internal("postmaster received shutdown request signal")));

	pending_pm_shutdown_request = false;

	/*
	 * 如果自上一次服务器循环以来到达了不止一个关闭请求信号，就采用
	 * 最立即的那一个。这与我们按任意顺序逐个处理时所适用的优先级
	 * 一致。
	 */
	if (pending_pm_immediate_shutdown_request)
	{
		pending_pm_immediate_shutdown_request = false;
		pending_pm_fast_shutdown_request = false;
		mode = ImmediateShutdown;
	}
	else if (pending_pm_fast_shutdown_request)
	{
		pending_pm_fast_shutdown_request = false;
		mode = FastShutdown;
	}
	else
		mode = SmartShutdown;

	switch (mode)
	{
		case SmartShutdown:

			/*
			 * 智能关闭：
			 *
			 * 等待子进程结束其工作，然后关闭。
			 */
			if (Shutdown >= SmartShutdown)
				break;
			Shutdown = SmartShutdown;
			ereport(LOG,
					(errmsg("received smart shutdown request")));

			/* 报告状态 */
			AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STOPPING);
#ifdef USE_SYSTEMD
			sd_notify(0, "STOPPING=1");
#endif

			/*
			 * 如果我们达到了正常运行，就直接进入等待客户端后端退出
			 * 的状态。如果已经处于 PM_STOP_BACKENDS 或更后的状态，则
			 * 不改变它。
			 */
			if (pmState == PM_RUN || pmState == PM_HOT_STANDBY)
				connsAllowed = false;
			else if (pmState == PM_STARTUP || pmState == PM_RECOVERY)
			{
				/* 应该没有客户端，因此继续停止子进程 */
				UpdatePMState(PM_STOP_BACKENDS);
			}

			/*
			 * 现在等待在线备份模式结束、后端退出。如果已经是这种
			 * 情况，PostmasterStateMachine 会采取下一步。
			 */
			PostmasterStateMachine();
			break;

		case FastShutdown:

			/*
			 * 快速关闭：
			 *
			 * 用 SIGTERM 中止所有子进程（回滚活动事务并退出），
			 * 在它们退出后关闭。
			 */
			if (Shutdown >= FastShutdown)
				break;
			Shutdown = FastShutdown;
			ereport(LOG,
					(errmsg("received fast shutdown request")));

			/* 报告状态 */
			AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STOPPING);
#ifdef USE_SYSTEMD
			sd_notify(0, "STOPPING=1");
#endif

			if (pmState == PM_STARTUP || pmState == PM_RECOVERY)
			{
				/* 只是静默地关闭后台进程 */
				UpdatePMState(PM_STOP_BACKENDS);
			}
			else if (pmState == PM_RUN ||
					 pmState == PM_HOT_STANDBY)
			{
				/* 报告我们即将清除活跃客户端会话 */
				ereport(LOG,
						(errmsg("aborting any active transactions")));
				UpdatePMState(PM_STOP_BACKENDS);
			}

			/*
			 * PostmasterStateMachine 会发出任何必要的信号，或者在没有
			 * 需要杀死的子进程时采取下一步。
			 */
			PostmasterStateMachine();
			break;

		case ImmediateShutdown:

			/*
			 * 立即关闭：
			 *
			 * 用 SIGQUIT 中止所有子进程，等待它们退出，再用 SIGKILL
			 * 终止余下的，然后退出，而不尝试正常关闭数据库系统。
			 */
			if (Shutdown >= ImmediateShutdown)
				break;
			Shutdown = ImmediateShutdown;
			ereport(LOG,
					(errmsg("received immediate shutdown request")));

			/* 报告状态 */
			AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STOPPING);
#ifdef USE_SYSTEMD
			sd_notify(0, "STOPPING=1");
#endif

			/* 告诉子进程尽快关闭 */
			/*（注意我们在这里不应用 send_abort_for_crash）*/
			SetQuitSignalReason(PMQUIT_FOR_STOP);
			TerminateChildren(SIGQUIT);
			UpdatePMState(PM_WAIT_BACKENDS);

			/* 为它们的死亡设置计时秒表 */
			AbortStartTime = time(NULL);

			/*
			 * 现在等待 backend 退出。如果一个都没有，
			 * PostmasterStateMachine 将执行下一步。
			 */
			PostmasterStateMachine();
			break;
	}
}

static void
handle_pm_child_exit_signal(SIGNAL_ARGS)
{
	pending_pm_child_exit = true;
	SetLatch(MyLatch);
}

/*
 * 在子进程死后进行清理。
 */
static void
process_pm_child_exit(void)
{
	int			pid;			/* 已死子进程的进程 id */
	int			exitstatus;		/* 它的退出状态 */

	pending_pm_child_exit = false;

	ereport(DEBUG4,
			(errmsg_internal("reaping dead processes")));

	while ((pid = waitpid(-1, &exitstatus, WNOHANG)) > 0)
	{
		PMChild    *pmchild;

		/*
		 * 检查这个子进程是否是启动进程。
		 */
		if (StartupPMChild && pid == StartupPMChild->pid)
		{
			ReleasePostmasterChildSlot(StartupPMChild);
			StartupPMChild = NULL;

			/*
			 * 启动进程是为了响应关闭请求而退出的（或者不管关闭请求
			 * 如何都正常完成了）。
			 */
			if (Shutdown > NoShutdown &&
				(EXIT_STATUS_0(exitstatus) || EXIT_STATUS_1(exitstatus)))
			{
				StartupStatus = STARTUP_NOT_RUNNING;
				UpdatePMState(PM_WAIT_BACKENDS);
				/* 其余部分由 PostmasterStateMachine 逻辑处理 */
				continue;
			}

			if (EXIT_STATUS_3(exitstatus))
			{
				ereport(LOG,
						(errmsg("shutdown at recovery target")));
				StartupStatus = STARTUP_NOT_RUNNING;
				Shutdown = Max(Shutdown, SmartShutdown);
				TerminateChildren(SIGTERM);
				UpdatePMState(PM_WAIT_BACKENDS);
				/* 其余部分由 PostmasterStateMachine 逻辑处理 */
				continue;
			}

			/*
			 * 启动进程的任何意外退出（包括 FATAL 退出）都是灾难性的，
			 * 因此要杀死其他子进程，并设置 StartupStatus，使我们在它们
			 * 退出后不会试图重新初始化。例外：如果 StartupStatus 是
			 * STARTUP_SIGNALED，那么我们之前向启动进程发送了 SIGQUIT；
			 * 那很可能就是它死亡的原因，并且在这种情况下我们确实想尝试
			 * 重启。
			 *
			 * 这一段还处理了另一种情况：我们在 PM_STARTUP 期间因为某个
			 * dead-end 子进程崩溃而发送了 SIGQUIT。在那种情况下，如果
			 * 启动进程因 SIGQUIT 而死，我们需要转换到 PM_WAIT_BACKENDS
			 * 状态，这样 PostmasterStateMachine 就能重启启动进程。（另一方面，
			 * 如果我们发出 SIGQUIT 太晚了，启动进程也可能正常完成。那种
			 * 情况下我们会直接往下走，开始正常操作。）
			 */
			if (!EXIT_STATUS_0(exitstatus))
			{
				if (StartupStatus == STARTUP_SIGNALED)
				{
					StartupStatus = STARTUP_NOT_RUNNING;
					if (pmState == PM_STARTUP)
						UpdatePMState(PM_WAIT_BACKENDS);
				}
				else
					StartupStatus = STARTUP_CRASHED;
				HandleChildCrash(pid, exitstatus,
								 _("startup process"));
				continue;
			}

			/*
			 * 启动成功，开始正常操作
			 */
			StartupStatus = STARTUP_NOT_RUNNING;
			FatalError = false;
			AbortStartTime = 0;
			ReachedNormalRunning = true;
			UpdatePMState(PM_RUN);
			connsAllowed = true;

			/*
			 * 在 postmaster 主循环的下一轮迭代中，我们会启动那些之前
			 * 尚未启动的后台任务，例如 autovacuum launcher 和后台
			 * 工作进程。
			 */
			StartWorkerNeeded = true;

			/* 此时我们确实已经开门营业了 */
			ereport(LOG,
					(errmsg("database system is ready to accept connections")));

			/* 报告状态 */
			AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_READY);
#ifdef USE_SYSTEMD
			sd_notify(0, "READY=1");
#endif

			continue;
		}

		/*
		 * 是 bgwriter 吗？正常退出可以忽略；如有必要，我们会在
		 * postmaster 主循环的下一轮迭代中启动一个新的。任何其他退出
		 * 情况都被视为崩溃。
		 */
		if (BgWriterPMChild && pid == BgWriterPMChild->pid)
		{
			ReleasePostmasterChildSlot(BgWriterPMChild);
			BgWriterPMChild = NULL;
			if (!EXIT_STATUS_0(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("background writer process"));
			continue;
		}

		/*
		 * 是 checkpointer 吗？
		 */
		if (CheckpointerPMChild && pid == CheckpointerPMChild->pid)
		{
			ReleasePostmasterChildSlot(CheckpointerPMChild);
			CheckpointerPMChild = NULL;
			if (EXIT_STATUS_0(exitstatus) && pmState == PM_WAIT_CHECKPOINTER)
			{
				/*
				 * 好的，我们看到 checkpointer 在被通知关闭后正常退出了。
				 * 我们知道 checkpointer 已经写了一个关闭检查点，否则我们
				 * 还会停留在 PM_WAIT_XLOG_SHUTDOWN 状态。
				 *
				 * 此时应该只剩下 dead-end 子进程和日志进程了。
				 */
				UpdatePMState(PM_WAIT_DEAD_END);
				ConfigurePostmasterWaitSet(false);
				SignalChildren(SIGTERM, btmask_all_except(B_LOGGER));
			}
				else
			{
				/*
				 * checkpointer 的任何意外退出（包括 FATAL 退出）都
				 * 被视为崩溃。
				 */
				HandleChildCrash(pid, exitstatus,
								_("checkpointer process"));
			}

			continue;
		}

		/*
		 * 是 wal writer 吗？正常退出可以忽略；如有必要，我们会在
		 * postmaster 主循环的下一轮迭代中启动一个新的。任何其他退出
		 * 情况都被视为崩溃。
		 */
		if (WalWriterPMChild && pid == WalWriterPMChild->pid)
		{
			ReleasePostmasterChildSlot(WalWriterPMChild);
			WalWriterPMChild = NULL;
			if (!EXIT_STATUS_0(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("WAL writer process"));
			continue;
		}

		/*
		 * 是 wal receiver 吗？如果退出状态为零（正常）或一（FATAL 退出），
		 * 我们假定一切都正常，就像普通后端一样。（如果我们需要一个新的
		 * wal receiver，会在 postmaster 主循环的下一轮迭代中启动一个。）
		 */
		if (WalReceiverPMChild && pid == WalReceiverPMChild->pid)
		{
			ReleasePostmasterChildSlot(WalReceiverPMChild);
			WalReceiverPMChild = NULL;
			if (!EXIT_STATUS_0(exitstatus) && !EXIT_STATUS_1(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("WAL receiver process"));
			continue;
		}

		/*
		 * 是 wal summarizer 吗？正常退出可以忽略；如有必要，我们会在
		 * postmaster 主循环的下一轮迭代中启动一个新的。任何其他退出
		 * 情况都被视为崩溃。
		 */
		if (WalSummarizerPMChild && pid == WalSummarizerPMChild->pid)
		{
			ReleasePostmasterChildSlot(WalSummarizerPMChild);
			WalSummarizerPMChild = NULL;
			if (!EXIT_STATUS_0(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("WAL summarizer process"));
			continue;
		}

		/*
		 * 是 autovacuum launcher 吗？正常退出可以忽略；如有必要，我们会在
		 * postmaster 主循环的下一轮迭代中启动一个新的。任何其他退出
		 * 情况都被视为崩溃。
		 */
		if (AutoVacLauncherPMChild && pid == AutoVacLauncherPMChild->pid)
		{
			ReleasePostmasterChildSlot(AutoVacLauncherPMChild);
			AutoVacLauncherPMChild = NULL;
			if (!EXIT_STATUS_0(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("autovacuum launcher process"));
			continue;
		}

		/*
		 * 是归档进程吗？如果退出状态为零（正常）或一（FATAL 退出），
		 * 我们假定一切都正常，就像普通后端一样，并只在 postmaster 主
		 * 循环的下一周期尝试启动一个新的，以重试归档剩余的文件。
		 */
		if (PgArchPMChild && pid == PgArchPMChild->pid)
		{
			ReleasePostmasterChildSlot(PgArchPMChild);
			PgArchPMChild = NULL;
			if (!EXIT_STATUS_0(exitstatus) && !EXIT_STATUS_1(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("archiver process"));
			continue;
		}

		/* 是系统日志进程吗？如果是，尝试启动一个新的 */
		if (SysLoggerPMChild && pid == SysLoggerPMChild->pid)
		{
			ReleasePostmasterChildSlot(SysLoggerPMChild);
			SysLoggerPMChild = NULL;

			/* 为了安全起见，先*启动*新的日志进程 */
			if (Logging_collector)
				StartSysLogger();

			if (!EXIT_STATUS_0(exitstatus))
				LogChildExit(LOG, _("system logger process"),
							 pid, exitstatus);
			continue;
		}

		/*
		 * 是 slot sync worker 吗？正常退出或 FATAL 退出可以忽略（FATAL
		 * 可能由 libpqwalreceiver 在提升期间收到启动进程的关闭请求时
		 * 引起）；如有必要，我们会在 postmaster 主循环的下一轮迭代中
		 * 启动一个新的。任何其他退出情况都被视为崩溃。
		 */
		if (SlotSyncWorkerPMChild && pid == SlotSyncWorkerPMChild->pid)
		{
			ReleasePostmasterChildSlot(SlotSyncWorkerPMChild);
			SlotSyncWorkerPMChild = NULL;
			if (!EXIT_STATUS_0(exitstatus) && !EXIT_STATUS_1(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("slot sync worker process"));
			continue;
		}

		/* 是 IO 工作进程吗？ */
		if (maybe_reap_io_worker(pid))
		{
			if (!EXIT_STATUS_0(exitstatus) && !EXIT_STATUS_1(exitstatus))
				HandleChildCrash(pid, exitstatus, _("io worker"));

			maybe_adjust_io_workers();
			continue;
		}

		/*
		 * 是后端还是后台工作进程？
		 */
		pmchild = FindPostmasterChildByPid(pid);
		if (pmchild)
		{
			CleanupBackend(pmchild, exitstatus);
		}

		/*
		 * 我们对这个子进程一无所知。这是极其意外的，因为我们确实
		 * 跟踪了所有我们 fork 出来的子进程。
		 */
		else
		{
			if (!EXIT_STATUS_0(exitstatus) && !EXIT_STATUS_1(exitstatus))
				HandleChildCrash(pid, exitstatus, _("untracked child process"));
			else
				LogChildExit(LOG, _("untracked child process"), pid, exitstatus);
		}
	}							/* 遍历待处理的子进程死亡报告 */

	/*
	 * 在清空 SIGCHLD 队列之后，看看是否有任何状态变化或动作要做。
	 */
	PostmasterStateMachine();
}

/*
 * CleanupBackend —— 在终止的后端或后台工作进程之后进行清理。
 *
 * 移除与该子进程关联的所有本地状态，并释放它的 PMChild 槽位。
 */
static void
CleanupBackend(PMChild *bp,
			   int exitstatus)	/* 子进程的退出状态。 */
{
	char		namebuf[MAXPGPATH];
	const char *procname;
	bool		crashed = false;
	bool		logged = false;
	pid_t		bp_pid;
	bool		bp_bgworker_notify;
	BackendType bp_bkend_type;
	RegisteredBgWorker *rw;

	/* 为日志消息构造一个进程名 */
	if (bp->bkend_type == B_BG_WORKER)
	{
		snprintf(namebuf, MAXPGPATH, _("background worker \"%s\""),
				 bp->rw->rw_worker.bgw_type);
		procname = namebuf;
	}
	else
		procname = _(GetBackendTypeDesc(bp->bkend_type));

	/*
	 * 如果后端以难看的方式死去，我们必须向所有其他后端发信号让它们
	 * 快速退出。如果退出状态为零（正常）或一（FATAL 退出），我们
	 * 假定一切都正常，并继续将该后端从活跃子进程列表中移除。
	 */
	if (!EXIT_STATUS_0(exitstatus) && !EXIT_STATUS_1(exitstatus))
		crashed = true;

#ifdef WIN32

	/*
	 * 在 win32 上，也将 ERROR_WAIT_NO_CHILDREN (128) 视为非致命情况，
	 * 因为它有时会在进程未能正常启动（远在它开始使用共享内存之前）
	 * 的负载下发生。微软报告说这与互斥锁失败有关：
	 * http://archives.postgresql.org/pgsql-hackers/2010-09/msg00790.php
	 */
	if (exitstatus == ERROR_WAIT_NO_CHILDREN)
	{
		LogChildExit(LOG, procname, bp->pid, exitstatus);
		logged = true;
		crashed = false;
	}
#endif

	/*
	 * 释放 PMChild 条目。
	 *
	 * 如果该进程关联了共享内存，这里还会检查它是否干净地脱离。
	 */
	bp_pid = bp->pid;
	bp_bgworker_notify = bp->bgworker_notify;
	bp_bkend_type = bp->bkend_type;
	rw = bp->rw;
	if (!ReleasePostmasterChildSlot(bp))
	{
		/*
		 * 哎呀，子进程没能自行清理干净。最终仍视为崩溃。
		 */
		crashed = true;
	}
	bp = NULL;

	/*
	 * 在崩溃情况下，立即退出，不重置后台工作进程的状态。不过，如果
	 * 启用了 restart_after_crash，后台工作进程的状态（例如 rw_pid）
	 * 仍然需要被重置，以便该工作进程能在崩溃恢复后重启。这个重置
	 * 由 ResetBackgroundWorkerCrashTimes() 处理，而不是在这里。
	 */
	if (crashed)
	{
		HandleChildCrash(bp_pid, exitstatus, procname);
		return;
	}

	/*
	 * 这个后端可能曾被安排在某些后台工作进程启动或停止时接收 SIGUSR1。
	 * 取消那些通知，因为我们不想向并非 PostgreSQL 后端的 PID 发信号。
	 * 在后端从未请求过此类通知的情况下（这大概很常见），会跳过这一步。
	 */
	if (bp_bgworker_notify)
		BackgroundWorkerStopNotifications(bp_pid);

	/*
	 * 如果它是后台工作进程，也更新它对应的 RegisteredBgWorker 条目。
	 */
	if (bp_bkend_type == B_BG_WORKER)
	{
		if (!EXIT_STATUS_0(exitstatus))
		{
			/* 记录时间戳，以便我们知道何时重启该工作进程。 */
			rw->rw_crashed_at = GetCurrentTimestamp();
		}
		else
		{
			/* 零退出状态表示终止 */
			rw->rw_crashed_at = 0;
			rw->rw_terminate = true;
		}

		rw->rw_pid = 0;
		ReportBackgroundWorkerExit(rw); /* 报告子进程死亡 */

		if (!logged)
		{
			LogChildExit(EXIT_STATUS_0(exitstatus) ? DEBUG1 : LOG,
						 procname, bp_pid, exitstatus);
			logged = true;
		}

		/* 让它被重启 */
		HaveCrashedWorker = true;
	}

	if (!logged)
		LogChildExit(DEBUG2, procname, bp_pid, exitstatus);
}

/*
 * 转入 FatalError 状态，以响应发生的某些糟糕情况。通常调用方已经
 * 记录了进入 FatalError 状态的原因。
 *
 * 只应在尚未处于 FatalError 或 ImmediateShutdown 状态时调用此函数。
 */
static void
HandleFatalError(QuitSignalReason reason, bool consider_sigabrt)
{
	int			sigtosend;

	Assert(!FatalError);
	Assert(Shutdown != ImmediateShutdown);

	SetQuitSignalReason(reason);

	if (consider_sigabrt && send_abort_for_crash)
		sigtosend = SIGABRT;
	else
		sigtosend = SIGQUIT;

	/*
	 * 向所有其他子进程发信号让它们退出。
	 *
	 * 我们本可以在这里排除 dead-end 子进程，但至少在发送 SIGABRT
	 * 时，包含它们似乎更好。
	 */
	TerminateChildren(sigtosend);

	FatalError = true;

	/*
	 * 选择合适的新的状态来应对这个致命错误。除非我们已经在关闭过程中，
	 * 否则我们经过 PM_WAIT_BACKENDS。对于关闭序列期间发生的错误，
	 * 我们直接切换到 PM_WAIT_DEAD_END。
	 */
	switch (pmState)
	{
		case PM_INIT:
			/* 不应该有任何子进程 */
			Assert(false);
			break;

			/* 等待子进程死亡 */
		case PM_STARTUP:
		case PM_RECOVERY:
		case PM_HOT_STANDBY:
		case PM_RUN:
		case PM_STOP_BACKENDS:
			UpdatePMState(PM_WAIT_BACKENDS);
			break;

		case PM_WAIT_BACKENDS:
			/* 可能还有更多后端需要等待 */
			break;

		case PM_WAIT_XLOG_SHUTDOWN:
		case PM_WAIT_XLOG_ARCHIVAL:
		case PM_WAIT_CHECKPOINTER:
		case PM_WAIT_IO_WORKERS:

			/*
			 * 注意：类似的代码存在于 PostmasterStateMachine() 在
			 * PM_STOP_BACKENDS/PM_WAIT_BACKENDS 状态下对 FatalError 的
			 * 处理中。
			 */
			ConfigurePostmasterWaitSet(false);
			UpdatePMState(PM_WAIT_DEAD_END);
			break;

		case PM_WAIT_DEAD_END:
		case PM_NO_CHILDREN:
			break;
	}

	/*
	 * ……如果这发生得不够快，现在我们就开始倒计时，准备毫不留情地
	 * 杀死它们。
	 */
	if (AbortStartTime == 0)
		AbortStartTime = time(NULL);
}

/*
 * HandleChildCrash —— 在失败的后端、bgwriter、checkpointer、walwriter、
 * autovacuum、归档进程、slot sync worker 或后台工作进程之后进行清理。
 *
 * 这里的目标是清理我们关于该子进程的本地状态，并向所有其他剩余的
 * 子进程发信号让它们快速退出。
 *
 * 调用者已经释放了它的 PMChild 槽位。
 */
static void
HandleChildCrash(int pid, int exitstatus, const char *procname)
{
	/*
	 * 只有在这是第一次进程崩溃、并且我们没有进行立即关闭时，我们才记录
	 * 消息并发送信号；否则，我们到这里只是为了更新 postmaster 对活跃
	 * 进程的认知。如果我们已经向子进程发过信号，出现非零的退出状态是
	 * 预料之中的，因此不要把日志弄乱。
	 */
	if (FatalError || Shutdown == ImmediateShutdown)
		return;

	LogChildExit(LOG, procname, pid, exitstatus);
	ereport(LOG,
			(errmsg("terminating any other active server processes")));

	/*
	 * 切换到错误状态。崩溃的进程已经被从 ActiveChildList 中移除。
	 */
	HandleFatalError(PMQUIT_FOR_CRASH, true);
}

/*
 * 记录子进程的死亡。
 */
static void
LogChildExit(int lev, const char *procname, int pid, int exitstatus)
{
	/*
	 * activity_buffer 的大小是任意的，但设成了与默认
	 * track_activity_query_size 相等
	 */
	char		activity_buffer[1024];
	const char *activity = NULL;

	if (!EXIT_STATUS_0(exitstatus))
		activity = pgstat_get_crashed_backend_activity(pid,
													   activity_buffer,
													   sizeof(activity_buffer));

	if (WIFEXITED(exitstatus))
		ereport(lev,

		/*------
		  译注：%s 是描述子进程的名词短语，例如
		  “server process”（服务器进程） */
				(errmsg("%s (PID %d) exited with exit code %d",
						procname, pid, WEXITSTATUS(exitstatus)),
				 activity ? errdetail("Failed process was running: %s", activity) : 0));
	else if (WIFSIGNALED(exitstatus))
	{
#if defined(WIN32)
		ereport(lev,

		/*------
		  译注：%s 是描述子进程的名词短语，例如
		  “server process”（服务器进程） */
				(errmsg("%s (PID %d) was terminated by exception 0x%X",
						procname, pid, WTERMSIG(exitstatus)),
				 errhint("See C include file \"ntstatus.h\" for a description of the hexadecimal value."),
				 activity ? errdetail("Failed process was running: %s", activity) : 0));
#else
		ereport(lev,

		/*------
		  译注：%s 是描述子进程的名词短语，例如
		  “server process”（服务器进程） */
				(errmsg("%s (PID %d) was terminated by signal %d: %s",
						procname, pid, WTERMSIG(exitstatus),
						pg_strsignal(WTERMSIG(exitstatus))),
				 activity ? errdetail("Failed process was running: %s", activity) : 0));
#endif
	}
	else
		ereport(lev,

		/*------
		  译注：%s 是描述子进程的名词短语，例如
		  “server process”（服务器进程） */
				(errmsg("%s (PID %d) exited with unrecognized status %d",
						procname, pid, exitstatus),
				 activity ? errdetail("Failed process was running: %s", activity) : 0));
}

/*
 * 推进 postmaster 的状态机，并适当地采取行动
 *
 * 这是 process_pm_shutdown_request()、process_pm_child_exit() 和
 * process_pm_pmsignal() 的公共代码，它们处理可能意味着我们需要改变状态的
 * 信号。
 */
static void
PostmasterStateMachine(void)
{
	/* 如果我们在进行智能关闭，尝试推进该状态。 */
	if (pmState == PM_RUN || pmState == PM_HOT_STANDBY)
	{
		if (!connsAllowed)
		{
			/*
			 * 当我们没有普通客户端后端在运行时，这个状态结束。然后
			 * 我们就可以准备停止其他子进程了。
			 */
			if (CountChildren(btmask(B_BACKEND)) == 0)
				UpdatePMState(PM_STOP_BACKENDS);
		}
	}

	/*
	 * 在 PM_WAIT_BACKENDS 状态下，等待所有普通后端，以及像 autovacuum
	 * 和后台工作进程这样与后端类似的其他进程退出。
	 *
	 * PM_STOP_BACKENDS 是一个瞬态，含义与 PM_WAIT_BACKENDS 相同，但
	 * 我们会在等待它们之前先发信号给这些进程。将其视为一个独立的
	 * pmState 使我们能在多条关闭代码路径间共享这段代码。
	 */
	if (pmState == PM_STOP_BACKENDS || pmState == PM_WAIT_BACKENDS)
	{
		BackendTypeMask targetMask = BTYPE_MASK_NONE;

		/*
		 * 当我们没有普通后端、没有 autovac launcher 或工作进程、也
		 * 没有 bgworker（包括未连接的）时，PM_WAIT_BACKENDS 状态结束。
		 */
		targetMask = btmask_add(targetMask,
								B_BACKEND,
								B_AUTOVAC_LAUNCHER,
								B_AUTOVAC_WORKER,
								B_BG_WORKER);

		/*
		 * 也不要 walwriter、bgwriter、slot sync worker 或 WAL summarizer。
		 */
		targetMask = btmask_add(targetMask,
								B_WAL_WRITER,
								B_BG_WRITER,
								B_SLOTSYNC_WORKER,
								B_WAL_SUMMARIZER);

		/* 如果我们处于恢复中，也停止 startup 和 walreceiver 进程 */
		targetMask = btmask_add(targetMask,
								B_STARTUP,
								B_WAL_RECEIVER);

		/*
		 * 如果我们在进行崩溃恢复或立即关闭，那么我们也会期望归档进程、
		 * checkpointer、io 工作进程和 walsender 一并退出，否则不会。
		 */
		if (FatalError || Shutdown >= ImmediateShutdown)
			targetMask = btmask_add(targetMask,
									B_CHECKPOINTER,
									B_ARCHIVER,
									B_IO_WORKER,
									B_WAL_SENDER);

		/*
		 * 通常归档进程、checkpointer、IO 工作进程和 walsender 会继续
		 * 运行；它们会在写入检查点记录后被终止。我们也暂时让 dead-end
		 * 子进程继续运行。syslogger 进程最后退出。
		 *
		 * 这个断言检查我们已经覆盖了所有后端类型，要么通过将它们包含
		 * 在 targetMask 中，要么通过在这里注明它们被允许继续运行。
		 */
#ifdef USE_ASSERT_CHECKING
		{
			BackendTypeMask remainMask = BTYPE_MASK_NONE;

			remainMask = btmask_add(remainMask,
									B_DEAD_END_BACKEND,
									B_LOGGER);

			/*
			 * 归档进程、checkpointer、IO 工作进程和 walsender 可能
			 * 已经在 targetMask 中，也可能不在。
			 */
			remainMask = btmask_add(remainMask,
									B_ARCHIVER,
									B_CHECKPOINTER,
									B_IO_WORKER,
									B_WAL_SENDER);

			/* 这些不是真正的 postmaster 子进程 */
			remainMask = btmask_add(remainMask,
									B_INVALID,
									B_STANDALONE_BACKEND);

			/* 所有类型都应该被包含在 targetMask 或 remainMask 中 */
			Assert((remainMask.mask | targetMask.mask) == BTYPE_MASK_ALL.mask);
		}
#endif

		/* 如果我们尚未向这些进程发信号让它们退出，现在就发 */
		if (pmState == PM_STOP_BACKENDS)
		{
			/*
			 * 忘记任何待处理的后台工作进程请求，因为我们不再愿意启动
			 * 任何新工作进程。（如果还有额外的请求到达，
			 * BackgroundWorkerStateChange 会拒绝它们。）
			 */
			ForgetUnstartedBackgroundWorkers();

			SignalChildren(SIGTERM, targetMask);

			UpdatePMState(PM_WAIT_BACKENDS);
		}

		/* 是否还有任何目标进程仍在运行？ */
		if (CountChildren(targetMask) == 0)
		{
			if (Shutdown >= ImmediateShutdown || FatalError)
			{
				/*
				 * 停止任何 dead-end 子进程，并停止创建新的。
				 *
				 * 注意：类似的代码存在于 HandleFatalError() 中，当错误
				 * 发生在 pmState > PM_WAIT_BACKENDS 时。
				 */
				UpdatePMState(PM_WAIT_DEAD_END);
				ConfigurePostmasterWaitSet(false);
				SignalChildren(SIGQUIT, btmask(B_DEAD_END_BACKEND));

				/*
				 * 我们在开始立即关闭或进入 FatalError 状态时，就已经
				 * 向辅助进程（日志进程除外）发送了 SIGQUIT（如果有的话）。
				 */
			}
			else
			{
				/*
				 * 如果我们到了这里，我们正在正常关闭。所有普通子进程
				 * 都走了，是时候告诉 checkpointer 执行关闭检查点了。
				 */
				Assert(Shutdown > NoShutdown);
				/* 如果 checkpointer 未运行，则启动它 */
				if (CheckpointerPMChild == NULL)
					CheckpointerPMChild = StartChildProcess(B_CHECKPOINTER);
				/* 并告诉它写入关闭检查点 */
				if (CheckpointerPMChild != NULL)
				{
					signal_child(CheckpointerPMChild, SIGINT);
					UpdatePMState(PM_WAIT_XLOG_SHUTDOWN);
				}
				else
				{
				/*
				 * 如果我们没能 fork 出 checkpointer，就直接关闭。
				 * 任何需要的清理都会在下次重启时进行。我们设置 FatalError，
				 * 以便退出时记录一条“异常关闭”的消息。
				 *
				 * 我们这里不参考 send_abort_for_crash，因为转储 core 文件
				 * 不太可能解释 checkpointer fork 失败的原因。
				 *
				 * XXX：也许值得引入一个不同的 PMQUIT 值，用来表明集簇
				 * 处于糟糕状态，但没有进程崩溃。但目前这条路径极不可能
				 * 被走到，因此在 quickdie() 中增加一个独立的错误消息
				 * 并不明显值得。
				 */
				HandleFatalError(PMQUIT_FOR_CRASH, false);
				}
			}
		}
	}

	/*
	 * 从 PM_WAIT_XLOG_SHUTDOWN 到 PM_WAIT_XLOG_ARCHIVAL 的状态转换
	 * 发生在 process_pm_pmsignal() 中，以响应 PMSIGNAL_XLOG_IS_SHUTDOWN。
	 */

	if (pmState == PM_WAIT_XLOG_ARCHIVAL)
	{
		/*
		 * 当除了 checkpointer、io 工作进程和 dead-end 子进程之外没有
		 * 其他子进程剩下时，PM_WAIT_XLOG_ARCHIVAL 状态结束。无论如何
		 * 此时不应该还有普通后端剩下；我们真正在等待的是 walsender
		 * 和归档进程退出。
		 */
		if (CountChildren(btmask_all_except(B_CHECKPOINTER, B_IO_WORKER,
											B_LOGGER, B_DEAD_END_BACKEND)) == 0)
		{
			UpdatePMState(PM_WAIT_IO_WORKERS);
			SignalChildren(SIGUSR2, btmask(B_IO_WORKER));
		}
	}

	if (pmState == PM_WAIT_IO_WORKERS)
	{
		/*
		 * 当只剩下 checkpointer 和 dead-end 子进程时，
		 * PM_WAIT_IO_WORKERS 状态结束。
		 */
		if (io_worker_count == 0)
		{
			UpdatePMState(PM_WAIT_CHECKPOINTER);

			/*
			 * 既然上面提到的进程已经走了，也告诉 checkpointer 关闭。
			 * 这样 checkpointer 就能在其他进程不干扰的情况下完成最后
			 * 一些清理工作。
			 */
			if (CheckpointerPMChild != NULL)
				signal_child(CheckpointerPMChild, SIGUSR2);
		}
	}

	/*
	 * 从 PM_WAIT_CHECKPOINTER 到 PM_WAIT_DEAD_END 的状态转换发生在
	 * process_pm_child_exit() 中。
	 */

	if (pmState == PM_WAIT_DEAD_END)
	{
		/*
		 * 当除了日志进程之外所有其他子进程都走了时，PM_WAIT_DEAD_END
		 * 状态结束。在正常关闭期间，剩下的全是 dead-end 后端，但在
		 * FatalError 处理中我们会带着更多进程直接跳到这里。注意它们
		 * 已经被发送了适当的关闭信号，无论是在通向 PM_WAIT_DEAD_END
		 * 的正常状态转换期间，还是在 FatalError 处理期间。
		 *
		 * 我们等待的原因是为了防止新的 postmaster 启动冲突的子进程；
		 * 这不是铁板钉钉的保护，但至少在“关闭后立即重启”的场景下
		 * 有帮助。
		 */
		if (CountChildren(btmask_all_except(B_LOGGER)) == 0)
		{
			/* 这些其他进程应该已经死了 */
			Assert(StartupPMChild == NULL);
			Assert(WalReceiverPMChild == NULL);
			Assert(WalSummarizerPMChild == NULL);
			Assert(BgWriterPMChild == NULL);
			Assert(CheckpointerPMChild == NULL);
			Assert(WalWriterPMChild == NULL);
			Assert(AutoVacLauncherPMChild == NULL);
			Assert(SlotSyncWorkerPMChild == NULL);
			/* 这里不考虑 syslogger */
			UpdatePMState(PM_NO_CHILDREN);
		}
	}

	/*
	 * 如果有人告诉我们关闭，一旦没有剩余子进程我们就退出。如果有过
	 * 崩溃，清理会在下次启动时进行。（在 PostgreSQL 8.3 之前，我们试图
	 * 在退出前从崩溃中恢复，但如果我们是因为从 init 收到 SIGTERM 而
	 * 退出的，那样做似乎不明智——很可能根本没有时间恢复，init 就会
	 * 决定 SIGKILL 我们。）
	 *
	 * 注意 syslogger 会继续运行。当它看到输入管道上的 EOF 时就会退出，
	 * 而 EOF 会在没有更多上游进程时发生。
	 */
	if (Shutdown > NoShutdown && pmState == PM_NO_CHILDREN)
	{
		if (FatalError)
		{
			ereport(LOG, (errmsg("abnormal database system shutdown")));
			ExitPostmaster(1);
		}
		else
		{
			/*
			 * postmaster 的正常退出在此处进行。我们无需在这里记录任何
			 * 日志，因为 UnlinkLockFiles 的 proc_exit 回调会这样做，
			 * 而那应当是最后一个对用户可见的动作。
			 */
			ExitPostmaster(0);
		}
	}

	/*
	 * 如果启动进程失败，或者用户不希望在后端崩溃后自动重启，就等待
	 * 所有非 syslogger 的子进程退出，然后退出 postmaster。当启动进程
	 * 失败时，我们不尝试重新初始化，因为它很有可能再次失败，而我们会
	 * 一直尝试下去。
	 */
	if (pmState == PM_NO_CHILDREN)
	{
		if (StartupStatus == STARTUP_CRASHED)
		{
			ereport(LOG,
					(errmsg("shutting down due to startup process failure")));
			ExitPostmaster(1);
		}
		if (!restart_after_crash)
		{
			ereport(LOG,
					(errmsg("shutting down because \"restart_after_crash\" is off")));
			ExitPostmaster(1);
		}
	}

	/*
	 * 如果我们需要从崩溃中恢复，就等待所有非 syslogger 的子进程退出，
	 * 然后重置共享内存并启动启动进程。
	 */
	if (FatalError && pmState == PM_NO_CHILDREN)
	{
		ereport(LOG,
				(errmsg("all server processes terminated; reinitializing")));

		/* 崩溃后移除残留的临时文件 */
		if (remove_temp_files_after_crash)
			RemovePgTempFiles();

		/* 允许后台工作进程立即重启 */
		ResetBackgroundWorkerCrashTimes();

		shmem_exit(1);

		/* 将控制文件重新读入本地内存 */
		LocalProcessControlFile(true);

		/* 重新创建共享内存和信号量 */
		CreateSharedMemoryAndSemaphores();

		UpdatePMState(PM_STARTUP);

		/* 确保我们在启动期间能够执行 I/O。 */
		maybe_adjust_io_workers();

		StartupPMChild = StartChildProcess(B_STARTUP);
		Assert(StartupPMChild != NULL);
		StartupStatus = STARTUP_RUNNING;
		/* 崩溃恢复已开始，重置 SIGKILL 标志 */
		AbortStartTime = 0;

		/* 重新开始接受服务器套接字连接事件 */
		ConfigurePostmasterWaitSet(true);
	}
}

static const char *
pmstate_name(PMState state)
{
#define PM_TOSTR_CASE(sym) case sym: return #sym
	switch (state)
	{
			PM_TOSTR_CASE(PM_INIT);
			PM_TOSTR_CASE(PM_STARTUP);
			PM_TOSTR_CASE(PM_RECOVERY);
			PM_TOSTR_CASE(PM_HOT_STANDBY);
			PM_TOSTR_CASE(PM_RUN);
			PM_TOSTR_CASE(PM_STOP_BACKENDS);
			PM_TOSTR_CASE(PM_WAIT_BACKENDS);
			PM_TOSTR_CASE(PM_WAIT_XLOG_SHUTDOWN);
			PM_TOSTR_CASE(PM_WAIT_XLOG_ARCHIVAL);
			PM_TOSTR_CASE(PM_WAIT_IO_WORKERS);
			PM_TOSTR_CASE(PM_WAIT_DEAD_END);
			PM_TOSTR_CASE(PM_WAIT_CHECKPOINTER);
			PM_TOSTR_CASE(PM_NO_CHILDREN);
	}
#undef PM_TOSTR_CASE

	pg_unreachable();
	return "";					/* silence compiler */
}

/*
 * 用于更新 pmState 的简单封装。提供这个封装的主要原因是，它能让
 * 记录所有状态转换变得容易。
 */
static void
UpdatePMState(PMState newState)
{
	elog(DEBUG1, "updating PMState from %s to %s",
		 pmstate_name(pmState), pmstate_name(newState));
	pmState = newState;
}

/*
 * 在状态改变后启动后台进程，或者在已有进程退出后重新启动。
 *
 * 检查当前的 pmState 以及任何后台进程的状态。如果有任何在当前状态下
 * 应该运行但缺失的后台进程，就启动它们。
 */
static void
LaunchMissingBackgroundProcesses(void)
{
	/* Syslogger 在所有状态下都活跃 */
	if (SysLoggerPMChild == NULL && Logging_collector)
		StartSysLogger();

	/*
	 * 配置的工作进程数量可能发生了变化，或者先前某次启动工作进程
	 * 可能失败了。检查我们是否需要启动/停止任何工作进程。
	 *
	 * 配置文件的改动总会触发对这个函数的调用，因此我们总会及时
	 * 处理配置变化。
	 */
	maybe_adjust_io_workers();

	/*
	 * checkpointer 和后台写入进程从启动起就一直活跃，直到开始关闭。
	 *
	 *（如果我们在进入 PM_WAIT_XLOG_SHUTDOWN 状态时 checkpointer 没有
	 * 运行，会再启动它一次来执行关闭检查点。那是在 PostmasterStateMachine()
	 * 中完成的，而不是在这里。）
	 */
	if (pmState == PM_RUN || pmState == PM_RECOVERY ||
		pmState == PM_HOT_STANDBY || pmState == PM_STARTUP)
	{
		if (CheckpointerPMChild == NULL)
			CheckpointerPMChild = StartChildProcess(B_CHECKPOINTER);
		if (BgWriterPMChild == NULL)
			BgWriterPMChild = StartChildProcess(B_BG_WRITER);
	}

	/*
	 * WAL 写入进程只在正常操作时才需要（否则我们无法写入任何新的 WAL）。
	 */
	if (WalWriterPMChild == NULL && pmState == PM_RUN)
		WalWriterPMChild = StartChildProcess(B_WAL_WRITER);

	/*
	 * 我们不希望 autovacuum 在二进制升级模式下运行，因为 autovacuum
	 * 可能在物理文件就位之前就更新空表的 relfrozenxid。
	 */
	if (!IsBinaryUpgrade && AutoVacLauncherPMChild == NULL &&
		(AutoVacuumingActive() || start_autovac_launcher) &&
		pmState == PM_RUN)
	{
		AutoVacLauncherPMChild = StartChildProcess(B_AUTOVAC_LAUNCHER);
		if (AutoVacLauncherPMChild != NULL)
			start_autovac_launcher = false; /* 信号已处理 */
	}

	/*
	 * 如果 WAL 归档始终启用，即使在恢复期间我们也允许启动归档进程。
	 */
	if (PgArchPMChild == NULL &&
		((XLogArchivingActive() && pmState == PM_RUN) ||
		 (XLogArchivingAlways() && (pmState == PM_RECOVERY || pmState == PM_HOT_STANDBY))) &&
		PgArchCanRestart())
		PgArchPMChild = StartChildProcess(B_ARCHIVER);

	/*
	 * 如果我们需要启动一个 slot sync worker，现在就尝试启动
	 *
	 * 当我们处于热备、快速或立即关闭未在进行中、slot sync 参数配置
	 * 正确，并且是第一次启动该工作进程，或自上次启动以来已经过了
	 * 足够长的时间时，我们才允许启动 slot sync worker。
	 */
	if (SlotSyncWorkerPMChild == NULL && pmState == PM_HOT_STANDBY &&
		Shutdown <= SmartShutdown && sync_replication_slots &&
		ValidateSlotSyncParams(LOG) && SlotSyncWorkerCanRestart())
		SlotSyncWorkerPMChild = StartChildProcess(B_SLOTSYNC_WORKER);

	/*
	 * 如果我们需要启动一个 WAL receiver，现在就尝试启动
	 *
	 * 注意：如果 walreceiver 进程已经在运行，我们似乎应该清除
	 * WalReceiverRequested。然而，如果 walreceiver 终止、而启动进程
	 * 立即请求一个新的，就会存在竞态条件：我们很可能在回收已死的
	 * walreceiver 进程之前就收到该请求的信号。比起漏掉启动一个我们需要的
	 * walreceiver，冒着启动一个多余的风险更好。（walreceiver 代码有逻辑
	 * 来识别自己在不需要时应该退出。）
	 */
	if (WalReceiverRequested)
	{
		if (WalReceiverPMChild == NULL &&
			(pmState == PM_STARTUP || pmState == PM_RECOVERY ||
			 pmState == PM_HOT_STANDBY) &&
			Shutdown <= SmartShutdown)
		{
			WalReceiverPMChild = StartChildProcess(B_WAL_RECEIVER);
			if (WalReceiverPMChild != 0)
				WalReceiverRequested = false;
			/* 否则保留该标志，以便稍后重试 */
		}
	}

	/* 如果我们需要启动一个 WAL summarizer，现在就尝试启动 */
	if (summarize_wal && WalSummarizerPMChild == NULL &&
		(pmState == PM_RUN || pmState == PM_HOT_STANDBY) &&
		Shutdown <= SmartShutdown)
		WalSummarizerPMChild = StartChildProcess(B_WAL_SUMMARIZER);

	/* 如有需要，让其他工作进程运行起来 */
	if (StartWorkerNeeded || HaveCrashedWorker)
		maybe_start_bgworkers();
}

/*
 * 返回信号的字符串表示。
 *
 * 因为这里只为我们在本文件中已经依赖的信号实现了映射，我们无需处理
 * 未实现的或数值相同的信号（就像 EWOULDBLOCK / EAGAIN 那样）。
 */
static const char *
pm_signame(int signal)
{
#define PM_TOSTR_CASE(sym) case sym: return #sym
	switch (signal)
	{
			PM_TOSTR_CASE(SIGABRT);
			PM_TOSTR_CASE(SIGCHLD);
			PM_TOSTR_CASE(SIGHUP);
			PM_TOSTR_CASE(SIGINT);
			PM_TOSTR_CASE(SIGKILL);
			PM_TOSTR_CASE(SIGQUIT);
			PM_TOSTR_CASE(SIGTERM);
			PM_TOSTR_CASE(SIGUSR1);
			PM_TOSTR_CASE(SIGUSR2);
		default:
			/* postmaster 发出的所有信号都应列在这里 */
			Assert(false);
			return "(unknown)";
	}
#undef PM_TOSTR_CASE

	return "";					/* 消除编译器告警 */
}

/*
 * 向一个 postmaster 子进程发送信号
 *
 * 在拥有 setsid() 的系统上，每个子进程都会把自己设置为一个进程组
 * 的组长。对于通常会被恰当解释的信号，我们向整个进程组发信号，而
 * 不仅仅是直接子进程。这让我们可以，例如，向一个被阻塞的
 * archive_recovery 脚本发送 SIGQUIT，或者向一个后端通过 system() 运行的
 * 脚本发送 SIGINT。
 *
 * 对于刚 fork 出来的子进程存在一个竞态条件：它们可能还没执行
 * setsid()。因此我们不仅向组发信号，也直接向子进程发信号。我们假设
 * 这样的子进程在试图生成任何孙进程之前会先处理该信号。我们也假设
 * 向子进程发两次信号不会造成任何问题。
 */
static void
signal_child(PMChild *pmchild, int signal)
{
	pid_t		pid = pmchild->pid;

	ereport(DEBUG3,
			(errmsg_internal("sending signal %d/%s to %s process with pid %d",
							 signal, pm_signame(signal),
							 GetBackendTypeDesc(pmchild->bkend_type),
							 (int) pmchild->pid)));

	if (kill(pid, signal) < 0)
		elog(DEBUG3, "kill(%ld,%d) failed: %m", (long) pid, signal);
#ifdef HAVE_SETSID
	switch (signal)
	{
		case SIGINT:
		case SIGTERM:
		case SIGQUIT:
		case SIGKILL:
		case SIGABRT:
			if (kill(-pid, signal) < 0)
				elog(DEBUG3, "kill(%ld,%d) failed: %m", (long) (-pid), signal);
			break;
		default:
			break;
	}
#endif
}

/*
 * 向目标子进程发送信号。
 */
static bool
SignalChildren(int signal, BackendTypeMask targetMask)
{
	dlist_iter	iter;
	bool		signaled = false;

	dlist_foreach(iter, &ActiveChildList)
	{
		PMChild    *bp = dlist_container(PMChild, elem, iter.cur);

		/*
		 * 如果我们需要区分 B_BACKEND 和 B_WAL_SENDER，检查是否有任何
		 * B_BACKEND 后端最近宣布它们实际上是 WAL sender。
		 */
		if (btmask_contains(targetMask, B_WAL_SENDER) != btmask_contains(targetMask, B_BACKEND) &&
			bp->bkend_type == B_BACKEND)
		{
			if (IsPostmasterChildWalSender(bp->child_slot))
				bp->bkend_type = B_WAL_SENDER;
		}

		if (!btmask_contains(targetMask, bp->bkend_type))
			continue;

		signal_child(bp, signal);
		signaled = true;
	}
	return signaled;
}

/*
 * 向子进程发送终止信号。这会考虑我们所有的子进程，syslogger 除外。
 */
static void
TerminateChildren(int signal)
{
	SignalChildren(signal, btmask_all_except(B_LOGGER));
	if (StartupPMChild != NULL)
	{
		if (signal == SIGQUIT || signal == SIGKILL || signal == SIGABRT)
			StartupStatus = STARTUP_SIGNALED;
	}
}

/*
 * BackendStartup —— 启动后端进程
 *
 * 返回值：如果 fork 失败则为 STATUS_ERROR，否则为 STATUS_OK。
 *
 * 注意：如果你修改了这段代码，也要考虑 StartAutovacuumWorker 和
 * StartBackgroundWorker。
 */
static int
BackendStartup(ClientSocket *client_sock)
{
	PMChild    *bn = NULL;
	pid_t		pid;
	BackendStartupData startup_data;
	CAC_state	cac;

	/*
	 * 记录 postmaster 从 accept 获得套接字的时间（用于记录连接建立
	 * 和设置的总耗时）。
	 */
	startup_data.socket_created = GetCurrentTimestamp();

	/*
	 * 分配并指派子进程槽位。注意我们必须在 fork 之前做这件事，以便
	 * 能够干净地处理失败（内存不足或子进程槽位不足）。
	 */
	cac = canAcceptConnections(B_BACKEND);
	if (cac == CAC_OK)
	{
		/* 以后可以改为 B_WAL_SENDER */
		bn = AssignPostmasterChildSlot(B_BACKEND);
		if (!bn)
		{
			/*
			 * 普通子进程太多了；改为启动一个 dead-end 子进程。
			 */
			cac = CAC_TOOMANY;
		}
	}
	if (!bn)
	{
		bn = AllocDeadEndChild();
		if (!bn)
		{
			ereport(LOG,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
			return STATUS_ERROR;
		}
	}

	/* 向下传递 canAcceptConnections 的状态 */
	startup_data.canAcceptConnections = cac;
	bn->rw = NULL;

	/* 尚未请求被任何 bgworker 通知 */
	bn->bgworker_notify = false;

	pid = postmaster_child_launch(bn->bkend_type, bn->child_slot,
								  &startup_data, sizeof(startup_data),
								  client_sock);
	if (pid < 0)
	{
		/* 在父进程中，fork 失败 */
		int			save_errno = errno;

		(void) ReleasePostmasterChildSlot(bn);
		errno = save_errno;
		ereport(LOG,
				(errmsg("could not fork new process for connection: %m")));
		report_fork_failure_to_client(client_sock, save_errno);
		return STATUS_ERROR;
	}

	/* 在父进程中，fork 成功 */
	ereport(DEBUG2,
			(errmsg_internal("forked new %s, pid=%d socket=%d",
							 GetBackendTypeDesc(bn->bkend_type),
							 (int) pid, (int) client_sock->sock)));

	/*
	 * 一切顺利，可以安全地将这个后端加入我们的后端列表了。
	 */
	bn->pid = pid;
	return STATUS_OK;
}

/*
 * 在关闭连接之前，尝试向客户端报告后端 fork() 失败。因为我们不想
 * 冒着阻塞 postmaster 的风险来处理这个连接，所以我们将连接设为非阻塞
 * 并只尝试一次。
 *
 * 这是一段粗糙的专用代码；我们不能使用后端 libpq，因为它还没有启动运行。
 */
static void
report_fork_failure_to_client(ClientSocket *client_sock, int errnum)
{
	char		buffer[1000];
	int			rc;

	/* 格式化错误消息数据包（始终使用 V2 协议） */
	snprintf(buffer, sizeof(buffer), "E%s%s\n",
			 _("could not fork new process for connection: "),
			 strerror(errnum));

	/* 将 port 设置为非阻塞。如果失败则不执行 send() */
	if (!pg_set_noblock(client_sock->sock))
		return;

	/* 在 EINTR 之后我们会重试，但忽略所有其他失败 */
	do
	{
		rc = send(client_sock->sock, buffer, strlen(buffer) + 1, 0);
	} while (rc < 0 && errno == EINTR);
}

/*
 * ExitPostmaster —— 清理
 *
 * 不要直接调用 exit() —— 始终经过这里！
 */
static void
ExitPostmaster(int status)
{
#ifdef HAVE_PTHREAD_IS_THREADED_NP

	/*
	 * 没有已知的理由会让 postmaster 在启动之后变成多线程。不过，我们
	 * 可能在到达 PostmasterMain 中的测试之前就通过错误退出到达这里，
	 * 因此提供与那里相同的提示。这条消息使用 LOG 级别，因为此时不干净的
	 * 关闭通常与干净的关闭看起来差别不大。
	 */
	if (pthread_is_threaded_np() != 0)
		ereport(LOG,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("postmaster became multithreaded"),
				 errhint("Set the LC_ALL environment variable to a valid locale.")));
#endif

	/* 应该清理共享内存并杀死所有后端 */

	/*
	 * 这里语义不太确定。当 Postmaster 死掉时，是否应该杀死所有后端？
	 * 大概不应该。
	 *
	 * 必须 —— vadim 1999-10-05
	 */

	proc_exit(status);
}

/*
 * 处理代表后端请求的 pmsignal 条件，并检查来自 pg_ctl 的提升和
 * logrotate 请求。
 */
static void
process_pm_pmsignal(void)
{
	bool		request_state_update = false;

	pending_pm_pmsignal = false;

	ereport(DEBUG2,
			(errmsg_internal("postmaster received pmsignal signal")));

	/*
	 * RECOVERY_STARTED 和 BEGIN_HOT_STANDBY 信号在非预期状态下会被忽略。
	 * 如果启动进程很快启动、完成恢复并退出，我们可能会先处理启动进程的
	 * 死亡。在这种情况下，我们不希望回到恢复状态。
	 */
	if (CheckPostmasterSignal(PMSIGNAL_RECOVERY_STARTED) &&
		pmState == PM_STARTUP && Shutdown == NoShutdown)
	{
		/* WAL 重做已经开始。我们已脱离重新初始化。 */
		FatalError = false;
		AbortStartTime = 0;
		reachedConsistency = false;

		/*
		 * 如果我们负责（重新）归档收到的文件，就启动归档进程。
		 */
		Assert(PgArchPMChild == NULL);
		if (XLogArchivingAlways())
			PgArchPMChild = StartChildProcess(B_ARCHIVER);

		/*
		 * 如果我们不打算稍后进入热备模式，就把 RECOVERY_STARTED 视为
		 * 我们已脱离启动状态，并相应地报告状态。
		 */
		if (!EnableHotStandby)
		{
			AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STANDBY);
#ifdef USE_SYSTEMD
			sd_notify(0, "READY=1");
#endif
		}

		UpdatePMState(PM_RECOVERY);
	}

	if (CheckPostmasterSignal(PMSIGNAL_RECOVERY_CONSISTENT) &&
		pmState == PM_RECOVERY && Shutdown == NoShutdown)
	{
		reachedConsistency = true;
	}

	if (CheckPostmasterSignal(PMSIGNAL_BEGIN_HOT_STANDBY) &&
		(pmState == PM_RECOVERY && Shutdown == NoShutdown))
	{
		ereport(LOG,
				(errmsg("database system is ready to accept read-only connections")));

		/* 报告状态 */
		AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_READY);
#ifdef USE_SYSTEMD
		sd_notify(0, "READY=1");
#endif

		UpdatePMState(PM_HOT_STANDBY);
		connsAllowed = true;

		/* 某些工作进程可能已被安排现在启动 */
		StartWorkerNeeded = true;
	}

	/* 处理后台工作进程的状态变化。 */
	if (CheckPostmasterSignal(PMSIGNAL_BACKGROUND_WORKER_CHANGE))
	{
		/* 只有在不停止时才接受新的工作进程请求。 */
		BackgroundWorkerStateChange(pmState < PM_STOP_BACKENDS);
		StartWorkerNeeded = true;
	}

	/* 如果被请求，告诉 syslogger 轮转日志文件 */
	if (SysLoggerPMChild != NULL)
	{
		if (CheckLogrotateSignal())
		{
			signal_child(SysLoggerPMChild, SIGUSR1);
			RemoveLogrotateSignalFiles();
		}
		else if (CheckPostmasterSignal(PMSIGNAL_ROTATE_LOGFILE))
		{
			signal_child(SysLoggerPMChild, SIGUSR1);
		}
	}

	if (CheckPostmasterSignal(PMSIGNAL_START_AUTOVAC_LAUNCHER) &&
		Shutdown <= SmartShutdown && pmState < PM_STOP_BACKENDS)
	{
		/*
		 * 启动 autovacuum 守护进程的一次迭代，即使 autovacuum 名义上
		 * 未启用。这样我们就能对事务 ID 回卷（wraparound）保持主动
		 * 防御。我们为主循环设置一个标志由它来执行，而不是尝试在这里
		 * 执行 —— 这是因为 autovac 进程本身可能发送该信号，而我们希望
		 * 在当前一次迭代完成后立即启动另一次迭代来处理它。
		 */
		start_autovac_launcher = true;
	}

	if (CheckPostmasterSignal(PMSIGNAL_START_AUTOVAC_WORKER) &&
		Shutdown <= SmartShutdown && pmState < PM_STOP_BACKENDS)
	{
		/* autovacuum launcher 希望我们启动一个 worker 进程。 */
		StartAutovacuumWorker();
	}

	if (CheckPostmasterSignal(PMSIGNAL_START_WALRECEIVER))
	{
		/* 启动进程希望我们启动 walreceiver 进程。 */
		WalReceiverRequested = true;
	}

	if (CheckPostmasterSignal(PMSIGNAL_XLOG_IS_SHUTDOWN))
	{
		/* Checkpointer 已完成关闭检查点 */
		if (pmState == PM_WAIT_XLOG_SHUTDOWN)
		{
			/*
			 * 如果我们有 archiver 子进程，告诉它执行最后一个归档周期
			 * 然后退出。同样，如果我们有 walsender 进程，告诉它们发送
			 * 任何剩余的 WAL 然后退出。
			 */
			Assert(Shutdown > NoShutdown);

			/* 最后一次唤醒 archiver */
			if (PgArchPMChild != NULL)
				signal_child(PgArchPMChild, SIGUSR2);

			/*
			 * 最后一次唤醒 walsender。此时应该不再有普通 backend 存在。
			 */
			SignalChildren(SIGUSR2, btmask(B_WAL_SENDER));

			UpdatePMState(PM_WAIT_XLOG_ARCHIVAL);
		}
		else if (!FatalError && Shutdown != ImmediateShutdown)
		{
			/*
			 * Checkpointer 只应在关闭期间执行关闭检查点。如果 checkpointer
			 * 因某种原因在其他情形下这样做了，我们别无选择只能崩溃重启
			 *（crash-restart）。
			 *
			 * 但是，如果一次有序关闭被崩溃或立即关闭“打断”，我们也可能
			 * 在 PM_WAIT_XLOG_SHUTDOWN 之外收到 PMSIGNAL_XLOG_IS_SHUTDOWN。
			 */
			ereport(LOG,
					(errmsg("WAL was shut down unexpectedly")));

			/*
			 * 在这里将 send_abort_for_crash 纳入考虑似乎没有帮助。
			 */
			HandleFatalError(PMQUIT_FOR_CRASH, false);
		}

		/*
		 * 需要运行 PostmasterStateMachine() 来检查我们是否已经可以进入
		 * 下一个状态。
		 */
		request_state_update = true;
	}

	/*
	 * 如果某个子进程请求，则尝试推进 postmaster 的状态机。
	 */
	if (CheckPostmasterSignal(PMSIGNAL_ADVANCE_STATE_MACHINE))
	{
		request_state_update = true;
	}

	/*
	 * 注意此动作相对于本函数其他动作的顺序。一般来说，这应该在其他动作
	 * 之后进行，以防它们产生 PostmasterStateMachine 需要知晓的影响。
	 * 不过，我们应该在 CheckPromoteSignal 步骤之前执行它；后者不会对
	 * 状态机产生任何（即时的）影响，但确实依赖于我们现在所处的状态。
	 */
	if (request_state_update)
	{
		PostmasterStateMachine();
	}

	if (StartupPMChild != NULL &&
		(pmState == PM_STARTUP || pmState == PM_RECOVERY ||
		 pmState == PM_HOT_STANDBY) &&
		CheckPromoteSignal())
	{
		/*
		 * 告诉启动进程完成恢复。
		 *
		 * 保留 promote 信号文件不动，让启动进程去执行 unlink。
		 */
		signal_child(StartupPMChild, SIGUSR2);
	}
}

/*
 * 空的（Dummy）信号处理器
 *
 * 我们将其用于那些在 postmaster 中实际不使用、但在 backend 中会使用的
 * 信号。如果我们在 postmaster 中对这类信号使用 SIG_IGN，那么新启动的
 * backend 可能会丢弃一个在它能够重新配置其信号处理之前就到达的信号。
 *（参见 tcop/postgres.c 中的注释。）
 */
static void
dummy_handler(SIGNAL_ARGS)
{
}

/*
 * 统计指定类型的子进程数量。
 */
static int
CountChildren(BackendTypeMask targetMask)
{
	dlist_iter	iter;
	int			cnt = 0;

	dlist_foreach(iter, &ActiveChildList)
	{
		PMChild    *bp = dlist_container(PMChild, elem, iter.cur);

		/*
		 * 如果我们需要区分 B_BACKEND 和 B_WAL_SENDER，检查是否有任何
		 * B_BACKEND 后端最近宣告它们实际上是 WAL sender。
		 */
		if (btmask_contains(targetMask, B_WAL_SENDER) != btmask_contains(targetMask, B_BACKEND) &&
			bp->bkend_type == B_BACKEND)
		{
			if (IsPostmasterChildWalSender(bp->child_slot))
				bp->bkend_type = B_WAL_SENDER;
		}

		if (!btmask_contains(targetMask, bp->bkend_type))
			continue;

		ereport(DEBUG4,
				(errmsg_internal("%s process %d is still running",
								 GetBackendTypeDesc(bp->bkend_type), (int) bp->pid)));

		cnt++;
	}
	return cnt;
}


/*
 * StartChildProcess —— 为 postmaster 启动一个辅助进程
 *
 * "type" 决定将启动哪种类型的子进程。所有子进程类型最初都会进入
 * AuxiliaryProcessMain，由它处理通用的初始化设置。
 *
 * StartChildProcess 的返回值是子进程的 PMChild 条目，失败时返回 NULL。
 */
static PMChild *
StartChildProcess(BackendType type)
{
	PMChild    *pmchild;
	pid_t		pid;

	pmchild = AssignPostmasterChildSlot(type);
	if (!pmchild)
	{
		if (type == B_AUTOVAC_WORKER)
			ereport(LOG,
					(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
					 errmsg("no slot available for new autovacuum worker process")));
		else
		{
			/* 不应发生，因为我们分配了足够的槽位 */
			elog(LOG, "no postmaster child slot available for aux process");
		}
		return NULL;
	}

	pid = postmaster_child_launch(type, pmchild->child_slot, NULL, 0, NULL);
	if (pid < 0)
	{
		/* 在父进程中，fork 失败 */
		ReleasePostmasterChildSlot(pmchild);
		ereport(LOG,
				(errmsg("could not fork \"%s\" process: %m", PostmasterChildName(type))));

		/*
		 * 在启动期间 fork 失败是致命的，但如果启动其他类型的子进程失败，
		 * 则无需立即崩溃。
		 */
		if (type == B_STARTUP)
			ExitPostmaster(1);
		return NULL;
	}

	/* 在父进程中，fork 成功 */
	pmchild->pid = pid;
	return pmchild;
}

/*
 * StartSysLogger —— 启动 syslogger 进程
 */
void
StartSysLogger(void)
{
	Assert(SysLoggerPMChild == NULL);

	SysLoggerPMChild = AssignPostmasterChildSlot(B_LOGGER);
	if (!SysLoggerPMChild)
		elog(PANIC, "no postmaster child slot available for syslogger");
	SysLoggerPMChild->pid = SysLogger_Start(SysLoggerPMChild->child_slot);
	if (SysLoggerPMChild->pid == 0)
	{
		ReleasePostmasterChildSlot(SysLoggerPMChild);
		SysLoggerPMChild = NULL;
	}
}

/*
 * StartAutovacuumWorker
 *		启动一个 autovac worker 进程。
 *
 * 该函数放在这里是因为它会把生成的 PID 录入 postmaster 的私有
 * backend 列表中。
 *
 * 注意 —— 这段代码大致与 BackendStartup 相匹配。
 */
static void
StartAutovacuumWorker(void)
{
	PMChild    *bn;

	/*
	 * 如果当前不具备运行一个进程的条件，则不要尝试，而是像 fork 失败
	 * 那样处理它。这通常不会发生，因为该信号本应只在可以这样做时由
	 * autovacuum launcher 发送，但我们必须进行检查，以避免在数据库
	 * 状态变化期间出现竞态条件问题。
	 */
	if (canAcceptConnections(B_AUTOVAC_WORKER) == CAC_OK)
	{
		bn = StartChildProcess(B_AUTOVAC_WORKER);
		if (bn)
		{
			bn->bgworker_notify = false;
			bn->rw = NULL;
			return;
		}
		else
		{
			/*
			 * fork 失败，继续向下执行以进行报告 —— 实际的错误消息已由
			 * StartChildProcess 记录
			 */
		}
	}

	/*
	 * 如果 launcher 正在运行，则将失败情况报告给它。（如果它没有运行，
	 * 我们甚至可能还没有连接到共享内存，所以不要尝试调用
	 * AutoVacWorkerFailed。）注意我们还需要给它发送信号以便它对该情况
	 * 作出响应，但我们不在这里做，而是等待 ServerLoop 来做。这样在情况
	 * 变得糟糕时，我们可以避免 autovac launcher 与 postmaster 之间快速
	 * 连续的乒乓式信号往返。
	 */
	if (AutoVacLauncherPMChild != NULL)
	{
		AutoVacWorkerFailed();
		avlauncher_needs_signal = true;
	}
}


/*
 * 创建 opts 文件
 */
static bool
CreateOptsFile(int argc, char *argv[], char *fullprogname)
{
	FILE	   *fp;
	int			i;

#define OPTS_FILE	"postmaster.opts"

	if ((fp = fopen(OPTS_FILE, "w")) == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", OPTS_FILE)));
		return false;
	}

	fprintf(fp, "%s", fullprogname);
	for (i = 1; i < argc; i++)
		fprintf(fp, " \"%s\"", argv[i]);
	fputs("\n", fp);

	if (fclose(fp))
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", OPTS_FILE)));
		return false;
	}

	return true;
}


/*
 * 启动一个新的 bgworker。
 * 启动时间条件必须已经被检查过。
 *
 * 成功返回 true，失败返回 false。
 * 无论哪种情况，都会相应地更新 RegisteredBgWorker 的状态。
 *
 * 注意 —— 这段代码大致与 BackendStartup 相匹配。
 */
static bool
StartBackgroundWorker(RegisteredBgWorker *rw)
{
	PMChild    *bn;
	pid_t		worker_pid;

	Assert(rw->rw_pid == 0);

	/*
	 * 分配并指派子进程槽位。注意我们必须在 fork 之前完成这一步，以便能够
	 * 干净地处理失败情况（内存不足或子进程槽位不足）。
	 *
	 * 将失败当作 worker 已崩溃来处理。这样，postmaster 会在再次尝试启动
	 * 它之前等待一段时间；如果我们立即重试，很可能会再次遇到同样的资源
	 * 耗尽情况。
	 */
	bn = AssignPostmasterChildSlot(B_BG_WORKER);
	if (bn == NULL)
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("no slot available for new background worker process")));
		rw->rw_crashed_at = GetCurrentTimestamp();
		return false;
	}
	bn->rw = rw;
	bn->bkend_type = B_BG_WORKER;
	bn->bgworker_notify = false;

	ereport(DEBUG1,
			(errmsg_internal("starting background worker process \"%s\"",
							 rw->rw_worker.bgw_name)));

	worker_pid = postmaster_child_launch(B_BG_WORKER, bn->child_slot,
										 &rw->rw_worker, sizeof(BackgroundWorker), NULL);
	if (worker_pid == -1)
	{
		/* 在 postmaster 中，fork 失败 …… */
		ereport(LOG,
				(errmsg("could not fork background worker process: %m")));
		/* 撤销 AssignPostmasterChildSlot 所做的操作 */
		ReleasePostmasterChildSlot(bn);

		/* 将该条目标记为已崩溃，以便我们稍后再次尝试 */
		rw->rw_crashed_at = GetCurrentTimestamp();
		return false;
	}

	/* 在 postmaster 中，fork 成功 …… */
	rw->rw_pid = worker_pid;
	bn->pid = rw->rw_pid;
	ReportBackgroundWorkerPID(rw);
	return true;
}

/*
 * 当前的 postmaster 状态是否需要启动一个具有指定 start_time 的 worker？
 */
static bool
bgworker_should_start_now(BgWorkerStartTime start_time)
{
	switch (pmState)
	{
		case PM_NO_CHILDREN:
		case PM_WAIT_CHECKPOINTER:
		case PM_WAIT_DEAD_END:
		case PM_WAIT_XLOG_ARCHIVAL:
		case PM_WAIT_XLOG_SHUTDOWN:
		case PM_WAIT_IO_WORKERS:
		case PM_WAIT_BACKENDS:
		case PM_STOP_BACKENDS:
			break;

		case PM_RUN:
			if (start_time == BgWorkerStart_RecoveryFinished)
				return true;
			/* fall through */

		case PM_HOT_STANDBY:
			if (start_time == BgWorkerStart_ConsistentState)
				return true;
			/* fall through */

		case PM_RECOVERY:
		case PM_STARTUP:
		case PM_INIT:
			if (start_time == BgWorkerStart_PostmasterStart)
				return true;
			/* fall through */
	}

	return false;
}

/*
 * 如果时机合适，启动后台工作进程。
 *
 * 作为副作用，bgworker 控制变量会根据是否可能还需要启动更多 worker
 * 而被设置或重置。
 *
 * 我们限制每次调用启动的 worker 数量，以避免在有很多这类请求待处理时
 * 长时间占用 postmaster 的注意力。只要 StartWorkerNeeded 为 true，
 * ServerLoop 就不会阻塞，并会在处理完其他任何问题后再次调用本函数。
 */
static void
maybe_start_bgworkers(void)
{
#define MAX_BGWORKERS_TO_LAUNCH 100
	int			num_launched = 0;
	TimestampTz now = 0;
	dlist_mutable_iter iter;

	/*
	 * 在崩溃恢复期间，在状态转换出恢复之前，我们无需被调用。
	 */
	if (FatalError)
	{
		StartWorkerNeeded = false;
		HaveCrashedWorker = false;
		return;
	}

	/* 除非在下面发现需要的理由，否则不需要再次被调用 */
	StartWorkerNeeded = false;
	HaveCrashedWorker = false;

	dlist_foreach_modify(iter, &BackgroundWorkerList)
	{
		RegisteredBgWorker *rw;

		rw = dlist_container(RegisteredBgWorker, rw_lnode, iter.cur);

		/* 如果已经在运行则忽略 */
		if (rw->rw_pid != 0)
			continue;

		/* 如果被标记为待终止，则清理并从列表中移除 */
		if (rw->rw_terminate)
		{
			ForgetBackgroundWorker(rw);
			continue;
		}

		/*
		 * 如果该 worker 之前崩溃过，也许它需要被重启（除非它在注册时指定
		 * 完全不希望被重启）。检查上一次崩溃发生在多久之前。如果上次崩溃
		 * 距现在太近，不要立即启动它；等经过足够的时间后再让它重启。
		 */
		if (rw->rw_crashed_at != 0)
		{
			if (rw->rw_worker.bgw_restart_time == BGW_NEVER_RESTART)
			{
				int			notify_pid;

				notify_pid = rw->rw_worker.bgw_notify_pid;

				ForgetBackgroundWorker(rw);

				/* 现在报告该 worker 已消失。 */
				if (notify_pid != 0)
					kill(notify_pid, SIGUSR1);

				continue;
			}

			/* 只在需要时才读取系统时间 */
			if (now == 0)
				now = GetCurrentTimestamp();

			if (!TimestampDifferenceExceeds(rw->rw_crashed_at, now,
											rw->rw_worker.bgw_restart_time * 1000))
			{
				/* 设置标志以记住我们有 worker 需要稍后启动 */
				HaveCrashedWorker = true;
				continue;
			}
		}

		if (bgworker_should_start_now(rw->rw_worker.bgw_start_time))
		{
			/* 在尝试启动 worker 之前重置崩溃时间 */
			rw->rw_crashed_at = 0;

			/*
			 * 尝试启动该 worker。
			 *
			 * 失败时，暂时放弃处理 worker，但设置 StartWorkerNeeded，
			 * 以便我们在 ServerLoop 的下一次迭代中回到这里重试。（我们
			 * 不想等待，因为可能还有其他准备就绪可运行的 worker。）我们
			 * 也可以设置 HaveCrashedWorker，因为该 worker 现在已被标记
			 * 为崩溃，但没有必要，因为本函数的下一次运行会处理这件事。
			 */
			if (!StartBackgroundWorker(rw))
			{
				StartWorkerNeeded = true;
				return;
			}

			/*
			 * 如果我们已经启动了允许的最大数量的 worker，则退出，但让
			 * ServerLoop 再次调用我们，以查找其他准备就绪可运行的 worker。
			 * 可能一个都没有，但我们会在下一次运行时得知。
			 */
			if (++num_launched >= MAX_BGWORKERS_TO_LAUNCH)
			{
				StartWorkerNeeded = true;
				return;
			}
		}
	}
}

static bool
maybe_reap_io_worker(int pid)
{
	for (int i = 0; i < MAX_IO_WORKERS; ++i)
	{
		if (io_worker_children[i] &&
			io_worker_children[i]->pid == pid)
		{
			ReleasePostmasterChildSlot(io_worker_children[i]);

			--io_worker_count;
			io_worker_children[i] = NULL;
			return true;
		}
	}
	return false;
}

/*
 * 启动或停止 IO worker，以缩小正在运行的 worker 数量与配置的 worker
 * 数量之间的差距。用于响应 io_workers GUC 的变化（通过增加或减少
 * worker 数量），以及响应 worker 因错误而终止的情况（通过启动“替补”
 * worker）。
 */
static void
maybe_adjust_io_workers(void)
{
	if (!pgaio_workers_enabled())
		return;

	/*
	 * 如果我们处于最终关闭状态，那么我们只是在等待所有进程退出。
	 */
	if (pmState >= PM_WAIT_IO_WORKERS)
		return;

	/* 在立即关闭（immediate shutdown）期间也不要启动新的 worker。 */
	if (Shutdown >= ImmediateShutdown)
		return;

	/*
	 * 如果我们处于崩溃重启的关闭阶段，不要启动新的 worker。但如果我们
	 * 已经在重新启动，那么我们*确实*需要启动。
	 */
	if (FatalError && pmState >= PM_STOP_BACKENDS)
		return;

	Assert(pmState < PM_WAIT_IO_WORKERS);

	/* 运行的数量不够？ */
	while (io_worker_count < io_workers)
	{
		PMChild    *child;
		int			i;

		/* 在 io_worker_children 数组中查找未使用的条目 */
		for (i = 0; i < MAX_IO_WORKERS; ++i)
		{
			if (io_worker_children[i] == NULL)
				break;
		}
		if (i == MAX_IO_WORKERS)
			elog(ERROR, "could not find a free IO worker slot");

		/* 尝试启动一个。 */
		child = StartChildProcess(B_IO_WORKER);
		if (child != NULL)
		{
			io_worker_children[i] = child;
			++io_worker_count;
		}
		else
			break;				/* 下次再试 */
	}

	/* 运行的数量太多？ */
	if (io_worker_count > io_workers)
	{
		/* 请求处于最高槽位的 IO worker 退出 */
		for (int i = MAX_IO_WORKERS - 1; i >= 0; --i)
		{
			if (io_worker_children[i] != NULL)
			{
				kill(io_worker_children[i]->pid, SIGUSR2);
				break;
			}
		}
	}
}


/*
 * 当某个 backend 请求在 worker 状态变化时得到通知时，我们会在它的
 * backend 条目中设置一个标志。后台工作进程机制需要知道这类 backend
 * 何时退出。
 */
bool
PostmasterMarkPIDForWorkerNotify(int pid)
{
	dlist_iter	iter;
	PMChild    *bp;

	dlist_foreach(iter, &ActiveChildList)
	{
		bp = dlist_container(PMChild, elem, iter.cur);
		if (bp->pid == pid)
		{
			bp->bgworker_notify = true;
			return true;
		}
	}
	return false;
}

#ifdef WIN32

/*
 * 面向 Windows 的 waitpid() 的子集实现。我们假定 pid 为 -1
 *（即检查所有子进程），且 options 为 WNOHANG（不等待）。
 */
static pid_t
waitpid(pid_t pid, int *exitstatus, int options)
{
	win32_deadchild_waitinfo *childinfo;
	DWORD		exitcode;
	DWORD		dwd;
	ULONG_PTR	key;
	OVERLAPPED *ovl;

	/* 尝试从队列中消费一个 win32_deadchild_waitinfo。 */
	if (!GetQueuedCompletionStatus(win32ChildQueue, &dwd, &key, &ovl, 0))
	{
		errno = EAGAIN;
		return -1;
	}

	childinfo = (win32_deadchild_waitinfo *) key;
	pid = childinfo->procId;

	/*
	 * 将句柄从等待中移除 —— 即使它被设置为只等待一次，这也是必需的
	 */
	UnregisterWaitEx(childinfo->waitHandle, NULL);

	if (!GetExitCodeProcess(childinfo->procHandle, &exitcode))
	{
		/*
		 * 永远不应发生。通知用户并设置一个固定的退出码。
		 */
		write_stderr("could not read exit code for process\n");
		exitcode = 255;
	}
	*exitstatus = exitcode;

	/*
	 * 关闭进程句柄。只有在这一点之后，PID 才能被内核回收再利用。
	 */
	CloseHandle(childinfo->procHandle);

	/*
	 * 释放在调用 RegisterWaitForSingleObject() 之前分配的结构体
	 */
	pfree(childinfo);

	return pid;
}

/*
 * 注意！下面的代码在线程池上执行！所有操作都必须是线程安全的！
 * 注意 elog() 及其同类函数*不能*被使用。
 */
static void WINAPI
pgwin32_deadchild_callback(PVOID lpParameter, BOOLEAN TimerOrWaitFired)
{
	/* 永远不应发生，因为我们使用 INFINITE 作为超时值。 */
	if (TimerOrWaitFired)
		return;

	/*
	 * 投递 win32_deadchild_waitinfo 对象以供 waitpid() 处理。如果失败，
	 * 我们会泄漏该对象，但我们同时也会泄漏整个进程并进入一个不可恢复的
	 * 状态，所以对此担忧意义不大。我们本想 panic，但无法从这个线程使用
	 * 那套基础设施。
	 */
	if (!PostQueuedCompletionStatus(win32ChildQueue,
									0,
									(ULONG_PTR) lpParameter,
									NULL))
		write_stderr("could not post child completion status\n");

	/* 将 SIGCHLD 信号入队。 */
	pg_queue_signal(SIGCHLD);
}

/*
 * 排入一个等待者，以便在该子进程死亡时发出信号。该等待将由操作系统的
 * 线程池自动处理。内存和进程句柄将由稍后对 waitpid() 的调用释放。
 */
void
pgwin32_register_deadchild_callback(HANDLE procHandle, DWORD procId)
{
	win32_deadchild_waitinfo *childinfo;

	childinfo = palloc(sizeof(win32_deadchild_waitinfo));
	childinfo->procHandle = procHandle;
	childinfo->procId = procId;

	if (!RegisterWaitForSingleObject(&childinfo->waitHandle,
									 procHandle,
									 pgwin32_deadchild_callback,
									 childinfo,
									 INFINITE,
									 WT_EXECUTEONLYONCE | WT_EXECUTEINWAITTHREAD))
		ereport(FATAL,
				(errmsg_internal("could not register process for wait: error code %lu",
								 GetLastError())));
}

#endif							/* WIN32 */

/*
 * 初始化用于监控 postmaster 死亡的唯一句柄。
 *
 * 在 postmaster 中被调用一次，以便子进程随后可以监控它们的父进程
 * 是否已死亡。
 */
static void
InitPostmasterDeathWatchHandle(void)
{
#ifndef WIN32

	/*
	 * 创建一个管道。Postmaster 保持管道的写端处于打开状态
	 *（POSTMASTER_FD_OWN），子进程则持有读端。子进程可以将读文件描述符
	 * 传给 select()，以便在 postmaster 死亡时被唤醒，或通过
	 *（read() == 0）检查 postmaster 是否死亡。子进程在 fork 之后必须
	 * 尽快关闭写端，因为在所有进程都关闭写 fd 之前，读端不会收到 EOF
	 * 信号。这一点在 ClosePostmasterPorts() 中处理。
	 */
	Assert(MyProcPid == PostmasterPid);
	if (pipe(postmaster_alive_fds) < 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg_internal("could not create pipe to monitor postmaster death: %m")));

	/* 通知 fd.c 我们为该管道占用了两个 FD。 */
	ReserveExternalFD();
	ReserveExternalFD();

	/*
	 * 设置 O_NONBLOCK，以允许通过 read() 调用来测试该 fd 是否存在。
	 */
	if (fcntl(postmaster_alive_fds[POSTMASTER_FD_WATCH], F_SETFL, O_NONBLOCK) == -1)
		ereport(FATAL,
				(errcode_for_socket_access(),
				 errmsg_internal("could not set postmaster death monitoring pipe to nonblocking mode: %m")));
#else

	/*
	 * 在 Windows 上，我们使用进程句柄来达到相同的目的。
	 */
	if (DuplicateHandle(GetCurrentProcess(),
						GetCurrentProcess(),
						GetCurrentProcess(),
						&PostmasterHandle,
						0,
						TRUE,
						DUPLICATE_SAME_ACCESS) == 0)
		ereport(FATAL,
				(errmsg_internal("could not duplicate postmaster handle: error code %lu",
								 GetLastError())));
#endif							/* WIN32 */
}
