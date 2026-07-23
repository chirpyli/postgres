/*-------------------------------------------------------------------------
 *
 * heaptuple.c
 *	  本文件包含堆元组（heap tuple）的访问与修改例程，以及
 *	  各类元组工具函数。
 *
 * 关于 varlena 与本代码的一些说明：
 *
 * 在 Postgres 8.3 之前，varlena 始终带有 4 字节长度头，因此
 * （至少）始终需要 4 字节对齐。这对于较短的 varlena 来说很浪费空间，
 * 例如 CHAR(1) 会占用 5 字节，并且为了对齐可能还需要最多
 * 3 个额外的填充字节。
 *
 * 如今，较短的 varlena（最多 126 个数据字节）会被缩减为 1 字节头，
 * 并且我们不对齐它。为了让那些不希望处理这种情况的数据类型相关函数
 * 无感知，这样的 datum 被视为“已 TOAST”，并将由 pg_detoast_datum
 * 扩展回正常的 4 字节头格式。（在性能关键的代码路径中，我们可以使用
 * pg_detoast_datum_packed 以及相应的访问宏来避免这一开销。）注意，
 * 这一转换直接在 heap_form_tuple 中完成，而不调用 heaptoast.c。
 *
 * 这一改动会破坏那些假设“放入元组但从未写入磁盘的值无需 detoast”
 * 的代码。希望这类地方很少。
 *
 * 在 pg_type/pg_attribute 中，varlena 仍然具有 INT（或 DOUBLE）对齐，
 * 因为这是未 TOAST 格式的正常要求。但对于 1 字节头格式我们忽略这一点。
 * 这意味着 varlena datum 的实际起始位置可能会因其所属格式而不同。为了
 * 确定实际存储的内容，我们必须要求对齐填充字节为零。（Postgres 实际上
 * 一直都是将它们置零的，但现在这是必须的！）由于 1 字节头 varlena 的
 * 第一个字节永远不会是零，我们可以检查前一个 datum 之后的第一个字节，
 * 从而判断它到底是填充字节还是 1 字节头 varlena 的起始。
 *
 * 请注意，过去我们可以依赖系统目录的第一个 varlena 列位于该目录
 * 对应的 C 结构体所指示的偏移处，但现在这样做是有风险的：只有当
 * 前一个字段是字对齐（word-aligned）时才是安全的，这样就不会存在
 * 任何填充。
 *
 * 我们不会对 attstorage 为 PLAIN 的 varlena 进行压缩打包，因为该数据类型
 * 并不预期需要 detoast 值。这一点尤其被 oidvector 和 int2vector 所使用，
 * 它们出现在系统目录中，而我们仍希望通过 C 结构体偏移来引用它们。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/heaptuple.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heaptoast.h"
#include "access/sysattr.h"
#include "access/tupdesc_details.h"
#include "common/hashfn.h"
#include "utils/datum.h"
#include "utils/expandeddatum.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"


/*
 * att 的数据类型是否允许压缩打包为 1 字节头的 varlena 格式？
 * 使用 TupleDescAttr() 并将 attstorage 赋值为
 * TYPSTORAGE_PLAIN 的函数不能使用压缩的 varlena 头，而调用
 * TupleDescInitEntry() 的函数使用 typeForm->typstorage
 * (TYPSTORAGE_EXTENDED)，因此可以使用压缩的 varlena 头，例如：
 *     CREATE TABLE test(a VARCHAR(10000) STORAGE PLAIN);
 *     INSERT INTO test VALUES (repeat('A',10));
 * 这一点可以用 pageinspect 验证。
 */
#define ATT_IS_PACKABLE(att) \
	((att)->attlen == -1 && (att)->attstorage != TYPSTORAGE_PLAIN)
/* 如果已经确定是 varlena，则使用此宏 */
#define VARLENA_ATT_IS_PACKABLE(att) \
	((att)->attstorage != TYPSTORAGE_PLAIN)

/* FormData_pg_attribute.attstorage != TYPSTORAGE_PLAIN 且 attlen 为 -1 */
#define COMPACT_ATTR_IS_PACKABLE(att) \
	((att)->attlen == -1 && (att)->attispackable)

/*
 * 以一种能够经受住 tupleDesc 销毁的方式，缓存按引用传递（pass-by-ref）
 * 的缺失属性。
 */

typedef struct
{
	int			len;
	Datum		value;
} missing_cache_key;

static HTAB *missing_cache = NULL;

static uint32
missing_hash(const void *key, Size keysize)
{
	const missing_cache_key *entry = (missing_cache_key *) key;

	return hash_bytes((const unsigned char *) entry->value, entry->len);
}

static int
missing_match(const void *key1, const void *key2, Size keysize)
{
	const missing_cache_key *entry1 = (missing_cache_key *) key1;
	const missing_cache_key *entry2 = (missing_cache_key *) key2;

	if (entry1->len != entry2->len)
		return entry1->len > entry2->len ? 1 : -1;

	return memcmp(DatumGetPointer(entry1->value),
				  DatumGetPointer(entry2->value),
				  entry1->len);
}

static void
init_missing_cache()
{
	HASHCTL		hash_ctl;

	hash_ctl.keysize = sizeof(missing_cache_key);
	hash_ctl.entrysize = sizeof(missing_cache_key);
	hash_ctl.hcxt = TopMemoryContext;
	hash_ctl.hash = missing_hash;
	hash_ctl.match = missing_match;
	missing_cache =
		hash_create("Missing Values Cache",
					32,
					&hash_ctl,
					HASH_ELEM | HASH_CONTEXT | HASH_FUNCTION | HASH_COMPARE);
}

/* ----------------------------------------------------------------
 *						杂项支持例程
 * ----------------------------------------------------------------
 */

/*
 * 返回某个属性的缺失值；若没有缺失值，则返回 NULL。
 */
