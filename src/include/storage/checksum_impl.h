/*-------------------------------------------------------------------------
 *
 * checksum_impl.h
 *	  数据页的校验和实现。
 *
 * 本文件的存在是为了方便可能希望检查 Postgres 页面校验和的外部程序。
 * 它们可以 #include 本文件以获得 storage/checksum.h 所引用的代码。
 * （注意：在外部编译时，你可能需要将 Assert() 重定义为空以成功编译。）
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/checksum_impl.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * 用于计算页面校验和的算法是以极快的计算速度为目标而选择的。
 * 当数据库工作集能放入操作系统文件缓存但放不进共享缓冲区时，
 * 可以以极快的速度读入页面，此时校验和算法本身可能成为最大的瓶颈。
 *
 * 校验和算法本身基于 FNV-1a 哈希（FNV 是 Fowler/Noll/Vo 的缩写）。
 * 普通 FNV-1a 哈希的原语每次按如下公式折叠 1 字节数据：
 *
 *	   hash = (hash ^ value) * FNV_PRIME
 *
 * FNV-1a 算法的说明见 http://www.isthe.com/chongo/tech/comp/fnv/
 *
 * PostgreSQL 并不直接使用 FNV-1a 哈希，因为它对高位混合不佳 —— 输入数据中的
 * 高位只会影响输出数据中的高位。为解决这个问题，我们在乘法之前将值右移
 * 17 位后与之异或。选择数字 17 是因为它与 FNV_PRIME 中置位的位置没有公约数，
 * 并且经验表明它能让最终迭代的高位比特快速"雪崩"扩散到低位。出于性能
 * 考虑，我们选择每次组合 4 字节。实际用作基础哈希公式是：
 *
 *	   hash = (hash ^ value) * FNV_PRIME ^ ((hash ^ value) >> 17)
 *
 * 此计算的主要瓶颈在于乘法延迟。为了隐藏延迟并利用 SIMD 并行性，会并行
 * 计算多个哈希值。页面被当作一个 32 列、元素为 32 位值的二维数组来处理。
 * 每一列被单独聚合为一个部分校验和。每个部分校验和使用不同的初始值
 * （FNV 术语中的 offset basis）。实际使用的初始值是随机选择的，因为这些
 * 值本身并不重要，重要的是它们彼此不同且不与真实数据中的任何值相同。
 * 在初始化部分校验和后，列中的每个值都按照上述公式进行聚合。最后再
 * 用值 0 额外执行两次公式迭代，以混合最后加入的值的比特。
 *
 * 然后这些部分校验和通过异或折叠在一起，形成一个单一的 32 位校验和。
 * 调用方可以安全地使用对 2^16-1 取模将其缩减为 16 位。这会导致对较小值
 * 有非常轻微的偏倚，但这对校验和的性能并不显著。
 *
 * 算法的选择基于 SIMD 指令集中可用的指令。这意味着一个快速且良好的算法
 * 需要以乘法作为主要混合算子。最简单的基于乘法的校验和原语就是 FNV 使用的
 * 那一个。所选的素数（prime）是为了让值有良好的分散性。它不存在已知会导致
 * 冲突的简单模式。对原语在 64 位键上进行的 5 位差分测试显示，在 100000 个
 * 随机键中没有 3 个或更多值发生差分冲突的情况。雪崩测试显示只有最后一个
 * 字的低位有偏倚。对 1-4 个不相关比特错误、杂散的 0 和 0xFF 字节、从随机
 * 位置到末尾用 0 字节覆盖页面、以及用 0x00、0xFF 和随机数据覆盖页面随机
 * 段落的测试，都显示出在误差范围内最优的 2e-16 误报率。
 *
 * 算法的向量化需要 32bit × 32bit -> 32bit 的整数乘法指令。截至 2013 年，
 * 相应的指令在 x86 的 SSE4.1 扩展（pmulld）和 ARM 的 NEON（vmul.i32）上
 * 可用。向量化需要编译器为我们完成向量化。对于较新的 GCC 版本，
 * -msse4.1 -funroll-loops -ftree-vectorize 这几个标志就足以实现向量化。
 *
 * 要使用的最佳并行度取决于 CPU 特定的指令延迟、SIMD 指令宽度、吞吐量以及
 * 可用于保存中间状态的寄存器数量。一般而言，更大的并行度更好，直到状态
 * 大到放不进寄存器、需要额外的 load-store 指令来换入换出值为止。所选的
 * 这个数字（并行度）是算法的一个固定部分，因为改变并行度会改变校验和结果。
 *
 * 并行度 32 的选择基于这样一个事实：它是能够放入架构可见的 x86 SSE 寄存器、
 * 同时还为中间值留出一些空闲寄存器的最大状态。对于未来带有 256 位向量寄存器
 * 的处理器，这会留下一些未被利用的性能。当向量化不可用时，将计算重构为
 * 每次计算一部分列并多次遍历以避免寄存器溢出可能是有益的。这个优化机会
 * 目前未被使用。当前的代码还假定编译器有能力展开内部循环以避免循环开销
 * 并最小化寄存器溢出。对于不够先进的编译器，手动展开内部循环可能更有益。
 */

