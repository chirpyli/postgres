/*
 *	PostgreSQL 针对 INET 和 CIDR 类型的定义。
 *
 *	src/backend/utils/adt/network.c
 *
 *	Jon Postel RIP 16 Oct 1998
 */

#include "postgres.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "access/stratnum.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "common/ip.h"
#include "lib/hyperloglog.h"
#include "libpq/libpq-be.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/inet.h"
#include "utils/lsyscache.h"
#include "utils/sortsupport.h"


/*
 * IPv4 的子网掩码尺寸是 0-32 之间的一个值，在 inet/cidr 缩写键中（尽可能）
 * 用 6 位表示。
 *
 * IPv4 的 inet/cidr 缩写键最多可用 25 位表示子网部分。
 */
#define ABBREV_BITS_INET4_NETMASK_SIZE	6
#define ABBREV_BITS_INET4_SUBNET		25

/* inet/cidr 的 sortsupport */
typedef struct
{
	int64		input_count;	/* 见到的非空值个数 */
	bool		estimating;		/* 如果正在估计基数则为 true */

	hyperLogLogState abbr_card; /* 基数估计器 */
} network_sortsupport_state;

static int32 network_cmp_internal(inet *a1, inet *a2);
static int	network_fast_cmp(Datum x, Datum y, SortSupport ssup);
static bool network_abbrev_abort(int memtupcount, SortSupport ssup);
static Datum network_abbrev_convert(Datum original, SortSupport ssup);
static List *match_network_function(Node *leftop,
									Node *rightop,
									int indexarg,
									Oid funcid,
									Oid opfamily);
static List *match_network_subset(Node *leftop,
								  Node *rightop,
								  bool is_eq,
								  Oid opfamily);
static bool addressOK(unsigned char *a, int bits, int family);
static inet *internal_inetpl(inet *ip, int64 addend);


/*
 * 通用的 INET/CIDR 输入例程
 */
static inet *
network_in(char *src, bool is_cidr, Node *escontext)
{
	int			bits;
	inet	   *dst;

	dst = (inet *) palloc0(sizeof(inet));

	/*
	 * 首先，检查这是 IPv6 还是 IPv4 地址。IPv6 地址中会在某处（实际上是多处）
	 * 出现 :，因此如果存在冒号，就假定为 V6，否则假定为 V4。
	 */

	if (strchr(src, ':') != NULL)
		ip_family(dst) = PGSQL_AF_INET6;
	else
		ip_family(dst) = PGSQL_AF_INET;

	bits = pg_inet_net_pton(ip_family(dst), src, ip_addr(dst),
							is_cidr ? ip_addrsize(dst) : -1);
	if ((bits < 0) || (bits > ip_maxbits(dst)))
		ereturn(escontext, NULL,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
		/* translator: first %s is inet or cidr */
				 errmsg("invalid input syntax for type %s: \"%s\"",
						is_cidr ? "cidr" : "inet", src)));

	/*
	 * 错误检查：CIDR 值不得在掩码长度之外设置任何位。
	 */
	if (is_cidr)
	{
		if (!addressOK(ip_addr(dst), bits, ip_family(dst)))
			ereturn(escontext, NULL,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid cidr value: \"%s\"", src),
					 errdetail("Value has bits set to right of mask.")));
	}

	ip_bits(dst) = bits;
	SET_INET_VARSIZE(dst);

	return dst;
}

Datum
inet_in(PG_FUNCTION_ARGS)
{
	char	   *src = PG_GETARG_CSTRING(0);

	PG_RETURN_INET_P(network_in(src, false, fcinfo->context));
}

Datum
cidr_in(PG_FUNCTION_ARGS)
{
	char	   *src = PG_GETARG_CSTRING(0);

	PG_RETURN_INET_P(network_in(src, true, fcinfo->context));
}


/*
 * 通用的 INET/CIDR 输出例程
 */
static char *
network_out(inet *src, bool is_cidr)
{
	char		tmp[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255/128")];
	char	   *dst;
	int			len;

	dst = pg_inet_net_ntop(ip_family(src), ip_addr(src), ip_bits(src),
						   tmp, sizeof(tmp));
	if (dst == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("could not format inet value: %m")));

	/* 对于 CIDR，若不存在 /n 则补上 */
	if (is_cidr && strchr(tmp, '/') == NULL)
	{
		len = strlen(tmp);
		snprintf(tmp + len, sizeof(tmp) - len, "/%u", ip_bits(src));
	}

	return pstrdup(tmp);
}

Datum
inet_out(PG_FUNCTION_ARGS)
{
	inet	   *src = PG_GETARG_INET_PP(0);

	PG_RETURN_CSTRING(network_out(src, false));
}

Datum
cidr_out(PG_FUNCTION_ARGS)
{
	inet	   *src = PG_GETARG_INET_PP(0);

	PG_RETURN_CSTRING(network_out(src, true));
}


/*
 *		network_recv		- 将外部二进制格式转换为 inet
 *
 * 外部表示为（各占一个字节）：family、bits、is_cidr、地址长度、按网络字节序
 * 排列的地址。
 *
 * is_cidr 的存在主要出于历史原因，不过它或许能让客户端复用部分代码。我们在
 * 输出时正确发送它，但在输入时忽略该值。
 */
static inet *
network_recv(StringInfo buf, bool is_cidr)
{
	inet	   *addr;
	char	   *addrptr;
	int			bits;
	int			nb,
				i;

	/* 确保 CIDR 值中未使用的位都被清零 */
	addr = (inet *) palloc0(sizeof(inet));

	ip_family(addr) = pq_getmsgbyte(buf);
	if (ip_family(addr) != PGSQL_AF_INET &&
		ip_family(addr) != PGSQL_AF_INET6)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
		/* translator: %s is inet or cidr */
				 errmsg("invalid address family in external \"%s\" value",
						is_cidr ? "cidr" : "inet")));
	bits = pq_getmsgbyte(buf);
	if (bits < 0 || bits > ip_maxbits(addr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
		/* translator: %s is inet or cidr */
				 errmsg("invalid bits in external \"%s\" value",
						is_cidr ? "cidr" : "inet")));
	ip_bits(addr) = bits;
	i = pq_getmsgbyte(buf);		/* 忽略 is_cidr */
	nb = pq_getmsgbyte(buf);
	if (nb != ip_addrsize(addr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
		/* translator: %s is inet or cidr */
				 errmsg("invalid length in external \"%s\" value",
						is_cidr ? "cidr" : "inet")));

	addrptr = (char *) ip_addr(addr);
	for (i = 0; i < nb; i++)
		addrptr[i] = pq_getmsgbyte(buf);

	/*
	 * 错误检查：CIDR 值不得在掩码长度之外设置任何位。
	 */
	if (is_cidr)
	{
		if (!addressOK(ip_addr(addr), bits, ip_family(addr)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("invalid external \"cidr\" value"),
					 errdetail("Value has bits set to right of mask.")));
	}

	SET_INET_VARSIZE(addr);

	return addr;
}

