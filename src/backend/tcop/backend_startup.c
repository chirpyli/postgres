/*-------------------------------------------------------------------------
 *
 * backend_startup.c
 *	  后端启动代码
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/tcop/backend_startup.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "access/xlog.h"
#include "access/xlogrecovery.h"
#include "common/ip.h"
#include "common/string.h"
#include "libpq/libpq.h"
#include "libpq/libpq-be.h"
#include "libpq/pqformat.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "replication/walsender.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/procsignal.h"
#include "storage/proc.h"
#include "tcop/backend_startup.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/guc_hooks.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/timeout.h"
#include "utils/varlena.h"

/* GUC 参数 */
bool		Trace_connection_negotiation = false;
uint32		log_connections = 0;
char	   *log_connections_string = NULL;

/* 其他全局变量 */

/*
 * ConnectionTiming 存储连接建立与初始化过程中各个时间点的时间戳。
 * ready_for_use 在此处初始化为一个特殊值，这样我们就能检查在 PostgresMain()
 * 中设置它之前是否已经设置过。
 */
ConnectionTiming conn_timing = {.ready_for_use = TIMESTAMP_MINUS_INFINITY};

static void BackendInitialize(ClientSocket *client_sock, CAC_state cac);
static int	ProcessSSLStartup(Port *port);
static int	ProcessStartupPacket(Port *port, bool ssl_done, bool gss_done);
static void ProcessCancelRequestPacket(Port *port, void *pkt, int pktlen);
static void SendNegotiateProtocolVersion(List *unrecognized_protocol_options);
static void process_startup_packet_die(SIGNAL_ARGS);
static void StartupPacketTimeoutHandler(void);
static bool validate_log_connections_options(List *elemlist, uint32 *flags);

/*
 * 新后端进程的入口点。
 *
 * 初始化连接、读取启动包、对客户端进行认证，并启动主处理循环。
 */
void
BackendMain(const void *startup_data, size_t startup_data_len)
{
	const BackendStartupData *bsdata = startup_data;

	Assert(startup_data_len == sizeof(BackendStartupData));
	Assert(MyClientSocket != NULL);

#ifdef EXEC_BACKEND

	/*
	 * 需要在后端中重新初始化 SSL 库，因为上下文结构包含函数指针，无法
	 * 通过参数文件传递。
	 *
	 * 如果由于某种原因重新加载失败（也许用户安装了损坏的密钥文件），
	 * 则不使用 SSL 继续运行；这总比所有连接都变得不可能要好。
	 *
	 * XXX 我们应该在所有子进程中都这样做吗？目前只在后端子进程中做就足够了。
	 */
#ifdef USE_SSL
	if (EnableSSL)
	{
		if (secure_initialize(false) == 0)
			LoadedSSL = true;
		else
			ereport(LOG,
					(errmsg("SSL configuration could not be loaded in child process")));
	}
#endif
#endif

	/* 执行额外的初始化并收集启动包 */
	BackendInitialize(MyClientSocket, bsdata->canAcceptConnections);

	/*
	 * 在共享内存中创建一个每个后端独立的 PGPROC 结构体。我们必须先完成
	 * 这一步，才能使用 LWLocks 或访问任何共享内存。
	 */
	InitProcess();

	/*
	 * 确保我们不再处于 PostmasterContext 中。（不过我们还不能删除它，
	 * 因为 InitPostgres 还需要 HBA 数据。）
	 */
	MemoryContextSwitchTo(TopMemoryContext);

	PostgresMain(MyProcPort->database_name, MyProcPort->user_name);
}


/*
 * BackendInitialize -- initialize an interactive (postmaster-child)
 *				backend process, and collect the client's startup packet.
 *
 * returns: nothing.  Will not return at all if there's any failure.
 *
 * Note: this code does not depend on having any access to shared memory.
 * Indeed, our approach to SIGTERM/timeout handling *requires* that
 * shared memory not have been touched yet; see comments within.
 * In the EXEC_BACKEND case, we are physically attached to shared memory
 * but have not yet set up most of our local pointers to shmem structures.
 */
