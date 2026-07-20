/*-------------------------------------------------------------------------
 *
 * timestamp.h
 *		Timestamp 与 Interval 的类型定义及相关宏。
 *
 * 注意：此文件必须能够在前端和后端两种上下文中被包含。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/datatype/timestamp.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DATATYPE_TIMESTAMP_H
#define DATATYPE_TIMESTAMP_H

/*
 * Timestamp 表示绝对时间。
 *
 * Interval 表示时间增量（delta time）。分别记录月（及年）、天，
 * 以及时/分/秒，因为在相对于某个绝对时间实例化之前，
 * 所跨越的已用时间是未知的。
 *
 * 注意，Postgres 中 "time interval" 指的是有界区间，
 * 即由起始和结束时间组成的区间，而非时间跨度 - thomas 97/03/20
 *
 * 时间戳，以及 interval 的时/分/秒字段，都以
 * int64 值存储，单位为微秒。（以前它们曾是
 * 以秒为单位的 double 值。）
 *
 * TimeOffset 和 fsec_t 是方便临时变量使用的类型定义。
 * 不要在磁盘存储的值中使用 fsec_t。
 * 此外，fsec_t 只用于*小数*秒；如果要存储的值可能达到很多秒，
 * 请小心溢出问题。
 */

typedef int64 Timestamp;
typedef int64 TimestampTz;
typedef int64 TimeOffset;
typedef int32 fsec_t;			/* 小数秒（单位：微秒） */


/*
 * 类型 interval 的存储格式。
 */
typedef struct
{
	TimeOffset	time;			/* 除天、月、年之外的所有时间单位 */
	int32		day;			/* 天，置于 time 之后以对齐 */
	int32		month;			/* 月和年，置于 time 之后以对齐 */
} Interval;

/*
 * 表示拆解后 interval 的数据结构。
 *
 * 出于历史原因，它的结构仿照时间戳使用的 struct pg_tm。
 * 与时间戳的情况不同，月和年不需要特殊解释：
 * 它们要么为零要么不为零。注意各字段
 * 可以为负；然而，由于在从 struct Interval 转换时进行了除法，
 * 只有 tm_mday 可能为 INT_MIN。这一点很重要，
 * 因为我们可能需要在某些代码路径中对这些值取负。
 */
struct pg_itm
{
	int			tm_usec;
	int			tm_sec;
	int			tm_min;
	int64		tm_hour;		/* 需要较宽的类型 */
	int			tm_mday;
	int			tm_mon;
	int			tm_year;
};

/*
 * 用于解码 interval 的数据结构。我们本可以直接使用 struct pg_itm，
 * 但那样 tm_usec 必须为 64 位的要求就会传播到
 * 并不需要它的地方。此外，省略解码过程中
 * 用不到的字段似乎是一种不错的防错措施。
 */
struct pg_itm_in
{
	int64		tm_usec;		/* 需要较宽的类型 */
	int			tm_mday;
	int			tm_mon;
	int			tm_year;
};


/* 这些数据类型 "precision"（精度）选项（typmod）的上限 */
#define MAX_TIMESTAMP_PRECISION 6
#define MAX_INTERVAL_PRECISION 6

/*
 *	舍入到 MAX_TIMESTAMP_PRECISION 位小数。
 *	注意：这也用于 interval 的舍入。
 */
#define TS_PREC_INV 1000000.0
#define TSROUND(j) (rint(((double) (j)) * TS_PREC_INV) / TS_PREC_INV)


/*
 * 用于日期时间相关计算的各种常量
 */

#define DAYS_PER_YEAR	365.25	/* 假设每四年一个闰年 */
#define MONTHS_PER_YEAR 12
/*
 *	DAYS_PER_MONTH 非常不精确。更精确的值是
 *	365.2425/12 = 30.436875，即 '30 天 10:29:06'。目前我们
 *	只返回整数天，但将来也许应该
 *	同时返回一个 'time' 值。ISO 8601 建议
 *	使用 30 天。
 */