Datum
inet_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_INET_P(network_recv(buf, false));
}

Datum
cidr_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_INET_P(network_recv(buf, true));
}


/*
 *		network_send		- 将 inet 转换为二进制格式
 */
static bytea *
network_send(inet *addr, bool is_cidr)
{
	StringInfoData buf;
	char	   *addrptr;
	int			nb,
				i;

	pq_begintypsend(&buf);
	pq_sendbyte(&buf, ip_family(addr));
	pq_sendbyte(&buf, ip_bits(addr));
	pq_sendbyte(&buf, is_cidr);
	nb = ip_addrsize(addr);
	pq_sendbyte(&buf, nb);
	addrptr = (char *) ip_addr(addr);
	for (i = 0; i < nb; i++)
		pq_sendbyte(&buf, addrptr[i]);
	return pq_endtypsend(&buf);
}

Datum
inet_send(PG_FUNCTION_ARGS)
{
	inet	   *addr = PG_GETARG_INET_PP(0);

	PG_RETURN_BYTEA_P(network_send(addr, false));
}

Datum
cidr_send(PG_FUNCTION_ARGS)
{
	inet	   *addr = PG_GETARG_INET_PP(0);

	PG_RETURN_BYTEA_P(network_send(addr, true));
}


Datum
inet_to_cidr(PG_FUNCTION_ARGS)
{
	inet	   *src = PG_GETARG_INET_PP(0);
	int			bits;

	bits = ip_bits(src);

	/* 安全检查 */
	if ((bits < 0) || (bits > ip_maxbits(src)))
		elog(ERROR, "invalid inet bit length: %d", bits);

	PG_RETURN_INET_P(cidr_set_masklen_internal(src, bits));
}

