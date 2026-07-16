/*-------------------------------------------------------------------------
 *
 * main.c
 *	  postgres 可执行程序的桩 main() 例程。
 *
 * 该函数会为 postgres 的任何形态（postmaster、独立后端进程、
 * 独立引导进程，或是 postmaster 单独 exec 出来的子进程）完成一些
 * 必要的启动任务，然后分派到该形态对应的 FooMain() 例程。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/main/main.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#if defined(WIN32)
#include <crtdbg.h>
#endif

#if defined(__NetBSD__)
#include <sys/param.h>
#endif

#include "bootstrap/bootstrap.h"
#include "common/username.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "tcop/tcopprot.h"
#include "utils/help_config.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/ps_status.h"


const char *progname;
static bool reached_main = false;

/* 用于分派到子程序的、必须排在最前面的特殊选项的名称 */
static const char *const DispatchOptionNames[] =
{
	[DISPATCH_CHECK] = "check",
	[DISPATCH_BOOT] = "boot",
	[DISPATCH_FORKCHILD] = "forkchild",
	[DISPATCH_DESCRIBE_CONFIG] = "describe-config",
	[DISPATCH_SINGLE] = "single",
	/* DISPATCH_POSTMASTER 没有名称 */
};

StaticAssertDecl(lengthof(DispatchOptionNames) == DISPATCH_POSTMASTER,
				 "array length mismatch");

static void startup_hacks(const char *progname);
static void init_locale(const char *categoryname, int category, const char *locale);
static void help(const char *progname);
static void check_root(const char *progname);


/*
 * 任何 Postgres 服务器进程都从这里开始执行。
 */
int
main(int argc, char *argv[])
{
	bool		do_check_root = true;
	DispatchOption dispatch_option = DISPATCH_POSTMASTER;

	reached_main = true;

	/*
	 * 如果当前平台支持，则设置一个处理器，在后端/postmaster
	 * 因致命信号或异常崩溃时被调用。
	 */
#if defined(WIN32)
	pgwin32_install_crashdump_handler();
#endif

	progname = get_progname(argv[0]);

	/*
	 * 平台相关的启动补丁
	 */
	startup_hacks(progname);

	/*
	 * 记录最初传入的 argv[] 数组的物理地址，以备 ps 显示使用。
	 * 在某些平台上，为了设置 ps 的进程标题，必须覆盖 argv[] 的存储区。
	 * 在这种情况下，save_ps_display_args 会创建并返回 argv[] 数组的一份新副本。
	 *
	 * save_ps_display_args 还可能会移动环境变量字符串以腾出额外空间。
	 * 因此应当在启动过程中尽可能早地执行此操作，以避免与那些可能
	 * 保存 getenv() 结果指针的代码产生冲突。
	 */
	argv = save_ps_display_args(argc, argv);

	/*
	 * 启动关键子系统：错误处理与内存管理
	 *
	 * 此后的代码允许使用 elog/ereport，不过消息本地化可能不会
	 * 立即生效，并且在 GUC 设置被加载之前，消息除了 stderr 之外
	 * 不会输出到任何地方。
	 */
	MyProcPid = getpid();
	MemoryContextInit();

	/*
	 * 设置栈深度检查的基准点。（在错误报告机制可用之前
	 * 启用此项没有意义。）
	 */
	(void) set_stack_base();

	/*
	 * 设置区域（locale）信息
	 */
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("postgres"));

	/*
	 * 在 postmaster 中，吸收 LC_COLLATE 和 LC_CTYPE 的环境值。
	 * 各个后端进程稍后会从 pg_database 读取设置来修改它们，
	 * 但 postmaster 无法这样做。如果我们将它们保留为 "C"，
	 * 那么 postmaster 中的消息本地化可能无法正常工作。
	 */
	init_locale("LC_COLLATE", LC_COLLATE, "");
	init_locale("LC_CTYPE", LC_CTYPE, "");

	/*
	 * LC_MESSAGES 会在后续的 GUC 选项处理过程中被设置，但我们在这里
	 * 先行设置它，以便启动阶段的错误消息能够被本地化。
	 */
#ifdef LC_MESSAGES
	init_locale("LC_MESSAGES", LC_MESSAGES, "");