#define DAYS_PER_MONTH	30		/* 假设每月正好 30 天 */
#define DAYS_PER_WEEK	7
#define HOURS_PER_DAY	24		/* 假设没有夏令时变化 */

/*
 *	这没有对不均匀的夏令时区间或闰秒进行调整，
 *	并且粗略估计闰年。更精确的取值是每年
 *	365.2422 天。
 */
#define SECS_PER_YEAR	(36525 * 864)	/* 避免浮点运算 */
#define SECS_PER_DAY	86400
#define SECS_PER_HOUR	3600
#define SECS_PER_MINUTE 60
#define MINS_PER_HOUR	60

#define USECS_PER_DAY	INT64CONST(86400000000)
#define USECS_PER_HOUR	INT64CONST(3600000000)
#define USECS_PER_MINUTE INT64CONST(60000000)
#define USECS_PER_SEC	INT64CONST(1000000)

/*
 * 我们允许的数字时区偏移量，东西方向最多到距格林尼治 15:59:59。
 * 目前，实际使用中最离谱的偏移记录保持者是时区
 * Asia/Manila（直到 1844 年为 -15:56:08）和 America/Metlakatla（直到 1867 年为 +15:13:42）。
 * 如果我们拒绝这些值，就会无法转储和恢复
 * 使用这些时区设置的旧 timestamptz 值。
 */
#define MAX_TZDISP_HOUR		15	/* 允许的最大小时部分 */
#define TZDISP_LIMIT		((MAX_TZDISP_HOUR + 1) * SECS_PER_HOUR)

/*
 * 我们保留最小和最大整数值，用于表示
 * timestamp（或 timestamptz）的 -infinity 与 +infinity。
 */
#define TIMESTAMP_MINUS_INFINITY	PG_INT64_MIN
#define TIMESTAMP_INFINITY	PG_INT64_MAX

/*
 * 历史上曾使用这些表示无穷的别名。
 */
#define DT_NOBEGIN		TIMESTAMP_MINUS_INFINITY
#define DT_NOEND		TIMESTAMP_INFINITY

#define TIMESTAMP_NOBEGIN(j)	\
	do {(j) = DT_NOBEGIN;} while (0)

#define TIMESTAMP_IS_NOBEGIN(j) ((j) == DT_NOBEGIN)

#define TIMESTAMP_NOEND(j)		\
	do {(j) = DT_NOEND;} while (0)

#define TIMESTAMP_IS_NOEND(j)	((j) == DT_NOEND)

#define TIMESTAMP_NOT_FINITE(j) (TIMESTAMP_IS_NOBEGIN(j) || TIMESTAMP_IS_NOEND(j))

/*
 * 无限 interval 通过将所有字段设为最小或
 * 最大整数值来表示。
 */
#define INTERVAL_NOBEGIN(i)	\
	do {	\
		(i)->time = PG_INT64_MIN;	\
		(i)->day = PG_INT32_MIN;	\
		(i)->month = PG_INT32_MIN;	\
	} while (0)

#define INTERVAL_IS_NOBEGIN(i)	\
	((i)->month == PG_INT32_MIN && (i)->day == PG_INT32_MIN && (i)->time == PG_INT64_MIN)

#define INTERVAL_NOEND(i)	\
	do {	\
		(i)->time = PG_INT64_MAX;	\
		(i)->day = PG_INT32_MAX;	\
		(i)->month = PG_INT32_MAX;	\
	} while (0)

#define INTERVAL_IS_NOEND(i)	\
	((i)->month == PG_INT32_MAX && (i)->day == PG_INT32_MAX && (i)->time == PG_INT64_MAX)

#define INTERVAL_NOT_FINITE(i) (INTERVAL_IS_NOBEGIN(i) || INTERVAL_IS_NOEND(i))