Datum
inet_set_masklen(PG_FUNCTION_ARGS)
{
	inet	   *src = PG_GETARG_INET_PP(0);
	int			bits = PG_GETARG_INT32(1);
	inet	   *dst;

	if (bits == -1)
		bits = ip_maxbits(src);

	if ((bits < 0) || (bits > ip_maxbits(src)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid mask length: %d", bits)));

	/* 克隆原始数据 */
	dst = (inet *) palloc(VARSIZE_ANY(src));
	memcpy(dst, src, VARSIZE_ANY(src));

	ip_bits(dst) = bits;

	PG_RETURN_INET_P(dst);
}

Datum
cidr_set_masklen(PG_FUNCTION_ARGS)
{
	inet	   *src = PG_GETARG_INET_PP(0);
	int			bits = PG_GETARG_INT32(1);

	if (bits == -1)
		bits = ip_maxbits(src);

	if ((bits < 0) || (bits > ip_maxbits(src)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid mask length: %d", bits)));

	PG_RETURN_INET_P(cidr_set_masklen_internal(src, bits));
}

/*
 * 复制 src 并将掩码长度设为 'bits'（该值必须对 family 有效）
 */
inet *
cidr_set_masklen_internal(const inet *src, int bits)
{
	inet	   *dst = (inet *) palloc0(sizeof(inet));

	ip_family(dst) = ip_family(src);
	ip_bits(dst) = bits;

	if (bits > 0)
	{
		Assert(bits <= ip_maxbits(dst));

		/* 复制地址中相应的字节，其余保留为 0 */
		memcpy(ip_addr(dst), ip_addr(src), (bits + 7) / 8);

		/* 清除最后一个不完整字节中不需要的位 */
		if (bits % 8)
			ip_addr(dst)[bits / 8] &= ~(0xFF >> (bits % 8));
	}

	/* 正确设置 varlena 头部 */
	SET_INET_VARSIZE(dst);

	return dst;
}

/*
 *	用于排序及 inet/cidr 比较的基础比较函数。
 *
 * 比较先针对网络部分的公共位，然后是网络部分的长度，最后是整个未掩码地址。
 * 其效果是网络部分成为主排序键，而网络部分相等时再按主机部分排序。注意，
 * 这仅在 CIDR 中掩码右侧的地址位保证为零时才合理；否则逻辑上相等的 CIDR
 * 可能会比较出不同结果。
 */

static int32
network_cmp_internal(inet *a1, inet *a2)
{
	if (ip_family(a1) == ip_family(a2))
	{
		int			order;

		order = bitncmp(ip_addr(a1), ip_addr(a2),
						Min(ip_bits(a1), ip_bits(a2)));
		if (order != 0)
			return order;
		order = ((int) ip_bits(a1)) - ((int) ip_bits(a2));
		if (order != 0)
			return order;
		return bitncmp(ip_addr(a1), ip_addr(a2), ip_maxbits(a1));
	}

	return ip_family(a1) - ip_family(a2);
}

Datum
network_cmp(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_INT32(network_cmp_internal(a1, a2));
}

/*
 * SortSupport 策略例程
 */
Datum
network_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = network_fast_cmp;
	ssup->ssup_extra = NULL;

	if (ssup->abbreviate)
	{
		network_sortsupport_state *uss;
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(ssup->ssup_cxt);

		uss = palloc(sizeof(network_sortsupport_state));
		uss->input_count = 0;
		uss->estimating = true;
		initHyperLogLog(&uss->abbr_card, 10);

		ssup->ssup_extra = uss;

		ssup->comparator = ssup_datum_unsigned_cmp;
		ssup->abbrev_converter = network_abbrev_convert;
		ssup->abbrev_abort = network_abbrev_abort;
		ssup->abbrev_full_comparator = network_fast_cmp;

		MemoryContextSwitchTo(oldcontext);
	}

	PG_RETURN_VOID();
}

/*
 * SortSupport 比较函数
 */
static int
network_fast_cmp(Datum x, Datum y, SortSupport ssup)
{
	inet	   *arg1 = DatumGetInetPP(x);
	inet	   *arg2 = DatumGetInetPP(y);

	return network_cmp_internal(arg1, arg2);
}

/*
 * 用于估计缩写键优化效果的回调。
 *
 * 我们不关注未缩写数据的基数，因为在权威的 inet 比较器中不存在等值快速路径。
 */
static bool
network_abbrev_abort(int memtupcount, SortSupport ssup)
{
	network_sortsupport_state *uss = ssup->ssup_extra;
	double		abbr_card;

	if (memtupcount < 10000 || uss->input_count < 10000 || !uss->estimating)
		return false;

	abbr_card = estimateHyperLogLog(&uss->abbr_card);

	/*
	 * 如果不同值超过 10 万，那么即使要排序数十亿行数据，我们也大概率仍得不
	 * 偿失，而撤销这么多缩写行的代价很可能并不值得。此时我们停止计数，因为
	 * 我们知道已经确定采用了缩写。
	 */
	if (abbr_card > 100000.0)
	{
		if (trace_sort)
			elog(LOG,
				 "network_abbrev: estimation ends at cardinality %f"
				 " after " INT64_FORMAT " values (%d rows)",
				 abbr_card, uss->input_count, memtupcount);
		uss->estimating = false;
		return false;
	}

	/*
	 * 目标最小基数为每约 2k 个非空输入 1 个。0.5 行的容差系数让我们在遇到真正
	 * 病态的数据时能更早中止——即前 2k 行（非空）中恰好只有一个缩写值的情况。
	 */
	if (abbr_card < uss->input_count / 2000.0 + 0.5)
	{
		if (trace_sort)
			elog(LOG,
				 "network_abbrev: aborting abbreviation at cardinality %f"
				 " below threshold %f after " INT64_FORMAT " values (%d rows)",
				 abbr_card, uss->input_count / 2000.0 + 0.5, uss->input_count,
				 memtupcount);
		return true;
	}

	if (trace_sort)
		elog(LOG,
			 "network_abbrev: cardinality %f after " INT64_FORMAT
			 " values (%d rows)", abbr_card, uss->input_count, memtupcount);

	return false;
}

/*
 * SortSupport 转换例程。将原始的 inet/cidr 表示转换为缩写键表示，使其能与
 * 简单的三路无符号整数比较配合工作。缩写比较通过一种借助精细填充来"调理"
 * 键值的编码方案，遵循 network_cmp_internal() 对 inet/cidr datum 的排序规则。
 *
 * 一点背景：inet 值有三个主要组成部分（以地址 1.2.3.4/24 为例）：
 *
 *     * 网络，即被掩码的位（1.2.3.0）。
 *     * 网络掩码尺寸（/24）。
 *     * 子网，即掩码之外的位（0.0.0.4）。
 *
 * cidr 值与此相同，只是仅含前两个部分——它们的所有子网位*必须*为零
 * （1.2.3.0/24）。
 *
 * IPv4 和 IPv6 在此构成上一致，区别在于 IPv4 地址最多 32 位，而 IPv6 为 64
 * 位，因此 IPv6 中每个部分都可能更大。
 *
 * inet/cidr 类型按照以下排序规则进行比较。若在某一步检测到不等，则比较结束；
 * 若某条规则打平，则算法落入下一条规则来打破平局：
 *
 *     1. IPv4 总是排在 IPv6 之前。
 *     2. 比较网络位。
 *     3. 比较网络掩码尺寸。
 *     4. 比较所有位（能走到此处，说明网络掩码位与网络掩码尺寸都相等，因此
 *        实际上我们只是在比较子网位）。
 *
 * 为 SortSupport 生成缩写键时，我们在确保将这些键作为整数比较时上述规则仍
 * 被遵守的前提下，尽量把内容塞进一个 datum。具体内容与 IP 地址族及 datum
 * 大小有关。
 *
 * IPv4
 * ----
 *
 * 4 字节 datum：
 *
 * 先放 1 位表示 IP 地址族（IPv4 或 IPv6；这一位在下面每种情形中都存在），
 * 其后紧跟除 1 位之外的全部网络掩码位。
 *
 * +----------+---------------------+
 * | 1 bit IP |   31 bits network   |     (1 bit network
 * |  family  |     (truncated)     |      omitted)
 * +----------+---------------------+
 *
 * 8 字节 datum：
 *
 * 我们有空间存放全部网络掩码位，其后是网络掩码尺寸，再其后是 25 位子网
 * （实践中 25 位通常已绰绰有余）。cidr datum 的子网位始终全为零。
 *
 * +----------+-----------------------+--------------+--------------------+
 * | 1 bit IP |    32 bits network    |    6 bits    |   25 bits subnet   |
 * |  family  |        (full)         | network size |    (truncated)     |
 * +----------+-----------------------+--------------+--------------------+
 *
 * IPv6
 * ----
 *
 * 4 字节 datum：
 *
 * +----------+---------------------+
 * | 1 bit IP |   31 bits network   |    (up to 97 bits
 * |  family  |     (truncated)     |   network omitted)
 * +----------+---------------------+
 *
 * 8 字节 datum：
 *
 * +----------+---------------------------------+
 * | 1 bit IP |         63 bits network         |    (up to 65 bits
 * |  family  |           (truncated)           |   network omitted)
 * +----------+---------------------------------+
 */
static Datum
network_abbrev_convert(Datum original, SortSupport ssup)
{
	network_sortsupport_state *uss = ssup->ssup_extra;
	inet	   *authoritative = DatumGetInetPP(original);
	Datum		res,
				ipaddr_datum,
				subnet_bitmask,
				network;
	int			subnet_size;

	Assert(ip_family(authoritative) == PGSQL_AF_INET ||
		   ip_family(authoritative) == PGSQL_AF_INET6);

	/*
	 * 通过取 IP 地址的前 4 或 8 字节，得到其无符号整数表示。IPv4 地址总是取
	 * 全部 4 字节。对于 IPv6 地址，在 8 字节 datum 下取前 8 字节，否则取 4 字节。
	 *
	 * 我们消费的是一个 unsigned char 数组，因此在小端系统上需要字节交换（inet
	 * 的 ipaddr 字段以最高有效字节在前的方式存储）。
	 */
	if (ip_family(authoritative) == PGSQL_AF_INET)
	{
		uint32		ipaddr_datum32;

		memcpy(&ipaddr_datum32, ip_addr(authoritative), sizeof(uint32));

		/* 在小端机器上必须做字节交换 */
#ifndef WORDS_BIGENDIAN
		ipaddr_datum = pg_bswap32(ipaddr_datum32);
#else
		ipaddr_datum = ipaddr_datum32;
#endif

		/* 初始化结果，但不设置 ipfamily 位 */
		res = (Datum) 0;
	}
	else
	{
		memcpy(&ipaddr_datum, ip_addr(authoritative), sizeof(Datum));

		/* 在小端机器上必须做字节交换 */
		ipaddr_datum = DatumBigEndianToNative(ipaddr_datum);

		/* 初始化结果，并设置 ipfamily（最高有效）位 */
		res = ((Datum) 1) << (SIZEOF_DATUM * BITS_PER_BYTE - 1);
	}

	/*
	 * ipaddr_datum 必须被"拆分"：高位进入缩写键的 "network" 部分（因掩码往往
	 * 末尾补零），而低位在有余地时进入 "subnet" 部分。这通常通过生成一个临时的
	 * datum 子网位掩码来实现，该掩码在后续生成子网位时可能复用。（注意，子网
	 * 位仅在 datum 为 8 字节的平台上配合 IPv4 datum 使用。）
	 *
	 * 子网中的位数用于生成 datum 子网位掩码。例如，对于 /24 的 IPv4 datum，
	 * 子网位有 8 位（因为 32 - 24 = 8），所以最终子网位掩码为 B'1111 1111'。
	 * 但对于 ipaddr 位无法全部放入 datum 的情况，我们需要显式处理（否则会用
	 * IPv6 值错误地掩掉 network 部分）。
	 */
	subnet_size = ip_maxbits(authoritative) - ip_bits(authoritative);
	Assert(subnet_size >= 0);
	/* 子网尺寸必须能配合前缀 ipaddr 的情形 */
	subnet_size %= SIZEOF_DATUM * BITS_PER_BYTE;
	if (ip_bits(authoritative) == 0)
	{
		/* 尽可能多的 ipaddr 位放入 subnet */
		subnet_bitmask = ((Datum) 0) - 1;
		network = 0;
	}
	else if (ip_bits(authoritative) < SIZEOF_DATUM * BITS_PER_BYTE)
	{
		/* 在 network 与 subnet 之间拆分 ipaddr 位 */
		subnet_bitmask = (((Datum) 1) << subnet_size) - 1;
		network = ipaddr_datum & ~subnet_bitmask;
	}
	else
	{
		/* 尽可能多的 ipaddr 位放入 network */
		subnet_bitmask = 0;
		network = ipaddr_datum;
	}

#if SIZEOF_DATUM == 8
	if (ip_family(authoritative) == PGSQL_AF_INET)
	{
		/*
		 * IPv4 配合 8 字节 datum：保留全部 32 个网络掩码位、网络掩码尺寸，以及
		 * 最高有效的 25 个 subnet 位
		 */
		Datum		netmask_size = (Datum) ip_bits(authoritative);
		Datum		subnet;

		/*
		 * 左移 31 位：6 位网络掩码尺寸 + 25 位 subnet 位。
		 *
		 * 我们不对因掩码而为零的 network 位与"真正"/未掩码的零位做任何区分。
		 * 一个通过比较未掩码的非零位与已掩码/清零的位来决出的缩写比较，实际上
		 * 是基于 ip_bits() 决出的，即便该比较不会到达 netmask_size 位。
		 */
		network <<= (ABBREV_BITS_INET4_NETMASK_SIZE +
					 ABBREV_BITS_INET4_SUBNET);

		/* 移位为末尾的 subnet 位腾出空间 */
		netmask_size <<= ABBREV_BITS_INET4_SUBNET;

		/* 提取 subnet 位但不移动它们 */
		subnet = ipaddr_datum & subnet_bitmask;

		/*
		 * 如果 subnet 位超过 25 个，我们就无法全部容纳。将 subnet 下移，以避免
		 * 覆盖那些本应只用于 netmask_size 的位。
		 *
		 * 像这样丢弃最低有效的 subnet 位是正确的，因为在 subnet 层级决出的缩写
		 * 比较，必然已经具有相等的 netmask_size/ip_bits() 值才能走到这一步。
		 */
		if (subnet_size > ABBREV_BITS_INET4_SUBNET)
			subnet >>= subnet_size - ABBREV_BITS_INET4_SUBNET;

		/*
		 * 组装最终的缩写键，同时不破坏必须保持为零的 ipfamily 位。
		 */
		res |= network | netmask_size | subnet;
	}
	else
#endif
	{
		/*
		 * 4 字节 datum，或者配合 8 字节 datum 的 IPv6：使用尽可能多的网络掩码位
		 * 填入最终缩写键。同时避免破坏先前已设置的 ipfamily 位。
		 */
		res |= network >> 1;
	}

	uss->input_count += 1;

	/* 对缩写键做哈希 */
	if (uss->estimating)
	{
		uint32		tmp;

#if SIZEOF_DATUM == 8
		tmp = (uint32) res ^ (uint32) ((uint64) res >> 32);
#else							/* SIZEOF_DATUM != 8 */
		tmp = (uint32) res;
#endif

		addHyperLogLog(&uss->abbr_card, DatumGetUInt32(hash_uint32(tmp)));
	}

	return res;
}

/*
 *	Boolean ordering tests.
 */
Datum
network_lt(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) < 0);
}

Datum
network_le(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) <= 0);
}