Datum
getmissingattr(TupleDesc tupleDesc,
			   int attnum, bool *isnull)
{
	CompactAttribute *att;

	Assert(attnum <= tupleDesc->natts);
	Assert(attnum > 0);

	att = TupleDescCompactAttr(tupleDesc, attnum - 1);

	if (att->atthasmissing)
	{
		AttrMissing *attrmiss;

		Assert(tupleDesc->constr);
		Assert(tupleDesc->constr->missing);

		attrmiss = tupleDesc->constr->missing + (attnum - 1);

		if (attrmiss->am_present)
		{
			missing_cache_key key;
			missing_cache_key *entry;
			bool		found;
			MemoryContext oldctx;

			*isnull = false;

			/* 无需缓存按值传递（by-value）的属性 */
			if (att->attbyval)
				return attrmiss->am_value;

			/* 必要时建立缓存 */
			if (missing_cache == NULL)
				init_missing_cache();

			/* 检查是否已存在缓存项 */
			Assert(att->attlen > 0 || att->attlen == -1);
			if (att->attlen > 0)
				key.len = att->attlen;
			else
				key.len = VARSIZE_ANY(attrmiss->am_value);
			key.value = attrmiss->am_value;

			entry = hash_search(missing_cache, &key, HASH_ENTER, &found);

			if (!found)
			{
				/* 缓存未命中，因此我们需要该 datum 的一个非临时副本 */
				oldctx = MemoryContextSwitchTo(TopMemoryContext);
				entry->value =
					datumCopy(attrmiss->am_value, false, att->attlen);
				MemoryContextSwitchTo(oldctx);
			}

			return entry->value;
		}
	}

	*isnull = true;
	return PointerGetDatum(NULL);
}

/*
 * heap_compute_data_size
 *		确定待构造元组的数据区大小
 */
Size
heap_compute_data_size(TupleDesc tupleDesc,
					   const Datum *values,
					   const bool *isnull)
{
	Size		data_length = 0;
	int			i;
	int			numberOfAttributes = tupleDesc->natts;

	for (i = 0; i < numberOfAttributes; i++)
	{
		Datum		val;
		CompactAttribute *atti;

		if (isnull[i])
			continue;

		val = values[i];
		atti = TupleDescCompactAttr(tupleDesc, i);

		if (COMPACT_ATTR_IS_PACKABLE(atti) &&
			VARATT_CAN_MAKE_SHORT(DatumGetPointer(val)))
		{
			/*
			 * 我们预期要将其转为短 varlena 头，因此
			 * 调整长度并且不计入任何对齐
			 */
			data_length += VARATT_CONVERTED_SHORT_SIZE(DatumGetPointer(val));
		}
		else if (atti->attlen == -1 &&
				 VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(val)))
		{
		/*
		 * 我们希望展平该扩展值，使得
		 * 构造出的元组不依赖于它
		 */
			data_length = att_nominal_alignby(data_length, atti->attalignby);
			data_length += EOH_get_flat_size(DatumGetEOHP(val));
		}
		else
		{
			data_length = att_datum_alignby(data_length, atti->attalignby,
											atti->attlen, val);
			data_length = att_addlength_datum(data_length, atti->attlen,
											  val);
		}
	}

	return data_length;
}

/*
 * heap_fill_tuple 及其他构建元组例程所用的每属性辅助函数。
 *
 * 填入一个数据值，或填入空值位图中的某一位
 */
static inline void
fill_val(CompactAttribute *att,
		 bits8 **bit,
		 int *bitmask,
		 char **dataP,
		 uint16 *infomask,
		 Datum datum,
		 bool isnull)
{
	Size		data_length;
	char	   *data = *dataP;

	/*
	 * 如果我们正在构建空值位图，则在此处为
		 * 当前列值设置相应的位。
	 */
	if (bit != NULL)
	{
		if (*bitmask != HIGHBIT)
			*bitmask <<= 1;
		else
		{
			*bit += 1;
			**bit = 0x0;
			*bitmask = 1;
		}

		if (isnull)
		{
			*infomask |= HEAP_HASNULL;
			return;
		}

		**bit |= *bitmask;
	}

	/*
	 * XXX 我们在指针值本身（而非偏移量）上使用 att_nominal_alignby 宏。
		 * 这有点像个 hack。
	 */
	if (att->attbyval)
	{
		/* 按值传递 */
		data = (char *) att_nominal_alignby(data, att->attalignby);
		store_att_byval(data, datum, att->attlen);
		data_length = att->attlen;
	}
	else if (att->attlen == -1)
	{
		/* varlena 变长类型 */
		Pointer		val = DatumGetPointer(datum);

		*infomask |= HEAP_HASVARWIDTH;
		if (VARATT_IS_EXTERNAL(val))
		{
			if (VARATT_IS_EXTERNAL_EXPANDED(val))
			{
				/*
				 * 我们希望展平该扩展值，使得
				 * 构造出的元组不依赖于它
				 */
				ExpandedObjectHeader *eoh = DatumGetEOHP(datum);

				data = (char *) att_nominal_alignby(data, att->attalignby);
				data_length = EOH_get_flat_size(eoh);
				EOH_flatten_into(eoh, data, data_length);
			}
			else
			{
				*infomask |= HEAP_HASEXTERNAL;
				/* 无需对齐，因为按定义它就是短的 */
				data_length = VARSIZE_EXTERNAL(val);
				memcpy(data, val, data_length);
			}
		}
		else if (VARATT_IS_SHORT(val))
		{
			/* 短 varlena 无需对齐 */
			data_length = VARSIZE_SHORT(val);
			memcpy(data, val, data_length);
		}
		else if (att->attispackable && VARATT_CAN_MAKE_SHORT(val))
		{
			/* 转为短 varlena —— 无需对齐 */
			data_length = VARATT_CONVERTED_SHORT_SIZE(val);
			SET_VARSIZE_SHORT(data, data_length);
			memcpy(data + 1, VARDATA(val), data_length - 1);
		}
		else
		{
			/* 完整 4 字节头的 varlena */
			data = (char *) att_nominal_alignby(data, att->attalignby);
			data_length = VARSIZE(val);
			memcpy(data, val, data_length);
		}
	}
	else if (att->attlen == -2)
	{
		/* cstring …… 永远不需要对齐 */
		*infomask |= HEAP_HASVARWIDTH;
		Assert(att->attalignby == sizeof(char));
		data_length = strlen(DatumGetCString(datum)) + 1;
		memcpy(data, DatumGetPointer(datum), data_length);
	}
	else
	{
		/* 定长、按引用传递 */
		data = (char *) att_nominal_alignby(data, att->attalignby);
		Assert(att->attlen > 0);
		data_length = att->attlen;
		memcpy(data, DatumGetPointer(datum), data_length);
	}

	data += data_length;
	*dataP = data;
}

/*
 * heap_fill_tuple
 *		从 values/isnull 数组载入元组的数据部分
 *
 * 我们还会填充空值位图（若有），并设置反映元组数据内容的
 * infomask 位。
 *
 * 注意：现在要求调用方必须预先将数据区置零。
 */
