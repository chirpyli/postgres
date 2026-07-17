/*-------------------------------------------------------------------------
 *
 * postgres.c
 *	  POSTGRES C 后端接口
 *
 * 部分版权所有 (c) 1996-2025, PostgreSQL Global Development Group
 * 部分版权所有 (c) 1994, Regents of the University of California
 *
 *
 * 标识
 *	  src/backend/tcop/postgres.c
 *
 * 说明
 *	  这是 postgres 后端的“主”模块，
 *	  因此也是“交通警察(traffic cop)”的主模块。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>

#ifdef USE_VALGRIND
#include <valgrind/valgrind.h>
#endif

#include "access/parallel.h"
#include "access/printtup.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "commands/async.h"
#include "commands/event_trigger.h"
#include "commands/prepare.h"
#include "common/pg_prng.h"
#include "jit/jit.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/pqsignal.h"
#include "mb/pg_wchar.h"
#include "mb/stringinfo_mb.h"
#include "miscadmin.h"
#include "nodes/print.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parser.h"
#include "pg_getopt.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "postmaster/interrupt.h"
#include "postmaster/postmaster.h"
#include "replication/logicallauncher.h"
#include "replication/logicalworker.h"
#include "replication/slotsync.h"
#include "replication/slot.h"
#include "replication/walsender.h"
#include "rewrite/rewriteHandler.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/sinval.h"
#include "tcop/backend_startup.h"
#include "tcop/fastpath.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/guc_hooks.h"
#include "utils/injection_point.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/snapmgr.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"
#include "utils/varlena.h"

/* ----------------
 *		全局变量
 * ----------------
 */
const char *debug_query_string; /* 客户端提供的查询字符串 */

/* 注意：whereToSendOutput 在 bootstrap/standalone 场景下被初始化 */
CommandDest whereToSendOutput = DestDebug;

/* 标记是否在会话结束时记录日志 */
bool		Log_disconnections = false;

int			log_statement = LOGSTMT_NONE;

/* 等待 N 秒，以便调试器可附加到进程 */
int			PostAuthDelay = 0;

/* 检查客户端是否仍然连接的间隔时间。 */
int			client_connection_check_interval = 0;

/* 用于限制非系统关系类型使用的标志位 */
int			restrict_nonsystem_relation_kind;

/* ----------------
 *		私有类型定义等
 * ----------------
 */

/* bind_param_error_callback 的参数类型 */
typedef struct BindParamCbData
{
	const char *portalName;
	int			paramno;		/* 从 0 开始的参数编号，初始为 -1 */
	const char *paramval;		/* 文本形式的输入字符串（若可用） */
} BindParamCbData;

/* ----------------
 *		私有变量
 * ----------------
 */

/*
 * 用于记录我们是否已经启动了事务的标志。
 * 对于扩展查询协议，该标志需要在多条消息之间保持。
 */
static bool xact_started = false;

/*
 * 该标志表示我们正在执行外层循环的从客户端读取操作，
 * 而不是在诸如 COPY FROM STDIN 之类的命令内部发生的任意客户端读取。
 */
static bool DoingCommandRead = false;

/*
 * 用于实现扩展查询协议消息“出错后跳过直到 Sync”行为的标志。
 */
static bool doing_extended_query_message = false;
static bool ignore_till_sync = false;

/*
 * 如果存在未命名的预备语句，则存储在此处。
 * 我们将其与 commands/prepare.c 维护的哈希表分开保存，
 * 以降低短生命周期查询的开销。
 */
static CachedPlanSource *unnamed_stmt_psrc = NULL;

/* 各种命令行开关 */
static const char *userDoption = NULL;	/* -D 开关 */
static bool EchoQuery = false;	/* -E 开关 */
static bool UseSemiNewlineNewline = false;	/* -j 开关 */

/* 是否因与恢复冲突而被取消，以及取消的原因 */
static volatile sig_atomic_t RecoveryConflictPending = false;
static volatile sig_atomic_t RecoveryConflictPendingReasons[NUM_PROCSIGNALS];

/* 复用的缓冲区，用于传递给 SendRowDescriptionMessage() */
static MemoryContext row_description_context = NULL;
static StringInfoData row_description_buf;

/* ----------------------------------------------------------------
 *		仅在本文件中使用的例程声明
 * ----------------------------------------------------------------
 */
static int	InteractiveBackend(StringInfo inBuf);
static int	interactive_getc(void);
static int	SocketBackend(StringInfo inBuf);
static int	ReadCommand(StringInfo inBuf);
static void forbidden_in_wal_sender(char firstchar);
static bool check_log_statement(List *stmt_list);
static int	errdetail_execute(List *raw_parsetree_list);
static int	errdetail_params(ParamListInfo params);
static int	errdetail_abort(void);
static void bind_param_error_callback(void *arg);
static void start_xact_command(void);
static void finish_xact_command(void);
static bool IsTransactionExitStmt(Node *parsetree);
static bool IsTransactionExitStmtList(List *pstmts);
static bool IsTransactionStmtList(List *pstmts);
static void drop_unnamed_stmt(void);
static void log_disconnections(int code, Datum arg);
static void enable_statement_timeout(void);
static void disable_statement_timeout(void);


/* ----------------------------------------------------------------
 *		valgrind 调试基础设施
 * ----------------------------------------------------------------
 */
#ifdef USE_VALGRIND
/* 该变量应在主循环开始处设置。 */
static unsigned int old_valgrind_error_count;

/*
 * 如果自从 old_valgrind_error_count 被更新以来 Valgrind 检测到了任何错误，
 * 则将当前查询作为原因报告。应在消息处理结束时调用。
 */
static void
valgrind_report_error_query(const char *query)
{
	unsigned int valgrind_error_count = VALGRIND_COUNT_ERRORS;

	if (unlikely(valgrind_error_count != old_valgrind_error_count) &&
		query != NULL)
		VALGRIND_PRINTF("Valgrind detected %u error(s) during execution of \"%s\"\n",
						valgrind_error_count - old_valgrind_error_count,
						query);
}

#else							/* !USE_VALGRIND */
#define valgrind_report_error_query(query) ((void) 0)
#endif							/* USE_VALGRIND */


/* ----------------------------------------------------------------
 *		获取用户输入的例程
 * ----------------------------------------------------------------
 */

/* ----------------
 *	InteractiveBackend() 用于用户交互式连接
 *
 *	用户输入的字符串会被放入其参数 inBuf 中，
 *	我们会表现得像是收到了一条 Q 消息。
 *
 *	如果读到文件结束符（EOF），则返回 EOF，表示应当关闭。
 * ----------------
 */

static int
InteractiveBackend(StringInfo inBuf)
{
	int			c;				/* 从 getc() 读取的字符 */

	/*
	 * 显示提示符并从用户处获取输入
	 */
	printf("backend> ");
	fflush(stdout);

	resetStringInfo(inBuf);

	/*
	 * 一直读取字符，直到遇到 EOF 或相应的分隔符。
	 */
	while ((c = interactive_getc()) != EOF)
	{
		if (c == '\n')
		{
			if (UseSemiNewlineNewline)
			{
				/*
				 * 在 -j 模式下，分号后紧跟两个换行符表示命令结束；
				 * 否则将换行符视为普通字符。
				 */
				if (inBuf->len > 1 &&
					inBuf->data[inBuf->len - 1] == '\n' &&
					inBuf->data[inBuf->len - 2] == ';')
				{
					/* 不妨直接丢弃第二个换行符 */
					break;
				}
			}
			else
			{
				/*
				 * 在普通模式下，换行符表示命令结束，除非前面跟着反斜杠。
				 */
				if (inBuf->len > 0 &&
					inBuf->data[inBuf->len - 1] == '\\')
				{
					/* 从 inBuf 中丢弃反斜杠 */
					inBuf->data[--inBuf->len] = '\0';
					/* 同时丢弃换行符 */
					continue;
				}
				else
				{
					/* 保留换行符，但结束命令 */
					appendStringInfoChar(inBuf, '\n');
					break;
				}
			}
		}

		/* 不是换行符，或换行符被视为普通字符 */
		appendStringInfoChar(inBuf, (char) c);
	}

	/* EOF 之前没有任何输入，意味着应当退出。 */
	if (c == EOF && inBuf->len == 0)
		return EOF;

	/*
	 * 否则我们拿到了一条用户查询，对其进行处理。
	 */

	/* 追加 '\0'，使其与消息场景表现一致。 */
	appendStringInfoChar(inBuf, (char) '\0');

	/*
	 * 如果指定了查询回显标志，则打印该查询。
	 */
	if (EchoQuery)
		printf("statement: %s\n", inBuf->data);
	fflush(stdout);

	return PqMsg_Query;
}

/*
 * interactive_getc —— 从 stdin 读取一个字符
 *
 * 尽管我们不是从“客户端”进程读取，但仍希望响应信号，
 * 尤其是 SIGTERM/SIGQUIT。
 */
static int
interactive_getc(void)
{
	int			c;

	/*
	 * 在读取期间，这不会处理 catchup 中断或通知。但这些对于独立后端
	 * 来说其实并不相关。为了正确处理 SIGTERM，die() 中有一个技巧，
	 * 会在此阶段直接处理中断……
	 */
	CHECK_FOR_INTERRUPTS();

	c = getc(stdin);

	ProcessClientReadInterrupt(false);

	return c;
}

/* ----------------
 *	SocketBackend()		用于前端-后端连接
 *
 *	返回消息类型码，并将消息体数据载入 inBuf。
 *
 *	如果连接丢失，则返回 EOF。
 * ----------------
 */
static int
SocketBackend(StringInfo inBuf)
{
	int			qtype;
	int			maxmsglen;

	/*
	 * 从前端获取消息类型码。
	 */
	HOLD_CANCEL_INTERRUPTS();
	pq_startmsgread();
	qtype = pq_getbyte();

	if (qtype == EOF)			/* 前端断开连接 */
	{
		if (IsTransactionState())
			ereport(COMMERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("unexpected EOF on client connection with an open transaction")));
		else
		{
			/*
			 * 此时无法向客户端发送 DEBUG 日志消息。由于我们马上就要
			 * 断开连接，因此无需恢复 whereToSendOutput。
			 */
			whereToSendOutput = DestNone;
			ereport(DEBUG1,
					(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST),
					 errmsg_internal("unexpected EOF on client connection")));
		}
		return qtype;
	}

	/*
	 * 在尝试读取消息体之前先校验消息类型码；如果我们已丢失同步，
	 * 那么说“未知命令”也比因为把垃圾数据当作长度字而耗尽内存要好。
	 * 我们还可以根据类型选择一个合理的长度字上限。（上限也可以选得更
	 * 精细一些，但这么做是否值得尚不清楚。）
	 *
	 * 这也给我们提供了一个尽早设置 doing_extended_query_message 标志的地方。
	 */
	switch (qtype)
	{
		case PqMsg_Query:
			maxmsglen = PQ_LARGE_MESSAGE_LIMIT;
			doing_extended_query_message = false;
			break;

		case PqMsg_FunctionCall:
			maxmsglen = PQ_LARGE_MESSAGE_LIMIT;
			doing_extended_query_message = false;
			break;

		case PqMsg_Terminate:
			maxmsglen = PQ_SMALL_MESSAGE_LIMIT;
			doing_extended_query_message = false;
			ignore_till_sync = false;
			break;

		case PqMsg_Bind:
		case PqMsg_Parse:
			maxmsglen = PQ_LARGE_MESSAGE_LIMIT;
			doing_extended_query_message = true;
			break;

		case PqMsg_Close:
		case PqMsg_Describe:
		case PqMsg_Execute:
		case PqMsg_Flush:
			maxmsglen = PQ_SMALL_MESSAGE_LIMIT;
			doing_extended_query_message = true;
			break;

		case PqMsg_Sync:
			maxmsglen = PQ_SMALL_MESSAGE_LIMIT;
			/* 停止任何正在进行的“跳过直到 Sync” */
			ignore_till_sync = false;
			/* 标记为“非扩展”，从而避免新的错误再次开启跳过 */
			doing_extended_query_message = false;
			break;

		case PqMsg_CopyData:
			maxmsglen = PQ_LARGE_MESSAGE_LIMIT;
			doing_extended_query_message = false;
			break;

		case PqMsg_CopyDone:
		case PqMsg_CopyFail:
			maxmsglen = PQ_SMALL_MESSAGE_LIMIT;
			doing_extended_query_message = false;
			break;

		default:

			/*
			 * 否则我们从前端收到了垃圾数据。我们将此视为致命错误，
			 * 因为我们很可能已经丢失了消息边界同步，且没有好的办法恢复。
			 */
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid frontend message type %d", qtype)));
			maxmsglen = 0;		/* 避免编译器告警 */
			break;
	}

	/*
	 * 在协议版本 3 中，所有前端消息在类型码之后都紧跟一个长度字；
	 * 因此我们可以独立于类型读取消息内容。
	 */
	if (pq_getmessage(inBuf, maxmsglen))
		return EOF;				/* 相应的错误消息已经记录 */
	RESUME_CANCEL_INTERRUPTS();

	return qtype;
}

/* ----------------
 *		ReadCommand 从前端或标准输入读取一条命令，
 *		将其放入 inBuf，并返回消息类型码（消息的首字节）。
 *		如果读到文件结束符，则返回 EOF。
 * ----------------
 */
static int
ReadCommand(StringInfo inBuf)
{
	int			result;

	if (whereToSendOutput == DestRemote)
		result = SocketBackend(inBuf);
	else
		result = InteractiveBackend(inBuf);
	return result;
}

/*
 * ProcessClientReadInterrupt() —— 处理特定于客户端读取的中断
 *
 * 在底层读取之前和之后调用。
 * 若没有数据可读且我们计划重试，则 blocked 为 true；
 * 若即将读取或已完成读取，则 blocked 为 false。
 *
 * 必须保留 errno！
 */
void
ProcessClientReadInterrupt(bool blocked)
{
	int			save_errno = errno;

	if (DoingCommandRead)
	{
		/* 检查读取之前/期间到达的常规中断 */
		CHECK_FOR_INTERRUPTS();

		/* 处理 sinval catchup 中断（如果有） */
		if (catchupInterruptPending)
			ProcessCatchupInterrupt();

		/* 处理通知中断（如果有） */
		if (notifyInterruptPending)
			ProcessNotifyInterrupt(true);
	}
	else if (ProcDiePending)
	{
		/*
		 * 我们正在退出。如果没有可读的数据，那么现在处理它是安全的
		 * （也是合理的）。如果我们尚未尝试读取，请确保已设置进程
		 * latch，这样若没有数据，我们就会回到这里并退出。如果已完成
		 * 读取，同样要确保进程 latch 已设置，因为我们在读取期间可能
		 * 已不经意地清除了它。
		 */
		if (blocked)
			CHECK_FOR_INTERRUPTS();
		else
			SetLatch(MyLatch);
	}

	errno = save_errno;
}

/*
 * ProcessClientWriteInterrupt() —— 处理特定于客户端写入的中断
 *
 * 在底层写入之前和之后调用。
 * 若无法写入数据且我们计划重试，则 blocked 为 true；
 * 若即将写入或已完成写入，则 blocked 为 false。
 *
 * 必须保留 errno！
 */
void
ProcessClientWriteInterrupt(bool blocked)
{
	int			save_errno = errno;

	if (ProcDiePending)
	{
		/*
		 * 我们正在退出。如果无法写入，那么我们应该立即处理它，
		 * 否则一个卡住的客户端可能会无限期地拖延我们对信号的响应。
		 * 如果我们尚未尝试写入，请确保已设置进程 latch，这样若写入会
		 * 阻塞，我们就会回到这里并退出。如果已完成写入，同样要确保
		 * 进程 latch 已设置，因为我们在写入期间可能已不经意地清除了它。
		 */
		if (blocked)
		{
			/*
			 * 如果 ProcessInterrupts 不会处理 ProcDiePending，
			 * 就不要去改动 whereToSendOutput。
			 */
			if (InterruptHoldoffCount == 0 && CritSectionCount == 0)
			{
				/*
				 * 我们不希望向客户端发送错误消息，因为 a) 那样可能会再次
				 * 阻塞，b) 由于我们可能已经发送了部分协议消息，它很可能会
				 * 导致协议同步丢失。
				 */
				if (whereToSendOutput == DestRemote)
					whereToSendOutput = DestNone;

				CHECK_FOR_INTERRUPTS();
			}
		}
		else
			SetLatch(MyLatch);
	}

	errno = save_errno;
}

/*
 * 仅执行原始解析（raw parsing）。
 *
 * 返回一个解析树（RawStmt 节点）列表，因为给定字符串中可能存在
 * 多条命令。
 *
 * 注意：对于交互式查询，将本例程与分析 & 重写阶段分开十分重要。
 * 分析与重写无法在已中止的事务中进行，因为它们需要访问数据库表。
 * 因此，我们依赖原始解析器来判断是否已经看到 COMMIT 或 ABORT 命令；
 * 当我们处于中止状态时，其他命令不会在原始解析阶段之外被进一步处理。
 */
List *
pg_parse_query(const char *query_string)
{
	List	   *raw_parsetree_list;

	TRACE_POSTGRESQL_QUERY_PARSE_START(query_string);

	if (log_parser_stats)
		ResetUsage();

	raw_parsetree_list = raw_parser(query_string, RAW_PARSE_DEFAULT);

	if (log_parser_stats)
		ShowUsage("PARSER STATISTICS");

#ifdef DEBUG_NODE_TESTS_ENABLED

	/* 可选的调试检查：将原始解析树通过 copyObject() 传递 */
	if (Debug_copy_parse_plan_trees)
	{
		List	   *new_list = copyObject(raw_parsetree_list);

		/* 这会同时检查 copyObject() 和 equal() 例程…… */
		if (!equal(new_list, raw_parsetree_list))
			elog(WARNING, "copyObject() failed to produce an equal raw parse tree");
		else
			raw_parsetree_list = new_list;
	}

		/*
		 * 可选的调试检查：将原始解析树通过 outfuncs/readfuncs 传递
		 */
		if (Debug_write_read_parse_plan_trees)
		{
			char	   *str = nodeToStringWithLocations(raw_parsetree_list);
			List	   *new_list = stringToNodeWithLocations(str);

			pfree(str);
			/* 这会同时检查 outfuncs/readfuncs 和 equal() 例程…… */
			if (!equal(new_list, raw_parsetree_list))
				elog(WARNING, "outfuncs/readfuncs failed to produce an equal raw parse tree");
		else
			raw_parsetree_list = new_list;
	}

#endif							/* DEBUG_NODE_TESTS_ENABLED */

	TRACE_POSTGRESQL_QUERY_PARSE_DONE(query_string);

	return raw_parsetree_list;
}

/*
 * 给定一个原始解析树（gram.y 的输出），以及（可选的）关于参数符号
 * ($n) 类型的信息，执行解析分析与规则重写。
 *
 * 返回一个 Query 节点列表，因为分析器或重写器都可能将一条查询扩展为
 * 多条。
 *
 * 注意：基于上述原因，本例程必须与原始解析分开。
 */
List *
pg_analyze_and_rewrite_fixedparams(RawStmt *parsetree,
								   const char *query_string,
								   const Oid *paramTypes,
								   int numParams,
								   QueryEnvironment *queryEnv)
{
	Query	   *query;
	List	   *querytree_list;

	TRACE_POSTGRESQL_QUERY_REWRITE_START(query_string);

	/*
	 * (1) 执行解析分析。
	 */
	if (log_parser_stats)
		ResetUsage();

	query = parse_analyze_fixedparams(parsetree, query_string, paramTypes, numParams,
									  queryEnv);

	if (log_parser_stats)
		ShowUsage("PARSE ANALYSIS STATISTICS");

	/*
	 * (2) 根据需要重写查询
	 */
	querytree_list = pg_rewrite_query(query);

	TRACE_POSTGRESQL_QUERY_REWRITE_DONE(query_string);

	return querytree_list;
}