Datum
network_eq(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) == 0);
}

Datum
network_ge(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) >= 0);
}

Datum
network_gt(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) > 0);
}

Datum
network_ne(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) != 0);
}

/*
 * 最小值/最大值支持函数。
 */
Datum
network_smaller(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (network_cmp_internal(a1, a2) < 0)
		PG_RETURN_INET_P(a1);
	else
		PG_RETURN_INET_P(a2);
}

Datum
network_larger(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (network_cmp_internal(a1, a2) > 0)
		PG_RETURN_INET_P(a1);
	else
		PG_RETURN_INET_P(a2);
}

/*
 * 用于 inet/cidr 上哈希索引的支持函数。
 */
Datum
hashinet(PG_FUNCTION_ARGS)
{
	inet	   *addr = PG_GETARG_INET_PP(0);
	int			addrsize = ip_addrsize(addr);

	/* XXX 这里假设数据结构中不存在填充字节 */
	return hash_any((unsigned char *) VARDATA_ANY(addr), addrsize + 2);
}

Datum
hashinetextended(PG_FUNCTION_ARGS)
{
	inet	   *addr = PG_GETARG_INET_PP(0);
	int			addrsize = ip_addrsize(addr);

	return hash_any_extended((unsigned char *) VARDATA_ANY(addr), addrsize + 2,
							 PG_GETARG_INT64(1));
}

/*
 *	布尔网络包含测试。
 */
Datum
network_sub(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (ip_family(a1) == ip_family(a2))
	{
		PG_RETURN_BOOL(ip_bits(a1) > ip_bits(a2) &&
					   bitncmp(ip_addr(a1), ip_addr(a2), ip_bits(a2)) == 0);
	}

	PG_RETURN_BOOL(false);
}

Datum
network_subeq(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (ip_family(a1) == ip_family(a2))
	{
		PG_RETURN_BOOL(ip_bits(a1) >= ip_bits(a2) &&
					   bitncmp(ip_addr(a1), ip_addr(a2), ip_bits(a2)) == 0);
	}

	PG_RETURN_BOOL(false);
}

Datum
network_sup(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (ip_family(a1) == ip_family(a2))
	{
		PG_RETURN_BOOL(ip_bits(a1) < ip_bits(a2) &&
					   bitncmp(ip_addr(a1), ip_addr(a2), ip_bits(a1)) == 0);
	}

	PG_RETURN_BOOL(false);
}