void
heap_fill_tuple(TupleDesc tupleDesc,
				const Datum *values, const bool *isnull,
				char *data, Size data_size,
				uint16 *infomask, bits8 *bit)
{
	bits8	   *bitP;
	int			bitmask;
	int			i;
	int			numberOfAttributes = tupleDesc->natts;

#ifdef USE_ASSERT_CHECKING
	char	   *start = data;
#endif

	if (bit != NULL)
	{
		bitP = &bit[-1];
		bitmask = HIGHBIT;
	}
	else
	{
		/* 仅为了让编译器保持安静（消除告警） */
		bitP = NULL;
		bitmask = 0;
	}

	*infomask &= ~(HEAP_HASNULL | HEAP_HASVARWIDTH | HEAP_HASEXTERNAL);

	for (i = 0; i < numberOfAttributes; i++)
	{
		CompactAttribute *attr = TupleDescCompactAttr(tupleDesc, i);

		fill_val(attr,
				 bitP ? &bitP : NULL,
				 &bitmask,
				 &data,
				 infomask,
				 values ? values[i] : PointerGetDatum(NULL),
				 isnull ? isnull[i] : true);
	}

	Assert((data - start) == data_size);
}


/* ----------------------------------------------------------------
 *						heap tuple interface
 * ----------------------------------------------------------------
 */

/* ----------------
 *		heap_attisnull	- 当元组属性不存在时返回真
 * ----------------
 */
bool
heap_attisnull(HeapTuple tup, int attnum, TupleDesc tupleDesc)
{
	/*
	 * 我们允许传入 NULL 的 tupledesc，用于那些预期不含缺失值的
	 * 关系，例如系统目录关系和索引。
	 */
	Assert(!tupleDesc || attnum <= tupleDesc->natts);
	if (attnum > (int) HeapTupleHeaderGetNatts(tup->t_data))
	{
		if (tupleDesc &&
			TupleDescCompactAttr(tupleDesc, attnum - 1)->atthasmissing)
			return false;
		else
			return true;
	}

	if (attnum > 0)
	{
		if (HeapTupleNoNulls(tup))
			return false;
		return att_isnull(attnum - 1, tup->t_data->t_bits);
	}

	switch (attnum)
	{
		case TableOidAttributeNumber:
		case SelfItemPointerAttributeNumber:
		case MinTransactionIdAttributeNumber:
		case MinCommandIdAttributeNumber:
		case MaxTransactionIdAttributeNumber:
		case MaxCommandIdAttributeNumber:
			/* 这些永远不会为 NULL */
			break;

		default:
			elog(ERROR, "invalid attnum: %d", attnum);
	}

	return false;
}

/* ----------------
 *		nocachegetattr
 *
 *		本函数仅在 fastgetattr() 中、于我们无法使用 cacheoffset
 *		且值不为 NULL 的情况下被调用。
 *
 *		本函数将属性偏移量缓存在属性描述符中。
 *
 *		另一种加速方式是把偏移量缓存在元组里，但那样似乎更麻烦，
 *		除非你愿意承受把那些偏移量真正放进写入磁盘的元组所带来的
 *		存储开销。呸。
 *
 *		该方案会比那种方式稍慢一些，但对于命中大量元组的查询应当
 *		表现良好。一旦你缓存了偏移量，用同一属性描述符检查其余所有
 *		元组就会快得多。-cim 5/4/91
 *
 *		注意：如果你需要修改本代码，也请参见 heap_deform_tuple。
 *		另请参见 nocache_index_getattr，那是针对索引元组的相同代码。
 * ----------------
 */
Datum
nocachegetattr(HeapTuple tup,
			   int attnum,
			   TupleDesc tupleDesc)
{
	HeapTupleHeader td = tup->t_data;
	char	   *tp;				/* 元组数据部分的指针 */
	bits8	   *bp = td->t_bits;	/* 元组中空值位图的指针 */
	bool		slow = false;	/* 是否需要遍历各属性？ */
	int			off;			/* 数据区内的当前偏移量 */

	/* ----------------
	 *	 三种情况：
	 *
	 *	 1: 没有任何 NULL，也没有变长属性。
	 *	 2: 在目标属性之后存在 NULL 或变长属性。
	 *	 3: 在目标属性之前存在 NULL 或变长属性。
	 * ----------------
	 */

	attnum--;

	if (!HeapTupleNoNulls(tup))
	{
		/*
		 * 元组的某处存在 NULL
		 *
		 * 检查前面的位是否包含 NULL……
		 */
		int			byte = attnum >> 3;
		int			finalbit = attnum & 0x07;

		/* 检查最后一个字节中、位于最终位“之前”的 NULL */
		if ((~bp[byte]) & ((1 << finalbit) - 1))
			slow = true;
		else
		{
			/* 检查任何“更早”的字节中是否存在 NULL */
			int			i;

			for (i = 0; i < byte; i++)
			{
				if (bp[i] != 0xFF)
				{
					slow = true;
					break;
				}
			}
		}
	}

	tp = (char *) td + td->t_hoff;

	if (!slow)
	{
		CompactAttribute *att;

		/*
		 * 如果执行到这里，说明直到（含）目标属性都没有 NULL
		 * 。如果已有缓存的偏移量，就可以直接使用它。
		 */
		att = TupleDescCompactAttr(tupleDesc, attnum);
		if (att->attcacheoff >= 0)
			return fetchatt(att, tp + att->attcacheoff);

		/*
		 * 否则，检查从当前位置到目标属性（含）之间是否存在变长属性。
		 * 如果不存在，就可以安全地廉价地为这些属性初始化缓存偏移量。
		 */
		if (HeapTupleHasVarWidth(tup))
		{
			int			j;

			for (j = 0; j <= attnum; j++)
			{
				if (TupleDescCompactAttr(tupleDesc, j)->attlen <= 0)
				{
					slow = true;
					break;
				}
			}
		}
	}

	if (!slow)
	{
		int			natts = tupleDesc->natts;
		int			j = 1;

		/*
		 * 如果执行到这里，说明直到（含）目标属性都没有 NULL 或变长属性
		 * ，并且包括目标属性，因此我们可以使用缓存的偏移量，
		 * 只是我们尚未拥有它，否则就不会执行到这里。由于
		 * 计算定宽列的偏移量开销很小，我们借此机会
		 * 为*所有*前导定宽列初始化缓存偏移量，
		 * 以期避免将来再次进入本
		 * 例程。
		 */
		TupleDescCompactAttr(tupleDesc, 0)->attcacheoff = 0;

		/* 我们可能在之前的慢速路径中已经设置过一些偏移量 */
		while (j < natts && TupleDescCompactAttr(tupleDesc, j)->attcacheoff > 0)
			j++;

		off = TupleDescCompactAttr(tupleDesc, j - 1)->attcacheoff +
			TupleDescCompactAttr(tupleDesc, j - 1)->attlen;

		for (; j < natts; j++)
		{
			CompactAttribute *att = TupleDescCompactAttr(tupleDesc, j);

			if (att->attlen <= 0)
				break;

			off = att_nominal_alignby(off, att->attalignby);

			att->attcacheoff = off;

			off += att->attlen;
		}

		Assert(j > attnum);

		off = TupleDescCompactAttr(tupleDesc, attnum)->attcacheoff;
	}
	else
	{
		bool		usecache = true;
		int			i;

		/*
		 * 现在我们明确必须小心地遍历元组。但我们仍然
		 * 可能还能为下次缓存一些偏移量。
		 *
		 * 注意 —— 这个循环有点 tricky。对于每个非 NULL 属性，
		 * 我们必须先考虑该属性之前的 alignment 填充，
		 * 然后依据其长度向前推进。NULL 没有存储空间，
		 * 也没有 alignment 填充。我们可以一直使用/设置
		 * attcacheoff，直到遇到 NULL 或变长属性为止。
		 */
		off = 0;
		for (i = 0;; i++)		/* 循环在 "break" 处退出 */
		{
			CompactAttribute *att = TupleDescCompactAttr(tupleDesc, i);

			if (HeapTupleHasNulls(tup) && att_isnull(i, bp))
			{
				usecache = false;
				continue;		/* 这不可能是目标属性 */
			}

			/* 如果已知下一个偏移量，就可以跳过其余部分 */
			if (usecache && att->attcacheoff >= 0)
				off = att->attcacheoff;
			else if (att->attlen == -1)
			{
				/*
				 * 只有当 varlena 属性的偏移量已经恰当对齐时，我们才能
				 * 缓存它，这样无论如何都不会有填充字节：那么该偏移量
				 * 对对齐或未对齐的值都有效。
				 */
				if (usecache &&
					off == att_nominal_alignby(off, att->attalignby))
					att->attcacheoff = off;
				else
				{
					off = att_pointer_alignby(off, att->attalignby, -1,
											  tp + off);
					usecache = false;
				}
			}
			else
			{
				/* 不是 varlena，因此可以安全地使用 att_nominal_alignby */
				off = att_nominal_alignby(off, att->attalignby);

				if (usecache)
					att->attcacheoff = off;
			}

			if (i == attnum)
				break;

			off = att_addlength_pointer(off, att->attlen, tp + off);

			if (usecache && att->attlen <= 0)
				usecache = false;
		}
	}

	return fetchatt(TupleDescCompactAttr(tupleDesc, attnum), tp + off);
}