/*
 * 执行解析分析与重写。它与 pg_analyze_and_rewrite_fixedparams 相同，
 * 不同之处在于可以从上下文中推导 $n 符号的数据类型信息。
 */
List *
pg_analyze_and_rewrite_varparams(RawStmt *parsetree,
								 const char *query_string,
								 Oid **paramTypes,
								 int *numParams,
								 QueryEnvironment *queryEnv)
{
	Query	   *query;
	List	   *querytree_list;

	TRACE_POSTGRESQL_QUERY_REWRITE_START(query_string);

	/*
	 * (1) 执行解析分析。
	 */
	if (log_parser_stats)
		ResetUsage();

	query = parse_analyze_varparams(parsetree, query_string, paramTypes, numParams,
									queryEnv);

	/*
	 * 检查所有参数类型是否都已确定。
	 */
	for (int i = 0; i < *numParams; i++)
	{
		Oid			ptype = (*paramTypes)[i];

		if (ptype == InvalidOid || ptype == UNKNOWNOID)
			ereport(ERROR,
					(errcode(ERRCODE_INDETERMINATE_DATATYPE),
					 errmsg("could not determine data type of parameter $%d",
							i + 1)));
	}

	if (log_parser_stats)
		ShowUsage("PARSE ANALYSIS STATISTICS");

	/*
	 * (2) 根据需要重写查询
	 */
	querytree_list = pg_rewrite_query(query);

	TRACE_POSTGRESQL_QUERY_REWRITE_DONE(query_string);

	return querytree_list;
}

/*
 * 执行解析分析与重写。它与 pg_analyze_and_rewrite_fixedparams 相同，
 * 不同之处在于：不采用固定的参数数据类型列表，而是提供一个解析器
 * 回调，它可以进行外部参数解析，以及可能的其他操作。
 */
List *
pg_analyze_and_rewrite_withcb(RawStmt *parsetree,
							  const char *query_string,
							  ParserSetupHook parserSetup,
							  void *parserSetupArg,
							  QueryEnvironment *queryEnv)
{
	Query	   *query;
	List	   *querytree_list;

	TRACE_POSTGRESQL_QUERY_REWRITE_START(query_string);

	/*
	 * (1) 执行解析分析。
	 */
	if (log_parser_stats)
		ResetUsage();

	query = parse_analyze_withcb(parsetree, query_string, parserSetup, parserSetupArg,
								 queryEnv);

	if (log_parser_stats)
		ShowUsage("PARSE ANALYSIS STATISTICS");

	/*
	 * (2) 根据需要重写查询
	 */
	querytree_list = pg_rewrite_query(query);

	TRACE_POSTGRESQL_QUERY_REWRITE_DONE(query_string);

	return querytree_list;
}

/*
 * 对解析分析产生的查询执行重写。
 *
 * 注意：查询必须刚刚来自解析器，因为我们不会对其执行 AcquireRewriteLocks()。
 */
List *
pg_rewrite_query(Query *query)
{
	List	   *querytree_list;

	if (Debug_print_parse)
		elog_node_display(LOG, "parse tree", query,
						  Debug_pretty_print);

	if (log_parser_stats)
		ResetUsage();

	if (query->commandType == CMD_UTILITY)
	{
		/* 不重写实用命令，直接将其放入结果列表 */
		querytree_list = list_make1(query);
	}
	else
	{
		/* 重写常规查询 */
		querytree_list = QueryRewrite(query);
	}

	if (log_parser_stats)
		ShowUsage("REWRITER STATISTICS");

#ifdef DEBUG_NODE_TESTS_ENABLED

	/* 可选的调试检查：将查询树通过 copyObject() 传递 */
	if (Debug_copy_parse_plan_trees)
	{
		List	   *new_list;

		new_list = copyObject(querytree_list);
		/* 这会同时检查 copyObject() 和 equal() 例程…… */
		if (!equal(new_list, querytree_list))
			elog(WARNING, "copyObject() failed to produce an equal rewritten parse tree");
		else
			querytree_list = new_list;
	}

	/* 可选的调试检查：将查询树通过 outfuncs/readfuncs 传递 */
	if (Debug_write_read_parse_plan_trees)
	{
		List	   *new_list = NIL;
		ListCell   *lc;

		foreach(lc, querytree_list)
		{
			Query	   *curr_query = lfirst_node(Query, lc);
			char	   *str = nodeToStringWithLocations(curr_query);
			Query	   *new_query = stringToNodeWithLocations(str);

			/*
			 * queryId 不会被保存到存储的规则中，但我们必须在此处
			 * 保留它，以避免破坏 pg_stat_statements。
			 */
			new_query->queryId = curr_query->queryId;

			new_list = lappend(new_list, new_query);
			pfree(str);
		}

		/* 这会同时检查 outfuncs/readfuncs 和 equal() 例程…… */
		if (!equal(new_list, querytree_list))
			elog(WARNING, "outfuncs/readfuncs failed to produce an equal rewritten parse tree");
		else
			querytree_list = new_list;
	}

#endif							/* DEBUG_NODE_TESTS_ENABLED */

	if (Debug_print_rewritten)
		elog_node_display(LOG, "rewritten parse tree", querytree_list,
						  Debug_pretty_print);

	return querytree_list;
}


/*
 * 为单条已经过重写的查询生成执行计划。
 * 这是对 planner() 的轻量封装，接受相同的参数。
 */
PlannedStmt *
pg_plan_query(Query *querytree, const char *query_string, int cursorOptions,
			  ParamListInfo boundParams)
{
	PlannedStmt *plan;

	/* 实用命令没有执行计划。 */
	if (querytree->commandType == CMD_UTILITY)
		return NULL;

	/* 优化器可能会调用用户自定义函数，因此必须持有一个快照。 */
	Assert(ActiveSnapshotSet());

	TRACE_POSTGRESQL_QUERY_PLAN_START();

	if (log_planner_stats)
		ResetUsage();

	/* 调用优化器 */
	plan = planner(querytree, query_string, cursorOptions, boundParams);

	if (log_planner_stats)
		ShowUsage("PLANNER STATISTICS");

#ifdef DEBUG_NODE_TESTS_ENABLED

	/* 可选的调试检查：将计划树通过 copyObject() 传递 */
	if (Debug_copy_parse_plan_trees)
	{
		PlannedStmt *new_plan = copyObject(plan);

		/*
		 * equal() 目前没有用于比较 Plan 节点的例程，因此
		 * 不要在此尝试进行相等性测试。也许将来会修复？
		 */
#ifdef NOT_USED
		/* 这会同时检查 copyObject() 和 equal() 例程…… */
		if (!equal(new_plan, plan))
			elog(WARNING, "copyObject() failed to produce an equal plan tree");
		else
#endif
			plan = new_plan;
	}

	/* 可选的调试检查：将计划树通过 outfuncs/readfuncs 传递 */
	if (Debug_write_read_parse_plan_trees)
	{
		char	   *str;
		PlannedStmt *new_plan;

		str = nodeToStringWithLocations(plan);
		new_plan = stringToNodeWithLocations(str);
		pfree(str);

		/*
		 * equal() 目前没有用于比较 Plan 节点的例程，因此
		 * 不要在此尝试进行相等性测试。也许将来会修复？
		 */
#ifdef NOT_USED
		/* 这会同时检查 outfuncs/readfuncs 和 equal() 例程…… */
		if (!equal(new_plan, plan))
			elog(WARNING, "outfuncs/readfuncs failed to produce an equal plan tree");
		else
#endif
			plan = new_plan;
	}

#endif							/* DEBUG_NODE_TESTS_ENABLED */

	/*
	 * 如果需要调试，则打印执行计划。
	 */
	if (Debug_print_plan)
		elog_node_display(LOG, "plan", plan, Debug_pretty_print);

	TRACE_POSTGRESQL_QUERY_PLAN_DONE();

	return plan;
}

/*
 * 为一组已经过重写的查询生成执行计划。
 *
 * 对于普通的可优化语句，调用优化器。对于实用命令，
 * 只需构造一个包装用的 PlannedStmt 节点。
 *
 * 结果是一个 PlannedStmt 节点列表。
 */
List *
pg_plan_queries(List *querytrees, const char *query_string, int cursorOptions,
				ParamListInfo boundParams)
{
	List	   *stmt_list = NIL;
	ListCell   *query_list;

	foreach(query_list, querytrees)
	{
		Query	   *query = lfirst_node(Query, query_list);
		PlannedStmt *stmt;

		if (query->commandType == CMD_UTILITY)
		{
			/* 实用命令无需规划。 */
			stmt = makeNode(PlannedStmt);
			stmt->commandType = CMD_UTILITY;
			stmt->canSetTag = query->canSetTag;
			stmt->utilityStmt = query->utilityStmt;
			stmt->stmt_location = query->stmt_location;
			stmt->stmt_len = query->stmt_len;
			stmt->queryId = query->queryId;
		}
		else
		{
			stmt = pg_plan_query(query, query_string, cursorOptions,
								 boundParams);
		}

		stmt_list = lappend(stmt_list, stmt);
	}

	return stmt_list;
}


/*
 * exec_simple_query
 *
 * 执行一条“simple Query”协议消息。
 */
static void
exec_simple_query(const char *query_string)
{
	CommandDest dest = whereToSendOutput;
	MemoryContext oldcontext;
	List	   *parsetree_list;
	ListCell   *parsetree_item;
	bool		save_log_statement_stats = log_statement_stats;
	bool		was_logged = false;
	bool		use_implicit_block;
	char		msec_str[32];

	/*
	 * 向各类监控设施报告查询。
	 */
	debug_query_string = query_string;

	pgstat_report_activity(STATE_RUNNING, query_string);

	TRACE_POSTGRESQL_QUERY_START(query_string);

	/*
	 * 我们使用 save_log_statement_stats，这样 ShowUsage 就不会因为
	 * 没有调用 ResetUsage 而报告错误的结果。
	 */
	if (save_log_statement_stats)
		ResetUsage();

	/*
	 * 启动一个事务命令。query_string 生成的所有查询都将位于同一个
	 * 命令块中，*除非*我们遇到了 BEGIN/COMMIT/ABORT 语句；遇到这类
	 * 语句后我们必须强制开始一个新的事务命令，否则 xact.c 中会出现
	 * 问题。（注意，这通常会改变当前的内存上下文。）
	 */
	start_xact_command();

	/*
	 * 清除任何已存在的未命名语句。（虽然并非绝对必要，但最好将
	 * simple-Query 模式定义为使用未命名的语句和 portal；这样可以确保
	 * 我们回收之前未命名操作所使用的存储空间。）
	 */
	drop_unnamed_stmt();

	/*
	 * 切换到合适的上下文以构造解析树。
	 */
	oldcontext = MemoryContextSwitchTo(MessageContext);

	/*
	 * 对查询进行基本的解析（即使我们处于已中止的事务状态，这也应是安全的！）
	 */
	parsetree_list = pg_parse_query(query_string);

	/* 如果 log_statement 要求，则立即记录日志 */
	if (check_log_statement(parsetree_list))
	{
		ereport(LOG,
				(errmsg("statement: %s", query_string),
				 errhidestmt(true),
				 errdetail_execute(parsetree_list)));
		was_logged = true;
	}

	/*
	 * 切换回事务上下文以进入循环。
	 */
	MemoryContextSwitchTo(oldcontext);

	/*
	 * 出于历史原因，如果在单条“simple Query”消息中给出了多条 SQL 语句，
	 * 我们会将它们作为单个事务执行，除非其中包含显式的事务控制命令，
	 * 使列表中的某些部分成为独立的事务。为了在事务机制中恰当地表现
	 * 这种行为，我们使用一个“隐式”事务块。
	 */
	use_implicit_block = (list_length(parsetree_list) > 1);

		/*
		 * 遍历原始解析树，并逐一处理。
		 */
		foreach(parsetree_item, parsetree_list)
		{
			RawStmt    *parsetree = lfirst_node(RawStmt, parsetree_item);
		bool		snapshot_set = false;
		CommandTag	commandTag;
		QueryCompletion qc;
		MemoryContext per_parsetree_context = NULL;
		List	   *querytree_list,
				   *plantree_list;
		Portal		portal;
		DestReceiver *receiver;
		int16		format;
		const char *cmdtagname;
		size_t		cmdtaglen;

		pgstat_report_query_id(0, true);
		pgstat_report_plan_id(0, true);

		/*
		 * 获取用于状态显示的命令名（它也会成为 PortalRun 内部的
		 * 默认完成标签）。设置 ps_status，并完成目标端需要的任何
		 * SQL 命令起始阶段的特殊处理。
		 */
		commandTag = CreateCommandTag(parsetree->stmt);
		cmdtagname = GetCommandTagNameAndLen(commandTag, &cmdtaglen);

		set_ps_display_with_len(cmdtagname, cmdtaglen);

		BeginCommand(commandTag, dest);

		/*
		 * 如果我们处于已中止的事务中，则拒绝除 COMMIT/ABORT 之外的所有
		 * 命令。这个检测必须在我们尝试进行解析分析、重写或规划之前进行，
		 * 因为所有这些阶段都会尝试访问数据库，而在中止状态下访问可能失败。
		 * （在这种状态下，允许一些额外的实用命令也许是安全的，但数量不多……）
		 */
		if (IsAbortedTransactionBlockState() &&
			!IsTransactionExitStmt(parsetree->stmt))
			ereport(ERROR,
					(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
					 errmsg("current transaction is aborted, "
							"commands ignored until end of transaction block"),
					 errdetail_abort()));

		/* 确保我们处于事务命令中 */
		start_xact_command();

		/*
		 * 如果使用了隐式事务块，并且我们尚不处于某个事务块中，则启动一个
		 * 隐式块，以强制本语句与后续语句归为一组。（我们必须在循环的每一
		 * 轮都这样做；否则，列表中的 COMMIT/ROLLBACK 会导致后面的语句
		 * 无法被归组。）
		 */
		if (use_implicit_block)
			BeginImplicitTransactionBlock();

		/* 如果在解析或前一条命令中收到取消信号，则退出 */
		CHECK_FOR_INTERRUPTS();

		/*
		 * 如果解析分析/规划需要快照，则建立一个快照。
		 */
		if (analyze_requires_snapshot(parsetree))
		{
			PushActiveSnapshot(GetTransactionSnapshot());
			snapshot_set = true;
		}

		/*
		 * 现在可以分析、重写并规划本查询了。
		 *
		 * 切换到合适的上下文以构造查询树和计划树（它们不能位于事务上下文中，
		 * 因为当事务执行 COMMIT/ROLLBACK 时该上下文会被重置）。如果我们有
		 * 多个解析树，则为每个解析树使用一个独立的上下文，以便在处理下一个
		 * 之前释放这块内存。而对于最后（或唯一）一个解析树，直接使用
		 * MessageContext 即可，它会在完成后很快被重置。如果出错，
		 * per_parsetree_context 会在 MessageContext 被重置时被删除。
		 */
		if (lnext(parsetree_list, parsetree_item) != NULL)
		{
			per_parsetree_context =
				AllocSetContextCreate(MessageContext,
									  "per-parsetree message context",
									  ALLOCSET_DEFAULT_SIZES);
			oldcontext = MemoryContextSwitchTo(per_parsetree_context);
		}
		else
			oldcontext = MemoryContextSwitchTo(MessageContext);

		querytree_list = pg_analyze_and_rewrite_fixedparams(parsetree, query_string,
															NULL, 0, NULL);

		plantree_list = pg_plan_queries(querytree_list, query_string,
										CURSOR_OPT_PARALLEL_OK, NULL);

		/*
		 * 解析/规划所用的快照已用完。
		 *
		 * 虽然复用同一个快照来执行查询（至少对于简单协议）看似可行，
		 * 但不幸的是，这会导致执行时使用的快照是在锁定查询中提到的任何
		 * 表之前获取的。这会产生用户可见的异常，因此不要这样做。详情
		 * 请参阅 https://postgr.es/m/flat/5075D8DF.6050500@fuzzy.cz。
		 */
		if (snapshot_set)
			PopActiveSnapshot();

		/* 如果在分析或规划期间收到了取消信号，则退出 */
		CHECK_FOR_INTERRUPTS();

		/*
		 * 创建未命名的 portal 来运行查询（可能多条）。如果已经存在，
		 * 则静默地将其丢弃。
		 */
		portal = CreatePortal("", true, true);
		/* 不要在 pg_cursors 中显示该 portal */
		portal->visible = false;

		/*
		 * 我们无需向 portal 中拷贝任何内容，因为这里传递的一切都位于
		 * MessageContext 或 per_parsetree_context 中，因此无论如何都会
		 * 比 portal 存活得更久。
		 */
		PortalDefineQuery(portal,
						  NULL,
						  query_string,
						  commandTag,
						  plantree_list,
						  NULL);

		/*
		 * 启动 portal。这里没有参数。
		 */
		PortalStart(portal, NULL, 0, InvalidSnapshot);

		/*
		 * 选择合适的输出格式：除非是从二进制游标执行 FETCH，否则使用
		 * 文本格式。（必须在这里做这件事实在有点别扭——但它避免了在
		 * 其他地方出现更别扭的情况。啊，向后兼容的乐趣……）
		 */
		format = 0;				/* 默认为 TEXT */
		if (IsA(parsetree->stmt, FetchStmt))
		{
			FetchStmt  *stmt = (FetchStmt *) parsetree->stmt;

			if (!stmt->ismove)
			{
				Portal		fportal = GetPortalByName(stmt->portalname);

				if (PortalIsValid(fportal) &&
					(fportal->cursorOptions & CURSOR_OPT_BINARY))
					format = 1; /* 二进制 */
			}
		}
		PortalSetResultFormat(portal, 1, &format);

		/*
		 * 现在我们可以创建目标端接收器对象。
		 */
		receiver = CreateDestReceiver(dest);
		if (dest == DestRemote)
			SetRemoteDestReceiverParams(receiver, portal);

		/*
		 * 切换回事务上下文以执行。
		 */
		MemoryContextSwitchTo(oldcontext);

		/*
		 * 运行 portal 直至完成，然后将其丢弃（连同接收器一起）。
		 */
		(void) PortalRun(portal,
						 FETCH_ALL,
						 true,	/* 始终为顶层 */
						 receiver,
						 receiver,
						 &qc);

		receiver->rDestroy(receiver);

		PortalDrop(portal, false);

		if (lnext(parsetree_list, parsetree_item) == NULL)
		{
			/*
			 * 如果这是查询字符串的最后一个解析树，则在报告命令完成之前
			 * 关闭事务语句。这样做是为了让任何事务结束时的错误在命令完成
			 * 消息发出之前就被报告，从而避免让客户端困惑——客户端期望的
			 * 是命令完成消息或错误，而不是两者先后都收到。此外，如果我们
			 * 正在使用隐式事务块，必须先将其关闭。
			 */
			if (use_implicit_block)
				EndImplicitTransactionBlock();
			finish_xact_command();
		}
		else if (IsA(parsetree->stmt, TransactionStmt))
		{
			/*
			 * 如果这是一条事务控制语句，则提交它。我们将为下一条命令
			 * 启动一个新的事务命令。
			 */
			finish_xact_command();
		}
		else
		{
			/*
			 * 如果我们没有调用 finish_xact_command()，那么最好不要看到
			 * XACT_FLAGS_NEEDIMMEDIATECOMMIT 被设置。（隐式事务块本应
			 * 阻止它被设置。）
			 */
			Assert(!(MyXactFlags & XACT_FLAGS_NEEDIMMEDIATECOMMIT));

			/*
			 * 每条查询之后都需要一次 CommandCounterIncrement，除了那些
			 * 开启或结束事务块的查询。
			 */
			CommandCounterIncrement();

			/*
			 * 在多查询字符串的查询之间禁用语句超时，这样超时会对每条
			 * 查询分别生效。（我们下一轮循环迭代会启动一个新的超时。）
			 */
			disable_statement_timeout();
		}

		/*
		 * 通知客户端本查询已处理完毕。注意，我们对每个原始解析树恰好
		 * 发送一份 EndCommand 报告，因此客户端发送的每条 SQL 命令都会
		 * 收到一份（无论是否经过重写）。（但因错误而中止的命令则根本
		 * 不会发送 EndCommand 报告。）
		 */
		EndCommand(&qc, dest, false);

		/* 现在可以丢弃 per-parsetree 上下文（如果创建过的话）。 */
		if (per_parsetree_context)
			MemoryContextDelete(per_parsetree_context);
	}							/* 结束遍历解析树的循环 */

	/*
	 * 关闭事务语句（如果有一个处于打开状态）。（这只在解析树列表为空时
	 * 才会起作用；否则最后一轮循环迭代已经做过了。）
	 */
	finish_xact_command();

	/*
	 * 如果没有任何解析树，则返回 EmptyQueryResponse 消息。
	 */
	if (!parsetree_list)
		NullCommand(dest);

	/*
	 * 如果合适，则记录耗时日志。
	 */
	switch (check_log_duration(msec_str, was_logged))
	{
		case 1:
			ereport(LOG,
					(errmsg("duration: %s ms", msec_str),
					 errhidestmt(true)));
			break;
		case 2:
			ereport(LOG,
					(errmsg("duration: %s ms  statement: %s",
							msec_str, query_string),
					 errhidestmt(true),
					 errdetail_execute(parsetree_list)));
			break;
	}

	if (save_log_statement_stats)
		ShowUsage("QUERY STATISTICS");

	TRACE_POSTGRESQL_QUERY_DONE(query_string);

	debug_query_string = NULL;
}