static void
BackendInitialize(ClientSocket *client_sock, CAC_state cac)
{
	int			status;
	int			ret;
	Port	   *port;
	char		remote_host[NI_MAXHOST];
	char		remote_port[NI_MAXSERV];
	StringInfoData ps_data;
	MemoryContext oldcontext;

	/* 通知 fd.c 与 client_sock 相关的长生命周期 FD */
	ReserveExternalFD();

	/*
	 * PreAuthDelay 是一个用于调查认证周期中问题的调试辅助手段：它可以在
	 * postgresql.conf 中设置，以便留出时间用调试器附加到新 fork 出的后端。
	 * （另见 PostAuthDelay，我们允许客户端通过 PGOPTIONS 传入，但它直到
	 * 认证之后才会生效。）
	 */
	if (PreAuthDelay > 0)
		pg_usleep(PreAuthDelay * 1000000L);

	/* 该标志将一直保持设置，直到 InitPostgres 完成认证 */
	ClientAuthInProgress = true;	/* 限制日志消息的可见性 */

	/*
	 * 初始化 libpq，并启用将 ereport 错误报告给客户端。现在必须这样做，
	 * 因为认证会使用 libpq 来发送消息。
	 *
	 * Port 结构体以及所有附着于它的数据结构都在 TopMemoryContext 中分配，
	 * 以便它们能存活到 PostgresMain 执行期间。我们无需担心失败时泄漏这块
	 * 内存，因为我们已经不在 postmaster 进程中了。
	 */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	port = MyProcPort = pq_init(client_sock);
	MemoryContextSwitchTo(oldcontext);

	whereToSendOutput = DestRemote; /* 现在向客户端报告 ereport 是安全的 */

	/* 将它们设为空，以防在正式设置之前就需要用到 */
	port->remote_host = "";
	port->remote_port = "";

	/*
	 * 我们安排在尝试收集启动包期间，如果收到 SIGTERM 或超时，就执行
	 * _exit(1)；而 SIGQUIT 则导致 _exit(2)。否则，如果某个有缺陷的客户端
	 * 未能及时发送数据包，postmaster 将无法干净地以 FAST 或 IMMED 方式
	 * 关闭数据库。
	 *
	 * 能够用 _exit(1) 退出，仅仅是因为我们尚未触碰共享内存；因此不需要
	 * 清理任何进程外部的状态。
	 */
	pqsignal(SIGTERM, process_startup_packet_die);
	/* SIGQUIT 处理函数已由 InitPostmasterChild 设置好 */
	InitializeTimeouts();		/* 建立 SIGALRM 处理函数 */
	sigprocmask(SIG_SETMASK, &StartupBlockSig, NULL);

	/*
	 * 获取远程主机名和端口，用于日志和状态显示。
	 */
	remote_host[0] = '\0';
	remote_port[0] = '\0';
	if ((ret = pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen,
								  remote_host, sizeof(remote_host),
								  remote_port, sizeof(remote_port),
								  (log_hostname ? 0 : NI_NUMERICHOST) | NI_NUMERICSERV)) != 0)
		ereport(WARNING,
				(errmsg_internal("pg_getnameinfo_all() failed: %s",
								 gai_strerror(ret))));

	/*
	 * 将 remote_host 和 remote_port 保存到 port 结构体中（此后，它们会
	 * 出现在日志消息的 log_line_prefix 数据中）。
	 */
	port->remote_host = MemoryContextStrdup(TopMemoryContext, remote_host);
	port->remote_port = MemoryContextStrdup(TopMemoryContext, remote_port);

	/* 现在，如果启用了相关选项，就可以记录连接已收到的日志 */
	if (log_connections & LOG_CONNECTION_RECEIPT)
	{
		if (remote_port[0])
			ereport(LOG,
					(errmsg("connection received: host=%s port=%s",
							remote_host,
							remote_port)));
		else
			ereport(LOG,
					(errmsg("connection received: host=%s",
							remote_host)));
	}

	/* 用于测试客户端的错误处理 */
#ifdef USE_INJECTION_POINTS
	INJECTION_POINT("backend-initialize", NULL);
	if (IS_INJECTION_POINT_ATTACHED("backend-initialize-v2-error"))
	{
		/*
		 * 这模拟了 v14 之前的服务器的早期错误。那时的服务器在处理启动包
		 * 之前发生的任何错误都使用版本 2 协议。
		 */
		FrontendProtocol = PG_PROTOCOL(2, 0);
		elog(FATAL, "protocol version 2 error triggered");
	}
