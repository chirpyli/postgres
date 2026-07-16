/*-------------------------------------------------------------------------
 *
 * postmaster.h
 *	  postmaster/postmaster.c 对外导出的内容。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/postmaster/postmaster.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _POSTMASTER_H
#define _POSTMASTER_H

#include "lib/ilist.h"
#include "miscadmin.h"

/*
 * 表示一个活跃的 postmaster 子进程的结构体。它主要用于记录我们
 * 拥有多少个子进程，并在必要时向它们发送相应的信号。所有 postmaster
 * 子进程都会被分配一个 PMChild 条目。这包括“普通”客户端会话，
 * 也包括 autovacuum 工作进程、walsender、后台工作进程以及辅助进程。
 * （注意，在启动之初，walsender 被标记为 B_BACKEND；当我们注意到它们
 * 修改了各自的 PMChildFlags 条目后，会将其重新标记为 B_WAL_SENDER。
 * 因此该检查必须在进行任何需要区分 walsender 与普通后端的操作之前完成。）
 *
 * “dead-end”（死胡同）子进程也会被分配一个 PMChild 条目：这些子进程
 * 仅为向一个意图连接的客户端发送友好的拒绝消息而启动。我们必须跟踪它们，
 * 因为它们关联到了共享内存，但我们知道它们永远不会成为活跃的后端。
 *
 * child_slot 是一个在所有运行中的子进程内唯一的标识符。它被用作
 * PMChildFlags 数组的索引。dead-end 子进程不会被分配 child_slot，
 * 其 child_slot == 0（有效的 child_slot 编号从 1 开始）。
 */
typedef struct
{
	pid_t		pid;			/* 后端进程的进程 id */
	int			child_slot;		/* 本后端的 PMChildSlot（如有） */
	BackendType bkend_type;		/* 子进程类型，见上文 */
	struct RegisteredBgWorker *rw;	/* 若该进程为 bgworker，则为相关信息 */
	bool		bgworker_notify;	/* 接收 bgworker 启动/停止通知 */
	dlist_node	elem;			/* ActiveChildList 中的链表节点 */
} PMChild;

#ifdef EXEC_BACKEND
extern PGDLLIMPORT int num_pmchild_slots;
#endif

/* GUC 选项 */
extern PGDLLIMPORT bool EnableSSL;
extern PGDLLIMPORT int SuperuserReservedConnections;
extern PGDLLIMPORT int ReservedConnections;
extern PGDLLIMPORT int PostPortNumber;
extern PGDLLIMPORT int Unix_socket_permissions;
extern PGDLLIMPORT char *Unix_socket_group;
extern PGDLLIMPORT char *Unix_socket_directories;
extern PGDLLIMPORT char *ListenAddresses;
extern PGDLLIMPORT bool ClientAuthInProgress;
extern PGDLLIMPORT int PreAuthDelay;
extern PGDLLIMPORT int AuthenticationTimeout;
extern PGDLLIMPORT bool log_hostname;
extern PGDLLIMPORT bool enable_bonjour;
extern PGDLLIMPORT char *bonjour_name;
extern PGDLLIMPORT bool restart_after_crash;
extern PGDLLIMPORT bool remove_temp_files_after_crash;
extern PGDLLIMPORT bool send_abort_for_crash;
extern PGDLLIMPORT bool send_abort_for_kill;

#ifdef WIN32
extern PGDLLIMPORT HANDLE PostmasterHandle;
#else
extern PGDLLIMPORT int postmaster_alive_fds[2];

/*
 * 这些常量表示 postmaster_alive_fds 中哪一个由 postmaster 持有，
 * 哪一个由子进程用于检测 postmaster 是否已经退出。
 */
#define POSTMASTER_FD_WATCH		0	/* 由子进程用于检测
									 * postmaster 是否退出 */
#define POSTMASTER_FD_OWN		1	/* 仅由 postmaster 保持打开 */
#endif

extern PGDLLIMPORT const char *progname;

extern PGDLLIMPORT bool redirection_done;
extern PGDLLIMPORT bool LoadedSSL;

pg_noreturn extern void PostmasterMain(int argc, char *argv[]);
extern void ClosePostmasterPorts(bool am_syslogger);
extern void InitProcessGlobals(void);

extern int	MaxLivePostmasterChildren(void);

extern bool PostmasterMarkPIDForWorkerNotify(int);

#ifdef WIN32
extern void pgwin32_register_deadchild_callback(HANDLE procHandle, DWORD procId);
#endif

/* 定义于 globals.c */
extern PGDLLIMPORT struct ClientSocket *MyClientSocket;

/* launch_backend.c 中函数的原型声明 */
extern pid_t postmaster_child_launch(BackendType child_type,
									 int child_slot,
									 void *startup_data,
									 size_t startup_data_len,
									 struct ClientSocket *client_sock);
const char *PostmasterChildName(BackendType child_type);
#ifdef EXEC_BACKEND
pg_noreturn extern void SubPostmasterMain(int argc, char *argv[]);
#endif

/* 定义于 pmchild.c */
extern PGDLLIMPORT dlist_head ActiveChildList;

extern void InitPostmasterChildSlots(void);
extern PMChild *AssignPostmasterChildSlot(BackendType btype);
extern PMChild *AllocDeadEndChild(void);
extern bool ReleasePostmasterChildSlot(PMChild *pmchild);
extern PMChild *FindPostmasterChildByPid(int pid);

/*
 * 这些值对应用于分派到各个子程序的、必须排在最前面的特殊选项。
 * 可以使用 parse_dispatch_option() 将一个选项名转换为这些值之一。
 */
typedef enum DispatchOption
{
	DISPATCH_CHECK,
	DISPATCH_BOOT,
	DISPATCH_FORKCHILD,
	DISPATCH_DESCRIBE_CONFIG,
	DISPATCH_SINGLE,
	DISPATCH_POSTMASTER,		/* 必须放在最后 */
} DispatchOption;

extern DispatchOption parse_dispatch_option(const char *name);

#endif							/* _POSTMASTER_H */