/*
 * exec_parse_message
 *
 * 执行一条“Parse”协议消息。
 */
static void
exec_parse_message(const char *query_string,	/* 要执行的字符串 */
				   const char *stmt_name,	/* 预备语句的名称 */
				   Oid *paramTypes, /* 参数类型 */
				   int numParams)	/* 参数个数 */
{
	MemoryContext unnamed_stmt_context = NULL;
	MemoryContext oldcontext;
	List	   *parsetree_list;
	RawStmt    *raw_parse_tree;
	List	   *querytree_list;
	CachedPlanSource *psrc;
	bool		is_named;
	bool		save_log_statement_stats = log_statement_stats;
	char		msec_str[32];

	/*
	 * 向各类监控设施报告查询。
	 */
	debug_query_string = query_string;

	pgstat_report_activity(STATE_RUNNING, query_string);

	set_ps_display("PARSE");

	if (save_log_statement_stats)
		ResetUsage();

	ereport(DEBUG2,
			(errmsg_internal("parse %s: %s",
							 *stmt_name ? stmt_name : "<unnamed>",
							 query_string)));

	/*
	 * 启动一个事务命令，以便运行解析分析等。（注意，这通常会改变
	 * 当前内存上下文。）如果已经处于事务命令中，则什么也不会发生。
	 * 如有必要，这也会启动语句超时计时。
	 */
	start_xact_command();

	/*
	 * 切换到合适的上下文以构造解析树。
	 *
	 * 根据预备语句是否命名，我们有两种策略。对于命名的预备语句，
	 * 我们在 MessageContext 中进行解析，并将完成的解析树拷贝到预备
	 * 语句的 plancache 条目中；随后 MessageContext 的重置会释放解析
	 * 和重写使用的临时空间。对于未命名的预备语句，我们假设该语句
	 * 不会长期存在，因此尽快释放临时空间可能不值得付出拷贝解析树的
	 * 代价。所以在这种情况，我们在此处创建 plancache 条目的
	 * query_context，并在其中完成所有解析工作。
	 */
	is_named = (stmt_name[0] != '\0');
	if (is_named)
	{
		/* 命名的预备语句 —— 在 MessageContext 中解析 */
		oldcontext = MemoryContextSwitchTo(MessageContext);
	}
	else
	{
		/* 未命名的预备语句 —— 释放任何先前的未命名语句 */
		drop_unnamed_stmt();
		/* 创建用于解析的上下文 */
		unnamed_stmt_context =
			AllocSetContextCreate(MessageContext,
								  "unnamed prepared statement",
								  ALLOCSET_DEFAULT_SIZES);
		oldcontext = MemoryContextSwitchTo(unnamed_stmt_context);
	}

	/*
	 * 对查询进行基本的解析（即使我们处于已中止的事务状态，这也应是安全的！）
	 */
	parsetree_list = pg_parse_query(query_string);

	/*
	 * 我们只允许预备语句中包含单条用户语句。这主要是为了保持协议简单——
	 * 否则我们就需要操心多个结果元组描述符之类的事情了。
	 */
	if (list_length(parsetree_list) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot insert multiple commands into a prepared statement")));

	if (parsetree_list != NIL)
	{
		bool		snapshot_set = false;

		raw_parse_tree = linitial_node(RawStmt, parsetree_list);

		/*
		 * 如果我们处于已中止的事务中，则拒绝除 COMMIT/ROLLBACK 之外的所有
		 * 命令。这个检测必须在我们尝试进行解析分析、重写或规划之前进行，
		 * 因为所有这些阶段都会尝试访问数据库，而在中止状态下访问可能失败。
		 * （在这种状态下，允许一些额外的实用命令也许是安全的，但数量不多……）
		 */
		if (IsAbortedTransactionBlockState() &&
			!IsTransactionExitStmt(raw_parse_tree->stmt))
			ereport(ERROR,
					(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
					 errmsg("current transaction is aborted, "
							"commands ignored until end of transaction block"),
					 errdetail_abort()));

		/*
		 * 在进行解析分析之前创建 CachedPlanSource，因为它需要看到
		 * 未经修改的原始解析树。
		 */
		psrc = CreateCachedPlan(raw_parse_tree, query_string,
								CreateCommandTag(raw_parse_tree->stmt));

		/*
		 * 如果解析分析需要快照，则建立一个快照。
		 */
		if (analyze_requires_snapshot(raw_parse_tree))
		{
			PushActiveSnapshot(GetTransactionSnapshot());
			snapshot_set = true;
		}

		/*
		 * 分析和重写查询。注意，最初指定的参数集合不需要是完整的，
		 * 因此我们必须使用 pg_analyze_and_rewrite_varparams()。
		 */
		querytree_list = pg_analyze_and_rewrite_varparams(raw_parse_tree,
														  query_string,
														  &paramTypes,
														  &numParams,
														  NULL);

		/* 解析所用的快照已用完 */
		if (snapshot_set)
			PopActiveSnapshot();
	}
	else
	{
		/* 空输入字符串。这是合法的。 */
		raw_parse_tree = NULL;
		psrc = CreateCachedPlan(raw_parse_tree, query_string,
								CMDTAG_UNKNOWN);
		querytree_list = NIL;
	}

	/*
	 * CachedPlanSource 必须先是 MessageContext 的直接子节点，然后我们
	 * 才能将 unnamed_stmt_context 重新挂接到它下面，否则我们会得到一个
	 * 断开的环形子图。这有点 hack，但比在上面更多地切换上下文要好一些。
	 */
	if (unnamed_stmt_context)
		MemoryContextSetParent(psrc->context, MessageContext);

	/* 完成 CachedPlanSource 的填充 */
	CompleteCachedPlan(psrc,
					   querytree_list,
					   unnamed_stmt_context,
					   paramTypes,
					   numParams,
					   NULL,
					   NULL,
					   CURSOR_OPT_PARALLEL_OK,	/* 允许并行模式 */
					   true);	/* 固定结果 */

	/* 如果在分析期间收到取消信号，则退出 */
	CHECK_FOR_INTERRUPTS();

	if (is_named)
	{
		/*
		 * 将查询作为预备语句存储。
		 */
		StorePreparedStatement(stmt_name, psrc, false);
	}
	else
	{
		/*
		 * 我们只是将 CachedPlanSource 保存到 unnamed_stmt_psrc。
		 */
		SaveCachedPlan(psrc);
		unnamed_stmt_psrc = psrc;
	}

	MemoryContextSwitchTo(oldcontext);

	/*
	 * 我们不会在此处关闭已打开的事务命令；那只在客户端发送 Sync 时
	 * 才会发生。取而代之的是执行 CommandCounterIncrement，以防在
	 * 解析/规划期间发生了什么。
	 */
	CommandCounterIncrement();

	/*
	 * 发送 ParseComplete。
	 */
	if (whereToSendOutput == DestRemote)
		pq_putemptymessage(PqMsg_ParseComplete);

	/*
	 * 如果合适，则记录耗时日志。
	 */
	switch (check_log_duration(msec_str, false))
	{
		case 1:
			ereport(LOG,
					(errmsg("duration: %s ms", msec_str),
					 errhidestmt(true)));
			break;
		case 2:
			ereport(LOG,
					(errmsg("duration: %s ms  parse %s: %s",
							msec_str,
							*stmt_name ? stmt_name : "<unnamed>",
							query_string),
					 errhidestmt(true)));
			break;
	}

	if (save_log_statement_stats)
		ShowUsage("PARSE MESSAGE STATISTICS");

	debug_query_string = NULL;
}

/*
 * exec_bind_message
 *
 * 处理一条“Bind”消息，从预备语句创建一个 portal
 */
static void
exec_bind_message(StringInfo input_message)
{
	const char *portal_name;
	const char *stmt_name;
	int			numPFormats;
	int16	   *pformats = NULL;
	int			numParams;
	int			numRFormats;
	int16	   *rformats = NULL;
	CachedPlanSource *psrc;
	CachedPlan *cplan;
	Portal		portal;
	char	   *query_string;
	char	   *saved_stmt_name;
	ParamListInfo params;
	MemoryContext oldContext;
	bool		save_log_statement_stats = log_statement_stats;
	bool		snapshot_set = false;
	char		msec_str[32];
	ParamsErrorCbData params_data;
	ErrorContextCallback params_errcxt;
	ListCell   *lc;

	/* 获取消息的固定部分 */
	portal_name = pq_getmsgstring(input_message);
	stmt_name = pq_getmsgstring(input_message);

	ereport(DEBUG2,
			(errmsg_internal("bind %s to %s",
							 *portal_name ? portal_name : "<unnamed>",
							 *stmt_name ? stmt_name : "<unnamed>")));

	/* 查找预备语句 */
	if (stmt_name[0] != '\0')
	{
		PreparedStatement *pstmt;

		pstmt = FetchPreparedStatement(stmt_name, true);
		psrc = pstmt->plansource;
	}
	else
	{
		/* 对未命名语句做特殊处理 */
		psrc = unnamed_stmt_psrc;
		if (!psrc)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_PSTATEMENT),
					 errmsg("unnamed prepared statement does not exist")));
	}

	/*
	 * 向各类监控设施报告查询。
	 */
	debug_query_string = psrc->query_string;

	pgstat_report_activity(STATE_RUNNING, psrc->query_string);

	foreach(lc, psrc->query_list)
	{
		Query	   *query = lfirst_node(Query, lc);

		if (query->queryId != INT64CONST(0))
		{
			pgstat_report_query_id(query->queryId, false);
			break;
		}
	}

	set_ps_display("BIND");

	if (save_log_statement_stats)
		ResetUsage();

	/*
	 * 启动一个事务命令，以便调用函数等。（注意，这通常会改变当前
	 * 内存上下文。）如果已经处于事务命令中，则什么也不会发生。
	 * 如有必要，这也会启动语句超时计时。
	 */
	start_xact_command();

	/* 切换回消息上下文 */
	MemoryContextSwitchTo(MessageContext);

	/* 获取参数格式码 */
	numPFormats = pq_getmsgint(input_message, 2);
	if (numPFormats > 0)
	{
		pformats = palloc_array(int16, numPFormats);
		for (int i = 0; i < numPFormats; i++)
			pformats[i] = pq_getmsgint(input_message, 2);
	}

	/* 获取参数值的个数 */
	numParams = pq_getmsgint(input_message, 2);

	if (numPFormats > 1 && numPFormats != numParams)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("bind message has %d parameter formats but %d parameters",
						numPFormats, numParams)));

	if (numParams != psrc->num_params)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("bind message supplies %d parameters, but prepared statement \"%s\" requires %d",
						numParams, stmt_name, psrc->num_params)));

	/*
	 * 如果我们处于已中止的事务状态，我们实际能运行的 portal 只有那些
	 * 包含 COMMIT 或 ROLLBACK 命令的。我们禁止绑定其他任何内容，以避免
	 * 与期望在有效事务内运行的基础设施发生冲突。我们也禁止绑定任何
	 * 参数，因为我们不能冒险调用用户自定义的 I/O 函数。
	 */
	if (IsAbortedTransactionBlockState() &&
		(!(psrc->raw_parse_tree &&
		   IsTransactionExitStmt(psrc->raw_parse_tree->stmt)) ||
		 numParams != 0))
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, "
						"commands ignored until end of transaction block"),
				 errdetail_abort()));

	/*
	 * 创建 portal。只有在指定了未命名 portal 时，才允许静默替换
	 * 一个已存在的 portal。
	 */
	if (portal_name[0] == '\0')
		portal = CreatePortal(portal_name, true, true);
	else
		portal = CreatePortal(portal_name, false, false);

	/*
	 * 准备将内容拷贝到 portal 的内存上下文中。我们把所有这些拷贝
	 * 放在最前面做，因为它有可能失败（内存不足），而我们不希望失败
	 * 发生在 GetCachedPlan 与 PortalDefineQuery 之间；那样会导致我们的
	 * plancache 引用计数泄漏。
	 */
	oldContext = MemoryContextSwitchTo(portal->portalContext);

	/* 将计划的查询字符串拷贝到 portal 中 */
	query_string = pstrdup(psrc->query_string);

	/* 同样拷贝语句名（除非是未命名的） */
	if (stmt_name[0])
		saved_stmt_name = pstrdup(stmt_name);
	else
		saved_stmt_name = NULL;

	/*
	 * 如果我们有参数需要获取（因为输入函数可能需要它），或者查询不是
	 * 实用命令（因此可能需要重新进行解析分析和规划），则建立一个快照。
	 * 我们让该快照一直保持有效，直到完成，这样 plancache.c 就不必
	 * 再获取新的快照。
	 */
	if (numParams > 0 ||
		(psrc->raw_parse_tree &&
		 analyze_requires_snapshot(psrc->raw_parse_tree)))
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		snapshot_set = true;
	}

	/*
	 * 获取参数（如果有），并存入 portal 的内存上下文。
	 */
	if (numParams > 0)
	{
		char	  **knownTextValues = NULL; /* 首次使用时再分配 */
		BindParamCbData one_param_data;

		/*
		 * 设置一个错误回调，这样如果本阶段发生错误，我们可以报告导致
		 * 问题的具体参数。
		 */
		one_param_data.portalName = portal->name;
		one_param_data.paramno = -1;
		one_param_data.paramval = NULL;
		params_errcxt.previous = error_context_stack;
		params_errcxt.callback = bind_param_error_callback;
		params_errcxt.arg = &one_param_data;
		error_context_stack = &params_errcxt;

		params = makeParamList(numParams);

		for (int paramno = 0; paramno < numParams; paramno++)
		{
			Oid			ptype = psrc->param_types[paramno];
			int32		plength;
			Datum		pval;
			bool		isNull;
			StringInfoData pbuf;
			char		csave;
			int16		pformat;

			one_param_data.paramno = paramno;
			one_param_data.paramval = NULL;

			plength = pq_getmsgint(input_message, 4);
			isNull = (plength == -1);

			if (!isNull)
			{
				char	   *pvalue;

				/*
				 * 我们不是到处拷贝数据，而是直接初始化一个 StringInfo，
				 * 令其指向消息缓冲区的正确部分。我们假设可以在消息缓冲区
				 * 上随意改写，以追加输入函数调用所需的结尾 NUL。
				 */
				pvalue = unconstify(char *, pq_getmsgbytes(input_message, plength));
				csave = pvalue[plength];
				pvalue[plength] = '\0';
				initReadOnlyStringInfo(&pbuf, pvalue, plength);
			}
			else
			{
				pbuf.data = NULL;	/* 避免未使用变量引起的编译器警告 */
				csave = 0;
			}

			if (numPFormats > 1)
				pformat = pformats[paramno];
			else if (numPFormats > 0)
				pformat = pformats[0];
			else
				pformat = 0;	/* 默认 = 文本 */

			if (pformat == 0)	/* 文本模式 */
			{
				Oid			typinput;
				Oid			typioparam;
				char	   *pstring;

				getTypeInputInfo(ptype, &typinput, &typioparam);

				/*
				 * 在调用 typinput 例程之前，我们必须进行编码转换。
				 */
				if (isNull)
					pstring = NULL;
				else
					pstring = pg_client_to_server(pbuf.data, plength);

				/* 现在可以记录输入字符串，以便出错时查看 */
				one_param_data.paramval = pstring;

				pval = OidInputFunctionCall(typinput, pstring, typioparam, -1);

				one_param_data.paramval = NULL;

				/*
				 * 如果之后可能需要记录参数，则在 MessageContext 中保存
				 * 一份转换后字符串的拷贝；然后释放编码转换的结果（如果
				 * 做过转换的话）。
				 */
				if (pstring)
				{
					if (log_parameter_max_length_on_error != 0)
					{
						MemoryContext oldcxt;

						oldcxt = MemoryContextSwitchTo(MessageContext);

						if (knownTextValues == NULL)
							knownTextValues = palloc0_array(char *, numParams);

						if (log_parameter_max_length_on_error < 0)
							knownTextValues[paramno] = pstrdup(pstring);
						else
						{
							/*
							 * 我们可以截断已保存的字符串，因为知道不会把
							 * 它全部打印出来。但我们必须多拷贝至少两个完整
							 * 字符，超出 BuildParamLogString 想要使用的长度；
							 * 否则它可能无法包含结尾的省略号。
							 */
							knownTextValues[paramno] =
								pnstrdup(pstring,
										 log_parameter_max_length_on_error
										 + 2 * MAX_MULTIBYTE_CHAR_LEN);
						}

						MemoryContextSwitchTo(oldcxt);
					}
					if (pstring != pbuf.data)
						pfree(pstring);
				}
			}
			else if (pformat == 1)	/* 二进制模式 */
			{
				Oid			typreceive;
				Oid			typioparam;
				StringInfo	bufptr;

				/*
				 * 调用参数类型的二进制输入转换器
				 */
				getTypeBinaryInputInfo(ptype, &typreceive, &typioparam);

				if (isNull)
					bufptr = NULL;
				else
					bufptr = &pbuf;

				pval = OidReceiveFunctionCall(typreceive, bufptr, typioparam, -1);

				/* 如果它没有消费整个缓冲区，则出问题了 */
				if (!isNull && pbuf.cursor != pbuf.len)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
							 errmsg("incorrect binary data format in bind parameter %d",
									paramno + 1)));
			}
			else
			{
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("unsupported format code: %d",
								pformat)));
				pval = 0;		/* 避免编译器告警 */
			}

		/* 恢复消息缓冲区内容 */
		if (!isNull)
			pbuf.data[plength] = csave;

			params->params[paramno].value = pval;
			params->params[paramno].isnull = isNull;

			/*
			 * 我们将参数标记为 CONST。这确保了任何自定义计划都能
			 * 充分利用参数值。
			 */
			params->params[paramno].pflags = PARAM_FLAG_CONST;
			params->params[paramno].ptype = ptype;
		}

		/* 弹出每个参数的错误回调 */
		error_context_stack = error_context_stack->previous;

		/*
		 * 一旦所有参数都已收到，如果配置如此，就为将来出错时打印它们
		 * 做好准备。（这保存在 portal 中，因此它们会在稍后查询执行时
		 * 出现。）
		 */
		if (log_parameter_max_length_on_error != 0)
			params->paramValuesStr =
				BuildParamLogString(params,
									knownTextValues,
									log_parameter_max_length_on_error);
	}
	else
		params = NULL;

	/* 完成在 portal 上下文中存储内容 */
	MemoryContextSwitchTo(oldContext);

	/*
	 * 设置另一个错误回调，这样如果在 BIND 处理的其余部分出错，
	 * 所有参数都会被记录。
	 */
	params_data.portalName = portal->name;
	params_data.params = params;
	params_errcxt.previous = error_context_stack;
	params_errcxt.callback = ParamsErrorCallback;
	params_errcxt.arg = &params_data;
	error_context_stack = &params_errcxt;

	/* 获取结果格式码 */
	numRFormats = pq_getmsgint(input_message, 2);
	if (numRFormats > 0)
	{
		rformats = palloc_array(int16, numRFormats);
		for (int i = 0; i < numRFormats; i++)
			rformats[i] = pq_getmsgint(input_message, 2);
	}

	pq_getmsgend(input_message);

	/*
	 * 从 CachedPlanSource 获取一个计划。(重新)规划产生的任何垃圾都会
	 * 在 MessageContext 中生成。计划的引用计数会被赋给 Portal，因此它
	 * 会在 portal 销毁时被释放。
	 */
	cplan = GetCachedPlan(psrc, params, NULL, NULL);

	/*
	 * 现在我们可以定义 portal 了。
	 *
	 * 不要在上面的 GetCachedPlan 调用与此处之间放置任何可能抛出错误的
	 * 代码。
	 */
	PortalDefineQuery(portal,
					  saved_stmt_name,
					  query_string,
					  psrc->commandTag,
					  cplan->stmt_list,
					  cplan);

	/* portal 已定义，根据其内容设置计划 ID。 */
	foreach(lc, portal->stmts)
	{
		PlannedStmt *plan = lfirst_node(PlannedStmt, lc);

		if (plan->planId != INT64CONST(0))
		{
			pgstat_report_plan_id(plan->planId, false);
			break;
		}
	}

	/* 参数 I/O 及解析/规划所用的快照已用完 */
	if (snapshot_set)
		PopActiveSnapshot();

	/*
	 * 现在我们可以启动 portal 执行了。
	 */
	PortalStart(portal, params, 0, InvalidSnapshot);

	/*
	 * 将结果格式请求应用到 portal。
	 */
	PortalSetResultFormat(portal, numRFormats, rformats);

	/*
	 * 绑定完成；移除参数错误回调。之后发出的条目会自行决定是否
	 * 记录参数。
	 */
	error_context_stack = error_context_stack->previous;

	/*
	 * 发送 BindComplete。
	 */
	if (whereToSendOutput == DestRemote)
		pq_putemptymessage(PqMsg_BindComplete);

	/*
	 * 如果合适，则记录耗时日志。
	 */
	switch (check_log_duration(msec_str, false))
	{
		case 1:
			ereport(LOG,
					(errmsg("duration: %s ms", msec_str),
					 errhidestmt(true)));
			break;
		case 2:
			ereport(LOG,
					(errmsg("duration: %s ms  bind %s%s%s: %s",
							msec_str,
							*stmt_name ? stmt_name : "<unnamed>",
							*portal_name ? "/" : "",
							*portal_name ? portal_name : "",
							psrc->query_string),
					 errhidestmt(true),
					 errdetail_params(params)));
			break;
	}

	if (save_log_statement_stats)
		ShowUsage("BIND MESSAGE STATISTICS");

	valgrind_report_error_query(debug_query_string);

	debug_query_string = NULL;
}