#endif

	/*
	 * 如果我们做了反向查找得到名称，不妨把结果保存下来，以免在认证期间
	 * 可能重复查找。
	 *
	 * 注意，我们不希望在上面指定 NI_NAMEREQD，因为那样对于没有 rDNS 记录
	 * 的客户端我们会什么都得不到。因此，我们必须检查得到的是否为数字形式
	 * 的 IPv4 或 IPv6 地址，如果是，则不要保存到 remote_hostname 中。
	 * （这个测试比较保守，有时可能把主机名误判为数字形式，但这个方向上的
	 * 误判是安全的；它最多只是导致一次可能的额外查找。）
	 */
	if (log_hostname &&
		ret == 0 &&
		strspn(remote_host, "0123456789.") < strlen(remote_host) &&
		strspn(remote_host, "0123456789ABCDEFabcdef:") < strlen(remote_host))
	{
		port->remote_hostname = MemoryContextStrdup(TopMemoryContext, remote_host);
	}

	/*
	 * 准备开始与客户端交互。我们会在一段延时后放弃并执行 _exit(1)，以免
	 * 有缺陷的客户端无限期地占用一个连接。上面的 PreAuthDelay 以及任何
	 * DNS 交互不计入该时间限制。
	 *
	 * 注意：AuthenticationTimeout 在等待启动包时于此应用，然后在 InitPostgres
	 * 中针对任何认证操作的持续时间再次应用。因此，一个恶意客户端在我们将其
	 * 踢掉之前，可能将进程占用接近两倍的 AuthenticationTimeout 时间。
	 *
	 * 注意：由于 PostgresMain 会再次调用 InitializeTimeouts，STARTUP_PACKET_TIMEOUT
	 * 的注册会丢失。这没关系，因为本函数之后我们不会再使用它。
	 */
	RegisterTimeout(STARTUP_PACKET_TIMEOUT, StartupPacketTimeoutHandler);
	enable_timeout_after(STARTUP_PACKET_TIMEOUT, AuthenticationTimeout * 1000);

	/* 处理直接的 SSL 握手 */
	status = ProcessSSLStartup(port);

	/*
	 * 接收启动包（它最终可能是一个取消请求包）。
	 */
	if (status == STATUS_OK)
		status = ProcessStartupPacket(port, false, false);

	/*
	 * 如果我们要因数据库状态而拒绝连接，现在就说明，而不是把时间浪费在
	 * 认证交互上。（这也使得可以编写一个 pg_ping 工具。）
	 */
	if (status == STATUS_OK)
	{
		switch (cac)
		{
			case CAC_STARTUP:
				ereport(FATAL,
						(errcode(ERRCODE_CANNOT_CONNECT_NOW),
						 errmsg("the database system is starting up")));
				break;
			case CAC_NOTHOTSTANDBY:
				if (!EnableHotStandby)
					ereport(FATAL,
							(errcode(ERRCODE_CANNOT_CONNECT_NOW),
							 errmsg("the database system is not accepting connections"),
							 errdetail("Hot standby mode is disabled.")));
				else if (reachedConsistency)
					ereport(FATAL,
							(errcode(ERRCODE_CANNOT_CONNECT_NOW),
							 errmsg("the database system is not yet accepting connections"),
							 errdetail("Recovery snapshot is not yet ready for hot standby."),
							 errhint("To enable hot standby, close write transactions with more than %d subtransactions on the primary server.",
									 PGPROC_MAX_CACHED_SUBXIDS)));
				else
					ereport(FATAL,
							(errcode(ERRCODE_CANNOT_CONNECT_NOW),
							 errmsg("the database system is not yet accepting connections"),
							 errdetail("Consistent recovery state has not been yet reached.")));
				break;
			case CAC_SHUTDOWN:
				ereport(FATAL,
						(errcode(ERRCODE_CANNOT_CONNECT_NOW),
						 errmsg("the database system is shutting down")));
				break;
			case CAC_RECOVERY:
				ereport(FATAL,
						(errcode(ERRCODE_CANNOT_CONNECT_NOW),
						 errmsg("the database system is in recovery mode")));
				break;
			case CAC_TOOMANY:
				ereport(FATAL,
						(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
						 errmsg("sorry, too many clients already")));
				break;
			case CAC_OK:
				break;
		}
	}

	/*
	 * 禁用超时，并再次阻止 SIGTERM。
	 */
	disable_timeout(STARTUP_PACKET_TIMEOUT, false);
	sigprocmask(SIG_SETMASK, &BlockSig, NULL);

	/*
	 * 作为一项安全检查，确认启动过程中尚未执行任何共享内存修改（如果之前
	 * 通过上面的 SIGTERM 或超时退出，这些修改需要被撤销），检查是否还没有
	 * 注册任何 on_shmem_exit 处理函数。（这并非完全可靠，因为有人可能误用
	 * on_proc_exit 处理函数来清理共享内存，但这是一个成本低且有帮助的检查。
	 * 我们不幸无法禁止 on_proc_exit 处理函数，因为 pq_init() 已经注册了一个。）
	 */
	check_on_shmem_exit_lists_are_empty();

	/*
	 * 如果它是一个错误包或取消包，则在此停止。ProcessStartupPacket
	 * 已经完成了任何适当的错误报告。
	 */
	if (status != STATUS_OK)
		proc_exit(0);

	/*
	 * 既然我们已经拿到了用户名和数据库名，就可以设置用于 ps 的进程标题了。
	 * 在启动过程中尽早这样做是好的。
	 */
	initStringInfo(&ps_data);
	if (am_walsender)
		appendStringInfo(&ps_data, "%s ", GetBackendTypeDesc(B_WAL_SENDER));
	appendStringInfo(&ps_data, "%s ", port->user_name);
	if (port->database_name[0] != '\0')
		appendStringInfo(&ps_data, "%s ", port->database_name);
	appendStringInfoString(&ps_data, port->remote_host);
	if (port->remote_port[0] != '\0')
		appendStringInfo(&ps_data, "(%s)", port->remote_port);

	init_ps_display(ps_data.data);
	pfree(ps_data.data);

	set_ps_display("initializing");
}