#endif

	/* 我们始终将这些设置为 "C"。相关解释参见 pg_locale.c。 */
	init_locale("LC_MONETARY", LC_MONETARY, "C");
	init_locale("LC_NUMERIC", LC_NUMERIC, "C");
	init_locale("LC_TIME", LC_TIME, "C");

	/*
	 * 既然我们已经从区域环境中吸收了足够的信息，现在移除任何
	 * LC_ALL 设置，以便让 pg_perm_setlocale 安装的环境变量生效。
	 */
	unsetenv("LC_ALL");

	/*
	 * 在大量其他处理之前，先捕获标准选项，特别是要在我们
	 * 强制要求非 root 运行之前进行。
	 */
	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			help(progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			fputs(PG_BACKEND_VERSIONSTR, stdout);
			exit(0);
		}

		/*
		 * 除上述之外，我们还允许 root 用户调用 "--describe-config"
		 * 和 "-C var"。这是相当安全的，因为这些都是只读操作。
		 * -C 的情况很重要，因为 pg_ctl 在 Windows 上可能仍持有
		 * 管理员权限时就会尝试调用它。注意，虽然 -C 通常可以
		 * 出现在任意 argv 位置，但如果你想绕过 root 检查，
		 * 必须将其放在最前面。这样可以降低我们把其他模式的
		 * -C 开关误认为是 postmaster/postgres 的 -C 开关的风险。
		 */
		if (strcmp(argv[1], "--describe-config") == 0)
			do_check_root = false;
		else if (argc > 2 && strcmp(argv[1], "-C") == 0)
			do_check_root = false;
	}

	/*
	 * 确保我们没有以 root 身份运行，除非所选的选项允许这样做。
	 */
	if (do_check_root)
		check_root(progname);

	/*
	 * 根据第一个参数分派到不同的子程序之一。
	 */

	if (argc > 1 && argv[1][0] == '-' && argv[1][1] == '-')
		dispatch_option = parse_dispatch_option(&argv[1][2]);

	switch (dispatch_option)
	{
		case DISPATCH_CHECK:
			BootstrapModeMain(argc, argv, true);
			break;
		case DISPATCH_BOOT:
			BootstrapModeMain(argc, argv, false);
			break;
		case DISPATCH_FORKCHILD:
#ifdef EXEC_BACKEND
			SubPostmasterMain(argc, argv);
#else
			Assert(false);		/* should never happen */
#endif
			break;
		case DISPATCH_DESCRIBE_CONFIG:
			GucInfoMain();
			break;
		case DISPATCH_SINGLE:
			PostgresSingleUserMain(argc, argv,
								   strdup(get_user_name_or_exit(progname)));
			break;
		case DISPATCH_POSTMASTER:
			PostmasterMain(argc, argv);
			break;
	}

	/* 上述函数都不应该返回 */
	abort();
}

/*
 * 返回与给定选项名匹配的 DispatchOption 值。如果找不到匹配，
 * 则返回 DISPATCH_POSTMASTER。
 */
DispatchOption
parse_dispatch_option(const char *name)
{
	for (int i = 0; i < lengthof(DispatchOptionNames); i++)
	{
		/*
		 * 与其他分派选项不同，"forkchild" 带有一个参数，
		 * 因此我们只对这一个查找前缀匹配。对于非 EXEC_BACKEND
		 * 的构建，我们永远不希望返回 DISPATCH_FORKCHILD，
		 * 所以在那种情况下跳过它。
		 */
		if (i == DISPATCH_FORKCHILD)
		{
#ifdef EXEC_BACKEND
			if (strncmp(DispatchOptionNames[DISPATCH_FORKCHILD], name,
						strlen(DispatchOptionNames[DISPATCH_FORKCHILD])) == 0)
				return DISPATCH_FORKCHILD;
#endif
			continue;
		}

		if (strcmp(DispatchOptionNames[i], name) == 0)
			return (DispatchOption) i;
	}

	/* 未找到匹配意味着这是 postmaster */
	return DISPATCH_POSTMASTER;
}

/*
 * 将平台相关的启动补丁放在这里。这是放置必须在任何新服务器进程
 * 启动早期就执行的代码的正确位置。注意，当后端进程或子引导进程
 * 被 fork 出来时，这段代码不会被执行，除非我们处于 fork/exec
 * 环境（即定义了 EXEC_BACKEND）。
 *
 * XXX 这里需要代码，恰恰证明了相关平台过于死板，
 * 无法在缺少辅助的情况下提供标准的 C 执行环境。
 * 如果可以的话，请避免在此处添加更多代码。
 */