/*
 * exec_execute_message
 *
 * 处理一个 portal 的“Execute”消息
 */
static void
exec_execute_message(const char *portal_name, long max_rows)
{
	CommandDest dest;
	DestReceiver *receiver;
	Portal		portal;
	bool		completed;
	QueryCompletion qc;
	const char *sourceText;
	const char *prepStmtName;
	ParamListInfo portalParams;
	bool		save_log_statement_stats = log_statement_stats;
	bool		is_xact_command;
	bool		execute_is_fetch;
	bool		was_logged = false;
	char		msec_str[32];
	ParamsErrorCbData params_data;
	ErrorContextCallback params_errcxt;
	const char *cmdtagname;
	size_t		cmdtaglen;
	ListCell   *lc;

	/* 调整目标端，以告知 printtup.c 该做什么 */
	dest = whereToSendOutput;
	if (dest == DestRemote)
		dest = DestRemoteExecute;

	portal = GetPortalByName(portal_name);
	if (!PortalIsValid(portal))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("portal \"%s\" does not exist", portal_name)));

	/*
	 * 如果原始查询是空字符串，则直接返回 EmptyQueryResponse。
	 */
	if (portal->commandTag == CMDTAG_UNKNOWN)
	{
		Assert(portal->stmts == NIL);
		NullCommand(dest);
		return;
	}

	/* portal 是否包含事务命令？ */
	is_xact_command = IsTransactionStmtList(portal->stmts);

	/*
	 * 我们必须将 sourceText 和 prepStmtName 拷贝到 MessageContext 中，
	 * 以防 portal 在 finish_xact_command 期间被销毁。不过我们不会
	 * 拷贝 portalParams，而是倾向于在那种情况下干脆不打印它们。
	 */
	sourceText = pstrdup(portal->sourceText);
	if (portal->prepStmtName)
		prepStmtName = pstrdup(portal->prepStmtName);
	else
		prepStmtName = "<unnamed>";
	portalParams = portal->portalParams;

	/*
	 * 向各类监控设施报告查询。
	 */
	debug_query_string = sourceText;

	pgstat_report_activity(STATE_RUNNING, sourceText);

	foreach(lc, portal->stmts)
	{
		PlannedStmt *stmt = lfirst_node(PlannedStmt, lc);

		if (stmt->queryId != INT64CONST(0))
		{
			pgstat_report_query_id(stmt->queryId, false);
			break;
		}
	}

	foreach(lc, portal->stmts)
	{
		PlannedStmt *stmt = lfirst_node(PlannedStmt, lc);

		if (stmt->planId != INT64CONST(0))
		{
			pgstat_report_plan_id(stmt->planId, false);
			break;
		}
	}

	cmdtagname = GetCommandTagNameAndLen(portal->commandTag, &cmdtaglen);

	set_ps_display_with_len(cmdtagname, cmdtaglen);

	if (save_log_statement_stats)
		ResetUsage();

	BeginCommand(portal->commandTag, dest);

	/*
	 * 在 MessageContext 中创建目标端接收器（我们不希望它在事务上下文中，
	 * 因为如果 portal 包含 VACUUM，事务上下文可能会被删除）。
	 */
	receiver = CreateDestReceiver(dest);
	if (dest == DestRemoteExecute)
		SetRemoteDestReceiverParams(receiver, portal);

	/*
	 * 确保我们处于事务命令中（由于之前的 BIND，通常已经如此）。
	 */
	start_xact_command();

	/*
	 * 如果我们针对一个已存在的 portal 重新发出 Execute 协议请求，那么
	 * 我们只是在获取更多行，而不是从头完全重新执行查询。对于 v3 portal，
	 * atStart 永远不会被重置，因此我们使用这个检查是安全的。
	 */
	execute_is_fetch = !portal->atStart;

	/* 如果 log_statement 要求，则立即记录日志 */
	if (check_log_statement(portal->stmts))
	{
		ereport(LOG,
				(errmsg("%s %s%s%s: %s",
						execute_is_fetch ?
						_("execute fetch from") :
						_("execute"),
						prepStmtName,
						*portal_name ? "/" : "",
						*portal_name ? portal_name : "",
						sourceText),
				 errhidestmt(true),
				 errdetail_params(portalParams)));
		was_logged = true;
	}

	/*
	 * 如果我们处于已中止的事务状态，我们实际能运行的 portal 只有那些
	 * 包含 COMMIT 或 ROLLBACK 命令的。
	 */
	if (IsAbortedTransactionBlockState() &&
		!IsTransactionExitStmtList(portal->stmts))
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, "
						"commands ignored until end of transaction block"),
				 errdetail_abort()));

	/* 在开始执行前检查取消信号 */
	CHECK_FOR_INTERRUPTS();

	/*
	 * 现在可以运行 portal 了。设置错误回调以便记录参数。参数必须
	 * 已经在绑定阶段被保存。
	 */
	params_data.portalName = portal->name;
	params_data.params = portalParams;
	params_errcxt.previous = error_context_stack;
	params_errcxt.callback = ParamsErrorCallback;
	params_errcxt.arg = &params_data;
	error_context_stack = &params_errcxt;

	if (max_rows <= 0)
		max_rows = FETCH_ALL;

	completed = PortalRun(portal,
						  max_rows,
						  true, /* 始终为顶层 */
						  receiver,
						  receiver,
						  &qc);

	receiver->rDestroy(receiver);

	/* 执行完成；移除参数错误回调 */
	error_context_stack = error_context_stack->previous;

	if (completed)
	{
		if (is_xact_command || (MyXactFlags & XACT_FLAGS_NEEDIMMEDIATECOMMIT))
		{
			/*
			 * 如果这是一条事务控制语句，则提交它。我们将为下一条命令
			 * （如果有）启动一个新的事务命令。如果语句要求立即提交，
			 * 同样如此。如果没有这个规定，我们要到收到 Sync 时才会
			 * 强制提交，如果客户端试图流水线化立即提交的语句，这会造成
			 * 危险。
			 */
			finish_xact_command();

			/*
			 * 这些命令通常没有任何参数，而且即便有，我们现在也无法打印
			 * 它们，因为存储在 finish_xact_command 期间已经消失了。所以
			 * 假装它们不存在。
			 */
			portalParams = NULL;
		}
		else
		{
			/*
			 * 每条查询之后都需要一次 CommandCounterIncrement，除了那些
			 * 开启或结束事务块的查询。
			 */
			CommandCounterIncrement();

			/*
			 * 每当我们完成一条 Execute 消息而没有立即提交事务时，
			 * 设置 XACT_FLAGS_PIPELINING。
			 */
			MyXactFlags |= XACT_FLAGS_PIPELINING;

			/*
			 * 每当我们完成一条 Execute 消息时，禁用语句超时。下一条
			 * 协议消息会启动一个新的超时。
			 */
			disable_statement_timeout();
		}

		/* 向客户端发送相应的 CommandComplete */
		EndCommand(&qc, dest, false);
	}
	else
	{
		/* portal 运行未完成，因此发送 PortalSuspended */
		if (whereToSendOutput == DestRemote)
			pq_putemptymessage(PqMsg_PortalSuspended);

		/*
		 * 每当我们挂起一条 Execute 消息时，同样设置
		 * XACT_FLAGS_PIPELINING。
		 */
		MyXactFlags |= XACT_FLAGS_PIPELINING;
	}

	/*
	 * 如果合适，则记录耗时日志。
	 */
	switch (check_log_duration(msec_str, was_logged))
	{
		case 1:
			ereport(LOG,
					(errmsg("duration: %s ms", msec_str),
					 errhidestmt(true)));
			break;
		case 2:
			ereport(LOG,
					(errmsg("duration: %s ms  %s %s%s%s: %s",
							msec_str,
							execute_is_fetch ?
							_("execute fetch from") :
							_("execute"),
							prepStmtName,
							*portal_name ? "/" : "",
							*portal_name ? portal_name : "",
							sourceText),
					 errhidestmt(true),
					 errdetail_params(portalParams)));
			break;
	}

	if (save_log_statement_stats)
		ShowUsage("EXECUTE MESSAGE STATISTICS");

	valgrind_report_error_query(debug_query_string);

	debug_query_string = NULL;
}

/*
 * check_log_statement
 *		根据 log_statement 判断命令是否应当被记录
 *
 * stmt_list 既可以是原始语法分析的输出，也可以是一个已规划语句列表
 */
static bool
check_log_statement(List *stmt_list)
{
	ListCell   *stmt_item;

	if (log_statement == LOGSTMT_NONE)
		return false;
	if (log_statement == LOGSTMT_ALL)
		return true;

	/* 否则我们必须检查语句，看是否应当记录 */
	foreach(stmt_item, stmt_list)
	{
		Node	   *stmt = (Node *) lfirst(stmt_item);

		if (GetCommandLogLevel(stmt) <= log_statement)
			return true;
	}

	return false;
}

/*
 * check_log_duration
 *		判断当前命令的耗时是否应当被记录
 *		我们还会检查本事务中的这条语句是否必须被记录
 *		（无论其耗时多少）。
 *
 * 返回值：
 *		0 表示无需记录
 *		1 表示只记录耗时
 *		2 表示同时记录耗时与查询详情
 *
 * 如果需要记录，耗时（毫秒）会被格式化为字符串填入 msec_str[]，
 * 该缓冲区必须至少有 32 字节。
 *
 * 如果调用者已经记录过查询详情，was_logged 应为 true（这会实质上
 * 阻止返回 2）。
 */
int
check_log_duration(char *msec_str, bool was_logged)
{
	if (log_duration || log_min_duration_sample >= 0 ||
		log_min_duration_statement >= 0 || xact_is_sampled)
	{
		long		secs;
		int			usecs;
		int			msecs;
		bool		exceeded_duration;
		bool		exceeded_sample_duration;
		bool		in_sample = false;

		TimestampDifference(GetCurrentStatementStartTimestamp(),
							GetCurrentTimestamp(),
							&secs, &usecs);
		msecs = usecs / 1000;

		/*
		 * 这个看似奇怪的、用于判断 log_min_duration_* 是否超标的测试，
		 * 是为了避免超长耗时下的整数溢出：在确认 secs * 1000 能放入
		 * int 之前，不要去计算它。
		 */
		exceeded_duration = (log_min_duration_statement == 0 ||
							 (log_min_duration_statement > 0 &&
							  (secs > log_min_duration_statement / 1000 ||
							   secs * 1000 + msecs >= log_min_duration_statement)));

		exceeded_sample_duration = (log_min_duration_sample == 0 ||
									(log_min_duration_sample > 0 &&
									 (secs > log_min_duration_sample / 1000 ||
									  secs * 1000 + msecs >= log_min_duration_sample)));

		/*
		 * 如果 log_statement_sample_rate = 0 则不记录。如果
		 * log_statement_sample_rate <= 1 则记录一个样本；如果
		 * log_statement_sample_rate = 1 则避免不必要的 PRNG 调用。
		 */
		if (exceeded_sample_duration)
			in_sample = log_statement_sample_rate != 0 &&
				(log_statement_sample_rate == 1 ||
				 pg_prng_double(&pg_global_prng_state) <= log_statement_sample_rate);

		if (exceeded_duration || in_sample || log_duration || xact_is_sampled)
		{
			snprintf(msec_str, 32, "%ld.%03d",
					 secs * 1000 + msecs, usecs % 1000);
			if ((exceeded_duration || in_sample || xact_is_sampled) && !was_logged)
				return 2;
			else
				return 1;
		}
	}

	return 0;
}

/*
 * errdetail_execute
 *
 * 如果存在，追加一行 errdetail() 以显示 EXECUTE 所引用的查询。
 * 参数是一个原始解析树列表。
 */
static int
errdetail_execute(List *raw_parsetree_list)
{
	ListCell   *parsetree_item;

	foreach(parsetree_item, raw_parsetree_list)
	{
		RawStmt    *parsetree = lfirst_node(RawStmt, parsetree_item);

		if (IsA(parsetree->stmt, ExecuteStmt))
		{
			ExecuteStmt *stmt = (ExecuteStmt *) parsetree->stmt;
			PreparedStatement *pstmt;

			pstmt = FetchPreparedStatement(stmt->name, false);
			if (pstmt)
			{
				errdetail("prepare: %s", pstmt->plansource->query_string);
				return 0;
			}
		}
	}

	return 0;
}

/*
 * errdetail_params
 *
 * 如果存在，追加一行 errdetail() 以显示绑定参数数据。
 * 注意，这只用于语句日志记录，因此它由 log_parameter_max_length
 * 控制，而不是 log_parameter_max_length_on_error。
 */
static int
errdetail_params(ParamListInfo params)
{
	if (params && params->numParams > 0 && log_parameter_max_length != 0)
	{
		char	   *str;

		str = BuildParamLogString(params, NULL, log_parameter_max_length);
		if (str && str[0] != '\0')
			errdetail("Parameters: %s", str);
	}

	return 0;
}

/*
 * errdetail_abort
 *
 * 如果存在，追加一行 errdetail() 以显示中止原因。
 */
static int
errdetail_abort(void)
{
	if (MyProc->recoveryConflictPending)
		errdetail("Abort reason: recovery conflict");

	return 0;
}

/*
 * errdetail_recovery_conflict
 *
 * 追加一行 errdetail() 以显示冲突来源。
 */
static int
errdetail_recovery_conflict(ProcSignalReason reason)
{
	switch (reason)
	{
		case PROCSIG_RECOVERY_CONFLICT_BUFFERPIN:
			errdetail("User was holding shared buffer pin for too long.");
			break;
		case PROCSIG_RECOVERY_CONFLICT_LOCK:
			errdetail("User was holding a relation lock for too long.");
			break;
		case PROCSIG_RECOVERY_CONFLICT_TABLESPACE:
			errdetail("User was or might have been using tablespace that must be dropped.");
			break;
		case PROCSIG_RECOVERY_CONFLICT_SNAPSHOT:
			errdetail("User query might have needed to see row versions that must be removed.");
			break;
		case PROCSIG_RECOVERY_CONFLICT_LOGICALSLOT:
			errdetail("User was using a logical replication slot that must be invalidated.");
			break;
		case PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK:
			errdetail("User transaction caused buffer deadlock with recovery.");
			break;
		case PROCSIG_RECOVERY_CONFLICT_DATABASE:
			errdetail("User was connected to a database that must be dropped.");
			break;
		default:
			break;
			/* 无 errdetail */
	}

	return 0;
}

/*
 * bind_param_error_callback
 *
 * 在 Bind 消息中解析参数时使用的错误上下文回调
 */
static void
bind_param_error_callback(void *arg)
{
	BindParamCbData *data = (BindParamCbData *) arg;
	StringInfoData buf;
	char	   *quotedval;

	if (data->paramno < 0)
		return;

	/* 如果有一个文本值，则将其加引号，并在必要时截断 */
	if (data->paramval)
	{
		initStringInfo(&buf);
		appendStringInfoStringQuoted(&buf, data->paramval,
									 log_parameter_max_length_on_error);
		quotedval = buf.data;
	}
	else
		quotedval = NULL;

	if (data->portalName && data->portalName[0] != '\0')
	{
		if (quotedval)
			errcontext("portal \"%s\" parameter $%d = %s",
					   data->portalName, data->paramno + 1, quotedval);
		else
			errcontext("portal \"%s\" parameter $%d",
					   data->portalName, data->paramno + 1);
	}
	else
	{
		if (quotedval)
			errcontext("unnamed portal parameter $%d = %s",
					   data->paramno + 1, quotedval);
		else
			errcontext("unnamed portal parameter $%d",
					   data->paramno + 1);
	}

	if (quotedval)
		pfree(quotedval);
}

/*
 * exec_describe_statement_message
 *
 * 处理一个预备语句的“Describe”消息
 */