/*
 * 检查直接的 SSL 连接。
 *
 * 这发生在启动包之前，因此我们要小心：如果它不是直接的 SSL 连接，就
 * 不要真正从流中读取任何字节。
 */
static int
ProcessSSLStartup(Port *port)
{
	int			firstbyte;

	Assert(!port->ssl_in_use);

	pq_startmsgread();
	firstbyte = pq_peekbyte();
	pq_endmsgread();
	if (firstbyte == EOF)
	{
		/*
		 * 与 ProcessStartupPacket 中一样，如果我们完全没有收到任何数据，
		 * 不要往日志里塞一条抱怨信息。
		 */
		return STATUS_ERROR;
	}

	if (firstbyte != 0x16)
	{
		/* 不是 SSL 握手消息 */
		return STATUS_OK;
	}

	/*
	 * 首字节表示标准的 SSL 握手消息
	 *
	 * （它不可能是 Postgres 启动长度，因为在网络字节序下那会是一个长达
	 * 数百兆字节的启动包）
	 */

#ifdef USE_SSL
	if (!LoadedSSL || port->laddr.addr.ss_family == AF_UNIX)
	{
		/* 不支持 SSL */
		goto reject;
	}

	if (secure_open_server(port) == -1)
	{
		/*
		 * 我们假设 secure_open_server() 已经发送了适当的 TLS 告警
		 */
		goto reject;
	}
	Assert(port->ssl_in_use);

	if (!port->alpn_used)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("received direct SSL connection request without ALPN protocol negotiation extension")));
		goto reject;
	}

	if (Trace_connection_negotiation)
		ereport(LOG,
				(errmsg("direct SSL connection accepted")));
	return STATUS_OK;
#else
	/* 此构建不支持 SSL */
	goto reject;
#endif

reject:
	if (Trace_connection_negotiation)
		ereport(LOG,
				(errmsg("direct SSL connection rejected")));
	return STATUS_ERROR;
}

/*
 * 读取客户端的启动包，并根据其内容采取相应的动作。
 *
 * 返回 STATUS_OK 或 STATUS_ERROR，也可能会调用 ereport(FATAL) 而根本不返回。
 *
 * （注意：ereport(FATAL) 的内容会发送给客户端，所以只有在你确实希望如此时
 * 才使用它。如果你不想向客户端发送任何内容（在检测到通信失败时通常是合适的），
 * 则返回 STATUS_ERROR。）
 *
 * 当加密层（目前为 TLS 或 GSSAPI）的协商完成时，设置 ssl_done 和/或
 * gss_done。任一加密层的成功协商都会设置这两个标志，但被拒绝的协商只设置
 * 该层的标志，因为客户端可能希望尝试另一个。我们不应在此对客户端的
 * 请求顺序做任何假设。
 */