static void
startup_hacks(const char *progname)
{
	/*
	 * Windows 特有的执行环境处理。
	 */
#ifdef WIN32
	{
		WSADATA		wsaData;
		int			err;

		/* 默认使输出流不带缓冲 */
		setvbuf(stdout, NULL, _IONBF, 0);
		setvbuf(stderr, NULL, _IONBF, 0);

		/* 准备 Winsock */
		err = WSAStartup(MAKEWORD(2, 2), &wsaData);
		if (err != 0)
		{
			write_stderr("%s: WSAStartup failed: %d\n",
						 progname, err);
			exit(1);
		}

		/*
		 * 默认情况下，abort() 只在 *非* 调试版本中生成崩溃转储。
		 * 由于我们的 Assert() / ExceptionalCondition() 使用了 abort()，
		 * 保留默认值会让调试更加困难。
		 *
		 * MINGW 自带的 C 运行库没有 _set_abort_behavior()。当使用
		 * mingw 针对 Microsoft 的 UCRT 时，它从不链接到库的调试版本，
		 * 因此也不需要调用 _set_abort_behavior()。
		 */
#if !defined(__MINGW32__) && !defined(__MINGW64__)
		_set_abort_behavior(_CALL_REPORTFAULT | _WRITE_ABORT_MSG,
							_CALL_REPORTFAULT | _WRITE_ABORT_MSG);
#endif							/* !defined(__MINGW32__) &&
								 * !defined(__MINGW64__) */

		/*
		 * SEM_FAILCRITICALERRORS 会使更多错误被报告给调用者。
		 *
		 * 我们曾经也指定了 SEM_NOGPFAULTERRORBOX，但那会阻止
		 * Windows 崩溃报告机制工作。其中包括已注册的即时调试器，
		 * 这会让在 Windows 上调试问题变得不必要地困难。现在我们尝试
		 * 在下面分别禁用弹出框的来源（注意 SEM_NOGPFAULTERRORBOX
		 * 实际上并没有阻止此类弹出框的所有来源）。
		 */
		SetErrorMode(SEM_FAILCRITICALERRORS);

		/*
		 * 在 stderr 上显示错误，而不是弹出对话框（注意这不会影响
		 * 源自 C 运行库的错误，见下文）。
		 */
		_set_error_mode(_OUT_TO_STDERR);

		/*
		 * 在 DEBUG 版本中，错误（包括断言）和 C 运行库错误都通过
		 * _CrtDbgReport 报告。默认情况下，此类错误会通过弹出框显示
		 * （即便使用了 NOGPFAULTERRORBOX），从而阻止程序继续运行。
		 * 我们改为将此类错误报告到 stderr（以及调试器）。这是 C 运行库
		 * 特有的行为，因此上面那些设置不足以抑制这些弹出框。
		 */
		_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
		_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
		_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
		_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
		_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
		_CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
	}
#endif							/* WIN32 */
}


/*
 * 为区域类别设置初始的永久值。如果失败（可能是由于环境中
 * LC_foo=invalid），则使用区域 C。如果连这也失败（可能是由于内存不足），
 * 整个启动过程将随之失败。当该函数返回时，我们保证已经为给定
 * 类别的环境变量设置了一个值。
 */
static void
init_locale(const char *categoryname, int category, const char *locale)
{
	if (pg_perm_setlocale(category, locale) == NULL &&
		pg_perm_setlocale(category, "C") == NULL)
		elog(FATAL, "could not adopt \"%s\" locale nor C locale for %s",
			 locale, categoryname);
}



/*
 * 帮助信息的显示应当与 PostmasterMain() 和 PostgresMain()
 * 接受的选项保持一致。
 *
 * XXX 在 Windows 上，这些消息的非 ASCII 本地化版本只有在控制台
 * 输出代码页覆盖所需字符时才能正确显示。write_console() 中
 * 输出的消息不会出现此问题。
 */