static void
exec_describe_statement_message(const char *stmt_name)
{
	CachedPlanSource *psrc;

	/*
	 * 启动一个事务命令。（注意，这通常会改变当前内存上下文。）
	 * 如果已经处于事务命令中，则什么也不会发生。
	 */
	start_xact_command();

	/* 切换回消息上下文 */
	MemoryContextSwitchTo(MessageContext);

	/* 查找预备语句 */
	if (stmt_name[0] != '\0')
	{
		PreparedStatement *pstmt;

		pstmt = FetchPreparedStatement(stmt_name, true);
		psrc = pstmt->plansource;
	}
	else
	{
		/* 对未命名语句做特殊处理 */
		psrc = unnamed_stmt_psrc;
		if (!psrc)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_PSTATEMENT),
					 errmsg("unnamed prepared statement does not exist")));
	}

	/* 预备语句不应具有可变的 result descs */
	Assert(psrc->fixed_result);

	/*
	 * 如果我们处于已中止的事务状态，则无法运行
	 * SendRowDescriptionMessage()，因为它需要访问系统表。因此，拒绝
	 * 描述那些会返回数据的语句。（我们不应简单地拒绝所有的 Describe，
	 * 因为那可能会破坏某些客户端发出 COMMIT 或 ROLLBACK 命令的能力，
	 * 如果它们使用盲目 Describe 任何内容的代码。）而描述参数是安全的，
	 * 不会做危险的事情，因此我们不对它做限制。
	 */
	if (IsAbortedTransactionBlockState() &&
		psrc->resultDesc)
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, "
						"commands ignored until end of transaction block"),
				 errdetail_abort()));

	if (whereToSendOutput != DestRemote)
		return;					/* 实际上什么也做不了…… */

	/*
	 * 首先描述参数……
	 */
	pq_beginmessage_reuse(&row_description_buf, PqMsg_ParameterDescription);
	pq_sendint16(&row_description_buf, psrc->num_params);

	for (int i = 0; i < psrc->num_params; i++)
	{
		Oid			ptype = psrc->param_types[i];

		pq_sendint32(&row_description_buf, (int) ptype);
	}
	pq_endmessage_reuse(&row_description_buf);

	/*
	 * 接下来发送 RowDescription 或 NoData 以描述结果……
	 */
	if (psrc->resultDesc)
	{
		List	   *tlist;

		/* 获取计划的主目标列表 */
		tlist = CachedPlanGetTargetList(psrc, NULL);

		SendRowDescriptionMessage(&row_description_buf,
								  psrc->resultDesc,
								  tlist,
								  NULL);
	}
	else
		pq_putemptymessage(PqMsg_NoData);
}

/*
 * exec_describe_portal_message
 *
 * 处理一个 portal 的“Describe”消息
 */
static void
exec_describe_portal_message(const char *portal_name)
{
	Portal		portal;

	/*
	 * 启动一个事务命令。（注意，这通常会改变当前内存上下文。）
	 * 如果已经处于事务命令中，则什么也不会发生。
	 */
	start_xact_command();

	/* 切换回消息上下文 */
	MemoryContextSwitchTo(MessageContext);

	portal = GetPortalByName(portal_name);
	if (!PortalIsValid(portal))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("portal \"%s\" does not exist", portal_name)));

	/*
	 * 如果我们处于已中止的事务状态，则无法运行
	 * SendRowDescriptionMessage()，因为它需要访问系统表。因此，拒绝
	 * 描述那些会返回数据的 portal。（我们不应简单地拒绝所有的
	 * Describe，因为那可能会破坏某些客户端发出 COMMIT 或 ROLLBACK
	 * 命令的能力，如果它们使用盲目 Describe 任何内容的代码。）
	 */
	if (IsAbortedTransactionBlockState() &&
		portal->tupDesc)
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, "
						"commands ignored until end of transaction block"),
				 errdetail_abort()));

	if (whereToSendOutput != DestRemote)
		return;					/* 实际上什么也做不了…… */

	if (portal->tupDesc)
		SendRowDescriptionMessage(&row_description_buf,
								  portal->tupDesc,
								  FetchPortalTargetList(portal),
								  portal->formats);
	else
		pq_putemptymessage(PqMsg_NoData);
}


/*
 * 启动/提交单条命令的便捷例程。
 */
static void
start_xact_command(void)
{
	if (!xact_started)
	{
		StartTransactionCommand();

		xact_started = true;
	}
	else if (MyXactFlags & XACT_FLAGS_PIPELINING)
	{
		/*
		 * 当第一条 Execute 消息完成时，后续的命令将在一个通过流水线化
		 * 创建的隐式事务块中执行。如果我们尚未处于某个事务块中（例如
		 * 由显式 BEGIN 启动的），则需要将事务状态更新为一个隐式块。
		 */
		BeginImplicitTransactionBlock();
	}

	/*
	 * 必要时启动语句超时。注意，这会有意地不去重置已经启动的超时计时，
	 * 以避免在 start_xact_command() 被反复调用（其间没有 finish_xact_command()
	 * 介入，例如 parse/bind/execute）时的计时开销。如果不希望这样，必须
	 * 显式地禁用超时。
	 */
	enable_statement_timeout();

	/* 必要时启动用于检查客户端是否已断开的超时。 */
	if (client_connection_check_interval > 0 &&
		IsUnderPostmaster &&
		MyProcPort &&
		!get_timeout_active(CLIENT_CONNECTION_CHECK_TIMEOUT))
		enable_timeout_after(CLIENT_CONNECTION_CHECK_TIMEOUT,
							 client_connection_check_interval);
}

static void
finish_xact_command(void)
{
	/* 每条命令之后取消活跃的语句超时 */
	disable_statement_timeout();

	if (xact_started)
	{
		CommitTransactionCommand();

#ifdef MEMORY_CONTEXT_CHECKING
		/* 检查所有在提交期间未被释放的内存上下文 */
		/* （那些已被释放的，在删除前已经被检查过了） */
		MemoryContextCheck(TopMemoryContext);
#endif

#ifdef SHOW_MEMORY_STATS
		/* 每次提交后打印内存统计，用于泄漏追踪 */
		MemoryContextStats(TopMemoryContext);
#endif

		xact_started = false;
	}
}


/*
 * 用于检查某条语句是否属于我们在事务中止状态下所允许执行的语句的
 * 便捷例程。
 */

/* 测试一个裸的解析树 */
static bool
IsTransactionExitStmt(Node *parsetree)
{
	if (parsetree && IsA(parsetree, TransactionStmt))
	{
		TransactionStmt *stmt = (TransactionStmt *) parsetree;

		if (stmt->kind == TRANS_STMT_COMMIT ||
			stmt->kind == TRANS_STMT_PREPARE ||
			stmt->kind == TRANS_STMT_ROLLBACK ||
			stmt->kind == TRANS_STMT_ROLLBACK_TO)
			return true;
	}
	return false;
}

/* 测试一个包含 PlannedStmt 节点的列表 */
static bool
IsTransactionExitStmtList(List *pstmts)
{
	if (list_length(pstmts) == 1)
	{
		PlannedStmt *pstmt = linitial_node(PlannedStmt, pstmts);

		if (pstmt->commandType == CMD_UTILITY &&
			IsTransactionExitStmt(pstmt->utilityStmt))
			return true;
	}
	return false;
}

/* 测试一个包含 PlannedStmt 节点的列表 */
static bool
IsTransactionStmtList(List *pstmts)
{
	if (list_length(pstmts) == 1)
	{
		PlannedStmt *pstmt = linitial_node(PlannedStmt, pstmts);

		if (pstmt->commandType == CMD_UTILITY &&
			IsA(pstmt->utilityStmt, TransactionStmt))
			return true;
	}
	return false;
}

/* 释放任何已存在的未命名预备语句 */
static void
drop_unnamed_stmt(void)
{
	/* 出于谨慎，避免出错时出现悬空指针 */
	if (unnamed_stmt_psrc)
	{
		CachedPlanSource *psrc = unnamed_stmt_psrc;

		unnamed_stmt_psrc = NULL;
		DropCachedPlan(psrc);
	}
}


/* --------------------------------
 *		PostgresMain() 中使用的信号处理例程
 * --------------------------------
 */

/*
 * quickdie() 在收到 postmaster 发来的 SIGQUIT 信号时被调用。
 *
 * 要么某个后端进程已经“完蛋”，要么我们被要求“立即”关闭；
 * 因此我们需要停下正在做的事情并退出。
 */
void
quickdie(SIGNAL_ARGS)
{
	sigaddset(&BlockSig, SIGQUIT);	/* 防止嵌套调用 */
	sigprocmask(SIG_SETMASK, &BlockSig, NULL);

	/*
	 * 在退出期间阻止中断；尽管我们刚刚阻塞了那些会排队新中断的信号，
	 * 但可能仍有一个处于挂起状态。我们不希望将一个 quickdie() 降级为
	 * 一次普通的查询取消。
	 */
	HOLD_INTERRUPTS();

	/*
	 * 如果我们正在中止客户端认证过程，不要冒险尝试向客户端发送任何
	 * 内容；我们很可能会违反协议，更不用说我们可能已经打断了 OpenSSL
	 * 或某些认证库的内部逻辑。
	 */
	if (ClientAuthInProgress && whereToSendOutput == DestRemote)
		whereToSendOutput = DestNone;

	/*
	 * 在退出前通知客户端，以便对发生了什么给出线索。
	 *
	 * 在信号处理函数中调用 ereport() 是值得怀疑的。它当然不是
	 * 异步信号安全的。但尝试一下似乎比突然断开连接、让客户端疑惑
	 * 到底发生了什么要好。我们在尝试发送消息时崩溃或挂起的可能性
	 * 微乎其微，而且收到 SIGQUIT 本身就表明已经有地方出了严重问题，
	 * 因此没什么可损失的。假设 postmaster 仍在运行，如果我们因某种
	 * 原因卡住，它很快就会用 SIGKILL 结束我们。
	 *
	 * 我们能做的一件让这稍微安全一点的事是清空错误上下文栈，这样
	 * 上下文回调就不会被调用。这里能触及的代码会少很多，而且上下文
	 * 信息对 SIGQUIT 报告而言也不太可能非常相关。
	 */
	error_context_stack = NULL;

	/*
	 * 当响应 postmaster 发出的信号时，我们仅将消息发送给客户端；
	 * 发送到服务器日志只会产生日志垃圾，而且还需要更多我们希望
	 * 能在信号处理函数中正常工作的代码。
	 *
	 * 理想情况下这些应该是 ereport(FATAL)，但那样我们就无法取回控制权
	 * 来强制进行正确类型的进程退出。
	 */
	switch (GetQuitSignalReason())
	{
		case PMQUIT_NOT_SENT:
			/* 嗯，SIGQUIT 凭空而来 */
			ereport(WARNING,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating connection because of unexpected SIGQUIT signal")));
			break;
		case PMQUIT_FOR_CRASH:
			/* 正在进行崩溃并重启的循环 */
			ereport(WARNING_CLIENT_ONLY,
					(errcode(ERRCODE_CRASH_SHUTDOWN),
					 errmsg("terminating connection because of crash of another server process"),
					 errdetail("The postmaster has commanded this server process to roll back"
							   " the current transaction and exit, because another"
							   " server process exited abnormally and possibly corrupted"
							   " shared memory."),
					 errhint("In a moment you should be able to reconnect to the"
							 " database and repeat your command.")));
			break;
		case PMQUIT_FOR_STOP:
			/* 立即模式停止 */
			ereport(WARNING_CLIENT_ONLY,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating connection due to immediate shutdown command")));
			break;
	}

	/*
	 * 我们不想运行 proc_exit() 或 atexit() 回调——我们之所以在这里，
	 * 是因为共享内存可能已损坏，所以我们不想尝试清理自己的事务。
	 * 把窗户钉死，然后离开小镇就好了。无论如何，这些回调在信号处理
	 * 函数中运行也是不安全的。
	 *
	 * 注意我们用的是 _exit(2) 而不是 _exit(0)。这是为了在有人向一个
	 * 随机后端手动发送 SIGQUIT 时，强制 postmaster 进入系统复位循环。
	 * 这之所以必要，正是因为我们没有清理自己的共享内存状态。
	 * （pmsignal.c 中的“死人开关”机制应该也能确保 postmaster 将此视为
	 * 一次崩溃，但双重保险没有坏处。）
	 */
	_exit(2);
}

/*
 * 来自 postmaster 的关闭信号：中止事务并在最方便的时机退出
 */
void
die(SIGNAL_ARGS)
{
	/* 不要打扰 proc_exit 的执行 */
	if (!proc_exit_inprogress)
	{
		InterruptPending = true;
		ProcDiePending = true;
	}

	/* 用于累计统计系统 */
	pgStatSessionEndCause = DISCONNECT_KILLED;

	/* 如果我们还在这里，唤醒所有等待进程 latch 的对象 */
	SetLatch(MyLatch);

	/*
	 * 如果我们处于单用户模式，我们希望立即退出——我们无法依赖 latch，
	 * 因为当 stdin/stdout 是文件时它们不会工作。这有点丑陋，但为了
	 * 单用户模式的好处而投入更多精力不太值得。
	 */
	if (DoingCommandRead && whereToSendOutput != DestRemote)
		ProcessInterrupts();
}

/*
 * 来自 postmaster 的查询取消信号：在最早方便的时机中止当前事务
 */
void
StatementCancelHandler(SIGNAL_ARGS)
{
	/*
	 * 不要干扰 proc_exit 的工作。
	 */
	if (!proc_exit_inprogress)
	{
		InterruptPending = true;
		QueryCancelPending = true;
	}

	/* 如果我们仍然在这里，则唤醒任何等待在该进程闩锁上的对象 */
	SetLatch(MyLatch);
}

/* 浮点异常的信号处理函数 */
void
FloatExceptionHandler(SIGNAL_ARGS)
{
	/* 我们不会返回，因此无需保存 errno */
	ereport(ERROR,
			(errcode(ERRCODE_FLOATING_POINT_EXCEPTION),
			 errmsg("floating-point exception"),
			 errdetail("An invalid floating-point operation was signaled. "
					   "This probably means an out-of-range result or an "
					   "invalid operation, such as division by zero.")));
}

/*
 * 通知下一次 CHECK_FOR_INTERRUPTS() 检查特定类型的恢复冲突。
 * 运行在 SIGUSR1 信号处理函数中。
 */
void
HandleRecoveryConflictInterrupt(ProcSignalReason reason)
{
	RecoveryConflictPendingReasons[reason] = true;
	RecoveryConflictPending = true;
	InterruptPending = true;
	/* latch 将由 procsignal_sigusr1_handler 设置 */
}

/*
 * 检查单个具体的冲突原因。
 */
static void
ProcessRecoveryConflictInterrupt(ProcSignalReason reason)
{
	switch (reason)
	{
		case PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK:

			/*
			 * 如果我们没有在等待某个锁，就永远不会发生死锁。
			 */
			if (GetAwaitedLock() == NULL)
				return;

			/* 故意 fall through 以检查对 pin 的等待 */
			/* FALLTHROUGH */

		case PROCSIG_RECOVERY_CONFLICT_BUFFERPIN:

			/*
			 * 如果请求的是 PROCSIG_RECOVERY_CONFLICT_BUFFERPIN，但我们
			 * 并没有阻塞 Startup 进程，那就没什么可做的了。
			 *
			 * 当请求的是 PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK 时，
			 * 如果我们正在等待锁，且启动进程并没有在等待 buffer pin
			 * （即它也在等待锁），我们就设置该标志，以便 ProcSleep()
			 * 会检查死锁。
			 */
			if (!HoldingBufferPinThatDelaysRecovery())
			{
				if (reason == PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK &&
					GetStartupBufferPinWaitBufId() < 0)
					CheckDeadLockAlert();
				return;
			}

			MyProc->recoveryConflictPending = true;

			/* 故意 fall through 到错误处理 */
			/* FALLTHROUGH */

		case PROCSIG_RECOVERY_CONFLICT_LOCK:
		case PROCSIG_RECOVERY_CONFLICT_TABLESPACE:
		case PROCSIG_RECOVERY_CONFLICT_SNAPSHOT:

			/*
			 * 如果我们已不再处于任何事务中，则忽略。
			 */
			if (!IsTransactionOrTransactionBlock())
				return;

			/* FALLTHROUGH */

		case PROCSIG_RECOVERY_CONFLICT_LOGICALSLOT:

			/*
			 * 如果我们不处于子事务中，那么抛出 ERROR 来解决冲突是可以的。
			 * 否则 fall through 到 FATAL 分支。
			 * 
			 * PROCSIG_RECOVERY_CONFLICT_LOGICALSLOT 是一个特例，它总是抛出
			 * ERROR（即永远不会升级为 FATAL），不过它仍必须遵循
			 * QueryCancelHoldoffCount，因此复用了这条代码路径。逻辑解码
			 * slot 只在执行逻辑解码时才会被获取。在逻辑解码过程中不会
			 * 运行任何用户控制的代码。在 [子]事务中止时，slot 会被释放。
			 * 因此用户控制的代码无法在复制 slot 被释放之前拦截错误。
			 * 
			 * XXX 其他我们可以只抛出 ERROR 的时机*可能*包括：
			 * 如果父事务中没有持有锁时的 PROCSIG_RECOVERY_CONFLICT_LOCK
			 * 
			 * 如果父事务没有持有快照且事务不是 transaction-snapshot 模式时的
			 * PROCSIG_RECOVERY_CONFLICT_SNAPSHOT
			 * 
			 * 如果父事务中没有打开临时文件或游标时的
			 * PROCSIG_RECOVERY_CONFLICT_TABLESPACE
			 */
			if (reason == PROCSIG_RECOVERY_CONFLICT_LOGICALSLOT ||
				!IsSubTransaction())
			{
				/*
				 * 如果已经中止，我们就不再需要取消。之所以在这里处理，是因为
				 * 我们不希望忽略已中止的子事务，当前这类子事务必须导致 FATAL。
				 */
				if (IsAbortedTransactionBlockState())
					return;

				/*
				 * 如果在等待客户端输入时发生恢复冲突，客户端大概只是空闲地
				 * 处于某个事务中，阻碍了恢复向前推进。这种情况下我们会
				 * fall through 到下面的 FATAL 分支将其“拔”出来。
				 */
				if (!DoingCommandRead)
				{
					/* 避免在前端/后端（FE/BE）协议中丢失同步。 */
					if (QueryCancelHoldoffCount != 0)
					{
						/*
						 * 重新置位并延迟该中断到稍后处理。参见
						 * ProcessInterrupts() 中的类似代码。
						 */
						RecoveryConflictPendingReasons[reason] = true;
						RecoveryConflictPending = true;
						InterruptPending = true;
						return;
					}

					/*
					 * 我们已获准抛出 ERROR。要么是逻辑 slot 的情况，要么我们
					 * 有一个可以中止的顶层事务，且冲突本身并非不可重试。
					 */
					LockErrorCleanup();
					pgstat_report_recovery_conflict(reason);
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("canceling statement due to conflict with recovery"),
							 errdetail_recovery_conflict(reason)));
					break;
				}
			}

			/* 故意 fall through 到会话取消 */
			/* FALLTHROUGH */

		case PROCSIG_RECOVERY_CONFLICT_DATABASE:

			/*
			 * 无法重试，因为数据库已被删除，或者我们之前判定无法用 ERROR
			 * 解决冲突而 fall through 了下来。终止该会话。
			 */
			pgstat_report_recovery_conflict(reason);
			ereport(FATAL,
					(errcode(reason == PROCSIG_RECOVERY_CONFLICT_DATABASE ?
							 ERRCODE_DATABASE_DROPPED :
							 ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("terminating connection due to conflict with recovery"),
					 errdetail_recovery_conflict(reason),
					 errhint("In a moment you should be able to reconnect to the"
							 " database and repeat your command.")));
			break;

		default:
			elog(FATAL, "unrecognized conflict mode: %d", (int) reason);
	}
}

/*
 * 检查每一种可能的恢复冲突原因。
 */
static void
ProcessRecoveryConflictInterrupts(void)
{
	/*
	 * 我们无需担心会打扰 proc_exit 的工作，因为 proc_exit_prepare()
	 * 会持有中断，因此 ProcessInterrupts() 不会调用我们。
	 */
	Assert(!proc_exit_inprogress);
	Assert(InterruptHoldoffCount == 0);
	Assert(RecoveryConflictPending);

	RecoveryConflictPending = false;

	for (ProcSignalReason reason = PROCSIG_RECOVERY_CONFLICT_FIRST;
		 reason <= PROCSIG_RECOVERY_CONFLICT_LAST;
		 reason++)
	{
		if (RecoveryConflictPendingReasons[reason])
		{
			RecoveryConflictPendingReasons[reason] = false;
			ProcessRecoveryConflictInterrupt(reason);
		}
	}
}

/*
 * ProcessInterrupts：CHECK_FOR_INTERRUPTS() 宏的内联之外部分
 * 
 * 如果有中断条件待处理，且处理它是安全的，那么清除标志并接受
 * 该中断。仅当 InterruptPending 为 true 时才会被调用。
 * 
 * 注意：如果 INTERRUPTS_CAN_BE_PROCESSED() 为 true，那么
 * ProcessInterrupts 保证在返回前清除 InterruptPending 标志。
 * （这并不等同于保证返回时它仍被清除；可能又来了另一个中断。
 * 但我们保证任何既有的中断都会被处理。）
 */
void
ProcessInterrupts(void)
{
	/* 现在可以接受任何中断了吗？ */
	if (InterruptHoldoffCount != 0 || CritSectionCount != 0)
		return;
	InterruptPending = false;

	if (ProcDiePending)
	{
		ProcDiePending = false;
		QueryCancelPending = false; /* ProcDie 优先于 QueryCancel */
		LockErrorCleanup();
		/* 与 quickdie 中一样，认证期间不要冒险向客户端发送。 */
		if (ClientAuthInProgress && whereToSendOutput == DestRemote)
			whereToSendOutput = DestNone;
		if (ClientAuthInProgress)
			ereport(FATAL,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("canceling authentication due to timeout")));
		else if (AmAutoVacuumWorkerProcess())
			ereport(FATAL,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating autovacuum process due to administrator command")));
		else if (IsLogicalWorker())
			ereport(FATAL,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating logical replication worker due to administrator command")));
		else if (IsLogicalLauncher())
		{
			ereport(DEBUG1,
					(errmsg_internal("logical replication launcher shutting down")));

			/*
			 * 逻辑复制启动器（launcher）可以随时被停止。
			 * 使用退出状态 1，以便后台工作进程会被重新启动。
			 */
			proc_exit(1);
		}
		else if (AmWalReceiverProcess())
			ereport(FATAL,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating walreceiver process due to administrator command")));
		else if (AmBackgroundWorkerProcess())
			ereport(FATAL,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating background worker \"%s\" due to administrator command",
							MyBgworkerEntry->bgw_type)));
		else if (AmIoWorkerProcess())
		{
			ereport(DEBUG1,
					(errmsg_internal("io worker shutting down due to administrator command")));

			proc_exit(0);
		}
		else
			ereport(FATAL,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating connection due to administrator command")));
	}

	if (CheckClientConnectionPending)
	{
		CheckClientConnectionPending = false;

		/*
		 * 检查连接是否丢失，并在仍配置的情况下重新置位；但如果已经
		 * 回到 DoingCommandRead 状态则不再检查。我们不想唤醒空闲
		 * 会话，而它们已经知道如何检测到丢失的连接。
		 */
		if (!DoingCommandRead && client_connection_check_interval > 0)
		{
			if (!pq_check_connection())
				ClientConnectionLost = true;
			else
				enable_timeout_after(CLIENT_CONNECTION_CHECK_TIMEOUT,
									 client_connection_check_interval);
		}
	}

	if (ClientConnectionLost)
	{
		QueryCancelPending = false; /* 丢失连接优先于 QueryCancel */
		LockErrorCleanup();
		/* 不要发送给客户端，我们已经确定该连接已断开。 */
		whereToSendOutput = DestNone;
		ereport(FATAL,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("connection to client lost")));
	}

	/*
	 * 在从客户端读取输入时，不允许查询取消中断，因为那样我们
	 * 可能会在前端/后端（FE/BE）协议中丢失同步。（Die 中断
	 * 是可以的，因为那种情况下我们不会再读取客户端的任何消息。）
	 * 
	 * 参见 ProcessRecoveryConflictInterrupts() 中的类似逻辑。
	 */
	if (QueryCancelPending && QueryCancelHoldoffCount != 0)
	{
		/*
		 * 重新置位 InterruptPending，以便我们在读完消息后立即处理
		 * 取消请求。（XXX 这相当丑陋：它让 INTERRUPTS_CAN_BE_PROCESSED()
		 * 变得复杂，也意味着我们不能在该函数中直接以该宏作为最初的
		 * 判断，因而这段代码也创造了让其他 bug 出现的可能。）
		 */
		InterruptPending = true;
	}
	else if (QueryCancelPending)
	{
		bool		lock_timeout_occurred;
		bool		stmt_timeout_occurred;

		QueryCancelPending = false;

		/*
		 * 如果 LOCK_TIMEOUT 与 STATEMENT_TIMEOUT 两个指示都被置位，
		 * 我们需要同时清除两者，因此总是把两者都取出来。
		 */
		lock_timeout_occurred = get_timeout_indicator(LOCK_TIMEOUT, true);
		stmt_timeout_occurred = get_timeout_indicator(STATEMENT_TIMEOUT, true);

		/*
		 * 如果两者都被置位，我们要报告先完成的那个超时；这样可确保
		 * 在机器足够慢、第二个超时在我们到达这里之前就触发时行为一致。
		 * 平局时武断地优先报告锁超时。
		 */
		if (lock_timeout_occurred && stmt_timeout_occurred &&
			get_timeout_finish_time(STATEMENT_TIMEOUT) < get_timeout_finish_time(LOCK_TIMEOUT))
			lock_timeout_occurred = false;	/* 上报语句超时 */

		if (lock_timeout_occurred)
		{
			LockErrorCleanup();
			ereport(ERROR,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("canceling statement due to lock timeout")));
		}
		if (stmt_timeout_occurred)
		{
			LockErrorCleanup();
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("canceling statement due to statement timeout")));
		}
		if (AmAutoVacuumWorkerProcess())
		{
			LockErrorCleanup();
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("canceling autovacuum task")));
		}

		/*
		 * 如果正在从客户端读取命令，就忽略取消请求——再发送一条
		 * 额外的错误消息没有任何作用。否则，直接抛出该错误。
		 */
		if (!DoingCommandRead)
		{
			LockErrorCleanup();
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("canceling statement due to user request")));
		}
	}

	if (RecoveryConflictPending)
		ProcessRecoveryConflictInterrupts();

	if (IdleInTransactionSessionTimeoutPending)
	{
		/*
		 * 如果 GUC 已被重置为 0，就忽略该信号。这一点很重要，因为
		 * GUC 更新本身不会禁用任何待处理的中断。我们需要在注入点
		 * 之前清除该标志，否则可能会在中断检查中循环。
		 */
		IdleInTransactionSessionTimeoutPending = false;
		if (IdleInTransactionSessionTimeout > 0)
		{
			INJECTION_POINT("idle-in-transaction-session-timeout", NULL);
			ereport(FATAL,
					(errcode(ERRCODE_IDLE_IN_TRANSACTION_SESSION_TIMEOUT),
					 errmsg("terminating connection due to idle-in-transaction timeout")));
		}
	}

	if (TransactionTimeoutPending)
	{
		/* 同上，如果 GUC 已被重置为 0 则忽略该信号。 */
		TransactionTimeoutPending = false;
		if (TransactionTimeout > 0)
		{
			INJECTION_POINT("transaction-timeout", NULL);
			ereport(FATAL,
					(errcode(ERRCODE_TRANSACTION_TIMEOUT),
					 errmsg("terminating connection due to transaction timeout")));
		}
	}

	if (IdleSessionTimeoutPending)
	{
		/* 同上，如果 GUC 已被重置为 0 则忽略该信号。 */
		IdleSessionTimeoutPending = false;
		if (IdleSessionTimeout > 0)
		{
			INJECTION_POINT("idle-session-timeout", NULL);
			ereport(FATAL,
					(errcode(ERRCODE_IDLE_SESSION_TIMEOUT),
					 errmsg("terminating connection due to idle-session timeout")));
		}
	}

	/*
	 * 如果有待处理的统计更新，且我们当前确实处于空闲状态
	 * （满足 PostgresMain() 中的条件），则现在上报统计信息。
	 */
	if (IdleStatsUpdateTimeoutPending &&
		DoingCommandRead && !IsTransactionOrTransactionBlock())
	{
		IdleStatsUpdateTimeoutPending = false;
		pgstat_report_stat(true);
	}

	if (ProcSignalBarrierPending)
		ProcessProcSignalBarrier();

	if (ParallelMessagePending)
		ProcessParallelMessages();

	if (LogMemoryContextPending)
		ProcessLogMemoryContextInterrupt();

	if (ParallelApplyMessagePending)
		ProcessParallelApplyMessages();

	if (SlotSyncShutdownPending)
		ProcessSlotSyncMessage();
}