static int
ProcessStartupPacket(Port *port, bool ssl_done, bool gss_done)
{
	int32		len;
	char	   *buf;
	ProtocolVersion proto;
	MemoryContext oldcontext;

retry:
	pq_startmsgread();

	/*
	 * 单独读取长度字段的首字节，这样我们就能判断是根本没有数据，还是
	 * 一个不完整的包。（这听起来可能效率不高，但实际上并非如此，因为
	 * pqcomm.c 中有缓冲。）
	 */
	if (pq_getbytes(&len, 1) == EOF)
	{
		/*
		 * 如果我们完全没有收到任何数据，不要往日志里塞一条抱怨信息；
		 * 这类情况常常出于正当原因发生。例如，我们可能是在响应
		 * NEGOTIATE_SSL_CODE 之后来到这里的，如果客户端不喜欢我们的响应，
		 * 它很可能只是直接断开连接。服务监控软件也常常只是打开再关闭一个
		 * 连接而不发送任何内容。（端口扫描器也是如此，它们可能不那么善意，
		 * 但发现它们并不是我们真正的工作。）
		 */
		return STATUS_ERROR;
	}

	if (pq_getbytes(((char *) &len) + 1, 3) == EOF)
	{
		/* 只收到了长度字段的一部分，因此报告该问题 */
		if (!ssl_done && !gss_done)
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("incomplete startup packet")));
		return STATUS_ERROR;
	}

	len = pg_ntoh32(len);
	len -= 4;

	if (len < (int32) sizeof(ProtocolVersion) ||
		len > MAX_STARTUP_PACKET_LENGTH)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid length of startup packet")));
		return STATUS_ERROR;
	}

	/*
	 * 分配保存启动包的空间，外加一个额外字节并初始化为零。这确保包内
	 * 所有字符串都以空字符结尾。
	 */
	buf = palloc(len + 1);
	buf[len] = '\0';

	if (pq_getbytes(buf, len) == EOF)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("incomplete startup packet")));
		return STATUS_ERROR;
	}
	pq_endmsgread();

	/*
	 * 第一个字段要么是一个协议版本号，要么是一个特殊的请求码。
	 */
	port->proto = proto = pg_ntoh32(*((ProtocolVersion *) buf));

	if (proto == CANCEL_REQUEST_CODE)
	{
		ProcessCancelRequestPacket(port, buf, len);
		/* 这其实不算错误，但我们不想再继续处理 */
		return STATUS_ERROR;
	}

	if (proto == NEGOTIATE_SSL_CODE && !ssl_done)
	{
		char		SSLok;

#ifdef USE_SSL

		/*
		 * 在禁用或 Unix 套接字上时不进行 SSL 协商。
		 *
		 * 如果我们已经有了一个直接的 SSL 连接，也不再协商 SSL。
		 */
		if (!LoadedSSL || port->laddr.addr.ss_family == AF_UNIX || port->ssl_in_use)
			SSLok = 'N';
		else
			SSLok = 'S';		/* 支持 SSL */
#else
		SSLok = 'N';			/* 不支持 SSL */
#endif

		if (Trace_connection_negotiation)
		{
			if (SSLok == 'S')
				ereport(LOG,
						(errmsg("SSLRequest accepted")));
			else
				ereport(LOG,
						(errmsg("SSLRequest rejected")));
		}

		while (secure_write(port, &SSLok, 1) != 1)
		{
			if (errno == EINTR)
				continue;		/* 如果被中断，直接重试 */
			ereport(COMMERROR,
					(errcode_for_socket_access(),
					 errmsg("failed to send SSL negotiation response: %m")));
			return STATUS_ERROR;	/* 关闭连接 */
		}

#ifdef USE_SSL
		if (SSLok == 'S' && secure_open_server(port) == -1)
			return STATUS_ERROR;
#endif

		/*
		 * 此时我们不应该有任何已缓冲的数据。如果有，那是在我们执行 SSL
		 * 握手之前收到的，因此它未被加密，并且确实可能由中间人注入。
		 * 我们将这种情况报告给客户端。
		 */
		if (pq_buffer_remaining_data() > 0)
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("received unencrypted data after SSL request"),
					 errdetail("This could be either a client-software bug or evidence of an attempted man-in-the-middle attack.")));

		/*
		 * 接下来应该是普通的启动包、取消包等，但不应是另一个 SSL 协商请求；
		 * 而 GSS 请求只有在 SSL 被拒绝时才应跟随其后（客户端可以按任意顺序
		 * 进行协商）
		 */
		ssl_done = true;
		if (SSLok == 'S')
		{
			/*
			 * 我们已完成 SSL 协商且协商正确，因此对 GSS 也做同样处理。
			 */
			gss_done = true;
		}
		goto retry;
	}
	else if (proto == NEGOTIATE_GSS_CODE && !gss_done)
	{
		char		GSSok = 'N';

#ifdef ENABLE_GSS
		/* 在 Unix 套接字上不进行 GSSAPI 加密 */
		if (port->laddr.addr.ss_family != AF_UNIX)
			GSSok = 'G';
#endif

		if (Trace_connection_negotiation)
		{
			if (GSSok == 'G')
				ereport(LOG,
						(errmsg("GSSENCRequest accepted")));
			else
				ereport(LOG,
						(errmsg("GSSENCRequest rejected")));
		}

		while (secure_write(port, &GSSok, 1) != 1)
		{
			if (errno == EINTR)
				continue;
			ereport(COMMERROR,
					(errcode_for_socket_access(),
					 errmsg("failed to send GSSAPI negotiation response: %m")));
			return STATUS_ERROR;	/* 关闭连接 */
		}

#ifdef ENABLE_GSS
		if (GSSok == 'G' && secure_open_gssapi(port) == -1)
			return STATUS_ERROR;
#endif

		/*
		 * 此时我们不应该有任何已缓冲的数据。如果有，那是在我们执行 GSS
		 * 握手之前收到的，因此它未被加密，并且确实可能由中间人注入。
		 * 我们将这种情况报告给客户端。
		 */
		if (pq_buffer_remaining_data() > 0)
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("received unencrypted data after GSSAPI encryption request"),
					 errdetail("This could be either a client-software bug or evidence of an attempted man-in-the-middle attack.")));

		/*
		 * 接下来应该是普通的启动包、取消包等，但不应是另一个 GSS 协商请求；
		 * 而 SSL 请求只有在 GSS 被拒绝时才应跟随其后（客户端可以按任意顺序
		 * 进行协商）
		 */
		gss_done = true;
		if (GSSok == 'G')
		{
			/*
			 * 我们已完成 GSS 协商且协商正确，因此对 SSL 也做同样处理。
			 */
			ssl_done = true;
		}
		goto retry;
	}

	/* 可以在此处添加额外的特殊包类型 */

	/*
	 * 现在设置 FrontendProtocol，这样 ereport() 就能知道如果我们在启动期间
	 * 失败应以何种格式发送。我们使用客户端请求的协议版本，除非它高于我们
	 * 支持的最新版本。新协议版本中的错误消息字段可能看起来不同，但那些
	 * 新客户端应该能够处理这一点。
	 */
	FrontendProtocol = Min(proto, PG_PROTOCOL_LATEST);

	/* 检查主协议版本是否在范围内。 */
	if (PG_PROTOCOL_MAJOR(proto) < PG_PROTOCOL_MAJOR(PG_PROTOCOL_EARLIEST) ||
		PG_PROTOCOL_MAJOR(proto) > PG_PROTOCOL_MAJOR(PG_PROTOCOL_LATEST))
		ereport(FATAL,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("unsupported frontend protocol %u.%u: server supports %u.0 to %u.%u",
						PG_PROTOCOL_MAJOR(proto), PG_PROTOCOL_MINOR(proto),
						PG_PROTOCOL_MAJOR(PG_PROTOCOL_EARLIEST),
						PG_PROTOCOL_MAJOR(PG_PROTOCOL_LATEST),
						PG_PROTOCOL_MINOR(PG_PROTOCOL_LATEST))));

	/*
	 * 现在从启动包中取出参数并保存到 Port 结构体中。
	 */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	/* 处理协议版本 3 的启动包 */
	{
		int32		offset = sizeof(ProtocolVersion);
		List	   *unrecognized_protocol_options = NIL;

		/*
		 * 扫描包体中的名称/选项对。由于上面将额外字节清零了，我们可以假设
		 * 包体内开始的任何字符串都以空字符结尾。
		 */
		port->guc_options = NIL;

		while (offset < len)
		{
			char	   *nameptr = buf + offset;
			int32		valoffset;
			char	   *valptr;

			if (*nameptr == '\0')
				break;			/* 找到了包终止符 */
			valoffset = offset + strlen(nameptr) + 1;
			if (valoffset >= len)
				break;			/* 缺少值，将在下面报错 */
			valptr = buf + valoffset;

			if (strcmp(nameptr, "database") == 0)
				port->database_name = pstrdup(valptr);
			else if (strcmp(nameptr, "user") == 0)
				port->user_name = pstrdup(valptr);
			else if (strcmp(nameptr, "options") == 0)
				port->cmdline_options = pstrdup(valptr);
			else if (strcmp(nameptr, "replication") == 0)
			{
			/*
			 * 出于向后兼容的考虑，replication 参数是一个混合体，允许其值为
			 * 布尔值或字符串 'database'。后者会连接到一个特定的数据库，
			 * 例如逻辑解码就需要这样做。
			 */
				if (strcmp(valptr, "database") == 0)
				{
					am_walsender = true;
					am_db_walsender = true;
				}
				else if (!parse_bool(valptr, &am_walsender))
					ereport(FATAL,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for parameter \"%s\": \"%s\"",
									"replication",
									valptr),
							 errhint("Valid values are: \"false\", 0, \"true\", 1, \"database\".")));
			}
			else if (strncmp(nameptr, "_pq_.", 5) == 0)
			{
			/*
			 * 任何以 _pq_. 开头的选项都保留用作协议级选项，但目前尚未定义
			 * 此类选项。
			 */
				unrecognized_protocol_options =
					lappend(unrecognized_protocol_options, pstrdup(nameptr));
			}
			else
			{
				/* 假定它是一个通用的 GUC 选项 */
				port->guc_options = lappend(port->guc_options,
											pstrdup(nameptr));
				port->guc_options = lappend(port->guc_options,
											pstrdup(valptr));

			/*
			 * 如果遇到 application_name，则将其复制到 port 中。这样做是为了
			 * 能在连接授权消息中记录 application_name。注意，GUC 本应被使用，
			 * 但我们尚未经过 GUC 设置阶段。
			 */
				if (strcmp(nameptr, "application_name") == 0)
				{
					port->application_name = pg_clean_ascii(valptr, 0);
				}
			}
			offset = valoffset + strlen(valptr) + 1;
		}

		/*
		 * 如果我们没有在给定包长度的末尾正好找到包终止符，则报错。
		 */
		if (offset != len - 1)
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid startup packet layout: expected terminator as last byte")));

		/*
		 * 如果客户端请求了更新的协议版本，或者客户端请求了我们不认识的
		 * 任何协议选项，就让它知道我们确实支持的最新次协议版本，以及任何
		 * 未被识别的选项的名称。
		 */
		if (PG_PROTOCOL_MINOR(proto) > PG_PROTOCOL_MINOR(PG_PROTOCOL_LATEST) ||
			unrecognized_protocol_options != NIL)
			SendNegotiateProtocolVersion(unrecognized_protocol_options);
	}

	/* 检查是否提供了用户名。 */
	if (port->user_name == NULL || port->user_name[0] == '\0')
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
				 errmsg("no PostgreSQL user name specified in startup packet")));

	/* 数据库名默认为用户名。 */
	if (port->database_name == NULL || port->database_name[0] == '\0')
		port->database_name = pstrdup(port->user_name);

	/*
	 * 将给出的数据库名和用户名截断到 Postgres 名称的长度。这样可以避免
	 * 在给出超长名称时出现查找失败。
	 */
	if (strlen(port->database_name) >= NAMEDATALEN)
		port->database_name[NAMEDATALEN - 1] = '\0';
	if (strlen(port->user_name) >= NAMEDATALEN)
		port->user_name[NAMEDATALEN - 1] = '\0';

	if (am_walsender)
		MyBackendType = B_WAL_SENDER;
	else
		MyBackendType = B_BACKEND;

	/*
	 * 普通的 walsender 后端（例如用于流复制的）不连接到特定的数据库。
	 * 但用于逻辑复制的 walsender 需要连接到一个特定的数据库。我们允许在
	 * 连接到数据库的情况下也发出流复制命令，因为先做一次基础备份、再从
	 * 该备份开始流式传输变更是合理的。
	 */
	if (am_walsender && !am_db_walsender)
		port->database_name[0] = '\0';

	/*
	 * 完成 Port 结构体的填充
	 */
	MemoryContextSwitchTo(oldcontext);

	return STATUS_OK;
}