/* ----------------
 *		heap_getsysattr
 *
 *		获取元组中某个系统属性的值。
 *
 * 这是 heap_getattr() 的支持例程。该函数已经确定 attnum
 * 指向一个系统属性。
 * ----------------
 */
Datum
heap_getsysattr(HeapTuple tup, int attnum, TupleDesc tupleDesc, bool *isnull)
{
	Datum		result;

	Assert(tup);

	/* 目前，没有任何系统属性会读取为 NULL。 */
	*isnull = false;

	switch (attnum)
	{
		case SelfItemPointerAttributeNumber:
			/* 按引用传递的数据类型 */
			result = PointerGetDatum(&(tup->t_self));
			break;
		case MinTransactionIdAttributeNumber:
			result = TransactionIdGetDatum(HeapTupleHeaderGetRawXmin(tup->t_data));
			break;
		case MaxTransactionIdAttributeNumber:
			result = TransactionIdGetDatum(HeapTupleHeaderGetRawXmax(tup->t_data));
			break;
		case MinCommandIdAttributeNumber:
		case MaxCommandIdAttributeNumber:

			/*
			 * cmin 和 cmax 现在都是同一个字段的别名，而该字段实际上
			 * 也可能是一个组合命令 id。XXX 或许我们应该在可能的情况下
			 * 返回“真实”的 cmin 或 cmax，即当我们处于发起该事务的
			 * 事务内部时？
			 */
			result = CommandIdGetDatum(HeapTupleHeaderGetRawCommandId(tup->t_data));
			break;
		case TableOidAttributeNumber:
			result = ObjectIdGetDatum(tup->t_tableOid);
			break;
		default:
			elog(ERROR, "invalid attnum: %d", attnum);
			result = 0;			/* 让编译器保持安静（消除告警） */
			break;
	}
	return result;
}

/* ----------------
 *		heap_copytuple
 *
 *		返回整个元组的一份副本
 *
 * HeapTuple 结构体、元组头以及元组数据都作为单个 palloc()
 * 块统一分配。
 * ----------------
 */
HeapTuple
heap_copytuple(HeapTuple tuple)
{
	HeapTuple	newTuple;

	if (!HeapTupleIsValid(tuple) || tuple->t_data == NULL)
		return NULL;

	newTuple = (HeapTuple) palloc(HEAPTUPLESIZE + tuple->t_len);
	newTuple->t_len = tuple->t_len;
	newTuple->t_self = tuple->t_self;
	newTuple->t_tableOid = tuple->t_tableOid;
	newTuple->t_data = (HeapTupleHeader) ((char *) newTuple + HEAPTUPLESIZE);
	memcpy(newTuple->t_data, tuple->t_data, tuple->t_len);
	return newTuple;
}

/* ----------------
 *		heap_copytuple_with_tuple
 *
 *		将一个元组拷贝进调用方提供的 HeapTuple 管理结构体
 *
 * 注意，调用本函数后，"dest" HeapTuple 不会被分配为单个 palloc()
 * 块（这与 heap_copytuple() 不同）。
 * ----------------
 */
void
heap_copytuple_with_tuple(HeapTuple src, HeapTuple dest)
{
	if (!HeapTupleIsValid(src) || src->t_data == NULL)
	{
		dest->t_data = NULL;
		return;
	}

	dest->t_len = src->t_len;
	dest->t_self = src->t_self;
	dest->t_tableOid = src->t_tableOid;
	dest->t_data = (HeapTupleHeader) palloc(src->t_len);
	memcpy(dest->t_data, src->t_data, src->t_len);
}

/*
 * 展开一个属性数量少于所需的元组。对于源元组中不存在的每个属性，
 * 若有缺失值则使用缺失值，否则将该属性设为 NULL。
 *
 * 源元组的属性数量必须少于所需数量。
 *
 * targetHeapTuple 与 targetMinimalTuple 二者只能传入其一，
 * 另一个参数必须为 NULL。
 */