Datum
network_supeq(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (ip_family(a1) == ip_family(a2))
	{
		PG_RETURN_BOOL(ip_bits(a1) <= ip_bits(a2) &&
					   bitncmp(ip_addr(a1), ip_addr(a2), ip_bits(a1)) == 0);
	}

	PG_RETURN_BOOL(false);
}

Datum
network_overlap(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (ip_family(a1) == ip_family(a2))
	{
		PG_RETURN_BOOL(bitncmp(ip_addr(a1), ip_addr(a2),
							   Min(ip_bits(a1), ip_bits(a2))) == 0);
	}

	PG_RETURN_BOOL(false);
}

/*
 * 用于网络子集/超集运算符的规划器支持函数
 */
Datum
network_subset_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Node	   *ret = NULL;

	if (IsA(rawreq, SupportRequestIndexCondition))
	{
		/* 尝试将运算符/函数调用转换为索引条件 */
		SupportRequestIndexCondition *req = (SupportRequestIndexCondition *) rawreq;

		if (is_opclause(req->node))
		{
			OpExpr	   *clause = (OpExpr *) req->node;

			Assert(list_length(clause->args) == 2);
			ret = (Node *)
				match_network_function((Node *) linitial(clause->args),
									   (Node *) lsecond(clause->args),
									   req->indexarg,
									   req->funcid,
									   req->opfamily);
		}
		else if (is_funcclause(req->node))	/* 保持谨慎 */
		{
			FuncExpr   *clause = (FuncExpr *) req->node;

			Assert(list_length(clause->args) == 2);
			ret = (Node *)
				match_network_function((Node *) linitial(clause->args),
									   (Node *) lsecond(clause->args),
									   req->indexarg,
									   req->funcid,
									   req->opfamily);
		}
	}

	PG_RETURN_POINTER(ret);
}

/*
 * match_network_function
 *	  尝试为网络子集/超集函数生成索引条件。
 *
 * 这一层只负责识别函数，并在必要时交换参数。
 */
static List *
match_network_function(Node *leftop,
					   Node *rightop,
					   int indexarg,
					   Oid funcid,
					   Oid opfamily)
{
	switch (funcid)
	{
		case F_NETWORK_SUB:
			/* 索引键必须在左侧 */
			if (indexarg != 0)
				return NIL;
			return match_network_subset(leftop, rightop, false, opfamily);

		case F_NETWORK_SUBEQ:
			/* 索引键必须在左侧 */
			if (indexarg != 0)
				return NIL;
			return match_network_subset(leftop, rightop, true, opfamily);

		case F_NETWORK_SUP:
			/* 索引键必须在右侧 */
			if (indexarg != 1)
				return NIL;
			return match_network_subset(rightop, leftop, false, opfamily);

		case F_NETWORK_SUPEQ:
			/* 索引键必须在右侧 */
			if (indexarg != 1)
				return NIL;
			return match_network_subset(rightop, leftop, true, opfamily);

		default:

			/*
			 * 只有某人将这个支持函数附加到了一个预期之外的函数时，才会走到这里。
			 * 也许我们应该报错，但目前先什么都不做。
			 */
			return NIL;
	}
}

/*
 * match_network_subset
 *	  尝试为网络子集函数生成索引条件。
 */
static List *
match_network_subset(Node *leftop,
					 Node *rightop,
					 bool is_eq,
					 Oid opfamily)
{
	List	   *result;
	Datum		rightopval;
	Oid			datatype = INETOID;
	Oid			opr1oid;
	Oid			opr2oid;
	Datum		opr1right;
	Datum		opr2right;
	Expr	   *expr;

	/*
	 * 对于非常量或 NULL 的比较值，无法做任何处理。
	 *
	 * 注意，由于我们只处理 RHS 上带有硬常量的情况，它自然就是伪常量，因此
	 * 我们无需操心去验证这一点。
	 */
	if (!IsA(rightop, Const) ||
		((Const *) rightop)->constisnull)
		return NIL;
	rightopval = ((Const *) rightop)->constvalue;

	/*
	 * 创建子句 "key >= network_scan_first( rightopval )"，若运算符不允许等值
	 * 则为 ">"。
	 */
	opr1oid = get_opfamily_member_for_cmptype(opfamily, datatype, datatype, is_eq ? COMPARE_GE : COMPARE_GT);
	if (opr1oid == InvalidOid)
		return NIL;

	opr1right = network_scan_first(rightopval);

	expr = make_opclause(opr1oid, BOOLOID, false,
						 (Expr *) leftop,
						 (Expr *) makeConst(datatype, -1,
											InvalidOid, /* 不可排序 */
											-1, opr1right,
											false, false),
						 InvalidOid, InvalidOid);
	result = list_make1(expr);

	/* 创建子句 "key <= network_scan_last( rightopval )" */

	opr2oid = get_opfamily_member_for_cmptype(opfamily, datatype, datatype, COMPARE_LE);
	if (opr2oid == InvalidOid)
		return NIL;

	opr2right = network_scan_last(rightopval);

	expr = make_opclause(opr2oid, BOOLOID, false,
						 (Expr *) leftop,
						 (Expr *) makeConst(datatype, -1,
											InvalidOid, /* 不可排序 */
											-1, opr2right,
											false, false),
						 InvalidOid, InvalidOid);
	result = lappend(result, expr);

	return result;
}


/*
 * 从网络数据类型中提取数据。
 */
Datum
network_host(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	char	   *ptr;
	char		tmp[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255/128")];

	/* 强制显示最大位数，忽略掩码长度…… */
	if (pg_inet_net_ntop(ip_family(ip), ip_addr(ip), ip_maxbits(ip),
						 tmp, sizeof(tmp)) == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("could not format inet value: %m")));

	/* 若存在 /n 则抑制之（现在本不该出现） */
	if ((ptr = strchr(tmp, '/')) != NULL)
		*ptr = '\0';

	PG_RETURN_TEXT_P(cstring_to_text(tmp));
}

/*
 * network_show 实现了 inet 和 cidr 到 text 的强制转换。其行为与 network_out
 * 并不完全相同，因此无法用 CoerceViaIO 取代它。
 */
Datum
network_show(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	int			len;
	char		tmp[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255/128")];

	if (pg_inet_net_ntop(ip_family(ip), ip_addr(ip), ip_maxbits(ip),
						 tmp, sizeof(tmp)) == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("could not format inet value: %m")));

	/* 若不存在 /n 则补上（实际上不会缺失） */
	if (strchr(tmp, '/') == NULL)
	{
		len = strlen(tmp);
		snprintf(tmp + len, sizeof(tmp) - len, "/%u", ip_bits(ip));
	}

	PG_RETURN_TEXT_P(cstring_to_text(tmp));
}

