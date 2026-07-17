/*-------------------------------------------------------------------------
 *
 * inet.h
 *	  声明对 INET 数据类型的操作。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/inet.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef INET_H
#define INET_H

#include "fmgr.h"

/*
 *	这是 IP 地址（包含 INET 和 CIDR 两种数据类型）的内部存储格式：
 */
typedef struct
{
	unsigned char family;		/* PGSQL_AF_INET or PGSQL_AF_INET6 */
	unsigned char bits;			/* 子网掩码中的位数 */
	unsigned char ipaddr[16];	/* 地址最多 128 位 */
} inet_struct;

/*
 * 这些取值用于 "family" 字段。
 *
 * 将非 AF_INET 类型都映射到 AF_INET，可使我们在缺少相应地址族（例如不存在
 * AF_INET6 时的 inet6 地址）的机器上也能正常工作，且不会引发转储/重载的
 * 需求。7.4 之前的数据库在磁盘上用 AF_INET 作为 family 类型。
 */
#define PGSQL_AF_INET	(AF_INET + 0)
#define PGSQL_AF_INET6	(AF_INET + 1)

/*
 * 在 Postgres 中，INET 和 CIDR 地址都以 varlena 对象表示，即上述结构体类型
 * 之前有一个 varlena 头部。该结构体描述的是在"未压缩"情况下我们内存中实际
 * 的内容。注意，由于最大数据尺寸仅 18 字节，INET/CIDR 总是会以 1 字节头的
 * varlena 格式存入元组。但我们仍需能应对 4 字节头的格式，因为各种代码可能
 * 会"好心"地对 1 字节头的 datum 进行"解压"。
 */
typedef struct
{
	char		vl_len_[4];		/* 不要直接修改此字段！ */
	inet_struct inet_data;
} inet;

/*
 *	访问宏。我们使用 VARDATA_ANY，以便在不 detoast 的情况下处理短头 varlena
 *	值。这需要一点技巧：VARDATA_ANY 假定 varlena 头部已经填好，而在构造新值
 *	时并非如此（直到调用 SET_INET_VARSIZE，而我们通常要到最后才能调用）。
 *	因此，我们总是把新分配的值初始化为零（使用 palloc0）。全零的长度字在
 *	VARDATA_ANY 看来就像非 1 字节的情况，从而能正确构造出未压缩的值。
 *
 *	注意，ip_addrsize()、ip_maxbits() 和 SET_INET_VARSIZE() 都要求 family
 *	字段被正确设置。
 */
#define ip_family(inetptr) \
	(((inet_struct *) VARDATA_ANY(inetptr))->family)

#define ip_bits(inetptr) \
	(((inet_struct *) VARDATA_ANY(inetptr))->bits)

#define ip_addr(inetptr) \
	(((inet_struct *) VARDATA_ANY(inetptr))->ipaddr)

#define ip_addrsize(inetptr) \
	(ip_family(inetptr) == PGSQL_AF_INET ? 4 : 16)

#define ip_maxbits(inetptr) \
	(ip_family(inetptr) == PGSQL_AF_INET ? 32 : 128)

#define SET_INET_VARSIZE(dst) \
	SET_VARSIZE(dst, VARHDRSZ + offsetof(inet_struct, ipaddr) + \
				ip_addrsize(dst))


/*
 *	这是 MAC 地址的内部存储格式：
 */
typedef struct macaddr
{
	unsigned char a;
	unsigned char b;
	unsigned char c;
	unsigned char d;
	unsigned char e;
	unsigned char f;
} macaddr;

/*
 *	这是 MAC8 地址的内部存储格式：
 */
typedef struct macaddr8
{
	unsigned char a;
	unsigned char b;
	unsigned char c;
	unsigned char d;
	unsigned char e;
	unsigned char f;
	unsigned char g;
	unsigned char h;
} macaddr8;

/*
 * fmgr 接口宏
 */
static inline inet *
DatumGetInetPP(Datum X)
{
	return (inet *) PG_DETOAST_DATUM_PACKED(X);
}

static inline Datum
InetPGetDatum(const inet *X)
{
	return PointerGetDatum(X);
}

#define PG_GETARG_INET_PP(n) DatumGetInetPP(PG_GETARG_DATUM(n))
#define PG_RETURN_INET_P(x) return InetPGetDatum(x)

/* 过时变体 */
static inline inet *
DatumGetInetP(Datum X)
{
	return (inet *) PG_DETOAST_DATUM(X);
}
#define PG_GETARG_INET_P(n) DatumGetInetP(PG_GETARG_DATUM(n))

/* macaddr 是定长、按引用传递的数据类型 */
static inline macaddr *
DatumGetMacaddrP(Datum X)
{
	return (macaddr *) DatumGetPointer(X);
}

static inline Datum
MacaddrPGetDatum(const macaddr *X)
{
	return PointerGetDatum(X);
}

#define PG_GETARG_MACADDR_P(n) DatumGetMacaddrP(PG_GETARG_DATUM(n))
#define PG_RETURN_MACADDR_P(x) return MacaddrPGetDatum(x)

/* macaddr8 是定长、按引用传递的数据类型 */
static inline macaddr8 *
DatumGetMacaddr8P(Datum X)
{
	return (macaddr8 *) DatumGetPointer(X);
}

static inline Datum
Macaddr8PGetDatum(const macaddr8 *X)
{
	return PointerGetDatum(X);
}

#define PG_GETARG_MACADDR8_P(n) DatumGetMacaddr8P(PG_GETARG_DATUM(n))
#define PG_RETURN_MACADDR8_P(x) return Macaddr8PGetDatum(x)

/*
 * network.c 中的支持函数
 */
extern inet *cidr_set_masklen_internal(const inet *src, int bits);
extern int	bitncmp(const unsigned char *l, const unsigned char *r, int n);
extern int	bitncommon(const unsigned char *l, const unsigned char *r, int n);

#endif							/* INET_H */
