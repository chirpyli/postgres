/*-------------------------------------------------------------------------
 *
 * elog.h
 *	  POSTGRES 的错误上报与日志定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/elog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ELOG_H
#define ELOG_H

#include <setjmp.h>

#include "lib/stringinfo.h"

/* 此时还无法包含 nodes.h，因此先前向声明 struct Node */
struct Node;


/* 错误级别代码 */
#define DEBUG5		10			/* 调试信息，按详尽程度递减分类。 */
#define DEBUG4		11
#define DEBUG3		12
#define DEBUG2		13
#define DEBUG1		14			/* 由 GUC 的 debug_* 变量使用 */
#define LOG			15			/* 服务器运行消息；默认仅发送到服务器日志。 */
#define LOG_SERVER_ONLY 16		/* 服务器上报方面与 LOG 相同，但从不发送给客户端。 */
#define COMMERROR	LOG_SERVER_ONLY /* 客户端通信问题；服务器上报方面与LOG 相同，但从不发送给客户端。 */
#define INFO		17			/* 用户特别请求的消息（例如VACUUM VERBOSE 的输出）；无论client_min_messages 如何，总是发送给客户端，但默认不写入服务器日志。 */
#define NOTICE		18			/* 对用户有用的、关于查询执行的提示性消息；默认发送给客户端，但不写入服务器日志。 */
#define WARNING		19			/* 警告。NOTICE 用于预期内的消息，例如 SERIAL 隐式创建序列； WARNING 用于非预期消息。 */
#define PGWARNING	19			/* 必须与 WARNING 相等；见下方 NOTE。 */
#define WARNING_CLIENT_ONLY	20	/* 照常发送给客户端的警告，但从不写入服务器日志。 */
#define ERROR		21			/* 用户错误——中止事务；回到 已知状态 */
#define PGERROR		21			/* 必须与 ERROR 相等；见下方 NOTE。 */
#define FATAL		22			/* 致命错误——中止进程 */
#define PANIC		23			/* 拉上其他后端一起陪葬 */

/*
 * 注意：别名 PGWARNING 与 PGERROR 用于处理那些对 WARNING 和/或
 * ERROR 做了另行定义的第三方头文件。例如，在包含此类头文件之后，
 * 可以把 ERROR 重新定义为 PGERROR。
 */


/* 用于紧凑表示 SQLSTATE 字符串的宏 */
#define PGSIXBIT(ch)	(((ch) - '0') & 0x3F)
#define PGUNSIXBIT(val) (((val) & 0x3F) + '0')

#define MAKE_SQLSTATE(ch1,ch2,ch3,ch4,ch5)	\
	(PGSIXBIT(ch1) + (PGSIXBIT(ch2) << 6) + (PGSIXBIT(ch3) << 12) + \
	 (PGSIXBIT(ch4) << 18) + (PGSIXBIT(ch5) << 24))

/* 这些宏依赖于这样一个事实：'0' 经 PGSIXBIT 处理后变为 0 */
#define ERRCODE_TO_CATEGORY(ec)  ((ec) & ((1 << 12) - 1))
#define ERRCODE_IS_CATEGORY(ec)  (((ec) & ~((1 << 12) - 1)) == 0)

/* 错误的 SQLSTATE 代码定义在一个单独的文件里 */
#include "utils/errcodes.h"

/*
 * 提供一种机制，防止在 elog() 或 ereport() 调用内部
 * 意外使用 "errno"。由于我们知道某些操作系统把 errno 定义成
 * 涉及函数调用的形式，我们会在局部作用域里放一个与该
 * 函数同名的局部变量，从而强制产生编译错误。在那些不以这种方式
 * 定义 errno 的平台上，不会发生任何事情，因此我们也不会收到警告……
 * 但只要在一些主流平台上能触发该检查，我们就可以接受这种权衡。
 */