/* client_connection_check_interval 的 GUC 检查钩子 */
bool
check_client_connection_check_interval(int *newval, void **extra, GucSource source)
{
	if (!WaitEventSetCanReportClosed() && *newval != 0)
	{
		GUC_check_errdetail("\"client_connection_check_interval\" must be set to 0 on this platform.");
		return false;
	}
	return true;
}

/*
 * log_parser_stats、log_planner_stats、log_executor_stats 的 GUC 检查钩子
 * 
 * 本函数与 check_log_stats 互相配合，防止这些变量被设置成不允许的组合。
 * 这是一个并不真正奏效的 hack；例如，在应用 pg_db_role_setting 的值时
 * 它可能会失败，即使最终状态本应是可以接受的。不过，由于这些变量属于
 * 生产环境中很少使用的遗留设置，我们对此予以容忍。
 */
bool
check_stage_log_stats(bool *newval, void **extra, GucSource source)
{
	if (*newval && log_statement_stats)
	{
		GUC_check_errdetail("Cannot enable parameter when \"log_statement_stats\" is true.");
		return false;
	}
	return true;
}

/* log_statement_stats 的 GUC 检查钩子 */
bool
check_log_stats(bool *newval, void **extra, GucSource source)
{
	if (*newval &&
		(log_parser_stats || log_planner_stats || log_executor_stats))
	{
		GUC_check_errdetail("Cannot enable \"log_statement_stats\" when "
							"\"log_parser_stats\", \"log_planner_stats\", "
							"or \"log_executor_stats\" is true.");
		return false;
	}
	return true;
}

/* transaction_timeout 的 GUC 赋值钩子 */
void
assign_transaction_timeout(int newval, void *extra)
{
	if (IsTransactionState())
	{
		/*
		 * 如果 transaction_timeout GUC 在事务块内部发生了改变，
		 * 就相应地启用或禁用定时器。
		 */
		if (newval > 0 && !get_timeout_active(TRANSACTION_TIMEOUT))
			enable_timeout_after(TRANSACTION_TIMEOUT, newval);
		else if (newval <= 0 && get_timeout_active(TRANSACTION_TIMEOUT))
			disable_timeout(TRANSACTION_TIMEOUT, false);
	}
}

/*
 * restrict_nonsystem_relation_kind 的 GUC 检查钩子
 */
bool
check_restrict_nonsystem_relation_kind(char **newval, void **extra, GucSource source)
{
	char	   *rawstring;
	List	   *elemlist;
	ListCell   *l;
	int			flags = 0;

	/* 需要一份可修改的字符串副本 */
	rawstring = pstrdup(*newval);

	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		/* 列表中存在语法错误 */
		GUC_check_errdetail("List syntax is invalid.");
		pfree(rawstring);
		list_free(elemlist);
		return false;
	}

	foreach(l, elemlist)
	{
		char	   *tok = (char *) lfirst(l);

		if (pg_strcasecmp(tok, "view") == 0)
			flags |= RESTRICT_RELKIND_VIEW;
		else if (pg_strcasecmp(tok, "foreign-table") == 0)
			flags |= RESTRICT_RELKIND_FOREIGN_TABLE;
		else
		{
			GUC_check_errdetail("Unrecognized key word: \"%s\".", tok);
			pfree(rawstring);
			list_free(elemlist);
			return false;
		}
	}

	pfree(rawstring);
	list_free(elemlist);

	/* 将标志保存到 *extra 中，供赋值函数使用 */
	*extra = guc_malloc(LOG, sizeof(int));
	if (!*extra)
		return false;
	*((int *) *extra) = flags;

	return true;
}

/* restrict_nonsystem_relation_kind 的 GUC 赋值钩子 */
void
assign_restrict_nonsystem_relation_kind(const char *newval, void *extra)
{
	int		   *flags = (int *) extra;

	restrict_nonsystem_relation_kind = *flags;
}

/*
 * set_debug_options --- 应用 “-d N” 命令行选项
 * 
 * -d 与设置 log_min_messages 并不完全相同，因为它还会启用其他输出选项。
 */
void
set_debug_options(int debug_flag, GucContext context, GucSource source)
{
	if (debug_flag > 0)
	{
		char		debugstr[64];

		sprintf(debugstr, "debug%d", debug_flag);
		SetConfigOption("log_min_messages", debugstr, context, source);
	}
	else
		SetConfigOption("log_min_messages", "notice", context, source);

	if (debug_flag >= 1 && context == PGC_POSTMASTER)
	{
		SetConfigOption("log_connections", "all", context, source);
		SetConfigOption("log_disconnections", "true", context, source);
	}
	if (debug_flag >= 2)
		SetConfigOption("log_statement", "all", context, source);
	if (debug_flag >= 3)
		SetConfigOption("debug_print_parse", "true", context, source);
	if (debug_flag >= 4)
		SetConfigOption("debug_print_plan", "true", context, source);
	if (debug_flag >= 5)
		SetConfigOption("debug_print_rewritten", "true", context, source);
}


bool
set_plan_disabling_options(const char *arg, GucContext context, GucSource source)
{
	const char *tmp = NULL;

	switch (arg[0])
	{
		case 's':				/* 顺序扫描 */
			tmp = "enable_seqscan";
			break;
		case 'i':				/* 索引扫描 */
			tmp = "enable_indexscan";
			break;
		case 'o':				/* 仅索引扫描 */
			tmp = "enable_indexonlyscan";
			break;
		case 'b':				/* 位图扫描 */
			tmp = "enable_bitmapscan";
			break;
		case 't':				/* TID 扫描 */
			tmp = "enable_tidscan";
			break;
		case 'n':				/* 嵌套循环 */
			tmp = "enable_nestloop";
			break;
		case 'm':				/* 归并连接 */
			tmp = "enable_mergejoin";
			break;
		case 'h':				/* 哈希连接 */
			tmp = "enable_hashjoin";
			break;
	}
	if (tmp)
	{
		SetConfigOption(tmp, "false", context, source);
		return true;
	}
	else
		return false;
}


const char *
get_stats_option_name(const char *arg)
{
	switch (arg[0])
	{
		case 'p':
			if (optarg[1] == 'a')	/* "parser" */
				return "log_parser_stats";
			else if (optarg[1] == 'l')	/* "planner" */
				return "log_planner_stats";
			break;

		case 'e':				/* "executor" */
			return "log_executor_stats";
			break;
	}

	return NULL;
}


/*
 * process_postgres_switches
 * 	  解析后端进程的命令行参数
 * 
 * 本函数会被调用两次：一次解析来自 postmaster 或命令行的安全选项，
 * 另一次解析来自客户端启动包的不安全选项。后者语法相同，但其作用
 * 可能受到限制。
 * 
 * 上述两种情况都会忽略 argv[0]（假定它是程序名）。
 * 
 * ctx 对于安全选项为 PGC_POSTMASTER；对于来自客户端的不安全选项为
 * PGC_BACKEND；对于来自超级用户客户端的不安全选项为 PGC_SU_BACKEND。
 * 
 * 如果命令行参数中给出了数据库名，则通过 *dbname 返回（仅当 *dbname
 * 初始为 NULL 时才允许）。
 */
void
process_postgres_switches(int argc, char *argv[], GucContext ctx,
						  const char **dbname)
{
	bool		secure = (ctx == PGC_POSTMASTER);
	int			errs = 0;
	GucSource	gucsource;
	int			flag;

	if (secure)
	{
		gucsource = PGC_S_ARGV; /* 这些开关来自命令行 */

		/*
		 * 忽略初始的 --single 参数（如果存在的话）
		 */
		if (argc > 1 && strcmp(argv[1], "--single") == 0)
		{
			argv++;
			argc--;
		}
	}
	else
	{
		gucsource = PGC_S_CLIENT;	/* 这些开关来自客户端 */
	}

#ifdef HAVE_INT_OPTERR

	/*
	 * 将其关闭，因为它要么被打印到 stderr 而非日志（那并非我们想要的位置），
	 * 要么此时 argv[0] 是 “--single”，那样会产生一条奇怪的错误消息。
	 * 我们会在下面打印自己的错误消息。
	 */
	opterr = 0;
#endif

	/*
	 * 解析命令行选项。注意：请与 postmaster/postmaster.c（选项集合不应
	 * 相互冲突）以及 main/main.c 中通用的 help() 函数保持同步。
	 */
	while ((flag = getopt(argc, argv, "B:bC:c:D:d:EeFf:h:ijk:lN:nOPp:r:S:sTt:v:W:-:")) != -1)
	{
		switch (flag)
		{
			case 'B':
				SetConfigOption("shared_buffers", optarg, ctx, gucsource);
				break;

			case 'b':
				/* 用于二进制升级的未公开标志 */
				if (secure)
					IsBinaryUpgrade = true;
				break;

			case 'C':
				/* 为与 postmaster 保持一致而忽略 */
				break;

			case '-':

				/*
				 * 如果用户放错了必须排在最前面的特殊选项（用于分派到子程序），则报错。
				 * parse_dispatch_option() 如果没找到匹配项会返回 DISPATCH_POSTMASTER，
				 * 因此对于其他任何情况都报错。
				 */
				if (parse_dispatch_option(optarg) != DISPATCH_POSTMASTER)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("--%s must be first argument", optarg)));

				/* FALLTHROUGH */
			case 'c':
				{
					char	   *name,
							   *value;

					ParseLongOption(optarg, &name, &value);
					if (!value)
					{
						if (flag == '-')
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
					SetConfigOption(name, value, ctx, gucsource);
					pfree(name);
					pfree(value);
					break;
				}

			case 'D':
				if (secure)
					userDoption = strdup(optarg);
				break;

			case 'd':
				set_debug_options(atoi(optarg), ctx, gucsource);
				break;

			case 'E':
				if (secure)
					EchoQuery = true;
				break;

			case 'e':
				SetConfigOption("datestyle", "euro", ctx, gucsource);
				break;

			case 'F':
				SetConfigOption("fsync", "false", ctx, gucsource);
				break;

			case 'f':
				if (!set_plan_disabling_options(optarg, ctx, gucsource))
					errs++;
				break;

			case 'h':
				SetConfigOption("listen_addresses", optarg, ctx, gucsource);
				break;

			case 'i':
				SetConfigOption("listen_addresses", "*", ctx, gucsource);
				break;

			case 'j':
				if (secure)
					UseSemiNewlineNewline = true;
				break;

			case 'k':
				SetConfigOption("unix_socket_directories", optarg, ctx, gucsource);
				break;

			case 'l':
				SetConfigOption("ssl", "true", ctx, gucsource);
				break;

			case 'N':
				SetConfigOption("max_connections", optarg, ctx, gucsource);
				break;

			case 'n':
				/* 为与 postmaster 保持一致而忽略 */
				break;

			case 'O':
				SetConfigOption("allow_system_table_mods", "true", ctx, gucsource);
				break;

			case 'P':
				SetConfigOption("ignore_system_indexes", "true", ctx, gucsource);
				break;

			case 'p':
				SetConfigOption("port", optarg, ctx, gucsource);
				break;

			case 'r':
				/* 将输出（stdout 与 stderr）发送到给定文件 */
				if (secure)
					strlcpy(OutputFileName, optarg, MAXPGPATH);
				break;

			case 'S':
				SetConfigOption("work_mem", optarg, ctx, gucsource);
				break;

			case 's':
				SetConfigOption("log_statement_stats", "true", ctx, gucsource);
				break;

			case 'T':
				/* 为与 postmaster 保持一致而忽略 */
				break;

			case 't':
				{
					const char *tmp = get_stats_option_name(optarg);

					if (tmp)
						SetConfigOption(tmp, "true", ctx, gucsource);
					else
						errs++;
					break;
				}

			case 'v':

				/*
				 * -v 在正常操作中已不再使用，因为在我们到达这里之前 FrontendProtocol
				 * 就已经被设置好了。我们保留这个开关只是可能用于独立（standalone）
				 * 模式，以防将来支持在独立后端中使用普通的 FE/BE 协议。
				 */
				if (secure)
					FrontendProtocol = (ProtocolVersion) atoi(optarg);
				break;

			case 'W':
				SetConfigOption("post_auth_delay", optarg, ctx, gucsource);
				break;

			default:
				errs++;
				break;
		}

		if (errs)
			break;
	}

	/* 可选的数据库名只有当 *dbname 为 NULL 时才应该存在。 */
	if (!errs && dbname && *dbname == NULL && argc - optind >= 1)
		*dbname = strdup(argv[optind++]);

	if (errs || argc != optind)
	{
		if (errs)
			optind--;			/* 针对前一个参数报错 */

		/* 根据上下文以略有不同的措辞书写错误消息 */
		if (IsUnderPostmaster)
			ereport(FATAL,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("invalid command-line argument for server process: %s", argv[optind]),
					errhint("Try \"%s --help\" for more information.", progname));
		else
			ereport(FATAL,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("%s: invalid command-line argument: %s",
						   progname, argv[optind]),
					errhint("Try \"%s --help\" for more information.", progname));
	}

	/*
	 * 重置 getopt(3) 库，使其在子进程中被再次调用或在本函数以另一个
	 * 数组第二次被调用时能正常工作。
	 */
	optind = 1;