/*
 * 客户端发送的是一个取消请求包，而不是一个普通的“启动新连接”包。
 * 执行必要的处理。不会向客户端回送任何内容。
 */
static void
ProcessCancelRequestPacket(Port *port, void *pkt, int pktlen)
{
	CancelRequestPacket *canc;
	int			len;

	if (pktlen < offsetof(CancelRequestPacket, cancelAuthCode))
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid length of cancel request packet")));
		return;
	}
	len = pktlen - offsetof(CancelRequestPacket, cancelAuthCode);
	if (len == 0 || len > 256)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid length of cancel key in cancel request packet")));
		return;
	}

	canc = (CancelRequestPacket *) pkt;
	SendCancelRequest(pg_ntoh32(canc->backendPID), canc->cancelAuthCode, len);
}

/*
 * 向客户端发送一个 NegotiateProtocolVersion。它让客户端知道，它要么请求了
 * 一个比我们所能支持的更新的次协议版本，要么请求了至少一个我们不理解的
 * 协议选项，或者两者皆有。FrontendProtocol 已经被设置为客户端请求的版本
 * 与我们已知如何支持的最高版本中较旧的那一个。如果我们已知的最高版本对
 * 客户端来说太旧，它可以放弃该连接。
 *
 * 我们还在响应中包含一份我们不理解的协议选项列表。这样客户端就可以包含
 * 可能出现在更新协议版本或第三方协议扩展中的可选参数，而不必担心如果那些
 * 选项不被理解就不得不重新连接；同时又能确保客户端知晓哪些选项真正被
 * 接受了。
 */