static void
help(const char *progname)
{
	printf(_("%s is the PostgreSQL server.\n\n"), progname);
	printf(_("Usage:\n  %s [OPTION]...\n\n"), progname);
	printf(_("Options:\n"));
	printf(_("  -B NBUFFERS        number of shared buffers\n"));
	printf(_("  -c NAME=VALUE      set run-time parameter\n"));
	printf(_("  -C NAME            print value of run-time parameter, then exit\n"));
	printf(_("  -d 1-5             debugging level\n"));
	printf(_("  -D DATADIR         database directory\n"));
	printf(_("  -e                 use European date input format (DMY)\n"));
	printf(_("  -F                 turn fsync off\n"));
	printf(_("  -h HOSTNAME        host name or IP address to listen on\n"));
	printf(_("  -i                 enable TCP/IP connections (deprecated)\n"));
	printf(_("  -k DIRECTORY       Unix-domain socket location\n"));
#ifdef USE_SSL
	printf(_("  -l                 enable SSL connections\n"));
#endif
	printf(_("  -N MAX-CONNECT     maximum number of allowed connections\n"));
	printf(_("  -p PORT            port number to listen on\n"));
	printf(_("  -s                 show statistics after each query\n"));
	printf(_("  -S WORK-MEM        set amount of memory for sorts (in kB)\n"));
	printf(_("  -V, --version      output version information, then exit\n"));
	printf(_("  --NAME=VALUE       set run-time parameter\n"));
	printf(_("  --describe-config  describe configuration parameters, then exit\n"));
	printf(_("  -?, --help         show this help, then exit\n"));

	printf(_("\nDeveloper options:\n"));
	printf(_("  -f s|i|o|b|t|n|m|h forbid use of some plan types\n"));
	printf(_("  -O                 allow system table structure changes\n"));
	printf(_("  -P                 disable system indexes\n"));
	printf(_("  -t pa|pl|ex        show timings after each query\n"));
	printf(_("  -T                 send SIGABRT to all backend processes if one dies\n"));
	printf(_("  -W NUM             wait NUM seconds to allow attach from a debugger\n"));

	printf(_("\nOptions for single-user mode:\n"));
	printf(_("  --single           selects single-user mode (must be first argument)\n"));
	printf(_("  DBNAME             database name (defaults to user name)\n"));
	printf(_("  -d 0-5             override debugging level\n"));
	printf(_("  -E                 echo statement before execution\n"));
	printf(_("  -j                 do not use newline as interactive query delimiter\n"));
	printf(_("  -r FILENAME        send stdout and stderr to given file\n"));

	printf(_("\nOptions for bootstrapping mode:\n"));
	printf(_("  --boot             selects bootstrapping mode (must be first argument)\n"));
	printf(_("  --check            selects check mode (must be first argument)\n"));
	printf(_("  DBNAME             database name (mandatory argument in bootstrapping mode)\n"));
	printf(_("  -r FILENAME        send stdout and stderr to given file\n"));

	printf(_("\nPlease read the documentation for the complete list of run-time\n"
			 "configuration settings and how to set them on the command line or in\n"
			 "the configuration file.\n\n"
			 "Report bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}



static void
check_root(const char *progname)
{
#ifndef WIN32
	if (geteuid() == 0)
	{
		write_stderr("\"root\" execution of the PostgreSQL server is not permitted.\n"
					 "The server must be started under an unprivileged user ID to prevent\n"
					 "possible system security compromise.  See the documentation for\n"
					 "more information on how to properly start the server.\n");
		exit(1);
	}

	/*
	 * 同时，确保真实用户 ID 和有效用户 ID 相同。以 setuid 程序
	 * 的形式从 root shell 执行是一个安全漏洞，因为在许多平台上，
	 * 如果真实 uid 是 root，恶意的子程序可以通过 setuid 重新变回 root。
	 * （由于实际上没有人将 postgres 用作 setuid 程序，主动去修复这种
	 * 情况似乎弊大于利；我们只需花点力气去检查它即可。）
	 */
	if (getuid() != geteuid())
	{
		write_stderr("%s: real and effective user IDs must match\n",
					 progname);
		exit(1);
	}
#else							/* WIN32 */
	if (pgwin32_is_admin())
	{
		write_stderr("Execution of PostgreSQL by a user with administrative permissions is not\n"
					 "permitted.\n"
					 "The server must be started under an unprivileged user ID to prevent\n"
					 "possible system security compromises.  See the documentation for\n"
					 "more information on how to properly start the server.\n");
		exit(1);
	}
#endif							/* WIN32 */
}

/*
 * 至少在 Linux 上，set_ps_display() 会破坏 /proc/$pid/environ。
 * sanitizer 库使用 /proc/$pid/environ 来实现 getenv()，因为它希望
 * 独立于 libc 工作。根据启用了哪些 sanitizer，sanitizer 库可能要
 * 等到我们调用了 set_ps_display() 之后才会被初始化，从而无法让
 * sanitizer 看到由环境提供的选项。
 *
 * 我们可以通过定义 __ubsan_default_options（libsanitizer 用来从
 * 应用程序获取默认值的弱符号）并返回 getenv("UBSAN_OPTIONS") 来
 * 绕过这个问题。但前提是 main 已经到达，这样我们就不会依赖一个
 * 尚不能正常工作的 getenv()。
 *
 * 另一方面，当启用了不同的 sanitizer 时，libsanitizer 可能会
 * 在自身尚未完全初始化时就早早调用本函数，导致递归并在 libsanitizer
 * 内部产生核心转储。为了防止这种情况，请确保本函数在编译时不包含
 * 任何 sanitizer 回调。
 *
 * 由于本函数在未运行 sanitizer 时不会被调用，似乎没有必要
 * 仅在条件满足时才编译它。
 */
const char *__ubsan_default_options(void);

#if __has_attribute(disable_sanitizer_instrumentation)
__attribute__((disable_sanitizer_instrumentation))
#endif
const char *
__ubsan_default_options(void)
{
	/* 在 libc 保证已初始化之前不要调用它 */
	if (!reached_main)
		return "";

	return getenv("UBSAN_OPTIONS");
}