static void
expand_tuple(HeapTuple *targetHeapTuple,
			 MinimalTuple *targetMinimalTuple,
			 HeapTuple sourceTuple,
			 TupleDesc tupleDesc)
{
	AttrMissing *attrmiss = NULL;
	int			attnum;
	int			firstmissingnum;
	bool		hasNulls = HeapTupleHasNulls(sourceTuple);
	HeapTupleHeader targetTHeader;
	HeapTupleHeader sourceTHeader = sourceTuple->t_data;
	int			sourceNatts = HeapTupleHeaderGetNatts(sourceTHeader);
	int			natts = tupleDesc->natts;
	int			sourceNullLen;
	int			targetNullLen;
	Size		sourceDataLen = sourceTuple->t_len - sourceTHeader->t_hoff;
	Size		targetDataLen;
	Size		len;
	int			hoff;
	bits8	   *nullBits = NULL;
	int			bitMask = 0;
	char	   *targetData;
	uint16	   *infoMask;

	Assert((targetHeapTuple && !targetMinimalTuple)
		   || (!targetHeapTuple && targetMinimalTuple));

	Assert(sourceNatts < natts);

	sourceNullLen = (hasNulls ? BITMAPLEN(sourceNatts) : 0);

	targetDataLen = sourceDataLen;

	if (tupleDesc->constr &&
		tupleDesc->constr->missing)
	{
		/*
		 * 如果存在缺失值，我们希望将它们填入元组。在此之前，
		 * 必须先为 values 数组以及变长数据计算额外的长度。
		 */
		attrmiss = tupleDesc->constr->missing;

		/*
		 * 在 attrmiss 中找到第一个在源元组中没有对应值的项。
		 * 此前的所有缺失项都可以忽略。
		 */
		for (firstmissingnum = sourceNatts;
			 firstmissingnum < natts;
			 firstmissingnum++)
		{
			if (attrmiss[firstmissingnum].am_present)
				break;
			else
				hasNulls = true;
		}

		/*
		 * 现在遍历缺失属性。如果存在缺失值，则为它
		 * 预留空间；否则，它将为 NULL。
		 */
		for (attnum = firstmissingnum;
			 attnum < natts;
			 attnum++)
		{
			if (attrmiss[attnum].am_present)
			{
				CompactAttribute *att = TupleDescCompactAttr(tupleDesc, attnum);

				targetDataLen = att_datum_alignby(targetDataLen,
												  att->attalignby,
												  att->attlen,
												  attrmiss[attnum].am_value);

				targetDataLen = att_addlength_pointer(targetDataLen,
													  att->attlen,
													  attrmiss[attnum].am_value);
			}
			else
			{
				/* 没有缺失值，因此必须为 NULL */
				hasNulls = true;
			}
		}
	}							/* 存在缺失值分支结束 */
	else
	{
	/*
	 * 如果根本不存在任何缺失值，则必须允许 NULL，
	 * 因为已知某些属性是缺失的。
	 */
		hasNulls = true;
	}

	len = 0;

	if (hasNulls)
	{
		targetNullLen = BITMAPLEN(natts);
		len += targetNullLen;
	}
	else
		targetNullLen = 0;

	/*
	 * 分配并将所需空间置零。注意元组体与 HeapTupleData
	 * 管理结构体是作为一整块统一分配的。
	 */
	if (targetHeapTuple)
	{
		len += offsetof(HeapTupleHeaderData, t_bits);
		hoff = len = MAXALIGN(len); /* 安全地对齐用户数据 */
		len += targetDataLen;

		*targetHeapTuple = (HeapTuple) palloc0(HEAPTUPLESIZE + len);
		(*targetHeapTuple)->t_data
			= targetTHeader
			= (HeapTupleHeader) ((char *) *targetHeapTuple + HEAPTUPLESIZE);
		(*targetHeapTuple)->t_len = len;
		(*targetHeapTuple)->t_tableOid = sourceTuple->t_tableOid;
		(*targetHeapTuple)->t_self = sourceTuple->t_self;

		targetTHeader->t_infomask = sourceTHeader->t_infomask;
		targetTHeader->t_hoff = hoff;
		HeapTupleHeaderSetNatts(targetTHeader, natts);
		HeapTupleHeaderSetDatumLength(targetTHeader, len);
		HeapTupleHeaderSetTypeId(targetTHeader, tupleDesc->tdtypeid);
		HeapTupleHeaderSetTypMod(targetTHeader, tupleDesc->tdtypmod);
		/* 我们还确保 t_ctid 在处理前保持无效（除非显式设置） */
		ItemPointerSetInvalid(&(targetTHeader->t_ctid));
		if (targetNullLen > 0)
			nullBits = (bits8 *) ((char *) (*targetHeapTuple)->t_data
								  + offsetof(HeapTupleHeaderData, t_bits));
		targetData = (char *) (*targetHeapTuple)->t_data + hoff;
		infoMask = &(targetTHeader->t_infomask);
	}
	else
	{
		len += SizeofMinimalTupleHeader;
		hoff = len = MAXALIGN(len); /* 安全地对齐用户数据 */
		len += targetDataLen;

		*targetMinimalTuple = (MinimalTuple) palloc0(len);
		(*targetMinimalTuple)->t_len = len;
		(*targetMinimalTuple)->t_hoff = hoff + MINIMAL_TUPLE_OFFSET;
		(*targetMinimalTuple)->t_infomask = sourceTHeader->t_infomask;
		/* 同一宏对 MinimalTuple 也适用 */
		HeapTupleHeaderSetNatts(*targetMinimalTuple, natts);
		if (targetNullLen > 0)
			nullBits = (bits8 *) ((char *) *targetMinimalTuple
								  + offsetof(MinimalTupleData, t_bits));
		targetData = (char *) *targetMinimalTuple + hoff;
		infoMask = &((*targetMinimalTuple)->t_infomask);
	}

	if (targetNullLen > 0)
	{
		if (sourceNullLen > 0)
		{
			/* 若位图原先已存在，则直接拷贝进来 —— 全部已设置 */
			memcpy(nullBits,
				   ((char *) sourceTHeader)
				   + offsetof(HeapTupleHeaderData, t_bits),
				   sourceNullLen);
			nullBits += sourceNullLen - 1;
		}
		else
		{
			sourceNullLen = BITMAPLEN(sourceNatts);
			/* 为所有已有属性设置 NOT NULL */
			memset(nullBits, 0xff, sourceNullLen);

			nullBits += sourceNullLen - 1;

			if (sourceNatts & 0x07)
			{
				/* 构造掩码（取反！） */
				bitMask = 0xff << (sourceNatts & 0x07);
				/* 完成 */
				*nullBits = ~bitMask;
			}
		}

		bitMask = (1 << ((sourceNatts - 1) & 0x07));
	}							/* 存在空值位图分支结束 */

	memcpy(targetData,
		   ((char *) sourceTuple->t_data) + sourceTHeader->t_hoff,
		   sourceDataLen);

	targetData += sourceDataLen;

	/* 现在填入缺失值 */
	for (attnum = sourceNatts; attnum < natts; attnum++)
	{
		CompactAttribute *attr = TupleDescCompactAttr(tupleDesc, attnum);

		if (attrmiss && attrmiss[attnum].am_present)
		{
			fill_val(attr,
					 nullBits ? &nullBits : NULL,
					 &bitMask,
					 &targetData,
					 infoMask,
					 attrmiss[attnum].am_value,
					 false);
		}
		else
		{
			fill_val(attr,
					 &nullBits,
					 &bitMask,
					 &targetData,
					 infoMask,
					 (Datum) 0,
					 true);
		}
	}							/* 遍历缺失属性循环结束 */
}