#if defined(errno) && defined(__linux__)
#define pg_prevent_errno_in_scope() int __errno_location pg_attribute_unused()
#elif defined(errno) && (defined(__darwin__) || defined(__FreeBSD__))
#define pg_prevent_errno_in_scope() int __error pg_attribute_unused()
#else
#define pg_prevent_errno_in_scope()
#endif


/*----------
 * 新风格的错误上报 API：使用方式如下：
 *		ereport(ERROR,
 *				errcode(ERRCODE_UNDEFINED_CURSOR),
 *				errmsg("portal \"%s\" not found", stmt->portalname),
 *				... 其它所需的 errxxx() 字段 ...);
 *
 * 错误级别是必需的，一条主错误消息（errmsg 或
 * errmsg_internal）同样必需。其余皆为可选项。若未指定 errcode()，
 * 则当 elevel 为 ERROR 或更高时默认为 ERRCODE_INTERNAL_ERROR，
 * 当 elevel 为 WARNING 时默认为 ERRCODE_WARNING，
 * 当 elevel 为 NOTICE 或更低时默认为 ERRCODE_SUCCESSFUL_COMPLETION。
 *
 * 在 Postgres v12 之前，辅助函数调用列表外面必须加额外的括号；
 * 现在这已经变成可选项。
 *
 * ereport_domain() 允许指定一个消息域（message domain），供那些
 * 希望使用与后端不同的消息目录（message catalog）的模块使用。
 * 为了避免每个 .o 文件都各持有一份默认文本域，我们在这里把它
 * 定义为 NULL，并让 errstart 插入默认文本域。模块既可以直接使用
 * ereport_domain()，更好的做法则是重写 TEXTDOMAIN 宏。
 *
 * 当存在 __builtin_constant_p 且 elevel >= ERROR 时，我们改为调用
 * errstart_cold() 而非 errstart()。这个版本的.errstart 函数被标记为
 * pg_attribute_cold，从而能促使支持的编译器生成更偏向非 ERROR
 * 情形的优化代码。因为我们把 __builtin_constant_p() 用于条件判断，
 * 当 elevel 不是编译期常量，或者虽是常量但小于 ERROR 时，编译器
 * 无需为这个分支生成任何代码，它只需无条件地调用 errstart() 即可。
 *
 * 如果 elevel >= ERROR，该调用不会返回；我们试图通过 pg_unreachable()
 * 把这个事实告知编译器。然而，除非编译器把 elevel 视为编译期常量，
 * 否则不会获得任何有用的优化效果，否则我们只是增加了代码体积。
 * 因此，如果存在 __builtin_constant_p，就利用它让第二个 if() 在
 * 非常量情形下完全消失。我们避免使用局部变量，因为那既无必要，
 * 又会让 gcc 无法在 -O0 优化级别下做出不可达（unreachable）推断。
 *----------
 */
#ifdef HAVE__BUILTIN_CONSTANT_P
#define ereport_domain(elevel, domain, ...)	\
	do { \
		pg_prevent_errno_in_scope(); \
		if (__builtin_constant_p(elevel) && (elevel) >= ERROR ? \
			errstart_cold(elevel, domain) : \
			errstart(elevel, domain)) \
			__VA_ARGS__, errfinish(__FILE__, __LINE__, __func__); \
		if (__builtin_constant_p(elevel) && (elevel) >= ERROR) \
			pg_unreachable(); \
	} while(0)
#else							/* 非 HAVE__BUILTIN_CONSTANT_P 的情况 */
#define ereport_domain(elevel, domain, ...)	\
	do { \
		const int elevel_ = (elevel); \
		pg_prevent_errno_in_scope(); \
		if (errstart(elevel_, domain)) \
			__VA_ARGS__, errfinish(__FILE__, __LINE__, __func__); \
		if (elevel_ >= ERROR) \
			pg_unreachable(); \
	} while(0)
#endif							/* HAVE__BUILTIN_CONSTANT_P */

#define ereport(elevel, ...)	\
	ereport_domain(elevel, TEXTDOMAIN, __VA_ARGS__)