static void
SendNegotiateProtocolVersion(List *unrecognized_protocol_options)
{
	StringInfoData buf;
	ListCell   *lc;

	pq_beginmessage(&buf, PqMsg_NegotiateProtocolVersion);
	pq_sendint32(&buf, FrontendProtocol);
	pq_sendint32(&buf, list_length(unrecognized_protocol_options));
	foreach(lc, unrecognized_protocol_options)
		pq_sendstring(&buf, lfirst(lc));
	pq_endmessage(&buf);

	/* 无需刷新，后续会有其他消息 */
}


/*
 * 在处理启动包期间收到 SIGTERM。
 *
 * 在信号处理函数中运行 proc_exit() 是相当不安全的。不过，由于我们尚未
 * 触碰共享内存，可以直接拔掉插头并退出，而不运行任何 atexit 处理函数。
 *
 * 有人可能会想尝试发送一条消息或记录一条日志，说明我们为何断开连接。
 * 然而，这本身也是相当不安全的。此外，向尚未完成认证、甚至可能还没有
 * 发送启动包的客户端透露数据库状态的线索，似乎也不可取。
 */
static void
process_startup_packet_die(SIGNAL_ARGS)
{
	_exit(1);
}

/*
 * 在处理启动包期间超时。与 process_startup_packet_die() 一样，我们通过
 * _exit(1) 退出。
 */