/*
 * 为 minimal HeapTuple 填入缺失值
 */
MinimalTuple
minimal_expand_tuple(HeapTuple sourceTuple, TupleDesc tupleDesc)
{
	MinimalTuple minimalTuple;

	expand_tuple(NULL, &minimalTuple, sourceTuple, tupleDesc);
	return minimalTuple;
}

/*
 * 为普通 HeapTuple 填入缺失值
 */
HeapTuple
heap_expand_tuple(HeapTuple sourceTuple, TupleDesc tupleDesc)
{
	HeapTuple	heapTuple;

	expand_tuple(&heapTuple, NULL, sourceTuple, tupleDesc);
	return heapTuple;
}

/* ----------------
 *		heap_copy_tuple_as_datum
 *
 *		将一个元组作为组合类型 Datum 进行拷贝
 * ----------------
 */
Datum
heap_copy_tuple_as_datum(HeapTuple tuple, TupleDesc tupleDesc)
{
	HeapTupleHeader td;

	/*
	 * 如果元组包含任何外部 TOAST 指针，我们必须将其内联
	 * 以满足组合类型 Datum 的约定。
	 */
	if (HeapTupleHasExternal(tuple))
		return toast_flatten_tuple_to_datum(tuple->t_data,
											tuple->t_len,
											tupleDesc);

	/*
	 * 针对简单情况的快速路径：只需制作一个 palloc 分配的副本，并插入
	 * 正确的组合类型 Datum 头部字段（因为这些字段可能未被设置，若
	 * 给定的元组来自磁盘，而非来自 heap_form_tuple）。
	 */
	td = (HeapTupleHeader) palloc(tuple->t_len);
	memcpy(td, tuple->t_data, tuple->t_len);

	HeapTupleHeaderSetDatumLength(td, tuple->t_len);
	HeapTupleHeaderSetTypeId(td, tupleDesc->tdtypeid);
	HeapTupleHeaderSetTypMod(td, tupleDesc->tdtypmod);

	return PointerGetDatum(td);
}

/*
 * heap_form_tuple
 *		根据给定的 values[] 与 isnull[] 数组构造一个元组，
 *		这两个数组的长度为 tupleDescriptor->natts 所指示。
 *
 * 结果在当前内存上下文中分配。
 */
HeapTuple
heap_form_tuple(TupleDesc tupleDescriptor,
				const Datum *values,
				const bool *isnull)
{
	HeapTuple	tuple;			/* 返回的元组 */
	HeapTupleHeader td;			/* 元组数据 */
	Size		len,
				data_len;
	int			hoff;
	bool		hasnull = false;
	int			numberOfAttributes = tupleDescriptor->natts;
	int			i;

	if (numberOfAttributes > MaxTupleAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("number of columns (%d) exceeds limit (%d)",
						numberOfAttributes, MaxTupleAttributeNumber)));

	/*
	 * 检查是否存在 NULL
	 */
	for (i = 0; i < numberOfAttributes; i++)
	{
		if (isnull[i])
		{
			hasnull = true;
			break;
		}
	}

	/*
	 * 确定所需的总空间
	 */
	len = offsetof(HeapTupleHeaderData, t_bits);

	if (hasnull)
		len += BITMAPLEN(numberOfAttributes);

	hoff = len = MAXALIGN(len); /* 安全地对齐用户数据 */

	data_len = heap_compute_data_size(tupleDescriptor, values, isnull);

	len += data_len;

	/*
	 * 分配并将所需空间置零。注意元组体与 HeapTupleData
	 * 管理结构体是作为一整块统一分配的。
	 */
	tuple = (HeapTuple) palloc0(HEAPTUPLESIZE + len);
	tuple->t_data = td = (HeapTupleHeader) ((char *) tuple + HEAPTUPLESIZE);

	/*
	 * 然后填入相关信息。注意，即便该元组可能永远不会成为 Datum，
	 * 我们也填充 Datum 字段。这样在需要时，HeapTupleHeaderGetDatum
	 * 即可识别该元组的类型。
	 */
	tuple->t_len = len;
	ItemPointerSetInvalid(&(tuple->t_self));
	tuple->t_tableOid = InvalidOid;

	HeapTupleHeaderSetDatumLength(td, len);
	HeapTupleHeaderSetTypeId(td, tupleDescriptor->tdtypeid);
	HeapTupleHeaderSetTypMod(td, tupleDescriptor->tdtypmod);
	/* 我们还确保 t_ctid 在处理前保持无效（除非显式设置） */
	ItemPointerSetInvalid(&(td->t_ctid));

	HeapTupleHeaderSetNatts(td, numberOfAttributes);
	td->t_hoff = hoff;

	heap_fill_tuple(tupleDescriptor,
					values,
					isnull,
					(char *) td + hoff,
					data_len,
					&td->t_infomask,
					(hasnull ? td->t_bits : NULL));

	return tuple;
}

/*
 * heap_modify_tuple
 *		基于旧元组与一组替换值构造一个新元组。
 *
 * replValues、replIsnull 和 doReplace 数组的长度必须为
 * tupleDesc->natts。在新元组中，doReplace 为真的列使用 replValues/
 * replIsnull 中的数据，doReplace 为假的列使用旧元组中的数据。
 *
 * 结果在当前内存上下文中分配。
 */