#include "storage/bufpage.h"

/* 要并行计算的校验和数量 */
#define N_SUMS 32
/* FNV-1a 哈希的素数乘子 */
#define FNV_PRIME 16777619

/* 使用联合体，使本代码在严格别名规则下仍然有效 */
typedef union
{
	PageHeaderData phdr;
	uint32		data[BLCKSZ / (sizeof(uint32) * N_SUMS)][N_SUMS];
} PGChecksummablePage;

/*
 * 用于初始化各并行 FNV 哈希、使其处于不同初始状态的基准偏移量。
 */
static const uint32 checksumBaseOffsets[N_SUMS] = {
	0x5B1F36E9, 0xB8525960, 0x02AB50AA, 0x1DE66D2A,
	0x79FF467A, 0x9BB9F8A3, 0x217E7CD2, 0x83E13D2C,
	0xF8D4474F, 0xE39EB970, 0x42C6AE16, 0x993216FA,
	0x7B093B5D, 0x98DAFF3C, 0xF718902A, 0x0B1C9CDB,
	0xE58F764B, 0x187636BC, 0x5D7B3BB1, 0xE73DE7DE,
	0x92BEC979, 0xCCA6C0B2, 0x304A0979, 0x85AA43D4,
	0x783125BB, 0x6CA8EAA2, 0xE407EAC6, 0x4B5CFC3E,
	0x9FBF8C76, 0x15CA20BE, 0xF2CA9FD3, 0x959BD756
};

/*
 * 计算一轮校验和。
 */
#define CHECKSUM_COMP(checksum, value) \
do { \
	uint32 __tmp = (checksum) ^ (value); \
	(checksum) = __tmp * FNV_PRIME ^ (__tmp >> 17); \
} while (0)

/*
 * 块校验和算法。页面必须充分对齐（至少位于 4 字节边界上）。
 */
static uint32
pg_checksum_block(const PGChecksummablePage *page)
{
	uint32		sums[N_SUMS];
	uint32		result = 0;
	uint32		i,
				j;

	/* 确保大小与算法兼容 */
	Assert(sizeof(PGChecksummablePage) == BLCKSZ);

	/* 将部分校验和初始化为对应的偏移量 */
	memcpy(sums, checksumBaseOffsets, sizeof(checksumBaseOffsets));

	/* 主校验和计算 */
	for (i = 0; i < (uint32) (BLCKSZ / (sizeof(uint32) * N_SUMS)); i++)
		for (j = 0; j < N_SUMS; j++)
			CHECKSUM_COMP(sums[j], page->data[i][j]);

	/* 最后再额外加入两轮全零以增加混合 */
	for (i = 0; i < 2; i++)
		for (j = 0; j < N_SUMS; j++)
			CHECKSUM_COMP(sums[j], 0);

	/* 用异或将各部分校验和折叠在一起 */
	for (i = 0; i < N_SUMS; i++)
		result ^= sums[i];

	return result;
}

/*
 * 计算 Postgres 页面的校验和。
 *
 * 页面必须充分对齐（至少位于 4 字节边界上）。
 * 还要注意，页面的校验和字段会被临时置零。
 *
 * 校验和包含块号（用于检测页面被以某种方式移动到其他位置的情况）、
 * 页面头部（不包括校验和本身）以及页面数据。
 */
uint16
pg_checksum_page(char *page, BlockNumber blkno)
{
	PGChecksummablePage *cpage = (PGChecksummablePage *) page;
	uint16		save_checksum;
	uint32		checksum;

	/* 我们只对正确初始化过的页面计算校验和 */
	Assert(!PageIsNew((Page) page));

	/*
	 * 保存 pd_checksum 并将其临时置零，这样校验和计算就不会受到
	 * 页面上存储的旧校验和的影响。之后将其恢复，因为实际更新校验和
	 * 并不属于本函数的 API 职责范围。
	 */
	save_checksum = cpage->phdr.pd_checksum;
	cpage->phdr.pd_checksum = 0;
	checksum = pg_checksum_block(cpage);
	cpage->phdr.pd_checksum = save_checksum;

	/* 混入块号以检测页面被错置的情况 */
	checksum ^= blkno;

	/*
	 * 缩减为 uint16（以放入 pd_checksum 字段），并加 1 作为偏移量。
	 * 这样可以避免校验和为零，这似乎是个好主意。
	 */
	return (uint16) ((checksum % 65535) + 1);
}