#ifdef HAVE_INT_OPTRESET
	optreset = 1;				/* 某些系统也需要这个 */
#endif
}


/*
 * PostgresSingleUserMain
 * 	  单用户模式的入口点。argc/argv 为要使用的命令行参数。
 * 
 * 执行单用户特有的初始化设置，然后调用 PostgresMain() 实际处理查询。
 * 在合理的情况下，单用户模式特有的初始化设置应放在这里，而不是放在
 * PostgresMain() 或 InitPostgres() 中。
 */
void
PostgresSingleUserMain(int argc, char *argv[],
					   const char *username)
{
	const char *dbname = NULL;

	Assert(!IsUnderPostmaster);

	/* 初始化启动进程环境。 */
	InitStandaloneProcess(argv[0]);

	/*
	 * 为命令行选项设置默认值。
	 */
	InitializeGUCOptions();

	/*
	 * Parse command-line options.
	 */
	process_postgres_switches(argc, argv, PGC_POSTMASTER, &dbname);

	/* 必须已经得到一个数据库名，或者有一个默认值（即用户名） */
	if (dbname == NULL)
	{
		dbname = username;
		if (dbname == NULL)
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("%s: no database nor user name specified",
							progname)));
	}

	/* 获取配置参数 */
	if (!SelectConfigFiles(userDoption, progname))
		proc_exit(1);

	/*
	 * 验证我们得到的 DataDir 看起来合理，并切换到该目录。
	 */
	checkDataDir();
	ChangeToDataDir();

	/*
	 * 为数据目录创建锁文件。
	 */
	CreateDataDirLockFile(false);

	/* 读取控制文件（包含错误检查与配置） */
	LocalProcessControlFile(false);

	/*
	 * 处理那些应在 postmaster 启动时预加载的库。
	 */
	process_shared_preload_libraries();

	/* 初始化 MaxBackends */
	InitializeMaxBackends();

	/*
	 * 在单用户模式下我们不需要 postmaster 子进程槽，但为了不写特殊
	 * 处理逻辑，仍然对它们进行初始化。
	 */
	InitPostmasterChildSlots();

	/* 初始化快速路径锁缓存的大小。 */
	InitializeFastPathLocks();

	/*
	 * 给预加载的库一个机会，让它们请求额外的共享内存。
	 */
	process_shmem_requests();

	/*
	 * 既然可加载模块已经请求过额外的共享内存，现在来确定那些依赖于
	 * 所需共享内存量的、运行时计算的 GUC 的值。
	 */
	InitializeShmemGUCs();

	/*
	 * 既然模块已经加载，我们就可以处理 wal_consistency_checking GUC
	 * 中指定的任何自定义资源管理器。
	 */
	InitializeWalConsistencyChecking();

	/*
	 * 创建共享内存等。（在单用户模式下其实并没有什么“共享”的东西，
	 * 但我们仍然必须拥有这些数据结构。）
	 */
	CreateSharedMemoryAndSemaphores();

	/*
	 * 估算可打开文件的数量。这必须在设置完信号量之后进行，因为在某些
	 * 平台上信号量也算作打开的文件。
	 */
	set_max_safe_fds();

	/*
	 * 记录独立后端的启动时间，大致位于 postmaster 在启动过程中记录该
	 * 时间的同一位置。
	 */
	PgStartTime = GetCurrentTimestamp();

	/*
	 * 在共享内存中创建一个每后端（per-backend）的 PGPROC 结构。我们
	 * 必须在使用 LWLocks 之前完成这一步。
	 */
	InitProcess();

	/*
	 * 既然已经初始化了足够的基础设施，剩下的工作可由 PostgresMain()
	 * 来完成。
	 */
	PostgresMain(dbname, username);
}


/* ----------------------------------------------------------------
 * PostgresMain
 *	   postgres 主循环 —— 所有后端（无论是交互式还是其他）都在这里循环
 *
 * dbname 为要连接的数据库名，username 为该会话所用的 PostgreSQL
 * 用户名。
 *
 * 注意：单用户模式特有的初始化设置应尽可能放到
 * PostgresSingleUserMain() 中。
 * ----------------------------------------------------------------
 */
void
PostgresMain(const char *dbname, const char *username)
{
	sigjmp_buf	local_sigjmp_buf;

	/* 这些变量必须是 volatile 的，以确保状态在 longjmp 之后仍被保留： */
	volatile bool send_ready_for_query = true;
	volatile bool idle_in_transaction_timeout_enabled = false;
	volatile bool idle_session_timeout_enabled = false;

	Assert(dbname != NULL);
	Assert(username != NULL);

	Assert(GetProcessingMode() == InitProcessing);

	/*
	 * 设置信号处理函数。（InitPostmasterChild 或 InitStandaloneProcess
	 * 已经设置好 BlockSig 并将其设为当前的信号掩码。）
	 * 
	 * 注意：postmaster 在 fork 子进程之前阻塞了所有信号，因此我们
	 * 不会存在这样的竞态：在设置好处理函数之前就收到信号。
	 * 
	 * 另请注意：最好不要使用任何在 postmaster 中被设为 SIG_IGN 的
	 * 信号。如果这样一个信号在我们能够把处理函数改为非 SIG_IGN 之前
	 * 到达，它会被丢弃。因此，应在 postmaster 中设置一个哑处理函数
	 * 来保留该信号。（当然，对于本地生成的信号，如 SIGALRM 和
	 * SIGPIPE，这不成问题。）
	 */
	if (am_walsender)
		WalSndSignals();
	else
	{
		pqsignal(SIGHUP, SignalHandlerForConfigReload);
		pqsignal(SIGINT, StatementCancelHandler);	/* 取消当前查询 */
		pqsignal(SIGTERM, die); /* 取消当前查询并退出 */

		/*
		 * 在 postmaster 的子后端中，用 quickdie 替换 SignalHandlerForCrashExit，
		 * 这样我们就能告诉客户端我们正在退出。
		 * 
		 * 在独立后端中，SIGQUIT 可以很容易地从键盘产生，而 SIGTERM 不能，
		 * 因此我们让这两个信号都执行 die() 而不是 quickdie()。
		 */
		if (IsUnderPostmaster)
			pqsignal(SIGQUIT, quickdie);	/* 硬崩溃时 */
		else
			pqsignal(SIGQUIT, die); /* 取消当前查询并退出 */
		InitializeTimeouts();	/* 建立 SIGALRM 处理函数 */

		/*
		 * 忽略向前端写入失败的情况。注意：如果前端关闭连接，我们会在控制
		 * 权下次返回到外层循环时注意到并干净地退出。这似乎比在谁也不知道
		 * 正在进行何种操作的输出过程中强行退出更安全……
		 */
		pqsignal(SIGPIPE, SIG_IGN);
		pqsignal(SIGUSR1, procsignal_sigusr1_handler);
		pqsignal(SIGUSR2, SIG_IGN);
		pqsignal(SIGFPE, FloatExceptionHandler);

		/*
		 * 重置一些 postmaster 接受但后端不接受的信号
		 */
		pqsignal(SIGCHLD, SIG_DFL); /*
		 * 在某些平台上，system() 需要这个（平台相关）。
		 */
	}

	/* 早期初始化 */
	BaseInit();

	/* 在初始事务期间，我们需要允许 SIGINT 等信号 */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * 生成一个随机的取消键（cancel key），如果这是一个为连接
	 * 提供服务的后端。InitPostgres() 会把它发布到共享内存中。
	 */
	Assert(MyCancelKeyLength == 0);
	if (whereToSendOutput == DestRemote)
	{
		int			len;

		len = (MyProcPort == NULL || MyProcPort->proto >= PG_PROTOCOL(3, 2))
			? MAX_CANCEL_KEY_LENGTH : 4;
		if (!pg_strong_random(&MyCancelKey, len))
		{
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not generate random cancel key")));
		}
		MyCancelKeyLength = len;
	}

	/*
	 * 常规初始化。
	 * 
	 * 注意：如果你想在这一带添加代码，请考虑把它放进 InitPostgres()
	 * 里面。特别是，任何涉及数据库访问的操作都应该放在那里，而不是这里。
	 * 
	 * 如果不是 WAL sender，则遵循 session_preload_libraries。
	 */
	InitPostgres(dbname, InvalidOid,	/* 要连接的数据库 */
				 username, InvalidOid,	/* 要作为的角色 */
				 (!am_walsender) ? INIT_PG_LOAD_SESSION_LIBS : 0,
				 NULL);			/* 无 out_dbname */

	/*
	 * 如果 PostmasterContext 还存在，回收其空间；在 InitPostgres()
	 * 完成之后我们就不再需要它了。
	 */
	if (PostmasterContext)
	{
		MemoryContextDelete(PostmasterContext);
		PostmasterContext = NULL;
	}

	SetProcessingMode(NormalProcessing);

	/*
	 * 现在所有 GUC 状态都已完全设置好。如果合适，就将其报告给客户端。
	 */
	BeginReportingGUCOptions();

	/*
	 * 同时设置用于记录会话结束的处理函数；我们必须等到现在，才能
	 * 确保 Log_disconnections 已经具有最终值。
	 */
	if (IsUnderPostmaster && Log_disconnections)
		on_proc_exit(log_disconnections, 0);

	pgstat_report_connect(MyDatabaseId);

	/* 执行特定于 WAL sender 进程的初始化。 */
	if (am_walsender)
		InitWalSender();

	/*
	 * 将该后端的取消信息发送给前端。
	 */
	if (whereToSendOutput == DestRemote)
	{
		StringInfoData buf;

		Assert(MyCancelKeyLength > 0);
		pq_beginmessage(&buf, PqMsg_BackendKeyData);
		pq_sendint32(&buf, (int32) MyProcPid);

		pq_sendbytes(&buf, MyCancelKey, MyCancelKeyLength);
		pq_endmessage(&buf);
		/* 无需刷新，因为 ReadyForQuery 会做这件事。 */
	}

	/* 独立运行情况下的欢迎横幅 */
	if (whereToSendOutput == DestDebug)
		printf("\nPostgreSQL stand-alone backend %s\n", PG_VERSION);

	/*
	 * 创建我们将在主循环中使用的内存上下文。
	 * 
	 * MessageContext 在主循环的每次迭代（即完成处理来自客户端的每个
	 * 命令消息）时都会被重置一次。
	 */
	MessageContext = AllocSetContextCreate(TopMemoryContext,
										   "MessageContext",
										   ALLOCSET_DEFAULT_SIZES);

	/*
	 * 创建用于 RowDescription 消息的内存上下文和缓冲区。由于
	 * exec_describe_statement_message() 会对几乎每条语句频繁执行，
	 * 我们不想每次都分配单独的缓冲区。
	 */
	row_description_context = AllocSetContextCreate(TopMemoryContext,
													"RowDescriptionContext",
													ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(row_description_context);
	initStringInfo(&row_description_buf);
	MemoryContextSwitchTo(TopMemoryContext);

	/* 如果合适，触发任何已定义的登录事件触发器 */
	EventTriggerOnLogin();

	/*
	 * POSTGRES 主处理循环从此处开始
	 * 
	 * 如果遇到异常，处理会在这里恢复，以便我们中止当前事务并启动一个新事务。
	 * 
	 * 你可能会奇怪为什么这里没有写成围绕 PG_TRY 构造的无限循环。原因是
	 * 这里是异常栈的最底端，因此如果使用 PG_TRY，在 CATCH 部分就完全
	 * 不会有生效的异常处理器。通过让最外层的 setjmp 始终处于活动状态，
	 * 我们至少有机会从错误恢复期间的错误中恢复过来。（如果因此陷入
	 * 无限循环，elog.c 内部状态栈溢出会很快将其停止。）
	 * 
	 * 注意：我们使用 sigsetjmp(..., 1)，因此本函数的信号掩码（即
	 * UnBlockSig）在 longjmp 回到此处时会被恢复。这一点很重要，以防
	 * 我们是 longjmp 出了某个信号处理函数（在某些平台上这会导致信号
	 * 保持阻塞）。它并非多余于 AbortTransaction() 中的解除阻塞，因为
	 * 后者仅在我们处于事务内部时才会被调用。
	 */

	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/*
		 * 注意：如果你想在这个 if 块中添加更多代码，请考虑它极有可能
		 * 应该放在 AbortTransaction() 中。这里直接处理的只应是那些保证
		 * *仅*适用于最外层错误恢复的事情，例如调整 FE/BE 协议状态。
		 */

		/* 由于没有使用 PG_TRY，必须手动重置错误栈 */
		error_context_stack = NULL;

		/* 清理期间阻止中断 */
		HOLD_INTERRUPTS();

		/*
		 * 忘记任何待处理的 QueryCancel 请求，反正我们也要回到空闲循环了，
		 * 并取消任何活跃的超时请求。（将来我们或许希望允许某些超时请求
		 * 存活下来，但至少需要调用 reschedule_timeouts()，以防我们是因为
		 * 查询取消打断了 SIGALRM 中断处理函数而到达这里的。）特别要注意：
		 * 我们必须清除语句超时和锁超时的指示标志，以防我们忘记的是一次
		 * 超时取消，从而导致将来任何普通的查询取消被误报为超时。
		 */
		disable_all_timeouts(false);	/* 先做，以避免竞态条件 */
		QueryCancelPending = false;
		idle_in_transaction_timeout_enabled = false;
		idle_session_timeout_enabled = false;

		/* 不再从客户端读取了。 */
		DoingCommandRead = false;

		/* 确保 libpq 处于良好状态 */
		pq_comm_reset();

		/* 将错误报告给客户端和/或服务器日志 */
		EmitErrorReport();

		/*
		 * 如果 Valgrind 在出错查询期间发现了问题，就打印该查询字符串
		 * （假设我们有它的话）。
		 */
		valgrind_report_error_query(debug_query_string);

		/*
		 * 确保在我们可能覆盖 debug_query_string 所指向的存储之前，
		 * 先把它重置。
		 */
		debug_query_string = NULL;

		/*
		 * 中止当前事务以进行恢复。
		 */
		AbortCurrentTransaction();

		if (am_walsender)
			WalSndErrorCleanup();

		PortalErrorCleanup();

		/*
		 * 我们无法在 AbortTransaction() 内部释放复制 slot，因为我们需要在
		 * 持有 slot 的同时启动和中止事务。但我们绝不需要在顶层错误之间
		 * 持有它们，因此在这里释放是没问题的。另外还有一个 before_shmem_exit()
		 * 回调，用于在 FATAL 错误时确保正确的清理。
		 */
		if (MyReplicationSlot != NULL)
			ReplicationSlotRelease();

		/* 我们也希望在出错时清理临时 slot。 */
		ReplicationSlotCleanup(false);

		jit_reset_after_error();

		/*
		 * 现在返回到正常的顶层上下文，并为下次清空 ErrorContext。
		 */
		MemoryContextSwitchTo(MessageContext);
		FlushErrorState();

		/*
		 * 如果我们正在处理扩展查询协议消息，则发起跳转到下一个
		 * Sync。这还会导致我们不再发出 ReadyForQuery（直到收到 Sync）。
		 */
		if (doing_extended_query_message)
			ignore_till_sync = true;

		/* 我们不再持有打开的事务命令了 */
		xact_started = false;

		/*
		 * 如果在从客户端读取消息时发生错误，我们可能已经搞不清上一条
		 * 消息在哪里结束、下一条从哪里开始。即使我们已经从错误中恢复，
		 * 也无法安全地再从客户端读取任何消息，因此这个连接也没什么
		 * 可做的了。
		 */
		if (pq_is_reading_msg())
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("terminating connection because protocol synchronization was lost")));

		/* 现在可以再次允许中断了 */
		RESUME_INTERRUPTS();
	}

	/* 我们现在可以处理 ereport(ERROR) 了 */
	PG_exception_stack = &local_sigjmp_buf;

	if (!ignore_till_sync)
		send_ready_for_query = true;	/* 最初，或出错之后 */

	/*
	 * 非错误查询在此处循环。
	 */

	for (;;)
	{
		int			firstchar;
		StringInfoData input_message;

		/*
		 * 在循环顶部，重置扩展查询消息标志，以免在“空闲”状态下
		 * 遇到的任何错误引发跳过。
		 */
		doing_extended_query_message = false;

		/*
		 * 出于 Valgrind 报告的目的，“当前查询”从这里开始。
		 */
#ifdef USE_VALGRIND
		old_valgrind_error_count = VALGRIND_COUNT_ERRORS;
#endif

		/*
		 * 释放上一轮查询周期遗留的存储，并在已清空的 MessageContext
		 * 中创建一个新的查询输入缓冲区。
		 */
		MemoryContextSwitchTo(MessageContext);
		MemoryContextReset(MessageContext);

		initStringInfo(&input_message);

		/*
		 * 同时考虑在可能的情况下释放我们的目录快照，以免它在我们等待
		 * 客户端期间阻碍全局 xmin 的推进。
		 */
		InvalidateCatalogSnapshotConditionally();

		/*
		 * (1) 如果我们已进入空闲状态，就告诉前端我们已经准备好接收
		 * 新的查询。
		 * 
		 * 注意：这包括 fflush() 刷新之前的输出。
		 * 
		 * 这也是将收集到的统计信息刷入累积统计系统、并更新 PS 统计
		 * 显示的好时机。我们避免每次都通过消息循环来做这些事，因为那
		 * 会拖慢批量消息的处理，也因为我们不想上报未提交的更新（那会
		 * 让 autovacuum 感到困惑）。如果我们不处于事务块中，通知处理器
		 * 也想要一次调用。
		 * 
		 * 此外，如果启用了空闲超时，就为它启动定时器。
		 */
		if (send_ready_for_query)
		{
			if (IsAbortedTransactionBlockState())
			{
				set_ps_display("idle in transaction (aborted)");
				pgstat_report_activity(STATE_IDLEINTRANSACTION_ABORTED, NULL);

				/* 启动 idle-in-transaction 定时器 */
				if (IdleInTransactionSessionTimeout > 0
					&& (IdleInTransactionSessionTimeout < TransactionTimeout || TransactionTimeout == 0))
				{
					idle_in_transaction_timeout_enabled = true;
					enable_timeout_after(IDLE_IN_TRANSACTION_SESSION_TIMEOUT,
										 IdleInTransactionSessionTimeout);
				}
			}
			else if (IsTransactionOrTransactionBlock())
			{
				set_ps_display("idle in transaction");
				pgstat_report_activity(STATE_IDLEINTRANSACTION, NULL);

				/* 启动 idle-in-transaction 定时器 */
				if (IdleInTransactionSessionTimeout > 0
					&& (IdleInTransactionSessionTimeout < TransactionTimeout || TransactionTimeout == 0))
				{
					idle_in_transaction_timeout_enabled = true;
					enable_timeout_after(IDLE_IN_TRANSACTION_SESSION_TIMEOUT,
										 IdleInTransactionSessionTimeout);
				}
			}
			else
			{
				long		stats_timeout;

				/*
				 * 处理收到的通知（包括自通知），如果有的话，并向客户端发送
				 * 相关消息。在这里做有助于在测试中确保行为稳定：如果在刚结束的
				 * 事务期间收到了任何通知，它们会在 ReadyForQuery 之前被客户端看到。
				 */
				if (notifyInterruptPending)
					ProcessNotifyInterrupt(false);

				/*
				 * 检查是否需要上报统计信息。如果 pgstat_report_stat() 认为现在
				 * 上报待处理统计信息还为时过早，或者锁竞争阻止了上报，它会告诉
				 * 我们何时应该再次尝试上报统计信息（这样在连接长时间空闲时，
				 * 统计更新就不会被不当地延迟）。只有当还没有正在进行的超时时我们
				 * 才启用该超时，因为我们在下面并不会禁用该超时。enable_timeout_after()
				 * 需要确定当前时间戳，这可能带来负面的性能影响。这没关系，因为
				 * pgstat_report_stat() 不会让我们比上一次调用更早被唤醒。
				 */
				stats_timeout = pgstat_report_stat(false);
				if (stats_timeout > 0)
				{
					if (!get_timeout_active(IDLE_STATS_UPDATE_TIMEOUT))
						enable_timeout_after(IDLE_STATS_UPDATE_TIMEOUT,
											 stats_timeout);
				}
				else
				{
					/* 所有统计信息都已刷新，无需该超时 */
					if (get_timeout_active(IDLE_STATS_UPDATE_TIMEOUT))
						disable_timeout(IDLE_STATS_UPDATE_TIMEOUT, false);
				}

				set_ps_display("idle");
				pgstat_report_activity(STATE_IDLE, NULL);

				/* 启动 idle-session 定时器 */
				if (IdleSessionTimeout > 0)
				{
					idle_session_timeout_enabled = true;
					enable_timeout_after(IDLE_SESSION_TIMEOUT,
										 IdleSessionTimeout);
				}
			}

			/* 上报任何最近变更的 GUC 选项 */
			ReportChangedGUCOptions();

			/*
			 * 当这个后端第一次准备好查询时，记录连接建立与设置各组成部分
			 * 所耗的时长。
			 */
			if (conn_timing.ready_for_use == TIMESTAMP_MINUS_INFINITY &&
				(log_connections & LOG_CONNECTION_SETUP_DURATIONS) &&
				IsExternalConnectionBackend(MyBackendType))
			{
				uint64		total_duration,
							fork_duration,
							auth_duration;

				conn_timing.ready_for_use = GetCurrentTimestamp();

				total_duration =
					TimestampDifferenceMicroseconds(conn_timing.socket_create,
													conn_timing.ready_for_use);
				fork_duration =
					TimestampDifferenceMicroseconds(conn_timing.fork_start,
													conn_timing.fork_end);
				auth_duration =
					TimestampDifferenceMicroseconds(conn_timing.auth_start,
													conn_timing.auth_end);

				ereport(LOG,
						errmsg("connection ready: setup total=%.3f ms, fork=%.3f ms, authentication=%.3f ms",
							   (double) total_duration / NS_PER_US,
							   (double) fork_duration / NS_PER_US,
							   (double) auth_duration / NS_PER_US));
			}

			ReadyForQuery(whereToSendOutput);
			send_ready_for_query = false;
		}

		/*
		 * (2) 允许异步信号在我们等待客户端输入时立即执行。（这必须是
		 * 有条件的，因为我们不希望例如代表 COPY FROM STDIN 的读取
		 * 做同样的事情。）
		 */
		DoingCommandRead = true;

		/*
		 * (3) 读取一条命令（循环在此阻塞）
		 */
		firstchar = ReadCommand(&input_message);

		/*
		 * (4) 如果空闲事务内（idle-in-transaction）和空闲会话（idle-session）
		 * 超时处于活动状态，就关闭它们。我们在步骤 (5) 之前做这件事，以便
		 * 任何最后一刻的超时都能被步骤 (5) 确定地检测到。
		 * 
		 * 这些超时中最多只有一个会处于活动状态，因此无需把对 timeout.c
		 * 的调用合并为一个。
		 */
		if (idle_in_transaction_timeout_enabled)
		{
			disable_timeout(IDLE_IN_TRANSACTION_SESSION_TIMEOUT, false);
			idle_in_transaction_timeout_enabled = false;
		}
		if (idle_session_timeout_enabled)
		{
			disable_timeout(IDLE_SESSION_TIMEOUT, false);
			idle_session_timeout_enabled = false;
		}

		/*
		 * (5) 再次禁用异步信号条件。
		 * 
		 * 查询取消在没有进行中的查询时应当是一个空操作，因此如果在我们
		 * 空闲时来了查询取消，只需重置 QueryCancelPending 即可。
		 * ProcessInterrupts() 在设置了 DoingCommandRead 时被调用会产生
		 * 这样的效果，因此在重置 DoingCommandRead 之前先检查中断。
		 */
		CHECK_FOR_INTERRUPTS();
		DoingCommandRead = false;

		/*
		 * (6) 检查在我们睡眠期间发生的任何其他有趣事件。
		 */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/*
		 * (7) 处理命令。但如果我们正在跳到 Sync，则忽略它。
		 */
		if (ignore_till_sync && firstchar != EOF)
			continue;

		switch (firstchar)
		{
			case PqMsg_Query:
				{
					const char *query_string;

					/* 设置 statement_timestamp() */
					SetCurrentStatementStartTimestamp();

					query_string = pq_getmsgstring(&input_message);
					pq_getmsgend(&input_message);

					if (am_walsender)
					{
						if (!exec_replication_command(query_string))
							exec_simple_query(query_string);
					}
					else
						exec_simple_query(query_string);

					valgrind_report_error_query(query_string);

					send_ready_for_query = true;
				}
				break;

			case PqMsg_Parse:
				{
					const char *stmt_name;
					const char *query_string;
					int			numParams;
					Oid		   *paramTypes = NULL;

					forbidden_in_wal_sender(firstchar);

					/* 设置 statement_timestamp() */
					SetCurrentStatementStartTimestamp();

					stmt_name = pq_getmsgstring(&input_message);
					query_string = pq_getmsgstring(&input_message);
					numParams = pq_getmsgint(&input_message, 2);
					if (numParams > 0)
					{
						paramTypes = palloc_array(Oid, numParams);
						for (int i = 0; i < numParams; i++)
							paramTypes[i] = pq_getmsgint(&input_message, 4);
					}
					pq_getmsgend(&input_message);

					exec_parse_message(query_string, stmt_name,
									   paramTypes, numParams);

					valgrind_report_error_query(query_string);
				}
				break;

			case PqMsg_Bind:
				forbidden_in_wal_sender(firstchar);

				/* 设置 statement_timestamp() */
				SetCurrentStatementStartTimestamp();

				/*
				 * 这条消息足够复杂，因此最好把字段提取放到行外（out-of-line）
				 * 去做。
				 */
				exec_bind_message(&input_message);

				/* exec_bind_message 会执行 valgrind_report_error_query */
				break;

			case PqMsg_Execute:
				{
					const char *portal_name;
					int			max_rows;

					forbidden_in_wal_sender(firstchar);

					/* 设置 statement_timestamp() */
					SetCurrentStatementStartTimestamp();

					portal_name = pq_getmsgstring(&input_message);
					max_rows = pq_getmsgint(&input_message, 4);
					pq_getmsgend(&input_message);

					exec_execute_message(portal_name, max_rows);

					/* exec_execute_message 会执行 valgrind_report_error_query */
				}
				break;

			case PqMsg_FunctionCall:
				forbidden_in_wal_sender(firstchar);

				/* 设置 statement_timestamp() */
				SetCurrentStatementStartTimestamp();

				/* 向各种监控设施报告查询。 */
				pgstat_report_activity(STATE_FASTPATH, NULL);
				set_ps_display("<FASTPATH>");

				/* 为该函数的调用启动一个事务 */
				start_xact_command();

				/*
				 * 注意：此时我们可能处于一个已中止的事务内部。在读完
				 * 函数调用消息之前，我们不能为此抛出错误，因此
				 * HandleFunctionRequest() 必须在读完之后检查这一点。注意不要
				 * 做任何假设我们处于有效事务内部的操作。
				 */

				/* 切换回消息上下文 */
				MemoryContextSwitchTo(MessageContext);

				HandleFunctionRequest(&input_message);

				/* 提交该函数调用的事务 */
				finish_xact_command();

				valgrind_report_error_query("fastpath function call");

				send_ready_for_query = true;
				break;

			case PqMsg_Close:
				{
					int			close_type;
					const char *close_target;

					forbidden_in_wal_sender(firstchar);

					close_type = pq_getmsgbyte(&input_message);
					close_target = pq_getmsgstring(&input_message);
					pq_getmsgend(&input_message);

					switch (close_type)
					{
						case 'S':
							if (close_target[0] != '\0')
								DropPreparedStatement(close_target, false);
							else
							{
								/* 对未命名语句做特殊处理 */
								drop_unnamed_stmt();
							}
							break;
						case 'P':
							{
								Portal		portal;

								portal = GetPortalByName(close_target);
								if (PortalIsValid(portal))
									PortalDrop(portal, false);
							}
							break;
						default:
							ereport(ERROR,
									(errcode(ERRCODE_PROTOCOL_VIOLATION),
									 errmsg("invalid CLOSE message subtype %d",
											close_type)));
							break;
					}

					if (whereToSendOutput == DestRemote)
						pq_putemptymessage(PqMsg_CloseComplete);

					valgrind_report_error_query("CLOSE message");
				}
				break;

			case PqMsg_Describe:
				{
					int			describe_type;
					const char *describe_target;

					forbidden_in_wal_sender(firstchar);

					/* 设置 statement_timestamp()（事务所需） */
					SetCurrentStatementStartTimestamp();

					describe_type = pq_getmsgbyte(&input_message);
					describe_target = pq_getmsgstring(&input_message);
					pq_getmsgend(&input_message);

					switch (describe_type)
					{
						case 'S':
							exec_describe_statement_message(describe_target);
							break;
						case 'P':
							exec_describe_portal_message(describe_target);
							break;
						default:
							ereport(ERROR,
									(errcode(ERRCODE_PROTOCOL_VIOLATION),
									 errmsg("invalid DESCRIBE message subtype %d",
											describe_type)));
							break;
					}

					valgrind_report_error_query("DESCRIBE message");
				}
				break;

			case PqMsg_Flush:
				pq_getmsgend(&input_message);
				if (whereToSendOutput == DestRemote)
					pq_flush();
				break;

			case PqMsg_Sync:
				pq_getmsgend(&input_message);

				/*
				 * 如果使用了流水线（pipelining），我们可能处于一个隐式
				 * 事务块中。在调用 finish_xact_command 之前先将其关闭。
				 */
				EndImplicitTransactionBlock();
				finish_xact_command();
				valgrind_report_error_query("SYNC message");
				send_ready_for_query = true;
				break;

				/*
				 * PqMsg_Terminate 表示前端正在关闭套接字。EOF 表示前端
				 * 连接意外丢失。无论哪种情况，都执行正常的关闭。
				 */
			case EOF:

				/* 用于累积统计系统 */
				pgStatSessionEndCause = DISCONNECT_CLIENT_EOF;

				/* FALLTHROUGH */

			case PqMsg_Terminate:

				/*
				 * 重置 whereToSendOutput，以防 ereport 试图向客户端发送
				 * 更多消息。
				 */
				if (whereToSendOutput == DestRemote)
					whereToSendOutput = DestNone;

				/*
				 * 注意：如果你想在这里添加更多代码，千万别！你想做的任何事情
				 * 都应该设置为 on_proc_exit 或 on_shmem_exit 回调，而不是放在这里。
				 * 否则在其他后端关闭场景中它将不会被调用。
				 */
				proc_exit(0);

			case PqMsg_CopyData:
			case PqMsg_CopyDone:
			case PqMsg_CopyFail:

				/*
				 * 按照协议规范，接受但忽略这些消息；我们大概是因为某个 COPY
				 * 失败而到达这里，前端仍在发送数据。
				 */
				break;

			default:
				ereport(FATAL,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("invalid frontend message type %d",
								firstchar)));
		}
	}							/* 输入读取循环结束 */
}