HeapTuple
heap_modify_tuple(HeapTuple tuple,
				  TupleDesc tupleDesc,
				  const Datum *replValues,
				  const bool *replIsnull,
				  const bool *doReplace)
{
	int			numberOfAttributes = tupleDesc->natts;
	int			attoff;
	Datum	   *values;
	bool	   *isnull;
	HeapTuple	newTuple;

	/*
	 * 视情况从元组或替换信息中为 values 和 isnull 数组分配并填充数据。
	 *
	 * 注意：这里究竟该使用 heap_deform_tuple()，还是仅对未被替换的列
	 * 调用 heap_getattr()，尚有争议。若被替换的列很多、未被替换的列
	 * 很少，后者可能更优。然而 heap_deform_tuple 的代价仅为 O(N)，而
	 * 若未被替换的列很多，heap_getattr 方式的代价将达到 O(N^2)，
	 * 因此倾向于线性代价似乎更好。
	 */
	values = (Datum *) palloc(numberOfAttributes * sizeof(Datum));
	isnull = (bool *) palloc(numberOfAttributes * sizeof(bool));

	heap_deform_tuple(tuple, tupleDesc, values, isnull);

	for (attoff = 0; attoff < numberOfAttributes; attoff++)
	{
		if (doReplace[attoff])
		{
			values[attoff] = replValues[attoff];
			isnull[attoff] = replIsnull[attoff];
		}
	}

	/*
	 * 根据 values 和 isnull 数组创建一个新元组
	 */
	newTuple = heap_form_tuple(tupleDesc, values, isnull);

	pfree(values);
	pfree(isnull);

	/*
	 * 复制旧元组的标识信息：t_ctid、t_self
	 */
	newTuple->t_data->t_ctid = tuple->t_data->t_ctid;
	newTuple->t_self = tuple->t_self;
	newTuple->t_tableOid = tuple->t_tableOid;

	return newTuple;
}

/*
 * heap_modify_tuple_by_cols
 *		基于旧元组与一组替换值构造一个新元组。
 *
 * 本函数与 heap_modify_tuple 类似，区别在于不使用布尔映射来指定
 * 要替换的哪些列，而是使用一个目标列号数组。当需要替换固定数量的列时，
 * 这种方式往往更方便。replCols、replValues 和 replIsnull 数组的长度
 * 必须为 nCols。目标列号从 1 开始编号。
 *
 * 结果在当前内存上下文中分配。
 */
HeapTuple
heap_modify_tuple_by_cols(HeapTuple tuple,
						  TupleDesc tupleDesc,
						  int nCols,
						  const int *replCols,
						  const Datum *replValues,
						  const bool *replIsnull)
{
	int			numberOfAttributes = tupleDesc->natts;
	Datum	   *values;
	bool	   *isnull;
	HeapTuple	newTuple;
	int			i;

	/*
	 * 从元组中为 values 和 isnull 数组分配并填充数据，
	 * 然后从输入数组中替换选中的列。
	 */
	values = (Datum *) palloc(numberOfAttributes * sizeof(Datum));
	isnull = (bool *) palloc(numberOfAttributes * sizeof(bool));

	heap_deform_tuple(tuple, tupleDesc, values, isnull);

	for (i = 0; i < nCols; i++)
	{
		int			attnum = replCols[i];

		if (attnum <= 0 || attnum > numberOfAttributes)
			elog(ERROR, "invalid column number %d", attnum);
		values[attnum - 1] = replValues[i];
		isnull[attnum - 1] = replIsnull[i];
	}

	/*
	 * 根据 values 和 isnull 数组创建一个新元组
	 */
	newTuple = heap_form_tuple(tupleDesc, values, isnull);

	pfree(values);
	pfree(isnull);

	/*
	 * 复制旧元组的标识信息：t_ctid、t_self
	 */
	newTuple->t_data->t_ctid = tuple->t_data->t_ctid;
	newTuple->t_self = tuple->t_self;
	newTuple->t_tableOid = tuple->t_tableOid;

	return newTuple;
}

/*
 * heap_deform_tuple
 *		给定一个元组，将数据抽取到 values/isnull 数组中；
 *		这是 heap_form_tuple 的逆操作。
 *
 *		values/isnull 数组的存储空间由调用方提供；其大小应依据
 *		tupleDesc->natts 而非 HeapTupleHeaderGetNatts(tuple->t_data)
 *		来确定。
 *
 *		注意，对于按引用传递的数据类型，放入 Datum 中的指针将指向
 *		给定的元组。
 *
 *		当元组的大部分或全部字段都需要抽取时，本例程会比围绕
 *		heap_getattr 的循环明显更快；一旦涉及任何不可缓存的属性
 *		偏移量，该循环的代价将变为 O(N^2)。
 */
void
heap_deform_tuple(HeapTuple tuple, TupleDesc tupleDesc,
				  Datum *values, bool *isnull)
{
	HeapTupleHeader tup = tuple->t_data;
	bool		hasnulls = HeapTupleHasNulls(tuple);
	int			tdesc_natts = tupleDesc->natts;
	int			natts;			/* 待抽取的属性数量 */
	int			attnum;
	char	   *tp;				/* 元组数据的指针 */
	uint32		off;			/* 元组数据中的偏移量 */
	bits8	   *bp = tup->t_bits;	/* 元组中空值位图的指针 */
	bool		slow = false;	/* 能否使用/设置 attcacheoff？ */

	natts = HeapTupleHeaderGetNatts(tup);

	/*
	 * 在继承场景下，给定的元组实际拥有的字段数可能多于调用方的预期。
	 * 不要让读取越过调用方数组的末尾。
	 */
	natts = Min(natts, tdesc_natts);

	tp = (char *) tup + tup->t_hoff;

	off = 0;

	for (attnum = 0; attnum < natts; attnum++)
	{
		CompactAttribute *thisatt = TupleDescCompactAttr(tupleDesc, attnum);

		if (hasnulls && att_isnull(attnum, bp))
		{
			values[attnum] = (Datum) 0;
			isnull[attnum] = true;
			slow = true;		/* 无法再使用 attcacheoff */
			continue;
		}

		isnull[attnum] = false;

		if (!slow && thisatt->attcacheoff >= 0)
			off = thisatt->attcacheoff;
		else if (thisatt->attlen == -1)
		{
			/*
			 * 只有当 varlena 属性的偏移量
			 * 已经恰当对齐时，我们才能缓存它，这样无论如何都不会有
			 * 出现填充字节：那么该偏移量对对齐或未对齐的值都有效
			 * 。
			 */
			if (!slow &&
				off == att_nominal_alignby(off, thisatt->attalignby))
				thisatt->attcacheoff = off;
			else
			{
				off = att_pointer_alignby(off, thisatt->attalignby, -1,
										  tp + off);
				slow = true;
			}
		}
		else
		{
			/* 不是 varlena，因此可以安全地使用 att_nominal_alignby */
			off = att_nominal_alignby(off, thisatt->attalignby);

			if (!slow)
				thisatt->attcacheoff = off;
		}

		values[attnum] = fetchatt(thisatt, tp + off);

		off = att_addlength_pointer(off, thisatt->attlen, tp + off);

		if (thisatt->attlen <= 0)
			slow = true;		/* 无法再使用 attcacheoff */
	}

	/*
	 * 若元组并未拥有 tupleDesc 所指示的全部属性，则将其余部分
	 * 按情况读取为 NULL 或缺失值。
	 */
	for (; attnum < tdesc_natts; attnum++)
		values[attnum] = getmissingattr(tupleDesc, attnum + 1, &isnull[attnum]);
}