#define TEXTDOMAIN NULL

extern bool message_level_is_interesting(int elevel);

extern bool errstart(int elevel, const char *domain);
extern pg_attribute_cold bool errstart_cold(int elevel, const char *domain);
extern void errfinish(const char *filename, int lineno, const char *funcname);

extern int	errcode(int sqlerrcode);

extern int	errcode_for_file_access(void);
extern int	errcode_for_socket_access(void);

extern int	errmsg(const char *fmt,...) pg_attribute_printf(1, 2);
extern int	errmsg_internal(const char *fmt,...) pg_attribute_printf(1, 2);

extern int	errmsg_plural(const char *fmt_singular, const char *fmt_plural,
						  unsigned long n,...) pg_attribute_printf(1, 4) pg_attribute_printf(2, 4);

extern int	errdetail(const char *fmt,...) pg_attribute_printf(1, 2);
extern int	errdetail_internal(const char *fmt,...) pg_attribute_printf(1, 2);

extern int	errdetail_log(const char *fmt,...) pg_attribute_printf(1, 2);

extern int	errdetail_log_plural(const char *fmt_singular,
								 const char *fmt_plural,
								 unsigned long n,...) pg_attribute_printf(1, 4) pg_attribute_printf(2, 4);

extern int	errdetail_plural(const char *fmt_singular, const char *fmt_plural,
							 unsigned long n,...) pg_attribute_printf(1, 4) pg_attribute_printf(2, 4);

extern int	errhint(const char *fmt,...) pg_attribute_printf(1, 2);
extern int	errhint_internal(const char *fmt,...) pg_attribute_printf(1, 2);

extern int	errhint_plural(const char *fmt_singular, const char *fmt_plural,
						   unsigned long n,...) pg_attribute_printf(1, 4) pg_attribute_printf(2, 4);

/*
 * errcontext() 通常在错误上下文回调函数（error context callback）
 * 中调用，而不是在 ereport() 调用内部。回调函数可能位于与
 * ereport() 调用不同的模块中，因此 errstart() 传入的消息域
 * 通常并不是翻译上下文消息所用的正确域。
 * set_errcontext_domain() 先设置要使用的域，而
 * errcontext_msg() 传入实际的消息文本。
 */
#define errcontext	set_errcontext_domain(TEXTDOMAIN),	errcontext_msg

extern int	set_errcontext_domain(const char *domain);

extern int	errcontext_msg(const char *fmt,...) pg_attribute_printf(1, 2);

extern int	errhidestmt(bool hide_stmt);
extern int	errhidecontext(bool hide_ctx);

extern int	errbacktrace(void);

extern int	errposition(int cursorpos);

extern int	internalerrposition(int cursorpos);
extern int	internalerrquery(const char *query);

extern int	err_generic_string(int field, const char *str);

extern int	geterrcode(void);
extern int	geterrposition(void);
extern int	getinternalerrposition(void);


/*----------
 * 旧风格的错误上报 API：使用方式如下：
 *		elog(ERROR, "portal \"%s\" not found", stmt->portalname);
 *----------
 */
#define elog(elevel, ...)  \
	ereport(elevel, errmsg_internal(__VA_ARGS__))


/*----------
 * 支持上报“软”错误（soft error），这类错误不需要通过中止整个事务
 * 来清理。使用方式如下：
 *		errsave(context,
 *				errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
 *				errmsg("invalid input syntax for type %s: \"%s\"",
 *					   "boolean", in_str),
 *				... 其它所需的 errxxx() 字段 ...);
 *
 * "context" 是一个节点指针或 NULL，其余辅助调用提供的错误细节
 * 与 ereport() 相同。如果 context 不是指向 ErrorSaveContext 节点的
 * 指针，那么 errsave(context, ...) 的行为与 ereport(ERROR, ...)
 * 完全一致。如果 context 是指向 ErrorSaveContext 节点的指针，那么
 * 辅助调用所提供的信息会被存入该上下文节点，控制流正常返回。
 * errsave() 的调用方随后必须完成任何必要的清理工作，并将控制权
 * 返回给它的调用方。该调用方必须检查 ErrorSaveContext 节点，
 * 以判断在信任本函数结果有意义之前是否发生了错误。
 *
 * errsave_domain() 允许指定消息域；它与 ereport_domain()
 * 完全类似。
 *----------
 */