/*
 * 如果我们是 WAL sender 进程，则抛出错误。
 * 
 * 这用于禁止在 WAL sender 进程中使用简单查询协议之外的任何其他
 * 消息。'firstchar' 指定收到的是哪种被禁止的消息，并用于构造
 * 错误消息。
 */
static void
forbidden_in_wal_sender(char firstchar)
{
	if (am_walsender)
	{
		if (firstchar == PqMsg_FunctionCall)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("fastpath function calls not supported in a replication connection")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("extended query protocol not supported in a replication connection")));
	}
}


static struct rusage Save_r;
static struct timeval Save_t;

void
ResetUsage(void)
{
	getrusage(RUSAGE_SELF, &Save_r);
	gettimeofday(&Save_t, NULL);
}

void
ShowUsage(const char *title)
{
	StringInfoData str;
	struct timeval user,
				sys;
	struct timeval elapse_t;
	struct rusage r;

	getrusage(RUSAGE_SELF, &r);
	gettimeofday(&elapse_t, NULL);
	memcpy(&user, &r.ru_utime, sizeof(user));
	memcpy(&sys, &r.ru_stime, sizeof(sys));
	if (elapse_t.tv_usec < Save_t.tv_usec)
	{
		elapse_t.tv_sec--;
		elapse_t.tv_usec += 1000000;
	}
	if (r.ru_utime.tv_usec < Save_r.ru_utime.tv_usec)
	{
		r.ru_utime.tv_sec--;
		r.ru_utime.tv_usec += 1000000;
	}
	if (r.ru_stime.tv_usec < Save_r.ru_stime.tv_usec)
	{
		r.ru_stime.tv_sec--;
		r.ru_stime.tv_usec += 1000000;
	}

	/*
	 * 我们在这里不展示的唯一统计信息是 ixrss、idrss、isrss。要解释
	 * 它们需要一些工作，而且大多数平台不会填写它们。
	 */
	initStringInfo(&str);

	appendStringInfoString(&str, "! system usage stats:\n");
	appendStringInfo(&str,
					 "!\t%ld.%06ld s user, %ld.%06ld s system, %ld.%06ld s elapsed\n",
					 (long) (r.ru_utime.tv_sec - Save_r.ru_utime.tv_sec),
					 (long) (r.ru_utime.tv_usec - Save_r.ru_utime.tv_usec),
					 (long) (r.ru_stime.tv_sec - Save_r.ru_stime.tv_sec),
					 (long) (r.ru_stime.tv_usec - Save_r.ru_stime.tv_usec),
					 (long) (elapse_t.tv_sec - Save_t.tv_sec),
					 (long) (elapse_t.tv_usec - Save_t.tv_usec));
	appendStringInfo(&str,
					 "!\t[%ld.%06ld s user, %ld.%06ld s system total]\n",
					 (long) user.tv_sec,
					 (long) user.tv_usec,
					 (long) sys.tv_sec,
					 (long) sys.tv_usec);
#ifndef WIN32

	/*
	 * 以下 rusage 字段并非由 POSIX 定义，但它们出现在当前所有
	 * 类 Unix 系统上，因此我们不加任何特殊检查就使用它们。其中
	 * 某些字段可以通过我们在 src/port/win32getrusage.c 中的 Windows
	 * 模拟以更多工作来提供。
	 */
	appendStringInfo(&str,
					 "!\t%ld kB max resident size\n",
#if defined(__darwin__)
	/* 在 macOS 上以字节为单位 */
					 r.ru_maxrss / 1024
#else
	/* 在大多数其他平台上以千字节为单位 */
					 r.ru_maxrss
#endif
		);
	appendStringInfo(&str,
					 "!\t%ld/%ld [%ld/%ld] filesystem blocks in/out\n",
					 r.ru_inblock - Save_r.ru_inblock,
	/* 他们只在 dec 喝咖啡 */
					 r.ru_oublock - Save_r.ru_oublock,
					 r.ru_inblock, r.ru_oublock);
	appendStringInfo(&str,
					 "!\t%ld/%ld [%ld/%ld] page faults/reclaims, %ld [%ld] swaps\n",
					 r.ru_majflt - Save_r.ru_majflt,
					 r.ru_minflt - Save_r.ru_minflt,
					 r.ru_majflt, r.ru_minflt,
					 r.ru_nswap - Save_r.ru_nswap,
					 r.ru_nswap);
	appendStringInfo(&str,
					 "!\t%ld [%ld] signals rcvd, %ld/%ld [%ld/%ld] messages rcvd/sent\n",
					 r.ru_nsignals - Save_r.ru_nsignals,
					 r.ru_nsignals,
					 r.ru_msgrcv - Save_r.ru_msgrcv,
					 r.ru_msgsnd - Save_r.ru_msgsnd,
					 r.ru_msgrcv, r.ru_msgsnd);
	appendStringInfo(&str,
					 "!\t%ld/%ld [%ld/%ld] voluntary/involuntary context switches\n",
					 r.ru_nvcsw - Save_r.ru_nvcsw,
					 r.ru_nivcsw - Save_r.ru_nivcsw,
					 r.ru_nvcsw, r.ru_nivcsw);
#endif							/* 非 WIN32 */

	/* 移除末尾的换行符 */
	if (str.data[str.len - 1] == '\n')
		str.data[--str.len] = '\0';

	ereport(LOG,
			(errmsg_internal("%s", title),
			 errdetail_internal("%s", str.data)));

	pfree(str.data);
}

/* 用于记录会话结束的 on_proc_exit 处理函数 */
static void
log_disconnections(int code, Datum arg)
{
	Port	   *port = MyProcPort;
	long		secs;
	int			usecs;
	int			msecs;
	int			hours,
				minutes,
				seconds;

	TimestampDifference(MyStartTimestamp,
						GetCurrentTimestamp(),
						&secs, &usecs);
	msecs = usecs / 1000;

	hours = secs / SECS_PER_HOUR;
	secs %= SECS_PER_HOUR;
	minutes = secs / SECS_PER_MINUTE;
	seconds = secs % SECS_PER_MINUTE;

	ereport(LOG,
			(errmsg("disconnection: session time: %d:%02d:%02d.%03d "
					"user=%s database=%s host=%s%s%s",
					hours, minutes, seconds, msecs,
					port->user_name, port->database_name, port->remote_host,
					port->remote_port[0] ? " port=" : "", port->remote_port)));
}

/*
 * 如果启用了语句超时，则启动语句超时定时器。
 * 
 * 如果已经有一个超时正在运行，不要重启定时器。那样可以在超时
 * 的精度与启动超时的开销之间取得折中。
 */
static void
enable_statement_timeout(void)
{
	/* 必须处于事务内部 */
	Assert(xact_started);

	if (StatementTimeout > 0
		&& (StatementTimeout < TransactionTimeout || TransactionTimeout == 0))
	{
		if (!get_timeout_active(STATEMENT_TIMEOUT))
			enable_timeout_after(STATEMENT_TIMEOUT, StatementTimeout);
	}
	else
	{
		if (get_timeout_active(STATEMENT_TIMEOUT))
			disable_timeout(STATEMENT_TIMEOUT, false);
	}
}

/* 如果处于活动状态，则禁用语句超时。 */
static void
disable_statement_timeout(void)
{
	if (get_timeout_active(STATEMENT_TIMEOUT))
		disable_timeout(STATEMENT_TIMEOUT, false);
}