/*
 * heap_freetuple
 */
void
heap_freetuple(HeapTuple htup)
{
	pfree(htup);
}


/*
 * heap_form_minimal_tuple
 *		根据给定的 values[] 与 isnull[] 数组构造一个 MinimalTuple，
 *		这两个数组的长度为 tupleDescriptor->natts 所指示。
 *
 * 本函数与 heap_form_tuple() 几乎相同，不同之处在于其结果是一个
 * “最小”元组，缺少 HeapTupleData 头部以及系统列的空间。
 *
 * 结果在当前内存上下文中分配。
 */
MinimalTuple
heap_form_minimal_tuple(TupleDesc tupleDescriptor,
						const Datum *values,
						const bool *isnull,
						Size extra)
{
	MinimalTuple tuple;			/* 返回的元组 */
	char	   *mem;
	Size		len,
				data_len;
	int			hoff;
	bool		hasnull = false;
	int			numberOfAttributes = tupleDescriptor->natts;
	int			i;

	Assert(extra == MAXALIGN(extra));

	if (numberOfAttributes > MaxTupleAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("number of columns (%d) exceeds limit (%d)",
						numberOfAttributes, MaxTupleAttributeNumber)));

	/*
	 * 检查是否存在 NULL
	 */
	for (i = 0; i < numberOfAttributes; i++)
	{
		if (isnull[i])
		{
			hasnull = true;
			break;
		}
	}

	/*
	 * 确定所需的总空间
	 */
	len = SizeofMinimalTupleHeader;

	if (hasnull)
		len += BITMAPLEN(numberOfAttributes);

	hoff = len = MAXALIGN(len); /* 安全地对齐用户数据 */

	data_len = heap_compute_data_size(tupleDescriptor, values, isnull);

	len += data_len;

	/*
	 * 分配并将所需空间置零。
	 */
	mem = palloc0(len + extra);
	memset(mem, 0, extra);
	tuple = (MinimalTuple) (mem + extra);

	/*
	 * 然后填入相关信息。
	 */
	tuple->t_len = len;
	HeapTupleHeaderSetNatts(tuple, numberOfAttributes);
	tuple->t_hoff = hoff + MINIMAL_TUPLE_OFFSET;

	heap_fill_tuple(tupleDescriptor,
					values,
					isnull,
					(char *) tuple + hoff,
					data_len,
					&tuple->t_infomask,
					(hasnull ? tuple->t_bits : NULL));

	return tuple;
}

/*
 * heap_free_minimal_tuple
 */
void
heap_free_minimal_tuple(MinimalTuple mtup)
{
	pfree(mtup);
}

/*
 * heap_copy_minimal_tuple
 *		拷贝一个 MinimalTuple
 *
 * 结果在当前内存上下文中分配。
 */
MinimalTuple
heap_copy_minimal_tuple(MinimalTuple mtup, Size extra)
{
	MinimalTuple result;
	char	   *mem;

	Assert(extra == MAXALIGN(extra));
	mem = palloc(mtup->t_len + extra);
	memset(mem, 0, extra);
	result = (MinimalTuple) (mem + extra);
	memcpy(result, mtup, mtup->t_len);
	return result;
}

/*
 * heap_tuple_from_minimal_tuple
 *		通过拷贝自 MinimalTuple 来创建一个 HeapTuple；
 *		系统列填充为零。
 *
 * 结果在当前内存上下文中分配。
 * HeapTuple 结构体、元组头以及元组数据均作为单个 palloc()
 * 块统一分配。
 */
HeapTuple
heap_tuple_from_minimal_tuple(MinimalTuple mtup)
{
	HeapTuple	result;
	uint32		len = mtup->t_len + MINIMAL_TUPLE_OFFSET;

	result = (HeapTuple) palloc(HEAPTUPLESIZE + len);
	result->t_len = len;
	ItemPointerSetInvalid(&(result->t_self));
	result->t_tableOid = InvalidOid;
	result->t_data = (HeapTupleHeader) ((char *) result + HEAPTUPLESIZE);
	memcpy((char *) result->t_data + MINIMAL_TUPLE_OFFSET, mtup, mtup->t_len);
	memset(result->t_data, 0, offsetof(HeapTupleHeaderData, t_infomask2));
	return result;
}

/*
 * minimal_tuple_from_heap_tuple
 *		通过拷贝自 HeapTuple 来创建一个 MinimalTuple
 *
 * 结果在当前内存上下文中分配。
 */
MinimalTuple
minimal_tuple_from_heap_tuple(HeapTuple htup, Size extra)
{
	MinimalTuple result;
	char	   *mem;
	uint32		len;

	Assert(extra == MAXALIGN(extra));
	Assert(htup->t_len > MINIMAL_TUPLE_OFFSET);
	len = htup->t_len - MINIMAL_TUPLE_OFFSET;
	mem = palloc(len + extra);
	memset(mem, 0, extra);
	result = (MinimalTuple) (mem + extra);
	memcpy(result, (char *) htup->t_data + MINIMAL_TUPLE_OFFSET, len);

	result->t_len = len;
	return result;
}

/*
 * 这主要是为了便于 JIT 内联该定义，但在调试会话中
 * 有时也很有用。
 */
size_t
varsize_any(void *p)
{
	return VARSIZE_ANY(p);
}