Datum
inet_abbrev(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	char	   *dst;
	char		tmp[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255/128")];

	dst = pg_inet_net_ntop(ip_family(ip), ip_addr(ip),
						   ip_bits(ip), tmp, sizeof(tmp));

	if (dst == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("could not format inet value: %m")));

	PG_RETURN_TEXT_P(cstring_to_text(tmp));
}

Datum
cidr_abbrev(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	char	   *dst;
	char		tmp[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255/128")];

	dst = pg_inet_cidr_ntop(ip_family(ip), ip_addr(ip),
							ip_bits(ip), tmp, sizeof(tmp));

	if (dst == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("could not format cidr value: %m")));

	PG_RETURN_TEXT_P(cstring_to_text(tmp));
}

Datum
network_masklen(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);

	PG_RETURN_INT32(ip_bits(ip));
}

Datum
network_family(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);

	switch (ip_family(ip))
	{
		case PGSQL_AF_INET:
			PG_RETURN_INT32(4);
			break;
		case PGSQL_AF_INET6:
			PG_RETURN_INT32(6);
			break;
		default:
			PG_RETURN_INT32(0);
			break;
	}
}

Datum
network_broadcast(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *dst;
	int			byte;
	int			bits;
	int			maxbytes;
	unsigned char mask;
	unsigned char *a,
			   *b;

	/* 确保任何未使用的位都被清零 */
	dst = (inet *) palloc0(sizeof(inet));

	maxbytes = ip_addrsize(ip);
	bits = ip_bits(ip);
	a = ip_addr(ip);
	b = ip_addr(dst);

	for (byte = 0; byte < maxbytes; byte++)
	{
		if (bits >= 8)
		{
			mask = 0x00;
			bits -= 8;
		}
		else if (bits == 0)
			mask = 0xff;
		else
		{
			mask = 0xff >> bits;
			bits = 0;
		}

		b[byte] = a[byte] | mask;
	}

	ip_family(dst) = ip_family(ip);
	ip_bits(dst) = ip_bits(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}

Datum
network_network(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *dst;
	int			byte;
	int			bits;
	unsigned char mask;
	unsigned char *a,
			   *b;

	/* 确保任何未使用的位都被清零 */
	dst = (inet *) palloc0(sizeof(inet));

	bits = ip_bits(ip);
	a = ip_addr(ip);
	b = ip_addr(dst);

	byte = 0;

	while (bits)
	{
		if (bits >= 8)
		{
			mask = 0xff;
			bits -= 8;
		}
		else
		{
			mask = 0xff << (8 - bits);
			bits = 0;
		}

		b[byte] = a[byte] & mask;
		byte++;
	}

	ip_family(dst) = ip_family(ip);
	ip_bits(dst) = ip_bits(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}

Datum
network_netmask(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *dst;
	int			byte;
	int			bits;
	unsigned char mask;
	unsigned char *b;

	/* 确保任何未使用的位都被清零 */
	dst = (inet *) palloc0(sizeof(inet));

	bits = ip_bits(ip);
	b = ip_addr(dst);

	byte = 0;

	while (bits)
	{
		if (bits >= 8)
		{
			mask = 0xff;
			bits -= 8;
		}
		else
		{
			mask = 0xff << (8 - bits);
			bits = 0;
		}

		b[byte] = mask;
		byte++;
	}

	ip_family(dst) = ip_family(ip);
	ip_bits(dst) = ip_maxbits(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}

Datum
network_hostmask(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *dst;
	int			byte;
	int			bits;
	int			maxbytes;
	unsigned char mask;
	unsigned char *b;

	/* 确保任何未使用的位都被清零 */
	dst = (inet *) palloc0(sizeof(inet));

	maxbytes = ip_addrsize(ip);
	bits = ip_maxbits(ip) - ip_bits(ip);
	b = ip_addr(dst);

	byte = maxbytes - 1;

	while (bits)
	{
		if (bits >= 8)
		{
			mask = 0xff;
			bits -= 8;
		}
		else
		{
			mask = 0xff >> (8 - bits);
			bits = 0;
		}

		b[byte] = mask;
		byte--;
	}

	ip_family(dst) = ip_family(ip);
	ip_bits(dst) = ip_maxbits(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}

/*
 * 如果两个地址来自同一地址族则返回 true，否则返回 false。用于检查我们是否能
 * 够创建一个同时包含两个网络的网络。
 */
Datum
inet_same_family(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(ip_family(a1) == ip_family(a2));
}

/*
 * 返回同时包含两个输入的最小 CIDR。
 */
Datum
inet_merge(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0),
			   *a2 = PG_GETARG_INET_PP(1);
	int			commonbits;

	if (ip_family(a1) != ip_family(a2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot merge addresses from different families")));

	commonbits = bitncommon(ip_addr(a1), ip_addr(a2),
							Min(ip_bits(a1), ip_bits(a2)));

	PG_RETURN_INET_P(cidr_set_masklen_internal(a1, commonbits));
}

/*
 * 将一个网络数据类型的值转换为近似标量值。
 *
 * 这用于估计涉及网络类型的不等运算符的选择率。
 *
 * 失败时（例如不支持的 typid），将 *failure 设为 true；否则该变量不变。
 */
double
convert_network_to_scalar(Datum value, Oid typid, bool *failure)
{
	switch (typid)
	{
		case INETOID:
		case CIDROID:
			{
				inet	   *ip = DatumGetInetPP(value);
				int			len;
				double		res;
				int			i;

				/*
				 * 注意，对于 IPv6 我们并不使用完整的地址。
				 */
				if (ip_family(ip) == PGSQL_AF_INET)
					len = 4;
				else
					len = 5;

				res = ip_family(ip);
				for (i = 0; i < len; i++)
				{
					res *= 256;
					res += ip_addr(ip)[i];
				}
				return res;
			}
		case MACADDROID:
			{
				macaddr    *mac = DatumGetMacaddrP(value);
				double		res;

				res = (mac->a << 16) | (mac->b << 8) | (mac->c);
				res *= 256 * 256 * 256;
				res += (mac->d << 16) | (mac->e << 8) | (mac->f);
				return res;
			}
		case MACADDR8OID:
			{
				macaddr8   *mac = DatumGetMacaddr8P(value);
				double		res;

				res = (mac->a << 24) | (mac->b << 16) | (mac->c << 8) | (mac->d);
				res *= ((double) 256) * 256 * 256 * 256;
				res += (mac->e << 24) | (mac->f << 16) | (mac->g << 8) | (mac->h);
				return res;
			}
	}

	*failure = true;
	return 0;
}

/*
 * int
 * bitncmp(l, r, n)
 *		比较位掩码 l 和 r，共 n 位。
 * 返回值：
 *		按 libc 惯例，返回 <0、>0 或 0。
 * 说明：
 *		假定采用网络字节序。这意味着 192.5.5.240/28 的第四个字节为 0x11110000。
 * 作者：
 *		Paul Vixie (ISC)，1996 年 6 月
 */
int
bitncmp(const unsigned char *l, const unsigned char *r, int n)
{
	unsigned int lb,
				rb;
	int			x,
				b;

	b = n / 8;
	x = memcmp(l, r, b);
	if (x || (n % 8) == 0)
		return x;

	lb = l[b];
	rb = r[b];
	for (b = n % 8; b > 0; b--)
	{
		if (IS_HIGHBIT_SET(lb) != IS_HIGHBIT_SET(rb))
		{
			if (IS_HIGHBIT_SET(lb))
				return 1;
			return -1;
		}
		lb <<= 1;
		rb <<= 1;
	}
	return 0;
}

/*
 * bitncommon：比较位掩码 l 和 r，最多 n 位。
 *
 * 返回相匹配的前导位数（0 到 n）。
 */
int
bitncommon(const unsigned char *l, const unsigned char *r, int n)
{
	int			byte,
				nbits;

	/* 最后一个字节中需要检查的位数 */
	nbits = n % 8;

	/* 检查完整的字节 */
	for (byte = 0; byte < n / 8; byte++)
	{
		if (l[byte] != r[byte])
		{
			/* 最后一个字节中至少有一个位不相同 */
			nbits = 7;
			break;
		}
	}

	/* 检查最后一个不完整字节中的位 */
	if (nbits != 0)
	{
		/* 计算首个不匹配字节的差值 */
		unsigned int diff = l[byte] ^ r[byte];

		/* 从最高位到最低位逐位比较 */
		while ((diff >> (8 - nbits)) != 0)
			nbits--;
	}

	return (8 * byte) + nbits;
}


/*
 * 验证 CIDR 地址是否合法（掩码长度之外没有设置位）
 */
static bool
addressOK(unsigned char *a, int bits, int family)
{
	int			byte;
	int			nbits;
	int			maxbits;
	int			maxbytes;
	unsigned char mask;

	if (family == PGSQL_AF_INET)
	{
		maxbits = 32;
		maxbytes = 4;
	}
	else
	{
		maxbits = 128;
		maxbytes = 16;
	}
	Assert(bits <= maxbits);

	if (bits == maxbits)
		return true;

	byte = bits / 8;

	nbits = bits % 8;
	mask = 0xff;
	if (bits != 0)
		mask >>= nbits;

	while (byte < maxbytes)
	{
		if ((a[byte] & mask) != 0)
			return false;
		mask = 0xff;
		byte++;
	}

	return true;
}


/*
 * 这些函数被规划器用来为 a << b 与 a <<= b 这类子句生成索引扫描的边界。
 */

/* 返回给定网络上某个 IP 的最小值 */
Datum
network_scan_first(Datum in)
{
	return DirectFunctionCall1(network_network, in);
}

/*
 * 返回给定网络上的"最后"一个 IP。它是广播地址，但 masklen 必须设为它的最大
 * 位数，因为 192.168.0.255/24 被认为小于 192.168.0.255/32。
 *
 * inet_set_masklen() 被改造为：当传入参数 '-1' 时，将 IPv6 的掩码长度取最大
 * 128、IPv4 取最大 32。
 */
Datum
network_scan_last(Datum in)
{
	return DirectFunctionCall2(inet_set_masklen,
							   DirectFunctionCall1(network_broadcast, in),
							   Int32GetDatum(-1));
}


/*
 * 客户端连接所用的 IP 地址（若为 Unix 套接字则为 NULL）
 */
Datum
inet_client_addr(PG_FUNCTION_ARGS)
{
	Port	   *port = MyProcPort;
	char		remote_host[NI_MAXHOST];
	int			ret;

	if (port == NULL)
		PG_RETURN_NULL();

	switch (port->raddr.addr.ss_family)
	{
		case AF_INET:
		case AF_INET6:
			break;
		default:
			PG_RETURN_NULL();
	}

	remote_host[0] = '\0';

	ret = pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen,
							 remote_host, sizeof(remote_host),
							 NULL, 0,
							 NI_NUMERICHOST | NI_NUMERICSERV);
	if (ret != 0)
		PG_RETURN_NULL();

	clean_ipv6_addr(port->raddr.addr.ss_family, remote_host);

	PG_RETURN_INET_P(network_in(remote_host, false, NULL));
}


/*
 * 客户端连接所用的端口（若为 Unix 套接字则为 NULL）
 */
Datum
inet_client_port(PG_FUNCTION_ARGS)
{
	Port	   *port = MyProcPort;
	char		remote_port[NI_MAXSERV];
	int			ret;

	if (port == NULL)
		PG_RETURN_NULL();

	switch (port->raddr.addr.ss_family)
	{
		case AF_INET:
		case AF_INET6:
			break;
		default:
			PG_RETURN_NULL();
	}

	remote_port[0] = '\0';

	ret = pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen,
							 NULL, 0,
							 remote_port, sizeof(remote_port),
							 NI_NUMERICHOST | NI_NUMERICSERV);
	if (ret != 0)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(DirectFunctionCall1(int4in, CStringGetDatum(remote_port)));
}


/*
 * 服务器接受连接的 IP 地址（若为 Unix 套接字则为 NULL）
 */
Datum
inet_server_addr(PG_FUNCTION_ARGS)
{
	Port	   *port = MyProcPort;
	char		local_host[NI_MAXHOST];
	int			ret;

	if (port == NULL)
		PG_RETURN_NULL();

	switch (port->laddr.addr.ss_family)
	{
		case AF_INET:
		case AF_INET6:
			break;
		default:
			PG_RETURN_NULL();
	}

	local_host[0] = '\0';

	ret = pg_getnameinfo_all(&port->laddr.addr, port->laddr.salen,
							 local_host, sizeof(local_host),
							 NULL, 0,
							 NI_NUMERICHOST | NI_NUMERICSERV);
	if (ret != 0)
		PG_RETURN_NULL();

	clean_ipv6_addr(port->laddr.addr.ss_family, local_host);

	PG_RETURN_INET_P(network_in(local_host, false, NULL));
}


/*
 * 服务器接受连接的端口（若为 Unix 套接字则为 NULL）
 */
Datum
inet_server_port(PG_FUNCTION_ARGS)
{
	Port	   *port = MyProcPort;
	char		local_port[NI_MAXSERV];
	int			ret;

	if (port == NULL)
		PG_RETURN_NULL();

	switch (port->laddr.addr.ss_family)
	{
		case AF_INET:
		case AF_INET6:
			break;
		default:
			PG_RETURN_NULL();
	}

	local_port[0] = '\0';

	ret = pg_getnameinfo_all(&port->laddr.addr, port->laddr.salen,
							 NULL, 0,
							 local_port, sizeof(local_port),
							 NI_NUMERICHOST | NI_NUMERICSERV);
	if (ret != 0)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(DirectFunctionCall1(int4in, CStringGetDatum(local_port)));
}


Datum
inetnot(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *dst;

	dst = (inet *) palloc0(sizeof(inet));

	{
		int			nb = ip_addrsize(ip);
		unsigned char *pip = ip_addr(ip);
		unsigned char *pdst = ip_addr(dst);

		while (--nb >= 0)
			pdst[nb] = ~pip[nb];
	}
	ip_bits(dst) = ip_bits(ip);

	ip_family(dst) = ip_family(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}


Datum
inetand(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *ip2 = PG_GETARG_INET_PP(1);
	inet	   *dst;

	dst = (inet *) palloc0(sizeof(inet));

	if (ip_family(ip) != ip_family(ip2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot AND inet values of different sizes")));
	else
	{
		int			nb = ip_addrsize(ip);
		unsigned char *pip = ip_addr(ip);
		unsigned char *pip2 = ip_addr(ip2);
		unsigned char *pdst = ip_addr(dst);

		while (--nb >= 0)
			pdst[nb] = pip[nb] & pip2[nb];
	}
	ip_bits(dst) = Max(ip_bits(ip), ip_bits(ip2));

	ip_family(dst) = ip_family(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}


Datum
inetor(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *ip2 = PG_GETARG_INET_PP(1);
	inet	   *dst;

	dst = (inet *) palloc0(sizeof(inet));

	if (ip_family(ip) != ip_family(ip2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot OR inet values of different sizes")));
	else
	{
		int			nb = ip_addrsize(ip);
		unsigned char *pip = ip_addr(ip);
		unsigned char *pip2 = ip_addr(ip2);
		unsigned char *pdst = ip_addr(dst);

		while (--nb >= 0)
			pdst[nb] = pip[nb] | pip2[nb];
	}
	ip_bits(dst) = Max(ip_bits(ip), ip_bits(ip2));

	ip_family(dst) = ip_family(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}


static inet *
internal_inetpl(inet *ip, int64 addend)
{
	inet	   *dst;

	dst = (inet *) palloc0(sizeof(inet));

	{
		int			nb = ip_addrsize(ip);
		unsigned char *pip = ip_addr(ip);
		unsigned char *pdst = ip_addr(dst);
		int			carry = 0;

		while (--nb >= 0)
		{
			carry = pip[nb] + (int) (addend & 0xFF) + carry;
			pdst[nb] = (unsigned char) (carry & 0xFF);
			carry >>= 8;

			/*
			 * 对 addend 做右移时必须小心，因为右移对负值而言并非可移植操作，而
			 * 简单地除以 256 也行不通（标准舍入方向不对，此外还可能存在舍入方式
			 * 相反的机器）。因此，我们显式清空低字节以消解除法正确结果上的疑虑，
			 * 然后采用除法而非移位。
			 */
			addend &= ~((int64) 0xFF);
			addend /= 0x100;
		}

		/*
		 * 此时，若原始 addend 为 >= 0，则 addend 和 carry 都应为零；若原始 addend
		 * 为 < 0，则 addend 应为 -1 且 carry 为 1。其他任何情况都意味着溢出。
		 */
		if (!((addend == 0 && carry == 0) ||
			  (addend == -1 && carry == 1)))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("result is out of range")));
	}

	ip_bits(dst) = ip_bits(ip);
	ip_family(dst) = ip_family(ip);
	SET_INET_VARSIZE(dst);

	return dst;
}


Datum
inetpl(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	int64		addend = PG_GETARG_INT64(1);

	PG_RETURN_INET_P(internal_inetpl(ip, addend));
}


Datum
inetmi_int8(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	int64		addend = PG_GETARG_INT64(1);

	PG_RETURN_INET_P(internal_inetpl(ip, -addend));
}


Datum
inetmi(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *ip2 = PG_GETARG_INET_PP(1);
	int64		res = 0;

	if (ip_family(ip) != ip_family(ip2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot subtract inet values of different sizes")));
	else
	{
		/*
		 * 我们使用传统的"取补、加一、相加"规则来构造差值，其中"加一"部分通过令
		 * 初始 carry 为 1 来实现。如果你不认为整数运算采用二进制补码形式，那也
		 * 只能如此了。
		 */
		int			nb = ip_addrsize(ip);
		int			byte = 0;
		unsigned char *pip = ip_addr(ip);
		unsigned char *pip2 = ip_addr(ip2);
		int			carry = 1;

		while (--nb >= 0)
		{
			int			lobyte;

			carry = pip[nb] + (~pip2[nb] & 0xFF) + carry;
			lobyte = carry & 0xFF;
			if (byte < sizeof(int64))
			{
				res |= ((int64) lobyte) << (byte * 8);
			}
			else
			{
				/*
				 * 输入宽于 int64：检查溢出。在能容纳的范围左侧的所有字节，应根据
				 * 当前已完成结果的符号，分别为 0 或 0xFF。
				 */
				if ((res < 0) ? (lobyte != 0xFF) : (lobyte != 0))
					ereport(ERROR,
							(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
							 errmsg("result is out of range")));
			}
			carry >>= 8;
			byte++;
		}

		/*
		 * 如果输入窄于 int64，则不可能溢出，但我们必须做正确的符号扩展。
		 */
		if (carry == 0 && byte < sizeof(int64))
			res |= ((uint64) (int64) -1) << (byte * 8);
	}

	PG_RETURN_INT64(res);
}


/*
 * clean_ipv6_addr --- 从 IPv6 地址字符串中移除任何 '%zone' 部分
 *
 * XXX 这终有一天应该被移除！
 *
 * 这是一个权宜之计，因为我们尚未在存储的 inet 值中支持 zone。由于
 * getnameinfo() 的结果可能包含 zone 说明，因此在任何要将 getnameinfo() 输出
 * 喂给 network_in 的地方都调用本函数来移除它。总好过完全失败。
 *
 * 另一种做法是让 network_in 自己忽略 %-部分，但那意味着我们会静默丢弃用户
 * 输入中的 zone 说明，这似乎不是个好主意。
 */
void
clean_ipv6_addr(int addr_family, char *addr)
{
	if (addr_family == AF_INET6)
	{
		char	   *pct = strchr(addr, '%');

		if (pct)
			*pct = '\0';
	}
}