#define errsave_domain(context, domain, ...)	\
	do { \
		struct Node *context_ = (context); \
		pg_prevent_errno_in_scope(); \
		if (errsave_start(context_, domain)) \
			__VA_ARGS__, errsave_finish(context_, __FILE__, __LINE__, __func__); \
	} while(0)

#define errsave(context, ...)	\
	errsave_domain(context, TEXTDOMAIN, __VA_ARGS__)

/*
 * "ereturn(context, dummy_value, ...);" 与
 * "errsave(context, ...); return dummy_value;" 完全等价。
 * 在函数在上报软错误后无需任何清理动作这种常见情形下，
 * 这样能省去一些打字工作。"dummy_value" 在函数为
 * 返回 void 时可以为空。
 */
#define ereturn_domain(context, dummy_value, domain, ...)	\
	do { \
		errsave_domain(context, domain, __VA_ARGS__); \
		return dummy_value; \
	} while(0)

#define ereturn(context, dummy_value, ...)	\
	ereturn_domain(context, dummy_value, TEXTDOMAIN, __VA_ARGS__)

extern bool errsave_start(struct Node *context, const char *domain);
extern void errsave_finish(struct Node *context,
						   const char *filename, int lineno,
						   const char *funcname);


/* 支持与 ereport() 调用相分离地构造错误消息字符串 */

extern void pre_format_elog_string(int errnumber, const char *domain);
extern char *format_elog_string(const char *fmt,...) pg_attribute_printf(1, 2);


/* 支持向错误报告附加上下文信息 */

typedef struct ErrorContextCallback
{
	struct ErrorContextCallback *previous;
	void		(*callback) (void *arg);
	void	   *arg;
} ErrorContextCallback;

extern PGDLLIMPORT ErrorContextCallback *error_context_stack;


/*----------
 * 用于捕获 ereport(ERROR) 退出的 API。这些宏的使用方式如下：
 *
 *		PG_TRY();
 *		{
 *			... 可能抛出 ereport(ERROR) 的代码 ...
 *		}
 *		PG_CATCH();
 *		{
 *			... 错误恢复代码 ...
 *		}
 *		PG_END_TRY();
 *
 * （花括号其实并非必要，但推荐使用，这样 pgindent 才能把该结构
 * 漂亮地缩进。）错误恢复代码既可以调用 PG_RE_THROW 把错误向外传播，
 * 也可以执行（子）事务中止。如果不这样做，可能会使系统处于
 * 不一致的状态，影响后续处理。
 *
 * 对于错误恢复代码与正常代码路径中的清理工作完全相同的常见情形，
 * 可以改用以下形式：
 *
 *		PG_TRY();
 *		{
 *			... 可能抛出 ereport(ERROR) 的代码 ...
 *		}
 *		PG_FINALLY();
 *		{
 *			... 清理代码 ...
 *		}
 *      PG_END_TRY();
 *
 * 清理代码在两种情形下都会被执行，且任何错误随后都会被重新抛出。
 *
 * 不能在同一个 PG_TRY()/PG_END_TRY() 块中同时使用 PG_CATCH() 和
 * PG_FINALLY()。
 *
 * 注意：虽然系统能正确地传播在恢复段中新产生的任何 ereport(ERROR)，
 * 但这种传播所支持的嵌套层数存在上限。最好让错误恢复段保持足够简单，
 * 使其无法产生任何新错误，至少在弹出错误栈之前如此。
 *
 * 注意：ereport(FATAL) 不会被这一结构捕获；控制流会径直穿过
 * proc_exit() 退出。因此，切勿把任何非进程本地资源的清理工作放入
 * 错误恢复段，至少不要在不考虑 ereport(FATAL) 期间会发生什么的前提下
 * 这样做。对于此类情形，storage/ipc.h 提供的
 * PG_ENSURE_ERROR_CLEANUP 宏或许能帮上忙。
 *
 * 注意：如果包含 PG_TRY 的函数中的某个局部变量在 PG_TRY 段被修改、
 * 并在 PG_CATCH 段被使用，那么该变量必须声明为 "volatile" 以满足
 * POSIX 规范。这并非吹毛求疵；我们确实见过因为这类变量未被标记，
 * 编译器错误地将其优化掉而引发的 bug。要注意，gcc 的 -Wclobbered
 * 警告对于捕捉此类疏忽几乎毫无用处。
 *
 * 这些宏中的每一个都接受一个可选参数，可用于给宏内部声明的变量
 * 追加一个后缀。在遇到需要嵌套 PG_TRY() 语句、并以 -Wshadow 编译时，
 * 这个后缀可以避免编译器发出关于变量遮蔽（shadowed）的警告。
 * 该可选后缀可以包含变量名所允许的任何字符。如果指定了后缀，那么
 * 在给定 PG_TRY() 语句的各个组成宏中，后缀必须保持一致。
 *----------
 */