/*
 * 儒略日（Julian date）支持。
 *
 * date2j() 和 j2date() 名义上处理 0..INT_MAX 范围内的儒略日，
 * 即公元前 4714-11-24 到公元 5874898-06-03。实际上，date2j() 在
 * 公元前 4714-11-24 之前的日期上也能工作并给出正确的负儒略日。
 * 我们依赖它在公元前 4714-11-01 之前也能如此工作。允许至少一天的
 * 余量是有必要的，这样时间戳轮换就不会产生
 * 在输入时会被拒绝的日期。例如，'4714-11-24 00:00 GMT BC' 是
 * 一个合法的 timestamptz 值，但在格林尼治以东的时区中，它会显示为
 * 公元前 4714-11-23 下午的某个时刻；如果我们无法处理这样的
 * 日期，就会出现转储/重载失败。因此，思路是让 IS_VALID_JULIAN
 * 接受比我们实际支持范围稍宽的日期，然后
 * 我们再通过 IS_VALID_DATE 或 IS_VALID_TIMESTAMP 做精确检查，
 * 如有需要时区轮换则在轮换之后。为了节省几个周期，我们可以让
 * IS_VALID_JULIAN 只检查到月份边界，因为其精确的截止值
 * 在此方案中不很关键。
 *
 * JULIAN_MINYEAR 为 -4713 而非 -4714 是正确的；它的定义是为了
 * 便于与 tm_year 值比较，在 tm_year 中我们遵循约定：
 * tm_year <= 0 表示 abs(tm_year)+1 个公元前年份。
 */

#define JULIAN_MINYEAR (-4713)
#define JULIAN_MINMONTH (11)
#define JULIAN_MINDAY (24)
#define JULIAN_MAXYEAR (5874898)
#define JULIAN_MAXMONTH (6)
#define JULIAN_MAXDAY (3)

#define IS_VALID_JULIAN(y,m,d) \
	(((y) > JULIAN_MINYEAR || \
	  ((y) == JULIAN_MINYEAR && ((m) >= JULIAN_MINMONTH))) && \
	 ((y) < JULIAN_MAXYEAR || \
	  ((y) == JULIAN_MAXYEAR && ((m) < JULIAN_MAXMONTH))))

/* Unix 与 Postgres 纪元中第 0 天对应的儒略日 */
#define UNIX_EPOCH_JDATE		2440588 /* == date2j(1970, 1, 1) */
#define POSTGRES_EPOCH_JDATE	2451545 /* == date2j(2000, 1, 1) */

/*
 * 日期与时间戳的范围限制。
 *
 * 传统上我们允许儒略日零作为一个合法的日期时间值，
 * 因此它是日期和时间戳共同的下界。
 *
 * 日期的上限是 5874897-12-31，比儒略日代码
 * 所能允许的略小。对于时间戳，上限是
 * 294276-12-31。int64 溢出上限会晚几天；同样，
 * 留一些余量可避免对边角情况下溢出的担忧，并提供一个
 * 对用户更简单的可见定义。
 */

/* 以儒略日形式表示的第一个允许日期与第一个不允许日期 */
#define DATETIME_MIN_JULIAN (0)
#define DATE_END_JULIAN (2147483494)	/* == date2j(JULIAN_MAXYEAR, 1, 1) */
#define TIMESTAMP_END_JULIAN (109203528)	/* == date2j(294277, 1, 1) */

/* 时间戳限制 */
#define MIN_TIMESTAMP	INT64CONST(-211813488000000000)
/* == (DATETIME_MIN_JULIAN - POSTGRES_EPOCH_JDATE) * USECS_PER_DAY */
#define END_TIMESTAMP	INT64CONST(9223371331200000000)
/* == (TIMESTAMP_END_JULIAN - POSTGRES_EPOCH_JDATE) * USECS_PER_DAY */

/* 检查日期范围（给定的是 Postgres 编号，非儒略日编号） */
#define IS_VALID_DATE(d) \
	((DATETIME_MIN_JULIAN - POSTGRES_EPOCH_JDATE) <= (d) && \
	 (d) < (DATE_END_JULIAN - POSTGRES_EPOCH_JDATE))

/* 检查时间戳范围 */
#define IS_VALID_TIMESTAMP(t)  (MIN_TIMESTAMP <= (t) && (t) < END_TIMESTAMP)

#endif							/* DATATYPE_TIMESTAMP_H */