static void
StartupPacketTimeoutHandler(void)
{
	_exit(1);
}

/*
 * log_connections GUC 检查钩子的辅助函数。
 *
 * `elemlist` 是传给 log_connections GUC 检查钩子 check_log_connections()
 * 的字符串输入经列表化后的版本。check_log_connections() 负责清理 `elemlist`。
 *
 * validate_log_connections_options() 在遇到错误且无法验证 GUC 输入时返回
 * false，否则返回 true。
 *
 * `flags` 返回应由其 assign 钩子存储到 log_connections GUC 中的标志位。
 */
static bool
validate_log_connections_options(List *elemlist, uint32 *flags)
{
	ListCell   *l;
	char	   *item;

	/*
	 * 出于向后兼容，我们单独接受这些标记。
	 *
	 * 在 PostgreSQL 18 之前，log_connections 是一个布尔型 GUC，接受 'true'、
	 * 'false'、'yes'、'no'、'on' 和 'off' 中任何无歧义的子串。由于
	 * log_connections 在 18 中变成了字符串列表，我们现在只接受完整的选项
	 * 字符串。
	 */
	static const struct config_enum_entry compat_options[] = {
		{"off", 0},
		{"false", 0},
		{"no", 0},
		{"0", 0},
		{"on", LOG_CONNECTION_ON},
		{"true", LOG_CONNECTION_ON},
		{"yes", LOG_CONNECTION_ON},
		{"1", LOG_CONNECTION_ON},
	};

	*flags = 0;

	/* 如果传入了空字符串，我们就完成了 */
	if (list_length(elemlist) == 0)
		return true;

	/*
	 * 现在检查向后兼容选项。它们必须始终单独指定，因此如果第一个选项是
	 * 一个向后兼容选项，同时又指定了其他选项，我们就报错。
	 */
	item = linitial(elemlist);

	for (size_t i = 0; i < lengthof(compat_options); i++)
	{
		struct config_enum_entry option = compat_options[i];

		if (pg_strcasecmp(item, option.name) != 0)
			continue;

		if (list_length(elemlist) > 1)
		{
			GUC_check_errdetail("Cannot specify log_connections option \"%s\" in a list with other options.",
								item);
			return false;
		}

		*flags = option.val;
		return true;
	}

	/* 现在检查各项（aspect）选项。空字符串已在前面处理过 */
	foreach(l, elemlist)
	{
		static const struct config_enum_entry options[] = {
			{"receipt", LOG_CONNECTION_RECEIPT},
			{"authentication", LOG_CONNECTION_AUTHENTICATION},
			{"authorization", LOG_CONNECTION_AUTHORIZATION},
			{"setup_durations", LOG_CONNECTION_SETUP_DURATIONS},
			{"all", LOG_CONNECTION_ALL},
		};

		item = lfirst(l);
		for (size_t i = 0; i < lengthof(options); i++)
		{
			struct config_enum_entry option = options[i];

			if (pg_strcasecmp(item, option.name) == 0)
			{
				*flags |= option.val;
				goto next;
			}
		}

		GUC_check_errdetail("Invalid option \"%s\".", item);
		return false;

next:	;
	}

	return true;
}


/*
 * log_connections 的 GUC 检查钩子
 */
bool
check_log_connections(char **newval, void **extra, GucSource source)
{
	uint32		flags;
	char	   *rawstring;
	List	   *elemlist;
	bool		success;

	/* 需要一份可修改的字符串副本 */
	rawstring = pstrdup(*newval);

	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		GUC_check_errdetail("Invalid list syntax in parameter \"%s\".", "log_connections");
		pfree(rawstring);
		list_free(elemlist);
		return false;
	}

	/* 验证逻辑全部在辅助函数中 */
	success = validate_log_connections_options(elemlist, &flags);

	/* 清理时间到 */
	pfree(rawstring);
	list_free(elemlist);

	if (!success)
		return false;

	/*
	 * 我们成功了，因此分配 `extra` 并将标志位保存到其中，供
	 * assign_log_connections() 使用。
	 */
	*extra = guc_malloc(LOG, sizeof(int));
	if (!*extra)
		return false;
	*((int *) *extra) = flags;

	return true;
}

/*
 * log_connections 的 GUC 赋值钩子
 */
void
assign_log_connections(const char *newval, void *extra)
{
	log_connections = *((int *) extra);
}