#define PG_TRY(...)  \
	do { \
		sigjmp_buf *_save_exception_stack##__VA_ARGS__ = PG_exception_stack; \
		ErrorContextCallback *_save_context_stack##__VA_ARGS__ = error_context_stack; \
		sigjmp_buf _local_sigjmp_buf##__VA_ARGS__; \
		bool _do_rethrow##__VA_ARGS__ = false; \
		if (sigsetjmp(_local_sigjmp_buf##__VA_ARGS__, 0) == 0) \
		{ \
			PG_exception_stack = &_local_sigjmp_buf##__VA_ARGS__

#define PG_CATCH(...)	\
		} \
		else \
		{ \
			PG_exception_stack = _save_exception_stack##__VA_ARGS__; \
			error_context_stack = _save_context_stack##__VA_ARGS__

#define PG_FINALLY(...) \
		} \
		else \
			_do_rethrow##__VA_ARGS__ = true; \
		{ \
			PG_exception_stack = _save_exception_stack##__VA_ARGS__; \
			error_context_stack = _save_context_stack##__VA_ARGS__

#define PG_END_TRY(...)  \
		} \
		if (_do_rethrow##__VA_ARGS__) \
				PG_RE_THROW(); \
		PG_exception_stack = _save_exception_stack##__VA_ARGS__; \
		error_context_stack = _save_context_stack##__VA_ARGS__; \
	} while (0)

#define PG_RE_THROW()  \
	pg_re_throw()

extern PGDLLIMPORT sigjmp_buf *PG_exception_stack;


/* 错误处理函数可能想要使用的东西 */

/*
 * ErrorData 保存在任意一次 ereport() 周期中累积起来的数据。
 * 任何非 NULL 的指针都必须指向 palloc 分配的数据。
 * （const 指针是例外；我们假设它们指向不可释放的
 * 常量字符串。）
 */
typedef struct ErrorData
{
	int			elevel;			/* 错误级别 */
	bool		output_to_server;	/* 是否上报到服务器日志？ */
	bool		output_to_client;	/* 是否上报到客户端？ */
	bool		hide_stmt;		/* 为 true 则不在 STATEMENT: 中包含语句 */
	bool		hide_ctx;		/* 为 true 则不在 CONTEXT: 中包含上下文 */
	const char *filename;		/* ereport() 调用处的 __FILE__ */
	int			lineno;			/* ereport() 调用处的 __LINE__ */
	const char *funcname;		/* ereport() 调用处的 __func__ */
	const char *domain;			/* 消息域 */
	const char *context_domain; /* 上下文消息所用的消息域 */
	int			sqlerrcode;		/* 编码后的 ERRSTATE */
	char	   *message;		/* 主错误消息（已翻译） */
	char	   *detail;			/* 详细错误消息 */
	char	   *detail_log;		/* 仅用于服务器日志的详细错误消息 */
	char	   *hint;			/* 提示消息 */
	char	   *context;		/* 上下文消息 */
	char	   *backtrace;		/* 回溯信息 */
	const char *message_id;		/* 主消息的 id（原始字符串） */
	char	   *schema_name;	/* 模式（schema）名 */
	char	   *table_name;		/* 表名 */
	char	   *column_name;	/* 列名 */
	char	   *datatype_name;	/* 数据类型名 */
	char	   *constraint_name;	/* 约束名 */
	int			cursorpos;		/* 在查询字符串中的游标索引 */
	int			internalpos;	/* 在 internalquery 中的游标索引 */
	char	   *internalquery;	/* 内部生成的查询文本 */
	int			saved_errno;	/* 进入时的 errno */

	/* 包含相关联的非常量字符串的上下文 */
	struct MemoryContextData *assoc_context;
} ErrorData;

extern void EmitErrorReport(void);
extern ErrorData *CopyErrorData(void);
extern void FreeErrorData(ErrorData *edata);
extern void FlushErrorState(void);
pg_noreturn extern void ReThrowError(ErrorData *edata);
extern void ThrowErrorData(ErrorData *edata);
pg_noreturn extern void pg_re_throw(void);

extern char *GetErrorContextStack(void);

/* 用于在消息发送到服务器日志之前拦截它们的钩子 */
typedef void (*emit_log_hook_type) (ErrorData *edata);
extern PGDLLIMPORT emit_log_hook_type emit_log_hook;


/* 可由 GUC 配置的参数 */

typedef enum
{
	PGERROR_TERSE,				/* 单行错误消息 */
	PGERROR_DEFAULT,			/* 推荐的风格 */
	PGERROR_VERBOSE,			/* 把所有事实都摆出来 */
}			PGErrorVerbosity;

extern PGDLLIMPORT int Log_error_verbosity;
extern PGDLLIMPORT char *Log_line_prefix;
extern PGDLLIMPORT int Log_destination;
extern PGDLLIMPORT char *Log_destination_string;
extern PGDLLIMPORT bool syslog_sequence_numbers;
extern PGDLLIMPORT bool syslog_split_messages;

/* 日志目标位图 */
#define LOG_DESTINATION_STDERR	 1
#define LOG_DESTINATION_SYSLOG	 2
#define LOG_DESTINATION_EVENTLOG 4
#define LOG_DESTINATION_CSVLOG	 8
#define LOG_DESTINATION_JSONLOG	16

/* 其它导出函数 */
extern void log_status_format(StringInfo buf, const char *format,
							  ErrorData *edata);
extern void DebugFileOpen(void);
extern char *unpack_sql_state(int sql_state);
extern bool in_error_recursion_trouble(void);

/* 各日志目标共用的函数 */
extern void reset_formatted_start_time(void);
extern char *get_formatted_start_time(void);
extern char *get_formatted_log_time(void);
extern const char *get_backend_type_for_log(void);
extern bool check_log_of_query(ErrorData *edata);
extern const char *error_severity(int elevel);
extern void write_pipe_chunks(char *data, int len, int dest);

/* 特定于各日志目标的函数 */
extern void write_csvlog(ErrorData *edata);
extern void write_jsonlog(ErrorData *edata);

/*
 * 把错误写入 stderr（或在 stderr 不可用时用等效手段）。
 * 用于 ereport/elog 尚不能安全使用之前（例如内存上下文、
 * GUC 加载等情形）。
 */
extern void write_stderr(const char *fmt,...) pg_attribute_printf(1, 2);
extern void vwrite_stderr(const char *fmt, va_list ap) pg_attribute_printf(1, 0);

#endif							/* ELOG_H */
